# Known limitations

This is the release's honest list of what is **documented and deliberately not fixed**. It is the
list `README.md` links to, and it is the complete one — there is no longer a longer copy anywhere
else.

**Read this before you file a bug.** Most of what is below has already been measured, argued and
decided. Several entries exist specifically so nobody re-derives them as new defects.

**How to read an entry.**

| marker | meaning |
|---|---|
| **OPEN** | present in the code you have. Verified against this tree on 2026-08-28. |
| **CLOSED** | filed earlier in the release work, fixed since. Kept, with its number, because 49 internal reports cite these by number. |
| **OWNER** | not a defect anybody can fix by editing code — it needs a decision from the project owner. |

**Numbers are stable.** Items 1–28 were filed during the seven implementation waves and keep their
original numbers even where the finding has since changed. Items 29+ come from the final acceptance
wave (2026-08-28).

**Line numbers drift, and these ones were machine-checked.** All 46 `file:line` citations below were
re-resolved against the working tree on 2026-08-28, each against an anchor string rather than by eye
— and ten of them had moved in the twenty minutes it took to write this file, because other work was
landing in the same tree. If one has moved again, the symbol name beside it is the durable reference.

**Every OPEN status here was mechanically re-checked, not asserted.** A second script re-resolves
each one against the tree — "is the constant still that value", "does that branch still notify
nothing", "do those two defaults still disagree" — and it caught two things while this file was being
written: an item a parallel fix had already closed, and one of its own needles that anchored on a
comment and wrongly reported a producer that is not there.

Both scripts are in the release report bundle as `w9handoff/check_lines.py` and
`w9handoff/reverify.sh`. **If you fix something below, run them**: the line for that item should
flip, and any citation you moved will be named.

**`reports/…` paths are not in this repository.** Several entries cite the release work's own
evidence — 49 tranche reports (1.4 MB of prose) plus their logs and frames (about 3.7 GB in total).
That material is a separate deliverable handed over with this branch and is deliberately not
committed. Everything a colleague needs in order to *act* is in this file; if you want the underlying
frame or log behind an item, ask the owner for the release report bundle.

---

## A. Gameplay and rules

### 1. Fire-rate tolerance lets a modified client sustain about +25% DPS — **OPEN, deliberate**

`FireRateTolerance = 0.2` (`Source/Trace/Gameplay/TraceWeaponComponent.h:938`) is the fraction of a
weapon's fire interval the server forgives, applied in `ServerFire`
(`TraceWeaponComponent.cpp:2427`). The melee twin is `SwingRateTolerance = 0.2`
(`TraceWeaponComponent.h:1185`).

Not tightened, on purpose: damage stays server-authoritative (the tolerance buys rate, not aim or
damage), this is a LAN/listen-server game between known players, and a false positive in the hottest
server validation path drops an honest player's shots. The doc block above the constant owns the
trade.

### 2. Hit-zone classification is a three-zone approximation — **OPEN as a balance question only**

**Correction to how this was filed.** Earlier internal copies of this list said zone classification
"uses the shot's entry point". It does not, and has not since `a46d19a`: the head test is a true
ray/sphere intersection, and the body/legs split is a height band sampled at the ray's **closest
approach to the capsule axis**, explicitly not at the entry point
(`Source/Trace/Gameplay/TraceHitZones.cpp:174-190`, which measures the old error at 2.9% of hits on
a flat map and explains why the impact point still has to stay on the surface).

What remains is genuinely open: the model is a capsule plus a head sphere with no per-limb geometry,
so where a shot along an arm or over a shoulder *should* land is a balance decision, not a bug.
`Trace.HitZoneTest` runs the offline self test of the model — note it is **compiled out of Shipping**
(`TraceHitZones.cpp:207`), like every other harness command.

### 3. Corpses and the slide are procedural presentation — **OPEN, deliberate**

Both are posed at runtime rather than played from authored animation. See also item 15 — on death
the mesh is hidden outright, so the "dead" colour treatment is almost never on screen.

### 11. `ATraceCore::DropAt` ignores its location argument — **OPEN**

`TraceCore.cpp:7240` — the parameter is literally commented out in the signature. `DropAt` queues a
fallback and calls `ReleaseHolder`, so a dropped Core goes to the **opposing team's nearest living
player** rather than being placed where the carrier died.

