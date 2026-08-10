// Copyright (c) Trace. All Rights Reserved.

#include "AI/TraceBotController.h"

#include "CollisionQueryParams.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"                 // TActorIterator
#include "Abilities/Characters/TraceSlimewall.h"   // v18: slime blocks bot vision (NoCollision by design)
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Math/NumericLimits.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Math/UnrealMathUtility.h"

#include "Abilities/TraceAbilityComponent.h"                  // spec v15 §3: the bots' ability path
#include "Abilities/Characters/TraceAbilityInputRelay.h"      // ...and the E router (Mace's reactivation)
#include "Abilities/Characters/TraceAbilitySetElle.h"         // ...and IsAwaitingSecondGate, for the same reason
#include "Abilities/Characters/TraceAbilitySetMace.h"         // ...and IsSpikeReadyToPull, to tell a pull from a throw
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceEndzone.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Settings/TraceUserSettings.h"   // GetLookScaleX (the live human look scale)
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "World/TraceArenaBuilder.h"

namespace TraceBotConstants
{
	/**
	 * Fallback field extents, used only if ATraceArenaBuilder has not resolved yet.
	 *
	 * Every real distance in this file is a fraction of the LIVE bounds; these exist so that a bot
	 * which ticks one frame before the builder is findable steers somewhere sane instead of
	 * dividing by zero. They are deliberately the ORIGINAL 8000 x 4000 pitch, so the degenerate
	 * case behaves like the smallest arena we have ever shipped rather than like the largest.
	 */
	static constexpr float FallbackHalfLength = 4000.f;
	static constexpr float FallbackHalfWidth = 2000.f;

	/** Strength of the wall push relative to the desired direction (both unit vectors). */
	static constexpr float WallAvoidWeight = 2.2f;

	/** Planar speed below which a bot that WANTS to move is considered stuck. */
	static constexpr float StuckSpeedThreshold = 90.f;

	/** Seconds of being stuck before the sideways evade kick fires. */
	static constexpr float StuckTriggerSeconds = 0.7f;

	/** How long an evade kick steers sideways before normal steering resumes. */
	static constexpr float EvadeDuration = 0.6f;

	/** Second LOS probe, low on the body. See ATraceBotController::HasLineOfSight. */
	static constexpr float TargetLowOffsetZ = -55.f;

	/** Seconds between re-rolls of the slow aim wobble. */
	static constexpr float AimErrorRefreshMin = 0.25f;
	static constexpr float AimErrorRefreshMax = 0.65f;

	/** Duelling: how far off the preferred range a bot tolerates before closing or backing off. */
	static constexpr float RangeDeadzoneFraction = 0.35f;

	/** Seconds between strafe-direction flips while duelling. */
	static constexpr float StrafeFlipMin = 0.9f;
	static constexpr float StrafeFlipMax = 2.1f;

	/** Inside this radius of its station, an escort stops shuffling and holds. */
	static constexpr float StationHoldRadius = 260.f;

	// --- MODE B (spec v4 §7) ----------------------------------------------------------------------

	/**
	 * How many bots per team contest a loose Core; the rest fall back and cover their own goal.
	 *
	 * Two is the smallest number that still makes the chase a contest rather than a foot race, and it
	 * leaves three of a five-man side goal-side of the ball. Sending everybody was the first version
	 * and it produced exactly what you would expect: both teams stacked on one point and an
	 * unguarded goal mouth for whoever came out of the pile with it.
	 *
	 * 2 -> 3 FOR SPEC v6. Two things changed at once and both push the same way. The catch zone
	 * (section 4.1) means a contesting bot no longer has to reach the Core, only the flight path, so a
	 * third chaser is now a real chance of an interception rather than a third body arriving late.
	 * And the ground turnover (section 4.2) means a throw that nobody contests is not a 50/50 scramble
	 * any more - it is decided by where it lands - so the moment to influence it is while it is still
	 * in the air. Three of five still leaves two goal-side.
	 */
	static constexpr int32 LooseCoreChasers = 3;

	/**
	 * How many bots per team hold a station in front of their own goal. The rest play normally.
	 *
	 * MEASURED: without a cap, "an enemy has the Core and I cannot reach them" caught nearly the
	 * whole side — 12358 goal-defence bot-ticks against 877 spent chasing over a 60 s sample on a
	 * 33600 uu field, because on a pitch that long most players are out of carrier range most of the
	 * time. A team standing in its own goal is not defending, it is absent.
	 */
	static constexpr int32 GoalDefenders = 2;

	/**
	 * Fraction of the way from our own goal toward the threat that a defender stands.
	 *
	 * Not ON the goal line: a defender standing in the mouth can be walked around, and the goal is
	 * finite in height, so the useful position is a little in front of it where the throwing lane is
	 * narrowest.
	 */
	static constexpr float GoalDefenceStandoffFraction = 0.22f;

	/**
	 * Inside this angle of the goal centre, a carrying bot will consider the shot at all.
	 *
	 * WIDENED from 35 degrees, and measured against the CONTROL rotation rather than the pawn's
	 * forward vector, which is what the old test used. Those two are different things while carrying:
	 * mouse1 is the throw, the bot is not aiming at anybody, and its capsule yaw follows where it is
	 * running - i.e. a weaving carrier's actor-forward swings ±25 degrees off the goal line by
	 * design (see the weave in BehaviourCarryToGoal). The control rotation is the thing that has to
	 * slew onto the target, it slews at AimTurnRateDegrees, and BotPassMaxLineUpSeconds gives it over
	 * a second to do it - so 75 degrees is comfortably inside what the line-up can actually deliver.
	 */
	static constexpr float ThrowAtGoalConeDegrees = 75.f;

	// --- MODE B, spec v5 §4: the ballistic shot and the carry-in ------------------------------------

	/**
	 * Safety margin on the Core's maximum ballistic range. A bot will not attempt a shot beyond this
	 * fraction of what the throw can physically carry.
	 *
	 * Not 1.0 because the launch pitch at maximum range is the single pitch that just barely reaches,
	 * and the bots throw through an aim slew with error on top; a throw solved at 99% of maximum
	 * range lands short of the mouth on any error at all. At 0.85 there is a real margin either side.
	 */
	static constexpr float ThrowRangeSafetyFraction = 0.85f;

	// --- MODE B, spec v13 §6: the charge-up throw ---------------------------------------------------

	/**
	 * The weakest a bot will ever release a throw at, as a fraction of full momentum.
	 *
	 * The spec's warning is explicit: "a bot that always instant-clicks throws at 15% power and mode B
	 * will look broken." 0.45 is comfortably above the 0.15 floor a human tap produces, which means a
	 * bot's shortest pass still leaves the hand like a throw rather than a fumble - and it is the
	 * charge at which a throw covers about 20% of full range (range goes as the square of speed), which
	 * is 700-odd uu on this pitch and further than any pass a bot actually chooses.
	 */
	static constexpr float ThrowMinCharge = 0.45f;

	/**
	 * Added to the charge a throw strictly needs, so a bot does not release at exactly the power that
	 * just barely arrives.
	 *
	 * The same argument as ThrowRangeSafetyFraction, one level down: the bots throw through an aim slew
	 * with error on top, and since spec v6 §4.2 a throw that falls short lands on the floor and is
	 * handed to the nearest enemy. Over-throwing slightly is recoverable; under-throwing is a turnover.
	 */
	static constexpr float ThrowChargeMargin = 0.12f;

	/**
	 * SPEC v6 section 4.2. Minimum shot quality a bot will take at goal when it is NOT about to die.
	 *
	 * 0.08 on the 0..1 ShotScore scale, i.e. "no further out than about 92% of what the Core can
	 * physically carry to the hoop". The gate exists because the cost of missing changed: a thrown
	 * Core that hits the ground now turns over to the nearest enemy, and a shot released at the edge
	 * of range lands in front of the goal it was aimed at, which is the single worst place to hand a
	 * defender the Core. Under pressure it is waived - a shot thrown a moment before dying is still
	 * better than being killed holding it, because a kill hands the Core over just as surely.
	 *
	 * MEASURED, AND WHY IT IS 0.08 AND NOT MORE. The first value tried was 0.22, and it was wrong for
	 * a reason worth recording: spec v6 §4.3 moved the goal 2400 uu further away (into the back wall
	 * instead of on the goal line) AND 1100 uu up, and throwing at a target that high costs range -
	 * the Core's ballistic maximum falls from ~5150 uu flat to ~3300 uu at the hoop's height after
	 * the safety fraction. A verified shot from 2600 uu therefore scores only 0.15, and 0.22 refused
	 * it: the scenario's step 5 measured "a throw did NOT leave" against a bot that had a good shot.
	 * At 0.08 only the last 8% of a range that is already short is refused, which is the ragged edge
	 * this is meant to trim and not the shot itself.
	 */
	static constexpr float ThrowAtGoalMinScore = 0.08f;

	/**
	 * SPEC v6 section 4.2. A clearance is thrown at the most advanced TEAMMATE, at this fraction of
	 * the way to them, rather than at open ground.
	 *
	 * The clearance used to be aimed halfway to the goal at nothing at all, on the reasoning that a
	 * loose Core is contestable by both sides. That reasoning is dead: a loose Core that lands is not
	 * contestable, it is the enemy's. So a clearance now has to be aimed at somebody who could
	 * plausibly catch it - and with the §4.1 catch zone reaching 500 uu, "plausibly" is a much wider
	 * target than a pass rule would accept. Short of the receiver rather than at them, so the Core
	 * arrives in front of a runner instead of behind one.
	 */
	static constexpr float ClearanceLeadFraction = 0.85f;

	/** Samples along the arc for the lane sweep. Ten segments resolves this arena's cover comfortably. */
	static constexpr int32 ThrowLaneSamples = 10;

	/**
	 * How far from the goal MOUTH a carrier commits to running it in rather than throwing it.
	 *
	 * Deliberately shorter than the shot range: inside this the bot is close enough that the run is
	 * the higher-percentage play, and the throw machine is switched off so it cannot change its mind
	 * (see ATraceBotController::bCommitCarryIn for the failure that motivates the latch).
	 */
	static constexpr float CarryInCommitDistance = 4200.f;

	/** Hysteresis: the commit is only released once the bot is this much further out again. */
	static constexpr float CarryInReleaseDistance = 5400.f;

	/** A carry-in that has not scored in this long is being held out of the mouth. Go back to throwing. */
	static constexpr float CarryInPatienceSeconds = 9.f;

	/**
	 * Perpendicular distance under which "which side of the trace am I on" stops being answerable.
	 *
	 * A bot standing exactly on the trace line has no meaningful perpendicular, so it picks a side
	 * from its strafe handedness and crosses sideways rather than dashing along the wall, which
	 * would never trip anything.
	 */
	static constexpr float TrailPerpDegenerate = 40.f;

	/** Dot product between current heading and the crossing direction required to commit a dash. */
	static constexpr float TrailDashAlignmentDot = 0.35f;

	/** Downward trace length used to answer "how high am I" for the crouch fast-fall. */
	static constexpr float GroundProbeLength = 2000.f;

	// --- SPEC v9 §1: INTERCEPTING THE TRACE ---------------------------------------------------
	//
	// THE BUG THESE EXIST TO MAKE IMPOSSIBLE. Two filters trim the trace down to the stretch worth
	// dashing at: BotTrailMinDistanceFromCarrier (900uu) drops the newest end, and
	// BotTrailMinPointLifeRemaining x WalkSpeed (320uu) drops the oldest. Both were calibrated when
	// a trace was as long as TrailLifetime x WalkSpeed. Spec v7 §2 capped a trace at
	// TrailMaxLengthUU = 1200uu, and 900 + 320 = 1220 > 1200: the two filters MET IN THE MIDDLE and
	// the candidate set became empty for every carrier on the field, permanently. Every hunting bot
	// fell through to the "no usable trace" branch, which pursues the carrier — which is precisely
	// what the report describes ("they run at the carrier, following the exact motion of the
	// carrier and stand by it").
	//
	// So neither margin is allowed to be an absolute any more. Each is a FRACTION OF THE TRACE THAT
	// ACTUALLY EXISTS, with the settings value as an upper bound. 0.45 + 0.20 < 1, so a usable band
	// survives at any trace length, at any future value of TrailMaxLengthUU, and this class of bug
	// cannot come back by retuning a number in another file.
	static constexpr float TrailHeadGapMaxFraction  = 0.45f;
	static constexpr float TrailTailSkipMaxFraction = 0.20f;

	/**
	 * How much a bad crossing ANGLE costs a candidate segment, as a multiple of its travel time.
	 *
	 * Picking the NEAREST trace point is not the same as picking the one you can cross. A point
	 * dead ahead along the trace's own direction is approached ALONG the line: the dash runs
	 * parallel to the thing it is trying to trip and the swept segment-vs-segment test never
	 * intersects. At 1.6 a parallel approach is worth 2.6x its distance, so a slightly further
	 * point that can be taken side-on wins — which is the difference between a crossing and a
	 * tail-chase.
	 */
	static constexpr float TrailApproachParallelPenalty = 1.6f;

	/**
	 * Safety factor on "will this point still be there when I arrive?".
	 *
	 * The trace is length-capped, so its tail is eaten at the carrier's own speed. A point
	 * D uu behind the carrier is deleted once the carrier has run a further (MaxLength - D). A bot
	 * that commits to a point which is trimmed before it arrives spends the whole carry running at
	 * ghosts — the failure BotTrailMinPointLifeRemaining was written for, expressed in the units
	 * the length-based trace actually has.
	 */
	static constexpr float TrailTrimSafetyFactor = 1.25f;

	/**
	 * Where an interceptor WAITS when it cannot dash yet, as a fraction of the dash's commit range.
	 *
	 * Without this a hunter with its dash on cooldown walks straight through the trace it is
	 * hunting (walking through it does nothing) and comes out the far side, where it has to turn
	 * round and come back — and while it is doing that it is, from the outside, a bot milling about
	 * next to the carrier. Holding just inside dash range on its own side of the line means the
	 * charge is spent the frame it lands, from a position it is already in.
	 */
	static constexpr float TrailPocketDashRangeFraction = 0.62f;

	/**
	 * How hard a pocketed interceptor paces ALONG the trace toward the carrier, in uu of steering
	 * bias. Without it a bot that has reached its pocket has nothing to do and stands still while
	 * the carrier runs away from it.
	 *
	 * SPEC v9 §1: it is CLAMPED by how far ahead the chosen point actually is (see the pocket block
	 * in BehaviourHuntCarrier). Unclamped, a bot that had arrived at its pocket steered almost
	 * purely along this vector — up the trace, past the head, and into the carrier — then turned
	 * round and came back. Repeated at 60Hz that is a bot that mirrors the carrier's turns and
	 * stands next to them, which is the second half of the reported symptom and was being produced
	 * by the very code written to prevent the first half.
	 */
	static constexpr float TrailPocketPaceWeight = 280.f;

	/**
	 * SPEC v9 §1 — HOW LONG A HUNTER HESITATES AFTER DECLINING A CROSSING, in seconds.
	 *
	 * FTraceBotProfile::TrailDashCommitChance used to be rolled once per dash CHARGE. That is an
	 * absorbing state and it is the reason the signature play disappeared: a bot that rolls "do not
	 * commit" then never dashes, so its charge is never spent, so no charge ever arrives, so the
	 * roll is never taken again. It is disarmed for the rest of its life. MEASURED on Easy
	 * (chance 0.60): trace dashes per 10s window ran 1, 3, 3, 0, 0, 0, 0, 1, 1 while the share of
	 * hunt ticks inside dash range held flat at ~52% — the bots kept arriving and kept not firing.
	 *
	 * Re-rolling on a timer turns the same knob into a HESITATION: expected wait is
	 * (1-chance)/chance x this, so Easy dithers ~0.8s before committing and Hard ~0.06s. The
	 * difficulty gradient survives — that is what "how readable is their timing" means here — but
	 * no value of the knob can permanently switch the mechanic off.
	 */
	static constexpr float TrailCommitRetrySeconds = 1.2f;

	/** Below this planar speed a carrier is laying no new trace, so there is no line to cut behind. */
	static constexpr float TrailCutBehindMinCarrierSpeed = 120.f;
}

// =================================================================================================
// PAWN CAPABILITY ADAPTER
//
// WHERE THE VERBS ACTUALLY LIVE
//   hover pass  ATraceCore::RequestPassInput / IsShieldSuppressedFor / FindPassTargetFor
//               (bound directly — see ApplyPassInput and IsCarrierShielded below)
//   slide       UTraceCharacterMovementComponent::SetWantsToSlide
//   dash        ATraceCharacter::DoDash + UTraceCharacterMovementComponent::GetDashCharges
//
// WHY THE PROBES BELOW EXIST ANYWAY, AND WHY IT IS NOT PARANOIA
//
// Spec v2 gives the pawn several new input verbs — begin/end a hover pass, crouch, and a dash
// that now has charges. This file does not own ATraceCharacter, and under this project's strict file
// ownership rule it must not add them. Written the obvious way, the bot brain would therefore fail
// to COMPILE until somebody else's half of the pass had landed, and a bot layer that cannot be built
// is a bot layer that cannot be measured — which for a game where nine of the ten players are bots
// means the whole ruleset goes untested. That is not hypothetical: this file was written, compiled
// and first measured while the pawn still only had the pre-spec instant DoPass().
//
// So every call into the pawn goes through one of the tiny two-overload adapters below. If the entry
// point exists, the first overload binds and the real mechanic is driven. If it does not, the second
// binds and the bot degrades to the nearest thing that DOES exist (mouse1, ACharacter::Crouch, a
// plain jump) instead of breaking the build. Each adapter also returns whether it bound the real
// verb, and LogCapabilities() prints the whole table once per match at Display — so "the bots do not
// slide" can never again be diagnosed from silence. It is a compile-time question with a run-time
// answer printed in the log.
//
// REQUIREMENTS ON THE PAWN, for whoever lands the other half:
//   * the entry points must be PUBLIC (a protected member fails the detection and silently falls
//     back — that is the one failure mode of this pattern);
//   * the names below are what is probed. If different names are chosen, this block is the only
//     thing that has to change, and it is nine lines per verb.
// =================================================================================================

namespace TraceBotPawnAPI
{
	// --- Hover pass: press / release mouse1 ------------------------------------------------------
	//
	// Fallback is DoFirePressed/DoFireReleased, which is not a guess: the spec says "while carrying
	// the core, mouse1 passes the core", so the fire entry point IS the pass entry point unless the
	// pawn chooses to split them. Under the pre-spec code the fallback is inert while carrying (the
	// weapon refuses to fire for a carrier), so it is safe in both worlds.

	/** What a pass press actually bound to. Reported in [BotCaps] and used to steer the hold logic. */
	enum class EPassBinding : uint8
	{
		/** DoFirePressed. mouse1 IS the pass while carrying (spec §4), so this is the honest default. */
		FireFallback = 0,

		/** DoPassPressed()/DoPassReleased(): the real held, cancellable hover pass. */
		HoverHold = 2
	};

	// Two-level probe, most-preferred first. The overload ranking does the choosing: called with the
	// literal 0, `int` is an exact match and `...` is worse — so the first candidate that COMPILES
	// for this pawn is the one that binds.
	//
	// There WAS a middle LegacyInstant rung probing ATraceCharacter::DoPass(). That function has been
	// deleted, and the rung went with it rather than being left to probe a name that no longer exists
	// anywhere — a detector that can never fire is the silent-fallback hazard this whole block warns
	// about, pointed at itself.
	template <typename T> auto PassPressed(T* C, int) -> decltype(C->DoPassPressed(), EPassBinding())
	{
		C->DoPassPressed();
		return EPassBinding::HoverHold;
	}
	template <typename T> EPassBinding PassPressed(T* C, ...)
	{
		C->DoFirePressed();
		return EPassBinding::FireFallback;
	}

	template <typename T> auto PassReleased(T* C, int) -> decltype(C->DoPassReleased(), bool())
	{
		C->DoPassReleased();
		return true;
	}
	template <typename T> bool PassReleased(T* C, long)
	{
		C->DoFireReleased();
		return false;
	}

	// BOOST IS GONE (spec v3 §1: "remove boost from the game entirely"). The Boost/CanBoost/HasBoost
	// adapters lived here and drove UTraceCharacterMovementComponent::StartBoost. The one job the
	// boost was doing for a navmesh-less AI — getting a wedged bot over the low neon furniture it had
	// walked into — is now a plain jump, gated on UTraceSettings::BotStuckJumpSeconds. See the unstick
	// branch in UpdateMovementTech().

	// --- Crouch: slide on the ground, cancel upward momentum in the air ---------------------------
	//
	// SetWantsToSlide() is a LEVEL, not an edge — held for as long as the key would be down — which
	// is exactly the shape this controller wants, since a slide and a fast-fall differ only in how
	// long the input is held for. The fallback is ACharacter::Crouch/UnCrouch, which always exists;
	// the movement component ORs the two together, so either drives the same simulation.

	template <typename M> auto SetSlide(M* Movement, bool bWants, int) -> decltype(Movement->SetWantsToSlide(bWants), bool())
	{
		Movement->SetWantsToSlide(bWants);
		return true;
	}
	template <typename M> bool SetSlide(M*, bool, long)
	{
		return false;
	}

	template <typename T> void CrouchFallback(T* C, bool bWants)
	{
		if (bWants)
		{
			C->Crouch();
		}
		else
		{
			C->UnCrouch();
		}
	}

	/** Charges the movement component says are left, or -1 when it does not track them. */
	template <typename T> auto DashCharges(const T* M, int) -> decltype(M->GetDashCharges())
	{
		return M->GetDashCharges();
	}
	template <typename T> int32 DashCharges(const T*, long)
	{
		return -1;
	}

	/** Which pass verb WOULD bind, without performing one. For the capability log and the hold logic. */
	template <typename T> auto PassBindingOf(T* C, int) -> decltype(C->DoPassPressed(), EPassBinding())
	{
		return EPassBinding::HoverHold;
	}
	template <typename T> EPassBinding PassBindingOf(T*, ...)
	{
		return EPassBinding::FireFallback;
	}

	inline const TCHAR* PassBindingName(EPassBinding Binding)
	{
		switch (Binding)
		{
		case EPassBinding::HoverHold:     return TEXT("DoPassPressed/DoPassReleased (real hover pass)");
		default:                          return TEXT("DoFirePressed (mouse1 fallback)");
		}
	}
}

/**
 * SPEC v15 §3 — the V (secondary ability) level, mirrored onto the pawn.
 *
 * Routed through UTraceAbilityInputRelay::RouteSecondaryEdge, which is the same function a human's V
 * key reaches, so a bot's suspend obeys every rule a player's does. Named after the file's existing
 * ApplyPassInput / ApplyCrouchInput pattern: the call site says what it MEANS rather than which
 * subsystem happens to implement it.
 */
static void ApplySecondaryInput(ATraceCharacter* Character, bool bPressed)
{
	if (Character == nullptr)
	{
		return;
	}

	if (APlayerState* OwnerState = Character->GetPlayerState<APlayerState>())
	{
		UTraceAbilityInputRelay::RouteSecondaryEdge(OwnerState, bPressed);
	}
}

/**
 * Crouch as one call, routed to whichever half of the pawn owns it.
 *
 * SetWantsToSlide() is a level on the movement component and is the predicted path; Crouch/UnCrouch
 * on the character is the always-present fallback. Wrapping both here means every call site in this
 * file says what it MEANS ("hold crouch") rather than which subsystem happens to implement it.
 */
static void ApplyCrouchInput(ATraceCharacter* Character, bool bWants)
{
	if (Character == nullptr)
	{
		return;
	}

	if (UTraceCharacterMovementComponent* Movement = Character->GetTraceMovement())
	{
		if (TraceBotPawnAPI::SetSlide(Movement, bWants, 0))
		{
			return;
		}
	}

	TraceBotPawnAPI::CrouchFallback(Character, bWants);
}

/**
 * Is this character a carrier who currently CANNOT be shot?
 *
 * ATraceCore owns this fact outright (§4: "nothing else in the codebase may store either of those
 * facts separately"), and UTraceHealthComponent reads the same static, so a bot's idea of who is
 * killable is the same object as the damage rule's. That equivalence is the whole point of asking
 * the Core rather than tracking a pass window locally: a punisher that believed the shield was down
 * a frame before the health component did would fire into invulnerability and call it a bug.
 */
static bool IsCarrierShielded(const ATraceCharacter* Character)
{
	if (Character == nullptr || !Character->IsCarrier())
	{
		return false;   // Not a carrier: shootable by the ordinary rules.
	}

	return !ATraceCore::IsShieldSuppressedFor(Character);
}

/**
 * The holder's mouse1 state for the hover pass.
 *
 * Driven straight into ATraceCore::RequestPassInput, which is the same entry point a human's mouse1
 * reaches — so a bot's pass is subject to every rule a player's is, including the 0.5 s dwell, the
 * "looking away cancels" test and the 2 s cooldown. The pawn-verb fallback below only matters if the
 * Core has not spawned yet.
 */
static bool ApplyPassInput(ATraceCharacter* Character, bool bPressed)
{
	if (Character == nullptr)
	{
		return false;
	}

	if (ATraceCore* TheCore = ATraceCore::Get(Character->GetWorld()))
	{
		// Character, not nullptr: the Core matches a release against whoever armed the latch, so a bot
		// that has just completed a pass can still deliver its own release, and one bot's release can
		// never cancel another's pass.
		TheCore->RequestPassInput(bPressed, Character);
		return true;
	}

	if (bPressed)
	{
		TraceBotPawnAPI::PassPressed(Character, 0);
	}
	else
	{
		TraceBotPawnAPI::PassReleased(Character, 0);
	}
	return false;
}

// =================================================================================================
// Measurement harness  (Trace.BotMetrics 1)
//
// WHY THIS EXISTS
// "Easy is too hard" and "Easy is too easy" were both diagnosed three times from screenshots and
// from a death log that does not say which corpse was the human. Both diagnoses were wrong, and the
// second one cost a full rebalance in the wrong direction. The question a tuning pass actually has
// to answer is not "did the human die" but WHERE IN THE CHAIN the threat stops:
//
//     a human is on the field
//   -> some bot is within SightRange of them
//   -> some bot is within MaxEngagementRange of them
//   -> ...with line of sight
//   -> ...and has actually acquired them and is slewing onto them
//   -> ...and is holding the trigger
//   -> ...and the bullets connect (damage per minute)
//   -> ...and that adds up to a death.
//
// Each of those is a separate percentage below, so a run says "engage+LOS 0.4% of bot-ticks" or
// "firing 6% but 0 damage" and the failing link names itself.
//
// SPEC v2 ADDED A SECOND QUESTION: is every new mechanic actually being PLAYED? Nine of the ten
// players are bots, so a rule no bot exercises is a rule that has never run. The [BotKit] line
// counts pass attempts, completions and aborts, punisher engagements, dashes at a trace, unstick
// jumps, slides and fast-falls, so "the bots cannot hover-pass" is answerable from one line of log instead
// of from watching.
//
// It is behind a cvar because the acquisition chain costs one extra line trace per bot per tick.
// =================================================================================================

#if !UE_BUILD_SHIPPING

static TAutoConsoleVariable<int32> CVarTraceBotMetrics(
	TEXT("Trace.BotMetrics"),
	0,
	TEXT("1: log a [BotMetrics] and a [BotKit] line every 10s. [BotMetrics] measures what the bots\n")
	TEXT("are doing to the human player (sight -> engage -> LOS -> aiming -> firing, damage/min,\n")
	TEXT("time-to-first-death). [BotKit] counts how often each spec-v2 mechanic was actually used.\n")
	TEXT("Costs one line trace per bot per tick; off by default."),
	ECVF_Default);

/**
 * SPEC v9 §1. The A/B LADDER for the interception work. Three rungs, one build.
 *
 *   0  the v8 behaviour: absolute trail filters, and a fallback that pursues the carrier.
 *      This is what the user played and described.
 *   1  the geometry rewrite: trace-relative filters, crossing-angle selection, trim-aware target
 *      choice, the cut-behind fallback and the dash pocket. Interceptors now nearly always HAVE a
 *      plan — and still almost never spend the dash, because of the commit ratchet below.
 *   2  (default) the commit fix: the trace-dash commitment is re-rolled on a hesitation timer
 *      instead of once per dash charge, and a pocketed hunter no longer paces into the carrier.
 *
 * A ladder rather than a boolean because rung 1 was measured and found insufficient: 12216 hunt
 * ticks with a plan produced NINE dashes, 8504 of them refused for "no-commit". Reporting "fixed"
 * off rung 1 is exactly the failure §0 of the spec is about, and keeping rung 1 reachable is what
 * lets the claim "the commit roll was the blocker" be a measurement rather than an assertion.
 *
 * Not a tuning knob. There is no configuration in which 0 or 1 is the right answer for play.
 */
static TAutoConsoleVariable<int32> CVarTraceBotInterceptFix(
	TEXT("Trace.Bot.InterceptFix"),
	2,
	TEXT("2 (default): spec v9 §1 complete - the geometry rewrite AND the dash-commit fix.\n")
	TEXT("1: geometry only (trace-relative filters, crossing angle, cut-behind, pocket). The\n")
	TEXT("   commit roll still happens once per charge, so a bot that rolls 'no' is disarmed for good.\n")
	TEXT("0: the v8 behaviour - absolute filters and a pursuit fallback. The reported symptom.\n")
	TEXT("Never ship a run with this below 2."),
	ECVF_Default);

#endif // !UE_BUILD_SHIPPING

/**
 * SPEC v15 §3's dependency on spec v15 §2 — a BACKSTOP now, and a red arm.
 *
 * WHAT CHANGED. This was written while §2 was another slice's unlanded work: ServerSetCharacter
 * refused every bot by name (spec v14 §3, "Bots remain characterless"), so everything in
 * TraceBotAbilityBrain.cpp was unreachable — not broken, unreachable, which is much harder to
 * notice — and this poll existed to make §3 measurable at all. §2 HAS LANDED.
 * ATraceGameMode::PollCharacterSelect fills every bot at 4 Hz, randomly, once the humans on its team
 * have settled, and it wins essentially every race with this 0.5 Hz poll.
 *
 * WHY IT IS STILL HERE, AND STILL 1. It only ever acts on a bot holding ETraceCharacterId::None, so
 * with §2 working it is a no-op — and if §2 ever leaves a bot behind (a bot spawned mid-match, a
 * roster edge case), the difference is a bot with no abilities rather than a crash, which is exactly
 * the failure that is invisible in a playtest. It obeys the same ordering rule §2 does: it defers
 * while any team-mate's select screen is open, and ServerSetCharacter enforces the rest.
 *
 * Set it to 0 to prove a bot ability behaviour is genuinely coming from the character: every
 * decision then short-circuits at "no character".
 */
static TAutoConsoleVariable<int32> CVarTraceBotAutoCharacter(
	TEXT("Trace.Bot.AutoCharacter"),
	1,
	TEXT("1 (default): a bot that STILL has no character once the match is live asks for a random\n")
	TEXT("   free one. A backstop behind spec v15 §2's own 4 Hz fill, which normally gets there first;\n")
	TEXT("   per-team uniqueness and §2's 'bots pick after the humans' ordering are enforced by\n")
	TEXT("   UTraceAbilityComponent::ServerSetCharacter, not here.\n")
	TEXT("0: this poll never fires. A bot §2 has not yet reached stays characterless and every bot\n")
	TEXT("   ability decision short-circuits at 'no character', which is an independent red arm."),
	ECVF_Default);

/** Which rung of the ladder above is in force. Always the top rung in a shipping build. */
static int32 BotInterceptFixLevel()
{
#if !UE_BUILD_SHIPPING
	return CVarTraceBotInterceptFix.GetValueOnAnyThread();
#else
	return 2;
#endif
}

/** Rung 1 and above: the interception GEOMETRY (where to stand, which point to cross). */
static bool BotInterceptFixEnabled()
{
	return BotInterceptFixLevel() >= 1;
}

/** Rung 2: the DECISION to spend the charge (the commit re-roll, and the pocket pace clamp). */
static bool BotInterceptCommitFixEnabled()
{
	return BotInterceptFixLevel() >= 2;
}

#if !UE_BUILD_SHIPPING

namespace TraceBotTelemetry
{
	/** Counters for the current 10-second reporting window. Reset after every dump. */
	struct FWindow
	{
		int32 BotTicks           = 0;   // bot-ticks taken while a living human enemy was on the field
		int32 TicksHumanInSight  = 0;   // ...with that human inside SightRange
		int32 TicksHumanInEngage = 0;   // ...inside MaxEngagementRange
		int32 TicksHumanLOS      = 0;   // ...inside MaxEngagementRange AND with line of sight
		int32 TicksAimingHuman   = 0;   // ...and the human was this bot's actual ShootTarget
		int32 TicksFiringAtHuman = 0;   // ...and the trigger was down

		float  NearestHumanDist    = TNumericLimits<float>::Max();
		double SumNearestHumanDist = 0.0;
		int32  DistSamples         = 0;

		/**
		 * Closest sample that was inside the engagement envelope but had NO line of sight.
		 *
		 * A window reporting 37% of bot-ticks in range and 0.0% with line of sight, at a closest
		 * approach of 655uu, is either a real perception bug or a player standing behind a wall, and
		 * those two need telling apart before any number in this table can be trusted.
		 */
		float BlockedNearestDist = TNumericLimits<float>::Max();
	};

	/**
	 * Cumulative use counts for every spec-v2 mechanic a bot can perform.
	 *
	 * Deliberately NOT windowed and NOT reset: the question these answer is "has this mechanic ever
	 * been exercised, and how often per match", which a rolling window makes harder to read. They
	 * are also incremented unconditionally (the increments are free); only the dump is gated.
	 */
	struct FKit
	{
		int32 PassAttempts      = 0;   // pass input actually pressed (shield went down)
		int32 PassCompletions   = 0;   // ...and the Core changed hands to the intended receiver
		int32 PassAbortsThreat  = 0;   // ...aborted because the situation turned dangerous
		int32 PassAbortsOther   = 0;   // ...aborted for any other reason (receiver died, LOS lost)
		int32 PassLineUpGiveUps = 0;   // gave up while lining up, i.e. shield never dropped

		int32 TraceDashes       = 0;   // dashes deliberately committed across an enemy trace
		int32 EscapeDashes      = 0;   // carrier dashes spent breaking away from a threat
		int32 DuelDashes        = 0;   // dashes spent closing or disengaging in a fight

