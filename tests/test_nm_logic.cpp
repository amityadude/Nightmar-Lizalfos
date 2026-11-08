// Standalone unit tests for the Nightmare Lizalfos decision logic (src/nm_logic.h).
// No game code and no Dusklight checkout are needed, only a C++ compiler:
//
//   g++ -std=c++17 -Wall -Wextra -I src tests/test_nm_logic.cpp -o /tmp/test_nm_logic && /tmp/test_nm_logic
//
// These cover the pure maths and probability rules. They say nothing about how the Lizalfos
// looks or feels in-game; that still has to be playtested.

#include "nm_logic.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);                           \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

// The engine's cM_atan2s(x, z): yaw measured from +Z toward +X, 0x10000 per turn.
static int16_t atan2s(float x, float z) {
    return (int16_t)(int)std::lround(std::atan2(x, z) * 32768.0 / M_PI);
}
static int16_t wrap(int v) { return (int16_t)v; }

// Move 'step' units along 'heading' (engine yaw) and report the signed change in the bearing
// measured from the player to the mover.
static int bearing_delta_after_step(float px, float pz, float mx, float mz, int16_t heading, float step) {
    int16_t before = atan2s(mx - px, mz - pz);
    float nx = mx + std::sin(heading * M_PI / 32768.0) * step;
    float nz = mz + std::cos(heading * M_PI / 32768.0) * step;
    int16_t after = atan2s(nx - px, nz - pz);
    return (int16_t)(after - before);
}

static void test_strafe_geometry() {
    int checked = 0;
    for (int deg = 0; deg < 360; deg += 7) {
        int16_t bearing = wrap((int)(deg / 360.0 * 65536.0));  // from player to lizalfos
        float r = 300.0f;
        float px = 10.0f, pz = -20.0f;
        float mx = px + std::sin(bearing * M_PI / 32768.0) * r;
        float mz = pz + std::cos(bearing * M_PI / 32768.0) * r;
        int16_t to_player = atan2s(px - mx, pz - mz);

        // Ask to move toward a bearing a quarter-turn *ahead* (positive diff): bearing must increase.
        int16_t target_up = wrap(bearing + 0x2000);
        int16_t h_up = nm::strafe_heading(to_player, bearing, target_up, 1);
        CHECK(bearing_delta_after_step(px, pz, mx, mz, h_up, 20.0f) > 0, "heading increases bearing");

        // And toward a bearing behind (negative diff): bearing must decrease.
        int16_t target_dn = wrap(bearing - 0x2000);
        int16_t h_dn = nm::strafe_heading(to_player, bearing, target_dn, 1);
        CHECK(bearing_delta_after_step(px, pz, mx, mz, h_dn, 20.0f) < 0, "heading decreases bearing");

        // A tangential step must not change the distance to the player much (it circles).
        float nx = mx + std::sin(h_up * M_PI / 32768.0) * 20.0f;
        float nz = mz + std::cos(h_up * M_PI / 32768.0) * 20.0f;
        float d0 = std::hypot(mx - px, mz - pz);
        float d1 = std::hypot(nx - px, nz - pz);
        CHECK(std::fabs(d1 - d0) < 2.0f, "strafe is tangential (circles, does not close or open)");
        checked++;
    }
    CHECK(checked > 40, "geometry sweep ran");

    // Nearly-opposite target: the hysteresis direction decides, and it is respected both ways.
    int16_t bearing = 0x1234, to_player = (int16_t)(bearing + 0x8000);
    int16_t tgt = (int16_t)(bearing + 0x7F00);
    CHECK(nm::strafe_heading(to_player, bearing, tgt, 1) == (int16_t)(to_player - 0x4000), "hysteresis +");
    CHECK(nm::strafe_heading(to_player, bearing, tgt, -1) == (int16_t)(to_player + 0x4000), "hysteresis -");
}

