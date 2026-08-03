# Design — rules, tunables, class map

Everything about how Trace plays, and how to change it without touching C++.

New to the Unreal Editor, or wondering why the arena viewport is empty? Read
[EDITOR.md](EDITOR.md) first — it covers the editor panels, Play-In-Editor, and where these
tunables appear in Project Settings.

---

## 1. The rules

Authoritative statement of the game. If code and this section disagree, the code is wrong.

### Structure
- **5 v 5.** Teams are `Blue` (cyan) and `Orange`.
- **One shared Core.** Both teams contest the same object — this is not flag-per-team.
- A team scores by getting the Core into the **opposing** team's endzone.
- **Each endzone spans the entire width of the field.** There is no way to run around one. The
  endzone is a band across the whole arena, `EndzoneDepth` deep, from sideline to sideline — so
  defending is about depth and timing, never about funnelling the carrier into a narrow mouth.
- **A completed pass scores.** If a teammate is already standing in the enemy endzone and you pass
  them the Core, the point is awarded the moment they take possession. You do not have to *carry* it
  across the line — the alley-oop is a legitimate and intended way to score.
- First to `ScoreToWin` (default **5**), or highest score when `MatchDuration` (default **10 min**)
  expires. Sides switch at half time (`ATraceGameState::GetDefendedEndSign()` is the authority on
  which end a team is defending; the arena repaints itself to match).

### The carrier
- **Invulnerable to bullets.** Absolutely, not partially. No hitscan can hurt them.
- **Cannot shoot.** The gun is unavailable while carrying.
- **Lays a trail** continuously behind them while carrying.
- Moves slightly faster than everyone else (`CarrierSpeedMultiplier`, default 1.08).

### The trail
- An **enemy of the carrier**, **while dashing**, passing through the trail → **the carrier dies**.
- **Walking or running through it does nothing at all.** No damage, no slow, no effect.
- **Teammates never trip it.**
- **Dash is the only counterplay to a carrier.** There is no other way to stop one.
- **A trail that exists is a trail that can be cut.** Once points have been laid they stay lethal to
  the current carrier's enemies for `TrailLifetime`, and a turnover does not grant anyone immunity:
  after the Core changes hands, the new carrier's enemies can cut the new trail immediately, from
  its first point (subject only to `TrailHeadGracePoints`). "I dashed through it and nothing
  happened" is a bug, never a rule.
- On carrier death: the Core **drops at the death location** and the trail is **cleared instantly**.

### Everyone else
- **Hitscan** weapon — instant, no travel time, `SpreadDegrees` of cone.
- **Positional damage**: head `100` (an instant kill), body `40`, legs `25`. Damage lives in
  `UTraceDamageSettings`, not in `UTraceSettings`.
- **No friendly fire.**
- Bullets never damage the carrier.
- **Everyone** — carrier included — has a **dash** on a short cooldown, a **slide** and a **boost**.

### Movement, view and feedback
- **The slide preserves momentum.** Crouching above `SlideEntrySpeedFraction × WalkSpeed` converts
  the speed you already have into a slide rather than resetting it: entry speed is a multiple of
  your *current* planar speed (not a fixed value), and it decays from there. Sliding out of a dash
  therefore carries the dash's speed into the slide instead of throwing it away. It also lasts
  meaningfully longer than a tap — long enough to be a traversal choice rather than a twitch.
- **The crosshair is drawn in both first and third person.** It used to be hidden while carrying,
  on the reasoning that a carrier has no gun and the camera pulls back. That was wrong in practice:
  the reticle is also the aim indicator for a *pass*, which is the carrier's only offensive action,
  and a screen with no centre marker reads as a broken HUD.
- **A dead player's model disappears immediately.** No corpse lingers on the field. A body that
  stays visible after death is read as a live enemy, which is the worst possible false signal in a
  game where the wrong dash costs you the point.

