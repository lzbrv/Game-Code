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

**What it buys.** The gameplay is text. Every change to the arena layout shows up in a pull request
as a readable diff with a comment explaining it, and an authored arena of this size with imported
meshes and baked lighting would be in the hundreds of megabytes to gigabytes rather than the ~8 MiB
a clone costs today.

**This section used to claim there was "no asset to lock". That is no longer true** — see §3.5. The
repo now tracks 641 binary assets (5.14 MiB in Git LFS): the baked arena, its materials and three
levels. Anything under `Content/` is checked out **read-only** and you must take a lock before you
can edit it. If Unreal refuses to save an asset, that is why, and the fix is a lock rather than a
`chmod` — see [GITHUB.md §4](GITHUB.md).

**What it costs.** You lose WYSIWYG. You cannot drag a cover block six metres to the left and see
how it feels; changing the arena means editing C++ constants, rebuilding, playing, and looking — a
minute or two per iteration where a level designer would expect a second. §2 below claws some of
that back (a preview button, and the builder's own layout properties *are* editable live in the
Details panel), but placing individual pieces by hand is genuinely not available. That is a real
cost and it gets worse as the arena gets more detailed. If Trace ever hires someone whose job is
level layout, this constraint is the first thing to revisit.

(`Content/Characters/Mannequins/` is imported Epic content, and unlike the rest it is **gitignored**
rather than tracked — every developer copies it out of their own engine install. See
[GITHUB.md](GITHUB.md).)

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

There is now a **third** button beside them, `Bake Arena Into Level`, and it is a different kind of
thing: the preview is transient and disappears, whereas the bake writes the arena into the level as
real placed actors you can select and move. See §3.5.

---

## 3. Opening the maps

| Level | What it is |
|---|---|
| `/Game/Maps/Arena_Baked` | The match, and **the map the game now ships and opens**. 573 real actors you can select and drag — see §3.5. |
| `/Game/Maps/Arena` | The same arena, built from code at `BeginPlay`. Empty in the editor. **Kept on purpose** as the control you compare against. |
| `/Game/Maps/MainMenu` | The **title screen**. Empty; `ATraceMenuHUD` draws the menu (UMG as of spec v17 §4, Canvas as the fallback). |

Ways to open any of them:

- **Content Browser** (bottom of the window) → `Content` → `Maps` → double-click.
- **File → Open Level…** (`Cmd+O` / `Ctrl+O`) → same entries.

### Which map the game boots into, and why

From `Config/DefaultEngine.ini`:

```ini
[/Script/EngineSettings.GameMapsSettings]
EditorStartupMap=/Game/Maps/MainMenu
GameDefaultMap=/Game/Maps/MainMenu
ServerDefaultMap=/Game/Maps/Arena_Baked
```

- `GameDefaultMap` is why launching the *game* (packaged, or `-game` from the command line) shows
  the title screen instead of dropping you into a match already in progress.
- `EditorStartupMap` is why the *editor* opens on the menu. It used to open on `Arena`; spec v17
  moved it so that pressing Play in a fresh editor gives you the same first screen a player gets.
- `ServerDefaultMap` is the dedicated-server path only — a dedicated server has nobody to show a
  menu to, so it needs an arena. It is on the **baked** one as of spec v17 §2.

**There are FOUR places that name a default arena, and this is the complete list** — you need it if
you ever want to go back to the procedural map:

| Where | Governs | Revert to |
|---|---|---|
| `Source/Trace/UI/TraceMatchOptions.h` → `TraceMaps::Arena` | The title screen's **PLAY button**. This is the one that matters day to day. | `TEXT("/Game/Maps/Arena")` |
| `Config/DefaultEngine.ini` → `ServerDefaultMap` | A dedicated server started with no map argument | `/Game/Maps/Arena` |
| `Scripts/_trace_common.sh` → `TRACE_DEFAULT_MAP` (and `_trace_common.bat`) | `Scripts/run-*.sh` when you do not pass `--map` | `/Game/Maps/Arena` |
| `Config/DefaultGame.ini` → `+MapsToCook` | What a packaged build contains. **Both** maps are listed; leave it that way. | — |

For a single run you never need to change any of them: `Scripts/run-listen-server.sh --map
/Game/Maps/Arena`.

Each map finds its game mode differently, which is worth knowing because it is invisible in the
editor UI:

- `Arena` and `Arena_Baked` use `GlobalDefaultGameMode` → `ATraceGameMode`.
- `MainMenu` is matched by name against `+GameModeMapPrefixes=(Name="MainMenu", …)` →
  `ATraceMenuGameMode`. The prefix table is used instead of a per-map override because a per-map
  override would have to be saved *inside* the binary `.umap`, which is exactly the hidden state
  this project builds everything in C++ to avoid.

