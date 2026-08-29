// Trace — character movement with a genuinely client-predicted movement kit.
//
// Nothing in here is an RPC that plays a montage. Dash, slide and the air fast-fall are all
// first-class movement states that ride the engine's saved-move pipeline, exactly like crouch or
// jump:
//
//   1. Input calls StartDash() / SetWantsToSlide() on the owning client, which raises an intent
//      flag.
//   2. FSavedMove_Trace::SetMoveFor() snapshots that intent (plus every timer, charge counter and
//      locked direction) into the move about to be simulated.
//   3. The client simulates the move immediately — the ability starts on the same frame the key is
//      pressed, at any ping.
//   4. GetCompressedFlags() packs the intents into FLAG_Custom_0/FLAG_Custom_2, which travel to the
//      server inside the ordinary ServerMove RPC. No extra RPC, no extra bandwidth.
//   5. The server runs UpdateFromCompressedFlags() → the identical simulation → authoritative
//      result. If it disagrees it sends a correction, and the client replays its unacknowledged
//      moves through PrepMoveFor()/MoveAutonomous().
//
// Step 5 is why every timer, charge count and latched direction is saved-move state and not a
// plain member: a replay restores the character to the *start* of an old move, and if the clocks
// did not come back with it, every correction mid-ability would resimulate with the wrong remaining
// time and the client would rubber-band.
//
// Everything these abilities touch is derived from data the replay path restores (Acceleration, the
// updated component's rotation, Velocity, the saved timers), so client and server always compute
// the same answer from the same inputs. Nothing here reads wall-clock time or per-frame input state
// that the replay cannot reproduce.
//
// --- THE KIT (contract §5, as amended by spec v3 §1–2) ----------------------------------------
//
//   DASH   VECTORIZED burst along a direction composed per input axis (spec v7 §5). The strafe
//          axis is horizontal; the forward axis follows the full aim ray, pitch included, so a
//          dash CAN and now DOES add vertical velocity. See "the vectorized dash" below.
//          Runs on a CHARGE system: one charge for everybody, two while carrying the Core, each
//          charge refilling on its own DashCooldown. See "charges" below.
//   JUMP   Plain ACharacter::Jump. Horizontal velocity is never touched, in either direction.
//   CROUCH On the ground: a slide (an entry-speed momentum carry you steer weakly).
//          In the air: a fast-fall that zeroes POSITIVE Z velocity only, on the press edge.
//   AIR    Source/Quake air acceleration — see below. Not an engine feature; ours.
//
//   BOOST IS GONE (spec v3 §1). The ability, its intent flag, its saved-move field, its
//   compressed-flag bit (FLAG_Custom_1, now free) and its settings have all been deleted. Nothing
//   in this file should ever grow a "boost" again; if a vertical launch is wanted later it is a new
//   design, not a resurrection.
//
// --- SPEC v7 §5: THE VECTORIZED DASH ----------------------------------------------------------
//
// "If only A or D is held, dash horizontally only: parallel to the ground. If W or S is held, dash
// with relation to mouse aim direction (ie if the player is pointing the mouse up, dash vertically,
// or if the player is pointing 45 degrees from the ground, dash that direction). If both W or S and
// A or D is held (W and D, W and A, etc) add the two vectors and normalize to one dash length."
//
// THIS REVERSES SPEC v3 §5, which said the dash was strictly horizontal and must never add vertical
// velocity. Every `Direction.Z = 0` that enforced that rule is gone. Do not put one back.
//
// The composition, in ComputeDashDirection(), which is a PURE function of (Acceleration, aim
// rotation) and is the only place the direction is ever derived:
//
//   1. Decompose Acceleration in the AIM YAW basis. That is the exact inverse of
//      ATraceCharacter::DoMove, which builds the input as Forward·YawX + Strafe·YawY — so
//      `Fwd = Accel · YawForward` and `Str = Accel · YawRight` recover the W/S and A/D amounts,
//      analogue magnitudes included, without the character having to plumb its raw axes down here.
//   2. Re-compose against a DIFFERENT forward: `Dir = AimRay·Fwd + YawRight·Str`, where AimRay is
//      the FULL control rotation including pitch and YawRight is dead level. A/D therefore stays
//      parallel to the ground no matter where the mouse points; W/S goes exactly where it points.
//   3. Normalise. "add the two vectors and normalize to one dash length" — a diagonal dash covers
//      the same distance as a straight one, which it would not if the sum were left unnormalised.
//   4. No directional input at all → the old fallback, a level dash along the capsule's facing.
//
// AT ZERO PITCH THIS IS THE OLD CODE, EXACTLY — and that is a proof, not a hope. {YawForward,
// YawRight} is an orthonormal basis of the horizontal plane, so decomposing a planar vector in it
// and recomposing with the same two axes is the identity; at pitch 0 the aim ray IS YawForward, so
// step 2 hands back the planar Acceleration unchanged and step 3 normalises it, which is
// line-for-line what BeginDash used to do. Nothing that dashes while looking level — every bot at
// range, every player not aiming up or down — can have changed behaviour at all. The mechanic only
// moves when the mouse does.
//
// GROUND GUARD. A grounded dash may not aim INTO the floor: if the composed direction points down
// while walking it is flattened and renormalised, because the alternative is a dash that spends a
// charge and moves the pawn nowhere (look at your feet, press W). A grounded dash that aims UP
// switches to MOVE_Falling in BeginDash, or PhysWalking would simply discard the Z it was given.
//
// PREDICTION. Acceleration already round-trips: MoveAutonomous restores it from the saved move on
// every replayed frame. THE AIM ROTATION DOES NOT — FSavedMove_Character::PrepMoveFor never
// restores it, so a replay would re-derive the direction from wherever the mouse is pointing NOW.
// With a horizontal dash that was invisible (only the yaw mattered and the yaw arrived through
// Acceleration); with a vertical one it is a Z-velocity desync, i.e. the rubber-band. The fix is
// GetDashAimRotation(): during a client replay (ACharacter::bClientUpdating) it returns
// ReplayAimRotation, which FSavedMove_Trace::PrepMoveFor loads from the base class's
// SavedControlRotation — the very rotation that was packed into that move's ServerMove and that the
// server had applied to the controller before it ran MoveAutonomous. Both ends therefore compose
// the direction from the same two inputs. No new saved-move field and no extra bandwidth.
//
// THE CLIMB. A dash is velocity on rails for its window, so a vertical one rises DashSpeed ×
// DashDuration (3300 × 0.18 = 594 uu, v16 §0) with gravity suspended, and would then exit still
// carrying 3300 uu/s upward — another 4960 uu of coast at the shipped 1.12 gravity scale, well past
// the arena's 1640 uu ceiling. The air-strafe
// ceiling does NOT bound this: it is planar-only by construction and cannot touch Z. So
// ApplyDashExitSpeed() now clamps POSITIVE exit Z to GetDashExitVerticalSpeedLimit() (JumpZVelocity)
// exactly as it already clamped planar speed to DashExitSpeedMultiplier × the ground limit. Total
// climb for a straight-up dash is 540 + 640²/(2·980) ≈ 749 uu, once, then a fall — and the next dash
// is a full DashCooldown away. Downward Z is left alone: a dive is not a climb.
//
// --- SOURCE / APEX MOMENTUM MODEL (spec v3 §2) -------------------------------------------------
//
// Three rules, and they are the whole point of the movement pass:
//
//   AIR ACCELERATION is the real Quake/Source projection formula, in CalcVelocity() while falling.
//        Project the current planar velocity onto the wish direction; the input may only raise that
//        PROJECTION up to AirMaxWishSpeed, at AirAcceleration uu/s². Input can therefore only ever
//        ADD velocity along the wish direction, so input perpendicular to travel ROTATES the
//        velocity vector at (slightly more than) constant magnitude instead of braking it. There is
//        no lerp toward the input direction anywhere in this file, because a lerp is exactly the
//        thing that makes strafing cost speed.
//        There is also NO air friction: releasing the stick in mid-air coasts at full speed.
//
//        SPEC v18 §1a ADDS ONE TERM AND ONLY ONE: an OPPOSITION BRAKE, scaled by the negative part
//        of dot(wish, travel). It is exactly zero from dead-ahead round to a dead-square 90° strafe
//        — so every frame of every gaining strafe is arithmetically unchanged — and rises smoothly
//        to AirStrafeOpposingDeceleration at a full 180° reversal. See ComputeAirStrafeStep() for
//        why the reversal produced NO change at all before it, which is the reported bug.
//
//   LANDING DOES NOT CLAMP. UCharacterMovementComponent::CalcVelocity brakes hard the moment
//        `IsExceedingMaxSpeed(MaxWalkSpeed)` is true — GroundFriction 8 × BrakingFrictionFactor 2
//        plus BrakingDecelerationWalking 2600 kills 1000uu/s of carried speed in about 60ms, which
//        is the "velocity is clamped to ground max speed on landing" the spec is complaining about.
//        We defeat it by taking over CalcVelocity() whenever planar speed exceeds the ground limit
//        and bleeding the EXCESS ourselves at GroundOverspeedFriction / GroundOverspeedBraking,
//        which are deliberately an order of magnitude gentler.
//
//   TRANSITIONS PRESERVE VELOCITY. run→jump never touched horizontal velocity and still does not.
//        jump→slide enters the slide at exactly the speed the pawn landed with. slide→jump used to
//        be a hard brake — EndSlide() clamped to GetMaxSpeed() × SlideExitMaxSpeedMultiplier, i.e.
//        to the walk speed — and now cannot end a slide below the speed the slide was running at.
//        dash→ground hands back DashExitSpeedMultiplier × the ground limit instead of the ground
//        limit itself. Every one of those ceilings is a knob.
//
// --- THE SLIDE IS A MOMENTUM CARRY, NOT A BRAKE, AND NOT A BOOST EITHER -----------------------
//
// SPEC v4 §1 CLOSED THE [CONFLICT] SPEC v3 LEFT OPEN, and it closed it against the boost. Verbatim:
// "You can remove the slideexitminspeedfraction value, as well as any other part of the slide code
// contradicting the movement list. The flat momentum boost should be ruled out, going with the
// source-style movement system instead."
//
// So the rule is one sentence: YOU KEEP WHAT YOU BROUGHT IN, FRICTION BLEEDS IT, NOTHING TOPS IT UP.
//
// ENTRY is ENTRY SPEED:
//
//       SlideSpeed = max(planar speed at entry,
//                        min(planar speed × SlideEntrySpeedMultiplier, SlideMaxSpeed))
//
//       SlideEntrySpeedMultiplier is 1.0, so this reduces to "the speed you arrived with". The outer
//       max() is not a boost and cannot manufacture speed — it only stops SlideMaxSpeed BRAKING
//       somebody who arrived above the cap, which would make pressing crouch a punishment for
//       arriving fast. SlideImpulse, the flat additive that used to sit inside this expression, has
//       been DELETED.
//
// MIDDLE bleeds slowly. SlideDeceleration is the friction dial and is meant to be small enough
//       that SlideDuration, not the decay, is what ends the slide.
// EXIT  hands the speed back and NEVER TOPS IT UP. EndSlide() carries SlideExitSpeedRetention of the
//       slide's current speed into normal movement, capped at max(SlideExitMaxSpeedMultiplier ×
//       GetMaxSpeed(), the slide's own speed) — that max() is what makes "slide → jump" preserve the
//       vector instead of resetting it. There is NO FLOOR any more: SlideExitMinSpeedFraction, which
//       handed a decayed slide back at exactly WalkSpeed (measured: a 73% speed GAIN), is DELETED.
// AFTER SlideCooldownSeconds (0.8s) must elapse from the slide's END before another can start. The
//       old SlideCooldown was measured from slide START, which made "the buffer between slides" a
//       number you had to compute rather than read.
//
// SlideMinCommitSeconds makes the first moments of a slide uncancellable, so a slide reads as a
// commitment rather than a tap, and so releasing the key a frame late cannot amputate it.
//
// --- THE SLIDE-JUMP (spec v4 §1) --------------------------------------------------------------
//
// "Sliding, however, doesn't feel like it does much; is it possible to add a slide-jump mechanic,
// also attempting to feel like apex legends."
//
// With the flat boost gone this is the whole reason to slide. The slide holds a fast vector low to
// the ground while friction bleeds it slowly; the slide-jump is how you cash that vector in before
// the bleed finishes, and it is the ONE transition in the kit where the design intends a reward for
// execution. It lives in DoJump() — the engine's own predicted jump entry point — rather than in
// OnMovementUpdated, because the jump has to consume the slide on the SAME frame it launches:
//
//   1. jumping while sliding (or within the coyote window after a slide ends) is a slide-jump;
//   2. it routes the slide out through EndSlide(), like every other slide exit, so the 0.8s
//      between-slides buffer is charged exactly once and on exactly one code path;
//   3. planar speed becomes (the slide's live speed) × SlideJumpHorizontalRetention, which at the
//      shipped 1.0 is pure preservation — what it actually buys the player is escaping the ground
//      friction that would otherwise have eaten the carry;
//   4. Velocity.Z, which Super::DoJump has just set to JumpZVelocity, is scaled by
//      SlideJumpZMultiplier;
//   5. a jump taken in the last SlideJumpWindowSeconds of the slide (or in the equally long coyote
//      window straight after it ends) is WELL TIMED and additionally multiplies the retention by
//      SlideJumpWindowSpeedBonus. Missing the window never costs anything — it only declines to pay
//      the bonus. A mechanic that punished a mistimed hop would simply stop being used.
//
// bSlideJumpEnabled turns the whole thing off, so "does sliding do anything now" can be A/B'd from
// one binary.
//
// --- SPEC v5 §3: THE SLIDE IS A ONE-SHOT ABILITY NOW ------------------------------------------
//
// "Sliding still feels pretty bad. Rather than making it a slide you can hold down, have it trigger
// once, like an ability, with a hidden cooldown to prevent spamming it. Increase the multiplier
// gained by perfectly timing a jump at the end of a slide."
//
// Three consequences, all of them here:
//
//   ONE PRESS, ONE SLIDE, FIXED LENGTH. Releasing the crouch key no longer ends a slide — the only
//        exits left are the duration expiring, the decay reaching the exit speed, leaving the ground
//        (for longer than the ledge grace, see below), a dash, and a slide-jump. Holding the key
//        cannot lengthen a slide and cannot chain one either, because activation is still driven by
//        the press EDGE (bSlideHeldLastMove) and by the landing transition.
//        SlideMinCommitSeconds AND SlideCommitRemaining ARE DELETED. A partial commit window is
//        meaningless once the whole slide is committed; leaving the knob in place would have left a
//        setting in the ini that silently did nothing, which is the exact failure mode this project
//        has been bitten by.
//
//   THE COOLDOWN IS HIDDEN. SlideCooldownSeconds (0.8s, from the slide's END) is unchanged and still
//        enforced in CanStartSlide(). GetSlideCooldownRemaining() exists for bots and debug only —
//        NOTHING MAY DRAW IT. The design intent is that the player feels the rhythm rather than
//        reading a meter, so a HUD element for it is a regression, not a missing feature.
//
//   THE WELL-TIMED HOP IS WHERE THE SKILL LIVES. The window bonus was 1.10, i.e. a 10% edge that no
//        player could feel. Spec v5 raises it (1.25 shipped) and adds a SECOND, independent reward:
//        SlideJumpWindowZBonus scales the launch's vertical velocity too, so a well-timed slide-jump
//        goes measurably further AND higher than a sloppy one. Missing the window still costs
//        nothing at all — it simply declines to pay either bonus.
//
// --- SPEC v5 §1: THE AIR-STRAFE ACCUMULATION CEILING -------------------------------------------
//
// "The air strafing feels incredible, but its too powerful with how much momentum can be gained.
// I think we need a hard cap on it or an exponential scale."
//
// THE TURN IS NOT TOUCHED. Read that sentence again before editing ApplySourceAirAcceleration: the
// projection formula, the absence of air friction, and the fact that perpendicular input rotates the
// velocity vector without costing a single uu/s are all exactly as they were. What is capped is the
// MAGNITUDE GAIN — the sqrt(v² + a²) − v that each strafe frame adds — and nothing else.
//
// The implementation is one extra step at the end of the formula:
//
//   1. Compute the new planar vector exactly as before (rotated, very slightly longer).
//   2. Gain = max(0, |new| − |old|). READ THE NEXT PARAGRAPH BEFORE TOUCHING THAT max(): it is not
//      belt and braces, it is load-bearing, and for four passes its comment said the opposite.
//   3. Scale that gain by ((HardCap − |old|) / (HardCap − SoftCap))^Exponent, clamped to [0,1] and
//      equal to 1 below the soft cap. At the soft cap the strafe is worth 100%; at the hard cap it
//      is worth 0%; in between it decays as an exponential falloff, which is what "harder and harder
//      to gain momentum past a certain point" means.
//   4. Rescale the new vector to |old| + ScaledGain, KEEPING ITS DIRECTION. This is the load-bearing
//      line: the rotation from step 1 survives in full, so at the hard cap a player can still carve
//      the vector round at constant speed forever — the Source feel — but cannot add to it.
//   5. Clamp to the hard cap as a backstop, floored at the entry speed so the cap can only ever
//      remove what THIS call just added and can never brake momentum that was carried into the air
//      (a slide-jump above the cap keeps every unit of it, exactly as MaxAirSpeed always did).
//
// bAirStrafeGainFalloff and bAirStrafeHardCap are independent: either can be turned off alone, so
// "falloff only", "cap only", "both" and "neither, i.e. Demo 5 behaviour" are all one ini edit away.
// Every term is a pure function of (planar speed, config), so the whole thing replays exactly and
// adds no saved-move state.
//
// --- SPEC v18 §1a: WHY REVERSING IN THE AIR DID NOTHING, AND IT WAS STEP 2 ---------------------
//
// "When inputting solely A or D and jumping, when you go the opposite direction during the jump (if
// pressing A and jumping then letting go of A and pressing D, for example) your momentum doesn't
// change at all. We want it so doing so slows down your momentum."
//
// THE DIAGNOSIS THAT WAS IN THIS FILE WAS WRONG, and it is worth spelling out because it survived
// four passes. Step 2's comment used to read "which the projection formula guarantees is >= 0", i.e.
// the formula never brakes. It does. "Only ever ADD along the wish direction" and "only ever make
// the vector longer" are different statements, and they come apart the moment the wish direction is
// opposed to travel: adding 128 uu/s of +X onto 800 uu/s of −X gives a vector 672 uu/s long. The
// projection formula, left alone, DOES slow a reversal — hard, at the full AirAcceleration.
//
// It was step 2's max(0, ...) that erased it, and then step 4 put the speed back. Worked through at
// the shipped numbers with a 16 ms frame:
//
//     |old| = 800 (−X)   wish = +X   AccelSpeed = min(8000·0.016, 160+800) = 128
//     |new| = |(−800,0) + (128,0)| = 672
//     Gain  = max(0, 672 − 800) = 0        <-- the loss is discarded here
//     Target= 800 + 0·GainScale = 800
//     step 4: rescale the 672-long vector to 800.  IDENTICAL TO WHERE IT STARTED.
//
// That is not "barely changes". It is a bit-exact restoration of the player's momentum on every
// single frame of the reversal, which is precisely the sentence the user wrote.
//
// THE FIX IS ONE NAMED TERM, NOT A REVERTED CLAMP. Letting the raw loss through would decelerate at
// AirAcceleration itself — 8000 uu/s², a dead stop from 800 in 0.1 s — which is the "feels like
// hitting a wall" outcome the spec rules out in as many words. So the max(0,...) STAYS (which also
// keeps the gain path byte-identical) and a separate AirStrafeOpposingDeceleration is subtracted,
// scaled by the negative part of dot(wish, travel):
//
//     Opposition = max(0, −dot(wish, travel))        0 at 90°, 0.5 at 120°, 1 at 180°
//     Target    -= AirStrafeOpposingDeceleration · Opposition · InputScale · dt
//
// AT 90° AND INSIDE IT THE TERM IS EXACTLY ZERO. Not small — zero, because the dot product is. So
// every frame of every speed-gaining strafe produces the identical float it produced before this
// existed, and "do not flatten the skill ceiling" is an equality rather than a hope.
// Trace.Move.V18.AirReverse checks it as one, in both arms, off one binary.
//
// --- SPEC v5 §7 / v12 §5: THE LEDGE RUBBER-BAND (THE MANTLE IS GONE) ---------------------------
//
// Demo 5: "When jumping on the edge of a raised section, it's glitchy and feels like rubber
// banding. Add a mantle, to solve this."
// Demo 11: "Remove mantling from the game, keep wall jumping. Make sure there's no bug when a
// player hits the top edge of an obstacle."
//
// THE MANTLE HAS BEEN DELETED (spec v12 §5) AND THE DIAGNOSIS BELOW IS WHY THAT IS SAFE. The
// rubber-band was never the mantle's to fix: it was a client/server disagreement about ground
// contact at a lip, and the two things that actually fix it — PerchRadiusThreshold and the ledge
// grace — are both still here and are both untouched by the removal. The mantle was a third fix
// layered on top, and it was the only one of the three that changed where the pawn ends up.
//
// DIAGNOSIS FIRST, because "rubber banding" in a predicted game is a claim about the network and not
// about feel. The mechanism, in this kit specifically:
//
//   A capsule landing on the lip of a raised section is supported by the outer few uu of its bottom
//   hemisphere. UCharacterMovementComponent ships PerchRadiusThreshold at 0, which means NO reduced-
//   radius perch test is done at all: the pawn is "walking" while a hair of the capsule overlaps the
//   ledge, and one sub-uu difference in where the sweep landed flips the answer. Client and server
//   slice the same second into different sub-steps, so they take that coin flip on different frames.
//
//   That is ordinarily worth a few uu. HERE IT IS WORTH HUNDREDS OF uu/s, because this component
//   attaches a completely different velocity model to each side of the flip:
//
//       IsFalling()       -> ApplySourceAirAcceleration: no friction, input adds speed
//       IsMovingOnGround()-> ApplyGroundOverspeedBleed / Super: friction, braking, speed removed
//
//   plus EndSlide() on leaving the ground (which rewrites Velocity and charges the 0.8s buffer), the
//   fast-fall (which zeroes Velocity.Z on a press edge only while airborne), and the landing
//   transition that charges the slide buffer. A one-frame disagreement about ground contact makes
//   client and server run different code, and the position error compounds until the server
//   corrects. THAT is the rubber-band, and a mantle bolted on top would not have removed it.
//
// THE TWO FIXES THAT REMAIN, AND THEY ARE THE ONES THAT MATTER:
//
//   1. PerchRadiusThreshold, set in the constructor. Gives the perch test a real band to decide in
//      instead of a knife edge, so the walking/falling answer at a lip is stable and both ends reach
//      it from the same geometry.
//   2. LEDGE GRACE (GroundGraceRemaining), saved-move state. The ability layer treats "on the ground
//      within the last LedgeGroundGraceSeconds" as grounded, so a one-frame contact blip can no
//      longer end a slide, fire a fast-fall or fake a landing. It deliberately does NOT touch the
//      engine's own physics mode — only which of this file's branches run — so it cannot change
//      where the pawn is, only stop the kit from disagreeing about it.
//
// NEITHER OF THOSE IS ALLOWED TO BE DELETED AS "MANTLE CODE". GetLedgeGroundGraceSeconds() reads a
// knob that still ships, GroundGraceRemaining still round-trips through the saved move, and
// PerchRadiusThreshold is still assigned in the constructor. They were written in the same pass as
// the mantle and they are indexed under the same spec section, which makes them the obvious thing
// to sweep out alongside it; sweeping them out is what would hand the Demo 5 complaint back.
//
// --- THE THIRD FIX, WHY IT IS GONE, AND WHAT MEASUREMENT SAYS ABOUT IT --------------------------
//
// The mantle was an automatic MOVE_Flying pull-up onto any reachable ledge, attempted on every
// airborne move. It is deleted in full: the ability, its six pieces of saved-move state, its eight
// tuning knobs, its Trace.MantleDebug CVar and the "a wall jump outranks a mantle" priority rule
// that only existed to stop it eating wall jumps (spec v9 §5). None of it is behind a switch; a
// disabled mantle would be a dead knob, and this project has shipped several.
//
// HOW THAT WAS CHECKED RATHER THAN ASSUMED. -TraceLedgeTest is the rig, and spec v12 §5 made it
// answer the question Demo 5 never did. It now runs the LIP case — a block SHORTER than the jump
// apex, so the pawn lands on the top edge instead of climbing the face — and reports, per contact
// and on the client that experiences it:
//
//   flips     ground-state changes during the contact window. 2 is the floor (jump, land). Every
//             extra one is the capsule oscillating on the lip, and every oscillation is a frame on
//             which client and server can pick different velocity models.
//   corr      server corrections that land inside the contact window, with their position error.
//             This is the number that says "prediction desync" or "no prediction desync", and it is
//             the number Demo 5 should have produced before anyone wrote a mantle.
//   keptPct   planar speed on the far side of the lip as a fraction of the speed at the jump. This
//             is "stall" and "pulled back" made countable.
//
// WHAT IT MEASURED. Two arms, ONE BINARY, same arena ledge (a TraceArenaBuilder cover box, top at
// 176 uu, against a 187 uu jump apex — it clears by 12.7 uu, which is why landing on its edge is the
// common case), same 40 ms client, same two jump distances (251 uu and 393 uu from the face), same
// contact count. The ONLY difference is PerchRadiusThreshold and the ledge grace:
//
//   DEMO 5 ARM (-TraceLedgeLegacy: perch 0, grace 0 — the state the complaint was made about)
//       flips/contact 2.50 (worst 3) | corr/contact 1.50, worst error 86.05 uu | kept 0.762 (worst
//       0.525 — 800 uu/s in, 420 uu/s out)
//       A longer eight-contact session in the same arm: flips 3.12 (worst 4), corr/contact 0.75 over
//       6 corrections, worst error 115.15 uu, worst retention 0.542.
//
//   SHIPPED ARM (perch 15, grace 0.08 — with the mantle deleted)
//       flips/contact 2.00 (worst 2) | corr/contact 0.00, worst error 0.00 uu | kept 1.000
//       Eighteen further contacts logged across other sessions: every one flips=2, corr=0, 0.00 uu.
//
// SO THE ANSWER TO "IS THERE A BUG AT THE TOP EDGE ONCE THE MANTLE IS GONE" IS NO, AND IT IS AN
// ANSWER WITH A CONTROL. The desync Demo 5 described is real and reproducible — 86 to 115 uu of
// correction and up to 48% of the player's speed, on a lip, on a client. It is removed by
// PerchRadiusThreshold and the ledge grace, NOT by the mantle: the mantle was absent from both arms
// above and the red arm is still red. That is the evidence that deleting it does not hand the bug
// back, and it is the measurement Demo 5 should have had before a mantle was written.
//
// CanAttemptJump() IS OVERRIDDEN FOR THIS, and it is not optional. The engine's version refuses to
// jump whenever bWantsToCrouch is set — a sane rule in a game where crouch shrinks the capsule and
// you might not have headroom to stand up. Here crouch NEVER resizes anything (see
// CanCrouchInCurrentState) and is the slide key, so that rule silently made the slide-jump
// impossible for any human player: ATracePlayerController drives crouch through ACharacter::Crouch(),
// which sets bWantsToCrouch, so CanJump() was false for the entire slide. The dev measurement
// harness did not catch it because it drives SetWantsToSlide() instead.
//
// --- SPEC v8 §7: THE WALL JUMP ------------------------------------------------------------------
//
// "Can you add a wall jump mechanic, where players can press jump right as they hit a wall to carry
// momentum in a new direction?"
//
// CARRY, NOT RESET. The whole request is in the second half of that sentence, so the launch is a
// REFLECTION of the incoming planar velocity about the wall normal — a player who arrives at 1400
// uu/s leaves at 1400 × WallJumpSpeedRetention in a new direction, not at walking pace with a fresh
// jump arc. Only the component that was travelling INTO the wall is mirrored; a glancing approach
// glances off, a head-on approach comes straight back.
//
// DETECTION IS FREE, AND THAT IS DELIBERATE. There is no per-frame wall probe: the wall contact is
// taken from UCharacterMovementComponent::HandleImpact, which PhysFalling already calls for every
// blocking hit that is not a landing spot. So the mechanic costs one virtual call on a frame the
// engine was already doing the sweep for, it fires on exactly the frame the capsule touched the
// wall, and — the load-bearing part — it is reproduced identically by the server and by every
// replayed move, because PhysFalling runs on all three and the arena's walls are static geometry.
// A hand-rolled probe would have been a second, differently-timed source of truth for the same fact.
//
// PREDICTED LIKE THE MANTLE. WallJumpNormal, WallJumpWindowRemaining and WallJumpsSinceGround are
// all saved-move state, round-tripped through Clear / SetMoveFor / PrepMoveFor / CombineWith and
// refused by CanCombineWith while the window is open. A correction that landed mid-window and lost
// them would replay a wall jump as an ordinary refused jump — a several-hundred-uu/s disagreement on
// the most visible frame of the move, which is the exact failure this project keeps paying for.
//
// THE ENGINE HAD TO BE TOLD TO ASK. ACharacter::CheckJumpInput never calls DoJump() in mid-air once
// JumpCurrentCount has reached JumpMaxCount, and JumpMaxCount is 1 — so with the stock value the
// wall jump could not have been reached at all, whatever DoJump did. RefreshEngineTunablesFromSettings
// raises JumpMaxCount to 1 + WallJumpMaxConsecutive so the engine ASKS on every mid-air press, and
// DoJump() answers false unless a wall window is genuinely open. That is why this is not a double
// jump: the extra counts buy the question, not the jump.
//
// THE LADDER IS CAPPED. Two close walls are an infinite staircase without a limit, so
// WallJumpsSinceGround counts consecutive wall jumps and is reset only by touching the ground
// (through IsGroundedForAbilities(), like every other ground test in this file, so a ledge blip
// cannot silently refill it on one machine and not the other).
//
// IT CANNOT BEAT THE AIR-STRAFE CAP (spec v5 §1). The launch's planar speed is clamped to
// max(entry planar speed, AirStrafeHardCapSpeed) — the same rule ApplySourceAirAcceleration uses, so
// the wall jump can carry speed that was already above the cap but can never ADD past it.
//
// PRIORITY: the slide-jump wins. A jump taken inside the slide-jump coyote window is still a
// slide-jump even if a wall window happens to be open, so nothing about spec v4 §1 or v5 §3 changes.
// The wall jump is only reached by a jump that is airborne, not a slide-jump, and next to a wall.
//
// --- SPEC v10 §5: WHY IT WAS STILL STICKY, AND WHAT WAS ACTUALLY WRONG ---------------------------
//
// "Wall jumping still feels like the player is sticking to the wall for a moment too long." Same
// words as v9, after v9 had already cut the window 0.25 -> 0.15 s and stopped the mantle stealing the
// press. So the remaining cause was NOT the window, and shaving it again would have been the third
// pass in a row spent on the wrong number.
//
// The stick was measured rather than guessed — see the STICK METER block further down this header for
// the definition, and RUN -TraceWallJumpTest to reproduce it. It is the milliseconds from the frame
// the capsule touches a wall to the frame the pawn is genuinely clear of it. Two causes fell out, and
// neither one is a tuning value:
//
//   CAUSE 1 — THE PLAYER'S OWN INPUT PULLS THEM BACK ONTO THE WALL, AND IT IS NOT CLOSE.
//     A player who just ran into a wall is still holding the stick into it — that is how they got
//     there — so ApplySourceAirAcceleration spends its entire per-frame allowance cancelling the
//     launch. Peak separation is v_out² / 2a, where a is AirAcceleration (8000 uu/s²) projected onto
//     the wall normal and v_out is the launch's outward component. Put the shipped numbers in:
//
//       FIRST wall jump of a chain, off a 1100 uu/s ground run, head on:
//         v_out = 1100 × 0.7695 + 360 = 1206 uu/s  ->  peak 91 uu. It escapes. This is the one
//         every previous measurement looked at, and it is the one case that was never broken.
//
//       EVERY WALL JUMP TAKEN FROM THE AIR — i.e. the second and third of a chain, which is how
//       players actually use the mechanic:
//         AirMaxWishSpeed is 160, and it is a hard cap on speed along the wish direction. So a pawn
//         returning to a wall under its own air control arrives at AT MOST 160 uu/s into the face.
//         The reflection therefore contributes at most 160 × 0.7695 = 123 uu/s and
//         WallJumpOutwardImpulse (360, flat — v16 §0 cut it from 420) is essentially the whole
//         launch:
//         v_out ≈ 483 uu/s  ->  peak 483² / 16000 = 15 uu. AGAINST A 34 uu CAPSULE RADIUS.
//         The pawn never gets half a capsule off the face before it is being driven back into it.
//
//     That is the stick. It is invisible to any amount of window tuning because it happens entirely
//     AFTER the launch, and invisible to a head-on-only harness because the head-on first jump is
//     the case that works.
//     THE FIX: for GetWallJumpControlLockoutSeconds() after a launch, the air-strafe wish direction
//     has its INTO-THE-LAUNCHED-FACE component projected out. Tangential and outward input are
//     untouched and accelerate at full strength, so the Source strafe survives intact — the player
//     may steer the launch anywhere except back into the wall they just left.
//
//   CAUSE 2 — A PRESS MADE ONE TICK EARLY IS SILENTLY EATEN.
//     ACharacter::JumpMaxHoldTime is 0, so bPressedJump survives exactly one tick. CheckJumpInput
//     runs at the START of PerformMovement; HandleImpact — the only thing that opens a wall window —
//     runs DURING the physics step after it. A press on the tick before contact is therefore handed
//     to a DoJump that correctly refuses it, and is then discarded. The player pressed jump "right as
//     they hit the wall", got nothing, and is left scraping down the face until they press again. v9
//     made this MORE likely, not less: a 0.15 s window means pressing early is now the easy mistake.
//     THE FIX: a refused mid-air press is remembered for GetWallJumpInputBufferSeconds() and consumed
//     by OnMovementUpdated the instant a window opens — which is the contact frame itself, one whole
//     frame earlier than CheckJumpInput could ever deliver it.
//
// BOTH ARE PREDICTED. WallJumpLaunchNormal, WallJumpControlLockoutRemaining and
// WallJumpInputBufferRemaining are saved-move state, round-tripped through Clear / SetMoveFor /
// PrepMoveFor and refused by CanCombineWith, exactly like the window they extend. The lockout changes
// the velocity a replayed frame produces and the buffer changes whether a launch happens at all, so
// either one left out of the saved move would be a several-hundred-uu/s rubber-band on the most
// visible frame of the move.
//
// AND THE 10% the spec also asks for is applied to the RETENTION (WallJumpMomentumScaleV10), not to
// the outward impulse. The impulse is the pawn's escape velocity from the face, and cause 1 above is
// the finding that it was already too weak to do that job — cutting it would have made the very
// symptom the same sentence complains about measurably worse.
//
// --- AS MEASURED. READ THIS BEFORE BELIEVING ANY OF THE ABOVE. ----------------------------------
//
// THE TWO CAUSES ABOVE ARE A DERIVATION, NOT A RESULT, AND THE PAIRED RUN DID NOT CONFIRM THEM.
// The first paired run was invalid: the harness pressed once per AIRBORNE APPROACH, so it never
// chained, and maxConsecutive was 1/2 in both arms — every sample was the first jump off a ground
// run, which CAUSE 1 itself names as the case that was never broken. That is fixed (see
// WallJumpTestLastChainCount) and the re-run is matched and chained, 9 head-on + 4 chained samples
// per arm, maxConsecutive 2/2 in both. Result, RED (v9 behaviour) -> GREEN (this fix):
//
//     HEAD-ON   63.6 -> 65.1 ms      GLANCING  75.7 -> 71.4 ms      CHAINED  54.0 -> 56.3 ms
//
// Mixed sign, 2-4 ms on samples of 2 to 9, and neverCleared was 0 of every sample in BOTH arms —
// the pawn always got off the wall, in both builds, in under 80 ms. THE FIX DOES NOT MOVE THE
// NUMBER. Do not repeat the v9 mistake of reading the paragraphs above as a confirmed fix.
//
// TWO THINGS ARE STILL UNTESTED, AND ONE OF THEM IS HALF THE DIAGNOSIS:
//   * The EARLY press fired ZERO times in both arms of the re-run ("presses early= 0" on every
//     line), because approach parity stops alternating once the pawn stays airborne and chains. So
//     the input buffer — the whole of CAUSE 2 — was never exercised. It is written, predicted and
//     unmeasured. Make the press rule alternate per PRESS rather than per approach before claiming
//     anything about it.
//   * peakOut is censored by construction: a sample CLOSES at WallStickClearUU (50 uu), so the
//     column can never show the 18 uu that CAUSE 1 predicts for the air-initiated case. It reads
//     ~54-58 uu in every cell of both arms because that is where the meter stops looking. To test
//     the derivation, raise the clear threshold or record peak separation on a sample that is
//     allowed to run past it.
//
// What IS confirmed by the run: the -10% retention landed (0.8550 -> 0.7695 in the live MOVECFG-V10
// dump), the lockout and buffer are live in the green arm (0.20 s / 0.12 s), and the change costs
// nothing in prediction — corrections in wall jump = 0.000 per jump on both arms.
//
// --- SPEC v10 §6: NO SHOOTING DURING A DASH -----------------------------------------------------
//
// AreWeaponActionsBlocked() is a thin alias of IsDashing() and deliberately nothing more: the spec's
// "as soon as they end the dash, let them shoot again" forbids any cooldown, so the gate has to be a
// pure function of DashTimeRemaining, which OnMovementUpdated drives to 0 on the frame the window
// closes. See the accessor for where the answer is valid and where it is not.
//
// --- SPEC v10 §1: THE KNIFE'S MOVEMENT PROFILE --------------------------------------------------
//
// "Players should move 30% faster with a knife, as well as have a higher momentum ceiling."
//
// One bit, bKnifeMovementProfile, set from the melee slice through SetKnifeMovementProfileActive()
// and read in four places: GetMaxSpeed() (ground speed × 1.30) and the three air ceilings
// (GetAirStrafeSoftCapSpeed, GetAirStrafeHardCapSpeed and — necessarily — GetMaxAirSpeed, because
// ApplySourceAirAcceleration takes the tighter of the last two and a raised hard cap under an
// unraised MaxAirSpeed would be inert). All four are MULTIPLIERS over the base values, so the knife
// stays 30% faster than whatever the gun is retuned to.
//
// The bit is saved-move state. It is SET from outside the move pipeline, like the carrier bit, and
// carries the same bounded one-RTT seam documented for that — but restoring it per move is what stops
// a correction from replaying an entire pre-swap move at post-swap speed.
//
// --- PATCH 28 §5: SOURCE SURF ON CURVED RAMPS --------------------------------------------------
//
// "Players should be able to accelerate using curved ramps, in accordance with source movement
// standards (kind of like surfing in CS:GO)."
//
// WHAT SOURCE SURF ACTUALLY IS, because it is not "a slide with the friction turned off". On a face
// STEEPER THAN THE WALKABLE LIMIT the player is not grounded at all — they stay in the air state,
// and every tick their velocity is CLIPPED AGAINST THE SURFACE PLANE instead of being stopped by it:
//
//     v' = v - n * (v·n) * overbounce            (Source's PM_ClipVelocity, overbounce 1)
//
// That single line is the whole mechanic. It removes the component going INTO the plane and leaves
// everything along it untouched, so gravity's along-plane component keeps accelerating the player
// down the slope while the plane's normal force does no work at all. Combine that with the air
// strafe already in this file — accelerating perpendicular to travel adds speed because the cap
// applies to the PROJECTION of velocity onto the wish direction — and a player can hold, steer and
// grow a very large vector on a ramp they could never have walked on.
//
// UE DOES THE OPPOSITE BY DEFAULT, AND THE CULPRIT IS ONE FUNCTION.
// UCharacterMovementComponent::ComputeSlideVector calls the base class's plane projection — which is
// already exactly ClipVelocity at overbounce 1 — and then, while falling, hands the result to
// HandleSlopeBoosting(). That function exists to stop players "boosting up slopes": it refuses to
// let a deflection carry the pawn higher than the un-deflected move would have, and when the
// original move was going DOWN it zeroes the deflection's vertical part outright and flattens the
// remainder. On a surf ramp that is precisely the frame that matters — a pawn falling onto a steep
// face is deflected up-and-along, which is the surf — so the stock component converts every surf
// contact into a horizontal scrape and the ramp reads as sandpaper.
//
// So THE FIX IS A SUBTRACTION, not an addition: ComputeSlideVector() is overridden to skip
// HandleSlopeBoosting on a SURF PLANE and hand back the honest plane projection. Everything else —
// walls, walkable floors, the two-wall crease, ledges — still goes through Super and is byte-for-byte
// unchanged. PhysFalling then does the rest for free: it already re-derives Velocity from the
// deflected delta (`Velocity = Delta / subTimeTickRemaining`), so clipping the delta IS clipping the
// velocity, on the client, on the server and on every replayed move, inside the sweep the engine was
// doing anyway. No per-frame probe, no second source of truth, exactly like the wall jump's sensor.
//
// WHAT COUNTS AS A SURF PLANE — IsSurfPlane(), and the test is the reason "you cannot surf ordinary
// geometry" is a proof rather than a hope:
//
//     GetSurfMinNormalZ() < Normal.Z < GetWalkableFloorZ()
//
// The upper bound READS THE LIVE ENGINE WALKABLE LIMIT. It is not a copied 0.71 literal, so if a
// designer ever changes the slope limit the surf band tracks it and the two can never disagree about
// what "walkable" means (this project's standing DEMO 21 rule, applied to a threshold instead of to a
// damage number). The lower bound keeps near-vertical faces out: those are the wall jump's, and
// letting them be surf planes would hand a player a corner to climb.
//
// NO GROUND FRICTION IS FREE, AND THAT IS THE POINT OF LEAVING THE ENGINE'S STATE MACHINE ALONE.
// IsValidLandingSpot() already refuses to land on an unwalkable face, so a surfing pawn is MOVE_Falling
// for the entire ride: CalcVelocity takes the AIR branch (ApplySourceAirAcceleration — the strafe),
// PhysWalking's friction and braking are not running at all, and the moment the ramp flattens out
// into walkable ground the pawn lands normally and the existing landing-momentum model (spec §2.2)
// carries the speed.
//
// *** "THE EXIT THEREFORE NEEDS NO CODE" IS WHAT THIS PARAGRAPH USED TO SAY, AND THE OWNER PLAYED IT
// *** AND SAID OTHERWISE: "when sliding down the curved surfaces, a player loses all momentum at the
// *** end of the curve. This is extremely unintuitive. A player should carry momentum from a curve
// *** down onto the flat floor."  DEMO 29 ITEM 4 measured it (-TraceSurfExitTest) and the sentence was
// *** wrong in three separate ways at once:
//
//   1. THE FAST LANE ENDED IN A WALL. A surfer does not stop at the end of a rail, they leave the
//      nose as a projectile — and the flight landed inside the innermost approach cover 1300 uu
//      further on. Measured: 1469 uu/s on the last frame of the ride, 52 uu/s on the floor. Patch 28
//      could not see it because its rig closed the sample the frame the SURF STATE closed, which is
//      in mid-air several hundred uu before the impact. Fixed in the ARENA, by deriving the rail's
//      far end from ATraceArenaBuilder::SurfRailExitClearance(), which asks GetSurfExitReach() here.
//   2. THE LANDING DELETED THE DESCENT. MaintainHorizontalGroundVelocity() throws the vertical
//      component away, and on a 47-61 degree face most of a descent is vertical. A real curve whose
//      tangent reached horizontal would keep it; our band's shallow end IS the walkable limit, so the
//      geometry is not allowed to flatten and the arc meets the floor at a kink. ProcessLanded()
//      models the missing piece of curve — capped by the ride's own speed, so it cannot invent any.
//   3. THE FLOOR TOOK THE REST INSIDE A SECOND. The overspeed bleed starts the frame the pawn lands;
//      a 1140 uu/s arrival was back at the 800 uu/s walk limit 0.75 s and 912 uu later.
//      SurfExitCarryRemaining holds the bleed off for GetSurfExitCarrySeconds() so the speed is
//      carried across the lane rather than surrendered at the bottom of the ramp.
//
// The air ceilings are still all floored at SpeedBefore, so nothing clamps a fast exit on the way
// out; that half of the old paragraph was true and still is.
//
// NO STAIR-STEPPING ONTO THE FACE, AND *** THE REFUSAL IS IN StepUp(), NOT IN CanStepUp() ***. Patch
// 28 refused in CanStepUp() and that was the second half of DEMO 29 item 4: MoveAlongFloor's recovery
// is `if (CanStepUp(Hit) || ...) { StepUp(); if (!stepped) { HandleImpact(); SlideAlongSurface(); } }
// else if (!Hit.Component->CanCharacterStepUp(...)) { HandleImpact(); SlideAlongSurface(); }`, and the
// rails' boxes DO allow CanCharacterStepUp — so refusing at the top took NEITHER branch. No impact, no
// slide, no movement, and PhysWalking then re-derived Velocity from the zero displacement. The four
// biggest structures Patch 28 added were flypaper: measured at 90, 60, 45, 30 and 20 degrees of
// approach, a pawn running at a rail at the 800 uu/s ground limit ended at 0 uu/s, every time.
//
// The guarantee is unchanged — StepUp() refuses a surf plane with the same one dot product, before any
// sweep, and Super's own down-sweep would reject an unwalkable landing above the start anyway. What
// changed is that a failed step-up is a case MoveAlongFloor knows how to recover from.
//
// AND A WALKING PAWN THAT LEANS INTO A SURF PLANE NOW STARTS A RIDE. See HandleImpact(): above
// GetSurfGroundEntryMinApproachSpeed() of INTO-THE-FACE speed the pawn leaves the ground and its
// velocity is clipped against the plane — the same two operations PhysFalling performs on a surfer,
// in Source's own order. That is the owner's "it still doesn't feel like you can surf INTO
// curves/curved ramps in order to gain momentum", and after it 5 of 5 approach angles gain speed
// (800 -> 1001..1089 uu/s) where 0 of 5 did before.
//
// THE BOUND, AND WHY THERE HAS TO BE ONE. Gravity along the plane is a constant acceleration and the
// clip never removes any of it, so on a long enough ramp the speed is unbounded — which is the one
// thing spec v5 §1 ("the air strafing feels incredible, but it's too powerful") already ruled out for
// the air model. GetSurfSpeedCeiling() bounds it, and it is DERIVED rather than typed:
//
//     ceiling = GetAirStrafeHardCapSpeed() x SurfSpeedCeilingMultiplier
//
// so it tracks the air cap (knife profile included — that accessor already folds in the knife
// multiplier) and cannot drift from it. Applied as max(ceiling, SurfEntrySpeed) exactly like every
// other ceiling in this file, so arriving fast is never punished; it only stops the ramp MAKING more.
//
// PREDICTION. SEVEN fields ride the saved move — Patch 28's five plus DEMO 29's two — and FOUR of them
// are load-bearing for the simulation rather than for the readout. Patch 28's two first:
//   * SurfContactRemaining is what IsSurfing() answers from, and IsSurfing() gates the speed ceiling.
//     A replay that lost it would run an uncapped frame the server capped.
//   * SurfEntrySpeed IS the ceiling's floor. A replay that lost it would clamp a fast entry down to
//     the shared ceiling on one machine only — several hundred uu/s on the most visible frame of the
//     ride, which is the exact class of rubber-band the rest of this file exists to prevent.
// SurfPlaneNormal is tested by IsSurfing() (a zero normal means no surf, exactly as WallJumpNormal
// works), and the remaining two are carried because a move's snapshot is either complete or it is a
// trap for the next person.
//
// Then DEMO 29 item 4's two, and the first of them is the only entry in this whole block that matters
// while the pawn is ON ITS FEET:
//   * SurfExitCarryRemaining decides whether ApplyGroundOverspeedBleed() runs on a grounded frame at
//     all. A replay that lost it bleeds a frame the server held, and the two ends part company by
//     several hundred uu/s within a handful of frames, on the ground, in plain view.
//   * SurfExitSpeed is the cap on the exit rollout. It is read from a frame that is already in the
//     past by the time the landing happens, so nothing in a replay can re-derive it; losing it lands
//     the client flattened where the server landed rolled out.
//
// CanCombineWith refuses any merge across a live surf, and across a live exit carry, for the reason it
// refuses one across the air model: both are per-sub-step clamps, so f(2dt) != f(dt) twice.
//
// THE CLIP IS A PUBLIC STATIC PURE FUNCTION (ClipVelocityAgainstPlane) for the same reason
// ComputeAirStrafeStep() is: "the velocity is clipped, not stopped" is a claim about arithmetic, and
// Trace.Move.Surf drives the SHIPPED arithmetic with synthetic vectors rather than a harness-local
// copy of it. This project has already been bitten by a harness holding a stale copy of a rule.
//
// GEOMETRY. Flat-faced ramps cannot show any of this, so ATraceArenaBuilder::BuildSurfRails() adds
// four curved surf rails to the procedural arena — see that function for the level-design argument
// and for the fact that its face angles are DERIVED from this component's walkable limit rather than
// typed next to it. DEMO 29 item 4 added a second derived quantity in the same spirit: the rail's FAR
// END is placed from GetSurfExitReach() so the lane a surfer is thrown into is clear of cover, which
// is the same rule ("ask the movement component, do not assume") applied to a clearance.
//
// --- WHY CHARGES AND NOT A SECOND TIMER -------------------------------------------------------
//
// The Core carrier gets an extra dash. Modelling that as "a second cooldown that only carriers
// have" needs a rule for what happens to the second timer when you gain or lose the Core mid-
// cooldown, and every answer to that is arbitrary. A charge pool has one obvious answer: the pool
// grows by one when you pick the Core up (and the new charge is available immediately, which is the
// point of the mechanic) and is clamped back down when you lose it. The refill timer never has to
// know how many charges exist.
//
// NOTE ON THE ONE UNAVOIDABLE PREDICTION SEAM. GetMaxDashCharges() reads ATraceCharacter::
// IsCarrier(), which is replicated state, not saved-move state — the client learns it up to half an
// RTT after the server does. For that window the two ends can disagree about the size of the pool,
// exactly as they already disagree about GetMaxSpeed()'s carrier multiplier. The consequence is
// bounded: a client that spends a second charge the server does not yet believe in gets one
// correction, and the counts reconverge, because the pool is resized from a *transition* in the
// carrier bit rather than recomputed from scratch. It cannot drift permanently.
//
// --- WHY THE MOMENTUM MODEL ADDS NO SAVED-MOVE STATE ------------------------------------------
//
// Air acceleration and the ground overspeed bleed are both PURE FUNCTIONS of (Velocity,
// Acceleration, DeltaTime, config). Velocity and Acceleration are already restored by
// FSavedMove_Character::PrepMoveFor / MoveAutonomous on every replayed frame, so a correction
// replays them exactly. That is deliberate: "retained velocity" is not a new variable to keep in
// sync, it is just Velocity, which the prediction system has always round-tripped. The only new
// saved field is one bit, bSavedMomentumActive, and it exists purely to stop the client MERGING two
// moves across an air-accel or overspeed frame, where the per-move clamps make the simulation
// non-linear in dt and one long move would not equal the two short ones it replaced.
//
// THAT IS TRUE OF THE MOMENTUM MODEL AND IS NOT TRUE OF SURF, which is a separate feature added by
// Patch 28 §5 and extended by DEMO 29 item 4, and DOES carry seven saved fields. The distinction is real rather than an inconsistency:
// the momentum model is a pure function of Velocity and Acceleration, both of which the replay path
// already restores, while surf's speed ceiling is floored at the speed the RIDE began with — a fact
// about the past that nothing in a replay can re-derive. See the Patch 28 block above.
//
// --- READING SETTINGS THIS SLICE DOES NOT OWN --------------------------------------------------
//
// The new knobs (air, momentum, slide entry/impulse/cooldown) are now plain UTraceSettings
// properties, read directly by the small accessors at the top of the .cpp (GetAirAcceleration,
// GetAirMaxWishSpeed, GetMaxAirSpeed, and so on). They were briefly behind a compile-time detection
// shim while TraceSettings.h belonged to another slice mid-pass; the properties landed and the shim
// collapsed, as intended. Every one of those accessors clamps on read, so nothing in this file
// touches UTraceSettings::Get() directly — go through them, and a bad ini cannot break the model.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "TraceCharacterMovementComponent.generated.h"

