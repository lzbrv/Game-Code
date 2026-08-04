# Trace

A 5v5 competitive arena shooter prototype for **Unreal Engine 5.8**, written entirely in C++.

Trace is a networking prototype first and a game second. It exists to prove out a real
multiplayer stack — client-side prediction, server-authoritative hitscan with server-side rewind,
delta-replicated state — on top of a game idea that is small enough to actually finish.

**There are no binary assets in this repository.** No `.uasset` we authored, no Blueprints. The
arena, the Core, the trail and the entire HUD are built in C++ from engine primitives
(`/Engine/BasicShapes/*` and Canvas drawing). The only content file in the repo is one empty level.
This is a deliberate constraint: it keeps the repo diffable, keeps merges sane for a four-person
team, and means a fresh clone builds and runs with zero asset pipeline.

**One exception, and it bites new clones:** the characters use Epic's default Mannequin
(`SKM_Manny_Simple` + `ABP_Unarmed`) for heads, limbs and run cycles. That is ~126 MB of imported
engine content and it is **gitignored**, so it is not in a fresh clone. Run
`Scripts/import-mannequin.sh` once per machine. Without it `ATraceCharacter` falls back to
primitives and everyone on the field is a capsule — if that is what you are looking at, this is why.

---

## The game

Two teams of five, one shared Core, one arena.

### Scoring

- There is **one Core**, and **both teams contest the same object**. It is not a flag-per-team
  setup — there is a single ball in play.
- You score by carrying the Core into the **opposing team's endzone** — or by **passing** it to a
  teammate who is already standing in it. The alley-oop is intended.
- A match is **two 10-minute halves with a side switch**; the highest score at the end wins.
  (Everything here is configurable — see [docs/DESIGN.md](docs/DESIGN.md).)

### The carrier

This is the part that makes Trace different from every other capture-the-thing game:

- **The carrier is invulnerable to bullets.** Not tanky — *immune*. Nothing hitscan can hurt them.
  You cannot shoot the carrier down. Ever.
- **The carrier cannot shoot.** Picking up the Core holsters your gun.
- **The carrier continuously lays a trail** behind them — a visible ribbon marking exactly where
  they have been for the last few seconds.

### The trail — the only counterplay

- An **enemy of the carrier** who passes through the trail **while dashing** kills **the carrier**.
- **Walking or running through the trail does nothing.** No damage, no slow, no effect at all.
  You must be mid-dash for the trail to trip.
- **Teammates never trip the trail.** The carrier's own team can run through it freely.
- Dash is therefore the *only* way to stop a carrier. Not damage. Not focus fire. A committed,
  cooldown-gated dash through the line they just drew.
- **The carrier can parry it.** A 0.1-second window that makes the trace invulnerable and turns the
  **entire trace red** for the duration. Dash into a red trace and you have wasted it. It is a
  reaction check, not a shield — 0.1 s of cover on a 1.5 s cooldown.

The design consequence: chasing a carrier is a positioning puzzle, not an aim duel. The carrier is
trying to draw a path that no one can safely cut; the defenders are trying to spend a dash at the
one moment the geometry works. Escorts matter, because a friendly body standing where the enemy
needs to dash is a real wall.

### Everyone else

- Everyone **not** carrying the Core has a **hitscan** gun — instant, no projectile travel time.
- **No friendly fire.**
- Bullets never damage the Core carrier (see above).
- **Everyone**, carrier included, has a **dash** on a short cooldown and a **slide**. There is no
  boost — it was removed.
- **Movement is Source/Apex-flavoured.** Real Quake-style air acceleration, so strafing in mid-air
  turns your velocity vector instead of braking it; landing does **not** clamp your speed to the
  ground maximum, it bleeds the excess off over a short run-out; and run→jump→slide→jump preserve
  the velocity vector rather than resetting it.
- The Core is **passed by holding the crosshair on a teammate for half a second** — it is not a
  thrown, catchable ball. The moment you start a pass **your shield drops and your trace goes
  invulnerable**, and both come back if you cancel. That half second is the risk beat the whole
  design turns on.
- When the carrier dies the trail is **cleared instantly**.

---

## Repo layout

