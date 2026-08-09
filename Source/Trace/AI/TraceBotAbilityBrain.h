// Trace — THE BOT ABILITY BRAIN (spec v15 §3).
//
// ===================================================================================================
// WHY THIS FILE EXISTS
// ===================================================================================================
//
// Spec v15 §3, verbatim: "Make it so that the AI for the bots when playing singleplayer is much more
// capable and natural at playing the game (especially for using abilities)."
//
// The diagnosis it starts from is not a tuning complaint. TraceBotController.cpp was 5,761 lines and
// contained ZERO references to Ability, TryActivate or UTraceAbilityComponent: bots did not use
// abilities badly, they had no code that could. Nine of the ten players in a singleplayer match are
// bots, so "the characters" — the entire spec v14 §6 pass — was a feature the game had never actually
// played. This file is the missing half.
//
// ===================================================================================================
// WHY IT IS A SEPARATE FILE AND A PLAIN CLASS
// ===================================================================================================
//
// SEPARATE, because the controller is already 5,761 lines and the ability decision is a genuinely
// different question from steering: it wants the world state the controller has already gathered, it
// produces key presses, and it has none of the controller's per-frame steering machinery in it.
//
// PLAIN (not a UObject, not a component), because it owns no replicated state, needs no reflection
// and must be destroyed exactly when the controller is. It is held BY VALUE on ATraceBotController.
//
// ===================================================================================================
// THE SHAPE: SITUATION IN, ORDERS OUT
// ===================================================================================================
//
// Plan() takes an FTraceBotAbilitySituation — a snapshot the controller has already computed for its
// own steering — and fills an FTraceBotAbilityOrders. It NEVER touches the pawn, never writes the
// control rotation and never presses anything itself. The controller applies the orders, in the
// controller's own tick order, through exactly the entry points a human's key press reaches:
//
//     E              UTraceAbilityInputRelay::RouteActivatePressed   (which also carries Mace's
//                                                                     reactivation — see its header)
//     V down / up    UTraceAbilityComponent::HandleSecondaryPressed / HandleSecondaryReleased
//     Jump           UTraceAbilityComponent::HandleJumpPressed, then ACharacter::Jump if it declined
//     Dash           ATraceCharacter::DoDash, through the controller's existing charge model
//
// That indirection is the whole safety argument for spec §4. This file decides WHEN to press a key.
// It never decides what an ability does to anybody, so it cannot be the place the carrier rule is
// broken — every effect still runs inside the character's own ability set, behind
// UTraceAbilityComponent::CanAffectTarget. Where this file has to CHOOSE a victim (Oyster's Pickler,
// Chut's bash), it asks that same choke point rather than testing IsCarrier itself, so a bot cannot
// even aim an ability at somebody the rule would refuse.
//
// ===================================================================================================
// "NATURAL" IS A REQUIREMENT, NOT A POLISH PASS
// ===================================================================================================
//
// Spec §3: "No instant reactions, no perfect tracking, no frame-one ability use. Give them reaction
// latency and imperfect aim." The user has previously complained the bots were TOO GOOD, so every
// path in here is gated by three things and all three scale with the existing difficulty profile:
//
//   1. AN INTENT LATCH. Wanting to press a key does not press it. The want is latched, a delay is
//      rolled from FTraceBotProfile::ReactionTimeSeconds (x a per-bot personality scale x jitter),
//      and the key is only pressed if the reason is STILL true when the delay expires. A bot that
//      wanted to Sting somebody who then walked round a corner does not Sting the corner.
//   2. A WILLINGNESS ROLL, taken once per opportunity off FTraceBotProfile::Aggression. Easy (0.45)
//      declines a lot of marginal opportunities; Hard (0.90) takes nearly all of them.
//   3. ABILITY AIM ERROR, a cone applied to every aimed ability (Mace's spike, Oyster's lob) on top
//      of the finite slew rate the controller already imposes. Bots miss with abilities too.
//
// None of that is optional and none of it is free: Trace.Bot.AbilityLatencyScale 0 and
// Trace.Bot.AbilityUseScale 4 exist so a measurement run can prove the gates are load-bearing.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Math/Vector.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId
#include "TraceTypes.h"                    // ETraceTeam

class APlayerState;
class ATraceBotController;
class ATraceCharacter;
class UTraceAbilityComponent;

class UWorld;

/**
 * MEASUREMENT, and the red arm.
 *
 * "Evidence: show bots actually activating abilities in a real match (log or measured counts per
 * character)" is the acceptance test for this pass, and it cannot be answered by reading code. These
 * counters are incremented at the point the key is actually pressed — in the controller, not in the
 * planner — so what they count is presses that reached the framework, not presses the brain wanted.
 *
 * The distinction matters: "attempted 40, fired 3" is a cooldown or a CanActivate refusal and is
 * fine; "attempted 0" is this whole file being dead, which is the state the project was in before
 * this pass and is exactly what AbilitiesEnabled() reproduces on demand.
 */