		int32 StuckJumps        = 0;   // wedged on geometry, tried a jump to clear it
		int32 Slides            = 0;
		int32 FastFalls         = 0;
		int32 SlideJumps        = 0;   // slides cashed in as a slide-jump inside the timing window

		int32 PunisherTicks     = 0;   // bot-ticks spent holding a bead on an enemy carrier
		int32 PunisherShots     = 0;   // trigger pulls in that role

		int32 EndzoneFlips      = 0;   // times a bot's attacking end changed (i.e. half time)
		int32 EndzoneUnresolved = 0;   // bots that had to fall back to the "Blue attacks +X" guess

		// --- MODE B (spec v4 §7). Zero for the whole match in mode A, which is itself the check
		//     that mode A has not quietly started running mode B's code.
		int32 ThrowAttempts     = 0;   // throw input actually pressed
		int32 ThrowsAtGoal      = 0;   // ...aimed at the goal mouth
		int32 ThrowsToTeammate  = 0;   // ...aimed at an open teammate
		int32 ThrowGiveUps      = 0;   // lined up on a throw and abandoned it
		int32 LooseChaseTicks   = 0;   // bot-ticks spent running at a loose Core
		int32 GoalDefenceTicks  = 0;   // bot-ticks spent holding a station in front of our own goal

		// --- MODE B, spec v5 §4. Why a shot at goal did or did not happen.
		//
		// at-goal=0 across 70 s of measured play was the headline failure, and the old counters could
		// not say which of the three gates refused it. These can: the shot is offered only when the
		// goal is inside the Core's REAL ballistic range and the arc is clear, and it is then weighed
		// against the pass rather than short-circuiting it.
		int32 ShotEvals         = 0;   // carrier evaluation ticks that considered a shot at goal
		int32 ShotOutOfRange    = 0;   // ...refused: the goal is further than a throw can carry
		int32 ShotNoSolution    = 0;   // ...refused: in range on paper, no launch pitch solves it
		int32 ShotBlocked       = 0;   // ...refused: the arc hits arena geometry
		int32 ShotLostToPass    = 0;   // ...available, but a pass to a teammate scored higher
		int32 CarryInCommits    = 0;   // times a carrier committed to running it in instead
		int32 CarryInTicks      = 0;   // bot-ticks spent driving at the mouth under that commit
		int32 CarryInAbandoned  = 0;   // commits that ran out of patience against a packed mouth

		/**
		 * Closest a carrier has come to the goal mouth, uu, over the whole run.
		 *
		 * THE NUMBER THAT DECIDES WHETHER ANY OF THE ABOVE CAN EVER FIRE. "out-of-range=140 of 140"
		 * says a shot was refused; it does not say whether it was refused by 200 uu or by 10000, and
		 * those call for opposite fixes - one is a throw that needs more reach, the other is a team
		 * that never gets up the pitch at all. On a 33600 uu field that distinction is the whole
		 * diagnosis.
		 */
		float CarrierClosestToGoal = -1.f;

		/** Teammate throws refused for being beyond what the Core can physically carry. */
		int32 PassOutOfThrowRange = 0;

		// --- SPEC v9 §1: WHY AN INTERCEPTOR IS OR IS NOT CROSSING THE TRACE -------------------------
		//
		// "dash=trace: 3" over a whole match is a symptom with six possible causes and the old counters
		// could not tell them apart. The user's report is precise — the bots "run at the carrier,
		// following the exact motion of the carrier and stand by it" — and that is a description of
		// ONE specific branch: BehaviourHuntCarrier's no-usable-trace fallback, which steers at a lead
		// point off the carrier's own velocity, i.e. a pursuit. So the first question any measurement
		// has to answer is how often a hunter has a PLAN (a point on the trace) at all, and when it
		// does not, which filter threw the trace away.
		//
		// The chain, and the counter that names each link:
		//     bot-tick in HuntCarrier                          HuntTicks
		//   -> a lethal trace point survived the filters       HuntTicksWithPlan / HuntTicksNoPlan
		//      (...or it did not, and why)                     PlanRejectShort / Band / Tangent
		//   -> the bot got within a dash of crossing it        HuntTicksInDashRange
		//   -> ...and actually spent the dash                  TraceDashes (above)
		//      (...or it did not, and why)                     DashBlocked*
		int32 HuntTicks           = 0;
		int32 HuntTicksWithPlan   = 0;
		int32 HuntTicksNoPlan     = 0;   // <- the pursuit fallback: this is the user's symptom
		int32 PlanRejectShort     = 0;   // fewer than two LETHAL points exist yet
		int32 PlanRejectBand      = 0;   // lethal points exist, but every one was filtered out
		int32 PlanRejectTangent   = 0;   // chosen segment had two coincident points, no direction
		int32 HuntTicksInDashRange = 0;  // a dash from here would reach the trace
		int32 DashBlockedParallel = 0;   // heading nearly along the trace, not across it
		int32 DashBlockedRange    = 0;   // trace further than the dash can carry
		int32 DashBlockedAlign    = 0;   // velocity not yet turned onto the crossing line
		int32 DashBlockedNoCharge = 0;   // no dash charge in hand
		int32 DashBlockedNoCommit = 0;   // charge in hand, but this charge rolled "do not commit"

		// The commit roll itself, because "no-commit=8504" does not say whether the roll is
		// UNLUCKY or STUCK, and those want opposite fixes. rolls/armed is the observed commit rate;
		// if it tracks FTraceBotProfile::TrailDashCommitChance the knob is behaving, and if `rolls`
		// is a handful across a whole match the bots are sitting in the absorbing state instead.
		int32 TrailCommitRolls = 0;
		int32 TrailCommitArmed = 0;

		/** Closest any hunter got to an enemy trace, perpendicular, uu. -1 if never measured. */
		float HuntClosestPerp = -1.f;

		/** Sum/count of the perpendicular gap on hunt ticks that had a plan, for a mean. */
		double HuntPerpSum = 0.0;
		int32  HuntPerpSamples = 0;
	};

	struct FState
	{
		bool   bStarted       = false;
		double StartTime      = 0.0;
		double NextDumpTime   = 0.0;
		double NextSampleTime = 0.0;

		/** Health sampled at 10Hz; every decrease is counted, which survives respawns and resets. */
		float  LastHumanHealth  = -1.f;
		double HumanDamageTaken = 0.0;

		int32  HumanDeaths    = 0;
		double FirstDeathTime = -1.0;

		FWindow Window;
		FKit    Kit;

		/** Printed once, the first time a bot possesses a pawn. See the capability adapter above. */
		bool bLoggedCapabilities = false;
	};

	static FState& Get()
	{
		static FState State;
		return State;
	}

	static FKit& Kit()
	{
		return Get().Kit;
	}

	/** Records the perpendicular gap between a hunter and the trace point it planned to cross. */
	static void NoteHuntPerp(float Perp)
	{
		FKit& K = Kit();
		if (K.HuntClosestPerp < 0.f || Perp < K.HuntClosestPerp)
		{
			K.HuntClosestPerp = Perp;
		}
		K.HuntPerpSum += static_cast<double>(Perp);
		++K.HuntPerpSamples;
	}

	static bool Enabled()
	{
		return CVarTraceBotMetrics.GetValueOnAnyThread() != 0;
	}

	/**
	 * Called once per bot per tick with everything that bot knows about the nearest human enemy.
	 * Distances of < 0 mean "no living human enemy exists", which is the pre-match / all-dead case
	 * and is excluded from the denominator so it cannot dilute the percentages.
	 */
	static void NoteBotTick(float DistToHuman, bool bInSight, bool bInEngage, bool bLOS, bool bAiming, bool bFiring)
	{
		if (DistToHuman < 0.f)
		{
			return;
		}

		FWindow& W = Get().Window;
		++W.BotTicks;
		W.TicksHumanInSight  += bInSight  ? 1 : 0;
		W.TicksHumanInEngage += bInEngage ? 1 : 0;
		W.TicksHumanLOS      += bLOS      ? 1 : 0;
		W.TicksAimingHuman   += bAiming   ? 1 : 0;
		W.TicksFiringAtHuman += bFiring   ? 1 : 0;

		W.NearestHumanDist = FMath::Min(W.NearestHumanDist, DistToHuman);
		W.SumNearestHumanDist += DistToHuman;
		++W.DistSamples;

		if (bInEngage && !bLOS)
		{
			W.BlockedNearestDist = FMath::Min(W.BlockedNearestDist, DistToHuman);
		}
	}

	/**
	 * -TraceWalkHuman=<seconds>: drive the human player at the fight, starting once the match is
	 * actually running.
	 *
	 * WHY A MEASUREMENT RUN NEEDS THIS
	 * An unattended run leaves the human standing on their own spawn, and the first tuning pass
	 * measured exactly that. It is a useless sample: the mean distance from the idle human to the
	 * nearest bot was 10000-15000uu across a whole match, because the objective is at the centre of a
	 * 24000uu field and every bot on both teams is playing it. The run reports "the bots never shoot
	 * the player" when what it has actually measured is the spawn being a long way from the ball.
	 *
	 * It routes through the existing Trace.SimInput console command rather than poking the pawn, so
	 * the input travels the real Enhanced Input path and this stays a measurement aid with no
	 * gameplay code of its own.
	 */
	static void MaybeWalkHuman(UWorld* World, double Now, double StartTime)
	{
		/**
		 * THE SYNTHETIC PLAYER, and why it steers.
		 *
		 * Two cruder models were tried and both measured the arena rather than the bots:
		 *
		 *   * Standing still on the spawn. Ten of thirteen windows had the player outside every bot's
		 *     SIGHT range, let alone its guns.
		 *   * Holding W. With no mouse input the heading never changes, so the player marched down a
		 *     fixed line for an entire run and bounced off the end wall.
		 *
		 * So the harness also steers, by feeding mouse deltas through the same Trace.SimAxis path the
		 * real look input uses. It is a crude bot, but it is a crude bot doing the RIGHT thing, and
		 * that is the difference between a number about bot skill and a number about level layout.
		 */
		constexpr double SteerInterval = 0.2;    // seconds between look corrections

		/**
		 * How the forward hold is re-issued, and the bug that made every earlier number meaningless.
		 *
		 * Trace.SimInput presses a key and schedules an UNCONDITIONAL release N seconds later. The
		 * release is not owned by the press that armed it — it just releases the key. So overlapping
		 * holds do not extend each other, they truncate each other, and an earlier version re-pressed
		 * W every 15s while asking for a 16s hold: the synthetic player walked for about one second
		 * in every fifteen, and every "difficulty" measurement taken with it was really a measurement
		 * of how far the player happened to drift from their own spawn pad.
		 *
		 * So the hold must end BEFORE its successor begins. Holding for 90% of the refresh period
		 * leaves a deterministic gap in which the stale release lands harmlessly.
		 */
		constexpr double WalkRefresh  = 2.0;
		constexpr double WalkHold     = WalkRefresh * 0.9;

		static double NextSteerTime = -1.0;
		static double NextWalkTime  = -1.0;
		static bool   bDisabled     = false;

		if (bDisabled)
		{
			return;
		}

		float WalkSeconds = 0.f;
		if (!FParse::Value(FCommandLine::Get(), TEXT("TraceWalkHuman="), WalkSeconds) || WalkSeconds <= 0.f)
		{
			bDisabled = true;   // Switch absent: never look again.
			return;
		}

		// Wait for a pawn. Injecting at engine init (where -ExecCmds runs) is silently dropped:
		// there is no player controller to route the key to yet.
		APlayerController* PC = World->GetFirstPlayerController();
		const ATraceCharacter* HumanChar = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
		if (HumanChar == nullptr || (Now - StartTime) < 2.0 || (Now - StartTime) > WalkSeconds)
		{
			return;
		}

		// --- Where is the player trying to get to ---------------------------------------------------
		//
		// THE FIGHT FIRST, THE OBJECTIVE SECOND — and why that ordering is what makes the number mean
		// anything.
		//
		// A synthetic player who walks at wherever the objective currently is spends most of a run
		// crossing half the pitch behind the play, and across four runs the mean distance to
		// the nearest bot varied by a factor of three purely by luck. The "difficulty" those runs
		// reported tracked that distance and not the profile: NORMAL measured *less* lethal than EASY
		// on exactly the runs where the human had wandered into their own back corner.
		//
		// A measurement whose variance is dominated by where the ball happened to roll cannot rank
		// three difficulties. So the synthetic player closes on the nearest living enemy whenever one
		// is anywhere near, and only falls back to the objective when the field is empty around them.
		// This deliberately measures the ENGAGED case — "how dangerous is a bot you have walked up
		// to" — which is the question the difficulty curve exists to answer.
		//
		// The radius is SHORT on purpose. At 8000uu the synthetic player tail-chased whichever bot
		// happened to be nearest across the whole pitch, a pursuit which never closes because a bot
		// walks at exactly the player's own speed.
		constexpr float EngageSeekRadius = 3500.f;

		FVector Objective = FVector::ZeroVector;   // Centre of the pitch is the last-resort default.

		const ATraceCharacter* SeekTarget = nullptr;
		const ATraceCharacter* AnyCarrier = nullptr;
		float SeekDistSq = FMath::Square(EngageSeekRadius);

		if (const ATraceGameMode* GM = World->GetAuthGameMode<ATraceGameMode>())
		{
			const ETraceTeam HumanTeam = HumanChar->GetTeam();
			for (const TWeakObjectPtr<ATraceCharacter>& WeakOther : GM->GetTrackedCharacters())
			{
				const ATraceCharacter* Other = WeakOther.Get();
				if (Other == nullptr || Other == HumanChar || !Other->IsAlive())
				{
					continue;
				}

				if (Other->IsCarrier())
				{
					AnyCarrier = Other;
				}

				// Walking at a teammate would park the human in the middle of their own pack, which
				// is a crowd, not a fight.
				if (HumanTeam != ETraceTeam::None && Other->GetTeam() == HumanTeam)
				{
					continue;
				}

				const float DistSq = static_cast<float>(FVector::DistSquared2D(HumanChar->GetActorLocation(), Other->GetActorLocation()));
				if (DistSq < SeekDistSq)
				{
					SeekDistSq = DistSq;
					SeekTarget = Other;
				}
			}
		}

		// The Core is a STATUS now (spec §2) — there is no loose object to walk at, so the objective
		// fallback is "wherever the Core currently is", i.e. whoever is holding it.
		if (SeekTarget != nullptr)
		{
			Objective = SeekTarget->GetActorLocation();
		}
		else if (AnyCarrier != nullptr)
		{
			Objective = AnyCarrier->GetActorLocation();
		}

		// --- Steer ----------------------------------------------------------------------------------
		if (Now >= NextSteerTime)
		{
			NextSteerTime = Now + SteerInterval;

			FVector ToObjective = Objective - HumanChar->GetActorLocation();
			ToObjective.Z = 0.f;

			if (!ToObjective.IsNearlyZero())
			{
				const float DesiredYaw = ToObjective.Rotation().Yaw;
				const float YawError = FRotator::NormalizeAxis(DesiredYaw - PC->GetControlRotation().Yaw);

				// Proportional, and divided by the look scale because that is what turns a raw mouse
				// delta into degrees. Under-corrected on purpose (0.5): this converges over a couple of
				// ticks instead of snapping, which keeps the cross-speed aim-error term honest.
				//
				// INTEGRATION FIX: this used to read UTraceSettings::LookSensitivity (2.5), which the
				// settings-menu work retired - the human's yaw scale is now the Look mapping's X
				// scalar, built from UTraceUserSettings. Dividing by the dead 2.5 under-corrected this
				// synthetic human's yaw by 1.67x, so it converged on its objective slowly and the
				// walk harness's engagement statistics were measured against a player that could not
				// turn properly. Read the value that is actually in the input chain.
				const float Sensitivity = FMath::Max(0.01f, UTraceUserSettings::Get().GetLookScaleX());
				const float Delta = FMath::Clamp(YawError, -60.f, 60.f) * 0.5f / Sensitivity;

				if (FMath::Abs(Delta) > 0.01f)
				{
					GEngine->Exec(World, *FString::Printf(TEXT("Trace.SimAxis MouseX %.3f"), Delta));
				}
			}
		}

		// --- Walk -----------------------------------------------------------------------------------
		if (Now >= NextWalkTime)
		{
			NextWalkTime = Now + WalkRefresh;
			GEngine->Exec(World, *FString::Printf(TEXT("Trace.SimInput W %.2f"), WalkHold));
		}

		// --- Anti-stuck -----------------------------------------------------------------------------
		// Walking straight at the objective drives the player into cover and holds them there: a run
		// logged the identical position for two consecutive windows, i.e. wedged on top of a block in
		// their own half while the match happened elsewhere. A sidestep unsticks it the way a player
		// would, and without it the sample is again measuring geometry.
		static FVector LastPos = FVector::ZeroVector;
		static double  LastMoveTime = 0.0;
		static bool    bStrafeRight = true;

		const FVector Pos = HumanChar->GetActorLocation();
		if (FVector::DistSquared2D(Pos, LastPos) > FMath::Square(120.f))
		{
			LastPos = Pos;
			LastMoveTime = Now;
		}
		else if ((Now - LastMoveTime) > 2.5)
		{
			LastMoveTime = Now;
			bStrafeRight = !bStrafeRight;
			GEngine->Exec(World, bStrafeRight ? TEXT("Trace.SimInput D 1.2") : TEXT("Trace.SimInput A 1.2"));
		}
	}

	/** Samples human health/deaths at 10Hz and emits the summaries every 10s. Safe to call per bot. */
	static void Poll(UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}

		FState& S = Get();
		const double Now = World->GetTimeSeconds();

		if (!S.bStarted)
		{
			S.bStarted       = true;
			S.StartTime      = Now;
			S.NextDumpTime   = Now + 10.0;
			S.NextSampleTime = Now;
		}

		MaybeWalkHuman(World, Now, S.StartTime);

		// ---- 10Hz health sampler -----------------------------------------------------------------
		if (Now >= S.NextSampleTime)
		{
			S.NextSampleTime = Now + 0.1;

			const APlayerController* PC = World->GetFirstPlayerController();
			const ATraceCharacter* HumanChar = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
			const ATracePlayerState* HumanState = (PC != nullptr) ? PC->GetPlayerState<ATracePlayerState>() : nullptr;

			if (HumanChar != nullptr && HumanChar->Health != nullptr)
			{
				const float H = HumanChar->Health->Health;
				if (S.LastHumanHealth >= 0.f && H < S.LastHumanHealth)
				{
					S.HumanDamageTaken += (S.LastHumanHealth - H);
				}
				S.LastHumanHealth = H;
			}
			else
			{
				// Dead or between pawns: forget the last reading so a respawn's jump back to full
				// health is not later read as damage.
				S.LastHumanHealth = -1.f;
			}

			if (HumanState != nullptr && HumanState->Deaths > S.HumanDeaths)
			{
				if (S.FirstDeathTime < 0.0)
				{
					S.FirstDeathTime = Now - S.StartTime;
				}
				S.HumanDeaths = HumanState->Deaths;
			}
		}

		// ---- 10s summary -------------------------------------------------------------------------
		if (Now < S.NextDumpTime)
		{
			return;
		}
		S.NextDumpTime = Now + 10.0;

		const FWindow& W = S.Window;
		const FKit& K = S.Kit;
		const double Elapsed = FMath::Max(1.0, Now - S.StartTime);
		const double Minutes = Elapsed / 60.0;

		auto Pct = [&W](int32 N) -> double
		{
			return (W.BotTicks > 0) ? (100.0 * N / W.BotTicks) : 0.0;
		};

		// Where the player actually was, and whether they were carrying. A SHIELDED carrier is
		// skipped by FindBestShootTarget by design, so a window with the player carrying explains an
		// aiming figure of zero without anything being wrong.
		FString HumanWhere = TEXT("<none>");
		bool bHumanCarrying = false;
		if (const APlayerController* HumanPC = World->GetFirstPlayerController())
		{
			if (const ATraceCharacter* HumanChar = Cast<ATraceCharacter>(HumanPC->GetPawn()))
			{
				HumanWhere = HumanChar->GetActorLocation().ToCompactString();
				bHumanCarrying = HumanChar->IsCarrier();
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[BotMetrics] t=%.0fs %s | deaths=%d first=%s kpm=%.2f | dmg=%.0f (%.0f/min) | ")
			TEXT("botticks=%d sight=%.1f%% engage=%.1f%% engage+LOS=%.1f%% aiming=%.1f%% firing=%.1f%% | ")
			TEXT("nearest=%.0fuu mean=%.0fuu | blocked-nearest=%.0fuu human=%s%s"),
			Elapsed,
			UTraceSettings::BotDifficultyToString(UTraceSettings::GetBotDifficulty()),
			S.HumanDeaths,
			(S.FirstDeathTime >= 0.0) ? *FString::Printf(TEXT("%.0fs"), S.FirstDeathTime) : TEXT("none"),
			S.HumanDeaths / FMath::Max(0.0166, Minutes),
			S.HumanDamageTaken,
			S.HumanDamageTaken / FMath::Max(0.0166, Minutes),
			W.BotTicks,
			Pct(W.TicksHumanInSight),
			Pct(W.TicksHumanInEngage),
			Pct(W.TicksHumanLOS),
			Pct(W.TicksAimingHuman),
			Pct(W.TicksFiringAtHuman),
			(W.DistSamples > 0) ? W.NearestHumanDist : 0.f,
			(W.DistSamples > 0) ? (W.SumNearestHumanDist / W.DistSamples) : 0.0,
			(W.BlockedNearestDist < TNumericLimits<float>::Max()) ? W.BlockedNearestDist : -1.f,
			*HumanWhere,
			bHumanCarrying ? TEXT(" CARRYING") : TEXT(""));

		// Cumulative, not windowed: the question is "did this mechanic ever run, and how often per
		// match", and a rolling window makes that harder to read rather than easier.
		UE_LOG(LogTraceGame, Display,
			TEXT("[BotKit] t=%.0fs | pass: attempt=%d done=%d abort-threat=%d abort-other=%d giveup=%d ")
			TEXT("| dash: trace=%d escape=%d duel=%d | stuckjump=%d slide=%d slidejump=%d fastfall=%d ")
			TEXT("| punish: ticks=%d shots=%d | endzone: flips=%d unresolved=%d"),
			Elapsed,
			K.PassAttempts, K.PassCompletions, K.PassAbortsThreat, K.PassAbortsOther, K.PassLineUpGiveUps,
			K.TraceDashes, K.EscapeDashes, K.DuelDashes,
			K.StuckJumps, K.Slides, K.SlideJumps, K.FastFalls,
			K.PunisherTicks, K.PunisherShots,
			K.EndzoneFlips, K.EndzoneUnresolved);

		// SPEC v9 §1. The interception chain, link by link. Read it left to right and the first
		// number that collapses is the bug: "plan=0 of 4000" means the bots never had a point on the
		// trace to run at and were therefore pursuing the carrier, which is exactly what the user
		// described; "plan high, in-range 0" means they had a plan and never reached it; "in-range
		// high, dash=0" means they reached it and would not spend the charge.
		auto HuntPct = [&K](int32 N) -> double
		{
			return (K.HuntTicks > 0) ? (100.0 * N / K.HuntTicks) : 0.0;
		};

		UE_LOG(LogTraceGame, Display,
			TEXT("[BotHunt] t=%.0fs | huntticks=%d plan=%d (%.1f%%) NO-PLAN=%d (%.1f%%) ")
			TEXT("| plan refused: short=%d band=%d tangent=%d ")
			TEXT("| in-dash-range=%d (%.1f%%) dashes=%d ")
			TEXT("| dash refused: parallel=%d range=%d align=%d no-charge=%d no-commit=%d ")
			TEXT("| commit rolls=%d armed=%d (%.0f%%) ")
			TEXT("| perp to trace: closest=%.0fuu mean=%.0fuu"),
			Elapsed,
			K.HuntTicks, K.HuntTicksWithPlan, HuntPct(K.HuntTicksWithPlan),
			K.HuntTicksNoPlan, HuntPct(K.HuntTicksNoPlan),
			K.PlanRejectShort, K.PlanRejectBand, K.PlanRejectTangent,
			K.HuntTicksInDashRange, HuntPct(K.HuntTicksInDashRange), K.TraceDashes,
			K.DashBlockedParallel, K.DashBlockedRange, K.DashBlockedAlign,
			K.DashBlockedNoCharge, K.DashBlockedNoCommit,
			K.TrailCommitRolls, K.TrailCommitArmed,
			(K.TrailCommitRolls > 0) ? (100.0 * K.TrailCommitArmed / K.TrailCommitRolls) : 0.0,
			K.HuntClosestPerp,
			(K.HuntPerpSamples > 0) ? (K.HuntPerpSum / K.HuntPerpSamples) : 0.0);

		// Mode B, on its own line and only when it has anything to say. In mode A every one of these
		// is zero and the line is suppressed, so its mere presence in a log is the evidence that the
		// mode-B bot code ran — and its absence is the evidence that mode A did not touch it.
		if (K.ThrowAttempts + K.LooseChaseTicks + K.GoalDefenceTicks + K.ShotEvals > 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[BotKitB] t=%.0fs | throw: attempt=%d at-goal=%d to-mate=%d giveup=%d ")
				TEXT("| loose: chaseticks=%d | defend: ticks=%d"),
				Elapsed,
				K.ThrowAttempts, K.ThrowsAtGoal, K.ThrowsToTeammate, K.ThrowGiveUps,
				K.LooseChaseTicks, K.GoalDefenceTicks);

			// Spec v5 §4. The line that says WHY a shot at goal did or did not happen, because
			// "at-goal=0" on its own is a symptom with four possible causes and no way to tell them
			// apart from outside.
			UE_LOG(LogTraceGame, Display,
				TEXT("[BotKitB] t=%.0fs | shot: evals=%d out-of-range=%d no-solution=%d blocked=%d lost-to-pass=%d ")
				TEXT("| carry-in: commits=%d ticks=%d abandoned=%d | closest a carrier got to the mouth: %.0f uu ")
				TEXT("| mate-throws refused as unreachable: %d"),
				Elapsed,
				K.ShotEvals, K.ShotOutOfRange, K.ShotNoSolution, K.ShotBlocked, K.ShotLostToPass,
				K.CarryInCommits, K.CarryInTicks, K.CarryInAbandoned,
				K.CarrierClosestToGoal, K.PassOutOfThrowRange);
		}

		S.Window = FWindow();
	}
}

#define TRACE_BOT_KIT(Field) (++TraceBotTelemetry::Kit().Field)
#define TRACE_BOT_HUNT_PERP(Value) TraceBotTelemetry::NoteHuntPerp(Value)

#else

#define TRACE_BOT_KIT(Field) do {} while (0)
#define TRACE_BOT_HUNT_PERP(Value) do {} while (0)

#endif // !UE_BUILD_SHIPPING

// =================================================================================================
// Construction / lifecycle
// =================================================================================================

ATraceBotController::ATraceBotController(const FObjectInitializer& OI)
	: Super(OI)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	// AAIController sets this to false. Bots need a PlayerState for three separate reasons: the team
	// (ATracePlayerState::Team drives spawn side, friendly fire and the trace's "is this an enemy"
	// test), the scoreboard, and ATraceGameMode::GetActivePlayerCount — without it a solo human plus
	// nine bots would still read as one player and the match would never leave WaitingForPlayers.
	bWantsPlayerState = true;

	// We write the control rotation ourselves every tick; it is the pawn's aim (see
	// ATraceCharacter::GetAimDirection). Letting the base class copy the pawn's facing into it would
	// make bots shoot wherever their body happened to be turning.
	bSetControlRotationFromPawnOrientation = false;

	// No behaviour tree exists to start, but say so explicitly rather than relying on the default.
	bStartAILogicOnPossess = false;
}

void ATraceBotController::BeginPlay()
{
	Super::BeginPlay();

	if (UWorld* World = GetWorld())
	{
		if (const ATraceArenaBuilder* Builder = ATraceArenaBuilder::Get(World))
		{
			FieldBounds = Builder->GetFieldBounds();
			bBoundsValid = (FieldBounds.IsValid != 0);
		}

		// The difficulty the player picked travels from the title screen as "?difficulty=easy" on
		// the map URL. Reading it here rather than in the game mode keeps the whole bot difficulty
		// story inside the two files that own it; UTraceSettings latches the first answer, so ten
		// bots calling this is one resolution and nine no-ops.
		if (const AGameModeBase* GameMode = World->GetAuthGameMode())
		{
			UTraceSettings::ResolveBotDifficultyFromOptions(GameMode->OptionsString);
		}
	}
}

void ATraceBotController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	if (!HasAuthority())
	{
		return;
	}

	// The bounds may not have been resolvable at BeginPlay (a bot spawned before the arena builder
	// finished, or a builder that appeared later). Possession is the second, always-late chance.
	if (!bBoundsValid)
	{
		if (UWorld* World = GetWorld())
		{
			if (const ATraceArenaBuilder* Builder = ATraceArenaBuilder::Get(World))
			{
				FieldBounds = Builder->GetFieldBounds();
				bBoundsValid = (FieldBounds.IsValid != 0);
			}
		}
	}

	// Order matters: UCharacterMovementComponent::TickComponent consumes the pawn's pending input
	// vector, and UTraceCharacterMovementComponent::BeginDash reads Acceleration to lock the dash
	// direction. If the controller ticked *after* the movement component, every dash would fire
	// along last frame's steering — which for the trace intercept is the difference between passing
	// through the trace and missing it.
	ATraceCharacter* PossessedCharacter = Cast<ATraceCharacter>(InPawn);
	if (PossessedCharacter != nullptr)
	{
		if (UCharacterMovementComponent* Movement = PossessedCharacter->GetCharacterMovement())
		{
			Movement->AddTickPrerequisiteActor(this);
		}
	}

	// Fresh personality per life so a bot that dies twenty times does not feel identical each time.
	PersonalitySkillBias = FMath::FRand();
	FormationBias = FMath::FRandRange(-1.f, 1.f);
	StrafeSign = (FMath::FRand() < 0.5f) ? -1.f : 1.f;

	State = ETraceBotState::Idle;
	LastLoggedState = ETraceBotState::Idle;
	bTriggerHeld = false;
	TimeUntilNextDecision = 0.f;
	ShootTarget = nullptr;
	AcquiredTarget = nullptr;
	bHadAcquiredTarget = false;
	AimZone = 0;
	StuckSeconds = 0.f;
	EvadeUntilTime = 0.f;
	BurstEndTime = 0.f;
	BurstRestUntilTime = 0.f;

	// A new pawn is a new body: no pass in flight, no crouch held, and the whole movement kit
	// charged. Leaving any of these latched across a respawn would leave the fresh pawn crouching or
	// believing it had already spent a dash it never took.
	PassPhase = ETraceBotPassPhase::None;
	PassReceiver = nullptr;
	bPassInputHeld = false;
	bCommitCarryIn = false;
	bPassOwnsAim = false;
	PassCooldownUntilTime = 0.f;
	NextPassEvalTime = 0.f;
	// Spec v13 §6: a wind-up is body state too, and a fresh pawn has not wound anything up.
	PlannedThrowCharge = 1.f;
	ThrowChargeReleaseTime = 0.f;
	bCrouchHeld = false;
	SlideEndTime = 0.f;
	SlideReadyTime = 0.f;
	SpentDashCharges = 0;
	NextDashRefillTime = 0.f;

	// SPEC v15 §3. A new body has pressed nothing and wants nothing; the brain re-rolls this life's
	// ability tempo off the same PersonalitySkillBias the aim and reaction clocks use, so a bot that
	// dies twenty times does not use its E on the same beat every time.
	AbilityOrders = FTraceBotAbilityOrders();
	bSecondaryInputHeld = false;
	if (const UWorld* AbilityWorld = GetWorld())
	{
		const float NowSeconds = static_cast<float>(AbilityWorld->GetTimeSeconds());
		AbilityBrain.OnPossessed(NowSeconds, PersonalitySkillBias);

		// The auto-character grace period. Deliberately generous: spec v15 §2's own selection has to
		// be able to win every race, and a bot picking its own character a beat before the select
		// screen resolves would be this pass quietly overriding another one.
		NextAutoCharacterTime = NowSeconds + 6.f;
	}

	// The attacking end has to be re-derived for the new pawn — and it is genuinely allowed to have
	// changed while this bot was dead, because half time can happen during a respawn.
	NextEndzoneResolveTime = 0.f;

	// A fresh pawn spawns with its dash already charged, so there is no refill edge to roll on. Roll
	// here and zero the watermark, or the first charge of every life would inherit whatever the
	// previous life happened to decide. Zero, not the live count: the count is read again next tick
	// and a spawn that comes up charged must not look like a refill that has already been rolled.
	LastSeenDashCharges = 0;
	bCommitDashWithThisCharge =
		FMath::FRand() < FMath::Clamp(UTraceSettings::GetBotProfile().TrailDashCommitChance, 0.f, 1.f);

	// ...and if that roll declined, the hesitation starts now rather than at some stale time from a
	// previous life. Zero would also work (any past time re-rolls immediately) but would give the
	// first frame of every life a free second roll, which is the difficulty knob applied twice.
	if (const UWorld* PossessWorld = GetWorld())
	{
		NextTrailCommitRollTime = static_cast<float>(PossessWorld->GetTimeSeconds())
			+ TraceBotConstants::TrailCommitRetrySeconds;
	}

	// A fresh spawn gets the full reacquire delay before it may shoot anyone. Without it, ten bots
	// respawning on the same frame all open fire on frame one, which is what a "wipe in 1.5s"
	// actually looked like from the inside.
	if (const UWorld* World = GetWorld())
	{
		const float Now = static_cast<float>(World->GetTimeSeconds());
		BlindUntilTime = Now + FMath::Max(0.f, UTraceSettings::GetBotProfile().ReacquireDelaySeconds);
	}

