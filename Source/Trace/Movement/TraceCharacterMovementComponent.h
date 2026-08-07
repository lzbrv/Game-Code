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
// DashDuration (3000 × 0.18 = 540 uu) with gravity suspended, and would then exit still carrying
// 3000 uu/s upward — another 4592 uu of coast, well past the arena's 1640 uu ceiling. The air-strafe
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
//        ADD velocity along the wish direction and can never subtract any, so input perpendicular
//        to travel ROTATES the velocity vector at (slightly more than) constant magnitude instead
//        of braking it. There is no lerp toward the input direction anywhere in this file, because
//        a lerp is exactly the thing that makes strafing cost speed.
//        There is also NO air friction: releasing the stick in mid-air coasts at full speed.
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
//   2. Gain = |new| − |old|, which the projection formula guarantees is >= 0.
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
// --- SPEC v5 §7: THE LEDGE RUBBER-BAND, AND THE MANTLE -----------------------------------------
//
// "When jumping on the edge of a raised section, it's glitchy and feels like rubber banding. Add a
// mantle, to solve this."
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
// So there are three fixes, and only the third is the one that was asked for:
//
//   1. PerchRadiusThreshold, set in the constructor. Gives the perch test a real band to decide in
//      instead of a knife edge, so the walking/falling answer at a lip is stable and both ends reach
//      it from the same geometry.
//   2. LEDGE GRACE (GroundGraceRemaining), saved-move state. The ability layer treats "on the ground
//      within the last LedgeGroundGraceSeconds" as grounded, so a one-frame contact blip can no
//      longer end a slide, fire a fast-fall or fake a landing. It deliberately does NOT touch the
//      engine's own physics mode — only which of this file's branches run — so it cannot change
//      where the pawn is, only stop the kit from disagreeing about it.
//   3. THE MANTLE. See below.
//
// --- THE MANTLE ---------------------------------------------------------------------------------
//
// Fully client-predicted, and it needs no new input and no new compressed flag: it triggers itself
// from state the replay path already restores (Velocity, Acceleration, the updated component's
// transform) plus the static arena geometry, which is identical on every machine. Detection runs in
// OnMovementUpdated, once per move, on client, server and every replayed move.
//
//   REACH   a forward trace at chest height, along the direction of travel, out to MantleReachUU.
//           Requires a near-vertical face (|Normal.Z| small) and requires the player to be PUSHING
//           INTO it (Acceleration·forward > 0), so falling past a wall never grabs it.
//   HEIGHT  a downward trace from above that face finds the ledge top. It must be between
//           MantleMinHeightUU and MantleMaxHeightUU above the pawn's feet — below the minimum the
//           engine's own step-up already handles it, above the maximum it is a wall and not a ledge.
//   CLEAR   a capsule sweep at the destination proves there is room to stand before anything moves.
//
//   The pull-up is TWO PHASES and never passes through solid geometry: straight up the face for
//   MantleUpPhaseFraction of MantleDurationSeconds, then forward over the lip for the rest, both as
//   ordinary swept movement in MOVE_Flying. Velocity is written in CalcVelocity (inside the physics
//   step, where it moves the pawn on the same frame) as (target − here) / time-left, which is
//   self-correcting: both ends independently recompute the same target from the same geometry and
//   converge on it, so even a small difference in where the mantle started cannot accumulate.
//
// MantleTimeRemaining, MantleTargetLocation, MantleUpTargetZ, MantleEntrySpeed and
// MantleCooldownRemaining are ALL saved-move state, round-tripped through Clear / SetMoveFor /
// PrepMoveFor and blocked from move-merging by CanCombineWith — a correction landing mid-mantle that
// lost them would replay the pull-up as a fall, which is the biggest rubber-band the kit could
// produce and the exact bug this section exists to remove.
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
//         v_out = 1100 × 0.7695 + 420 = 1266 uu/s  ->  peak 100 uu. It escapes. This is the one
//         every previous measurement looked at, and it is the one case that was never broken.
//
//       EVERY WALL JUMP TAKEN FROM THE AIR — i.e. the second and third of a chain, which is how
//       players actually use the mechanic:
//         AirMaxWishSpeed is 160, and it is a hard cap on speed along the wish direction. So a pawn
//         returning to a wall under its own air control arrives at AT MOST 160 uu/s into the face.
//         The reflection therefore contributes at most 160 × 0.7695 = 123 uu/s and
//         WallJumpOutwardImpulse (420, flat) is essentially the whole launch:
//         v_out ≈ 543 uu/s  ->  peak 543² / 16000 = 18 uu. AGAINST A 34 uu CAPSULE RADIUS.
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

	// --- Mantle API (spec v5 §7) -----------------------------------------------------------------

	/** True while the ledge pull-up owns the pawn. Movement input, dash, slide and jump are all off. */
	bool IsMantling() const;

	/** Seconds of pull-up left, 0 when not mantling. For anim/HUD tells; never feeds the simulation. */
	float GetMantleTimeRemaining() const { return FMath::Max(0.f, MantleTimeRemaining); }

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

	// --- Per-move intents ------------------------------------------------------------------------
	//
	// Public because the saved move and the compressed-flag unpack both drive them. Not UPROPERTYs:
	// they are per-move scratch state. bWantsToDash is one-shot (consumed at the end of the move);
	// bWantsToSlide is a level, held for as long as the key is down.

	uint8 bWantsToDash : 1;
	uint8 bWantsToSlide : 1;