**To see the title screen**: open `MainMenu` and press Play. You get the real menu with working
buttons, and starting a match from it travels you to the arena the same way the shipped game does.

---

## 3.5 `/Game/Maps/Arena_Baked` — the arena as actors you can actually move

Everything in §1 and §2 is about working around a level with nothing in it. `Arena_Baked` is the
level that does have something in it: 573 placed actors, each with a readable label
(`Wall_North_01`, `Cover_37`, `Goal_Ring_Rim_12`, `Key_Light` — note the numbering is zero-padded to
two digits, so it is `Cover_37`, not `Cover_037`), foldered in the World Outliner
under `Arena/Wall`, `Arena/Cover`, `Arena/Scoring`, `Arena/Spawns`, `Arena/Lighting`. You can click
a wall and drag it, and it stays dragged.

**To produce or re-produce it**, from a terminal:

```bash
Scripts/bake-arena.sh            # first time
Scripts/bake-arena.sh --force    # replace an existing bake
```

or, in the editor, select the builder in `Arena` and press **Bake Arena Into Level** on its Details
panel. Either way the bake runs the *same* `BuildArena()` the game runs and records what it built,
so the baked geometry is the procedural geometry — not a second implementation that can drift.

**To play it**: `Scripts/run-listen-server.sh --map /Game/Maps/Arena_Baked`, or just press Play with
the level open.

Five things to know before you rely on it:

- **`Arena_Baked` IS the default arena now, and `/Game/Maps/Arena` is kept as the control.** Spec
  v17 §2 promoted it: the PLAY button on the title screen, `Scripts/run-listen-server.sh` with no
  `--map`, and a dedicated server started with no map argument all open `Arena_Baked`. (An earlier
  version of this page said the opposite — "nothing boots into it by default" — and that sentence
  was left behind by the promotion.) The procedural map is not deprecated and is not going away: it
  is the thing you measure the baked map against when the baked map starts behaving oddly, and both
  were measured side by side producing the same field size, the same endzone and goal volumes, the
  same Core spawn point and the same ~1291 wall-fitter boxes. Three single lines choose the default,
  each documented in place: `Source/Trace/UI/TraceMatchOptions.h` (`TraceMaps::Arena` — the PLAY
  button), `Config/DefaultEngine.ini` (`ServerDefaultMap`) and `Scripts/_trace_common.sh`
  (`TRACE_DEFAULT_MAP`, mirrored in `_trace_common.bat`). To play the procedural one for a single
  run: `Scripts/run-listen-server.sh --map /Game/Maps/Arena`.
- **The runtime build skips itself here.** The builder detects a baked level (its `bLevelIsPreBaked`
  flag, and independently the presence of any `ATraceBakedPiece`) and *adopts* the placed actors —
  re-wiring the scoring volumes, the mode-tagged furniture, the half-time repaint and the lighting —
  instead of constructing a second arena on top of the saved one.
- **One File Per Actor is on**, so the level is a 6.6 KB `.umap` plus one `.uasset` per actor (573
  of them today — 572 from the v15 bake plus the `Core_Spawn` marker spec v17 added) under
  `Content/__ExternalActors__/Maps/Arena_Baked/`. **This is the thing that lets two
  people edit the arena at the same time** — you lock the *actor*, not the map, so one person moving
  `Cover_37` does not block anyone else from touching the other 572. Lock the `.umap` itself only
  for level-wide changes (World Settings, the level Blueprint, a lighting build).

  In practice: **right-click the actor in the World Outliner → Revision Control → Check Out**, which
  takes the lock for you and is the only route a designer needs. From a terminal, the label works
  too — `Scripts/lock.sh Cover_37` finds the GUID-named package behind it. Release it with
  `Scripts/unlock.sh Cover_37` once you have pushed. Full workflow in [GITHUB.md §4](GITHUB.md).

  Assets are checked out **read-only** until you hold the lock, so "Unreal will not let me save" is
  the expected first symptom of forgetting to take one.
- **It costs draw calls.** Cross-actor batching is impossible once geometry is real actors: the
  arena goes from ~411 primitives to ~1048. That is the price of editability, and it is why the
  procedural path stays.
