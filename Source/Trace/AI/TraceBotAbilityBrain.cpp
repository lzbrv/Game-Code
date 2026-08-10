// Trace — the bot ability brain (spec v15 §3). See TraceBotAbilityBrain.h for why this exists.

#include "AI/TraceBotAbilityBrain.h"

#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"

#include "AI/TraceBotController.h"
#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/Characters/TraceAbilitySetChut.h"
#include "Abilities/Characters/TraceAbilitySetElle.h"
#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Abilities/Characters/TraceAbilitySetOyster.h"
#include "Abilities/Characters/TraceAbilitySetRocco.h"
#include "Abilities/Characters/TraceAbilitySetRoxie.h"
#include "Abilities/Characters/TraceAbilitySetSlimeball.h"
#include "Abilities/Characters/TraceAbilitySetX.h"
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// TUNING — every knob is a CVar under Trace.Bot.*
//
// Spec v15 §3: "Tuning: use CVars (Trace.Bot.*), NOT TraceSettings.h — another agent owns that file."
// So nothing in this pass adds a UTraceSettings property. The three SKILL dials are deliberately
// *multipliers on the existing per-difficulty FTraceBotProfile* rather than absolutes, which is what
// makes "keep the existing difficulty knobs working and scale the new behaviour with them" literally
// true: Easy's Aggression of 0.45 and ReactionTimeSeconds of 0.70 still set the ability tempo, and
// these only bend the curve.
// =================================================================================================