class ACharacter;

UCLASS()
class TRACE_API UTraceCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

	// Needs to read/write the ability state directly when snapshotting and restoring moves.
	friend class FSavedMove_Trace;

public:
	UTraceCharacterMovementComponent();

	virtual void BeginPlay() override;

	// --- Prediction pipeline -------------------------------------------------------------------

	virtual FNetworkPredictionData_Client* GetPredictionData_Client() const override;

	/**
	 * Runs on the server for every ServerMove, and on the owning client for every replayed move.
	 * Pure state restore — never trigger gameplay side effects from here.
	 */
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;

	/**
	 * THE MOMENTUM MODEL LIVES HERE, not in OnMovementUpdated.
	 *
	 * CalcVelocity is called by PhysFalling and PhysWalking from *inside* the physics step, once per
	 * sub-step, with that sub-step's delta and with Velocity.Z already stripped. That is the only
	 * place where a velocity change actually moves the pawn on the same frame it is computed;
	 * anything written in OnMovementUpdated lands a frame late. It is also called identically on the
	 * client, on the server and on every replayed move, so overriding it is prediction-safe.
	 *
	 * Two branches take over from the engine, both of them stateless:
	 *   FALLING  → Source/Quake air acceleration, no air friction.
	 *   WALKING while planar speed exceeds the ground limit → our own gentle overspeed bleed, so
	 *              landing carries momentum instead of being clamped.
	 * Everything else falls through to Super.
	 */
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;

	/** Where the ability kit is simulated. Called once per move, on every machine. */
	virtual void OnMovementUpdated(float DeltaSeconds, const FVector& OldLocation, const FVector& OldVelocity) override;

	virtual float GetMaxSpeed() const override;

	/**
	 * ALWAYS FALSE, and that is load-bearing.
	 *
	 * The crouch key arrives as ACharacter::Crouch(), which only sets bWantsToCrouch if the movement
	 * component reports CanEverCrouch() — so NavAgentProps.bCanCrouch is enabled in the constructor
	 * to let that flag (and its FLAG_WantsToCrouch round-trip) through. But bWantsToCrouch is being
	 * used here as an INPUT, not as a request to shrink the capsule: this override is what stops
	 * UpdateCharacterStateBeforeMovement from calling UCharacterMovementComponent::Crouch() and
	 * resizing it.
	 *
	 * The capsule must not move. It is this project's single source of truth for hitscan resolution,
	 * for the lag-compensation pose history the server rewinds, and for the trail trip test. A slide
	 * that silently halved the pawn's hit height would change all three, on the server only, in the
	 * middle of the one mechanic the game is about.
	 */
	virtual bool CanCrouchInCurrentState() const override;

	/**
	 * THE SLIDE-JUMP LIVES HERE. See the header note.
	 *
	 * DoJump is the engine's predicted jump entry point: ACharacter::CheckJumpInput calls it from
	 * inside PerformMovement, on the client, on the server and on every replayed move, driven by
	 * bPressedJump — which is already saved-move state (FLAG_JumpPressed). Putting the slide-jump here
	 * rather than in OnMovementUpdated is what lets the jump consume the slide on the same frame it
	 * launches, and gets the whole thing predicted for free.
	 */
	virtual bool DoJump(bool bReplayingMoves, float DeltaTime) override;

	/**
	 * DROPS THE ENGINE'S "!bWantsToCrouch" CLAUSE, and that is the entire point of the override.
	 *
	 * UCharacterMovementComponent::CanAttemptJump() refuses to jump while bWantsToCrouch is set. That
	 * is correct for a game where crouch shrinks the capsule — you may not have the headroom to stand
	 * back up. It is wrong here: CanCrouchInCurrentState() is hardwired to false, the capsule never
	 * resizes, and the crouch key is the SLIDE key. Left alone it made the slide-jump unreachable for
	 * every human player, because ATracePlayerController slides through ACharacter::Crouch() and that
	 * sets bWantsToCrouch for the whole slide.
	 */
	virtual bool CanAttemptJump() const override;

	/**
	 * SPEC v8 §7. THE WALL JUMP'S ONLY SENSOR.
	 *
	 * PhysFalling calls this for every blocking hit that was not a landing spot, on the client, on the
	 * server and on every replayed move, from inside the same sweep it was already doing. A hit on a
	 * near-vertical face while airborne opens the wall-jump window; everything else falls through.
	 *
	 * Calls Super immediately (it drives the physics-interaction impulses) and never touches Velocity.
	 */
	virtual void HandleImpact(const FHitResult& Hit, float TimeSlice = 0.f, const FVector& MoveDelta = FVector::ZeroVector) override;

	/**
	 * PATCH 28 §5 — THE WHOLE OF SURF, AND IT IS A SUBTRACTION.
	 *
	 * UCharacterMovementComponent::ComputeSlideVector projects the delta onto the surface plane (which
	 * IS Source's ClipVelocity at overbounce 1) and then, while falling, runs it through
	 * HandleSlopeBoosting() — the anti-slope-boost rule that refuses to let a deflection carry the pawn
	 * higher than the un-deflected move would have, and that ZEROES the deflection's vertical part when
	 * the original move was heading down. That last clause is every surf contact there is.
	 *
	 * So on a SURF PLANE this returns the honest projection, with the Source overbounce, and skips the
	 * boost guard. Every other surface — walls, walkable floors, the two-wall crease, ledge moves — is
	 * handed straight to Super and is arithmetically unchanged.
	 *
	 * PhysFalling re-derives Velocity from the returned delta, on all three machines, so this one
	 * override is also where "velocity is clipped rather than stopped" actually happens. There is no
	 * second velocity writer and no per-frame probe.
	 */
	virtual FVector ComputeSlideVector(const FVector& Delta, const float Time, const FVector& Normal,
		const FHitResult& Hit) const override;

	/**
	 * DEMO 29 ITEM 4(b) — THIS OVERRIDE NO LONGER REFUSES A SURF PLANE, AND THAT IS A BUG FIX.
	 *
	 * Patch 28 refused a surf plane here so a player could not stair-step up a rail. The refusal is
	 * still made — it moved to StepUp() below — because refusing HERE had a consequence nobody
	 * predicted: MoveAlongFloor's two recovery branches are `if (CanStepUp(Hit) || ...)` and
	 * `else if (!Hit.Component->CanCharacterStepUp(...))`, and the rail's boxes DO allow
	 * CanCharacterStepUp, so a false from here took NEITHER. No HandleImpact, no SlideAlongSurface, no
	 * movement — and PhysWalking then re-derives Velocity from the zero displacement that produced.
	 *
	 * Measured with -TraceSurfApproachTest at the 800 uu/s ground limit: running at a rail at 90, 60,
	 * 45, 30 and even 20 degrees to it left the pawn at 0 uu/s, every time. The rails were flypaper.
	 *
	 * Everything falls through to Super now, except under the Trace.Move.SurfLegacyExit A/B arm, which
	 * restores the refusal so the two behaviours are one table on one binary.
	 */
	virtual bool CanStepUp(const FHitResult& Hit) const override;

	/**
	 * DEMO 29 ITEM 4(b) — WHERE THE "no stair-stepping onto a surf face" GUARANTEE LIVES NOW.
	 *
	 * Refuses a step-up whose hit surface is a surf plane and hands everything else to Super. Same one
	 * dot product Patch 28 used, in the one place where a refusal means "this step-up failed" rather
	 * than "there was no impact at all" — MoveAlongFloor answers a failed StepUp() with HandleImpact()
	 * and SlideAlongSurface(), which is the recovery a walking pawn needs and never got.
	 *
	 * Super's own down-sweep would reject an unwalkable landing that ends higher than it started, and
	 * a surf plane is unwalkable by construction, so this is belt-and-braces. It is worth having: on
	 * FACETED geometry a single mis-sized facet is all it would take for one of those probes to find a
	 * walkable ledge and let a player climb a surf ramp on foot.
	 */
	virtual bool StepUp(const FVector& GravDir, const FVector& Delta, const FHitResult& Hit,
		FStepDownResult* OutStepDownResult = nullptr) override;

	/**
	 * DEMO 29 ITEM 4(a) — THE EXIT ROLLOUT: THE PIECE OF CURVE THE COLLISION GEOMETRY CANNOT HAVE.
	 *
	 * A curved ramp whose tangent reaches horizontal turns a descent into floor speed — the normal
	 * force does no work, so |v| survives and only its direction changes. OUR RAMPS CANNOT: the surf
	 * band's shallow end IS the engine's walkable limit, so the shallowest facet a rail may end on is
	 * still 47 degrees and the arc meets the floor at a kink. The engine's landing then deletes the
	 * vertical component outright and the whole descent is thrown away. That is half of the owner's
	 * "a player should carry momentum from a curve down onto the flat floor", and it is the half no
	 * knob could reach.
	 *
	 * So on the touchdown that ends a ride the planar velocity is rescaled to the speed the RIDE was
	 * worth, keeping the direction the engine's own landing produced. It is capped by SurfExitSpeed
	 * (the 3D speed on the last frame the pawn was on the face) and by GetSurfSpeedCeiling(), and
	 * floored at the planar speed the landing already produced — so it can only ROTATE earned speed
	 * into the floor, never add. A long fall after the ride still costs exactly what it costs today.
	 *
	 * Reads before Super and writes after: Super is what sets the movement mode and flattens Z.
	 */
	virtual void ProcessLanded(const FHitResult& Hit, float remainingTime, int32 Iterations) override;

	/**
	 * PATCH 28 §5 — THE SECOND ENGINE RULE THAT DEFEATS SURF, AND IT WAS FOUND BY MEASURING.
	 *
	 * UCharacterMovementComponent::LimitAirControl removes the INTO-THE-SURFACE part of the air-control
	 * acceleration before the move, with the comment "allow movement parallel to the wall, but not into
	 * it because that may push us up". It removes it in the HORIZONTAL plane: everything along
	 * Normal.GetSafeNormal2D() goes.
	 *
	 * On a wall that is right. On a RAMP it deletes the one input surfing is made of. A ramp's
	 * horizontal normal points straight down the slope, so "the horizontal component along the normal"
	 * IS the up-slope/down-slope axis — the axis a surfer holds to stay on the face. The engine throws
	 * the whole of it away, and what is left is the along-track component that changes nothing.
	 *
	 * MEASURED, and this is why the override exists rather than being reasoned into being. The first
	 * -TraceSurfTest run had an ideal-strafe arm and a NO-INPUT control arm, and they agreed to within
	 * 1 uu/s on four of five rungs (800 uu/s entry: 985 strafed, 984 with the stick untouched). All of
	 * the gain was gravity; none of it was the player. A surf feature in which input does nothing is
	 * not a surf feature.
	 *
	 * SO ON A SURF PLANE THE ACCELERATION IS PASSED THROUGH UNTOUCHED — which is not a licence, it is
	 * SOURCE'S OWN ORDER OF OPERATIONS. Source accelerates first (PM_AirAccelerate) and clips second
	 * (PM_TryPlayerMove -> ClipVelocity); the part of the input that really did point into the plane is
	 * removed by the clip a few lines later, and the part along the plane — including up-slope —
	 * survives, which is the entire mechanic. Pre-emptively deleting it is UE trying to do the clip's
	 * job in the horizontal plane, where it cannot tell "into the ramp" from "up the ramp".
	 *
	 * Walls are untouched: IsSurfPlane() is false for anything at or below GetSurfMinNormalZ(), so the
	 * wall jump's faces still get the engine's rule and a player still cannot ride their own input up a
	 * corner.
	 */
	virtual FVector LimitAirControl(float DeltaTime, const FVector& FallAcceleration,
		const FHitResult& HitResult, bool bCheckForValidLandingSpot) override;