```
Trace.uproject              Project descriptor. Pins EngineAssociation to "5.8" — see docs/GITHUB.md.
Config/
  DefaultEngine.ini         Engine-level config: default map, net driver, renderer.
  DefaultGame.ini           Gameplay tunables — [/Script/Trace.TraceSettings] lives here.
  DefaultInput.ini          Switches the engine to Enhanced Input classes, mouse/console keys.
Content/
  Maps/                     The one and only content directory. Holds Arena.umap (see Setup).
Scripts/
  build.sh                  Wraps UnrealBuildTool. Also --projectfiles and --clean.
  generate-map.sh           Creates the empty /Game/Maps/Arena level headlessly.
  generate_map.py           The editor-Python it drives. Not a standalone python3 script.
  generate_content.py       Produces M_TraceSurface / M_TraceNeon into /Game/Generated/Materials.
  import-mannequin.sh       Imports Epic's Mannequin (~126 MB). Run once per machine or every
                            character on the field is a capsule.
  run-listen-server.sh      Host a listen server on :7777. The default way to play right now.
  run-client.sh             Connect a client to <ip>:7777.
  run-dedicated-server.sh   Headless server. Needs --editor on a launcher engine — see NETWORKING.
  setup-lfs.sh              One-time Git LFS bootstrap after cloning.
  _trace_common.sh          Shared library: finds the engine, checks the toolchain.
  *.bat                     Windows counterparts of all of the above, same flags. See SETUP.
Source/
  Trace.Target.cs           Standalone game target.
  TraceEditor.Target.cs     Editor target — this is what you normally build.
  TraceServer.Target.cs     Dedicated server target.
  Trace/
    Trace.Build.cs          Module dependencies.
    Trace.h / Trace.cpp     Module boilerplate + the LogTraceGame log category.
    TraceTypes.h            Shared enums, team colours, the replicated trail structs.
    TraceSettings.{h,cpp}   UTraceSettings — every gameplay number, ini-configurable.
    Core/                   GameMode, GameState, PlayerState, PlayerController, Character.
    Movement/               TraceCharacterMovementComponent — predicted dash and slide,
                            Source-style air acceleration, carried-momentum landing.
    Gameplay/               Health, Weapon, HitZones, Trail, Core, Parry, Endzone, Tracer.
    Settings/               TraceUserSettings — persisted per-player controls and video.
    Net/                    TraceLagCompensationComponent — pose history + server rewind.
    UI/                     TraceHUD — Canvas-only HUD and scoreboard.
    World/                  TraceArenaBuilder, TraceTeamPlayerStart.
docs/                       SETUP, EDITOR, NETWORKING, GITHUB, DESIGN. You are here.
.gitattributes              Git LFS + file-locking rules. Read docs/GITHUB.md.
.gitignore                  Everything generated. Read docs/GITHUB.md.
```

Every script prints the raw engine command it is about to run before running it, so you can read
one line of terminal output and learn the underlying `Build.sh` / `UnrealEditor` invocation. They
are conveniences, not abstractions — you can always copy the printed command and run it yourself.

They locate your engine automatically. If yours is somewhere unusual, either
`export UE_ROOT="/path/to/UE_5.8"` or write that path into a `.ue-root` file at the repo root
(gitignored, per-developer).

Generated directories — `Binaries/`, `Intermediate/`, `DerivedDataCache/`, `Saved/` — are **not**
in the repo and must never be committed. They are rebuilt from source on every machine.
See [docs/GITHUB.md](docs/GITHUB.md) for why this matters more in Unreal than anywhere else.

---

## Quickstart (5 minutes — *if* Xcode and UE 5.8 are already installed)

If they are not, stop here and read **[docs/SETUP.md](docs/SETUP.md)** first. Installing the
toolchain is a multi-hour, ~100 GB job, not a five-minute one.

```bash
# 0a. One-time: make sure a *pinned* Xcode 26.1.1 (not just Command Line Tools) is selected.
#     26.4+ is incompatible with UE 5.8, and the App Store currently gives you 26.6.
xcode-select --print-path        # must print /Applications/Xcode-26.1.1.app/Contents/Developer

# 0b. One-time: Xcode 26 no longer bundles the Metal compiler. Without this the editor
#     dies with "cannot execute tool 'metal' due to missing Metal Toolchain". ~705 MB.
xcodebuild -downloadComponent MetalToolchain
xcrun -sdk macosx metal --version    # must print a version, e.g. Apple metal version 32023.830

# 1. Clone. Git LFS must be installed *before* you clone or binaries arrive as text pointers.
git lfs install
git clone https://github.com/lzbrv/Game-Code.git trace
cd trace
Scripts/setup-lfs.sh             # verifies the LFS filters took effect

# 2. Generate IDE project files, then build the editor target.
#    First build is 5-20 minutes; incremental builds afterwards are seconds.
Scripts/build.sh --projectfiles
Scripts/build.sh

# 3. Create the one required level (see below).
Scripts/generate-map.sh

# 4. Import the character art (~126 MB, gitignored, once per machine).
#    Skip this and every player on the field is an untextured capsule.
Scripts/import-mannequin.sh

# 5. Open it.
open Trace.uproject
```