The bounds clamp that feeds it (`ATraceGameMode::ClampCoreDropLocation`, `TraceGameMode.cpp:1030`,
shared by the death and disconnect paths) is implemented and correct, but it is a guard on a value
the callee discards. Making `DropAt` honour a position is a change to the possession rules, not a
repair — hence untouched.

### 14. There is no spawn shield in this build — **OPEN, and it is not a missing feature**

`grep -riE "spawn(protect|grace|invul|shield|immun)"` over `Source/Trace` returns **zero** matches,
and `UTraceHealthComponent::IsInvulnerable()` (`TraceHealthComponent.cpp:419`) derives invulnerability
solely from holding the Core with the shield unsuppressed. The **carrier** shield is the only
invulnerability in the game. Nobody should go looking for a spawn shield; there has never been one.

### 15. "Dead" is a hide, not a dim — **OPEN, deliberate**

`ApplyTeamColors()` has a dead branch (body colour ×0.2, `EmissivePower = EmissiveDead`,
`TraceCharacterBody.cpp:1009`) but on death the mesh is hidden outright
(`TraceCharacter.cpp:1260`, `[Corpse] … hidden on death`). So the dim path paints only the single
pre-hide frame and the fallback primitives on a machine with no character art. Consistent with item 3;
recorded so it is not re-derived as a bug.

### 34. Patch 28's throw-speed change broke `Trace.ModeB.Verify` step 5 — **OWNER DECISION**

Patch 28 §4 cut `CoreThrowSpeed` from 3300 to 2900 (`Config/DefaultGame.ini:1571`). Step 5 of
`Trace.ModeB.Verify` — "a bot scores by throwing at the goal" — now fails, and it is the throw speed
that does it. Proved by an A/B on **one binary**, two runs per arm: shipped 2900 fails twice
(9 PASS / 1 FAIL), the same binary forced to 3300 passes twice (10 PASS / 0 FAIL), and the two
historical runs of the suite from 2026-08-23 also pass at 3300.

The arithmetic is deterministic because the step's placement is scripted: the shot needs **5354 uu**
of reach and the Core now carries **4997 uu** — out of range by 357 uu, where at 3300 it had 6728.
Closing the distance does not rescue it: head-on, the shot first becomes legal 201 uu before
`CarryInCommitDistance` latches the carry-in, which is about 0.2 s at a carrier's 976 uu/s and
inside the bot's own reaction delay.

**Three honest options, all the owner's:**

1. **Accept it.** A slower Core means bots shoot from closer, and step 5's 4800 uu standoff is simply
   the wrong test now. Moving the standoff is one constant in `TraceCoreHarness.cpp`.
2. **Lower `CarryInCommitDistance`** from 4200 so the throw window reopens — a real change to how bots
   play, not a test fix.
3. **Retune mode B.** Range goes as the square of speed, so a 12.1% speed cut is a 22.8% range cut,
   and the goal has been 2400 uu further out and 1100 uu up since spec v6 §4.3.

**Lowering `ThrowAtGoalMinScore` is not one of them**: at that standoff the shot is past the end of
range, not near its ragged edge, so a gate of 0.0 would still refuse it. Full evidence:
`reports/W8-KNOBS.md` §4.3.

---

## B. Movement and the map

### 30. The surf rails are not on the map the game loads — **OPEN, owner call to re-bake**

Patch 28 §5's surf mechanic is implemented, bounded, predicted and measured. It is also
**unreachable by a player**, because the four rails are built by `ATraceArenaBuilder::BuildSurfRails()`
(`TraceArenaBuilder.cpp:4094`), called only from the procedural build path
(`TraceArenaBuilder.cpp:2408-2412`). A baked level adopts its placed actors and builds nothing.

* `ServerDefaultMap=/Game/Maps/Arena_Baked` (`Config/DefaultEngine.ini:120`)
* `TraceMaps::Arena` → `/Game/Maps/Arena_Baked` (`Source/Trace/UI/TraceMatchOptions.h:43`), and both
  **PLAY** and **PRACTICE** on the title screen travel there.
