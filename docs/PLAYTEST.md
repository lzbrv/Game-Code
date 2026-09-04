# Trace — playtest guide

**A friend sent you a game. This is everything you need to play it with them.**

You do not need the source code, an Unreal install, or any of the rest of this repository. Sections 1
to 6 are written for someone who has never seen this project. Section 8 is for whoever is *making*
the build.

The short version:

| | |
|---|---|
| **Install** | Unzip it. macOS and Windows will both refuse to open it once — §1 and §2 tell you the exact click. |
| **Network** | Everyone joins the same Tailscale tailnet. The host reads their address off the title screen. |
| **Host** | Press **PLAY**. That is it — every match is joinable. |
| **Join** | Pick **JOIN**, type the host's address, press Enter. |
| **Port** | **UDP 7777** on the host. The Windows firewall prompt is the one thing you must not dismiss. |

Everything here was measured against a real packaged build, not inferred from documentation. Where
something is **untested**, it says so in those words. Two things in this file have never been run at
all and both are labelled — do not assume the rest is guesswork; it is not.

---

## 1. macOS — getting it to open

### What you got

A zip containing **`Trace-Mac-Shipping.app`**. The zip is **385 MB**; it expands to **717 MB**.
Unzip it anywhere — Applications, Desktop, Downloads, it does not matter.

### It will refuse to open the first time. This is normal.

The app is not code-signed by a registered Apple developer, so macOS quarantines anything downloaded
and blocks it. **Double-clicking will fail**, and on recent macOS the dialog says the app
*"is damaged and can't be opened"*.

**That message is a lie.** Nothing is damaged. macOS says "damaged" when it means "unsigned". Do not
delete it and ask for another copy — you will get the identical result.

**Two ways to fix it. Either works, once, per download.**

**The easy one — no Terminal:**

1. **Right-click** (or Control-click) `Trace-Mac-Shipping.app`
2. Choose **Open**
3. In the dialog that appears, click **Open** again

It has to be the right-click menu. Double-clicking will keep failing even after this works once.

**The reliable one — one line in Terminal:**

```
xattr -dr com.apple.quarantine ~/Downloads/Trace-Mac-Shipping.app
```

Change the path to wherever you actually put it. Then double-click normally. If you are unsure of the
path, type `xattr -dr com.apple.quarantine ` (with the trailing space) and then drag the app from
Finder into the Terminal window — it fills the path in for you.

> On macOS 15 and newer, right-click → Open has been removed for some downloads. If you do not get an
> **Open** button, use the Terminal command, or go to
> **System Settings → Privacy & Security**, scroll down, and click **Open Anyway** next to the
> message about Trace.

### Two things that will stop it dead, and are worth checking before you download 717 MB

* **Apple Silicon only.** The binary is `arm64` and there is no Intel version. An Intel Mac cannot run
  it at all, and Rosetta does not help — Rosetta translates Intel code for Apple Silicon, not the
  other way round. Any Mac from roughly 2020 or earlier is out. Check with  → About This Mac; you
  need "Apple M1" or later, not "Intel".
* **macOS 14 (Sonoma) or newer.** Older macOS refuses to launch it.

---

## 2. Windows — getting it to open

> **UNTESTED SECTION.** The Windows build has never been produced or run. Nobody on this project
> owns a Windows machine that has built it — see §8.2. The steps below are the standard Windows
> behaviour for an unsigned game and the firewall behaviour this game's networking requires; they are
> not a transcript of somebody doing it. Expect the substance to be right and the exact dialog wording
> to vary with your Windows version.

### What you got

A zip of a folder — most likely called **`Windows`** — containing `Trace.exe` with a `Trace` folder
beside it. The exact folder name is the engine's choice and is not verified here; whatever it is
called, the `.exe` and the folder next to it travel together. Size not measured; the Mac build is
385 MB zipped and 717 MB unzipped, so budget roughly that.

**Unzip the whole folder and keep it together.** `Trace.exe` on its own is not the game — it is a
launcher, and the several hundred megabytes of content sit beside it. Copying just the .exe out
produces something that starts and immediately closes.

### SmartScreen will block it once

The build is unsigned, so the first launch shows a blue box: **"Windows protected your PC"**, with
only a **Don't run** button visible.

