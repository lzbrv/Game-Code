# Trace

A 5v5 competitive arena shooter for **Unreal Engine 5.8**, written entirely in C++.

Trace is a networking prototype first and a game second. It exists to prove out a real
multiplayer stack — client-side prediction, server-authoritative hitscan with server-side rewind,
delta-replicated state — on top of a game idea that is small enough to actually finish.

**The gameplay logic is all C++, and the repository also contains real, editable assets.** For
most of this project's life it held no authored binaries at all — the arena, the Core, the trail and
the whole HUD were built in code from engine primitives (`/Engine/BasicShapes/*` and Canvas
drawing). The procedural builder still exists, but the repo now tracks
**~1,650 binary files, ~70 MB via Git LFS**: the baked shipping arena `/Game/Maps/Arena_Baked`
(647 One-File-Per-Actor packages), its materials, the ten character bodies and the animation set they
share, the UI art and font atlases, the input and character data assets, the sound bank sources under
`Art/Sounds/`, and two collaborator test maps (`Fish_Arena_Test`, `Fish_Map_Test`).

**What that means for you in practice:** binary assets cannot be merged, so the team locks a file
*before* editing it rather than resolving conflicts afterwards. `Content/**` is checked out
read-only until you take a lock — that is deliberate, and `chmod` is not the fix. Read
**[docs/GITHUB.md](docs/GITHUB.md) §4 before you open anything in the editor.** A fresh clone is
about 70 MB of LFS payload plus a small Git object store.

**One exception worth understanding:** Epic's default Mannequin (`SKM_Manny_Simple` + `ABP_Unarmed`)
is still needed. Each of the ten characters now has its own committed body — `SK_Rocco`, `SK_Elle`
and the rest under `Content/Trace/Characters/<Name>/` — and all ten are bound to one shared skeleton
so one retargeted animation set (`Content/Trace/Characters/Shared/Anims/`) drives every one of them.
But that set is **retargeted from** Epic's `ABP_Unarmed`, the Mannequin is still the fallback body
when the character art is missing, and the practice-range dummies are Mannequins by design (see
[docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md) item 33). That engine content is ~126 MB and
**gitignored**, so it is not in a fresh clone — it is copied out of the Unreal install you already
have.

`Scripts/build.sh` (and `build.bat`) **imports it automatically** when it is missing, so a
fresh clone just works. Pass `--no-art`, or set `TRACE_SKIP_ART_IMPORT=1`, to skip that. To run it
by hand: `Scripts/import-mannequin.sh` on macOS/Linux, `Scripts\import-mannequin.bat` on Windows.

If every player on the field is a plain capsule, the import has not run: the game says so on screen
and names the script. The usual cause is an engine installed without **Templates and Feature Packs**
ticked, since that is where the source art lives — the import script detects that and tells you how
to fix it in the Epic Games Launcher.

---

## The game

Two teams of five, one shared Core, one arena.

### Scoring — the Core is a ball, and it flies

- There is **one Core**, and **both teams contest the same object**. It is not a flag-per-team
  setup — there is a single ball in play.
- The default mode is **goals** (`ScoringMode=ThrownCoreAndGoals`, mode B): each end of the field
  has a raised goal mouth about 2,000 uu wide, and you score by getting the Core through the
  **opposing team's goal** — carried in or **thrown** in.
- **The Core is a physical, thrown, interceptable object.** While carrying, **left mouse charges a
  throw and releases it the instant you let go** — a tap is a short lob (15% power), a 0.6 s hold
  is full power, and the throw inherits your own momentum, so a jumping or sprinting throw
  genuinely carries. **The first player to contact a loose Core picks it up**, both teams alike;
  a generous catch magnet (450 uu) funnels near-misses into a catcher's hands, so interception is
  a real defensive play, not a pixel hunt.
- **Turnovers have a rule of their own:** when the Core changes hands between teams by hitting the
  ground, the team that dropped it is locked out for 5 s, and anyone on the other team can
  **pull** it to themselves — hover the crosshair on it and hold (0.3 s ring, line of sight
  required). The pull is on your melee button when the pull ring is on screen, or on `G` as a
  dedicated bind.
- A match is **two 8-minute halves with a side switch**; the highest score at the end wins. The
  half does not cut a play dead: when the clock expires the whistle waits for the next dead ball
  (a goal, a turnover between teams, or the Core coming down — capped at 60 s). A lead of **8**
  ends the match early (mercy rule). There is no "first to N wins" — the clock is the win
  condition.