**The one manual step, once per repo:** the level `/Game/Maps/Arena` must exist. It is an
*entirely empty* level — the arena builds itself in C++ at `BeginPlay`, so this asset is pure
boilerplate. `Scripts/generate-map.sh` creates it for you, but note what it actually is: it drives
`Scripts/generate_map.py` inside the editor via Unreal's `pythonscript` commandlet. It is **not** a
standalone `python3` script and running it with your system Python will not work — nothing outside
the engine can write a `.umap`. It also needs the **Python Editor Script Plugin** enabled. If the
plugin is off, the run fails immediately with "the pythonscript commandlet could not be found";
enable it (Edit → Plugins → *Python Editor Script Plugin*, restart) or just make the level by hand:
File → New Level → **Empty Level**, save as `Content/Maps/Arena`. Full detail in
[docs/SETUP.md](docs/SETUP.md#the-one-manual-step-gamemapsarena).

Then press **Play**. To test multiplayer immediately, set the PIE player count to 2 and net mode
to *Play As Listen Server*. For two real processes:

```bash
Scripts/run-listen-server.sh                   # window 1: host
Scripts/run-client.sh 127.0.0.1 --pos 700,0    # window 2: joiner
```

A **listen server is the supported path today.** A true dedicated server (`TraceServer`) cannot be
built from a Launcher-installed engine — the build fails with *"Server targets are not currently
supported from this engine distribution"* — so it needs a source build of the engine. The target
file is correct and stays in the repo for when someone does that; until then use the listen server,
or `Scripts/run-dedicated-server.sh --editor`, which runs the editor binary headlessly with
`-server` and needs no server target.

See [docs/NETWORKING.md](docs/NETWORKING.md#3-dedicated-server--requires-a-source-built-engine).

### Controls

| Input | Action |
|---|---|
| `W` `A` `S` `D` | Move |
| Mouse | Look |
| `Space` | Jump |
| **Left Mouse** | Fire (disabled while carrying the Core) |
| **Right Mouse** | Pass — hold on a teammate for half a second |
| **Left Shift** | Dash |
| **Left Ctrl** | Slide (on the ground) / fast-fall (in the air) |
| **`Q`** | Parry — carrier only. 0.1s of trace invulnerability; the whole trace flashes red |
| `Tab` | Scoreboard |

Input is Enhanced Input, constructed entirely in C++ at runtime — there are no input `.uasset`s to
open. **Every action above is rebindable in-game** (Options → Controls); the bindings persist
through `UTraceUserSettings`, and the defaults live in `Source/Trace/Settings/TraceUserSettings.cpp`.

---

## Documentation

| Doc | Read it when |
|---|---|
| **[docs/SETUP.md](docs/SETUP.md)** | Setting up a Mac from zero. Start here on a new machine. |
| **[docs/NETWORKING.md](docs/NETWORKING.md)** | You want to playtest — locally, or with the four of us across the internet. |
| **[docs/GITHUB.md](docs/GITHUB.md)** | **Before your first commit.** Unreal + Git has rules that are not optional. |
| **[docs/EDITOR.md](docs/EDITOR.md)** | New to the Unreal Editor, or wondering why the viewport is empty. Panels, PIE, and the arena preview button. |
| **[docs/DESIGN.md](docs/DESIGN.md)** | Tuning the game, or adding a feature. Full knob table, the arena layout and the class map. |

---

## Status

Prototype. It plays, it replicates, and it is not balanced. The netcode is the part that is meant to
be production-shaped; the arena is Tron-styled but is still made of engine primitives.

It currently has: a title menu with difficulty selection, a post-match result screen, two-half match
flow with a side switch, first-person with a third-person blend while carrying, animated Epic
Mannequin characters, a 24000 × 9600 neon arena built in C++, bots that play the full ruleset, an
editor arena-preview button, and every gameplay number live-editable in Project Settings while
Play-In-Editor is running.