#if !UE_BUILD_SHIPPING
	/**
	 * SPEC v8 §1 — "dash feels rubber bandy", answered as CORRECTIONS PER DASH.
	 *
	 * Logs this pawn's dash-attributed correction rate. Public and on the class because the counters
	 * it reads are protected and the console command that drives it (Trace.DashNetReport) is a free
	 * function; the alternative was exposing four raw counters that nothing else has any business
	 * touching.
	 *
	 * ON AN AUTHORITATIVE PAWN THE ANSWER IS ALWAYS 0 AND MEANS NOTHING — a listen host cannot be
	 * corrected. The line says so itself when HasAuthority(), because that exact confusion is how
	 * the previous pass reported the dash as correction-free while the user was feeling it snap.
	 */
	void LogDashNetReport() const;

	/**
	 * SPEC v8 §7 — the wall jump, as numbers, on the machine that can disagree with the server.
	 *
	 * Prints how many wall jumps this pawn took, the entry -> launch planar speed (the "carry momentum"
	 * claim), how far the launch was TURNED from the approach (the "in a new direction" claim), the
	 * highest consecutive count reached, how many presses the anti-ladder cap refused, and how many
	 * corrections landed inside a wall-jump window (the prediction claim — must be read on a CLIENT).
	 */
	void LogWallJumpReport() const;

	/**
	 * SPEC v10 §5 — THE STICK METER, printed. See the STICK METER block in the private section below
	 * for what is being measured and why the v9 numbers could not have shown it.
	 *
	 * Public for the same reason LogWallJumpReport() is: Trace.WallJumpReport is a free function and
	 * calls both. Two reports rather than one because they answer different questions — that one is
	 * about what the LAUNCH did, this one is about how long the PLAYER was stuck.
	 */
	void LogWallStickReport() const;

	/**
	 * PATCH 28 §5 — SURF, AS NUMBERS.
	 *
	 * Prints every surf this pawn has taken: how many, the mean and best ENTRY -> EXIT speed change
	 * (the "accelerate using curved ramps" claim, as a ratio and as uu/s), how often the derived
	 * ceiling actually bound (the "it is bounded" claim), how many surfs were refused because the face
	 * was walkable (the "you cannot surf ordinary geometry" claim), and how many server corrections
	 * landed inside a surf window (the prediction claim — read this one on a CLIENT; on an
	 * authoritative pawn it is zero for a reason that has nothing to do with surf).
	 *
	 * Public for the same reason LogWallJumpReport() is: Trace.Move.SurfReport is a free function and
	 * the counters it reads are protected.
	 */
	void LogSurfReport() const;

	/**
	 * SPEC v18 §1b — arms the air drift ledger on THIS pawn for @p Seconds of world time.
	 *
	 * Public because Trace.Move.V18.AirDrift is a free console command and the ledger's state is not.
	 * Re-arming restarts the session; the report prints itself when the window closes.
	 */
	void ArmAirDriftMeter(float Seconds);

	/**
	 * SPEC v9 §§5-8 — every tuned number and every KNOCK-ON of the gravity change, in one block.
	 *
	 * §8 says gravity "touches everything: jump height, air time, the mantle, the wall jump, the
	 * vertical dash arc" and asks for the knock-on effects WITH NUMBERS. Those numbers are closed-form
	 * functions of the getters this component already exposes — apex is v²/2g, air time is 2v/g — so
	 * they are derived here from the live getters rather than eyeballed off a screenshot, which means
	 * they can never drift from what the pawn is actually simulating.
	 *
	 * Run it once per arm (`-TraceLegacyTuning` for BEFORE) and diff the two blocks. Every line prints
	 * the input knob next to the result so a reader can check the arithmetic.
	 *
	 * The headroom lines are the ones that matter: MANTLE HEADROOM is (jump apex - the tallest ledge
	 * the mantle will take), and if the gravity increase drives it negative the mantle has become
	 * unreachable by jumping and needs re-tuning. Saying so is the point — §8 asks for that rather
	 * than for a silent compensation.
	 */
	void LogV9TuningReport() const;

	/**
	 * Drives the wall jump from code so it can be measured offscreen and on a client: runs the pawn at
	 * the nearest perimeter wall and presses jump through ACharacter::Jump() — the real predicted path,
	 * saved move and all — every time IsWallJumpAvailable() says a press would count. Enabled with
	 * -TraceWallJumpTest. Never runs on a replayed move, for TickDashPitchTest's reason.
	 */
	void TickWallJumpTest(float DeltaSeconds);

	/**
	 * PATCH 28 §5 — THE SPEED-GAIN RIG. Enabled with -TraceSurfTest.
	 *
	 * Drives a LADDER OF ENTRY SPEEDS onto a real arena surf rail and records what each one comes off
	 * with, so "accelerate using curved ramps" is a curve rather than an adjective. Each run places
	 * the pawn just off the rail's face with a chosen entry speed along the rail, then drives the
	 * IDEAL Source strafe every frame — the wish direction held perpendicular to the current planar
	 * velocity, on the up-slope side, which is the input that extracts the most from the projection
	 * formula — and closes the sample when IsSurfing() goes false.
	 *
	 * IT ASKS THE ARENA WHERE THE RAMP IS rather than knowing: ATraceArenaBuilder::GetSurfRailProbe()
	 * is the single definition of the rail's geometry, so a rig that placed the pawn from its own copy
	 * of the numbers could pass forever after the level moved. Never runs on a replayed move, for
	 * TickDashPitchTest's reason.
	 */
	void TickSurfTest(float DeltaSeconds);

	/**
	 * DEMO 29 ITEM 4 (a) — THE EXIT RIG. Enabled with -TraceSurfExitTest.
	 *
	 * "When sliding down the curved surfaces, a player loses all momentum at the end of the curve."
	 * TickSurfTest cannot answer that: it closes its sample on the frame the SURF STATE closes, and on
	 * every strafed rung of its ladder that frame was still airborne with the ride in progress. This
	 * follows one ride through the four places its speed can go — the last frame on the face, the last
	 * airborne frame, the first grounded frame, and two seconds of floor — and reports the DISTANCE
	 * carried, because "carry momentum onto the flat floor" is a claim about distance.
	 */
	void TickSurfExitRun(float DeltaSeconds);

	/**
	 * DEMO 29 ITEM 4 (b) — THE APPROACH RIG. Enabled with -TraceSurfApproachTest.
	 *
	 * "It still doesn't feel like you can surf INTO curves/curved ramps in order to gain momentum."
	 * Every arm of TickSurfTest starts a ride by teleporting a pawn onto the face or walking it off
	 * the crest, so nothing in this project had ever measured the APPROACH. This one never puts a pawn
	 * on the face: it runs one at the rail across the flat floor at the ground limit, at a ladder of
	 * angles, with a 0-degree parallel run as the control.
	 */
	void TickSurfApproachRun(float DeltaSeconds);

	/** The exit rig's table. Public for the same reason LogSurfReport() is — see that method. */
	void LogSurfExitTable() const;

	/** The approach rig's table. */
	void LogSurfApproachTable() const;

	/**
	 * SPEC v8 §5 — "the two dash charges aren't working anymore, for the carrier", answered ON A CLIENT
	 * WHILE ACTUALLY CARRYING. Enabled with -TraceCarrierChargeTest.
	 *
	 * It has two halves, one per process, because the claim spans both:
	 *
	 *   SERVER half. A joined client cannot walk to the middle of a 24000 uu arena in an offscreen
	 *   test, and Trace.DebugTakeCore only ever targets the LOCAL pawn — on a listen host that is the
	 *   host, which is the one machine spec v8 §0 says does not count. So the authority hands the Core
	 *   to the JOINED CLIENT's pawn through ATraceCore::TryPickup(), the same funnel the pickup sphere
	 *   uses, so bIsCarrier really replicates and the client learns it the way a player would.
	 *
	 *   CLIENT half. Fires two dashes in quick succession twice over: once BEFORE the Core arrives
	 *   (the control — one charge, so the second press must be refused) and once after the pool has
	 *   refilled WHILE CARRYING (two charges, so both presses must launch). Counting launches rather
	 *   than reading the counter is the point: "it's acting as one dash charge" is a claim about how
	 *   many dashes come out, and a charge that exists but cannot be spent would pass a counter check.
	 *
	 * Never runs on a replayed move, for TickDashPitchTest's reason.
	 */
	void TickCarrierChargeTest(float DeltaSeconds);

	/**
	 * SPEC v9 §2, AND THE §0 LESSON — "-TraceSingleDashTest".
	 *
	 * The v8 harness above pressed dash TWICE FROM A FULL POOL, counted two launches and reported the
	 * carrier's charges fixed. The user says it is still broken. That test could not have failed:
	 * both launches really do happen. It exercised neither the consumption path nor the refill path,
	 * and it never looked at the HUD at all.
	 *
	 * This one reproduces the three reported sentences directly, on a CLIENT, WHILE CARRYING:
	 *   phase 0  dash ONCE from a full pool  -> "when dash is used, both charges are consumed"
	 *   phase 1  watch the 1 -> 2 refill
	 *   phase 2  drain to zero
	 *   phase 3  watch the 0 -> 1 refill     -> "when the first refills, they both do"
	 * and at every sample it compares the pool against THE NUMBER OF PIPS ATraceHUD WOULD DRAW,
	 * obtained from the real ATracePlayerController::GetDashHudState -> "despite the hud showing two".
	 *
	 * Never runs on a replayed move, for TickDashPitchTest's reason.
	 */
	void TickSingleDashTest(float DeltaSeconds);

	/** One ledger row: the true pool, and what the HUD would render for it. See the .cpp. */
	struct FTraceSingleDashSample SampleSingleDashTest() const;

	/**
	 * DIAGNOSTIC ONLY — "Trace.MoveCorrections 1" (or -TraceMoveCorrections).
	 *
	 * Every server correction that reaches this client passes through here, and logging it with the
	 * error magnitude and the pawn's state at the time is what turns "it feels like rubber banding"
	 * into a number. It is the evidence for spec v5 §7's diagnosis and the measurement that the
	 * ledge fixes are checked against.
	 *
	 * Note the overload: UE 5.8 dispatches the FMovementBaseInterfaceData* form directly from
	 * ClientHandleMoveResponse, so overriding the older UPrimitiveComponent* form would never fire.
	 *
	 * Observation only — it calls Super immediately and changes nothing.
	 */
	virtual void OnClientCorrectionReceived(class FNetworkPredictionData_Client_Character& ClientData,
		float TimeStamp, FVector NewLocation, FVector NewVelocity,
		struct FMovementBaseInterfaceData* NewMovementBaseInterfaceData, FName NewBaseBoneName,
		bool bHasBase, bool bBaseRelativePosition, uint8 ServerMovementMode,
		FVector ServerGravityDirection) override;
