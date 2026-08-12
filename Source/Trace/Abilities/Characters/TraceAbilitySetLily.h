// Trace — LILY (spec v19 §3, Demo 18).
//
// ===================================================================================================
// THE DOC, VERBATIM
// ===================================================================================================
//
//   MOVEMENT  "an extra dash" — 2 normally, 3 while carrying the Core — and "she has only 60 health"
//
//   PASSIVE   "+30% wall-jump momentum bonus"  (hers alone; the global wall-jump numbers must not move)
//
//   ACTIVATED "Zip (30 s): for 5 s she can fly. Jump goes up at walking speed, slide/crouch goes down,
//             all other movement mechanics apply as usual. With the core the duration is halved."
//             And, separately: "If she activates it and then picks up the core, the remaining duration
//             is halved."
//
// ===================================================================================================
// THE SUBTLE CLAUSE, AND THE MISREADING IT IS GUARDING AGAINST
// ===================================================================================================
//
// The two Core sentences are the SAME 0.5 applied at two different moments, and the second one is the
// one that is easy to get wrong:
//
//     cast while carrying          5 s  ->  2.5 s     (halve the DURATION)
//     Core arrives 4 s into a Zip  1 s  ->  0.5 s     (halve WHAT IS LEFT)
//
// The wrong implementation — "on pickup, clamp the remaining time to the carrier duration" — gives
// 2.5 s in the second case, i.e. it LENGTHENS a Zip that was nearly over, and rewards picking the
// Core up late. Trace.Lily.ZipVerify stages exactly that case (1 s left, then a pickup) and asserts
// 0.5, because 2.5 is the answer the natural mistake produces and an assertion that cannot tell them
// apart is not an assertion.
//
// IT APPLIES ONCE PER PICKUP, not once per Zip. Drop the Core and take it again and what is left
// halves again. That is the literal reading, and it is the one that cannot be farmed by juggling.
//
// ===================================================================================================
// WHAT IS LIVE IN THIS FILE, AND WHAT IS WAITING ON SOMEBODY ELSE'S ONE-LINER
// ===================================================================================================
//
// *** LIVE, END TO END: ZIP. *** The flight, both halvings, the jump/crouch controls and the death
// stop are all in this file and run today. Trace.Lily.ZipVerify proves the halvings red-arm first.
//
// *** NOT LIVE: ALL THREE OF THE OTHERS. *** The extra dash charge, the 60 health and the wall-jump
// bonus each change a number owned by a slice this pass does not own:
//
//     the charge pool   Movement/TraceCharacterMovementComponent.cpp  (GetMaxDashCharges)
//     max health        Gameplay/TraceHealthComponent.cpp             (GetMaxHealth)
//     the wall jump     Movement/TraceCharacterMovementComponent.cpp  (TryWallJump)
//
// They are implemented here, exposed through TraceAbilityTraits (Abilities/TraceAbilityTypes.h), and
// need one call each. UNTIL THOSE CALLS EXIST LILY HAS 100 HEALTH, ONE DASH AND AN ORDINARY WALL
// JUMP. That is in the report, not buried here.
//
// ===================================================================================================
// HOW ZIP MOVES HER, AND THE LIMIT OF DOING IT FROM AN ABILITY SET
// ===================================================================================================
//
// It writes UCharacterMovementComponent::Velocity.Z from TickAbilities(), on the server and on the
// owning client, from state both machines compute the same way — the pattern Mace's suspend
// established and for the identical reasons (GravityScale is re-pushed from UTraceSettings on every
// simulated move, and FSavedMove_Trace lives in a slice this file does not own).
//
// *** TickAbilities RUNS AT 20 Hz AND PHYSICS RUNS AT 60+. *** So the vertical command is applied in
// ~50 ms lumps into a simulation that integrates gravity continuously, and a naive "set Z to 0 to
// hover" sags at about g x dt / 2 = 24.5 uu/s, i.e. roughly 120 uu over a full 5 s Zip. That is not
// acceptable for something the doc calls flying, so ApplyZip adds the closed-form correction for
// exactly one tick of gravity (see the comment there). It makes the MEAN vertical velocity the
// commanded one; it does not make the flight frame-accurate, and the honest fix is a movement mode in
// the prediction path. That is named in the report as a limitation, not hidden as a detail.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "UObject/ObjectMacros.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetLily.generated.h"

/** Lily's bits in FTraceAbilityNetState::Flags. Only one character is live per component, so these cannot collide. */
namespace TraceLilyFlags
{
	/**
	 * Zip is up. DELIBERATELY TraceAbilityFlags::EffectActive.
	 *
	 * Spec v19 §4.2 — "death wipes active abilities" — is enforced centrally by
	 * UTraceAbilityComponent::ApplyDeathStateWipe, which clears exactly EffectActive, MovementActive
	 * and EffectEndMatchTime. Choosing that bit and that field is what makes Zip stop on death for
	 * free, on every machine, without this file being trusted to remember.
	 */
	inline constexpr uint8 Zipping = 1 << 0;   // == TraceAbilityFlags::EffectActive
}

