#pragma once

/**
 * @file nm_logic.h
 * @brief Nightmare Lizalfos decision logic. Pure: standard types only, no engine calls, and every
 *        random roll is supplied by the caller. The engine glue (lizalfos_nm.cpp) owns all game
 *        interaction. This split keeps the "brain" unit-testable without the game
 *        (see tests/test_nm_logic.cpp).
 *
 * Difficulty levels (mod config var "difficulty"):
 *   0 = Disabled  : vanilla behaviour, nothing in here is consulted.
 *   1 = Advanced  : longer perception, faster, reacts defensively to Link's swings.
 *   2 = Nightmare : everything: adaptive memory, counters, combos, flanking, poise, enrage.
 */

#include <stddef.h>
#include <stdint.h>

namespace nm {

enum Level { LEVEL_OFF = 0, LEVEL_ADVANCED = 1, LEVEL_NIGHTMARE = 2 };

enum SwingKind { SWING_NONE = 0, SWING_SWORD, SWING_JUMP, SWING_SPIN };

enum Reaction { REACT_NONE = 0, REACT_GUARD, REACT_DODGE_SIDE, REACT_DODGE_BACK };

// Attack choices. The actor maps these onto its existing attack actions; no new animations.
enum Tactic { TACTIC_QUICK = 0, TACTIC_STEP = 1, TACTIC_TAIL = 2 };

/**
 * Lightweight combat memory. Every field is an exponentially-decayed event weight, so recent
 * behaviour counts more and old habits fade (adaptive memory decay). Owned by one actor and
 * zero-initialised at creation: no globals, nothing to leak.
 */
struct Memory {
    float sword;    // sword swings
    float jump;     // jump attacks
    float spin;     // spin attacks / spin charges
    float arrow;    // arrows fired
    float roll;     // rolls / sidesteps
    float guard;    // time spent behind the shield
    float retreat;  // backing away
    float heal;     // bottle use nearby
    float front;    // swings made while Link faces the Lizalfos' front
    float behind;   // swings made from behind the Lizalfos
    uint16_t last_cut;   // cut bit of the previous swing
    uint8_t repeat;      // consecutive swings with the same cut bit (capped)
    uint8_t idle;        // frames-ish since the last swing (saturating), resets 'repeat'
};

// Per-frame retention. 0.9965^30 ~= 0.90 per second, i.e. a half-life of roughly 6-7 seconds.
static const float kDecay = 0.9965f;
static const uint8_t kRepeatCap = 5;
static const int kRepeatForgetFrames = 120;  // ~4s without a swing forgets the combo streak

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline void mem_decay(Memory& m) {
    m.sword *= kDecay;
    m.jump *= kDecay;
    m.spin *= kDecay;
    m.arrow *= kDecay;
    m.roll *= kDecay;
    m.guard *= kDecay;
    m.retreat *= kDecay;
    m.heal *= kDecay;
    m.front *= kDecay;
    m.behind *= kDecay;
    if (m.idle < 255) {
        m.idle++;
    }
    if (m.idle > kRepeatForgetFrames) {
        m.repeat = 0;
        m.last_cut = 0;
    }
}

inline void mem_note_swing(Memory& m, SwingKind kind, uint16_t cut_bit, bool lizalfos_faces_link) {
    switch (kind) {
    case SWING_JUMP: m.jump += 1.0f; break;
    case SWING_SPIN: m.spin += 1.0f; break;
    default: m.sword += 1.0f; break;
    }
    if (lizalfos_faces_link) {
        m.front += 1.0f;
    } else {
        m.behind += 1.0f;
    }
    if (cut_bit != 0 && cut_bit == m.last_cut) {
        if (m.repeat < kRepeatCap) {
            m.repeat++;
        }
    } else {
        m.repeat = 0;
    }
    m.last_cut = cut_bit;
    m.idle = 0;
}

inline void mem_note_arrow(Memory& m) { m.arrow += 1.0f; }
inline void mem_note_roll(Memory& m) { m.roll += 1.0f; }
inline void mem_note_heal(Memory& m) { m.heal += 1.0f; }
inline void mem_note_guarding(Memory& m) { m.guard += 0.04f; }   // per frame spent guarding
inline void mem_note_retreat(Memory& m) { m.retreat += 0.05f; }  // per frame spent backing off

// Habit thresholds. Deliberately modest so a change in Link's strategy shows up within seconds.
inline bool habit_archer(const Memory& m) { return m.arrow > 1.5f; }
inline bool habit_roller(const Memory& m) { return m.roll > 2.0f; }
inline bool habit_turtle(const Memory& m) { return m.guard > 1.5f; }
inline bool habit_runner(const Memory& m) { return m.retreat > 1.5f; }
inline bool habit_rear_attacker(const Memory& m) { return m.behind > 2.0f && m.behind > m.front; }

/**
 * Chance of raising the shield against a swing. Repeating the same cut raises it step by step,
 * and it is hard-capped, so the Lizalfos gets harder to fool but never becomes unbeatable.
 */
inline float guard_chance(const Memory& m, int level, SwingKind kind) {
    if (level <= LEVEL_OFF) {
        return 0.0f;
    }
    if (level == LEVEL_ADVANCED) {
        return kind == SWING_SWORD ? 0.45f : 0.20f;
    }
    float base = kind == SWING_SWORD ? 0.28f : (kind == SWING_SPIN ? 0.22f : 0.12f);
    base += 0.10f * m.repeat;                  // same combo again and again
    base += 0.02f * clampf(m.sword, 0.0f, 6.0f);  // sustained sword pressure
    base += 0.03f * clampf(m.front, 0.0f, 4.0f);  // Link keeps attacking head-on
    return clampf(base, 0.0f, 0.78f);
}

/** Chance of getting out of the way instead. Rolled only if the guard roll failed. */
inline float dodge_chance(const Memory& m, int level, SwingKind kind) {
    if (level <= LEVEL_OFF) {
        return 0.0f;
    }
    if (level == LEVEL_ADVANCED) {
        return kind == SWING_SWORD ? 0.15f : 0.30f;
    }
    float base = kind == SWING_SWORD ? 0.34f : (kind == SWING_JUMP ? 0.60f : 0.50f);
    base += 0.05f * m.repeat;
    return clampf(base, 0.0f, 0.70f);
}

/**
 * Decide how to answer a swing Link has just started. 'r_guard' and 'r_dodge' are independent
 * uniform [0,1) rolls from the caller. Out-of-reach swings are ignored so the Lizalfos keeps
 * closing in instead of flinching at everything.
 */
inline Reaction react_to_swing(const Memory& m, int level, SwingKind kind, float dist, bool guard_ok,
                               bool dodge_ok, float r_guard, float r_dodge) {
    if (level <= LEVEL_OFF || kind == SWING_NONE) {
        return REACT_NONE;
    }
    const float reach = (kind == SWING_JUMP) ? 460.0f : (kind == SWING_SPIN ? 340.0f : 330.0f);
    if (dist > reach) {
        return REACT_NONE;
    }
    // Jumps and spins are answered by distance first; a shield is the weaker answer to them.
    if (kind != SWING_SWORD && dodge_ok && r_dodge < dodge_chance(m, level, kind)) {
        return REACT_DODGE_BACK;
    }
    if (guard_ok && r_guard < guard_chance(m, level, kind)) {
        return REACT_GUARD;
    }
    if (kind == SWING_SWORD && dodge_ok && r_dodge < dodge_chance(m, level, kind)) {
        // Vertical/stab swings are sidestepped; the caller picks the side. Wide swings: back off.
        return (m.last_cut == 0x1 || m.last_cut == 0x2 || m.last_cut == 0x200) ? REACT_DODGE_SIDE
                                                                              : REACT_DODGE_BACK;
    }
    return REACT_NONE;
}

/**
 * Heading (engine yaw, 0x10000 = full turn) that circles toward 'target_bearing' around Link.
 *
 *   angle_to_player   : yaw from the Lizalfos to Link (cM_atan2s of the offset)
 *   bearing_from_link : yaw from Link to the Lizalfos (angle_to_player + 0x8000)
 *   target_bearing    : yaw (from Link) where we would like to stand, e.g. behind Link
 *
 * Moving along (angle_to_player - 0x4000) increases the bearing, (angle_to_player + 0x4000)
 * decreases it. Verified numerically in tests/test_nm_logic.cpp.
 */
inline int16_t strafe_heading(int16_t angle_to_player, int16_t bearing_from_link, int16_t target_bearing,
                              int hysteresis_dir) {
    int16_t diff = (int16_t)(target_bearing - bearing_from_link);
    int dir;
    if (diff > 0x7000 || diff < -0x7000) {
        dir = hysteresis_dir >= 0 ? 1 : -1;  // nearly opposite: keep going the way we were
    } else {
        dir = diff >= 0 ? 1 : -1;
    }
    return dir > 0 ? (int16_t)(angle_to_player - 0x4000) : (int16_t)(angle_to_player + 0x4000);
}

/** Pick an attack. Never repeats 'last' when another option fits, so combos stay varied. */
inline Tactic pick_tactic(float dist, bool behind, bool link_is_turtling, Tactic last, bool has_last,
                          float r) {
    // Weights as (quick, step, tail) before the no-repeat rule.
    float wq, ws, wt;
    if (behind) {
        wq = 0.15f; ws = 0.10f; wt = 0.75f;
    } else if (dist < 300.0f) {
        wq = 0.50f; ws = 0.25f; wt = 0.25f;
    } else {
        wq = 0.10f; ws = 0.60f; wt = 0.30f;
    }
    if (link_is_turtling) {
        wt += 0.20f;  // the tail sweep goes around a raised shield better than a stab
        wq -= 0.10f;
        if (wq < 0.02f) {
            wq = 0.02f;
        }
    }
    if (has_last) {
        if (last == TACTIC_QUICK) wq = 0.0f;
        else if (last == TACTIC_STEP) ws = 0.0f;
        else wt = 0.0f;
    }
    float total = wq + ws + wt;
    float pick = r * total;
    if (pick < wq) return TACTIC_QUICK;
    if (pick < wq + ws) return TACTIC_STEP;
    return TACTIC_TAIL;
}

/** Additional attacks chained after the first (0..2). A hard cap lives in the actor too. */
inline int combo_extra(int level, bool enraged, const Memory& m, float r) {
    if (level < LEVEL_NIGHTMARE) {
        return 0;
    }
    float p1 = enraged ? 0.55f : 0.35f;
    float p2 = enraged ? 0.30f : 0.12f;
    if (habit_turtle(m)) {
        p1 += 0.10f;  // keep the pressure on a defensive Link
    }
    if (r < p2) return 2;
    if (r < p1) return 1;
    return 0;
}

/** Probability the Lizalfos exploits an opening (a missed swing, a roll recovery, ...). */
inline float counter_chance(int level, bool enraged) {
    if (level < LEVEL_NIGHTMARE) {
        return 0.0f;
    }
    return enraged ? 0.85f : 0.65f;
}

/** Probability that a launched attack holds its wind-up to catch an early dodge. */
inline float delay_chance(const Memory& m, bool enraged) {
    float p = 0.18f;
    if (habit_roller(m)) {
        p += 0.30f;  // Link rolls a lot: mix up the timing
    }
    if (enraged) {
        p -= 0.08f;  // fewer pauses when enraged
    }
    return clampf(p, 0.05f, 0.55f);
}

/** Chance per engagement of a deliberate "false opening" (bait). Always rare. */
inline float bait_chance(bool enraged) { return enraged ? 0.05f : 0.12f; }

/** True once health has dropped to 35% or below. */
inline bool is_enraged(int health, int max_health) {
    return max_health > 0 && health > 0 && health * 100 <= max_health * 35;
}

/** Perception multiplier applied to sight range and hearing range. */
inline float sense_mul(int level) {
    return level >= LEVEL_NIGHTMARE ? 2.5f : (level == LEVEL_ADVANCED ? 1.5f : 1.0f);
}

/** Frames of continued pursuit after losing Link (vanilla is 40). */
inline int interest_frames(int level) {
    return level >= LEVEL_NIGHTMARE ? 300 : (level == LEVEL_ADVANCED ? 90 : 40);
}

/** Run-speed multiplier. Enrage adds a little on top; the total is capped. */
inline float speed_mul(int level, bool enraged) {
    float m = level >= LEVEL_NIGHTMARE ? 1.2f : (level == LEVEL_ADVANCED ? 1.1f : 1.0f);
    if (enraged) {
        m += 0.1f;
    }
    return clampf(m, 1.0f, 1.3f);
}

/** Attack animation speed multiplier (applied on top of the Lazy Tweaks "slower enemies" speed). */
inline float attack_speed_mul(int level, bool enraged) {
    float m = level >= LEVEL_NIGHTMARE ? 1.1f : (level == LEVEL_ADVANCED ? 1.05f : 1.0f);
    if (enraged) {
        m += 0.05f;
    }
    return m;
}

/** True when |yaw_diff| <= half_arc. Handles the s16 wrap (-32768) without overflow. */
inline bool in_arc(int16_t yaw_diff, int half_arc) {
    int d = yaw_diff;
    if (d < 0) {
        d = -d;
    }
    return d <= half_arc;
}

/**
 * Fixed-capacity per-actor state pool. Keys are (address, process id): the id changes whenever
 * an actor is re-created at the same address, so a stale slot can never be mistaken for a live
 * actor. No allocation ever happens after construction; a full pool recycles the slot that has
 * been idle the longest.
 */
template <class T, int N>
struct Pool {
    struct Slot {
        uintptr_t addr;
        uint32_t id;
        uint32_t touched;
        bool used;
        T value;
    };
    Slot slots[N];
    uint32_t tick;

