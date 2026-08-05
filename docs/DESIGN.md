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
- **Two halves of `ATraceGameMode::HalfDuration` (default 600 s = 10 min each)**, with sides
  switching at half time (`ATraceGameState::GetDefendedEndSign()` is the authority on which end a
  team is defending; the arena repaints itself to match). Highest score at the end wins.
  `ScoreToWin` (default **5**) only ends the match early if `ATraceGameMode::bEndMatchAtScoreToWin`
  is turned on, and it is **off by default** — "first to 5" would cut the second half, and the side
  switch that justifies it, out of most matches. Both live on the game mode
  (`[/Script/Trace.TraceGameMode]`), not on `UTraceSettings`.

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
  the current carrier's enemies until **new trace pushes them off the tail** — the trace is capped by
  `TrailMaxLengthUU` (**1200 uu**), not by any clock. **Spec v7 §1: a stationary carrier keeps their
  entire trace indefinitely.** Before this, standing still let the trace time out and made the
  carrier literally unkillable, the trace being the only counterplay. A turnover does
  not grant anyone immunity: after the Core changes hands, the new carrier's enemies can cut the new
  trail immediately, from its first point (subject only to `TrailHeadGracePoints`). "I dashed
  through it and nothing happened" is a bug, never a rule — with the one exception below.
- **A turnover delays the new trace from FORMING, and only that.** When the Core changes *team*, the
  new carrier lays no new points for `CoreTurnoverGraceSeconds` (**0.4 s**, down from 1.0). Segments
  already on the field stay lethal throughout. The grace exists so a turnover does not wrap the new
  carrier in lethal trace on top of the scrum they just won it in; at a full second the
  counter-attack also got a free run with nothing behind it, which is why it is now 0.4.
- **The parry is the carrier's answer to the dash.** See *The parry* below.
- On carrier death: the trail is **cleared instantly** and the Core goes to the killing side.

### The parry

New this pass. Verbatim from the notes: *"Create a parry mechanic for the core carrier. Parrying
gives your trace invulnerability for .1seconds. It also makes the entire trace turn red for the
duration of the parry. If an enemy would break your trace with a dash, parrying as they dash
protects the trace."*

- **Bound to `Q` by default**, and rebindable like everything else (Options → Controls). The notes
  suggested "right mouse or Q"; right mouse is already **Pass**, and parry is a carrier-only ability,
  so overloading it onto the carrier's own pass button would make the two mechanics — the pair a
  carrier needs most — unusable together.
- **Carrier only.** A non-carrier pressing the bind does nothing.
- **0.1 s** of trace invulnerability (`ParryDuration`). An enemy dashing through the trace inside
  that window neither breaks it nor kills the carrier.
- **The ENTIRE trace turns red** for the duration (`ParryTintColor`, `ParryGlowScale`) — every
  segment, not just the ones nearby. That is the tell, and it is deliberately readable by *both*
  players: the carrier confirms the parry registered, and the enemy already committed to a dash
  learns it is about to be wasted.
- **1.5 s cooldown** (`ParryCooldown`) — an assumption; the notes do not specify one. 0.1 s in 1.5 s
  is ~7% uptime, which keeps the parry a reaction check rather than a shield. Spamming it covers
  almost nothing.
- **Server-authoritative.** The server decides whether a dash landed inside a parry window; the red
  tint is predicted locally for responsiveness.
- It **composes with** the existing pass-window trace invulnerability rather than replacing it.
  Parrying mid-pass is a no-op because the trace is already invulnerable, and the parry ending must
  not clear an invulnerability the pass still owns.

### Everyone else
- **Hitscan** weapon — instant, no travel time, **and no spread at all**. The shot *is* the aim ray:
  `UTraceWeaponComponent::FireOnce` does not roll a cone, so a miss is yours.
- **Positional damage**: head `100` (an instant kill), body `40`, legs `25`. Damage lives in
  `UTraceDamageSettings`, not in `UTraceSettings`.
- **No friendly fire.**
- Bullets never damage the carrier.
- **Everyone** — carrier included — has a **dash** on a short cooldown and a **slide**.
  **There is no boost.** It was removed entirely this pass: the bind, the settings, the HUD pip, the
  saved-move state and the bots' use of it.

### Movement, view and feedback

The movement model was rebuilt this pass to the notes' brief: *"mimicking apex legends movement and
source engine"*. Three rules, and they are the whole of it.