#if !UE_BUILD_SHIPPING
	// Print the pawn capability table exactly once per session. See the adapter block at the top of
	// this file: a mechanic that silently fell back to its degraded form is otherwise indetectable
	// from the outside, and this project has twice lost a day to concluding a mechanic was dead when
	// the only dead thing was the evidence.
	if (PossessedCharacter != nullptr && !TraceBotTelemetry::Get().bLoggedCapabilities)
	{
		TraceBotTelemetry::Get().bLoggedCapabilities = true;

		const UTraceCharacterMovementComponent* Movement = PossessedCharacter->GetTraceMovement();
		const int32 Charges = (Movement != nullptr) ? TraceBotPawnAPI::DashCharges(Movement, 0) : -1;
		const bool bSlide = (Movement != nullptr) && TraceBotPawnAPI::SetSlide(
			const_cast<UTraceCharacterMovementComponent*>(Movement), false, 0);

		// The pass is Core-first (ATraceCore::RequestPassInput is the same entry point mouse1 reaches);
		// the pawn verb is only what a Core-less world would fall back to. Report whichever is live.
		const bool bCorePass = (ATraceCore::Get(GetWorld()) != nullptr);

		UE_LOG(LogTraceGame, Display,
			TEXT("[BotCaps] pass=%s | slide=%s | shield-query=%s | dash-charges=%s"),
			bCorePass
				? TEXT("ATraceCore::RequestPassInput (held, cancellable hover pass)")
				: TraceBotPawnAPI::PassBindingName(TraceBotPawnAPI::PassBindingOf(PossessedCharacter, 0)),
			bSlide ? TEXT("SetWantsToSlide") : TEXT("ACharacter::Crouch fallback"),
			TEXT("ATraceCore::IsShieldSuppressedFor"),
			(Charges >= 0) ? TEXT("GetDashCharges") : TEXT("NOT BOUND — using the shadow model"));
	}
#endif

	// Start facing up the field rather than at the wall behind the spawn pad. Resolve the endzone
	// first, or the very first facing is the pre-switch one.
	MyTeam = ETraceTeam::None;
	if (const ATracePlayerState* MyState = GetPlayerState<ATracePlayerState>())
	{
		MyTeam = MyState->Team;
	}
	ResolveAttackEndzone();

	const FVector Goal = GetAttackGoalLocation();
	if (InPawn != nullptr)
	{
		FRotator Facing = (Goal - InPawn->GetActorLocation()).Rotation();
		Facing.Pitch = 0.f;
		Facing.Roll = 0.f;
		SetControlRotation(Facing);
	}
}

void ATraceBotController::OnUnPossess()
{
	// Release everything we are holding against the pawn we are about to lose. A held trigger keeps
	// the weapon component ticking on a corpse; a held PASS input is worse, because on the pawn side
	// it is a live vulnerable window that nothing would ever close.
	if (ATraceCharacter* BotCharacter = GetBotCharacter())
	{
		if (bTriggerHeld)
		{
			BotCharacter->DoFireReleased();
		}
		if (bPassInputHeld)
		{
			ApplyPassInput(BotCharacter, false);
		}
		if (bCrouchHeld)
		{
			ApplyCrouchInput(BotCharacter, false);
		}
		// SPEC v15 §3. The same argument as the pass input, one level worse: a held V is Mace with
		// gravity switched off, and a release that never arrives leaves her floating forever.
		if (bSecondaryInputHeld)
		{
			ApplySecondaryInput(BotCharacter, false);
		}
	}

	bTriggerHeld = false;
	bPassInputHeld = false;
	bCrouchHeld = false;
	bSecondaryInputHeld = false;
	PassPhase = ETraceBotPassPhase::None;
	PassReceiver = nullptr;
	AbilityBrain.OnUnPossessed();
	AbilityOrders = FTraceBotAbilityOrders();

	Super::OnUnPossess();
}

void ATraceBotController::SetBotDisplayName(const FString& InName)
{
	if (!HasAuthority())
	{
		return;
	}

	if (APlayerState* BotState = GetPlayerState<APlayerState>())
	{
		BotState->SetPlayerName(InName);
	}
}

const TCHAR* ATraceBotController::StateToString(ETraceBotState InState)
{
	switch (InState)
	{
	case ETraceBotState::CarryToGoal:   return TEXT("CarryToGoal");
	case ETraceBotState::HuntCarrier:   return TEXT("HuntCarrier");
	case ETraceBotState::PunishPasser:  return TEXT("PunishPasser");
	case ETraceBotState::EscortCarrier: return TEXT("EscortCarrier");
	case ETraceBotState::Regroup:       return TEXT("Regroup");
	case ETraceBotState::Fight:         return TEXT("Fight");
	case ETraceBotState::ChaseLooseCore: return TEXT("ChaseLooseCore");
	case ETraceBotState::DefendGoal:    return TEXT("DefendGoal");
	default:                            return TEXT("Idle");
	}
}

const TCHAR* ATraceBotController::PassPhaseToString(ETraceBotPassPhase InPhase)
{
	switch (InPhase)
	{
	case ETraceBotPassPhase::Lining:   return TEXT("Lining");
	case ETraceBotPassPhase::Holding:  return TEXT("Holding");
	case ETraceBotPassPhase::Charging: return TEXT("Charging");
	case ETraceBotPassPhase::Cooldown: return TEXT("Cooldown");
	default:                           return TEXT("None");
	}
}

// =================================================================================================
// Tick
// =================================================================================================

void ATraceBotController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// AAIController only ever exists on the server, but say it out loud: nothing below may run
	// anywhere else, and a listen server hosting clients must not let this path leak.
	if (!HasAuthority())
	{
		return;
	}

	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr || !BotCharacter->IsAlive())
	{
		// Dead or pawnless. Drop everything we are holding — in particular the pass input, which on
		// the pawn side is a live "I am vulnerable" window — and wait for the GameMode's respawn
		// timer, which is the same timer humans are on.
		if (BotCharacter != nullptr)
		{
			if (bTriggerHeld)
			{
				BotCharacter->DoFireReleased();
			}
			if (bPassInputHeld)
			{
				ApplyPassInput(BotCharacter, false);
			}
			if (bCrouchHeld)
			{
				ApplyCrouchInput(BotCharacter, false);
			}
			if (bSecondaryInputHeld)
			{
				ApplySecondaryInput(BotCharacter, false);
			}
		}

		bTriggerHeld = false;
		bPassInputHeld = false;
		bCrouchHeld = false;
		bSecondaryInputHeld = false;
		PassPhase = ETraceBotPassPhase::None;
		PassReceiver = nullptr;
		bCommitCarryIn = false;
		State = ETraceBotState::Idle;

		// The cooldown keeps running while dead (spec v14 §5) and that is deliberate, but a latched
		// INTENT must not survive: the reason it was latched for died with the pawn.
		AbilityBrain.OnUnPossessed();
		AbilityOrders = FTraceBotAbilityOrders();
		return;
	}

	DesiredMoveDirection = FVector::ZeroVector;
	bWantsToAim = false;
	bPassOwnsAim = false;
	bWantsDashThisTick = false;
	bWantsJumpThisTick = false;
	ShootTarget = nullptr;

	GatherWorldState();
	TickDashCharges(DeltaSeconds);

	// ---------------------------------------------------------------------------------------------
	// One roll per dash CHARGE, taken on the frame a charge ARRIVES. See the field comment: this is
	// what gives FTraceBotProfile::TrailDashCommitChance a meaning a designer can reason about, and
	// it is the difference between "the defence presses hard" and "the defence presses always".
	//
	// SPEC v9 §1 — MEASURED, AND THE SECOND REASON THE SIGNATURE PLAY WAS MISSING. The edge used to
	// be `charges > 0 && !was > 0`, and the flag was cleared by the interceptor the moment it ASKED
	// to dash. Those two do not compose. RequestDash raises an intent; the pawn can still refuse it
	// (UTraceCharacterMovementComponent::CanDash is false in the air, mid-dash, and for a few frames
	// after landing), and when it does the charge count never changes — so there is no falling edge,
	// therefore no rising edge, therefore NO RE-ROLL, and the bot sits on `bCommitDashWithThisCharge
	// = false` for the rest of its life. A 60 s Hard sample refused 4963 of 8869 hunt ticks for
	// "no-commit" against a profile that commits 95% of charges.
	//
	// So: the flag is no longer cleared on request, and the edge is on the charge COUNT RISING
	// rather than on the boolean. A charge that was asked for and refused stays armed and is tried
	// again next tick, which is what a defender standing next to a trace should do; a charge that
	// was really spent comes back off the cooldown as a new charge and gets a fresh roll.
	//
	// SPEC v9 §1, THE REMAINING HALF — AND THE ONE THAT ACTUALLY HID THE MECHANIC.
	//
	// Fixing the EDGE was not enough, because "one roll per charge" is itself an absorbing state.
	// A bot that rolls "do not commit" declines the crossing; declining means it never dashes;
	// never dashing means the charge is never spent; a charge that is never spent never arrives
	// again; and the rising edge that would re-roll therefore never happens. The bot is disarmed
	// permanently — and it does not go and do something else, it stands in the dash pocket beside
	// the trace pacing after the carrier, which is precisely the behaviour the user described.
	//
	// MEASURED on rung 1, Easy (TrailDashCommitChance 0.60), 110s: trace dashes per 10s window ran
	// 1, 3, 3, 0, 0, 0, 0, 1, 1 — a burst at kickoff while every bot still had its first, unrolled
	// charge, then nothing — while the share of hunt ticks spent INSIDE dash range held flat at
	// ~52% and 8504 of 12216 planned ticks were refused for "no-commit". The bots kept arriving in
	// position and kept not firing. A ratchet, not bad luck.
	//
	// A hunter that declines now hesitates for TrailCommitRetrySeconds and rolls again, so the knob
	// reads as "how long does this difficulty dither before it commits" — Easy ~0.8s, Normal ~0.4s,
	// Hard ~0.06s — and no setting of it can switch the signature play off. The charge-arrival roll
	// is kept as well: a charge landing is a genuinely new decision and should not have to wait out
	// a hesitation timer left over from the last one.
	// ---------------------------------------------------------------------------------------------
	{
		const int32 Charges = GetDashCharges();
		const float RollNow = (GetWorld() != nullptr) ? static_cast<float>(GetWorld()->GetTimeSeconds()) : 0.f;

		const bool bChargeArrived = (Charges > LastSeenDashCharges);
		const bool bHesitationExpired = BotInterceptCommitFixEnabled()
			&& (Charges > 0)
			&& !bCommitDashWithThisCharge
			&& (RollNow >= NextTrailCommitRollTime);

		if (bChargeArrived || bHesitationExpired)
		{
			bCommitDashWithThisCharge =
				FMath::FRand() < FMath::Clamp(UTraceSettings::GetBotProfile().TrailDashCommitChance, 0.f, 1.f);
			NextTrailCommitRollTime = RollNow + TraceBotConstants::TrailCommitRetrySeconds;

			TRACE_BOT_KIT(TrailCommitRolls);
			if (bCommitDashWithThisCharge)
			{
				TRACE_BOT_KIT(TrailCommitArmed);
			}
		}
		LastSeenDashCharges = Charges;
	}

	TimeUntilNextDecision -= DeltaSeconds;
	if (TimeUntilNextDecision <= 0.f)
	{
		const float Interval = FMath::Max(0.05f, UTraceSettings::GetBotProfile().DecisionInterval);
		// Stagger the cadence per bot so ten bots never all re-plan on the same frame.
		TimeUntilNextDecision = Interval * FMath::FRandRange(0.8f, 1.2f);
		DecideState();
	}

	UpdateMovementIntent(DeltaSeconds);

	// The pass runs EVERY tick, not on the decision cadence. A 0.5 s dwell steered from a 0.3 s
	// decision tick loses whole attempts to phase alignment, and the abort path has to be able to
	// react to a threat inside a frame or two — that half second is the only window in the game
	// where the carrier can be shot, and it is the one thing the spec asks to get exactly right.
	if (bIAmCarrier)
	{
		// The mode decides which verb mouse1 has, so it decides which machine runs. Only ever one of
		// them: ScoringMode is latched for the match.
		if (bModeB)
		{
			UpdateThrow(DeltaSeconds);
		}
		else
		{
			UpdatePass(DeltaSeconds);
		}
	}
	else if (bModeB)
	{
		// MODE B. No dwell, so there is no "we were holding and it landed" case to detect: a throw
		// that leaves is a throw that happened, and its counter is incremented at the point of
		// release. All that is left is closing out an attempt that was still lining up when the Core
		// went somewhere else — killed mid-line-up, or intercepted after an earlier throw.
		//
		// (What became of a loose Core is deliberately NOT counted here. Ten bots would each count
		// the same pickup, and the Core already logs every one of them exactly once, with the team
		// and the grace decision attached: see the [ModeB] INTERCEPTION / RECOVERY lines.)
		bCommitCarryIn = false;

		// SPEC v13 §6: Charging counts here too, and it is the more important of the two. A bot killed
		// or intercepted mid-wind-up still has the button DOWN, and AbortThrow is what puts it back up.
		// Releasing at this point cannot launch anything - ATraceCore cleared the charge when it took
		// the Core off us, and refuses a release it has no matching press for - but leaving the latch
		// down would arm the next possession's first frame with a press nobody made.
		if (PassPhase == ETraceBotPassPhase::Lining || PassPhase == ETraceBotPassPhase::Charging)
		{
			TRACE_BOT_KIT(ThrowGiveUps);
			AbortThrow(TEXT("lost the Core while lining up or charging a throw"));
		}
	}
	else if (PassPhase != ETraceBotPassPhase::None && PassPhase != ETraceBotPassPhase::Cooldown)
	{
		// No longer carrying, mid-attempt. Two very different things look like this and they must be
		// counted apart, because the ratio between them is the headline number for whether hover
		// passing works at all:
		//
		//   * we were HOLDING and the intended receiver now has the Core — the dwell elapsed and the
		//     transfer landed. That is a completed pass;
		//   * anything else — somebody killed us, or an enemy took the Core off us during the window
		//     we deliberately opened. That is the risk side of the trade being collected, and it is
		//     supposed to happen sometimes.
		const ATraceCharacter* Receiver = PassReceiver.Get();
		const bool bCompleted = (PassPhase == ETraceBotPassPhase::Holding)
			&& Receiver != nullptr
			&& Receiver->IsCarrier();

		if (bCompleted)
		{
			TRACE_BOT_KIT(PassCompletions);
			UE_LOG(LogTraceGame, Display, TEXT("[BotPass] %s -> %s COMPLETED (hover hold)"),
				*GetNameSafe(GetPlayerState<APlayerState>()),
				*GetNameSafe(Receiver->GetPlayerState<APlayerState>()));
		}

		AbortPass(bCompleted ? TEXT("transfer landed") : TEXT("lost the Core mid-attempt"));
	}

	// SPEC v15 §3. Between the behaviours and the aim, for the ordering argument on UpdateAbilities().
	UpdateAutoCharacter();
	UpdateAbilities(DeltaSeconds);

	UpdateCombat(DeltaSeconds);
	UpdateMovementTech(DeltaSeconds);
	ApplySteering(DeltaSeconds);

	TraceBotAbilityStats::Poll(GetWorld());

#if !UE_BUILD_SHIPPING
	// Measurement only, and only when a run asked for it. See the TraceBotTelemetry block above for
	// why the acquisition chain is sampled here rather than inferred from the death log.
	if (TraceBotTelemetry::Enabled())
	{
		const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
		const FVector MyLocation = BotCharacter->GetActorLocation();

		const ATraceCharacter* NearestHuman = nullptr;
		float NearestHumanDist = -1.f;

		for (const ATraceCharacter* Other : LiveEnemies)
		{
			if (Other == nullptr || !Other->IsPlayerControlled())
			{
				continue;
			}

			const float D = static_cast<float>(FVector::Dist(MyLocation, Other->GetActorLocation()));
			if (NearestHuman == nullptr || D < NearestHumanDist)
			{
				NearestHuman = Other;
				NearestHumanDist = D;
			}
		}

		if (NearestHuman != nullptr)
		{
			const float EngageRange = FMath::Max(100.f, FMath::Min(Profile.SightRange, Profile.MaxEngagementRange));
			const bool bInSight  = NearestHumanDist <= Profile.SightRange;
			const bool bInEngage = NearestHumanDist <= EngageRange;
			const bool bLOS      = bInEngage && HasLineOfSight(NearestHuman);
			const bool bAiming   = bWantsToAim && (ShootTarget.Get() == NearestHuman);
			const bool bFiring   = bAiming && bTriggerHeld;

			TraceBotTelemetry::NoteBotTick(NearestHumanDist, bInSight, bInEngage, bLOS, bAiming, bFiring);
		}

		TraceBotTelemetry::Poll(GetWorld());
	}
#endif
}

// =================================================================================================
// World state
// =================================================================================================

void ATraceBotController::GatherWorldState()
{
	MyTeam = ETraceTeam::None;
	if (const ATracePlayerState* MyState = GetPlayerState<ATracePlayerState>())
	{
		MyTeam = MyState->Team;
	}

	Carrier = nullptr;
	bIAmCarrier = false;
	bTeammateIsCarrier = false;
	bEnemyIsCarrier = false;

	LiveTeammates.Reset();
	LiveEnemies.Reset();

	ATraceCharacter* BotCharacter = GetBotCharacter();

	// One late retry for the arena bounds. Everything positional in this class is a fraction of
	// them, so a bot that never resolves them plays the whole match on the fallback pitch — which
	// on a 24000-long arena means steering at a goal 8000uu short of the real one.
	if (!bBoundsValid)
	{
		if (const ATraceArenaBuilder* Builder = ATraceArenaBuilder::Get(GetWorld()))
		{
			FieldBounds = Builder->GetFieldBounds();
			bBoundsValid = (FieldBounds.IsValid != 0);
		}
	}

	NearestEnemy = nullptr;
	NearestEnemyDistSq = TNumericLimits<float>::Max();

	const ATraceGameMode* GameMode = GetTraceGameMode();
	if (GameMode == nullptr || BotCharacter == nullptr)
	{
		return;
	}

	// THE CORE IS A STATUS (spec §2), so the carrier is found by asking the PLAYERS, not by asking
	// the Core actor who it is attached to. That is not just tidier: it means this class has no
	// dependency at all on ATraceCore, so the Core degenerating to a cosmetic attachment — which is
	// exactly what the spec asks for — cannot break the bots. It also means "the Core" and "the
	// carrier" can never disagree here, because there is only one source for both.
	const FVector MyLocation = BotCharacter->GetActorLocation();
	for (const TWeakObjectPtr<ATraceCharacter>& WeakOther : GameMode->GetTrackedCharacters())
	{
		ATraceCharacter* Other = WeakOther.Get();
		if (Other == nullptr || !Other->IsAlive())
		{
			continue;
		}

		if (Other->IsCarrier())
		{
			Carrier = Other;
			if (Other == BotCharacter)
			{
				bIAmCarrier = true;
			}
			else if (MyTeam != ETraceTeam::None && Other->GetTeam() == MyTeam)
			{
				bTeammateIsCarrier = true;
			}
			else
			{
				bEnemyIsCarrier = true;
			}
		}

		if (Other == BotCharacter)
		{
			continue;
		}

		if (MyTeam != ETraceTeam::None && Other->GetTeam() == MyTeam)
		{
			LiveTeammates.Add(Other);
			continue;
		}

		LiveEnemies.Add(Other);

		const float DistanceSquared = static_cast<float>(FVector::DistSquared(MyLocation, Other->GetActorLocation()));
		if (DistanceSquared < NearestEnemyDistSq)
		{
			NearestEnemyDistSq = DistanceSquared;
			NearestEnemy = Other;
		}
	}

	// --- MODE B: is there a Core lying out in the world right now? -------------------------------
	//
	// This is the ONE place this class asks the Core actor anything about possession, and it is
	// unavoidable: "the Core is loose" is a fact about an object, and the roster scan above - which
	// deliberately asks the PLAYERS who is carrying, so that the bots do not depend on ATraceCore -
	// cannot see it, because while the Core is loose nobody is carrying. In mode A the answer is
	// always false and the bots' dependency on the Core is exactly what it was.
	bModeB = false;
	bCoreLoose = false;
	LooseCorePoint = FVector::ZeroVector;
	LooseCoreDistSq = 0.f;

	if (const ATraceCore* TheCore = ATraceCore::Get(GetWorld()))
	{
		bModeB = TheCore->IsModeB();
		if (bModeB)
		{
			// LEAD BY HOW LONG IT WILL ACTUALLY TAKE ME TO GET THERE, not by a fixed 0.25 s.
			//
			// The fixed lead was sized for "the Core is 40 uu further on by the time my movement input
			// lands", which is a correction for input latency and not for a chase. Spec v6 §4.1 makes
			// the difference matter: with a 500 uu catch zone, a bot only has to reach the FLIGHT PATH
			// rather than the Core itself, so a bot that runs at where the Core is now - and is
			// therefore always behind it - never contests a throw it could have. Leading by its own
			// time-to-arrive puts it on the arc.
			//
			// Two calls because the lead depends on the distance and the distance depends on the lead:
			// the first is the un-led position, which is close enough to size the second.
			FVector Instant = FVector::ZeroVector;
			if (TheCore->GetLooseCoreInterceptPoint(0.f, Instant))
			{
				const float RunSpeed = FMath::Max(200.f, UTraceSettings::Get().WalkSpeed);
				const float TimeToArrive = static_cast<float>(FVector::Dist2D(MyLocation, Instant)) / RunSpeed;

				// Capped: past about a second and a half the ballistic extrapolation is guessing at an
				// arc that a catch zone or a bounce will have changed anyway.
				bCoreLoose = TheCore->GetLooseCoreInterceptPoint(
					FMath::Clamp(TimeToArrive, 0.25f, 1.5f), LooseCorePoint);
			}

			if (bCoreLoose)
			{
				LooseCoreDistSq = static_cast<float>(FVector::DistSquared2D(MyLocation, LooseCorePoint));
			}
		}
	}

	// Sides switch at half time. Polling is a two-actor scan once a second, which is cheaper than
	// any scheme for detecting the switch would be, and it cannot miss one.
	const float Now = GetWorld() ? static_cast<float>(GetWorld()->GetTimeSeconds()) : 0.f;
	if (Now >= NextEndzoneResolveTime)
	{
		NextEndzoneResolveTime = Now + FMath::Max(0.1f, UTraceSettings::Get().BotEndzoneResolveInterval);
		ResolveAttackEndzone();
	}
}

void ATraceBotController::ResolveAttackEndzone()
{
	UWorld* World = GetWorld();
	if (World == nullptr || MyTeam == ETraceTeam::None)
	{
		return;
	}

	// ATraceEndzone::ScoresHere() is the authority on "does my team score by entering this box".
	// Asking it — rather than reproducing "Blue attacks +X" — is what makes the second half correct
	// no matter HOW the game mode implements the switch: swapping the zones' OwningTeam, moving the
	// zones, or re-assigning the spawns all produce the right answer here, because all three have to
	// end up changing what ScoresHere() returns or the human's own scoring would be wrong too.
	// IsZoneActive() is not optional now that BOTH shapes exist at once. ATraceArenaBuilder builds
	// the endzones AND the mode-B goals up front and arms whichever pair the scoring mode calls for,
	// so there are FOUR ATraceEndzone actors in the world and two of them are disarmed. Without this
	// test the loop takes whichever the actor iterator reaches first. It happens to come out right
	// today — the endzone and the goal at the same end share an OwningTeam and an X/Y centre — but
	// that is luck, not a rule, and it would break the moment the two shapes stop being concentric.
	const ATraceEndzone* Target = nullptr;
	for (TActorIterator<ATraceEndzone> It(World); It; ++It)
	{
		ATraceEndzone* Zone = *It;
		if (IsValid(Zone) && Zone->IsZoneActive() && Zone->ScoresHere(MyTeam))
		{
			Target = Zone;
			break;
		}
	}

	if (Target == nullptr)
	{
		// Fall back to the geometric rule, but say so — loudly and once. A silent fallback here is
		// precisely the "bot runs at the wrong endzone in the second half" bug, and it would look
		// like broken steering rather than like a missing actor.
		if (!bWarnedNoEndzone)
		{
			bWarnedNoEndzone = true;
			TRACE_BOT_KIT(EndzoneUnresolved);
			UE_LOG(LogTraceGame, Warning,
				TEXT("[BotEndzone] %s (team %d) found no ATraceEndzone that it scores in; falling back to the ")
				TEXT("static 'Blue attacks +X' rule, which does NOT survive the half-time side switch."),
				*GetNameSafe(GetPlayerState<APlayerState>()), static_cast<int32>(MyTeam));
		}

		bAttackGoalValid = false;
		return;
	}

	FVector Centre = (Target->Trigger != nullptr)
		? Target->Trigger->GetComponentLocation()
		: Target->GetActorLocation();

	// --- MODE B: the scoring volume is the GOAL, not the endzone. ---------------------------------
	//
	// Steering at the endzone centre in mode B would be wrong in a way that is easy to miss: the two
	// share a centre line, so a bot would still look like it was attacking correctly while running
	// at a box the rule no longer scores. Ask the Core for the goal mouth, which is answered from
	// the very same boxes ATraceCore::CheckGoalScore awards points off - the same equivalence
	// argument as asking ATraceEndzone::ScoresHere above, one layer along.
	//
	// Also resolve the goal this team DEFENDS, which is the goal our OPPONENT scores in. Mode A has
	// no such thing (the endzone is the full width of the field, so there is no mouth to stand in
	// front of) and leaves bDefendGoalValid false.
	bDefendGoalValid = false;
	if (bModeB)
	{
		FVector GoalCentre = FVector::ZeroVector;
		if (ATraceCore::GetAttackGoalCentre(World, MyTeam, GoalCentre))
		{
			Centre = GoalCentre;
		}

		FVector OwnGoalCentre = FVector::ZeroVector;
		if (ATraceCore::GetAttackGoalCentre(World, TraceOpposingTeam(MyTeam), OwnGoalCentre))
		{
			DefendGoalCentre = OwnGoalCentre;
			bDefendGoalValid = true;
		}
	}

	AttackGoalCentre = Centre;
	bAttackGoalValid = true;
	bWarnedNoEndzone = false;

	const float FieldCentreX = bBoundsValid ? static_cast<float>(FieldBounds.GetCenter().X) : 0.f;
	AttackSideSign = (Centre.X >= FieldCentreX) ? 1.f : -1.f;

	if (AttackSideSign != LoggedAttackSideSign)
	{
		const bool bIsFlip = (LoggedAttackSideSign != 0.f);
		LoggedAttackSideSign = AttackSideSign;

		if (bIsFlip)
		{
			TRACE_BOT_KIT(EndzoneFlips);
		}

		// Display, not Verbose, and deliberately one line per bot per flip. Ten lines at half time is
		// not spam, it is the proof that all ten players turned round — which is the single piece of
		// evidence this pass was asked for that cannot be inferred from anything else.
		UE_LOG(LogTraceGame, Display,
			TEXT("[BotEndzone] %s (team %d) %s attacking endzone at X=%.0f Y=%.0f (side %+.0f)"),
			*GetNameSafe(GetPlayerState<APlayerState>()),
			static_cast<int32>(MyTeam),
			bIsFlip ? TEXT("SWITCHED to") : TEXT("is"),
			Centre.X, Centre.Y, AttackSideSign);
	}
}

void ATraceBotController::DecideState()
{
	const ETraceBotState Previous = State;

	if (bIAmCarrier)
	{
		State = ETraceBotState::CarryToGoal;
	}
	else if (bCoreLoose)
	{
		// MODE B ONLY (bCoreLoose is false for the whole match in mode A).
		//
		// A loose Core outranks everything except carrying one, for both teams at once: "the first
		// player to contact the core should pick it up", so whoever gets there decides the next
		// possession. That makes it the most valuable thing on the field and the one moment where
		// standing off is strictly wrong.
		//
		// But not EVERYBODY sprints at it. Five bots converging on one point is a scrum that leaves
		// the goal wide open behind them, and the ranking below is the same distance ordering the
		// interceptor/punisher split uses: consistent across bots with no shared table, because every
		// bot computes it from the same replicated positions. The closest few contest; the rest fall
		// back and cover the mouth.
		int32 CloserTeammates = 0;
		for (const ATraceCharacter* Mate : LiveTeammates)
		{
			if (Mate == nullptr)
			{
				continue;
			}
			if (FVector::DistSquared2D(Mate->GetActorLocation(), LooseCorePoint) < LooseCoreDistSq)
			{
				++CloserTeammates;
			}
		}

		State = (CloserTeammates < TraceBotConstants::LooseCoreChasers)
			? ETraceBotState::ChaseLooseCore
			: (ShouldHoldGoalDefence() ? ETraceBotState::DefendGoal : ETraceBotState::Fight);
	}
	else if (bEnemyIsCarrier)
	{
		// Two jobs, not one, and both are needed for the spec's central loop to close.
		//
		// A SHIELDED carrier cannot be shot, so the only unilateral answer is to cross their trace
		// and dash through it — that is the interceptors. But the carrier also has a second, chosen
		// moment of weakness: the half second of a hover pass, when the shield drops. Somebody has to
		// be pointing a gun at them when that happens, or passing is a free reset and the whole
		// risk/reward beat the design is built on never costs anything.
		//
		// Both roles are rank-based off the same distance ordering, so they are consistent across
		// bots with no shared table: the closest InterceptorCount hunt, the next PunisherCount hold a
		// bead, and the rest go back to fighting the escorts. Sending everyone at the trace was the
		// old behaviour, and it left the escorts entirely uncovered.
		const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
		const int32 Rank = GetCarrierPressureRank();
		const int32 Interceptors = FMath::Max(0, Profile.InterceptorCount);
		const int32 Punishers = FMath::Max(0, Profile.PunisherCount);

		if (Rank == INDEX_NONE)
		{
			// Too far from the carrier, or unable to see them. In mode A there is nothing to guard —
			// the endzone is the whole width of the field — so the honest answer is to keep fighting.
			// In mode B there IS a mouth to stand in front of, and the players who cannot reach the
			// carrier are exactly the ones who should be standing in it — but only the nearest couple
			// of them. MEASURED: without the ShouldHoldGoalDefence cap this branch caught almost the
			// whole team (12358 goal-defence bot-ticks against 877 spent chasing in a 60 s sample, on
			// a 33600 uu field where most players are out of carrier range most of the time), which
			// reads as a side that has stopped playing.
			State = ShouldHoldGoalDefence() ? ETraceBotState::DefendGoal : ETraceBotState::Fight;
		}
		else if (Rank < Interceptors)
		{
			State = ETraceBotState::HuntCarrier;
		}
		else if (Rank < Interceptors + Punishers)
		{
			State = ETraceBotState::PunishPasser;
		}
		else
		{
			State = ETraceBotState::Fight;
		}
	}
	else if (bTeammateIsCarrier)
	{
		State = ETraceBotState::EscortCarrier;
	}
	else if (NearestEnemy.IsValid())
	{
		// Nobody holds the Core. Under spec v2 this is a short window — the carrier just died and
		// the grant to their killer has not landed, or the match has not started — so the right move
		// is to keep fighting rather than to run at an object that no longer exists.
		State = ETraceBotState::Fight;
	}
	else
	{
		State = ETraceBotState::Regroup;
	}

	if (State != Previous && State != LastLoggedState)
	{
		LastLoggedState = State;
		UE_LOG(LogTraceGame, Verbose, TEXT("[Bot] %s -> %s"),
			*GetNameSafe(GetPlayerState<APlayerState>()), StateToString(State));
	}
}

bool ATraceBotController::ShouldHoldGoalDefence() const
{
	// Mode A has no goal mouth to stand in front of, so nobody ever holds this station there.
	if (!bModeB || !bDefendGoalValid)
	{
		return false;
	}

	const ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return false;
	}

	// The same distance-ranking scheme as the interceptor/punisher split and the loose-Core chase:
	// every bot computes the same ordering from the same positions, so the defence assigns itself
	// with no shared table and no arbitration pass. Ranked on distance to OUR OWN goal, which is the
	// thing being guarded — ranking on distance to the threat would send whoever happened to be
	// nearest the ball back to defend, which is the opposite of the intent.
	const float MyDistSq =
		static_cast<float>(FVector::DistSquared2D(BotCharacter->GetActorLocation(), DefendGoalCentre));

	int32 CloserTeammates = 0;
	for (const ATraceCharacter* Mate : LiveTeammates)
	{
		if (Mate != nullptr
			&& FVector::DistSquared2D(Mate->GetActorLocation(), DefendGoalCentre) < MyDistSq)
		{
			++CloserTeammates;
		}
	}

	return CloserTeammates < TraceBotConstants::GoalDefenders;
}

int32 ATraceBotController::GetCarrierPressureRank() const
{
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	const ATraceCharacter* EnemyCarrier = Carrier.Get();
	if (BotCharacter == nullptr || EnemyCarrier == nullptr)
	{
		return INDEX_NONE;
	}

	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();

	// "Near the carrier" is two separate limits multiplied together.
	//
	// The field fraction is what keeps the role meaningful when the arena changes size — a constant
	// 2000uu radius would mean almost nobody qualifies on a 24000-long pitch and the mechanic
	// disappears the moment the arena grows.
	//
	// The SightRange cap is what keeps it FAIR. With geometry alone, every defender within a third
	// of the arena set off after a carrier they had no way of seeing, so a pickup was answered by
	// the entire enemy team converging from all sides at once: measured at 74% of all deaths and
	// zero captures in a full minute. A bot that cannot see the carrier now keeps doing its own job.
	const float Radius = FMath::Min(
		HalfFieldLength() * FMath::Max(0.f, UTraceSettings::Get().BotInterceptRadiusFieldFraction),
		FMath::Max(100.f, Profile.SightRange));
	const float RadiusSq = FMath::Square(Radius);

	const FVector CarrierLocation = EnemyCarrier->GetActorLocation();
	const float MyDistSq = static_cast<float>(FVector::DistSquared2D(BotCharacter->GetActorLocation(), CarrierLocation));

	if (MyDistSq > RadiusSq)
	{
		return INDEX_NONE;
	}

	// Rank by distance among the teammates who are also in range. No shared table and no arbitration
	// pass: every bot computes the same ordering from the same replicated positions, so the set is
	// consistent without anyone owning it.
	int32 CloserTeammates = 0;
	for (const ATraceCharacter* Mate : LiveTeammates)
	{
		if (Mate == nullptr)
		{
			continue;
		}

		const float MateDistSq = static_cast<float>(FVector::DistSquared2D(Mate->GetActorLocation(), CarrierLocation));
		if (MateDistSq <= RadiusSq && MateDistSq < MyDistSq)
		{
			++CloserTeammates;
		}
	}

	return CloserTeammates;
}

// =================================================================================================
// Dash charges  (spec §5: the carrier gets a second charge, both on a 4 s cooldown)
// =================================================================================================

