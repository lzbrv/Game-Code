# Demo 17 / 18 — what is actually done, and what is not

> **POINT-IN-TIME SNAPSHOT (2026-08-12) — do not treat as current.** At least one claim below has
> since gone false: Mortimer's mantle IS implemented now
> (`Source/Trace/Abilities/Characters/TraceAbilitySetMortimer.cpp`). Verify against the live tree
> before acting on anything here.

Written 2026-08-12, after the build pass and its four adversarial verifiers
finished. **Read this before treating anything from Demo 17 or 18 as
shipped.** The commit message on `5b2996e` claims integration and
verification never ran; that was wrong — they ran, and every one of the four
verifiers returned **PARTIAL**. Their findings are below.

The build is green (`Scripts/build.sh` exit 0, jumbo-collision check clean) and
a real match plays: 8 bots on `Arena_Baked?mode=b`, goals scored by both teams,
shooting, dashing and abilities all exercised over 260 s.

---

## Landed and believed correct

| Area | Effect on the player |
|---|---|
| Elle, second gate | Pressing E twice on the spot is now refused for free instead of burning the whole 35 s on two rings inside each other. The 4 s window stays open. |
| Elle, gate entry | A gate takes you when you **step into** it. Standing in your own mouth no longer throws you across the map once a second. |
| Slimeball | Jump leaves a wall stick as a real wall jump. Holding V no longer glues you back on, and you cannot climb one wall forever. |
| Roxie | The rocket is drawn as wide as the blast that kills you (13 → 90 uu across) and glows, so it can be seen and sidestepped. It was **already** a travelling projectile — the spec's "built as an instant trace" was wrong for this build. |
| Mace | 3 s cooldown after a suspend ends, measured from the release or the expiry, never the press. |
| Core turnovers | The Core must visibly land before it counts as loose. |
| Out of bounds | Falling out of the world kills you. |
| Death | Death wipes your active effects; cooldowns keep ticking. |
| Lily | 60 health, two dash charges (three carrying), +30% wall-jump momentum. |
| Mortimer | Quarter-length dash; can hold a Core throw twice as long. |
| Practice range | `Scripts/run-practice-range.sh`. Delivered in the Arena as a game mode — no new map. |
| Rocco / X (Demo 18) | Ripple lifetime 4.0 → 5.5 s; X's vulnerable speed bonus +10% → +15%. |
| Kill-refreshes-dash | Now has a caller and works. **Off by default** — it is an experiment. |

---

## Known broken or unproven — do not report these as done

### Features that do not exist
- **Mortimer's mantle is not implemented.** `bMortimerCanMantle` and
  `MortimerMantleGenerosity` are dead knobs; `TraceAbilityTraits::IsMantleAllowed`
  has zero callers. The old implementation (137 lines in the .cpp, 36 in the .h,
  `TryBeginMantle` alone 323 lines) is in `git show dffea7c`. The integrator
  refused to restore it as a "one-line hook" because it is not one. **Deliberate,
  and still missing.**
- **Lily's Zip does not hover.** `Trace.Lily.FlightTest` **FAILS**: she climbs
  1678 uu in 1.2 s where 960 was expected, then falls all of it back while
  holding nothing. Crouch-to-descend is wired but ineffective. The obvious fix
  (`GravityScale = 0`) is the trap Mace's and Slimeball's headers already
  document — do not reach for it.
- **Lily's 60 health is not delivered until she respawns.** `GetMaxHealth()` is
  correct, but `ServerSetCharacter` never re-applies `Health`, so she measures
  100.0 on the character she starts as.

### Harnesses that cannot fail — the dangerous category
A test whose red arm and green arm agree is not measuring its rule. It will keep
printing PASS while the feature rots.

- **`Trace.Characters.Verify`** reports *14 passed / 6 failed* identically in its
  green arm **and** its red arm, at both bots=6 and bots=8. Those 6 failures mean
  nothing until the harness can discriminate.
- **Elle's step-in rule is unproven and its red arm is dead.**
  `Trace.Elle.SnapStepIn 0` returns identical counts to green. Worse, four
  assertions print PASS with the ability *switched off* (`Trace.Elle.SnapEnabled 0`),
  including "a second mouth appears" and "the two mouths are PAIRED (0 of 0)".
- **`Trace.Elle.SnapPressTest` overclaims.** Its verdict says players can "step
  through" a portal; the harness contains **no teleport assertion at all**.
- **The grounded-turnover counter is a tautology** (`TraceCore.cpp:5890-5920`),
  and its red arm did not discriminate. It also passes with **zero margin**: worst
  gap 6 uu against a slack of 6 uu, so a 6.5 uu resting gap on any new geometry
  stalls a legitimate turnover.
- **Death-wipe is proven for 1 of 10 named statuses** (Chut's Chud). cloak,
  Modded, vulnerable, poison, suspend, stick and Zip have no test and no red arm.
  Elle's gates and Slimeball's wall are deliberately *not* wiped, and nothing
  ratifies that decision.

### Real behaviour worth a look
- **`Trace.Ammo.Test`: the trigger spent 7 rounds in 1.40 s where `FireInterval`
  0.40 predicts ~3.** Exactly one round leaves the clip per shot, so this is the
  fire *rate* — or the harness reading a different knob than the firing code
  uses. Either way somebody should chase it.
- `Trace.Ammo.CarrierTest` fails 2 of 7, both fixture-staging (a reload that
  never begins). The five substantive carrier assertions pass.
- `Trace.Integration.Verify`: 6 passed, 1 failed, 2 not exercised. The failure
  fired in *neither* arm — the fixture never staged a Rocco stack.

### The menu (Demo 18) is partly the artist's art and partly not
UMG is **not** the default path. On screen from the sheet: the wordmark, the
button plates in hover/default/disabled, and the PLAY and SETTINGS words. **Not**
on screen anywhere:
- **The slider.** `T_MenuSliderTrack` and `T_MenuSliderHandle` are imported and
  referenced by nothing.
- **The KEYBIND and KEY words.** Imported and unplaced; the settings header still
  reads CONTROLS in Roboto and the key chips are flat rectangles.
- **The cursor**, except on the UMG title screen. The Canvas title screen and the
  settings overlay still draw the old vector crosshair.

**Still blocked on the font.** Demo 18 says to use the exact font, but a PNG
cannot supply a typeface, and JOIN / DIFFICULTY / MODE / QUIT and every key name
have to be rendered live. Drop a `.ttf` or `.otf` in and it is a one-constant
change.

---

## Process notes

- **`SPEC_V19.md` was missing for the whole run.** Four of six agents reported it
  independently; it lives in a scratchpad that did not survive. Everything was
  built against task-brief restatements, so §-number compliance is unverifiable.
- **`?mode=b&bots=8` silently runs MODE A.** URL options chain with `?`, not `&`.
  At least one agent's evidence may have been taken in the wrong mode.
- **A concurrent session edited `TraceCharacter.{h,cpp}` mid-run and committed the
  repository** while the integrator was told not to. That was the railgun work
  (`09e2cbf`); it broke one intermediate build with an undefined
  `BuildRailgunViewModel` symbol before settling. Nothing was lost, but the
  integrator's build evidence has that gap in it.
- Two temporary debug probes are on disk and **deliberately not committed**:
  `Source/Trace/Debug/TraceVerif6Probe.cpp` and `TraceVerifV19Probe.cpp`. They
  are verifier scaffolding and must be deleted, not shipped.