- The older **endzone mode (A)** still exists behind `?mode=a` and plays the original hover-pass
  ruleset — frozen, and without characters or abilities. In the default goals mode the
  half-second hover-pass still exists alongside the throw (`PassHoldSeconds=0.5`).

### The carrier

This is the part that makes Trace different from every other capture-the-thing game:

- **The carrier is invulnerable to bullets.** Not tanky — *immune*. Nothing hitscan can hurt them.
  You cannot shoot the carrier down. Ever.
- **The carrier cannot shoot.** Picking up the Core holsters your gun.
- **The carrier is fast** — 1.22× run speed, the same speed as a knife carrier.
- **The carrier continuously lays a trail** behind them — a visible ribbon marking exactly where
  they have been for the last couple of seconds.

### The trail — the counterplay

- An **enemy of the carrier** who passes through the trail **while dashing** kills **the carrier**.
- **Walking or running through the trail does nothing.** No damage, no slow, no effect at all.
  You must be mid-dash for the trail to trip.
- **Teammates never trip the trail.** The carrier's own team can run through it freely.
- Dash is the way to stop a carrier who never lets go of the ball. In goals mode the other lever
  is the ball itself: force a bad throw, intercept it, win the turnover pull.
- **The carrier can parry.** A 0.175-second window that makes the trace invulnerable and turns the
  **entire trace red** for the duration. Dash into a red trace and you have wasted it. It is a
  reaction check, not a shield — 0.175 s of cover on a 1.5 s cooldown.

The design consequence: chasing a carrier is a positioning puzzle, not an aim duel. The carrier is
trying to draw a path that no one can safely cut; the defenders are trying to spend a dash at the
one moment the geometry works. Escorts matter, because a friendly body standing where the enemy
needs to dash is a real wall.

### Characters and abilities

Ten playable characters — **Rocco, Lily, Roxie, Elle, Slimeball, Mortimer, Chut, Mace, Oyster
and X** — picked on a select screen, each with its **own body mesh, silhouette and accent colour**
and each with two abilities on top of the shared kit:

- **`E`** — the activated ability (Elle's teleport gates, Mace's spike, Oyster's poison jar,
  Mortimer's quake, X's swarm mark, …).