namespace TraceBotAbilityStats
{
	/**
	 * THE RED ARM. False under `Trace.Bot.Abilities 0`, which removes every ability decision in this
	 * pass at once and restores the measured pre-v15 behaviour: bots that never press E, V or an
	 * ability jump. Modelled on Trace.Ability.Integration, for the same reason — a harness that
	 * cannot go red is not a harness.
	 */
	bool AbilitiesEnabled();

	/** True under `Trace.Bot.AbilityMetrics 1`: emit the [BotAbility] line every 10 s. */
	bool MetricsEnabled();

	/** A press of E reached the framework. @p bFired is what the framework said about it. */
	void NoteActivate(ETraceCharacterId Id, bool bFired, bool bWasSpikePull, const TCHAR* Reason,
	                  const APlayerState* Who);

	/** The V level changed. */
	void NoteSecondary(ETraceCharacterId Id, bool bDown);

	/** A jump was offered to the ability hook. @p bConsumed is what the hook said. */
	void NoteAbilityJump(ETraceCharacterId Id, bool bConsumed);

	/** A dash was spent for an ability reason. */
	void NoteAbilityDash(ETraceCharacterId Id);

	/** The reaction latency an ability press was actually delivered with. Aggregated for the dump. */
	void NoteLatency(float Seconds);

	/** Emits the 10 s summary. Safe to call once per bot per tick. */
	void Poll(const UWorld* World);
}

/**
 * ATraceBotController::State, forward-declared rather than included.
 *
 * TraceBotController.h holds an FTraceBotAbilityBrain by value, so it includes THIS header; this
 * header must therefore not include it back. An opaque-enum-declaration with a fixed underlying type
 * is a complete type, so the situation struct below can hold one by value.
 */
enum class ETraceBotState : uint8;

/** Which key the brain wants pressed. One at a time — see FTraceBotAbilityBrain::Pending. */
enum class ETraceBotAbilityAct : uint8
{
	None = 0,
	/** E, the activated ability. Routed through the relay, so Mace's reactivation is included. */
	Activate,
	/** Jump, offered to the ability hook first (Rocco's midair redirect, Oyster's jar jump). */
	Jump,
	/** A dash spent for an ability reason (Chut's bash, Oyster's jar-on-the-approach). */
	Dash
};

/** What Want() did with a request. Callers act on Declined, which is the difficulty knob biting. */
enum class ETraceBotWantResult : uint8
{
	/** The willingness roll said no. The caller should hesitate before offering again — see below. */
	Declined = 0,
	/** A DIFFERENT act is already latched. One decision at a time; try again when it resolves. */
	Busy,
	/** A new intent was latched and its reaction clock has started. */
	Latched,
	/** The existing intent for this act was kept alive and its aim refreshed. */
	Kept
};

/**
 * Everything the brain is allowed to know, gathered once per tick by the controller.
 *
 * Deliberately all raw pointers and plain values: it lives for exactly one Plan() call, inside the
 * controller's tick, where every one of these was already validated.
 */
struct FTraceBotAbilitySituation
{
	/** The bot's pawn. Never null — Plan() is not called without one. */
	ATraceCharacter* Self = nullptr;

	/**
	 * The controller, for the queries it already implements (line of sight). Never null.
	 *
	 * NOT named "Owner". AActor has a member of that name, clang does not warn about the shadow and
	 * MSVC's C4458/C4459 does — this project has lost the Windows build to exactly that four times.
	 */
	const ATraceBotController* BotController = nullptr;

	/** The bot's ability component, or null when the player has none yet. */
	UTraceAbilityComponent* Abilities = nullptr;

	/** Which behaviour the controller settled on this tick. Drives the "the dash is spoken for" test. */
	ETraceBotState BotState = static_cast<ETraceBotState>(0);

	/** Living enemies this tick. Owned by the controller; valid for this call only. */
	const TArray<ATraceCharacter*>* Enemies = nullptr;

	/** Whoever holds the Core, either side, or null. */
	ATraceCharacter* Carrier = nullptr;
	bool bIAmCarrier = false;
	bool bTeammateIsCarrier = false;
	bool bEnemyIsCarrier = false;

	/** This tick's duel target, or null. Set by the controller's behaviours before Plan() runs. */
	ATraceCharacter* ShootTarget = nullptr;

	/** True while the bot's shield is down for a hover pass. Nothing clever happens in that window. */
	bool bPassing = false;

