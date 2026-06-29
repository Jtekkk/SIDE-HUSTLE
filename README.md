# SIDE HUSTLE

A roguelike written in **modern C++ (C++23)**. You're a freelancer descending a
procedurally generated *corporate dungeon* — fight Bugs, Needy Clients,
Recruiters and Middle Managers, grab cash and coffee, level up, and claw down
**twelve floors across four themed acts**, past an **act boss** at the foot of
each, to confront the CEO and go full-time on your side hustle.

It ships in **two front-ends over one shared engine**:

- a **graphical** version (GPU-rendered window via [raylib](https://www.raylib.com/)), and
- a **terminal** version (truecolor ANSI, runs anywhere a terminal does — no deps).

![SIDE HUSTLE — graphical version](docs/screenshot.png)

The same `sh::Game` simulation drives both; the front-ends are pure view + input
layers (see `src/game/Game.hpp`'s read-only API and `advance()`).

The terminal version, for comparison:

```
  SIDE HUSTLE — a corporate dungeon crawl
  #######  ############
  #.....#  #..........#
  #.....####..........####      .
  ###....................@M......
HP [##########----------] 18/30   Floor 2   Lvl 2 (xp 4/30)   $44
```

## The descent — story & acts

The run is a twelve-floor descent down the corporate ladder, split into four
**acts** (three floors each). Each act has its own zone name, its own narrative
beats that play in the message log as you arrive, and an **act boss** guarding
the stairs — you can't leave a floor until its guardian falls. Every boss shares
the telegraphed `&` moveset (periodic summons + a wind-up radial shockwave),
scaled up act by act:

| Act | Zone | Floors | Act boss |
|:---:|------|:------:|----------|
| I | The Open Floor | 1–3 | **the Scrum Lord** — *"This wasn't in the sprint."* |
| II | Middle Management | 4–6 | **the Regional VP** — wants to "circle back", with your skull |
| III | The Executive Suite | 7–9 | **the Board Chair** — demands a "quick sync" (no agenda) |
| IV | The C-Suite | 10–12 | **the CEO** — *"Let's talk equity."* |

The story canon (act for a floor, which boss guards it, the beats that play, the
ending) all lives in one header, [`src/game/Story.hpp`](src/game/Story.hpp), so
the engine and both front-ends read the same script. Beat that final fight and
you walk out a free founder.

## Build & play

With CMake:

```sh
cmake -B build && cmake --build build
./build/side-hustle
```

Or with the bundled Makefile (no CMake needed):

```sh
make run        # build and play
make selftest   # headless self-play smoke test
```

Requires a C++23 compiler (GCC 13+ or Clang 17+).

### Graphical version (raylib)

The renderer is a small **deferred-style 2D lighting pipeline** with GLSL shaders:

- **Procedural HD textures** — stone floors (cellular noise) and rough rock walls
  (Perlin noise) generated at startup, plus 2.5D wall blocks with lit top edges
  and dark front faces for real depth.
- **Normal-mapped per-pixel dynamic lighting** — normal maps are derived from
  the noise heightfields so floors and walls catch the torch with real relief.
  A light buffer is accumulated with one soft light per *visible* tile (so walls
  actually occlude light), and the composite shader adds directional shading
  from up to 8 lights (a flickering player torch + nearest item/stairs/boss
  lights), gated by the occluded light so shadows are preserved.
- **Composite shader** — `albedo × (ambient + light)` with a filmic tonemap,
  saturation lift, and vignette; explored-but-unseen tiles fall back to a cool
  ambient (the classic warm-light / cool-shadow look).
- **Bloom** — the lit scene is downsampled and run through a separable gaussian
  blur (shader) for glow.
- **Volumetric light shafts (god-rays)** — a radial-blur shader with a
  bright-pass casts soft shafts from the torch, plus drifting **dust motes** in
  the torchlight.
- Smooth fractional-scroll camera, tweened movement + idle-bob, a particle
  system, floating damage numbers, mipmapped TrueType text, and shaded sprites
  with outlines and eyes. The cyan hero is hue-separated from gold loot and warm
  enemies for instant readability.

The GUI build links **raylib statically** and embeds its font, so on Windows it's
a single self-contained `.exe` (no DLLs, no asset files). raylib's prebuilt
static libs aren't committed — fetch them once, then build:

```sh
scripts/fetch-raylib.sh     # downloads raylib 5.5 libs into third_party/

make gui                    # -> ./side-hustle-gui      (Linux window)
make windows-gui            # -> ./side-hustle-gui.exe  (Windows, single file)
```

Same controls as below, plus: choose a difficulty on the title screen, and press
**Enter** on the stairs to descend. A virtual-framebuffer smoke test is built in:
`SH_SMOKE=120 ./side-hustle-gui` auto-plays ~120 frames and exits (used to
validate the render loop headlessly).

### Windows

The game is cross-platform — the only OS-specific code is the raw-terminal
layer in `core/Terminal.hpp`, which has a Windows Console backend behind
`#ifdef _WIN32`. Build a native `.exe` two ways:

```sh
# On Windows with MSYS2 / MinGW:
g++ -std=c++23 -O2 -Isrc $(find src -name '*.cpp') -o side-hustle.exe

# Cross-compiling from Linux with mingw-w64:
make windows        # -> ./side-hustle.exe
```

The cross-compiled `.exe` is statically linked, so it's a single self-contained
file that depends only on stock Windows DLLs (`KERNEL32`, `msvcrt`) — just
double-click it or run it from `cmd` / PowerShell. Use a modern terminal
(Windows Terminal, or Windows 10+ conhost) for the ANSI colors.

## Controls

| Keys | Action |
|------|--------|
| `w a s d`, `h j k l`, or arrow keys | Move / bump-to-attack |
| `y u b n` | Move diagonally |
| **Shift + direction** (GUI) / **uppercase `H J K L Y U B N`** (terminal) | **Shove** — kick a foe back hard, or push a barrel/crate |
| **Ctrl + direction** (GUI) / **`Z` + direction** (terminal) | **Dash** — lunge up to 3 tiles (cooldown), close gaps or escape |
| **`x`** | **Slam** — ground-pound: AoE knockback + stun to everything adjacent (cooldown) |
| `e` | Drink a coffee (heals, if you're carrying one) |
| `.` or space | Wait a turn |
| `>` | Descend the stairs (when standing on `>`) |
| `q` | Quit |

Movement is turn-based: every step you take, the monsters take one too. Walk
into a monster to attack it. Find the `>` stairs on each floor to go deeper —
a **descent compass** (a gold arrow at the edge of the view, plus an `Exit (>)`
bearing in the HUD like *"24 tiles SE"*) always points the way, so you never
get lost on the big floors. But every third floor an **act boss (`&`) guards
the stairs**, and on the **final floor the CEO (`&`) blocks the exit**, so
you'll have to put each of them down to win
(see [The descent](#the-descent--story--acts)).

### Command-line flags

```sh
./side-hustle --easy            # gentler: more HP, fewer/weaker foes
./side-hustle --hard            # brutal: tougher, denser foes
./side-hustle --seed=12345      # reproducible dungeon
./side-hustle --name=ada        # name on the leaderboard
./side-hustle --no-scores       # don't read/write the score file
```

Runs are scored on depth, level and cash (with a bonus for escaping) and saved
to a local `side-hustle-scores.txt` leaderboard shown on the title and
game-over screens.

### Physics & interactions

Combat isn't just "bump until someone dies" — every hit is a **knockback**, and
positioning is the game:

- A hit shoves the target along the strike direction; distance = attacker
  **force** − target **weight**. Light foes (Bugs) fly; heavy ones (Managers)
  barely move — and Managers shove *you*.
- Momentum resolves into the world: slammed into a **wall** = impact damage;
  into **another enemy** = both hurt and the second is knocked too (chains);
  onto **spikes** = bleed; into a **pit** = instant death; into an **explosive
  barrel** = detonation.
- **Barrels** (`0`) are pushable (Sokoban-style) and chain-explode in a radius —
  set up combos by knocking a foe into one beside a pack.
- **Pits** (`O`) and barrels are obstacles enemies path *around*, so terrain
  becomes a weapon you exploit with forced movement.
- **Shove** (Shift+direction) is a dedicated kick: knock a foe back hard for
  almost no damage (pure setup), or push a **crate**/barrel several tiles.
- **Crates** (`=`) are inert cover: shove one into a **pit to fill it**, into a
  foe to crush it, or in front of a **Phisher** to block its shots.
- **Status effects**: a hard wall/crate slam **stuns** a foe (it skips a turn);
  barrel blasts set things **on fire** (damage over time — including you).
- **Phishers** (`p`) are ranged: they keep their distance and fire **projectiles**
  down straight lines. Break their line of fire, dodge, or hide behind a crate.

### What you'll meet

| Glyph | Thing | Notes |
|:-----:|-------|-------|
| `@` | You | Don't let your HP hit zero |
| `b` | Bug | Weak, light — flies far when hit |
| `c` | Needy Client | Hits harder |
| `r` | Recruiter | Tough, heavier |
| `p` | Phisher | Ranged — kites and fires projectiles down straight lines |
| `M` | Middle Manager | Heavy: resists knockback, shoves you hard |
| `&` | Act boss / CEO | Guards the stairs every third floor; summons minions and telegraphs a radial shockwave. Four of them, one per act, ending with the CEO |
| `$` | Cash | Score |
| `!` | Coffee | Stashed in your bag; drink with `e` to heal |
| `/` | Better laptop | Permanent attack upgrade |
| `O` | Pit | Knock enemies in for an instant kill (and don't get shoved in) |
| `^` | Spikes | Hurts whatever steps or is shoved onto it |
| `0` | Barrel | Push it; detonates (and burns) when something slams into it |
| `=` | Crate | Inert cover — shove into pits to fill them, or to block Phishers |

## Architecture & the "advanced C++"

The code is split into small, focused translation units under `src/`:

```
src/
├── core/
│   ├── Vec2.hpp        integer 2D vector; defaulted operator<=>, std::hash spec
│   ├── Rng.hpp         seedable std::mt19937_64 wrapper, concept-constrained pick()
│   ├── Grid.hpp        generic Grid<T> constrained by a GridCell concept
│   └── Terminal.hpp    RAII raw-mode terminal guard (restores on scope exit)
├── world/
│   ├── Map.hpp         tiles + visibility flags
│   ├── DungeonGen.*    BSP dungeon generation over a std::unique_ptr tree
│   ├── Fov.*           recursive shadow-casting field of view
│   └── Pathfinding.*   A* with a custom-hashed Vec2 and std::priority_queue
├── game/
│   ├── Components.hpp  data-oriented Entity / Item structs
│   ├── Story.hpp       acts, act bosses, per-floor narrative beats (canon)
│   ├── Scores.*        persistent leaderboard (std::filesystem + fstream)
│   └── Game.*          turn loop, combat, progression, rendering, menus
└── main.cpp            arg parsing; interactive vs. --selftest
```

Techniques on display:

- **C++20 concepts** constrain the generic `Grid<T>` (`GridCell`) and the
  `Rng::pick` iterator template (`std::forward_iterator`).
- **RAII** owns the terminal: `Terminal`'s destructor always restores cooked
  mode and the cursor, so a crash or early exit can never leave your shell
  broken.
- **Smart pointers**: the BSP partition tree is built from
  `std::unique_ptr<BspNode>` and frees itself automatically.
- **Defaulted `operator<=>`** gives `Vec2` all its comparisons for free, and a
  `std::hash<Vec2>` specialization lets it key the A* `unordered_map`/`set`.
- **Algorithms & ranges**: `std::clamp`, `std::ranges::count_if`,
  `std::priority_queue`, `<random>` distributions.
- **24-bit "truecolor" renderer** with distance-based FOV light falloff
  (`core/Ansi.hpp` does the RGB lerp/scale math), Unicode shaded walls and a
  box-drawing UI frame, plus a gradient HP bar. Best in a truecolor terminal
  (most modern ones, incl. Windows Terminal); UTF-8 output is handled per-OS.
- **`std::filesystem` + `std::fstream`** persist the high-score table, with a
  round-tripping text format and graceful handling of a missing/corrupt file.
- **`std::format`** builds every HUD, menu and log line; **designated
  initializers** populate score records.
- Classic game-AI algorithms implemented from scratch: **BSP** level
  generation, **recursive shadow-casting FOV**, and **A\*** monster pathfinding.

## Headless self-test

`./side-hustle --selftest [--seed=N]` runs the full simulation with no terminal:
it auto-paths the player toward the stairs floor by floor (exercising
generation, FOV, A\*, combat and progression), prints a per-floor report and an
ASCII map snapshot, and exits. Handy as a CI smoke test that the engine builds
and runs end-to-end.