* Counted directly: **0 of the 647** external actor files under
  `Content/__ExternalActors__/Maps/Arena_Baked/` carry any `SurfRail*` piece — which is what
  `BuildSurfRails()` labels its geometry (`SurfRailFace`, `…Body`, `…Nose`, `…Fill`, `…Access`,
  `…CrestLine`). **Control, and it is load-bearing:** the same scan finds `SM_KitOctagon` in 1 file
  and `Floor_Lamp` in 37, matching the arena census, so the zero is a real zero.

  Two ways to get that scan wrong, both of which were hit: `grep` needs `-a` or it reads the
  `.uasset` packages as binary and returns nothing for *every* needle; and the needle must be the
  actor name, because the bare word `Surf` matches **171** of the 647 files — every one of them a
  `Surface_*` material-instance name.

`Source/Trace/TraceSettings.h:2411` says this in a comment beside `bSurfEnabled`. To ride the rails
today: `Scripts/run-listen-server.sh --map /Game/Maps/Arena`. To put them in the shipping map,
somebody has to re-bake — **and a re-bake is not free**; see the warning under item 44.

### 31. The surf speed ceiling is `max(entry speed, 1719)`, not a flat 1719 uu/s — **OPEN**

`GetSurfSpeedCeiling()` (`TraceCharacterMovementComponent.cpp:1788-1799`) returns
`FMath::Max(SurfEntrySpeed, GetAirStrafeHardCapSpeed() × GetSurfSpeedCeilingMultiplier())`. The
derived cap is 1,375 × 1.25 = **1,719 uu/s** (1,375 is `AirStrafeHardCapSpeed=1250` times
`AirStrafeAsymptoteScale=1.100000`, `Config/DefaultGame.ini:856,864`; the game logs the whole
derivation at boot as `ceiling=1719 uu/s = airHardCap 1375 x 1.25`). But a player who **dashes** onto
a rail arrives at `DashSpeed=3300` (`Config/DefaultGame.ini:745`) and keeps it — 1.92× the headline
number. There is no ratchet (the clamp is
`min(v, max(entry, cap))`, so you can never exceed what you brought), but summaries that quote a flat
1,719 uu/s ceiling are quoting the floor of the ceiling, not the ceiling.

**A units mismatch inside it, unfixed.** `SurfEntrySpeed` is the pawn's **planar** speed at first
contact, while the clamp is applied to `Velocity.Size()` (**3D**). Enter with planar 1,600 and
vertical −1,200 (3D 2,000) and the ceiling is `max(1600, 1719) = 1719`, so the pawn is scaled down by
281 uu/s on arrival — which is the opposite of what the comment on that block promises. Two different
speed definitions on the two sides of one comparison.

### 32. Surf's "zero corrections" claim is narrower than it sounds — **OPEN**

The two-process prediction evidence is **eight rides at `NetEmulation.PktLag 40` on
`/Game/Maps/Arena`**. Not covered anywhere: packet **loss** (interesting because `CanCombineWith`
refuses to merge moves while surfing, so a surfing client sends more unmergeable moves than usual),
client/server **tick-rate asymmetry**, surfing **while carrying the Core**, and surf × dash or
surf × parry. Read the claim as "zero corrections at 40 ms of clean latency, no loss, matched tick
rates, no Core, no dash".

### 41. The surf audit's clip table cannot see `SurfOverbounce` — **OPEN, harness only**

`TraceMovementSurf.cpp:242` sets `const float Overbounce = 1.f;` and passes that literal to
`ClipVelocityAgainstPlane`, so table [2] of `Trace.Move.Surf` produces a byte-identical result with
the knob overridden. **The game is correct** — `ComputeSlideVector` reads `GetSurfOverbounce()`. Only
the harness written to prove the clip is blind to the knob.

---

## C. Characters, art and identity

### 16. CLOSED — the accent hues were re-spaced

Filed as "four of the ten accent hues sit inside their own team's hue band". Wave 5 re-spaced the
whole ring: every accent is now ≥ 40° of sRGB hue from **both** team colours
(`Source/Trace/Core/TraceCharacterRoster.cpp:231`, and `Scripts/character_bodies.py`'s `CHARACTERS`
table).

### 27. CLOSED — the duplicated FX accent constants now read the roster live

Filed as "six per-character accent hues are duplicated as FX constants and are a wave behind the
bodies". The duplicates are gone: an accent-owning burst reads its hue live from
`TraceCharacterRoster`, and the seven old literals survive only as a cheat-gated red arm
(`Trace.Fx.LegacyAccents`, `Source/Trace/Gameplay/TraceFxBurst.cpp:82`, `ECVF_Cheat`, never shipped
at 1). A tree-wide sync checker enforces it.