	/** Where the bot is steering, and where its objective is. Both world space; move dir is planar. */
	FVector DesiredMoveDirection = FVector::ZeroVector;
	FVector AttackGoal = FVector::ZeroVector;

	/** Dash charges the controller believes it has right now. */
	int32 DashCharges = 0;

	/** World seconds. The brain's timers are all world time, like the rest of the controller's. */
	float Now = 0.f;
};

/** What the controller should do about it. Applied in the controller's own tick order. */
struct FTraceBotAbilityOrders
{
	/** Press E this tick, AFTER AddMovementInput (Rocco's Ripple takes its direction from it). */
	bool bActivate = false;

	/** Press jump this tick. The ability hook gets first refusal, exactly as a human's does. */
	bool bJump = false;

	/** Spend a dash for an ability reason. Goes through the controller's charge model. */
	bool bDash = false;

	/** The V level. The controller mirrors this onto the pawn and releases it on death / unpossess. */
	bool bSecondaryHeld = false;

	/** True while an ability owns the crosshair. Suppresses the combat aim, like the pass does. */
	bool bOwnsAim = false;
	FVector AimPoint = FVector::ZeroVector;

	/** True when the ability wants the bot to steer somewhere specific (Rocco's dodge). */
	bool bOverrideMove = false;
	FVector MoveDirection = FVector::ZeroVector;

	/**
	 * True when the ability needs the bot to STOP STEERING ENTIRELY.
	 *
	 * Exactly one thing needs it and it is not a nicety. Mace's spike pull is cancelled by "any
	 * movement input" (spec v14 §6), and a bot steers every single frame — MEASURED: 13 pulls in a
	 * 150 s match, and all 13 logged "[Mace] Pull ended (movement input), spike removed" within a
	 * frame or two of starting. The ability fired, was counted, and did nothing. A zero
	 * MoveDirection cannot express this, because zero is also what "no opinion" looks like.
	 */
	bool bSuppressMove = false;

	/** Short, human-readable, for the [BotAbility] log. Never null while any flag above is set. */
	const TCHAR* Reason = nullptr;

	/**
	 * Seconds between this bot DECIDING to use the ability and the key actually being pressed.
	 *
	 * Reported on the firing tick only, and it is the direct measurement of spec v15 §3's "no
	 * frame-one ability use". A run whose minimum is 0.00 s has lost the reaction latch, whatever the
	 * code looks like — which is exactly the class of claim this project does not accept from reading.
	 */
	float MeasuredLatencySeconds = 0.f;
};

/**
 * One bot's ability decisions.
 *
 * Held by value on ATraceBotController; reset on every possession, because a new pawn is a new body
 * and a latched intent from a previous life would fire on the frame the next one spawns.
 */
class FTraceBotAbilityBrain
{
public:
	/** A new pawn. Clears every latch and every timer. */
	void OnPossessed(float Now, float InPersonalityBias);

	/** Losing the pawn. Only clears state — releasing the V input is the controller's job. */
	void OnUnPossessed();

	/** Decide. Fills @p Out; touches nothing else in the world. */
	void Plan(const FTraceBotAbilitySituation& Situation, float DeltaSeconds, FTraceBotAbilityOrders& Out);

	/** The character this brain is currently planning for. None when the player has none. */
	ETraceCharacterId GetPlannedCharacter() const { return PlannedCharacter; }

private:
	// ---------------------------------------------------------------------------------------------
	// Per-character planners. Each one may call Want*() and may write the aim / move overrides.
	// ---------------------------------------------------------------------------------------------

	void PlanRocco(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out);
	void PlanChut(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out);
	void PlanMace(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out);
	void PlanOyster(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out);
	void PlanX(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out);

	// ---------------------------------------------------------------------------------------------
	// THE INTENT LATCH — the whole of "no frame-one ability use"
	// ---------------------------------------------------------------------------------------------

	/**
	 * "I would like to press @p Act, because @p Reason."
	 *
	 * The FIRST call starts a rolled reaction delay and takes the willingness roll. Subsequent calls
	 * with the same act merely keep it alive. Nothing is pressed until Resolve() finds the delay
	 * expired AND the reason still being asserted this tick.
	 *
	 * WHY IT RETURNS A RESULT. The willingness roll is taken ONCE per intent, not per tick — rolled
	 * per tick, anything above about 0.2 fires within two frames and every difficulty behaves
	 * identically. But "declined" then has to mean something, or the next tick simply rolls again and
	 * the knob is meaningless in the other direction. So a caller that is told Declined starts its own
	 * short hesitation, and FTraceBotProfile::Aggression reads as "how long does this difficulty dither
	 * before it commits" — exactly the shape spec v9 §1 settled on for the trace-dash commit, and for
	 * the same reason.
	 *
	 * @param AimPoint  where the ability wants to look. Pass nullptr for the abilities that do not aim.
	 * @param MoveDirection  where the ability needs the bot STEERING when it fires. Rocco's midair
	 *                       redirect blends velocity toward the pawn's acceleration, so a dodge that
	 *                       does not also steer sideways is a small hop and nothing else. Carried by
	 *                       the latch, so the steering is held for the whole reaction delay.
	 */
	ETraceBotWantResult Want(const FTraceBotAbilitySituation& Situation, ETraceBotAbilityAct Act,
	                         const TCHAR* Reason, const FVector* AimPoint = nullptr,
	                         const FVector* MoveDirection = nullptr);