#endif

	// --- Dash API ------------------------------------------------------------------------------

	/**
	 * Requests a dash. Call on the machine that owns the input (the autonomous client, or the
	 * server for a listen-host / AI). Raises bWantsToDash; the next simulated move consumes it.
	 * Safe to spam — CanDash() gates the actual activation.
	 */
	void StartDash();

	/** A charge is available, the pawn is alive, and it is in a movement mode that can dash. */
	bool CanDash() const;

	/**
	 * SPEC v7 §5. Composes the dash direction from an input acceleration and an aim rotation.
	 *
	 * A pure function — no member state is read beyond the fallback facing, and nothing is written —
	 * which is what lets BeginDash(), the client's replay of BeginDash() and Trace.DashVectorTest all
	 * go through the SAME code. If you are tempted to inline this back into BeginDash(), don't: the
	 * measured verification of "W+D is one dash length" would then be measuring a copy.
	 *
	 * @param InAcceleration world-space input acceleration; only its X/Y are read, and only its
	 *                       decomposition in InAimRotation's yaw basis matters, not its magnitude.
	 * @param InAimRotation  the FULL control rotation. Its yaw defines the input basis and the level
	 *                       strafe axis; its pitch is what makes W/S vertical.
	 * @return a unit vector, always. Never zero.
	 */
	FVector ComputeDashDirection(const FVector& InAcceleration, const FRotator& InAimRotation) const;

	/** The direction locked in at the last BeginDash(). Unit length; meaningless before the first dash. */
	FVector GetDashDirection() const { return DashDirection; }

#if !UE_BUILD_SHIPPING
	/**
	 * The measured verification of spec v7 §5, behind the console command `Trace.DashVectorTest`.
	 *
	 * A member rather than a free function only so that it can reach the protected settings accessors
	 * and report the reach and the exit ceiling in the same table as the vectors. Offline, const, and
	 * safe to call on the CDO — it drives the shipping ComputeDashDirection() with synthetic inputs
	 * and touches nothing else.
	 *
	 * @return the number of assertions that failed. 0 is a pass.
	 */
	int32 RunDashVectorTest() const;
#endif

	/** True for the DashDuration window. Read by the trail trip test — the whole game hangs on it. */
	bool IsDashing() const;

	/**
	 * SPEC v10 §6 — "Don't let players shoot while in a dash animation. As soon as they end the dash,
	 * let them shoot again." THE GATE THE WEAPON SLICE CALLS. Also gates the knife swing (§6's
	 * [ASSUMPTION], which the spec states outright).
	 *
	 * This is deliberately a THIN alias of IsDashing() and nothing more, because the spec's second
	 * sentence is the whole requirement: there is no cooldown, no grace, no "recovery" — the gate is a
	 * pure function of DashTimeRemaining, which OnMovementUpdated drives to exactly 0 on the frame the
	 * dash window closes. The frame IsDashing() goes false, the very next fire press is allowed.
	 *
	 * WHERE IT IS VALID. DashTimeRemaining is saved-move state, so it is correct on the owning client
	 * (predicted, and replayed identically after a correction) and on the SERVER for every pawn it
	 * simulates — an autonomous proxy's dash clock is advanced by MoveAutonomous -> OnMovementUpdated,
	 * a bot's by its own PerformMovement. Both ends of the "client predicts the shot, server validates
	 * it" pattern therefore read the same answer, which is what a fire gate needs.
	 *
	 * It is NOT valid on a SIMULATED proxy (another player's pawn, as seen on your machine): nothing
	 * replicates DashTimeRemaining, so a simulated proxy reads false forever. Do not use this to
	 * decide whether somebody ELSE may shoot — the shooter's own client and the server both have the
	 * truth, and that is where the gate belongs.
	 */
	bool AreWeaponActionsBlocked() const { return IsDashing(); }

	// --- SPEC v10 §1: THE KNIFE'S MOVEMENT PROFILE -------------------------------------------------

	/**
	 * SPEC v10 §1 — "Players should move 30% faster with a knife, as well as have a higher momentum
	 * ceiling." THE ENTRY POINT FOR THE MELEE SLICE. Call it from the equip/swap code with true when
	 * the knife becomes the active weapon and false when it stops being one.
	 *
	 * CALL IT ON BOTH ENDS: on the server (from the authoritative equip) and on the owning client
	 * (from its predicted equip). The bit is round-tripped through FSavedMove_Trace so a correction
	 * replays every move under the profile that move actually ran with — but the bit is SET from
	 * outside the move pipeline, so if only one end is told, that end simulates a different pawn and
	 * the correction path pays for it on every step. This is the same seam GetMaxSpeed()'s carrier
	 * multiplier already has and it is documented in exactly the same terms: bounded, self-correcting,
	 * and only as wide as the time it takes the equip to reach both machines.
	 *
	 * Idempotent, and cheap enough to call every frame if that is easier than tracking the edge.
	 */
	void SetKnifeMovementProfileActive(bool bActive);

	/** True while the knife movement profile (speed multiplier + raised air-strafe caps) is applied. */
	bool IsKnifeMovementProfileActive() const { return bKnifeMovementProfile != 0; }

	/**
	 * SECONDS UNTIL THE POOL GAINS ITS NEXT CHARGE. 0 only when the pool is FULL.
	 *
	 * SPEC v9 §2 CHANGED THE MEANING OF THIS FUNCTION, and the old meaning was the bug. It used to
	 * return 0 whenever any charge was in hand — so a carrier at 1 of 2 reported "nothing is
	 * recharging", ATracePlayerController::GetDashHudState turned that into RechargeFraction 1.0,
	 * and ATraceHUD::DrawChargePips filled the still-regenerating pip solid. The HUD read 2 of 2 on
	 * a pawn holding 1, which is precisely "the carrier still has only one dash, despite the hud
	 * showing two. When the first refills, they both do." See the .cpp for the full derivation of
	 * all three reported symptoms from this one line.
	 *
	 * It now matches FTraceDashHudState::Remaining's own documented contract exactly, so the meter
	 * 1 - Remaining/(DashDuration + DashCooldown) is the true progress of the NEXT pip — the divisor
	 * is GetDashRechargeWindow() by construction, so the two agree.
	 *
	 * NOT a readiness test. "Can I dash" is GetDashCharges() > 0 / CanDash(); a carrier with one
	 * banked charge and one regenerating can dash right now and still reads several seconds here.
	 */
	float GetDashCooldownRemaining() const;

	/** Dash charges available right now. */
	int32 GetDashCharges() const { return DashCharges; }

	/** Size of the charge pool: BaseDashCharges, plus CarrierExtraDashCharges while carrying. */
	int32 GetMaxDashCharges() const;

	/**
	 * SPEC v7 §6. SERVER. Hands back ONE spent dash charge immediately, and tells the owning client.
	 *
	 * Verbatim: "reset ... one of their dash cooldowns to zero. If the first dash is already off
	 * cooldown, the second one should be set to zero instead." The two charges are A POOL WITH ONE
	 * REFILL CLOCK, not two independent timers, so that sentence has exactly one implementation:
	 *
	 *   both spent (0 of 2) -> 1 of 2, and the clock already running keeps running for the second.
	 *                          That IS "the first is refunded, the second keeps its cooldown".
	 *   one spent  (1 of 2) -> 2 of 2, pool full, clock cleared. "The second one is set to zero."
	 *   none spent (2 of 2) -> false. The spec's own third case: nothing to do.
	 *
	 * @return true if a charge was actually handed back, false if the pool was already full or this
	 *         is not the authority. The caller LOGS the false case rather than retrying — a refund
	 *         nobody was owed is not a failure.
	 */
	bool RefundDashCharge();

	/**
	 * Mirrors RefundDashCharge onto the owning client so the HUD meter moves on the same frame.
	 *
	 * Not optional. ATracePlayerController::GetDashHudState reads GetDashCharges() off the LOCAL
	 * component, so a listen host sees the refund the instant the server writes it and a remote
	 * client would otherwise see nothing at all: DashCharges is saved-move state, replayed from the
	 * client's own prediction, never replicated down as a property.
	 *
	 * The documented prediction seam: a correction that replays moves older than this RPC can briefly
	 * undo the refund, after which the counts reconverge — the same seam the carrier's extra charge
	 * already has, and the reason this is a Reliable RPC rather than a one-shot event.
	 */
	UFUNCTION(Client, Reliable)
	void ClientRefundDashCharge();

	/**
	 * World time at which this pawn was last observed inside its dash window, on the server only
	 * (-1000 if never). Exists because the server advances a remote client's dash clock inside
	 * MoveAutonomous, which can consume several client moves in one server frame: a whole short dash
	 * can begin and end *between* two ticks of the trail's TG_PostPhysics trip test, whose
	 * IsDashing() sample would then read false even though the displacement it is testing was made
	 * while dashing. The trail treats "dashing within the last fraction of a second" as dashing.
	 *
	 * Deliberately NOT saved-move state: it is a server-side observation that never feeds movement,
	 * so it cannot desync prediction.
	 */
	float GetLastDashActiveWorldTime() const { return LastDashActiveWorldTime; }

	// --- Crouch / slide / fast-fall API ---------------------------------------------------------

	/**
	 * The crouch key, as a HELD state — call with true on press and false on release.
	 *
	 * One key, two meanings, resolved by where the pawn is when it goes down:
	 *   on the ground → FIRE the slide ability (spec v5 §3). One press buys one slide of
	 *                   SlideDuration seconds; the release is ignored, and holding the key neither
	 *                   lengthens the slide nor starts a second one.
	 *   in the air    → fast-fall, i.e. zero out POSITIVE Z velocity once, on the press edge
	 *
	 * STILL A LEVEL AND NOT AN EDGE, deliberately: the edge is derived inside the simulation from
	 * bSlideHeldLastMove, which is saved-move state, so a replayed move reproduces the press exactly.
	 * A caller that pulsed an edge would have to guarantee the pulse survived a correction, and it
	 * cannot. Callers (ATracePlayerController, ATraceBotController) need no change for spec v5 —
	 * they may keep holding the key; it simply stops meaning anything after the first frame.
	 *
	 * EITHER THIS OR ACharacter::Crouch() WORKS. The two are ORed together every move (see
	 * IsCrouchHeld()): ATracePlayerController drives the human through Crouch()/UnCrouch(), which
	 * rides FLAG_WantsToCrouch, while this entry point rides FLAG_Custom_2. Both are client-predicted
	 * and both mean exactly the same thing to the simulation. Neither ever resizes the capsule — see
	 * CanCrouchInCurrentState().
	 */
	void SetWantsToSlide(bool bWants);

	/** The crouch key as the simulation sees it: this slice's flag OR the engine's bWantsToCrouch. */
	bool IsCrouchHeld() const { return (bWantsToSlide != 0) || (bWantsToCrouch != 0); }

	// --- Jump held (Demo 19 item 4) --------------------------------------------------------------

	/**
	 * THE JUMP KEY AS A HELD LEVEL — the twin of IsCrouchHeld(), and it exists because until now
	 * NOTHING IN THIS PROJECT COULD ANSWER THE QUESTION "is jump still down?".
	 *
	 * THE BUG THIS CLOSES, because it is worth writing down where the next person will find it:
	 * UTraceAbilityComponent::HandleJumpReleased() — and therefore every character's OnJumpReleased()
	 * hook — HAS NO CALLER ANYWHERE. ATracePlayerController::OnJumpCompleted() forwards the release to
	 * ACharacter::StopJumping() and stops there, while OnJumpStarted() *does* forward the press. So a
	 * character that latched "jump is down" in its press hook latched it FOREVER. Lily's Zip did
	 * exactly that: one tap of space flew her up for the whole 5 s flight and crouch could never win,
	 * because her descend test was `!bJumpHeld && IsCrouchHeld()`. That is Demo 19 item 4, both halves,
	 * from one cause.
	 *
	 * ACharacter::bPressedJump is NOT this. JumpMaxHoldTime is 0, so ClearJumpInput drops it after a
	 * single tick (see the note above TryConsumeSlideJump) — and Lily's hook consumes the press before
	 * ACharacter::Jump() ever runs, so for her it is never even set.
	 *
	 * A LEVEL, LIKE THE SLIDE FLAG, AND FOR THE SAME REASONS. It rides FLAG_Custom_1 — the bit spec v3
	 * §1 freed when boost was deleted — so the server learns a remote client's release through the
	 * ordinary saved-move path rather than through an RPC that does not exist. Re-assert it while the
	 * key is down; the writer is UTraceAbilitySetLily::SampleClimbIntent(), which polls the player's
	 * bound Jump key on the machine that has a keyboard.
	 *
	 * *** IT GOES STALE ON PURPOSE. *** IsJumpHeld() answers false once JumpHeldStaleSeconds have
	 * passed without a re-assert, so a writer that is switched off, disconnected or simply forgets to
	 * send the release can no longer fly anybody forever. The failure mode of the whole mechanism is a
	 * quarter of a second of unwanted climb, not five seconds of it.
	 */
	void SetJumpHeld(bool bHeld);

	/** True while the jump key is down AND that fact is fresh. See SetJumpHeld(). */
	bool IsJumpHeld() const;

	/**
	 * How long a SetJumpHeld(true) stays true without being re-asserted.
	 *
	 * Comfortably longer than the 20 Hz (50 ms) ability tick that re-asserts it and than a listen
	 * server's move cadence, and far shorter than anything a player would call "it kept going".
	 */
	static constexpr double JumpHeldStaleSeconds = 0.25;

	/** True for the duration of a slide. */
	bool IsSliding() const;

	/**
	 * Seconds before another slide is allowed (0 when ready). Measured from the last slide's END.
	 *
	 * THE HIDDEN COOLDOWN (spec v5 §3). Enforced, but deliberately never surfaced: this exists for
	 * bots (which need to know whether asking for a slide is worth anything) and for the measurement
	 * harness. DO NOT DRAW IT ON THE HUD — the design intent is that the player learns the rhythm by
	 * feel, and a meter would turn a hidden cost into a resource to be optimised.
	 */
	float GetSlideCooldownRemaining() const { return FMath::Max(0.f, SlideCooldownRemaining); }

	// --- Ledge API (spec v5 §7, minus the mantle removed in v12 §5) -------------------------------

	/**
	 * "Grounded" as the ABILITY LAYER sees it: actually on the ground, or within LedgeGroundGrace-
	 * Seconds of having been. See the ledge section of the header — this is the hysteresis that stops
	 * a one-frame contact blip on a ledge lip from ending a slide or faking a landing on one machine
	 * and not the other. It never contradicts the engine about where the pawn IS.
	 */
	bool IsGroundedForAbilities() const;

	// --- Slide-jump readouts (HUD, bots, debug) --------------------------------------------------

	/**
	 * True when jumping RIGHT NOW would be a slide-jump: mid-slide, or inside the coyote window that
	 * follows a slide. Pure query — reads saved-move state only, so it is safe for a bot to poll.
	 */
	bool IsSlideJumpAvailable() const;

	/**
	 * True when a slide-jump taken RIGHT NOW would collect SlideJumpWindowSpeedBonus, i.e. the slide
	 * has less than SlideJumpWindowSeconds left (or just ended having been in that state).
	 *
	 * This is the number a HUD tell should be driven from: the timing window is unlearnable without
	 * feedback, and the whole point of the mechanic is that it rewards a read.
	 */
	bool IsSlideJumpWellTimed() const;

	// --- Wall-jump readouts (HUD, bots, debug) — spec v8 §7 --------------------------------------

	/**
	 * True when pressing jump RIGHT NOW would be a wall jump: airborne, inside the contact window, and
	 * still under the consecutive cap. Pure query over saved-move state, so a bot may poll it.
	 */
	bool IsWallJumpAvailable() const;

	/** Outward normal of the wall the window is open on. Zero when no window is open. */
	FVector GetWallJumpNormal() const { return WallJumpNormal; }

	/** Wall jumps taken since the pawn last touched the ground. Capped at WallJumpMaxConsecutive. */
	int32 GetWallJumpsSinceGround() const { return WallJumpsSinceGround; }

	// --- Momentum readouts (HUD, debug, measurement) --------------------------------------------

	/** Horizontal speed, in uu/s. The number every part of this pass is actually about. */
	float GetPlanarSpeed() const;

	/**
	 * True when the pawn is carrying more horizontal speed than normal ground movement would grant.
	 * This is the state that used to be erased on landing and is now bled off instead.
	 */
	bool IsCarryingExcessSpeed() const;

	/**
	 * SPEC v18 §1a. ONE SUB-STEP OF THE AIR MODEL, AS A PURE FUNCTION.
	 *
	 * The whole of the Source air formula, the spec v5 §1 accumulation ceiling and the v18 §1a
	 * opposition brake, taking their inputs as arguments and reading nothing but the tuning getters.
	 * ApplySourceAirAcceleration() is now a thin wrapper that resolves the wish direction (including
	 * the v10 §5 into-wall projection, which needs member state) and calls this.
	 *
	 * IT IS SPLIT OUT FOR EXACTLY THE REASON ComputeDashDirection() IS. "Reversing in the air slows
	 * you, and turning into your travel direction still does not" is a claim about this arithmetic at
	 * a range of angles, and the only honest way to check it is to drive the SHIPPED arithmetic with
	 * synthetic vectors — a harness that re-implements the formula is measuring its own copy, and this
	 * project has already been bitten by a harness holding a stale copy of a rule. If you are tempted
	 * to inline this back into ApplySourceAirAcceleration(), don't.
	 *
	 * @param InPlanarVelocity current horizontal velocity. Z is ignored and never written.
	 * @param InWishDirection  the wish direction, normalised inside. Zero-length means "no input", and
	 *                         returns the velocity untouched (Source has no air friction and neither
	 *                         do we).
	 * @param InInputScale     analog deflection in [0,1]. 1 for a keyboard.
	 * @param InDeltaTime      the sub-step length, in seconds.
	 * @return the new planar velocity. Z is always 0.
	 */
	FVector ComputeAirStrafeStep(const FVector& InPlanarVelocity, const FVector& InWishDirection,
		float InInputScale, float InDeltaTime) const;

	// --- PATCH 28 §5: surf readouts and the clip, as a pure function -----------------------------

	/**
	 * SOURCE'S PM_ClipVelocity, EXACTLY, AND THE ONLY COPY OF IT.
	 *
	 *     out = in - normal * (in·normal) * overbounce
	 *
	 * plus Source's per-axis STOP_EPSILON snap, which is not decoration: without it a component that
	 * should be exactly zero comes back as a few thousandths of a uu/s of drift into (or out of) the
	 * plane, and a pawn riding a ramp for four seconds at 120 Hz integrates that into a visible
	 * detach. Overbounce 1 is the value Source uses for the player and is what makes this a pure
	 * projection; above 1 the pawn is pushed off the plane, below 1 it sinks into it.
	 *
	 * PUBLIC AND STATIC so Trace.Move.Surf can drive the SHIPPED arithmetic with synthetic vectors
	 * instead of re-typing the formula into a harness — the same rule ComputeAirStrafeStep() is split
	 * out under, and for the same reason: two copies of a rule are two things that can disagree.
	 */
	static FVector ClipVelocityAgainstPlane(const FVector& In, const FVector& Normal, float Overbounce);

	/**
	 * True if @p Normal is a face this component will surf: steeper than the pawn can walk on, but not
	 * a wall.
	 *
	 *     GetSurfMinNormalZ() < Normal.Z < GetWalkableFloorZ()
	 *
	 * THE UPPER BOUND IS THE LIVE ENGINE LIMIT AND MUST STAY THAT WAY. It is what makes "a player
	 * cannot surf ordinary walkable geometry" true by construction rather than by tuning: the same
	 * number IsWalkable() uses to decide the pawn is standing is the number that refuses the surf.
	 * A copied 0.71 here would be a second opinion about what walkable means.
	 */
	bool IsSurfPlane(const FVector& Normal) const;

	/**
	 * The SLOPE BAND this component will surf, in degrees from horizontal — the same fact IsSurfPlane()
	 * tests, expressed the way a level builder needs it.
	 *
	 * PUBLIC, AND THIS IS THE DEMO 21 RULE APPLIED TO GEOMETRY. ATraceArenaBuilder::BuildSurfRails()
	 * cuts its ramp faces from this band with a margin at each end, so the ramps TRACK the movement
	 * rule instead of carrying a copy of it: change the walkable limit or SurfMinNormalZ and the arena
	 * re-cuts itself, and there is no second set of angles to forget. A builder that typed "46 to 61
	 * degrees" beside this would be one retune away from shipping a ramp nobody can surf.
	 *
	 * @param OutMinDegrees the shallowest surfable slope — the engine's own walkable limit.
	 * @param OutMaxDegrees the steepest — acos(GetSurfMinNormalZ()), i.e. where the wall band starts.
	 */
	void GetSurfSlopeBandDegrees(float& OutMinDegrees, float& OutMaxDegrees) const;

	/** True while this pawn is riding a surf plane (or within the contact grace of having been). */
	bool IsSurfing() const;

	/** The plane currently being surfed. Zero when not surfing. */
	FVector GetSurfPlaneNormal() const { return SurfPlaneNormal; }

	/** Speed the current surf was entered at, in uu/s. This is the floor under the surf ceiling. */
	float GetSurfEntrySpeed() const { return SurfEntrySpeed; }

	/** Seconds the current surf has been running. */
	float GetSurfElapsedSeconds() const { return SurfElapsedSeconds; }

	/** Highest speed reached during the current surf, in uu/s. */
	float GetSurfPeakSpeed() const { return SurfPeakSpeed; }

	/**
	 * The bound. max(entry speed, GetAirStrafeHardCapSpeed() x SurfSpeedCeilingMultiplier).
	 *
	 * DERIVED, NOT TYPED. The multiplier is the only new number; the base is the air-strafe hard cap
	 * this file already ships, so the knife profile's 1.25x is folded in for free and a retune of the
	 * air ceiling moves the surf ceiling with it. Floored at the entry speed like every other ceiling
	 * here, so a player who arrives above it keeps what they brought and simply cannot add.
	 */
	float GetSurfSpeedCeiling() const;

	/**
	 * DEMO 29 ITEM 4. The fastest a surf can be on ANY weapon profile, and the number ARENA GEOMETRY
	 * has to clear — a rail's exit lane must be safe for the fastest pawn that can come off it, not
	 * merely for the one holding a gun. GetSurfSpeedCeiling() folds the knife multiplier in only while
	 * bKnifeMovementProfile is set, and it is never set on the CDO a builder reads.
	 *
	 * Not floored at SurfEntrySpeed: this is a property of the tuning, and geometry cannot be re-cut
	 * per pawn.
	 */
	float GetSurfSpeedCeilingMax() const;

	/**
	 * DEMO 29 ITEM 4. How far past the lip of a surf ramp a surfer can still travel through the air,
	 * leaving it at @p LaunchHeight above the floor. reach = GetSurfSpeedCeilingMax() * sqrt(2h/g).
	 *
	 * ATraceArenaBuilder asks this so a rail's exit lane can be kept clear, and it exists because the
	 * shipped rails were NOT clear: the exit rig measured a ride leaving the nose at 1469 uu/s and
	 * arriving on the floor at 52, having flown 1300 uu into the innermost approach cover. A builder
	 * that typed "leave 1500 uu clear" would be one air-cap retune away from doing it again with
	 * nothing to say so — the DEMO 21 rule, applied to a clearance.
	 *
	 * @param LaunchHeight  height above the floor the surfer leaves at, uu.
	 * @param WorldGravityZ the WORLD's gravity, uu/s^2 (signed). Passed in because a CDO has no
	 *                      physics volume and UMovementComponent::GetGravityZ() dereferences one.
	 *                      GravityScale is applied inside.
	 */
	float GetSurfExitReach(float LaunchHeight, float WorldGravityZ) const;

	/** True while a ride's momentum is still being carried — see SurfExitCarryRemaining. */
	bool IsSurfExitCarryActive() const;

	// --- The audit surface (spec v16 §5) ---------------------------------------------------------
	//
	// Read-only forwarders onto tuning getters and instrument counters that are otherwise protected,
	// so Movement/TraceMovementAuditV16.cpp can print an EXPECTED column that IS the shipped
	// derivation rather than a second copy of it.
	//
	// WHY NOT JUST RECOMPUTE IN THE HARNESS. Because "1 + (SlideJumpWindowSpeedBonus - 1) x
	// SlideJumpBonusScale" typed into a test file is a second opinion about the rule, and this
	// project's standing lesson is that two copies of a rule are two things that can disagree — a
	// harness holding the stale copy reports PASS forever after the shipped one moves. Forwarding
	// keeps one definition.
	//
	// Nothing here has a setter and nothing here is called by gameplay. If a future pass makes these
	// getters public in their own right, delete these forwarders rather than leaving both.

	/** Live slide velocity, in uu/s. Zero when not sliding. */
	float GetSlideSpeedForAudit() const { return SlideSpeed; }

	/** SPEC v18 §1a. The opposition brake actually in force, uu/s² at a dead-on reversal. */
	float GetAirStrafeOpposingDecelerationForAudit() const { return GetAirStrafeOpposingDeceleration(); }

	/** Fraction of a strafe's speed GAIN granted at this planar speed. Never touches the brake. */
	float GetAirStrafeGainScaleForAudit(const float PlanarSpeed) const { return GetAirStrafeGainScale(PlanarSpeed); }

	/** Seconds left on the v10 §5 buffered mid-air jump press. See GetWallJumpInputBufferSeconds(). */
	float GetWallJumpInputBufferRemainingForAudit() const { return WallJumpInputBufferRemaining; }

	/** Seconds left on the wall-jump contact window. Zero when no face is latched. */
	float GetWallJumpWindowRemainingForAudit() const { return WallJumpWindowRemaining; }

	/** Seconds of "the ability layer still counts this pawn as grounded" left. See the ledge grace. */
	float GetGroundGraceRemainingForAudit() const { return GroundGraceRemaining; }

	/** Seconds of dash window left. Zero when not dashing. */
	float GetDashTimeRemainingForAudit() const { return DashTimeRemaining; }

	/** Seconds of slide-jump coyote grace left. Zero when no slide has recently ended. */
	float GetSlideJumpGraceRemainingForAudit() const { return SlideJumpGraceRemaining; }

	/** Seconds of slide left, the same number the well-timed window is measured against. */
	float GetSlideTimeLeftForAudit() const { return GetSlideTimeLeft(); }

	/**
	 * How long a slide lasts in total, with every modifier applied — SlideDuration x
	 * SlideMaxLengthScale - SlideDurationTrimSeconds (spec v24 §8).
	 *
	 * Published so that code which must not OUTLIVE a slide can ask how long one is instead of
	 * mirroring the arithmetic in a knob of its own (spec v24 §0). The bots are the live caller:
	 * TraceBotController's slide branch clamps its crouch hold to this, because BotSlideHoldSeconds
	 * was an absolute whose documentation claimed it tracked the slide and did not — when §8 cut the
	 * slide to 0.86 s the bots went on holding crouch for 1.60 s and crouch-walked the difference.
	 *
	 * Unlike the members above this is a pure config read, not live state, so it is meaningful
	 * BEFORE a slide starts — which is exactly when a caller deciding how long to hold needs it.
	 */
	float GetSlideDurationForAudit() const { return GetSlideDuration(); }

	/** The well-timed multiplier actually in force, both readings of spec v9 §7 resolved. */
	float GetSlideJumpWindowSpeedBonusForAudit() const { return GetSlideJumpWindowSpeedBonus(); }

	/**
	 * PATCH 28 §5. The overbounce ClipVelocityAgainstPlane() is actually called with on the shipped
	 * path, clamp applied — i.e. the third argument ComputeSlideVector() passes.
	 *
	 * IT IS THE THIRD ARGUMENT OF A PUBLIC STATIC, AND THAT IS THE WHOLE REASON IT IS HERE.
	 * ClipVelocityAgainstPlane is public precisely so Trace.Move.Surf can drive the SHIPPED arithmetic
	 * rather than a harness-local copy of it, and a public function driven with a private argument is
	 * only half of that promise: the table was calling it with a literal 1.f and therefore answered
	 * the same thing at every value of SurfOverbounce, which is how W8-KNOBS's 1.15 override produced
	 * a byte-identical clip table. Forwarding rather than publishing the tuning getter keeps the knob
	 * accessors grouped where every other one lives, per the block comment above.
	 */
	float GetSurfOverbounceForAudit() const { return GetSurfOverbounce(); }

	/** Planar retention a slide-jump applies OUTSIDE the well-timed window. */
	float GetSlideJumpHorizontalRetentionForAudit() const { return GetSlideJumpHorizontalRetention(); }

	/** Length of the well-timed window, in seconds, at the end of a slide. */
	float GetSlideJumpWindowSecondsForAudit() const { return GetSlideJumpWindowSeconds(); }

	/** SPEC v26 §3. Slide-jumps taken in the CURRENT chain. 0 when no chain is running. */
	int32 GetSlideJumpChainBoostsForAudit() const { return SlideJumpChainBoosts; }

	/**
	 * SPEC v26 §3. The chain's measured ceiling in uu/s, or 0 while it has not been reached yet.
	 *
	 * Published rather than recomputed in the harness for the reason the note above
	 * GetSlideJumpWindowSpeedBonusForAudit's neighbours gives: an expected value re-derived in a test
	 * file is a SECOND OPINION about the rule, and this project has had two of those disagree.
	 */
	float GetSlideJumpChainCeilingForAudit() const { return SlideJumpChainCeiling; }

	/** SPEC v26 §3. The speed at which a chain is considered spent, for the pawn as it stands now. */
	float GetSlideJumpChainResetSpeedForAudit() const { return GetSlideJumpChainResetSpeed(); }

	/** SPEC v26 §3. Whether the ceiling is in force, dev A/B arm and designer switch both folded in. */
	bool IsSlideJumpChainCapEnabledForAudit() const { return IsSlideJumpChainCapEnabled(); }

	/** SPEC v26 §3. Boosts a chain may compound before the ceiling closes, clamped as shipped. */
	int32 GetSlideJumpChainCapBoostsForAudit() const { return GetSlideJumpChainCapBoosts(); }

