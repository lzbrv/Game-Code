// Trace — Oyster's poison, and the per-vector alarm that proves it never touches a Core carrier.
//
// ===================================================================================================
// SPEC v14 §6, OYSTER'S PASSIVE, VERBATIM
// ===================================================================================================
//
//   "An enemy touching a jar breaks it, poisoning nearby enemies: 3 damage every 0.5 s for 4 s, and
//    -30% speed for 4 s."
//
// TWO DIFFERENT EFFECT CLASSES IN ONE SENTENCE, and that is the whole reason this is a component with
// a tick rather than a one-shot burst of damage:
//
//   the 3-per-0.5 s is ETraceAbilityEffect::Damage   -> NEVER reaches a Core carrier. No knob.
//   the -30% speed  is ETraceAbilityEffect::Control  -> does not reach one either, under the §4
//                                                       [ASSUMPTION] (bCarrierImmuneToAbilityControl).
//
// ===================================================================================================
// WHY THE CHOKE POINT IS ASKED ON EVERY SINGLE TICK, NOT ONCE AT APPLICATION
// ===================================================================================================
//
// Spec §4 names "Oyster's poison ticks" as a specific way the founding invariant can break, and the
// interesting case is not "poison a carrier" — that is refused at the burst. It is:
//
//     an ordinary player is poisoned, and one second later they PICK UP THE CORE.
//
// A poison that decided it was allowed when it landed would keep ticking 3 damage into a carrier for
// three more seconds and would keep them slowed for the rest of the fight. So the answer is not
// cached: TickPoison() re-asks UTraceAbilityComponent for BOTH effect classes every tick, and the
// slow is re-decided every frame. Becoming the carrier stops both within a frame; dropping the Core
// resumes them for whatever is left of the 4 s. Trace.Oyster.CarrierTest exercises exactly that
// transition, because it is the case a "check it once" implementation passes every other test with.
//
// ===================================================================================================
// THE SLOW IS APPLIED BY CLAMPING VELOCITY, AND THAT IS A COMPROMISE
// ===================================================================================================
//
// The framework's intended path for a speed change is UTraceCharacterAbilitySet::GetMoveSpeedMultiplier
// -> UTraceAbilityComponent::GetMoveSpeedMultiplierFor -> the movement component. It cannot carry a
// poison slow for two independent reasons:
//   1. NOTHING CALLS IT. Movement/TraceCharacterMovementComponent.cpp has no such call yet; it is a
//      cross-file need both foundation reports already filed, and this slice does not own that file.
//   2. Even when it does, that hook asks the VICTIM'S OWN character. The victim is usually a
//      Mannequin (ETraceCharacterId::None) or somebody else's character, and neither has any reason
//      to know about Oyster's poison. A debuff another player applied is not a passive of the victim.
//
// So the slow is a per-frame clamp on planar velocity, run on the server AND on the owning client
// (this component replicates its two fields precisely so the owner can clamp too, which is what keeps
// the correction path quiet). The proper fix is an external debuff multiplier inside
// UTraceCharacterMovementComponent::GetMaxSpeed(); it is named in the report as a cross-file need.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/ObjectMacros.h"

#include "TraceOysterPoison.generated.h"

class ATraceCharacter;
class UTraceAbilityComponent;

/**
 * THE ALARM, per effect vector.
 *
 * The framework's TraceAbility::GetCarrierAbilityDamageHitCount() counts ability DAMAGE that reached
 * a carrier. Oyster has four distinct ways to touch a player and only one of them is damage, so a
 * single damage counter would have said "clean" while Pickler quietly yanked a carrier across the
 * map. Each vector is counted separately, and counted for carriers and non-carriers alike:
 *
 *   Carrier   must be ALL ZERO for the life of a correct process.
 *   Other     is the fixture proving itself — a run in which nothing landed on anybody proves
 *             nothing about carriers, it proves the harness never fired.
 *
 * Counted at the point the effect is APPLIED (the decision), not at the point it is observed, so a
 * pull that launches a stationary bot nowhere still counts. What is under test is whether the rule
 * let the effect happen.
 */
namespace TraceOyster
{
	struct FEffectTally
	{
		/** A 3-damage poison tick that actually moved health. */
		int32 PoisonTicks = 0;
		/** A poison burst that actually attached (or refreshed) poison on a target. */
		int32 PoisonApplications = 0;
		/** A frame on which the -30% slow was decided to be in force for a target. Server only. */
		int32 SlowFrames = 0;
		/** A Pickler impact that actually moved health. */
		int32 PicklerDamageHits = 0;
		/** A Pickler pull that actually launched a target. */
		int32 PicklerPulls = 0;