### The Core
- Can be thrown/passed in the aim direction.
- **Anyone may catch or pick it up — teammate or enemy.** Interception is intended.
- The thrower cannot re-catch their own throw for `PickupLockoutAfterThrow`.
- If left loose and untouched for `CoreResetTime`, it returns to the centre pedestal.

### Why the rules are shaped this way

The invulnerable carrier removes aim from the most important moment in the game and replaces it
with movement and geometry. You cannot delete a carrier with damage, so a carrier run becomes a
spatial problem: they are drawing a line, and the defence has to find a place and a moment where
cutting that line is possible. Because dash is cooldown-gated, defenders can be baited into
spending it, and the carrier's job is partly to make defenders dash at nothing.

The trail's grace period at the head (`TrailHeadGracePoints`) is what stops this collapsing into
"dash at the carrier's back." You have to cut where they *were*, which means predicting or
cornering them, not chasing.

No friendly fire plus "anyone can catch the Core" makes escorting real: a teammate's body is a
legitimate wall, and a bad pass is a turnover rather than a free reset.

---

## 2. Tunables — `UTraceSettings`

Every number in the game lives in one place: `Source/Trace/TraceSettings.h`, exposed as a
`UDeveloperSettings` subclass. Read it in code with `UTraceSettings::Get()`.

### Where the values live

The class is declared `UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Trace Gameplay"))`.

That `config = Game` is what determines the file: **the values live in
`Config/DefaultGame.ini`**, under the section

```ini
[/Script/Trace.TraceSettings]
```

The section name is `[/Script/<ModuleName>.<ClassNameWithoutTheUPrefix>]`. This is a common
first-week trip-up in Unreal: **put the block in `DefaultEngine.ini` and it is silently ignored** —
no warning, no log line, the defaults just quietly apply and you spend an hour wondering why your
change did nothing. `DefaultEngine.ini` is for engine-level configuration (default map, net driver,
renderer settings); gameplay tunables belong in `DefaultGame.ini`.

Every key in the tables below is already written out explicitly in `Config/DefaultGame.ini`, with
its default value, rather than being left to fall back to the C++ defaults. That is deliberate: it
means the whole game's tuning is one readable, reviewable, diffable text file, and a tuning change
shows up in a PR as a one-line diff instead of as an invisible absence. **Where the ini and the C++
defaults in `TraceSettings.h` disagree, the ini wins at runtime.**

### Tuning without recompiling

Three ways, in order of convenience:

**a) Project Settings UI** — Edit → Project Settings → **Game** → **Trace Gameplay** (damage is on
the neighbouring **Trace Damage Zones** page). Every knob below appears there under its category,
and changes are written straight to `Config/DefaultGame.ini`.

**These apply live while Play-In-Editor is running.** Drag a slider mid-match and the change takes
effect without stopping and restarting PIE — which is the entire point of keeping every number in
one `UDeveloperSettings` object and reading it through `UTraceSettings::Get()` at the point of use
rather than caching it in a constructor. This is the right way to iterate; see
[EDITOR.md §6](EDITOR.md#6-project-settings--game--trace-gameplay).

**b) Edit the ini directly:**

```ini
; Config/DefaultGame.ini
[/Script/Trace.TraceSettings]
ScoreToWin=3
MatchDuration=300.000000
DashCooldown=2.000000
TrailLifetime=8.000000
bRequireDashToTripTrail=True
```

Booleans are `True`/`False`. Floats want a decimal point. Editing the file on disk requires an
editor restart to take effect.

**c) Command line, for a one-off test without dirtying the repo:**

```bash
"$UE_EDITOR" "$PWD/Trace.uproject" /Game/Maps/Arena?listen -game -log \
    -ini:Game:[/Script/Trace.TraceSettings]:DashCooldown=1.0 \
    -ini:Game:[/Script/Trace.TraceSettings]:TrailLifetime=10.0
```