#if !UE_BUILD_SHIPPING
	// Dev-only with the CorrectionCount/CorrectionError* members they read (declared in the dev
	// block below) and with their one caller, TraceMovementAuditV16.cpp. Unguarded, these inline
	// bodies were the movement header's Shipping compile break.

	/** Server movement corrections this client has received. Always 0 on an authority. */
	int32 GetCorrectionCountForAudit() const { return CorrectionCount; }

	/** Worst single correction distance, in uu. */
	float GetCorrectionWorstForAudit() const { return CorrectionErrorWorst; }

	/** Mean correction distance, in uu, over every correction so far. */
	float GetCorrectionMeanForAudit() const
	{
		return CorrectionErrorTotal / static_cast<float>(FMath::Max(1, CorrectionCount));
	}
#endif

	// --- Per-move intents ------------------------------------------------------------------------
	//
	// Public because the saved move and the compressed-flag unpack both drive them. Not UPROPERTYs:
	// they are per-move scratch state. bWantsToDash is one-shot (consumed at the end of the move);
	// bWantsToSlide is a level, held for as long as the key is down.

	uint8 bWantsToDash : 1;
	uint8 bWantsToSlide : 1;

	/**
	 * Demo 19 item 4. The jump key as a level, riding FLAG_Custom_1. WRITE IT THROUGH SetJumpHeld()
	 * and READ IT THROUGH IsJumpHeld() — the raw bit is only half the state, because the staleness
	 * stamp below is what stops a missing release from lasting forever.
	 *
	 * Nothing in the movement simulation reads it: it is carried here because this component already
	 * has a client -> server channel for exactly this shape of fact and the ability layer does not.
	 */
	uint8 bWantsToJumpHold : 1;

	/**
	 * FPlatformTime::Seconds() at the last SetJumpHeld(true). Real time, not match time, and
	 * deliberately NOT saved-move state: it is a local freshness watchdog on each machine, never an
	 * input to a simulated move, so it cannot desync prediction.
	 */
	double JumpHeldStampSeconds = 0.0;

