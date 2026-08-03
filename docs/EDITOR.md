# Editor — why the viewport is empty, and how to work in it anyway

If you opened `Arena` in the Unreal Editor and got a blank grid, nothing is broken and you have not
missed a step. This document explains what you are looking at, what the editor is actually *for* on
this project, and the handful of panels and buttons that matter.

---

## 1. Why the viewport is empty

`Content/Maps/Arena.umap` is 6 KB and contains **no actors at all**. It is a deliberately empty
level. The same is true of `Content/Maps/MainMenu.umap`.

Everything you see when you press Play is created at runtime by C++:

| What you see | Who makes it | When |
|---|---|---|
| Floor, walls, grid, dais, rails, cover, endzone gates, lights, fog, post-process | `ATraceArenaBuilder` (`Source/Trace/World/TraceArenaBuilder.cpp`) | `BeginPlay` |
| Player starts, the two endzone triggers | `ATraceArenaBuilder` | `BeginPlay`, server only |
| The Core, the characters, the bots | `ATraceGameMode` | match start |
| Every pixel of the HUD and the title screen | `ATraceHUD` / `ATraceMenuHUD`, pure Canvas | every frame |

The exact chain for the arena is: `ATraceGameMode::PreInitializeComponents` calls
`EnsureArenaBuilt()`, which finds an existing `ATraceArenaBuilder` in the level or spawns one at the
origin, then calls `ArenaBuilder->EnsureBuilt()`. That builds roughly 800 static-mesh components
from `/Engine/BasicShapes` primitives.

So the editor viewport is showing you the truth: the level really is empty. The game only exists
while something is running `BeginPlay` — which is what "player mode" (Play In Editor, or a `-game`
launch) does.

### The tradeoff, stated honestly

This was a choice, not an accident, and it cuts both ways.

**What it buys.** The entire game is text. There is no authored `.umap` to merge, no asset to lock,
no lighting build to check in. The whole tracked repo is 94 files and about 1.8 MB, and `.git`
itself is 1.4 MB — an authored arena of this size with imported meshes and baked lighting would be
in the hundreds of megabytes to gigabytes. Every change to the arena layout shows up in a pull
request as a readable diff with a comment explaining it, and four people can work on the world at
once without coordinating.

**What it costs.** You lose WYSIWYG. You cannot drag a cover block six metres to the left and see
how it feels; changing the arena means editing C++ constants, rebuilding, playing, and looking — a
minute or two per iteration where a level designer would expect a second. §2 below claws some of
that back (a preview button, and the builder's own layout properties *are* editable live in the
Details panel), but placing individual pieces by hand is genuinely not available. That is a real
cost and it gets worse as the arena gets more detailed. If Trace ever hires someone whose job is
level layout, this constraint is the first thing to revisit.

(The no-binary-assets rule applies to things *we* author. `Content/Characters/Mannequins/` is
imported Epic content and is binary; it is tracked with Git LFS. See [GITHUB.md](GITHUB.md).)

---

## 2. Seeing the arena without playing

`ATraceArenaBuilder` has two editor-only buttons that construct the real arena directly into the
viewport, so you can look at it, fly around it and measure it without launching a match.

**The workflow, two clicks:**

1. Open `Arena`. In the **Place Actors** panel (Window → Place Actors), search for
   `TraceArenaBuilder` and drag one into the level. **Put it at the origin** (0, 0, 0) — the whole
   arena is built in this actor's local space, so the builder's transform *is* the arena's
   transform.
2. With it selected, look at the **Details** panel for the **Trace | Preview** category and press
   **Build Preview In Editor**. The full arena — floor, walls, dais, wings, rails, cover, flank
   structures, endzones, lighting rig, fog and post-process — appears immediately.

Press **Clear Preview In Editor** to take it all away again.

While a preview is up, **editing any layout property rebuilds it automatically**. So you can select
the builder, change `FieldWidth` or `WallHeight` or `EndzoneDepth` or a lighting intensity in the
Details panel, and watch the arena change in the viewport — which is the closest thing this project
has to level editing.