- **YOU CANNOT CLICK ONE TILE INSIDE A PIECE — 659 of the 835 blocks are batched.** This is the one
  limit of the bake that surprises people, and until spec v17 it was written down nowhere. A "piece"
  (a wall, a cover block, a bank terrace) is one `ATraceBakedPiece` actor, and inside it the repeated
  blocks live in an *Instanced Static Mesh* component rather than as separate components. You can
  always select, move, rotate, hide or delete a **whole piece** in the viewport. You cannot
  rubber-band-select tile 12 of a terrace. **Every piece now tells you its own numbers**: select it
  and read the read-only **Editing Note** box in the Details panel, which reports how many selectable
  meshes and how many batched blocks *that* piece has, and the three ways round it —
  (i) change the Instanced Static Mesh component's mesh or material to change all of them at once,
  (ii) expand that component's `Instances` array in the Details panel and edit one transform there,
  or (iii) re-bake with different settings and accept the extra draw calls. The note is editor-only
  and transient: it is recomputed from the real components every time the actor registers, so it
  cannot go stale and it is not saved into the `.uasset`.

**Your endzone edits DO survive now, and that changed in spec v17.** Older versions of this page
said a hand-resized endzone was "re-derived from the builder's layout at load", and that was true:
`AdoptBakedArena` used to overwrite the placed shape silently. It now *verifies* instead — if the
placed volume differs from what the builder would have made, **the placed shape is kept** and the
log prints a warning naming both numbers. Read that warning the other way round too: if you see it
and you did not edit anything, the map has gone stale against a code or settings change, and
`Scripts/bake-arena.sh --force` is the fix.

One edit still will not survive a reload, and it is known: the floor lamps' intensity and radius are
re-applied from the builder's properties whenever video settings change. Move those by changing the
builder's properties and re-baking, not by hand.

---

## 4. The panels that matter

If you have closed one, everything below is under the **Window** menu.

**Viewport** — the 3D view. On this project it is mostly a place to press Play, plus (with the
preview above) a place to look at the arena's shape. Right-mouse-drag looks, `WASD` while
right-dragging flies, `F` frames the selected actor, `Alt+drag` orbits. The **G** key toggles "game
view", which hides editor-only icons and grid.

**Content Browser** — every asset in the project. Spec v17 changed this list substantially, so here
is the whole of it:

| Folder | What is in it | Generated? |
|---|---|---|
| `Maps/` | Three levels. `MainMenu` and `Arena` are empty and build themselves at runtime; **`Arena_Baked` is not** — it is 573 real actors, see below, and it is now the map the PLAY button opens | `Arena_Baked` was baked once |
| `Trace/Materials/Parents/` | `M_TraceSurface`, `M_TraceNeon` — the two hand-authored shader graphs. Editing one recompiles shaders and changes all 64 instances at once | `Scripts/generate_content.py` |
| `Trace/Materials/Instances/` | 64 `MI_*` assets, one per colour the arena uses. **These are the ones you want.** Tick a checkbox, change a number, save — nothing recompiles | from the bake |
| `Trace/Input/` | 14 `IA_*` input actions and `IMC_Trace` | `Scripts/generate-input-assets.py` |
| `Trace/Data/Characters/` | 5 `DA_Character_*` definitions — name, accent colour, the three ability card texts | `Scripts/generate-data-assets.py` |
| `Trace/UI/HUD/` | `WBP_TraceHudCorner`, `WBP_TraceHudStatusChip` — the ammo plate and status chips in the bottom-right | `Scripts/generate-hud-widgets.py` |
| `Trace/UI/Menu/` | `WBP_TitleMenu`, `WBP_MenuRow` — the title screen | `Scripts/generate-menu-widgets.py` |
| `Characters/Mannequins/` | Imported Epic animation content | `Scripts/import-mannequin.sh`, **not committed** |
| `Generated/Materials/` | The old script output. Kept only as the second fallback arm for materials — see §7 | **not committed** |

**Two rules that cover almost everything in that table.** (1) Anything in the "Generated?" column
naming a `Scripts/generate-*.py` is **rewritten from scratch every time that script runs** — a hand
edit in the editor is fine for trying something out, but treat it as temporary; to make a change
stick, change the script (or the C++ table it reads) and regenerate. (2) Every one of these files is
a binary `.uasset`, so Git cannot merge two people's edits — **lock it before you open it**
(`Scripts/lock.sh`), and see [GITHUB.md](GITHUB.md).

There are still no Blueprint *classes* with gameplay logic. The four `WBP_` assets are Widget
Blueprints, and they deliberately carry only the widget tree and its styling: every behaviour lives
in a C++ `UUserWidget` subclass with `BindWidget` properties, under the same harnesses as before. If
you rename or delete one of the bound widgets, the game notices, says so at Error level with the
missing name, and falls back to the old Canvas drawing rather than showing you a HUD with a hole in
it.

