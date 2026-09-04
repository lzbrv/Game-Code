# Networking — how to playtest, and what the prototype actually does

Three parts now. **§0 is the one you want**: playing together from inside the game, with no scripts
and no terminal. After that, the operational detail (scripts, dedicated servers, Tailscale), and
then the explanatory half — what Trace's netcode does and why, in plain English.

---

## 0. Playing together — from the title screen

**This is the normal path. Nobody needs a script or a terminal.**

### Host

1. Launch Trace. You land on the title screen.
2. Under the tagline there is a chip that reads **`YOUR ADDRESS   100.101.102.103:7777`**. That is
   the address other people type. It is also printed on the **PLAY** row and repeated inside the
   JOIN prompt, so you never have to go looking for it.
3. Press **PLAY**.

That is the whole host flow. **PLAY always starts a listen server** — every match is joinable, and
there is no separate "host" button to forget. A listen server with nobody connected ticks exactly
like a standalone game, so this costs a solo player nothing at all.

While the match runs, the top-right of the HUD reads **`HOSTING  100.101.102.103:7777`** with a
count of the humans in the match underneath. You can read your own address off your own screen
mid-game when somebody asks for it.

### Everyone else

1. Launch Trace.
2. Select **JOIN** and press Enter. A prompt opens with a text field.
3. Type the host's address. **Ctrl+V / Cmd+V pastes.** Backspace, Delete and the arrow keys work;
   there is a visible caret. If you leave the port off, `:7777` is added for you.
4. Press **Enter**.

The address is **remembered between sessions** (in `Saved/Config/<Platform>/GameUserSettings.ini`,
which is not in source control), so the next test is JOIN → Enter → Enter. It is remembered on the
*attempt*, not on success — a join that times out because a firewall was shut is exactly the one you
will want to retry unchanged.

### When it does not work, it says so

A failed connection used to be completely silent, which is why the Demo 5 report could not tell a
broken VPN from a broken game. Now:

- The travel screen names what it is doing — `CONNECTING TO 100.101.102.103:7777`.
- On failure the game returns to the title screen with an amber banner across the top giving the
  reason in plain English (`CONNECTION TIMED OUT. CHECK THE ADDRESS, THE VPN, AND UDP 7777 ON THE
  HOST.`) plus the engine's own failure code underneath for the bug report.
- Mid-match disconnects raise the same banner over the HUD.
- If UDP 7777 is already held by another process, you are warned on the title screen *and* before
  travelling. **The port moves — hosting does not fail.** Measured: with a second copy of the game
  already on 7777, `UIpNetDriver` binds the next free port instead and logs
  `IpNetDriver listening on port 7778`. The match is perfectly joinable, just not at `:7777`, so the
  in-game chip reads the port the driver *actually* bound (`UNetDriver::LocalAddr`) rather than the
  one that was asked for. **Read the address off the HUD, not off the title screen, if you saw that
  warning.**
- If a net driver never comes up at all, the HUD reads `OFFLINE - NOBODY CAN JOIN` rather than
  pretending to host. (The engine does not fail loudly on its own here: `UEngine::LoadMap` logs one
  `LogNet` error and loads the map anyway, standalone. That silent degrade is the trap this replaces.)

### Firewall — the one thing outside the game

**The host must allow inbound UDP 7777.**

- **macOS**: System Settings → Network → Firewall → Options → allow incoming connections for
  `UnrealEditor` (and `UnrealEditor-Cmd`, and `Trace` if you run a packaged build). macOS normally
  prompts the first time; **if anyone ever clicked "Deny" it stays denied, silently, forever.** This
  has cost people whole evenings.
- **Windows**: allow the same binaries on Private networks in Windows Defender Firewall.
- **Over Tailscale** this is usually all you need — no port forwarding, no router config. See §4.
- Symptom of a blocked port: the client sits on `CONNECTING TO …` and then reports
  `CONNECTION TIMED OUT`. The host's log shows nothing at all, because nothing ever arrived.

### Checking it worked

In the console (**`~`**) on either machine:

```
TraceNetInfo
```

prints the net mode, this machine's endpoint, every local adapter address, and the full PlayerState
roster with bots marked. Two non-bot entries means two humans are in one match. The title screen
also logs the host endpoint and whether UDP 7777 is free on every launch, at `Display`, so the answer
is already in any log somebody sends you.

### What the bug actually was

For the record, because it will save the next person the same week: `ATraceMenuHUD::StartMatch()`
travelled to the arena with `?difficulty=…?mode=…` and **no `listen` option**. Without that one word
`UEngine::LoadMap` never calls `UWorld::Listen`, no net driver is created and nothing binds UDP 7777.
Both machines pressed PLAY, both got a private standalone match, and no connection was ever possible
— the VPN was almost certainly fine. There was also no Host or Join UI anywhere in the project.
`Scripts/run-listen-server.sh` *did* append `?listen`, which is exactly why the documented path
looked correct while the in-game path silently was not.

### Three more bugs that were hiding behind it

The missing `listen` meant no client had ever actually reached a match, so nothing downstream of the
join had ever run. All three of these turned up in the first two-process test and are fixed:

1. **The client had no arena and fell through the world.** `ATraceArenaBuilder` is spawned server-side
   and clients build their geometry from its `BeginPlay` — so the actor has to *replicate* first. Its
   `NetCullDistanceSquared` was the 15000 uu default and the player starts are at X = ±15600, so it
   was irrelevant from frame one and never arrived. The client's log had no `Arena built` line at all
   and its pawn logged `Z=-19636`, falling. Fixed with `bAlwaysRelevant = true` in the constructor.