1. Click **More info** (small link, easy to miss — it is above the button)
2. Click **Run anyway**

Once per machine. Some browsers also mark the download itself as unsafe; you may have to choose
**Keep** in the browser's download list before you can even unzip it.

### The firewall prompt — DO NOT DISMISS THIS ONE

**This is the most likely reason your evening goes wrong.**

The first time you press **PLAY**, Windows Defender Firewall pops up asking whether to allow
`Trace.exe` to communicate on a network.

* **Tick "Private networks"** and click **Allow access**.
* If you click **Cancel**, or press Escape, or the box appears behind the game and you never see it,
  Windows silently blocks the port — **and the game will not tell you anything is wrong.** You will
  be happily playing on your own while everyone trying to join gets a connection timeout, and from
  your side it looks like *their* problem.

Only the **host** needs this. Joining players make an outbound connection, which the firewall allows
by default.

**If you already dismissed it**, nothing prompts you a second time. Re-allow it by hand:

1. **Settings → Privacy & security → Windows Security → Firewall & network protection**
2. **Allow an app through firewall** → **Change settings**
3. Find **Trace** in the list and tick **Private**. If it is not listed at all, click
   **Allow another app…** → **Browse…** and point it at the `Trace.exe` you unzipped.

If you cannot find it, the fastest reset is to delete any `Trace` entries in that list entirely and
launch the game again — with no rule present, Windows prompts you afresh.

---

## 3. Tailscale — how you reach each other

This game connects **directly, by IP address**. There is no matchmaking, no server list, no lobby
code. That works fine on one home network, but if you are in different houses you need something that
puts you all on one private network. That is what Tailscale does.

### Everyone does this once

1. Install Tailscale: <https://tailscale.com/download> (free for personal use).
2. **Everyone must be on the same tailnet.** One person's account owns it and invites the rest —
   Tailscale admin console → **Users** → **Invite external users**, or simply have everyone sign in
   with the same account. Installing Tailscale is not enough on its own: if you are each on your own
   tailnet you will not be able to see each other, and the symptom is an ordinary connection timeout
   with nothing to suggest the cause.
3. Make sure it is actually **running**, not merely installed.

### Find your address

```
tailscale ip -4
```

You get one line, something like `100.124.112.27`. Tailscale addresses always start with **100.**
and are always four numbers.

* macOS: open Terminal. If `tailscale` is not found, the command is
  `/Applications/Tailscale.app/Contents/MacOS/Tailscale ip -4`.
* Windows: open Command Prompt or PowerShell.
* Either OS: the Tailscale menu-bar / system-tray icon shows the same address, and clicking it copies
  it.

### **`tailscale ip` can lie to you. This is a real trap and it cost an afternoon.**

`tailscale ip -4` prints an address from *stored settings*, **even when Tailscale is switched off.**
Measured on the development machine: it printed `100.124.112.27` while `tailscale status` said
`Tailscale is stopped`, no network interface carried that address, and pinging it failed.

So the address alone proves nothing. **Check the connection, not the number:**

```
tailscale status
```

If it says **`Tailscale is stopped`**, that is your problem. Fix it:

```
tailscale up
```

Or click the menu-bar / tray icon and switch it on. `tailscale status` should then list your machine
and everyone else's. **Peers shown as `offline` cannot be reached** — they have to open Tailscale on
their own machine.

Quick proof it is really working: `tailscale ping <the-host's-address>`. If that does not reply, the
game will not connect either, and no amount of retyping the address in the game will change that.

---

## 4. Hosting a game

**There is no host button.** Every match you start is also a server. The title screen says so, at the
bottom: **`PLAY ALSO HOSTS - EVERY MATCH IS JOINABLE`**.

1. Launch Trace. You land on the title screen.
2. Look at the address chip under the tagline. It reads:

   ```
   YOUR ADDRESS   100.124.112.27:7777
   ```

   **That is the line you send your friends.** The game picks your Tailscale address on purpose —
   when a machine has several addresses, a `100.x` one is ranked above a home-router `192.168.x` one,
   because the Tailscale address is the one that works from another house.

   If the chip shows a `192.168.` or `10.` address instead, Tailscale is not up. Go back to §3.