- **`V`** — the movement ability (Lily's zip, Rocco's ride, Roxie's rocket, …).

Abilities are server-authoritative like everything else, and they exist only in the default goals
mode — mode A predates the roster and disables them.

### Everyone else

- Everyone **not** carrying the Core has three weapons: a **hitscan pistol** (`1`), an **SMG**
  (`2`) and a **knife** (`3`), with `R` to reload and a melee attack on right mouse.
- **No friendly fire.**
- Bullets never damage the Core carrier (see above).
- **Everyone**, carrier included, has a **dash** (charge-based, 3.5 s cooldown) and a **slide**.
  There is no boost — it was removed.
- **Movement is Source/Apex-flavoured.** Real Quake-style air acceleration, so strafing in mid-air
  turns your velocity vector instead of braking it; landing does **not** clamp your speed to the
  ground maximum, it bleeds the excess off over a short run-out; and run→jump→slide→jump preserve
  the velocity vector rather than resetting it.
- **Wall jump** is on (`bWallJumpEnabled`), and there is a **surf** verb: hit a face steeper than
  the engine's walkable limit and you slide along it instead of scraping down it, gaining speed up
  to a derived ceiling. Surf has one large caveat — **the shipping map has nothing to surf on.** The
  rails are built by the procedural arena builder, and `Arena_Baked` is a baked level that builds
  nothing, so you have to launch `/Game/Maps/Arena` to try it. See
  [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md) item 30.
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
  Maps/                     Arena.umap (empty; the procedural builder fills it), Arena_Baked.umap
                            (THE shipping map), MainMenu.umap, and the Fish_* collaborator test maps.
  Trace/                    Committed authored assets: Materials, UI art + font atlases, Input,
                            Data (character data assets), Audio, Weapons, Art (the kit meshes), and
                            Characters/ — the ten bodies, the one shared skeleton, and the one
                            retargeted animation set under Characters/Shared/Anims.
  __ExternalActors__/       One .uasset per actor of the baked maps. This is One File Per Actor — it
                            is what lets several people edit one map without fighting over the .umap.
Art/                        Source-form art (not .uasset): Sounds/ (71 WAVs, the generated bank —
                            no provenance note yet, see KNOWN_LIMITATIONS item 8), Fonts/ (Lato +
                            the licensed-face rules), Pack/, Characters/, Railgun/, Smg/.
Scripts/
  build.sh                  Wraps UnrealBuildTool. Also --projectfiles and --clean.
  bake-arena.sh             Re-bakes the procedural arena into the editable /Game/Maps/Arena_Baked.
  generate-map.sh           Recreates the empty /Game/Maps/Arena level headlessly (rarely needed —
                            the level is committed).
  generate_content.py       Produces the M_TraceSurface / M_TraceNeon material parents.
  import-mannequin.sh       Imports Epic's Mannequin (~126 MB) from your own engine install.
                            build.sh runs this for you when the art is missing.
  import-characters.sh      The generated-character pipeline, one stage per editor run: generate →
                            materials → import → retarget → portraits. The assets are committed;
                            you only need this if you change a body recipe.
  run-listen-server.sh      Host a listen server on :7777. The default way to play right now.
  run-client.sh             Connect a client to <ip>:7777.
  run-dedicated-server.sh   Headless server. Needs --editor on a launcher engine — see NETWORKING.
  run-practice-range.sh     Boot straight into the practice range. (Windows: the .bat twin.)
                            The owner's first-person arms rig is drawn HERE AND NOWHERE ELSE;
                            `Trace.Practice.ArmsRig 0` in the console swaps back to the pack
                            hands for an A/B without relaunching.
  verify-practice-range.sh  Three headless runs that prove the practice range's cheats cannot leak
                            into a real match — red arm first, then green, then the range itself.
  setup-lfs.sh              One-time Git LFS bootstrap after cloning.
  setup-hooks.sh            One-time: points git at .githooks/ (config-hygiene tripwire + LFS).
  lock.sh / unlock.sh       Take/release the Git LFS lock you need BEFORE editing an asset.
  _trace_common.sh          Shared library: finds the engine, checks the toolchain.
  *.bat                     Windows counterparts, same flags. See SETUP.
Source/
  Trace.Target.cs           Standalone game target.
  TraceEditor.Target.cs     Editor target — this is what you normally build.
  TraceServer.Target.cs     Dedicated server target.
  Trace/
    Trace.Build.cs          Module dependencies.
    TraceTypes.h            Shared enums, team colours, the replicated trail structs.
    TraceSettings.{h,cpp}   UTraceSettings — every gameplay number, ini-configurable.
    Abilities/              UTraceAbilityComponent + the ten per-character ability sets.
    AI/                     TraceBotController — bots that play the full ruleset.
    Audio/                  TraceAudio, the sound bank, sound events, audio verification.
    Core/                   GameMode, GameState, PlayerState, PlayerController, Character, roster.
    Data/                   Character definitions and data verification.
    Debug/                  Input harness, stats dump, verification probes.
    Gameplay/               Health, Weapon, HitZones, Trail, Core, Parry, Endzone, Tracer, Melee.
    Modes/                  The practice range (game mode, actors, verification).
    Movement/               TraceCharacterMovementComponent — predicted dash, slide, wall jump
                            and surf; Source-style air acceleration, carried-momentum landing.
    Net/                    TraceLagCompensationComponent — pose history + server rewind.
    Settings/               TraceUserSettings — persisted per-player controls, audio and video.
    UI/                     TraceHUD, menus, character select, kill feed, the bitmap-font text
                            renderers (UI/Text/), UMG widget generators (UI/Widgets/).
    World/                  TraceArenaBuilder, the bake pieces, team player starts.
docs/                       Nine docs + generated stats. See the Documentation table below.
                            KNOWN_LIMITATIONS.md is the one to read before filing a bug.
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
#    build.sh also imports the Mannequin character art when it is missing —
#    ~126 MB copied from your own engine install, not from GitHub.
Scripts/build.sh --projectfiles
Scripts/build.sh

# 3. Open the project. All maps are committed — there is no map-generation step.
open Trace.uproject
```

Open `Content/Maps/Arena_Baked.umap` and press **Play**. To test multiplayer immediately, set the
PIE player count to 2 and net mode to *Play As Listen Server*. For two real processes:

```bash
Scripts/run-listen-server.sh                   # window 1: host
Scripts/run-client.sh 127.0.0.1 --pos 700,0    # window 2: joiner
```

### The two arenas: `Arena_Baked` ships, `Arena` regenerates

**`/Game/Maps/Arena_Baked` is the shipping map.** It is the server default map
(`Config/DefaultEngine.ini`), and both PLAY and PRACTICE on the title screen travel to it
(`TraceMaps::Arena` in `Source/Trace/UI/TraceMatchOptions.h`). It is a real level of **647 placed,
individually selectable, readably labelled actors** (`Wall_North_01`, `Cover_37`,
`Goal_Ring_Rim_12`) — 559 of them arena geometry the builder recognises, the rest lights, volumes and
starts — with **One File Per Actor** enabled so several people can edit it at once: each actor is its
own lockable file, so two people moving two different pieces never block each other. The game prints
the tally on every load (`[Arena] Baked level adopted, nothing built: 559 baked pieces …`). See
[docs/GITHUB.md §4.4](docs/GITHUB.md).

`/Game/Maps/Arena` is the procedural twin: an empty level the C++ arena builder
(`ATraceArenaBuilder`) fills at `BeginPlay`. It is where arena code changes are developed, and it
is what `Scripts/bake-arena.sh` runs to produce a fresh `Arena_Baked` when the builder changes:

```bash
Scripts/bake-arena.sh                                        # re-bake (again: --force)
Scripts/run-listen-server.sh --map /Game/Maps/Arena          # play the procedural twin
```

Two things worth knowing:

- **The runtime build skips itself on a baked level.** `ATraceArenaBuilder` detects it (via its
  `bLevelIsPreBaked` flag, and independently via the presence of any `ATraceBakedPiece`) and adopts
  the placed actors instead of constructing a second arena on top of them.
- **The bake costs draw calls.** Cross-actor batching is impossible once the geometry is real
  actors — that is the price of editability, and it is why the procedural path stays.

`Scripts/bake-arena.sh --help` documents the rest, including `--nullrhi` (only safe once
`Content/Trace/Materials` exists — a first bake has to compile shader maps and needs a real RHI).

---

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
| **Left Mouse** | Fire — and while carrying the Core, **throw**: hold to charge, release to launch |
| **Right Mouse** | Melee — and Core **pull**, when the turnover pull ring is on screen |
| `1` / `2` / `3` | Equip pistol / SMG / knife (number row, not numpad) |
| `R` | Reload |
| `E` | Activated ability (character-specific) |
| `V` | Movement ability (character-specific) |
| **`Q`** or **Mouse 4** | Parry — carrier only. 0.175 s of trace invulnerability; the whole trace flashes red |
| `G` | Pull a turned-over Core (dedicated bind; right mouse also pulls when the ring shows) |
| `F` | Inspect the knife (cosmetic flourish; any real action interrupts it) |
| **Left Shift** | Dash |
| **Left Ctrl** | Slide (on the ground) / fast-fall (in the air) |
| `Tab` | Scoreboard |

There is also a **Pass/Throw** row in the options that ships **unbound** — left mouse already
throws while carrying, so a dedicated key is optional.

Input is Enhanced Input. There ARE input assets to open — `IA_*` actions and `IMC_Trace` under
`Content/Trace/Input/` — and if they are missing the controller builds the same objects in C++ at
runtime, exactly as it always did, and logs which path it took.

**But the keys do not come from `IMC_Trace`, and this is the one thing that catches people out.**
Re-keying that asset changes what you see in the editor, not what you play: the runtime mappings are
rebuilt from `UTraceUserSettings` on every settings change. **Every action above is rebindable
in-game** (Options → Controls); each player's bindings persist in
`Saved/Config/<Platform>/TraceUserSettings.ini`, and the shipped defaults live in the action table in
`Source/Trace/Settings/TraceUserSettings.cpp`. Change a default there, then re-run
`Scripts/generate-input-assets.py` so the asset stops being a stale picture — `Trace.Input.VerifyAssets`
in the console tells you, in red, when the two have drifted apart.

---

## Documentation

| Doc | Read it when |
|---|---|
| **[docs/SETUP.md](docs/SETUP.md)** | Setting up a Mac from zero. Start here on a new machine. |
| **[docs/NETWORKING.md](docs/NETWORKING.md)** | You want to playtest — locally, or across the internet. |
| **[docs/GITHUB.md](docs/GITHUB.md)** | **Before your first commit.** Unreal + Git has rules that are not optional. |
| **[docs/EDITOR.md](docs/EDITOR.md)** | New to the Unreal Editor, or wondering why the viewport is empty. Panels, PIE, and the arena preview button. |
| **[docs/DESIGN.md](docs/DESIGN.md)** | Historical design reference — carries a banner: it predates the ability system. Tuning truth is `TraceSettings.h` + `Config/DefaultGame.ini`. |
| **[docs/MIGRATION.md](docs/MIGRATION.md)** | Moving the repo / history surgery. |
| **[docs/FONTS.md](docs/FONTS.md)** | The bitmap-font text pipeline and the font licensing rules. |
| **[docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md)** | **Before you file a bug.** Every documented, deliberately-unfixed thing in the build, with the file and line it lives at. |
| **[docs/DEMO_17_18_STATUS.md](docs/DEMO_17_18_STATUS.md)** | Point-in-time verification snapshot (2026-08-12) — banner-marked; some claims have aged. |
| `docs/TraceStats.csv` | Generated by the `Trace.DumpStats` console command — not hand-maintained. |

---

## Status

It plays, it replicates, and the netcode is the part that is meant to be production-shaped.

It currently has: a **ten-character roster with abilities** and a character select screen; three
weapons (pistol, SMG, knife) with reload, melee and weapon hotkeys; the thrown-Core goals ruleset
with charge throws, catches and turnover pulls; two-half match flow with a side switch and
deferred half-time; a **practice range** (its own game mode, reachable from the title screen);
bots that play the full ruleset with selectable difficulty (Easy/Normal/Hard, default Normal); a
title menu with real menu art and a bitmap-font text renderer; a post-match result screen; a
38,400 × 9,600 uu neon arena (33,600 goal-to-goal plus two hockey-style pockets) shipped as an
editable baked level; first-person with a third-person blend while carrying; and every gameplay
number live-editable in Project Settings while Play-In-Editor is running.

All ten characters now render with their **own** body mesh and their own accent, driven by one
retargeted animation set on one shared skeleton. Be honest about what that art is: it is generated
low-poly geometry, not modelled characters. Eight of the ten are separable at the design's own
3,000 uu "who is that" distance; **Rocco and Elle are not** (see
[known limitations](docs/KNOWN_LIMITATIONS.md) item 28). The practice-range dummies are still Epic's
Mannequin, so two body styles are on screen in the same session (item 33). The arena is Tron-styled
engine primitives with authored materials. It is not balanced yet.

**What "it builds" means here.** Both build configurations compile and link, and the editor plus
`Scripts/run-listen-server.sh` is the path that is genuinely exercised — hundreds of headless runs,
a screenshot battery and a command-level acceptance suite. The **packaged game has never been run**:
the Shipping binary aborts at launch on this install layout and there is no cooked content in the
project. That is not a regression, but nobody should read "both configs green" as "the shipped game
starts". Detail and reproduction: [known limitations](docs/KNOWN_LIMITATIONS.md) item 29.

## Known limitations

**The full list is [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md)** — 44 numbered entries,
each with the file and line it lives at and whether it is open, closed or waiting on an owner
decision. Read it before filing a bug; most of what looks wrong in this build has already been
measured and decided.

The ones most likely to bite you first:

- **The surf rails are not in the map you will load** (item 30). Patch 28's surf mechanic works and is
  measured, on `/Game/Maps/Arena` only. PLAY and PRACTICE both go to `Arena_Baked`, which has none.
- **The packaged game has never been run** (item 29). "Both configs green" means the Shipping target
  links, not that it starts.
- **"ACTIVATED" clips on the character-select cards on any 16:10 display** (item 39), and reads
  correctly at 1920x1080 — which is why it survived this long.
- **There is no spawn shield** (item 14). The Core carrier's shield is the only invulnerability in
  the build. It is not missing; it never existed.
- **Fire-rate tolerance.** The server accepts shots up to 20% faster than the weapon's fire
  interval (`FireRateTolerance`, `Source/Trace/Gameplay/TraceWeaponComponent.h`), so a modified
  client could sustain ~+25% DPS. Deliberately not fixed this release: damage stays
  server-authoritative, the game is a LAN/listen-server game between known players, and a
  false-positive in the hottest server validation path would drop honest players' shots (item 1).
- **Hit-zone classification is a three-zone approximation** — a head sphere plus a body/legs height
  band, with no per-limb geometry, so extreme angles are a balance question rather than a bug.
  `Trace.HitZoneTest` runs the model's self test in a development build (item 2).
- **Corpses and the slide are procedural presentation** — posed at runtime, pending real
  animation assets (item 3).
- **Owner git/config writes, documented but not executed:** the two tracked `.slnx` files, the
  `prompt note files/` directory, and the `MultiUserClient` plugin enabled in `Trace.uproject`
  (items 4-6).
- **Sound provenance.** `Art/Sounds/` holds 71 WAVs and no `SOURCE_NOTES.md`; the provenance and
  licence note is pending with the owner. **Do not assume a licence for those files** (item 8).
- **Font licensing.** The licensed font files are not in the repo and must never be committed; the
  baked glyph atlases are committed, and whether the Sofachrome EULA permits that is an open
  owner-level verification task. Typography is frozen meanwhile — read
  [docs/FONTS.md](docs/FONTS.md) before touching it (item 9).