int32 ATraceBotController::GetDashCharges() const
{
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return 0;
	}

	const UTraceCharacterMovementComponent* Movement = BotCharacter->GetTraceMovement();
	if (Movement == nullptr)
	{
		return 0;
	}

	// Authoritative answer, when the movement component has one.
	const int32 Reported = TraceBotPawnAPI::DashCharges(Movement, 0);
	if (Reported >= 0)
	{
		return Reported;
	}

	// Shadow model. Exact rather than approximate, because this controller is the only thing that
	// ever asks this pawn to dash — so the count of outstanding charges really is the count of
	// dashes it has issued and not yet seen refill. Still gated on the component's own CanDash(),
	// which is what stops a bot from believing it has a charge while mid-dash or mid-fall.
	if (!Movement->CanDash())
	{
		return 0;
	}

	const int32 MaxCharges = bIAmCarrier ? 2 : 1;
	return FMath::Max(0, MaxCharges - SpentDashCharges);
}

void ATraceBotController::TickDashCharges(float DeltaSeconds)
{
	if (SpentDashCharges <= 0)
	{
		NextDashRefillTime = 0.f;
		return;
	}

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const float Now = static_cast<float>(World->GetTimeSeconds());
	const float Cooldown = FMath::Max(0.1f, UTraceSettings::Get().BotDashCooldownSeconds);

	if (NextDashRefillTime <= 0.f)
	{
		NextDashRefillTime = Now + Cooldown;
		return;
	}

	if (Now >= NextDashRefillTime)
	{
		--SpentDashCharges;
		// Charges refill one at a time on the same cooldown, so a carrier that spends both waits
		// four seconds for the first and eight for the second — which is what makes holding one in
		// reserve a real decision rather than a formality.
		NextDashRefillTime = (SpentDashCharges > 0) ? (Now + Cooldown) : 0.f;
	}

	// Losing the Core drops the maximum from two to one, so an outstanding second charge has to be
	// forgotten or the bot spends the rest of its life believing it owes a charge it can never repay.
	const int32 MaxCharges = bIAmCarrier ? 2 : 1;
	SpentDashCharges = FMath::Min(SpentDashCharges, MaxCharges);
}

bool ATraceBotController::RequestDash(bool bReserveLast)
{
	const int32 Charges = GetDashCharges();
	const int32 Needed = bReserveLast ? 2 : 1;

	if (Charges < Needed)
	{
		return false;
	}

	bWantsDashThisTick = true;
	++SpentDashCharges;
	return true;
}

// =================================================================================================
// Movement intent
// =================================================================================================

void ATraceBotController::UpdateMovementIntent(float DeltaSeconds)
{
	switch (State)
	{
	case ETraceBotState::CarryToGoal:   BehaviourCarryToGoal(DeltaSeconds);   break;
	case ETraceBotState::HuntCarrier:   BehaviourHuntCarrier(DeltaSeconds);   break;
	case ETraceBotState::PunishPasser:  BehaviourPunishPasser(DeltaSeconds);  break;
	case ETraceBotState::EscortCarrier: BehaviourEscortCarrier(DeltaSeconds); break;
	case ETraceBotState::Regroup:       BehaviourRegroup(DeltaSeconds);       break;
	case ETraceBotState::Fight:         BehaviourFight(DeltaSeconds);         break;
	case ETraceBotState::ChaseLooseCore: BehaviourChaseLooseCore(DeltaSeconds); break;
	case ETraceBotState::DefendGoal:    BehaviourDefendGoal(DeltaSeconds);    break;
	default:                            DesiredMoveDirection = FVector::ZeroVector; break;
	}
}

void ATraceBotController::BehaviourCarryToGoal(float DeltaSeconds)
{
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return;
	}

	const FVector MyLocation = BotCharacter->GetActorLocation();

	// SPEC v5 §4. Once committed to carrying it in, run at the CENTRE of the mouth rather than at
	// this bot's own attack lane, and stop weaving.
	//
	// Both of those are corrections to behaviour that is right in mode A and wrong at a 2000 uu goal.
	// GetAttackGoalLocation() spreads bots across ±BotAttackLaneFieldFraction of the field half-width
	// so they do not funnel down the middle of a FULL-WIDTH endzone, and the weave adds ±0.45 of
	// lateral steering on top so the trace a carrier lays is not a straight line. Against a mouth
	// only ±1000 uu wide, both of those aim the carrier at the goal POST.
	const bool bDriveAtMouth = bCommitCarryIn;
	FVector Goal = GetAttackGoalLocation();

	if (bDriveAtMouth)
	{
		FVector GoalCentre = FVector::ZeroVector;
		if (ATraceCore::GetAttackGoalCentre(GetWorld(), MyTeam, GoalCentre))
		{
			Goal = GoalCentre;
		}
		TRACE_BOT_KIT(CarryInTicks);
	}

	FVector ToGoal = Goal - MyLocation;
	ToGoal.Z = 0.f;
	DesiredMoveDirection = ToGoal.GetSafeNormal();

	// Weave: a carrier running dead straight is trivially intercepted, and the trace it lays is a
	// straight line that a defender only has to touch once. Suppressed on the final approach - see
	// above; a weave that costs 400 uu of lateral drift is the difference between the net and a post.
	if (!bDriveAtMouth && !DesiredMoveDirection.IsNearlyZero())
	{
		const FVector Right = FVector::CrossProduct(FVector::UpVector, DesiredMoveDirection);
		const float Weave = FMath::Sin(static_cast<float>(GetWorld()->GetTimeSeconds()) * 1.4f + FormationBias * 3.f) * 0.45f;
		DesiredMoveDirection = (DesiredMoveDirection + Right * Weave).GetSafeNormal();
	}

	// Never shoot while carrying — mouse1 is the PASS now, and the weapon refuses anyway.
	bWantsToAim = false;

	// --- The two dash charges, and what each one is for ------------------------------------------
	//
	// Spec §5 gives the carrier a second charge. Spending both on the same impulse would waste the
	// gift: the reason a carrier wants two is that the first can be spent making ground while the
	// second is still there for the moment a defender lines up on the trace. So:
	//
	//   * ESCAPE (reserve nothing). An enemy inside the panic radius is the emergency the second
	//     charge exists for, so this is allowed to take the last one.
	//   * ADVANCE (reserve one). A clear run at the goal is worth a charge, but never the last one.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
	const float Aggression = FMath::Clamp(Profile.Aggression, 0.f, 1.f);
	const float PanicRadius = FMath::Max(0.f, Settings.BotCarrierPanicRadius) * (0.55f + 0.75f * Aggression);

	const bool bThreatened = NearestEnemy.IsValid() && NearestEnemyDistSq < FMath::Square(PanicRadius);

	if (bThreatened)
	{
		if (RequestDash(/*bReserveLast=*/false))
		{
			TRACE_BOT_KIT(EscapeDashes);
		}
	}
	else if (!IsPassing())
	{
		// Do not burn a charge mid-pass: the dash would take the crosshair off the receiver and
		// abort the very thing it was meant to protect.
		const float GoalDistance = static_cast<float>(FVector::Dist2D(MyLocation, Goal));
		const float SprintBand = HalfFieldLength() * 0.55f;

		// On a committed carry-in the run at the mouth IS the play, so it is worth spending charges
		// on much more freely - and worth spending the last one, because a carrier that reaches the
		// mouth and scores does not need an escape charge afterwards.
		const float DashUrgency = bDriveAtMouth ? 5.f : 1.5f;

		if (GoalDistance < SprintBand && FMath::FRand() < Aggression * DeltaSeconds * DashUrgency)
		{
			if (RequestDash(/*bReserveLast=*/!bDriveAtMouth))
			{
				TRACE_BOT_KIT(EscapeDashes);
			}
		}
	}
}

void ATraceBotController::BehaviourHuntCarrier(float DeltaSeconds)
{
	ATraceCharacter* BotCharacter = GetBotCharacter();
	ATraceCharacter* EnemyCarrier = Carrier.Get();
	if (BotCharacter == nullptr || EnemyCarrier == nullptr)
	{
		return;
	}

	// Only the difficulty-independent settings are read here. The one profile knob that governs
	// intercepting — TrailDashCommitChance — is consumed once per dash charge in Tick(), not here.
	const UTraceSettings& Settings = UTraceSettings::Get();

	const FVector MyLocation = BotCharacter->GetActorLocation();

	FVector TrailPoint = FVector::ZeroVector;
	FVector TrailTangent = FVector::ZeroVector;
	const bool bHaveTrail = FindTrailInterceptPoint(TrailPoint, TrailTangent);

	TRACE_BOT_KIT(HuntTicks);
	if (bHaveTrail)
	{
		TRACE_BOT_KIT(HuntTicksWithPlan);
	}
	else
	{
		TRACE_BOT_KIT(HuntTicksNoPlan);
	}

	// An interceptor has one job. But if the carrier's shield happens to be DOWN — they chose to
	// pass while we were closing — then take the shot as well: killing a passing carrier is one of
	// the three legal ways to take the Core (spec §2), and refusing it because "this bot is an
	// interceptor" would be pedantry.
	if (!IsCarrierShielded(EnemyCarrier) && HasLineOfSight(EnemyCarrier))
	{
		ShootTarget = EnemyCarrier;
		bWantsToAim = true;
		DesiredAimPoint = GetAimPointOn(EnemyCarrier);
	}
	else
	{
		bWantsToAim = false;
	}

	if (!bHaveTrail)
	{
		// ------------------------------------------------------------------------------------------
		// No usable trace yet — the carrier has only just taken it, or is standing still and laying
		// nothing. SPEC v9 §1: THIS BRANCH IS THE REPORTED BUG, so it does not get to be a pursuit.
		//
		// It used to steer at `carrier + velocity * 0.18 x half-field`. Two failure modes, and the
		// report describes both: the lead point is glued to the carrier's velocity, so the bot copies
		// every turn they make; and when the carrier slows the lead collapses onto the carrier and
		// the bot walks up and stands next to them. Neither ever crosses the trace, because the trace
		// is behind the carrier and this aimed in front of them.
		//
		// The replacement runs at the line the trace is ABOUT to occupy — a fixed gap behind the
		// carrier, offset a dash's reach to one side — so the bot is waiting beside the trace, facing
		// across it, at the moment there is one to cross.
		// ------------------------------------------------------------------------------------------
		FVector Station = FVector::ZeroVector;
		if (!BotInterceptFixEnabled())
		{
			// The v8 pursuit, kept behind `Trace.Bot.InterceptFix 0` so the reported symptom can be
			// reproduced on demand from this build.
			const FVector CarrierVelocity = EnemyCarrier->GetVelocity();
			const float LeadDistance = HalfFieldLength() * FMath::Max(0.f, Settings.BotEscortLeadFieldFraction);
			const FVector LeadPoint = EnemyCarrier->GetActorLocation() + CarrierVelocity.GetSafeNormal2D() * LeadDistance;

			FVector ToLead = LeadPoint - MyLocation;
			ToLead.Z = 0.f;
			DesiredMoveDirection = ToLead.GetSafeNormal();
		}
		else if (FindCutBehindStation(Station))
		{
			FVector ToStation = Station - MyLocation;
			ToStation.Z = 0.f;
			DesiredMoveDirection = ToStation.GetSafeNormal();
		}
		else
		{
			// Nothing to cut behind at all (no facing, no velocity, no resolved end). Close the
			// distance so that whatever happens next happens within reach.
			FVector ToCarrier = EnemyCarrier->GetActorLocation() - MyLocation;
			ToCarrier.Z = 0.f;
			DesiredMoveDirection = ToCarrier.GetSafeNormal();
		}
		return;
	}

	// ---------------------------------------------------------------------------------------------
	// The crossing.
	//
	// The old code steered AT the nearest trace point and dashed once it was 120-420uu away. Two
	// things were wrong with that, and together they are most of why the signature kill was 1.3% of
	// deaths:
	//
	//   * Steering at a point on a wall parks you against the wall. The server's trip test is a
	//     swept segment-vs-segment test — it wants you to CROSS the trace, and a bot that arrives
	//     alongside it and dashes forward runs parallel to the thing it is trying to hit.
	//   * The 120uu inner bound was a dead zone. A bot that walked all the way in — which is what
	//     steering at the point makes it do — ended up inside it and then could never dash at all.
	//
	// So: decompose the vector to the point into components along and across the trace, aim PAST
	// the trace along the perpendicular, and gate the dash on how far the bot must actually travel
	// to get through.
	// ---------------------------------------------------------------------------------------------

	FVector ToPoint = TrailPoint - MyLocation;
	ToPoint.Z = 0.f;

	const FVector Along = TrailTangent * FVector::DotProduct(ToPoint, TrailTangent);
	const FVector Across = ToPoint - Along;
	const float PerpDistance = static_cast<float>(Across.Size());

	FVector CrossDirection;
	if (PerpDistance > TraceBotConstants::TrailPerpDegenerate)
	{
		CrossDirection = Across / PerpDistance;
	}
	else
	{
		// Standing on the line: there is no "toward the trace", so pick a side and cross sideways.
		CrossDirection = FVector::CrossProduct(FVector::UpVector, TrailTangent).GetSafeNormal() * StrafeSign;
	}

	const float Overshoot = FMath::Max(0.f, Settings.BotTrailCrossOvershoot);
	const float DashRange = FMath::Max(50.f, Settings.BotTrailDashRange);

	TRACE_BOT_HUNT_PERP(PerpDistance);
	if (PerpDistance <= DashRange)
	{
		TRACE_BOT_KIT(HuntTicksInDashRange);
	}

	// ---------------------------------------------------------------------------------------------
	// SPEC v9 §1: THE POCKET.
	//
	// Whether this bot is allowed to cross the line right now is decided BEFORE the steering, not
	// after it, because it changes where the bot should be standing.
	//
	//   * With a charge it may spend: aim PAST the trace, so the dash sweeps through it. (The old
	//     code did this unconditionally.)
	//   * Without one: hold just inside dash range on ITS OWN side of the line and pace along it.
	//
	// The second case is not caution, it is the difference between a defender and a bystander.
	// Walking through a trace does nothing — only a dash trips it — so a hunter with its dash on
	// cooldown that keeps steering at the through-point walks straight over the line, comes out the
	// far side, turns round, and comes back. From the outside that is a bot milling around next to
	// the carrier, which is half of what the report describes. Holding the pocket means the charge
	// is spent the frame it lands, from a position the bot is already in.
	// ---------------------------------------------------------------------------------------------
	const bool bMayCross = !BotInterceptFixEnabled()
		|| ((GetDashCharges() > 0) && bCommitDashWithThisCharge);

	FVector Steer;
	if (bMayCross)
	{
		const FVector ThroughPoint = TrailPoint + CrossDirection * Overshoot;
		Steer = ThroughPoint - MyLocation;
		Steer.Z = 0.f;
	}
	else
	{
		const float Pocket = DashRange * TraceBotConstants::TrailPocketDashRangeFraction;
		const FVector PocketPoint = TrailPoint - CrossDirection * Pocket;

		Steer = PocketPoint - MyLocation;
		Steer.Z = 0.f;

		// Pace ALONG the trace toward the carrier while holding. TrailTangent runs from the chosen
		// point toward the head, i.e. toward the carrier, so this keeps the bot level with the run
		// instead of being left behind by it — and a bot that has arrived at its pocket has a
		// direction to move in rather than standing still.
		//
		// SPEC v9 §1: CLAMPED BY HOW FAR AHEAD THE POINT ACTUALLY IS. Unclamped this is a constant
		// 280uu shove up the trace, and `Steer` is a difference vector that goes to zero as the bot
		// arrives — so an arrived bot steered almost purely along the tangent, ran up the trace,
		// past the head and into the carrier, then turned round and came back. That IS "following
		// the exact motion of the carrier and standing by it": the pocket, which exists to stop the
		// bot walking uselessly through the trace, was generating the other half of the same
		// symptom. `Ahead` is positive only while the chosen point is still in front of us along
		// the trace, so the pace dies exactly when the bot is level with it and reverses if it has
		// overrun.
		const float PaceWeight = TraceBotConstants::TrailPocketPaceWeight;
		if (BotInterceptCommitFixEnabled())
		{
			const float Ahead = static_cast<float>(FVector::DotProduct(ToPoint, TrailTangent));
			Steer += TrailTangent * FMath::Clamp(Ahead, -PaceWeight, PaceWeight);
		}
		else
		{
			Steer += TrailTangent * PaceWeight;
		}
	}

	DesiredMoveDirection = Steer.GetSafeNormal();

	if (GetDashCharges() <= 0)
	{
		TRACE_BOT_KIT(DashBlockedNoCharge);
		return;
	}

	if (!bCommitDashWithThisCharge)
	{
		TRACE_BOT_KIT(DashBlockedNoCommit);
		return;
	}

	// ---------------------------------------------------------------------------------------------
	// When to actually spend the dash.
	//
	// NOT "am I within N uu of the trace". The dash travels along DesiredMoveDirection, which points
	// at the through-point and is therefore usually oblique to the trace — so the distance it has to
	// cover to reach it is the perpendicular gap divided by how much of the heading is
	// perpendicular. Gating on the raw perpendicular gap instead overstates the dash's reach by a
	// factor of 1/CrossFraction: a bot 380uu away but approaching at 35 degrees needs 660uu of
	// travel and only has 468uu of dash, so it fires, falls short, and eats the cooldown having
	// achieved nothing. Doing that once per approach is enough to hide the mechanic.
	// ---------------------------------------------------------------------------------------------
	const float CrossFraction = static_cast<float>(FVector::DotProduct(DesiredMoveDirection, CrossDirection));
	if (CrossFraction <= TraceBotConstants::TrailDashAlignmentDot)
	{
		TRACE_BOT_KIT(DashBlockedParallel);
		return;   // Running nearly parallel to the trace; closing further is better than dashing.
	}

	const float DistanceToCross = PerpDistance / CrossFraction;

	// Dash reach is DashSpeed * DashDuration (~468uu by default). BotTrailDashRange must stay under
	// it, or the dash stops short of the trace it is aimed at.
	if (DistanceToCross > DashRange)
	{
		TRACE_BOT_KIT(DashBlockedRange);
		return;
	}

	// BeginDash locks the direction from the current acceleration, so a dash fired while still
	// turning goes somewhere useless. A stationary bot is exempt: it has no heading to disagree
	// with, and this tick's AddMovementInput is what the dash will read.
	const FVector Heading = BotCharacter->GetVelocity().GetSafeNormal2D();
	const bool bAligned = Heading.IsNearlyZero()
		|| FVector::DotProduct(Heading, DesiredMoveDirection) > TraceBotConstants::TrailDashAlignmentDot;

	if (!bAligned)
	{
		TRACE_BOT_KIT(DashBlockedAlign);
		return;
	}

	// Never reserve here. Breaking the trace is the highest-value thing any dash in this game can
	// do — it kills the carrier AND transfers the Core in one action — so a defender in position
	// with a charge in hand spends it.
	// NOT cleared here any more — see the roll block in Tick(). RequestDash raises an INTENT, and
	// the pawn is allowed to refuse it; clearing the commit on the request meant one refused dash
	// disarmed this bot permanently. The charge count coming back up is what ends this charge.
	if (RequestDash(/*bReserveLast=*/false))
	{
		TRACE_BOT_KIT(TraceDashes);

		UE_LOG(LogTraceGame, Verbose, TEXT("[BotIntercept] %s dashing across trace: perp %.0fuu, travel %.0fuu"),
			*GetNameSafe(GetPlayerState<APlayerState>()), PerpDistance, DistanceToCross);
	}
}

void ATraceBotController::BehaviourPunishPasser(float DeltaSeconds)
{
	ATraceCharacter* BotCharacter = GetBotCharacter();
	ATraceCharacter* EnemyCarrier = Carrier.Get();
	if (BotCharacter == nullptr || EnemyCarrier == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	const FVector MyLocation = BotCharacter->GetActorLocation();
	const FVector CarrierLocation = EnemyCarrier->GetActorLocation();

	FVector ToCarrier = CarrierLocation - MyLocation;
	ToCarrier.Z = 0.f;
	const float Distance = static_cast<float>(ToCarrier.Size());
	const FVector Forward = ToCarrier.GetSafeNormal();

	// Hold a standoff rather than closing all the way in. A punisher that walks onto the carrier is
	// standing in its own interceptors' crossing lanes, and is the first thing a dashing carrier
	// gets away from.
	const float Range = FMath::Max(200.f, Settings.BotPunishRange);
	const float Standoff = Range * FMath::Clamp(Settings.BotPunishStandoffFraction, 0.1f, 1.f);
	const float Deadzone = Standoff * 0.25f;

	const float Now = GetWorld() ? static_cast<float>(GetWorld()->GetTimeSeconds()) : 0.f;
	if (Now >= StrafeFlipTime)
	{
		StrafeFlipTime = Now + FMath::FRandRange(TraceBotConstants::StrafeFlipMin, TraceBotConstants::StrafeFlipMax);
		StrafeSign = -StrafeSign;
	}
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward) * StrafeSign;

	if (Distance > Standoff + Deadzone)
	{
		DesiredMoveDirection = (Forward + Right * 0.3f).GetSafeNormal();
	}
	else if (Distance < Standoff - Deadzone)
	{
		DesiredMoveDirection = (-Forward * 0.7f + Right * 0.7f).GetSafeNormal();
	}
	else
	{
		DesiredMoveDirection = Right;
	}

	// --- The bet ---------------------------------------------------------------------------------
	//
	// Aim at the carrier and be ready. Bullets do nothing while the shield is up, and the pass window
	// is half a second — far shorter than this bot's reaction delay — so a punisher that waited to
	// SEE the shield drop before acquiring would never once land a shot. It has to already be on
	// target when it happens, which means acquiring a target it currently cannot hurt.
	//
	// That is the whole trick, and it is why this is a separate state rather than a special case
	// inside Fight: FindBestShootTarget() correctly refuses a shielded carrier, because for every
	// other bot on the field spending a reaction window on an invulnerable target is strictly worse
	// than shooting someone killable.
	//
	// The trigger is gated on ATraceCore::IsShieldSuppressedFor in UpdateCombat, so a punisher holds
	// its fire while the shield is up and opens the moment the pass drops it. Holding fire rather
	// than spraying matters: pulling the trigger at an invulnerable target would consume this bot's
	// burst and rest window on nothing, and it would be resting at exactly the instant the half
	// second it has been waiting for arrives.
	if (Distance <= Range && HasLineOfSight(EnemyCarrier))
	{
		ShootTarget = EnemyCarrier;
		bWantsToAim = true;
		DesiredAimPoint = GetAimPointOn(EnemyCarrier);
		TRACE_BOT_KIT(PunisherTicks);
	}
	else if (ATraceCharacter* Other = FindBestShootTarget())
	{
		// Carrier is behind cover or out of range: do not stand there doing nothing.
		ShootTarget = Other;
		bWantsToAim = true;
		DesiredAimPoint = GetAimPointOn(Other);
	}
}

void ATraceBotController::BehaviourEscortCarrier(float DeltaSeconds)
{
	ATraceCharacter* BotCharacter = GetBotCharacter();
	ATraceCharacter* FriendlyCarrier = Carrier.Get();
	if (BotCharacter == nullptr || FriendlyCarrier == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	const FVector CarrierLocation = FriendlyCarrier->GetActorLocation();
	const FVector Goal = GetAttackGoalLocation();

	FVector CarrierToGoal = Goal - CarrierLocation;
	CarrierToGoal.Z = 0.f;
	CarrierToGoal = CarrierToGoal.GetSafeNormal();

	const FVector Right = FVector::CrossProduct(FVector::UpVector, CarrierToGoal);
	const float Spread = HalfFieldWidth() * FMath::Max(0.f, Settings.BotFormationSpreadFieldFraction);

	// Roughly half the escorts break deep instead of screening.
	//
	// This is not decoration, it is what makes a pass reachable at all. The screening station sits
	// ahead of the carrier — but the carrier moves at WalkSpeed * CarrierSpeedMultiplier and an
	// escort moves at plain WalkSpeed, so a screener can never actually reach a point ahead of the
	// player it is chasing. With every escort screening, no teammate is ever closer to the goal than
	// the carrier and there is never a legal receiver: measured at zero passes across fifty runs.
	//
	// PersonalitySkillBias is fixed for the life, so a bot does not flip role mid-run and jitter
	// between two stations.
	const bool bDeepRunner = (PersonalitySkillBias > 0.5f);

	const float LeadDistance = HalfFieldLength() * FMath::Max(0.f, Settings.BotEscortLeadFieldFraction);
	const float DeepStandoff = HalfFieldLength() * FMath::Max(0.f, Settings.BotDeepRunnerStandoffFieldFraction);

	FVector Station = bDeepRunner
		? (Goal - CarrierToGoal * DeepStandoff + Right * FormationBias * Spread * 0.6f)
		: (CarrierLocation + CarrierToGoal * LeadDistance + Right * FormationBias * Spread);

	// --- BE CATCHABLE ----------------------------------------------------------------------------
	//
	// The pass now requires the carrier's crosshair to be ON the receiver, with line of sight, for
	// half a second. That turns "get open" from a nicety into a hard requirement: a receiver behind
	// a pillar is not a receiver at all, however well positioned it is, and under the old thrown-Core
	// rules it still sort of was.
	//
	// So a deep runner that has broken line of sight with its own carrier slides back along the line
	// to them until it is visible again. It costs a little field position and it is the difference
	// between a station and an option.
	if (bDeepRunner && !HasLineOfSight(FriendlyCarrier))
	{
		const FVector Toward = (CarrierLocation - BotCharacter->GetActorLocation()).GetSafeNormal2D();
		Station = BotCharacter->GetActorLocation() + Toward * 800.f;
	}

	FVector ToStation = Station - BotCharacter->GetActorLocation();
	ToStation.Z = 0.f;

	// Inside the station radius, stop shuffling and just hold.
	DesiredMoveDirection = (ToStation.SizeSquared() > FMath::Square(TraceBotConstants::StationHoldRadius))
		? ToStation.GetSafeNormal()
		: FVector::ZeroVector;

	if (ATraceCharacter* Target = FindBestShootTarget())
	{
		ShootTarget = Target;
		bWantsToAim = true;
		DesiredAimPoint = GetAimPointOn(Target);
	}
}

void ATraceBotController::BehaviourRegroup(float DeltaSeconds)
{
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return;
	}

	// Nobody is carrying and nobody is in sight. Under spec v2 there is nothing loose to run at, so
	// the useful thing is to be somewhere the play is about to be: up-field of our own half, on this
	// bot's own lane, rather than clustered on the spawn pads.
	const FVector Goal = GetAttackGoalLocation();
	const FVector MyLocation = BotCharacter->GetActorLocation();

	FVector Centre = bBoundsValid ? FieldBounds.GetCenter() : FVector::ZeroVector;
	Centre.Y += HalfFieldWidth() * 0.35f * FMath::Clamp(FormationBias, -1.f, 1.f);
	Centre.Z = MyLocation.Z;

	// Halfway between the middle of the pitch and our attacking end: forward enough to contest, not
	// so forward that a whole team is standing in the enemy endzone when the Core is granted.
	const FVector Station = FMath::Lerp(Centre, Goal, 0.25f);

	FVector ToStation = Station - MyLocation;
	ToStation.Z = 0.f;

	DesiredMoveDirection = (ToStation.SizeSquared() > FMath::Square(TraceBotConstants::StationHoldRadius))
		? ToStation.GetSafeNormal()
		: FVector::ZeroVector;
}

void ATraceBotController::BehaviourFight(float DeltaSeconds)
{
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return;
	}

	ATraceCharacter* Target = FindBestShootTarget();

	if (Target != nullptr)
	{
		ShootTarget = Target;
		bWantsToAim = true;
		DesiredAimPoint = GetAimPointOn(Target);
	}
	else
	{
		// Nobody worth shooting. Close on the nearest enemy we know about, but do NOT raise
		// bWantsToAim: an out-of-envelope enemy is something the bot is walking toward, not
		// something it is drawing a bead on. Keeping those separate is what makes the engagement
		// range cap mean anything — otherwise a bot "cannot shoot you from there" but still tracks
		// you perfectly the whole way in and fires the instant you cross the line.
		Target = NearestEnemy.Get();
	}

	if (Target == nullptr)
	{
		DesiredMoveDirection = FVector::ZeroVector;
		return;
	}

	const FVector MyLocation = BotCharacter->GetActorLocation();
	FVector ToTarget = Target->GetActorLocation() - MyLocation;
	ToTarget.Z = 0.f;
	const float Distance = static_cast<float>(ToTarget.Size());
	const FVector Forward = ToTarget.GetSafeNormal();

	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
	const float PreferredRange = FMath::Max(200.f, Profile.PreferredCombatRange);
	const float Deadzone = PreferredRange * TraceBotConstants::RangeDeadzoneFraction;

	const float Now = GetWorld() ? static_cast<float>(GetWorld()->GetTimeSeconds()) : 0.f;
	if (Now >= StrafeFlipTime)
	{
		StrafeFlipTime = Now + FMath::FRandRange(TraceBotConstants::StrafeFlipMin, TraceBotConstants::StrafeFlipMax);
		StrafeSign = -StrafeSign;
	}

	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward) * StrafeSign;

	if (Distance > PreferredRange + Deadzone)
	{
		DesiredMoveDirection = (Forward * 1.0f + Right * 0.35f).GetSafeNormal();
	}
	else if (Distance < PreferredRange - Deadzone)
	{
		DesiredMoveDirection = (-Forward * 0.8f + Right * 0.7f).GetSafeNormal();

		// A duel at knife range is one the bot is losing on positioning: with zero weapon spread
		// (spec §6) there is no longer any accuracy penalty for the enemy either, so distance is the
		// only defence left. Spend the dash to make some.
		if (Distance < PreferredRange * 0.35f && FMath::FRand() < Profile.Aggression * DeltaSeconds * 2.f)
		{
			if (RequestDash(/*bReserveLast=*/false))
			{
				TRACE_BOT_KIT(DuelDashes);
			}
		}
	}
	else
	{
		DesiredMoveDirection = Right;
	}
}

// =================================================================================================
// MODE B behaviours  (spec v4 §7)
// =================================================================================================

void ATraceBotController::BehaviourChaseLooseCore(float DeltaSeconds)
{
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr || !bCoreLoose)
	{
		DesiredMoveDirection = FVector::ZeroVector;
		return;
	}

	TRACE_BOT_KIT(LooseChaseTicks);

	const FVector MyLocation = BotCharacter->GetActorLocation();

	FVector ToCore = LooseCorePoint - MyLocation;
	ToCore.Z = 0.f;
	DesiredMoveDirection = ToCore.GetSafeNormal();

	// The chase is a race and it is the one moment in mode B where losing by half a metre loses the
	// possession, so a dash is well spent here — and unlike the carrier's dash there is no second
	// charge to hold in reserve, because there is nothing to escape from yet.
	const float Distance = static_cast<float>(ToCore.Size());
	const float DashBand = FMath::Max(600.f, HalfFieldLength() * 0.12f);

	if (Distance > 250.f && Distance < DashBand)
	{
		// Only worth it when somebody is actually contesting: an uncontested walk-up does not need a
		// charge, and spending one leaves the bot flat if an enemy arrives a second later.
		bool bContested = false;
		for (const ATraceCharacter* Enemy : LiveEnemies)
		{
			if (Enemy != nullptr
				&& FVector::DistSquared2D(Enemy->GetActorLocation(), LooseCorePoint) < FMath::Square(DashBand))
			{
				bContested = true;
				break;
			}
		}

		if (bContested && FMath::FRand() < UTraceSettings::GetBotProfile().Aggression * DeltaSeconds * 2.5f)
		{
			if (RequestDash(/*bReserveLast=*/false))
			{
				TRACE_BOT_KIT(EscapeDashes);
			}
		}
	}

	// Keep shooting while chasing. Unlike carrying — where mouse1 IS the throw and the weapon refuses
	// anyway — a bot running at a loose Core is an ordinary armed player, and the enemy running at
	// the same point is a legitimate target. UpdateCombat picks it; all this does is not suppress it.
	bWantsToAim = true;
	if (ATraceCharacter* Target = FindBestShootTarget())
	{
		ShootTarget = Target;
		DesiredAimPoint = GetAimPointOn(Target);
	}
	else
	{
		bWantsToAim = false;
	}
}

void ATraceBotController::BehaviourDefendGoal(float DeltaSeconds)
{
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return;
	}

	if (!bDefendGoalValid)
	{
		// No goal resolved (mode A, or the boxes have not been derived yet). Fall through to the
		// behaviour that is always safe rather than standing still.
		BehaviourRegroup(DeltaSeconds);
		return;
	}

	TRACE_BOT_KIT(GoalDefenceTicks);

	const FVector MyLocation = BotCharacter->GetActorLocation();

	// The thing we are defending against, in priority order: an enemy who already has the Core, the
	// loose Core itself, then the nearest enemy. Standing between THAT and the mouth is the whole job.
	FVector Threat = MyLocation;
	if (bEnemyIsCarrier && Carrier.IsValid())
	{
		Threat = Carrier->GetActorLocation();
	}
	else if (bCoreLoose)
	{
		Threat = LooseCorePoint;
	}
	else if (NearestEnemy.IsValid())
	{
		Threat = NearestEnemy->GetActorLocation();
	}

	FVector Station = FMath::Lerp(DefendGoalCentre, Threat, TraceBotConstants::GoalDefenceStandoffFraction);

	// Spread along the mouth rather than stacking on its centre, using the same per-bot bias the
	// attacking lane offset uses, so two defenders cover two thirds of the goal instead of one third
	// of it twice.
	Station.Y += HalfFieldWidth()
		* FMath::Max(0.f, UTraceSettings::Get().BotAttackLaneFieldFraction) * 0.5f
		* FMath::Clamp(FormationBias, -1.f, 1.f);
	Station.Z = MyLocation.Z;

	FVector ToStation = Station - MyLocation;
	ToStation.Z = 0.f;

	DesiredMoveDirection = (ToStation.SizeSquared() > FMath::Square(TraceBotConstants::StationHoldRadius))
		? ToStation.GetSafeNormal()
		: FVector::ZeroVector;

	// A defender that does not shoot is a cone. Aim while holding the station.
	if (ATraceCharacter* Target = FindBestShootTarget())
	{
		ShootTarget = Target;
		bWantsToAim = true;
		DesiredAimPoint = GetAimPointOn(Target);
	}
}


