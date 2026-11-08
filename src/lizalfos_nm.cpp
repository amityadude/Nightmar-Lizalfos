// Nightmare Lizalfos: engine glue.
//
// How this works without touching the game source
// -----------------------------------------------
// The Lizalfos (e_dn_class, d_a_e_dn.cpp) runs its whole brain inside one file-local function,
// action(), which calls damage_check() first and then dispatches to the e_dn_* action functions
// and applies movement. A mod can only wrap whole functions, so this file never replaces the
// vanilla AI. It:
//
//   * PRE  action()       observes Link, decides, and switches the vanilla action/mode (guard,
//                         attack, a mod-owned dodge mode) *before* vanilla runs, so vanilla's own
//                         action functions play the existing animations, sounds and hit windows.
//   * POST action()       adjusts horizontal movement, wind-up timing, combos and bookkeeping
//                         after vanilla ran. Horizontal displacement is rewritten relative to the
//                         position at the start of the frame; vanilla's BG collision
//                         (objacch.CrrPos) runs afterwards, so walls still stop the actor.
//   * PRE/POST damage_check()  shield block by real collision, and poise.
//
// Fairness rules that hold everywhere below: no invulnerability is ever granted (the shield only
// cancels a hit whose attack object really struck the shield sphere), nothing teleports, no new
// hitbox, animation or sound is created, and every hit that lands still reduces health.
//
// Nothing is allocated per frame. Per-actor state lives in a fixed pool keyed by actor address
// and process id.

#include "lizalfos_nm.h"

#include "nm_logic.h"

#include "d/dolzel_rel.h"  // pulls in the common game headers the actor headers expect first

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_e_dn.h"
#include "d/actor/d_a_player.h"
#include "d/d_bg_s_gnd_chk.h"
#include "d/d_bg_s_lin_chk.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "SSystem/SComponent/c_lib.h"
#include "SSystem/SComponent/c_math.h"
#include "Z2AudioLib/Z2Instances.h"

#include <math.h>

namespace lnm {

int g_level = nm::LEVEL_NIGHTMARE;
Capabilities g_caps = {};

namespace {

// ---------------------------------------------------------------------------------------------
// Mirrors of the vanilla file-local enums in d_a_e_dn.cpp (same values; the vanilla file keeps
// them private). If a Dusklight update renumbers them, these must follow.
// ---------------------------------------------------------------------------------------------
enum Action {
    A_NORMAL = 0,
    A_FIGHT_RUN = 3,
    A_ATTACK_0 = 4,
    A_ATTACK = 5,
    A_TAIL_ATTACK = 6,
    A_GUARD = 7,
    A_JUMP = 8,
    A_STAY = 9,
    A_OTOREAC = 10,
    A_BOMB_ACTION = 11,
    A_DRAWBACK = 12,
    A_WOLFBITE = 13,
    A_S_DAMAGE = 20,
    A_DAMAGE = 21,
};

enum Anim {
    ANM_ATTACK_TAIL_01 = 4,  // quick stab   (ACTION_ATTACK_0)
    ANM_ATTACK_TAIL_02 = 5,  // stepping cut (ACTION_ATTACK)
    ANM_ATTACK_01 = 8,       // tail sweep   (ACTION_TAIL_ATTACK)
    ANM_WAIT_02 = 0x20,      // the hop used for circling / the guard back-hop
};

// A mode value the vanilla e_dn_fight_run switch has no case for. Vanilla leaves it alone, so the
// actor stays a perfectly normal ACTION_FIGHT_RUN actor (collision, damage, ledge checks, events)
// while the mod drives its movement. Any change of action by vanilla ends it.
constexpr s16 MODE_NM_DODGE = 100;

constexpr int kPoolSize = 32;
constexpr float kEngageRange = 400.0f;  // vanilla attack_init_range is 350; Nightmare closes in
constexpr int kArcFront = 0x3000;       // ~67 degrees either side counts as "facing"

inline bool is_attack(int a) { return a == A_ATTACK_0 || a == A_ATTACK || a == A_TAIL_ATTACK; }
inline bool is_combat(int a) { return a == A_FIGHT_RUN || is_attack(a) || a == A_GUARD; }
inline bool is_idle(int a) { return a == A_NORMAL || a == A_OTOREAC; }

inline int action_of(nm::Tactic t) {
    return t == nm::TACTIC_QUICK ? A_ATTACK_0 : (t == nm::TACTIC_STEP ? A_ATTACK : A_TAIL_ATTACK);
}
inline nm::Tactic tactic_of(int a) {
    return a == A_ATTACK_0 ? nm::TACTIC_QUICK : (a == A_ATTACK ? nm::TACTIC_STEP : nm::TACTIC_TAIL);
}
// Animation frame at which a delayed attack holds its wind-up (just before the hit window).
inline int hold_frame_of(nm::Tactic t) {
    return t == nm::TACTIC_QUICK ? 19 : (t == nm::TACTIC_STEP ? 10 : 28);
}

// ---------------------------------------------------------------------------------------------
// Per-actor state. Plain data only (the pool zero-initialises it).
// ---------------------------------------------------------------------------------------------
struct Snapshot {
    bool valid;
    s16 action, mode, health, inv, unk_timer_4;
    f32 speedF, speedY, f704, f708, f74c;
    s16 f70c, f70e, f750, f724y, angle_y;
    s8 f710;
};

struct Brain {
    nm::Memory mem;

