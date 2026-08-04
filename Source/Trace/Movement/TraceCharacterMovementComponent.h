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
// --- THE SLIDE IS A MOMENTUM CARRY, NOT A BRAKE -----------------------------------------------
//
// ENTRY is ENTRY SPEED (spec §2.3: "entry speed determines slide velocity"):
//
//       SlideSpeed = max(planar speed at entry,
//                        min(planar speed × SlideEntrySpeedMultiplier + SlideImpulse, SlideMaxSpeed))
//
//       SlideEntrySpeedMultiplier defaults to 1.0 and SlideImpulse to 0, which is the pure "no flat
//       momentum boost" reading. The spec contains a second, contradictory line asking for the
//       slide to INCREASE momentum; rather than guess, both readings are knobs and the shipped
//       defaults implement the first one. Raise SlideImpulse to get the second.
//
// MIDDLE bleeds slowly. SlideDeceleration is the friction dial and is meant to be small enough
//       that SlideDuration, not the decay, is what ends the slide.
// EXIT  hands the speed back. EndSlide() carries SlideExitSpeedRetention of the slide's current
//       speed into normal movement, floored at SlideExitMinSpeedFraction of the walk speed, and
//       capped at max(SlideExitMaxSpeedMultiplier × GetMaxSpeed(), the slide's own speed) — that
//       max() is what makes "slide → jump" preserve the vector instead of resetting it.
// AFTER SlideCooldownSeconds (0.8s, spec §2.3) must elapse from the slide's END before another can
//       start. The old SlideCooldown was measured from slide START, which made "the buffer between
//       slides" a number you had to compute rather than read.
//
// SlideMinCommitSeconds makes the first moments of a slide uncancellable, so a slide reads as a
// commitment rather than a tap, and so releasing the key a frame late cannot amputate it.
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
	 *   on the ground → start a slide (and hold it; releasing ends the slide early)
	 *   in the air    → fast-fall, i.e. zero out POSITIVE Z velocity once, on the press edge
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

	/** Seconds before another slide is allowed (0 when ready). Measured from the last slide's END. */
	float GetSlideCooldownRemaining() const { return FMath::Max(0.f, SlideCooldownRemaining); }

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

	/** Locks the direction, sets the entry speed and starts the slide + commit windows. */
	void BeginSlide();

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
	float GetSlideDuration() const;
	float GetSlideDeceleration() const;
	float GetSlideMinCommitSeconds() const;
	float GetSlideExitSpeedRetention() const;
	float GetSlideExitMinSpeedFraction() const;
	float GetSlideExitMaxSpeedMultiplier() const;

	/** Spec §2.3. Seconds between slides, measured from the END of the previous one. */
	float GetSlideCooldownSeconds() const;

	/** Spec §2.3 / the [CONFLICT]: entry speed × this, plus GetSlideImpulse(), is the slide speed. */
	float GetSlideEntrySpeedMultiplier() const;

	/** Spec §2.3 / the [CONFLICT]: a FLAT additive boost on slide entry. 0 by default. */
	float GetSlideImpulse() const;

	// Air / momentum tuning.
	bool  IsSourceAirAccelerationEnabled() const;
	float GetAirAcceleration() const;
	float GetAirMaxWishSpeed() const;
	float GetMaxAirSpeed() const;
	bool  IsLandingMomentumPreserved() const;
	float GetGroundOverspeedFriction() const;
	float GetGroundOverspeedBraking() const;
	float GetGroundOverspeedTurnRate() const;

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

	/**
	 * Seconds left of the window in which releasing crouch will NOT cancel the slide.
	 *
	 * A slide is a commitment: it is worth its cooldown precisely because you cannot bail out of it
	 * the instant it stops being convenient. It also fixes a mundane input problem — the slide is
	 * driven by a HELD level, so a key that comes up one frame early (or a bot whose hold timer
	 * expires early) used to amputate the slide, and the player never learns why theirs was short.
	 *
	 * SAVED-MOVE STATE, like every other clock here: a correction mid-slide must rewind this with
	 * the rest, or the replay decides the slide was cancellable when the original decided it was not
	 * and the two ends disagree about where the pawn went.
	 */
	float SlideCommitRemaining;

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
	float SavedSlideCommitRemaining;
	float SavedSlideSpeed;
	float SavedSlideBufferRemaining;
	FVector SavedSlideDirection;
	uint8 bSavedSlideHeldLastMove : 1;
	uint8 bSavedWasAirborneLastMove : 1;
};

/** Client prediction data whose only job is to hand out FSavedMove_Trace instances. */
class TRACE_API FNetworkPredictionData_Client_Trace : public FNetworkPredictionData_Client_Character
{
public:
	typedef FNetworkPredictionData_Client_Character Super;

	explicit FNetworkPredictionData_Client_Trace(const UCharacterMovementComponent& ClientMovement);

	virtual FSavedMovePtr AllocateNewMove() override;
};
