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
//   DASH   Horizontal-plane burst along the current input direction. NEVER adds vertical velocity.
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

#if !UE_BUILD_SHIPPING
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

	/** True for the DashDuration window. Read by the trail trip test — the whole game hangs on it. */
	bool IsDashing() const;

	/**
	 * Seconds until a dash is available again (0 when one is ready RIGHT NOW, charges included).
	 *
	 * Kept as the single number the HUD draws: it still spans the active dash window plus the
	 * refill, so a meter drawn as 1 - Remaining/(DashDuration + DashCooldown) is still correct. A
	 * carrier who has spent one of two charges reads 0 here, because they can in fact dash again.
	 */
	float GetDashCooldownRemaining() const;

	/** Dash charges available right now. */
	int32 GetDashCharges() const { return DashCharges; }

	/** Size of the charge pool: BaseDashCharges, plus CarrierExtraDashCharges while carrying. */
	int32 GetMaxDashCharges() const;

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

	/** Planar world-space direction locked at activation. */
	FVector DashDirection;

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
#endif
};

/**
 * One simulated movement frame, extended with the whole movement kit's state.
 *
 * Contract for the five overrides (all of them call Super first):
 *   Clear()              wipe every added field — moves are pooled and reused
 *   SetMoveFor()         capture CMC state *before* the move is simulated
 *   PrepMoveFor()        push that captured state back into the CMC before a replay
 *   GetCompressedFlags() pack the intents for the wire
 *   CanCombineWith()     refuse to merge moves whose ability or momentum state differs
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
};

/** Client prediction data whose only job is to hand out FSavedMove_Trace instances. */
class TRACE_API FNetworkPredictionData_Client_Trace : public FNetworkPredictionData_Client_Character
{
public:
	typedef FNetworkPredictionData_Client_Character Super;

	explicit FNetworkPredictionData_Client_Trace(const UCharacterMovementComponent& ClientMovement);

	virtual FSavedMovePtr AllocateNewMove() override;
};