	/** Turns an expired, still-asserted intent into orders. Called once, at the end of Plan(). */
	void Resolve(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out);

	/** Drops the current intent. Called when the reason stops being true. */
	void ClearIntent();

	// ---------------------------------------------------------------------------------------------
	// Shared queries
	// ---------------------------------------------------------------------------------------------

	/** Nearest living enemy with line of sight inside @p MaxRange, or null. Distance in @p OutDist. */
	ATraceCharacter* FindVisibleEnemy(const FTraceBotAbilitySituation& Situation, float MaxRange, float& OutDist) const;

	/** Living enemies within @p Radius of @p Point that the §4 choke point would let us affect. */
	int32 CountAffectableEnemiesNear(const FTraceBotAbilitySituation& Situation, const FVector& Point,
	                                 float Radius, ETraceAbilityEffect Effect, FVector& OutCentroid) const;

	/** True when an enemy could shoot the bot right now: alive, in engagement range, with sight. */
	bool IsUnderThreat(const FTraceBotAbilitySituation& Situation) const;

	/** Adds this bot's ability aim error to a world aim point. Range-scaled, like the combat cone. */
	FVector ApplyAbilityAimError(const FTraceBotAbilitySituation& Situation, const FVector& AimPoint);

	/**
	 * Solves the launch pitch that drops Oyster's Pickler on @p TargetPoint, with aim error applied.
	 * False when nothing in the scanned range reaches it. See the definition for why a first-order
	 * "raise the aim point by the drop" does not work for this projectile.
	 */
	bool SolvePicklerLob(const FTraceBotAbilitySituation& Situation, const FVector& TargetPoint,
	                     FVector& OutAimPoint);

	// ---------------------------------------------------------------------------------------------
	// State
	// ---------------------------------------------------------------------------------------------

	/** Which character the last Plan() was for. A change wipes the per-character scratch below. */
	ETraceCharacterId PlannedCharacter = ETraceCharacterId::None;

	float PersonalityBias = 0.5f;

	// --- the intent latch ---
	ETraceBotAbilityAct Pending = ETraceBotAbilityAct::None;
	const TCHAR* PendingReason = nullptr;
	float PendingReadyTime = 0.f;
	/** When the intent was latched, so the delivered latency can be MEASURED and not assumed. */
	float PendingLatchedTime = 0.f;
	/** Last tick on which the reason was re-asserted, so a stale intent decays instead of firing. */
	float PendingRefreshedTime = 0.f;
	bool  bPendingHasAim = false;
	FVector PendingAimPoint = FVector::ZeroVector;
	bool  bPendingHasMove = false;
	FVector PendingMoveDirection = FVector::ZeroVector;

	/** Earliest world time this bot may press ANY ability key again. Stops same-frame chaining. */
	float NextAnyAbilityTime = 0.f;

	// --- ability aim error, refreshed on the same cadence the combat wobble uses ---
	FVector2D AbilityAimError = FVector2D::ZeroVector;
	float AbilityAimErrorRefreshTime = 0.f;

	// --- Mace ---
	/** A wall the spike could reach, found by the bot's own probe. Mace's own sweep is the authority. */
	FVector SpikeCandidateAnchor = FVector::ZeroVector;
	bool  bHasSpikeCandidate = false;
	float NextSpikeProbeTime = 0.f;
	/** World time the current V hold ends. Zero when not suspending. */
	float SuspendReleaseTime = 0.f;
	float NextSuspendTime = 0.f;

	// --- Oyster ---
	float NextPicklerProbeTime = 0.f;
	FVector PicklerAimPoint = FVector::ZeroVector;
	bool  bHasPicklerAim = false;
	/** Hesitation after a DECLINED approach dash. Firing needs none: the dash charge is the throttle. */
	float NextApproachDashTime = 0.f;

	// --- Chut ---
	/** Hesitation after a DECLINED bash. Firing needs no throttle: the dash charge is the throttle. */
	float NextBashTime = 0.f;

	// --- Rocco ---
	/** Hesitation after a DECLINED redirect. Firing needs none: the extra jump is one per airtime. */
	float NextRedirectTime = 0.f;
};