protected:
	/** Locks the direction, spends a charge, starts the dash window and launches the velocity. */
	void BeginDash();

	/** Locks the direction, sets the entry speed and starts the slide's fixed-length window. */
	void BeginSlide();

	// --- Mantle (spec v5 §7) ---------------------------------------------------------------------

	/**
	 * Looks for a climbable ledge ahead and, if it finds one with room to stand, starts the pull-up.
	 * Returns true if a mantle began. Pure function of restored state + static geometry, so it makes
	 * the same decision on the client, on the server and on every replayed move.
	 *
	 * ApproachVelocity is OnMovementUpdated's OldVelocity — the velocity at the START of the move,
	 * before any collision response. It has to be, and that is not a nicety: the frame a jump's
	 * capsule meets a ledge face is the frame the sweep zeroes the planar velocity against it, so the
	 * current Velocity says the pawn was standing still and the speed gate refuses. This parameter is
	 * why the mantle fires at all.
	 */
	bool TryBeginMantle(const FVector& ApproachVelocity);

	/** Cheap pre-test: alive, enabled, off cooldown, airborne, not already busy with another ability. */
	bool CanAttemptMantle() const;

	/**
	 * One sub-step of the pull-up, written from inside CalcVelocity so it moves the pawn on the same
	 * frame. Phase 1 climbs to MantleUpTargetZ, phase 2 crosses to MantleTargetLocation.
	 */
	void ApplyMantleVelocity(float DeltaTime);

	/** Hands the pawn back to MOVE_Falling with its entry speed (capped at the ground limit). */
	void EndMantle();

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

	/**
	 * SPEC v9 §5. Seconds a wall jump puts the MANTLE on cooldown for.
	 *
	 * "If a player inputs a wall jump, that overrides a mantle." Refusing the mantle on the launch
	 * frame is only half of it — the pawn is still beside the same ledge for several frames after,
	 * and an auto-mantle needs no input to claim it. This window is what stops the mantle undoing a
	 * wall jump one frame after it happened. Applied by pushing MantleCooldownRemaining, which is
	 * already saved-move state, so it adds no prediction plumbing.
	 */
	float GetWallJumpMantleLockoutSeconds() const;

	/** Flat uu/s pushed straight out along the wall normal, on top of the reflection. */
	float GetWallJumpOutwardImpulse() const;

	/** Vertical launch, as a multiple of JumpZVelocity — the unit every other launch here uses. */
	float GetWallJumpVerticalMultiplier() const;

	/** Consecutive wall jumps allowed without touching the ground. The anti-ladder cap. */
	int32 GetWallJumpMaxConsecutive() const;

	/** Largest |Normal.Z| still counted as a wall. Above it the surface is a ramp, not a face. */
	float GetWallJumpMaxNormalZ() const;

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

	// --- Mantle / ledge tuning (spec v5 §7) -------------------------------------------------------

	bool  IsMantleEnabled() const;

	/** How far ahead of the capsule's surface a ledge face may be and still be grabbed. */
	float GetMantleReachUU() const;

	/** Below this the engine's own step-up handles it and a mantle would look like a stutter. */
	float GetMantleMinHeightUU() const;

	/** Above this it is a wall, not a ledge. Hip-to-shoulder plus the jump's own rise. */
	float GetMantleMaxHeightUU() const;

	/** Total length of the pull-up. */
	float GetMantleDurationSeconds() const;

	/** Fraction of that spent climbing before the pawn moves forward over the lip. */
	float GetMantleUpPhaseFraction() const;

	/** Blocks an immediate re-grab of the same lip after a mantle ends. */
	float GetMantleCooldownSeconds() const;

	/** Minimum planar speed toward the wall. Stops a standing pawn vacuuming itself up every wall. */
	float GetMantleMinForwardSpeed() const;

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

	// --- Ledge / mantle state (all saved/restored by FSavedMove_Trace) ----------------------------

	/**
	 * Seconds of "the ability layer still counts this pawn as grounded" left after ground contact is
	 * lost. Refilled to LedgeGroundGraceSeconds on every grounded move.
	 *
	 * SAVED-MOVE STATE. It gates EndSlide(), the fast-fall and the landing transition, so a replay
	 * that lost it would resolve a ledge blip differently from the original — which is precisely the
	 * class of divergence it was added to remove.
	 */
	float GroundGraceRemaining;

	/** Seconds of pull-up left. Non-zero IS "mantling"; the mantle owns Velocity for its whole run. */
	float MantleTimeRemaining;

	/** Length the current mantle started with, so the two phases can be timed against a fixed total. */
	float MantleTotalTime;

	/** Where the pawn is being pulled to: standing on the ledge, capsule centre. */
	FVector MantleTargetLocation;

	/** Z the climb phase rises to before the pawn moves forward. Always >= the target's Z. */
	float MantleUpTargetZ;

	/** Planar speed at the instant the mantle began, handed back (capped) on exit. */
	float MantleEntrySpeed;

	/** Blocks re-grabbing the same lip the frame after a mantle ends. Charged in EndMantle(). */
	float MantleCooldownRemaining;

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
	// --- Slide measurement ("-TraceSlideDebug", or `Trace.SlideDebug 1`) -------------------------
	//
	// Deliberately NOT saved-move state and never read by the simulation: these only observe. They
	// are what let a headless match answer "are slides actually longer now" with numbers instead of
	// an opinion. The running mean they feed is process-wide (a file-scope counter in the .cpp), not
	// per-pawn, because the question is about the mechanic and ten bots is the sample.
	float SlideDebugStartTime = 0.f;
	FVector SlideDebugStartLocation = FVector::ZeroVector;
	float SlideDebugEntrySpeed = 0.f;
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
	 * and over, counting ground-state flips, mantles and (on a client) server corrections.
	 *
	 * It builds its own geometry on purpose. The arena's raised sections are real ledges, but their
	 * positions depend on the arena builder's tuning, and a diagnosis of a prediction bug has to be
	 * repeatable against the same lip every time or the numbers measure the level.
	 *
	 * Runs on the LOCALLY CONTROLLED pawn in any net mode — unlike TickMomentumMeasure, which is
	 * standalone-only — because the whole point is to measure what a networked client experiences.
	 */
	void TickLedgeTest(float DeltaSeconds);

	float LedgeTestTime = -1.f;
	int32 LedgeTestPhase = 0;
	float LedgeTestPhaseTime = 0.f;
	int32 LedgeTestRun = 0;
	int32 LedgeTestGroundFlips = 0;
	int32 LedgeTestMantles = 0;
	uint8 bLedgeTestWasGrounded : 1;
	FVector LedgeTestStart = FVector::ZeroVector;
	FVector LedgeTestRunDirection = FVector::ForwardVector;
	TWeakObjectPtr<AActor> LedgeTestBlock;
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

	/**
	 * SPEC v9 §5, THE MEASUREMENT FOR "IT FEELS LIKE STICKING TO THE WALL FOR A SECOND".
	 *
	 * Counts mantles that STARTED while a wall-jump window was open — i.e. wall jumps the player was
	 * still entitled to make and the automatic mantle took away, locking the pawn in MOVE_Flying (where
	 * CanAttemptJump() refuses every press) for GetMantleDurationSeconds().
	 *
	 * This is the number that separates the two arms: under -TraceLegacyTuning the mantle wins the
	 * frame and this climbs; with the §5 priority in force it must be ZERO. Dev-only, never saved,
	 * counted on the record pass only.
	 */
	int32 WallJumpMantleSteals = 0;

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
	 * Intents. FLAG_Custom_0 = dash, FLAG_Custom_2 = crouch/slide held.
	 * FLAG_Custom_1 used to be boost and is now FREE — spec v3 §1 deleted the ability.
	 */
	uint8 bSavedWantsToDash : 1;
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
	 * The ledge grace and the whole mantle (spec v5 §7).
	 *
	 * Every one of these is restored by PrepMoveFor. A correction that landed mid-pull-up and lost
	 * them would replay the mantle as a fall — the pawn would be on top of the ledge on one machine
	 * and at the bottom of it on the other, which is the largest possible version of the exact bug
	 * this feature was added to fix.
	 */
	float SavedGroundGraceRemaining;
	float SavedMantleTimeRemaining;
	float SavedMantleTotalTime;
	FVector SavedMantleTargetLocation;
	float SavedMantleUpTargetZ;
	float SavedMantleEntrySpeed;
	float SavedMantleCooldownRemaining;

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