// =================================================================================================
// Hover passing  (spec §4)
//
// THE BEAT THIS IMPLEMENTS
//   * mouse1 while carrying begins a pass at whatever teammate the crosshair is on;
//   * the moment it begins, the trace becomes invulnerable AND the shield drops;
//   * holding on that teammate for 0.5 s completes the transfer;
//   * looking away, or letting go, cancels it and restores both.
//
// So a pass is a deliberate half-second of being killable, bought in exchange for making the trace
// safe. A bot that passed whenever it had a receiver would simply be handing the enemy free kills,
// and a bot that never passed would never exercise the mechanic at all. What follows is the middle:
// pick the moment, commit, and bail out if the moment turns.
// =================================================================================================

void ATraceBotController::UpdatePass(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (World == nullptr || BotCharacter == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
	const float Now = static_cast<float>(World->GetTimeSeconds());

	// --- Cooldown --------------------------------------------------------------------------------
	if (PassPhase == ETraceBotPassPhase::Cooldown)
	{
		if (Now >= PassCooldownUntilTime)
		{
			PassPhase = ETraceBotPassPhase::None;
		}
		return;
	}

	// --- Idle: look for a reason to start one ----------------------------------------------------
	if (PassPhase == ETraceBotPassPhase::None)
	{
		if (Now < NextPassEvalTime)
		{
			return;
		}
		NextPassEvalTime = Now + FMath::Max(0.05f, Settings.BotPassEvalInterval);

		// The rule's own 2 s cooldown is authoritative and this controller's mirror is only a
		// convenience, so defer to it. Starting a line-up the rule would refuse is not harmful, but
		// it does put the bot into an aim state it cannot act on, which reads on screen as a carrier
		// that has forgotten where the goal is.
		if (const ATraceCore* CoreForCooldown = ATraceCore::Get(World))
		{
			if (CoreForCooldown->GetPassCooldownRemaining() > 0.f)
			{
				return;
			}
		}

		float Advantage = 0.f;
		ATraceCharacter* Receiver = ChooseReceiver(Advantage);
		if (Receiver == nullptr)
		{
			return;
		}

		// --- IS THIS A SAFE MOMENT TO BE SHOOTABLE? -----------------------------------------------
		//
		// This is the risk half of the loop, and it is the one thing that stops the bots from making
		// the pass look free. Starting a pass drops the shield for at least half a second; every
		// enemy that currently has line of sight to this bot from inside punish range is a gun that
		// gets that half second for nothing.
		//
		// PassCaution scales how many of those a bot will accept. On Easy it is low on purpose —
		// Easy bots pass into trouble, die for it, and hand the player the Core, which is one of the
		// few ways to make Easy easier that does not involve making the bots look incompetent.
		const int32 Covering = CountEnemiesCoveringMe();
		const float Caution = FMath::Clamp(Profile.PassCaution, 0.f, 1.f);

		// At caution 1 a bot wants a completely clean window; at caution 0 it tolerates two guns.
		const int32 Tolerated = FMath::RoundToInt(FMath::Lerp(2.f, 0.f, Caution));

		const bool bUnderPressure = NearestEnemy.IsValid()
			&& NearestEnemyDistSq < FMath::Square(FMath::Max(0.f, Settings.BotPassPressureRadius));

		// The exception that makes the mechanic show up in a real match: a carrier who is ABOUT to
		// lose the Core anyway has nothing to protect. Passing out of a collapsing position is the
		// correct play even into a covered window, and it is also the version of the pass a player
		// most often sees, because it happens where the fighting is.
		if (Covering > Tolerated && !bUnderPressure)
		{
			return;
		}

		if (FMath::FRand() > FMath::Clamp(Profile.PassChance, 0.f, 1.f))
		{
			return;
		}

		PassPhase = ETraceBotPassPhase::Lining;
		PassReceiver = Receiver;
		PassPhaseStartTime = Now;

		UE_LOG(LogTraceGame, Verbose, TEXT("[BotPass] %s lining up on %s (advantage %.0fuu, %d covering, %s)"),
			*GetNameSafe(GetPlayerState<APlayerState>()),
			*GetNameSafe(Receiver->GetPlayerState<APlayerState>()),
			Advantage, Covering, bUnderPressure ? TEXT("under pressure") : TEXT("open field"));
		return;
	}

	// --- Everything below needs a live, still-legal receiver -------------------------------------
	ATraceCharacter* Receiver = PassReceiver.Get();
	if (Receiver == nullptr || !Receiver->IsAlive())
	{
		AbortPass(TEXT("receiver died"));
		return;
	}

	// Lead the receiver very slightly. The transfer is instantaneous (the Core is a status now, not
	// a projectile), so there is no flight time to lead — but the receiver is still MOVING, and the
	// aim has to stay on their capsule for the whole dwell rather than on where they were when the
	// attempt started.
	PassAimPoint = Receiver->GetActorLocation() + FVector(0.f, 0.f, Settings.BotAimBodyOffsetZ);
	bPassOwnsAim = true;

	const FVector ViewLocation = BotCharacter->GetPawnViewLocation();
	const FRotator DesiredRotation = (PassAimPoint - ViewLocation).Rotation();
	const FRotator CurrentRotation = GetControlRotation();

	const float AimErrorDegrees =
		FMath::Abs(FRotator::NormalizeAxis(CurrentRotation.Yaw - DesiredRotation.Yaw)) +
		FMath::Abs(FRotator::NormalizeAxis(CurrentRotation.Pitch - DesiredRotation.Pitch));

	// --- "Is my crosshair actually on them?" ------------------------------------------------------
	//
	// Ask the Core, not our own trigonometry. ATraceCore::FindPassTargetFor() is the exact test the
	// rule uses to decide who a pass goes to and whether it stays valid, so using it here makes the
	// bot's belief and the rule's ruling the same object. The angular fallback below is only for a
	// world where the Core has not spawned; when the two ever disagreed, the symptom was a bot that
	// held a perfect-looking pass for the full dwell and never transferred anything.
	const ATraceCore* TheCore = ATraceCore::Get(World);
	const ATraceCharacter* CoreTarget = (TheCore != nullptr) ? TheCore->FindPassTargetFor(BotCharacter) : nullptr;

	const bool bOnReceiver = (TheCore != nullptr)
		? (CoreTarget == Receiver)
		: (AimErrorDegrees <= FMath::Max(0.5f, Settings.BotPassAimToleranceDegrees));

	const bool bCanSeeReceiver = HasLineOfSight(Receiver);

	if (PassPhase == ETraceBotPassPhase::Lining)
	{
		// Shield is still UP here — the input has not been pressed — so lining up costs nothing and
		// can be abandoned for free. Only the commit below is dangerous.
		if (!bCanSeeReceiver)
		{
			AbortPass(TEXT("lost sight of receiver while lining up"));
			return;
		}

		if (Now - PassPhaseStartTime > FMath::Max(0.1f, Settings.BotPassMaxLineUpSeconds))
		{
			TRACE_BOT_KIT(PassLineUpGiveUps);
			AbortPass(TEXT("line-up timed out"));
			return;
		}

		if (!bOnReceiver)
		{
			return;   // Still slewing. UpdateCombat honours bPassOwnsAim and drives the rotation.
		}

		// Release the trigger explicitly BEFORE committing. On the mouse1 fallback binding the pass
		// input IS the fire input, so a trigger this controller believes it is holding would fight
		// the pass press.
		if (bTriggerHeld)
		{
			BotCharacter->DoFireReleased();
			bTriggerHeld = false;
		}

		// COMMIT. From this instant the shield is down and the trace is safe.
		const bool bCoreDriven = ApplyPassInput(BotCharacter, true);
		const TraceBotPawnAPI::EPassBinding Binding = TraceBotPawnAPI::PassBindingOf(BotCharacter, 0);
		TRACE_BOT_KIT(PassAttempts);

		// The LegacyInstant early-completion branch used to live here, for a pre-spec pawn that threw
		// on the press with nothing to hold. That binding is gone (ATraceCharacter::DoPass is
		// deleted), so every pass now genuinely holds and completes through the dwell below.

		bPassInputHeld = true;
		PassPhase = ETraceBotPassPhase::Holding;
		PassPhaseStartTime = Now;

		UE_LOG(LogTraceGame, Display, TEXT("[BotPass] %s -> %s: input DOWN, shield dropped [%s]"),
			*GetNameSafe(GetPlayerState<APlayerState>()),
			*GetNameSafe(Receiver->GetPlayerState<APlayerState>()),
			bCoreDriven ? TEXT("ATraceCore::RequestPassInput") : TraceBotPawnAPI::PassBindingName(Binding));
		return;
	}

	// --- Holding: shield is down, the clock is running -------------------------------------------
	check(PassPhase == ETraceBotPassPhase::Holding);

	// "Looking away cancels the pass" is a rule, so drifting off the receiver is not merely a missed
	// completion, it is a cancel — and the shield comes back. Give up on our own terms instead, so
	// the bot restarts the attempt cleanly rather than fighting the pawn's own cancel.
	if (!bCanSeeReceiver || !bOnReceiver)
	{
		AbortPass(TEXT("drifted off the receiver"));
		return;
	}

	// --- The abort that makes this a real decision ------------------------------------------------
	//
	// A threat that has appeared SINCE the commit is a reason to cut the loss. Half a second is long
	// enough for a duel to arrive, and the alternative to aborting is dying with the Core to a
	// defender who was rewarded for nothing more than walking round a corner.
	//
	// Note what this does NOT do: it does not abort merely because the situation was risky when the
	// attempt started. That was already priced in above. Bailing on the original risk every time
	// would mean no bot ever completed a contested pass, which is most of them.
	const int32 CoveringNow = CountEnemiesCoveringMe();
	const float Caution = FMath::Clamp(Profile.PassCaution, 0.f, 1.f);
	if (CoveringNow >= 3 && Caution > 0.5f)
	{
		TRACE_BOT_KIT(PassAbortsThreat);
		AbortPass(TEXT("collapsed on mid-hold"));
		return;
	}

	// The pawn owns completion: it is the thing that knows the dwell has elapsed and that flips the
	// Core over. This controller only has to keep the input down for at least as long as the rule
	// asks, plus a margin, and then let go. If the transfer happened, bIAmCarrier will have gone
	// false and Tick() closes the attempt out on the next frame.
	const float HoldNeeded = FMath::Max(0.05f, Settings.BotPassHoldSeconds) + FMath::Max(0.f, Settings.BotPassHoldMargin);
	if (Now - PassPhaseStartTime >= HoldNeeded)
	{
		// Held the full dwell and we are still the carrier, so the transfer did not take. Let go
		// rather than standing there unshielded forever — this is the failure path that exists
		// because the pawn's aim test is stricter than this controller's, and the [BotKit] counters
		// are what make the gap between attempts and completions visible.
		TRACE_BOT_KIT(PassAbortsOther);
		AbortPass(TEXT("dwell elapsed without a transfer"));
	}
}

// =================================================================================================
// MODE B throwing  (spec v4 §7)
//
// THE BEAT THIS IMPLEMENTS
//   * mouse1 while carrying THROWS the Core forward, instantly and irrevocably;
//   * anybody's first contact with it takes it, enemy or teammate;
//   * so a throw is a bet, not a commitment: there is no dwell, no shield cost and no abort, and
//     the entire decision is "is the thing I am throwing at better than the ground under my feet".
//
// A bot that never threw would make mode B untestable, and a bot that threw constantly would turn
// every possession into a coin flip. What follows is the middle: shoot at the goal when the goal is
// in range and in front of me, throw to a teammate who is meaningfully further up the field
// otherwise, and clear it forward rather than die with it when the position is collapsing.
// =================================================================================================

namespace TraceThrowBallistics
{
	/**
	 * The Core's launch, expressed once.
	 *
	 * ATraceCore builds its launch velocity as `AimDirection * Speed + Up * (Speed * UpBias)`, so the
	 * up-bias term is added in WORLD space and does not rotate with the aim. That single detail is
	 * what stops this being the textbook ballistic solve: the launch speed is not constant with pitch
	 * (it is Speed * sqrt(1 + bias^2 + 2*bias*sin(pitch))), so the closed form is a quartic and is not
	 * worth writing. Bisection on a monotone residual is.
	 */
	struct FModel
	{
		float Speed = 2236.f;      // uu/s along the aim direction
		float UpBias = 0.29f;      // extra world-up, as a fraction of Speed
		float GravityZ = -970.f;   // signed, uu/s^2
	};

	/**
	 * Height, relative to the launch point, at which a throw at pitch @p SinPitch/@p CosPitch crosses
	 * horizontal distance @p Range. Returns false when it never gets there at all.
	 */
	static bool HeightAtRange(const FModel& Model, float SinPitch, float CosPitch, float Range, float& OutHeight,
		float* OutFlightSeconds = nullptr)
	{
		const float Vx = Model.Speed * CosPitch;
		if (Vx <= 1.f || Range <= 0.f)
		{
			return false;
		}

		const float Vz = Model.Speed * SinPitch + Model.Speed * Model.UpBias;
		const float T = Range / Vx;

		OutHeight = Vz * T + 0.5f * Model.GravityZ * T * T;
		if (OutFlightSeconds != nullptr)
		{
			*OutFlightSeconds = T;
		}
		return true;
	}

	/** Pitch limits, in radians. Below -60 the Core is thrown at the floor; above 80 it is thrown at the sky. */
	static constexpr float MinPitchRadians = -1.047f;   // -60 deg
	static constexpr float MaxPitchRadians = 1.396f;    //  80 deg

	/**
	 * Lowest pitch whose arc passes through (@p Range, @p HeightDelta), or false if none does.
	 *
	 * The residual (height at range, minus the target height) is sampled across the pitch band and
	 * the FIRST sign change from below is bisected. Sampling rather than starting from an analytic
	 * guess because the residual has two roots — the low arc and the lob — and the low one is wanted.
	 */
	static bool SolvePitch(const FModel& Model, float Range, float HeightDelta, float& OutPitch,
		float& OutFlightSeconds)
	{
		constexpr int32 SampleCount = 36;

		float PreviousPitch = MinPitchRadians;
		float PreviousResidual = 0.f;
		bool bHavePrevious = false;

		for (int32 Index = 0; Index <= SampleCount; ++Index)
		{
			const float Pitch = FMath::Lerp(MinPitchRadians, MaxPitchRadians,
				static_cast<float>(Index) / static_cast<float>(SampleCount));

			float Height = 0.f;
			if (!HeightAtRange(Model, FMath::Sin(Pitch), FMath::Cos(Pitch), Range, Height))
			{
				continue;
			}

			const float Residual = Height - HeightDelta;

			if (bHavePrevious && ((PreviousResidual <= 0.f && Residual >= 0.f) || (PreviousResidual >= 0.f && Residual <= 0.f)))
			{
				// Bisect. 20 halvings of a 140-degree band is well under a thousandth of a degree,
				// which is far finer than the aim slew that has to deliver it.
				float Low = PreviousPitch;
				float High = Pitch;
				float LowResidual = PreviousResidual;

				for (int32 Step = 0; Step < 20; ++Step)
				{
					const float Mid = 0.5f * (Low + High);
					float MidHeight = 0.f;
					if (!HeightAtRange(Model, FMath::Sin(Mid), FMath::Cos(Mid), Range, MidHeight))
					{
						break;
					}
					const float MidResidual = MidHeight - HeightDelta;

					if ((LowResidual <= 0.f) == (MidResidual <= 0.f))
					{
						Low = Mid;
						LowResidual = MidResidual;
					}
					else
					{
						High = Mid;
					}
				}

				OutPitch = 0.5f * (Low + High);

				float Unused = 0.f;
				HeightAtRange(Model, FMath::Sin(OutPitch), FMath::Cos(OutPitch), Range, Unused, &OutFlightSeconds);
				return true;
			}

			PreviousPitch = Pitch;
			PreviousResidual = Residual;
			bHavePrevious = true;
		}

		return false;
	}
}

/**
 * The Core's live flight model, asked of the Core rather than reconstructed.
 *
 * The previous version of this read Trace.ModeB.ThrowSpeed / GravityScale / ThrowUpBias off the
 * console directly, which silently ignored the UTraceSettings properties that OVERRIDE those console
 * variables — so it was already reading numbers the Core was not using, and it would have missed the
 * v5 weight model completely.
 */
static TraceThrowBallistics::FModel MakeThrowModel(const UWorld* World, float ChargeScale = 1.f)
{
	TraceThrowBallistics::FModel Model;

	// SPEC v13 §6. THE CHARGE SCALES THE WHOLE IMPULSE, WHICH IS EXACTLY ONE MULTIPLICATION HERE.
	//
	// ATraceCore's launch is (aim * Speed + up * Speed * UpBias) * charge, and this model's own launch
	// is (aim * Model.Speed + up * Model.Speed * Model.UpBias). Scaling Model.Speed and leaving UpBias
	// alone reproduces the Core's expression term for term - the loft stays the same FRACTION of a
	// smaller impulse, which is what makes a half-charged throw the same shape of arc, only shorter.
	// Scaling UpBias too would make weak throws flatter, which is not what the Core does.
	Model.Speed = FMath::Max(100.f, ATraceCore::GetThrowSpeed() * FMath::Max(0.05f, ChargeScale));
	Model.UpBias = ATraceCore::GetThrowUpBias();
	Model.GravityZ = FMath::Min(-1.f, ATraceCore::GetThrowGravityZ(World));
	return Model;
}

float ATraceBotController::ChooseThrowCharge(const FVector& From, const FVector& WorldTarget) const
{
	// FULL POWER IS 1.0 BY DEFINITION under UTraceSettings' charge arithmetic - a full hold reaches
	// exactly the throw the game had before v13, and CoreThrowChargeMaxFraction caps the HOLD rather
	// than the power. So the bots' ceiling is 1.0 and not that knob: a bot has no reason to overcharge
	// even where the tuning allows it, because every one of its range decisions (MaxThrowRange,
	// ChooseReceiver, the shot score) is measured against the full-power arc.
	constexpr float FullMax = 1.f;
	const float Floor = ATraceCore::GetThrowChargeFloor();

	// A bot never taps. The spec's own warning - "a bot that always instant-clicks will throw at 15%
	// power and mode B will look broken" - is about exactly this number, and it is deliberately well
	// above the floor: even a pass to somebody standing next to you should leave the hand like a throw.
	const float MinBotCharge = FMath::Clamp(FMath::Max(Floor, TraceBotConstants::ThrowMinCharge), 0.f, FullMax);

	const float Reach = MaxThrowRange(From, static_cast<float>(WorldTarget.Z - From.Z), FullMax)
		* TraceBotConstants::ThrowRangeSafetyFraction;
	const float Distance = static_cast<float>(FVector::Dist2D(From, WorldTarget));

	if (Reach <= 1.f || Distance <= 1.f)
	{
		return FullMax;   // Nothing to reason about; throw it properly.
	}

	// Range goes as speed squared, so the charge that just covers D is sqrt(D / R). The margin is not
	// decoration: since spec v6 §4.2 a throw that lands is handed to the nearest enemy, so arriving at
	// the exact limit of the arc means arriving on the floor and giving the Core away.
	const float Needed = FMath::Sqrt(FMath::Clamp(Distance / Reach, 0.f, 1.f)) * FullMax
		+ TraceBotConstants::ThrowChargeMargin;

	return FMath::Clamp(Needed, MinBotCharge, FullMax);
}

bool ATraceBotController::SolveThrowLaunch(const FVector& From, const FVector& WorldTarget,
	FVector& OutLaunchDirection, float* OutFlightSeconds, float ChargeScale) const
{
	if (static_cast<float>(FVector(WorldTarget.X - From.X, WorldTarget.Y - From.Y, 0.0).Size()) < 1.f)
	{
		OutLaunchDirection = FVector::UpVector;
		if (OutFlightSeconds != nullptr)
		{
			*OutFlightSeconds = 0.f;
		}
		return true;
	}

	const TraceThrowBallistics::FModel Model = MakeThrowModel(GetWorld(), ChargeScale);

	// SPEC v8 §4 FALLOUT. The Core now leaves at impulse + thrower velocity, so a bot that solves the
	// impulse alone aims wrong BY ITS OWN VELOCITY — a running bot throwing across its direction of
	// travel misses by (its speed x flight time), which is thousands of uu on a full-field throw. It
	// was measured live: a bot running away from its aim launched at 1233 uu/s against a 2081 uu/s
	// impulse, i.e. the inherited term dominated the shot and the solver never knew.
	//
	// THE TRICK: hitting Target at time t with an extra CONSTANT velocity V is exactly hitting
	// (Target - V*t) without it. V is constant over the flight (the Core's gravity is already in the
	// model), so no new ballistics are needed — only the target moves, and the existing solve is
	// reused verbatim.
	//
	// It is a fixed point, because the displacement depends on the flight time the solve returns:
	// two passes, since the flight time barely moves once the range is roughly right. Converges in
	// two for every case measured; the loop keeps the last successful answer if a later pass falls
	// out of range, so adding lead can never turn a reachable shot into a refusal.
	const FVector Inherited = ATraceCore::GetInheritedThrowVelocity(GetBotCharacter());

	FVector EffectiveTarget = WorldTarget;
	bool bSolved = false;

	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		const FVector Delta = EffectiveTarget - From;
		FVector Horizontal = FVector(Delta.X, Delta.Y, 0.0);
		const float Range = static_cast<float>(Horizontal.Size());
		if (Range < 1.f)
		{
			break;
		}
		Horizontal /= Range;

		float Pitch = 0.f;
		float Flight = 0.f;
		if (!TraceThrowBallistics::SolvePitch(Model, Range, static_cast<float>(Delta.Z), Pitch, Flight))
		{
			break;
		}

		OutLaunchDirection = (Horizontal * FMath::Cos(Pitch) + FVector::UpVector * FMath::Sin(Pitch)).GetSafeNormal();
		if (OutFlightSeconds != nullptr)
		{
			*OutFlightSeconds = Flight;
		}
		bSolved = true;

		// Standing still (or inheritance turned off) — the displacement is zero and the extra passes
		// would be identical. Bail on the first solve so the common case costs exactly what it did.
		if (Inherited.IsNearlyZero())
		{
			break;
		}

		EffectiveTarget = WorldTarget - Inherited * Flight;
	}

	return bSolved;
}

float ATraceBotController::MaxThrowRange(const FVector& /*From*/, float HeightDelta, float ChargeScale) const
{
	const TraceThrowBallistics::FModel Model = MakeThrowModel(GetWorld(), ChargeScale);

	// Walk the pitch band and keep the furthest range that still arrives at the wanted height. The
	// bisection in SolvePitch answers "can I reach THIS range"; this answers "how far can I reach at
	// all", and the two must agree, so both are computed from the same HeightAtRange.
	float Best = 0.f;
	constexpr int32 SampleCount = 36;

	for (int32 Index = 0; Index <= SampleCount; ++Index)
	{
		const float Pitch = FMath::Lerp(TraceThrowBallistics::MinPitchRadians, TraceThrowBallistics::MaxPitchRadians,
			static_cast<float>(Index) / static_cast<float>(SampleCount));

		const float SinPitch = FMath::Sin(Pitch);
		const float CosPitch = FMath::Cos(Pitch);
		const float Vx = Model.Speed * CosPitch;
		if (Vx <= 1.f)
		{
			continue;
		}

		// Time at which the arc passes DOWN through the target height: the positive root of
		// 0.5*g*t^2 + Vz*t - HeightDelta = 0.
		const float Vz = Model.Speed * SinPitch + Model.Speed * Model.UpBias;
		const float A = 0.5f * Model.GravityZ;
		const float Discriminant = Vz * Vz + 4.f * A * HeightDelta;
		if (Discriminant < 0.f)
		{
			continue;   // Never gets that high at this pitch.
		}

		const float Root = FMath::Sqrt(Discriminant);
		const float T = (-Vz - Root) / (2.f * A);   // A is negative, so this is the LATER root.
		if (T <= 0.f)
		{
			continue;
		}

		Best = FMath::Max(Best, Vx * T);
	}

	return Best;
}

bool ATraceBotController::HasThrowLane(const FVector& From, const FVector& WorldTarget,
	const FVector& LaunchDirection, float FlightSeconds, float ChargeScale) const
{
	const UWorld* World = GetWorld();
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	if (World == nullptr || BotCharacter == nullptr || FlightSeconds <= 0.f)
	{
		return true;   // Nothing to sweep. Refusing here would refuse every point-blank shot.
	}

	const TraceThrowBallistics::FModel Model = MakeThrowModel(World, ChargeScale);

	// SPEC v8 §4. Sweep the arc the Core ACTUALLY FLIES, not the one the impulse alone would produce.
	// ComputeThrowLaunchVelocity is ATraceCore's own launch expression (impulse + inherited velocity,
	// mode-gated and null-safe), so the lane test and the throw cannot drift apart the way a
	// hand-rebuilt copy of the formula just did — this line used to be that copy.
	//
	// SPEC v13 §6: at the charge this throw is actually going to be released at. A lane swept for a
	// full-power arc is the wrong lane for a half-power one - it clears cover the real throw would hit
	// and hits cover the real throw would clear.
	const FVector LaunchVelocity = ATraceCore::ComputeThrowLaunchVelocity(BotCharacter, LaunchDirection, ChargeScale);

	FCollisionQueryParams Params(TEXT("TraceBotThrowLane"), /*bTraceComplex=*/false, BotCharacter);

	// The last 10% is deliberately not swept. The mouth of the goal has a crossbar, a sill and two
	// posts around it, and a Core arriving at the back of the net legitimately ends inside geometry;
	// sweeping into it would report every good shot as blocked.
	const float SweepSeconds = FlightSeconds * 0.9f;

	// FROM THE MUZZLE, NOT THE EYE. ATraceCore::ThrowFromHolder launches at
	// GetPawnViewLocation() + direction * ThrowMuzzleForward, so starting the sweep at the eye tests
	// 70 uu of trajectory the Core never occupies - which is inside the capsule, and against a bot
	// pressed up against cover is enough to report a lane that is actually clear as blocked.
	const FVector MuzzleOffset = LaunchDirection * ATraceCore::GetThrowMuzzleForward();

	FVector Previous = From + MuzzleOffset;
	for (int32 Index = 1; Index <= TraceBotConstants::ThrowLaneSamples; ++Index)
	{
		const float T = SweepSeconds * (static_cast<float>(Index) / static_cast<float>(TraceBotConstants::ThrowLaneSamples));
		const FVector Point = From + MuzzleOffset + LaunchVelocity * T
			+ FVector(0.f, 0.f, 0.5f * Model.GravityZ * T * T);

		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Previous, Point, ECC_Visibility, Params))
		{
			// NAMED, at Display, and only while the metrics harness is on. "blocked=18 of 18" is a
			// symptom that looks identical whether the arc is clipping a cover box, the goal frame or
			// the floor under the bot's own feet, and those want completely different fixes.
			const float NowSeconds = static_cast<float>(World->GetTimeSeconds());
			if (TraceBotTelemetry::Enabled() && NowSeconds >= NextThrowLaneLogTime)
			{
				NextThrowLaneLogTime = NowSeconds + 5.f;
				UE_LOG(LogTraceGame, Display,
					TEXT("[BotThrowLane] %s: arc blocked at sample %d/%d by %s (%s) at %s"),
					*GetNameSafe(GetPlayerState<APlayerState>()), Index, TraceBotConstants::ThrowLaneSamples,
					*GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()),
					*Hit.ImpactPoint.ToCompactString());
			}
			return false;
		}
		Previous = Point;
	}

	return true;
}

FVector ATraceBotController::ComputeThrowAimPoint(const FVector& WorldTarget) const
{
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return WorldTarget;
	}

	const FVector From = BotCharacter->GetPawnViewLocation();

	// SPEC v13 §6. AT THE CHARGE THIS ATTEMPT IS GOING TO BE RELEASED AT, not at full power. This is
	// the single most important line of the bot half of §6: the aim and the launch have to be solved
	// for the same impulse or the bot lobs at a full-power solution and then throws at 40%, landing
	// short of everything by the difference.
	FVector LaunchDirection = FVector::ZeroVector;
	if (SolveThrowLaunch(From, WorldTarget, LaunchDirection, nullptr, PlannedThrowCharge))
	{
		// A point along the solved launch ray. The caller turns this back into a rotation, so the
		// distance is arbitrary as long as it is far enough not to lose precision.
		return From + LaunchDirection * FMath::Max(1000.f, static_cast<float>(FVector::Dist(From, WorldTarget)));
	}

	// Out of range: aim at the ceiling of what the throw CAN do, in the target's direction, rather
	// than flat at a target that cannot be reached. Callers gate on range before getting here, so
	// this is the clearance throw and the "throw it forward at nothing" case.
	FVector Horizontal = FVector(WorldTarget.X - From.X, WorldTarget.Y - From.Y, 0.0).GetSafeNormal();
	if (Horizontal.IsNearlyZero())
	{
		Horizontal = BotCharacter->GetActorForwardVector().GetSafeNormal2D();
	}

	constexpr float ClearanceElevationDegrees = 25.f;
	const float Pitch = FMath::DegreesToRadians(ClearanceElevationDegrees);
	return From + (Horizontal * FMath::Cos(Pitch) + FVector::UpVector * FMath::Sin(Pitch)) * 3000.f;
}