static void test_memory_decay_and_adaptation() {
    nm::Memory m = {};
    for (int i = 0; i < 4; i++) nm::mem_note_arrow(m);
    CHECK(nm::habit_archer(m), "4 arrows => archer habit");

    // Link stops shooting: the habit must fade on its own (about 6-7s half-life at 30fps).
    int frames = 0;
    while (nm::habit_archer(m) && frames < 2000) {
        nm::mem_decay(m);
        frames++;
    }
    CHECK(frames > 60 && frames < 600, "archer habit fades within seconds, not instantly and not never");

    // Repeating the same cut builds the streak, a different cut resets it, idling forgets it.
    nm::Memory s = {};
    for (int i = 0; i < 8; i++) nm::mem_note_swing(s, nm::SWING_SWORD, 0x1, true);
    CHECK(s.repeat == nm::kRepeatCap, "repeat streak is capped");
    nm::mem_note_swing(s, nm::SWING_SWORD, 0x4, true);
    CHECK(s.repeat == 0, "different cut resets the streak");
    for (int i = 0; i < 3; i++) nm::mem_note_swing(s, nm::SWING_SWORD, 0x4, true);
    CHECK(s.repeat == 3, "streak rebuilds");
    for (int i = 0; i < nm::kRepeatForgetFrames + 5; i++) nm::mem_decay(s);
    CHECK(s.repeat == 0, "streak is forgotten after idling");

    // Memory is plain data: a zeroed struct is a valid, empty memory.
    nm::Memory z = {};
    CHECK(!nm::habit_archer(z) && !nm::habit_roller(z) && !nm::habit_turtle(z) &&
              !nm::habit_runner(z) && !nm::habit_rear_attacker(z),
          "empty memory has no habits");
}

static void test_never_unbeatable() {
    // Worst case memory: maximum streak, heavy sword and frontal pressure.
    nm::Memory worst = {};
    worst.repeat = nm::kRepeatCap;
    worst.sword = 50.0f;
    worst.front = 50.0f;
    worst.roll = 50.0f;
    worst.guard = 50.0f;
    for (int k = nm::SWING_SWORD; k <= nm::SWING_SPIN; k++) {
        float g = nm::guard_chance(worst, nm::LEVEL_NIGHTMARE, (nm::SwingKind)k);
        float d = nm::dodge_chance(worst, nm::LEVEL_NIGHTMARE, (nm::SwingKind)k);
        CHECK(g <= 0.78f + 1e-6f, "guard chance is hard-capped");
        CHECK(d <= 0.70f + 1e-6f, "dodge chance is hard-capped");
        // With rolls just above the caps nothing is ever done: Link always has an opening.
        nm::Reaction r = nm::react_to_swing(worst, nm::LEVEL_NIGHTMARE, (nm::SwingKind)k, 200.0f,
                                                  true, true, 0.79f, 0.71f);
        CHECK(r == nm::REACT_NONE, "a high roll always leaves Link an opening");
    }

    // Repetition makes the guard more likely, monotonically.
    float prev = 0.0f;
    for (uint8_t rep = 0; rep <= nm::kRepeatCap; rep++) {
        nm::Memory m = {};
        m.repeat = rep;
        float g = nm::guard_chance(m, nm::LEVEL_NIGHTMARE, nm::SWING_SWORD);
        CHECK(g >= prev, "guard chance rises with repetition");
        prev = g;
    }

    CHECK(nm::counter_chance(nm::LEVEL_NIGHTMARE, true) < 1.0f, "counters are never guaranteed");
    CHECK(nm::counter_chance(nm::LEVEL_ADVANCED, false) == 0.0f, "Advanced has no counters");
    CHECK(nm::guard_chance(worst, nm::LEVEL_OFF, nm::SWING_SWORD) == 0.0f, "Disabled never guards");
    CHECK(nm::react_to_swing(worst, nm::LEVEL_OFF, nm::SWING_SWORD, 100.0f, true, true, 0.0f, 0.0f) ==
              nm::REACT_NONE,
          "Disabled never reacts");
}