    // bookkeeping
    bool active;           // pre ran this frame, post should process
    u8 level;              // level this brain last ran at
    s16 pre_action;        // action just before vanilla action() runs (after the mod decided)
    s16 pre_mode;
    f32 before_x, before_y, before_z;  // position at the start of the frame
    s16 cur_yaw_before, shape_yaw_before;
    f32 base_range;        // vanilla sight range (restored on shutdown)
    bool range_applied;
    s16 max_hp;

    // perception
    f32 seen_x, seen_y, seen_z;
    s16 lost_frames;
    bool have_seen;
    bool los;

    // observation of Link (edges)
    bool prev_cut_at, prev_roll, prev_heal, arrow_near;
    s16 swing_age;
    bool swing_hit;
    f32 prev_dist;
    s16 pending_frames;
    u8 pending_reaction;

    // decisions
    s16 counter_window, counter_delay, atk_cooldown, dodge_cd, guard_cd, bait_cd, bait_window, alert_cd;
    u8 combo_left, chain_len, last_attack;
    bool has_last_attack;
    bool enraged;

    // wind-up hold
    s16 hold_left, hold_frame;
    f32 hold_rate;
    bool holding, hold_done;

    // mod-owned dodge
    s16 dodge_frames, dodge_heading;
    f32 dodge_speed;
    s8 strafe_dir;
    s16 strafe_timer;

