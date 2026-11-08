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
// Difficulty levels:
//   Advanced  - the "fair" tier: the shield only cancels a hit whose attack object really struck
//               the shield sphere, no invulnerability, normal damage.
//   Nightmare - the "read and punish" tier (modelled on a Darknut duelist mod): the moment Link
//               presses an attack it rolls to read it, snaps to face him and guards from any angle
//               (the game's own guard state and invulnerability) or dodges, then punishes the miss.
//               After a run of reads it leaves a short opening, and every hit Link lands resets the
//               run, so it can always be beaten. It also swings faster, chains up to 5-7 swings,
//               runs faster and hits harder (configurable). Its health is never changed.
// Everywhere: nothing teleports (every mod-driven step is capped), no new hitbox, animation or
// sound is created, and every hit that lands still reduces health.
//
// Nothing is allocated per frame. Per-actor state lives in a fixed pool keyed by actor address
// and process id.

// Dusklight's dolphin/types.h (force-included on every translation unit) can define `nullptr` as a
// macro on MSVC, which breaks standard-library headers. Drop it before anything else is parsed.
#ifdef nullptr
#undef nullptr
#endif

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

#define NM_DBG(...) \
    do { \
        if (g_debug && g_log) g_log(__VA_ARGS__); \
    } while (0)

namespace lnm {

int g_level = nm::LEVEL_NIGHTMARE;
int g_damage_mult = 3;
void (*g_log)(const char* fmt, ...) = nullptr;
bool g_debug = true;
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
constexpr float kEngageRange = 620.0f;  // vanilla attack_init_range is 350; Nightmare closes in
constexpr s16 kArmTicks = 40;            // how long a read keeps the guard up (~1.3 s at 30 fps)
constexpr f32 kReadRange = 560.0f;       // swings it reads from up to here
constexpr f32 kReadRangeWide = 680.0f;   // jump strikes and spins
constexpr f32 kReadFaceCos = -0.1f;      // a narrow swing only counts if Link faces roughly toward it
constexpr int kArcFront = 0x3000;       // ~67 degrees either side counts as "facing"

// True on the 1st, (n+1)th, ... call for this counter: keeps the log readable.
inline bool every(int& counter, int n) { return (counter++ % n) == 0; }

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
// Last frame of an attack's hit window (vanilla's own at_chk_flag ranges); after it the swing is recovering.
inline int attack_end_frame(int a) { return a == A_ATTACK_0 ? 28 : (a == A_ATTACK ? 30 : 41); }
inline int attack_start_frame(int a) { return a == A_ATTACK_0 ? 22 : (a == A_ATTACK ? 13 : 33); }

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