### 28. Two characters do not read at the design's own 3,000 uu gate: **ROCCO and ELLE** — **OWNER DECISION**

Eight of the ten now read cold at 46 px. The two that remain are blocked by their own design sheets,
not by execution:

* **Rocco** — his sheet freezes his chassis ("baseline §4.1 verbatim, no deviations") and freezes the
  crest at 6 uu thick against a crown-break ceiling of 8, so there is nothing legal left to widen on
  the template body.
* **Elle** — her identity is explicitly "silhouette only" and her accent programme lives on the
  half-cape, so front-on she has no crown ornament at all.

Each is one owner decision away: bless a sheet deviation on Rocco's crest, or allow Elle a crown
break. Frame: `reports/frames-W5-BODYVAR/T3_lineup_3000uu_downsampled_x6.png`.

### 33. The practice-range dummies wear a different body from the players — **OPEN**

The five practice-range targets (`DummyCount = 5`,
`Source/Trace/Modes/TracePracticeRange.cpp:36`) are **Epic's Mannequin**, and deliberately so:
`ATraceGameMode::PollCharacterSelect`'s bot fill skips any bot on `ETraceTeam::None`, which is the
team the dummies are on so they stay damageable, cannot move the scoreboard and cannot receive the
Core (`Source/Trace/Modes/TracePracticeActors.h:32-45`; the line that decides it is
`:38`, "*they stay Mannequins*").

The **side effect** is that two character styles ship together, and the smooth anatomical mannequin
the players used to wear is now standing in the range as a target while the ten playable characters
are the newer blocky bodies. A new player's first practice session is a blocky Rocco shooting at
sleek mannequins. Cosmetic, deliberate in mechanism, unintended in effect — and it is the strongest
argument against reading the roster bodies as a chosen low-poly style, because the project still
contains and still uses the other one.

### 35. CLOSED — Elle's character-select card now matches her kit

Filed on 2026-08-28 as "the card promises 40% and the game gives 30%": Patch 28 §3 cut
`ElleSlideJumpGainBonus` to `0.3f` and nobody moved the card. Fixed the same day. The card at
`Source/Trace/Core/TraceCharacterRoster.cpp:260` now reads *"WELL-TIMED SLIDE JUMPS GIVE HER **30%**
MORE OF THE MOMENTUM BOOST THAN ANYONE ELSE"*, matching `Source/Trace/TraceSettings.h:5494` and
`Config/DefaultGame.ini:2476`, and `DA_Character_Elle.uasset` was regenerated with it so the data
round-trip stays green.

**The lesson is worth keeping**, because the checker did not catch this and still would not:
`Trace.VerifyCharacterData` compares the *card* against the *C++ table*, and while both said 40% it
was structurally unable to see the disagreement with the tuning knob. Any future card that quotes a
number must be checked against the knob, not against its own copy.

---

## D. UI and HUD

### 10. See item 17

Filed separately as "the shield-blocked hit marker cannot draw for ordinary gun fire". The marker has
since been implemented, so the finding is now entirely about its two missing producers and is stated
once, below.

### 17. The shield-blocked hit marker draws, but has no live producer — **OPEN**

The marker itself ships and is photographed: four axis-aligned ticks, shield white, 0.25 s, no grow.
Neither of its two producers exists.