void ATraceBotController::UpdateThrow(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (World == nullptr || BotCharacter == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
	const float Now = static_cast<float>(World->GetTimeSeconds());

	// --- Committed to running it in? Then there is nothing to decide. -----------------------------
	//
	// SPEC v5 §4, and the fix for "scoring by carrying into a goal never fired in ~7 minutes". The
	// code path was always live; what never happened was a bot getting close to an open mouth and
	// CHOOSING to keep hold of the Core. Inside the commit radius the throw machine is off. See
	// UpdateCarryInCommit and ATraceBotController::bCommitCarryIn.
	//
	// AHEAD OF THE COOLDOWN, deliberately. The throw cooldown is 2.3 s and a carrier crosses 1800 uu
	// in that time; evaluating the commit only when the throw machine happens to be idle would let a
	// bot sprint through the whole commit band and out the other side without ever noticing it, and
	// BehaviourCarryToGoal - which reads the latch every frame to decide whether to weave - would be
	// steering off the mouth the entire way in.
	UpdateCarryInCommit();

	// --- SPEC v13 §6: A CHARGE IN PROGRESS IS A COMMITMENT, AND IT OUTRANKS EVERYTHING BELOW. ------
	//
	// AHEAD OF THE CARRY-IN CHECK AND THE COOLDOWN, deliberately, because the button is already down
	// and RELEASING IT IS THE THROW. There is no such thing as "abort" once a charge has started: the
	// only two things a caller can do are release now (a weak throw at whatever is under the crosshair)
	// or release on time (the throw the bot decided on), and the second is always the better of the
	// two. The wind-up is at most Trace.ModeB.ThrowChargeSeconds, so the cost of this commitment is
	// bounded at a second.
	//
	// The aim keeps tracking through the hold - a receiver keeps running while the bot winds up - and
	// PlannedThrowCharge is deliberately NOT re-derived here: the hold length was computed from it when
	// the button went down, and a charge that changed its mind mid-hold would release at a power the
	// aim was not solved for.
	if (PassPhase == ETraceBotPassPhase::Charging)
	{
		if (!bThrowAtGoal && PassReceiver.IsValid() && PassReceiver->IsAlive())
		{
			ThrowTargetPoint = PassReceiver->GetActorLocation() + FVector(0.f, 0.f, Settings.BotAimBodyOffsetZ);
		}

		PassAimPoint = ComputeThrowAimPoint(ThrowTargetPoint);
		bPassOwnsAim = true;

		if (Now < ThrowChargeReleaseTime)
		{
			return;   // Still winding up.
		}

		// RELEASE. Same entry point a human's mouse-up reaches; the Core measures the hold on its own
		// clock and launches. The bot never tells it how long it held.
		ApplyPassInput(BotCharacter, false);
		bPassInputHeld = false;

		TRACE_BOT_KIT(ThrowAttempts);
		if (bThrowAtGoal)
		{
			TRACE_BOT_KIT(ThrowsAtGoal);
		}
		else if (PassReceiver.IsValid())
		{
			TRACE_BOT_KIT(ThrowsToTeammate);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[BotThrow] %s RELEASED at %s (%s) after a %.2fs charge (x%.2f of full power)"),
			*GetNameSafe(GetPlayerState<APlayerState>()),
			bThrowAtGoal ? TEXT("the GOAL") : (PassReceiver.IsValid()
				? *GetNameSafe(PassReceiver->GetPlayerState<APlayerState>()) : TEXT("open ground (clearance)")),
			bThrowAtGoal ? TEXT("shot") : (PassReceiver.IsValid() ? TEXT("pass") : TEXT("clearance")),
			Now - PassPhaseStartTime, PlannedThrowCharge);

		AbortThrow(nullptr);   // Not a failure: this is how the attempt is closed out and cooled down.
		return;
	}

	if (bCommitCarryIn)
	{
		if (PassPhase == ETraceBotPassPhase::Lining)
		{
			AbortThrow(TEXT("committed to carrying it in"));
		}
		return;
	}

	// --- Cooldown --------------------------------------------------------------------------------
	if (PassPhase == ETraceBotPassPhase::Cooldown)
	{
		if (Now >= PassCooldownUntilTime)
		{
			PassPhase = ETraceBotPassPhase::None;
		}
		return;
	}

	const FVector MyLocation = BotCharacter->GetActorLocation();

	// --- Idle: is there anything worth throwing at? -----------------------------------------------
	if (PassPhase == ETraceBotPassPhase::None)
	{
		if (Now < NextPassEvalTime)
		{
			return;
		}
		NextPassEvalTime = Now + FMath::Max(0.05f, Settings.BotPassEvalInterval);

		const bool bUnderPressure = NearestEnemy.IsValid()
			&& NearestEnemyDistSq < FMath::Square(FMath::Max(0.f, Settings.BotPassPressureRadius));

		// --- 1. THE SHOT AT GOAL. --------------------------------------------------------------
		//
		// The reason mode B exists: "scoring should happen when the core is thrown into the goal".
		//
		// MEASURED FAILURE THIS REPLACES: at-goal = 0 shots across 70 s of play while 5 of 14 throws
		// went to a teammate — every goal that landed came from the scripted verifier or a deflection.
		// Three things were wrong and all three are fixed here.
		//
		//  (a) THE RANGE WAS NOT A RANGE. "HalfFieldLength() * 0.55" is 9240 uu on this pitch, and it
		//      is a fact about the map rather than about the Core: the light Core carried ~5000 uu on
		//      a flat throw, the heavy one carries ~3400. So the gate simultaneously let through
		//      shots that could not physically arrive AND, because the carrier is usually much
		//      further out than 9240 on a 33600 uu field, refused every shot that could. It is now
		//      MaxThrowRange() — the Core's own ballistics, at the goal's own height.
		//  (b) THE AIM WAS FLAT. The elevation was a first-order drop correction with no solution for
		//      the loft that actually extends a throw. Aiming UP roughly doubles the reachable
		//      distance, which is the difference between "the goal is never in range" and "the goal
		//      is in range from a third of our own half". SolveThrowLaunch does the real solve.
		//  (c) THERE WAS NO LANE TEST AT ALL, only a 35-degree cone against the pawn's forward
		//      vector — which is not the thing that has to slew, and which a weaving carrier swings
		//      off the goal line by design. Now: the cone is measured on the control rotation and
		//      widened to what the slew can deliver, and the ARC is swept against arena geometry.
		bool bShotAvailable = false;
		float ShotScore = 0.f;
		FVector GoalCentre = FVector::ZeroVector;
		FVector ShotLaunchDirection = FVector::ZeroVector;
		FBox GoalBoxFallback(ForceInit);

		if (ATraceCore::GetAttackGoalCentre(World, MyTeam, GoalCentre))
		{
			TRACE_BOT_KIT(ShotEvals);

			const FVector ViewLocation = BotCharacter->GetPawnViewLocation();

			// AIM AT THE HOOP, NOT AT THE BOX (spec v6 §4.3).
			//
			// GetAttackGoalCentre still returns the centre of the goal BOX, and since v6 that box is
			// the ring's bounding SLAB - a square whose corners are back wall. Aiming at its centre is
			// aiming at the middle of the hoop only by coincidence of it being centred; what actually
			// matters, and what the old "aim 250 uu inside the mouth" fudge was for, is that the aim
			// point sits just past the plane a Core has to cross, in the middle of the circle. Ask the
			// Core for the ring and there is nothing left to reconstruct.
			//
			// The fallback is the pre-v6 path verbatim, for an externally registered box goal.
			FVector AimTarget = GoalCentre;
			FVector RingCentre = FVector::ZeroVector;
			FVector RingAxis = FVector::ZeroVector;
			float RingRadius = 0.f;

			if (ATraceCore::GetAttackGoalRing(World, MyTeam, RingCentre, RingRadius, RingAxis))
			{
				// A little way THROUGH the hoop, along its own axis, so a solution that lands exactly
				// on target is already over the line rather than kissing the plane.
				constexpr double ThroughTheHoop = 200.0;
				AimTarget = RingCentre - RingAxis.GetSafeNormal() * ThroughTheHoop;
			}
			else if (ATraceCore::GetAttackGoalBox(World, MyTeam, GoalBoxFallback))
			{
				constexpr double InsideTheMouth = 250.0;

				const double FieldCentreX = bBoundsValid ? FieldBounds.GetCenter().X : 0.0;
				const bool bGoalIsPositiveX = (GoalBoxFallback.GetCenter().X >= FieldCentreX);

				AimTarget.X = bGoalIsPositiveX
					? (GoalBoxFallback.Min.X + InsideTheMouth) : (GoalBoxFallback.Max.X - InsideTheMouth);
				AimTarget.Y = GoalBoxFallback.GetCenter().Y;
				AimTarget.Z = GoalBoxFallback.Min.Z + (GoalBoxFallback.Max.Z - GoalBoxFallback.Min.Z) * 0.5;
			}

			const float GoalDistance = static_cast<float>(FVector::Dist2D(MyLocation, AimTarget));
			const float Reach = MaxThrowRange(ViewLocation, static_cast<float>(AimTarget.Z - ViewLocation.Z))
				* TraceBotConstants::ThrowRangeSafetyFraction;

			// The cone is a PRE-FILTER against committing the aim to a 180-degree turn, and it is
			// measured on the control rotation because that is what slews. Pressure waives it: a
			// carrier about to die should throw at the goal even over its own shoulder.
			FVector ToGoal = AimTarget - MyLocation;
			ToGoal.Z = 0.f;
			const FVector Facing = GetControlRotation().Vector().GetSafeNormal2D();
			const float ConeCos = FMath::Cos(FMath::DegreesToRadians(TraceBotConstants::ThrowAtGoalConeDegrees));
			const bool bInFront = !ToGoal.IsNearlyZero()
				&& FVector::DotProduct(ToGoal.GetSafeNormal(), Facing) >= ConeCos;

#if !UE_BUILD_SHIPPING
			if (TraceBotTelemetry::Enabled())
			{
				float& Closest = TraceBotTelemetry::Kit().CarrierClosestToGoal;
				Closest = (Closest < 0.f) ? GoalDistance : FMath::Min(Closest, GoalDistance);
			}
#endif

			if (GoalDistance > Reach)
			{
				TRACE_BOT_KIT(ShotOutOfRange);
			}
			else if (!bInFront && !bUnderPressure)
			{
				// Not counted as a refusal: the bot is simply facing the wrong way this instant and
				// will be facing the right way within a second of running at the goal.
			}
			else
			{
				float FlightSeconds = 0.f;
				if (!SolveThrowLaunch(ViewLocation, AimTarget, ShotLaunchDirection, &FlightSeconds))
				{
					TRACE_BOT_KIT(ShotNoSolution);
				}
				else if (!HasThrowLane(ViewLocation, AimTarget, ShotLaunchDirection, FlightSeconds))
				{
					TRACE_BOT_KIT(ShotBlocked);
				}
				else
				{
					// 1 at the mouth, 0 at the edge of what the throw can carry. Pressure is worth a
					// lot: a shot released a moment before dying is the whole point of having one.
					ShotScore = 1.f - FMath::Clamp(GoalDistance / FMath::Max(1.f, Reach), 0.f, 1.f);
					ShotScore += bUnderPressure ? 0.35f : 0.f;

					// SPEC v6 §4.2 CHANGED WHAT A MISS COSTS, so it has to change when a bot shoots.
					// Before v6 a shot that fell short left the Core lying in front of the goal for
					// either side to recover - i.e. a miss was free, and taking the shot at maximum
					// range was strictly correct. Now a throw that touches the ground is handed
					// straight to the nearest defender, and in front of their own goal that is the
					// worst place on the field to give it away. A shot from the ragged edge of range
					// is therefore no longer a free option and is refused unless the alternative is
					// dying with it.
					bShotAvailable = bUnderPressure
						|| ShotScore >= TraceBotConstants::ThrowAtGoalMinScore;

					if (bShotAvailable)
					{
						ThrowTargetPoint = AimTarget;
					}
					else
					{
						TRACE_BOT_KIT(ShotOutOfRange);
					}
				}
			}
		}

		// --- 2. THE THROW TO A TEAMMATE. -------------------------------------------------------
		//
		// ChooseReceiver is the mode-A pass rule and it is reused verbatim: "an open teammate, in
		// range, with line of sight, meaningfully further up the field". What differs is what happens
		// next — the Core is thrown at them and ANYBODY may intercept it, which is the risk mode B
		// substitutes for mode A's dropped shield.
		//
		// WEIGHED against the shot rather than losing to it outright. The old code took the shot the
		// instant its (broken) range test passed and never looked at the pass; inverting that would
		// be the same mistake the other way round. Both options are scored on the same 0..1 scale —
		// how much closer to a goal does this throw get the Core — so the comparison means something:
		// a pass that gains half the remaining distance beats a shot from the edge of throwing range,
		// and a shot from the edge of the box beats any pass.
		float Advantage = 0.f;
		ATraceCharacter* Receiver = ChooseReceiver(Advantage);

		if (Receiver != nullptr && bShotAvailable)
		{
			const float PassScore = FMath::Clamp(Advantage / FMath::Max(1.f, HalfFieldLength()), 0.f, 1.f);
			if (PassScore > ShotScore)
			{
				bShotAvailable = false;
				TRACE_BOT_KIT(ShotLostToPass);
			}
		}

		if (bShotAvailable)
		{
			bThrowAtGoal = true;
			PassPhase = ETraceBotPassPhase::Lining;
			PassReceiver = nullptr;
			PassPhaseStartTime = Now;
			return;
		}

		if (Receiver == nullptr)
		{
			// --- 3. THE CLEARANCE. -------------------------------------------------------------
			//
			// Nothing that passes the pass rule, and enemies closing. Throwing is still better than
			// being killed holding it - a kill hands the Core to the killer outright - so a cornered
			// carrier must not stand still until it dies, which was the first version's most visible
			// failure.
			//
			// WHAT SPEC v6 §4.2 CHANGED. The old clearance was thrown at a point halfway to the goal,
			// at nobody, on the reasoning that a loose Core is contestable by our side too. It no
			// longer is: a throw that lands is the enemy's by rule. So the clearance is now aimed at
			// the most ADVANCED LIVING TEAMMATE even though they failed ChooseReceiver's stricter
			// test, because with the §4.1 catch zone reaching 500 uu off a capsule the bar for "could
			// they catch this" is much lower than the bar for "is this a good pass". A clearance at a
			// teammate is a throw that might be caught; a clearance at open ground is a gift.
			if (bUnderPressure && FMath::FRand() < FMath::Clamp(Profile.PassChance, 0.f, 1.f))
			{
				const FVector Goal = GetAttackGoalLocation();

				const ATraceCharacter* Outlet = nullptr;
				double BestAdvance = -TNumericLimits<double>::Max();
				for (const ATraceCharacter* Mate : LiveTeammates)
				{
					if (Mate == nullptr || !Mate->IsAlive())
					{
						continue;
					}

					// "Most advanced" = closest to the goal we are attacking, and no further from us
					// than the Core can actually be thrown, so the clearance is not a shot into space
					// dressed up as a pass.
					const double ToGoal = FVector::Dist2D(Mate->GetActorLocation(), Goal);
					const double FromMe = FVector::Dist2D(Mate->GetActorLocation(), MyLocation);
					const float Carry = MaxThrowRange(BotCharacter->GetPawnViewLocation(), Settings.BotAimBodyOffsetZ)
						* TraceBotConstants::ThrowRangeSafetyFraction;

					if (FromMe > static_cast<double>(Carry))
					{
						continue;
					}

					const double Advance = -ToGoal;
					if (Advance > BestAdvance)
					{
						BestAdvance = Advance;
						Outlet = Mate;
					}
				}

				if (Outlet != nullptr)
				{
					const FVector OutletPoint = Outlet->GetActorLocation() + FVector(0.f, 0.f, Settings.BotAimBodyOffsetZ);
					ThrowTargetPoint = FMath::Lerp(MyLocation, OutletPoint, TraceBotConstants::ClearanceLeadFraction);
				}
				else
				{
					// Genuinely nobody. Throw it forward anyway - being killed holding it is not better
					// - but keep it short so it lands in our own half rather than presenting it to a
					// defender in front of their goal.
					ThrowTargetPoint = FMath::Lerp(MyLocation, Goal, 0.3f);
				}

				bThrowAtGoal = false;
				PassPhase = ETraceBotPassPhase::Lining;
				PassReceiver = nullptr;
				PassPhaseStartTime = Now;
			}
			return;
		}

		if (FMath::FRand() > FMath::Clamp(Profile.PassChance, 0.f, 1.f))
		{
			return;
		}

		bThrowAtGoal = false;
		PassReceiver = Receiver;
		ThrowTargetPoint = Receiver->GetActorLocation() + FVector(0.f, 0.f, Settings.BotAimBodyOffsetZ);
		PassPhase = ETraceBotPassPhase::Lining;
		PassPhaseStartTime = Now;
		return;
	}

	// --- Lining up. There is no Holding phase in mode B: the throw fires and it is gone. ----------
	if (PassPhase != ETraceBotPassPhase::Lining)
	{
		// A leftover Holding phase from a mid-match mode switch. Close it out cleanly.
		AbortThrow(TEXT("stale phase after a mode change"));
		return;
	}

	// A teammate throw tracks its receiver; a shot at goal and a clearance are aimed at fixed points.
	if (!bThrowAtGoal && PassReceiver.IsValid())
	{
		ATraceCharacter* Receiver = PassReceiver.Get();
		if (!Receiver->IsAlive())
		{
			AbortThrow(TEXT("receiver died"));
			return;
		}
		ThrowTargetPoint = Receiver->GetActorLocation() + FVector(0.f, 0.f, Settings.BotAimBodyOffsetZ);
	}

	// SPEC v13 §6. DECIDE THE POWER BEFORE SOLVING THE AIM, every frame of the line-up, because the aim
	// is a function of the power: a lofted solution at 40% charge points somewhere quite different from
	// the same solution at 100%. Recomputed rather than latched at the start of the line-up because the
	// bot and its receiver are both moving throughout it.
	PlannedThrowCharge = ChooseThrowCharge(BotCharacter->GetPawnViewLocation(), ThrowTargetPoint);

	PassAimPoint = ComputeThrowAimPoint(ThrowTargetPoint);
	bPassOwnsAim = true;

	const FVector ViewLocation = BotCharacter->GetPawnViewLocation();
	const FRotator DesiredRotation = (PassAimPoint - ViewLocation).Rotation();
	const FRotator CurrentRotation = GetControlRotation();

	const float AimErrorDegrees =
		FMath::Abs(FRotator::NormalizeAxis(CurrentRotation.Yaw - DesiredRotation.Yaw)) +
		FMath::Abs(FRotator::NormalizeAxis(CurrentRotation.Pitch - DesiredRotation.Pitch));

	if (Now - PassPhaseStartTime > FMath::Max(0.1f, Settings.BotPassMaxLineUpSeconds))
	{
		TRACE_BOT_KIT(ThrowGiveUps);
		AbortThrow(TEXT("throw line-up timed out"));
		return;
	}

	// Deliberately the ANGULAR test, not ATraceCore::FindPassTargetFor. That function answers "who
	// would a HOVER PASS go to", which is a question mode B does not ask: a throw goes wherever the
	// bot is pointing and hits whatever it hits. Asking it here would refuse every shot at goal,
	// because a goal mouth is not a legal pass target and never will be.
	if (AimErrorDegrees > FMath::Max(0.5f, Settings.BotPassAimToleranceDegrees))
	{
		return;   // Still slewing. UpdateCombat honours bPassOwnsAim and drives the rotation.
	}

	// On the mouse1 binding the throw input IS the fire input; release the trigger first so the two
	// do not fight, exactly as the hover pass does.
	if (bTriggerHeld)
	{
		BotCharacter->DoFireReleased();
		bTriggerHeld = false;
	}

	// SPEC v13 §6. START THE CHARGE. The press no longer throws - it winds up, and the release a
	// fraction of a second later is what launches. Same entry point a human's mouse1 reaches, so the
	// bots still have no private door: ATraceCore measures the hold on its own clock between these two
	// calls and derives the momentum from it.
	//
	// "Bots must use it - a bot that always instant-clicks throws at 15% power and mode B will look
	// broken." So the hold is derived from the power the attempt needs (ChooseThrowCharge, above) and
	// converted by the Core's own inverse, not by a local copy of the curve.
	const float HoldSeconds = ATraceCore::GetThrowHoldSecondsForScale(PlannedThrowCharge);

	ApplyPassInput(BotCharacter, true);
	bPassInputHeld = true;

	PassPhase = ETraceBotPassPhase::Charging;
	PassPhaseStartTime = Now;
	ThrowChargeReleaseTime = Now + HoldSeconds;

	UE_LOG(LogTraceGame, Display,
		TEXT("[BotThrow] %s CHARGING for %s (%s) - %.0f uu away, x%.2f power, holding %.2fs, aim error %.1fdeg"),
		*GetNameSafe(GetPlayerState<APlayerState>()),
		bThrowAtGoal ? TEXT("the GOAL") : (PassReceiver.IsValid()
			? *GetNameSafe(PassReceiver->GetPlayerState<APlayerState>()) : TEXT("open ground (clearance)")),
		bThrowAtGoal ? TEXT("shot") : (PassReceiver.IsValid() ? TEXT("pass") : TEXT("clearance")),
		FVector::Dist2D(ViewLocation, ThrowTargetPoint), PlannedThrowCharge, HoldSeconds,
		AimErrorDegrees);
}

void ATraceBotController::UpdateCarryInCommit()
{
	const UWorld* World = GetWorld();
	const ATraceCharacter* BotCharacter = GetBotCharacter();

	if (World == nullptr || BotCharacter == nullptr || !bModeB || !bIAmCarrier)
	{
		bCommitCarryIn = false;
		return;
	}

	FVector GoalCentre = FVector::ZeroVector;
	if (!ATraceCore::GetAttackGoalCentre(World, MyTeam, GoalCentre))
	{
		bCommitCarryIn = false;
		return;
	}

	const float Now = static_cast<float>(World->GetTimeSeconds());

	// Distance to the MOUTH, not to the box centre.
	//
	// Since spec v6 §4.3 the mouth is the ring, so ask for the ring: the box is its bounding slab and
	// its near face is 160 uu short of the hoop's plane. (Pre-v6 the box ran from the goal line back
	// to the end wall and its centre was 1200 uu inside the net, which is the error this originally
	// corrected - the correction is smaller now but the reason for it is identical.)
	FVector MouthPoint = GoalCentre;
	FVector RingCentre = FVector::ZeroVector;
	FVector RingAxis = FVector::ZeroVector;
	float RingRadius = 0.f;

	if (ATraceCore::GetAttackGoalRing(World, MyTeam, RingCentre, RingRadius, RingAxis))
	{
		MouthPoint = RingCentre;
	}
	else
	{
		FBox GoalBox(ForceInit);
		if (ATraceCore::GetAttackGoalBox(World, MyTeam, GoalBox))
		{
			const double FieldCentreX = bBoundsValid ? FieldBounds.GetCenter().X : 0.0;
			MouthPoint.X = (GoalBox.GetCenter().X >= FieldCentreX) ? GoalBox.Min.X : GoalBox.Max.X;
		}
	}

	const float Distance = static_cast<float>(FVector::Dist2D(BotCharacter->GetActorLocation(), MouthPoint));

	if (!bCommitCarryIn)
	{
		if (Distance <= TraceBotConstants::CarryInCommitDistance)
		{
			bCommitCarryIn = true;
			CarryInCommitTime = Now;
			TRACE_BOT_KIT(CarryInCommits);

			// Display, not Verbose. This project has twice declared a working mechanic dead because
			// its only log line was below the default verbosity, and "bots never carry it in" was
			// exactly that kind of claim. It fires a handful of times a match, which is not spam.
			UE_LOG(LogTraceGame, Display,
				TEXT("[BotCarryIn] %s COMMITTED to running it in, %.0f uu from the mouth"),
				*GetNameSafe(GetPlayerState<APlayerState>()), Distance);
		}
		return;
	}

	// --- Releasing the commit. Two ways out, and both are needed. ---------------------------------
	//
	// Pushed back out (a turnover it survived, a dash away from a defender) - hysteresis so a bot on
	// the boundary does not flicker between running and throwing every evaluation tick.
	if (Distance > TraceBotConstants::CarryInReleaseDistance)
	{
		bCommitCarryIn = false;
		return;
	}

	// Or held out of the mouth. Without this a carrier walled off by two defenders would stand there
	// refusing to throw for the rest of its life, which is a worse bug than the one being fixed.
	if ((Now - CarryInCommitTime) > TraceBotConstants::CarryInPatienceSeconds)
	{
		bCommitCarryIn = false;
		TRACE_BOT_KIT(CarryInAbandoned);
		UE_LOG(LogTraceGame, Display,
			TEXT("[BotCarryIn] %s gave up after %.1fs, still %.0f uu out"),
			*GetNameSafe(GetPlayerState<APlayerState>()), Now - CarryInCommitTime, Distance);
	}
}

void ATraceBotController::AbortThrow(const TCHAR* Reason)
{
	const UWorld* World = GetWorld();
	const float Now = (World != nullptr) ? static_cast<float>(World->GetTimeSeconds()) : 0.f;

	// SPEC v13 §6 CHANGED WHAT THIS LINE MEANS. Before v13 a throw was pressed and released in the same
	// call and could never leave the input latched, so this was pure defence against a mode switch
	// mid-hover-pass. Now a mode-B throw genuinely holds the button for up to a second, and this is the
	// path that puts it back up when the attempt ends any other way than through a release.
	//
	// It is reached with the button still down in exactly one situation: the Core left this bot's hands
	// mid-charge (killed, intercepted, half-timed). Releasing there cannot launch anything - ATraceCore
	// clears the charge when it takes the Core off a holder and refuses a release with no matching
	// press - so this only tidies the latch. The normal release path clears bPassInputHeld itself
	// BEFORE calling here, so a completed throw never re-enters this branch.
	if (bPassInputHeld)
	{
		if (ATraceCharacter* BotCharacter = GetBotCharacter())
		{
			ApplyPassInput(BotCharacter, false);
		}
		bPassInputHeld = false;
	}

	bPassOwnsAim = false;
	bThrowAtGoal = false;
	PlannedThrowCharge = 1.f;
	ThrowChargeReleaseTime = 0.f;
	PassReceiver = nullptr;
	PassPhase = ETraceBotPassPhase::Cooldown;
	PassCooldownUntilTime = Now + FMath::Max(0.f, UTraceSettings::Get().BotPassCooldownSeconds);

	if (Reason != nullptr)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[BotThrow] %s aborted (%s)"),
			*GetNameSafe(GetPlayerState<APlayerState>()), Reason);
	}
}

void ATraceBotController::AbortPass(const TCHAR* Reason)
{
	UWorld* World = GetWorld();
	ATraceCharacter* BotCharacter = GetBotCharacter();
	const float Now = (World != nullptr) ? static_cast<float>(World->GetTimeSeconds()) : 0.f;

	const bool bWasHolding = (PassPhase == ETraceBotPassPhase::Holding);

	if (bPassInputHeld && BotCharacter != nullptr)
	{
		ApplyPassInput(BotCharacter, false);
	}
	bPassInputHeld = false;
	bPassOwnsAim = false;

	// The rule's own cooldown only starts on a COMPLETED or cancelled pass, and either way this bot
	// should not immediately re-enter. Applying it to a give-up during line-up too costs nothing
	// (the shield never dropped) and stops a bot from thrashing at an unreachable receiver.
	PassPhase = ETraceBotPassPhase::Cooldown;
	PassCooldownUntilTime = Now + FMath::Max(0.f, UTraceSettings::Get().BotPassCooldownSeconds);
	PassReceiver = nullptr;

	UE_LOG(LogTraceGame, Verbose, TEXT("[BotPass] %s aborted (%s)%s"),
		*GetNameSafe(GetPlayerState<APlayerState>()), Reason,
		bWasHolding ? TEXT(" — shield restored") : TEXT(""));
}

ATraceCharacter* ATraceBotController::ChooseReceiver(float& OutAdvantage) const
{
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return nullptr;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	const FVector MyLocation = BotCharacter->GetActorLocation();
	const FVector Goal = GetAttackGoalLocation();
	const float MyGoalDistance = static_cast<float>(FVector::Dist2D(MyLocation, Goal));

	const float HalfLength = HalfFieldLength();

	// Both of these were fixed constants tuned against a 4000uu half-length. On a 12000uu one a
	// 3200uu pass cannot cross a third of the pitch and no receiver is ever in range, so the whole
	// mechanic quietly switches itself off the moment the arena grows.
	float PassRange = FMath::Max(
		FMath::Max(0.f, Settings.BotPassMinRange),
		HalfLength * FMath::Max(0.f, Settings.BotPassRangeFieldFraction));

	// --- MODE B: A PASS IS A THROW, AND A THROW HAS A RANGE. -------------------------------------
	//
	// The number above is 9240 uu on this pitch, and in mode A that is exactly right: the hover pass
	// transfers possession outright and does not have to travel. In mode B the SAME number selects a
	// receiver the Core cannot physically reach - it carries ~7300 uu on the best arc and ~3400 flat
	// under the v5 weight model - so the bot solved an impossible shot, fell through to the fixed
	// 25-degree clearance elevation, and lobbed the Core into open ground two thousand uu short of a
	// teammate. That is the "they will throw short and the change will look like a regression" this
	// pass was warned about, and it is not visible in any counter that only records that a throw
	// happened.
	//
	// So in mode B the receiver search is clamped to what the throw can actually deliver, with the
	// same safety margin the shot at goal uses.
	if (bModeB)
	{
		const ATraceCharacter* Me = GetBotCharacter();
		const FVector ViewLocation = (Me != nullptr) ? Me->GetPawnViewLocation() : MyLocation;
		const float Reach = MaxThrowRange(ViewLocation, Settings.BotAimBodyOffsetZ)
			* TraceBotConstants::ThrowRangeSafetyFraction;

		if (Reach > 1.f)
		{
			PassRange = FMath::Min(PassRange, Reach);
		}
	}

	// A pass to somebody standing on top of the carrier gains nothing and reads in the log as a bug.
	const float MinPassDistance = FMath::Max(0.f, Settings.BotPassMinDistance);

	// Under pressure this is a hot potato, not a play: any open teammate will do, including one
	// level with the carrier. That single relaxation is what turns passing from a rare optimum into
	// something that happens in a match.
	const float PressureRadius = FMath::Max(0.f, Settings.BotPassPressureRadius);
	const bool bUnderPressure = NearestEnemy.IsValid() && NearestEnemyDistSq < FMath::Square(PressureRadius);

	const float RequiredAdvantage = bUnderPressure
		? 0.f
		: HalfLength * FMath::Max(0.f, Settings.BotPassMinGoalAdvantageFieldFraction);

	ATraceCharacter* BestReceiver = nullptr;
	float BestScore = -TNumericLimits<float>::Max();
	OutAdvantage = 0.f;

	for (ATraceCharacter* Mate : LiveTeammates)
	{
		if (Mate == nullptr || !Mate->IsAlive())
		{
			continue;
		}

		const float PassDistance = static_cast<float>(FVector::Dist(MyLocation, Mate->GetActorLocation()));
		if (PassDistance < MinPassDistance)
		{
			continue;
		}
		if (PassDistance > PassRange)
		{
			// Counted only in mode B, where the refusal is a ballistic fact worth measuring rather
			// than the ordinary "too far away" the mode-A rule has always had.
			if (bModeB)
			{
				TRACE_BOT_KIT(PassOutOfThrowRange);
			}
			continue;
		}

		const float MateGoalDistance = static_cast<float>(FVector::Dist2D(Mate->GetActorLocation(), Goal));
		const float Advantage = MyGoalDistance - MateGoalDistance;
		if (Advantage < RequiredAdvantage)
		{
			continue;
		}

		// "Open" is doing real work here, and more than it used to. The rule requires the crosshair
		// to be genuinely ON the receiver for the whole dwell, so a teammate the carrier cannot see
		// is not a candidate at all — under the old thrown-Core rules a blocked pass was a turnover,
		// but under these rules it is simply an attempt that can never complete.
		if (!HasLineOfSight(Mate))
		{
			continue;
		}

		// Prefer the biggest gain, but discount long throws slightly: a distant receiver takes longer
		// to slew onto, and every extra degree of slew is spent inside the vulnerable window.
		const float Score = Advantage - 0.25f * PassDistance;
		if (Score > BestScore)
		{
			BestScore = Score;
			OutAdvantage = Advantage;
			BestReceiver = Mate;
		}
	}

	return BestReceiver;
}

int32 ATraceBotController::CountEnemiesCoveringMe() const
{
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return 0;
	}

	// Same radius the punisher uses, on purpose: the carrier's idea of "someone can shoot me" and
	// the defender's idea of "I can shoot the carrier" have to be the same number, or the two halves
	// of the risk/reward loop are playing different games and neither reads correctly.
	const float Radius = FMath::Max(100.f, UTraceSettings::Get().BotPunishRange);
	const float RadiusSq = FMath::Square(Radius);
	const FVector MyLocation = BotCharacter->GetActorLocation();

	int32 Count = 0;
	for (const ATraceCharacter* Enemy : LiveEnemies)
	{
		if (Enemy == nullptr || !Enemy->IsAlive())
		{
			continue;
		}

		if (FVector::DistSquared(MyLocation, Enemy->GetActorLocation()) > RadiusSq)
		{
			continue;
		}

		if (HasLineOfSight(Enemy))
		{
			++Count;
		}
	}

	return Count;
}

// =================================================================================================
// Movement kit  (spec §5)
//
// Four verbs, four jobs. None of them fire at random, and each is here because of something a bot
// with only "walk and dash" measurably did badly:
//
//   UNSTICK JUMP  — get over the thing that is in the way. With no navmesh the bots' one failure
//                   mode is wedging against the arena's waist-high neon furniture and sidestepping
//                   along it for seconds; a jump is a vertical answer to a problem the horizontal
//                   evade cannot solve, and clears the low trim that causes most of these. This used
//                   to be a BOOST; spec v3 §1 deleted that feature, and the carrier's vertical
//                   escape is the dash charge pool's job now.
//   SLIDE         — a committed burst while already running in a straight line, and a lower profile
//                   while doing it. Started only above BotSlideMinSpeed, because sliding from a
//                   standstill is just crouching in the open.
//   FAST-FALL     — get back to the ground. A bot in the air cannot dash usefully, cannot change
//                   direction and is a clean silhouette; killing the upward momentum is how it stops
//                   being all three of those at once.
//
// The discretionary ones are gated on FTraceBotProfile::MovementTechChance so a difficulty can dial
// how busy the bots look, and every use is counted in [BotKit] so "the bots never slide" is
// answerable. The unstick jump is deliberately NOT gated on that chance: it is a recovery, not a
// flourish, and a bot that only sometimes escapes a wall is a bot that sometimes stands in one for
// the rest of the match.
// =================================================================================================

void ATraceBotController::UpdateMovementTech(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (World == nullptr || BotCharacter == nullptr)
	{
		return;
	}

	const UCharacterMovementComponent* Movement = BotCharacter->GetCharacterMovement();
	if (Movement == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
	const float Now = static_cast<float>(World->GetTimeSeconds());
	const float TechChance = FMath::Clamp(Profile.MovementTechChance, 0.f, 1.f);

	const bool bOnGround = Movement->IsMovingOnGround();
	// Not named "Velocity": UMovementComponent declares a public member of that name, and the project
	// has already lost a Windows build to C4458/C4459 once. clang never warns, so the only defence is
	// not writing the name.
	const FVector BotVelocity = BotCharacter->GetVelocity();
	const float PlanarSpeed = static_cast<float>(BotVelocity.Size2D());

	// --- Cash a slide in as a SLIDE-JUMP ---------------------------------------------------------
	//
	// Spec v4 §1 makes the slide-jump the payoff move: with the flat momentum boost deleted, jumping
	// out of a slide inside the timing window is the ONLY thing that makes sliding worth doing. A
	// bot that slides but never slide-jumps therefore demonstrates the half of the mechanic that no
	// longer pays, and the half that does pay is invisible in every 5v5 the user watches — which is
	// the same as it being untestable.
	//
	// UTraceCharacterMovementComponent::IsSlideJumpWellTimed() is the movement code's own answer to
	// "would a jump RIGHT NOW collect the bonus", read rather than re-derived here so the bot can
	// never drift from the rule a human is being taught. It is a pure query over saved-move state.
	//
	// Ordered BEFORE the slide-end branch below on purpose: once that branch releases crouch the
	// slide is over and the window is spent, so a bot checking afterwards would only ever jump out
	// of slides it had already thrown away.
	if (bCrouchHeld && bOnGround)
	{
		const UTraceCharacterMovementComponent* TraceMovement = Cast<UTraceCharacterMovementComponent>(Movement);
		if (TraceMovement != nullptr && TraceMovement->IsSlideJumpWellTimed())
		{
			bWantsJumpThisTick = true;

			// Release crouch on the same tick. ACharacter::Crouch() sets bWantsToCrouch, and the
			// slide is driven through it, so leaving it held would have the bot re-enter a slide on
			// the frame it lands instead of carrying the speed it just bought.
			ApplyCrouchInput(BotCharacter, false);
			bCrouchHeld = false;
			SlideReadyTime = Now + FMath::Max(0.f, Settings.BotSlideCooldownSeconds);

			TRACE_BOT_KIT(SlideJumps);
		}
	}

	// --- End an in-progress slide ----------------------------------------------------------------
	if (bCrouchHeld && bOnGround && Now >= SlideEndTime)
	{
		ApplyCrouchInput(BotCharacter, false);
		bCrouchHeld = false;
		SlideReadyTime = Now + FMath::Max(0.f, Settings.BotSlideCooldownSeconds);
	}

	// --- Crouch fast-fall ------------------------------------------------------------------------
	//
	// Spec §5: crouch in the air cancels upward momentum. So it is only worth pressing while the
	// momentum is still upward — once the bot is already falling there is nothing left to cancel.
	if (!bOnGround)
	{
		const bool bRising = BotVelocity.Z > 50.f;

		float HeightAboveFloor = TraceBotConstants::GroundProbeLength;
		{
			const FVector Start = BotCharacter->GetActorLocation();
			FCollisionQueryParams Params(TEXT("TraceBotGroundProbe"), /*bTraceComplex=*/false, BotCharacter);
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, Start, Start - FVector(0.f, 0.f, TraceBotConstants::GroundProbeLength), ECC_Visibility, Params))
			{
				HeightAboveFloor = static_cast<float>(Hit.Distance);
			}
		}

		if (bRising && !bCrouchHeld && HeightAboveFloor > FMath::Max(0.f, Settings.BotFastFallMinHeight)
			&& FMath::FRand() < TechChance)
		{
			ApplyCrouchInput(BotCharacter, true);
			bCrouchHeld = true;
			// Held until the bot lands; the ground branch above releases it on the next tick after
			// touchdown, because SlideEndTime is in the past for a fast-fall.
			SlideEndTime = 0.f;
			TRACE_BOT_KIT(FastFalls);
		}

		return;   // Nothing else in this kit is usable in the air.
	}

	// --- Unstick jump ------------------------------------------------------------------------------
	//
	// The surviving half of the deleted boost: a bot that has been pushing into geometry without
	// moving for this long tries a jump, which clears the low neon trim responsible for most wedges.
	// StuckSeconds is reset on the way out so the bot re-earns the next attempt rather than
	// hammering jump every frame while it stays stuck.
	//
	// The new corner banks (spec §7) make this matter more than it did on the flat map: a bot that
	// steers straight at a terrace riser and stalls has exactly this shape.
	if (StuckSeconds > FMath::Max(0.1f, Settings.BotStuckJumpSeconds))
	{
		bWantsJumpThisTick = true;
		StuckSeconds = 0.f;
		TRACE_BOT_KIT(StuckJumps);
	}

	// --- Slide -----------------------------------------------------------------------------------
	//
	// Only while already moving fast in a straight line at something. Sliding sideways in a duel
	// reads as a twitch; sliding down the field with the Core reads as intent, which is the whole
	// reason to give a bot the verb.
	if (!bCrouchHeld && Now >= SlideReadyTime && PlanarSpeed > FMath::Max(50.f, Settings.BotSlideMinSpeed))
	{
		const FVector Heading = BotVelocity.GetSafeNormal2D();
		const bool bCommitted = !DesiredMoveDirection.IsNearlyZero()
			&& FVector::DotProduct(Heading, DesiredMoveDirection) > 0.9f;

		// Sliding takes the bot low and fast and costs it some steering authority, so it is only
		// worth it when the bot is actually going somewhere: carrying, or closing a gap under fire.
		const bool bWorthIt = bIAmCarrier
			|| State == ETraceBotState::HuntCarrier
			|| State == ETraceBotState::EscortCarrier;

		if (bCommitted && bWorthIt && !IsPassing() && FMath::FRand() < TechChance * DeltaSeconds * 2.f)
		{
			ApplyCrouchInput(BotCharacter, true);
			bCrouchHeld = true;
			SlideEndTime = Now + FMath::Max(0.1f, Settings.BotSlideHoldSeconds);
			TRACE_BOT_KIT(Slides);
		}
	}
}