    // Nightmare read-and-punish state
    f32 rage;                  // 0..1, grows with every hit Link lands (1 at low health)
    u8 hits;                   // hits Link has landed on this Lizalfos
    s16 read_cd, expose, streak, streak_age, dodge_recent, landed_age;
    u8 landed[4];              // how often each kind of swing got through lately
    u8 last_kind;
    bool fast_swing, ran;
    s16 prev_cut_type;
    f32 atk_rate_set;          // the animation rate the mod last set for the current swing

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

bool read_swing(e_dn_class* dn, Brain* b, fopAc_ac_c* link, nm::SwingKind kind, f32 dist, s16 ang);

// Rewrites this frame's horizontal displacement. Vanilla already moved the actor during action();
// BG collision (CrrPos) runs after that, so a rewritten displacement is still collided.
void set_horizontal_delta(e_dn_class* dn, Brain* b, f32 dx, f32 dz, f32 cap = 36.0f) {
    // Per-frame cap: a hard bound on how far any mod-driven step can ever move (anti-clipping).
    const f32 mag = sqrtf(dx * dx + dz * dz);
    if (mag > cap) {
        const f32 k = cap / mag;
        dx *= k;
        dz *= k;
    }
    ac(dn)->current.pos.x = b->before_x + dx;
    ac(dn)->current.pos.z = b->before_z + dz;
}

// A fast step (this frame's displacement, measured from where the frame started) may only replace
// vanilla's own step if the straight path is free of walls and the landing spot has ground under it.
bool step_clear(e_dn_class* dn, Brain* b, f32 dx, f32 dz) {
    fopAc_ac_c* a = ac(dn);
    cXyz start(b->before_x, b->before_y + 40.0f, b->before_z);
    cXyz end(b->before_x + dx, b->before_y + 40.0f, b->before_z + dz);
    dBgS_LinChk lin;
    lin.Set(&start, &end, a);
    if (dComIfG_Bgsp().LineCross(&lin)) {
        return false;
    }
    dBgS_GndChk gnd;
    cXyz probe(end.x, b->before_y + 60.0f, end.z);
    gnd.SetPos(&probe);
    const f32 gy = dComIfG_Bgsp().GroundCross(&gnd);
    return !(gy == -G_CM3D_F_INF || gy < b->before_y - 120.0f);
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
    if (b->read_cd > 0) b->read_cd--;
    if (b->expose > 0) b->expose--;
    if (b->dodge_recent > 0) b->dodge_recent--;
    if (b->streak > 0 && ++b->streak_age > 150) {
        b->streak = 0;
        b->streak_age = 0;
    }
    if (b->landed_age > 0 && --b->landed_age == 0) {
        for (int i = 0; i < 4; i++) b->landed[i] = 0;
    }

    if (b->max_hp < a->health) b->max_hp = a->health;
    b->enraged = lvl >= nm::LEVEL_NIGHTMARE && nm::is_enraged(a->health, b->max_hp);
    b->rage = lvl >= nm::LEVEL_NIGHTMARE ? nm::rage_of(b->hits, b->enraged) : 0.0f;

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

    // A swing starts when the attack flag rises, or when the swing type changes mid-combo.
    const int cut_type = (int)pl->getCutType();
    const bool new_swing = cut_at && (!b->prev_cut_at || (cut_type != 0 && b->prev_cut_type != 0 && cut_type != b->prev_cut_type));
    if (new_swing) {  // a swing just started
        nm::SwingKind kind = nm::SWING_SWORD;
        if (jumping || cut_bit == 0x100) {
            kind = nm::SWING_JUMP;
        } else if (cut_bit == 0x80 || cut_bit == 0x800 || pl->checkCutTurnCharge()) {
            kind = nm::SWING_SPIN;
        }
        nm::mem_note_swing(b->mem, kind, cut_bit, lizalfos_faces_link);
        b->swing_age = 0;
        b->swing_hit = false;

        if (lvl >= nm::LEVEL_NIGHTMARE) {
            // Read it the moment it is pressed and answer at once (guard from any angle, or dodge).
            b->last_kind = (u8)kind;
            const bool read = read_swing(dn, b, link, kind, dist, ang);
            NM_DBG("[NM] Link swing kind=%d dist=%.0f -> %s (streak=%d expose=%d rage=%.1f)", (int)kind, dist,
                   read ? "READ" : "no read", (int)b->streak, (int)b->expose, b->rage);
        } else {
        // Decide how to answer it (rolled once per swing; acted on after a short reaction delay).
        const bool guard_ok = b->guard_cd == 0 && lizalfos_faces_link && dn->action == A_FIGHT_RUN;
        const bool dodge_ok = b->dodge_cd == 0 && dn->action == A_FIGHT_RUN && dn->objacch.ChkGroundHit();
        // Link keeps hitting from behind: the shield is useless there, so lean on the sidestep
        // (halving the roll doubles its chance, still capped by dodge_chance) and react a frame sooner.
        const bool rear_pressure = lvl >= nm::LEVEL_NIGHTMARE && !lizalfos_faces_link && nm::habit_rear_attacker(b->mem);
        b->pending_reaction = (u8)nm::react_to_swing(b->mem, lvl, kind, dist, guard_ok, dodge_ok, cM_rndF(1.0f),
                                                     cM_rndF(1.0f) * (rear_pressure ? 0.5f : 1.0f));
        b->pending_frames = lvl >= nm::LEVEL_NIGHTMARE ? (rear_pressure ? 1 : 3) : 8;
        NM_DBG("[NM] Link swing kind=%d dist=%.0f facing=%d -> reaction=%d (0 none,1 guard,2 side,3 back)", (int)kind, dist,
               (int)lizalfos_faces_link, (int)b->pending_reaction);
        }
    } else if (!cut_at && b->prev_cut_at) {  // the swing just ended
        if (!b->swing_hit && lvl >= nm::LEVEL_NIGHTMARE && dist < kEngageRange + 80.0f &&
            cM_rndF(1.0f) < nm::counter_chance(lvl, b->enraged)) {
            b->counter_window = 30;  // missed swing (or one it blocked): an opening
            b->counter_delay = 1 + (s16)cM_rndF(3.0f);
        }
    }
    if (cut_at && b->swing_age < 600) b->swing_age++;
    b->prev_cut_at = cut_at;
    b->prev_cut_type = cut_at ? (s16)cut_type : 0;

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
            if (b->los && dn->action == A_FIGHT_RUN && lvl >= nm::LEVEL_NIGHTMARE) {
                // Nightmare answers every arrow: mostly the guard (from any angle), sometimes a hop.
                if (b->dodge_cd == 0 && dn->objacch.ChkGroundHit() && cM_rndF(1.0f) < 0.35f) {
                    b->pending_reaction = nm::REACT_DODGE_SIDE;
                } else {
                    b->pending_reaction = nm::REACT_GUARD;
                }
                b->pending_frames = 0;
            } else if (b->los && dn->action == A_FIGHT_RUN) {
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
        if (lvl >= nm::LEVEL_NIGHTMARE) {
            // Relentless: up to chain_max swings, each following the last with 95%.
            const int mx = nm::chain_max(b->rage);
            int n = 0;
            while (n < mx && cM_rndF(1.0f) < nm::kChainChance) n++;
            b->combo_left = (u8)n;
        } else {
            b->combo_left = (u8)nm::combo_extra(lvl, b->enraged, b->mem, cM_rndF(1.0f));
        }
    }
    b->hold_done = false;
    b->holding = false;
    b->hold_left = 0;
    b->hold_frame = (s16)hold_frame_of(t);
    b->atk_rate_set = 0.0f;
    b->fast_swing = lvl >= nm::LEVEL_NIGHTMARE && cM_rndF(1.0f) < 0.5f;  // half the swings come out extra fast
    if (lvl >= nm::LEVEL_NIGHTMARE && cM_rndF(1.0f) < nm::delay_chance(b->mem, b->enraged)) {
        b->hold_left = 3 + (s16)cM_rndF(6.0f);  // 3..8 frames, hard-bounded
    }
    (void)dn;
}

void launch_attack(e_dn_class* dn, Brain* b, f32 dist, bool link_behind, bool chained) {
    nm::Tactic t = nm::pick_tactic(dist, link_behind, nm::habit_turtle(b->mem),
                                   (nm::Tactic)b->last_attack, b->has_last_attack, cM_rndF(1.0f));
    // Same reach limits vanilla applies: the quick stab is short, the tail sweep is point-blank.
    if (t == nm::TACTIC_TAIL && dist > 300.0f && !link_behind) t = nm::TACTIC_STEP;
    if (t == nm::TACTIC_QUICK && dist > 330.0f) t = nm::TACTIC_STEP;
    if (chained && g_level >= nm::LEVEL_NIGHTMARE) {
        const s16 to_link = fopAcM_searchPlayerAngleY(ac(dn));  // each chained swing turns to face Link
        ac(dn)->shape_angle.y = to_link;
        ac(dn)->current.angle.y = to_link;
    }
    dn->action = (s16)action_of(t);
    dn->mode = 0;
    begin_attack(dn, b, t, chained);
    NM_DBG("[NM] attack tactic=%d dist=%.0f chained=%d combo_left=%d hold=%d", (int)t, dist, (int)chained, (int)b->combo_left,
           (int)b->hold_left);
    b->counter_window = 0;
    b->bait_window = 0;
}

void start_guard(e_dn_class* dn, Brain* b, s16 ang) {
    // Mirrors vanilla's own fight_run -> ACTION_GUARD entry. Advanced removes vanilla's guard
    // invulnerability (in POST); Nightmare keeps it, snaps to face Link and holds the guard.
    const bool nightmare = g_level >= nm::LEVEL_NIGHTMARE;
    if (nightmare) {
        ac(dn)->shape_angle.y = ang;
        ac(dn)->current.angle.y = ang;
    }
    dn->action = A_GUARD;
    dn->mode = (cc_pl_cut_bit_get() == 0x100) ? 0 : 5;
    dn->field_0x708 = 30.0f;
    dn->field_0x70c = ac(dn)->shape_angle.y;
    dn->timer[0] = nightmare ? kArmTicks : 0;
    b->guard_cd = nightmare ? 0 : 110;
    NM_DBG("[NM] guard raised");
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
        if (!dir_clear(dn, cand[i], 340.0f)) {
            continue;
        }
        b->dodge_heading = cand[i];
        b->dodge_frames = 0;
        b->dodge_speed = r == nm::REACT_DODGE_BACK ? 46.0f : 42.0f;
        dn->action = A_FIGHT_RUN;
        dn->mode = MODE_NM_DODGE;
        anm_init(dn, ANM_WAIT_02, 1.0f, J3DFrameCtrl::EMode_LOOP, 3.2f);  // the existing hop, much faster
        ac(dn)->speed.y = 11.0f;
        b->dodge_cd = g_level >= nm::LEVEL_NIGHTMARE ? 3 : 150;
        b->dodge_recent = 25;
        if (b->holding && dn->anm_p != nullptr) b->holding = false;  // the new animation resets the rate
        b->hold_left = 0;
        NM_DBG("[NM] dodge kind=%d heading=%d", (int)r, (int)cand[i]);
        return true;
    }
    NM_DBG("[NM] dodge wanted but boxed in");
    return false;  // boxed in: no dodge (never clips, never teleports)
}

