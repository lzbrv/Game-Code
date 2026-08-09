// Trace — ROCCO, spec v14 §6.
//
//   PASSIVE   "3% speed boost from headshot kills for 1 second, stacking, each kill extends the
//             timer on the entire boost." ONE timer over the WHOLE stack, refreshed by every kill —
//             not one timer per stack. [ASSUMPTION] the stack is capped (RoccoHeadshotSpeedStackMax,
//             10 = +30%), because the spec itself asks for the cap and for it to be a knob.
//
//   MOVEMENT  "a very small second jump, which allows Rocco to change direction midair, instantly."
//             The DIRECTION CHANGE is the ability; the height is deliberately a fraction of a real
//             jump. One extra jump per airtime, restored on landing.
//
//   ACTIVATED "Ripple" — a dash in any direction on a SEPARATE cooldown from the standard dash,
//             leaving a ridable path behind for 4 s. 20 s cooldown. See TraceRippleActor.h, which
//             carries the clause-by-clause reading; this file is only the trigger and the tuning.
//
// EVERYTHING THAT TOUCHES ANOTHER PLAYER GOES THROUGH THE CHOKE POINT. Rocco's only such thing is
// ripple entry, and it asks as ETraceAbilityEffect::Beneficial — which is exactly how §6's "the Core
// carrier can use it" is satisfied WITHOUT an exception to §4's rule.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectPtr.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetRocco.generated.h"

class ATraceRippleActor;

UCLASS()
class TRACE_API UTraceAbilitySetRocco : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Rocco; }

	// --- lifecycle --------------------------------------------------------------------------------
	virtual void OnUnequipped() override;
	virtual void OnPawnSpawned() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// --- activated: Ripple ------------------------------------------------------------------------
	virtual bool  ActivateAbility() override;
	virtual float GetActivatedCooldownSeconds() const override;

	// --- movement: the second jump ----------------------------------------------------------------
	virtual bool OnJumpPressed() override;

	// --- passive: the headshot speed stack ---------------------------------------------------------
	virtual float GetMoveSpeedMultiplier() const override;
	virtual void  OnKill(ATraceCharacter* Victim, FName Cause, bool bHeadshot) override;

	// --- queries, for the HUD and the verification harness -----------------------------------------

	/** Stacks live right now, honouring the timer. 0 once the single shared window has closed. */
	int32 GetLiveStackCount() const;

	/** Seconds left on the ONE window covering the whole stack. 0 when there is no boost. */
	float GetStackSecondsRemaining() const;

	/** The live ripple, or null. Server-side truth; a client sees the replicated actor instead. */
	ATraceRippleActor* GetActiveRipple() const { return ActiveRipple.Get(); }

	/** True while this airtime's extra jump is still available. */
	bool IsSecondJumpAvailable() const { return !bSecondJumpUsed; }

	/** The path a Ripple fired RIGHT NOW would take. Pure; used by the harness and by ActivateAbility. */
	bool ComputeRipplePath(FVector& OutStart, FVector& OutDirection, float& OutLength) const;

private:
	/** Server-side handle on the ripple this Rocco owns. Exactly one at a time. */
	TWeakObjectPtr<ATraceRippleActor> ActiveRipple;

	/**
	 * One extra jump per airtime. NOT in the replicated net state on purpose: the jump is decided on
	 * the machine that owns the input and must be answered in the same frame it is pressed, so a
	 * value that arrives a round trip later would be the wrong answer. Every machine keeps its own
	 * copy and every machine clears it on the same event (touching the ground), so they agree.
	 */
	bool bSecondJumpUsed = false;

	/** Destroys the live ripple, if any. Idempotent. */
	void DestroyActiveRipple();
};