// =================================================================================================
// ABILITIES  (spec v15 §3)
//
// The DECISION is in FTraceBotAbilityBrain (AI/TraceBotAbilityBrain.h). What lives here is the
// plumbing: gathering the situation the brain is allowed to see, and turning its orders into the
// same key presses a human makes. Nothing below decides what an ability does to anybody — every
// effect still runs inside the character's own ability set, behind the spec §4 choke point.
// =================================================================================================

void ATraceBotController::UpdateAbilities(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (World == nullptr || BotCharacter == nullptr)
	{
		AbilityOrders = FTraceBotAbilityOrders();
		return;
	}

	FTraceBotAbilitySituation Situation;
	Situation.Self               = BotCharacter;
	Situation.BotController      = this;
	Situation.Abilities          = UTraceAbilityComponent::Get(BotCharacter);
	Situation.BotState           = State;
	Situation.Enemies            = &LiveEnemies;
	Situation.Carrier            = Carrier.Get();
	Situation.bIAmCarrier        = bIAmCarrier;
	Situation.bTeammateIsCarrier = bTeammateIsCarrier;
	Situation.bEnemyIsCarrier    = bEnemyIsCarrier;
	Situation.ShootTarget        = ShootTarget.Get();
	// Lining up counts as passing here as well as holding: the aim is already committed to a
	// team-mate, and an ability that grabbed the crosshair back would abort the attempt.
	Situation.bPassing           = (PassPhase == ETraceBotPassPhase::Lining
	                                || PassPhase == ETraceBotPassPhase::Holding
	                                || PassPhase == ETraceBotPassPhase::Charging);
	Situation.DesiredMoveDirection = DesiredMoveDirection;
	Situation.AttackGoal         = GetAttackGoalLocation();
	Situation.DashCharges        = GetDashCharges();
	Situation.Now                = static_cast<float>(World->GetTimeSeconds());

	AbilityBrain.Plan(Situation, DeltaSeconds, AbilityOrders);

	// --- apply the two orders that are not key presses --------------------------------------------

	// Mace's spike pull is cancelled by ANY movement input (spec v14 §6), and a bot steers every
	// frame — so the only way for a bot to use the pull at all is to genuinely let go of the stick.
	// Checked before the redirect because the two are mutually exclusive and this one is a hard rule
	// rather than a preference.
	if (AbilityOrders.bSuppressMove)
	{
		DesiredMoveDirection = FVector::ZeroVector;
	}
	// Rocco's midair redirect only means anything if the bot is also steering the new way on the same
	// frame — the ability blends the pawn's velocity toward its ACCELERATION, so with no steering
	// change it is a small hop and nothing else.
	else if (AbilityOrders.bOverrideMove && !AbilityOrders.MoveDirection.IsNearlyZero())
	{
		DesiredMoveDirection = AbilityOrders.MoveDirection.GetSafeNormal();
	}

	// V is a LEVEL, so it is mirrored rather than pressed: the edge is wherever the level changes,
	// and every other exit from this class (death, unpossess) releases it explicitly.
	if (AbilityOrders.bSecondaryHeld != bSecondaryInputHeld)
	{
		bSecondaryInputHeld = AbilityOrders.bSecondaryHeld;
		ApplySecondaryInput(BotCharacter, bSecondaryInputHeld);

		if (Situation.Abilities != nullptr)
		{
			TraceBotAbilityStats::NoteSecondary(Situation.Abilities->GetCharacterId(), bSecondaryInputHeld,
				bSecondaryInputHeld ? AbilityOrders.SecondaryReason : nullptr);

			// SPEC v18 §3. Roxie's rocket is the one V ability that is a DECISION with a reaction delay
			// in front of it (Mace's suspend and Slimeball's stick are levels raised by a decision that
			// has already been counted against its own key), so its latency is reported here and only
			// on the tick the brain says the tap resolved. Without the flag this branch would also
			// count Slimeball's stick, whose latency the jump above has already reported — and the
			// reaction mean is the number spec v15 §3 is judged on.
			if (AbilityOrders.bSecondaryPressedNow)
			{
				TraceBotAbilityStats::NoteLatency(AbilityOrders.MeasuredLatencySeconds);
			}
		}
	}

	// A dash asked for by an ability goes through the SAME charge model everything else uses, so a
	// Chut bot cannot bash its way out of the reserve a carrier is supposed to keep back.
	//
	// Counted only in [BotAbility], deliberately NOT in the existing [BotKit] dash counters: those
	// three (trace / escape / duel) are a partition of why a dash was spent, and folding a fourth
	// reason into one of them would corrupt a number this project has already tuned against twice.
	if (AbilityOrders.bDash && RequestDash(/*bReserveLast=*/bIAmCarrier))
	{
		if (Situation.Abilities != nullptr)
		{
			TraceBotAbilityStats::NoteAbilityDash(Situation.Abilities->GetCharacterId());
			TraceBotAbilityStats::NoteLatency(AbilityOrders.MeasuredLatencySeconds);
		}
	}

	// The jump is raised here and consumed in ApplySteering, where the ability hook gets first
	// refusal — same ordering argument as the dash.
	if (AbilityOrders.bJump)
	{
		bWantsJumpThisTick = true;

		// The jump is an ability press like any other and it was made to wait like any other, so it
		// belongs in the reaction sample. It was missing from it until spec v18 §3: with Rocco's
		// redirect, Mace's "jump to suspend" and now Slimeball's "jump to the wall" all arriving on
		// this key, leaving it out was quietly dropping the majority of the presses this pass makes.
		if (Situation.Abilities != nullptr)
		{
			TraceBotAbilityStats::NoteLatency(AbilityOrders.MeasuredLatencySeconds);
		}
	}
}

void ATraceBotController::ApplyAbilityInputs()
{
	if (!AbilityOrders.bActivate)
	{
		return;
	}
	AbilityOrders.bActivate = false;   // one press per order, whatever happens below

	ATraceCharacter* BotCharacter = GetBotCharacter();
	UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(BotCharacter);
	if (Abilities == nullptr)
	{
		return;
	}

	const ETraceCharacterId MyCharacter = Abilities->GetCharacterId();
	if (MyCharacter == ETraceCharacterId::None)
	{
		return;
	}

	// The cooldown BEFORE the press, so "did it fire" is a fact rather than an inference. A press
	// that lands moves the deadline; one refused by CanActivate or fizzled (Mace's spike finding no
	// wall) leaves it exactly where it was.
	const float CooldownBefore = Abilities->GetActivatedCooldownRemaining();

	// Mace's reactivation is the one press that is NOT an activation, and telling the two apart
	// afterwards is impossible (a pull charges no cooldown, and neither does a refusal), so ask
	// before pressing.
	bool bWasSpikePull = false;
	if (const UTraceAbilitySetMace* Mace = Abilities->GetAbilitySetAs<UTraceAbilitySetMace>())
	{
		bWasSpikePull = Mace->IsSpikeReadyToPull();
	}

	// SPEC v18 §2 GAVE THE COOLDOWN TEST A SECOND BLIND SPOT, and it is Mace's exactly.
	//
	// Elle's SNAP is one cast made of two presses, and press 1 deliberately charges NOTHING
	// (UTraceAbilitySetElle::GetActivatedCooldownSeconds returns 0 while the 4 s window is open) so
	// that press 2 can reach ActivateAbility at all. The cooldown therefore does not move on the press
	// that puts the FIRST gate on the ground — and a first gate is unambiguously the ability working.
	// Read before, compared after, for the same reason the cooldown is.
	const UTraceAbilitySetElle* Elle = Abilities->GetAbilitySetAs<UTraceAbilitySetElle>();
	const bool bElleWasIdle = (Elle != nullptr) && !Elle->IsAwaitingSecondGate();

	// THE HUMAN'S PATH, not a private one. RouteActivatePressed is where "a second E means PULL"
	// lives — UTraceAbilityComponent::TryActivate() cannot carry it, because the throw has just
	// started a 20 s cooldown. See TraceAbilityInputRelay.h.
	UTraceAbilityInputRelay::RouteActivatePressed(BotCharacter->GetPlayerState<APlayerState>());

	const bool bElleOpenedTheWindow = bElleWasIdle && Elle != nullptr && Elle->IsAwaitingSecondGate();

	const bool bFired = bWasSpikePull
		|| bElleOpenedTheWindow
		|| (Abilities->GetActivatedCooldownRemaining() > CooldownBefore);

	TraceBotAbilityStats::NoteActivate(MyCharacter, bFired, bWasSpikePull, AbilityOrders.Reason,
		GetPlayerState<APlayerState>());
	TraceBotAbilityStats::NoteLatency(AbilityOrders.MeasuredLatencySeconds);
}

void ATraceBotController::UpdateAutoCharacter()
{
	if (CVarTraceBotAutoCharacter.GetValueOnAnyThread() == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const float Now = static_cast<float>(World->GetTimeSeconds());
	if (Now < NextAutoCharacterTime)
	{
		return;
	}
	NextAutoCharacterTime = Now + 2.f;

	APlayerState* MyState = GetPlayerState<APlayerState>();
	UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(MyState);
	if (MyState == nullptr || Abilities == nullptr)
	{
		return;
	}

	if (Abilities->GetCharacterId() != ETraceCharacterId::None)
	{
		return;   // somebody has already given this bot a character. Nothing to do.
	}

	// Characters off (the §3 toggle, or mode A's freeze) means characterless is the CORRECT state and
	// this function must not fight it. UTraceAbilityComponent::AreCharactersEnabled is the one answer
	// to that question for the whole project; asked rather than re-derived.
	if (!UTraceAbilityComponent::AreCharactersEnabled(this))
	{
		return;
	}

	// ---- DEFER TO A SELECTION THAT IS STILL IN PROGRESS ------------------------------------------
	//
	// Spec v15 §2's ordering requirement, respected from the outside: "bots must not pick until every
	// human on their team has locked in". This function cannot know whose turn it is, but it can see
	// that SOMEBODY on this team still has the select screen open — and while that is true, picking
	// would be exactly the queue-jump §2 forbids. The existing 30 s select timeout is what guarantees
	// this is not a deadlock: it closes every screen and auto-assigns, and only then does this run.
	//
	// Costs one scan of at most ten player states, on a 2 s poll.
	if (const AGameStateBase* BaseState = World->GetGameState())
	{
		const ATracePlayerState* MyTraceState = Cast<ATracePlayerState>(MyState);
		for (const APlayerState* Entry : BaseState->PlayerArray)
		{
			const ATracePlayerState* Other = Cast<ATracePlayerState>(Entry);
			if (Other == nullptr || Other == MyTraceState)
			{
				continue;
			}
			if (MyTraceState != nullptr && Other->Team != MyTraceState->Team)
			{
				continue;   // per-team uniqueness is per team; the other side's screen is not our business
			}
			if (Other->IsCharacterSelectOpen())
			{
				return;
			}
		}
	}

	// A -TraceBotCharacterOverride SCAFFOLD USED TO SIT HERE and is deleted. It cleared
	// APlayerState::bIsABot so a bot could hold a character at all, because two places then enforced
	// spec v14 §3's "bots remain characterless" — UTraceAbilityComponent::ServerSetCharacter refused
	// bots outright, and ATraceGameMode::PollCharacterSelect swept any character a bot was holding
	// back off it. Spec v15 §2 reversed both, so the scaffold is dead: bots hold characters through
	// the ordinary path now. Do not bring it back; forcing bIsABot false also breaks scoreboard and
	// kill-feed bot labelling, which is exactly the sort of measurement distortion §2 no longer needs.

	// A character nobody on this team holds, chosen at RANDOM among the free ones — spec v15 §2, "all
	// loading in with randomly chosen characters". The framework's own picker, so per-team uniqueness
	// is its rule and not a second copy of it here.
	//
	// PickRandomFreeCharacterFor, not PickFreeCharacterFor (which returns the lowest free id): this is
	// the second of the two paths that can assign a bot a character — ATraceGameMode::
	// PollCharacterSelect's 4 Hz fill is the first, and it wins nearly every race — and the two must
	// obey the same sentence of the spec. A bot filled by whichever path happened to get there first
	// must not be the difference between "random" and "always Rocco".
	const ETraceCharacterId Wanted = UTraceAbilityComponent::PickRandomFreeCharacterFor(MyState);
	if (Wanted == ETraceCharacterId::None)
	{
		return;   // the team is full of characters. Fine: this bot plays as a Mannequin.
	}

	Abilities->ServerSetCharacter(Wanted);

	if (Abilities->GetCharacterId() == Wanted)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[BotAbility] %s took %s (Trace.Bot.AutoCharacter)."),
			*MyState->GetPlayerName(), TraceCharacterIdToString(Wanted));
		return;
	}

	// REFUSED — say so once instead of retrying in silence for the whole match, but note that this is
	// no longer the alarm it was.
	//
	// Since spec v15 §2 there are exactly two ways ServerSetCharacter refuses a bot, and BOTH are
	// TEMPORARY and self-resolving, so this poll picking the character up two seconds later is the
	// expected outcome rather than a failure:
	//
	//   1. THE ORDERING GATE. A human on this bot's team has not settled yet (§2: "the computers
	//      should wait for any actual humans on its team to choose"). The 30 s select timeout is what
	//      guarantees it ends — and note the screen-open scan above already catches the common case,
	//      so reaching here means a human whose screen is shut but who holds no character yet.
	//   2. PER-TEAM UNIQUENESS. Somebody took this character between the pick above and the request —
	//      the game mode's 4 Hz fill is the other assignment path and it usually gets there first.
	//
	// It is a Log and not a Warning for that reason. What it must NOT say is the thing it used to:
	// that ServerSetCharacter "still enforces spec v14 §3, bots stay characterless". That rule is
	// gone, and a reader chasing it would be chasing code that no longer exists.
	static bool bWarnedRefused = false;
	if (!bWarnedRefused)
	{
		bWarnedRefused = true;
		UE_LOG(LogTraceGame, Log,
			TEXT("[BotAbility] %s asked for %s and was refused — either a human on its team has not settled "
			     "yet (spec v15 §2's ordering rule) or a team-mate took it first. Both are temporary; this "
			     "poll retries every 2s and the select timeout bounds the wait. Logged once."),
			*MyState->GetPlayerName(), TraceCharacterIdToString(Wanted));
	}
}

// =================================================================================================
// Combat
// =================================================================================================