namespace TraceBotAbilityBrainLocal
{
	static TAutoConsoleVariable<int32> CVarBotAbilities(
		TEXT("Trace.Bot.Abilities"),
		1,
		TEXT("1 (default): bots use their character's abilities (spec v15 §3).\n")
		TEXT("0: THE RED ARM. Every ability decision in the bot brain is removed at once and the bots\n")
		TEXT("   behave exactly as they did before this pass — they never press E, V or an ability\n")
		TEXT("   jump, whatever character they are holding. This is the state the project shipped in,\n")
		TEXT("   and it is what makes the [BotAbility] counts evidence rather than an assertion."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarBotAbilityUseScale(
		TEXT("Trace.Bot.AbilityUseScale"),
		1.f,
		TEXT("Multiplier on how willing a bot is to take a marginal ability opportunity. The base is\n")
		TEXT("FTraceBotProfile::Aggression (Easy 0.45, Normal 0.70, Hard 0.90), so 1.0 keeps the\n")
		TEXT("difficulty spread. 0 disarms the willingness roll entirely (bots decline everything);\n")
		TEXT("values above 1 are for measurement runs that need a lot of activations quickly."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarBotAbilityLatencyScale(
		TEXT("Trace.Bot.AbilityLatencyScale"),
		1.35f,
		TEXT("Multiplier on FTraceBotProfile::ReactionTimeSeconds for the delay between a bot DECIDING\n")
		TEXT("to use an ability and pressing the key. Above 1 by default on purpose: choosing an\n")
		TEXT("ability is a slower decision than pulling a trigger, and frame-one ability use is the\n")
		TEXT("single most robotic thing a bot can do. 0 removes the latch, which is the arm that shows\n")
		TEXT("the latch was doing something."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarBotAbilityAimError(
		TEXT("Trace.Bot.AbilityAimErrorScale"),
		1.f,
		TEXT("Multiplier on the aim-error cone applied to AIMED abilities (Mace's spike, Oyster's\n")
		TEXT("Pickler lob). The base is FTraceBotProfile::AimErrorDegrees plus its range term, i.e. the\n")
		TEXT("same model the gun uses, so a bot that sprays with a rifle also lobs jars imprecisely."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarBotAbilityMetrics(
		TEXT("Trace.Bot.AbilityMetrics"),
		0,
		TEXT("1: log a [BotAbility] line every 10s — activations per character, refusals, V holds,\n")
		TEXT("ability jumps and ability dashes. Off by default; free when off."),
		ECVF_Default);

	// =============================================================================================
	// SPEC v18 §3 — the three new characters
	// =============================================================================================

	static TAutoConsoleVariable<int32> CVarBotNewCharacterAbilities(
		TEXT("Trace.Bot.NewCharacterAbilities"),
		1,
		TEXT("1 (default): bots play Roxie, Elle and Slimeball with their abilities (spec v18 §3).\n")
		TEXT("0: THE RED ARM FOR THIS PASS, and it is narrower than Trace.Bot.Abilities on purpose.\n")
		TEXT("   It removes ONLY the three new planners, so a measurement run shows Rocco, Chut, Mace,\n")
		TEXT("   Oyster and X still using their abilities in the same match while a bot Roxie never\n")
		TEXT("   fires a rocket, a bot Elle never places a gate and a bot Slimeball never sticks —\n")
		TEXT("   which is exactly the state spec v18 §3 calls \"reads as broken\", reproduced on demand."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarBotRoxieRocketRoomBehind(
		TEXT("Trace.Bot.RoxieRocketRoomBehindUU"),
		380.f,
		TEXT("How much clear space a bot Roxie wants BEHIND her before she fires the rocket, in uu.\n")
		TEXT("The rocket's whole point is that it throws her backwards; firing it with a wall two feet\n")
		TEXT("behind her spends a 35 s cooldown on a bump. 0 disables the check."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarBotSnapRotationSpan(
		TEXT("Trace.Bot.SnapRotationSpanUU"),
		1100.f,
		TEXT("How far a bot Elle wants to have travelled from her first SNAP gate before she plants the\n")
		TEXT("second, in uu. This is the length of the rotation the pair opens; two mouths a few metres\n")
		TEXT("apart are a 35 s cooldown spent on nothing. She plants it anyway when the 4 s window is\n")
		TEXT("about to lapse, because a lapsed cast costs the full cooldown and gives nothing at all."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarBotSlimeballWallSearch(
		TEXT("Trace.Bot.SlimeballWallSearchUU"),
		300.f,
		TEXT("How far a bot Slimeball looks for a wall worth sticking to, in uu. Larger than the\n")
		TEXT("ability's own SlimeballWallStickRangeUU (90) on purpose: the bot LEANS toward the wall it\n")
		TEXT("found for a moment as it jumps, so the wall only has to be within a step, not in reach."),
		ECVF_Default);

	/** How long the V TAP that fires Roxie's rocket is held down for, in seconds. */
	static constexpr float SecondaryTapSeconds = 0.12f;

	static bool NewCharacterAbilitiesEnabled()
	{
		return CVarBotNewCharacterAbilities.GetValueOnAnyThread() != 0;
	}

	/** Minimum world seconds between any two ability key presses by the same bot. */
	static constexpr float MinGapBetweenPresses = 0.45f;

	/**
	 * How long a latched intent survives without being re-asserted, in seconds.
	 *
	 * Not zero, and the reason is the decision cadence: a planner that only re-evaluates its target
	 * every few frames would otherwise drop its own intent between evaluations and never fire. Not
	 * long either — the point of the latch is that a reason which STOPS being true cancels the press.
	 */
	static constexpr float IntentStaleSeconds = 0.35f;

	/** The skill dials, resolved from the live profile at the point of use so they retune with PIE. */
	static float Willingness(float PersonalityBias)
	{
		const float Base = FMath::Clamp(UTraceSettings::GetBotProfile().Aggression, 0.f, 1.f);
		// Personality spreads a team out either side of its profile, exactly as the reaction clock
		// and the aim cone already do — five bots on one difficulty must not act as one organism.
		const float Personal = 0.75f + 0.5f * FMath::Clamp(PersonalityBias, 0.f, 1.f);
		return FMath::Clamp(Base * Personal * FMath::Max(0.f, CVarBotAbilityUseScale.GetValueOnAnyThread()), 0.f, 1.f);
	}

	static float ReactionDelay(float PersonalityBias)
	{
		const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
		const float Jitter = FMath::Clamp(Profile.ReactionJitterFraction, 0.f, 0.95f);
		const float PersonalityScale = 1.35f - 0.7f * FMath::Clamp(PersonalityBias, 0.f, 1.f);

		return FMath::Max(0.f, Profile.ReactionTimeSeconds)
			* PersonalityScale
			* FMath::FRandRange(1.f - Jitter, 1.f + Jitter)
			* FMath::Max(0.f, CVarBotAbilityLatencyScale.GetValueOnAnyThread());
	}

	/** How far a standard dash carries, from the settings the dash itself reads. */
	static float DashReachUU()
	{
		const UTraceSettings& Settings = UTraceSettings::Get();
		return FMath::Max(100.f, Settings.DashSpeed * Settings.DashDuration);
	}
}

// =================================================================================================
// Measurement
// =================================================================================================

namespace TraceBotAbilityStats
{
	namespace
	{
		// (Named enclosing namespace, so the jumbo/unity build cannot collide this with another
		// file's counters — see Scripts/check-jumbo-build-collisions.py.)
		struct FCounters
		{
			/** Indexed by ETraceCharacterId, so slot 0 (None) stays zero and proves the gate works. */
			int32 Fired[static_cast<int32>(ETraceCharacterId::Count)] = {};
			int32 Attempts[static_cast<int32>(ETraceCharacterId::Count)] = {};

			int32 SpikePulls = 0;

			/**
			 * V presses, PER CHARACTER since spec v18 §2 gave the key to three of them. One total was
			 * enough while Mace's suspend was the only V ability; it would now sum a suspend, a rocket
			 * and a wall stick into a number that describes none of them.
			 */
			int32 SecondaryDowns[static_cast<int32>(ETraceCharacterId::Count)] = {};

			/** The v18 §3 OUTCOMES — see TraceBotAbilityStats::NoteRocketFired for why presses are not enough. */
			int32 RoxieRocketsFired = 0;
			int32 SlimeballSticks = 0;
			int32 ElleCloaks = 0;
			int32 ElleCloakEscapes = 0;

			int32 AbilityJumps = 0;
			int32 AbilityJumpsConsumed = 0;
			int32 AbilityDashes = 0;

			int32  LatencySamples = 0;
			double LatencySum = 0.0;
			float  LatencyMin = -1.f;
			float  LatencyMax = 0.f;

			double NextDumpTime = -1.0;
			double StartTime = -1.0;
		};

		FCounters& Counters()
		{
			static FCounters Singleton;
			return Singleton;
		}
	}

	bool AbilitiesEnabled()
	{
		return TraceBotAbilityBrainLocal::CVarBotAbilities.GetValueOnAnyThread() != 0;
	}

	bool MetricsEnabled()
	{
		return TraceBotAbilityBrainLocal::CVarBotAbilityMetrics.GetValueOnAnyThread() != 0;
	}

	void NoteActivate(ETraceCharacterId Id, bool bFired, bool bWasSpikePull, const TCHAR* Reason,
	                  const APlayerState* Who)
	{
		const int32 Index = FMath::Clamp(static_cast<int32>(Id), 0, static_cast<int32>(ETraceCharacterId::Count) - 1);
		FCounters& C = Counters();
		++C.Attempts[Index];
		if (bFired)
		{
			++C.Fired[Index];
		}
		if (bWasSpikePull)
		{
			++C.SpikePulls;
		}

		// Display, not Verbose. This line IS the evidence the spec asks for, and a run that has to be
		// re-taken with a higher verbosity is a run that has to be re-taken.
		UE_LOG(LogTraceGame, Display, TEXT("[BotAbility] %s (%s) pressed E -> %s%s  reason=%s"),
			*GetNameSafe(Who), TraceCharacterIdToString(Id),
			bFired ? TEXT("FIRED") : TEXT("refused (cooldown / CanActivate / fizzle)"),
			bWasSpikePull ? TEXT(" [spike reactivation]") : TEXT(""),
			(Reason != nullptr) ? Reason : TEXT("?"));
	}

	void NoteSecondary(ETraceCharacterId Id, bool bDown, const TCHAR* Reason)
	{
		if (!bDown)
		{
			return;
		}
		const int32 Index = FMath::Clamp(static_cast<int32>(Id), 0, static_cast<int32>(ETraceCharacterId::Count) - 1);
		++Counters().SecondaryDowns[Index];

		if (Reason != nullptr)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[BotAbility] %s pressed V  reason=%s"),
				TraceCharacterIdToString(Id), Reason);
		}
		else
		{
			// Mace's suspend reaches here: its hold is started directly by PlanMace rather than by the
			// intent latch, so there is no reason string to print and there never was one.
			UE_LOG(LogTraceGame, Verbose, TEXT("[BotAbility] %s pressed V (secondary)."), TraceCharacterIdToString(Id));
		}
	}

	void NoteRocketFired()
	{
		++Counters().RoxieRocketsFired;
		UE_LOG(LogTraceGame, Display, TEXT("[BotAbility] Roxie: rocket away (V). She is on her way backwards."));
	}

	void NoteWallStick()
	{
		++Counters().SlimeballSticks;
		UE_LOG(LogTraceGame, Display, TEXT("[BotAbility] Slimeball: got hold of a wall (V held)."));
	}

	void NoteCloak()
	{
		++Counters().ElleCloaks;
		UE_LOG(LogTraceGame, Display, TEXT("[BotAbility] Elle: cloaked (she just passed or threw the Core)."));
	}

	void NoteCloakEscape()
	{
		++Counters().ElleCloakEscapes;
		UE_LOG(LogTraceGame, Display,
			TEXT("[BotAbility] Elle: breaking away under the cloak — steering away from the nearest enemy."));
	}

	void NoteAbilityJump(ETraceCharacterId Id, bool bConsumed)
	{
		FCounters& C = Counters();
		++C.AbilityJumps;
		if (bConsumed)
		{
			++C.AbilityJumpsConsumed;
			UE_LOG(LogTraceGame, Verbose, TEXT("[BotAbility] %s: jump consumed by the ability hook."),
				TraceCharacterIdToString(Id));
		}
	}

	void NoteAbilityDash(ETraceCharacterId /*Id*/)
	{
		++Counters().AbilityDashes;
	}

	void NoteLatency(float Seconds)
	{
		FCounters& C = Counters();
		++C.LatencySamples;
		C.LatencySum += static_cast<double>(Seconds);
		if (C.LatencyMin < 0.f || Seconds < C.LatencyMin)
		{
			C.LatencyMin = Seconds;
		}
		C.LatencyMax = FMath::Max(C.LatencyMax, Seconds);
	}

	void Poll(const UWorld* World)
	{
		if (World == nullptr || !MetricsEnabled())
		{
			return;
		}

		FCounters& C = Counters();
		const double Now = World->GetTimeSeconds();
		if (C.StartTime < 0.0)
		{
			C.StartTime = Now;
			C.NextDumpTime = Now + 10.0;
			return;
		}
		if (Now < C.NextDumpTime)
		{
			return;
		}
		C.NextDumpTime = Now + 10.0;

		auto Slot = [](ETraceCharacterId Id) { return static_cast<int32>(Id); };

		UE_LOG(LogTraceGame, Display,
			TEXT("[BotAbility] t=%.0fs | E fired/attempted per character: Rocco=%d/%d Chut=%d/%d ")
			TEXT("Mace=%d/%d Oyster=%d/%d X=%d/%d Roxie=%d/%d Elle=%d/%d Slimeball=%d/%d ")
			TEXT("| none(should be 0)=%d/%d ")
			TEXT("| spike-pulls=%d ability-jumps=%d (consumed %d) ability-dashes=%d"),
			Now - C.StartTime,
			C.Fired[Slot(ETraceCharacterId::Rocco)],  C.Attempts[Slot(ETraceCharacterId::Rocco)],
			C.Fired[Slot(ETraceCharacterId::Chut)],   C.Attempts[Slot(ETraceCharacterId::Chut)],
			C.Fired[Slot(ETraceCharacterId::Mace)],   C.Attempts[Slot(ETraceCharacterId::Mace)],
			C.Fired[Slot(ETraceCharacterId::Oyster)], C.Attempts[Slot(ETraceCharacterId::Oyster)],
			C.Fired[Slot(ETraceCharacterId::X)],      C.Attempts[Slot(ETraceCharacterId::X)],
			C.Fired[Slot(ETraceCharacterId::Roxie)],  C.Attempts[Slot(ETraceCharacterId::Roxie)],
			C.Fired[Slot(ETraceCharacterId::Elle)],   C.Attempts[Slot(ETraceCharacterId::Elle)],
			C.Fired[Slot(ETraceCharacterId::Slimeball)], C.Attempts[Slot(ETraceCharacterId::Slimeball)],
			C.Fired[Slot(ETraceCharacterId::None)],   C.Attempts[Slot(ETraceCharacterId::None)],
			C.SpikePulls, C.AbilityJumps, C.AbilityJumpsConsumed, C.AbilityDashes);

		// SPEC v18 §3's own acceptance test, on its own line. V is three abilities now, and the three
		// OUTCOME columns are the ones that answer "does a bot Roxie actually fire a rocket" — a press
		// column cannot, because her rocket, unlike E, is arbitrated inside her own ability set.
		UE_LOG(LogTraceGame, Display,
			TEXT("[BotAbility] t=%.0fs | V presses: Mace(suspend)=%d Roxie(rocket)=%d Slimeball(stick)=%d ")
			TEXT("| v18 outcomes: rockets away=%d wall-sticks=%d Elle cloaks=%d (escaped under %d)"),
			Now - C.StartTime,
			C.SecondaryDowns[Slot(ETraceCharacterId::Mace)],
			C.SecondaryDowns[Slot(ETraceCharacterId::Roxie)],
			C.SecondaryDowns[Slot(ETraceCharacterId::Slimeball)],
			C.RoxieRocketsFired, C.SlimeballSticks, C.ElleCloaks, C.ElleCloakEscapes);

		// THE "NATURAL" MEASUREMENT. min must never be 0.00 — that would mean an ability fired on the
		// frame it was decided, which is the single most robotic thing a bot can do and is what spec
		// v15 §3 names first. mean tracks FTraceBotProfile::ReactionTimeSeconds, so the difficulty
		// gradient is visible here even when the 20 s cooldown caps the activation COUNT.
		UE_LOG(LogTraceGame, Display,
			TEXT("[BotAbility] t=%.0fs | reaction latency before an ability press: n=%d min=%.2fs mean=%.2fs max=%.2fs"),
			Now - C.StartTime, C.LatencySamples,
			(C.LatencyMin < 0.f) ? 0.f : C.LatencyMin,
			(C.LatencySamples > 0) ? (C.LatencySum / C.LatencySamples) : 0.0,
			C.LatencyMax);
	}
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void FTraceBotAbilityBrain::OnPossessed(float Now, float InPersonalityBias)
{
	PersonalityBias = FMath::Clamp(InPersonalityBias, 0.f, 1.f);
	PlannedCharacter = ETraceCharacterId::None;

	ClearIntent();

	// A fresh pawn does not get to press an ability on frame one. One reaction time of grace, so ten
	// bots respawning on the same frame do not all Sting at once — the same argument the controller
	// already makes for BlindUntilTime.
	NextAnyAbilityTime = Now + TraceBotAbilityBrainLocal::ReactionDelay(PersonalityBias);

	AbilityAimError = FVector2D::ZeroVector;
	AbilityAimErrorRefreshTime = 0.f;

	SecondaryHoldUntilTime = 0.f;

	bHasSpikeCandidate = false;
	NextSpikeProbeTime = 0.f;
	NextSuspendTime = 0.f;

	NextPicklerProbeTime = 0.f;
	bHasPicklerAim = false;
	NextApproachDashTime = 0.f;

	NextBashTime = 0.f;
	NextRedirectTime = 0.f;

	// --- spec v18 §3 ---
	//
	// The three "was it true last tick" flags are cleared to FALSE rather than sampled, and that is
	// deliberate: a fresh pawn is not stuck, not cloaked and has a ready rocket, so false is the truth
	// AND it means the first sample after a respawn cannot invent an outcome that never happened.
	NextRocketTime = 0.f;
	bRocketWasReady = false;

	SnapFirstGateLocation = FVector::ZeroVector;
	SnapFirstPressTime = 0.f;
	bSnapFirstGatePlaced = false;
	NextSnapTime = 0.f;
	CloakEscapeStartTime = 0.f;
	bCloakEscapeAllowed = false;
	bCloakEscapeCounted = false;
	bWasCloaked = false;

	StickWallPoint = FVector::ZeroVector;
	bHasStickWall = false;
	NextStickProbeTime = 0.f;
	NextStickTime = 0.f;
	StickApproachUntilTime = 0.f;
	bWasStuck = false;
}

void FTraceBotAbilityBrain::OnUnPossessed()
{
	ClearIntent();
	SecondaryHoldUntilTime = 0.f;
	bHasSpikeCandidate = false;
	bHasPicklerAim = false;
	bHasStickWall = false;
	bSnapFirstGatePlaced = false;
	StickApproachUntilTime = 0.f;
	PlannedCharacter = ETraceCharacterId::None;
}

void FTraceBotAbilityBrain::ClearIntent()
{
	Pending = ETraceBotAbilityAct::None;
	PendingReason = nullptr;
	PendingReadyTime = 0.f;
	PendingLatchedTime = 0.f;
	PendingRefreshedTime = 0.f;
	bPendingHasAim = false;
	PendingAimPoint = FVector::ZeroVector;
	bPendingHasMove = false;
	PendingMoveDirection = FVector::ZeroVector;
}

// =================================================================================================
// THE INTENT LATCH
// =================================================================================================

ETraceBotWantResult FTraceBotAbilityBrain::Want(const FTraceBotAbilitySituation& Situation,
                                               ETraceBotAbilityAct Act, const TCHAR* Reason,
                                               const FVector* AimPoint, const FVector* MoveDirection)
{
	if (Act == ETraceBotAbilityAct::None)
	{
		return ETraceBotWantResult::Busy;
	}

	// An intent already in flight for a DIFFERENT act is not replaced. Changing your mind halfway
	// through a reaction and pressing immediately is the one thing this whole mechanism exists to
	// stop: it would hand a bot a zero-latency second option every time the first one lapsed.
	if (Pending != ETraceBotAbilityAct::None && Pending != Act)
	{
		return ETraceBotWantResult::Busy;
	}

	if (Pending == Act)
	{
		// Keep it alive, and let a later evaluation refine the aim (a moving Pickler target) or the
		// steering (a dodge whose shooter has moved).
		PendingRefreshedTime = Situation.Now;
		if (AimPoint != nullptr)
		{
			PendingAimPoint = *AimPoint;
			bPendingHasAim = true;
		}
		if (MoveDirection != nullptr)
		{
			PendingMoveDirection = *MoveDirection;
			bPendingHasMove = true;
		}
		return ETraceBotWantResult::Kept;
	}

	// --- a NEW intent: the willingness roll, then the reaction clock ------------------------------
	if (FMath::FRand() >= TraceBotAbilityBrainLocal::Willingness(PersonalityBias))
	{
		return ETraceBotWantResult::Declined;
	}

	Pending = Act;
	PendingReason = Reason;
	PendingReadyTime = Situation.Now + TraceBotAbilityBrainLocal::ReactionDelay(PersonalityBias);
	PendingLatchedTime = Situation.Now;
	PendingRefreshedTime = Situation.Now;
	bPendingHasAim = (AimPoint != nullptr);
	PendingAimPoint = (AimPoint != nullptr) ? *AimPoint : FVector::ZeroVector;
	bPendingHasMove = (MoveDirection != nullptr);
	PendingMoveDirection = (MoveDirection != nullptr) ? *MoveDirection : FVector::ZeroVector;
	return ETraceBotWantResult::Latched;
}

void FTraceBotAbilityBrain::Resolve(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out)
{
	if (Pending == ETraceBotAbilityAct::None)
	{
		return;
	}

	// The reason stopped being asserted. This is the branch that makes the latch a REACTION rather
	// than a queue: a bot that decided to Sting somebody who then broke line of sight forgets about
	// it instead of stinging an empty corridor a moment later.
	if ((Situation.Now - PendingRefreshedTime) > TraceBotAbilityBrainLocal::IntentStaleSeconds)
	{
		ClearIntent();
		return;
	}

	if (Situation.Now < PendingReadyTime || Situation.Now < NextAnyAbilityTime)
	{
		// Still slewing / still waiting. An AIMED ability owns the crosshair for the whole wait, which
		// is what makes the aim slew part of the cost rather than something that happens after the
		// decision — a human turns to face the wall before throwing the spike. A STEERED one owns the
		// movement input for the same reason and with more force: Rocco's redirect reads the pawn's
		// acceleration on the frame it fires, so the lean has to already be there.
		if (bPendingHasAim)
		{
			Out.bOwnsAim = true;
			Out.AimPoint = PendingAimPoint;
		}
		if (bPendingHasMove)
		{
			Out.bOverrideMove = true;
			Out.MoveDirection = PendingMoveDirection;
		}
		if (bPendingHasAim || bPendingHasMove)
		{
			Out.Reason = PendingReason;
		}
		return;
	}

	switch (Pending)
	{
	case ETraceBotAbilityAct::Activate: Out.bActivate = true; break;
	case ETraceBotAbilityAct::Jump:     Out.bJump = true;     break;
	case ETraceBotAbilityAct::Dash:     Out.bDash = true;     break;

	// THE V TAP. It is expressed as a very short HOLD rather than a one-frame flag, because V is a
	// level all the way down to UTraceAbilityInputRelay::RouteSecondaryEdge — the press and the
	// release are two calls and the ability sees both, exactly as it does from a human's key. The
	// level block at the end of Plan() turns this deadline back into the bool the controller mirrors.
	case ETraceBotAbilityAct::Secondary:
		SecondaryHoldUntilTime = Situation.Now + TraceBotAbilityBrainLocal::SecondaryTapSeconds;
		Out.bSecondaryPressedNow = true;
		Out.SecondaryReason = PendingReason;
		break;

	default: break;
	}

	if (bPendingHasAim)
	{
		Out.bOwnsAim = true;
		Out.AimPoint = PendingAimPoint;
	}
	if (bPendingHasMove)
	{
		Out.bOverrideMove = true;
		Out.MoveDirection = PendingMoveDirection;
	}
	Out.Reason = PendingReason;
	Out.MeasuredLatencySeconds = Situation.Now - PendingLatchedTime;

	NextAnyAbilityTime = Situation.Now + TraceBotAbilityBrainLocal::MinGapBetweenPresses;
	ClearIntent();
}

// =================================================================================================
// Shared queries
// =================================================================================================

ATraceCharacter* FTraceBotAbilityBrain::FindVisibleEnemy(const FTraceBotAbilitySituation& Situation,
                                                         float MaxRange, float& OutDist) const
{
	OutDist = TNumericLimits<float>::Max();
	ATraceCharacter* Best = nullptr;

	if (Situation.Enemies == nullptr || Situation.Self == nullptr || Situation.BotController == nullptr)
	{
		return nullptr;
	}

	const FVector MyLocation = Situation.Self->GetActorLocation();
	const float MaxRangeSq = FMath::Square(FMath::Max(1.f, MaxRange));
	float BestDistSq = MaxRangeSq;

	for (ATraceCharacter* Other : *Situation.Enemies)
	{
		if (Other == nullptr || !Other->IsAlive())
		{
			continue;
		}

		// Compared in squared space throughout, and the square root is taken once at the end — the
		// line trace below is the expensive part and it must only run for a candidate that has
		// already won on distance.
		const float DistSq = static_cast<float>(FVector::DistSquared(MyLocation, Other->GetActorLocation()));
		if (DistSq > BestDistSq)
		{
			continue;
		}

		// The controller's own rule, asked rather than re-derived: only static geometry blocks, so a
		// team-mate standing in the way is a contest and not a reason to hold the ability.
		if (!Situation.BotController->HasLineOfSight(Other))
		{
			continue;
		}

		BestDistSq = DistSq;
		Best = Other;
	}

	if (Best != nullptr)
	{
		OutDist = FMath::Sqrt(BestDistSq);
	}
	return Best;
}

int32 FTraceBotAbilityBrain::CountAffectableEnemiesNear(const FTraceBotAbilitySituation& Situation,
                                                        const FVector& Point, float Radius,
                                                        ETraceAbilityEffect Effect, FVector& OutCentroid) const
{
	OutCentroid = Point;

	if (Situation.Enemies == nullptr || Situation.Abilities == nullptr)
	{
		return 0;
	}

	const float RadiusSq = FMath::Square(FMath::Max(1.f, Radius));
	FVector Sum = FVector::ZeroVector;
	int32 Count = 0;

	for (const ATraceCharacter* Other : *Situation.Enemies)
	{
		if (Other == nullptr || !Other->IsAlive())
		{
			continue;
		}
		if (FVector::DistSquared(Point, Other->GetActorLocation()) > RadiusSq)
		{
			continue;
		}

		// *** SPEC v14 §4 / v15 §3, AND THE ONE RULE THIS FILE IS MOST ABLE TO BREAK. ***
		//
		// "No ability may damage a Core carrier. Route through the existing choke point, do not
		// reimplement the check." So the bot does not ask "is this a carrier" — it asks the choke
		// point the ability itself will ask, with the same effect class, and a target it refuses is
		// not counted as a reason to throw. The carrier is protected by CanAffectTarget whatever this
		// file does; asking here as well is what stops a bot from wasting a 20 s cooldown lobbing a
		// jar at somebody the rules will make it bounce off.
		if (!Situation.Abilities->CanAffectTarget(Other, Effect))
		{
			continue;
		}

		Sum += Other->GetActorLocation();
		++Count;
	}

	if (Count > 0)
	{
		OutCentroid = Sum / static_cast<float>(Count);
	}
	return Count;
}

bool FTraceBotAbilityBrain::IsUnderThreat(const FTraceBotAbilitySituation& Situation) const
{
	float Dist = 0.f;
	return FindVisibleEnemy(Situation, UTraceSettings::GetBotProfile().MaxEngagementRange, Dist) != nullptr;
}

FVector FTraceBotAbilityBrain::ApplyAbilityAimError(const FTraceBotAbilitySituation& Situation, const FVector& AimPoint)
{
	const ATraceCharacter* BotPawn = Situation.Self;
	if (BotPawn == nullptr)
	{
		return AimPoint;
	}

	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
	const FVector ViewLocation = BotPawn->GetPawnViewLocation();
	FVector ToAim = AimPoint - ViewLocation;
	const float Range = static_cast<float>(ToAim.Size());
	if (Range < KINDA_SMALL_NUMBER)
	{
		return AimPoint;
	}

	if (Situation.Now >= AbilityAimErrorRefreshTime)
	{
		// Refreshed on a slow cadence like the combat wobble, not per frame: a per-frame reroll is a
		// tremor, and a tremor averages out over the aim slew into no error at all.
		AbilityAimErrorRefreshTime = Situation.Now + FMath::FRandRange(0.35f, 0.8f);

		float ErrorScale = FMath::Max(0.f, Profile.AimErrorDegrees)
			+ FMath::Max(0.f, Profile.AimErrorPerThousandRange) * (Range / 1000.f);
		ErrorScale *= (1.35f - 0.7f * PersonalityBias);
		ErrorScale *= FMath::Max(0.f, TraceBotAbilityBrainLocal::CVarBotAbilityAimError.GetValueOnAnyThread());
		ErrorScale = FMath::Min(ErrorScale, FMath::Max(0.f, Profile.AimErrorMaxDegrees));

		AbilityAimError.X = FMath::FRandRange(-ErrorScale, ErrorScale);
		AbilityAimError.Y = FMath::FRandRange(-ErrorScale, ErrorScale) * 0.5f;
	}

	FRotator Aim = ToAim.Rotation();
	Aim.Yaw += AbilityAimError.X;
	Aim.Pitch = FMath::Clamp(Aim.Pitch + AbilityAimError.Y, -85.f, 85.f);

	return ViewLocation + Aim.Vector() * Range;
}

// =================================================================================================
// Plan
// =================================================================================================

void FTraceBotAbilityBrain::Plan(const FTraceBotAbilitySituation& Situation, float /*DeltaSeconds*/,
                                 FTraceBotAbilityOrders& Out)
{
	Out = FTraceBotAbilityOrders();

	if (Situation.Self == nullptr || Situation.Abilities == nullptr)
	{
		ClearIntent();
		PlannedCharacter = ETraceCharacterId::None;
		return;
	}

	// THE RED ARM. One branch removes the entire pass, which is what makes the green arm a
	// measurement. Note it runs BEFORE the V level is computed, so a suspended Mace is dropped too.
	if (!TraceBotAbilityStats::AbilitiesEnabled())
	{
		ClearIntent();
		SecondaryHoldUntilTime = 0.f;
		return;
	}

	const ETraceCharacterId MyCharacter = Situation.Abilities->GetCharacterId();
	if (MyCharacter != PlannedCharacter)
	{
		// The character changed under us (selection, half time, the mode-A freeze). Everything latched
		// belonged to the character that has just left.
		ClearIntent();
		SecondaryHoldUntilTime = 0.f;
		bHasSpikeCandidate = false;
		bHasPicklerAim = false;
		bHasStickWall = false;
		bSnapFirstGatePlaced = false;
		StickApproachUntilTime = 0.f;
		bRocketWasReady = false;
		bWasStuck = false;
		bWasCloaked = false;
		bCloakEscapeAllowed = false;
		bCloakEscapeCounted = false;
		PlannedCharacter = MyCharacter;
	}

	if (MyCharacter == ETraceCharacterId::None)
	{
		return;   // the default characterless Mannequin. Every hook in the framework is a no-op here.
	}

	// A carrier mid-pass has its shield down for half a second and that window is the only time it
	// can be shot. Doing anything else with the hands during it — least of all something that owns
	// the crosshair — is how a bot cancels its own pass. The pass machine wins outright.
	if (Situation.bPassing)
	{
		ClearIntent();
		SecondaryHoldUntilTime = 0.f;
		return;
	}

	// SPEC v18 §3. Watched BEFORE any decision, so a press made this tick cannot be mistaken for the
	// outcome of the last one. Costs three pointer compares on the five older characters.
	ObserveNewCharacterOutcomes(Situation);

	switch (MyCharacter)
	{
	case ETraceCharacterId::Rocco:  PlanRocco(Situation, Out);  break;
	case ETraceCharacterId::Chut:   PlanChut(Situation, Out);   break;
	case ETraceCharacterId::Mace:   PlanMace(Situation, Out);   break;
	case ETraceCharacterId::Oyster: PlanOyster(Situation, Out); break;
	case ETraceCharacterId::X:      PlanX(Situation, Out);      break;

	// --- spec v18 §3. The red arm is INSIDE the switch rather than around the three calls, so that
	//     "a bot Roxie that never fires a rocket" is reproduced by the same default: break; the code
	//     had before this pass existed. -----------------------------------------------------------
	case ETraceCharacterId::Roxie:
		if (TraceBotAbilityBrainLocal::NewCharacterAbilitiesEnabled()) { PlanRoxie(Situation, Out); }
		break;
	case ETraceCharacterId::Elle:
		if (TraceBotAbilityBrainLocal::NewCharacterAbilitiesEnabled()) { PlanElle(Situation, Out); }
		break;
	case ETraceCharacterId::Slimeball:
		if (TraceBotAbilityBrainLocal::NewCharacterAbilitiesEnabled()) { PlanSlimeball(Situation, Out); }
		break;

	default: break;
	}

	Resolve(Situation, Out);

	// SPEC v18 §3 — SLIMEBALL'S STICK STARTS ON THE SAME TICK AS THE JUMP THAT MAKES IT POSSIBLE.
	//
	// His own file describes the natural human input as "hold V, then jump into the wall", and its
	// retry loop only looks for a wall while V is down AND he is falling. So the V level has to go
	// down as the jump is pressed, not after he has landed again — and the jump is the act the
	// reaction latch is carrying, so this is the first moment the decision is known to have fired.
	// Pressing V early costs nothing: OnSecondaryPressed with no wall in reach is a free no-op.
	if (MyCharacter == ETraceCharacterId::Slimeball && Out.bJump && SecondaryHoldUntilTime <= 0.f)
	{
		const float MinHold = 1.1f;
		const float MaxHold = 2.6f;
		SecondaryHoldUntilTime = Situation.Now + FMath::FRandRange(MinHold, MaxHold);

		// The jump's reason IS the V press's reason — they are one decision arriving on two keys.
		Out.SecondaryReason = Out.Reason;

		// Lean toward the wall for a moment after take-off. A jump straight up beside a wall is 90 uu
		// of nothing: ProbeForWall measures from his capsule CENTRE and reaches 90 uu, so he has to
		// close most of the gap himself. This is the same argument as Rocco's redirect needing the
		// steering to be there on the frame it fires.
		StickApproachUntilTime = Situation.Now + 0.7f;

		// One stick at a time, and a gap after it: the hold plus a few seconds. Without this he would
		// re-latch the moment the hold lapsed and live on the wall.
		NextStickTime = SecondaryHoldUntilTime + 3.f;
	}

	// SPEC v18 §3 — ELLE'S FIRST GATE. Recorded here rather than in PlanElle because THIS is the tick
	// the press actually happens on (the controller runs ApplyAbilityInputs later in the same frame),
	// and because IsAwaitingSecondGate() is still false right now, which is what identifies it as the
	// OPENING press rather than the completing one.
	if (MyCharacter == ETraceCharacterId::Elle && Out.bActivate)
	{
		if (const UTraceAbilitySetElle* ElleSet = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetElle>())
		{
			if (!ElleSet->IsAwaitingSecondGate())
			{
				SnapFirstGateLocation = Situation.Self->GetActorLocation();
				SnapFirstPressTime = Situation.Now;
				bSnapFirstGatePlaced = true;
			}
			else
			{
				bSnapFirstGatePlaced = false;   // that was the completing press; the cast is over
			}
		}
	}

	// V is a LEVEL, not an edge — Mace's suspend, Slimeball's stick and (as a very short hold) Roxie's
	// rocket tap all arrive through it — so it is asserted after Resolve() rather than through it.
	if (SecondaryHoldUntilTime > 0.f)
	{
		Out.bSecondaryHeld = (Situation.Now < SecondaryHoldUntilTime);
		if (!Out.bSecondaryHeld)
		{
			SecondaryHoldUntilTime = 0.f;
		}
	}
}

// =================================================================================================
// SPEC v18 §3 — did the ability actually LAND?
// =================================================================================================

void FTraceBotAbilityBrain::ObserveNewCharacterOutcomes(const FTraceBotAbilitySituation& Situation)
{
	switch (PlannedCharacter)
	{
	case ETraceCharacterId::Roxie:
	{
		const UTraceAbilitySetRoxie* Roxie = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetRoxie>();
		if (Roxie != nullptr)
		{
			// READY -> NOT READY is the rocket leaving the tube. Nothing else moves that deadline:
			// the 35 s is charged by ATraceRoxieRocket's spawn and by nothing else in her set.
			const bool bReadyNow = Roxie->IsRocketReady();
			if (bRocketWasReady && !bReadyNow)
			{
				TraceBotAbilityStats::NoteRocketFired();
			}
			bRocketWasReady = bReadyNow;
		}
		break;
	}

	case ETraceCharacterId::Slimeball:
	{
		const UTraceAbilitySetSlimeball* Slime = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetSlimeball>();
		if (Slime != nullptr)
		{
			const bool bStuckNow = Slime->IsStuck();
			if (!bWasStuck && bStuckNow)
			{
				TraceBotAbilityStats::NoteWallStick();
			}
			bWasStuck = bStuckNow;
		}
		break;
	}

	case ETraceCharacterId::Elle:
	{
		const UTraceAbilitySetElle* ElleSet = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetElle>();
		if (ElleSet != nullptr)
		{
			// The cloak is a PASSIVE nobody presses, so this count is the only evidence that a bot
			// Elle is triggering it at all — i.e. that bots pass and throw the Core often enough for
			// her defining trait to exist in a match.
			const bool bCloakedNow = ElleSet->IsCloaked();
			if (!bWasCloaked && bCloakedNow)
			{
				TraceBotAbilityStats::NoteCloak();

				// BOTH GATES ROLLED ONCE, HERE, rather than per tick in the planner — a per-tick roll
				// averages out to "always, immediately", which is the frame-one behaviour spec v15 §3
				// names first and the difficulty knob doing nothing at all.
				//
				// The willingness roll is what puts the cloak escape under Trace.Bot.AbilityUseScale
				// with everything else in this file. Without it, an Easy bot would still disengage
				// perfectly every time she gave the Core away — the one thing in this pass that no
				// difficulty setting could soften.
				CloakEscapeStartTime = Situation.Now + TraceBotAbilityBrainLocal::ReactionDelay(PersonalityBias);
				bCloakEscapeAllowed = (FMath::FRand() < TraceBotAbilityBrainLocal::Willingness(PersonalityBias));
				bCloakEscapeCounted = false;
			}
			bWasCloaked = bCloakedNow;
		}
		break;
	}

	default:
		break;
	}
}

// =================================================================================================
// ROCCO — "Ripple to cross open ground or to give a teammate a lane; the midair redirect to dodge."
// =================================================================================================

void FTraceBotAbilityBrain::PlanRocco(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out)
{
	ATraceCharacter* BotPawn = Situation.Self;
	UWorld* WorldPtr = BotPawn->GetWorld();
	const UTraceAbilitySetRocco* Rocco = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetRocco>();
	if (WorldPtr == nullptr || Rocco == nullptr)
	{
		return;
	}

	const UTraceCharacterMovementComponent* Movement = BotPawn->GetTraceMovement();
	const bool bAirborne = (Movement != nullptr) && !Movement->IsMovingOnGround();

	// --- THE MIDAIR REDIRECT: "to dodge" ---------------------------------------------------------
	//
	// §6's own words for the movement ability are "a very small second jump, which allows Rocco to
	// change direction midair, instantly", and §3 says a bot should use it TO DODGE. So the trigger
	// is being airborne, in somebody's sights, with the extra jump still in hand — and the ability
	// itself takes its direction from the pawn's ACCELERATION, which means the dodge is only real if
	// the bot also steers sideways on the same frame. Hence the move override; without it the
	// redirect would blend the bot's velocity toward the direction it was already running and be
	// invisible.
	//
	// AND THE PART THAT MAKES IT REACHABLE AT ALL. Measured on the first green run: 112 jumps reached
	// the ability hook and NONE were consumed, because bots are essentially never airborne while an
	// enemy is in sight — the movement kit only leaves the ground for a slide-jump, an unstick jump
	// or a jar jump, and none of those happen in a duel. An ability that can only fire in a state the
	// bot never enters has not been implemented, it has been written down. So a threatened Rocco with
	// his extra jump in hand JUMPS, which is an ordinary jump costing nothing, and the redirect then
	// has an airtime to live in. That is also what a human does with this character.
	{
		float ThreatDist = 0.f;
		const ATraceCharacter* Shooter = (Rocco->IsSecondJumpAvailable() && Situation.Now >= NextRedirectTime && !Situation.bIAmCarrier)
			? FindVisibleEnemy(Situation, UTraceSettings::GetBotProfile().MaxEngagementRange, ThreatDist)
			: nullptr;

		if (Shooter != nullptr && !bAirborne)
		{
			const ETraceBotWantResult HopResult = Want(Situation, ETraceBotAbilityAct::Jump,
				TEXT("Rocco: jump into a duel, redirect on the way down"));
			if (HopResult == ETraceBotWantResult::Declined)
			{
				NextRedirectTime = Situation.Now + 1.5f;
			}
		}
		else if (Shooter != nullptr)
		{
			// Across the line of fire, not away from it: the whole value of an instant direction
			// change against a finite-slew-rate aim is lateral, and running straight away from a
			// shooter is the one direction their crosshair does not have to move at all.
			//
			// The handedness is the bot's existing dodge sign rather than a fresh coin per frame — a
			// per-frame flip would average to no lateral movement at all, which is the failure mode
			// this whole branch exists to avoid.
			FVector ToShooter = Shooter->GetActorLocation() - BotPawn->GetActorLocation();
			ToShooter.Z = 0.f;
			const float Sign = (PersonalityBias < 0.5f) ? -1.f : 1.f;
			const FVector Lateral = FVector::CrossProduct(FVector::UpVector, ToShooter.GetSafeNormal()) * Sign;

			if (!Lateral.IsNearlyZero())
			{
				// The steering is carried BY THE LATCH, not asserted here: the redirect reads the
				// pawn's acceleration on the frame it fires, which is one reaction delay from now,
				// and a lean asserted only on the deciding frame would be long gone by then.
				const ETraceBotWantResult Result = Want(Situation, ETraceBotAbilityAct::Jump,
					TEXT("Rocco: midair redirect to dodge"), /*AimPoint*/ nullptr, &Lateral);

				// Only a DECLINE hesitates. Firing needs no throttle — IsSecondJumpAvailable() is
				// false for the rest of the airtime the moment the hook consumes the jump.
				if (Result == ETraceBotWantResult::Declined)
				{
					NextRedirectTime = Situation.Now + 1.f;
				}
			}
		}
	}

	// --- THE RIPPLE: "to cross open ground, or to give a teammate a lane" -------------------------
	//
	// A 20 s cooldown on a traversal tool is a decision about the NEXT few seconds, not about this
	// frame, so the test is about the run ahead rather than about a target. Two reasons qualify:
	//
	//   * OPEN GROUND. The bot has a long way to go, the way is clear, and nobody is close enough to
	//     make sprinting down a fixed line suicidal. This is the carrier's traversal and the
	//     interceptor's closing tool.
	//   * A TEAMMATE'S LANE. §6: the ripple is ridable by "any character, either team", and the Core
	//     carrier can use it. A ripple is laid where ROCCO runs (ActivateAbility composes its
	//     direction from his own acceleration), so "giving a teammate a lane" means running ahead of
	//     the carrier along their line and laying it there. That is the extra condition, not a
	//     different call.
	if (Situation.Abilities->GetActivatedCooldownRemaining() > 0.f)
	{
		return;
	}

	FVector Heading = Situation.DesiredMoveDirection;
	Heading.Z = 0.f;
	Heading = Heading.GetSafeNormal();
	if (Heading.IsNearlyZero() || bAirborne)
	{
		return;   // a Ripple with no direction is a fizzle, and it is laid on the ground
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float RippleLength = FMath::Max(200.f,
		(Settings.DashSpeed * Settings.RoccoRippleDashSpeedMultiplier)
		* (Settings.DashDuration * Settings.RoccoRippleDashDurationMultiplier));

	// Is the run actually open? A ripple laid into a wall two metres ahead is the "press E off
	// cooldown" behaviour the spec explicitly refuses.
	{
		const FVector Start = BotPawn->GetActorLocation();
		FCollisionQueryParams Params(TEXT("TraceBotRippleLane"), /*bTraceComplex=*/false, BotPawn);
		FHitResult Hit;
		if (WorldPtr->LineTraceSingleByChannel(Hit, Start, Start + Heading * RippleLength, ECC_Visibility, Params))
		{
			bHasSpikeCandidate = false;   // (shared scratch is per-character; nothing to keep here)
			return;
		}
	}

	// How far is there still to go? Riding a ripple 400 uu from the goal is a waste of the cooldown.
	const float ToObjective = static_cast<float>(FVector::Dist2D(BotPawn->GetActorLocation(), Situation.AttackGoal));

	float NearestEnemyDist = 0.f;
	const bool bEnemyClose = FindVisibleEnemy(Situation, 900.f, NearestEnemyDist) != nullptr;

	const bool bCrossingOpenGround = (ToObjective > RippleLength * 2.f) && !bEnemyClose;

	bool bGivingALane = false;
	if (Situation.bTeammateIsCarrier && Situation.Carrier != nullptr)
	{
		const FVector CarrierHeading = Situation.Carrier->GetVelocity().GetSafeNormal2D();
		const float NearCarrier = static_cast<float>(FVector::Dist2D(BotPawn->GetActorLocation(),
			Situation.Carrier->GetActorLocation()));

		bGivingALane = !CarrierHeading.IsNearlyZero()
			&& NearCarrier < 2000.f
			&& FVector::DotProduct(CarrierHeading, Heading) > 0.8f;
	}

	if (bCrossingOpenGround || bGivingALane)
	{
		Want(Situation, ETraceBotAbilityAct::Activate,
			bGivingALane ? TEXT("Rocco: Ripple to give the carrier a lane")
			             : TEXT("Rocco: Ripple to cross open ground"));
	}
}

// =================================================================================================
// CHUT — "Chud before taking a fight, refreshed on knife kills; the dash bash to displace."
// =================================================================================================

void FTraceBotAbilityBrain::PlanChut(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& /*Out*/)
{
	ATraceCharacter* BotPawn = Situation.Self;
	const UTraceAbilitySetChut* Chut = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetChut>();
	if (Chut == nullptr)
	{
		return;
	}

	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();

	// --- CHUD FIRST: "before taking a fight" -------------------------------------------------------
	//
	// ORDERED BEFORE THE BASH, and that ordering is a fix rather than a preference. Want() refuses a
	// second act while one is latched, so whichever block runs first owns the bot's attention — and
	// the bash is offered on every tick a dash charge exists, i.e. nearly always. MEASURED with the
	// blocks the other way round: Oyster's activated ability (same structure) fired 3 times in 150 s
	// because its approach-dash intent was permanently in the way. The scarce resource wins: E is a
	// 20 s cooldown, a dash charge is 4 s.
	//
	// The whole value of a 10 s damage reduction is that it is UP when the shooting starts, so the
	// trigger is an enemy about to be in a fight with this bot, not an enemy already shooting it. The
	// reaction latch then puts the press a beat before contact rather than on the frame the first
	// bullet lands, which is what "before taking a fight" reads as from outside.
	const bool bChudReady = !Chut->IsChudActive()
		&& Situation.Abilities->GetActivatedCooldownRemaining() <= 0.f;

	if (bChudReady)
	{
		float EnemyDist = 0.f;
		const ATraceCharacter* Contact = FindVisibleEnemy(Situation, Profile.PreferredCombatRange * 1.6f, EnemyDist);

		// GetHealthPercent(), not Health/MaxHealth: GetMaxHealth() is private, and the percent form is
		// the accessor the HUD already uses, so a bot's idea of "hurt" is the bar the player sees.
		const bool bHurt = (BotPawn->Health != nullptr) && BotPawn->Health->GetHealthPercent() < 0.6f;

		if (Contact != nullptr)
		{
			Want(Situation, ETraceBotAbilityAct::Activate,
				bHurt ? TEXT("Chut: Chud, already hurt and still in the fight")
				      : TEXT("Chut: Chud before taking a fight"));
		}
		else if (bHurt && IsUnderThreat(Situation))
		{
			Want(Situation, ETraceBotAbilityAct::Activate, TEXT("Chut: Chud under fire"));
		}
	}

	// --- THE BASH: "to displace" -------------------------------------------------------------------
	//
	// The bash itself is automatic — it happens when the END of a standard dash contacts somebody —
	// so "use the dash bash" means CHOOSING TO DASH AT A PERSON, which is a decision no bot made
	// before this pass (the dash was spent on the trace intercept, on escapes and on duels only).
	//
	// The victim is chosen through the §4 choke point with ETraceAbilityEffect::Control, which is the
	// exact class §6 says the bash is and the exact reason a carrier is exempt from it. So a Chut bot
	// will never spend a dash trying to knock the Core carrier around; it will go and cross their
	// trace instead, which is what actually takes the Core off them.
	//
	// NEVER while hunting a trace or chasing a loose Core. Those two states are what the dash charge
	// is FOR — crossing an enemy carrier's trace is the game's signature kill, and it is the one
	// mechanic this project has already had to rescue twice from bots that would not spend the
	// charge. An ability that quietly competed for the same charge would undo that work invisibly.
	const bool bDashIsSpokenFor = (Situation.BotState == ETraceBotState::HuntCarrier)
		|| (Situation.BotState == ETraceBotState::ChaseLooseCore);

	if (Situation.DashCharges <= 0 || Situation.Now < NextBashTime || Situation.bIAmCarrier || bDashIsSpokenFor)
	{
		return;
	}

	const float Reach = TraceBotAbilityBrainLocal::DashReachUU();
	const FVector MyLocation = BotPawn->GetActorLocation();

	const ATraceCharacter* BashTarget = nullptr;
	float BestDist = TNumericLimits<float>::Max();

	if (Situation.Enemies != nullptr)
	{
		for (const ATraceCharacter* Other : *Situation.Enemies)
		{
			if (Other == nullptr || !Other->IsAlive())
			{
				continue;
			}
			if (!Situation.Abilities->CanAffectTarget(Other, ETraceAbilityEffect::Control))
			{
				continue;   // the choke point's answer, including "that is the Core carrier"
			}

			FVector ToOther = Other->GetActorLocation() - MyLocation;
			ToOther.Z = 0.f;
			const float Dist = static_cast<float>(ToOther.Size());
			if (Dist > Reach || Dist >= BestDist)
			{
				continue;
			}

			// Roughly ahead: the dash locks its direction from the movement input, so a target
			// behind the bot cannot be reached by dashing this frame however close it is.
			if (FVector::DotProduct(ToOther.GetSafeNormal(), Situation.DesiredMoveDirection.GetSafeNormal()) < 0.5f)
			{
				continue;
			}

			BestDist = Dist;
			BashTarget = Other;
		}
	}

	if (BashTarget != nullptr)
	{
		const ETraceBotWantResult Result = Want(Situation, ETraceBotAbilityAct::Dash,
			TEXT("Chut: dash bash to displace"));
		if (Result == ETraceBotWantResult::Declined)
		{
			NextBashTime = Situation.Now + 2.f;   // hesitate; firing throttles itself on the charge
		}
	}
}

// =================================================================================================
// MACE — "Spike to reach height or close distance; V-suspend to hold an angle."
// =================================================================================================

void FTraceBotAbilityBrain::PlanMace(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out)
{
	ATraceCharacter* BotPawn = Situation.Self;
	UWorld* WorldPtr = BotPawn->GetWorld();
	UTraceAbilitySetMace* Mace = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetMace>();
	if (WorldPtr == nullptr || Mace == nullptr)
	{
		return;
	}

	const UTraceCharacterMovementComponent* Movement = BotPawn->GetTraceMovement();
	const bool bAirborne = (Movement != nullptr) && !Movement->IsMovingOnGround();

	// --- BEING PULLED: LET GO OF THE STICK --------------------------------------------------------
	//
	// §6: "Any movement input cancels the pull and removes the spike." A bot steers every frame, so
	// without this the pull is cancelled on the tick after it starts and Mace pays a 20 s cooldown
	// for a spike that travels, embeds, and is then deleted. MEASURED before this branch existed:
	// 13 pulls in a 150 s match, 13 "[Mace] Pull ended (movement input)" lines. The ability was being
	// counted as used and was doing nothing, which is the worst of both worlds.
	//
	// Standing still is the correct read of the rule rather than a workaround for it: a human using
	// the pull takes their hand off WASD, and the ride is the reward for doing so.
	if (Mace->IsPulling())
	{
		ClearIntent();
		Out.bSuppressMove = true;
		return;
	}

	// --- THE REACTIVATION ------------------------------------------------------------------------
	//
	// A spike that embeds and is never pulled on is half an ability. IsSpikeReadyToPull() is Mace's
	// own answer, and the press is routed through UTraceAbilityInputRelay::RouteActivatePressed,
	// which is where the "a second E means PULL" rule lives — the framework's TryActivate() cannot
	// carry it, because the throw has just started a 20 s cooldown. Highest priority: the embed
	// window is 2 s and a hesitation that outlasts it wastes the whole cast.
	//
	// SINCE SPEC v15 §6 THIS IS TRUE FROM THE MOMENT OF THE THROW, not from the embed: the predicate
	// now also covers a spike in flight, and a press made then is remembered and fires on the frame it
	// lands. That suits the intent latch below rather than fighting it — the reaction delay is rolled
	// during the ~0.2-0.3 s of flight instead of after it, so the bot arrives at the embed already
	// having decided, which is what a human who threw it on purpose looks like.
	if (Mace->IsSpikeReadyToPull())
	{
		Want(Situation, ETraceBotAbilityAct::Activate, TEXT("Mace: reactivate — pull to the spike"));
		return;
	}

	// --- V-SUSPEND: "to hold an angle" ------------------------------------------------------------
	//
	// Hold V in the air and gravity stops for up to 1.25 s. The play it buys is a stationary firing
	// platform at a height a running player cannot hold, so the trigger is: airborne, above the
	// ground, with somebody to shoot at. It is a HOLD, so the release is a timer rather than another
	// decision, and the timer is deliberately shorter than the ability's own cap — a bot that always
	// suspends for the full 1.25 s reads as a machine, and running out of suspend mid-air is worse
	// than choosing to stop.
	float AngleTargetDist = 0.f;
	const ATraceCharacter* AngleTarget = (Situation.Now >= NextSuspendTime && !Situation.bIAmCarrier)
		? FindVisibleEnemy(Situation, UTraceSettings::GetBotProfile().MaxEngagementRange, AngleTargetDist)
		: nullptr;

	if (SecondaryHoldUntilTime <= 0.f && AngleTarget != nullptr)
	{
		const float MaxHold = FMath::Max(0.2f, UTraceSettings::Get().MaceSuspendMaxSeconds);

		if (bAirborne)
		{
			if (FMath::FRand() < TraceBotAbilityBrainLocal::Willingness(PersonalityBias))
			{
				// The hold is SHORTER than the ability's own 1.25 s cap, and rolled. A bot that always
				// suspends for exactly the maximum reads as a machine, and running the meter out in
				// mid-air is strictly worse than choosing to come down.
				SecondaryHoldUntilTime = Situation.Now + FMath::FRandRange(MaxHold * 0.45f, MaxHold * 0.85f);
				NextSuspendTime = Situation.Now + MaxHold + 2.f;
			}
			else
			{
				NextSuspendTime = Situation.Now + 1.f;   // declined; offer again shortly
			}
		}
		else
		{
			// GET IN THE AIR FIRST. The suspend is only reachable while airborne, and a bot that waits
			// to be airborne for some other reason would essentially never use it: the movement kit
			// only leaves the ground for a slide-jump, an unstick jump or a jar jump, none of which
			// happen in a duel. So holding the angle starts with the jump that makes it possible —
			// which is an ordinary jump for Mace (nothing in her set claims the key), and therefore
			// costs nothing if the suspend is then declined.
			const ETraceBotWantResult Result = Want(Situation, ETraceBotAbilityAct::Jump,
				TEXT("Mace: jump to suspend and hold an angle"));
			if (Result == ETraceBotWantResult::Declined)
			{
				NextSuspendTime = Situation.Now + 1.5f;
			}
		}
	}

	// A suspend that is running owns the body. Throwing a spike out of it is legal and is also two
	// decisions at once, which is not what a person does.
	if (SecondaryHoldUntilTime > 0.f)
	{
		return;
	}

	// --- THE SPIKE: "to reach height or close distance" -------------------------------------------
	if (Situation.Abilities->GetActivatedCooldownRemaining() > 0.f)
	{
		bHasSpikeCandidate = false;
		return;
	}

	// Probe for a wall on a slow cadence: this is several line traces and the answer is about the
	// next few seconds, not about this frame.
	if (Situation.Now >= NextSpikeProbeTime)
	{
		NextSpikeProbeTime = Situation.Now + 0.6f;
		bHasSpikeCandidate = false;

		const float Range = FMath::Max(100.f, UTraceSettings::Get().MaceSpikeRangeUU);
		const float MaxWallNormalZ = FMath::Clamp(UTraceSettings::Get().WallJumpMaxNormalZ, 0.f, 1.f);

		FVector Forward = Situation.AttackGoal - BotPawn->GetActorLocation();
		Forward.Z = 0.f;
		Forward = Forward.GetSafeNormal();
		if (Forward.IsNearlyZero())
		{
			Forward = BotPawn->GetActorForwardVector();
		}

		// A short fan rather than one ray: the useful anchor is rarely dead ahead, and raising the
		// pitch is what turns "close distance" into "reach height". This only NOMINATES a point —
		// UTraceAbilitySetMace::ResolveSpikeAnchor re-runs the authoritative sweep from the bot's
		// actual aim when E is pressed, and a miss there is a free fizzle with no cooldown charged.
		static const float YawFan[] = { 0.f, -22.f, 22.f, -45.f, 45.f };
		static const float PitchFan[] = { 12.f, 26.f, 0.f };

		const FVector Start = BotPawn->GetMuzzleLocation();
		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
		FCollisionQueryParams QueryParams(TEXT("TraceBotSpikeProbe"), /*bTraceComplex=*/false, BotPawn);

		float BestScore = -1.f;
		for (float Pitch : PitchFan)
		{
			for (float Yaw : YawFan)
			{
				FRotator Dir = Forward.Rotation();
				Dir.Yaw += Yaw;
				Dir.Pitch += Pitch;

				FHitResult Hit;
				if (!WorldPtr->LineTraceSingleByObjectType(Hit, Start, Start + Dir.Vector() * Range, ObjectParams, QueryParams))
				{
					continue;
				}
				if (FMath::Abs(Hit.ImpactNormal.Z) > MaxWallNormalZ)
				{
					continue;   // a floor or a ramp. Mace's own sweep applies the same test.
				}

				// Prefer anchors that are FAR (the pull is a traversal) and HIGH (the pull is also how
				// she reaches ledges). Both are what §3 asks the ability to be used for.
				const float Reach = static_cast<float>(Hit.Distance);
				const float Rise = static_cast<float>(Hit.ImpactPoint.Z - Start.Z);
				const float Score = Reach + FMath::Max(0.f, Rise) * 1.5f;
				if (Score > BestScore)
				{
					BestScore = Score;
					SpikeCandidateAnchor = Hit.ImpactPoint;
					bHasSpikeCandidate = true;
				}
			}
		}
	}

	if (!bHasSpikeCandidate)
	{
		return;
	}

	// Worth doing? Closing distance only matters if there IS distance, and reaching height only
	// matters if the anchor is actually above her.
	const float ToAnchor = static_cast<float>(FVector::Dist(BotPawn->GetActorLocation(), SpikeCandidateAnchor));
	const float Rise = static_cast<float>(SpikeCandidateAnchor.Z - BotPawn->GetActorLocation().Z);
	const bool bReachesHeight = Rise > 250.f;
	const bool bClosesDistance = ToAnchor > FMath::Max(600.f, TraceBotAbilityBrainLocal::DashReachUU() * 1.5f);

	if (!bReachesHeight && !bClosesDistance)
	{
		return;
	}

	const FVector AimAt = ApplyAbilityAimError(Situation, SpikeCandidateAnchor);
	Want(Situation, ETraceBotAbilityAct::Activate,
		bReachesHeight ? TEXT("Mace: Spike to reach height") : TEXT("Mace: Spike to close distance"),
		&AimAt);
}

// =================================================================================================
// OYSTER — "jars on the approach; Pickler to zone a choke or peel a carrier's escort."
// =================================================================================================

/**
 * The ballistic solve for Oyster's Pickler lob.
 *
 * The jar leaves at AimDir * Speed + up * Speed * UpBias and then falls under plain world gravity
 * (ATraceOysterJar integrates GetGravityZ directly), so the HORIZONTAL speed depends on the pitch
 * and the familiar first-order trick — "aim at the target, then raise the aim point by the drop" —
 * has no fixed point at all. A coarse scan over pitch does have one, and it is cheap enough to run
 * on the probe cadence: the damage radius is 420 uu, so a couple of degrees of slop still puts the
 * jar in the same doorway.
 *
 * Takes the LOW arc (the first sign change scanning upward): less time in the air is less time for
 * the people being zoned to walk out of it.
 *
 * @return false when no pitch in the scanned range reaches the point — a genuinely out-of-range lob,
 *         which must not be attempted rather than thrown short.
 */
bool FTraceBotAbilityBrain::SolvePicklerLob(const FTraceBotAbilitySituation& Situation,
                                            const FVector& TargetPoint, FVector& OutAimPoint)
{
	const ATraceCharacter* BotPawn = Situation.Self;
	const UWorld* WorldPtr = (BotPawn != nullptr) ? BotPawn->GetWorld() : nullptr;
	if (BotPawn == nullptr || WorldPtr == nullptr)
	{
		return false;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Speed = FMath::Max(1.f, Settings.OysterPicklerThrowSpeed);
	const float UpBias = FMath::Max(0.f, Settings.OysterPicklerThrowUpBias);
	const float Gravity = WorldPtr->GetGravityZ();

	const FVector Start = BotPawn->GetMuzzleLocation();
	FVector Flat = TargetPoint - Start;
	const float TargetZ = static_cast<float>(Flat.Z);
	Flat.Z = 0.f;
	const float Horizontal = static_cast<float>(Flat.Size());
	if (Horizontal < 1.f)
	{
		return false;
	}

	float PreviousError = 0.f;
	for (int32 Step = 0; Step <= 44; ++Step)
	{
		const float PitchDeg = -20.f + static_cast<float>(Step) * 2.f;   // -20 .. +68
		const float Pitch = FMath::DegreesToRadians(PitchDeg);
		const float HorizontalSpeed = Speed * FMath::Cos(Pitch);
		if (HorizontalSpeed < 1.f)
		{
			continue;
		}

		const float FlightTime = Horizontal / HorizontalSpeed;
		const float ArriveZ = (Speed * FMath::Sin(Pitch) + Speed * UpBias) * FlightTime
			+ 0.5f * Gravity * FlightTime * FlightTime;
		const float Error = ArriveZ - TargetZ;

		if (Step > 0 && ((Error >= 0.f) != (PreviousError >= 0.f)))
		{
			FRotator LaunchRotation = Flat.Rotation();
			LaunchRotation.Pitch = PitchDeg - 1.f;
			OutAimPoint = ApplyAbilityAimError(Situation,
				Start + LaunchRotation.Vector() * FMath::Max(300.f, Horizontal));
			return true;
		}
		PreviousError = Error;
	}

	return false;
}

void FTraceBotAbilityBrain::PlanOyster(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& /*Out*/)
{
	ATraceCharacter* BotPawn = Situation.Self;
	const UTraceAbilitySetOyster* Oyster = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetOyster>();
	if (Oyster == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// --- PICKLER FIRST: "to zone a choke or peel a carrier's escort" --------------------------------
	//
	// Ordered ahead of the approach dash for the reason spelled out in PlanChut: Want() refuses a
	// second act while one is latched, and the dash is offered on nearly every tick, so with the
	// blocks the other way round the Pickler fired 3 times in a 150 s match. E is the scarce resource.
	if (Situation.Abilities->GetActivatedCooldownRemaining() <= 0.f)
	{
		if (Situation.Now >= NextPicklerProbeTime)
		{
			NextPicklerProbeTime = Situation.Now + 0.4f;
			bHasPicklerAim = false;

			const float BlastRadius = FMath::Max(100.f, Settings.OysterPicklerDamageRadiusUU);

			// Two aiming rules, in the spec's own order of preference.
			//
			//  1. PEEL A CARRIER'S ESCORT. An enemy carrier is untouchable (the choke point refuses
			//     both damage and control on them), but the players AROUND them are not, and
			//     stripping the escort is what lets this team's interceptors get to the trace. So the
			//     jar is aimed at the escort cluster, never at the carrier — and "never at the
			//     carrier" is not a test this file makes, it is what CanAffectTarget returns inside
			//     CountAffectableEnemiesNear.
			//  2. ZONE A CHOKE. Otherwise, at the biggest reachable knot of enemies.
			FVector BestPoint = FVector::ZeroVector;
			int32 BestCount = 0;
			const TCHAR* BestWhy = nullptr;

			if (Situation.bEnemyIsCarrier && Situation.Carrier != nullptr)
			{
				FVector Centroid = FVector::ZeroVector;
				const int32 Escorts = CountAffectableEnemiesNear(Situation, Situation.Carrier->GetActorLocation(),
					BlastRadius * 2.f, ETraceAbilityEffect::Damage, Centroid);
				if (Escorts >= 1)
				{
					BestPoint = Centroid;
					BestCount = Escorts;
					BestWhy = TEXT("Oyster: Pickler to peel the carrier's escort");
				}
			}

			if (BestCount == 0 && Situation.Enemies != nullptr)
			{
				for (const ATraceCharacter* Other : *Situation.Enemies)
				{
					if (Other == nullptr || !Other->IsAlive())
					{
						continue;
					}
					FVector Centroid = FVector::ZeroVector;
					const int32 Count = CountAffectableEnemiesNear(Situation, Other->GetActorLocation(),
						BlastRadius, ETraceAbilityEffect::Damage, Centroid);
					if (Count > BestCount)
					{
						BestCount = Count;
						BestPoint = Centroid;
						BestWhy = (Count >= 2) ? TEXT("Oyster: Pickler to zone a choke")
						                       : TEXT("Oyster: Pickler onto a contested lane");
					}
				}
			}

			if (BestCount > 0 && Situation.BotController != nullptr)
			{
				// A single target is not worth a 20 s cooldown unless the jar will actually land on
				// them; two or more always is. That asymmetry is the difference between "zoning" and
				// "throwing a jar at a man across the arena".
				const float Dist = static_cast<float>(FVector::Dist(BotPawn->GetActorLocation(), BestPoint));
				const bool bWorthIt = (BestCount >= 2) ? (Dist < 3000.f) : (Dist < 1600.f);

				if (bWorthIt && SolvePicklerLob(Situation, BestPoint, PicklerAimPoint))
				{
					bHasPicklerAim = true;
					Want(Situation, ETraceBotAbilityAct::Activate,
						(BestWhy != nullptr) ? BestWhy : TEXT("Oyster: Pickler"), &PicklerAimPoint);
				}
			}
		}
		else if (bHasPicklerAim)
		{
			// Between probes, keep the intent alive so the reaction latch is not reset by the cadence.
			Want(Situation, ETraceBotAbilityAct::Activate, TEXT("Oyster: Pickler"), &PicklerAimPoint);
		}
	}
	else
	{
		bHasPicklerAim = false;
	}

	// --- "JARS ON THE APPROACH" ---------------------------------------------------------------------
	//
	// The jar is a PASSIVE: one drops at the start of every dash. So "jars on the approach" is not a
	// new ability call at all, it is a reason to spend a dash that no bot had before — the dash was
	// previously reserved for the trace intercept, escapes and duels, none of which fire while a bot
	// is walking up to a contested area. This is the whole difference between an Oyster bot that
	// leaves a trail of poison behind it and one whose passive never once fires in a match.
	const bool bDashIsSpokenFor = (Situation.BotState == ETraceBotState::HuntCarrier)
		|| (Situation.BotState == ETraceBotState::ChaseLooseCore);

	if (Situation.DashCharges <= 0 || Situation.Now < NextApproachDashTime || bDashIsSpokenFor)
	{
		return;
	}

	float ContactDist = 0.f;
	const bool bApproachingContact =
		FindVisibleEnemy(Situation, UTraceSettings::GetBotProfile().MaxEngagementRange, ContactDist) != nullptr
		&& ContactDist > TraceBotAbilityBrainLocal::DashReachUU();

	// Escorting a carrier is the other place a jar earns its keep: it goes down on the ground the
	// chasers have to run over.
	const bool bScreening = (Situation.BotState == ETraceBotState::EscortCarrier);

	if ((bApproachingContact || bScreening) && !Situation.DesiredMoveDirection.IsNearlyZero())
	{
		const ETraceBotWantResult Result = Want(Situation, ETraceBotAbilityAct::Dash,
			TEXT("Oyster: dash on the approach to lay a jar"));
		if (Result == ETraceBotWantResult::Declined)
		{
			NextApproachDashTime = Situation.Now + 2.5f;
		}
	}
}

// =================================================================================================
// X — "Sting before a duel, not into an empty corridor."
// =================================================================================================

void FTraceBotAbilityBrain::PlanX(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& /*Out*/)
{
	const UTraceAbilitySetX* X = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetX>();
	if (X == nullptr)
	{
		return;
	}

	// Already loaded: the framework's own CanActivate refuses this, and pressing anyway would burn a
	// press for nothing. Asked here so the intent latch is never occupied by a doomed decision.
	if (X->IsStingLoaded() || Situation.Abilities->GetActivatedCooldownRemaining() > 0.f)
	{
		return;
	}

	// "NOT INTO AN EMPTY CORRIDOR" is the whole rule, and it is a rule about there being somebody to
	// sting. Sting loads five bullets, so it is worth pressing only when the next five bullets are
	// actually going somewhere: a living enemy, in range, in sight, that the choke point would let
	// the mark land on. Asking CanAffectTarget with Control is what makes a lone Core carrier — who
	// cannot be marked at all — not count as a duel worth loading for.
	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();

	float Dist = 0.f;
	const ATraceCharacter* Duel = FindVisibleEnemy(Situation, Profile.MaxEngagementRange, Dist);
	if (Duel == nullptr || !Situation.Abilities->CanAffectTarget(Duel, ETraceAbilityEffect::Control))
	{
		return;
	}

	// A duel, not a sighting. Either this bot has already picked them as its target, or they are
	// close enough that a fight is about to happen whether it picked them or not — 1.5 x the profile's
	// preferred range, because Sting has to be loaded BEFORE the shooting starts to be worth anything,
	// and PreferredCombatRange is where the shooting already is.
	//
	// It is still the "not into an empty corridor" test: at 4800 uu of sight range this refuses about
	// half of what the bot can see, and it refuses all of it when the only thing visible is a Core
	// carrier (whom the choke point above has already ruled out).
	const bool bCommitted = (Situation.ShootTarget == Duel)
		|| (Dist < Profile.PreferredCombatRange * 1.5f);
	if (!bCommitted)
	{
		return;
	}

	Want(Situation, ETraceBotAbilityAct::Activate, TEXT("X: Sting before a duel"));
}

// =================================================================================================
// ROXIE (spec v18 §3) — "rocket for the displacement as much as the damage; Modded before a fight."
// =================================================================================================
//
// THE ORDER OF THE TWO BLOCKS BELOW IS A DECISION, and it is the opposite of Chut's and Oyster's.
//
// Those two put E first because E was the scarce resource and the dash was offered on nearly every
// tick. Roxie has no dash ability at all: her two cooldowns are 35 s (the rocket, on V) and 25 s
// (MODDED, on E), and it is the ROCKET whose opportunity is narrow — it needs somebody roughly in
// front of her AND clear ground behind her to be thrown into. MODDED's trigger is "a fight is about
// to happen", which is true for whole seconds at a time. So the narrow window goes first; if it were
// second, the intent latch would be holding a MODDED press through every one of them.
// =================================================================================================

void FTraceBotAbilityBrain::PlanRoxie(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& /*Out*/)
{
	ATraceCharacter* BotPawn = Situation.Self;
	UWorld* WorldPtr = BotPawn->GetWorld();
	const UTraceAbilitySetRoxie* Roxie = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetRoxie>();
	if (WorldPtr == nullptr || Roxie == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
	const FVector MyLocation = BotPawn->GetActorLocation();

	// --- THE ROCKET: "for the displacement as much as the damage" ---------------------------------
	if (Roxie->IsRocketReady() && Situation.Now >= NextRocketTime)
	{
		// How far the thing actually flies, from the two knobs that decide it. Clamped to what the bot
		// can see anyway: a rocket at a man it has no line of sight to is a rocket at a wall.
		const float RocketReach = FMath::Min(
			FMath::Max(400.f, Settings.RoxieRocketSpeed * Settings.RoxieRocketLifetimeSeconds),
			Profile.MaxEngagementRange);

		float TargetDist = 0.f;
		ATraceCharacter* Victim = FindVisibleEnemy(Situation, RocketReach, TargetDist);

		// THE §4 CHOKE POINT, ASKED RATHER THAN RE-DERIVED — and for Roxie this matters more than for
		// anybody else on the roster, because her rocket is the flat 100 that spec v18 §2 calls "the
		// most dangerous addition yet". The rule itself lives in ATraceRoxieRocket ->
		// UTraceAbilitySetRoxie::ApplyRocketDamageTo -> UTraceCharacterAbilitySet::DealDamage ->
		// UTraceAbilityComponent::CanAffectTarget, and a carrier is safe from this bot whatever this
		// function decides. What asking HERE buys is that she does not spend a 35 s cooldown on a
		// target the rules will make the rocket fly straight through.
		//
		// It DOES cost her one situation, and it is the right trade: a bot Roxie whose only visible
		// enemy is the Core carrier will not rocket at all, even though the recoil alone would still
		// carry her out of trouble. 35 s is a lot to spend on a shot that is guaranteed to deal zero,
		// and the nearest visible enemy behind her is almost never the carrier — a carrier is being
		// chased, not chasing.
		if (Victim != nullptr && !Situation.Abilities->CanAffectTarget(Victim, ETraceAbilityEffect::Damage))
		{
			Victim = nullptr;
		}

		if (Victim != nullptr)
		{
			FVector ToVictim = Victim->GetActorLocation() - MyLocation;
			ToVictim.Z = 0.f;
			const FVector AimDirection = ToVictim.GetSafeNormal();

			// IS THERE ANYWHERE TO GO? The rocket's own words in §2 are "launches her backwards, fast
			// and far", so firing it with a wall at her shoulder blades spends the whole cooldown on a
			// bump and a bang. One trace, against the same visibility channel the bot's sight uses.
			bool bRoomBehind = true;
			const float RoomWanted = FMath::Max(0.f,
				TraceBotAbilityBrainLocal::CVarBotRoxieRocketRoomBehind.GetValueOnAnyThread());
			if (RoomWanted > 0.f && !AimDirection.IsNearlyZero())
			{
				const FVector Behind = MyLocation - AimDirection * RoomWanted;
				FCollisionQueryParams Params(TEXT("TraceBotRoxieBackswing"), /*bTraceComplex=*/false, BotPawn);
				FHitResult Hit;
				bRoomBehind = !WorldPtr->LineTraceSingleByChannel(Hit, MyLocation, Behind, ECC_Visibility, Params);
			}

			// A CARRIER MAY FIRE IT, BUT ONLY DOWN THE LANE SHE CAME FROM.
			//
			// Every other movement ability in this file refuses outright while carrying (Rocco's
			// redirect, Mace's suspend), and for them that is right: a carrier who hops or hangs has
			// simply stopped running. The rocket is different in kind, because the displacement has a
			// SIGN — she is thrown away from whatever she shoots. Fired at the chasers behind her, the
			// recoil throws her at the endzone, which is the best use of the ability on the whole
			// roster. Fired at somebody ahead, it throws her back down the pitch and undoes the run.
			// So the carrier keeps it, with the direction test that tells the two apart.
			bool bAllowedWhileCarrying = true;
			if (Situation.bIAmCarrier)
			{
				FVector ToGoal = Situation.AttackGoal - MyLocation;
				ToGoal.Z = 0.f;
				const FVector GoalDirection = ToGoal.GetSafeNormal();
				bAllowedWhileCarrying = !GoalDirection.IsNearlyZero()
					&& FVector::DotProduct(AimDirection, GoalDirection) < -0.5f;
			}

			if (bRoomBehind && bAllowedWhileCarrying)
			{
				// Two reasons, and the log tells them apart because they are genuinely different plays.
				// "Hurt and being shot at" is the escape — she is buying distance and the 100 damage is
				// the bonus. Anything else is the shot, and being thrown backwards is the cost she pays
				// for taking it.
				const bool bEscaping = (BotPawn->Health != nullptr) && BotPawn->Health->GetHealthPercent() < 0.55f;

				const FVector AimAt = ApplyAbilityAimError(Situation,
					Victim->GetActorLocation() + FVector(0.f, 0.f, 40.f));

				const ETraceBotWantResult Result = Want(Situation, ETraceBotAbilityAct::Secondary,
					Situation.bIAmCarrier ? TEXT("Roxie: rocket back down the lane, ride the recoil to the endzone")
					: bEscaping           ? TEXT("Roxie: rocket to blow herself out of a losing fight")
					                      : TEXT("Roxie: rocket — 100 on impact and a long way backwards"),
					&AimAt);

				if (Result == ETraceBotWantResult::Declined)
				{
					NextRocketTime = Situation.Now + 1.5f;
				}
			}
		}
	}

	// --- MODDED: "before a fight" ------------------------------------------------------------------
	//
	// Chut's Chud, verbatim in shape, and deliberately so: both are a several-second buff whose entire
	// value is that it is ALREADY UP when the shooting starts, so both trigger on an enemy who is
	// about to be in a fight with this bot rather than on one who is already shooting it. The reaction
	// latch then puts the press a beat before contact.
	//
	// MODDED is 5 seconds and "one clip", though, which is much shorter than Chud's 10 — so the range
	// test is tighter (1.25x preferred combat range, against Chut's 1.6x). Pressed at 1.6x she would
	// spend half of it walking.
	if (Roxie->IsModdedActive() || Situation.Abilities->GetActivatedCooldownRemaining() > 0.f)
	{
		return;
	}

	float ContactDist = 0.f;
	const ATraceCharacter* Contact = FindVisibleEnemy(Situation, Profile.PreferredCombatRange * 1.25f, ContactDist);
	if (Contact != nullptr)
	{
		Want(Situation, ETraceBotAbilityAct::Activate, TEXT("Roxie: MODDED before a fight"));
	}
}

// =================================================================================================
// ELLE (spec v18 §3) — "Snap to make a rotation; cloak-escape after passing."
// =================================================================================================

void FTraceBotAbilityBrain::PlanElle(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out)
{
	ATraceCharacter* BotPawn = Situation.Self;
	const UTraceAbilitySetElle* Elle = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetElle>();
	if (Elle == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const FVector MyLocation = BotPawn->GetActorLocation();

	// --- THE CLOAK ESCAPE: "right after passing or throwing the trace" -----------------------------
	//
	// The cloak is a PASSIVE — nothing presses it, it simply arrives the moment she gives the Core
	// away — so "use it" cannot mean pressing a key. It means doing the thing the three seconds are
	// FOR: leaving. A bot that passes and then stands in the same doorway has been handed the best
	// disengage on the roster and used it to stand still.
	//
	// This is steering and not a key press, so it is not routed through Want(). It is still gated: the
	// escape does not begin until CloakEscapeStartTime, one rolled reaction delay after the cloak went
	// up (see ObserveNewCharacterOutcomes), because vanishing and sprinting away on the same frame is
	// the frame-one behaviour spec v15 §3 names first.
	if (Elle->IsCloaked() && bCloakEscapeAllowed && Situation.Now >= CloakEscapeStartTime
		&& !Situation.bIAmCarrier)
	{
		float ThreatDist = 0.f;
		const ATraceCharacter* Threat = FindVisibleEnemy(Situation,
			UTraceSettings::GetBotProfile().MaxEngagementRange, ThreatDist);

		if (Threat != nullptr)
		{
			FVector Away = MyLocation - Threat->GetActorLocation();
			Away.Z = 0.f;
			Away = Away.GetSafeNormal();
			if (!Away.IsNearlyZero())
			{
				// Away and ACROSS, not straight back. Running directly away from a shooter is the one
				// direction their crosshair does not have to move at all — the same argument Rocco's
				// dodge makes, and it matters more here because the cloak is her only protection.
				const float Sign = (PersonalityBias < 0.5f) ? -1.f : 1.f;
				const FVector Lateral = FVector::CrossProduct(FVector::UpVector, Away) * Sign;

				Out.bOverrideMove = true;
				Out.MoveDirection = (Away * 0.75f + Lateral * 0.65f).GetSafeNormal();
				Out.Reason = TEXT("Elle: cloaked after the pass — break away while they cannot see her");

				// Counted ONCE per cloak, on the first frame the steering is actually taken over. The
				// escape is the only thing in this pass that presses no key, so this counter is the
				// only thing that can tell a build where it runs from one where it never gets here.
				if (!bCloakEscapeCounted)
				{
					bCloakEscapeCounted = true;
					TraceBotAbilityStats::NoteCloakEscape();
				}
			}
		}
	}

	// --- SNAP, PRESS 2: the far end of the rotation, and it has a HARD DEADLINE --------------------
	//
	// This block is first among the key presses and it is not a preference. §2: "If no second gate
	// inside 4 s the first expires", and Elle's own file charges the FULL 35 s cooldown when that
	// happens. A fluffed cast is therefore the single most expensive mistake this character can make,
	// so nothing else is allowed to occupy the intent latch while the window is open.
	if (Elle->IsAwaitingSecondGate())
	{
		if (!bSnapFirstGatePlaced)
		{
			// The window is open but this brain did not open it — she was mid-cast when the bot took
			// the pawn over, or the first gate went down before the character change. Complete it
			// anyway rather than letting it lapse; a pair somewhere beats 35 s for nothing.
			SnapFirstGateLocation = MyLocation;
			SnapFirstPressTime = Situation.Now;
			bSnapFirstGatePlaced = true;
		}

		const float Window = FMath::Max(0.1f, Settings.ElleSnapSecondGateWindowSeconds);
		const float WindowLeft = (SnapFirstPressTime + Window) - Situation.Now;
		const float Span = static_cast<float>(FVector::Dist2D(MyLocation, SnapFirstGateLocation));
		const float WantedSpan = FMath::Max(0.f,
			TraceBotAbilityBrainLocal::CVarBotSnapRotationSpan.GetValueOnAnyThread());

		// 2.5 s, and the number is derived rather than picked: the longest reaction this pass can roll
		// is Easy's 0.70 s x 1.35 (personality) x 1.35 (Trace.Bot.AbilityLatencyScale) x 1.45 (jitter)
		// = 1.85 s, so a decision taken with 2.5 s left still lands inside a 4 s window on the slowest
		// bot in the game. Below that she stops holding out for distance and plants it where she is.
		const bool bFarEnough = (Span >= WantedSpan);
		const bool bOutOfTime = (WindowLeft <= 2.5f);

		if (bFarEnough || bOutOfTime)
		{
			// No hesitation on a decline, unlike every other caller in this file. A decline here is not
			// "maybe later", it is the 4 s window ticking away — so the offer is re-made every tick and
			// the willingness roll gets as many chances as the window has frames.
			Want(Situation, ETraceBotAbilityAct::Activate,
				bFarEnough ? TEXT("Elle: second gate — the rotation is long enough to be worth it")
				           : TEXT("Elle: second gate before the window lapses"));
		}
		return;
	}

	bSnapFirstGatePlaced = false;

	// --- SNAP, PRESS 1: "to make a rotation" -------------------------------------------------------
	//
	// A rotation is a long move between two places, so the trigger is the same shape as Rocco's "cross
	// open ground": a long way still to go, and the bot actually running it. Placed in a fight it is
	// not a rotation, it is a portal in a doorway two people are standing in — so a visible enemy
	// close by cancels it, which also keeps her from casting instead of shooting.
	// BOTH cooldowns are asked, and they are not the same number. Elle's own bookkeeping is the one the
	// ability enforces (her file explains the one branch where the two disagree, after a fluffed cast),
	// while the framework's is the one UTraceAbilityComponent::TryActivate tests before it asks her
	// anything. A press that either would refuse is a press the intent latch should never have been
	// occupied by.
	FText Refusal;
	if (Situation.Now < NextSnapTime
		|| Situation.Abilities->GetActivatedCooldownRemaining() > 0.f
		|| !Elle->CanActivate(Refusal))
	{
		return;
	}

	FVector Heading = Situation.DesiredMoveDirection;
	Heading.Z = 0.f;
	Heading = Heading.GetSafeNormal();
	if (Heading.IsNearlyZero())
	{
		return;   // she is not going anywhere, so there is no rotation to open
	}

	const float ToObjective = static_cast<float>(FVector::Dist2D(MyLocation, Situation.AttackGoal));
	const float WantedSpan = FMath::Max(200.f,
		TraceBotAbilityBrainLocal::CVarBotSnapRotationSpan.GetValueOnAnyThread());

	float NearestEnemyDist = 0.f;
	const bool bEnemyClose = FindVisibleEnemy(Situation, 1200.f, NearestEnemyDist) != nullptr;

	// Twice the span she is going to try to open, so that the run she is about to make is long enough
	// to contain the whole cast with distance to spare.
	if (ToObjective < WantedSpan * 2.f || bEnemyClose)
	{
		return;
	}

	// Escorting the carrier is the rotation worth having: the mouth goes down on the ground the
	// carrier is about to leave, and the far one opens where the team needs to arrive.
	const bool bWithCarrier = Situation.bTeammateIsCarrier && Situation.Carrier != nullptr
		&& FVector::Dist2D(MyLocation, Situation.Carrier->GetActorLocation()) < 2500.f;

	const ETraceBotWantResult Result = Want(Situation, ETraceBotAbilityAct::Activate,
		bWithCarrier ? TEXT("Elle: SNAP — open a rotation for the carrier's team")
		             : TEXT("Elle: SNAP — open a rotation across open ground"));

	if (Result == ETraceBotWantResult::Declined)
	{
		NextSnapTime = Situation.Now + 2.f;
	}
}

// =================================================================================================
// SLIMEBALL (spec v18 §3) — "stick to a wall to hold an angle; wall off a choke."
// =================================================================================================

void FTraceBotAbilityBrain::PlanSlimeball(const FTraceBotAbilitySituation& Situation, FTraceBotAbilityOrders& Out)
{
	ATraceCharacter* BotPawn = Situation.Self;
	UWorld* WorldPtr = BotPawn->GetWorld();
	const UTraceAbilitySetSlimeball* Slime = Situation.Abilities->GetAbilitySetAs<UTraceAbilitySetSlimeball>();
	if (WorldPtr == nullptr || Slime == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const FTraceBotProfile& Profile = UTraceSettings::GetBotProfile();
	const FVector MyLocation = BotPawn->GetActorLocation();

	// --- ALREADY ON THE WALL: hold on, and SHOOT ---------------------------------------------------
	//
	// Mace's suspend takes the same early return and for the same reason. Being stuck IS the play —
	// +30% fire rate and 30% off body shots, from an angle a running player cannot hold — and throwing
	// a wall out of it would be two decisions at once, which is not a thing a person does. The V level
	// is already asserted; the hold's own deadline brings him down.
	if (Slime->IsStuck())
	{
		return;
	}

	// LEAN TOWARD THE WALL FOR THE MOMENT AFTER TAKE-OFF.
	//
	// His own ProbeForWall measures 90 uu from the capsule CENTRE, so "beside a wall" and "in reach of
	// it" are not the same place and a jump straight up beside one grabs nothing. The window is set on
	// the tick the jump fires (see Plan()) and this is what spends it.
	//
	// Asserted here, before the E block, and it survives Resolve(): the only intent that could
	// overwrite bOverrideMove is one that was latched WITH a move direction, and the Slimewall below
	// latches with an aim point and no move.
	if (Situation.Now < StickApproachUntilTime && bHasStickWall)
	{
		FVector ToWall = StickWallPoint - MyLocation;
		ToWall.Z = 0.f;
		if (!ToWall.IsNearlyZero())
		{
			Out.bOverrideMove = true;
			Out.MoveDirection = ToWall.GetSafeNormal();
			Out.Reason = TEXT("Slimeball: into the wall");
		}
	}

	// --- THE SLIMEWALL: "wall off a choke" ---------------------------------------------------------
	//
	// E first, for the reason PlanChut and PlanOyster both spell out: Want() refuses a second act while
	// one is latched, and the stick is offered on nearly every tick a wall is nearby, so with the
	// blocks the other way round the 25 s ability would go unused for whole matches.
	//
	// The aim point is where the wall GOES, near enough: it is thrown 400 uu along his planar aim, so
	// aiming at a knot of enemies drops it between him and them. Aim error is applied like every other
	// aimed ability here — a bot that sprays with a rifle does not place a wall to the millimetre.
	//
	// NO PRE-CHECK AGAINST ResolveSlimewallPlacement(), deliberately. That query reads his CURRENT aim,
	// and during the reaction delay his aim is still slewing toward the point below — so a pre-check
	// would be answering a question about the wrong direction, and the honest failure mode is already
	// free: ActivateAbility returns false when there is nowhere to put it and charges no cooldown.
	if (Slime->GetActiveWall() == nullptr && Situation.Abilities->GetActivatedCooldownRemaining() <= 0.f)
	{
		const float SlowRadius = FMath::Max(200.f, Settings.SlimewallLengthUU * 0.5f);

		FVector WallAt = FVector::ZeroVector;
		const TCHAR* WallWhy = nullptr;

		// 1. A KNOT OF ENEMIES. The 35% slow is Control and goes through the choke point inside
		//    ATraceSlimewall, so CountAffectableEnemiesNear is asked with the same effect class the
		//    wall will use: a lone enemy carrier, whom the wall may not slow, is not a reason to spend
		//    the cooldown on the slow. (He may still get one thrown at him by rule 3 below, and should
		//    — blocking sight is not something an ability does TO a person, so the carrier rule has
		//    nothing to say about it.)
		if (Situation.Enemies != nullptr)
		{
			int32 BestCount = 0;
			for (const ATraceCharacter* Other : *Situation.Enemies)
			{
				if (Other == nullptr || !Other->IsAlive())
				{
					continue;
				}
				if (FVector::Dist2D(MyLocation, Other->GetActorLocation()) > 2200.f)
				{
					continue;
				}

				FVector Centroid = FVector::ZeroVector;
				const int32 Count = CountAffectableEnemiesNear(Situation, Other->GetActorLocation(),
					SlowRadius, ETraceAbilityEffect::Control, Centroid);
				if (Count >= 2 && Count > BestCount)
				{
					BestCount = Count;
					WallAt = Centroid;
					WallWhy = TEXT("Slimeball: Slimewall across a choke they are pushing through");
				}
			}
		}

		// 2. SCREEN THE CORE. Carrying it, or running with whoever is: the wall goes up between the
		//    Core and the nearest chaser, which is what the 35% slow and the blocked sight are both
		//    for.
		if (WallWhy == nullptr && (Situation.bIAmCarrier || Situation.BotState == ETraceBotState::EscortCarrier))
		{
			float ChaserDist = 0.f;
			const ATraceCharacter* Chaser = FindVisibleEnemy(Situation, 1800.f, ChaserDist);
			if (Chaser != nullptr)
			{
				WallAt = Chaser->GetActorLocation();
				WallWhy = Situation.bIAmCarrier
					? TEXT("Slimeball: Slimewall behind him to screen his own run")
					: TEXT("Slimeball: Slimewall to screen the carrier");
			}
		}

		// 3. BREAK THE LINE OF SIGHT HE IS LOSING TO. Hurt, in somebody's crosshair, and a wall is the
		//    only thing in his kit that ends the exchange without him having to win it.
		if (WallWhy == nullptr && BotPawn->Health != nullptr && BotPawn->Health->GetHealthPercent() < 0.5f)
		{
			float ShooterDist = 0.f;
			const ATraceCharacter* Shooter = FindVisibleEnemy(Situation, Profile.MaxEngagementRange, ShooterDist);
			if (Shooter != nullptr)
			{
				WallAt = Shooter->GetActorLocation();
				WallWhy = TEXT("Slimeball: Slimewall to break the sightline while he is hurt");
			}
		}

		if (WallWhy != nullptr)
		{
			const FVector AimAt = ApplyAbilityAimError(Situation, WallAt + FVector(0.f, 0.f, 40.f));
			Want(Situation, ETraceBotAbilityAct::Activate, WallWhy, &AimAt);
		}
	}

	// --- THE WALL STICK: "to hold an angle" --------------------------------------------------------
	//
	// The whole ability is unreachable from the ground: UTraceAbilitySetSlimeball::OnSecondaryPressed
	// requires IsFalling(), and the bot movement kit only leaves the ground for a slide-jump, an
	// unstick jump or a jar jump — none of which happen in a duel. This is the same hole Rocco's
	// redirect and Mace's suspend both fell into and it has the same answer: the decision that starts
	// the ability is A JUMP, which costs Slimeball nothing (his set claims no jump hook) and buys the
	// airtime the stick lives in. The V level goes down with it, in Plan().
	if (Situation.bIAmCarrier || Situation.Now < NextStickTime)
	{
		return;   // a carrier who welds himself to a wall has stopped running the Core in
	}

	const UTraceCharacterMovementComponent* Movement = BotPawn->GetTraceMovement();
	if (Movement == nullptr || !Movement->IsMovingOnGround())
	{
		return;   // already airborne: this is the jump's decision and the jump has not been made here
	}

	// Somebody to hold the angle ON. Sticking to a wall in an empty corridor is the "press it off
	// cooldown" behaviour spec v15 §3 refuses, and it would pin him in the open for nothing.
	float TargetDist = 0.f;
	if (FindVisibleEnemy(Situation, Profile.MaxEngagementRange, TargetDist) == nullptr)
	{
		return;
	}

	// Probe for a wall on a slow cadence — eight traces, and the answer is about the next few seconds.
	if (Situation.Now >= NextStickProbeTime)
	{
		NextStickProbeTime = Situation.Now + 0.5f;
		bHasStickWall = false;

		const float Search = FMath::Max(60.f,
			TraceBotAbilityBrainLocal::CVarBotSlimeballWallSearch.GetValueOnAnyThread());
		const float MaxNormalZ = FMath::Clamp(Settings.SlimeballWallStickMaxSurfaceNormalZ, 0.f, 1.f);

		// BY OBJECT TYPE, not by channel, for the reason his own ProbeForWall records: the stock Pawn
		// profile BLOCKS the WorldStatic channel, so a channel trace would report a team-mate standing
		// beside him as a wall and he would jump at a person.
		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
		FCollisionQueryParams QueryParams(TEXT("TraceBotSlimeWallProbe"), /*bTraceComplex=*/false, BotPawn);

		float BestDistSq = TNumericLimits<float>::Max();
		for (int32 Step = 0; Step < 8; ++Step)
		{
			const FRotator Spoke(0.f, static_cast<float>(Step) * 45.f, 0.f);
			const FVector Direction = Spoke.Vector();

			FHitResult Hit;
			if (!WorldPtr->LineTraceSingleByObjectType(Hit, MyLocation, MyLocation + Direction * Search,
				ObjectParams, QueryParams))
			{
				continue;
			}
			if (FMath::Abs(Hit.ImpactNormal.Z) > MaxNormalZ)
			{
				continue;   // a floor or a ramp. His own sweep applies exactly this test.
			}

			// NEAREST wins. A wall he can be against by the top of the jump is the only kind that is
			// any use — his own probe reaches 90 uu from the capsule centre and no further.
			const float DistSq = static_cast<float>(FVector::DistSquared(MyLocation, Hit.ImpactPoint));
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				StickWallPoint = Hit.ImpactPoint;
				bHasStickWall = true;
			}
		}
	}

	if (!bHasStickWall)
	{
		return;
	}

	// The steering is carried BY THE LATCH: the lean has to already be there on the frame the jump
	// fires, one reaction delay from now, or he takes off from where he was standing.
	FVector ToWall = StickWallPoint - MyLocation;
	ToWall.Z = 0.f;
	const FVector Lean = ToWall.GetSafeNormal();
	if (Lean.IsNearlyZero())
	{
		return;
	}

	const ETraceBotWantResult Result = Want(Situation, ETraceBotAbilityAct::Jump,
		TEXT("Slimeball: up the wall to hold an angle"), /*AimPoint*/ nullptr, &Lean);

	if (Result == ETraceBotWantResult::Declined)
	{
		NextStickTime = Situation.Now + 2.f;
	}
}
