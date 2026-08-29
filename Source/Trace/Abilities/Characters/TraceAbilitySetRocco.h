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

	// --- the §1.2 client FX router ----------------------------------------------------------------
	//
	// ROCCO'S ONLY ROUTED ELEMENT IS THE STACK TELL, and the other two §2.9 loops are deliberately
	// somewhere else:
	//
	//   the RIDE FX and the RIDE LOOP live on ATraceRippleActor, because the rider may be any of the
	//   ten characters and this state pad belongs to exactly one of them. That file's header carries
	//   the full argument; it is not an optimisation, it is the only correct home for them.
	//
	//   the SECOND JUMP's ring is a one-shot, not a loop, so it is an ATraceFxBurst fired from the
	//   server at the accept site — the burst actor's replication IS the multicast and a router edge
	//   would be a second, later copy of the same beat.
	//
	// What IS here is the accent lift, which is a while-active presentation of a replicated number
	// (Stacks) and therefore exactly what §1.2 was written for.
	virtual void OnClientStateEdge(const FTraceAbilityNetState& Old, const FTraceAbilityNetState& New) override;
	virtual void SyncClientFx(const FTraceAbilityNetState& Current) override;

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

	/**
	 * FX_AUDIO_PLAN §2.9's stack tell, as a number: what his body's accent Glow is being multiplied
	 * by right now. 1.0 = no lift. Pure, and safe on any machine — it reads the replicated stack.
	 *
	 * §2.9: "body accent-stripe Glow scales 1.7 + 0.35/stack, cap 3.0, while stacks > 0". 1.7 is
	 * ART_BIBLE §4.5's body-accent tier, so the lift tops out at a little under 1.8x and a full stack
	 * of ten looks the same as a stack of four — which is correct, because the tell says "he is
	 * boosted", not "he is boosted by exactly seven".
	 */
	float GetAccentGlowMultiplier() const;

	/** True while this set is holding the accent lifted on its pawn. For the harness. */
	bool IsAccentLifted() const { return bAccentLifted; }

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

	// --- FX_AUDIO_PLAN §2.9's stack tell ----------------------------------------------------------
	//
	// *** WHY THIS IS A POLLED WRITE AND NOT A ONE-SHOT ON THE EDGE. ***
	//
	// The lift is a scalar ("AccentGlow") on the pawn's body MIDs, and those MIDs are STOMPED by
	// ATraceCharacter::ApplyColorToSkeletalMesh on every team change, carrier change, death and
	// visual poll — that function writes AccentGlow = EmissivePower unconditionally, because the
	// accent is a state read (8 normal / 30 carrier / 0 dead) and not a per-effect knob.
	//
	// So an edge-only write would survive exactly until Rocco picked up the Core, and the tell would
	// silently vanish for the rest of the boost. MASTER_PLAN's risk 4 names this class of failure and
	// names the mitigation: kit accent lifts are STOMP-REFRESH-TOLERANT, and the restore goes back
	// through ApplyTeamColors() rather than through a remembered number.
	//
	// ApplyStackAccentTell therefore re-asserts the lift from TickAbilities (20 Hz, every machine)
	// while the stack is up, LATCHING whatever base value it finds so a stomp mid-boost simply moves
	// the base rather than fighting the lift. ClearStackAccentTell calls ApplyTeamColors() once, so
	// the pawn returns to the canonical value the stomp would have given it and this file never has
	// to know what EmissiveNormal, EmissiveCarrier or EmissiveDead are.

	/** Re-asserts the lift on the pawn's body MIDs. Cheap and idempotent; safe when there is no lift. */
	void ApplyStackAccentTell();

	/** Puts the accent back through ATraceCharacter::ApplyTeamColors(). Idempotent. */
	void ClearStackAccentTell();

	/** True while this set has the accent lifted, so the restore runs exactly once. */
	bool bAccentLifted = false;

	/** The stomp base last observed on the body MIDs, and the value last written over it. */
	float AccentBaseGlow = 0.f;
	float LastWrittenAccentGlow = -1.f;
};