protected:
	/** Locks the direction, spends a charge, starts the dash window and launches the velocity. */
	void BeginDash();

	/** Locks the direction, sets the entry speed and starts the slide's fixed-length window. */
	void BeginSlide();

	// --- Wall jump (spec v8 §7) ------------------------------------------------------------------

	/**
	 * Consumes an open wall-jump window and writes the redirected launch velocity. Called from
	 * DoJump(), i.e. from inside the engine's own predicted jump entry point, so it rides the saved
	 * move exactly as the slide-jump does.
	 *
	 * @return true if a wall jump was actually taken. False leaves Velocity untouched.
	 */
	bool TryWallJump();

	/**
	 * THE ONE EXIT. Ends a slide and hands the player back WITH their momentum.
	 *
	 * Every way out of a slide routes through here — the duration expiring, the decay reaching the
	 * exit speed, the key coming up, walking off a ledge, and a dash cancelling it — because the
	 * exit speed rule has to be identical on all of them or "slide cancel" becomes a different (and
	 * better, and unintended) move than "let the slide finish".
	 *
	 * Also where the 0.8s between-slides buffer is charged, because the buffer is measured from the
	 * END of a slide and this is the only place a slide ends.
	 *
	 * Idempotent: safe to call when no slide is running, in which case it does nothing at all.
	 */
	void EndSlide();

	/** True if a slide could start on this exact frame (ground, off cooldown, moving fast enough). */
	bool CanStartSlide() const;

	/**
	 * Bleeds planar speed down to DashExitSpeedMultiplier × GetMaxSpeed() in one step.
	 *
	 * Used the frame a dash ends. Without it CalcVelocity only sheds the excess through the
	 * (now deliberately gentle) overspeed bleed, which at DashSpeed would keep the player moving
	 * long after IsDashing() — and therefore the trail rule — said the dash was over. Doing it here
	 * rather than through a velocity cap is what keeps it predictable: it happens on exactly the
	 * frame the saved timer crosses zero, and that frame replays identically on client and server.
	 *
	 * DashExitSpeedMultiplier > 1 is what stops this being a "state transition that resets the
	 * velocity vector" (spec §2.4): the dash hands back a fast player, not a walking one, and the
	 * remainder then bleeds off through the overspeed friction like any other carried momentum.
	 */
	void ApplyDashExitSpeed();

	/**
	 * SPEC v7 §5. Writes DashDirection × DashSpeed into Velocity — the single writer shared by the
	 * launch in BeginDash() and the per-frame re-assert in OnMovementUpdated().
	 *
	 * All three axes. The only exception is a pawn still on the ground whose dash is not aiming up,
	 * whose Z is held at zero because PhysWalking would discard it anyway.
	 */
	void ApplyDashVelocity();

	// --- The momentum model (both stateless; see the header note on prediction) -------------------

	/**
	 * Quake/Source air acceleration for one sub-step. Assumes Velocity.Z has already been stripped
	 * by PhysFalling and never writes it.
	 *
	 * A THIN WRAPPER since spec v18 §1a: it resolves the wish direction from Acceleration, applies the
	 * v10 §5 into-wall projection (the only part that needs member state), and hands the rest to
	 * ComputeAirStrafeStep(). Put new air-model arithmetic in THAT function, not here, or the measured
	 * verification stops covering it.
	 */
	void ApplySourceAirAcceleration(float DeltaTime);

	/**
	 * One sub-step of the carried-momentum bleed, used instead of the engine's braking whenever
	 * planar speed exceeds the ground limit. Steers, then sheds only the EXCESS, and can never drop
	 * the pawn below the speed normal ground movement would have given it anyway.
	 */
	void ApplyGroundOverspeedBleed(float DeltaTime);

	// Settings are read live (never cached in the constructor) so both ends of the wire always
	// agree with the config, and so designers can retune without a rebuild.
	float GetDashSpeed() const;
	float GetDashDuration() const;
	float GetDashCooldown() const;

	/** DashDuration + DashCooldown: the cooldown is measured from dash START, as it always was. */
	float GetDashRechargeWindow() const;

	/** Multiple of the ground speed limit a dash is allowed to hand back on exit. >= 1. */
	float GetDashExitSpeedMultiplier() const;

	/**
	 * SPEC v7 §5. Ceiling on the POSITIVE Z velocity a dash may hand back when its window closes.
	 *
	 * JumpZVelocity, deliberately: the number every other vertical launch in the kit is expressed as
	 * a multiple of, and one that is already engine-configurable. It exists because the air-strafe
	 * ceiling is planar-only and so bounds nothing vertical — see the header note "THE CLIMB". If a
	 * designer ever wants this independent of the jump it wants a UTraceSettings knob (named in the
	 * integration notes), not a literal here.
	 */
	float GetDashExitVerticalSpeedLimit() const;

	/**
	 * SPEC v7 §5. The aim rotation this move's dash direction must be composed from.
	 *
	 * Live control rotation normally; ReplayAimRotation while ACharacter::bClientUpdating, i.e.
	 * inside ClientUpdatePositionAfterServerUpdate's replay loop. THAT BRANCH IS THE WHOLE REASON A
	 * VERTICAL DASH DOES NOT RUBBER-BAND — read the header note "PREDICTION" before touching it.
	 */
	FRotator GetDashAimRotation() const;

	// Slide tuning, same rule: read live, every frame, never cached.
	/** ONE PRESS BUYS EXACTLY THIS MANY SECONDS (spec v5 §3). Release does not shorten it. */
	float GetSlideDuration() const;
	float GetSlideDeceleration() const;
	float GetSlideExitSpeedRetention() const;
	float GetSlideExitMaxSpeedMultiplier() const;

	/** Seconds between slides, measured from the END of the previous one. */
	float GetSlideCooldownSeconds() const;

	/** Entry speed × this IS the slide speed. 1.0, and that is now a decision (spec v4 §1). */
	float GetSlideEntrySpeedMultiplier() const;

	// Slide-jump tuning. Read live, like everything else.
	bool  IsSlideJumpEnabled() const;
	float GetSlideJumpHorizontalRetention() const;
	float GetSlideJumpZMultiplier() const;

	/**
	 * Length of the well-timed window, measured backwards from the slide's END — and, deliberately,
	 * ALSO the length of the coyote window after the slide has ended. One knob for both because they
	 * are one continuous window in the player's hands: the moment the slide runs out is the moment
	 * they are aiming at, and half of the presses that mean to hit it land a frame or two late.
	 */
	float GetSlideJumpWindowSeconds() const;

	/** Multiplier applied to the retention when the hop lands inside the window. 1.10 -> 1.25 (v5 §3). */
	float GetSlideJumpWindowSpeedBonus() const;

	/**
	 * Multiplier applied to the launch's VERTICAL velocity when the hop lands inside the window —
	 * spec v5 §3's "make the well-timed case feel distinctly better".
	 *
	 * A speed bonus alone is nearly invisible at a glance: 25% more planar speed on a 0.9s arc reads
	 * as "I think that went further". Height is the readable channel — the camera rises, the arc
	 * lengthens for free, and the two bonuses multiply into a jump that goes somewhere a mistimed one
	 * cannot reach. Floored at 1 for the same reason as the speed bonus: hitting the window must
	 * never be worth less than missing it.
	 */
	float GetSlideJumpWindowZBonus() const;

	// --- SPEC v26 §3 — the chain ceiling ---------------------------------------------------------
	//
	// "Add a ceiling to slide jump momentum boosts, so that you can't chain them over and over to go
	// faster and faster [...] cap it at what the momentum is after you do two consecutive slide
	// boosts."
	//
	// The -20% half of §3 needs no accessor of its own: it is a factor inside
	// GetSlideJumpWindowSpeedBonus(), so every existing reader (the audit, the V9TUNING report,
	// Elle's seam, the SLIDEJUMP debug line) reports the shipped number without being told.

	/** False turns the ceiling off entirely and restores v25's uncapped compounding. */
	bool IsSlideJumpChainCapEnabled() const;

	/**
	 * How many boosts of a chain are allowed to compound before the ceiling closes. The note's TWO.
	 *
	 * Clamped to at least 1 here: a zero would cap a chain at the speed it started with, which is not
	 * a ceiling on the boost, it is the deletion of the move.
	 */
	int32 GetSlideJumpChainCapBoosts() const;

	/**
	 * The planar speed at or below which the pawn is deemed to have GIVEN THE MOMENTUM BACK, ending
	 * the current chain. Derived from the pawn's own live GetMaxSpeed(), never from a typed number,
	 * so a carrier, a knife and every ability speed passive move it with them.
	 *
	 * Meaningless while sliding or dashing (the planar speed is then that ability's, not the
	 * player's), which is why the only caller checks those first.
	 */
	float GetSlideJumpChainResetSpeed() const;

	/**
	 * Seconds until the running slide will end, BY EITHER ROUTE. 0 when no slide is running.
	 *
	 * A slide has two exits, and the timing window has to respect both or it is unhittable half the
	 * time. SlideTimeRemaining counts down SlideDuration; but the slide ALSO ends the moment
	 * SlideSpeed decays past SlideExitSpeedFraction x WalkSpeed, and at the shipped numbers that is
	 * the exit a slide entered at walking pace actually takes.
	 *
	 * MEASURED, and this is why the function exists: entering at 800 uu/s, SlideDeceleration 260
	 * reaches the 400 uu/s exit threshold after 1.54 s, while SlideDuration is 1.8 s — so
	 * SlideTimeRemaining was still 0.26 s when the slide ended and never once dipped under the 0.20 s
	 * window. Every slide-jump out of a normal-speed slide scored "mistimed" no matter when it was
	 * pressed, which would have read to a player as the reward being broken.
	 *
	 * Pure function of saved-move state and config, so it replays exactly.
	 */
	float GetSlideTimeLeft() const;

	// Air / momentum tuning.
	bool  IsSourceAirAccelerationEnabled() const;
	float GetAirAcceleration() const;
	float GetAirMaxWishSpeed() const;
	float GetMaxAirSpeed() const;

	// --- The air-strafe accumulation ceiling (spec v5 §1) ----------------------------------------
	//
	// Two INDEPENDENT limiters, either of which can be turned off on its own so the user can A/B
	// "falloff only" against "cap only" against "neither".

	/** Diminishing returns on the strafe's speed GAIN. Never affects the turn. */
	bool  IsAirStrafeFalloffEnabled() const;

	/**
	 * SPEC v9 §8. One scalar over BOTH air-strafe caps — "move the asymptote on momentum slightly
	 * higher". Scaling them together slides the curve up without changing its shape; scaling only
	 * one would re-shape the falloff instead of moving it. Ships at 1.10 (a nudge, per the spec:
	 * the Demo 5 cap stays, it just sits 10% further out). Name-bound as AirStrafeAsymptoteScale.
	 */
	float GetAirStrafeAsymptoteScale() const;

	/** Speed at which the falloff starts. Below it a strafe is worth exactly what it was in Demo 5. */
	float GetAirStrafeSoftCapSpeed() const;

	/** Absolute ceiling on speed BUILT in the air. Also the point the falloff decays to zero at. */
	float GetAirStrafeHardCapSpeed() const;

	/** Falloff shape. 1 = linear taper, 2 = the shipped quadratic, higher = a longer flat top. */
	float GetAirStrafeFalloffExponent() const;

	/** The hard cap as a backstop in its own right, usable with the falloff switched off. */
	bool  IsAirStrafeHardCapEnabled() const;

	/**
	 * Fraction of a strafe's speed gain that is actually granted at this planar speed: 1 below the
	 * soft cap, 0 at the hard cap, ((Hard-Speed)/(Hard-Soft))^Exponent in between.
	 *
	 * Pure function of (Speed, config) — no state, so it replays exactly, and it is the one place the
	 * curve is defined. The measurement harness prints it at a range of speeds.
	 */
	float GetAirStrafeGainScale(float PlanarSpeed) const;

	// --- SPEC v18 §1a: THE OPPOSITION BRAKE ------------------------------------------------------

	/**
	 * SPEC v18 §1a. Deceleration, in uu/s², applied when the air input points AGAINST travel.
	 *
	 * "When inputting solely A or D and jumping, when you go the opposite direction during the jump
	 * your momentum doesn't change at all. We want it so doing so slows down your momentum."
	 *
	 * SCALED BY HOW OPPOSED THE INPUT IS — by the negative part of dot(wish, travel), so this value is
	 * the rate at a dead-on 180° reversal, half of it at 120°, and EXACTLY ZERO at 90° and anywhere
	 * inside it. That last clause is the load-bearing one: the air strafe is a ~90° input, so the
	 * brake is arithmetically absent from every frame of a normal strafe and the skill ceiling cannot
	 * have moved. A binary "is the input opposed" test would have made a 91° counter-steer feel like
	 * hitting a wall, which is why it is a ramp.
	 *
	 * WHY IT IS NEEDED AT ALL, given that the raw projection formula already loses speed on a
	 * reversal: see ComputeAirStrafeStep(). The spec v5 §1 gain ceiling clamps the frame's speed DELTA
	 * at zero and then rescales the turned vector back to the entry speed, which erases that loss
	 * exactly. Left alone it produces "momentum doesn't change at all" to the last float bit.
	 *
	 * 2200 uu/s² is deliberately well under AirAcceleration (8000): a full reversal bleeds at about a
	 * quarter of the rate a strafe builds, so 1000 uu/s takes ~0.45 s to kill. That reads as "I slowed
	 * myself down", not "I hit a wall" — the spec asks for the first in as many words.
	 *
	 * Pure function of config, so it needs no saved-move state and replays exactly.
	 */
	float GetAirStrafeOpposingDeceleration() const;

	// --- Wall-jump tuning (spec v8 §7) -----------------------------------------------------------
	//
	// Bound BY NAME against UTraceSettings (TraceMoveKnob), exactly like the spec v5 knobs: the
	// properties do not exist in UTraceSettings yet, and the alternative — hardcoded literals — would
	// ship a brand-new mechanic with nothing to tune. The moment the integrator adds the UPROPERTYs
	// (the names are listed in the report) the ini takes over with no change here. BeginPlay's
	// MOVEKNOB report prints BOUND or FALLBACK for every one of them, every run.

	/** Master switch, so "is the wall jump making this worse" is one ini edit rather than a rebuild. */
	bool  IsWallJumpEnabled() const;

	/** Seconds after touching a wall in which a jump press is still a wall jump. */
	float GetWallJumpWindowSeconds() const;

	/** Fraction of the incoming planar SPEED the reflected launch keeps. 1.0 is pure preservation. */
	float GetWallJumpSpeedRetention() const;

	// SPEC v9 §5's GetWallJumpMantleLockoutSeconds() lived here. It put the mantle on cooldown after a
	// launch so an auto-mantle could not undo the wall jump a frame later. With the mantle deleted in
	// v12 §5 there is nothing left to lock out, so the knob is gone rather than left reading a value
	// nothing consumes. The wall jump itself is untouched: see TryWallJump().

	/** Flat uu/s pushed straight out along the wall normal, on top of the reflection. */
	float GetWallJumpOutwardImpulse() const;

	/** Vertical launch, as a multiple of JumpZVelocity — the unit every other launch here uses. */
	float GetWallJumpVerticalMultiplier() const;

	/** Consecutive wall jumps allowed without touching the ground. The anti-ladder cap. */
	int32 GetWallJumpMaxConsecutive() const;

	/** Largest |Normal.Z| still counted as a wall. Above it the surface is a ramp, not a face. */
	float GetWallJumpMaxNormalZ() const;

	// --- PATCH 28 §5: surf tuning ----------------------------------------------------------------
	//
	// Name-bound against UTraceSettings like every knob added since spec v5, so the ini can drive them
	// the moment the UPROPERTYs land, and BeginPlay's MOVEKNOB report says BOUND or FALLBACK for each
	// one rather than letting a missing property be silent.

	/** Master switch. Off makes ComputeSlideVector() and CanStepUp() pure Super calls. */
	bool IsSurfEnabled() const;

	/**
	 * Floor of the surf band. Below this a face is a WALL and belongs to the wall jump, not to surf.
	 *
	 * Deliberately ABOVE GetWallJumpMaxNormalZ()'s 0.4 default so the two bands cannot overlap: a
	 * surface is a wall, a surf plane, or a floor, and never two of them at once.
	 */
	float GetSurfMinNormalZ() const;

	/** Source's ClipVelocity overbounce. 1.0 is a pure projection and is what Source uses. */
	float GetSurfOverbounce() const;

	/**
	 * How long a surf survives without a fresh contact.
	 *
	 * A ramp is swept several times per move and a fast pawn can be genuinely airborne for a frame or
	 * two between facets of a curved face. Without a grace the state would flicker off and on across
	 * every facet joint, which is exactly the "transitions between ramp faces" case this feature is
	 * supposed to make seamless — and a flickering ceiling is a flickering clamp.
	 */
	float GetSurfContactGraceSeconds() const;

	/** Multiple of GetAirStrafeHardCapSpeed() the surf may reach. The bound, as a ratio. */
	float GetSurfSpeedCeilingMultiplier() const;

	// --- DEMO 29 ITEM 4 tuning -------------------------------------------------------------------

	/**
	 * Whether a WALKING pawn that leans into a surf plane leaves the ground and starts a ride.
	 * Off restores "a rail is a thing you scrape along", which is the whole of complaint 4(b).
	 */
	bool IsSurfGroundEntryEnabled() const;

	/**
	 * How much of the approach must be heading INTO the face before a floor-level run becomes a ride,
	 * uu/s. Derived from GetMaxSpeed() so it means the same thing after a walk-speed retune; at the
	 * shipped numbers it is 11.5 degrees off parallel at a full run.
	 */
	float GetSurfGroundEntryMinApproachSpeed() const;

	/** Seconds the ground overspeed bleed is held off after a ride ends. The "carry it" clock. */
	float GetSurfExitCarrySeconds() const;

	/** Fraction of the ordinary overspeed bleed still applied during that window. 0 holds the speed. */
	float GetSurfExitCarryBleedScale() const;

	/** How much of the ride's descent the landing rotates into the floor. 1 is a real curve; 0 is UE. */
	float GetSurfExitRolloutRetention() const;

	// --- SPEC v10 §5, THE TWO NEW WALL-JUMP KNOBS -------------------------------------------------
	//
	// See the "WHY THE WALL JUMP STILL FELT STICKY" block at the top of the .cpp for the measurement
	// these two came out of. Neither is a re-shave of the v9 numbers; they fix two different, measured
	// causes that the v9 window cut could not have touched.

	/**
	 * SPEC v10 §5 (CAUSE 1). Seconds after a wall jump during which air-strafe input may NOT push back
	 * into the face the pawn just launched off.
	 *
	 * THE MEASUREMENT THIS EXISTS FOR: a player who ran into a wall is, by definition, still holding
	 * the stick INTO it, so ApplySourceAirAcceleration (8000 uu/s²) spends its whole allowance
	 * cancelling the launch. Peak separation is v_out²/2a, and because AirMaxWishSpeed caps return
	 * speed at 160 uu/s, every wall jump taken FROM THE AIR launches at only ~543 uu/s outward and
	 * peaks at 18 uu — against a 34 uu capsule radius. The pawn never gets half a capsule off the
	 * face, which is precisely "sticking to the wall for a moment too long". See the header for the
	 * full derivation and for why the first jump of a chain hid this from every previous measurement.
	 *
	 * IT IS NOT AN AIR-CONTROL LOCKOUT. Only the component of the wish direction pointing INTO the
	 * launched-off face is removed; everything tangential and everything outward still accelerates at
	 * full strength, so the player keeps the whole Source air-strafe during the window and can still
	 * carve the launch anywhere except back into the wall. Removing air control outright would trade
	 * one bad feeling for another.
	 *
	 * Round-tripped through the saved move with WallJumpLaunchNormal — it changes the velocity a
	 * replayed move produces, so it has to be, or it would rubber-band.
	 */
	float GetWallJumpControlLockoutSeconds() const;

	/**
	 * SPEC v10 §5 (CAUSE 2). Seconds a mid-air jump press that found no wall is remembered for.
	 *
	 * THE BUG IT FIXES: ACharacter::JumpMaxHoldTime is 0, so bPressedJump lives for exactly one tick,
	 * and CheckJumpInput runs at the START of PerformMovement while HandleImpact — the only thing that
	 * opens a wall window — runs DURING the physics step that follows it. A press made on the tick
	 * BEFORE contact is therefore consumed by a DoJump that correctly refuses it, and is then gone.
	 * The player pressed jump "right as they hit the wall", nothing happened, and they are left
	 * scraping down the face waiting to press again — with v9's 0.15 s window, missing early is now
	 * the easiest way to miss. Buffering the press converts that dead frame into a wall jump on the
	 * contact frame itself, which is also one whole frame earlier than the CheckJumpInput path could
	 * ever deliver.
	 *
	 * Consumed in OnMovementUpdated, after the window has been ticked and therefore after HandleImpact
	 * has had its say for this frame. Saved-move state, like every other clock here.
	 */
	float GetWallJumpInputBufferSeconds() const;

	// --- SPEC v10 §1, THE KNIFE MOVEMENT PROFILE KNOBS --------------------------------------------
	//
	// All three are multipliers over the BASE values rather than absolute speeds, which is what
	// "tunable separately from the base values" buys: retuning WalkSpeed or the asymptote moves the
	// knife with it and the knife stays exactly 30% faster than whatever the gun is.

	/** SPEC v10 §1. Ground speed multiplier while the knife is out. 1.30 = the spec's "30% faster". */
	float GetKnifeMoveSpeedMultiplier() const;

	/** SPEC v10 §1. Multiplier on AirStrafeSoftCapSpeed while the knife is out. */
	float GetKnifeAirStrafeSoftCapMultiplier() const;

	/**
	 * SPEC v10 §1. Multiplier on AirStrafeHardCapSpeed — and on MaxAirSpeed — while the knife is out.
	 *
	 * MaxAirSpeed IS SCALED BY THIS TOO, and it has to be. ApplySourceAirAcceleration takes the
	 * TIGHTER of the two ceilings (min(MaxAirSpeed, HardCap)), so with MaxAirSpeed left at 1600 a hard
	 * cap raised past it would be silently inert and the knob would do nothing at all — the exact
	 * "misnamed knob silently does nothing" failure this project keeps paying for, in a different
	 * costume. Scaling both keeps the hard cap the binding limit at every setting.
	 */
	float GetKnifeAirStrafeHardCapMultiplier() const;

	bool  IsLandingMomentumPreserved() const;
	float GetGroundOverspeedFriction() const;
	float GetGroundOverspeedBraking() const;
	float GetGroundOverspeedTurnRate() const;

	// --- Ledge tuning (spec v5 §7; the eight Mantle* knobs were deleted in v12 §5) ----------------

	/**
	 * How long the ability layer keeps believing the pawn is grounded after contact is lost.
	 *
	 * The ledge hysteresis. 0 restores exactly the Demo 5 behaviour, which is what the desync was
	 * measured against.
	 */
	float GetLedgeGroundGraceSeconds() const;

	/**
	 * Pushes UTraceSettings values into the engine-owned fields the physics step reads directly
	 * (MaxWalkSpeed, and AirControl, which decides how much of Acceleration survives
	 * GetFallingLateralAcceleration before our air model ever sees it).
	 *
	 * BeginPlay copies them once, which is exactly the caching this file's own comments warn
	 * against: with only that copy, retuning WalkSpeed in Project Settings during PIE did nothing
	 * until the map was reloaded. Called once per simulated move so the editor's values are live,
	 * and cheap enough to be unconditional (two float compares). Not a prediction hazard: both ends
	 * read the same config, and a designer editing the number mid-PIE is a single-process situation.
	 */
	void RefreshEngineTunablesFromSettings();

	// --- Dash state (all saved/restored by FSavedMove_Trace) --------------------------------------

	/** Seconds of dash left. */
	float DashTimeRemaining;

	/** Charges available. Resized by GetMaxDashCharges() transitions; spent by BeginDash(). */
	int32 DashCharges;

	/**
	 * Seconds until the next charge is handed back, or 0 when the pool is full. One timer serves the
	 * whole pool: it restarts itself while charges are still missing, which is what makes two
	 * charges refill sequentially rather than simultaneously.
	 */
	float DashRechargeRemaining;

	/**
	 * Pool size as of the previous move. The pool is resized from the DELTA against
	 * GetMaxDashCharges() so that picking the Core up grants the extra charge immediately and
	 * dropping it takes exactly one back — see the header note on the prediction seam.
	 */
	int32 LastMaxDashCharges;

	/**
	 * Unit world-space direction locked at activation. SPEC v7 §5 made this FULLY 3D — it carries a
	 * Z component now, and every consumer must use all three axes. It is saved and restored by
	 * FSavedMove_Trace (SavedDashDirection) as a whole vector, which is what keeps a vertical dash
	 * predicting identically on both ends of the wire.
	 */
	FVector DashDirection;

	/**
	 * SPEC v7 §5. The control rotation of the move currently being REPLAYED, loaded by
	 * FSavedMove_Trace::PrepMoveFor from the base class's SavedControlRotation.
	 *
	 * Read only through GetDashAimRotation(), and only while ACharacter::bClientUpdating is true —
	 * that gate is what makes the value impossible to leak into a live move, so this needs no
	 * clearing. Never replicated and never sent: the server already has this rotation, because it is
	 * the same one FCharacterNetworkMoveData packs into the ServerMove and applies to the controller
	 * before running MoveAutonomous.
	 */
	FRotator ReplayAimRotation;

	/** True once PrepMoveFor has loaded ReplayAimRotation at least once. Paired with bClientUpdating. */
	uint8 bReplayAimRotationValid : 1;

	// --- Slide state (all saved/restored) ---------------------------------------------------------

	float SlideTimeRemaining;

	/**
	 * Seconds left of the between-slides buffer. Charged in EndSlide() — spec §2.3 asks for a
	 * buffer "between slides", which is a from-END measurement; charging it at slide start (as the
	 * old SlideCooldown did) made the actual gap SlideCooldown minus SlideDuration, a number the
	 * designer had to compute instead of read.
	 */
	float SlideCooldownRemaining;

	float SlideSpeed;
	FVector SlideDirection;

	// SlideCommitRemaining WAS HERE AND IS DELETED (spec v5 §3). It held the window in which
	// releasing crouch would not cancel the slide. A one-shot ability cannot be cancelled by the key
	// at all, so the whole idea of a PARTIAL commit is gone: every slide is committed for its whole
	// duration. GetSlideMinCommitSeconds() went with it, and UTraceSettings::SlideMinCommitSeconds
	// should be deleted too rather than left in the ini doing nothing.

	/**
	 * Seconds of "I pressed crouch and meant it" left over from a press that could not start a slide
	 * yet — because the pawn was mid-dash, or still in the air.
	 *
	 * Without this, "dash and then slide out of it" is impossible unless the player releases and
	 * re-presses crouch inside the 180ms the dash lasts, and "press crouch just before landing"
	 * silently does nothing. Both are things players do constantly — and the second one is now the
	 * primary way to convert an air-strafe into a slide, which is the whole Apex loop.
	 */
	float SlideBufferRemaining;

	/**
	 * COYOTE TIME FOR THE SLIDE-JUMP: seconds after a slide ended during which a jump still counts as
	 * a slide-jump. Charged in EndSlide() to GetSlideJumpWindowSeconds(), consumed by DoJump().
	 *
	 * A slide ends on its own after SlideDuration, and a player who jumps two frames later did mean
	 * to slide-jump. Without this, the payoff move would fail at random for reasons the player cannot
	 * see, which is worse than not having it: they would land in ground friction, watch the carry
	 * evaporate, and conclude the mechanic is broken.
	 *
	 * SAVED-MOVE STATE, like every other clock here. A correction that landed mid-window and lost it
	 * would replay a slide-jump as an ordinary jump, and the two ends would disagree about a velocity
	 * difference of several hundred uu/s — the single most visible rubber-band the kit could produce.
	 */
	float SlideJumpGraceRemaining;

	/**
	 * Whether the slide that just ended was inside its well-timed window when it ended, so that a hop
	 * taken during the coyote grace above is scored the same as one taken a frame earlier.
	 *
	 * Stored rather than recomputed because once the slide is over SlideTimeRemaining is 0 and the
	 * information is gone. It also keeps the grace honest: a slide CANCELLED early (crouch released
	 * after the commit window, or a dash) ends nowhere near its window, so its grace is worth the
	 * ordinary retention and not the bonus.
	 *
	 * Saved-move state for the same reason as the clock beside it.
	 */
	uint8 bSlideJumpGraceWellTimed : 1;

	/**
	 * SPEC v26 §3 — how many slide-jumps the CURRENT CHAIN has taken. 0 means no chain is running.
	 *
	 * A chain is a run of slide-jumps taken without ever giving the momentum back. It is ended in
	 * OnMovementUpdated the moment the pawn is back on its feet at or below
	 * GetSlideJumpChainResetSpeed(), which is what "consecutive" means: the boosts stop being
	 * consecutive when there is nothing left to compound.
	 *
	 * SAVED-MOVE STATE, for exactly the reason SlideJumpGraceRemaining above is. This counter decides
	 * whether the ceiling clamps, so a correction that lost it would replay a clamped hop as an
	 * unclamped one and the two ends would disagree about several hundred uu/s on the most visible
	 * frame in the kit — the same failure the grace window's capture exists to prevent.
	 */
	int32 SlideJumpChainBoosts;

	/**
	 * SPEC v26 §3 — the ceiling itself, in uu/s: the highest planar launch speed this chain reached
	 * during its first GetSlideJumpChainCapBoosts() boosts. 0 while no chain is running.
	 *
	 * MEASURED, NOT COMPUTED. It is one of the chain's own launches, which is what makes it relative:
	 * it already contains the boost knobs, the character's passive and the speed the player brought
	 * into the chain, and it moves when any of those move. A formula (entry x multiplier^2) would be
	 * wrong here because a slide DECAYS while the player waits for the well-timed window, so the
	 * speed actually reached after two boosts depends on how long each of their slides ran.
	 *
	 * Saved-move state alongside the counter, and for the same reason.
	 */
	float SlideJumpChainCeiling;

	/**
	 * bWantsToSlide as it stood at the END of the previous move, so the air fast-fall can fire on the
	 * press EDGE rather than continuously. Saved state: without it a replayed move would see a stale
	 * edge and fast-fall a second time, cancelling a jump the player did make.
	 */
	uint8 bSlideHeldLastMove : 1;

	/**
	 * Whether the pawn was airborne at the END of the previous move — i.e. this move can detect a
	 * LANDING as a transition rather than as a state.
	 *
	 * This is what makes "hold crouch through a landing" start a slide, which is the whole Apex loop
	 * and is spec v3 §2.4's jump->slide transition. It cannot be done from the press edge alone,
	 * because a crouch press made in the air is CONSUMED by the fast-fall (deliberately: one press,
	 * one meaning) and the input buffer is far shorter than a jump. Without this bit, measured
	 * behaviour was that landing fast and holding crouch produced no slide at all, and 1293 uu/s of
	 * carried momentum simply bled away.
	 *
	 * Charges the buffer exactly once per landing, so holding the key still cannot chain slides —
	 * the next move sees the pawn already grounded and does nothing.
	 *
	 * SAVED-MOVE STATE for the same reason as bSlideHeldLastMove: a replay that lost it would decide
	 * a mid-air move was a landing and start a slide the original never started.
	 */
	uint8 bWasAirborneLastMove : 1;

	// --- Ledge state (saved/restored by FSavedMove_Trace) -----------------------------------------
	//
	// One field, and it is the survivor of the v12 §5 mantle removal. See the ledge section of the
	// header: this is fix (2) of the two that actually address the Demo 5 rubber-band, and it is not
	// mantle state even though it was written in the same pass.

	/**
	 * Seconds of "the ability layer still counts this pawn as grounded" left after ground contact is
	 * lost. Refilled to LedgeGroundGraceSeconds on every grounded move.
	 *
	 * SAVED-MOVE STATE. It gates EndSlide(), the fast-fall and the landing transition, so a replay
	 * that lost it would resolve a ledge blip differently from the original — which is precisely the
	 * class of divergence it was added to remove.
	 */
	float GroundGraceRemaining;

	// --- Wall-jump state (all saved/restored by FSavedMove_Trace) — spec v8 §7 -------------------

	/**
	 * Outward normal of the last wall touched while airborne, flattened to the horizontal plane and
	 * normalised. Zero when no window is open.
	 *
	 * Written only from HandleImpact(), which runs identically on client, server and replay.
	 */
	FVector WallJumpNormal;

	/** Seconds of "a jump press right now is a wall jump" left. Charged by HandleImpact(). */
	float WallJumpWindowRemaining;

	/**
	 * THE VELOCITY THE PAWN HIT THE WALL WITH, planar, captured in HandleImpact() — and the difference
	 * between "carry momentum in a new direction" and a 420 uu/s nudge.
	 *
	 * MEASURED on a client: reading Velocity at press time gives entry=0 for a head-on approach. By
	 * then UCharacterMovementComponent::PhysFalling has re-derived Velocity from the distance the
	 * capsule actually travelled that sub-step, and a pawn stopped dead by a wall travelled nothing.
	 * The momentum the spec asks to redirect only exists on the frame of contact, so it is captured
	 * there and held for the length of the window.
	 *
	 * Saved-move state like the rest of the window: a correction landing between the contact and the
	 * press must not replay the launch from a different approach speed.
	 */
	FVector WallJumpEntryVelocity;

	/**
	 * Consecutive wall jumps since the pawn was last grounded. Reset through IsGroundedForAbilities(),
	 * not IsMovingOnGround(), so a one-frame ledge blip cannot refill the ladder on one machine only.
	 */
	int32 WallJumpsSinceGround;

	// --- SPEC v10 §5 state (saved/restored by FSavedMove_Trace, like everything above) -------------

	/**
	 * The outward normal of the face the LAST wall jump launched off, held for
	 * WallJumpControlLockoutRemaining and zero otherwise.
	 *
	 * Distinct from WallJumpNormal on purpose: that one is the face a jump COULD launch off and is
	 * cleared the instant the launch happens, so it is gone exactly when this is needed.
	 */
	FVector WallJumpLaunchNormal;

	/** Seconds left in which air input may not push back into WallJumpLaunchNormal's face. */
	float WallJumpControlLockoutRemaining;

	/**
	 * Seconds left on a remembered mid-air jump press that found no wall. Charged by DoJump's refusal
	 * path, consumed by OnMovementUpdated the moment a window opens. See
	 * GetWallJumpInputBufferSeconds() for the frame-ordering bug it exists to close.
	 */
	float WallJumpInputBufferRemaining;

	// --- PATCH 28 §5 surf state (all saved/restored by FSavedMove_Trace) --------------------------

	/**
	 * The surf plane currently being ridden, unit length. Zero when not surfing.
	 *
	 * Written only from HandleImpact(), which PhysFalling calls for every blocking hit that was not a
	 * landing spot — on the client, on the server and on every replayed move, from inside the sweep
	 * the engine was already doing. Same sensor the wall jump uses, and for the same reason: a
	 * hand-rolled probe would be a second, differently-timed source of truth for the same fact.
	 */
	FVector SurfPlaneNormal;

	/**
	 * Seconds of surf left without a fresh contact. Refreshed on every surf contact, ticked down in
	 * OnMovementUpdated, and the thing IsSurfing() actually answers from.
	 *
	 * SIMULATION-CRITICAL: IsSurfing() gates the speed ceiling, so a replay that lost this would run
	 * an uncapped frame where the server ran a capped one.
	 */
	float SurfContactRemaining;

	/**
	 * Planar speed the pawn entered this surf with. THE FLOOR UNDER THE CEILING, so it is as
	 * simulation-critical as the clock above: lose it on a replay and a fast entry is clamped down to
	 * the shared ceiling on one machine only.
	 */
	float SurfEntrySpeed;

	/** Seconds this surf has been running. Carried so a move's snapshot is complete. */
	float SurfElapsedSeconds;

	/** Highest speed seen during this surf, uu/s. Carried for the same reason. */
	float SurfPeakSpeed;

	// --- DEMO 29 ITEM 4: the exit (also saved/restored by FSavedMove_Trace) -----------------------

	/**
	 * Seconds of "this pawn is still carrying a ride" left.
	 *
	 * Armed when a surf closes and re-armed by ProcessLanded() on the touchdown that spends the
	 * rollout, so a long flight off the end of a rail cannot eat the floor carry it earned. Two
	 * consumers, and they are on opposite sides of the landing:
	 *   * AIRBORNE, it is how long the exit rollout stays available;
	 *   * GROUNDED, it is how long ApplyGroundOverspeedBleed() is scaled by
	 *     GetSurfExitCarryBleedScale() — 0 by default, i.e. the speed is HELD.
	 *
	 * SIMULATION-CRITICAL, and unlike every other clock in this class it matters while the pawn is on
	 * the ground: a replay that lost it bleeds a frame the server did not.
	 */
	float SurfExitCarryRemaining;

	/**
	 * 3D speed on the last frame this pawn was actually ON a surf face, uu/s. Zero when spent.
	 *
	 * THE CAP ON THE EXIT ROLLOUT, and the reason the rollout cannot manufacture speed: a free fall
	 * after the ride adds nothing, because the target is never allowed above what the ride itself was
	 * worth. 3D rather than planar on purpose — on a 47-61 degree face most of a descent is vertical,
	 * and the vertical part is exactly what the landing throws away and what the rollout puts back.
	 *
	 * Kept current every frame of a ride rather than latched at the close, because by the time the
	 * close is detected MaintainHorizontalGroundVelocity() has already deleted the Z it describes.
	 */
	float SurfExitSpeed;

	// --- SPEC v10 §1 state ------------------------------------------------------------------------

	/**
	 * "The knife is the active weapon", as far as MOVEMENT is concerned. Written only by
	 * SetKnifeMovementProfileActive(); read by GetMaxSpeed() and the three air-strafe ceiling
	 * accessors.
	 *
	 * Saved-move state so a replayed move runs under the profile it originally ran under, and refused
	 * by CanCombineWith across a change so two moves either side of a swap are never merged into one
	 * move simulated entirely at the wrong speed.
	 */
	uint8 bKnifeMovementProfile : 1;

	/** See GetLastDashActiveWorldTime(). Server observation only; never saved or replicated. */
	float LastDashActiveWorldTime = -1000.f;