    // shield / damage
    const void* blocked_obj;
    s16 block_frames;
    bool hit_this_frame;
    s16 hit_inv;
    s16 health_before;
    Snapshot snap;
};

nm::Pool<Brain, kPoolSize> g_pool;
bool g_pool_ready = false;

Brain* brain_of(e_dn_class* dn, bool* created = nullptr) {
    if (!g_pool_ready) {
        g_pool.clear();
        g_pool_ready = true;
    }
    return g_pool.acquire(dn, fopAcM_GetID(dn), created);
}

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------
inline fopAc_ac_c* ac(e_dn_class* dn) { return &dn->actor; }

void anm_init(e_dn_class* dn, int id, f32 morf, u8 attr, f32 rate) {
    dn->anm_p->setAnm((J3DAnmTransform*)dComIfG_getObjectRes("E_dn", id), attr, morf, rate, 0.0f, -1.0f);
    dn->anm_no = id;
}

// Same test vanilla's other_bg_check uses: eye height to Link's head, BG only.
bool line_blocked(e_dn_class* dn, const cXyz& target) {
    dBgS_LinChk lin;
    cXyz start = ac(dn)->current.pos;
    start.y = ac(dn)->eyePos.y;
    cXyz end = target;
    end.y += 100.0f;
    lin.Set(&start, &end, ac(dn));
    return dComIfG_Bgsp().LineCross(&lin);
}

// Environmental awareness: a straight run of 'len' units along 'heading' must not hit a wall at
// knee height and must end on ground that is not a deep drop.
bool dir_clear(e_dn_class* dn, s16 heading, f32 len) {
    fopAc_ac_c* a = ac(dn);
    cXyz start = a->current.pos;
    start.y += 40.0f;
    cXyz end = start;
    end.x += cM_ssin(heading) * len;
    end.z += cM_scos(heading) * len;

    dBgS_LinChk lin;
    lin.Set(&start, &end, a);
    if (dComIfG_Bgsp().LineCross(&lin)) {
        return false;
    }

    dBgS_GndChk gnd;
    cXyz probe = end;
    probe.y = a->current.pos.y + 60.0f;
    gnd.SetPos(&probe);
    const f32 gy = dComIfG_Bgsp().GroundCross(&gnd);
    if (gy == -G_CM3D_F_INF || gy < a->current.pos.y - 120.0f) {
        return false;
    }
    return true;
}

inline s16 yaw_to(const cXyz& from, const cXyz& to) { return cM_atan2s(to.x - from.x, to.z - from.z); }

// Rewrites this frame's horizontal displacement. Vanilla already moved the actor during action();
// BG collision (CrrPos) runs after that, so a rewritten displacement is still collided.
void set_horizontal_delta(e_dn_class* dn, Brain* b, f32 dx, f32 dz) {
    // Per-frame cap: a hard bound on how far any mod-driven step can ever move (anti-clipping).
    const f32 mag = sqrtf(dx * dx + dz * dz);
    if (mag > 36.0f) {
        const f32 k = 36.0f / mag;
        dx *= k;
        dz *= k;
    }
    ac(dn)->current.pos.x = b->before_x + dx;
    ac(dn)->current.pos.z = b->before_z + dz;
}

void release_actor(e_dn_class* dn, Brain* b) {
    if (b->range_applied) {
        dn->pl_range = b->base_range;
        b->range_applied = false;
    }
    if (b->holding && dn->anm_p != nullptr) {
        dn->anm_p->setPlaySpeed(b->hold_rate > 0.0f ? b->hold_rate : 1.0f);
        b->holding = false;
    }
    if (dn->action == A_FIGHT_RUN && dn->mode == MODE_NM_DODGE) {
        dn->mode = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------------------------
struct ArrowScan {
    e_dn_class* dn;
    bool found;
    bool from_front;
};

void* arrow_sub(void* actor, void* data) {
    if (!fopAcM_IsActor(actor) || fopAcM_GetName(actor) != fpcNm_ARROW_e) {
        return nullptr;
    }
    const u32 param = fopAcM_GetParam(actor);
    if (param != 1 && param != 2) {  // the same player-arrow filter vanilla's shot_s_sub uses
        return nullptr;
    }
    ArrowScan* scan = (ArrowScan*)data;
    fopAc_ac_c* arrow = (fopAc_ac_c*)actor;
    fopAc_ac_c* me = ac(scan->dn);
    cXyz to_me = me->current.pos - arrow->current.pos;
    if (to_me.abs() > 900.0f) {
        return nullptr;
    }
    cXyz moved = arrow->current.pos - arrow->old.pos;
    if (moved.x * to_me.x + moved.z * to_me.z <= 0.0f) {  // not flying toward us
        return nullptr;
    }
    scan->found = true;
    const s16 from = cM_atan2s(-to_me.x, -to_me.z);  // yaw from us to the arrow
    scan->from_front = nm::in_arc((s16)(me->shape_angle.y - from), 0x3800);
    return nullptr;  // keep iterating: fpcM_Search stops at the first non-NULL
}

struct AlertScan {
    e_dn_class* source;
};

void* alert_sub(void* actor, void* data) {
    if (!fopAcM_IsActor(actor) || fopAcM_GetName(actor) != fpcNm_E_DN_e) {
        return nullptr;
    }
    e_dn_class* other = (e_dn_class*)actor;
    AlertScan* scan = (AlertScan*)data;
    if (other == scan->source || other->status != 0 || other->actor.health <= 0) {
        return nullptr;
    }
    if (!is_idle(other->action)) {  // never wake scripted waits (ACTION_STAY) or busy actors
        return nullptr;
    }
    cXyz d = other->actor.current.pos - scan->source->actor.current.pos;
    if (d.abs() > 1500.0f) {
        return nullptr;
    }
    other->action = A_FIGHT_RUN;
    other->mode = -10;  // vanilla "found you" entry: plays Z2SE_EN_DN_V_FIND and the wait pose
    other->timer[0] = 60;
    other->unk_timer_1 = 40;
    return nullptr;
}


void observe(e_dn_class* dn, Brain* b, daPy_py_c* pl, fopAc_ac_c* link, f32 dist, s16 ang) {
    fopAc_ac_c* a = ac(dn);
    const int lvl = g_level;

    nm::mem_decay(b->mem);
    if (b->counter_window > 0) b->counter_window--;
    if (b->counter_delay > 0) b->counter_delay--;
    if (b->atk_cooldown > 0) b->atk_cooldown--;
    if (b->dodge_cd > 0) b->dodge_cd--;
    if (b->guard_cd > 0) b->guard_cd--;
    if (b->bait_cd > 0) b->bait_cd--;
    if (b->bait_window > 0) b->bait_window--;
    if (b->alert_cd > 0) b->alert_cd--;
    if (b->block_frames > 0 && --b->block_frames == 0) b->blocked_obj = nullptr;

    if (b->max_hp < a->health) b->max_hp = a->health;
    b->enraged = lvl >= nm::LEVEL_NIGHTMARE && nm::is_enraged(a->health, b->max_hp);

    // Line of sight, refreshed every 4 frames (a handful of ray casts per second per actor).
    if ((dn->frame_counter & 3) == 0) {
        b->los = !line_blocked(dn, link->current.pos);
    }
    const f32 sight = (b->base_range >= 50000.0f ? b->base_range : b->base_range * nm::sense_mul(lvl)) + 50.0f;
    if (b->los && dist < sight) {
        b->seen_x = link->current.pos.x;
        b->seen_y = link->current.pos.y;
        b->seen_z = link->current.pos.z;
        b->have_seen = true;
        b->lost_frames = 0;
    } else if (b->have_seen && b->lost_frames < 32000) {
        b->lost_frames++;
    }

    // --- Link: edges -------------------------------------------------------------------------
    const bool cut_at = pl->getCutAtFlg() != 0;
    const u16 cut_bit = cc_pl_cut_bit_get();
    const bool rolling = pl->checkFrontRoll() != 0 || pl->checkSideStep();
    const bool guarding = pl->checkPlayerGuard() != 0;
    const bool jumping = pl->checkCutJumpMode() != 0 || pl->getCutType() == daPy_py_c::CUT_TYPE_JUMP;
    const bool lizalfos_faces_link = nm::in_arc((s16)(a->shape_angle.y - ang), 0x4000);
    const bool healing = fopAcM_GetName(link) == fpcNm_ALINK_e &&
                         ((daAlink_c*)link)->mProcID == daAlink_c::PROC_BOTTLE_DRINK;

    if (cut_at && !b->prev_cut_at) {  // a swing just started
        nm::SwingKind kind = nm::SWING_SWORD;
        if (jumping || cut_bit == 0x100) {
            kind = nm::SWING_JUMP;
        } else if (cut_bit == 0x80 || cut_bit == 0x800 || pl->checkCutTurnCharge()) {
            kind = nm::SWING_SPIN;
        }
        nm::mem_note_swing(b->mem, kind, cut_bit, lizalfos_faces_link);
        b->swing_age = 0;
        b->swing_hit = false;

        // Decide how to answer it (rolled once per swing; acted on after a short reaction delay).
        const bool guard_ok = b->guard_cd == 0 && lizalfos_faces_link && dn->action == A_FIGHT_RUN;
        const bool dodge_ok = b->dodge_cd == 0 && dn->action == A_FIGHT_RUN && dn->objacch.ChkGroundHit();
        // Link keeps hitting from behind: the shield is useless there, so lean on the sidestep
        // (halving the roll doubles its chance, still capped by dodge_chance) and react a frame sooner.
        const bool rear_pressure = lvl >= nm::LEVEL_NIGHTMARE && !lizalfos_faces_link && nm::habit_rear_attacker(b->mem);
        b->pending_reaction = (u8)nm::react_to_swing(b->mem, lvl, kind, dist, guard_ok, dodge_ok, cM_rndF(1.0f),
                                                     cM_rndF(1.0f) * (rear_pressure ? 0.5f : 1.0f));
        b->pending_frames = lvl >= nm::LEVEL_NIGHTMARE ? (rear_pressure ? 1 : 3) : 8;
    } else if (!cut_at && b->prev_cut_at) {  // the swing just ended
        if (!b->swing_hit && lvl >= nm::LEVEL_NIGHTMARE && dist < kEngageRange + 80.0f &&
            cM_rndF(1.0f) < nm::counter_chance(lvl, b->enraged)) {
            b->counter_window = 30;  // missed swing: an opening
            b->counter_delay = 2 + (s16)cM_rndF(5.0f);
        }
    }
    if (cut_at && b->swing_age < 600) b->swing_age++;
    b->prev_cut_at = cut_at;

    if (rolling && !b->prev_roll) {
        nm::mem_note_roll(b->mem);
    } else if (!rolling && b->prev_roll && lvl >= nm::LEVEL_NIGHTMARE && dist < kEngageRange &&
               cM_rndF(1.0f) < nm::counter_chance(lvl, b->enraged)) {
        b->counter_window = 20;  // roll recovery is an opening
        b->counter_delay = 1 + (s16)cM_rndF(3.0f);
    }
    b->prev_roll = rolling;

    if (guarding) nm::mem_note_guarding(b->mem);

    // Link backing away: moving, and heading away from us.
    if (link->speedF > 4.0f && !nm::in_arc((s16)(link->current.angle.y - (s16)(ang + 0x8000)), 0x5000)) {
        nm::mem_note_retreat(b->mem);
    }

    if (healing && !b->prev_heal) {
        nm::mem_note_heal(b->mem);
        if (lvl >= nm::LEVEL_NIGHTMARE && dist < 1200.0f && b->los) {
            b->counter_window = 90;  // punish healing: press the attack
            b->counter_delay = 0;
            b->atk_cooldown = 0;
        }
    }
    b->prev_heal = healing;

    // Arrows: every 4 frames, any player arrow flying toward us.
    if ((dn->frame_counter & 3) == 1) {
        ArrowScan scan = {dn, false, false};
        fpcM_Search(arrow_sub, &scan);
        if (scan.found && !b->arrow_near) {
            nm::mem_note_arrow(b->mem);
            if (b->los && dn->action == A_FIGHT_RUN) {
                if (scan.from_front && b->guard_cd == 0) {
                    b->pending_reaction = nm::REACT_GUARD;
                    b->pending_frames = lvl >= nm::LEVEL_NIGHTMARE ? 2 : 6;
                } else if (!scan.from_front && b->dodge_cd == 0 && dn->objacch.ChkGroundHit() &&
                           lvl >= nm::LEVEL_NIGHTMARE) {
                    b->pending_reaction = nm::REACT_DODGE_SIDE;
                    b->pending_frames = 2;
                }
            }
        }
        b->arrow_near = scan.found;
    }

    b->prev_dist = dist;
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------
void begin_attack(e_dn_class* dn, Brain* b, nm::Tactic t, bool chained) {
    const int lvl = g_level;
    b->last_attack = (u8)t;
    b->has_last_attack = true;
    if (!chained) {
        b->chain_len = 0;
        b->combo_left = (u8)nm::combo_extra(lvl, b->enraged, b->mem, cM_rndF(1.0f));
    }
    b->hold_done = false;
    b->holding = false;
    b->hold_left = 0;
    b->hold_frame = (s16)hold_frame_of(t);
    if (lvl >= nm::LEVEL_NIGHTMARE && cM_rndF(1.0f) < nm::delay_chance(b->mem, b->enraged)) {
        b->hold_left = 6 + (s16)cM_rndF(9.0f);  // 6..14 frames, hard-bounded
    }
    (void)dn;
}

void launch_attack(e_dn_class* dn, Brain* b, f32 dist, bool link_behind, bool chained) {
    nm::Tactic t = nm::pick_tactic(dist, link_behind, nm::habit_turtle(b->mem),
                                   (nm::Tactic)b->last_attack, b->has_last_attack, cM_rndF(1.0f));
    // Same reach limits vanilla applies: the quick stab is short, the tail sweep is point-blank.
    if (t == nm::TACTIC_TAIL && dist > 300.0f && !link_behind) t = nm::TACTIC_STEP;
    if (t == nm::TACTIC_QUICK && dist > 330.0f) t = nm::TACTIC_STEP;
    dn->action = (s16)action_of(t);
    dn->mode = 0;
    begin_attack(dn, b, t, chained);
    b->counter_window = 0;
    b->bait_window = 0;
}

void start_guard(e_dn_class* dn, Brain* b) {
    // Mirrors vanilla's own fight_run -> ACTION_GUARD entry, minus its 15 frames of invulnerability.
    dn->action = A_GUARD;
    dn->mode = (cc_pl_cut_bit_get() == 0x100) ? 0 : 5;
    dn->field_0x708 = 30.0f;
    dn->field_0x70c = ac(dn)->shape_angle.y;
    dn->timer[0] = 0;
    b->guard_cd = g_level >= nm::LEVEL_NIGHTMARE ? 60 : 110;
}

bool start_dodge(e_dn_class* dn, Brain* b, nm::Reaction r, s16 ang_to_link) {
    // Candidate headings in preference order.
    s16 cand[3];
    if (r == nm::REACT_DODGE_BACK) {
        cand[0] = (s16)(ang_to_link + 0x8000);
        cand[1] = (s16)(ang_to_link + 0x8000 + 0x2800);
        cand[2] = (s16)(ang_to_link + 0x8000 - 0x2800);
    } else {
        const s16 side = b->strafe_dir >= 0 ? 0x4000 : -0x4000;
        cand[0] = (s16)(ang_to_link + side);
        cand[1] = (s16)(ang_to_link - side);
        cand[2] = (s16)(ang_to_link + 0x8000);
    }
    for (int i = 0; i < 3; i++) {
        if (!dir_clear(dn, cand[i], 230.0f)) {
            continue;
        }
        b->dodge_heading = cand[i];
        b->dodge_frames = 0;
        b->dodge_speed = r == nm::REACT_DODGE_BACK ? 21.0f : 19.0f;
        dn->action = A_FIGHT_RUN;
        dn->mode = MODE_NM_DODGE;
        anm_init(dn, ANM_WAIT_02, 2.0f, J3DFrameCtrl::EMode_LOOP, 1.8f);  // the existing hop, faster
        ac(dn)->speed.y = 11.0f;
        b->dodge_cd = g_level >= nm::LEVEL_NIGHTMARE ? 80 : 150;
        return true;
    }
    return false;  // boxed in: no dodge (never clips, never teleports)
}

// ---------------------------------------------------------------------------------------------
// Decision (runs in PRE, only while vanilla is in its plain chase state)
// ---------------------------------------------------------------------------------------------
void decide(e_dn_class* dn, Brain* b, f32 dist, s16 ang) {
    const int lvl = g_level;
    const bool facing = nm::in_arc((s16)(ac(dn)->shape_angle.y - ang), kArcFront);
    const bool link_behind = !nm::in_arc((s16)(ac(dn)->shape_angle.y - ang), 0x5000);

    // 1) A reaction to Link's swing or arrow, after the reaction delay.
    if (b->pending_reaction != nm::REACT_NONE) {
        if (b->pending_frames > 0) {
            b->pending_frames--;
        } else {
            const nm::Reaction r = (nm::Reaction)b->pending_reaction;
            b->pending_reaction = nm::REACT_NONE;
            if (r == nm::REACT_GUARD) {
                start_guard(dn, b);
                return;
            }
            if (start_dodge(dn, b, r, ang)) {
                return;
            }
        }
    }

    if (lvl < nm::LEVEL_NIGHTMARE) {
        return;  // Advanced stops at reactions; vanilla picks the attacks
    }

    // Footwork variety: flip the preferred circling side now and then.
    if (--b->strafe_timer <= 0) {
        b->strafe_timer = 45 + (s16)cM_rndF(60.0f);
        b->strafe_dir = cM_rndF(1.0f) < 0.5f ? -1 : 1;
    }

    // 2) Punish an opening, once the counter delay has passed.
    if (b->counter_window > 0 && b->counter_delay == 0 && dist < kEngageRange && b->los && facing) {
        launch_attack(dn, b, dist, link_behind, false);
        return;
    }

    // 3) A false opening: hold position for a moment (vanilla's own attack gate is held shut),
    //    then lunge as soon as Link commits.
    if (b->bait_window > 0) {
        if (dn->timer[2] < 2) dn->timer[2] = 2;
        if (b->bait_window == 1) {
            b->counter_window = 40;
            b->counter_delay = 0;
        }
        return;
    }
    if (b->bait_cd == 0 && dist < kEngageRange && dist > 220.0f && b->los && !b->enraged &&
        cM_rndF(1.0f) < nm::bait_chance(b->enraged) * 0.05f) {
        b->bait_window = 18;
        b->bait_cd = 360;
        return;
    }

    // 4) Engage: closer, sooner than vanilla's 30..60 frame gap.
    if (b->atk_cooldown == 0 && dist < kEngageRange && b->los && facing && dn->timer[2] == 0) {
        dn->timer[2] = 20 + (s16)cM_rndF(20.0f);
        launch_attack(dn, b, dist, link_behind, false);
    }
}

void alert_others(e_dn_class* dn, Brain* b) {
    if (b->alert_cd > 0) {
        return;
    }
    b->alert_cd = 120;
    AlertScan scan = {dn};
    fpcM_Search(alert_sub, &scan);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Hook entry points
// ---------------------------------------------------------------------------------------------
void action_pre(e_dn_class* dn) {
    Brain* b = brain_of(dn);
    b->active = false;
    b->snap.valid = false;

    const int lvl = g_level;
    if (lvl <= nm::LEVEL_OFF) {
        release_actor(dn, b);
        return;
    }
    if (dn->status != 0 || dn->anm_p == nullptr || dn->actor.health <= 0 || dn->death_flag != 0) {
        return;  // not running yet, or dying: vanilla only
    }
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == nullptr || dComIfGp_event_runCheck() || dComIfGs_getLife() == 0) {
        return;  // cutscene / Link dead: leave everything to vanilla
    }
    daPy_py_c* pl = (daPy_py_c*)link;
    fopAc_ac_c* a = ac(dn);

    // First sight of this actor, or a difficulty change.
    if (b->base_range <= 0.0f) {
        b->base_range = dn->pl_range;
        b->max_hp = a->health;
        b->los = true;
        b->strafe_dir = 1;
        b->strafe_timer = 30;
    }
    if (b->level != (u8)lvl) {
        b->level = (u8)lvl;
        b->counter_window = b->atk_cooldown = 0;
        b->combo_left = b->chain_len = 0;
        b->pending_reaction = nm::REACT_NONE;
    }
    // Sight range: vanilla's own pl_check()s read pl_range, so widening it widens detection.
    // "Unlimited" (>= 50000, set by vanilla for param 0xFF) is left alone.
    if (b->base_range < 50000.0f) {
        dn->pl_range = b->base_range * nm::sense_mul(lvl);
        b->range_applied = true;
    }

    b->active = true;
    b->before_x = a->current.pos.x;
    b->before_y = a->current.pos.y;
    b->before_z = a->current.pos.z;
    b->cur_yaw_before = a->current.angle.y;
    b->shape_yaw_before = a->shape_angle.y;
    b->hit_this_frame = false;
    b->health_before = a->health;

    const f32 dist = fopAcM_searchPlayerDistanceXZ(a);
    const s16 ang = fopAcM_searchPlayerAngleY(a);

    observe(dn, b, pl, link, dist, ang);

    // Idle -> alerted is noticed in POST; here: keep chasing what we last saw. Vanilla gives up 40
    // frames after losing sight; extend that, but only while there is somewhere left to look.
    if (dn->action == A_FIGHT_RUN && b->have_seen && !b->los && b->lost_frames < nm::interest_frames(lvl)) {
        const f32 dx = b->seen_x - a->current.pos.x;
        const f32 dz = b->seen_z - a->current.pos.z;
        if (dx * dx + dz * dz > 140.0f * 140.0f && dn->unk_timer_1 < 2) {
            dn->unk_timer_1 = 2;
        }
    }

    if (dn->action == A_FIGHT_RUN && dn->mode >= 0 && dn->mode != MODE_NM_DODGE) {
        decide(dn, b, dist, ang);
    } else if (dn->action != A_FIGHT_RUN) {
        b->pending_reaction = nm::REACT_NONE;
    }

    b->pre_action = dn->action;
    b->pre_mode = dn->mode;
}

void action_post(e_dn_class* dn) {
    Brain* b = brain_of(dn);
    if (!b->active) {
        return;
    }
    b->active = false;
    const int lvl = g_level;
    fopAc_ac_c* a = ac(dn);
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == nullptr || dn->anm_p == nullptr) {
        return;
    }
    const f32 dist = fopAcM_searchPlayerDistanceXZ(a);
    const s16 ang = fopAcM_searchPlayerAngleY(a);
    const int cur = dn->action;

    // --- noticing Link: wake the neighbours ---------------------------------------------------
    if (is_idle(b->pre_action) && cur == A_FIGHT_RUN) {
        b->lost_frames = 0;
        if (lvl >= nm::LEVEL_NIGHTMARE) {
            alert_others(dn, b);
        }
    }

    // --- attacks: vanilla started one on its own / one just finished --------------------------
    if (is_attack(cur) && !is_attack(b->pre_action)) {
        begin_attack(dn, b, tactic_of(cur), false);
    }
    if (is_attack(b->pre_action) && cur == A_FIGHT_RUN && dn->mode == 0) {
        // The attack ran to its end (or was parried): chain, or settle into a short cooldown.
        b->holding = false;
        const bool can_chain = lvl >= nm::LEVEL_NIGHTMARE && b->combo_left > 0 && b->chain_len < 2 && b->los &&
                               dist < kEngageRange + 60.0f && nm::in_arc((s16)(a->shape_angle.y - ang), kArcFront);
        if (can_chain) {
            b->combo_left--;
            b->chain_len++;
            launch_attack(dn, b, dist, !nm::in_arc((s16)(a->shape_angle.y - ang), 0x5000), true);
        } else {
            b->chain_len = 0;
            b->combo_left = 0;
            b->atk_cooldown = b->enraged ? 12 : (lvl >= nm::LEVEL_NIGHTMARE ? 24 : 0);
            const s16 gap = b->enraged ? 12 : (lvl >= nm::LEVEL_NIGHTMARE ? 26 : 60);
            if (dn->timer[2] > gap) dn->timer[2] = gap;
        }
    }

    // --- delayed attacks: hold the wind-up for a bounded number of frames ---------------------
    if (is_attack(dn->action) && b->hold_left > 0 && !b->hold_done) {
        if ((int)dn->anm_p->getFrame() >= b->hold_frame) {
            if (!b->holding) {
                b->hold_rate = dn->anm_p->getPlaySpeed();
                if (b->hold_rate <= 0.0f) b->hold_rate = 1.0f;
                b->holding = true;
            }
            dn->anm_p->setPlaySpeed(0.0f);
            if (--b->hold_left == 0) {
                dn->anm_p->setPlaySpeed(b->hold_rate);
                b->holding = false;
                b->hold_done = true;
            }
        }
    } else if (b->holding) {  // the action ended mid-hold (hit, parry...): give the speed back
        if (dn->anm_p->getPlaySpeed() == 0.0f) dn->anm_p->setPlaySpeed(b->hold_rate > 0.0f ? b->hold_rate : 1.0f);
        b->holding = false;
    }

    // --- guard: no omnidirectional invulnerability -------------------------------------------
    if (cur == A_GUARD) {
        if (b->hit_this_frame) {
            dn->invulnerability_timer = b->hit_inv;  // normal post-hit recovery, as outside a guard
        } else if (dn->invulnerability_timer == 5 || dn->invulnerability_timer == 15) {
            dn->invulnerability_timer = 0;  // vanilla's guard-only values; blocking is by collision
        }
        nm::mem_note_guarding(b->mem);
    }

    // --- horizontal movement overrides -------------------------------------------------------
    const f32 dx0 = a->current.pos.x - b->before_x;
    const f32 dz0 = a->current.pos.z - b->before_z;

    if (cur == A_FIGHT_RUN && dn->mode == MODE_NM_DODGE) {
        if (dn->timer[2] < 2) dn->timer[2] = 2;  // keep vanilla from starting an attack mid-dodge
        const int f = ++b->dodge_frames;
        const f32 v = f <= 8 ? b->dodge_speed : b->dodge_speed * (14 - f) / 6.0f;
        bool stop = f >= 14 || !dir_clear(dn, b->dodge_heading, 90.0f);
        if (!stop) {
            set_horizontal_delta(dn, b, cM_ssin(b->dodge_heading) * v, cM_scos(b->dodge_heading) * v);
        } else {
            dn->mode = 0;  // hand back to vanilla's chase (it re-inits the run animation)
            if (lvl >= nm::LEVEL_NIGHTMARE && dist < kEngageRange && cM_rndF(1.0f) < 0.5f) {
                b->counter_window = 24;  // a good dodge leaves the attacker open
                b->counter_delay = 4;
            }
        }
        return;
    }

    if (cur != A_FIGHT_RUN) {
        return;
    }

    const bool dash = dn->mode == 0 || dn->mode == 1;
    if (dash) {
        f32 mul = nm::speed_mul(lvl, b->enraged);
        if (nm::habit_runner(b->mem)) mul = nm::clampf(mul + 0.08f, 1.0f, 1.3f);  // chase a runner harder
        if (b->prev_heal) mul = nm::clampf(mul + 0.1f, 1.0f, 1.3f);
        const f32 mag = sqrtf(dx0 * dx0 + dz0 * dz0);
        f32 ndx = dx0 * mul;
        f32 ndz = dz0 * mul;

        // Lost sight: run toward where Link was last seen, not toward Link (no wallhack).
        const bool chasing_memory = b->have_seen && !b->los && b->lost_frames >= 8 && lvl >= nm::LEVEL_ADVANCED;
        if (chasing_memory && mag > 0.01f) {
            cXyz seen(b->seen_x, b->seen_y, b->seen_z);
            const s16 to_seen = yaw_to(a->current.pos, seen);
            ndx = cM_ssin(to_seen) * mag * mul;
            ndz = cM_scos(to_seen) * mag * mul;
            a->current.angle.y = b->cur_yaw_before;
            cLib_addCalcAngleS2(&a->current.angle.y, to_seen, 2, 0x800);
            a->shape_angle.y = b->shape_yaw_before;
            cLib_addCalcAngleS2(&a->shape_angle.y, a->current.angle.y, 2, 0x2000);
        } else if (nm::habit_archer(b->mem) && dist > 350.0f && mag > 0.01f && lvl >= nm::LEVEL_NIGHTMARE) {
            // Link favours arrows: weave while closing in so a straight shot misses.
            const s16 weave = (s16)(cM_ssin((s16)(dn->frame_counter * 0x0A00)) * 0x1800);
            const s16 h = (s16)(cM_atan2s(dx0, dz0) + weave);
            ndx = cM_ssin(h) * mag * mul;
            ndz = cM_scos(h) * mag * mul;
        }
        if (mul > 1.0f || chasing_memory || (nm::habit_archer(b->mem) && dist > 350.0f)) {
            set_horizontal_delta(dn, b, ndx, ndz);
        }
    } else if (dn->mode == 2 && lvl >= nm::LEVEL_NIGHTMARE && dist > 150.0f && dist < 650.0f) {
        // Circling hop (vanilla's "wait for an opening" mode): drift around Link instead of
        // bobbing in place, on the airborne frames of each hop only.
        const int fr = (int)dn->anm_p->getFrame();
        if ((fr >= 7 && fr <= 13) || (fr >= 21 && fr <= 27)) {
            // Prefer to end up behind Link when he keeps turtling or rolling; otherwise wander.
            const s16 bearing = (s16)(ang + 0x8000);  // from Link to us
            s16 target = (s16)(bearing + (b->strafe_dir >= 0 ? 0x4000 : -0x4000));
            if (nm::habit_turtle(b->mem) || nm::habit_roller(b->mem)) {
                target = (s16)(link->shape_angle.y + 0x8000);
            }
            const s16 h = nm::strafe_heading(ang, bearing, target, b->strafe_dir);
            if (dir_clear(dn, h, 110.0f)) {
                set_horizontal_delta(dn, b, dx0 * 0.3f + cM_ssin(h) * 9.0f, dz0 * 0.3f + cM_scos(h) * 9.0f);
            } else {
                b->strafe_dir = (s8)-b->strafe_dir;  // wall or ledge that way: circle the other way
            }
        }
    }
}

void damage_pre(e_dn_class* dn) {
    Brain* b = brain_of(dn);
    b->snap.valid = false;
    const int lvl = g_level;
    if (!b->active) {
        return;  // disabled, cutscene, dying, or not running yet: vanilla only
    }
    fopAc_ac_c* a = ac(dn);

    // Shield by real collision: a hit is absorbed only when the very attack object that touched
    // the body also touched the shield sphere (or is the swing already blocked a moment ago).
    // Anything that reaches the body without crossing the shield lands normally.
    bool any_hit = false;
    for (int i = 0; i < 3; i++) {
        if (!dn->cc_sph[i].ChkTgHit()) {
            continue;
        }
        any_hit = true;
        const void* obj = dn->cc_sph[i].GetTgHitObj();
        const bool same_swing = obj != nullptr && obj == b->blocked_obj && b->block_frames > 0;
        const bool on_shield = dn->action == A_GUARD && dn->tate_sph.ChkTgHit() &&
                               dn->tate_sph.GetTgHitObj() == dn->cc_sph[i].GetTgHitObj();
        if (same_swing || on_shield) {
            if (on_shield) {
                b->blocked_obj = obj;
                b->block_frames = 10;  // the swing keeps overlapping for a few frames
            }
            dn->cc_sph[i].ClrTgHit();
            b->swing_hit = true;
        } else {
            b->swing_hit = true;
        }
    }
    (void)any_hit;

    // Poise snapshot (Nightmare, combat actions only, still alive). Restored in POST if vanilla
    // answers an ordinary hit with a stagger.
    if (lvl >= nm::LEVEL_NIGHTMARE && is_combat(dn->action) && a->health > 0) {
        Snapshot& s = b->snap;
        s.valid = true;
        s.action = dn->action;
        s.mode = dn->mode;
        s.health = a->health;
        s.inv = dn->invulnerability_timer;
        s.unk_timer_4 = dn->unk_timer_4;
        s.speedF = a->speedF;
        s.speedY = a->speed.y;
        s.f704 = dn->field_0x704;
        s.f708 = dn->field_0x708;
        s.f74c = dn->field_0x74c;
        s.f70c = dn->field_0x70c;
        s.f70e = dn->field_0x70e;
        s.f750 = dn->field_0x750;
        s.f724y = dn->field_0x724.y;
        s.angle_y = a->current.angle.y;
        s.f710 = dn->field_0x710;
    }
}

// Link attacks that vanilla (and the Lazy Tweaks fork) deliberately treat as knock-down moves.
bool heavy_cut(daPy_py_c* pl) {
    switch (pl->getCutType()) {
    case daPy_py_c::CUT_TYPE_JUMP:
    case daPy_py_c::CUT_TYPE_HEAD_JUMP:  // helm splitter
    case daPy_py_c::CUT_TYPE_TURN_LEFT:  // spin attack
    case daPy_py_c::CUT_TYPE_TURN_RIGHT:
    case daPy_py_c::CUT_TYPE_MORTAL_DRAW_A:  // hidden skills
    case daPy_py_c::CUT_TYPE_MORTAL_DRAW_B:
        return true;
    default:
        return false;
    }
}

void damage_post(e_dn_class* dn) {
    Brain* b = brain_of(dn);
    fopAc_ac_c* a = ac(dn);
    if (!b->active) {
        return;
    }

    // A hit landed this frame (health went down): remember the normal post-hit recovery time that
    // vanilla / the fork chose, ignoring the 1000-frame lock that a knock-down sets.
    if (a->health < b->health_before) {
        b->hit_this_frame = true;
        b->swing_hit = true;
        s16 inv = dn->invulnerability_timer;
        if (inv >= 100 || inv <= 0) {
            const bool strong = dn->at_info.mpCollider != nullptr && dn->at_info.mpCollider->ChkAtType(AT_TYPE_UNK);
            inv = (strong && dn->at_info.mAttackPower > 1) ? 20 : 10;  // vanilla's own values
        }
        b->hit_inv = inv;
    }

    Snapshot& s = b->snap;
    if (!s.valid) {
        return;
    }
    s.valid = false;

    const bool small = dn->action == A_S_DAMAGE;
    const bool knockdown = dn->action == A_DAMAGE;
    const bool wolf_kick = knockdown && dn->field_0x710 != 0;  // vanilla wolf-kick special case
    if ((!small && !knockdown) || wolf_kick || a->health <= 0 || s.action == A_S_DAMAGE || s.action == A_DAMAGE) {
        return;  // not a stagger, lethal hit, special interaction, or already staggered: vanilla owns it
    }
    // Heavy attacks (jump strike, helm splitter, spin, hidden skills) keep their knock-down so
    // Link's finishers stay available; everything else is absorbed.
    if (knockdown && heavy_cut((daPy_py_c*)dComIfGp_getPlayer(0))) {
        return;
    }

    // Poise: keep the damage vanilla already applied, undo only the stagger.
    dn->action = s.action;
    dn->mode = s.mode;
    dn->unk_timer_4 = s.unk_timer_4;
    a->speedF = s.speedF;
    a->speed.y = s.speedY;
    dn->field_0x704 = s.f704;
    dn->field_0x708 = s.f708;
    dn->field_0x74c = s.f74c;
    dn->field_0x70c = s.f70c;
    dn->field_0x70e = s.f70e;
    dn->field_0x750 = s.f750;
    dn->field_0x724.y = s.f724y;
    dn->field_0x710 = s.f710;
    a->current.angle.y = s.angle_y;
    dn->invulnerability_timer = b->hit_this_frame ? b->hit_inv : s.inv;  // normal recovery, never 1000
    dn->sound.startCreatureVoice(Z2SE_EN_DN_V_DAMAGE, -1);  // the existing pain voice
}

void actor_delete(e_dn_class* dn) {
    if (!g_pool_ready) {
        return;
    }
    Brain* b = brain_of(dn);
    release_actor(dn, b);
    g_pool.release(dn);
}

void shutdown() {
    if (!g_pool_ready) {
        return;
    }
    for (auto& slot : g_pool.slots) {
        if (!slot.used) {
            continue;
        }
        base_process_class* proc = fpcM_SearchByID(slot.id);
        if (proc != nullptr && (uintptr_t)proc == slot.addr && fpcM_GetName(proc) == fpcNm_E_DN_e) {
            release_actor((e_dn_class*)proc, &slot.value);
        }
    }
    g_pool.clear();
}

}  // namespace lnm