2. **The client saw only 5 of the 10 players.** Same root cause on `ATraceCharacter`: a 15000 uu cull
   radius on a 33600 × 9600 field means half the roster is never relevant. Now
   `SetNetCullDistanceSquared(40000² )`, which covers the diagonal. Reading an enemy's trail from
   across the arena is the whole game, so there is no distance at which a player stops mattering.
3. **Every ground correction was being thrown away.** A real client logged this **2948 times in 30
   seconds**:

   ```
   LogNetPlayerMovement: Warning: ClientAdjustPosition_Implementation could not resolve the new
   relative movement base actor, ignoring server correction! ... on base None
   ```

   `MovementBaseUtility::IsDynamicBase()` is *exactly* `Mobility == Movable`, and the arena builder
   creates all 1187 pieces of geometry Movable — so the engine classified the **floor** as a moving
   platform, sent corrections as base-relative, and named a primitive component that is built locally
   on each machine, is not a replicated subobject, has no NetGUID and can never resolve on a client.
   A client that cannot be corrected while standing on the floor drifts from the server without
   limit, which is the network half of *"jumping on the edge of a raised section feels like rubber
   banding"*. `ATraceCharacter::SetBase` now refuses the arena as a movement base — the arena does
   not move, so it must not be one. **Measured after: 0 of these warnings, and 1 correction total
   (at spawn) where there had been 24.**

If you are chasing a movement desync, `-TraceLedgeTest -TraceMoveCorrections` on a **client** against
a listen server is the run that shows it; on a listen host corrections are structurally zero and
every number reads clean.

---

## Unreal's network model in 90 seconds

If you are new to Unreal specifically, this is the mental model everything below depends on.