**It cannot leak into the map.** Everything the preview creates is marked transient, so it is never
written into the `.umap` even if you save the level with a preview showing; and the runtime build
tears down any surviving preview before it builds for real, so you can never end up with two floors
and two sets of endzone triggers. Both buttons also refuse to run outside an editor world.

One thing to know: `ATraceGameMode::EnsureArenaBuilt()` **reuses a builder that is already in the
level** rather than spawning its own. That is by design and is what makes the preview actor also
work as a play-time arena with your edited values — but it does mean a builder you leave in a saved
level becomes the one the match uses. If you only wanted to look, clear the preview and delete the
actor.

Under the hood these are `UFUNCTION(CallInEditor)` members —
`ATraceArenaBuilder::BuildPreviewInEditor()` and `ClearPreviewInEditor()`, both `WITH_EDITOR`-only —
plus a `PostEditChangeProperty` override for the live rebuild. `CallInEditor` is the general Unreal
mechanism for "a button on the Details panel"; it is worth remembering if you want one of your own.

---

## 3. Opening the two maps

There are exactly two levels.

| Level | What it is |
|---|---|
| `/Game/Maps/Arena` | The match. Empty until `ATraceArenaBuilder` runs. |
| `/Game/Maps/MainMenu` | The **title screen**. Empty; `ATraceMenuHUD` draws the whole menu in Canvas. |

Two ways to open either one:

- **Content Browser** (bottom of the window) → `Content` → `Maps` → double-click `Arena` or
  `MainMenu`.
- **File → Open Level…** (`Cmd+O` / `Ctrl+O`) → same two entries.

### Which map the game boots into, and why

From `Config/DefaultEngine.ini`:

```ini
[/Script/EngineSettings.GameMapsSettings]
EditorStartupMap=/Game/Maps/Arena
GameDefaultMap=/Game/Maps/MainMenu
ServerDefaultMap=/Game/Maps/Arena
```

- `GameDefaultMap` is why launching the *game* (packaged, or `-game` from the command line) shows
  the title screen instead of dropping you into a match already in progress.
- `EditorStartupMap` is why the *editor* opens on `Arena` — that is what you are usually here to
  work on.
- `ServerDefaultMap` stays on the arena because a dedicated server has nobody to show a menu to.

Each map finds its game mode differently, which is worth knowing because it is invisible in the
editor UI:

- `Arena` uses `GlobalDefaultGameMode` → `ATraceGameMode`.
- `MainMenu` is matched by name against `+GameModeMapPrefixes=(Name="MainMenu", …)` →
  `ATraceMenuGameMode`. The prefix table is used instead of a per-map override because a per-map
  override would have to be saved *inside* the binary `.umap`, which is exactly the hidden state
  this project builds everything in C++ to avoid.

**To see the title screen**: open `MainMenu` and press Play. You get the real menu with working
buttons, and starting a match from it travels you to the arena the same way the shipped game does.

---

## 4. The panels that matter

If you have closed one, everything below is under the **Window** menu.

**Viewport** — the 3D view. On this project it is mostly a place to press Play, plus (with the
preview above) a place to look at the arena's shape. Right-mouse-drag looks, `WASD` while
right-dragging flies, `F` frames the selected actor, `Alt+drag` orbits. The **G** key toggles "game
view", which hides editor-only icons and grid.

**Content Browser** — every asset in the project. There is very little: `Maps/` (two empty levels),
`Characters/Mannequins/` (imported Epic animation content), and `Generated/Materials/` (two
materials produced by a script — see §7). There are no Blueprints and no input assets; all of that
is C++.

**Outliner** — the list of actors in the level. On this project it is **nearly empty before you
press Play and full afterwards**, which is a good live demonstration of §1: hit Play and watch it
fill with the arena builder, the endzones, the player starts, the Core, ten characters and their
controllers. It is genuinely useful during PIE — select a running actor there and its live state
appears in the Details panel.

**Details** — properties of whatever is selected. This is where a placed `ATraceArenaBuilder`
exposes `FieldLength`, `FieldWidth`, `WallHeight`, `EndzoneDepth`, the lighting intensities and the
post-process values, and where any `CallInEditor` buttons appear.

