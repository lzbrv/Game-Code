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
#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Abilities/Characters/TraceAbilitySetOyster.h"
#include "Abilities/Characters/TraceAbilitySetRocco.h"
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
			int32 SecondaryDowns = 0;
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

	void NoteSecondary(ETraceCharacterId Id, bool bDown)
	{
		if (!bDown)
		{
			return;
		}
		++Counters().SecondaryDowns;
		UE_LOG(LogTraceGame, Verbose, TEXT("[BotAbility] %s held V (secondary)."), TraceCharacterIdToString(Id));
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
			TEXT("Mace=%d/%d Oyster=%d/%d X=%d/%d | none(should be 0)=%d/%d ")
			TEXT("| spike-pulls=%d V-holds=%d ability-jumps=%d (consumed %d) ability-dashes=%d"),
			Now - C.StartTime,
			C.Fired[Slot(ETraceCharacterId::Rocco)],  C.Attempts[Slot(ETraceCharacterId::Rocco)],
			C.Fired[Slot(ETraceCharacterId::Chut)],   C.Attempts[Slot(ETraceCharacterId::Chut)],
			C.Fired[Slot(ETraceCharacterId::Mace)],   C.Attempts[Slot(ETraceCharacterId::Mace)],
			C.Fired[Slot(ETraceCharacterId::Oyster)], C.Attempts[Slot(ETraceCharacterId::Oyster)],
			C.Fired[Slot(ETraceCharacterId::X)],      C.Attempts[Slot(ETraceCharacterId::X)],
			C.Fired[Slot(ETraceCharacterId::None)],   C.Attempts[Slot(ETraceCharacterId::None)],
			C.SpikePulls, C.SecondaryDowns, C.AbilityJumps, C.AbilityJumpsConsumed, C.AbilityDashes);

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

	bHasSpikeCandidate = false;
	NextSpikeProbeTime = 0.f;
	SuspendReleaseTime = 0.f;
	NextSuspendTime = 0.f;

	NextPicklerProbeTime = 0.f;
	bHasPicklerAim = false;
	NextApproachDashTime = 0.f;

	NextBashTime = 0.f;
	NextRedirectTime = 0.f;
}

void FTraceBotAbilityBrain::OnUnPossessed()
{
	ClearIntent();
	SuspendReleaseTime = 0.f;
	bHasSpikeCandidate = false;
	bHasPicklerAim = false;
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
		SuspendReleaseTime = 0.f;
		return;
	}

	const ETraceCharacterId MyCharacter = Situation.Abilities->GetCharacterId();
	if (MyCharacter != PlannedCharacter)
	{
		// The character changed under us (selection, half time, the mode-A freeze). Everything latched
		// belonged to the character that has just left.
		ClearIntent();
		SuspendReleaseTime = 0.f;
		bHasSpikeCandidate = false;
		bHasPicklerAim = false;
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
		SuspendReleaseTime = 0.f;
		return;
	}

	switch (MyCharacter)
	{
	case ETraceCharacterId::Rocco:  PlanRocco(Situation, Out);  break;
	case ETraceCharacterId::Chut:   PlanChut(Situation, Out);   break;
	case ETraceCharacterId::Mace:   PlanMace(Situation, Out);   break;
	case ETraceCharacterId::Oyster: PlanOyster(Situation, Out); break;
	case ETraceCharacterId::X:      PlanX(Situation, Out);      break;
	default: break;
	}

	Resolve(Situation, Out);

	// The suspend is a LEVEL, not an edge, so it is asserted after Resolve() rather than through it.
	if (SuspendReleaseTime > 0.f)
	{
		Out.bSecondaryHeld = (Situation.Now < SuspendReleaseTime);
		if (!Out.bSecondaryHeld)
		{
			SuspendReleaseTime = 0.f;
		}
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

	if (SuspendReleaseTime <= 0.f && AngleTarget != nullptr)
	{
		const float MaxHold = FMath::Max(0.2f, UTraceSettings::Get().MaceSuspendMaxSeconds);

		if (bAirborne)
		{
			if (FMath::FRand() < TraceBotAbilityBrainLocal::Willingness(PersonalityBias))
			{
				// The hold is SHORTER than the ability's own 1.25 s cap, and rolled. A bot that always
				// suspends for exactly the maximum reads as a machine, and running the meter out in
				// mid-air is strictly worse than choosing to come down.
				SuspendReleaseTime = Situation.Now + FMath::FRandRange(MaxHold * 0.45f, MaxHold * 0.85f);
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
	if (SuspendReleaseTime > 0.f)
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
