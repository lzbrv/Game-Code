// Trace — MACE (spec v14 §6).
//
// ===================================================================================================
// THE DOC, VERBATIM
// ===================================================================================================
//
//   PASSIVE   "+30% magnet radius for the Core arcing to her. The base is now 450 uu (reduced 10% in
//             Demo 12), so Mace's is 585 uu. Derive it, do not hardcode."
//
//   MOVEMENT  "hold V in the air to suspend for up to 1.25 s — gravity does not affect her, and she
//             may move laterally capped at 550 uu/s. Releasing V cancels immediately and gravity
//             resumes."
//
//   ACTIVATED "Spike: throws a roped spike in her aim direction for a medium distance. On hitting a
//             WALL it embeds for 2 s. Reactivating pulls her toward it at the momentum ceiling (the
//             air-strafe hard cap). Any movement input cancels the pull and removes the spike. She
//             obeys normal physics while pulled — bouncing off a wall cancels it. She can shoot while
//             pulled, and be shot."   [ASSUMPTION] 20 s cooldown; §6 leaves it unspecified. FLAGGED.
//
// ===================================================================================================
// MACE AND THE CARRIER CHOKE POINT
// ===================================================================================================
//
// Mace has no ability that damages, slows, pulls or knocks back another player. Nothing she does
// takes a target at all: the suspend and the pull both write HER OWN velocity, and the spike anchors
// to world geometry. So there is no CanAffect() call in this file, and that is not an oversight — it
// is the reason Oyster's file is full of them and this one is not. If a future tuning pass ever lets
// the spike catch a player, or lets the pull drag somebody along, THAT is the moment this file gains
// a CanAffect(Target, Control) and not a moment later.
//
// ===================================================================================================
// HOW SUSPEND AND PULL MOVE HER, AND WHY IT IS DONE THIS WAY
// ===================================================================================================
//
// Both write UCharacterMovementComponent::Velocity every frame from TickAbilities(), on the SERVER
// and on the OWNING CLIENT, from state both machines compute the same way.
//
// The obvious implementations are both closed:
//   * GravityScale = 0 for the suspend does not survive one frame. UTraceCharacterMovementComponent::
//     RefreshEngineTunablesFromSettings() re-pushes GravityScale (and MaxWalkSpeed, and AirControl)
//     from UTraceSettings on EVERY simulated move, precisely so those values stay live under
//     Trace.LiveEdit. Anything this file wrote there would be overwritten before the next physics
//     step, and would additionally be wrong on a replayed move.
//   * A proper saved-move flag is the right long-term answer and is NOT available: FSavedMove_Trace
//     and the whole prediction path live in Movement/TraceCharacterMovementComponent.h, which this
//     slice does not own.
//
// So the suspend zeroes Velocity.Z and clamps the planar speed each frame, and the pull writes the
// whole velocity vector each frame. Because both ends run the identical rule off the identical
// replicated inputs, they agree to within one frame of gravity — MEASURED, and the number is in the
// report. It is not free: neither is saved-move state, so a corrected/replayed move re-derives
// velocity without the ability and the correction path absorbs the difference. That is a real, named
// limitation of this pass and the fix is a cross-file one.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetMace.generated.h"

class ATraceMaceSpike;

/** Mace's bits in FTraceAbilityNetState::Flags. Only one character is live per component, so these cannot collide. */
namespace TraceMaceFlags
{
	/** Suspending: holding V in the air. TraceAbilityFlags::EffectActive. */
	inline constexpr uint8 Suspending = 1 << 0;
	/** Being pulled toward the spike. TraceAbilityFlags::MovementActive. */
	inline constexpr uint8 Pulling = 1 << 1;
	/** A spike is embedded and may be pulled on. TraceAbilityFlags::AuxActive. */
	inline constexpr uint8 SpikeEmbedded = 1 << 2;
	/** A spike is in flight and may NOT be pulled on yet. TraceAbilityFlags::Charged. */
	inline constexpr uint8 SpikeInFlight = 1 << 3;
}

