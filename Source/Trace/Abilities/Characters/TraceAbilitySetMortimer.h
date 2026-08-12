// Trace — MORTIMER (spec v19 §3, Demo 18).
//
// ===================================================================================================
// THE DOC, VERBATIM
// ===================================================================================================
//
//   PASSIVE   "his dash is 75% shorter" and "he can charge the core up to 2x as long as anyone else,
//             on the same linear scale, so he throws it twice as far"
//
//   MOVEMENT  "he can mantle onto objects, 30% more generous than the old in-game mantle"
//
//   ACTIVATED "only while carrying the core AND standing on the ground or the top of an object, a
//             blast that knocks nearby enemies away"
//
// ===================================================================================================
// WHAT IS LIVE IN THIS FILE, AND WHAT IS A KNOB WAITING FOR SOMEBODY ELSE'S ONE-LINER
// ===================================================================================================
//
// *** LIVE, END TO END: QUAKE. *** The whole activated ability — the posture gate, the victim search,
// the choke point and the launch — is in this file and runs today. Trace.Mortimer.BlastCarrierTest
// proves it, red arm first.
//
// *** NOT LIVE: THE THREE PASSIVE HALVES. *** Every one of them changes a number owned by a slice
// this pass does not own:
//
//     the dash's reach     Movement/TraceCharacterMovementComponent.cpp  (GetDashSpeed)
//     the mantle           Movement/TraceCharacterMovementComponent.cpp  (deleted in d2319b2)
//     the Core throw cap   Gameplay/TraceCore.cpp                        (GetThrowChargeScaleForHold)
//
// They are IMPLEMENTED here, exposed through TraceAbilityTraits (Abilities/TraceAbilityTypes.h), and
// each needs exactly one call in one of those files. Until those calls exist, Mortimer dashes and
// throws like everybody else and cannot mantle. THAT IS SAID OUT LOUD IN THE REPORT rather than left
// to be discovered: a passive that is written, tested in isolation and never called is this project's
// single most repeated failure (see the TraceAbilityIntegration note in TraceAbilityComponent.h,
// which exists because five characters shipped exactly that way).
//
// ===================================================================================================
// THE MANTLE, AND WHY IT CANNOT COME BACK FOR EVERYBODY
// ===================================================================================================
//
// It was added in `dffea7c` (Demo 5) and DELETED in `d2319b2` (Demo 11). The deletion commit is not a
// tidy-up, it is a measurement: the "rubber banding on the edge of a raised section" the mantle was
// layered over turned out to be a genuine client prediction desync, and removing the mantle both
// forced that fix and proved it on a joined client at 40 ms —
//
//     shipped: 5/5 contacts, 0.00 corrections per contact, worst error 0.00 uu
//     legacy:  1.00 corrections per contact, worst error 88.11 uu, speed kept as low as 0.521
//
// The mantle itself was ALSO broken when it was written (0/8 successful mantles, because the ledge
// probes hit the probing pawn — one AddIgnoredActor took it to 7/8) and the fixed version is what
// `git show dffea7c` restores.
//
// SO THE RECOVERY IS GATED, NOT GLOBAL. TraceAbilityTraits::IsMantleAllowed() is false for every
// character but Mortimer and is meant to be the FIRST question CanAttemptMantle() asks, so for the
// other nine there is no probe, no MOVE_Flying, no pull-up, and therefore no new way for a client and
// a server to disagree about a ledge. "Do not bring the ledge bug back for everyone" becomes a
// property of the control flow rather than a promise.
//
// ===================================================================================================
// QUAKE AND THE CARRIER CHOKE POINT — THE IRONY, STATED PRECISELY
// ===================================================================================================
//
// A knockback is a Control effect, so every victim goes through
// UTraceAbilityComponent::CanAffectTarget(Victim, Control) and there is NO carrier test in this file.
//
// The irony the spec flags: MORTIMER IS HIMSELF THE CARRIER WHEN HE CASTS IT. That is fine and it is
// the reason the choke point is asked about the TARGET and never about the instigator — a rule that
// asked "is the caster carrying?" would refuse this ability outright, and a rule that skipped the
// check "because he is the carrier anyway" would be the bug.
//
// AND THE TEST THAT MATTERS IS THE ONE THAT LOOKS VACUOUS. With one Core in play, an enemy carrier
// cannot exist while Mortimer is carrying, so in a real match the choke point can never fire for
// Quake. That is exactly why the harness calls ApplyBlastTo() DIRECTLY on a live carrier instead of
// waiting for a situation the game cannot produce: what is being proved is that the code path is
// routed, so that it stays routed on the day a second Core, a practice range, or a dropped-Core rule
// makes the situation reachable. A rule that is only correct because the situation never arises is
// not a rule, it is a coincidence.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "UObject/ObjectMacros.h"

#include "Abilities/TraceCharacterAbilitySet.h"

#include "TraceAbilitySetMortimer.generated.h"

class ATraceCharacter;