3. Press **PLAY** (Enter on the highlighted row).
4. If a **SELECT YOUR CHARACTER** screen appears, pick with the number keys `1`–`9` and `0`, or
   the arrow keys, then **Enter** to **LOCK IN**. There is a countdown and you get a character
   anyway if you do nothing.
5. You are in. **Hold `Tab`.** Top right of the screen you get:

   ```
   HOSTING  100.124.112.27:7777
   0 PLAYERS CONNECTED - 1 HUMAN PLAYER
   ```

   That is your proof that hosting worked, and the counts go up as people arrive. **This chip is the
   authority on your address, not the title screen** — see the warning immediately below.

   **You have to hold `Tab` to see it most of the time, and that surprises people.** The panel is
   deliberately not burned into every frame. It shows on its own for about **ten seconds** at match
   start and again for ten seconds each time the answer changes — someone joining, someone leaving —
   and any time you hold `Tab` for the scoreboard. If you look up thirty seconds into a match and
   the corner is empty, **nothing is wrong**; hold `Tab`. The scoreboard's own footer says
   `HOLD TAB`.

   The one exception is the failure state below, which is never hidden.

Your friends can join at any time. You do not have to wait on the menu for them.

### If the title screen warns you about the port

If something else on your machine is already using UDP 7777 — most often a second copy of Trace you
forgot to close — the title screen shows:

```
PORT 7777 IS BUSY ON THIS MACHINE - THE HUD WILL SHOW THE REAL PORT IN-GAME
```

**Hosting still works.** The game quietly moves to the next free port. But the number on the title
screen is then wrong, and if you read it out to your friends you will send them to a port nothing is
listening on. **Start the match, hold `Tab`, and read the real address off the `HOSTING …` panel in
the top right instead.** It will not end in `:7777`.

### If the top right says `OFFLINE - NOBODY CAN JOIN`

Exactly what it says: the match is running but no server started, so nobody can connect. On Windows
this is almost always the firewall prompt from §2. Quit to the title screen and start again after
fixing it.

**You do not have to hold `Tab` for this one.** Unlike the `HOSTING` panel, the failure state is
pinned on screen permanently and cannot be hidden — that is deliberate, because it is precisely the
state that otherwise looks identical to a working host from the inside.

---

## 5. Joining a game

1. Launch Trace.
2. Move down to **JOIN** (W/S or the arrow keys) and press **Enter**.
3. A panel opens: **JOIN A GAME** / *TYPE THE HOST'S ADDRESS*.
4. Type the host's address — `100.124.112.27` — and press **Enter**.

   * You can leave the port off. The panel tells you so:
     *`PORT 7777 IS ADDED FOR YOU IF YOU LEAVE IT OFF`*.
   * **You can paste**: `Ctrl+V` on Windows, `Cmd+V` on macOS. The panel confirms with
     *`PASTED FROM CLIPBOARD`*. Paste it — a mistyped digit is the single most common cause of "it
     doesn't work".
   * `Esc` cancels, `Backspace` deletes.
   * The panel also shows **`THIS MACHINE IS <your address>`** at the bottom, so you can read your own
     address out without leaving the screen.

5. You get **`CONNECTING TO …`**, then character select if it is shown, then the match.
6. **Hold `Tab`** in the match and you should see, top right:

   ```
   CONNECTED  100.124.112.27:7777
   2 HUMAN PLAYERS
   ```

   Same as for the host: it also appears on its own for ten seconds when you arrive, and it is not
   on screen the rest of the time. An empty corner is not a problem.

**Next time is two keypresses.** The game remembers the last address you tried, shows it on the JOIN
row, and pre-fills the box — so the same four people playing again tomorrow is JOIN, Enter, Enter.
It remembers the address you *attempted*, not only ones that worked, which is deliberate: the usual
reason to retry is that the host has just fixed their firewall.

### Controls on the title screen

The screen no longer prints these — the key legend that used to run along the bottom was taken off
in D30 — so this is now the only place they are written down:

| Key | Does |
| --- | --- |
| `W` / `S`, or the up/down arrows | Move the highlight |
| `A` / `D` | Change the value on the row you are on |
| `Enter` | Select |
| `Esc` | Jump to QUIT, then quit |

`Esc` first jumps the highlight to **QUIT** rather than quitting immediately, so a stray Escape
cannot close the game. Press it again to actually quit.