Excellent for A/B-testing a feel change across two server instances.

> ### The one rule about config and multiplayer
>
> **Everyone must be running the same `DefaultGame.ini`.** Movement values in particular —
> `WalkSpeed`, `DashSpeed`, `DashDuration`, `DashCooldown` — are used by **client-side prediction**.
> If a client's config disagrees with the server's, the client predicts one thing, the server
> computes another, and you get a correction on **every single dash**. It will look like a netcode
> bug. It is a config drift bug.
>
> So: config changes are **committed to `main`** like code, and everybody pulls. Do not tune
> gameplay values locally-only and then join someone else's server.

### Match

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `ScoreToWin` | int32 | `5` | Points to win. **Drop to 2–3 for playtests** so you get several full match resets per session. |
| `MatchDuration` | float | `600` | Match length in seconds. On expiry, highest score wins. 300 for quick tests. |
| `PlayersPerTeam` | int32 | `5` | Team cap used when auto-balancing joiners. Set to 2 if you're testing 2v2. |
| `MinPlayersToStart` | int32 | `2` | Below this, the match stays in `WaitingForPlayers`. **Set to 1 if you want to run around solo** while iterating. |
| `RespawnDelay` | float | `3` | Seconds dead before respawn. The dead player's model is removed **immediately** on death — this delay is the wait, not a corpse timer. **The biggest pacing lever in the game.** Short = constant pressure and no space for a carrier run; long = deaths matter and a broken defence stays broken long enough to score through. |
| `WarmupDuration` | float | `5` | Countdown before the match goes `InProgress`. Set to 0 to skip while testing. |

### Combat

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `MaxHealth` | float | `100` | Health on spawn. |
| `HitscanRange` | float | `28000` | Max trace distance in Unreal units (1 uu = 1 cm). The field is 24000 uu long, so this reaches end to end. **Lower it to ~6000 to force close-range engagements** and make the arena feel bigger. |
| `FireInterval` | float | `0.16` | Seconds between shots — 6.25 shots/sec. **Was `0.12`; the rate was cut by 25% so that a duel is decided by two or three deliberate shots rather than by who holds the trigger longest.** Minimum body TTK is 2 × 0.16 = **0.32 s**. The server validates fire rate against this with a small tolerance. |
| `bFriendlyFire` | bool | `false` | Off. Also controls whether the lag-comp resolver skips teammates. Turning it on materially changes escorting — probably keep it off. |
| `SpreadDegrees` | float | `0` | Cone half-angle. Zero: the gun is exact, so a miss is yours. Raise it if long-range duels start feeling deterministic in a bad way — but keep it at 0 whenever you are debugging hit registration, because you want no randomness while checking rewind. |

Damage is **positional** and lives on a separate page, `UTraceDamageSettings`
(`Source/Trace/Gameplay/TraceHitZones.h`, Project Settings → Game → **Trace Damage Zones**,
`[/Script/Trace.TraceDamageSettings]`):

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `HeadDamage` | float | `100` | **An instant kill.** One clean head hit ends a non-carrier, which is what makes aim worth having when the fire rate is deliberately slow. |
| `BodyDamage` | float | `40` | Three body shots kill (120 ≥ 100). With `FireInterval` at 0.16 that is a 0.32 s minimum body TTK. |
| `LegDamage` | float | `25` | Four leg shots kill. Legs are the cheap hit on a moving target, so they pay the least. |

