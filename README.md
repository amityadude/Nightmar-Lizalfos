# Nightmare Lizalfos

A [Dusklight](https://github.com/TwilitRealm/dusklight) mod, built from
[TwilitRealm/mod-template](https://github.com/TwilitRealm/mod-template), that gives the Lizalfos (`e_dn_class`)
an adaptive combat AI. It changes only the Lizalfos. The game source, Link and every other enemy are untouched.

> **Status: compiled and linked against the Dusklight v2.0.2 SDK, logic unit-tested, and the hooks were confirmed
> attaching in-game. The v2 behaviours below have not been play-tested yet**, so expect to tune numbers.

## Difficulty

Two settings in the in-game mods panel (saved in the mod's `config.json`):

| Setting | Values |
|---|---|
| `difficulty` (**Lizalfos difficulty**) | 0 Disabled, 1 Advanced, **2 Nightmare (default)** |
| `damage_level` (**Lizalfos damage**, Nightmare only) | 0 = x1 (vanilla), 1 = x2, **2 = x3 (default)**, 3 = x4, 4 = x6 |

* **Disabled**: vanilla Lizalfos. All mod state is released.
* **Advanced** (the fair tier): longer sight, slightly faster, raises its shield and sidesteps your swings. Blocks only what
  really hits the shield, no invulnerability, normal damage.
* **Nightmare** (the brutal tier, modelled on a Darknut duelist mod):
  * **It reads you.** The moment you press an attack it rolls a 93% to 99% chance to read it, snaps to face you and either
    guards from any angle (the game's own guard state and invulnerability, held about 0.9 s) or hops away (back or sideways).
    Jump strikes are always sidestepped. Spins are mostly back-hopped. Attack again while it is landing and it dodges again.
  * **It punishes.** When your swing ends (blocked or missed), or after a dodge, it swings back within a couple of frames.
    After its own swing it often hops aside rather than stand in your counter.
  * **It is relentless and fast.** Attack gap of 2 to 3 frames (vanilla 30 to 60), engages from 620 units, chains 5 to 7 swings,
    attack animations 1.6x (and 40% of swings 1.25x on top), run speed 1.6x, 3 to 8 frame feints, quick turning.
  * **It gets fiercer.** Every hit you land adds rage (full after 3 hits, or at 35% health): faster swings and run, longer
    chains, a shorter opening, and 1.5x more damage on top of the damage setting.
  * **Poise.** An ordinary hit costs it health but no stagger, and it swings straight back. Jump strikes, helm splitters, spins
    and hidden skills still knock it down, and so do lethal hits, so your finishers work.
  * **It stays beatable.** After 8 reads in a row (up to 11 at full rage) it leaves a gap of 8 frames (down to 4 at full rage,
    never below 3) where it drops its guard and does not read. Every hit you land resets the run. Its health is never changed.
  * Still adapts: it remembers repeated moves, rolling, shield use, healing, arrows and backing away, and leans on the answer
    to them (see below).

## How it works

The Lizalfos runs its whole brain in one file-local function, `action()`, which calls `damage_check()` first. A mod can
only wrap whole functions, so the mod never replaces the vanilla AI:

* **PRE `action()`**: observes Link, reads his swings, decides, and switches the vanilla action or mode before vanilla runs.
* **POST `action()`**: attack speed, movement, chaining, wind-up turning, guard hold and bookkeeping.
* **PRE/POST `damage_check()`**: poise, and (Advanced) shield-by-collision.
* **POST `daE_DN_Execute()`**: scales the damage of its sword and tail (Nightmare).
* **PRE `daE_DN_Delete()`**: frees the actor's state slot.

Hooks use the SDK's `src/d/actor/d_a_e_dn.cpp#<function>` aliases (`DEFINE_HOOK_SYMBOL`). If your Dusklight build has no
`action` symbol, the mod refuses to load and says why. If another hook is missing, the rest still runs and the mods panel
shows which part is OFF ("Active hooks: ...").

### Nightmare decision order

1. **Read** (runs the instant Link presses an attack, from chase, guard, a dodge landing, or while holding or recovering from
   its own swing): guard (snap to face you, vanilla guard state, invulnerable) or dodge. A read that completes the run of
   reads becomes an **opening** instead.
2. **Guard counter**: when your swing ends while it is guarding, it leaves the guard and attacks.
3. **Counter**: after a missed swing, a roll, a dodge or a shrugged-off hit, attack within 1 to 3 frames.
4. **Engage**: attack from up to 620 units with a 3 to 6 frame gap, chain up to 5 to 7 swings, then often hop aside.

Arrows are answered too: mostly a guard (from any angle), sometimes a side hop.

### Adaptive memory

Each Lizalfos keeps a small decayed memory of Link's habits: repeated sword swings and combos, jump attacks, arrows, rolls,
shield use, retreating, attacks from behind and healing. Counts decay over time. Probabilities are hard-capped. Examples:

| Link habit | Response |
|---|---|
| Repeats a move | Higher read chance (up to 99%), more back-hops for spins and sword swings. |
| A move keeps landing | It dodges that kind of swing up to 36% more for 20 seconds. |
| Rolls a lot | Ends up behind Link and attacks as the roll recovers; more wind-up feints. |
| Turtles behind the shield | Prefers the tail sweep and flanking. |
| Favours arrows | Weaves while closing in. |
| Runs away | Chases faster. |
| Heals | Presses the attack for the healing window. |

### Other tactics

Sight range is widened through vanilla's own `pl_range`. Line of sight is a real ray cast every 4 frames (no wallhacks). When
sight is lost it investigates the last-seen position for a bounded time. It alerts idle Lizalfos within 1500 units. Every
mod-driven dodge checks for walls and drops first, and no step is longer than 36 units per frame.

### Debug log

`debug_log` (config, default 1) prints `[NM] ...` lines to the Dusklight Logs window: every read, guard, dodge, attack,
opening and absorbed stagger. Set it to 0 to silence it.

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

* **Balance is untested.** The hooks attach in-game; the v2 Nightmare numbers have not been play-tested. Everything is in
  `src/nm_logic.h` (probabilities, caps) and the constants at the top of `src/lizalfos_nm.cpp`.
* Nightmare guards from any angle on purpose. If it feels unbeatable, lower the read chance or raise the opening in `nm_logic.h`.
* The enum values of `d_a_e_dn.cpp` are file-local and mirrored in `lizalfos_nm.cpp`. If Dusklight renumbers them, update the mirror.
* The vanilla `l_HIO` tuning block is unreachable from a mod, so the 620 unit engage range is a mod constant.
* No new animations: every guard, dodge and swing reuses a vanilla one.
* Wolf kick, hookshot and wolf-bite interactions stay vanilla. Heavy hits still knock down, by design.
* Damage scaling needs the `daE_DN_Execute` hook. If it is OFF in the panel, Nightmare still works but hits for normal damage.
* Speed multipliers scale displacement. The run animation is not re-timed, so very high speeds can look like foot sliding.
* **Advanced** is untuned. Windows, macOS, iOS and Android builds come only from the template's CI and have never run.
