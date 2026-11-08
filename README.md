# Nightmare Lizalfos

A [Dusklight](https://github.com/TwilitRealm/dusklight) mod, built from
[TwilitRealm/mod-template](https://github.com/TwilitRealm/mod-template), that gives the Lizalfos (`e_dn_class`)
an adaptive combat AI. It changes only the Lizalfos. The game source, Link and every other enemy are untouched.

> **Status: compiled and linked against the Dusklight v2.0.2 SDK, logic unit-tested. Not yet run in the game.**
> Every behaviour below is implemented in source. None of it has been observed in-game, so expect to tune numbers.

## Difficulty

One setting, `difficulty`, saved in the mod's `config.json` and shown as a dropdown in the in-game mods panel
(**Lizalfos difficulty**).

| Value | Name | What it does |
|---|---|---|
| 0 | Disabled | Vanilla Lizalfos. All mod state is released. |
| 1 | Advanced | Longer sight, slightly faster chase, raises its shield and sidesteps your swings, remembers where it last saw you. |
| 2 | **Nightmare** (default) | Everything in Advanced, plus adaptive memory, counters, combos, delayed attacks, bait, circling, enrage, poise, alerting nearby Lizalfos. |

## Fairness rules

* No invulnerability. The shield blocks only a hit whose attack object really struck the shield collision sphere.
* Every confirmed hit still deals its normal damage. Poise removes the stagger, not the damage.
* Nothing teleports. Every mod-driven step is capped at 36 units per frame, and BG collision (`CrrPos`) still runs after it.
* No new hitboxes, animations or sounds. Attacks use the vanilla attack actions, so vanilla hit windows, sounds and animations play.
* Death, scripted events, cutscenes, wolf-bite and hookshot interactions stay vanilla.
* No per-frame allocation. State is a fixed pool of 32 slots.

## How it works

The Lizalfos runs its whole brain in one file-local function, `action()`, which calls `damage_check()` first. A mod can
only wrap whole functions, so the mod never replaces the vanilla AI:

* **PRE `action()`** observes Link, decides, and switches the vanilla action or mode before vanilla runs.
* **POST `action()`** adjusts movement, wind-up timing, combos and bookkeeping.
* **PRE/POST `damage_check()`** does shield-by-collision and poise.
* **PRE `daE_DN_Delete()`** frees the actor's state slot.

Hooks use the SDK's `src/d/actor/d_a_e_dn.cpp#<function>` aliases (`DEFINE_HOOK_SYMBOL`). If your Dusklight build has no
`action` symbol, the mod refuses to load and says why. If only `damage_check` or `daE_DN_Delete` is missing, the AI still
runs and the log and mods panel say which parts are off.

### State machine and priorities (Nightmare)

Decisions run only while vanilla is in its plain chase state (`ACTION_FIGHT_RUN`), in this order:

1. A pending **reaction** to Link's swing or arrow, after a short reaction delay: raise the shield, sidestep or back-hop.
2. A **counter** after a missed swing, a roll or healing: attack once the counter delay passes.
3. A **bait** window: hold position and invite a swing, then lunge.
4. **Engage**: start an attack at up to 400 units, with a shorter gap than vanilla.

Between those, vanilla's own chase and circling hop run. The mod adds speed, last-seen steering and circling on top.

### Shield and projectiles

The shield is the vanilla guard action with the vanilla shield sphere. The mod removes vanilla's 5 and 15 frame guard
invulnerability. A swing is blocked only when the same attack object touched the shield sphere and a body sphere. A swing
from behind or the side reaches the body without crossing the shield and lands normally. Arrows are scanned every 4 frames.
A frontal arrow raises the shield. A flank arrow triggers a sidestep at Nightmare.

### Poise

Before `damage_check()` the mod snapshots the stagger fields. If vanilla answers an ordinary hit with a small flinch or
knock-down, the mod restores them and keeps the damage. It does **not** remove the stagger when the hit is lethal, when
it is a wolf kick, or when it is a jump strike, helm splitter, spin attack or hidden skill. Those keep vanilla behaviour so
Link's finishers still work.

### Adaptive memory

Each Lizalfos keeps a small decayed memory of Link's habits: repeated sword swings and combos, jump attacks, arrows, rolls,
shield use, retreating, attacks from behind and healing. Counts decay over time. Probabilities are hard-capped, so no habit
turns into a guaranteed outcome. Examples:

| Link habit | Response |
|---|---|
| Spams swings | More shield and sidestep reactions, then counter on the miss. |
| Rolls a lot | Aims to end up behind Link and attacks as the roll recovers. |
| Turtles behind the shield | Prefers the tail sweep and flanking. |
| Favours arrows | Weaves while closing in. |
| Runs away | Chases faster. |
| Heals | Presses the attack for the healing window. |
| Attacks from behind | Reacts a frame sooner and sidesteps about twice as often (the shield cannot cover the back). |

### Other tactics

Sight range is widened through vanilla's own `pl_range`. Line of sight is a real ray cast every 4 frames (no wallhacks). When
sight is lost it investigates the last-seen position for a bounded time. It alerts idle Lizalfos within 1500 units. It
chains 2 or 3 attacks, delays the wind-up by 6 to 14 frames, enrages at low health, and checks for walls and drops before
any mod-driven dodge or circling step.

## Building

Requirements: CMake 3.26 or newer, Ninja, a C++20 compiler, git, and network access (the first configure fetches Dusklight).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

The output is `build/mods/nightmare_lizalfos.dusk`. A local build is valid for **your platform only**. Copy it into the
game's mods folder:

* Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
* Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
* macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

To build against an existing Dusklight checkout instead of fetching one:

```sh
cmake -S . -B build -G Ninja -DDUSKLIGHT_DIR=/path/to/dusklight
```

### Distribution build (all platforms)

Push this repository to GitHub. The included workflow builds every platform and merges them into one `.dusk`
(artifact **`mod-combined`**). Pushing a tag creates a GitHub release containing that combined bundle.

### Matching your Dusklight build

The mod is built against `DUSKLIGHT_VERSION` in `CMakeLists.txt` (default `v2.0.2`). The hooks resolve against the symbols
of the Dusklight build you actually run. Fork builds such as Lazy Tweaks keep the `e_dn_class` layout unchanged from
v2.0.2 (checked with a header diff), but the mod has not been loaded into either. If you run a different build, point the
mod at it, for example:

```sh
cmake -S . -B build -G Ninja -DDUSKLIGHT_REPOSITORY=https://github.com/kamilink7/dusklight.git -DDUSKLIGHT_VERSION=lazytweaks
```

For CI, edit the `DUSKLIGHT_VERSION` line in `CMakeLists.txt` (and `DUSKLIGHT_REPOSITORY` in `cmake/FetchDusklight.cmake`).

## Tests

The decision logic (`src/nm_logic.h`) has no engine dependency and has unit tests:

```sh
g++ -std=c++17 -Wall -Wextra -I src tests/test_nm_logic.cpp -o test_nm_logic && ./test_nm_logic
```

Expected last line: `ALL TESTS PASSED`. The engine glue (`src/lizalfos_nm.cpp`) cannot be unit-tested outside the game.

## Files

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Template build, target `nightmare_lizalfos`, optional tests target. |
| `mod.json.in` | Mod id, name, version, author, description. |
| `src/mod.cpp` | Entry point, hook declarations, config var, settings panel. |
| `src/lizalfos_nm.h` / `src/lizalfos_nm.cpp` | Engine glue: observation, decisions, movement, shield, poise. |
| `src/nm_logic.h` | Pure decision logic: memory, decay, probabilities, fixed pool. |
| `tests/test_nm_logic.cpp` | Unit tests for `nm_logic.h`. |

## Known limitations

* **Untested in-game.** Hook symbol resolution, balance and the feel of every behaviour are unverified.
* The enum values of `d_a_e_dn.cpp` are file-local and mirrored in `lizalfos_nm.cpp`. If Dusklight renumbers them, update the mirror.
* The vanilla `l_HIO` tuning block is unreachable from a mod, so the 400 unit engage range is a mod constant.
* No new shield-bash animation, and no diagonal run-angle approach: both would need new assets or replacing vanilla code.
* Wolf kick, hookshot and wolf-bite interactions stay vanilla. Heavy hits still knock down, by design.
* Speed multipliers scale displacement. The run animation is not re-timed, so very high speeds can look like foot sliding.
* **Advanced** is untuned. Windows, macOS, iOS and Android builds come only from the template's CI and have never run.