**Output Log** — where the game talks to you. **This is the single most useful panel on this
project.** All of Trace's own logging goes to one category, `LogTraceGame`; type `LogTraceGame` into
the log's filter box to hide the engine's noise.

One catch that has fooled people twice: **a lot of the interesting lines are logged at `Verbose`,
which is filtered out by default.** "The endzone never logged, so scoring must be broken" is a
conclusion that has been reached and been wrong. Turn them on with either

- the in-editor console (`` ` `` backtick/tilde, or `^`): `Log LogTraceGame Verbose`, or
- the command line: `-LogCmds="LogTraceGame Verbose"`.

---

## 5. Play In Editor

Press **Play** in the toolbar (`Alt+P`).

### This project spawns a real network session every time

Trace is multiplayer-only — replication, prediction, lag-compensated hitscan and the trail trip test
only exist once there is a server and a client — so `Config/DefaultEditorPerProjectUserSettings.ini`
makes the Play button do that by default:

```ini
[/Script/UnrealEd.LevelEditorPlaySettings]
PlayNetMode=PIE_ListenServer     ; the editor window hosts; the others join
PlayNumberOfClients=2            ; one host + one joiner
RunUnderOneProcess=True          ; all instances in one process, one shared Output Log
```

So pressing Play gives you **two windows**, and they are real networked clients, not a split screen.
That is intentional. Teams are then filled out with bots (`bFillTeamsWithBots`), so a solo session
is still a full 5v5.

**To change the player count**: the dropdown arrow next to the Play button → **Number of Players**,
or Editor Preferences → Level Editor → Play → Multiplayer Options. Anything you change there is
written to `Saved/Config/…/EditorPerProjectUserSettings.ini`, which is gitignored and **overrides
the committed file for you locally** — handy, but remember it when your Play button stops matching
a teammate's.

### Getting the mouse back — the thing everyone gets stuck on

While PIE has focus the game captures your cursor, and the usual reflex (moving the mouse to the
menu bar) does nothing.

| Key | Effect |
|---|---|
| **`Shift+F1`** | Releases the mouse from the game window. The most important one. |
| **`Esc`** | Stops PIE entirely and returns to the editor. |
| **`F8`** | Ejects: detaches you from your pawn into a free camera, with the mouse free. Press again to re-possess. |
| Click in the viewport | Recaptures the mouse and resumes playing. |

### Running outside the editor

For screenshots or a clean measurement, launch the game standalone and off-screen so no window
appears:

```bash
"/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
  "$PWD/Trace.uproject" -game -RenderOffScreen -windowed -resx=1280 -resy=720 \
  -abslog=/tmp/trace.log
```

Add a map after the project path (e.g. `/Game/Maps/Arena?listen`) to skip the menu. The wrapper
scripts in `Scripts/` do the same thing with the engine path worked out for you, and print the raw
command before running it.

---

## 6. Project Settings → Game → Trace Gameplay

**Every tunable number in the game lives in one place.** Edit → **Project Settings…** → under the
**Game** category:

| Page | Class | Section in `Config/DefaultGame.ini` |
|---|---|---|
| **Trace Gameplay** | `UTraceSettings` (`Source/Trace/TraceSettings.h`) | `[/Script/Trace.TraceSettings]` |
| **Trace Damage Zones** | `UTraceDamageSettings` (`Source/Trace/Gameplay/TraceHitZones.h`) | `[/Script/Trace.TraceDamageSettings]` |

Both are `UDeveloperSettings` subclasses declared `config = Game, defaultconfig`, which is what puts
them on those pages and what writes your edits into `Config/DefaultGame.ini` — a committed, readable
text file, so a tuning change reviews as a one-line diff.

The full annotated list of what each value does is in [DESIGN.md §2](DESIGN.md#2-tunables--utracesettings).

### Changes apply live during Play-In-Editor

You do not have to stop PIE, change a number and start again. **Start a PIE session, leave it
running, open Project Settings, and drag a value — the running game picks it up.**

That works because of a rule the codebase follows deliberately: nothing caches a gameplay constant.
Every use site calls `UTraceSettings::Get()` (which returns the class default object the settings UI
is editing) at the moment it needs the value, rather than copying it into a member in a constructor.
The one unavoidable exception is `MaxWalkSpeed`, which the engine's movement component owns and
reads internally — so `UTraceCharacterMovementComponent` re-pushes it from the settings at the top
of every movement update (`RefreshWalkSpeedFromSettings()`). Without that, retuning `WalkSpeed`
during PIE did nothing until the map reloaded.

Two caveats:

- Some values are only read **once per event**, so you see the change on the *next* one — a
  respawn delay applies to the next death, a warmup duration to the next match.
- A few things are latched for the duration of a match on purpose. Bot difficulty is the notable
  one: `ATraceGameMode::InitGame` resolves it per map load so a mid-match config reload cannot
  change the bots out from under a player.

If you would rather edit the file than the UI, the same values are in `Config/DefaultGame.ini` under
`[/Script/Trace.TraceSettings]`. Editing the file on disk needs an editor restart; editing through
Project Settings does not.

> **One rule about config and multiplayer.** Movement values (`WalkSpeed`, `DashSpeed`,
> `DashDuration`, `DashCooldown`) feed **client-side prediction**. If a client's `DefaultGame.ini`
> disagrees with the server's, the client predicts one thing, the server computes another, and you
> get a correction on every dash that looks exactly like a netcode bug. Commit config changes like
> code and make sure everyone has pulled.

---

## 7. Common gotchas

**The editor holds the compiled game code, so builds fail while it is open.** The editor loads
`Binaries/Mac/libUnrealEditor-Trace.dylib` and keeps it locked. A build started with the editor open
either fails outright or side-steps into a numbered copy — `libUnrealEditor-Trace-0001.dylib`,
`-0002`, `-0004` are all sitting in `Binaries/Mac/` right now as evidence of exactly that. The
reliable loop is: **close the editor → `Scripts/build.sh` → reopen the editor.**

**After a C++ change, reopen the editor.** Live Coding (`Ctrl+Alt+F11`) can patch function bodies in
place and is fine for tweaking the body of an existing function. It cannot add or change a
`UPROPERTY`, `UCLASS`, `UFUNCTION` or anything else the header tool generates code for — those need a
real rebuild and a fresh editor. If a property you just added is not showing up in the Details panel
or Project Settings, this is why.

**The arena materials are generated, not committed.** `Content/Generated/` is gitignored;
`Scripts/generate_content.py` produces `M_TraceSurface` and `M_TraceNeon` into
`/Game/Generated/Materials/`. If they are missing, `ATraceArenaBuilder` falls back to
`/Engine/BasicShapes/BasicShapeMaterial` and logs a warning — the arena still plays, it just renders
flat and grey instead of neon. **A grey arena after a fresh clone means "run the content script",
not "the lighting is broken".** The two `.umap` files can be regenerated the same way with
`Scripts/generate-map.sh`.

**Where things land.** All of `Saved/` is gitignored and regenerable:

| Path | What |
|---|---|
| `Saved/Screenshots/` | Screenshots, including everything `-TraceAutoShot` captures |
| `Saved/Logs/Trace.log` | The editor/game log, unless you passed `-abslog=` |
| `Saved/Config/` | Your *local* editor preference overrides (see §5) |
| `DerivedDataCache/`, `Intermediate/`, `Binaries/` | Build and shader output. Never commit these. |

**First launch is slow.** The first editor start on a machine compiles Metal shaders — 10 to 40
minutes, and several GB into `DerivedDataCache/`. It is not hung. See [SETUP.md](SETUP.md).

**Don't save the Arena level with actors in it.** Dragging something into `Arena` and hitting save
turns an empty 6 KB text-friendly file into authored binary content, and quietly gives up the
property described in §1. If you placed a preview builder to look at the arena, remove it before
saving — or just don't save the level.

---

## Where to go next

- [DESIGN.md](DESIGN.md) — the rules, every tunable and what it does, and the class map.
- [SETUP.md](SETUP.md) — installing the engine, building, and generating the maps.
- [NETWORKING.md](NETWORKING.md) — how prediction, replication and server rewind actually work here.
- [GITHUB.md](GITHUB.md) — the Git/LFS workflow and why `Saved/`, `Binaries/` and friends are ignored.