    void clear() {
        for (int i = 0; i < N; i++) {
            slots[i] = Slot{};
        }
        tick = 0;
    }

    /** Returns the slot for (addr,id), creating a zeroed one if needed. Never returns NULL. */
    T* acquire(const void* addr, uint32_t id, bool* created) {
        tick++;
        const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
        int free_idx = -1;
        int oldest = 0;
        for (int i = 0; i < N; i++) {
            Slot& s = slots[i];
            if (!s.used) {
                if (free_idx < 0) {
                    free_idx = i;
                }
                continue;
            }
            if (s.addr == a && s.id == id) {
                s.touched = tick;
                if (created) {
                    *created = false;
                }
                return &s.value;
            }
            if ((uint32_t)(tick - s.touched) > (uint32_t)(tick - slots[oldest].touched) || !slots[oldest].used) {
                oldest = i;
            }
        }
        int idx = free_idx >= 0 ? free_idx : oldest;
        Slot& s = slots[idx];
        s = Slot{};
        s.used = true;
        s.addr = a;
        s.id = id;
        s.touched = tick;
        if (created) {
            *created = true;
        }
        return &s.value;
    }

    /** Frees the slot for (addr, any id) if present. */
    void release(const void* addr) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
        for (int i = 0; i < N; i++) {
            if (slots[i].used && slots[i].addr == a) {
                slots[i] = Slot{};
            }
        }
    }
};

}  // namespace nm
