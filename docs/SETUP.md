# Setup — macOS / Apple Silicon, from zero

This is the exact path from a clean Mac to a running build of Trace. Follow it in order. Steps 1–4
are the ones people get wrong, and getting them wrong produces error messages that do not describe
the actual problem.

Everything here assumes **Apple Silicon** (M1/M2/M3/M4) and a **Launcher-installed** engine.

---

## Verified working configuration

This exact stack is confirmed building and running Trace. If something is broken on your machine,
diff your setup against this before debugging anything else.

| | Confirmed value |
|---|---|
| macOS | **26.5 "Tahoe"**, Apple Silicon (arm64), 16 cores, 48 GB RAM |
| Unreal Engine | **5.8.1**, Launcher install at `/Users/Shared/Epic Games/UE_5.8` (`++UE5+Release-5.8`, CL 56057345) |
| Xcode | **26.1.1** (build 17B100) at `/Applications/Xcode-26.1.1.app`, selected via `xcode-select` |
| Metal Toolchain | **17B54**, installed **separately** from Xcode — see §3 |

On that stack, measured:

- `TraceEditor` builds clean. `Trace` (standalone game) builds clean.
- `TraceServer` **does not build** — launcher engines refuse server targets. See
  [NETWORKING.md](NETWORKING.md#3-dedicated-server--requires-a-source-built-engine).
- `/Game/Maps/Arena` was generated headlessly by `Scripts/generate-map.sh`.
- A headless server run (`UnrealEditor-Cmd … -server -nullrhi`) reached `Game Engine Initialized`,
  built the arena (`Arena built (8000 x 4000 uu, visuals=no, authority=yes): 7 components`), spawned
  the Core, and exited with zero errors or warnings from our module.

---

## 0. What this costs you

Budget honestly. This is not a `npm install`.

| Step | Download | Disk after install | Wall-clock |
|---|---|---|---|
| Full Xcode | ~10–15 GB | ~20–35 GB | 20–60 min |
| Metal Toolchain (separate on Xcode 26 — see §3) | ~705 MB | ~1 GB | 2–5 min |
| Epic Games Launcher | ~200 MB | ~1 GB | 2 min |
| UE 5.8 engine (no debug symbols) | ~35–50 GB | **~60 GB** | 30–120 min |
| UE 5.8 *with* "Editor symbols for debugging" | +~40 GB | **~130–150 GB** | +30–60 min |
| Trace repo clone | < 100 MB | < 200 MB | seconds |
| First C++ compile | — | ~2–5 GB (`Intermediate/`) | 5–20 min |
| First editor launch (Metal shader compile) | — | ~2–10 GB (`DerivedDataCache/`) | 10–40 min |

**Have ~200 GB free before you start.** The Epic installer needs meaningfully more free space
during install than the final footprint, which is the cause of the classic "not enough disk space"
error on a drive that visibly has enough.

Disk-size figures for the engine are community numbers — Epic publishes no official disk
requirement for any platform. Treat them as planning estimates, not guarantees.

**Only one person on the team needs the debug symbols** (whoever will be stepping into engine
code). The other three should uncheck that option and save ~80 GB each.

---

## 1. Install the full Xcode — Command Line Tools are not enough

> This is the single most common reason "Unreal doesn't work on my Mac."

Unreal needs Xcode's **Metal shader toolchain** to compile shaders. The Command Line Tools package
(`/Library/Developer/CommandLineTools`) ships `clang`, `git`, `make` and the SDK headers, but it
does **not** ship the Metal compiler. If you only have CLT, the editor will fail — typically with
some variant of:

```
Unreal Engine requires Xcode to compile shaders for Metal
```

or a wall of shader-compile failures at first launch. You cannot work around this. Install Xcode.

### Which version

Per Epic's official [macOS Development Requirements](https://dev.epicgames.com/documentation/en-us/unreal-engine/macos-development-requirements-for-unreal-engine)
page for **UE 5.8**:

| | Minimum | Recommended |
|---|---|---|
| macOS | Sonoma 14.5 | latest macOS Sequoia 15 |
| **Xcode** | **26.0** | **26.1.1** |
| CPU | M1 or M2 (feature-dependent) | Apple Silicon M3 |
| RAM | 16 GB | 32 GB+ |
| GPU | Metal 1.2 compatible | Metal 1.2 compatible |

And a direct quote from that page, which matters:

> **"Xcode 26.4 is not compatible with Unreal Engine."**

**Target Xcode 26.1.1. Pin it.** Not "whatever the App Store gives you."

> **Measured today: the App Store and `xcodes install --latest` both currently deliver Xcode 26.6.**
> That is past 26.4 — i.e. past the version Epic explicitly calls broken, and well past anything
> Epic has validated against 5.8. Newest-is-safest does not hold here. Do not install the latest.

### Option A (recommended, and what the verified config uses) — `xcodes`, so you can pin

```bash
brew install xcodes
xcodes install 26.1.1          # asks for your Apple ID; downloads and installs to /Applications
xcodes installed               # list what you have
```

That installs to `/Applications/Xcode-26.1.1.app`. Then select it (this is the step that matters —
see §2):

```bash
sudo xcode-select -s /Applications/Xcode-26.1.1.app/Contents/Developer
```

**Multiple Xcode versions coexist safely** (`Xcode.app`, `Xcode-26.1.1.app`, `Xcode-26.6.app`, …).
**Nothing needs uninstalling.** If you already have 26.6 for other work, keep it — install 26.1.1
alongside it and point `xcode-select` at 26.1.1 while you are working on Trace. That is exactly what
version pinning is for, and it is why `xcodes` is worth the two-minute install.

### Option B — App Store

Only fine if it gives you 26.1.x, which right now it will not — it gives 26.6. Use Option A. If you
are already on an App Store Xcode, note that it will happily auto-update and silently break your
engine build; turn off automatic app updates.

### Then, always:

```bash
open -a /Applications/Xcode-26.1.1.app   # launch the pinned one once and accept the licence —
                                         # Unreal cannot do this for you
sudo xcodebuild -license accept
```

(`open -a Xcode` by name will launch whichever Xcode macOS feels like. Use the full path when you
have more than one installed.)

---

## 2. Point `xcode-select` at Xcode — verify it, do not assume it

```bash
sudo xcode-select -s /Applications/Xcode-26.1.1.app/Contents/Developer
xcode-select --print-path
```

The second command **must** print the path to your pinned 26.1.1 install:

```
/Applications/Xcode-26.1.1.app/Contents/Developer
```

(If your 26.1.1 lives at plain `/Applications/Xcode.app` — an App Store install, or one you renamed
— use that path instead. What matters is that the selected developer directory is a *full Xcode*
and is *26.1.1*, not which filename it happens to have. `xcodes installed` tells you which is which.)

If it prints `/Library/Developer/CommandLineTools`, Unreal will not find a usable toolchain — even
though Xcode is installed and sitting right there in `/Applications`. This is the most common
"can't find Xcode" cause on a machine that clearly has Xcode.

> **This happens repeatedly, not once.** Installing Homebrew, installing a Node/Python toolchain,
> running a `softwareupdate` that pulls a new CLT, or letting Xcode update itself will all
> re-point `xcode-select` at the Command Line Tools. **Re-run `xcode-select --print-path` any time
> the engine suddenly stops building.** It is a five-second check that explains an hour of
> confusion.

Sanity check that `xcrun` resolves at all:

```bash
xcrun -sdk macosx metal --version
```

If that errors saying it cannot find `metal` *at all*, you are still on CLT or on a broken Xcode
install. If it errors specifically about a **missing Metal Toolchain**, that is normal on Xcode 26 —
go to step 3, which is the fix.

---

## 3. Install the Metal Toolchain — Xcode 26 no longer ships it

> **Xcode 26 unbundled the Metal compiler.** A complete, correctly selected Xcode 26.1.1 install is
> **not enough**. Every Mac collaborator will hit this. Budget a ~705 MB download.

You do not find out at install time. You find out at first editor launch or first shader compile,
when the editor fails with:

```
Xcode Metal Compiler error: error: error: cannot execute tool 'metal' due to missing Metal
Toolchain; use: xcodebuild -downloadComponent MetalToolchain
```

(The doubled `error: error:` is verbatim from the engine, not a typo here — search for it and you
will land back on this section.)

The fix is the command in the message:

```bash
xcodebuild -downloadComponent MetalToolchain
```

~705 MB. It downloads and installs against whichever Xcode `xcode-select` currently points at, so
**do step 2 first** — otherwise you install the toolchain into an Xcode you are not using.

Verify:

```bash
xcrun -sdk macosx metal --version
```

Must print a version, e.g.:

```
Apple metal version 32023.830
```

That output is the gate. If you get it, Metal shader compilation will work. If you still get the
"missing Metal Toolchain" error, re-check `xcode-select --print-path` (step 2) and re-run the
download against the right Xcode.

---

## 4. Install the Epic Games Launcher

```bash
brew install --cask epic-games
```

That cask is current (Epic Games Launcher, macOS 13+). It installs **only the launcher** — there is
no Homebrew cask for the engine itself. The engine is downloaded through the launcher GUI.

Sign in, then go to the **Unreal Engine** tab → **Library**.

---

## 5. Install Unreal Engine 5.8 — and why we pin it

In the Library, click **+** next to ENGINE VERSIONS and select **5.8** (take the latest 5.8.x
hotfix the launcher offers).

Before clicking Install, hit **Options** and **uncheck**:

- **Editor symbols for debugging** — ~80 GB. Only the person debugging engine internals needs this.
- **Starter Content**, **Templates and Feature Packs**, and every target platform you are not
  shipping to (iOS, Android, tvOS, Linux). Trace uses zero starter content by design.

Leave **Engine Source** unchecked unless you specifically want to read engine `.cpp` in Xcode. It
is useful, but it is not needed to build, and it costs disk.

### Why 5.8, and why pinned

- 5.8 shipped **June 2026** and is the **latest stable** line, with a 5.8.1 hotfix (July 2026).
- Epic has said **5.8 is the last planned major UE5 release**, so it will absorb the remaining UE5
  hotfixes rather than being abandoned mid-line.
- **It is the only line whose official requirements name Xcode 26.** The 5.7 documentation still
  specifies Xcode 15.2/15.4 — versions that will not run on macOS 26 at all. If your Mac is on
  macOS 26 Tahoe, 5.7 is not a real option regardless of how appealing "N-1" sounds.

**Everyone on the team installs the same version. Not "5.8-ish" — the same 5.8.x build.** Mixed
engine versions across a team produce asset-version errors, silent `.uasset` upgrades on save
(which are then unopenable by everyone else), and mismatched `EngineAssociation` diffs. See
[GITHUB.md](GITHUB.md#8-the-engineassociation-gotcha). Do not let the launcher auto-upgrade you;
if a 5.8.2 appears, one person tries it first and the team moves together or not at all.

### macOS 26 "Tahoe" — read this before you blame your own setup

Epic's 5.8 requirements page **does not mention macOS 26 at all**. Tahoe is effectively
unvalidated by Epic, despite Xcode 26 being the *required* toolchain. That is an unresolved
contradiction on Epic's side, not on yours. Known issues, all from community reports rather than
Epic release notes — treat as "things to recognise", not as confirmed facts:

- **Editor crashes at ~10% initialisation on Tahoe**, reported across 5.6.1 / 5.7 / 5.8, traced to
  `FMacPlatformMemory::GetStats` (a byte count passed where the Mach API wants an `integer_t`
  count). Whether the current 5.8 hotfix fixes this is **not documented** in the release notes.
  There is a third-party `DYLD_INSERT_LIBRARIES` shim floating around GitHub for this — **do not
  deploy that across the team.** Injecting an unvetted dylib into the editor on four machines to
  work around an unconfirmed bug is a worse problem than the bug.
- **macOS 26.5 breaks the Metal shader converter.** Reported workarounds: launch the editor with
  `-BindlessOff`, or raise `MaxVersion` in the engine's `Apple_SDK.json`. Try `-BindlessOff` first,
  it is non-destructive:
  ```bash
  "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
      /path/to/Trace.uproject -BindlessOff
  ```

**Therefore: one person installs and validates the exact engine build first.** Get the editor
opening an empty project on one machine before the other three spend two hours downloading it.
Then everyone installs that same build.

---

## 6. Git and Git LFS

```bash
brew install git git-lfs
git lfs install                 # one-time, per user account — sets up the LFS filters
```

`git lfs install` must run **before** you clone. If you clone first, LFS-tracked files land as
small text pointer files and Unreal will fail to load them with cryptic errors. (Recoverable with
`git lfs pull`, but avoid the detour.)

Then clone:

```bash
git clone https://github.com/lzbrv/Game-Code.git ~/trace
cd ~/trace
Scripts/setup-lfs.sh            # installs the filters if needed, verifies, and pulls LFS content
```

`Scripts/setup-lfs.sh` is non-destructive — it never rewrites history. It prints each raw `git lfs`
command it runs, so it doubles as an explanation of what LFS is doing to your checkout. Run it with
`--verify` any time you suspect LFS is not engaged.

The clone pulls about **70 MB**, nearly all of it binary assets through LFS (~1,650 files — the
baked arena, its materials, the ten character bodies and their shared animation set, UI art, input
and character data, sounds, and the collaborator test maps).

### The first thing that will confuse you: `Content/` is read-only

Straight after cloning, every asset is checked out read-only:

```
$ stat -f '%Sp %N' Content/Maps/Arena_Baked.umap
-r--r--r-- Content/Maps/Arena_Baked.umap
```

**This is deliberate and you must not `chmod` it away.** Binary assets cannot be merged, so the team
takes a *lock* before editing one; the read-only bit is what stops two people starting the same edit
in the first place. Taking the lock makes your copy writable:

```bash
Scripts/lock.sh Content/Maps/Arena_Baked.umap   # before you open it
#   ... edit, commit, push ...
Scripts/unlock.sh Content/Maps/Arena_Baked.umap # after you have pushed
```

If Unreal ever says it cannot save an asset, this is why — you do not hold the lock.

Read **[GITHUB.md](GITHUB.md) before your first commit**, and §4 of it before you open any asset in
the editor. Unreal breaks the normal Git workflow in ways that are not obvious and are painful to
undo.

---

## 7. Generate the Xcode project files

Unreal generates `.xcodeproj` / `.xcworkspace` from `Source/*.Target.cs` and `*.Build.cs`. These
generated files are **not committed** — everyone generates their own, and you regenerate whenever
you add, rename or delete a `.h`/`.cpp`.

```bash
cd ~/trace
Scripts/build.sh --projectfiles
```

That wrapper finds your engine and prints the exact command it runs before running it, which is:

```bash
export UE_ROOT="/Users/Shared/Epic Games/UE_5.8"
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" -projectfiles \
    -project="$PWD/Trace.uproject" -game -progress
```

Note the quoting: `Epic Games` contains a space, so `$UE_ROOT` must be quoted everywhere it
appears. Unquoted, you get a baffling "no such file or directory: /Users/Shared/Epic". This is the
main reason the scripts exist.

### If the scripts cannot find your engine

They look in `/Users/Shared/Epic Games/UE_5.8` first, then any other `UE_5.x` under the usual
launcher roots. Two ways to override, both per-developer:

```bash
export UE_ROOT="/wherever/UE_5.8"          # this shell only
echo "/wherever/UE_5.8" > .ue-root         # this clone, permanently — .ue-root is gitignored
```

Never commit a path. `.ue-root` exists precisely so nobody is tempted to put a machine-specific
path into a tracked file (the same instinct that breaks `EngineAssociation` — see
[GITHUB.md](GITHUB.md#8-the-engineassociation-gotcha)).

Equivalent GUI paths, both fine:

- **From the editor:** Tools → *Refresh Xcode Project* (the menu item name follows whatever you
  picked in Editor Preferences → General → Source Code → **Source Code Editor**).
- **From Finder:** right-click `Trace.uproject` → Services → *Generate Xcode project files*.

### You do not have to use Xcode as your editor

Xcode must be *installed* (step 1) for Metal. It does not have to be where you write code. Set
Editor Preferences → Source Code Editor to **Visual Studio Code** and generate VS Code project
files instead; most people find it much less painful on Mac. Either way, keep building through
`Build.sh` or through the editor's own **Compile** button.

---

## 8. First build

```bash
Scripts/build.sh
```

which runs:

```bash
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" TraceEditor Mac Development \
    "$PWD/Trace.uproject" -waitmutex
```

The argument order is `<Target> <Platform> <Configuration>`. Our three targets:

| Target | What it is | Build it with |
|---|---|---|
| `TraceEditor` | The editor + game code | `Scripts/build.sh` — **the default, and what you want 95% of the time** |
| `Trace` | Standalone game client | `Scripts/build.sh -t Trace` |
| `TraceServer` | Dedicated server | **Cannot be built from a Launcher engine.** `Scripts/build.sh -t TraceServer` fails with `Server targets are not currently supported from this engine distribution.` It needs a source-built engine — see [NETWORKING.md](NETWORKING.md#3-dedicated-server--requires-a-source-built-engine) |

The first two are verified building clean on the stack at the top of this file. The third is an
engine-distribution limitation, not a defect in `TraceServer.Target.cs` — the target file is correct
and stays in the repo for when someone sets up a source build.

`Scripts/build.sh --clean` forces a full rebuild. Re-run `--projectfiles` whenever you add, rename
or delete a `.h`/`.cpp`, or Xcode will not see the new file.

Expect **5–20 minutes** the first time (it compiles every generated reflection stub in the module).
Incremental builds after a one-file change are seconds.

Then:

```bash
open Trace.uproject
```

**First launch compiles Metal shaders and will appear to hang.** It has not hung. Give it 10–40
minutes and watch the "Compiling Shaders" counter in the bottom-right. This happens once per
machine; the results are cached in `DerivedDataCache/`. Do not force-quit it — a half-populated DDC
means it starts over.

---

## Character art — automatic, but worth knowing about

If every player on the field is a plain coloured capsule, this section is why.

Each of the ten characters now has its **own committed body** under
`Content/Trace/Characters/<Name>/`, and all ten share one skeleton so a single retargeted animation
set (`Content/Trace/Characters/Shared/Anims/`) drives every one of them. Epic's default Mannequin
(`SKM_Manny_Simple` + `ABP_Unarmed`) is still needed anyway, for three reasons: the shared animation
set is retargeted **from** it, it is the fallback body when the character art is missing, and the
practice-range dummies are Mannequins by design.

That Epic art is **not in the repository** — `.gitignore` excludes `/Content/Characters/`
deliberately. It is ~126 MB of binary content, and it would be ~126 MB added to *every* clone and
pull, against a GitHub Git-LFS free tier of 10 GiB of storage and 10 GiB/month of bandwidth shared
across the whole account. Keeping it out is a large part of why a clone is ~70 MB rather than
~200 MB. (See [GITHUB.md §5](GITHUB.md) for the quota arithmetic — and note that the widely-quoted
"1 GiB free" figure is out of date.)

Instead it is copied out of **the Unreal install you already have** — every UE 5.8 install ships it
under `Templates/TemplateResources`. No network access is involved.

**`Scripts/build.sh` and `Scripts/build.bat` do this for you automatically** whenever the art is
missing, so a fresh clone just works. You will see it once:

```
[trace] Character art missing or incomplete — importing Epic's Mannequin
[trace] Importing 128 .uasset files (126M)...
[trace] Character art imported.
```

Subsequent builds say nothing. To run it by hand, skip it, or force a re-copy:

```bash
Scripts/import-mannequin.sh              # macOS / Linux
Scripts\import-mannequin.bat             # Windows
Scripts/import-mannequin.sh --verify     # report what is present, copy nothing
Scripts/import-mannequin.sh --force      # recopy everything
Scripts/build.sh --no-art                # build without the art check
```

**If the import fails**, it is almost always because the engine was installed without **Templates
and Feature Packs** — that is where the source art lives. Open the Epic Games Launcher → Unreal
Engine 5.8 → Options, tick it, and Apply. The script detects this case and tells you the same thing.

The build never fails over missing art: the game runs with fallback shapes and says so on screen.

---

## The one manual step: `/Game/Maps/Arena`

Trace builds its entire arena in C++ at `BeginPlay`. But Unreal still needs a **level asset** to
load into, and a `.umap` is a binary asset that cannot be generated by the C++ build.

So exactly one content file must exist: an **empty level** saved at `/Game/Maps/Arena`
(on disk: `Content/Maps/Arena.umap`).

### Automated

```bash
Scripts/generate-map.sh
```

**Read this before you run it, because the obvious guess about how it works is wrong.**

`Scripts/generate_map.py` is *editor* Python, not system Python. `python3 Scripts/generate_map.py`
fails, and always will: a `.umap` is a serialised graph of engine objects, and the only thing that
can write one is the engine itself. So the wrapper boots the editor headlessly in commandlet mode
and runs the script *inside* it:

```bash
"$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/Trace.uproject" \
    -run=pythonscript -script="$PWD/Scripts/generate_map.py" \
    -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
```

Two prerequisites follow from that:

1. **You must have built `TraceEditor` first** (step 8). The commandlet loads the project's editor
   module; there is nothing to load on a fresh clone.
2. **The Python Editor Script Plugin must be enabled.** `-run=pythonscript` *is* that plugin. If it
   is off you get:
   ```
   The 'pythonscript' commandlet could not be found
   ```
   Fix it in the editor — Edit → **Plugins** → search "Python" → tick **Python Editor Script
   Plugin** → restart — or add it to the `"Plugins"` array in `Trace.uproject`:
   ```json
   { "Name": "PythonScriptPlugin", "Enabled": true }
   ```
   Check with `grep PythonScriptPlugin Trace.uproject` — the script warns you before it launches if
   the plugin is not listed. If it is not, either enable it once, or use the by-hand route below,
   which is genuinely fine: it takes 30 seconds and you only ever do it once per repo.

Useful flags:

```bash
Scripts/generate-map.sh --force        # delete and recreate an existing Arena.umap
Scripts/generate-map.sh --nullrhi      # skip RHI init; faster, but drop it first if creation fails
Scripts/generate-map.sh --dry-run      # just print the command
Scripts/generate-map.sh -m /Game/Maps/ArenaTest
```

The script is idempotent and safe to re-run. Once the resulting `Arena.umap` is committed, nobody
else needs to do any of this — they just clone.

### By hand (if the commandlet route is fighting you)

1. Open `Trace.uproject`.
2. **File → New Level…** → choose **Empty Level**. Not "Basic" — Basic ships a floor, a light and
   a player start that will fight with `ATraceArenaBuilder`.
3. **File → Save Current Level As…** → navigate to `Content/Maps` → name it exactly `Arena`.
4. Verify the path reads `/Game/Maps/Arena` in the Content Browser.

The level stays empty. The floor, walls, endzones, pedestal, player starts, directional light and
sky light are all spawned by `ATraceArenaBuilder` at runtime. If you open the map and see nothing
but a black void, that is correct.

`Config/DefaultEngine.ini` already points the two startup maps, and it deliberately points them at
**different** levels:

- `EditorStartupMap=/Game/Maps/Arena` — the editor opens on the arena, which is what you want to be
  looking at while you work.
- `GameDefaultMap=/Game/Maps/MainMenu` — a launched (non-PIE) game boots to the title screen, and the
  menu is what starts a match.

So pressing Play drops you straight into the arena, while running the packaged game or
`-game` with no map override gives you the menu first. Both are correct; neither needs changing.

---

## 9. Run it

Press **Play** in the editor toolbar.

To immediately confirm multiplayer works, before pressing Play open the dropdown next to it:

- **Number of Players:** `2`
- **Net Mode:** `Play As Listen Server`

You get two windows, one hosting.

For two *real* processes — which is what actually catches replication bugs, because PIE shares one
process and hides them:

```bash
Scripts/run-listen-server.sh                    # host
Scripts/run-client.sh 127.0.0.1 --pos 700,0     # joiner, offset so the windows don't stack
```

Full playtesting guide — including how the four of you play across the internet — is in
**[NETWORKING.md](NETWORKING.md)**.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `cannot execute tool 'metal' due to missing Metal Toolchain` | **Xcode 26 does not bundle the Metal compiler.** Everyone hits this. | `xcodebuild -downloadComponent MetalToolchain` → step 3 |
| `Unreal Engine requires Xcode to compile shaders for Metal` | CLT selected, or Xcode missing | `xcode-select --print-path` → step 2 |
| Editor "can't find Xcode" despite Xcode being installed | Homebrew/CLT re-pointed `xcode-select` | Step 2, again |
| Xcode is installed and selected but shaders still will not compile | Metal Toolchain missing, or installed against a *different* Xcode | `xcrun -sdk macosx metal --version` → step 3 |
| Xcode updated itself and the engine broke | You are now on 26.4+ (App Store ships 26.6) | Install 26.1.1 with `xcodes` and re-select it — step 1/2 |
| `Server targets are not currently supported from this engine distribution.` | Building `TraceServer` on a Launcher engine — impossible by design | Use a listen server. [NETWORKING.md](NETWORKING.md#3-dedicated-server--requires-a-source-built-engine) |
| Build fails right after installing anything with Homebrew | Same as above | Step 2, again |
| `no such file or directory: /Users/Shared/Epic` | Unquoted `$UE_ROOT` (space in path) | Quote it: `"$UE_ROOT/..."` |
| "The engine associated with this project is not installed" / engine picker appears | `EngineAssociation` was rewritten to someone's machine-local GUID | [GITHUB.md](GITHUB.md#8-the-engineassociation-gotcha) |
| Editor crashes at ~10% on launch | Known macOS 26 Tahoe issue | See §5. Try `-BindlessOff`; escalate to the team before improvising |
| Missing module `Trace` / "would you like to rebuild?" | Editor binary is stale or absent | `Scripts/build.sh`, then reopen |
| Map `/Game/Maps/Arena` not found | The one manual step was skipped | See above |
| `The 'pythonscript' commandlet could not be found` | Python Editor Script Plugin not enabled | See the map section above — enable it, or make the level by hand |
| `Could not find an Unreal Engine 5.8 installation` from a script | Engine installed somewhere unexpected | `export UE_ROOT=...` or write `.ue-root` (step 7) |
| A script warns the engine "does not look like 5.8" | You are pointed at a different version | Fix it. Mixed versions across the team silently corrupt assets — see [GITHUB.md](GITHUB.md#everyone-runs-the-same-engine-version--non-negotiable) |
| Content files load as a few lines of text | Cloned before `git lfs install` | `git lfs install && git lfs pull` |
| Xcode project has no files / stale after adding a class | Project files not regenerated | Re-run step 7 |
| Everything is inexplicably broken after a bad merge or engine update | Stale generated state | `rm -rf Binaries Intermediate DerivedDataCache`, regenerate, rebuild |

That last one is safe by construction — all four directories are generated and are in
`.gitignore`. Deleting them costs you a rebuild, never work.

---

## Appendix: Windows collaborators

Trace is cross-platform by contract; nothing in the source is Mac-only. On Windows the equivalent
setup is:

1. **Visual Studio 2022** with the *Game development with C++* workload and the *Unreal Engine
   installer* component. (This replaces steps 1–3 — VS is the Windows toolchain requirement, and
   there is no Metal on Windows, so the Metal Toolchain step does not apply.)
2. Epic Games Launcher → same pinned **UE 5.8.x**.
3. Same `git lfs install` requirement, before cloning.

Everything after that has a real batch script — you do not hand-type engine command lines:

| Windows | macOS equivalent | What it does |
|---|---|---|
| `Scripts\setup-lfs.bat` | `setup-lfs.sh` | One-time Git LFS bootstrap and verification |
| `Scripts\build.bat --projectfiles` | `build.sh --projectfiles` | Generates `Trace.sln` (same as right-click `Trace.uproject` → *Generate Visual Studio project files*) |
| `Scripts\build.bat` | `build.sh` | Builds `TraceEditor Win64 Development` |
| `Scripts\generate-map.bat` | `generate-map.sh` | Creates the one required `/Game/Maps/Arena` level headlessly |
| `Scripts\run-listen-server.bat` | `run-listen-server.sh` | Hosts a listen server on :7777 |
| `Scripts\run-client.bat <ip>` | `run-client.sh <ip>` | Connects a client |
| `Scripts\run-dedicated-server.bat` | `run-dedicated-server.sh` | Dedicated server — read [NETWORKING.md §3](NETWORKING.md#3-dedicated-server--requires-a-source-built-engine) first, the launcher-engine limitation applies on Windows too |
| `Scripts\_trace_common.bat` | `_trace_common.sh` | Shared library the others call. Not run directly. |

The `.bat` scripts take the same flags as their `.sh` counterparts, find the engine the same way
(`UE_ROOT`, or a `.ue-root` file), and print the raw engine command before running it — so every
command line in this document and in NETWORKING.md is reachable on Windows without translating it
by hand. The `.sh` wrappers remain macOS/Linux only; run the `.bat` ones from a normal `cmd` or
PowerShell prompt at the repo root.

Target names, configurations and everything else are identical across platforms. Only the engine's
own batch-file path differs (`Engine\Build\BatchFiles\Build.bat` instead of `.../Mac/Build.sh`), and
the scripts handle that.

If you would rather drive it from the IDE: open `Trace.sln`, set configuration to **Development
Editor**, platform **Win64**, and build. Same result as `Scripts\build.bat`.