#if !UE_BUILD_SHIPPING
	// --- SPEC v18 §1b: THE AIR DRIFT LEDGER -------------------------------------------------------
	//
	// "When mid jump with some forward momentum already, it feels like new movement vectors happen
	// without any player inputs or strafes, which is awkward."
	//
	// The spec's instruction is "reproduce it before theorising", and the only honest reproduction of
	// "velocity changed and I did not ask for it" is a PER-MOVE LEDGER: for every simulated move on
	// which the pawn is airborne AND its input acceleration is zero, record how much the planar
	// velocity moved and which of this component's mid-air velocity writers was live at the time.
	//
	// Ticked from OnMovementUpdated (the record pass only — a replay re-runs the same frames and would
	// count each one several times), armed by Trace.Move.V18.AirDrift, defined in
	// Movement/TraceMovementV18.cpp next to the command that reads it. Observation only: nothing here
	// is read by the simulation, so it cannot desync anything and it is not saved-move state.
	//
	// @param OldVelocity the velocity at the START of this move, which is exactly what
	//                    OnMovementUpdated is handed — so the delta covers the whole move including
	//                    every physics sub-step, which is the window the player perceives.
	void TickAirDriftMeter(float DeltaSeconds, const FVector& OldVelocity);

	/** World seconds the ledger stops recording at. Negative when disarmed, which is the default. */
	float AirDriftUntilTime = -1.f;

	/** Airborne, zero-input moves seen; and of those, how many moved the planar velocity at all. */
	int32 AirDriftSamples = 0;
	int32 AirDriftMovedSamples = 0;

	/** Worst single-move planar change over the session, and the total, both in uu/s. */
	float AirDriftWorstStep = 0.f;
	float AirDriftTotalStep = 0.f;

	/** Which of this component's writers was live on the worst move. See the .cpp for the bit list. */
	int32 AirDriftWorstFlags = 0;
	float AirDriftWorstBefore = 0.f;
	float AirDriftWorstAfter = 0.f;

	/** Per-writer tallies, so the report can name a cause instead of printing a number. */
	int32 AirDriftWallLaunches = 0;
	int32 AirDriftDashExits = 0;
	int32 AirDriftSlideExits = 0;

	/**
	 * Moves that changed the planar velocity with NOTHING live to explain it — no wall contact, no
	 * dash exit, no slide exit, no ledge grace.
	 *
	 * THIS IS THE ONLY NUMBER THAT WOULD CONVICT THE AIR MODEL ITSELF, which is why it is counted
	 * separately rather than derived by subtraction in the report. "Moved samples minus the ones I can
	 * name" silently counts a wall contact as unexplained the moment a new writer is added and nobody
	 * updates the subtraction.
	 */
	int32 AirDriftUnexplained = 0;

	/** Snapshot taken when the ledger was armed, so the correction count is a delta. */
	int32 AirDriftCorrectionsAtArm = 0;

	/**
	 * SPEC v18 §1b. Wall jumps launched by the v10 §5 CAUSE 2 press buffer rather than by a press on
	 * that frame — i.e. the only way this component can hand a player several hundred uu/s on a frame
	 * they pressed nothing.
	 *
	 * Lifetime count, incremented in OnMovementUpdated; the drift report prints a delta against the
	 * value at arming. Observation only, never read by the simulation, never saved-move state.
	 */
	int32 WallJumpBufferedLaunches = 0;
	int32 AirDriftBufferedLaunchesAtArm = 0;

	/**
	 * Previous-move values, so an EVENT ("a wall jump fired", "the dash window closed") can be derived
	 * as a transition. TickAirDriftMeter runs after OnMovementUpdated has already advanced every clock,
	 * so the state it sees is post-event and only a transition can name what happened.
	 */
	int32 AirDriftPrevWallJumps = 0;
	float AirDriftPrevDashTime = 0.f;
	float AirDriftPrevSlideTime = 0.f;

	// --- Slide measurement ("-TraceSlideDebug", or `Trace.SlideDebug 1`) -------------------------
	//
	// Deliberately NOT saved-move state and never read by the simulation: these only observe. They
	// are what let a headless match answer "are slides actually longer now" with numbers instead of
	// an opinion. The running mean they feed is process-wide (a file-scope counter in the .cpp), not
	// per-pawn, because the question is about the mechanic and ten bots is the sample.
	float SlideDebugStartTime = 0.f;
	FVector SlideDebugStartLocation = FVector::ZeroVector;
	float SlideDebugEntrySpeed = 0.f;

	// --- SPEC v24 §8: the bonus window, MEASURED (same switch as the slide measurement above) -----
	//
	// v24 §8 asks for the window to open 0.4 s earlier and the slide to be 0.4 s shorter, and asks
	// for the OPEN and CLOSE times to be measured from the game rather than read off the constants.
	// GetSlideJumpWindowSeconds() is anchored to the slide's END and GetSlideTimeLeft() also folds in
	// the decay exit, so the moment the window actually opens is NOT "duration - window" in general —
	// it is a runtime transition, and this is the only honest way to name it.
	//
	// SlideWindowOpenTime is seconds since the slide began at the FIRST frame IsSlideJumpWellTimed()
	// returned true; -1 means the window never opened during that slide. Observation only, never
	// saved-move state, never read by the simulation.
	float SlideWindowOpenTime = -1.f;
	bool bSlideWindowWasOpen = false;
#endif

#if !UE_BUILD_SHIPPING
	/**
	 * "-TraceDashPitchTest": SPEC v7 §5, measured on a REAL pawn rather than on paper.
	 *
	 * Trace.DashVectorTest exercises ComputeDashDirection() and stops there, which leaves three
	 * runtime claims unchecked, all of them new: that a grounded upward dash actually reaches
	 * MOVE_Falling instead of being flattened by PhysWalking, that the per-frame re-assert holds Z
	 * on rails for the whole window, and that the exit clamp bounds the climb. This harness aims the
	 * local player pawn at a scripted pitch, holds a scripted W/A/S/D combination, fires a real dash
	 * through StartDash(), and prints the height and distance it actually travelled.
	 *
	 * Same rules as the other two harnesses: locally player-controlled only, off unless the switch is
	 * on the command line, Display-level logging, compiled out of shipping.
	 */
	void TickDashPitchTest(float DeltaSeconds);

	float DashPitchTestTime = -1.f;
	int32 DashPitchTestPhase = 0;

	/**
	 * Which pass over the phase list this is. SEVEN DASHES IS NOT A SAMPLE: corrections arrive in
	 * bursts of four or five from a single bad dash, so one seven-dash session put the SAME build and
	 * the SAME cvar at 0.43/dash once and 2.29/dash an hour later. The list is walked several times so
	 * the A/B compares tens of dashes per arm instead of seven. Trace.DashPitchTestCycles sets it.
	 */
	int32 DashPitchTestCycle = 0;
	float DashPitchTestPhaseTime = 0.f;
	float DashPitchTestYaw = 0.f;
	float DashPitchTestPeakRise = 0.f;
	FVector DashPitchTestStart = FVector::ZeroVector;
	FVector DashPitchTestLaunchVelocity = FVector::ZeroVector;
	uint8 bDashPitchTestFired : 1;
	uint8 bDashPitchTestLogged : 1;
#endif

#if !UE_BUILD_SHIPPING
	/**
	 * "-TraceMoveMeasure": a scripted, headless exercise of the momentum model on the local player
	 * pawn that prints MEASURED numbers for the four things spec §2 is about — speed retained
	 * through an air strafe turn, speed carried through a landing, slide entry vs exit speed, and
	 * the between-slides buffer.
	 *
	 * It exists because none of those can be verified from a screenshot and because crouch has no
	 * key bound in a headless run. It drives the same public entry points the input layer does
	 * (Jump / SetWantsToSlide / StartDash) and logs at Display, never Verbose.
	 *
	 * Standalone + locally controlled + player controlled only, off unless the switch is on the
	 * command line, and compiled out of shipping.
	 */
	void TickMomentumMeasure(float DeltaSeconds);

	float MeasureTime = -1.f;
	int32 MeasurePhase = 0;
	float MeasurePhaseTime = 0.f;
	float MeasureMarkA = 0.f;
	float MeasureMarkB = 0.f;
	FVector MeasureMarkDirection = FVector::ZeroVector;

	/**
	 * The direction the harness runs in, chosen once at start as "toward the middle of the field".
	 *
	 * Not a constant world axis. The first run of this harness sprinted along +X from a spawn pad,
	 * hit the endzone wall two frames after reaching full speed, and reported a jump that "lost" 760
	 * uu/s — which was a collision, not the movement model. A measurement that can be invalidated by
	 * where the pawn happened to spawn is not a measurement.
	 */
	FVector MeasureRunDirection = FVector::ForwardVector;

	/**
	 * Where the harness started, so the slide-jump phases can be run back on ground the earlier
	 * phases already proved is clear.
	 *
	 * Needed because the CHAIN phases teleport to the middle of the arena, and the first slide-jump
	 * measured there reported 70% of the slide's speed on the first airborne frame while the movement
	 * component's own log reported 110% at the instant of launch. The difference was a ~50 degree
	 * ROTATION of the velocity vector, not a loss of magnitude — i.e. SlideAlongSurface deflecting the
	 * pawn off midfield cover, which is a measurement of the arena and not of the mechanic. Exactly
	 * the same trap MeasureRunDirection exists to avoid.
	 */
	FVector MeasureHomeLocation = FVector::ZeroVector;

	/**
	 * "-TraceLedgeTest": spawns a block of a known height in front of the pawn and runs at it, over
	 * and over, measuring what happens at the top edge.
	 *
	 * It builds its own geometry on purpose. The arena's raised sections are real ledges, but their
	 * positions depend on the arena builder's tuning, and a diagnosis of a prediction bug has to be
	 * repeatable against the same lip every time or the numbers measure the level.
	 *
	 * Runs on the LOCALLY CONTROLLED pawn in any net mode — unlike TickMomentumMeasure, which is
	 * standalone-only — because the whole point is to measure what a networked client experiences.
	 *
	 * SPEC v12 §5 REWORKED WHAT IT MEASURES, and that is the substance of the task rather than the
	 * mantle deletion. Before, it asked "did a mantle fire?" against a block TALLER than the jump
	 * apex, because that is the only case a mantle could ever fire in. But the complaint — "jumping
	 * on the edge of a raised section" — is the case where the jump CLEARS the top and the capsule
	 * lands on the lip, which the mantle never touched. So the default block height is now below the
	 * jump apex, and the harness reports three things PER CONTACT rather than one count per session:
	 *
	 *   flips  ground-state changes between the jump and the settle. 2 is clean (leave, arrive).
	 *          3+ is the capsule chattering on the edge, which is the mechanism by which client and
	 *          server end up running different velocity models on the same frame.
	 *   corr   corrections received inside that same window, and their worst position error. This is
	 *          the direct test for "rubber banding" — it is a claim about the network, so it has to
	 *          be answered with corrections and not with feel.
	 *   kept   planar speed after the lip as a fraction of the speed at the jump. Below ~1.0 is a
	 *          stall; a negative displacement across the window is a pull-back.
	 *
	 * The correction numbers are DELTAS of CorrectionCount / CorrectionErrorWorst snapshotted at the
	 * jump, rather than a new attribution clock: OnClientCorrectionReceived already maintains those
	 * totals, and a delta of an existing counter cannot drift out of step with the thing it counts.
	 */
	void TickLedgeTest(float DeltaSeconds);

	float LedgeTestTime = -1.f;
	int32 LedgeTestPhase = 0;
	float LedgeTestPhaseTime = 0.f;
	int32 LedgeTestRun = 0;
	int32 LedgeTestGroundFlips = 0;
	uint8 bLedgeTestWasGrounded : 1;
	FVector LedgeTestRunDirection = FVector::ForwardVector;
	TWeakObjectPtr<AActor> LedgeTestBlock;

	/** A point ON the vertical face the pawn runs at, and the top surface just past it. */
	FVector LedgeTestFacePoint = FVector::ZeroVector;
	FVector LedgeTestTopPoint = FVector::ZeroVector;
	float LedgeTestLedgeHeight = 0.f;

	/** Snapshots taken on the jump frame; the per-contact numbers are all deltas against these. */
	int32 LedgeTestFlipsAtJump = 0;
	int32 LedgeTestCorrAtJump = 0;
	float LedgeTestWorstErrAtJump = 0.f;
	float LedgeTestSpeedAtJump = 0.f;
	FVector LedgeTestPosAtJump = FVector::ZeroVector;

	/** Aggregates over the whole session, so one line can summarise every contact. */
	int32 LedgeTestContacts = 0;
	int32 LedgeTestContactFlips = 0;
	int32 LedgeTestContactCorrections = 0;
	int32 LedgeTestWorstContactFlips = 0;
	float LedgeTestWorstContactErr = 0.f;
	float LedgeTestKeptFractionTotal = 0.f;
	float LedgeTestWorstKeptFraction = 1.f;
	int32 LedgeTestLandedOnTop = 0;
	int32 LedgeTestPulledBack = 0;
#endif