UCLASS()
class TRACE_API UTraceAbilitySetLily : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Lily; }

	// --- lifecycle -------------------------------------------------------------------------------
	virtual void OnEquipped() override;
	virtual void OnUnequipped() override;
	virtual void OnPawnDied() override;
	virtual void OnHalfTime() override;
	virtual void TickAbilities(float DeltaSeconds) override;

	// =============================================================================================
	// MOVEMENT + PASSIVE — all three read through TraceAbilityTraits, never by casting
	// =============================================================================================

	/** §3: "an extra dash". +1 on top of everybody's pool, from UTraceSettings::LilyExtraDashCharges. */
	int32 GetExtraDashCharges() const;

	/** §3: "only 60 health". An ABSOLUTE, from UTraceSettings::LilyMaxHealth. 0 would mean "no opinion". */
	float GetMaxHealthOverride() const;

	/**
	 * §3: "+30% wall-jump momentum bonus". 1.30, from UTraceSettings::LilyWallJumpMomentumBonus.
	 *
	 * It multiplies the RETENTION term — the part of the launch that is the speed she arrived with —
	 * and never the flat outward impulse or the vertical multiplier. See the knob's comment.
	 */
	float GetWallJumpMomentumScale() const;

	// =============================================================================================
	// ACTIVATED — ZIP
	// =============================================================================================

	virtual bool CanActivate(FText& OutReason) const override;
	virtual bool ActivateAbility() override;

	/** §3: "Zip (30 s)". UTraceSettings::LilyZipCooldownSeconds. */
	virtual float GetActivatedCooldownSeconds() const override;

	/**
	 * Jump while flying is CLIMB, not a jump. Consumed (returns true) so the ordinary jump — and the
	 * wall jump, and the slide-jump grace — never runs mid-Zip; §3's "jump goes up at walking speed"
	 * is a different verb on the same key.
	 */
	virtual bool OnJumpPressed() override;
	virtual void OnJumpReleased() override;

	// --- readouts, for the HUD, the bots and the harness ------------------------------------------

	/** True while she is flying. Correct on every machine — it reads the replicated flag. */
	bool IsZipping() const;

	/** Seconds of flight left, 0 when not flying. */
	float GetZipRemaining() const;

	/** What a Zip started RIGHT NOW would last: the full duration, halved if she is carrying. */
	float GetZipDurationForNow() const;

#if !UE_BUILD_SHIPPING
	/**
	 * DEV ONLY. Forces the remaining flight time, so a harness can stage "4 s in with 1 s left"
	 * without spending four seconds of real time on it.
	 *
	 * Exists for ONE caller: Trace.Lily.ZipVerify. The mid-Zip pickup clause is a claim about what
	 * happens to a PARTIALLY SPENT Zip, and the natural wrong implementation ("clamp to 2.5 s") is
	 * indistinguishable from the right one until the remaining time is below 2.5 s. So the harness has
	 * to be able to get there, and waiting is not a test, it is a delay.
	 */
	void DebugSetZipRemaining(float Seconds);
#endif

private:
	/** Begins the flight. Duration is halved here if she is ALREADY carrying (§3's first clause). */
	void StartZip();

	/** Ends it, for any reason. Never touches the E cooldown — the framework owns that. */
	void StopZip(const TCHAR* Why);

	/** One tick of flight: the guards, then the single Velocity.Z write. */
	void ApplyZip(float DeltaSeconds);

	/** §3's SECOND clause: the Core has just arrived mid-flight, so halve WHAT IS LEFT. */
	void HalveRemainingForCorePickup();

	/** Server: push bZipping / ZipEndMatchTime into the replicated scratch pad. */
	void PublishState();

	/** Server or owning client. A simulated proxy's velocity is replicated and must not be written. */
	bool ShouldDriveMovement() const;

	/** True while she is flying, from THIS machine's own state rather than the replicated mirror. */
	bool bZipping = false;

	/** Absolute match-clock time the flight ends. Never a countdown — see the cooldown contract. */
	float ZipEndMatchTime = 0.f;

	/** Jump held, for the climb. Press and release both arrive; a lost release must not fly her forever. */
	bool bJumpHeld = false;

	/** Last carrier reading, so a PICKUP is an edge and not a level. Halving on a level would halve every tick. */
	bool bWasCarrier = false;

	/** Mid-Zip pickups this flight has seen. Instrumentation the harness reads; not replicated. */
	int32 ZipCorePickups = 0;
};