// Drops a swing it was winding up or recovering from (so the guard/dodge can take over).
void cancel_attack(e_dn_class* dn, Brain* b) {
    if (b->holding && dn->anm_p != nullptr) {
        dn->anm_p->setPlaySpeed(b->hold_rate > 0.0f ? b->hold_rate : 1.0f);
    }
    b->holding = false;
    b->hold_left = 0;
    b->hold_done = true;
    b->combo_left = 0;
    b->atk_rate_set = 0.0f;
    dn->at_chk_flag = 0;
}

// Nightmare: the moment Link presses an attack, roll to read it and answer on the spot. Returns
// true if it reacted (guard or dodge). Never reads while it is staggered, mid-leap, or during the
// short opening it leaves after a run of reads.
bool read_swing(e_dn_class* dn, Brain* b, fopAc_ac_c* link, nm::SwingKind kind, f32 dist, s16 ang) {
    if (b->expose > 0 || b->read_cd > 0) {
        return false;
    }
    const int act = dn->action;
    const bool in_dodge = act == A_FIGHT_RUN && dn->mode == MODE_NM_DODGE;
    // Link attacks again right after it dodged, while it is still landing: it dodges again.
    const bool double_dodge = in_dodge && b->dodge_recent > 0 && b->dodge_frames >= 3;
    const bool ready = (act == A_FIGHT_RUN && dn->mode >= 0 && !in_dodge) || act == A_GUARD;
    // A swing it is holding back, or already recovering from, is dropped for the guard/dodge.
    bool recovering = false;
    if (is_attack(act) && dn->anm_p != nullptr) {
        // Anything but the hit window itself: the wind-up (dropped for a guard/dodge), a held feint, or the recovery.
        const int fr = (int)dn->anm_p->getFrame();
        recovering = b->holding || fr > attack_end_frame(act) + 1 || fr < attack_start_frame(act) - 2;
    }
    if (!(ready || recovering || double_dodge)) {
        return false;
    }
    const bool wide = kind != nm::SWING_SWORD;
    if (dist > (wide ? kReadRangeWide : kReadRange) || dist < 1.0f) {
        return false;
    }
    if (!wide) {
        const f32 ex = (ac(dn)->current.pos.x - link->current.pos.x) / dist;  // Link -> it
        const f32 ez = (ac(dn)->current.pos.z - link->current.pos.z) / dist;
        if (ex * cM_ssin(link->shape_angle.y) + ez * cM_scos(link->shape_angle.y) < kReadFaceCos) {
            return false;  // Link is swinging away from it
        }
    }
    const bool can_dodge = act != A_GUARD && (double_dodge || (b->dodge_cd == 0 && dn->objacch.ChkGroundHit()));
    // A jump strike is never even rolled for: it just sidesteps it.
    const bool jump_counter = kind == nm::SWING_JUMP && can_dodge;
    if (!jump_counter && !double_dodge && cM_rndF(1.0f) >= nm::read_chance(b->mem, recovering)) {
        return false;  // missed the read: this swing connects
    }

    b->read_cd = 1;
    b->streak_age = 0;
    b->streak++;
    if (b->streak >= nm::streak_limit(b->rage)) {
        // A run of reads: it leaves a short opening. This swing is not answered, and a guard that is
        // still up is dropped, so this one can connect.
        b->streak = 0;
        b->expose = (s16)nm::expose_ticks(b->rage);
        if (act == A_GUARD) {
            dn->timer[0] = 0;
        }
        NM_DBG("[NM] opening: no reads for %d frames", (int)b->expose);
        return false;
    }

    float back, side;
    nm::dodge_odds(kind, b->mem.repeat >= 2, b->landed[(int)kind & 3], back, side);
    if (double_dodge) {
        back = 0.35f;
        side = 0.65f;
    }
    if (!can_dodge) {
        back = side = 0.0f;
    }
    const float r = cM_rndF(1.0f);
    if (is_attack(act)) {
        cancel_attack(dn, b);
    }
    bool done = false;
    if (r < back) {
        done = start_dodge(dn, b, nm::REACT_DODGE_BACK, ang);
    } else if (r < back + side) {
        done = start_dodge(dn, b, nm::REACT_DODGE_SIDE, ang);
    }
    if (done) {
        return true;
    }
    if (double_dodge) {
        return false;  // it never guards from the middle of a landing
    }
    if (act == A_GUARD && dn->mode == 7) {
        if (dn->timer[0] < kArmTicks) dn->timer[0] = kArmTicks;  // already guarding: keep the guard up
    } else {
        start_guard(dn, b, ang);
    }
    return true;
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
                start_guard(dn, b, ang);
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

    // Footwork: turn on the spot toward Link much faster than vanilla, so no attack starts facing away.
    if (b->los && dist < 900.0f && !facing && dn->mode <= 2) {
        cLib_addCalcAngleS2(&ac(dn)->shape_angle.y, ang, 1, 0x4000);
        ac(dn)->current.angle.y = ac(dn)->shape_angle.y;
    }

    // 2) Punish an opening, once the counter delay has passed.
    if (b->counter_window > 0 && b->counter_delay == 0 && dist < kEngageRange && b->los && facing) {
        launch_attack(dn, b, dist, link_behind, false);
        return;
    }

    // 3) Engage: from further out and with almost no gap (vanilla waits 30..60 frames).
    if (b->atk_cooldown == 0 && dist < kEngageRange && b->los && facing && dn->timer[2] == 0) {
        dn->timer[2] = 1;
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
    static int s_calls = 0, s_gate_a = 0, s_gate_b = 0, s_beat = 0;
    if (s_calls++ == 0) {
        NM_DBG("[NM] action_pre is being called (level=%d)", g_level);
    }
    Brain* b = brain_of(dn);
    b->active = false;
    b->snap.valid = false;

    const int lvl = g_level;
    if (lvl <= nm::LEVEL_OFF) {
        release_actor(dn, b);
        return;
    }
    if (dn->status != 0 || dn->anm_p == nullptr || dn->actor.health <= 0 || dn->death_flag != 0) {
        if (every(s_gate_a, 600)) {
            NM_DBG("[NM] skip: status=%d anm_p=%d hp=%d death=%d", (int)dn->status, (int)(dn->anm_p != nullptr),
                   (int)dn->actor.health, (int)dn->death_flag);
        }
        return;  // not running yet, or dying: vanilla only
    }
    fopAc_ac_c* link = dComIfGp_getPlayer(0);
    if (link == nullptr || dComIfGp_event_runCheck() || dComIfGs_getLife() == 0) {
        if (every(s_gate_b, 600)) {
            NM_DBG("[NM] skip: link=%d event=%d life=%d", (int)(link != nullptr), (int)dComIfGp_event_runCheck(),
                   (int)dComIfGs_getLife());
        }
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
    b->ran = true;
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
    if (every(s_beat, 180)) {
        NM_DBG("[NM] active: action=%d mode=%d dist=%.0f los=%d hp=%d/%d range=%.0f", (int)dn->action, (int)dn->mode, dist,
               (int)b->los, (int)a->health, (int)b->max_hp, dn->pl_range);
    }

    // Idle -> alerted is noticed in POST; here: keep chasing what we last saw. Vanilla gives up 40
    // frames after losing sight; extend that, but only while there is somewhere left to look.
    if (dn->action == A_FIGHT_RUN && b->have_seen && !b->los && b->lost_frames < nm::interest_frames(lvl)) {
        const f32 dx = b->seen_x - a->current.pos.x;
        const f32 dz = b->seen_z - a->current.pos.z;
        if (dx * dx + dz * dz > 140.0f * 140.0f && dn->unk_timer_1 < 2) {
            dn->unk_timer_1 = 2;
        }
    }

    if (lvl >= nm::LEVEL_NIGHTMARE) {
        // Vanilla waits 30..60 frames between attacks; never more than a few here.
        if (dn->action == A_FIGHT_RUN && dn->timer[2] > 2) dn->timer[2] = 2;
        // Chasing Link in plain sight: face him instantly, so vanilla never stops to turn and never loses a step.
        if (dn->action == A_FIGHT_RUN && (dn->mode == 0 || dn->mode == 1) && b->los) {
            a->current.angle.y = ang;
        }
        // Guard counter: it blocked (or Link whiffed), so it leaves the guard and swings back at once.
        if (dn->action == A_GUARD && dn->mode >= 6 && b->expose == 0 && b->counter_window > 0 &&
            b->counter_delay == 0 && b->los && dist < kEngageRange) {
            launch_attack(dn, b, dist, false, false);
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
        const bool can_chain = lvl >= nm::LEVEL_NIGHTMARE && b->combo_left > 0 && b->chain_len < 8 && b->los &&
                               dist < kEngageRange + 60.0f && nm::in_arc((s16)(a->shape_angle.y - ang), 0x5000);
        if (can_chain) {
            b->combo_left--;
            b->chain_len++;
            launch_attack(dn, b, dist, !nm::in_arc((s16)(a->shape_angle.y - ang), 0x5000), true);
        } else {
            b->chain_len = 0;
            b->combo_left = 0;
            b->atk_cooldown = 0;
            const s16 gap = lvl >= nm::LEVEL_NIGHTMARE ? 0 : 60;
            if (dn->timer[2] > gap) dn->timer[2] = gap;
            // Hit and run: after its own swing it often hops aside rather than stand in Link's counter.
            if (lvl >= nm::LEVEL_NIGHTMARE && dist < 340.0f && b->dodge_cd == 0 && dn->objacch.ChkGroundHit() &&
                cM_rndF(1.0f) < 0.4f) {
                start_dodge(dn, b, cM_rndF(1.0f) < 0.5f ? nm::REACT_DODGE_SIDE : nm::REACT_DODGE_BACK, ang);
            }
        }
    }

    // --- Nightmare attack speed and wind-up tracking --------------------------------------------
    if (lvl >= nm::LEVEL_NIGHTMARE && is_attack(dn->action)) {
        if (!b->holding) {
            const f32 cur_rate = dn->anm_p->getPlaySpeed();
            // Re-apply whenever the animation rate is not the one the mod set (a new animation reset it).
            if (cur_rate > 0.01f && fabsf(cur_rate - b->atk_rate_set) > 0.02f) {
                f32 mult = nm::attack_speed_mul(lvl, b->rage);
                if (b->fast_swing) mult *= 1.2f;
                b->atk_rate_set = cur_rate * mult;
                dn->anm_p->setPlaySpeed(b->atk_rate_set);
            }
        }
        // Until the hit window opens the swing keeps turning toward Link, so side-stepping is not enough.
        if ((int)dn->anm_p->getFrame() < hold_frame_of(tactic_of(dn->action)) - 2) {
            cLib_addCalcAngleS2(&a->shape_angle.y, ang, 2, 0x1000);
            a->current.angle.y = a->shape_angle.y;
        }
    } else {
        b->atk_rate_set = 0.0f;
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

    // --- guard ------------------------------------------------------------------------------
    if (cur == A_GUARD) {
        if (lvl >= nm::LEVEL_NIGHTMARE) {
            // Keep vanilla's guard invulnerability (the game's own block) and hold the guard while Link
            // keeps swinging. During the opening after a run of reads the guard is dropped instead.
            if (b->expose > 0) {
                if (dn->mode == 7) dn->timer[0] = 0;
                dn->invulnerability_timer = b->hit_this_frame ? b->hit_inv : 0;
            } else if (dn->mode == 7 && b->prev_cut_at && dist < kReadRangeWide && dn->timer[0] < 3) {
                dn->timer[0] = 3;
            }
        } else if (b->hit_this_frame) {
            dn->invulnerability_timer = b->hit_inv;  // normal post-hit recovery, as outside a guard
        } else if (dn->invulnerability_timer == 5 || dn->invulnerability_timer == 15) {
            dn->invulnerability_timer = 0;  // Advanced: vanilla's guard-only values; blocking is by collision
        }
        nm::mem_note_guarding(b->mem);
    }

    // --- horizontal movement overrides -------------------------------------------------------
    const f32 dx0 = a->current.pos.x - b->before_x;
    const f32 dz0 = a->current.pos.z - b->before_z;

    if (cur == A_FIGHT_RUN && dn->mode == MODE_NM_DODGE) {
        if (dn->timer[2] < 2) dn->timer[2] = 2;  // keep vanilla from starting an attack mid-dodge
        const int f = ++b->dodge_frames;
        const f32 v = f <= 5 ? b->dodge_speed : b->dodge_speed * (10 - f) / 5.0f;
        bool stop = f >= 10 || !dir_clear(dn, b->dodge_heading, 110.0f);
        if (!stop) {
            set_horizontal_delta(dn, b, cM_ssin(b->dodge_heading) * v, cM_scos(b->dodge_heading) * v, 56.0f);
        } else {
            dn->mode = 0;  // hand back to vanilla's chase (it re-inits the run animation)
            if (lvl >= nm::LEVEL_NIGHTMARE && dist < kEngageRange + 100.0f && cM_rndF(1.0f) < 0.95f) {
                b->counter_window = 30;  // it swings the moment it lands
                b->counter_delay = 1;
            }
        }
        return;
    }

    if (cur != A_FIGHT_RUN) {
        return;
    }

    const bool dash = dn->mode == 0 || dn->mode == 1;
    if (dash) {
        f32 mul = nm::speed_mul(lvl, b->rage);
        if (nm::habit_runner(b->mem)) mul = nm::clampf(mul + 0.08f, 1.0f, 2.6f);  // chase a runner harder
        if (b->prev_heal) mul = nm::clampf(mul + 0.1f, 1.0f, 2.6f);
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
            // Fast steps (up to 64 units a frame) only replace vanilla's step when the path is free.
            if (lvl < nm::LEVEL_NIGHTMARE) {
                set_horizontal_delta(dn, b, ndx, ndz);
            } else if (step_clear(dn, b, ndx, ndz)) {
                set_horizontal_delta(dn, b, ndx, ndz, 64.0f);
            }
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
        const bool open = lvl >= nm::LEVEL_NIGHTMARE && b->expose > 0;  // the opening: nothing is blocked
        if (!open && (same_swing || on_shield)) {
            if (on_shield) {
                b->blocked_obj = obj;
                b->block_frames = 10;  // the swing keeps overlapping for a few frames
            }
            dn->cc_sph[i].ClrTgHit();
            b->swing_hit = true;
            NM_DBG("[NM] shield blocked a hit");
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
    const int lvl = g_level;

    // A hit landed this frame (health went down): remember the normal post-hit recovery time that
    // vanilla / the fork chose, ignoring the 1000-frame lock that a knock-down sets.
    if (a->health < b->health_before) {
        b->hit_this_frame = true;
        b->swing_hit = true;
        if (lvl >= nm::LEVEL_NIGHTMARE) {
            // A hit got through: it gets fiercer, forgets its run of reads, and dodges that kind of swing more.
            if (b->hits < 9) b->hits++;
            b->streak = 0;
            const int k = b->last_kind & 3;
            if (b->landed[k] < 3) b->landed[k]++;
            b->landed_age = 600;
        }
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

    NM_DBG("[NM] poise: stagger absorbed, hp=%d", (int)a->health);
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
    if (lvl >= nm::LEVEL_NIGHTMARE) {
        b->counter_window = 24;  // it shakes the hit off and swings straight back
        b->counter_delay = 1;
    }
}

// After vanilla's whole Execute(): scale the sword's damage to Link. Vanilla re-sets the collider's
// attack power (1 for the stab/cut, 2 for the tail sweep) every frame it is active, so this never compounds.
void execute_post(e_dn_class* dn) {
    if (!g_pool_ready) {
        return;
    }
    Brain* b = brain_of(dn);
    if (!b->ran) {
        return;
    }
    b->ran = false;
    if (g_level < nm::LEVEL_NIGHTMARE || g_damage_mult <= 1) {
        return;
    }
    const int base = dn->tail_at_sph_flag == 2 ? 2 : 1;
    int atp = (int)((float)(base * g_damage_mult) * (1.0f + 0.5f * b->rage) + 0.5f);
    if (atp > 20) atp = 20;
    dn->at_sph.SetAtAtp(atp);
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