In a match, `Esc` opens the pause menu: **RESUME / SETTINGS / VIDEO / RETURN TO TITLE / QUIT**.

---

## 6. The port

* **UDP 7777**, on the host only.
* **UDP, not TCP.** If somebody is opening a port by hand on a router or in a firewall rule, TCP 7777
  will do nothing at all. This catches people out.
* Over Tailscale you should not need to open anything — Tailscale carries the traffic itself. The port
  matters for the *local* firewall on the host machine (§2), not for a router.
* Only the host needs it open. Joiners connect outward.

---

## 7. When it does not work

**Work down this list in order.** It is ordered by how often each one is actually the problem, not by
how technical it sounds.

**First: read the screen.** A failed join does not fail silently — it drops you back to the title
screen with an amber banner that stays up for a minute and says what went wrong in plain English. The
messages you are likely to see:

| On screen | What it actually means |
|---|---|
| `CONNECTION TIMED OUT. CHECK THE ADDRESS, THE VPN, AND UDP 7777 ON THE HOST.` | Nothing answered. Host's firewall, Tailscale down at one end, or a wrong address. By far the most common. |
| `COULD NOT REACH THAT ADDRESS.` | No route at all — usually Tailscale not running on one of the two machines. |
| `THAT ADDRESS IS NOT VALID. USE  <ip>:7777.` | A typo. Retype or paste it. |
| `THE SERVER REFUSED THE CONNECTION.` | You reached them, and they said no. Usually a full match. |
| `CLIENT AND SERVER ARE RUNNING DIFFERENT BUILDS.` | Somebody has an older copy. Everyone needs the same download. |
| `COULD NOT LISTEN ON UDP 7777. ANOTHER COPY MAY ALREADY BE HOSTING.` | On the host: you have Trace open twice. Close the other one. |

Then, in order:

1. **Is the host actually hosting?** Ask them to **hold `Tab`** in the match and read the top right
   of their screen. It must say `HOSTING`. If it says `OFFLINE - NOBODY CAN JOIN` — which shows
   without holding anything — nothing else on this list will help until that is fixed.
2. **Did the host allow the Windows firewall prompt?** §2. If they are on Windows and they cannot
   remember seeing it, assume they dismissed it — that is the normal outcome. Re-allow it by hand.
3. **Is Tailscale actually up on both machines?** `tailscale status` on each — the word `stopped`
   anywhere is the answer. Do not trust `tailscale ip`; see §3.
4. **Are you on the same tailnet?** `tailscale status` on the joiner should *list the host's machine
   by name*. If it does not appear at all, you are on separate tailnets and no address will work.
   Somebody has to send an invite.
5. **Can you reach them outside the game?** `tailscale ping <host address>`. No reply means the
   problem is the network, not the game, and everything below is a waste of time.
6. **Is the address right?** Have the host hold `Tab` and read it off the `HOSTING …` panel *in the
   match*, not off the title screen — if they saw the "PORT 7777 IS BUSY" warning, those two
   disagree and the panel is the correct one. Paste rather than retype.
7. **Is anybody running a second copy of Trace?** On the host that moves the port. On the joiner it is
   harmless but confusing.
8. **Everyone on the same build?** A version mismatch produces
   `CLIENT AND SERVER ARE RUNNING DIFFERENT BUILDS.` Re-download from the same link.
9. **Restart the game on both ends.** Genuinely worth doing before anything below it.
10. **Try over an ordinary home network first**, both machines in one house, joiner typing the host's
    `192.168.x.x`. If that works and Tailscale does not, the problem is Tailscale and not the game.

**If somebody asks you for a log, there is not one.** Measured on the shipped Mac build on
2026-09-04: two full sessions — launch, host, play — wrote **no log file at all**. Not a short one;
none. This is not a subtlety about the log being sparse. The release build has its logging compiled
out.

The control that makes that a real finding rather than a failure to look: in the same window the
same two runs *did* write
`~/Library/Containers/com.YourCompany.Trace/Data/Library/Application Support/Epic/Trace/Saved/Config/Mac/GameUserSettings.ini`
and two crash-reporter config files, so the app was certainly writing to that container. It just
never wrote a log into it.

So **do not go hunting** in
`~/Library/Containers/com.YourCompany.Trace/Data/Library/Logs/Trace/`. If a `Trace.log` is sitting
there it is stale, from some earlier build, and sending it will mislead whoever reads it — check its
timestamp before you send anything.