		int32 Total() const { return PoisonTicks + PoisonApplications + SlowFrames + PicklerDamageHits + PicklerPulls; }
		void Reset() { *this = FEffectTally(); }
	};

	/** Effects that landed on a player holding the Core. MUST STAY ALL ZERO. */
	TRACE_API FEffectTally& CarrierTally();

	/** Effects that landed on anybody else. The fixture's own proof of life. */
	TRACE_API FEffectTally& OtherTally();

	TRACE_API void ResetTallies();

	/** Files @p Landed under the right tally and logs an Error if the victim was the carrier. */
	TRACE_API void RecordEffect(const ATraceCharacter* Target, const TCHAR* VectorName, int32 FEffectTally::* Field);
}

/**
 * One target's poison. Server-authoritative; replicated so the owning client clamps its own speed.
 *
 * Created on the victim's pawn by ATraceOysterJar's burst and by nothing else. Removes itself when
 * the 4 s runs out, and dies with the pawn on a respawn — which is correct: poison is a property of
 * a body, not of a player.
 */
UCLASS(ClassGroup = (Trace))
class TRACE_API UTraceOysterPoisonComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTraceOysterPoisonComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * SERVER ONLY. Poisons @p Target on behalf of @p SourceComp, or refreshes an existing poison.
	 *
	 * Does NOT check the choke point itself — the caller must already have, because whether to attach
	 * at all is the caller's decision (a carrier is never poisoned in the first place). Every tick
	 * AFTER this re-checks, which is where the "picked the Core up mid-poison" case is caught.
	 *
	 * @return the live component, or null when nothing could be attached.
	 */
	static UTraceOysterPoisonComponent* ApplyTo(ATraceCharacter* Target, UTraceAbilityComponent* SourceComp);

	/** The live poison on @p Target, or null. */
	static UTraceOysterPoisonComponent* Find(const ATraceCharacter* Target);

	/** Absolute match time the poison ends. Replicated. */
	float GetEndMatchTime() const { return EndMatchTime; }

	/** True when the -30% slow is IN FORCE this frame — i.e. the choke point last said yes. Replicated. */
	bool IsSlowActive() const { return bSlowActive; }

	/** Total damage this poison has actually dealt. Server only; the harness reads it. */
	float GetDamageDealtSoFar() const { return DamageDealtSoFar; }

	/**
	 * SPEC v14 §6 — THE ONE DEFINITION OF "HOW MUCH SLOWER", read by two callers.
	 *
	 * UTraceCharacterMovementComponent::GetMaxSpeed() multiplies by it (through
	 * TraceAbilityDebuff::GetMoveSpeedMultiplier) so that ACCELERATION targets the slowed speed
	 * rather than only being clipped after the fact, and ApplySlowClamp() enforces the same ceiling
	 * on the frames the acceleration model would otherwise overshoot it.
	 *
	 * Because both read this, and because ApplySlowClamp now takes its limit from GetMaxSpeed()
	 * (which already includes this factor), the -30% is applied exactly ONCE. It was 1.0 - fraction
	 * before this integration and it is 1.0 - fraction now; what changed is where it is spelled.
	 *
	 * 1.0 whenever the slow is not in force — which includes every frame the §4 choke point refused,
	 * because bSlowActive is that answer.
	 */
	float GetSpeedMultiplier() const;

private:
	/** REPLICATED. Absolute match-clock time, never a duration — the framework's rule, everywhere. */
	UPROPERTY(Replicated)
	float EndMatchTime = 0.f;

	/**
	 * REPLICATED. Re-decided by the server every tick from the choke point, so the owning client
	 * clamps its own speed on exactly the frames the server does and the correction path stays quiet.
	 */
	UPROPERTY(Replicated)
	bool bSlowActive = false;

	/** Absolute match time of the next 3-damage tick. */
	float NextTickMatchTime = 0.f;

	float DamageDealtSoFar = 0.f;

	/** Who poisoned them. Weak: Oyster may die, leave, or change character mid-poison. */
	TWeakObjectPtr<UTraceAbilityComponent> SourceComponent;

	ATraceCharacter* GetVictim() const;
	float MatchTimeNow() const;

	/** Server. Damage ticks + the per-frame slow decision. */
	void TickAuthority();

	/** Every machine that simulates the victim. The clamp itself. */
	void ApplySlowClamp();
};