#if !UE_BUILD_SHIPPING
	/** Corrections observed on this pawn since it spawned. Diagnostic only; never feeds movement. */
	int32 CorrectionCount = 0;
	float CorrectionErrorTotal = 0.f;
	float CorrectionErrorWorst = 0.f;

	// --- SPEC v8 §1: CORRECTIONS PER DASH, MEASURED ON A CLIENT ---------------------------------
	//
	// "Dash feels rubber bandy" is a claim about the correction rate DURING a dash, and the previous
	// pass answered it with a whole-session correction count taken on the host — where corrections
	// are impossible by construction, so the number was zero for a reason that had nothing to do with
	// the dash. These four fields attribute each correction to the dash it landed inside (or just
	// after), so the answer is a RATE: corrections per dash, on the machine that can actually have
	// them. Printed by Trace.DashNetReport and at the end of -TraceDashPitchTest.
	int32 DashNetDashCount = 0;
	int32 DashNetCorrectionsInDash = 0;
	float DashNetCorrectionErrorInDash = 0.f;

	/** World time the current dash's attribution window closes. Observation only; never saved. */
	float DashNetAttributionUntil = -1000.f;

	/** Next world time the once-a-second charge-pool readout (spec v8 §5) is due. */
	float DashPoolDebugNextLogTime = 0.f;

	// --- SPEC v8 §7: THE WALL JUMP, MEASURED ----------------------------------------------------
	//
	// Same argument as the dash block above. "Preserves and redirects momentum" and "is predicted"
	// are two measurements, not an opinion, and the second one is only readable on a client.
	int32 WallJumpCount = 0;
	int32 WallJumpCapRefusals = 0;
	int32 WallJumpMaxConsecutiveSeen = 0;
	float WallJumpEntrySpeedSum = 0.f;
	float WallJumpLaunchSpeedSum = 0.f;
	float WallJumpTurnDegreesSum = 0.f;
	float WallJumpLaunchZSum = 0.f;
	int32 WallJumpCorrectionsInWindow = 0;

	// --- PATCH 28 §5: SURF, MEASURED --------------------------------------------------------------
	//
	// Same argument as the wall-jump block above. "Players accelerate on curved ramps", "it is
	// bounded" and "it is predicted" are three measurements, and the third is only readable on a
	// machine that can be corrected.
	int32 SurfCount = 0;
	int32 SurfClosedCount = 0;
	float SurfEntrySpeedSum = 0.f;
	float SurfExitSpeedSum = 0.f;
	float SurfBestGain = 0.f;
	float SurfWorstGain = 0.f;
	float SurfLongestSeconds = 0.f;
	int32 SurfCeilingBinds = 0;
	int32 SurfContactsRefused = 0;
	int32 SurfCorrectionsInWindow = 0;

	/**
	 * DEMO 29 ITEM 4, COUNTED. How many rides ended in a rollout that actually added planar speed, how
	 * much it added in total, and how many rides were STARTED from the ground by running at a rail.
	 * All three are the new claims, so all three are numbers in Trace.Move.SurfReport rather than
	 * assertions in a comment.
	 */
	int32 SurfRolloutCount = 0;
	float SurfRolloutGainSum = 0.f;
	int32 SurfGroundEntries = 0;

	/** World time the current surf's correction-attribution window closes. Never saved. */
	float SurfAttributionUntil = -1000.f;

	/** -TraceSurfTest state: which rung of the entry-speed ladder, and the phase clock. */
	int32 SurfTestRun = -1;
	float SurfTestPhaseTime = 0.f;
	float SurfTestEntrySpeed = 0.f;
	float SurfTestPeak = 0.f;
	uint8 bSurfTestArmed = 0;
	uint8 bSurfTestReported = 0;

	// SPEC v9 §5's WallJumpMantleSteals counter lived here: it counted mantles that started while a
	// wall-jump window was open, i.e. wall jumps the automatic mantle took away. With the mantle
	// deleted in v12 §5 the quantity is identically zero by construction and the counter is gone.
	// The wall jump no longer has to win a race against anything — it is the only thing that reads a
	// wall contact now, and TryWallJump()'s only gate is its own window.

	/** World time the current wall jump's correction-attribution window closes. Never saved. */
	float WallJumpAttributionUntil = -1000.f;

	/** -TraceWallJumpTest state. Phase clock, and the yaw that points at the chosen wall. */
	float WallJumpTestTime = -1.f;
	float WallJumpTestYaw = 0.f;
	uint8 bWallJumpTestReported = 0;

	/**
	 * SPEC v10 §5 — the harness clock for BACKING OFF AND TAKING ANOTHER RUN AT THE WALL.
	 *
	 * THE v8 HARNESS SEIZED UP AND NOBODY NOTICED. It held the movement key into the wall forever, so
	 * the moment the pawn landed with its nose against the face its planar speed was ~0 — and the
	 * "grounded, get airborne" branch requires >300 uu/s before it will press jump. The pawn stood
	 * there for the rest of the run. Measured: THREE wall jumps in 22 seconds, and then nothing.
	 * Every wall-jump number this project has ever reported was a mean over a handful of samples
	 * taken in the first few seconds of the run, which is worth knowing about the v8/v9 reports too.
	 *
	 * While this clock is ahead of WallJumpTestTime the harness pushes AWAY from the wall (aim stays
	 * on the wall, only the movement input reverses — a player strafing back for another go) and
	 * refuses the ordinary get-airborne jump, so the pawn always arrives at the face with a run-up.
	 */
	float WallJumpTestRunUpUntil = -1.f;

	/**
	 * SPEC v10 §5 — HOW THE HARNESS PRESSES JUMP, AND WHY THE OLD RULE COULD NOT SEE THE BUG.
	 *
	 * The v8 harness pressed jump ONLY when IsWallJumpAvailable() already said yes. That is a test
	 * that the mechanic fires when it is asked at the perfect moment — it is structurally incapable of
	 * reproducing "I pressed jump right as I hit the wall and nothing happened", because it never
	 * presses at any moment except the perfect one. Measured on this build with the old rule: RED and
	 * GREEN were indistinguishable at ~48 ms, which is what a harness says when it is not testing the
	 * complaint.
	 *
	 * So approaches now ALTERNATE:
	 *   EVEN  — press EARLY, aimed WallJumpTestPressLeadSeconds ahead of contact by a line trace along
	 *           the approach. This is the human press, and in the RED arm it is eaten by frame
	 *           ordering (CheckJumpInput runs before the physics step that touches the wall).
	 *   ODD   — press on IsWallJumpAvailable(), i.e. the old rule, kept so the launch-and-escape half
	 *           of the measurement still has samples in both arms.
	 *
	 * One press per approach (the latch), because a human presses once and then wonders why nothing
	 * happened — a harness that mashes would paper over exactly the bug being measured.
	 *
	 * ...EXCEPT AFTER A SUCCESSFUL LAUNCH, AND THAT EXCEPTION IS THE WHOLE MEASUREMENT.
	 * An "approach" runs from leaving the ground to landing, so with a bare latch the pawn pressed
	 * once, wall-jumped, and then coasted the rest of the flight without ever pressing again. Every
	 * sample was therefore the FIRST jump of a chain, off a full-speed ground run — which the CAUSE 1
	 * derivation at the top of this header identifies as the ONE CASE THAT WAS NEVER BROKEN. The first
	 * paired run proved it from the other side: maxConsecutive=1/2 in BOTH arms, and peakOut ~55 uu
	 * against the ~18 uu the air-initiated case predicts. The harness was measuring the healthy case
	 * and reporting it as the mechanic.
	 *
	 * So WallJumpTestLastChainCount watches WallJumpsSinceGround and re-arms the latch the moment it
	 * INCREASES — i.e. only on a launch that actually happened. A refused press still consumes the
	 * approach, so the eaten-press columns keep measuring exactly what they measured before, while the
	 * pawn now chains up to WallJumpMaxConsecutive and the second jump — the one taken from the air at
	 * AirMaxWishSpeed, with nothing left to reflect — finally appears in the sample.
	 */
	int32 WallJumpTestApproach = 0;
	uint8 bWallJumpTestPressLatched = 0;
	uint8 bWallJumpTestWasAirborne = 0;

	/** Last WallJumpsSinceGround the harness saw, so a launch (and only a launch) re-arms the latch. */
	int32 WallJumpTestLastChainCount = 0;

	/** Presses the harness actually made, split by rule, so "the early press never fired" is visible. */
	int32 WallStickEarlyPresses[2] = { 0, 0 };
	int32 WallStickOnTimePresses[2] = { 0, 0 };

	// =============================================================================================
	// SPEC v10 §5 — THE STICK METER. THE NUMBER THE COMPLAINT IS ABOUT.
	//
	// "Wall jumping still feels like the player is sticking to the wall for a moment too long" is a
	// claim about TIME, and v9 answered it by shortening a config value and reporting that the config
	// value was shorter. That is not a measurement of the symptom. This is:
	//
	//   the milliseconds between the frame the capsule first TOUCHES a wall and the frame the pawn
	//   has actually moved WallStickClearUU away from that face, measured along the face's normal.
	//
	// Everything the player calls "sticky" is inside that interval — the wait for the window, the
	// press that got eaten, the launch, and the launch being clawed straight back by held input. It
	// makes no assumption about WHICH of those dominates, which is the point: it is the number the
	// fix has to move, whatever the fix turns out to be.
	//
	// The first contact of a bout anchors the sample; re-contacts while the sample is open do not
	// re-anchor it, because a player scraping down a face is having ONE sticky experience, not thirty.
	// A sample is closed by clearing the face, by landing, or by WallStickTimeoutSeconds.
	//
	// Dev-only, never saved, never read by the simulation.
	// =============================================================================================

	/** Open sample: world time of first contact, the capsule position then, and the face's normal. */
	float WallStickContactTime = -1.f;
	FVector WallStickAnchor = FVector::ZeroVector;
	FVector WallStickNormal = FVector::ZeroVector;

	/** World time the launch actually fired inside the open sample, or -1 if it has not (yet). */
	float WallStickLaunchTime = -1.f;

	/** Furthest the pawn has been from the face, along its normal, during the open sample. */
	float WallStickPeakOutUU = 0.f;

	/** Which harness phase the open sample belongs to: 0 = head-on approach, 1 = glancing. */
	int32 WallStickPhase = 0;

	/**
	 * Per-phase aggregates. Index 0 = head-on, 1 = glancing. The glancing column is the one that
	 * matters: a head-on approach reflects a big outward velocity and escapes on its own, while a
	 * glancing one has almost nothing to reflect and the flat outward impulse IS the whole launch —
	 * so it is the case the held input can actually beat.
	 */
	int32 WallStickSamples[2] = { 0, 0 };
	float WallStickClearMsSum[2] = { 0.f, 0.f };
	float WallStickClearMsWorst[2] = { 0.f, 0.f };
	float WallStickPressMsSum[2] = { 0.f, 0.f };
	float WallStickPeakOutSum[2] = { 0.f, 0.f };
	int32 WallStickNeverCleared[2] = { 0, 0 };

	/**
	 * Contacts that never produced a launch at all, counted but kept OUT of the aggregates above.
	 *
	 * The complaint is "WALL JUMPING feels sticky", so the mean has to be over contacts that were
	 * actually wall-jumped. Brushing a wall while running past it, or touching one after the
	 * anti-ladder cap has spent both jumps, is a contact the player never asked anything of — folding
	 * those into the mean would bury the signal under the arena's geometry. They are reported on their
	 * own line instead, because a fix that made them MORE common would be worth knowing about.
	 */
	int32 WallStickNoLaunch[2] = { 0, 0 };

	/**
	 * THE CHAINED CUT, AND IT IS THE ONE THE COMPLAINT LIVES IN — see CAUSE 1 at the top of this
	 * header. A sample is CHAINED if the pawn had already wall-jumped since it last touched the
	 * ground, i.e. it arrived at this wall under air control alone. AirMaxWishSpeed caps that arrival
	 * at 160 uu/s, so the reflection contributes ~123 uu/s and the flat WallJumpOutwardImpulse is
	 * essentially the entire launch — the 18 uu peak against a 34 uu capsule that the derivation
	 * predicts. The FIRST jump of a chain arrives off a full-speed ground run and escapes on its own.
	 *
	 * Kept as one bucket across both phases rather than another [2]: the split that matters here is
	 * first-vs-chained, and per-phase chained samples would be too thin to mean anything in a run this
	 * length. The per-phase arrays above still count these samples too — this is a cross-cut of the
	 * same population, not a separate one, so the two sets of numbers are not additive.
	 */
	uint8 bWallStickSampleChained = 0;
	int32 WallStickChainedSamples = 0;
	int32 WallStickChainedNoLaunch = 0;
	float WallStickChainedClearMsSum = 0.f;
	float WallStickChainedClearMsWorst = 0.f;
	float WallStickChainedPeakOutSum = 0.f;
	int32 WallStickChainedNeverCleared = 0;

	/** Opens / advances / closes the sample. Called from HandleImpact and OnMovementUpdated. */
	void BeginWallStickSample(const FVector& PlanarNormal);
	void TickWallStickSample();
	void CloseWallStickSample(bool bCleared);

	// --- SPEC v8 §5: THE CARRIER'S TWO CHARGES, EXERCISED (-TraceCarrierChargeTest) ---------------
	//
	// Phase 0 = the control (not carrying), phase 1 = carrying. Launches are counted from the
	// DashTimeRemaining edge rather than from the press, so a press that was silently refused for want
	// of a charge is visible as a missing launch.
	float CarrierTestTime = -1.f;
	float CarrierTestPhaseTime = 0.f;
	int32 CarrierTestPhase = 0;
	int32 CarrierTestPresses = 0;
	int32 CarrierTestLaunches = 0;
	uint8 bCarrierTestWasDashing = 0;
	uint8 bCarrierTestCoreGiven = 0;
	int32 CarrierTestChargesAtStart = 0;
	int32 CarrierTestMaxAtStart = 0;

	// --- SPEC v9 §2: THE SINGLE-DASH REPRODUCTION (-TraceSingleDashTest) --------------------------
	//
	// Phase -1 = waiting for the Core, 0 = one press, 1 = the 1->2 refill, 2 = drain to zero,
	// 3 = the 0->1 refill. Nothing here is saved-move state: it is pure observation and never
	// influences a simulated move except through StartDash(), which is the same call a key press
	// makes.
	int32 SingleDashPhase = -1;
	float SingleDashPhaseTime = 0.f;
	int32 SingleDashPresses = 0;
	uint8 bSingleDashCoreGiven = 0;
	uint8 bSingleDashReported = 0;
	int32 SingleDashFailures = 0;
	float SingleDashNextSampleTime = 0.f;

	/** Previous sample, so a single-frame GAIN can be measured rather than inferred from a total. */
	int32 SingleDashPrevCharges = 0;
	int32 SingleDashPrevHudPips = 0;

	/** Running worsts. Divergence is |pips drawn - charges held|. */
	int32 SingleDashMaxHudDivergence = 0;
	int32 SingleDashMaxTrueGain = 0;
	int32 SingleDashMaxHudGain = 0;

	/**
	 * HOW LONG the meter and the pool disagreed, summed over the run — the check that "the HUD shows
	 * two" is really about.
	 *
	 * The first version of this check asked whether the two numbers were EVER unequal on any sampled
	 * frame, and it failed the fixed build for a reason that is not a bug: a continuous progress meter
	 * necessarily reaches 100% on the frame BEFORE the integer it is counting up to lands, so a 60 Hz
	 * sample catches the regenerating pip at fraction 0.999 with the pool still one short, for one
	 * frame. Tightening the pip threshold would only move that frame; there is no threshold at which a
	 * filling bar and a stepping integer agree on every sample.
	 *
	 * Duration is the honest discriminator, and it is a STRONGER test of the user's complaint rather
	 * than a weaker one. Their symptom is a meter that reads two while the pawn holds one for the
	 * WHOLE 3.68 s recharge window — on the legacy arm this accumulates seconds. A sub-frame lead
	 * accumulates ~16 ms and no player can see it. The budget below is one 60 Hz frame with margin.
	 */
	float SingleDashDivergentSeconds = 0.f;

	/** Pips drawn on the exact frame the pool climbed 0 -> 1. "They both do" would read 2 here. */
	int32 SingleDashHudPipsAtFirstRefill = -1;
#endif
};

/**
 * One simulated movement frame, extended with the whole movement kit's state.
 *
 * Contract for the seven overrides (all of them call Super first):
 *   Clear()              wipe every added field — moves are pooled and reused
 *   SetMoveFor()         capture CMC state *before* the move is simulated
 *   PostUpdate()         capture the AIM the move was actually sent with (record pass only)
 *   PrepMoveFor()        push that captured state back into the CMC before a replay
 *   GetCompressedFlags() pack the intents for the wire
 *   CanCombineWith()     refuse to merge moves whose ability or momentum state differs
 *   CombineWith()        re-base the added state onto the OLDER move's start, when two moves merge
 */
class TRACE_API FSavedMove_Trace : public FSavedMove_Character
{
public:
	typedef FSavedMove_Character Super;

	FSavedMove_Trace();
	virtual ~FSavedMove_Trace() = default;

	virtual void Clear() override;
	virtual uint8 GetCompressedFlags() const override;
	virtual bool CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const override;
	virtual void SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, class FNetworkPredictionData_Client_Character& ClientData) override;
	virtual void PrepMoveFor(class ACharacter* C) override;

	/**
	 * SPEC v8 §1 — THE FIX FOR THE VECTORIZED DASH'S RUBBER-BAND, AND THE REASON THIS OVERRIDE EXISTS.
	 *
	 * The base class's SavedControlRotation looks like the right place to read the dash's aim from on
	 * a replay, and spec v7 used it. IT IS NOT SAFE, because FSavedMove_Character::PostUpdate WRITES
	 * it — and ClientUpdatePositionAfterServerUpdate calls PostUpdate(PostUpdate_Replay) on every move
	 * it has just replayed. So the first correction after a dash OVERWRITES that move's stored aim
	 * with wherever the mouse is pointing at correction time; a SECOND correction covering the same
	 * unacknowledged move then replays the dash from that stomped rotation while the server keeps
	 * composing from the rotation the move was actually sent with. On a horizontal dash the damage was
	 * a yaw error; on the spec v7 vectorized dash it is a Z-velocity disagreement, i.e. the exact
	 * rubber-band the user reported and the previous pass could not see from the host.
	 *
	 * Recording the aim into a field of OUR OWN, on the RECORD pass only, makes it immutable for the
	 * life of the move: it is the rotation FCharacterNetworkMoveData packed into this move's
	 * ServerMove, it is what the server applied to the controller before MoveAutonomous, and no number
	 * of replays can move it.
	 *
	 * THAT ALONE MADE THE RUBBER-BAND FIVE TIMES WORSE, AND THIS IS THE PART TO READ.
	 *
	 * Measured on a joined client at PktLag 40 both ways: 0.43 corrections per dash with the v7 source,
	 * 2.29 with the replay reading the recorded aim instead. SavedControlRotation has a SECOND consumer
	 * that a replay-side fix never reaches — FCharacterNetworkMoveData::ClientFillNetworkMoveData does
	 * `ControlRotation = ClientMove.SavedControlRotation`, and a correction RE-SENDS every unacknowledged
	 * move. So the aim the server re-simulates from is whatever the replay pass last stomped into that
	 * field. Fixing only the client's replay makes client and server compose the dash from two different
	 * rotations on every corrected dash; v7 was "wrong but agreed".
	 *
	 * So this override does both halves: it records the aim on the record pass, and on the REPLAY pass it
	 * puts SavedControlRotation back to it — undoing the engine's stomp before the resend reads it. The
	 * aim a move was made with is a fact about that move, and the resend then carries what the original
	 * ServerMove for that timestamp already carried.
	 */
	virtual void PostUpdate(ACharacter* C, EPostUpdateMode PostUpdateMode) override;

	/**
	 * When two moves merge, the combined move STARTS where the older one started — so its start-of-move
	 * ability state has to be the older move's too.
	 *
	 * Super re-bases position, rotation, velocity and acceleration and stops there, which left every
	 * added clock in this class one frame in the future relative to the move's own start. A replay of
	 * a merged move therefore restored a dash recharge (or a ledge grace, or a wall window) that was
	 * DeltaTime too short. CanCombineWith already refuses the loud cases — any live ability, and any
	 * airborne or overspeed frame — so this was never a large error, but it was a divergence between
	 * what the client simulated and what a replay of the same move would produce, which is precisely
	 * the class of bug this pass exists to remove.
	 *
	 * The aim rotation is deliberately NOT re-based: the combined move is SENT with the newer move's
	 * control rotation, so the replay has to use the newer one to match the server.
	 */
	virtual void CombineWith(const FSavedMove_Character* OldMove, ACharacter* InCharacter,
		APlayerController* PC, const FVector& OldStartLocation) override;

	/**
	 * Intents. FLAG_Custom_0 = dash, FLAG_Custom_1 = jump held, FLAG_Custom_2 = crouch/slide held.
	 *
	 * FLAG_Custom_1 was boost's, was freed by spec v3 §1, and is spent again by Demo 19 item 4 on the
	 * jump-held level — see UTraceCharacterMovementComponent::SetJumpHeld() for why the release had to
	 * reach the server through a saved move rather than through the ability layer.
	 */
	uint8 bSavedWantsToDash : 1;
	uint8 bSavedWantsToJumpHold : 1;
	uint8 bSavedWantsToSlide : 1;

	/**
	 * "This move was simulated under the momentum model" — airborne, or carrying excess ground
	 * speed. Not CMC state and therefore not restored by PrepMoveFor; it exists only so
	 * CanCombineWith can refuse to merge such moves. Both branches of the model clamp per sub-step
	 * (min(AirAcceleration·dt, AddSpeed); max(GroundLimit, Speed − Bleed·dt)), so one combined move
	 * of length 2dt is NOT equal to two moves of length dt, and merging would hand the server a
	 * simulation the client never ran.
	 */
	uint8 bSavedMomentumActive : 1;

	/** Every clock, charge counter and locked direction as it stood *before* this move was simulated. */
	float SavedDashTimeRemaining;
	float SavedDashRechargeRemaining;
	int32 SavedDashCharges;
	int32 SavedLastMaxDashCharges;
	FVector SavedDashDirection;

	/**
	 * SPEC v8 §1. The aim rotation this move was SENT to the server with, recorded once on the
	 * PostUpdate_Record pass and never touched again. See PostUpdate() for why the base class's
	 * SavedControlRotation could not be used and what it cost.
	 *
	 * Costs no bandwidth: it is never serialised. It is a client-side memo of a value the server
	 * already received inside the ordinary ServerMove.
	 */
	FRotator SavedDashAimRotation;

	/**
	 * True once PostUpdate_Record has written SavedDashAimRotation for THIS move. A move that has not
	 * been recorded yet (or was recorded with no controller) must fall back to the base class's
	 * SavedControlRotation rather than replaying the dash from a zero rotation, which would aim every
	 * replayed dash due north and level.
	 */
	uint8 bSavedDashAimRotationValid : 1;

	float SavedSlideTimeRemaining;
	float SavedSlideCooldownRemaining;
	float SavedSlideSpeed;
	float SavedSlideBufferRemaining;
	FVector SavedSlideDirection;
	uint8 bSavedSlideHeldLastMove : 1;
	uint8 bSavedWasAirborneLastMove : 1;

	/** The slide-jump's coyote window and its "this hop is worth the bonus" bit. */
	float SavedSlideJumpGraceRemaining;
	uint8 bSavedSlideJumpGraceWellTimed : 1;

	/**
	 * SPEC v26 §3. The chain counter and the ceiling it measured.
	 *
	 * Same argument as the coyote window directly above: these two decide whether DoJump CLAMPS the
	 * launch speed, so a correction that landed mid-chain and lost them would replay a clamped hop as
	 * an unclamped one. The disagreement would be several hundred uu/s on the most visible frame in
	 * the kit — which is precisely the class of rubber-band the rest of this block exists to prevent.
	 */
	int32 SavedSlideJumpChainBoosts;
	float SavedSlideJumpChainCeiling;

	/**
	 * The ledge grace (spec v5 §7). The six Mantle* companions that used to sit here went with the
	 * mantle in v12 §5.
	 *
	 * Restored by PrepMoveFor, and it must stay that way. It gates EndSlide(), the fast-fall and the
	 * landing transition, so a replayed move that lost it would resolve a ledge blip differently from
	 * the original and put the client and the server on different velocity models — which is the
	 * rubber-band itself, not a symptom of it.
	 */
	float SavedGroundGraceRemaining;

	/**
	 * The wall jump (spec v8 §7). Same argument as the mantle's block above: a correction that landed
	 * inside the contact window and lost these would replay the wall jump as a refused mid-air jump,
	 * and client and server would disagree about the entire redirected launch.
	 */
	FVector SavedWallJumpNormal;
	float SavedWallJumpWindowRemaining;
	int32 SavedWallJumpsSinceGround;

	/** The approach velocity the open window was charged with. See WallJumpEntryVelocity. */
	FVector SavedWallJumpEntryVelocity;

	/**
	 * SPEC v10 §5. The post-launch into-wall input lockout and the buffered press.
	 *
	 * BOTH ARE LOAD-BEARING FOR PREDICTION, and for the same reason the rest of this block is. The
	 * lockout changes the VELOCITY a replayed air-strafe frame produces (it removes the into-wall
	 * component of the wish direction), so a replay that lost it would accelerate the pawn straight
	 * back at a wall the server never pushed it toward. The buffer decides whether a wall jump happens
	 * AT ALL on the contact frame, which is the largest single-frame disagreement the kit can have.
	 */
	FVector SavedWallJumpLaunchNormal;
	float SavedWallJumpControlLockoutRemaining;
	float SavedWallJumpInputBufferRemaining;

	/**
	 * PATCH 28 §5. THE SURF, AND TWO OF THESE FIVE CHANGE WHAT A REPLAYED FRAME COMPUTES.
	 *
	 * SavedSurfContactRemaining is what IsSurfing() answers from and IsSurfing() gates the speed
	 * ceiling, so a correction that landed mid-ride and lost it would replay an UNCAPPED frame the
	 * server capped. SavedSurfEntrySpeed is the ceiling's own floor — max(entry, ceiling) — so losing
	 * it clamps a fast entry down to the shared ceiling on the client only. Either one is worth
	 * several hundred uu/s on the frames a player is watching most closely, which is the exact class
	 * of rubber-band the rest of this block exists to prevent.
	 *
	 * The normal is tested by IsSurfing() (zero means no surf, exactly as SavedWallJumpNormal works).
	 * The elapsed clock and the peak drive no simulation and are carried anyway: a snapshot that is
	 * nearly complete is a trap for whoever next makes one of them load-bearing.
	 */
	FVector SavedSurfPlaneNormal;
	float SavedSurfContactRemaining;
	float SavedSurfEntrySpeed;
	float SavedSurfElapsedSeconds;
	float SavedSurfPeakSpeed;

	/**
	 * DEMO 29 ITEM 4. THE EXIT, AND BOTH OF THESE CHANGE WHAT A REPLAYED FRAME COMPUTES.
	 *
	 * SavedSurfExitCarryRemaining is the window in which ApplyGroundOverspeedBleed() is held off, so
	 * a replay that lost it BLEEDS A GROUNDED FRAME THE SERVER HELD — the first entry in this block
	 * that can disagree while the pawn is on its feet, and therefore the most visible.
	 * SavedSurfExitSpeed is the cap on the exit rollout in ProcessLanded(); it is set from a frame
	 * that is already in the past by the time the landing happens, so nothing in a replay can
	 * re-derive it, and a replay that lost it lands flattened where the server landed rolled out.
	 *
	 * Written in the same shape as the five above them because they are the same fact one step later,
	 * and because a snapshot that is nearly complete is a trap for whoever next makes one of these
	 * load-bearing.
	 */
	float SavedSurfExitCarryRemaining;
	float SavedSurfExitSpeed;

	/**
	 * SPEC v10 §1. The knife movement profile as it stood before this move ran.
	 *
	 * Restored by PrepMoveFor so a replayed move uses the ceilings and the ground speed that move
	 * originally ran under, and tested by CanCombineWith so two moves either side of a weapon swap are
	 * never merged into one move simulated entirely at the wrong speed.
	 */
	uint8 bSavedKnifeMovementProfile : 1;
};

/** Client prediction data whose only job is to hand out FSavedMove_Trace instances. */
class TRACE_API FNetworkPredictionData_Client_Trace : public FNetworkPredictionData_Client_Character
{
public:
	typedef FNetworkPredictionData_Client_Character Super;

	explicit FNetworkPredictionData_Client_Trace(const UCharacterMovementComponent& ClientMovement);

	virtual FSavedMovePtr AllocateNewMove() override;
};