FVector ATraceBotController::GetAimPointOn(const ATraceCharacter* Target) const
{
	if (Target == nullptr)
	{
		return FVector::ZeroVector;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// AimZone is rolled once per acquisition in UpdateCombat, not per tick — see the field comment.
	float OffsetZ = Settings.BotAimBodyOffsetZ;
	if (AimZone > 0)
	{
		OffsetZ = Settings.BotAimHeadOffsetZ;
	}
	else if (AimZone < 0)
	{
		OffsetZ = Settings.BotAimLegOffsetZ;
	}

	// Scale the offsets with the capsule so a crouching or sliding target is still aimed at
	// correctly. The offsets are authored against the standing 88uu half height; a slide halves it,
	// and a head shot aimed 62uu above the centre of a crouched capsule is a clean miss over the top.
	float HalfHeightScale = 1.f;
	if (const UCapsuleComponent* Capsule = Target->GetCapsuleComponent())
	{
		HalfHeightScale = FMath::Clamp(Capsule->GetScaledCapsuleHalfHeight() / 88.f, 0.25f, 2.f);
	}

	return Target->GetActorLocation() + FVector(0.f, 0.f, OffsetZ * HalfHeightScale);
}

void ATraceBotController::UpdateCombat(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (World == nullptr || BotCharacter == nullptr)
	{
		return;
	}

	const float Now = static_cast<float>(World->GetTimeSeconds());
	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();

	// --- The pass owns the aim outright ----------------------------------------------------------
	//
	// While lining up or holding a pass the crosshair has one job, and it is not shooting. This is
	// first in the function rather than folded into the aim branch below because the two must never
	// both be honoured: a bot that let the combat aim bias its pass aim would drift off the receiver
	// and cancel its own pass, which is the rule's own failure mode arriving by accident.
	if (bPassOwnsAim)
	{
		if (bTriggerHeld)
		{
			BotCharacter->DoFireReleased();
			bTriggerHeld = false;
		}

		const FRotator DesiredRotation = (PassAimPoint - BotCharacter->GetPawnViewLocation()).Rotation();
		const float TurnRate = FMath::Max(10.f, Profile.AimTurnRateDegrees);
		FRotator NewRotation = FMath::RInterpConstantTo(GetControlRotation(), DesiredRotation, DeltaSeconds, TurnRate);
		NewRotation.Roll = 0.f;
		SetControlRotation(NewRotation);
		return;
	}

	// --- ...and so does an AIMED ability (spec v15 §3) ---------------------------------------------
	//
	// Mace's spike goes where she is LOOKING and Oyster's jar is lobbed along the aim direction, so
	// for those the crosshair has a job that is not shooting — exactly as it does during a pass, and
	// handled the same way rather than by biasing the combat aim, because a blended aim would satisfy
	// neither. Ranked BELOW the pass: dropping the shield is the more dangerous commitment and the
	// brain already declines to plan anything while one is in flight.
	//
	// The trigger comes off for the duration, for the same reason it does during a pass: the aim
	// point here is frequently a WALL (Mace) or a patch of floor (Oyster), and a held trigger would
	// empty a burst into it. The gate below is measured against the bot's own aim point, so without
	// this a bot lining up a spike would read as "on target" and fire the whole time.
	if (AbilityOrders.bOwnsAim)
	{
		const FRotator DesiredRotation = (AbilityOrders.AimPoint - BotCharacter->GetPawnViewLocation()).Rotation();
		const float TurnRate = FMath::Max(10.f, Profile.AimTurnRateDegrees);
		FRotator NewRotation = FMath::RInterpConstantTo(GetControlRotation(), DesiredRotation, DeltaSeconds, TurnRate);
		NewRotation.Roll = 0.f;
		SetControlRotation(NewRotation);

		if (bTriggerHeld)
		{
			BotCharacter->DoFireReleased();
			bTriggerHeld = false;
		}
		return;
	}

	// --- Reacquire delay ------------------------------------------------------------------------
	// A bot whose target dies must go blind for a moment. Without this the opening of every match
	// was a simultaneous ten-way wipe: each bot killed someone and was already tracking the next
	// player on the very next frame, so the whole team traded out inside a second and a half.
	//
	// bHadAcquiredTarget is tracked separately from the weak pointer on purpose: killing a pawn
	// destroys it and nulls the pointer, which is otherwise indistinguishable from "never had one".
	if (bHadAcquiredTarget)
	{
		const ATraceCharacter* Previous = AcquiredTarget.Get();
		if (Previous == nullptr || !Previous->IsAlive())
		{
			BlindUntilTime = Now + FMath::Max(0.f, Profile.ReacquireDelaySeconds);
			AcquiredTarget = nullptr;
			bHadAcquiredTarget = false;
		}
	}

	const bool bBlind = (Now < BlindUntilTime);
	if (bBlind)
	{
		// Visibly loses track of you: the bot stops aiming and goes back to looking where it walks.
		bWantsToAim = false;
		ShootTarget = nullptr;
	}

	ATraceCharacter* CurrentTarget = bWantsToAim ? ShootTarget.Get() : nullptr;

	// --- Reaction clock -------------------------------------------------------------------------
	// Restarted whenever the target changes, so flanking a bot genuinely buys you its whole reaction
	// window rather than nothing.
	if (CurrentTarget != AcquiredTarget.Get())
	{
		AcquiredTarget = CurrentTarget;
		bHadAcquiredTarget = (CurrentTarget != nullptr);
		TargetAcquiredTime = (CurrentTarget != nullptr) ? Now : -1000.f;

		const float Jitter = FMath::Clamp(Profile.ReactionJitterFraction, 0.f, 0.95f);
		// A high-skill-bias bot reacts faster, so the same profile produces a spread of opponents
		// rather than five clones firing on the same frame.
		const float PersonalityScale = 1.35f - 0.7f * PersonalitySkillBias;
		CurrentReactionDelay = FMath::Max(0.f, Profile.ReactionTimeSeconds)
			* PersonalityScale
			* FMath::FRandRange(1.f - Jitter, 1.f + Jitter);

		// --- WHICH BODY ZONE, decided once per acquisition ---------------------------------------
		//
		// Damage is positional now: head 100 (an instant kill), body 40, leg 25. Rolling this per
		// tick would have a bot flicker between the head and the chest and reliably hit the neck
		// gap between them; rolling it per acquisition makes it a commitment, and makes the profile
		// number mean what it says — "this bot goes for the head three engagements in ten".
		//
		// Easy rolls zero here, always. See FTraceBotProfile::HeadshotAimFraction for why that, and
		// not a wider error cone, is what keeps Easy at its measured ~0.72 deaths/minute.
		AimZone = (FMath::FRand() < FMath::Clamp(Profile.HeadshotAimFraction, 0.f, 1.f)) ? 1 : 0;

		// Force a fresh error roll for the new target, so switching does not inherit the wobble that
		// happened to be aimed at the last one.
		AimErrorNextRefreshTime = 0.f;
	}

	// --- Aim ------------------------------------------------------------------------------------
	FRotator DesiredRotation = GetControlRotation();

	if (bWantsToAim && CurrentTarget != nullptr)
	{
		// Re-derive the aim point every tick so the chosen ZONE tracks a target that is moving,
		// crouching or sliding. The zone itself is fixed for the acquisition; only the point moves.
		DesiredAimPoint = GetAimPointOn(CurrentTarget);

		if (Now >= AimErrorNextRefreshTime)
		{
			AimErrorNextRefreshTime = Now + FMath::FRandRange(TraceBotConstants::AimErrorRefreshMin, TraceBotConstants::AimErrorRefreshMax);

			const FVector ViewLocation = BotCharacter->GetPawnViewLocation();
			FVector ToTarget = DesiredAimPoint - ViewLocation;
			const float Range = static_cast<float>(ToTarget.Size());
			const FVector LineOfSight = ToTarget.GetSafeNormal();

			// Speed ACROSS the line of sight, not raw speed: a target running straight at a bot is
			// easy and should stay easy. This is the term that makes strafing a real defence and
			// stops bots from tracking a sprinter across the arena as if they were standing still.
			const FVector TargetVelocity = CurrentTarget->GetVelocity();
			const FVector CrossVelocity = TargetVelocity - LineOfSight * FVector::DotProduct(TargetVelocity, LineOfSight);
			const float CrossSpeed = static_cast<float>(CrossVelocity.Size());

			float ErrorScale = FMath::Max(0.f, Profile.AimErrorDegrees)
				+ FMath::Max(0.f, Profile.AimErrorPerThousandRange) * (Range / 1000.f)
				+ FMath::Max(0.f, Profile.AimErrorPerThousandCrossSpeed) * (CrossSpeed / 1000.f);

			ErrorScale *= (1.35f - 0.7f * PersonalitySkillBias);
			ErrorScale = FMath::Min(ErrorScale, FMath::Max(0.f, Profile.AimErrorMaxDegrees));

			AimError.X = FMath::FRandRange(-ErrorScale, ErrorScale);

			// Vertical error is halved, and that matters much more than it used to. With positional
			// damage the vertical axis is what decides head / body / leg, so a full-width vertical
			// wobble would turn every deliberate zone choice into a coin flip and make
			// HeadshotAimFraction meaningless as a difficulty dial.
			AimError.Y = FMath::FRandRange(-ErrorScale, ErrorScale) * 0.5f;
		}

		DesiredRotation = (DesiredAimPoint - BotCharacter->GetPawnViewLocation()).Rotation();
		DesiredRotation.Yaw += AimError.X;
		DesiredRotation.Pitch = FMath::Clamp(DesiredRotation.Pitch + AimError.Y, -80.f, 80.f);
	}
	else
	{
		// Not shooting: look where we are going, so the muzzle (which is yaw-aligned to the control
		// rotation) does not trail behind the body.
		const FVector Facing = DesiredMoveDirection.IsNearlyZero() ? BotCharacter->GetActorForwardVector() : DesiredMoveDirection;
		DesiredRotation = FRotator(0.f, Facing.Rotation().Yaw, 0.f);
	}

	// Finite slew rate is what stops a bot from being an instant-snap aimbot; it is also what makes
	// strafing a real counterplay against them.
	const float TurnRate = FMath::Max(10.f, Profile.AimTurnRateDegrees);
	FRotator NewRotation = FMath::RInterpConstantTo(GetControlRotation(), DesiredRotation, DeltaSeconds, TurnRate);
	NewRotation.Roll = 0.f;
	SetControlRotation(NewRotation);

	// --- Trigger --------------------------------------------------------------------------------
	//
	// The fire gate is measured against DesiredRotation — the bot's OWN, error-offset aim point —
	// and not against the true line to the target.
	//
	// That inversion is the whole aim model. The old code compared the bot's rotation to the exact
	// bearing of the target, which meant raising the error past the fire cone did not make bots
	// miss, it made them stop shooting. Measured against its own aim point instead, a bot fires
	// promptly and the error is a genuine miss.
	bool bOnTarget = false;

	// A CARRIER cannot shoot at all: mouse1 is the pass now. The gate is IsCarrier rather than
	// "am I mid-pass", because the weapon is unavailable for the whole carry, not just the window.
	if (CurrentTarget != nullptr && CurrentTarget->IsAlive() && !BotCharacter->IsCarrier() && !bBlind)
	{
		// Do not waste a burst on a shielded carrier when the pawn will tell us it is shielded. If it
		// will not (see the capability adapter), this reads false and the punisher fires anyway,
		// which is wasteful but correct — the shot that lands the instant the shield drops is the
		// one that matters, and there is no ammo to conserve.
		const bool bTargetShielded = IsCarrierShielded(CurrentTarget);

		if (!bTargetShielded && Now - TargetAcquiredTime >= CurrentReactionDelay)
		{
			const float YawError = FMath::Abs(FRotator::NormalizeAxis(NewRotation.Yaw - DesiredRotation.Yaw));
			const float PitchError = FMath::Abs(FRotator::NormalizeAxis(NewRotation.Pitch - DesiredRotation.Pitch));

			bOnTarget = (YawError + PitchError) < FMath::Max(0.5f, Profile.FireConeDegrees);
		}
	}

	// --- Burst discipline -----------------------------------------------------------------------
	// A held trigger on target kills in well under half a second, and nothing about reaction time or
	// aim error survives that. Bursting is the dial that actually sets bot DPS, and the gap between
	// bursts is the window a player uses to break line of sight or close.
	bool bShouldFire = false;

	if (bOnTarget && Now >= BurstRestUntilTime)
	{
		if (!bTriggerHeld)
		{
			BurstEndTime = Now + FMath::FRandRange(
				FMath::Max(0.02f, Profile.BurstDurationMin),
				FMath::Max(0.02f, Profile.BurstDurationMax));
			bShouldFire = true;
		}
		else
		{
			bShouldFire = (Now < BurstEndTime);
		}
	}

	if (bShouldFire && !bTriggerHeld)
	{
		BotCharacter->DoFirePressed();
		bTriggerHeld = true;

		if (State == ETraceBotState::PunishPasser)
		{
			TRACE_BOT_KIT(PunisherShots);
		}
	}
	else if (!bShouldFire && bTriggerHeld)
	{
		BotCharacter->DoFireReleased();
		bTriggerHeld = false;

		// Rest after EVERY release, not only after a burst that ran its full length. A burst cut
		// short because the target ducked behind cover must not be followed by an instant new burst
		// the moment they step back out — that is just continuous fire wearing a costume.
		BurstRestUntilTime = FMath::Max(
			BurstRestUntilTime,
			Now + FMath::FRandRange(
				FMath::Max(0.f, Profile.BurstRestMin),
				FMath::Max(0.f, Profile.BurstRestMax)));
	}
}

ATraceCharacter* ATraceBotController::FindBestShootTarget() const
{
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	if (BotCharacter == nullptr)
	{
		return nullptr;
	}

	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();

	// Awareness and willingness to shoot are two different numbers. MaxEngagementRange is the one
	// that decides whether a trigger is ever pulled, and it sits well inside SightRange so that a
	// player can be seen — and reacted to — without being immediately shot at from across the map.
	const float EngageRange = FMath::Max(100.f, FMath::Min(Profile.SightRange, Profile.MaxEngagementRange));
	const float EngageRangeSq = FMath::Square(EngageRange);

	const UTraceSettings& Settings = UTraceSettings::Get();
	const FVector MyLocation = BotCharacter->GetActorLocation();

	// Score is squared distance scaled by a preference weight, so "nearest" is still the default rule
	// and the weights only bend it. Lower is better.
	//
	// THE HUMAN WEIGHT
	// Straight nearest-first made the one human in the match the LEAST shot-at actor on the field:
	// nine bots are packed around the objective, so a bot with the player cleanly in the open at
	// 2000uu would pick a teammate-of-a-teammate at 1500uu essentially every time. In a singleplayer
	// match the player is the point; a modest weight makes them a preferred target without making
	// the bots ignore each other, which would look obviously scripted.
	const float HumanWeight = FMath::Clamp(Settings.BotHumanTargetBias, 0.05f, 1.f);

	ATraceCharacter* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();

	// The target this bot is already lined up on, if it is still worth shooting. See the switch
	// hysteresis below.
	ATraceCharacter* Sticky = AcquiredTarget.Get();
	float StickyScore = TNumericLimits<float>::Max();

	for (ATraceCharacter* Other : LiveEnemies)
	{
		if (Other == nullptr)
		{
			continue;
		}

		// Shooting a SHIELDED carrier is a rules-level no-op, and spending the reaction window on
		// them instead of on a killable enemy is worse than not shooting at all.
		//
		// But a carrier whose shield is DOWN — mid-pass — is the single most valuable target on the
		// field: killing them is one of the three ways the Core changes hands, and the killer takes
		// it. So the test is the SHIELD, not the carry, and it is read out of ATraceCore, which is
		// the same fact UTraceHealthComponent::IsInvulnerable consults.
		if (IsCarrierShielded(Other))
		{
			continue;
		}

		const float DistanceSq = static_cast<float>(FVector::DistSquared(MyLocation, Other->GetActorLocation()));
		if (DistanceSq > EngageRangeSq)
		{
			continue;
		}

		const float Score = DistanceSq * (Other->IsPlayerControlled() ? HumanWeight : 1.f);

		// The line trace is the expensive part, so it goes last — after both the range reject and a
		// score that cannot win. Skipping it for the sticky target would be wrong, though: its score
		// is needed below even when some other candidate is nominally better.
		const bool bCouldWin = (Score < BestScore);
		if (!bCouldWin && Other != Sticky)
		{
			continue;
		}

		if (!HasLineOfSight(Other))
		{
			continue;
		}

		if (Other == Sticky)
		{
			StickyScore = Score;
		}

		if (bCouldWin)
		{
			BestScore = Score;
			Best = Other;
		}
	}

	// --- Switch hysteresis ------------------------------------------------------------------------
	//
	// Changing target restarts the whole reaction clock (see UpdateCombat). Around a contested
	// objective the nearest enemy flips every second or two as bodies move, so a bot re-rolled its
	// ~1s Easy reaction delay faster than it could ever finish one and simply never pulled the
	// trigger: measured at 19.3% of bot-ticks aiming but 1.9% firing.
	//
	// So a challenger has to be meaningfully better than the target the bot is already tracking,
	// not merely better. The fraction is in SQUARED distance, so the default 0.45 means "roughly
	// two-thirds of the current target's range".
	if (Best != nullptr && Sticky != nullptr && Best != Sticky && StickyScore < TNumericLimits<float>::Max())
	{
		if (Sticky->IsAlive() && BestScore > StickyScore * FMath::Clamp(Settings.BotTargetSwitchFraction, 0.f, 1.f))
		{
			Best = Sticky;
		}
	}

	return Best;
}

/**
 * Does a live Slimewall stand between these two points?
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT A COLLISION CHANNEL. Spec v18 §2 says Slimeball's wall "can be
 * shot through, but obstructs vision", so the slab is deliberately NoCollision on every channel —
 * and this project's hitscan traces on ECC_Visibility. Giving the slab a Visibility response to
 * block bot sight would therefore also eat bullets and break the ability's own headline clause.
 *
 * The consequence, caught by verification rather than by play: the wall blocked a HUMAN's vision and
 * not a BOT's, because ATraceBotController::HasLineOfSight is an ECC_Visibility ray. Half the
 * ability simply did not exist against AI, which is the mode this team playtests most.
 *
 * So vision is asked as a geometry trace AND this test. Slimewalls are rare (a handful alive, 4 s
 * each), so the iteration is cheap; the maths is an exact segment-vs-OBB slab test rather than a
 * bounds approximation, because a wall you can see round the edge of is worse than no wall.
 *
 * Named distinctively and file-static on purpose: UBT concatenates .cpp files into one translation
 * unit, so a generic helper name here would collide with another file's (see
 * Scripts/check-jumbo-build-collisions.py).
 */
static bool TraceBotSlimewallBlocksSegment(const UWorld* WorldPtr, const FVector& SegStart, const FVector& SegEnd)
{
	if (WorldPtr == nullptr)
	{
		return false;
	}

	for (TActorIterator<ATraceSlimewall> It(const_cast<UWorld*>(WorldPtr)); It; ++It)
	{
		const ATraceSlimewall* Wall = *It;
		if (!IsValid(Wall))
		{
			continue;
		}

		const FVector Extents = Wall->GetHalfExtentsUU();
		if (Extents.IsNearlyZero())
		{
			continue;
		}

		// The slab's own basis: X along the aim (its thickness), Y across it, Z up — the order
		// ServerSpawn documents for InHalfExtents.
		FVector AxisX = Wall->GetWallAim().GetSafeNormal();
		if (AxisX.IsNearlyZero())
		{
			continue;
		}
		const FVector AxisZ = FVector::UpVector;
		const FVector AxisY = FVector::CrossProduct(AxisZ, AxisX).GetSafeNormal();

		// Segment into the slab's local space, then the standard slab (ray-vs-AABB) test.
		const FVector Centre = Wall->GetWallCenter();
		const FVector LocalStart(
			FVector::DotProduct(SegStart - Centre, AxisX),
			FVector::DotProduct(SegStart - Centre, AxisY),
			FVector::DotProduct(SegStart - Centre, AxisZ));
		const FVector LocalEnd(
			FVector::DotProduct(SegEnd - Centre, AxisX),
			FVector::DotProduct(SegEnd - Centre, AxisY),
			FVector::DotProduct(SegEnd - Centre, AxisZ));

		const FVector Delta = LocalEnd - LocalStart;
		float EnterT = 0.f;
		float ExitT = 1.f;
		bool bMiss = false;

		for (int32 Axis = 0; Axis < 3 && !bMiss; ++Axis)
		{
			const float Origin = LocalStart[Axis];
			const float Direction = Delta[Axis];
			const float Limit = Extents[Axis];

			if (FMath::IsNearlyZero(Direction))
			{
				// Parallel to this pair of faces: only a miss if it starts outside them.
				bMiss = (Origin < -Limit) || (Origin > Limit);
				continue;
			}

			float Near = (-Limit - Origin) / Direction;
			float Far = (Limit - Origin) / Direction;
			if (Near > Far)
			{
				Swap(Near, Far);
			}

			EnterT = FMath::Max(EnterT, Near);
			ExitT = FMath::Min(ExitT, Far);
			bMiss = (EnterT > ExitT);
		}

		if (!bMiss)
		{
			return true;
		}
	}

	return false;
}

bool ATraceBotController::HasLineOfSight(const AActor* Target) const
{
	const UWorld* World = GetWorld();
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	if (World == nullptr || BotCharacter == nullptr || Target == nullptr)
	{
		return false;
	}

	// ECC_Visibility rather than a pawn-blocking channel on purpose: the character capsule uses the
	// stock Pawn profile, whose one custom response is Visibility = Ignore. So this trace is blocked
	// by arena geometry (walls, the centre pedestal) and by nothing else — which is exactly the
	// question being asked. Testing against a pawn-blocking channel would instead report "no LOS"
	// every time a teammate wandered in front, and the bots would go quiet in a crowd.
	FCollisionQueryParams Params(TEXT("TraceBotLineOfSight"), /*bTraceComplex=*/false, BotCharacter);
	Params.AddIgnoredActor(Target);

	// FROM THE EYE, NOT THE MUZZLE.
	//
	// GetMuzzleLocation() is the eye stepped along the full aim rotation — including pitch — which is
	// a weapon-barrel position, not a perception one. It can end up on the far side of a surface the
	// bot is pressed against, and a trace that STARTS inside a blocking primitive reports a hit, so a
	// bot hugging cover reads as blind in every direction at once. GetPawnViewLocation() is inside
	// the capsule, and the capsule is ignored below, so it is always a legal origin.
	const FVector Start = BotCharacter->GetPawnViewLocation();

	// Two body points rather than one. A single ray at a fixed height is a coin flip against this
	// arena's thin neon trim and low cover: a run measured 40.7% of bot-ticks with the player inside
	// engagement range and 0.0% of them with line of sight, at a closest approach of 439uu. Chest and
	// feet-ish between them survive a strip of geometry cutting one ray.
	const FVector TargetOrigin = Target->GetActorLocation();

	const FVector HighPoint = TargetOrigin + FVector(0.f, 0.f, UTraceSettings::Get().BotAimBodyOffsetZ);
	const FVector LowPoint = TargetOrigin + FVector(0.f, 0.f, TraceBotConstants::TargetLowOffsetZ);

	// Geometry AND slime. A Slimewall is NoCollision by design (spec v18 §2's "can be shot through"),
	// so it is invisible to the ECC_Visibility rays below and has to be asked about separately —
	// otherwise the wall obstructs a human's vision and not a bot's, which is exactly what
	// verification caught.
	if (!World->LineTraceTestByChannel(Start, HighPoint, ECC_Visibility, Params)
		&& !TraceBotSlimewallBlocksSegment(World, Start, HighPoint))
	{
		return true;
	}

	return !World->LineTraceTestByChannel(Start, LowPoint, ECC_Visibility, Params)
		&& !TraceBotSlimewallBlocksSegment(World, Start, LowPoint);
}

bool ATraceBotController::FindTrailInterceptPoint(FVector& OutPoint, FVector& OutTangent) const
{
	if (!BotInterceptFixEnabled())
	{
		return FindTrailInterceptPointLegacy(OutPoint, OutTangent);
	}

	const ATraceCharacter* BotCharacter = GetBotCharacter();
	const ATraceCharacter* EnemyCarrier = Carrier.Get();
	if (BotCharacter == nullptr || EnemyCarrier == nullptr)
	{
		return false;
	}

	const UTraceTrailComponent* TrailComponent = EnemyCarrier->Trail;
	if (TrailComponent == nullptr)
	{
		return false;
	}

	const TArray<FTraceTrailPoint>& Points = TrailComponent->TrailPoints.Items;
	const UTraceSettings& Settings = UTraceSettings::Get();

	// ---------------------------------------------------------------------------------------------
	// 1. WHAT IS ACTUALLY LETHAL.
	//
	// Asked of the trail component rather than re-derived from TrailHeadGracePoints. The old code
	// took `Points.Num() - TrailHeadGracePoints` wholesale, which has not been the server's rule
	// since the head exemption became a DISTANCE (one trace radius) capped by that count — so the
	// bots and the trip test disagreed about which end of the trace kills. ComputeLastLethalIndex()
	// is the single function the trip test, the renderer and the parry verifier all read; a bot that
	// asks anything else is planning against a trace that does not exist.
	// ---------------------------------------------------------------------------------------------
	const int32 LastLethal = FMath::Min(TrailComponent->ComputeLastLethalIndex(), Points.Num() - 1);

	// A tangent needs two points, so one lethal point is not a segment.
	if (LastLethal < 1)
	{
		TRACE_BOT_KIT(PlanRejectShort);
		return false;
	}

	// ---------------------------------------------------------------------------------------------
	// 2. HOW FAR BEHIND THE CARRIER EACH POINT IS, ALONG THE PATH.
	//
	// Along the path, not as the crow flies, and that is a real behavioural difference and not a
	// nicety. A carrier who doubles back leaves trace that is a long way behind them in path terms
	// and a few hundred uu away in space; it is fully lethal, it is the single most inviting thing
	// on the field, and a straight-line test rejects it as "too close to the carrier". Path distance
	// is also the only measure the length cap is expressed in, so it is the measure the trim
	// prediction below has to use as well.
	// ---------------------------------------------------------------------------------------------
	TArray<float, TInlineAllocator<64>> BehindHead;
	BehindHead.SetNumUninitialized(LastLethal + 1);

	{
		const int32 HeadIndex = Points.Num() - 1;
		double Walked = 0.0;
		for (int32 Index = HeadIndex; Index > LastLethal; --Index)
		{
			Walked += FVector::Dist(Points[Index - 1].Location, Points[Index].Location);
		}

		BehindHead[LastLethal] = static_cast<float>(Walked);
		for (int32 Index = LastLethal; Index > 0; --Index)
		{
			Walked += FVector::Dist(Points[Index - 1].Location, Points[Index].Location);
			BehindHead[Index - 1] = static_cast<float>(Walked);
		}
	}

	const float HeadOffset = BehindHead[LastLethal];          // grace stub under the carrier's feet
	const float TailDistance = BehindHead[0];                 // the whole lethal path, head to tail
	const float UsableLength = FMath::Max(0.f, TailDistance - HeadOffset);

	// ---------------------------------------------------------------------------------------------
	// 3. THE TWO MARGINS — AND THE ARITHMETIC THAT USED TO CANCEL THE WHOLE TRACE.
	//
	// 900 (head) + 320 (tail) = 1220 against a 1200uu trace. Read as absolutes they overlapped and
	// left nothing. Read as a share of the trace in front of us they cannot: 0.45 + 0.20 < 1.
	// The settings values still bind whenever the trace is long enough for them to mean what they
	// say, so a designer raising TrailMaxLengthUU gets the intended behaviour back automatically.
	// ---------------------------------------------------------------------------------------------
	const float RawHeadGap = FMath::Max(0.f, Settings.BotTrailMinDistanceFromCarrier);
	const float RawTailSkip = FMath::Max(0.f, Settings.BotTrailMinPointLifeRemaining)
		* FMath::Max(0.f, Settings.WalkSpeed);

	const float HeadGap = FMath::Min(RawHeadGap,
		HeadOffset + UsableLength * TraceBotConstants::TrailHeadGapMaxFraction);
	const float TailSkip = FMath::Min(RawTailSkip,
		UsableLength * TraceBotConstants::TrailTailSkipMaxFraction);
	const float TailLimit = TailDistance - TailSkip;

	// ---------------------------------------------------------------------------------------------
	// 4. HOW LONG EACH POINT HAS LEFT.
	//
	// v7 §1 removed age expiry, so a point dies only when NEW trace at the head pushes the path past
	// TrailMaxLengthUU: a point D behind the carrier is deleted once the carrier has run a further
	// (MaxLength - D). Divide by the carrier's speed and that is seconds. A carrier standing still
	// deletes nothing, which is why this has to be derived from their speed and not from a clock.
	// ---------------------------------------------------------------------------------------------
	const float MaxLength = UTraceTrailComponent::GetTraceMaxLengthUU();
	const float CarrierSpeed = static_cast<float>(EnemyCarrier->GetVelocity().Size2D());
	const float MySpeed = FMath::Max(200.f, Settings.WalkSpeed);

	const FVector MyLocation = BotCharacter->GetActorLocation();

	// ---------------------------------------------------------------------------------------------
	// 5. CHOOSING. THREE TIERS, AND THE LAST ONE IS WHY THIS FUNCTION CAN NO LONGER RETURN FALSE
	//    WITH A LETHAL TRACE IN FRONT OF IT.
	//
	//   1. in the band AND still there when we arrive   — what we want
	//   2. in the band but the length cap will eat it   — worth running at anyway; by the time the
	//                                                     bot arrives the carrier has laid more trace
	//                                                     in roughly the same place
	//   3. outside the band                             — a sparsely sampled trace (a dash lays
	//                                                     points ~540uu apart) can genuinely have no
	//                                                     vertex inside it; take the nearest one to
	//                                                     the band rather than giving up
	//
	// Tier 3 is the guard rail. Falling through to "no plan" is what turns an interceptor into a
	// pursuer, and that is the bug this whole section exists to make unreachable.
	// ---------------------------------------------------------------------------------------------
	int32 BestIndex = INDEX_NONE;
	float BestScore = TNumericLimits<float>::Max();

	int32 DoomedIndex = INDEX_NONE;
	float DoomedScore = TNumericLimits<float>::Max();

	int32 OutOfBandIndex = INDEX_NONE;
	float OutOfBandError = TNumericLimits<float>::Max();

	for (int32 Index = 0; Index < LastLethal; ++Index)
	{
		const FVector PointLocation = Points[Index].Location;

		FVector Tangent = FVector(Points[Index + 1].Location) - PointLocation;
		Tangent.Z = 0.f;
		Tangent = Tangent.GetSafeNormal();
		if (Tangent.IsNearlyZero())
		{
			continue;   // Two coincident points: no direction to cross.
		}

		const float Behind = BehindHead[Index];

		if (Behind < HeadGap || Behind > TailLimit)
		{
			const float BandError = FMath::Max(HeadGap - Behind, Behind - TailLimit);
			if (BandError < OutOfBandError)
			{
				OutOfBandError = BandError;
				OutOfBandIndex = Index;
			}
			continue;
		}

		FVector ToPoint = PointLocation - MyLocation;
		ToPoint.Z = 0.f;
		const float Distance = static_cast<float>(ToPoint.Size());
		const float ArriveSeconds = Distance / MySpeed;

		// Crossing angle. 1 when the bot would arrive square to the trace, 0 when it would arrive
		// along it. A dash along the line sweeps a segment parallel to the one it has to intersect
		// and trips nothing, however close it gets — so a nearer point that can only be taken
		// end-on is a worse target than a further one that can be taken side-on.
		const FVector ApproachDirection = (Distance > KINDA_SMALL_NUMBER)
			? (ToPoint / Distance)
			: FVector::CrossProduct(FVector::UpVector, Tangent).GetSafeNormal();
		const float Perpendicularity =
			1.f - FMath::Abs(static_cast<float>(FVector::DotProduct(ApproachDirection, Tangent)));

		const float Score = ArriveSeconds
			* (1.f + TraceBotConstants::TrailApproachParallelPenalty * (1.f - Perpendicularity));

		// Will it still be there when we get there? Only meaningful while the carrier is moving:
		// a carrier standing still deletes nothing.
		bool bSurvives = true;
		if (CarrierSpeed > KINDA_SMALL_NUMBER)
		{
			const float TrimSeconds = FMath::Max(0.f, MaxLength - Behind) / CarrierSpeed;
			bSurvives = TrimSeconds >= ArriveSeconds * TraceBotConstants::TrailTrimSafetyFactor;
		}

		if (bSurvives)
		{
			if (Score < BestScore)
			{
				BestScore = Score;
				BestIndex = Index;
			}
		}
		else if (Score < DoomedScore)
		{
			DoomedScore = Score;
			DoomedIndex = Index;
		}
	}

	if (BestIndex == INDEX_NONE)
	{
		BestIndex = (DoomedIndex != INDEX_NONE) ? DoomedIndex : OutOfBandIndex;
	}

	if (BestIndex == INDEX_NONE)
	{
		// Every lethal segment was degenerate — the carrier has been standing still long enough that
		// the whole path is one blob. There is nothing to cross.
		TRACE_BOT_KIT(PlanRejectTangent);
		return false;
	}

	const FVector Chosen = Points[BestIndex].Location;
	FVector ChosenTangent = FVector(Points[BestIndex + 1].Location) - Chosen;
	ChosenTangent.Z = 0.f;
	ChosenTangent = ChosenTangent.GetSafeNormal();

	if (ChosenTangent.IsNearlyZero())
	{
		TRACE_BOT_KIT(PlanRejectTangent);
		return false;
	}

	OutPoint = Chosen;
	OutTangent = ChosenTangent;
	return true;
}

bool ATraceBotController::FindCutBehindStation(FVector& OutStation) const
{
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	const ATraceCharacter* EnemyCarrier = Carrier.Get();
	if (BotCharacter == nullptr || EnemyCarrier == nullptr)
	{
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const FVector MyLocation = BotCharacter->GetActorLocation();
	const FVector CarrierLocation = EnemyCarrier->GetActorLocation();

	FVector CarrierVelocity = EnemyCarrier->GetVelocity();
	CarrierVelocity.Z = 0.f;
	const float CarrierSpeed = static_cast<float>(CarrierVelocity.Size());

	// Which way the trace extends FROM the carrier: straight back down their line of travel.
	//
	// When they are barely moving there is no line of travel to use, so fall back to the geometry of
	// the pitch: a carrier runs at the end they score in, so the trace they are about to lay points
	// at the end THIS bot scores in. AttackSideSign is that end, already resolved and already
	// half-time-correct.
	FVector TraceDirection;
	if (CarrierSpeed > TraceBotConstants::TrailCutBehindMinCarrierSpeed)
	{
		TraceDirection = CarrierVelocity / CarrierSpeed;
		TraceDirection = -TraceDirection;
	}
	else if (AttackSideSign != 0.f)
	{
		TraceDirection = FVector(AttackSideSign, 0.f, 0.f);
	}
	else
	{
		TraceDirection = (MyLocation - CarrierLocation).GetSafeNormal2D();
	}

	if (TraceDirection.IsNearlyZero())
	{
		return false;
	}

	// How far back down that line to stand. The same head gap the intercept filter uses, budgeted
	// the same way against the trace's own maximum length so the station is a place trace can
	// actually be. Spread along the line by this bot's formation bias so three interceptors do not
	// converge on one spot and shove each other off it.
	const float MaxLength = UTraceTrailComponent::GetTraceMaxLengthUU();
	const float Gap = FMath::Min(
		FMath::Max(0.f, Settings.BotTrailMinDistanceFromCarrier),
		MaxLength * TraceBotConstants::TrailHeadGapMaxFraction)
		* (1.f + 0.35f * FormationBias);

	const FVector Anchor = CarrierLocation + TraceDirection * FMath::Max(120.f, Gap);

	// Stand OFF the line, on the side the bot is already on, at roughly a dash's reach — so the
	// approach is across the future trace rather than along it, and the crossing is one dash away
	// the moment the trace exists.
	FVector Perpendicular = FVector::CrossProduct(FVector::UpVector, TraceDirection).GetSafeNormal();
	if (Perpendicular.IsNearlyZero())
	{
		Perpendicular = FVector(0.f, 1.f, 0.f);
	}

	FVector AnchorToMe = MyLocation - Anchor;
	AnchorToMe.Z = 0.f;
	const float Side = (FVector::DotProduct(AnchorToMe, Perpendicular) >= 0.f) ? 1.f : -1.f;

	const float Standoff = FMath::Max(50.f, Settings.BotTrailDashRange)
		* TraceBotConstants::TrailPocketDashRangeFraction;

	OutStation = Anchor + Perpendicular * (Side * Standoff);
	OutStation.Z = MyLocation.Z;
	return true;
}

bool ATraceBotController::FindTrailInterceptPointLegacy(FVector& OutPoint, FVector& OutTangent) const
{
	const ATraceCharacter* BotCharacter = GetBotCharacter();
	const ATraceCharacter* EnemyCarrier = Carrier.Get();
	if (BotCharacter == nullptr || EnemyCarrier == nullptr)
	{
		return false;
	}

	const UTraceTrailComponent* TrailComponent = EnemyCarrier->Trail;
	if (TrailComponent == nullptr)
	{
		return false;
	}

	const TArray<FTraceTrailPoint>& Points = TrailComponent->TrailPoints.Items;

	const UTraceSettings& Settings = UTraceSettings::Get();

	// The newest TrailHeadGracePoints entries are exempt from the server's trip test (they exist so
	// a carrier cannot be killed by the trace leaving their own feet). Dashing at them would be a
	// wasted charge, so they are excluded here too — this number must track the settings value.
	const int32 Grace = FMath::Max(0, Settings.TrailHeadGracePoints);
	const int32 Usable = Points.Num() - Grace;

	// A tangent needs two points, so one usable point is not a segment.
	if (Usable < 2)
	{
		TRACE_BOT_KIT(PlanRejectShort);
		return false;
	}

	// SPEC v7 §1 REWROTE THIS FILTER, AND NOT COSMETICALLY.
	//
	// This used to reject points with less than BotTrailMinPointLifeRemaining seconds of LIFE LEFT,
	// computed as BirthServerTime < now - TrailLifetime + margin. Points no longer expire by age at
	// all — the tail is trimmed only when new trace at the head pushes the path past
	// UTraceTrailComponent::GetTraceMaxLengthUU(). Against a stationary carrier that age test rejects
	// EVERY point (they are all older than any cutoff measured backwards from now), so the bots would
	// have discarded the whole trace and never planned an intercept — a silent, total regression of
	// the game's signature defensive play, on exactly the carrier the v7 change makes killable.
	//
	// Same intent, expressed in the units the trace now lives in: skip the OLDEST stretch of the path,
	// measured as a DISTANCE walked back from the tail. BotTrailMinPointLifeRemaining is still the
	// "fraction of the trace written as an absolute" it has always been — 0.40s x WalkSpeed 800 =
	// 320uu of the 1200uu trace, the same calibrated oldest-quarter it used to discard. It stays
	// paired with the trace's size; if TrailMaxLengthUU moves, move this with it.
	const float TailSkipUU = FMath::Max(0.f, Settings.BotTrailMinPointLifeRemaining)
		* FMath::Max(0.f, Settings.WalkSpeed);

	// Walk the path from the oldest end and find the first index at least TailSkipUU along it. The
	// path is measured, not assumed uniform: a carrier who dashed laid points far further apart than
	// TrailPointSpacing, and index arithmetic would skip a wildly variable amount of real trace.
	int32 FirstAcceptableIndex = 0;
	if (TailSkipUU > 0.f)
	{
		double Walked = 0.0;
		for (int32 Index = 0; Index + 1 < Usable; ++Index)
		{
			Walked += FVector::Dist(Points[Index].Location, Points[Index + 1].Location);
			if (Walked >= static_cast<double>(TailSkipUU))
			{
				FirstAcceptableIndex = Index + 1;
				break;
			}
		}

		// A trace shorter than the margin has no "oldest quarter" worth naming. Rejecting all of it
		// would be the age filter's bug in new clothes, so clamp: a short trace stays huntable, and
		// the head grace above is what protects the carrier's heels.
		FirstAcceptableIndex = FMath::Clamp(FirstAcceptableIndex, 0, FMath::Max(0, Usable - 2));
	}

	const FVector MyLocation = BotCharacter->GetActorLocation();
	const FVector CarrierLocation = EnemyCarrier->GetActorLocation();

	// Established trace only. The server's head-grace window is a few entries, which at the default
	// spacing is barely 180uu — near enough that a defender standing where the carrier just took the
	// Core can stab the trace at their heels before the run has begun. That is not the play this
	// mechanic exists for, and it is why every carry used to die within three seconds.
	const float MinCarrierGapSq = FMath::Square(FMath::Max(0.f, Settings.BotTrailMinDistanceFromCarrier));

	int32 BestIndex = INDEX_NONE;
	float BestDistanceSq = TNumericLimits<float>::Max();

	// Index 0 is the oldest point. Start past the tail margin computed above, and stop one short of
	// the last usable entry so a tangent can always be built forward from BestIndex.
	for (int32 Index = FirstAcceptableIndex; Index < Usable - 1; ++Index)
	{
		const FVector PointLocation = Points[Index].Location;

		if (FVector::DistSquared2D(CarrierLocation, PointLocation) < MinCarrierGapSq)
		{
			continue;
		}

		const float DistanceSq = static_cast<float>(FVector::DistSquared2D(MyLocation, PointLocation));
		if (DistanceSq < BestDistanceSq)
		{
			BestDistanceSq = DistanceSq;
			BestIndex = Index;
		}
	}

	if (BestIndex == INDEX_NONE)
	{
		TRACE_BOT_KIT(PlanRejectBand);
		return false;
	}

	const FVector Chosen = Points[BestIndex].Location;
	FVector Tangent = FVector(Points[BestIndex + 1].Location) - Chosen;
	Tangent.Z = 0.f;
	Tangent = Tangent.GetSafeNormal();

	if (Tangent.IsNearlyZero())
	{
		// Two coincident points (the carrier stood still). No direction to cross, so treat it as no
		// usable trace rather than crossing an arbitrary way and burning a charge.
		TRACE_BOT_KIT(PlanRejectTangent);
		return false;
	}

	OutPoint = Chosen;
	OutTangent = Tangent;
	return true;
}

// =================================================================================================
// Steering
// =================================================================================================

void ATraceBotController::ApplySteering(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	ATraceCharacter* BotCharacter = GetBotCharacter();
	if (World == nullptr || BotCharacter == nullptr)
	{
		return;
	}

	const float Now = static_cast<float>(World->GetTimeSeconds());
	FVector Direction = DesiredMoveDirection;
	Direction.Z = 0.f;

	const bool bWantedToMove = !Direction.IsNearlyZero();

	// --- Stuck detection ------------------------------------------------------------------------
	// There is no navmesh, so arena obstacles are handled the cheap way: notice that we are pushing
	// into something and are not moving, then kick sideways — and if that does not work either,
	// UpdateMovementTech() escalates to a jump over the top.
	if (bWantedToMove && BotCharacter->GetVelocity().Size2D() < TraceBotConstants::StuckSpeedThreshold)
	{
		StuckSeconds += DeltaSeconds;
		if (StuckSeconds > TraceBotConstants::StuckTriggerSeconds && Now >= EvadeUntilTime)
		{
			EvadeUntilTime = Now + TraceBotConstants::EvadeDuration;
			const float Sign = (FMath::FRand() < 0.5f) ? -1.f : 1.f;
			EvadeDirection = FVector::CrossProduct(FVector::UpVector, Direction.GetSafeNormal()) * Sign;
		}
	}
	else
	{
		StuckSeconds = 0.f;
	}

	if (Now < EvadeUntilTime && !EvadeDirection.IsNearlyZero())
	{
		Direction = (Direction.GetSafeNormal() * 0.4f + EvadeDirection).GetSafeNormal();
	}

	// --- Wall repulsion -------------------------------------------------------------------------
	if (bBoundsValid && bWantedToMove)
	{
		const float Margin = FMath::Max(1.f, UTraceSettings::Get().BotWallAvoidMargin);
		const FVector MyLocation = BotCharacter->GetActorLocation();
		FVector Push = FVector::ZeroVector;

		auto AccumulatePush = [&Push, Margin](float DistanceToWall, const FVector& InwardNormal)
		{
			if (DistanceToWall < Margin)
			{
				const float Strength = 1.f - FMath::Max(0.f, DistanceToWall) / Margin;
				Push += InwardNormal * Strength;
			}
		};

		AccumulatePush(static_cast<float>(MyLocation.X - FieldBounds.Min.X), FVector(1.f, 0.f, 0.f));
		AccumulatePush(static_cast<float>(FieldBounds.Max.X - MyLocation.X), FVector(-1.f, 0.f, 0.f));
		AccumulatePush(static_cast<float>(MyLocation.Y - FieldBounds.Min.Y), FVector(0.f, 1.f, 0.f));
		AccumulatePush(static_cast<float>(FieldBounds.Max.Y - MyLocation.Y), FVector(0.f, -1.f, 0.f));

		if (!Push.IsNearlyZero())
		{
			Direction = (Direction.GetSafeNormal() + Push * TraceBotConstants::WallAvoidWeight).GetSafeNormal();
		}
	}

	Direction = Direction.GetSafeNormal();
	if (!Direction.IsNearlyZero())
	{
		BotCharacter->AddMovementInput(Direction, 1.f);
	}

	// Every impulse below MUST be raised after AddMovementInput and before the movement component
	// ticks: UTraceCharacterMovementComponent::BeginDash locks the direction from Acceleration,
	// which is derived from the input vector we have only just contributed, and the jump wants the
	// same frame's heading. The tick prerequisite installed in OnPossess guarantees that ordering.
	// SPEC v15 §3. E goes here and not earlier, and the reason is the same one the dash has: Rocco's
	// Ripple composes its direction with the SAME pure function the dash uses
	// (UTraceCharacterMovementComponent::ComputeDashDirection over the pawn's acceleration), so a
	// press issued before AddMovementInput lays the path along last frame's steering.
	ApplyAbilityInputs();

	if (bWantsDashThisTick)
	{
		BotCharacter->DoDash();
	}
	bWantsDashThisTick = false;

	if (bWantsJumpThisTick)
	{
		// THE ABILITY GETS FIRST REFUSAL ON THE JUMP KEY, exactly as it does for a human — see
		// ATracePlayerController::OnJumpStarted. Rocco's midair redirect and Oyster's jar jump both
		// live behind this hook, and a bot that called Jump() directly would be the one player on the
		// field for whom those two abilities did not exist. Everybody else returns false and reaches
		// ACharacter::Jump exactly as before.
		//
		// No ServerHandleJumpPressed: a bot controller only ever exists on the server, so the call
		// above already WAS the authoritative one.
		bool bAbilityTookTheJump = false;
		if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(BotCharacter))
		{
			if (Abilities->GetCharacterId() != ETraceCharacterId::None && TraceBotAbilityStats::AbilitiesEnabled())
			{
				bAbilityTookTheJump = Abilities->HandleJumpPressed();
				TraceBotAbilityStats::NoteAbilityJump(Abilities->GetCharacterId(), bAbilityTookTheJump);
			}
		}

		if (!bAbilityTookTheJump)
		{
			BotCharacter->Jump();
		}
	}
	bWantsJumpThisTick = false;
}

// =================================================================================================
// Small helpers
// =================================================================================================

ATraceCharacter* ATraceBotController::GetBotCharacter() const
{
	return Cast<ATraceCharacter>(GetPawn());
}

ATraceGameMode* ATraceBotController::GetTraceGameMode() const
{
	const UWorld* World = GetWorld();
	return (World != nullptr) ? Cast<ATraceGameMode>(World->GetAuthGameMode()) : nullptr;
}

float ATraceBotController::HalfFieldLength() const
{
	if (!bBoundsValid)
	{
		return TraceBotConstants::FallbackHalfLength;
	}

	return FMath::Max(1.f, static_cast<float>(FieldBounds.Max.X - FieldBounds.Min.X) * 0.5f);
}

float ATraceBotController::HalfFieldWidth() const
{
	if (!bBoundsValid)
	{
		return TraceBotConstants::FallbackHalfWidth;
	}

	return FMath::Max(1.f, static_cast<float>(FieldBounds.Max.Y - FieldBounds.Min.Y) * 0.5f);
}

FVector ATraceBotController::GetAttackGoalLocation() const
{
	const UTraceSettings& Settings = UTraceSettings::Get();

	// The goal is a LINE, not a point: an endzone spans the whole width, and which part of it a
	// carrier crosses is free choice. Aiming every bot at the exact centre of it throws that away and
	// funnels the entire match down the middle of the field. See
	// UTraceSettings::BotAttackLaneFieldFraction for the measurement that says so.
	const float LaneOffset = HalfFieldWidth()
		* FMath::Max(0.f, Settings.BotAttackLaneFieldFraction)
		* FMath::Clamp(FormationBias, -1.f, 1.f);

	const float Margin = FMath::Max(1.f, Settings.BotWallAvoidMargin);

	// --- The authoritative answer -----------------------------------------------------------------
	//
	// Resolved from the ATraceEndzone this team SCORES IN, which is the same question the endzone
	// trigger itself answers when it decides whether to award a point. That equivalence is the whole
	// reason to do it this way: the bots and the scoring rule cannot drift apart across the half-time
	// side switch, because they are reading the same fact.
	if (bAttackGoalValid)
	{
		float GoalY = static_cast<float>(AttackGoalCentre.Y) + LaneOffset;
		if (bBoundsValid)
		{
			GoalY = FMath::Clamp(GoalY,
				static_cast<float>(FieldBounds.Min.Y) + Margin,
				static_cast<float>(FieldBounds.Max.Y) - Margin);
		}

		// --- MODE B: THE LANE MUST FIT INSIDE THE MOUTH. -----------------------------------------
		//
		// THIS IS WHY NOBODY EVER CARRIED THE CORE IN. The attack lane above is correct for mode A
		// and actively harmful in mode B, and the numbers are not close: BotAttackLaneFieldFraction
		// is 0.30 of the field HALF-WIDTH, i.e. ±1440 uu on a 9600 uu pitch, against a goal mouth
		// that was ±1600 uu and is now ±1000 uu (spec v5 §4). So a bot with a high |FormationBias|
		// spent the whole match running at a point OUTSIDE the goal it was trying to score in,
		// arrived beside the post, and the swept carry-in test in ATraceCore correctly never fired.
		// Seven minutes of "the code path exists but never runs" was one clamp.
		//
		// The lane is kept, not deleted: spreading five attackers is still right, and inside the
		// mouth there is still 1400 uu of usable spread. It is simply clamped to the box that scores,
		// with an inset so a carrier aiming at the very edge is still inside after the capsule
		// radius and a frame of overshoot.
		if (bModeB)
		{
			FBox GoalBox(ForceInit);
			if (ATraceCore::GetAttackGoalBox(GetWorld(), MyTeam, GoalBox))
			{
				const double MouthHalfY = (GoalBox.Max.Y - GoalBox.Min.Y) * 0.5;
				const double Inset = FMath::Min(300.0, MouthHalfY * 0.35);

				GoalY = FMath::Clamp(GoalY,
					static_cast<float>(GoalBox.Min.Y + Inset),
					static_cast<float>(GoalBox.Max.Y - Inset));
			}
		}

		return FVector(AttackGoalCentre.X, GoalY, AttackGoalCentre.Z);
	}

	// --- Fallback ---------------------------------------------------------------------------------
	//
	// Blue defends -X and therefore attacks +X (ATraceArenaBuilder::TeamEndSign). This is ONLY
	// reached when no endzone answered — ResolveAttackEndzone() has already logged a warning saying
	// so — and it is knowingly wrong in the second half. It exists so that a bot with no endzone
	// still plays football rather than standing still, not because it is correct.
	if (!bBoundsValid)
	{
		return FVector::ZeroVector;
	}

	const float Inset = FMath::Max(0.f, Settings.BotGoalInsetFromWall);
	const FVector Centre = FieldBounds.GetCenter();

	const float AttackX = (MyTeam == ETraceTeam::Blue)
		? static_cast<float>(FieldBounds.Max.X) - Inset
		: static_cast<float>(FieldBounds.Min.X) + Inset;

	const float GoalY = FMath::Clamp(
		static_cast<float>(Centre.Y) + LaneOffset,
		static_cast<float>(FieldBounds.Min.Y) + Margin,
		static_cast<float>(FieldBounds.Max.Y) - Margin);

	return FVector(AttackX, GoalY, Centre.Z);
}