/**
 * Why Quake refused. Returned by CheckBlastPosture() so the log, the HUD toast and the harness can
 * all say the same sentence, and so "it did nothing" is never the whole story a player gets.
 */
UENUM()
enum class ETraceMortimerBlastRefusal : uint8
{
	/** It would fire. */
	Allowed = 0,
	/** No pawn, dead, or no movement component. */
	NoPawn,
	/** "only while carrying the core" — he is not. */
	NotCarryingCore,
	/** "standing on the ground or the top of an object" — he is in the air. */
	Airborne
};

TRACE_API const TCHAR* TraceMortimerBlastRefusalToString(ETraceMortimerBlastRefusal Reason);

UCLASS()
class TRACE_API UTraceAbilitySetMortimer : public UTraceCharacterAbilitySet
{
	GENERATED_BODY()

public:
	virtual ETraceCharacterId GetCharacterId() const override { return ETraceCharacterId::Mortimer; }

	// =============================================================================================
	// PASSIVE — the two halves. Both are read through TraceAbilityTraits, never by casting.
	// =============================================================================================

	/**
	 * §3: "his dash is 75% shorter". 0.25 of everybody's dash REACH, from
	 * UTraceSettings::MortimerDashDistanceScale.
	 *
	 * It scales the dash's SPEED so that its DURATION — and therefore the trace it leaves, the parry
	 * window and the dash-hit sweep — is untouched. See the knob's comment for why that matters.
	 */
	float GetDashDistanceScale() const;

	/**
	 * §3: "up to 2x as long ... on the same linear scale". 2.0, from
	 * UTraceSettings::MortimerThrowChargeHoldScale.
	 *
	 * It multiplies the CAP on t = HeldSeconds / CoreThrowChargeSeconds inside
	 * ATraceCore::GetThrowChargeScaleForHold and nothing else, so the shipped line
	 * Power = Floor + (1 - Floor) x t is extrapolated rather than replaced.
	 */
	float GetThrowChargeHoldScale() const;

	// =============================================================================================
	// MOVEMENT — the mantle, recovered from dffea7c and gated to him alone
	// =============================================================================================

	/** True unless UTraceSettings::bMortimerCanMantle has been switched off. See the header. */
	bool AllowsMantle() const;

	/** §3: "30% more generous". 1.30, from UTraceSettings::MortimerMantleGenerosity. */
	float GetMantleGenerosityScale() const;

	// =============================================================================================
	// ACTIVATED — QUAKE
	// =============================================================================================

	/** §3's two conditions, as one question, with the reason. Pure; safe on any machine. */
	ETraceMortimerBlastRefusal CheckBlastPosture() const;

	/** The framework's pre-flight. Wraps CheckBlastPosture() and phrases it for the player. */
	virtual bool CanActivate(FText& OutReason) const override;

	/**
	 * Fire Quake. Returns true — and therefore charges the cooldown — only when the posture held.
	 *
	 * On the SERVER it finds and launches every victim. On the OWNING CLIENT it does nothing but
	 * agree that the press was legal: a knockback is somebody else's position and predicting it would
	 * show this player enemies flying who never moved.
	 */
	virtual bool ActivateAbility() override;

	/** §3 gives no number. [ASSUMPTION] UTraceSettings::MortimerBlastCooldownSeconds (20 s). */
	virtual float GetActivatedCooldownSeconds() const override;

	// =============================================================================================
	// The blast's two halves, public because the harness drives them directly. See the header.
	// =============================================================================================

	/**
	 * SERVER ONLY. Find every enemy inside the radius and launch them. Returns how many were moved.
	 *
	 * @param OutConsidered  how many living pawns were inside the radius at all, victims or not. The
	 *                       difference between this and the return value is what the choke point,
	 *                       friendly fire and line of sight refused, and a harness that cannot see it
	 *                       cannot tell "nobody was near" from "the rule fired".
	 */
	int32 RunBlast(int32& OutConsidered);

	/**
	 * SERVER ONLY. THE PER-VICTIM PATH, CHOKE POINT INCLUDED. One call, one victim, no radius test —
	 * so a harness can aim it at a Core carrier that a real match could never produce beside him.
	 *
	 * @return true only if the victim was actually launched.
	 */
	bool ApplyBlastTo(ATraceCharacter* Victim) const;

	/** Quakes this ability set has fired. Dev instrumentation; a harness reads it to prove a press landed. */
	int32 GetBlastCount() const { return BlastCount; }

private:
	/** Nothing but the launch: direction, falloff and LaunchCharacter. Assumes every rule has passed. */
	void LaunchVictim(ATraceCharacter* Victim, const FVector& FromLocation) const;

	/** bMortimerBlastNeedsLineOfSight's trace. True when the knob is off. */
	bool HasLineOfSightTo(const ATraceCharacter* Victim) const;

	/** Quakes fired by this set since it was equipped. Not replicated; server and local client each count their own. */
	int32 BlastCount = 0;
};