- **Air control is a real Source/Quake acceleration projection, not a lerp.** Each airborne frame
  the current velocity is projected onto the wish direction, and input may only raise *that
  projection*, up to `AirMaxWishSpeed`, at `AirAcceleration` uu/s². Input can therefore only ever
  *add* velocity along the wish direction and can never subtract any — so input perpendicular to
  travel **rotates the velocity vector at essentially constant magnitude** instead of braking it.
  That is exactly what "full control authority to redirect velocity in air" asks for, and it is why
  there is no lerp toward the input direction anywhere in the movement component: a lerp is the
  thing that makes strafing cost speed. There is also **no air friction** — releasing the stick
  mid-air coasts at full speed.
- **Landing does not clamp.** Unreal has no landing clamp you can switch off; it has `CalcVelocity`,
  which brakes hard the moment planar speed exceeds `MaxWalkSpeed`, and between `GroundFriction` 8,
  `BrakingFrictionFactor` 2 and `BrakingDecelerationWalking` 2600 that kills 1000 uu/s in about
  60 ms — indistinguishable from a clamp. The movement component takes `CalcVelocity` over for
  exactly those frames and bleeds the **excess** at `GroundOverspeedFriction` /
  `GroundOverspeedBraking` instead, an order of magnitude gentler. Momentum carries from air to
  ground and runs out over a short distance rather than being deleted in one frame.
- **State transitions preserve the velocity vector.** run→jump never touched horizontal velocity.
  jump→slide enters the slide at exactly the speed you landed with. slide→jump used to be a hard
  brake (the exit clamped to walk speed) and now cannot end a slide below the speed the slide was
  running at. dash→ground hands back `DashExitSpeedMultiplier` × the ground limit instead of the
  ground limit itself. Every one of those ceilings is a knob.