A log that is useful for diagnosis has to come from a **Development** build, which whoever sent you
the game can produce with one flag (§8.1). Ask them for one; do not try to extract it from this
build.

---

## 8. For whoever is making the build

Everything above this line is for a player. Everything below needs the repository.

### 8.1 macOS

```
Scripts/package.sh
```

That is the whole command. It cooks content and produces:

```
Saved/Packaged/Mac/Trace-Mac-Shipping.app        717 MB   (385 MB zipped)
```

Roughly a minute and a half on a warm cache; budget much longer for the first cook on a fresh
machine. `Scripts/package.sh --help` documents the options. The useful ones:

* `-c Development` — same cooked content, but it logs. This is what to build for a playtester who is
  reporting a bug, because a Shipping build's log is empty.
* `--iterate` — fast re-cook after a content change.
* `-n` — print the RunUAT command and run nothing.

**`Scripts/build.sh` does not produce a sendable game.** It proves the Shipping target links. Linking
is not shipping — see `docs/KNOWN_LIMITATIONS.md` item 29 for the two real defects that hid behind
that distinction for seven waves.

**Import the character art before you package**, or your players get untextured placeholder shapes:

```
Scripts/import-mannequin.sh      # once per clone; the art is gitignored
Scripts/package.sh
```

*Both* steps are required, and until 2026-09-04 the second one silently ignored the first — see
§8.4.

**Send the `.app` as a zip** (Finder → Compress). A `.app` is a directory, and most file-transfer
services mangle it otherwise.

### 8.2 Windows

**It cannot be built on the Mac, and this is not a limitation of the script.** Unreal does not
cross-compile a Windows game from macOS, and a macOS engine install contains no Win64 binaries at
all:

```
$ ls "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/"
DotNET    Mac    ThirdParty
```

`Scripts/package.sh -p Win64` refuses immediately and prints what to do instead, rather than failing
forty minutes into a cook with a confusing message about a missing target receipt.

On the Windows checkout, it is one command:

```
Scripts\build.bat          :: first time only, and the cook needs it
Scripts\package.bat
```