UCLASS()
class TRACE_API UTraceAbilitySetMace : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Mace; }

	// --- lifecycle ---------------------------------------------------------------------------------
	virtual void OnEquipped() override;
	virtual void OnUnequipped() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// --- PASSIVE: the magnet ------------------------------------------------------------------------

	/**
	 * §6: "+30% magnet radius ... Derive it, do not hardcode." 1 + UTraceSettings::MaceMagnetRadiusBonus,
	 * read live, so 450 x 1.30 = 585 falls out of CoreCatchRadius and follows it if it is retuned.
	 *
	 * NOTHING CALLS THIS YET. Gameplay/TraceCore.cpp must multiply its per-player catch radius by
	 * UTraceAbilityComponent::GetMagnetRadiusMultiplierFor(Candidate); that file is not this slice's.
	 * The multiplier is correct and provable in isolation (Trace.Mace.Verify prints the derived uu),
	 * and it does nothing in a live match until that one line lands.
	 */
	virtual float GetMagnetRadiusMultiplier() const override;

	// --- MOVEMENT: hold V to suspend ----------------------------------------------------------------

	/** V pressed. Starts the suspend if she is airborne and off cooldown. Returns true if it started. */
	virtual bool OnSecondaryPressed() override;

	/** V released. "Releasing V cancels immediately and gravity resumes." */
	virtual void OnSecondaryReleased() override;

	// --- ACTIVATED: Spike ---------------------------------------------------------------------------

	virtual float GetActivatedCooldownSeconds() const override;
	virtual bool  CanActivate(FText& OutReason) const override;

	/** Throws the spike. Returns FALSE (a free fizzle, no cooldown) when the aim finds no wall. */
	virtual bool ActivateAbility() override;

	/**
	 * The REACTIVATION. Starts the pull toward an embedded spike.
	 *
	 * NOT reachable through TryActivate() and that is deliberate — see UTraceAbilityInputRelay's
	 * header. TryActivate() refuses while the cooldown the throw just started is running, and
	 * weakening that check for one character would put a second opinion into the one cooldown
	 * contract the whole framework rests on. The relay routes the press here instead.
	 *
	 * Safe to call on any machine; does nothing unless a spike is embedded and ready.
	 */
	void RequestSpikePull();

	/** True when a press of E should be read as the reactivation rather than as a fresh throw. */
	bool IsSpikeReadyToPull() const;

	/** Called by the spike actor the frame its flight ends. Server only. */
	void NotifySpikeEmbedded(ATraceMaceSpike* WhichSpike);

	// --- state, for the HUD and for Trace.Mace.Verify -------------------------------------------------

	bool IsSuspending() const { return bSuspending; }
	bool IsPulling() const { return bPulling; }
	ATraceMaceSpike* GetSpike() const { return Spike.Get(); }

	/** The momentum ceiling the pull runs at, derived per §6 from the air-strafe hard cap. */
	float GetPullSpeed() const;

	/** Seconds of suspend left, 0 when not suspending. For the HUD and the harness. */
	float GetSuspendRemaining() const;

	/**
	 * HARNESS ONLY. Throws a spike at an explicit anchor, bypassing the aim sweep, so the spike and
	 * pull can be exercised without steering a pawn at a wall. Server only.
	 */
	ATraceMaceSpike* DebugThrowSpikeAt(const FVector& Anchor, float TravelSpeedOverride);

	/** HARNESS ONLY. The aim sweep on its own: "would a throw from here find a wall, and where?" */
	bool ResolveSpikeAnchor(FVector& OutAnchor, FVector& OutNormal, FString& OutWhy) const;

private:
	// --- suspend ------------------------------------------------------------------------------------
	bool  bSuspendHeld = false;
	bool  bSuspending = false;
	float SuspendEndMatchTime = 0.f;
	float SuspendReadyMatchTime = 0.f;

	void StartSuspend();
	void StopSuspend(const TCHAR* Why);
	void ApplySuspend(float DeltaSeconds);

	// --- spike / pull -------------------------------------------------------------------------------
	TWeakObjectPtr<ATraceMaceSpike> Spike;

	/** Absolute match time the 2 s embed window closes. Meaningless while the spike is in flight. */
	float SpikeEmbedEndMatchTime = 0.f;

	bool    bPulling = false;
	FVector PullAnchor = FVector::ZeroVector;
	FVector PullLastLocation = FVector::ZeroVector;

	/**
	 * The pull's first tick does not test for movement input.
	 *
	 * §6 says "any movement input cancels", and taken literally a player still holding W from the
	 * approach cancels the pull on the frame she starts it — the ability would be unusable in
	 * motion, which cannot be what "reactivating pulls her toward it" means. One frame of grace is
	 * the smallest thing that makes the rule mean "let go and then press" rather than "you must
	 * already be still". It is a frame, not a window: the second tick tests, and holding a key
	 * cancels then.
	 */
	bool bPullSkipInputTestThisTick = false;

	void StartPull();
	void StopPull(const TCHAR* Why, bool bRemoveSpike);
	void ApplyPull(float DeltaSeconds);

	void ClearSpike();

	/** Server only. Mirrors the local state into the replicated scratch pad for the HUD and proxies. */
	void PublishState();

	/** True on the machines that actually simulate this pawn's movement: the server and its owner. */
	bool ShouldDriveMovement() const;
};