**Outliner** — the list of actors in the level. On `MainMenu` and `Arena` it is **nearly empty
before you press Play and full afterwards**, which is a good live demonstration of §1: hit Play and
watch it
fill with the arena builder, the endzones, the player starts, the Core, ten characters and their
controllers. It is genuinely useful during PIE — select a running actor there and its live state
appears in the Details panel.

`Arena_Baked` is the opposite and that is the whole point of it: **573 actors are already there
before you press Play**, foldered under `Arena/Wall`, `Arena/Cover`, `Arena/Scoring`, `Arena/Spawns`
and `Arena/Lighting`, each with a readable name (`Wall_North_01`, `Cover_37`, `Core_Spawn`). Click
one and move it — after taking its lock (§3.5). The builder is still in that level but it adopts the
placed geometry instead of rebuilding it. Note `Arena/Spawns/Core_Spawn`: dragging that one marker
moves where the Core starts at kickoff, where it resets to after a score, and where the game treats
as its home. Delete it and the game quietly computes the same point itself and says so in the log.

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

The **Trace Gameplay** page is organised into these categories: **Match**, **Combat**,
**Movement | Walk / Air / Landing / Dash / Slide**, **Core** and **Core | Pass**, **Parry**,
**HUD**, **Trail**, **Bots** (with `Difficulty`, `Intercept`, `Positioning`, `Targeting`, `Passing`,
`HoverPass`, `Punish`, `Aim` and `Movement` sub-groups) and **Net**.

Three knobs landed at integration, each replacing a hardcoded constant that this project's rules
forbid: **Parry** now owns `ParryDuration`, `ParryCooldown`, `ParryTintColor` and `ParryGlowScale`
(`Gameplay/TraceParry.cpp` reads them and nothing else may); **HUD** owns
`ThirdPersonCrosshairScale`; and **Bots | Movement** owns `BotStuckJumpSeconds`, the stuck-recovery
timer that inherited the deleted boost's one useful job.

**Everything on this page is read by something.** That is a rule, not a description, and it is
enforced by grep at integration in both directions — every `config` property has a reader, and every
reader names a property that exists. Eighteen knobs were deleted this pass precisely because they
were not:

- the whole `Movement | Boost` category, plus its two bot mirrors — boost no longer exists;
- five `Core` values describing a thrown, catchable Core that the hover pass replaced
  (`PassSpeed`, `PassUpwardBias`, `PickupRadius`, `PickupLockoutAfterThrow`, `CoreResetTime`);
- the seven-scalar `Bots | Legacy` block, superseded by the difficulty profiles;
- `BotPassSafeRadius`, which duplicated `BotPunishRange`;
- `MatchDuration`, `SpreadDegrees` and `LookSensitivity`.

A slider that silently does nothing is worse than no slider. If you find one, delete it or wire it —
do not fold it under `AdvancedDisplay` and hope. The check is one grep: every `config` property in
`TraceSettings.h` should appear as a `Settings.<Name>` or `Get().<Name>` read somewhere in
`Source/`.

If you would rather read the values back than trust the panel, `Trace.DumpSettings` in the console
logs what `UTraceSettings::Get()` is returning **right now** — movement, air and landing, slide,
pass, parry, trail — plus every live pawn's engine-owned `MaxWalkSpeed`, which is the one family of
values that is a copy and could therefore disagree with the table. `Trace.LiveEditTest <delay>
<PropertyName> <value>` drives the exact code path the details panel drives, after a delay, so an
unattended run can prove that live editing works mid-match.

### Changes apply live during Play-In-Editor

You do not have to stop PIE, change a number and start again. **Start a PIE session, leave it
running, open Project Settings, and drag a value — the running game picks it up.**

That works because of a rule the codebase follows deliberately: nothing caches a gameplay constant.
Every use site calls `UTraceSettings::Get()` (which returns the class default object the settings UI
is editing) at the moment it needs the value, rather than copying it into a member in a constructor.
The unavoidable exceptions are the handful of values the **engine** owns a copy of and reads
internally — `MaxWalkSpeed`, `MaxWalkSpeedCrouched` and `AirControl` — so
`UTraceCharacterMovementComponent` re-pushes them from the settings on every simulated move
(`RefreshEngineTunablesFromSettings()`). Without that, retuning `WalkSpeed` during PIE did nothing
until the map reloaded. `UTraceSettings::ApplyLiveMovementTuning()` does the same job from the other
end, on every property edit, which additionally reaches pawns whose movement is not currently
simulating — precisely the state a paused-PIE tuning session leaves them in.

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