Output lands in `Saved\Packaged\Windows\`. Zip and send **that whole folder**.

> **`Scripts\package.bat` has never been run.** It was written on macOS against `package.sh`, on a
> machine that cannot execute it. Read the comment block at the top of the file before concluding the
> project is broken: the three failures to expect first are a missing Visual Studio C++ toolchain, a
> `UE_ROOT` pointing at an incomplete engine install, and — most likely — the project's editor
> modules not being built, which makes the cook fail minutes in with what looks like a content error.
> `Scripts\build.bat` first fixes the third.

### 8.3 What is and is not signed

| | macOS | Windows |
|---|---|---|
| Signed | Ad-hoc only — `codesign` reports `Signature=adhoc`, `TeamIdentifier=not set` | Not at all |
| Notarized | No — `spctl -a -vvv -t exec` returns `rejected` | n/a |
| Consequence | Gatekeeper blocks it; §1 | SmartScreen blocks it; §2 |

Getting rid of both prompts means an Apple Developer account ($99/yr, plus notarization) and a
Windows code-signing certificate. For a weekend playtest among friends, telling them about §1 and §2
in advance is the proportionate answer.

The bundle identifier is still the engine placeholder **`com.YourCompany.Trace`**. Harmless for a
playtest, but it is why the sandbox container in §7 has that name, and any other project that also
never changed the placeholder would share it.

### 8.4 The character art was not in the package until 2026-09-04

Worth knowing because the symptom looks like a broken install and the on-screen remedy is useless to
a player.

`ATraceCharacter` names Epic's Mannequin through a **soft** object reference built from a string path
(`TraceCharacter.cpp:260-261`). The cooker does not follow soft references, and `/Game/Characters`
was not in `DirectoriesToAlwaysCook` — so with the art fully imported and sitting on disk, the
packaged game still shipped **zero** Mannequin packages. Every player rendered as a fallback
primitive under a red **`CHARACTER ART NOT INSTALLED`** banner whose text tells them to run
`./Scripts/import-mannequin.sh` — a shell script a playtester with a `.app` and no repository cannot
run.

Fixed by adding `+DirectoriesToAlwaysCook=(Path="/Game/Characters")` to `Config/DefaultGame.ini`.
Measured on the shipped `Trace-Mac.utoc` both ways: the needles `SKM_Manny_Simple`, `ABP_Unarmed` and
`SK_Mannequin` go 0 → 1 occurrence each, with `SM_SideRampConcave` present at 4 in both scans and a
deliberately absent needle at 0 in both, so neither result is an artefact of the scan. Cost: +14 MB,
703 → 717 MB.

**If you still see that banner in a build you made**, you did not run `Scripts/import-mannequin.sh`
before packaging. The art is gitignored, so a fresh clone has none of it.

### 8.5 Things about a packaged build that will confuse you

* **A Shipping build ignores a map or a server address on the command line.** That is the engine, not
  this project: `UGameInstance::StartGameInstance` blanks the command line in Shipping unless
  `UE_ALLOW_MAP_OVERRIDE_IN_SHIPPING` is defined
  (`Engine/Source/Runtime/Engine/Private/GameInstance.cpp:642`). Use the menu — which is what a
  playtester does anyway. A Development package honours both.
* **Shipping compiles logging out**, so a broken Shipping build is silent. Diagnose with
  `Scripts/package.sh -c Development`.
* **The Mac Shipping build needed a networking entitlement.** Unreal signs Shipping Mac builds with
  `Sandbox.NoNet.entitlements` by default (`BaseEngine.ini:3461-3462`), which grants neither
  `network.client` nor `network.server`, so the packaged game could not open a socket at all — it
  bounced off `?listen` and reported "PORT 7777 IS BUSY" while nothing at all held the port.
  Overridden in `Config/DefaultEngine.ini`. Do not remove that override.

---

## 9. Things that look like bugs and are not

Read this before reporting any of them. All are known, deliberate or accepted, and are recorded in
`docs/KNOWN_LIMITATIONS.md` with the item numbers below.

**In your first ten minutes:**

* **There is no spawn protection.** You can be shot the instant you respawn. There has never been a
  spawn shield in this game and it is not missing — the only invulnerability in Trace is the Core
  carrier's shield. (item 14)
* **Bodies vanish instead of falling over.** On death the character is hidden outright rather than
  ragdolled. Deliberate. (items 3, 15)
* **The practice range dummies look nothing like the players.** The five targets are Epic's smooth
  Mannequin while the ten playable characters are the newer blocky bodies. Deliberate in mechanism,
  odd-looking in effect. (item 33)
* **`ACTIVATED` is cut off on the character-select cards** — it reads `ACTIVAT` or `ACTIVATE`. It is a
  layout bug that only shows on 16:10 displays, which includes most MacBook screens, and never on a
  16:9 monitor. Cosmetic. (item 39)
* **On the settings and character-select screens the selection follows your mouse**, even when you are
  using the keyboard and have not moved the mouse — it just needs to be resting over the panel, and
  both panels are screen-centred. Move the pointer off to the side. (item 26)

**If somebody told you about it and you cannot find it:**

* **The surf rails are not in the map the game loads.** The mechanic exists and works; the four rails
  are only built by the procedural arena, and both PLAY and PRACTICE open the baked one. Nothing you
  can do in this build will find them. (item 30)
* **The endzone ruleset is gone.** There used to be a SCORING MODE row on the title screen that
  switched between full-width endzones and goals. Goals is the game now; the row, the `?mode=a` URL
  option and the setting behind them have all been removed, so there is nothing to look for.
* **Bots score sometimes.** They are meant to. How well they should defend is an open design question,
  not a defect.

**Cosmetic, already filed, please do not re-report:** the spent-magazine slot in the Canvas ammo
corner is at the wrong end and about 3.7× too wide (item 24); HUD panels other than the score bar
draw at 72% opacity and the world bleeds through them (item 23); a headless launch logs three engine
errors about `/Game/Generated/Materials`, which is harmless (item 42).

---

*Facts in this file were measured on 2026-09-04 against `Saved/Packaged/Mac/Trace-Mac-Shipping.app`
and the source tree of the same date, except where marked **UNTESTED** — which is §2 in its entirety
and `Scripts\package.bat` in §8.2. Neither has ever been run, because no Windows machine was
available to run them on.*