* **Hitscan.** `UTraceLagCompensationComponent::ResolveHitscan` **skips** a shielded Core carrier as a
  candidate (`TraceLagCompensationComponent.cpp:325`, "the Core carrier is invulnerable to bullets by
  design - do not even resolve them"), so no victim resolves, no notification is sent, and the shooter
  gets silence. Producing the fact where the shot is refused is a change inside the carrier-immunity
  rule — an owner-level decision, not a repair.
* **Melee.** `ServerSwing`'s `else if (Hit.bBlockedByCarrierShield)` branch
  (`TraceWeaponComponent.cpp:3565`) logs and notifies nothing. One line would give the marker a live
  producer.

`Trace.Weapon.ShieldBlockTest` asserts the current silence, so a future change is noticed.

### 18. A blocked hit plays **both** the ordinary Bodyshot sound and ShieldBlock — **OPEN**

`ATracePlayerController::ClientNotifyHit` plays Headshot/Bodyshot unconditionally
(`TracePlayerController.cpp:3389`) and the ShieldBlock cue plays from the HUD on the same
confirmation. The design asks for the hit sound to be **suppressed** when the hit was blocked. The fix
is one guard on the existing `bShieldBlocked` parameter, which is already in scope on that line.

### 19. CLOSED — the V-cooldown row has a real producer

Roxie's `GetSecondaryCooldownDisplay` override landed
(`Source/Trace/Abilities/TraceAbilityComponent.cpp:701`), and the temporary capture arm it replaced is
now unreachable and documented as such at `TraceAbilityComponent.cpp:744`.

### 23. Every HUD panel except the score bar draws at 72% opacity — **OPEN**

`TraceHUDStyle::PanelFill` is `(0.02, 0.03, 0.05, 0.72)` (`Source/Trace/UI/TraceHUD.cpp:148`) and
governs the kill feed, the scoreboard and every secondary panel. Only the top score bar was lifted,
and it is now fully opaque (`TopPanelFill` alpha `1.00`, `TraceHUD.cpp:176`). Measured world-bleed
through the kill feed over a lit walkway — luminance spread p95−p5 — was **64.3 at 1080p and 128.7 at
4K** (`reports/W5-UIQA.md`, which also records the score bar going 57.7 → 34.0 when it was lifted, and
the scoreboard unchanged at 26.7 → 27.3). One constant governs all of them.

### 24. The Canvas ammo corner draws the spent magazine slot at the wrong end, ~3.7× too wide — **OPEN**

Both corner renderers pass `Trace.HUD.V16Shots` (27 passed / 0 failed) and both really do draw 29 lit
ticks at 29/30. But UMG puts the single spent slot at the **right** end at tick width (~7 px) while
Canvas puts it at the **left** end at 22 px, so the Canvas strip has an unslotted grey gutter before
the first round. The harness asserts the lit-tick count and the ammo string, not the spent slot's
geometry, so it cannot see this. Frame: `reports/frames-W5-UIQA/AB_ammoticks.png`. **If this is ever
fixed, add a spent-slot assertion or it regresses silently.**

### 25. The shipped HUD renderer default depends on a static-init race — **OPEN**

`Source/Trace/UI/TraceMenuHUD.cpp:217` declares `static int32 GUseUMG = 1` and its help text calls 1
the default since spec v20. `Source/Trace/UI/TraceHUD.cpp:4361` declares
`static constexpr int32 SharedToggleDefault = 0`. Whichever translation unit registers the CVar first
wins. In practice `TraceMenuHUD` wins and both observed runs booted with the UMG corner active — but
the comment at `TraceHUD.cpp:4346`, which states in capitals that the two numbers now agree, is
**false as written**, and it was written to close exactly this hazard.

### 26. The mouse pointer beats the keyboard on the options page and on character select — **OPEN**

`FTraceOptionsMenu::PollInput` runs `PollNavigation(PC)` then `PollMouse(PC)`
(`Source/Trace/UI/TraceOptionsMenu.cpp:2171-2172`), so the selection follows whatever the OS pointer is
resting over even when the pointer has not moved. Both panels are screen-centred, so a desktop pointer
is very often inside them. Character-select navigation should be confirmed by hand before trusting any
scripted select-screen capture.

### 39. "ACTIVATED" clips on every 16:10 display and never on a 16:9 one — **OPEN**

On the character-select cards the label **"ACTIVATED"** is overprinted by the cooldown chip: it reads
`ACTIVAT` at 1728×1117 (what macOS hands the game by default on the owner's laptop), `ACTIVATE` at
1920×1200 and at 3456×2234, and in full only at 1920×1080. Cause: `UIScale` is derived from view
**height** while the label's column is a fraction of view **width**. This is the defect most likely to
be seen by a colleague and least likely to be seen on a developer's monitor.

### 20–22. Historical UI notes — **recorded so they are not re-derived**

* **20.** The original audit's frames are 1728×1117, not the 1920×1080 its method section claims, so
  any width-dependent finding taken from them was observed in a narrower window. The one that
  mattered — the join-prompt hint overflow — **does not overflow at any width tested** (840 px of ink
  in a 920 px interior at the audit's own geometry; 560 px in a 589 px interior at 1280×720). The
  measure-and-shrink guard is in and correct; it is simply never asked to act.
* **21.** Two of the audit's "front door" findings were per-machine ini state, not shipped defaults:
  the magenta crosshair came from a hand-set `CrosshairColorIndex=7`, and the title screen's
  `100.101.102.103:7777` came from `LastJoinAddress` in a local `GameUserSettings.ini`. A clean
  install shows one address on the front door.
* **22.** The cursor art is unchanged since the audit (a white blade with an amber rim), and the two
  title renderers draw different pointers — the UMG title draws the blade, the Canvas title draws a
  cyan gap-cross. The slider thumb is the same cursor sprite, re-tinted, so a pointer resting on a
  slider puts two identical blades on one bar. Owner-level art decision.

---

## E. Build, packaging and anti-cheat

### 29. "Both build configs green" has never meant the shipped game **runs** — **OPEN, and important**

`Scripts/build.sh --prove-shipping` proves the Shipping target **compiles and links**. That is exactly
what its help text claims, and it is all seven waves ever exercised. The artefact itself was run for
the first time on 2026-08-28, and it does not start:

* **The bare binary aborts at launch.**
  `dyld: Library not loaded: @rpath/libmetalirconverter.dylib`, SIGABRT, confirmed by macOS's own
  crash report. The library exists, under
  `<engine>/Engine/Binaries/ThirdParty/Apple/MetalShaderConverter/Mac/`. The binary's own rpath is
  `@loader_path/../../../../../../../Shared/Epic Games/UE_5.8/…` — **seven** `..` from
  `Binaries/Mac/`, which walks past `/` and asks for a non-existent `/Shared/Epic Games/…`. Seven is
  the correct count from **inside the `.app` bundle**, three levels deeper. Reproduce with
  `otool -l Binaries/Mac/Trace-Mac-Shipping | grep -A2 LC_RPATH`.
* **The `.app` copy clears dyld and then exits 1 with no output**, because a Shipping game target
  compiles logging out — and because **there is no cooked or packaged content anywhere in the
  project**: a whole-tree `find . -type d \( -name 'Cooked*' -o -name 'StagedBuilds' -o -name
  'Packaged*' \)` returns nothing.

Neither is a regression, and neither means the code is wrong: a UE game target legitimately needs
cooked content, and the rpath depth is an install-layout artefact of an engine under `/Users/Shared`
with the project under `/Users/<user>`. What it means is that **the packaged-game path is wholly
unexercised**. The supported path is the editor plus `Scripts/run-listen-server.sh`, and that path is
exercised heavily.

### 36. Twenty shipped rule arms are not cheat-flagged — **OPEN, one word each**

Of 674 console registrations, 446 are compiled out of Shipping and 163 ship inert behind `ECVF_Cheat`
(`DISABLE_CHEAT_CVARS` is 1) — counted twice on two different Shipping binaries eleven hours apart and
identical both times (`reports/W8-BATTERY.md` §8.3, §11). Of the 65 that ship live, **20 are
self-described red arms that switch a shipped gameplay rule off** — including `Trace.Ammo.Enabled 0` ("the clip is infinite and nothing ever
reloads") and `Trace.Bounds.Enabled 0` ("removes the rule"). They are `ECVF_Default`, so the one
injection route that survives in Shipping — the `[ConsoleVariables]` section of a user-writable
`Engine.ini` — does not refuse them. Their own siblings are already cheat-flagged, so changing
`ECVF_Default` → `ECVF_Cheat` closes an inconsistency rather than making a policy.

**What does hold**, and was proved on the linked artefact rather than on source: the grant-core command
and the roster/select rule arms are **physically absent** from the Shipping binary — zero symbols from
all 24 harness translation units survive the link.

### 37. Command-line switches are not covered by the cheat-cvar regime — **OPEN**

`-ExecCmds`, `[ConsoleVariables]`, the console and `-TraceExec` are all genuinely closed in Shipping.
The hole is direct `FParse` of the command line: `-TraceSurfLegacyAirLimit` and ten further arms are
alive there. A player who can edit their own shortcut can reach them. See `reports/W8-ADVERSARIAL.md`
§2.2–2.3 for the enumeration.

### 42. Three engine-level errors on every headless boot — **OPEN, cosmetic**

Two `ConstructorHelpers::FObjectFinder` calls still name the deleted `/Game/Generated/Materials`
(`TraceCharacterInternal.h:1272-1273`), so every process logs two `LogUObjectGlobals: Error: CDO
Constructor (TraceCharacter): Failed to find …` lines — three `: Error:` lines per boot in total, once
the one long-standing unrelated engine error is counted (`reports/W8-ADVERSARIAL.md` §4.3). Harmless to
rendering — the committed material parents under `/Game/Trace/Materials/Parents/` win — but the
project's own convention, written into `Scripts/generate-data-assets.py`, is that a clean headless
start logs **one** engine error, so any wrapper that reasons about that count is now calibrated
against the wrong number.

---

## F. Test-harness caveats (read before trusting a red result)

### 12. Two harness fixtures are stale against the shipped rules — **OPEN, pre-existing**

* `Trace.Ammo.Test` expects ~4 rounds from a 1.40 s held trigger, but `bPistolFullAuto` is `false`
  (`Source/Trace/TraceSettings.h:6641`, `Config/DefaultGame.ini:545` — spec v29 §2b, one shot per
  press), so it reports 13/14.
* `Trace.Chut.ChudRefreshTest` allows 0.90 s between its press and its read and measures 8.70 s
  against an 8.75 s threshold on this machine, so it reports 12/13 while the refresh plainly fired
  (its red arm measures ~4.09 s in the same slot).

### 13. Three fixtures must not share a match — **OPEN, run them alone**

`Trace.Bounds.Verify` kills the local pawn and invalidates any fixture sharing its match: run it
**alone**, not merely apart from `Trace.Ability.CarrierChokeTest`. `Trace.Integration.Verify` and
`Trace.Ability.DeathWipeTest` also fight — `DeathWipeTest` fails beside it and passes alone.

### 38. The carrier trail's "drawn == lethal" holds except at long trail lengths — **OPEN**

The trail's geometry holds on every shape tested — hairpin, self-crossing, dash corner, climb, drop,
mixed spacing, two-point, one-point, both end caps — and the drawn centreline **is** the lethal
polyline at shipped settings. It comes apart when the trail is made much longer than it ships. The
fixture cannot see it, and when the harness does go red it names the wrong cause. Reproduction and
the exact cliff: `reports/W8-ADVERSARIAL.md` §1.

### 40. `Trace.Move.AuditV16`'s green is not repeatable — **OPEN, harness**

Same command, same binary, same map, two minutes apart: WALK top speed read `0.000 uu/s → FAIL` in one
run and `800.000` in the next; the CARRIER speed multiplier read `INVALID — no Core could be picked up`
and then `1.220`. The diagnostic sentence the harness prints on that failure is itself wrong — it says
"a velocity of 800 against a displacement of 0 is a pawn jammed in a wall" while printing velocity 0
against a displacement of 1065 uu. Pre-existing; the row is measured by driving the pawn across
whatever ground it happens to be standing on.

### 43. No census red arm has ever failed on a `role=Simulated` pawn — **OPEN, coverage gap**

The claim that all ten characters render with their own body and animation **on remote clients** rests
on nine `role=Simulated` PASS rows. Every red arm that has ever gone red did so on an `Authority` or
`Autonomous` pawn:

| arm | where run | which pawns went red |
|---|---|---|
| `BodyMeshEventsOnly 1` | listen server | 8 pawns, all `role=Authority` |
| `BodyAnimIgnore 1` | listen server | 9 anim classes, all `role=Authority` |
| `BodyMeshEventsOnly 1` | client | 1 pawn, `role=Autonomous` — the client's own |
| `BodyAnimIgnore` / `BodyMeshKeepOverrides` | **never run on a client** | — |

On a client the arm bites the client's *own* pawn, because remote PlayerStates arrive together with
their pawns — so the arm is structurally incapable of reddening a simulated pawn. The wiring was
separately confirmed correct at asset level; what is missing is that the rows carrying the remote claim
have never been demonstrated falsifiable. `Trace.Characters.BodyAnimIgnore 1` **run on a client** would
close it in one run.

---

## G. Owner-action items — documented, not executed

These need a git or config write, which the release work deliberately left to the owner.

### 4. Two tracked `.slnx` files — **OWNER**

`Trace.slnx` and `Automation_Trace.slnx` are committed (from `78b6a26`). `.gitignore` now carries a
`*.slnx` rule so no new ones can land, but untracking the existing two (`git rm --cached`) is an owner
git write.

### 5. The `prompt note files/` directory — **OWNER**

Tracked spec PDFs at the repo root, awaiting owner untracking or relocation.

### 6. The `MultiUserClient` plugin is enabled — **OWNER**

`Trace.uproject:32`. The project descriptor is on the "must not change" list for this work, so
disabling the plugin is the owner's decision.

### 7. CLOSED — the stray `Content/ramp.uasset` was removed

Filed as an owner disposition. It was removed with evidence during the kit rename:
`Scripts/rename-kit-assets.py` verified **zero referencers** before deleting it, and records that all
three ramp assets name the same source FBX with three different import hashes — this was the middle
re-export and nothing used it. It is still in git history at `78b6a26`, so
`git checkout 78b6a26 -- Content/ramp.uasset` brings it back.

### 8. `Art/Sounds/` has no provenance note — **OWNER**

**71 WAV files** now live under `Art/Sounds/` (in LFS), across `Abilities/`, `Combat/`, `Footsteps/`,
`Music/`, `UI/` and the root. There is no `SOURCE_NOTES.md` and no licence statement.
**Do not assume a licence for those files.** Generated bank material should carry its own provenance;
the pre-existing WAVs remain unattributed until the owner speaks.

### 9. Font licensing — **OWNER, and unchanged by this work**

The licensed font **files** (Sofachrome, Erbaum) are not in the repository and must never be committed;
they are gitignored, with an exception only for SIL-OFL Lato. The **baked glyph atlases**
(`T_FontAtlas*` under `Content/Trace/UI/Fonts/`) **are** committed, and whether the Sofachrome EULA
permits redistributing rasterised atlases is an **owner-level verification task that is still open**.
Typography is frozen meanwhile. `docs/FONTS.md` carries the licence text, the reasoning and the honest
caveat that a full-alphabet atlas is a greyer position than pre-rendering a fixed word list — read it
before touching anything typographic.

### 44. A re-bake of `Arena_Baked` is not free — **OWNER, and the reason surf is not in the map**

The obvious answer to item 30 is "re-bake the arena". Know the cost first: the last preserving re-bake
**lost nine hand-placed top-centre-tower pieces** contributed by a collaborator, because
`UEditorActorSubsystem::DuplicateActor` cannot work in a commandlet. They were recovered by hand and
re-created by a different route, and the recovery is verified — the census holds all 18 cluster pieces
paired one-to-one by mirror, and a collision probe at (0, −2590) reads 705.2 uu, matching its untouched
mirror control to a tenth of a unit. **Nothing is currently lost.** But five further hand-copied centre
floor lamps (`Floor_Lamp_3/4/6/7/8`) are also carried through by hand and are an open owner call, and a
re-bake rewrites all 647 external actor packages.

---

## H. Non-goals — restated so nobody "fixes" them

* **The legacy trail renderer is guarded, not deleted.**
* **Mode A (endzones) is frozen** without characters or abilities, deliberately
  (`Config/DefaultGame.ini`, the comment above the mode-A block).
* **A dedicated server target needs a source-built engine.** A Launcher-installed engine refuses it
  ("Server targets are not currently supported from this engine distribution"). The listen server is
  the supported path; `Scripts/run-dedicated-server.sh --editor` is the headless stand-in.
* **Bot goal-scoring competence** is an open design question, not a bug. Bots **can** score — a boot
  smoke caught one charging a 0.60 s throw and scoring (`[ModeB] GOAL by Blue (thrown in)`). How often
  they should, and how well they should defend, is the owner's call. See also item 34.
* **`Content/Trace/Characters/Shared/SK_TraceBodyHitDonor`** is a deliberate editor-only asset — ten
  physics assets point their `PreviewSkeletalMesh` at it, and it never cooks. Do not "clean it up".
* **The corner banks were deliberately not restored.** The census proved a collaborator replaced them
  with two 41,522 uu sideline ramps, and a restored bank's collision would sit 171 uu above the ramp
  surface. This is a resolved question, not a pending one.

---

## Windows caveat

`Config/DefaultEngine.ini` no longer carries `RayTracingMode=Full` or
`bGenerateNaniteFallbackMeshes=True`. That strip is **Mac smoke-verified only**; the Windows proof is a
collaborator's next editor launch. A Windows editor may try to re-append those lines, and the
pre-commit `Scripts/config-hygiene.py --check` hook is the tripwire — install it with
`Scripts/setup-hooks.sh` (once per clone; Git does not clone hooks).

---

*Last reconciled against the working tree on 2026-08-28.*
