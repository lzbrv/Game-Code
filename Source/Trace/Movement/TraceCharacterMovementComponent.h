// Trace — character movement with a genuinely client-predicted movement kit.
//
// Nothing in here is an RPC that plays a montage. Dash, boost, slide and the air fast-fall are all
// first-class movement states that ride the engine's saved-move pipeline, exactly like crouch or
// jump:
//
//   1. Input calls StartDash() / StartBoost() / SetWantsToSlide() on the owning client, which
//      raises an intent flag.
//   2. FSavedMove_Trace::SetMoveFor() snapshots that intent (plus every timer, charge counter and
//      locked direction) into the move about to be simulated.
//   3. The client simulates the move immediately — the ability starts on the same frame the key is
//      pressed, at any ping.
//   4. GetCompressedFlags() packs the intents into FLAG_Custom_0..2, which travel to the server
//      inside the ordinary ServerMove RPC. No extra RPC, no extra bandwidth.
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
// updated component's rotation, the saved timers), so client and server always compute the same
// answer from the same inputs. Nothing here reads wall-clock time or per-frame input state that
// the replay cannot reproduce.
//
// --- THE KIT (contract §5) -------------------------------------------------------------------
//
//   DASH   Horizontal-plane burst along the current input direction. NEVER adds vertical velocity.
//          Runs on a CHARGE system: one charge for everybody, two while carrying the Core, each
//          charge refilling on its own DashCooldown. See "charges" below.
//   JUMP   Untouched — plain ACharacter::Jump.
//   CROUCH On the ground: a slide (a directional, decaying burst you steer weakly).
//          In the air: a fast-fall that zeroes POSITIVE Z velocity only, on the press edge.
//   BOOST  Ground-only vertical launch on a long cooldown. A separate bind from jump.
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
// carrier bit rather than recomputed from scratch. It cannot drift permanently. Fixing it properly
// would mean putting the carrier bit in the compressed flags, i.e. letting the client assert its
// own carrier state to the server, which is not a trade worth making.

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

	/** Where the whole kit is simulated. Called once per move, on every machine. */
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

	// --- Boost API -------------------------------------------------------------------------------

	/** Requests a boost. Same contract as StartDash(): raises intent, CanBoost() gates activation. */
	void StartBoost();

	/** Off cooldown, alive, and standing on the ground. */
	bool CanBoost() const;

	/** Seconds until the next boost is allowed (0 when ready). */
	float GetBoostCooldownRemaining() const;

	/** The full boost cooldown from UTraceSettings — the denominator for a HUD meter. */
	float GetBoostCooldown() const;

	// --- Per-move intents ------------------------------------------------------------------------
	//
	// Public because the saved move and the compressed-flag unpack both drive them. Not UPROPERTYs:
	// they are per-move scratch state. bWantsToDash and bWantsToBoost are one-shot (consumed at the
	// end of the move); bWantsToSlide is a level, held for as long as the key is down.

	uint8 bWantsToDash : 1;
	uint8 bWantsToBoost : 1;
	uint8 bWantsToSlide : 1;

protected:
	/** Locks the direction, spends a charge, starts the dash window and launches the velocity. */
	void BeginDash();

	/** Locks the direction, sets the entry speed and starts the slide window. */
	void BeginSlide();

	/** Ends a slide and hands the player back at walking speed. */
	void EndSlide();

	/** Spends the boost cooldown, sets +Z and drops the pawn into MOVE_Falling. */
	void BeginBoost();

	/** True if a slide could start on this exact frame (ground, off cooldown, moving fast enough). */
	bool CanStartSlide() const;

	/**
	 * Bleeds planar speed back down to GetMaxSpeed() in one step.
	 *
	 * Used the frame a dash or a slide ends. Without it CalcVelocity only sheds the excess through
	 * braking friction, which at DashSpeed takes the better part of a second — so the ability would
	 * keep moving the player long after IsDashing()/IsSliding() (and therefore the trail rule) said
	 * it was over. Doing it here rather than through a velocity cap is what keeps it predictable: it
	 * happens on exactly the frame the saved timer crosses zero, and that frame replays identically
	 * on the client and the server.
	 */
	void ClampPlanarSpeedToMax();

	// Settings are read live (never cached in the constructor) so both ends of the wire always
	// agree with the config, and so designers can retune without a rebuild.
	float GetDashSpeed() const;
	float GetDashDuration() const;
	float GetDashCooldown() const;

	/** DashDuration + DashCooldown: the cooldown is measured from dash START, as it always was. */
	float GetDashRechargeWindow() const;

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
	float SlideCooldownRemaining;
	float SlideSpeed;
	FVector SlideDirection;

	/**
	 * Seconds of "I pressed crouch and meant it" left over from a press that could not start a slide
	 * yet — because the pawn was mid-dash, or still in the air.
	 *
	 * Without this, "dash and then slide out of it" is impossible unless the player releases and
	 * re-presses crouch inside the 180ms the dash lasts, and "press crouch just before landing"
	 * silently does nothing. Both are things players do constantly. The buffer is short enough that
	 * it cannot turn a held key into a slide loop: the ground slide still needs a fresh PRESS, and
	 * SlideCooldown still gates the repeat.
	 */
	float SlideBufferRemaining;

	/**
	 * bWantsToSlide as it stood at the END of the previous move, so the air fast-fall can fire on the
	 * press EDGE rather than continuously. Saved state: without it a replayed move would see a stale
	 * edge and fast-fall a second time, cancelling a jump the player did make.
	 */
	uint8 bSlideHeldLastMove : 1;

	// --- Boost state (saved/restored) --------------------------------------------------------------

	float BoostCooldownRemaining;

	/** See GetLastDashActiveWorldTime(). Server observation only; never saved or replicated. */
	float LastDashActiveWorldTime = -1000.f;

#if !UE_BUILD_SHIPPING
	/**
	 * "-TraceMoveKitTest": a scripted, headless exercise of the whole kit on the local player pawn.
	 *
	 * It exists because crouch and boost have no key bound to them yet — the mapping context lives in
	 * ATracePlayerController, which this slice does not own — so without it there is no way to prove
	 * slide, fast-fall or boost actually run in a real world with real collision and real gravity.
	 * It drives the same public entry points the input layer will (StartDash / SetWantsToSlide /
	 * StartBoost), and logs the measured result at Display.
	 *
	 * Standalone + locally controlled + player controlled only, off unless the switch is on the
	 * command line, and compiled out of shipping.
	 */
	void TickSelfTest(float DeltaSeconds);

	float SelfTestTime = -1.f;
	int32 SelfTestStep = 0;
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
 *   CanCombineWith()     refuse to merge moves whose ability state differs
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

	/** Intents. FLAG_Custom_0 = dash, FLAG_Custom_1 = boost, FLAG_Custom_2 = crouch/slide held. */
	uint8 bSavedWantsToDash : 1;
	uint8 bSavedWantsToBoost : 1;
	uint8 bSavedWantsToSlide : 1;

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

	float SavedBoostCooldownRemaining;
};

/** Client prediction data whose only job is to hand out FSavedMove_Trace instances. */
class TRACE_API FNetworkPredictionData_Client_Trace : public FNetworkPredictionData_Client_Character
{
public:
	typedef FNetworkPredictionData_Client_Character Super;

	explicit FNetworkPredictionData_Client_Trace(const UCharacterMovementComponent& ClientMovement);

	virtual FSavedMovePtr AllocateNewMove() override;
};
