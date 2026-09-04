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

A zip whose name starts with **`Trace-Mac`**. It expands to a folder holding two things:

* **`Trace-Mac-Shipping.app`** — the game.
* **`READ ME FIRST - Trace will not open until you do this.txt`** — the same instructions as below,
  shipped inside the zip so they cannot get separated from the app. Scripts/package-mac-dist.sh
  writes that file every time a build is packaged; nobody has to remember to attach it.

The zip is around **375 MB** and expands to **717 MB**.

### It will refuse to open the first time. This is normal, and here is exactly why.

Two separate facts, and it takes **both** of them to produce the failure:

1. **The app is ad-hoc signed.** `codesign -dvv` on it reports `Signature=adhoc` and
   `TeamIdentifier=not set`. That is a *real, valid* signature — `codesign --verify --deep --strict`
   passes — it simply has no certificate chain behind it, because nobody has paid Apple $99/year for
   a Developer ID. Gatekeeper therefore cannot attribute the app to anyone.
2. **macOS quarantines downloads.** A browser, Messages, AirDrop, Slack or Discord attaches the
   `com.apple.quarantine` extended attribute to whatever it hands you. The network does not do this;
   the *downloading application* does.

Quarantine **plus** no Developer ID means macOS refuses to launch it, usually with:

> *"Trace is damaged and can't be opened. You should move it to the Trash."*

**That message is a lie about the cause.** Nothing is damaged. macOS says "damaged" when it means
"I cannot tell who signed this". Do not delete it and ask for another copy — you will get the
identical result.

**This was measured, not inferred.** On macOS 26.5, with the real path a recipient takes — quarantine
set on the zip the way Safari sets it, expanded with Archive Utility by double-clicking, launched
with `open`:

| | CPU seconds the launch actually used, 20–25 s after `open` |
|---|---|
| Downloaded zip, expanded and double-clicked as-is | **0 s** — it never runs |
| Same zip with quarantine cleared first | **~30 s** — the game is up |

> **Why CPU time and not "did a process appear".** *Both* cases leave a process behind, so counting
> processes answers the wrong question. A refused launch is also **App-Translocated** — macOS copies
> the bundle to `/private/var/folders/…/AppTranslocation/<uuid>/d/` and starts it *there*, so it does
> not even appear under the folder you launched it from. That process then sits at 0.0 % CPU
> forever. A launch macOS allowed burns tens of CPU-seconds immediately, opening paks and compiling
> shaders. Four orders of magnitude apart; the process count does not separate them at all.

### *** THE TRAP: THE DOCUMENTED FIX STOPS WORKING THE MOMENT YOU DOUBLE-CLICK ***

This is the thing that was wrong with every previous version of this section, and it is almost
certainly why playtesters kept hitting the wall despite the remedy being written down.

**macOS locks an app bundle against modification once it has decided not to launch it.** So
`xattr -dr com.apple.quarantine <app>` — the universally-cited fix, and the one this file used to
lead with — works perfectly right up until the recipient double-clicks the app, and fails afterwards
with a wall of `Operation not permitted` on every file inside the bundle. `sudo` does not help; it is
not that kind of permission.

And of course *double-clicking is what people do first*. Nobody opens a README until something has
already failed. So the instruction was only valid in the state nobody is in by the time they read it.

**Measured, four for four, on macOS 26.5, same zip, same folder, same minute — the only variable is
whether the app had been double-clicked before the command was run:**

| Extraction | Double-clicked first? | `xattr -dr` result | Quarantined files left |
|---|---|---|---|
| `x2` | no | succeeds silently | **0** |
| `x3` | yes | `Operation not permitted` ×64 | **64** |
| `x4` | no | succeeds silently | **0** |
| `x5` | yes | `Operation not permitted` ×64 | **64** |

So there are three fixes, and **which one you need depends on what you have already done.**

### Fix 0 — do it to the ZIP, before anyone unzips (the one to tell people)

A zip is an ordinary file. None of the app-bundle protections apply to it, so this route cannot get
into the state above at all:

```
xattr -d com.apple.quarantine ~/Downloads/Trace-Mac-Shipping.zip
```

(no `r`). Then double-click the zip. Everything that comes out is already clean — **verified: 0
quarantined files in the extraction, and the app launches with no further step.** This is what the
`SEND-THIS-MESSAGE.txt` that `package.sh` writes tells people to do, because it is the only
instruction that is still correct after they have ignored it once.

### Fix 1 — unzipped, but not double-clicked yet

```
xattr -dr com.apple.quarantine ~/Downloads/Trace-Mac/Trace-Mac-Shipping.app
```

Note the `r`: Archive Utility puts the flag on all 64 files inside the bundle, not just the top one.

### Fix 2 — already double-clicked, already saw "damaged" (most people)

Do not fight the lock; step around it. Copy the bundle without its extended attributes, which needs
no permission on the original because it only *reads* it:

```
ditto --noextattr --noqtn ~/Downloads/Trace-Mac/Trace-Mac-Shipping.app ~/Trace.app
```

Then open `~/Trace.app`. **Verified end to end on the shipped artefact, in this order:** the blocked
copy burns **0 s of CPU**, `xattr -dr` on it fails with `Operation not permitted` and leaves all 64
flags in place, and the `ditto` copy has no xattrs, still passes `codesign --verify --deep --strict`,
and **runs (35 s of CPU in 25 s of wall clock)**.

The `Unlock Trace (double-click me).command` shipped inside the zip does exactly this decision: it
tries Fix 1, checks whether any quarantine flag actually survived (rather than trusting `xattr`'s
exit status, which is not reliable here), and falls back to Fix 2 automatically, telling the player
where the working copy is.

### Better still: don't let it get quarantined at all

Quarantine comes from the downloader, so a transfer that is not a download never acquires it. Both of
these were measured to produce a file with **no** `com.apple.quarantine` attribute at all:

```
curl -L -o ~/Downloads/Trace-Mac-Shipping.zip "<the link>"
scp  someone@100.x.x.x:Trace-Mac-Shipping.zip ~/Downloads/
```

If the group is already on Tailscale to play, `scp` over the tailnet skips this entire section.

### What about right-click → Open?

It used to be the no-Terminal answer and it is no longer reliable. On macOS 15 and newer, Apple
removed the right-click → Open bypass for apps in this state; for an app that gets the "damaged"
dialog there is often no **Open** button offered at all, and no **Open Anyway** in
**System Settings → Privacy & Security** either, because that path exists for apps that are *signed
by an identifiable developer and not notarized* — which this one is not. Try it if you like; if you
do not get an **Open** button within two clicks, use the Terminal command above. It always works.

### What an Apple Developer account would change

$99/year buys a *Developer ID Application* certificate. With one, the build would be signed with the
hardened runtime, submitted to `xcrun notarytool`, and stapled — after which it opens on a
double-click, on any Mac, with no instructions and no Terminal, and this whole section disappears.
That is the only thing that removes the problem rather than working around it. Nothing above is a
substitute for it; it is the best that is reachable without it.

### Two things that will stop it dead, and are worth checking before you download 717 MB

* **Apple Silicon only.** The binary is `arm64` and there is no Intel version. An Intel Mac cannot run
  it at all, and Rosetta does not help — Rosetta translates Intel code for Apple Silicon, not the
  other way round. Any Mac from roughly 2020 or earlier is out. Check with  → About This Mac; you
  need "Apple M1" or later, not "Intel".
  *If someone in the group has an Intel Mac,* a universal build is one flag —
  `Scripts/package.sh -- -architecture=arm64+x86_64` — at the cost of roughly double the build time
  and a noticeably bigger download. Nothing else about the project changes.
* **macOS 14 (Sonoma) or newer.** Older macOS refuses to launch it.

### Before you connect: check the two builds match

Bottom right of the title screen, every build prints a version and an eight-character code:

```
V 0.1.0   NET 51920028
```

**Everyone in the session must see the same `NET` code.** Windows players see it in the same corner.
Two machines with different codes cannot connect — the engine refuses the handshake — and the error
message blames the connection rather than the version. Comparing the two corners takes five seconds
and saves an evening. §7 explains what the code is made of and how to compare two
checkouts without launching anything.

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
`~/Library/Containers/io.github.lzbrv.trace/Data/Library/Application Support/Epic/Trace/Saved/Config/Mac/GameUserSettings.ini`
and two crash-reporter config files, so the app was certainly writing to that container. It just
never wrote a log into it.

So **do not go hunting** in
`~/Library/Containers/io.github.lzbrv.trace/Data/Library/Logs/Trace/`. If a `Trace.log` is sitting
there it is stale, from some earlier build, and sending it will mislead whoever reads it — check its
timestamp before you send anything.

A log that is useful for diagnosis has to come from a **Development** build, which whoever sent you
the game can produce with one flag (§8.1). Ask them for one; do not try to extract it from this
build.

### The `NET` code — the five-second check that replaces an evening

Bottom right of every title screen, in every build on every platform:

```
V 0.1.0   NET 51920028
```

**If two machines show different `NET` codes they cannot connect.** The engine refuses the handshake
during connect, and the message a player gets is about the *connection*, not the version — which is
why this is worth checking first rather than last.

**What the code is.** Unreal normally derives its network compatibility value from a mix of the
project name, the project version, and *the engine install's own changelist* — and that last term
comes out of `Engine/Build/Build.version` on the machine that built it, not out of this repository.
Two people on "UE 5.8" whose installs differ by a hotfix therefore compute different values from
identical source, and the failure looks like a networking problem. This project does not rely on
that. It pins the value to two things that both live in the repository:

* `NetProtocolVersion` in `Source/Trace/UI/TraceNetworking.h`
* `ProjectVersion` in `Config/DefaultGame.ini`

so any two builds of the same commit agree by construction, on any platform, on any 5.8 install. The
trade-off is stated where it is made: this no longer refuses a connection between two *different
engine versions*, so `NetProtocolVersion` must be bumped by hand when the engine moves.

**Three ways to read it, in order of who can use them:**

| Who | How |
|---|---|
| A player, any build including Shipping | Title screen, bottom right |
| Anyone with the repository, no build needed | `python3 Scripts/netversion.py` |
| A developer with a console (Development builds) | `Trace.NetVersion` |

`Scripts/netversion.py` computes the value from the source files in about a second, with no engine
and no build, and prints the string it hashed as well as the code:

```
NET 51920028
  from : "trace netproto 1, project 0.1.0"
```

Run it on the Mac and on the Windows machine before a session. Identical output means the two builds
will agree. Different output means the two *checkouts* differ, and rebuilding will not help — pull.

`Scripts/netversion.py --verify <log>` checks the script against a Development run's own log, so the
script is tested against the binary that actually performs the handshake rather than trusted.

---

---

## 8. For whoever is making the build

Everything above this line is for a player. Everything below needs the repository.

### 8.1 macOS

```
Scripts/package.sh
```

That is the whole command. It cooks content and produces **two** things:

```
Saved/Packaged/Mac/Trace-Mac-Shipping.app        717 MB   the bundle
Saved/Packaged/Mac/Trace-Mac-Shipping.zip        385 MB   THE THING YOU SEND
```

**Send the zip. Never send the `.app` on its own.** The zip holds the app *and* a README named
`READ ME FIRST - Trace will not open until you do this.txt`, and the README is the half that makes
the app openable on somebody else's Mac (§1). This used to be a warning printed at the end of a
forty-minute cook, and the owner's playtesters still hit the wall — because a warning is read once
and the artefact it warned about is sent, on its own, weeks later. `Scripts/package-mac-dist.sh`
now builds the pair, and `package.sh` calls it, so the sendable artefact exists by construction
rather than by anyone remembering to assemble it.

Before it writes the zip, that script gates the four things that make a bundle fail on somebody
else's machine in a way that does not look like its own cause, and **refuses to build a
distributable if any of them fail**:

| Gate | Why it exists |
|---|---|
| Code signature verifies | A *broken* signature produces the same "damaged" dialog as quarantine and has a completely different fix. The two must be told apart before the artefact leaves. |
| Sandbox has `network.client` + `network.server` | Shipping's default Mac entitlements have **no network at all**; the game then runs, reports `COULD NOT LISTEN ON UDP 7777`, and is unjoinable with nothing on the port. See the block in `Config/DefaultEngine.ini`. |
| `CFBundleIdentifier` is not `com.YourCompany.*` | Epic's placeholder. Every un-renamed UE project ships as `com.YourCompany.<Project>`, so macOS cannot tell this game from any other. |
| Cooked content present | A bundle with none links, packages, reports success and exits instantly at launch (`KNOWN_LIMITATIONS.md` item 29). |

It then **expands its own zip and launches what comes out**, so "it packaged" and "it runs" are not
the same claim taken on trust. `--prove-quarantine` adds the other arm: it quarantines a copy exactly
the way a download does and shows that copy failing to start. That arm is off by default only because
it deliberately trips Gatekeeper and may put a dialog on your screen.

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

**Do not zip it by hand.** `package.sh` already made the zip, with the README inside it and with
`ditto --sequesterRsrc`, which is what keeps the code signature intact through the round trip. A
hand-rolled `zip -r` can break the signature, and a broken signature produces the *same* "damaged"
dialog as quarantine while being immune to the quarantine fix — the worst possible failure, because
the recipient follows correct instructions and still cannot open it.

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

The bundle identifier **is no longer Epic's placeholder**. It was `com.YourCompany.Trace` until
2026-09-04 — the name every un-renamed UE project ships under, which meant macOS could not tell this
game from any other unshipped UE project, and any Gatekeeper approval or sandbox container was keyed
to a name shared with all of them. It is now `io.github.lzbrv.trace`, set by `BundleIdentifier` under
`[/Script/MacTargetPlatform.XcodeProjectSettings]` in `Config/DefaultEngine.ini`, and it is the
reason the sandbox container path in §7 has that name.

`io.github.lzbrv` is reverse-DNS for a name the owner actually controls (the GitHub account this
repository lives under), which is the whole point of reverse-DNS and why it is not `com.trace`.
Anyone who ran a build from before that date will get a fresh container; for a prototype with no
saved data that is invisible.

`Scripts/package-mac-dist.sh` refuses to build a distributable from a bundle whose identifier is back
on the placeholder, so this cannot silently regress.

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