- **One machine is the server. It is right about everything.** Clients are, formally, guesses.
- Every actor has a **role** on each machine: `ROLE_Authority` (the server's copy — the real one),
  `ROLE_AutonomousProxy` (your own pawn on your own client — the one you predict), and
  `ROLE_SimulatedProxy` (everyone else's pawn on your screen — interpolated, always slightly in
  the past).
- **Replication** is state flowing server → client automatically: mark a `UPROPERTY` as
  `Replicated`, and the server pushes changes down. It is *eventually consistent*, not a message
  queue — clients see the latest value, not every intermediate one.
- **RPCs** are explicit calls: `Server` (client → server, "please do this"), `Client`
  (server → one client), `NetMulticast` (server → everyone). `Reliable` RPCs are guaranteed and
  ordered; `Unreliable` ones may be dropped. Cosmetics go unreliable; state changes go reliable.
- **Three ways to run:**
  - **Listen server** — one player's game is also the server. Zero setup, but that player has
    literally zero latency and a real competitive advantage. Fine for dev, and **it is what this
    team uses today** (see §3 for why).
  - **Dedicated server** — a headless build with no rendering. Everyone is a client, everyone has
    honest latency. This is how you evaluate whether it *actually* feels good. **Building one
    requires a source-built engine — a launcher engine cannot. See §3.**
  - **Standalone** — no networking at all.
- **Default port: UDP 7777.** Unreal is UDP. Do not go looking for a TCP port to forward.

---

## 1. Play-In-Editor (the 10-second loop)

The dropdown arrow next to the **Play** button → **Advanced Settings** / the inline options:

- **Number of Players:** `2` to `10` (Trace is 5v5, so `10` is a full match).
- **Net Mode:** **Play As Listen Server**.
- **Run Under One Process:** on (default). Faster iteration; all clients share one process.

Press Play. You get one window per player; window 1 is the host.

Untick **Run Under One Process** when you want each client in its own OS process — slower to
launch, but it catches bugs where two "clients" were accidentally sharing state, and it is closer
to reality.

### Simulate latency in PIE

Editor Preferences → **Level Editor → Play → Multiplayer Options → Network Emulation**. Enable it
and pick a preset (`Average`, `Bad`) or set custom values. **Turn this on for most of your
testing.** A prediction bug is invisible at 0 ms and obvious at 120 ms. Trace's dash, hitscan and
trail all involve prediction or rewind; testing them at LAN latency tells you nothing.

Rough targets: `Average` ≈ 60–90 ms round-trip (a good regional match), `Bad` ≈ 150–250 ms with
loss (someone on hotel wifi). Trace should be playable at both. If dash feels rubber-bandy at 90 ms,
that is a bug in the saved-move implementation, not "just latency."

### What PIE will *not* catch

- Anything that depends on the client and server being different processes with different memory
  (a stray pointer to a server-only object will "work" in PIE).
- Real packet loss and reordering patterns.
- `IsLocallyControlled()` / `HasAuthority()` mistakes that happen to be true in a single process.

Which is why the next two sections exist.

---

## 2. A real listen server on your LAN

**This is the default path for this team.** A dedicated server needs a source-built engine, which
nobody has yet (§3) — so two real processes with one of them hosting is how you catch replication
bugs today.

> **You do not need any of this to play together — see §0.** The scripts below exist for automation
> and for launching straight into the arena without passing through the menu. They and the in-game
> flow produce the same thing: a listen server on UDP 7777.

Host, from the repo root:

```bash
Scripts/run-listen-server.sh
```

which prints and runs:

```bash
export UE_ROOT="/Users/Shared/Epic Games/UE_5.8"
UE_EDITOR="$UE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"

"$UE_EDITOR" "$PWD/Trace.uproject" "/Game/Maps/Arena?listen" \
    -game -log -windowed -ResX=1280 -ResY=720 -port=7777
```

- `?listen` is the URL option that makes the map load as a listen server. That is the whole
  mechanism — Unreal's map travel takes URL-style options.
- `-game` runs the game rather than the editor.
- `-log` opens a console window. **Always pass `-log`.** Without it you are debugging blind.

Find your LAN address:

```bash
ipconfig getifaddr en0        # wifi;  try en1 if that's empty
```

Clients:

```bash
Scripts/run-client.sh 192.168.1.42
Scripts/run-client.sh 192.168.1.42:7777 --pos 700,0     # second window on the same Mac
```

which is just:

```bash
"$UE_EDITOR" "$PWD/Trace.uproject" 192.168.1.42:7777 -game -log -windowed
```

The bare `IP:PORT` where a map path would normally go **is** the travel URL — that is the whole
mechanism by which an Unreal client joins a server.

Or launch with no address and connect from the console — press **`~`** (tilde) in-game and type:

```
open 192.168.1.42:7777
```

`open <address>` is the single most useful console command in Unreal networking. `open <map>` with
no address restarts you into a local game — useful for bailing out.

You can also launch a listen server straight from the editor: Play dropdown → **Standalone Game**
with Net Mode *Play As Listen Server*, then have others `open` your address.

---

## 3. Dedicated server — requires a source-built engine

The honest test: nobody gets a free 0 ms advantage. It is also the one thing on this page you
**cannot** do with the engine this team currently has installed.

> **A Launcher-installed engine cannot build server targets. At all.** Not a misconfiguration, not
> something a flag fixes. Until someone on the team sets up a *source* build of Unreal, the
> practical path is the **listen server** in §2 — that is the documented default for this team right
> now, and everything else in this document works with it.

### What happens if you try anyway

`Scripts/run-dedicated-server.sh` builds the `TraceServer` target before launching it. On a launcher
engine that build fails immediately. Measured on this project:

```
$ Build.sh TraceServer Mac Development -project=.../Trace.uproject
Server targets are not currently supported from this engine distribution.
Result: Failed (OtherCompilationError)
```

That is a restriction of the binary engine distribution Epic ships through the Launcher. **It is
not a bug in `TraceServer.Target.cs`** — that target file is correct, it is kept in the repo
deliberately, and it is what a source build will use unchanged. Do not "fix" it.

### The closest thing that works today: the editor binary in server mode

`--editor` skips the `TraceServer` build entirely and runs the editor binary with `-server`. This
needs no server target, and therefore works on a launcher engine:

```bash
Scripts/run-dedicated-server.sh --editor
# =>  "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" "$PWD/Trace.uproject" \
#         /Game/Maps/Arena -server -log -port=7777
```

Verified on the stack in [SETUP.md](SETUP.md#verified-working-configuration): with `-nullrhi` added,
this reaches `Game Engine Initialized`, builds the arena headlessly
(`Arena built (8000 x 4000 uu, visuals=no, authority=yes): 7 components` — note `visuals=no`, i.e.
the code below is doing its job) and spawns the Core, with no errors or warnings from our module.

It is still the editor binary, not a cooked server build, so it is heavier and slower to boot than a
real `TraceServer` would be. But the net mode is genuinely `NM_DedicatedServer`, so it exercises the
code paths a listen server hides.

Other flags: `--no-build` (run whatever binary is already there), `--port`, `-c Shipping`. Note that
`--no-build` will not rescue the default path — there is no `TraceServer` binary to run, because it
never built.

Clients connect exactly as above: `Scripts/run-client.sh <ip>`, or `open <ip>:7777` in the console.

### What a real dedicated server would take

Keeping this here because it is a real, finite piece of work someone can pick up:

1. Link your Epic account to your GitHub account (Epic account settings → Connections → Accounts),
   which grants access to the **private** `EpicGames/UnrealEngine` repository. Without that link the
   repo 404s — it is not public.
2. Clone the `5.8` branch, run `Setup.sh` then `GenerateProjectFiles.sh`, and build the engine. This
   is a long compile and a large amount of disk, on top of the launcher engine you already have.
3. Point Trace at that engine (`export UE_ROOT=...` or a `.ue-root` file — see
   [SETUP.md](SETUP.md#if-the-scripts-cannot-find-your-engine)), then
   `Scripts/run-dedicated-server.sh` works as originally written:
   ```bash
   "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" TraceServer Mac Development \
       "$PWD/Trace.uproject" -waitmutex

   "$PWD/Binaries/Mac/TraceServer" "$PWD/Trace.uproject" /Game/Maps/Arena -log -port=7777
   ```

Nobody needs to do this to work on the game. It matters when you want honest latency numbers for
everyone, including the host.

Things that behave differently on a dedicated server (and under `-server`), and that Trace's code
has to respect:

- **There is no local player.** `GetLocalPlayer()` is null, HUD never draws, no camera exists.
  Any code path that assumes a viewport must be guarded.
- **Materials and shaders are not cooked for server targets.** Visual setup must be behind
  `if (GetNetMode() != NM_DedicatedServer)`. Trace does this — the arena builder, trail meshes and
  tracers all skip their visual work on a dedicated server.
- Nothing renders, so it is cheap: a Trace server is a few percent of one core.

---

## 4. Playing remotely — the four of us, different houses

### Recommended: Tailscale

For a four-person dev team this is the right answer, and it is not close. Tailscale is a WireGuard
mesh VPN: every machine gets a stable `100.x.y.z` address and can reach every other machine
directly, as if they were on the same LAN. **No port forwarding, no router config, no dynamic-DNS,
no NAT debugging, works from a coffee shop.**

The free *Personal* plan covers **6 users with unlimited personal devices** (it was raised from
3 users / 100 devices in April 2026) — comfortably more than four people need.

**Setup, once per person:**

```bash
brew install --cask tailscale-app
```
(The Homebrew cask was renamed from `tailscale` to `tailscale-app`. There is also a Mac App Store
build and a standalone download at tailscale.com/download — any of them is fine, they join the same
network.)

1. One person creates the tailnet by signing in (GitHub/Google/Microsoft SSO).
2. That person invites the other three from the Tailscale admin console
   (`login.tailscale.com` → **Users → Invite**). They accept and sign in to the *same* tailnet.
3. Everyone runs `tailscale status` and sees four machines with `100.x` addresses.

**Then play — the normal way (no terminal):**

Host launches Trace and presses **PLAY**. The title screen already shows their tailnet address in the
`YOUR ADDRESS` chip — `tailscale ip -4` and the chip will agree, because the address ranking prefers
the `100.64.0.0/10` CGNAT block Tailscale hands out over anything else. Everyone else picks **JOIN**,
pastes that address, presses Enter. See §0.

**Or from the terminal, if you are scripting it:**

```bash
# Host — find your tailnet address, then start a server exactly as in §2:
tailscale ip -4                        # e.g. 100.101.102.103
Scripts/run-listen-server.sh           # or run-dedicated-server.sh --editor (see §3)

# Everyone else:
Scripts/run-client.sh 100.101.102.103
#   ...or launch however you like and type in the in-game console:
#   open 100.101.102.103:7777
```

That is genuinely all of it. No router login.

**Firewall, even on Tailscale.** Tailscale removes the *router* problem, not the *host machine*
problem: the host's own OS firewall still has to allow the game binary to accept inbound
connections on UDP 7777. On macOS that is System Settings → Network → Firewall → Options. If it was
ever denied it stays denied silently — see §0.

Two caveats worth knowing:
- Tailscale prefers a **direct** peer-to-peer path but will fall back to a relay (DERP) if both
  ends are behind hostile NATs. Relayed connections add latency. Check with
  `tailscale ping <peer>` — it reports `direct` or `via DERP`. If you are relayed and it feels bad,
  enabling UPnP/NAT-PMP on the host's router usually fixes it.
- It is a dev-time tool. You will not ship a game by asking players to install a VPN.

### Alternative: port forwarding UDP 7777

If you refuse to install anything, the host can forward the port. This works, it is just more
fragile.

1. **Give the host machine a static LAN IP** or a DHCP reservation, so the forward doesn't break
   when the router hands out a different address next week.
2. In the router admin page, forward **UDP** port **7777** → host's LAN IP, port 7777.
   Not TCP. Unreal is UDP.
3. Allow the binary through the macOS firewall: System Settings → Network → Firewall → Options →
   add `UnrealEditor` / `UnrealEditor-Cmd` (and `TraceServer`, if you ever have one) and allow
   incoming connections. (macOS will normally prompt
   the first time; if you clicked "Deny" once, it silently stays denied — this wastes a lot of
   people's evenings.)
4. Find the host's **public** IP: `curl -4 ifconfig.me`.
5. Everyone else: `open <public-ip>:7777`.

Where this goes wrong:
- **CGNAT.** If your ISP puts you behind carrier-grade NAT (very common on mobile broadband and
  increasingly on fibre), port forwarding cannot work at all — you do not own a public address.
  Symptom: the forward is configured correctly and nothing connects. Test: compare
  `curl -4 ifconfig.me` against the WAN address shown in your router. If they differ, you are
  behind CGNAT. Use Tailscale.
- **Residential IPs change.** The host's address will silently change and everyone's saved address
  breaks.
- You are exposing a port on your home network to the internet. A dev-build game server is not a
  hardened service.

### Eventually: Steam or Epic Online Services

Neither of the above is how you ship. When Trace needs real matchmaking, the path is Unreal's
**Online Subsystem** layer:

- **Steam OSS** (`OnlineSubsystemSteam`) — a built-in engine plugin. Gives you lobbies, a server
  browser, friend invites, and crucially **Steam's relay/NAT punch-through**, so players never
  configure anything. Requires a Steam appid (a $100 Steamworks fee); works with appid `480`
  (Spacewar) for development. This is the low-effort path for a PC game.
- **Epic Online Services** (`OnlineSubsystemEOS` / EOS SDK) — free, cross-platform, engine-native,
  gives you identity, lobbies, sessions, P2P and relays without a Steam dependency. More setup;
  no store lock-in.

Both slot in behind the same `IOnlineSubsystem` interfaces, so the gameplay code above does not
change — you replace "type an IP" with "join a session." Do not build this now. It is a distraction
until the game is worth joining.

---

## 4.5 Cross-platform: the network compatibility value

**Mac and Windows players can only connect if their builds compute the same network version.** This
section is about making that a fact rather than a hope, because the failure mode is expensive: the
engine refuses the connection during the handshake and the player is told the *connection* failed.

### What Unreal does by default, and why it is not enough here

`FNetworkVersion::GetLocalNetworkVersion` (UE 5.8,
`Engine/Source/Runtime/Core/Private/Misc/NetworkVersion.cpp:221`) CRC32s this string:

```
"<ProjectName> <ProjectVersion>, NetCL: <n>, EngineNetworkVersion: <e>, GameNetworkVersion: <g>"
```

Three of those five terms **come from the engine install rather than from this repository**:

* `NetCL` — `ENGINE_NET_VERSION` is `0` in 5.8 (`NetworkVersion.h:12`), so it falls through to
  `BuildSettings::GetCompatibleChangelist()`, which is the `CompatibleChangelist` field of
  `<engine>/Engine/Build/Build.version`. On this machine's UE 5.8.1 that is **55116800**.
* `EngineNetworkVersion` — `FEngineNetworkCustomVersion::LatestVersion`, an engine header constant.
* `GameNetworkVersion` — `FGameNetworkCustomVersion::LatestVersion`, likewise.

So the default says: *these two machines agree if their engine installs agree*. Two people who both
believe they are on "UE 5.8" but differ by a hotfix compute different values from identical source,
and nothing in the failure names the engine.

### What this project does instead

`TraceNet::InstallNetVersionOverride()` (`Source/Trace/UI/TraceNetworking.cpp`), called from
`FTraceGameModule::StartupModule`, binds `FNetworkVersion::GetLocalNetworkVersionOverride` — the
engine's own supported hook — to a checksum built from exactly two things, **both of which live in
this repository**:

```
"trace netproto <NetProtocolVersion>, project <ProjectVersion>"
```

* `NetProtocolVersion` — `Source/Trace/UI/TraceNetworking.h`, currently `1`
* `ProjectVersion` — `Config/DefaultGame.ini`, currently `0.1.0`

Any two builds of the same commit therefore agree by construction, on any platform, on any 5.8
install. Today's value is **`NET 51920028`** (decimal 1368522792).

### The trade-off, stated where it is made

This **removes** the engine's accidental protection against connecting two different engine versions.
Two builds of the same commit on 5.8 and on some future 5.9 would now agree on the version number and
then disagree about the wire format, which fails later and less clearly. That is acceptable for a
project pinned to one engine by `Trace.uproject`'s `EngineAssociation` — and it is why
`NetProtocolVersion` exists. **Bump it** when the engine version moves, when a replicated property is
added/removed/reordered in a way old clients cannot read, or when an RPC signature changes.

### `FCrc::StrCrc32` is portable, and that is checked rather than assumed

`TCHAR` is 2 bytes on Windows and 4 on macOS, so a naive string hash would differ by platform.
`Crc.h`'s `StrCrc32` widens every character to 32 bits before folding it in and says why in its own
comment — *"we always want to treat every CRC as if it was based on 4 byte chars ... so we want
consistency between equivalent strings with different character types"*. The table at `Crc.cpp:211`
is the standard reflected CRC-32 (`0xEDB88320`). This is also why the engine's own default value
works cross-platform at all.

### Three ways to read the value

| Who | How |
|---|---|
| A player on any build, including Shipping | Title screen, bottom right: `V 0.1.0   NET 51920028` |
| Anyone with the repository — no engine, no build, one second | `python3 Scripts/netversion.py` |
| A developer with a console | `Trace.NetVersion` |

The title screen is the one that matters for a playtest: Shipping compiles logging out and has no
console, so it is the only place a player can see it. `Scripts/netversion.py --verify <log>` checks
the script against a Development run's own log, so the script is tested against the binary that
performs the handshake rather than trusted alongside it.

### What is proven and what is not

**Proven on this machine:** the override is installed and in force (`LogNetVersion: Checksum from
delegate: 1368522792`, and `Trace.NetVersion` reports that the engine's own
`GetLocalNetworkVersion()` returns the same number); the value is byte-stable across two separate
builds of the same source; `Scripts/netversion.py` predicts it from source alone; the code renders on
the title screen; the literals are present in the Shipping artefact.

**Not proven, and cannot be from a Mac:** that a real Windows client connects to a real Mac host.
There is no Win64 toolchain on this machine and Unreal does not cross-compile a Windows game from
macOS. What has been removed is the *class* of failure where identical source produces different
numbers. The first real cross-platform session is still the first real cross-platform session.

**Audited for platform divergence in the net path:** there is no `#if PLATFORM_MAC`,
`PLATFORM_WINDOWS`, `__APPLE__` or `_WIN32` anywhere under `Source/` (zero occurrences); no
replicated property uses a platform-sized type (`long`, `size_t`, `PTRINT`); the only custom
`NetDeltaSerialize` is `FTraceTrailPointArray`'s, which delegates straight to the engine's
`FFastArraySerializer::FastArrayDeltaSerialize`; and the only project-authored hash that could have
been TCHAR-width-dependent is the compatibility checksum itself, addressed above. The one other
`GetTypeHash(FString)` in the project (`TraceCharacterSelect.cpp`, a layout signature) is local
log de-duplication and never reaches the wire.

---

## 5. What Trace already does for netcode

Written for someone who has not read the source. Each item names where it lives.

### Client-side prediction of movement *and* dash
`Movement/TraceCharacterMovementComponent`

Unreal's `CharacterMovementComponent` already predicts walking: your client simulates your own
movement immediately, sends the input to the server, and the server replays it. If the server
disagrees, it sends a correction and the client silently replays its queued moves from the
corrected state. That is why your own character never feels laggy.

Custom abilities break this by default — a dash implemented naively would only start after a
round-trip, which at 100 ms feels broken. Trace extends the prediction system properly:
`FSavedMove_Trace` records the dash intent, plus the dash timer and cooldown, into each saved
move; the intent travels to the server packed into a single bit of the compressed-flags byte
(`FLAG_Custom_0`); `UpdateFromCompressedFlags` unpacks it on the server and during client replay.

The reason the *timer and cooldown* are also saved and restored is subtle and matters: if a
correction arrives mid-dash and the client replays its moves, it must replay them with the dash
state it actually had at that moment. Otherwise every correction desyncs the dash and you get
rubber-banding exactly when the player is doing the most important thing in the game.

**Practical result:** dash fires on the frame you press Shift, regardless of ping, and the server
still decides where you ended up.

### Server-authoritative hitscan with server-side rewind
`Gameplay/TraceWeaponComponent`, `Net/TraceLagCompensationComponent`

The problem: you see enemies where they *were*, roughly one round-trip ago. If the server checks
your shot against where they are *now*, you have to lead every target, and the game feels like it
is stealing your hits.

The fix is lag compensation. The server keeps a short rolling history of every character's capsule
pose (`RecordFrame`, one entry per tick, kept for `LagCompHistoryDuration`). When a client fires,
it stamps the shot with `GetServerWorldTimeSeconds()` at the moment it pulled the trigger. The
server clamps that timestamp — **never further back than `MaxRewindTime` (default 0.25 s)** — then
interpolates each character's pose to that instant and tests the shot against those historical
positions.

Two details that make this implementation better than most tutorials:

- **No actor is ever moved.** Rewind is pure math: a world trace for static geometry to bound the
  range, then a segment-vs-capsule test per candidate using `FMath::SegmentDistToSegmentSafe`
  against the historical pose. Physically teleporting actors backwards and forwards (the common
  approach) is slow, breaks overlaps and can corrupt physics state.
- **The clamp is the security boundary.** A hostile client could otherwise claim it fired ten
  seconds ago and shoot people who have since walked behind a wall. `MaxRewindTime` bounds how much
  of the past a client is allowed to buy. Raising it favours high-ping shooters; lowering it favours
  the target's right to have gotten behind cover. 0.25 s is a normal shipping value.

The resolver skips the shooter, dead players, **the Core carrier (invulnerable by design)**, and —
unless `bFriendlyFire` is on — teammates. Headshots are a height test against the historical
capsule, not a separate collider.

### Delta-replicated trail
`Gameplay/TraceTrailComponent`, `FTraceTrailPointArray` in `TraceTypes.h`

The trail is up to 256 points that change every tick. A plain `TArray<FVector>` marked `Replicated`
would resend **the entire array** every time any element changed — the classic way to saturate a
connection with something that looks harmless in the editor.

Instead the trail uses `FFastArraySerializer`, Unreal's delta-replication container. It tracks
per-item replication IDs and sends only **adds, removes and changes**. Adding one point at the head
and expiring one at the tail costs two small deltas, not 256 vectors. Clients get
`PostReplicatedAdd` / `PreReplicatedRemove` callbacks and rebuild the visual cylinders incrementally.

The points are `FVector_NetQuantize`, so each is sent at 1-unit precision instead of three full
floats.

### Quantised RPC payloads
Throughout, but see `ServerFire`

`FVector_NetQuantize` sends a position at 1 uu precision; `FVector_NetQuantizeNormal` sends a
direction in 16 bits per component. Full `FVector` on the wire is 96 bits (or 192 at double
precision) for accuracy nobody can perceive. Every RPC that carries a position or direction in
Trace uses the quantised types. It is free, and it compounds — a fire RPC at 8 shots/second from
10 players is a lot of packets.

### Unreliable multicast for cosmetics, reliable RPCs for state
`MulticastFireEffects` (unreliable) vs `ClientNotifyHit` (reliable)

Reliable RPCs are retransmitted until acknowledged and are ordered — they consume bandwidth and,
worse, a saturated reliable buffer will **disconnect the client**. So:

- Muzzle flashes and tracers → `NetMulticast, Unreliable`. If one is dropped, a bullet trail is
  briefly missing. Nobody notices, and nothing desyncs.
- Scores, carrier changes, deaths, hit confirmation → reliable, or plain property replication.

### Owner-skipping multicasts
`UTraceWeaponComponent::ServerFire`

The shooter's client already drew its own tracer the instant it clicked — that is the prediction.
If the server then multicast the effect to *everyone*, the shooter would see two tracers. So the
server sends the cosmetic multicast to everyone **except** the owning client. Small thing, very
visible when it is wrong.

### One shared clock
`ATraceGameState::GetServerWorldTimeSeconds()`

Every timestamp in Trace — fire times, trail point birth times, the match clock, rewind targets —
is expressed in *server world time*. `AGameStateBase` already replicates this: the server
periodically replicates its `GetTimeSeconds()`, and each client computes a delta against its own
local time. Trace uses the built-in clock rather than hand-rolling an RTT handshake, because a
hand-rolled clock is a large amount of subtle code to get slightly worse results.

Its limitation, stated plainly: **the delta ignores one-way latency**, so a client's idea of server
time trails the server's by roughly half a round-trip, and it is smoothed. That is completely fine
for cooldowns, match timers and the rewind timestamp (which the server clamps anyway). It would not
be fine as the sole basis for sub-frame hit registration — which is why the rewind path clamps and
validates rather than trusting the client's stamp.

Always store it in `double`. It returns `double` on UE 5.3+ and used to return `float`.

### The net settings that back all of this
`Config/DefaultEngine.ini`

Worth knowing, because three of them are load-bearing rather than decorative:

| Setting | Value | Why |
|---|---|---|
| `NetServerMaxTickRate` | `60` | Server simulation rate. The lag-comp history buffer (`LagCompHistoryDuration = 1 s`) is sized assuming roughly this — at 60 Hz that is ~60 recorded poses per player. Drop the tick rate and rewind gets coarser. |
| `MaxClientRate` / `MaxInternetClientRate` | `100000` | Raised well above the 15 KB/s engine default. Ten characters plus a replicated trail does not fit in the default budget, and the symptom of overrunning it is *not* an error — it is silent starvation of low-priority actors. |
| `bSmoothFrameRate` | `False` | Frame-rate smoothing lies to `DeltaTime`. Client prediction replays moves against `DeltaTime`; a lie there is a desync. Leave it off. |
| `MAXPOSITIONERRORSQUARED` | `3.0` (engine default) | Squared distance in uu² past which the server corrects a client. Tempting to raise when dash produces corrections — don't. Corrections while dashing mean the *prediction* is wrong, and raising the threshold hides the bug instead of fixing it. |
| `bMovementTimeDiscrepancyResolution` | `False` | Detection is on, resolution is off. Resolution retroactively slows a client whose claimed move time drifts, which is deeply confusing while you are tuning movement. Turn it on before shipping, not before playtesting. |

---

## 6. Console commands for testing

Press **`~`** in-game.

### Connection

| Command | What it does |
|---|---|
| `TraceNetInfo` | **Trace's own.** Net mode, this machine's endpoint, every local adapter, and the full PlayerState roster with bots marked. The direct answer to "are we in the same match?" |
| `open <ip>:7777` | Connect to a server |
| `open <map>?listen` | Become a listen server on the given map |
| `disconnect` | Drop back to the main menu / local game |
| `travel <map>` | Server-side map change, bringing clients with you |

### Measuring

| Command | What it shows |
|---|---|
| `stat net` | **The main one.** In/out bandwidth, packet counts, ping, per-channel breakdown. Watch this while playing. |
| `stat unit` | Frame / game / draw / GPU ms. Confirms a "network" problem isn't a framerate problem. |
| `stat game` | Gameplay tick cost — check the trail trip-test isn't dominating. |
| `net.Reliable.Debug 1` | Log reliable RPCs. Loud; use in bursts. |
| `p.NetShowCorrections 1` | **Essential for movement work.** Draws a capsule wherever the server corrected your client, and logs the mismatch. Any correction while dashing in a straight line is a prediction bug. |
| `p.NetShowCorrections 0` | Off |

### Simulating bad networks

UE 5 exposes packet emulation as `NetEmulation.*` console commands (the older `Net.PktLag` family
still exists as a legacy alias). These only work in non-Shipping builds.

| Command | Effect |
|---|---|
| `NetEmulation.PktLag 100` | Add 100 ms one-way delay (≈200 ms RTT) |
| `NetEmulation.PktLagVariance 20` | Jitter, ±20 ms — jitter breaks more code than flat latency does |
| `NetEmulation.PktLoss 5` | Drop 5% of packets |
| `NetEmulation.PktOrder 1` | Reorder packets |
| `NetEmulation.PktDup 2` | Duplicate 2% of packets |
| `NetEmulation.Off` | Clear all emulation |

Legacy equivalents if the above are unavailable on your build: `Net.PktLag`, `Net.PktLagVariance`,
`Net.PktLoss`, `Net.PktOrder`, `Net.PktDup`.

Run emulation **on the client** to simulate that client's bad connection; run it **on the server**
to make everyone's life hard at once.

### Running these headlessly — `-TraceExec`

Most of Trace's verification commands (`Trace.VerifyKnobs`, `Trace.ModeB.Verify 1`,
`Trace.Trail.TestHeadGap`, `Trace.TestRecoil`) only mean anything once there is a live match, and the
engine's own `-ExecCmds` fires during engine init — on the **title screen**, where every one of them
is a no-op. So there was no way to run them without a human typing into a console, which is how a
check quietly stops being run.

```bash
-TraceExec="Trace.VerifyKnobs|Trace.ModeB.Verify 1"  -TraceExecAt=12  [-TraceExecOn=Match|Menu]
```

Commands are separated by `|` (not `,` — several of them take numeric arguments). `-TraceExecAt` is
seconds after the HUD comes up and defaults to 8; `-TraceExecOn` defaults to `Match`. They are run in
order, through the local player controller, so commands that resolve "the local pawn" work.

**Do not batch two commands that both drive pawns.** Running `Trace.TestRecoil` alongside
`Trace.Trail.TestHeadGap` in one pass reported `max aimErr 116.78 deg`; run in isolation the same
build reports `0.0000 deg over 347 samples`. The head-gap test dashes characters around, and the
recoil harness measured the yank. One driving test per run.

### Trace-specific debugging

Server-side rewind visualisation is behind a setting rather than a cvar — set
`bDrawServerRewindDebug=True` under `[/Script/Trace.TraceSettings]` (see
[DESIGN.md](DESIGN.md#tuning-without-recompiling)) to draw the historical capsules the server tested
against. Turn it off again; it is noisy.

---

## 7. A playtest checklist that actually finds bugs

Run this after any change to movement, weapons or the trail. Two real processes — listen server
(§2), or `run-dedicated-server.sh --editor` (§3) if you want the host out of the game — with
`NetEmulation.PktLag 80` and `PktLagVariance 15` on at least one client. Do not run it entirely in
PIE; see §1 for what PIE hides.

- [ ] **Dash feels instant on a laggy client.** Press Shift, watch for movement on the same frame.
      `p.NetShowCorrections 1` on: dashing in a straight line should produce **zero** corrections.
- [ ] **Dash cooldown agrees between client and server.** Spam Shift; the HUD pip and the actual
      ability must not disagree.
- [ ] **Shooting a strafing target at 80 ms hits where you aimed**, not where they are now. Turn on
      `bDrawServerRewindDebug` and confirm the rewound capsule sits under your crosshair.
- [ ] **Shooting someone who just rounded a corner does *not* hit** if more than `MaxRewindTime`
      has passed.
- [ ] **Bullets do nothing to the carrier**, from any angle, at any ping.
- [ ] **No friendly fire**, including a teammate standing directly in the muzzle.
- [ ] **Walking through an enemy trail does nothing.** Run it end to end. Zero effect.
- [ ] **Dashing through an enemy trail kills the carrier** — and the Core drops at the carrier's
      location on *every* machine, and the trail disappears everywhere at once.
- [ ] **Dashing through a *teammate's* trail does nothing.**
- [ ] **`stat net` bandwidth stays flat while a carrier runs a long trail.** If out-bandwidth
      climbs with trail length, delta replication has regressed to a full array resend — that is
      the single most likely performance regression in this codebase.
- [ ] **A pass can be intercepted by an enemy** and the carrier state updates on all clients.
- [ ] **Late joiner** sees correct scores, correct carrier, correct trail. (Join a match already
      in progress — this catches replication that only works via RPC and not via property state.)
- [ ] **Client disconnect mid-carry** drops the Core rather than deleting it.

### Two processes on one machine, headlessly

The entry points in §0 can be driven from the command line, which is how this flow is regression
tested. `-TraceAutoPlay=<sec>` presses PLAY (and therefore hosts). `-TraceAutoJoin=<sec>` drives the
real JOIN row — it selects the row, opens the real prompt, fills the real field and, **2.5 s later**,
submits through the real `ConfirmJoin()`. That delay is deliberate: it is the window in which
`-TraceAutoShot` can photograph the filled prompt. The address comes from `-TraceJoinAddress=`, or
from the remembered one if that is absent.

```bash
UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"

# Host
"$UE" Trace.uproject -game -RenderOffScreen -windowed -resx=1280 -resy=720 \
    -abslog=/tmp/host.log -TraceAutoPlay=3 &

# Client, a few seconds later
"$UE" Trace.uproject -game -RenderOffScreen -windowed -resx=1280 -resy=720 \
    -abslog=/tmp/client.log -TraceAutoJoin=3 -TraceJoinAddress=127.0.0.1:7777 &
```

What to grep for, and what each line proves:

What to grep for, and what each line proves. Note that `Join succeeded` is a **server**-side line and
`Welcomed by server` is the **client**-side one — they are the two ends of the same handshake, so
check for each in the right log:

| Log | Line | Proves |
|---|---|---|
| host | `Title screen: PLAY -> /Game/Maps/Arena?difficulty=…?mode=…?listen` | the URL carries `listen` at all — this is the bug that was fixed |
| host | `IpNetDriver listening on port 7777` | `?listen` reached `UWorld::Listen` and the socket bound |
| host | `NotifyAcceptedConnection … RemoteAddr:` then `Join succeeded:` | a real remote connection completed |
| host | `[Net] PlayerController BeginPlay: netMode=ListenServer local=0 authority=1` | a **remote** player's controller exists on the server (`local=0` is the tell) |
| host | `Evicted a bot to make room for a human.` | bots yield their slot as humans arrive |
| host + client | `[Net] … - 2 human player(s) of 10 player states, N characters in the world.` | both ends agree on the roster |
| client | `Title screen: JOIN -> ClientTravel('…', TRAVEL_Absolute)` | the JOIN row, not a command-line shortcut, started the connection |
| client | `Welcomed by server (Level: /Game/Maps/Arena, Game: /Script/Trace.TraceGameMode)` | the client is in the server's match |
| client | `[Net] Match HUD ready: CLIENT endpoint=…` | the client HUD resolved its role from the real net driver |

`-TraceAutoShot=<sec>` on either process writes a screenshot to `Saved/Screenshots/`, which is how
the client's view of the match is confirmed. Always pass `-RenderOffScreen` so no window appears.

**On a shared machine, kill only your own PIDs.** `pkill -f UnrealEditor` will also take out anybody
else's run, including a headless test somebody is halfway through.