### Movement

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `WalkSpeed` | float | `820` | Base ground speed (Unreal's default character is 600). **Was `720`.** The field is 24000 uu end to end; at 720 a full-field carry was a long jog with nothing happening in the middle of it. |
| `CarrierSpeedMultiplier` | float | `1.08` | Carrier speed = `WalkSpeed × this` = 885.6. **>1 means the carrier outruns chasers, deliberately** — you are not supposed to catch them, you are supposed to cut them. This is the single strongest lever on carrier power; nudge it in 0.02 steps, not 0.2. |
| `DashSpeed` | float | `3000` | Speed during the dash. **Was `2600`**, raised with `WalkSpeed` so the dash stays a distinct burst rather than a nudge above running. |
| `DashDuration` | float | `0.18` | Dash length in seconds. **Dash distance = `DashSpeed × DashDuration` = 540 uu.** Change either and the distance moves — think in distance, then pick the pair. |
| `DashCooldown` | float | `3.5` | **The most important number in the game.** Dash is the only counterplay to a carrier, so this directly sets how often a defence gets a chance. **Was `4`**: with the trail also shortened to 2.8 s there is less line to cut, so defenders get their attempt back sooner. Tune this before anything else. |
| `BaseDashCharges` | int32 | `1` | Dash charges everyone has. The pool refills one charge at a time, each on `DashCooldown`. |
| `CarrierExtraDashCharges` | int32 | `1` | Extra charges granted **while carrying**. The carrier can dash twice; that is the compensation for the cooldown being long enough to matter for defenders. |

Dash locks its direction at activation (last input direction, else actor forward) and cannot be
steered mid-dash. That is intentional — a steerable dash is both a prediction nightmare and removes
the commitment that makes trail-cutting a real decision.

### Slide

Crouch while moving. **A slide spends momentum you already have** — it starts from your *current*
speed rather than from a fixed number, so sliding out of a dash carries the dash's speed with it.
It never resizes the capsule: the capsule is the single source of truth for hit resolution, lag
compensation and the trail trip test.

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `SlideEntrySpeedFraction` | float | `0.55` | Fraction of `WalkSpeed` you must already be moving at before crouch will start a slide. Stops "tap crouch from a standstill" being free speed. |
| `SlideSpeedMultiplier` | float | `1.35` | Entry speed multiplier applied to `max(current planar speed, WalkSpeed)`. **The `current planar speed` term is the momentum preservation** — a fast entry stays fast. Trimmed from `1.45` because the speed now survives the whole slide instead of decaying, so the instant kick needs to be smaller. |
| `SlideMaxSpeed` | float | `1900` | Hard ceiling on entry speed, so slide-out-of-dash cannot compound into something unbounded. Raised from `1500`, where a slide out of a 3000 uu/s dash was a speed **loss** — the opposite of preserving momentum. |
| `SlideDuration` | float | `1.8` | Longest a slide may last even if it has not decayed to the exit speed. Lengthened from `1.0` so a slide is a traversal decision, not a twitch. Measured mean slide: 0.67 s / 526 uu before, **1.44 s / 1301 uu after**. |
| `SlideDeceleration` | float | `260` | uu/s bled off every second. **This is the momentum dial.** Lowered from `750`, which stripped more speed per second than a slide could carry and ended most slides early on the exit-speed check — that is what made the slide read as a brake. At `260` a 1.8 s slide sheds only ~470 uu/s in total. |
| `SlideExitSpeedFraction` | float | `0.5` | Fraction of `WalkSpeed` at which a decaying slide gives up and hands the player back. Lowered from `0.6` so the slide runs to its duration rather than bailing early. |
| `SlideTurnRateDegrees` | float | `130` | Degrees/second a slide may be steered. `0` would make a slide a rail. |
| `SlideCooldown` | float | `2.4` | Measured from slide **start**, so chained slides cannot sustain speed above walking indefinitely. Raised from `0.6`: against a 1.8 s slide, a 0.6 s cooldown measured from the start is *no cooldown at all* and slides chain end to end forever. |
| `SlideInputBufferSeconds` | float | `0.25` | How long a crouch press that could not slide yet (mid-dash, airborne) stays queued. This is what makes "dash, then slide out of it" and "press crouch just before you land" work. Only a fresh press charges it, so holding the key cannot chain slides. |
| `SlideMinCommitSeconds` | float | `0.55` | Seconds at the start of a slide during which releasing crouch will **not** cancel it. Without this a slide is only as long as you hold the key, so lengthening `SlideDuration` changes nothing for anyone who taps crouch. A dash still overrides the commit window. |
| `SlideExitSpeedRetention` | float | `1.0` | Fraction of the slide's **live** speed carried into normal movement on exit. This and the two rows below are the actual momentum contract: the old exit only ever clamped *down*, so a slide handed you back at 80% of a run and made you re-accelerate. |
| `SlideExitMinSpeedFraction` | float | `1.0` | Floor on the exit speed as a fraction of `WalkSpeed` — a slide can never end slower than a run. |
| `SlideExitMaxSpeedMultiplier` | float | `1.0` | Ceiling on the exit speed as a multiple of max speed. **Keep at 1.0.** Above 1, `CalcVelocity`'s input branch clamps to the *current* speed once it exceeds max, so any overspeed handed back is kept for as long as a movement key is held and slide-cancel spam becomes the fastest way to cross the arena. |

### Boost

A ground-only vertical launch on its own bind, separate from jump.

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `BoostZVelocity` | float | `813` | Upward velocity granted by a boost. **Was `1150`.** Apex scales with `v²/2g`, so this is the value that halves the height: against the default 980 gravity, 1150 reached ~675 uu and 813 reaches ~337 uu. (Halving the *velocity* would have quartered the height, which is not the same change.) |
| `BoostCooldown` | float | `12` | Long on purpose — boost is a commitment, not general mobility. |

### Core

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `PassSpeed` | float | `2400` | Launch speed of a thrown Core. |
| `PassUpwardBias` | float | `0.14` | Upward component added to the throw direction, giving passes a slight arc so they clear heads instead of hitting the first body in front of you. Raise for floatier, more interceptable passes. |
| `PickupRadius` | float | `110` | How close you must be to a loose Core to grab it. Generous on purpose — fumbling a pickup is not interesting. |
| `PickupLockoutAfterThrow` | float | `0.35` | Seconds the **thrower** cannot re-catch their own throw. Stops self-pass cheese (throw forward, run into it, repeat). |
| `CoreResetTime` | float | `15` | Seconds a loose, untouched Core waits before returning to the centre pedestal. Prevents stalemates where the Core sits in a corner. |

### Trail

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `TrailLethality` | enum | `KillsCarrier` | What happens when the trail is tripped. `KillsCarrier` is the real game. `KillsToucher` and `KillsBoth` exist as **experiments** — `KillsBoth` in particular turns the trail into a mutual-destruction trade and is worth trying once. |
| `bOnlyEnemiesTripTrail` | bool | `true` | Teammates pass through freely. Setting false makes escorting suicidal and is almost certainly bad — but it's one flag if you want to see. |
| `bRequireDashToTripTrail` | bool | `true` | **The core rule.** False turns Trace into a completely different game (any contact kills the carrier), where carriers die instantly and the mode collapses. Useful as an A/B to demonstrate why the dash requirement exists. |
| `TrailLifetime` | float | `2.8` | Seconds each trail point survives. **The lever on how much history is exposed.** Longer trail = more surface area for defenders to cut = weaker carrier. **Was `4`.** At the current carrier speed (885.6 uu/s) 2.8 s is ~2,480 uu of trail — about a tenth of the field, so a carrier's exposure is now local to where they are rather than a line halfway across the arena. Pair every change here with `DashCooldown`: those two numbers between them decide whether carriers live. |
| `TrailPointSpacing` | float | `60` | Distance the carrier must move before a new point is recorded. Lower = smoother trail and more accurate trip tests, at the cost of more points and more bandwidth. Don't go below ~30 without watching `stat net`. |
| `TrailRadius` | float | `45` | Radius of each trail segment, for both the trip test and the visual. Slightly narrower than a character capsule (34 uu), so cutting requires reasonable precision. |
| `TrailHeight` | float | `190` | Height of the trail volume, roughly a character's height. Prevents jumping cleanly over it. |
| `MaxTrailPoints` | int32 | `256` | Hard cap on recorded points. At the defaults, `TrailLifetime` expires points long before this binds (~78 points), so the cap is a safety net against a config that would flood the network. **If you raise `TrailLifetime` a lot, check whether this cap starts binding instead.** |
| `TrailHeadGracePoints` | int32 | `3` | The newest N segments cannot be tripped. **This is what stops "dash at the carrier's back" from being the whole game** — you must cut where they *were*, not where they are. Raise it if point-blank trail kills feel cheap; lower it if carriers feel untouchable. |

### Net

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `bEnableLagCompensation` | bool | `true` | Master switch for server-side rewind. Turn it **off** to feel exactly how bad hitscan is without it — genuinely worth doing once. |
| `MaxRewindTime` | float | `0.25` | Maximum seconds the server will rewind a shot. **This is a security boundary, not a feel knob** — it bounds how much of the past a client can claim. Higher favours high-ping shooters; lower favours the target's right to have reached cover. 0.25 s is a normal shipping value. |
| `LagCompHistoryDuration` | float | `1` | How much pose history the server retains per character. **Must comfortably exceed `MaxRewindTime`** — keep at least 2–4× headroom, or rewind requests fall off the end of the buffer and silently stop compensating. |
| `bDrawServerRewindDebug` | bool | `false` | Draws the historical capsules the server tested a shot against. Invaluable when hit registration feels wrong; very noisy. Off in normal play. |

Full networking explanation in [NETWORKING.md](NETWORKING.md#5-what-trace-already-does-for-netcode).

---

## 3. Class map

Everything lives in `Source/Trace/`. There are no Blueprints and no authored assets.

### Foundation

| Type | File | Role |
|---|---|---|
| `LogTraceGame` | `Trace.h` / `Trace.cpp` | The project's log category. `UE_LOG(LogTraceGame, ...)`. **Not `LogTrace`** — that name is taken by the engine's own Unreal Insights category (the one that prints "Initializing trace…" / "Control listening on port 33269"), and colliding with it makes our output impossible to filter. Filter ours with e.g. `-LogCmds="LogTraceGame Verbose"`. |
| enums + `FTraceTrailPoint(Array)` + `FTraceLagCompFrame` | `TraceTypes.h` | Shared vocabulary: `ETraceTeam`, `ECoreState`, `ETrailLethality`, `ETraceMatchState`, team colours, the fast-array trail structs, and the pose-history frame. Everything depends on this header. |
| `UTraceSettings` | `TraceSettings.{h,cpp}` | Every number in §2 except damage. `UTraceSettings::Get()`. Project Settings → Game → **Trace Gameplay**. |
| `UTraceDamageSettings` + `ETraceHitZone` | `Gameplay/TraceHitZones.{h,cpp}` | Positional damage: head 100 / body 40 / legs 25, and the hit-zone resolution that picks between them. Project Settings → Game → **Trace Damage Zones**. Replaced the old flat `HitscanDamage` / `HeadshotMultiplier` pair, which no longer exist. |
| — | `Config/DefaultEngine.ini` | Default map, framework classes, net driver rates, renderer. Nanite/Lumen/VSM are **off** on purpose: the whole game is `BasicShapes` meshes, and SM6 is M2-or-newer on Apple Silicon. |
| — | `Config/DefaultInput.ini` | Points `DefaultPlayerInputClass` / `DefaultInputComponentClass` at the Enhanced Input classes. That is the *only* thing needed to enable Enhanced Input — the actions and mapping context are all built in C++. |
| — | `Config/DefaultGame.ini` | Every tunable in §2, written out explicitly, plus `MaxPlayers=10` and the packaging rule that force-cooks `/Engine/BasicShapes`. |

### Match flow — `Core/`

| Class | Base | Role |
|---|---|---|
| `ATraceGameMode` | `AGameModeBase` | **Server only, never exists on clients.** Spawns the arena and the Core, assigns teams on join, drives match state, schedules respawns, handles scoring and match end. Death causes are tagged `"Bullet"`, `"Trail"`, `"Fell"`. |
| `ATraceGameState` | `AGameStateBase` | Replicated match state everyone can see: both scores, `ETraceMatchState`, match end time, and the Core reference. Also the **shared clock** via the inherited `GetServerWorldTimeSeconds()`. |
| `ATracePlayerState` | `APlayerState` | Per-player replicated state: team, kills, deaths, carrier flag. Survives seamless travel via `CopyProperties`. |
| `ATracePlayerController` | `APlayerController` | Owns input. **Enhanced Input is constructed entirely in C++** — the `UInputMappingContext` and every `UInputAction` are `NewObject`'d at runtime, so there are no input assets to open and remapping is a code change. Routes input to the pawn's `Do*` methods; receives hit/death notifications from the server. |

### The pawn — `Core/`, `Movement/`, `Gameplay/`

| Class | Base | Role |
|---|---|---|
| `ATraceCharacter` | `ACharacter` | The player. Capsule is the **only** collider; body (cylinder), head (sphere) and nose (cone) are `NoCollision` visual meshes with team-coloured dynamic materials. Owns the health, weapon, trail and lag-comp components. |
| `UTraceCharacterMovementComponent` | `UCharacterMovementComponent` | **Client-predicted dash.** `FSavedMove_Trace` saves the dash intent, timer and cooldown; the intent crosses the wire as one bit of the compressed-flags byte. See [NETWORKING.md](NETWORKING.md#client-side-prediction-of-movement-and-dash). |
| `UTraceHealthComponent` | `UActorComponent` | Health, death, and **invulnerability**. `ApplyDamage()` early-outs while the owner is the carrier; `Kill()` deliberately bypasses that — which is how trail deaths work at all. |
| `UTraceWeaponComponent` | `UActorComponent` | Hitscan firing. Client predicts the tracer, `ServerFire` (quantised payload, timestamped) does the authoritative resolution. Refuses to fire while carrying, dead, or on cooldown. |

### Netcode — `Net/`, `Gameplay/`

| Class | Base | Role |
|---|---|---|
| `UTraceLagCompensationComponent` | `UActorComponent` | Records a capsule pose per server tick; `GetPoseAtTime()` interpolates. The static `ResolveHitscan()` does the rewound trace. **No actor is ever moved** — it is pure segment-vs-capsule maths against historical poses. |
| `ATraceTracer` | `AActor` | Cosmetic bullet trail: a stretched cylinder, team-tinted, self-destructs after ~0.08 s. |
| `UTraceTrailComponent` | `USceneComponent` | The trail. Server appends/expires points and runs the trip test each tick; clients rebuild pooled cylinder meshes from the delta-replicated `FTraceTrailPointArray`. |

### World and objectives — `Gameplay/`, `World/`

| Class | Base | Role |
|---|---|---|
| `ATraceCore` | `AActor` | The objective. Sphere collision + sphere mesh + projectile movement (disabled while carried). Handles pickup, throw, drop and reset; drives the carrier flag on the character, the player state and the trail component. |
| `ATraceEndzone` | `AActor` | Box trigger with an `OwningTeam`, spanning the **full width** of the field. Server only. Scores on overlap *and* on a 10 Hz geometric poll of the current carrier's position — the poll is what makes a **pass completed to a teammate already inside the zone** score, since no overlap event fires for someone who is already standing there. `ScoresHere()` holds the one direction rule: `Team == TraceOpposingTeam(OwningTeam)`. |
| `ATraceArenaBuilder` | `AActor` | **Builds the entire playfield in C++ at `BeginPlay`** — a 24000 × 12000 uu floor, four 2600 uu walls, a stepped centre dais and Core pedestal, wing platforms, lane rails, cover, flank structures, two full-width endzones with team-tinted patches and lit gates, 5 player starts per team, the lighting rig, fog and post-process. Repaints both ends at half time via `ApplyTeamSides()`. This is why `/Game/Maps/Arena` can be empty — see [EDITOR.md](EDITOR.md). |
| `ATraceTeamPlayerStart` | `APlayerStart` | A `APlayerStart` that knows its team, so `ChoosePlayerStart` spawns you on your own side. |

### HUD — `UI/`

| Class | Base | Role |
|---|---|---|
| `ATraceHUD` | `AHUD` | **Pure Canvas, zero UMG.** Everything is drawn in `DrawHUD()`: crosshair (**drawn in both first and third person**, i.e. while carrying too — it is the pass aim indicator), health bar, both scores, match clock, Core status banner, dash cooldown pip, respawn countdown, hit marker, and the Tab scoreboard with K/D and ping. No widget assets, so the HUD is fully diffable and reviewable in a PR. |

---

## 4. Where to add things next

The prototype is deliberately unfinished in specific places. Roughly in order of value:

**Tune before you build.** The most valuable next hour is not code: run a 4-person playtest and
move `DashCooldown`, `CarrierSpeedMultiplier`, `TrailLifetime` and `TrailHeadGracePoints`. Those
four numbers *are* the game. Everything else is decoration until they are right.

**A second ability, if dash proves too binary.** The current design has exactly one interaction
verb against a carrier. If playtests show carriers are either uncatchable or free, the fix might be
a second tool rather than more tuning — a short wall, a slow field, a thrown blocker. Add it as a
component on `ATraceCharacter` alongside the weapon, and predict it the same way the dash is
predicted (a second custom flag bit — note that `FSavedMove_Character` gives you **only four**
custom bits total).

**Sound.** There is none, and it is the largest single feel gap. A dash whoosh, a hit confirm and
a trail-trip cue would do more for readability than any visual work. Needs audio assets, which
means it is the first thing that will exercise the Git LFS workflow properly — read
[GITHUB.md](GITHUB.md) first.

**Better trail visuals.** Currently pooled cylinders. A ribbon mesh or a Niagara emitter would look
enormously better. Keep the *trip test* geometry (`TrailRadius`, `TrailHeight`) separate from the
visual, so tuning how it looks never silently changes how it plays.

**Match flow.** Warmup, halftime and side switching have shipped (`ATraceGameState::CurrentHalf` /
`bHalfTimeBreak`, with `ATraceArenaBuilder::ApplyTeamSides` repainting the ends). Overtime has not.
`ATraceGameMode` is where it goes.

**A real net-condition test pass.** Everything in
[NETWORKING.md §7](NETWORKING.md#7-a-playtest-checklist-that-actually-finds-bugs) should be run at
80 ms and 200 ms before anyone calls the netcode done.

**Automated tests.** None yet. The lag-compensation maths (`GetPoseAtTime` interpolation,
`ResolveHitscan` segment-capsule intersection) is pure and deterministic, which makes it the
obvious first target for Unreal's automation framework — and it is exactly the kind of code where a
silent regression is invisible in playtesting.

### Things to leave alone

- **Don't add `.uasset` dependencies casually.** The no-binary-assets constraint is what makes this
  repo pleasant to work in as a four-person team. Every asset you add is a file that cannot be
  merged and needs a lock. Add them when they earn it, not by reflex.
- **Don't hand-roll a network clock.** `GetServerWorldTimeSeconds()` is already replicated and
  already good enough for everything this game does.
- **Don't move actors to do lag compensation.** The current maths-only approach is faster and does
  not corrupt physics state. Every tutorial that teleports actors backwards is teaching a worse
  version of this.