static void test_reach_and_kinds() {
    nm::Memory m = {};
    // Out of reach: ignore the swing (keep closing in).
    CHECK(nm::react_to_swing(m, nm::LEVEL_NIGHTMARE, nm::SWING_SWORD, 600.0f, true, true, 0.0f, 0.0f) ==
              nm::REACT_NONE,
          "out-of-reach sword swing is ignored");
    // Jump attack in range with a good roll: back away, don't shield.
    CHECK(nm::react_to_swing(m, nm::LEVEL_NIGHTMARE, nm::SWING_JUMP, 300.0f, true, true, 0.0f, 0.0f) ==
              nm::REACT_DODGE_BACK,
          "jump attacks are answered with a backstep");
    // Close sword swing, guard roll wins.
    CHECK(nm::react_to_swing(m, nm::LEVEL_NIGHTMARE, nm::SWING_SWORD, 150.0f, true, true, 0.0f, 0.0f) ==
              nm::REACT_GUARD,
          "close sword swing can be shielded");
    // Guard not available (cooldown / not facing): fall back to a dodge or nothing, never a guard.
    nm::Reaction r = nm::react_to_swing(m, nm::LEVEL_NIGHTMARE, nm::SWING_SWORD, 150.0f, false, true,
                                              0.0f, 0.0f);
    CHECK(r != nm::REACT_GUARD, "no guard when guard is unavailable");
    // Nothing available at all.
    CHECK(nm::react_to_swing(m, nm::LEVEL_NIGHTMARE, nm::SWING_SWORD, 150.0f, false, false, 0.0f, 0.0f) ==
              nm::REACT_NONE,
          "no options => no reaction");
    // Vertical cuts are sidestepped, wide cuts are backed away from.
    nm::Memory v = {};
    v.last_cut = 0x1;
    CHECK(nm::react_to_swing(v, nm::LEVEL_NIGHTMARE, nm::SWING_SWORD, 150.0f, false, true, 0.0f, 0.0f) ==
              nm::REACT_DODGE_SIDE,
          "vertical cut => sidestep");
    nm::Memory w = {};
    w.last_cut = 0x4;
    CHECK(nm::react_to_swing(w, nm::LEVEL_NIGHTMARE, nm::SWING_SWORD, 150.0f, false, true, 0.0f, 0.0f) ==
              nm::REACT_DODGE_BACK,
          "wide cut => backstep");
}

static void test_tactics_and_combos() {
    // Never repeats the previous attack, whatever the roll.
    for (int last = 0; last < 3; last++) {
        for (int i = 0; i < 100; i++) {
            float r = i / 100.0f;
            for (int behind = 0; behind < 2; behind++) {
                for (float dist : {120.0f, 280.0f, 380.0f}) {
                    nm::Tactic t = nm::pick_tactic(dist, behind != 0, false, (nm::Tactic)last, true, r);
                    CHECK((int)t != last, "tactic never repeats the previous attack");
                }
            }
        }
    }
    // Distribution sanity: from behind, the tail sweep dominates; up close the quick stab is common.
    int tail = 0, quick = 0;
    for (int i = 0; i < 1000; i++) {
        float r = i / 1000.0f;
        if (nm::pick_tactic(200.0f, true, false, nm::TACTIC_QUICK, false, r) == nm::TACTIC_TAIL) tail++;
        if (nm::pick_tactic(200.0f, false, false, nm::TACTIC_QUICK, false, r) == nm::TACTIC_QUICK) quick++;
    }
    CHECK(tail > 600, "tail sweep dominates from behind");
    CHECK(quick > 400, "quick stab common at close range");
    // A turtling Link draws more tail sweeps than a normal one.
    int tail_turtle = 0, tail_normal = 0;
    for (int i = 0; i < 1000; i++) {
        float r = i / 1000.0f;
        if (nm::pick_tactic(250.0f, false, true, nm::TACTIC_STEP, true, r) == nm::TACTIC_TAIL) tail_turtle++;
        if (nm::pick_tactic(250.0f, false, false, nm::TACTIC_STEP, true, r) == nm::TACTIC_TAIL) tail_normal++;
    }
    CHECK(tail_turtle > tail_normal, "shield-hugging Link draws more tail sweeps");

    // Combos: bounded (never more than 2 extra), longer when enraged, none below Nightmare.
    nm::Memory m = {};
    int extra_calm = 0, extra_enraged = 0;
    for (int i = 0; i < 1000; i++) {
        float r = i / 1000.0f;
        int a = nm::combo_extra(nm::LEVEL_NIGHTMARE, false, m, r);
        int b = nm::combo_extra(nm::LEVEL_NIGHTMARE, true, m, r);
        CHECK(a >= 0 && a <= 2 && b >= 0 && b <= 2, "combo length is bounded");
        extra_calm += a;
        extra_enraged += b;
        CHECK(nm::combo_extra(nm::LEVEL_ADVANCED, true, m, r) == 0, "Advanced has no combos");
    }
    CHECK(extra_enraged > extra_calm, "enrage lengthens combos");
}

