// Trace — CHUT, spec v14 §6.
//
//   PASSIVE   the knife deals 50 from the FRONT (the standard is 30). The 60° back zone stays at
//             100 — §6's [ASSUMPTION], and it is a knob rather than an omission so that "unchanged"
//             is a decision somebody made rather than a value nobody looked at.
//
//   MOVEMENT  BASH — hitting a player with the END of his standard dash knocks them along his
//             direction of travel. "NO EFFECT ON THE CORE CARRIER" — which is not a special case
//             here: it is ETraceAbilityEffect::Control asked of the §4 choke point, and the choke
//             point's Control answer for a carrier is exactly the sentence §6 wrote.
//             "Dashing through a trace still kills the carrier normally" — untouched, because the
//             bash never goes near the trail's trip test and never cancels a dash.
//
//   ACTIVATED CHUD — 30% less damage from body shots and melees for 10 s, REFRESHED (not extended,
//             not stacked) by a knife kill. 20 s cooldown. §6 [ASSUMPTION]: headshots and trace
//             deaths are unaffected, because it names body shots and melees.
//
// ===================================================================================================
// WHY THE BASH IS DRIVEN BY A POLL AS WELL AS BY THE HOOK
// ===================================================================================================
//
// UTraceAbilityComponent::NotifyDashHitCharacter is the framework's intended entry point, and this
// class implements OnDashHitCharacter for it. But NOTHING IN THE PROJECT CALLS IT YET — the movement
// slice has to, and that is a cross-file change this pass does not own. A bash that exists only
// behind an uncalled hook is a bash that does not exist.
//
// So the same single apply path (TryBash) is ALSO driven from TickAbilities on the server, which
// polls the pawn's own public dash state (IsDashing / GetDashDirection) and sweeps for victims. Both
// routes are guarded by BashedThisDash, so when the movement slice does start calling the hook the
// result is one bash per victim per dash, not two.
//
// The poll is the working implementation; the hook is the seam. Neither is a second RULE — both
// funnel into TryBash and TryBash asks the choke point exactly once.

#pragma once

#include "CoreMinimal.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetChut.generated.h"

UCLASS()
class TRACE_API UTraceAbilitySetChut : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Chut; }

	// --- lifecycle --------------------------------------------------------------------------------
	virtual void OnPawnSpawned() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// --- activated: Chud --------------------------------------------------------------------------
	virtual bool  ActivateAbility() override;
	virtual float GetActivatedCooldownSeconds() const override;

	// --- movement: the bash -----------------------------------------------------------------------
	virtual bool OnDashStarted(const FVector& DashDirection) override;
	virtual void OnDashEnded(bool bReachedFullDistance) override;
	virtual void OnDashHitCharacter(ATraceCharacter* Other, float DashProgress) override;
	virtual float GetDashHitSweepRadius() const override;

	// --- passive: the knife, and Chud's reduction --------------------------------------------------
	virtual float ModifyOutgoingDamage(float Damage, const FTraceAbilityDamageContext& Context) const override;
	virtual float ModifyIncomingDamage(float Damage, const FTraceAbilityDamageContext& Context) const override;
	virtual void  OnKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot) override;

	// --- queries, for the HUD and the verification harness -----------------------------------------

	/** True while Chud's window is open. Correct on every machine — it reads replicated state. */
	bool IsChudActive() const;

	/** Seconds left on Chud, 0 when it is down. */
	float GetChudSecondsRemaining() const;

	/**
	 * THE ONE APPLY PATH FOR THE BASH. Both the poll and OnDashHitCharacter come through here, and
	 * so does the harness — so what is verified is what ships.
	 *
	 * AUTHORITY ONLY. Applies, in order: the end-of-dash window, the reach, the §4 choke point
	 * (Control), and only then a launch.
	 *
	 * @param Victim         who is being hit.
	 * @param DashProgress   0..1 through the dash. §6 says the END of the dash, so an early value
	 *                       is refused rather than clamped.
	 * @param DashDirection  Chut's direction of travel; the knock goes this way, plus an up bias.
	 * @param OverrideGapUU  negative (the default) measures the gap from Chut's CURRENT location.
	 *                       The poll passes the distance to its swept segment instead, because at
	 *                       the component's 20 Hz tick a dash travels further between samples than
	 *                       the reach — see LastPolledLocation.
	 * @return true when the victim was actually launched.
	 */
	bool TryBash(ATraceCharacter* Victim, float DashProgress, const FVector& DashDirection,
	             float OverrideGapUU = -1.f);

	/** How many victims the current (or most recent) dash has bashed. */
	int32 GetBashedThisDashCount() const { return BashedThisDash.Num(); }

private:
	/** Server-side dash-edge tracking for the poll. See the header for why the poll exists. */
	bool  bWasDashing = false;
	float DashStartWorldTime = 0.f;
	FVector PolledDashDirection = FVector::ZeroVector;

	/**
	 * Chut's location at the previous poll, so the reach is a SWEPT SEGMENT rather than a point.
	 *
	 * This is not a refinement, it is a correctness fix forced by where the poll lives:
	 * UTraceAbilityComponent ticks at TickInterval = 0.05 s, and a dash covers DashSpeed x 0.05 =
	 * 150 uu between two samples — more than the whole 130 uu bash reach. A point test at each
	 * sample would therefore step straight over a player standing between two samples, and the bash
	 * would "work" in a harness that places the victim and miss in a real dash. Testing the victim
	 * against the segment closes the gap without widening the tuned reach by a single uu.
	 */
	FVector LastPolledLocation = FVector::ZeroVector;
	bool bHasLastPolledLocation = false;

	/** One bash per victim per dash — the guard that lets the poll and the hook coexist. */
	TArray<TWeakObjectPtr<ATraceCharacter>> BashedThisDash;

	/** Server only. Sweeps for victims inside the bash reach and calls TryBash on each. */
	void PollDashForBash(float DeltaSeconds);

	/** Clears the per-dash bookkeeping. */
	void ResetDashTracking();

	bool HasBashed(const ATraceCharacter* Victim) const;
};