- **The slide is a momentum carry, not a brake.** Crouching above
  `SlideEntrySpeedFraction × WalkSpeed` converts the speed you already have into a slide:
  `SlideSpeed = max(entry speed, min(entry speed × SlideEntrySpeedMultiplier + SlideImpulse,
  SlideMaxSpeed))`. Sliding out of a dash carries the dash's speed into the slide instead of
  throwing it away, and a **0.8 s buffer between slides** (`SlideCooldownSeconds`, measured from the
  slide's *end*) is what stops slide-chaining being free travel.
  **A conflict in the notes is shipped as two knobs, not as a decision made for you:** one line says
  *"entry speed determines slide velocity (no flat momentum boost)"* and another says *"have the
  slide increase momentum"*. `SlideEntrySpeedMultiplier` ships at **1.0** and `SlideImpulse` at
  **0**, which is the first reading. Raise either for the second.
- **The crosshair is drawn in both first and third person.** It used to be hidden while carrying,
  on the reasoning that a carrier has no gun and the camera pulls back. That was wrong in practice:
  the reticle is also the aim indicator for a *pass*, which is the carrier's only offensive action,
  and a screen with no centre marker reads as a broken HUD.
- **A dead player's model disappears immediately.** No corpse lingers on the field. A body that
  stays visible after death is read as a live enemy, which is the worst possible false signal in a
  game where the wrong dash costs you the point.

### The Core — the hover pass

The Core is **not a thrown, catchable object**. It is a *status* that transfers directly from one
player to another, and there is no loose Core to pick up.

- **Passing is a held input with a dwell.** Put the crosshair on a teammate and hold the pass input
  for `PassHoldSeconds` (**0.5 s**) — the transfer completes when the dwell does.
- **The pass is the game's risk beat.** The instant the input goes down the carrier's **shield
  drops** and their **trace goes invulnerable**; both are restored if the pass is cancelled. That is
  the half-second in which a defender who kept a gun on the carrier can convert.
- **A completed pass into the enemy endzone scores** — see *Structure* above.
- Acquisition is generous on purpose, because it has to survive two people running through cover:
  a **9°** aim cone *or* the aim ray passing within `CapsuleRadius + 70 uu` of the receiver's
  capsule axis, with **three line-of-sight probes** (chest, head, knees) of which any clear one
  counts. Maximum pass range is `PassMaxRange` (**8000 uu**).
- **Momentary illegality does not cancel a pass.** An in-flight pass survives
  `PassValidationGraceSeconds` (**0.15 s**) of the receiver being out of sight, off the crosshair or
  out of range; acquisition keeps returning the last receiver for `PassAcquireStickySeconds`
  (**0.20 s**) so the HUD's pass option does not flicker. A receiver who *dies* or changes team
  cancels instantly, with no grace.
- **A cancelled pass costs 0.25 s, not 2 s.** Completions spend `PassCooldownSeconds` (2.0);
  cancellations spend `PassCancelCooldownSeconds` (0.25). Charging a full two seconds for a pass the
  player never chose to abandon is most of why passing was reported as inconsistent.

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

Every key in the tables below is written out explicitly in `Config/DefaultGame.ini`, with its
default value and a comment explaining it, rather than being left to fall back to the C++ defaults.
That is deliberate: it means the game's tuning is one readable, reviewable, diffable text file, and
a tuning change shows up in a PR as a one-line diff instead of as an invisible absence. (The
difficulty-independent `Bots|*` scalars are the exception and are not mirrored into the ini; they
run on their C++ defaults until somebody adds a key.)

> **Where the ini and the C++ defaults in `TraceSettings.h` disagree, the ini wins at runtime — and
> this has caught people out more than once.** If you change a default in the header and the game
> does not change, look in the ini. **Change both.** `Trace.DumpSettings` in the console prints what
> the game is *actually* running on, which is the only thing that settles the question.

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
DashCooldown=2.000000
TrailLifetime=3.000000
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
| `ScoreToWin` | int32 | `5` | Points to win outright — **but only if `ATraceGameMode::bEndMatchAtScoreToWin` is on, and it is off by default.** Normally the match runs both halves and the highest score wins. |
| `PlayersPerTeam` | int32 | `5` | Team cap used when auto-balancing joiners. Set to 2 if you're testing 2v2. |
| `MinPlayersToStart` | int32 | `2` | Below this, the match stays in `WaitingForPlayers`. **Set to 1 if you want to run around solo** while iterating. |
| `RespawnDelay` | float | `3` | Seconds dead before respawn. The dead player's model is removed **immediately** on death — this delay is the wait, not a corpse timer. **The biggest pacing lever in the game.** Short = constant pressure and no space for a carrier run; long = deaths matter and a broken defence stays broken long enough to score through. |
| `WarmupDuration` | float | `5` | Countdown before the match goes `InProgress`. Set to 0 to skip while testing. |

**Half length is not on this page.** The match is two halves; the enforced length of each is
`ATraceGameMode::HalfDuration` (default **600 s**), which is `config = Game` on the *game mode* and
therefore lives under `[/Script/Trace.TraceGameMode]` in the same ini file. `MatchDuration` used to
sit here as a dead knob and was **deleted this pass** — nothing in the rules read it.

### Combat

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `MaxHealth` | float | `100` | Health on spawn. |
| `HitscanRange` | float | `28000` | Max trace distance in Unreal units (1 uu = 1 cm). The field is 24000 uu long, so this reaches end to end. **Lower it to ~6000 to force close-range engagements** and make the arena feel bigger. |
| `FireInterval` | float | `0.16` | Seconds between shots — 6.25 shots/sec. **Was `0.12`; the rate was cut by 25% so that a duel is decided by two or three deliberate shots rather than by who holds the trigger longest.** Minimum body TTK is 2 × 0.16 = **0.32 s**. The server validates fire rate against this with a small tolerance. |
| `bFriendlyFire` | bool | `false` | Off. Also controls whether the lag-comp resolver skips teammates. Turning it on materially changes escorting — probably keep it off. |

**There is no spread knob.** `SpreadDegrees` was **deleted this pass** along with the code that
would have read it: `UTraceWeaponComponent::FireOnce` does not roll a cone at all, so the shot *is*
the aim ray. The roll was removed rather than configured to zero so that a stale ini cannot quietly
reintroduce inaccuracy the design has deleted, and so a modified client cannot roll itself a zero
nobody else gets.

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
| `DashExitSpeedMultiplier` | float | `1.25` | **New.** Multiple of the ground speed limit a dash hands back when it ends. `1.0` is the old behaviour, and the old behaviour was the most visible velocity reset in the kit — you spent a cooldown, crossed 540 uu and arrived slower than a slide would have left you. Above 1 the surplus is handed back as real overspeed and then bleeds off through `GroundOverspeedFriction`, so it cannot become permanent free speed. |

Dash locks its direction at activation (last input direction, else actor forward) and cannot be
steered mid-dash. That is intentional — a steerable dash is both a prediction nightmare and removes
the commitment that makes trail-cutting a real decision.

### Air control and landing

The Source/Apex model described in §1. Every airborne frame:

```
CurrentAlongWish = Velocity · WishDir
AddSpeed         = min(MaxAirSpeed, AirMaxWishSpeed) − CurrentAlongWish
if AddSpeed <= 0 → no change this frame
Accel            = min(AirAcceleration × dt, AddSpeed)
Velocity        += Accel × WishDir
```

The cap applying to the component *along the wish direction*, rather than to total speed, is the
whole trick: point the stick sideways to your travel and `CurrentAlongWish` is ~0, so you get the
full add applied at ninety degrees — the vector rotates and its magnitude barely changes.

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `bSourceAirAcceleration` | bool | `true` | Master switch. Off restores Unreal's stock `AirControl` lerp, so the new feel can be A/B'd from one binary. Does **not** restore the landing brake — that is `bPreserveLandingMomentum`. |
| `AirMaxWishSpeed` | float | `160` | **The air-strafe dial**, in uu/s along the wish direction. Source ships the equivalent of ~57, which is deliberately tiny; this is higher because the notes ask for strafing in mid-air to be *more* effective, not less. Lower it toward 60 for the classic sharp Quake feel where only perpendicular input does anything. It does **not** raise your top speed. |
| `AirAcceleration` | float | `8000` | uu/s². Decides whether a turn takes one frame or several: at 8000 and 60 Hz a frame can add 133 uu/s, so the 160 cap is nearly reached per frame. Lower it for gradual, floaty air control. |
| `MaxAirSpeed` | float | `1600` | Ceiling on what air **input** may accelerate you to. Speed carried into the air by a dash or slide is never clamped — clamping it is precisely the "transitions reset velocity" complaint. |
| `AirFriction` | float | `0` | Lateral drag in the air (the engine's `FallingLateralFriction`). **Zero by design** — Source and Quake have none, and that is what makes a jump preserve the speed you took into it. Any non-zero value quietly undoes momentum preservation on every airborne frame. |
| `AirControl` | float | `1.0` | Engine-owned (`UCharacterMovementComponent::AirControl`), pushed in by `RefreshEngineTunablesFromSettings()`. **Was 0.45.** Under the stock model `AirControl` *was* the air model and 0.45 was how you stopped it being too strong; under the Source model it sits in front of the real model and scales acceleration before it is ever seen. Leave it at 1 and tune `AirMaxWishSpeed`. |
| `bPreserveLandingMomentum` | bool | `true` | The spec's "velocity carries over from air to ground". Off restores the engine's hard brake and makes the three overspeed knobs inert. |
| `GroundOverspeedFriction` | float | `2.0` | Proportional bleed applied to the excess above walk speed, replacing `GroundFriction` (8) for those frames. **The main "how long does carried speed last" dial.** At 8 the excess is gone almost immediately and preserving it buys nothing. |
| `GroundOverspeedBraking` | float | `400` | Flat uu/s² with no input, the counterpart of `BrakingDecelerationWalking` (2600). Separate from friction because a proportional bleed alone has a long tail — 2000→1000 takes as long as 1000→500 — so this is what actually lands the pawn. |
| `GroundOverspeedTurnRate` | float | `180` | Degrees/second an overspeed pawn may steer while it bleeds down. Unlimited would let a landing's momentum be carried round a corner at full value, i.e. free speed; zero would make a fast landing a rail. Raise it if landings feel "on ice", lower it if they feel too free. |

### Slide

Crouch while moving. **A slide spends momentum you already have** — it starts from your *current*
speed rather than from a fixed number, so sliding out of a dash carries the dash's speed with it.
It never resizes the capsule: the capsule is the single source of truth for hit resolution, lag
compensation and the trail trip test.

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `SlideEntrySpeedFraction` | float | `0.55` | Fraction of `WalkSpeed` you must already be moving at before crouch will start a slide. Stops "tap crouch from a standstill" being free speed. |
| `SlideEntrySpeedMultiplier` | float | `1.0` | Entry speed multiplier applied to the **actual entry speed** (not `max(speed, WalkSpeed)` — that floor *was* the flat boost, and it is gone). **Renamed from `SlideSpeedMultiplier` and moved 1.35 → 1.00 this pass**, because the notes say "entry speed determines slide velocity (no flat momentum boost)". At 1.0 the slide is exactly the speed you brought — and that is still momentum *preservation*, because the term it multiplies is your current speed. |
| `SlideImpulse` | float | `0` | **New, and the other side of a conflict in the notes.** A *flat* uu/s boost on slide entry, worth the same whether you entered at a walk or out of a dash — which is exactly what "no flat momentum boost" rules out and what "have the slide increase momentum" asks for. Ships at 0 (the first reading). Try 150–300 for a noticeable kick. Applied **after** the `SlideMaxSpeed` clamp on purpose, so dialling it up from zero is never silently eaten by the entry cap. |
| `SlideMaxSpeed` | float | `1900` | Hard ceiling on entry speed, so slide-out-of-dash cannot compound into something unbounded. Raised from `1500`, where a slide out of a 3000 uu/s dash was a speed **loss** — the opposite of preserving momentum. |
| `SlideDuration` | float | `1.8` | Longest a slide may last even if it has not decayed to the exit speed. Lengthened from `1.0` so a slide is a traversal decision, not a twitch. Measured mean slide: 0.67 s / 526 uu before, **1.44 s / 1301 uu after**. |
| `SlideDeceleration` | float | `260` | uu/s bled off every second. **This is the momentum dial.** Lowered from `750`, which stripped more speed per second than a slide could carry and ended most slides early on the exit-speed check — that is what made the slide read as a brake. At `260` a 1.8 s slide sheds only ~470 uu/s in total. |
| `SlideExitSpeedFraction` | float | `0.5` | Fraction of `WalkSpeed` at which a decaying slide gives up and hands the player back. Lowered from `0.6` so the slide runs to its duration rather than bailing early. |
| `SlideTurnRateDegrees` | float | `130` | Degrees/second a slide may be steered. `0` would make a slide a rail. |
| `SlideCooldownSeconds` | float | `0.8` | **The buffer between slides — the notes ask for ".8second".** Renamed from `SlideCooldown` and, more importantly, **re-based: it is now measured from the slide's END, not its start.** Under the old convention the gap between slides was a number you had to compute (cooldown minus duration) and any value below `SlideDuration` meant no cooldown at all — a trap that is exactly why the old value was 2.4. The rename is deliberate so a stale ini carrying 2.4 cannot land on the new knob and silently produce a four-second gap. |
| `SlideInputBufferSeconds` | float | `0.25` | How long a crouch press that could not slide yet (mid-dash, airborne) stays queued. This is what makes "dash, then slide out of it" and "press crouch just before you land" work. Only a fresh press charges it, so holding the key cannot chain slides. |
| ~~`SlideMinCommitSeconds`~~ | — | — | **DELETED in spec v5 §3, and the property is gone from `UTraceSettings`, `DefaultGame.ini` and the movement component.** It described a *partial* commit window, which a one-shot ability has no room for: the whole slide is committed the moment it starts, so releasing crouch cannot cancel any part of it. Listed here only so a stale ini or an old note does not send anyone hunting for a slider that no longer exists. |
| `SlideExitSpeedRetention` | float | `1.0` | Fraction of the slide's **live** speed carried into normal movement on exit. This and the two rows below are the actual momentum contract: the old exit only ever clamped *down*, so a slide handed you back at 80% of a run and made you re-accelerate. |
| `SlideExitMinSpeedFraction` | float | `1.0` | Floor on the exit speed as a fraction of `WalkSpeed` — a slide can never end slower than a run. |
| `SlideExitMaxSpeedMultiplier` | float | `1.0` | Ceiling on the exit speed as a multiple of max speed. **Keep at 1.0.** Above 1, `CalcVelocity`'s input branch clamps to the *current* speed once it exceeds max, so any overspeed handed back is kept for as long as a movement key is held and slide-cancel spam becomes the fastest way to cross the arena. |

### Boost — removed

**There is no boost.** The whole feature was deleted this pass on the note *"remove boost from the
game entirely"*: the bind, the saved-move state, the HUD element, the bots' use of it, and the four
settings that described it — `BoostZVelocity`, `BoostCooldown`, `BotBoostCooldownSeconds` and
`BotBoostStuckSeconds`. If a vertical launch is ever wanted again it is a new design, not a
resurrection of these knobs.

### Core — the hover pass

**Five dead knobs were removed from this category and replaced by nine live ones.** The Core stopped
being a thrown, catchable object some passes ago, but `PassSpeed`, `PassUpwardBias`, `PickupRadius`,
`PickupLockoutAfterThrow` and `CoreResetTime` stayed on the page describing a projectile and a
pickup that no longer exist — read by nothing (`ATraceCore::Throw` ignores its speed argument,
`IsPickupLockedOutFor` returns false unconditionally, and there is no loose Core to reset). What
replaces them is the set of numbers the hover pass actually runs on, which until this pass were
compile-time constants inside `TraceCore.cpp`.

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `PassHoldSeconds` | float | `0.5` | Dwell on the receiver before the Core transfers. **This is the risk beat, in seconds** — the carrier's shield is down for all of it. Longer gives punishers a bigger window; shorter makes passing close to free. Keep `BotPassHoldSeconds` equal. |
| `PassCooldownSeconds` | float | `2.0` | Before another pass may start after one **completes**. |
| `PassCancelCooldownSeconds` | float | `0.25` | Before another pass may start after one was **cancelled**. **Was the same 2.0, and that is a large part of the reported pass bug:** hold on a teammate, watch the ring fill, watch it vanish because they clipped a rail, then get nothing for two more seconds while still holding the button on a legal target. Cannot be 0 — the cancel path flips the shield and forces a net update, so a permanently-illegal target would churn that every frame. |
| `PassMaxRange` | float | `8000` | Longest legal pass. Keep it above the bots' own pass range (~6600 uu, from `BotPassRangeFieldFraction`) or bots start passes the rules then refuse. |
| `PassAimConeDegrees` | float | `9` | Half-angle of the acquisition cone. **Was 6**, which required holding a teammate 4000 uu away inside a circle ~420 uu across while both of you ran — a prime suspect in the pass-inconsistency report. First number to raise if passing still feels like it is refusing you. |
| `PassAimSlack` | float | `70` | Extra uu on the receiver's capsule radius for the second, distance-based test: the aim ray counts as on-target if it passes within `CapsuleRadius + this` of their capsule axis. **Was 40.** Either test acquiring is enough — the cone makes distant teammates reachable, this stops near ones feeling sloppy, because at point-blank a 9° cone is narrower than a body. |
| `PassValidationGraceSeconds` | float | `0.15` | How long an **in-flight** pass survives a receiver who has momentarily stopped being legal. A pass was measured dying **24 ms** before completing because the receiver crossed behind a lane rail; against the denser cover that is routine, and without a grace every blink is a cancelled pass. Covers **only** the transient geometric tests — a receiver who dies or changes team cancels instantly. Above ~0.4 a receiver can hide fully behind a block and still catch. |
| `PassAcquireStickySeconds` | float | `0.20` | How long **acquisition** keeps returning the last receiver found after they stop passing the tests. This is the *display* half of the report: the HUD polls ~20 times a second, so a flickering target makes the pass option itself flicker — "sometimes the pass option doesn't show up". Identity, team, life and range are still re-checked every poll. |
| `bPassMultiPointLos` | bool | `true` | Probe chest, head **and** knees for line of sight and accept any clear one. A single chest ray against the new 176 / 352 / 616 uu cover boxes is close to a coin flip — a receiver whose head and shoulders are plainly visible over a 1× block fails it. Chest is probed first, so the common case still costs one trace. |
| `PassTargetChestOffsetZ` | float | `20` | Height above the receiver's origin that the aim point and the first LOS probe both use. Chest, not feet. **Both must use the same offset** or the pass is aimed at a point the LOS test is not checking. |
| `CoreTurnoverGraceSeconds` | float | `0.4` | Seconds after the Core changes **team** before the new carrier's trace begins forming. **Was 1.0.** Delays formation only — segments already on the field stay lethal. A pass between teammates gets no grace at all, by design. |

### Parry

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `ParryDuration` | float | `0.10` | Seconds of trace invulnerability. **The entire mechanic is this number.** At 0.1 a parry is a read of the incoming dash; at 0.4 it is a panic button and the dash — the defence's only counterplay — stops being reliable. Raise only if playtesting says the window is unhittable at real latency, and raise the cooldown with it. |
| `ParryCooldown` | float | `1.5` | Before the carrier may parry again, from the parry's start. An assumption; the notes give no number. 0.1 in 1.5 is ~7% uptime, so spamming it covers almost nothing. Drop it toward 0.5 and the carrier can simply hold the lane covered, which reads as "the carrier is immune". |
| `ParryTintColor` | colour | `(1, 0.03, 0.06)` | What the **entire** trace turns for the duration. **Green and blue are near zero on purpose and must stay that way**: the trace is an unlit emissive drawn at glow well above 1, so any channel with weight in it clips to white at the tonemapper — the exact failure measured when the trace ran at glow 3.4 and became a shapeless white slab. |
| `ParryGlowScale` | float | `2.6` | Emissive multiplier while parrying. **Above the pass window's 1.9 on purpose** — the two states must not be confusable at a glance. Red at 2.6 is a step change in brightness as well as hue, so the tell survives being seen edge-on, at range or in peripheral vision, which is the only way an enemy already committed to a dash will see it. Do not push far past 3. |

### Trail

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `TrailLethality` | enum | `KillsCarrier` | What happens when the trail is tripped. `KillsCarrier` is the real game. `KillsToucher` and `KillsBoth` exist as **experiments** — `KillsBoth` in particular turns the trail into a mutual-destruction trade and is worth trying once. |
| `bOnlyEnemiesTripTrail` | bool | `true` | Teammates pass through freely. Setting false makes escorting suicidal and is almost certainly bad — but it's one flag if you want to see. |
| `bRequireDashToTripTrail` | bool | `true` | **The core rule.** False turns Trace into a completely different game (any contact kills the carrier), where carriers die instantly and the mode collapses. Useful as an A/B to demonstrate why the dash requirement exists. |
| `TrailMaxLengthUU` | float | `1200` | **Spec v7 §§1-2. THE expiry rule, and the lever on how much history is exposed.** Points leave the tail only when new trace at the head pushes the path past this — there is no clock anywhere in the retirement path, so a carrier who stops moving keeps their whole trace. Longer trail = more surface area for defenders to cut = weaker carrier. Derived as the old `TrailLifetime` 2.0 s × `WalkSpeed` 800 = 1600 uu, **minus 25%**. Pair every change here with `DashCooldown` **and** with `BotTrailMinPointLifeRemaining`, which is a fraction of the trace written as an absolute (0.40 × `WalkSpeed` = 320 uu, the oldest ~25%): move one without the other and the interceptor bots quietly get worse at the game's signature play. |
| `TrailLifetime` | float | `2.00` | **No longer the expiry rule** — spec v7 §1 deleted time-based expiry. Two jobs remain: it derives `TrailMaxLengthUU` when that is left at or below zero (× `WalkSpeed` × 0.75), and it is the age-fade reference for the legacy renderer arm (`Trace.Trail.Renderer 0`). **Do not reintroduce an expiry read of it** — a surviving timer restores the stand-still exploit. **Was `2.8`, and `4` before that.** |
| `TrailPointSpacing` | float | `60` | Distance the carrier must move before a new point is recorded. Lower = smoother trail and more accurate trip tests, at the cost of more points and more bandwidth. Don't go below ~30 without watching `stat net`. |
| `TrailRadius` | float | `22.5` | **Half the trace's width, lethal and drawn at once** — the trip test and every renderer arm resolve it through one accessor, so visible-but-harmless ribbon and invisible-but-lethal hitboxes are both inexpressible. **Was `45`**, the full player-model width; spec v7 §3 halved it ("it doesn't need to be the full width of the player model"). Now well under a character capsule (34 uu), so cutting requires real precision. |
| `TrailHeight` | float | `63` | Height of the trail volume, lethal and drawn at once, centred on mid-model. **Was `190`** (roughly a character's height); spec v7 §3 kept only the **middle third** "in order to make visibility around the trace better". A jump or ramp now clears it ~63 uu sooner, so it no longer reliably prevents jumping over. Since v7 this no longer moves the third-person carry camera — see `TraceCharacterLayout::CarryPivotZ`. |
| `MaxTrailPoints` | int32 | `256` | Hard cap on recorded points, oldest dropped first. **Not an expiry rule** — spec v7 §1 left `TrailMaxLengthUU` as the only thing that retires a point, and at the defaults that binds two orders of magnitude sooner (1200 uu at `TrailPointSpacing` 60 = ~21 points against this 256). It survives purely as a memory and bandwidth ceiling for a pathological spacing setting. **If you raise `TrailMaxLengthUU` a lot, or drop `TrailPointSpacing` a lot, check whether this cap starts binding instead** — it is measured in points, and the trace is measured in uu. |
| `TrailHeadGracePoints` | int32 | `3` | The newest N segments cannot be tripped. **This is what stops "dash at the carrier's back" from being the whole game** — you must cut where they *were*, not where they are. Raise it if point-blank trail kills feel cheap; lower it if carriers feel untouchable. |

### Net

| Key | Type | Default | What it does / how to tune |
|---|---|---|---|
| `bEnableLagCompensation` | bool | `true` | Master switch for server-side rewind. Turn it **off** to feel exactly how bad hitscan is without it — genuinely worth doing once. |
| `MaxRewindTime` | float | `0.25` | Maximum seconds the server will rewind a shot. **This is a security boundary, not a feel knob** — it bounds how much of the past a client can claim. Higher favours high-ping shooters; lower favours the target's right to have reached cover. 0.25 s is a normal shipping value. |
| `LagCompHistoryDuration` | float | `1` | How much pose history the server retains per character. **Must comfortably exceed `MaxRewindTime`** — keep at least 2–4× headroom, or rewind requests fall off the end of the buffer and silently stop compensating. |
| `bDrawServerRewindDebug` | bool | `false` | Draws the historical capsules the server tested a shot against. Invaluable when hit registration feels wrong; very noisy. Off in normal play. |

Full networking explanation in [NETWORKING.md](NETWORKING.md#5-what-trace-already-does-for-netcode).

### The arena

The field is **not** on the `UTraceSettings` page — it lives on `ATraceArenaBuilder`, which is an
actor, so its properties are edited on the actor's Details panel (and are what the editor preview
button rebuilds live; see [EDITOR.md §2](EDITOR.md#2-seeing-the-arena-without-playing)).

**It was rebuilt this pass from the collaborator's overhead sketch.** The numbers that matter:

| Property | Value | Why |
|---|---|---|
| `FieldLength` | `24000` | Unchanged. A full-field carry is ~33 s at carrier speed, which is what gives the trail-cutting counterplay time to happen. |
| `FieldWidth` | **`9600`** | **Was `12000`.** The sketch is drawn at **length : width = 2.5 : 1**, so the length is kept and the width narrowed. Everything derived — endzone volumes, spawn pads, bot steering bounds, `GetFieldBounds()`, the half-time side switch — is expressed as a fraction of these two numbers and follows automatically. |
| `WallHeight` | `2600` | Four perimeter walls with lit trim, a kick rail and vertical ribs. |
| `EndzoneDepth` | `2400` | The endzones are solid bands at each end spanning the **full width**, sideline to sideline. |
| `BankHeight` | `352` | The four corner banks — see below. 2× player height, matching the sketch's mid-height structures. |

**Structure heights are keyed to the player, not hardcoded.** The sketch's colour key is green
outline = 1× player height, orange = 2×, red = 3.5×. The capsule is 88 uu half-height, so
**1× = 176 uu, 2× = 352 uu, 3.5× = 616 uu** — and `ATraceArenaBuilder::PlayerHeightUU()` derives
that from `ATraceCharacter`'s class default object rather than assuming it, falling back to 176 and
saying so in the log if the CDO cannot be read. Change the capsule and the cover follows.

The layout, all of it from the sketch:

- a **shallow stadium bowl** — four terraced corner banks, one per quadrant, raised along the long
  edges and stepping **down** toward a flat central playfield. These are the sketch's green arrows
  ("slants down in the direction of the arrow"). Every riser is under `MaxStepHeight`, so they are
  walkable from any direction and cannot trap a bot that steers straight at its target;
- a small **diamond at the exact centre** — a three-tier stepped platform carrying the Core
  pedestal, ringed by four light pylons;
- a **tall 3.5× tower at top centre**, standing on the dividing line;
- a scatter of cover boxes and long low bars at exactly 1× / 2× / 3.5×;
- full-width lit gates on both endzones, flank buttresses and a continuous high rail, light bridges,
  lane floor stripes and endzone corner pylons.

**The halves are mirrored, deliberately.** The hand sketch is not symmetric — the left half has a
long horizontal bar and a diagonal, the right a vertical bar and a different scatter. The builder
mirrors **one** half through the centre line instead, because the match plays two halves with a side
switch and an asymmetric field would hand one team the better half for ten minutes. Everything
except the top-centre tower is mirrored in X; the tower sits *on* the dividing line, so it belongs
to neither half. If the asymmetry is ever wanted back it is a per-spec `XSign` filter in
`BuildCoverField` and nothing else.

`HitscanRange` (28000) still spans the field: the diagonal of 24000 × 9600 is ~25,849 uu.

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
| `ATraceCore` | `AActor` | The objective, and a **status rather than a physical ball**: it is essentially always held. Runs the hover-pass state machine (acquire → hold → transfer, with the grace and sticky windows), drives the carrier flag on the character, the player state and the trail component, and owns the kickoff/turnover paths. `Throw()`, `TryPickup()` and `DropAt()` survive only as legacy shims for debug commands — `Throw()` re-expresses itself as a pass *input* and ignores the direction and speed it is given. |
| `ATraceEndzone` | `AActor` | Box trigger with an `OwningTeam`, spanning the **full width** of the field. Server only. Scores on overlap *and* on a 10 Hz geometric poll of the current carrier's position — the poll is what makes a **pass completed to a teammate already inside the zone** score, since no overlap event fires for someone who is already standing there. `ScoresHere()` holds the one direction rule: `Team == TraceOpposingTeam(OwningTeam)`. |
| `ATraceArenaBuilder` | `AActor` | **Builds the entire playfield in C++ at `BeginPlay`** — see *The arena* below. Repaints both ends at half time via `ApplyTeamSides()`. This is why `/Game/Maps/Arena` can be empty — see [EDITOR.md](EDITOR.md). |
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