static void test_enrage_and_scalars() {
    CHECK(!nm::is_enraged(100, 100), "full health is not enraged");
    CHECK(!nm::is_enraged(36, 100), "36% is not enraged");
    CHECK(nm::is_enraged(35, 100), "35% is enraged");
    CHECK(nm::is_enraged(1, 150), "almost dead is enraged");
    CHECK(!nm::is_enraged(0, 100), "dead is not 'enraged'");
    CHECK(!nm::is_enraged(10, 0), "unknown max health never enrages");

    for (int lvl = 0; lvl <= 2; lvl++) {
        CHECK(nm::speed_mul(lvl, true) <= 1.3f + 1e-6f, "speed multiplier is capped");
        CHECK(nm::speed_mul(lvl, false) >= 1.0f, "speed multiplier never slows");
        CHECK(nm::sense_mul(lvl) >= 1.0f, "perception never shrinks");
        CHECK(nm::interest_frames(lvl) >= 40, "never forgets faster than vanilla");
    }
    CHECK(nm::interest_frames(0) == 40, "Disabled keeps vanilla interest time");
    CHECK(nm::sense_mul(0) == 1.0f && nm::speed_mul(0, false) == 1.0f, "Disabled is the identity");
    CHECK(nm::delay_chance(nm::Memory{}, false) < nm::bait_chance(false) + 0.2f, "delays are occasional");
    nm::Memory roller = {};
    roller.roll = 5.0f;
    CHECK(nm::delay_chance(roller, false) > nm::delay_chance(nm::Memory{}, false),
          "a rolling Link draws more delayed attacks");
    CHECK(nm::bait_chance(false) < 0.2f, "baiting stays rare");
}


static void test_pool_and_arc() {
    struct V { int hits; };
    static nm::Pool<V, 4> pool;
    pool.clear();
    int a, b, c, d, e;
    bool created = false;

    V* va = pool.acquire(&a, 1, &created);
    CHECK(created && va->hits == 0, "first acquire creates a zeroed slot");
    va->hits = 7;
    V* va2 = pool.acquire(&a, 1, &created);
    CHECK(!created && va2 == va && va2->hits == 7, "same (addr,id) returns the same slot");

    // Same address, new process id (actor re-created in the same memory): must start fresh.
    V* vb = pool.acquire(&a, 2, &created);
    CHECK(created && vb->hits == 0, "a re-created actor never inherits stale state");

    pool.acquire(&b, 3, nullptr);
    pool.acquire(&c, 4, nullptr);
    pool.acquire(&d, 5, nullptr);  // pool is now full (a/1, a/2, b, c, d => one recycled)
    V* ve = pool.acquire(&e, 6, &created);
    CHECK(created && ve != nullptr, "a full pool recycles instead of failing");

    // release() frees every slot for an address.
    pool.release(&e);
    V* ve2 = pool.acquire(&e, 6, &created);
    CHECK(created && ve2->hits == 0, "released slot is recreated zeroed");

    CHECK(nm::in_arc(0, 0x3800) && nm::in_arc(0x3800, 0x3800) && nm::in_arc(-0x3800, 0x3800), "arc edges inclusive");
    CHECK(!nm::in_arc(0x3801, 0x3800) && !nm::in_arc(-0x3801, 0x3800), "outside the arc");
    CHECK(!nm::in_arc((int16_t)-32768, 0x3800), "s16 minimum does not overflow");
}

int main() {
    test_strafe_geometry();
    test_memory_decay_and_adaptation();
    test_never_unbeatable();
    test_reach_and_kinds();
    test_tactics_and_combos();
    test_enrage_and_scalars();
    test_pool_and_arc();
    if (g_fail == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
