// Trace — character movement with a genuinely client-predicted dash.
//
// The dash is not an RPC that plays a montage. It is a first-class movement state that rides the
// engine's saved-move pipeline, exactly like crouch or jump:
//
//   1. Input calls StartDash() on the owning client, which raises bWantsToDash.
//   2. FSavedMove_Trace::SetMoveFor() snapshots that intent (plus the dash timer, the cooldown and
//      the locked direction) into the move about to be simulated.
//   3. The client simulates the move immediately — the dash starts on the same frame the key is
//      pressed, at any ping.
//   4. GetCompressedFlags() packs the intent into FLAG_Custom_0, which travels to the server
//      inside the ordinary ServerMove RPC. No extra RPC, no extra bandwidth.
//   5. The server runs UpdateFromCompressedFlags() → the identical simulation → authoritative
//      result. If it disagrees it sends a correction, and the client replays its unacknowledged
//      moves through PrepMoveFor()/MoveAutonomous().
//
// Step 5 is why the dash timer and the cooldown are saved-move state and not plain members: a
// replay restores the character to the *start* of an old move, and if the dash clock did not come
// back with it, every correction mid-dash would resimulate with the wrong remaining time and the
// client would rubber-band. Same for the locked direction.
//
// Everything the dash touches is derived from data the replay path restores (Acceleration, the
// updated component's rotation, the saved timers), so client and server always compute the same
// answer from the same inputs. Nothing here reads wall-clock time or per-frame input state that
// the replay cannot reproduce.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "TraceCharacterMovementComponent.generated.h"

class ACharacter;

UCLASS()
class TRACE_API UTraceCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

	// Needs to read/write the dash state directly when snapshotting and restoring moves.
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

	/** Where the dash is actually simulated. Called once per move, on every machine. */
	virtual void OnMovementUpdated(float DeltaSeconds, const FVector& OldLocation, const FVector& OldVelocity) override;

	virtual float GetMaxSpeed() const override;

	// --- Dash API ------------------------------------------------------------------------------

	/**
	 * Requests a dash. Call on the machine that owns the input (the autonomous client, or the
	 * server for a listen-host / AI). Raises bWantsToDash; the next simulated move consumes it.
	 * Safe to spam — CanDash() gates the actual activation.
	 */
	void StartDash();

	/** Cooldown elapsed, has an owner, and in a movement mode that can dash. */
	bool CanDash() const;

	/** True for the DashDuration window. Read by the trail trip test — the whole game hangs on it. */
	bool IsDashing() const;

	/** Seconds until the next dash is allowed (0 when ready). Includes the active dash window. */
	float GetDashCooldownRemaining() const;

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

	/**
	 * Dash intent for the move currently being simulated. Public because the saved move and the
	 * compressed-flag unpack both drive it. Not a UPROPERTY: it is per-move scratch state.
	 */
	uint8 bWantsToDash : 1;

protected:
	/** Locks the direction, starts the dash window and the cooldown, and launches the velocity. */
	void BeginDash();

	// Settings are read live (never cached in the constructor) so both ends of the wire always
	// agree with the config, and so designers can retune the dash without a rebuild.
	float GetDashSpeed() const;
	float GetDashDuration() const;
	float GetDashCooldown() const;

	/** Seconds of dash left. Saved/restored by FSavedMove_Trace. */
	float DashTimeRemaining;

	/**
	 * Seconds until the next dash. Starts at DashDuration + DashCooldown, i.e. the cooldown is
	 * measured from dash *start* (see UTraceSettings::DashCooldown). Saved/restored.
	 */
	float DashCooldownRemaining;

	/** Planar world-space direction locked at activation. Saved/restored. */
	FVector DashDirection;

	/** See GetLastDashActiveWorldTime(). Server observation only; never saved or replicated. */
	float LastDashActiveWorldTime = -1000.f;
};

/**
 * One simulated movement frame, extended with the dash state.
 *
 * Contract for the five overrides (all of them call Super first):
 *   Clear()              wipe every added field — moves are pooled and reused
 *   SetMoveFor()         capture CMC state *before* the move is simulated
 *   PrepMoveFor()        push that captured state back into the CMC before a replay
 *   GetCompressedFlags() pack the intent for the wire
 *   CanCombineWith()     refuse to merge moves whose dash state differs
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

	/** Dash requested during this move. Travels to the server as FLAG_Custom_0. */
	uint8 bSavedWantsToDash : 1;

	/** Dash clocks and locked direction as they stood *before* this move was simulated. */
	float SavedDashTimeRemaining;
	float SavedDashCooldownRemaining;
	FVector SavedDashDirection;
};

/** Client prediction data whose only job is to hand out FSavedMove_Trace instances. */
class TRACE_API FNetworkPredictionData_Client_Trace : public FNetworkPredictionData_Client_Character
{
public:
	typedef FNetworkPredictionData_Client_Character Super;

	explicit FNetworkPredictionData_Client_Trace(const UCharacterMovementComponent& ClientMovement);

	virtual FSavedMovePtr AllocateNewMove() override;
};
