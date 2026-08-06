#include "Gameplay/TraceWeaponComponent.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"                       // TActorIterator, for the recoil test harness
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"   // recoil: the camera boom's tick order
#include "Math/UnrealMathUtility.h"

#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceHitZones.h"
#include "Gameplay/TraceTracer.h"
#include "Net/TraceLagCompensationComponent.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"

/**
 * Logs, per shot, the zone the SHOOTER's own predicted trace produced and the zone the SERVER
 * scored, plus whether they agree.
 *
 * Spec section 6 calls the disagreement out by name: "the client-side predicted trace and the
 * server's authoritative trace must use the SAME zone model, or players will see hits that do not
 * register". They share one implementation (FTraceHitZoneModel), so the only thing left that can
 * differ is the POSE each is fed - live-and-interpolated on the client, rewound history on the
 * server. This is how that is measured rather than assumed. Off by default; it is one line per
 * shot and ten bots fire a lot.
 */
static TAutoConsoleVariable<int32> CVarTraceDebugHitZones(
	TEXT("Trace.DebugHitZones"),
	0,
	TEXT("1: log the predicted vs server-resolved damage zone for every shot fired by a local player."),
	ECVF_Default);

// =================================================================================================
// SHOT STATISTICS - the answer to "shooting feels WAY more inconsistent"
//
// The complaint is about FEEL, and feel is a distribution. Damage is head 100 / body 40 / legs 25
// against 100 health, so the time to kill is 1, 3 or 4 rounds depending purely on which band a shot
// lands in. If the bands sit where the player thinks they do, that reads as positional damage; if
// they do not, it reads as the gun randomly deciding how much it feels like doing today - every
// shot registering, none of them worth the same.
//
// So this accumulates, on the authority, for every shot the server accepts:
//   * the zone histogram, which is the headline number,
//   * WHERE on the body each shot landed as a fraction of the target's height, so the bands can be
//     checked against where people actually aim rather than against an assumption,
//   * the same shot re-classified at the ray's closest approach to the body axis instead of at the
//     capsule entry point, which isolates one specific suspected defect,
//   * predicted (client/shooter) vs authoritative (server) zone agreement - the 567/0 baseline,
//   * how often the world trace truncated the shot, and how short.
//
// Off by default and one integer increment per shot when on. Bots fire constantly, so a 90 s match
// is a few thousand samples, which is more than enough to see a skew.
// =================================================================================================

/**
 * Per-shot recoil trace: what the kick was, what it accumulated to, and where the view ended up.
 *
 * Spec v5 section 6 asks for recoil that is felt but does not move the bullet, and the only way to
 * tell those two apart from outside is to print the view state around the moment of fire. Off by
 * default - one line per shot, and at 150 RPM a firing bot squad is still a lot of lines.
 */
static TAutoConsoleVariable<int32> CVarTraceDebugRecoil(
	TEXT("Trace.DebugRecoil"),
	0,
	TEXT("1: log the upward recoil kick, the accumulated climb and the resulting control pitch for every locally fired shot."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarTraceShotStats(
	TEXT("Trace.ShotStats"),
	0,
	TEXT("1: accumulate the hit-zone / impact-height distribution for every server-accepted shot. Trace.ShotStats.Dump prints it."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarTraceShotStatsInterval(
	TEXT("Trace.ShotStats.Interval"),
	20.f,
	TEXT("Seconds between automatic Trace.ShotStats dumps. 0 disables the automatic dump."),
	ECVF_Default);

namespace TraceShotStats
{
	/** Height buckets, as twentieths of the target's full capsule height (0.05 = ~8.8 uu). */
	constexpr int32 NumHeightBuckets = 20;

	struct FStats
	{
		// --- what the local input path did ---
		int32 LocalShotsFired = 0;
		int32 RefusedCarrier = 0;
		int32 RefusedDead = 0;

		// --- what the server did with them ---
		int32 ServerShotsAccepted = 0;
		int32 ServerRejectedRate = 0;
		int32 ServerRejectedState = 0;
		int32 ServerRejectedPayload = 0;

		// --- outcomes ---
		int32 ZoneCount[4] = { 0, 0, 0, 0 };          // None / Head / Body / Legs
		int32 AltZoneCount[4] = { 0, 0, 0, 0 };       // same, classified at closest approach
		int32 ZoneReclassified = 0;                   // entry-point verdict != closest-approach verdict
		int32 ReclassLegsToBody = 0;
		int32 ReclassBodyToLegs = 0;

		int32 HeightHistogram[NumHeightBuckets] = {};
		double HeightFractionSum = 0.0;
		int32 HeightSamples = 0;

		// --- pose provenance ---
		int32 VictimPoseRewound = 0;
		int32 VictimPoseLive = 0;
		int32 VictimNonStandingPosture = 0;           // PostureScale < 0.99, i.e. mid-slide
		double PostureSum = 0.0;

		// --- world geometry interaction ---
		int32 WorldTruncated = 0;
		int32 WorldStartPenetrating = 0;
		int32 WorldTruncatedUnder200 = 0;

		// --- predicted vs authoritative ---
		int32 PredictionComparisons = 0;
		int32 PredictionAgree = 0;
		int32 PredictionZoneMismatch = 0;
		int32 PredictionVictimMismatch = 0;

		double LastDumpTime = 0.0;
	};

	static FStats GStats;

	static void Reset()
	{
		const double Keep = GStats.LastDumpTime;
		GStats = FStats();
		GStats.LastDumpTime = Keep;
	}

	static float Percent(int32 Part, int32 Whole)
	{
		return (Whole > 0) ? (100.f * static_cast<float>(Part) / static_cast<float>(Whole)) : 0.f;
	}

	static void Dump()
	{
		const FStats& S = GStats;
		const int32 Hits = S.ZoneCount[1] + S.ZoneCount[2] + S.ZoneCount[3];

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE SHOT STATS =========="));
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT input     : local fired %d | refused carrier %d, dead %d"),
			S.LocalShotsFired, S.RefusedCarrier, S.RefusedDead);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT server    : accepted %d | rejected rate %d, state %d, payload %d"),
			S.ServerShotsAccepted, S.ServerRejectedRate, S.ServerRejectedState, S.ServerRejectedPayload);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT outcome   : hits %d (%.1f%% of accepted) | misses %d"),
			Hits, Percent(Hits, S.ServerShotsAccepted), S.ZoneCount[0]);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT ZONES     : HEAD %d (%.1f%%)  BODY %d (%.1f%%)  LEGS %d (%.1f%%)   [of %d hits]"),
			S.ZoneCount[1], Percent(S.ZoneCount[1], Hits),
			S.ZoneCount[2], Percent(S.ZoneCount[2], Hits),
			S.ZoneCount[3], Percent(S.ZoneCount[3], Hits),
			Hits);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT ZONES-alt : HEAD %d (%.1f%%)  BODY %d (%.1f%%)  LEGS %d (%.1f%%)   [classified at closest approach]"),
			S.AltZoneCount[1], Percent(S.AltZoneCount[1], Hits),
			S.AltZoneCount[2], Percent(S.AltZoneCount[2], Hits),
			S.AltZoneCount[3], Percent(S.AltZoneCount[3], Hits));
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT reclassify: %d of %d hits (%.1f%%) change tier | legs->body %d, body->legs %d"),
			S.ZoneReclassified, Hits, Percent(S.ZoneReclassified, Hits),
			S.ReclassLegsToBody, S.ReclassBodyToLegs);

		if (S.HeightSamples > 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("SHOTSTAT height    : mean impact at %.3f of body height (feet 0.00, crown 1.00). Hip band is at 0.46, head sphere centre 0.905."),
				S.HeightFractionSum / static_cast<double>(S.HeightSamples));

			FString Histogram;
			for (int32 Bucket = 0; Bucket < NumHeightBuckets; ++Bucket)
			{
				Histogram += FString::Printf(TEXT("  %.2f-%.2f %5d (%4.1f%%)%s"),
					Bucket / static_cast<float>(NumHeightBuckets),
					(Bucket + 1) / static_cast<float>(NumHeightBuckets),
					S.HeightHistogram[Bucket], Percent(S.HeightHistogram[Bucket], S.HeightSamples),
					((Bucket % 4) == 3) ? TEXT("\n") : TEXT(""));
			}
			UE_LOG(LogTraceGame, Display, TEXT("SHOTSTAT histogram (closest-approach height):\n%s"), *Histogram);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT pose      : rewound %d, live %d | mid-slide victims %d (%.1f%%), mean posture %.4f"),
			S.VictimPoseRewound, S.VictimPoseLive, S.VictimNonStandingPosture,
			Percent(S.VictimNonStandingPosture, Hits),
			(Hits > 0) ? (S.PostureSum / Hits) : 1.0);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT world     : truncated %d (%.1f%%) | start-penetrating %d | truncated under 200uu %d"),
			S.WorldTruncated, Percent(S.WorldTruncated, S.ServerShotsAccepted),
			S.WorldStartPenetrating, S.WorldTruncatedUnder200);
		UE_LOG(LogTraceGame, Display,
			TEXT("SHOTSTAT predict   : %d compared | AGREE %d (%.2f%%) | zone mismatch %d | victim mismatch %d"),
			S.PredictionComparisons, S.PredictionAgree, Percent(S.PredictionAgree, S.PredictionComparisons),
			S.PredictionZoneMismatch, S.PredictionVictimMismatch);
		UE_LOG(LogTraceGame, Display, TEXT("======================================"));
	}
} // namespace TraceShotStats

static FAutoConsoleCommand GTraceShotStatsDumpCmd(
	TEXT("Trace.ShotStats.Dump"),
	TEXT("Prints the accumulated hit-zone / impact-height distribution gathered while Trace.ShotStats is 1."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceShotStats::Dump(); }));

static FAutoConsoleCommand GTraceShotStatsResetCmd(
	TEXT("Trace.ShotStats.Reset"),
	TEXT("Clears the accumulated shot statistics."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceShotStats::Reset(); }));

UTraceWeaponComponent::UTraceWeaponComponent()
{
	// Ticks only while the trigger is held on the machine that owns the input.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	// No replicated state of its own, but the component must be replicated for its RPCs to route.
	SetIsReplicatedByDefault(true);
}

void UTraceWeaponComponent::BeginPlay()
{
	Super::BeginPlay();

	// -------------------------------------------------------------------------------------------
	// TICK ORDER, AND THE ONE-FRAME CAMERA LAG IT FIXES.
	//
	// The recoil kick is written to the control rotation from this component's tick. The camera boom
	// reads the control rotation from ITS tick (USpringArmComponent::UpdateDesiredArmLocation via
	// GetTargetRotation) and moves the camera to match. Both live in TG_PrePhysics, and component
	// tick order inside a group is registration order - the boom is created in the character's
	// constructor, so it ticks FIRST and the camera renders one frame behind every kick.
	//
	// MEASURED, before this call existed: Trace.TestRecoil reported aimErr = 0.8000 deg on the frame
	// after the first shot - exactly one kick - against a project guarantee of 0.0000. The bullet was
	// never wrong (the shot is sampled and sent BEFORE the kick, so it goes where the crosshair was),
	// but the probe that guards the guarantee could not tell those two things apart, and neither
	// could the next person to read its output.
	//
	// One prerequisite edge fixes it: the boom updates after this component, so the kick and the
	// camera that renders it land in the same frame. This is an engine API called on their component
	// rather than an edit to their file, and it is a no-op for every pawn that is not a local human -
	// bots have no camera boom worth ordering and no recoil to order it against.
	// -------------------------------------------------------------------------------------------
	if (GetRecoilController() == nullptr)
	{
		return;
	}

	if (const AActor* OwnerActor = GetOwner())
	{
		if (USpringArmComponent* CameraBoom = OwnerActor->FindComponentByClass<USpringArmComponent>())
		{
			CameraBoom->AddTickPrerequisiteComponent(this);
		}
	}
}

ATraceCharacter* UTraceWeaponComponent::GetTraceCharacter() const
{
	return Cast<ATraceCharacter>(GetOwner());
}

double UTraceWeaponComponent::GetServerTimeSeconds() const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0.0;
	}

	// ATraceGameState inherits the engine's replicated shared clock; on the server this is simply
	// world time, on a client it is world time plus the replicated delta.
	if (const ATraceGameState* TraceGameState = World->GetGameState<ATraceGameState>())
	{
		return TraceGameState->GetServerWorldTimeSeconds();
	}
	if (const AGameStateBase* GameState = World->GetGameState())
	{
		return GameState->GetServerWorldTimeSeconds();
	}
	return World->GetTimeSeconds();
}

double UTraceWeaponComponent::GetLocalTimeSeconds() const
{
	const UWorld* World = GetWorld();
	return (World != nullptr) ? World->GetTimeSeconds() : 0.0;
}

bool UTraceWeaponComponent::CanFire() const
{
	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return false;
	}
	if (!Character->IsAlive())
	{
		return false;
	}
	if (Character->IsCarrier())
	{
		// Spec §4: the carrier cannot shoot. Note this is NOT symmetrical with the shield any more —
		// the shield drops for the duration of a pass (ATraceCore::IsShieldSuppressedFor) but the gun
		// stays locked, which is the whole risk beat: mid-pass you are shootable and unarmed.
		//
		// ATraceCharacter::DoFirePressed also routes a carrier's mouse1 to the pass instead of here,
		// so this gate is a second, independent guarantee rather than the only one.
		return false;
	}

	const double FireInterval = FMath::Max(0.01f, UTraceSettings::Get().FireInterval);
	return (GetLocalTimeSeconds() - LastLocalFireTime) >= FireInterval;
}

void UTraceWeaponComponent::StartFire()
{
	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsLocallyControlled())
	{
		// Input is a local concept: a proxy copy of somebody else's pawn must never fire.
		return;
	}

	bTriggerHeld = true;
	SetComponentTickEnabled(true);

	// A gun that is silent BY DESIGN is indistinguishable from broken input, and this rule cost real
	// debugging time once already. Say so, once per press, at Verbose.
	if (Character->IsCarrier())
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[%s] Fire refused: carrying the Core (carriers trade the gun for bullet immunity)."),
			*GetNameSafe(Character));

		if (CVarTraceShotStats.GetValueOnGameThread() != 0)
		{
			++TraceShotStats::GStats.RefusedCarrier;
		}
	}
	else if (!Character->IsAlive() && CVarTraceShotStats.GetValueOnGameThread() != 0)
	{
		++TraceShotStats::GStats.RefusedDead;
	}

	if (CanFire())
	{
		FireOnce();
	}
}

void UTraceWeaponComponent::StopFire()
{
	bTriggerHeld = false;

	// The tick is NOT switched off here any more. Recoil recovery is the whole point of releasing
	// the trigger, so the component has to keep ticking until the view has been handed back; the
	// tick disables itself below once there is nothing left to recover.
	if (RecoilAppliedPitch <= UE_KINDA_SMALL_NUMBER)
	{
		SetComponentTickEnabled(false);
	}
}

void UTraceWeaponComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsLocallyControlled())
	{
		// The pawn is gone or is no longer ours: genuine teardown, drop everything. The climb goes
		// with it — there is no view left to hand back to.
		bTriggerHeld = false;
		ResetRecoil();
		SetComponentTickEnabled(false);
		return;
	}

	if (!Character->IsAlive())
	{
		// Dead. Forget the climb rather than recovering it: the death camera owns the view now, and
		// a recovery that keeps writing control rotation would fight it for the whole respawn.
		ResetRecoil();
	}
	else
	{
		TickRecoil(DeltaTime);
	}

	// Firing is gated separately from recoil, and deliberately does NOT clear bTriggerHeld while
	// ineligible: this component only hears about the trigger on press and release, so clearing it
	// means a player who is still physically holding the button gets nothing after passing the Core
	// away or after respawning — they would have to release and press again for no reason they can
	// see. Keep ticking, skip firing, and resume the instant the gate reopens.
	if (bTriggerHeld && Character->IsAlive() && !Character->IsCarrier() && CanFire())
	{
		FireOnce();
	}

	if (!bTriggerHeld && RecoilAppliedPitch <= UE_KINDA_SMALL_NUMBER)
	{
		SetComponentTickEnabled(false);
	}
}

// =================================================================================================
// UPWARDS RECOIL  (spec v5 section 6)
// =================================================================================================

APlayerController* UTraceWeaponComponent::GetRecoilController() const
{
	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return nullptr;
	}

	// APlayerController and LOCAL, both load-bearing. A bot's AAIController fails the cast, which is
	// how bots are kept out of the recoil model; a remote client's proxy pawn on the server fails
	// IsLocalController, which is how the server is kept from kicking a view it does not own.
	APlayerController* RecoilController = Cast<APlayerController>(Character->GetController());
	return (RecoilController != nullptr && RecoilController->IsLocalController()) ? RecoilController : nullptr;
}

namespace TraceRecoilMath
{
	/**
	 * Folds any DOWNWARD view movement the player made since our last write into the accumulator,
	 * so their own compensation is not paid back to them as a second kick when the gun settles.
	 *
	 * This is the difference between recoil you can fight and recoil that fights back: without it, a
	 * player who drags 3 degrees down to hold the crosshair on a chest has those 3 degrees taken off
	 * AGAIN by the recovery and ends the burst aiming at the floor.
	 *
	 * Upward player movement is deliberately NOT credited: looking up is not paying off a climb, and
	 * crediting it would let a player bank recovery by flicking upward between shots.
	 */
	void ConsumePlayerCompensation(double CurrentPitch, bool bEnabled, double& InOutAppliedPitch,
		double& InOutTrackedPitch, bool& bInOutTrackingValid)
	{
		if (bInOutTrackingValid && bEnabled)
		{
			const double PlayerDelta = CurrentPitch - InOutTrackedPitch;
			if (PlayerDelta < 0.0)
			{
				InOutAppliedPitch = FMath::Max(0.0, InOutAppliedPitch + PlayerDelta);
			}
		}

		InOutTrackedPitch = CurrentPitch;
		bInOutTrackingValid = true;
	}
}

void UTraceWeaponComponent::AddRecoilPitch(APlayerController* RecoilController, double DeltaPitchDegrees)
{
	if (RecoilController == nullptr || FMath::IsNearlyZero(DeltaPitchDegrees))
	{
		return;
	}

	FRotator ControlRotation = RecoilController->GetControlRotation();
	const double OldPitch = FRotator::NormalizeAxis(ControlRotation.Pitch);

	// The camera manager's own stops, so the climb cannot push the view past the point the player's
	// mouse could reach and then owe them a recovery from somewhere that was never applied.
	double MinPitch = -89.9;
	double MaxPitch = 89.9;
	if (const APlayerCameraManager* CameraLimits = RecoilController->PlayerCameraManager)
	{
		const double LimitLow = FRotator::NormalizeAxis(CameraLimits->ViewPitchMin);
		const double LimitHigh = FRotator::NormalizeAxis(CameraLimits->ViewPitchMax);
		if (LimitLow < LimitHigh)
		{
			MinPitch = LimitLow;
			MaxPitch = LimitHigh;
		}
	}

	const double NewPitch = FMath::Clamp(OldPitch + DeltaPitchDegrees, MinPitch, MaxPitch);
	const double Applied = NewPitch - OldPitch;

	// Read back what LANDED, not what was asked for. Against the pitch stop those differ, and an
	// accumulator that believes the unclamped figure would owe the player a recovery it never
	// applied — the view would sink below where they were aiming when the burst ended.
	if (!FMath::IsNearlyZero(Applied))
	{
		ControlRotation.Pitch = NewPitch;
		RecoilController->SetControlRotation(ControlRotation);
		RecoilAppliedPitch = FMath::Max(0.0, RecoilAppliedPitch + Applied);
	}

	RecoilTrackedPitch = NewPitch;
	bRecoilTrackingValid = true;
}

void UTraceWeaponComponent::ApplyRecoilKick()
{
	const UTraceSettings& Settings = UTraceSettings::Get();
	if (!Settings.bRecoilEnabled)
	{
		return;
	}

	APlayerController* RecoilController = GetRecoilController();
	if (RecoilController == nullptr)
	{
		return;
	}

	const double Now = GetLocalTimeSeconds();
	if ((Now - LastRecoilShotTime) > FMath::Max(0.f, Settings.RecoilBurstResetSeconds))
	{
		// A gap long enough to count as a new burst resets the GROWTH only. Whatever climb is still
		// on the view keeps recovering on its own schedule; the two are independent by design.
		RecoilBurstShotIndex = 0;
	}
	LastRecoilShotTime = Now;

	// Settle up with the player's own movement before adding to the accumulator, or their pull-down
	// since the last tick would be counted against the kick we are about to apply.
	TraceRecoilMath::ConsumePlayerCompensation(
		FRotator::NormalizeAxis(RecoilController->GetControlRotation().Pitch),
		Settings.bRecoilPlayerCompensationCancels,
		RecoilAppliedPitch, RecoilTrackedPitch, bRecoilTrackingValid);

	const double Growth = 1.0 + FMath::Max(0.f, Settings.RecoilPitchGrowthPerShot) * static_cast<double>(RecoilBurstShotIndex);
	const double Headroom = FMath::Max(0.0, static_cast<double>(FMath::Max(0.f, Settings.RecoilMaxPitchDegrees)) - RecoilAppliedPitch);

	// Truncated at the ceiling rather than clamped after the fact, so the view never overshoots and
	// visibly snaps back down.
	const double Kick = FMath::Min(FMath::Max(0.f, Settings.RecoilPitchPerShot) * Growth, Headroom);
	++RecoilBurstShotIndex;

	const double PitchBefore = RecoilTrackedPitch;
	if (Kick > 0.0)
	{
		AddRecoilPitch(RecoilController, Kick);
	}

	if (CVarTraceDebugRecoil.GetValueOnGameThread() != 0)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Recoil] shot %d of burst: kick %+.3fdeg (growth x%.2f, headroom %.3f) | pitch %+.3f -> %+.3f | climb %.3fdeg | yaw untouched"),
			RecoilBurstShotIndex, Kick, Growth, Headroom, PitchBefore, RecoilTrackedPitch, RecoilAppliedPitch);
	}
}

void UTraceWeaponComponent::TickRecoil(float DeltaTime)
{
	if (RecoilAppliedPitch <= UE_KINDA_SMALL_NUMBER)
	{
		RecoilAppliedPitch = 0.0;
		return;
	}

	APlayerController* RecoilController = GetRecoilController();
	if (RecoilController == nullptr)
	{
		ResetRecoil();
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// NOTE the recovery deliberately runs even when bRecoilEnabled has just been switched OFF: the
	// climb already on the view has to be handed back, or turning the feature off mid-burst strands
	// the player looking at the sky.
	TraceRecoilMath::ConsumePlayerCompensation(
		FRotator::NormalizeAxis(RecoilController->GetControlRotation().Pitch),
		Settings.bRecoilPlayerCompensationCancels,
		RecoilAppliedPitch, RecoilTrackedPitch, bRecoilTrackingValid);

	if (RecoilAppliedPitch <= UE_KINDA_SMALL_NUMBER)
	{
		RecoilAppliedPitch = 0.0;
		return;
	}

	if ((GetLocalTimeSeconds() - LastRecoilShotTime) < FMath::Max(0.f, Settings.RecoilRecoveryDelaySeconds))
	{
		return;
	}

	// Exponential rather than FMath::FInterpTo's linearisation, so the curve is identical at 30 and
	// 240 fps — a recovery whose shape depends on frame rate is a feel bug nobody can reproduce.
	const double Proportional = RecoilAppliedPitch
		* (1.0 - FMath::Exp(-static_cast<double>(FMath::Max(0.f, Settings.RecoilRecoverySpeed)) * DeltaTime));

	// ...plus a linear floor, because a purely proportional return has an infinite tail and would
	// leave a fraction of a degree on the view for the whole of the next engagement.
	const double Floor = static_cast<double>(FMath::Max(0.f, Settings.RecoilRecoveryMinRateDegrees)) * DeltaTime;

	const double Step = FMath::Min(RecoilAppliedPitch, FMath::Max(Proportional, Floor));
	if (Step > 0.0)
	{
		AddRecoilPitch(RecoilController, -Step);
	}
}

void UTraceWeaponComponent::ResetRecoil()
{
	RecoilAppliedPitch = 0.0;
	RecoilBurstShotIndex = 0;
	bRecoilTrackingValid = false;
	RecoilTrackedPitch = 0.0;
}

void UTraceWeaponComponent::FireOnce()
{
	ATraceCharacter* Character = GetTraceCharacter();
	UWorld* World = GetWorld();
	if (Character == nullptr || World == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// Timestamp first: this is the instant the player believes they fired, and it is what the server
	// rewinds to. Taking it before any of the work below keeps it honest.
	const double FireServerTime = GetServerTimeSeconds();
	LastLocalFireTime = GetLocalTimeSeconds();

	if (CVarTraceShotStats.GetValueOnGameThread() != 0)
	{
		++TraceShotStats::GStats.LocalShotsFired;
	}

	const FVector Origin = Character->GetMuzzleLocation();
	FVector Dir = Character->GetAimDirection().GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = Character->GetActorForwardVector();
	}

	// NO SPREAD. Spec section 6: "There is no movement inaccuracy. Set spread to 0." The shot is
	// exactly the aim ray, moving or still, and UTraceSettings::SpreadDegrees is deliberately not
	// read - leaving the roll in place but configured to zero would mean a stale .ini quietly
	// reintroduced inaccuracy the design has removed. This also retires the "a modified client
	// could roll zero spread" cheat, since zero is now the rule for everyone.
	//
	// The gun is precise; the DAMAGE ZONES are what make aim matter now (head 100 / body 40 /
	// legs 25), which is a skill test the player can see and learn rather than a hidden dice roll.

	const float Range = FMath::Max(1.f, Settings.HitscanRange);

	// Cosmetic-only local resolve: where should *our* tracer stop? No damage is applied on the
	// client under any circumstances - the server owns that entirely.
	//
	// This runs the SAME resolver the server will run, rather than a plain ECC_Visibility line
	// trace. A line trace on that channel never stops on a player: the character capsule uses the
	// stock "Pawn" profile, whose one custom response is Visibility = Ignore. The shooter would
	// therefore watch their own tracer punch through an enemy and terminate on the wall behind, on
	// the same frame the server sent back a hit marker - the exact contradiction that makes a
	// hitscan prototype feel broken. ResolveHitscan already traces the world for static geometry
	// internally and always writes OutImpactPoint, so this also removes a duplicate trace.
	//
	// On a client GetAuthGameMode() is null and the lag-comp histories are empty, so it resolves
	// against the live (interpolated) poses the player can actually see - which is the right answer
	// for a tracer. Nothing here is authoritative; the server re-resolves from scratch.
	FVector TracerEnd = Origin + Dir * Range;
	{
		FVector PredictedImpact = TracerEnd;
		ETraceHitZone PredictedZone = ETraceHitZone::None;
		ATraceCharacter* PredictedVictim = UTraceLagCompensationComponent::ResolveHitscan(
			World, Character, Origin, Dir, Range,
			static_cast<float>(FireServerTime), PredictedImpact, PredictedZone);
		TracerEnd = PredictedImpact;

		LastPredictedZone = PredictedZone;
		LastPredictedVictim = PredictedVictim;
		LastPredictedFireServerTime = FireServerTime;
	}

	// "Did the beam stop on something" - anything short of full range means a surface (a body or
	// world geometry) terminated it, and that is where the impact flash belongs.
	const bool bImpacted = FVector::DistSquared(Origin, TracerEnd) < static_cast<double>(Range) * Range * 0.998;

	PlayLocalTracer(Origin, TracerEnd, bImpacted);

	// Viewmodel recoil, once per ROUND. This is the only place that knows a round actually left the
	// gun, so a held burst kicks per shot instead of once on the trigger press. Cosmetic only,
	// rate-limited inside, and a no-op on any machine that is not looking out of this pawn.
	Character->NotifyWeaponFired();

	ServerFire(FVector_NetQuantize(Origin), FVector_NetQuantizeNormal(Dir), static_cast<float>(FireServerTime));

	// UPWARDS RECOIL, LAST — AND THE ORDER IS THE WHOLE DESIGN (spec v5 section 6).
	//
	// Origin and Dir were sampled at the top of this function and have already gone to the server,
	// so the round that causes this kick flies exactly where the crosshair was when the trigger
	// broke. The kick moves the control rotation, which is what the NEXT shot will be built from —
	// that is recoil — and it moves the camera with it, because ATraceCharacter::ResolveAimRotation
	// and the camera are both pure functions of that same rotation. Hence aimErr stays 0.0000 deg.
	//
	// Nothing about this replicates. The server is told a DIRECTION, not a rotation, so there is no
	// recoil state for the two machines to disagree about and the authoritative trace resolves the
	// same ray the shooter saw. Client-predicted for feel, invisible to the hit resolution.
	ApplyRecoilKick();
}

void UTraceWeaponComponent::PlayLocalTracer(const FVector& From, const FVector& To, bool bImpacted) const
{
	UWorld* World = GetWorld();
	ATraceCharacter* Character = GetTraceCharacter();
	if (World == nullptr || Character == nullptr)
	{
		return;
	}

	ATraceTracer::Spawn(World, From, To, TraceTeamColor(Character->GetTeam()), bImpacted);
}

void UTraceWeaponComponent::AccumulateShotStats(ETraceHitZone ServerZone, const ATraceCharacter* Victim,
	const FTraceHitscanDiagnostics& Diagnostics)
{
	TraceShotStats::FStats& S = TraceShotStats::GStats;

	const int32 ZoneIndex = FMath::Clamp(static_cast<int32>(ServerZone), 0, 3);
	++S.ZoneCount[ZoneIndex];

	if (Diagnostics.bWorldTraceHit)
	{
		++S.WorldTruncated;
		if (Diagnostics.bWorldStartPenetrating)
		{
			++S.WorldStartPenetrating;
		}
		if (Diagnostics.WorldHitDistance >= 0.0 && Diagnostics.WorldHitDistance < 200.0)
		{
			++S.WorldTruncatedUnder200;
		}
	}

	if (Diagnostics.bHaveVictim)
	{
		const int32 AltIndex = FMath::Clamp(static_cast<int32>(Diagnostics.ZoneAtClosestApproach), 0, 3);
		++S.AltZoneCount[AltIndex];

		if (Diagnostics.ZoneAtClosestApproach != ServerZone)
		{
			++S.ZoneReclassified;
			if (ServerZone == ETraceHitZone::Legs && Diagnostics.ZoneAtClosestApproach == ETraceHitZone::Body)
			{
				++S.ReclassLegsToBody;
			}
			else if (ServerZone == ETraceHitZone::Body && Diagnostics.ZoneAtClosestApproach == ETraceHitZone::Legs)
			{
				++S.ReclassBodyToLegs;
			}
		}

		if (Diagnostics.ClosestHeightFraction >= 0.0)
		{
			const int32 Bucket = FMath::Clamp(
				static_cast<int32>(Diagnostics.ClosestHeightFraction * TraceShotStats::NumHeightBuckets),
				0, TraceShotStats::NumHeightBuckets - 1);
			++S.HeightHistogram[Bucket];
			S.HeightFractionSum += Diagnostics.ClosestHeightFraction;
			++S.HeightSamples;
		}

		if (Diagnostics.bVictimPoseRewound)
		{
			++S.VictimPoseRewound;
		}
		else
		{
			++S.VictimPoseLive;
		}

		S.PostureSum += Diagnostics.VictimFrame.PostureScale;
		if (Diagnostics.VictimFrame.PostureScale < 0.99)
		{
			++S.VictimNonStandingPosture;
		}
	}

	// Predicted vs authoritative. Only meaningful when both halves ran here; a remote client's
	// prediction lives in another process and cannot be compared from inside this one.
	const ATraceCharacter* Shooter = GetTraceCharacter();
	if (Shooter != nullptr && Shooter->IsLocallyControlled() && LastPredictedFireServerTime > 0.0)
	{
		++S.PredictionComparisons;
		const bool bSameVictim = (LastPredictedVictim.Get() == Victim);
		const bool bSameZone = (LastPredictedZone == ServerZone);
		if (bSameVictim && bSameZone)
		{
			++S.PredictionAgree;
		}
		else
		{
			if (!bSameZone) { ++S.PredictionZoneMismatch; }
			if (!bSameVictim) { ++S.PredictionVictimMismatch; }
		}
	}

	// Automatic dump so a headless run needs no console at the end of it.
	const float Interval = CVarTraceShotStatsInterval.GetValueOnGameThread();
	if (Interval > 0.f)
	{
		const double Now = GetLocalTimeSeconds();
		if (S.LastDumpTime <= 0.0)
		{
			S.LastDumpTime = Now;
		}
		else if ((Now - S.LastDumpTime) >= Interval)
		{
			S.LastDumpTime = Now;
			TraceShotStats::Dump();
		}
	}
}

bool UTraceWeaponComponent::ServerFire_Validate(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientFireServerTime)
{
	// Validation failure disconnects the client, so only reject payloads that are outright
	// impossible to reason about. Everything else is handled with an early return below.
	if (Origin.ContainsNaN() || Direction.ContainsNaN())
	{
		return false;
	}
	if (!FMath::IsFinite(ClientFireServerTime))
	{
		return false;
	}
	return true;
}

void UTraceWeaponComponent::ServerFire_Implementation(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientFireServerTime)
{
	AActor* OwnerActor = GetOwner();
	UWorld* World = GetWorld();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || World == nullptr || Character == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}

	// ---- payload sanity (never check() on network input) ---------------------------------
	const bool bCollectStats = (CVarTraceShotStats.GetValueOnGameThread() != 0);

	FVector Dir(Direction);
	const double DirLengthSq = Dir.SizeSquared();
	if (DirLengthSq < 0.25 || DirLengthSq > 4.0)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: rejecting non-unit direction from %s"), *GetNameSafe(OwnerActor));
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedPayload; }
		return;
	}
	Dir = Dir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedPayload; }
		return;
	}
	if (FMath::Abs(Origin.X) > MaxReasonableCoordinateUU || FMath::Abs(Origin.Y) > MaxReasonableCoordinateUU || FMath::Abs(Origin.Z) > MaxReasonableCoordinateUU)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: rejecting out-of-world origin from %s"), *GetNameSafe(OwnerActor));
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedPayload; }
		return;
	}

	// ---- state gate ----------------------------------------------------------------------
	if (!Character->IsAlive() || Character->IsCarrier())
	{
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedState; }
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// ---- fire rate, with slack for honest jitter -----------------------------------------
	const double FireInterval = FMath::Max(0.01f, Settings.FireInterval);
	const double LocalNow = GetLocalTimeSeconds();
	if ((LocalNow - LastAcceptedFireTime) < FireInterval * (1.0 - FireRateTolerance))
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: rate-limited %s (%.3fs since last accepted)"),
			*GetNameSafe(OwnerActor), LocalNow - LastAcceptedFireTime);
		if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedRate; }
		return;
	}
	LastAcceptedFireTime = LocalNow;
	if (bCollectStats) { ++TraceShotStats::GStats.ServerShotsAccepted; }

	// ---- rewind window -------------------------------------------------------------------
	const double ServerNow = GetServerTimeSeconds();
	double RewindTime = ServerNow;
	if (Settings.bEnableLagCompensation && ClientFireServerTime > 0.f)
	{
		// Clamping is what bounds the exploit: however stale or futuristic the client's stamp is,
		// we only ever look back at most MaxRewindTime and never forward at all.
		const double MaxRewind = FMath::Max(0.f, Settings.MaxRewindTime);
		RewindTime = FMath::Clamp(static_cast<double>(ClientFireServerTime), ServerNow - MaxRewind, ServerNow);
	}

	// ---- muzzle sanity, measured against where the shooter *was* --------------------------
	FVector ShotOrigin(Origin);
	FVector ReferencePoint = Character->GetMuzzleLocation();
	if (const UTraceLagCompensationComponent* ShooterLagComp = Character->FindComponentByClass<UTraceLagCompensationComponent>())
	{
		FTraceLagCompFrame ShooterFrame;
		if (ShooterLagComp->GetPoseAtTime(static_cast<float>(RewindTime), ShooterFrame))
		{
			// Comparing against the shooter's live position would punish anyone with latency, since
			// they legitimately fired from where they used to be.
			ReferencePoint = ShooterFrame.CapsuleCenter;
		}
	}

	if (FVector::DistSquared(ShotOrigin, ReferencePoint) > MaxOriginErrorUU * MaxOriginErrorUU)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: implausible muzzle from %s, snapping to server pose"), *GetNameSafe(OwnerActor));
		ShotOrigin = ReferencePoint;
	}

	// ---- resolve -------------------------------------------------------------------------
	const float ShotRange = FMath::Max(1.f, Settings.HitscanRange);
	FVector ImpactPoint = ShotOrigin + Dir * ShotRange;
	ETraceHitZone Zone = ETraceHitZone::None;
	FTraceHitscanDiagnostics Diagnostics;
	ATraceCharacter* Victim = UTraceLagCompensationComponent::ResolveHitscan(
		World,
		Character,
		ShotOrigin,
		Dir,
		ShotRange,
		static_cast<float>(RewindTime),
		ImpactPoint,
		Zone,
		bCollectStats ? &Diagnostics : nullptr);

	if (bCollectStats)
	{
		AccumulateShotStats(Zone, Victim, Diagnostics);
	}

	bool bKilled = false;
	if (Victim != nullptr)
	{
		if (UTraceHealthComponent* VictimHealth = Victim->FindComponentByClass<UTraceHealthComponent>())
		{
			// Spec section 6: head 100 / body 40 / legs 25. No multiplier, no base damage - the zone
			// IS the damage. UTraceSettings::HitscanDamage and HeadshotMultiplier are no longer read
			// by the weapon (see the report); the numbers live in UTraceDamageSettings.
			const float Damage = FTraceHitZoneModel::DamageForZone(Zone);

			// SPEC v8 §6, the kill feed's headshot icon. The zone is known EXACTLY here and nowhere
			// after: ApplyDamage takes a cause and the health component clamps at zero, so a head
			// shot and a shin shot both arrived at the death handler as plain "Bullet". The feed was
			// reduced to inferring it from the victim's previous health (PreviousHealth > BodyDamage
			// ⇒ provably a head shot), which under-reports — a head shot on a victim already down to
			// 20 was indistinguishable from a body shot and drew the plain round.
			//
			// Naming the zone at the one site that still knows it costs nothing and makes the icon
			// exact in both directions. The feed already accepts this name; every other cause is
			// unchanged, so nothing else in the taxonomy moves.
			const FName DamageCause = (Zone == ETraceHitZone::Head) ? FName(TEXT("Headshot")) : FName(TEXT("Bullet"));
			VictimHealth->ApplyDamage(Damage, Character->GetController(), DamageCause);

			// ApplyDamage no-ops against an invulnerable target, so read the result rather than
			// assuming the hit landed.
			bKilled = !VictimHealth->IsAlive();
		}

		if (ATracePlayerController* ShooterController = Cast<ATracePlayerController>(Character->GetController()))
		{
			// The zone rides along so the shooter's hitmarker can say WHICH zone paid out. Positional
			// damage is only learnable if the feedback is positional too.
			ShooterController->ClientNotifyHit(bKilled, Zone);
		}
	}

	// ---- predicted-vs-authoritative agreement check (dev instrumentation) -------------------
	// Only meaningful when this same process also ran the predicted trace, i.e. the shooter is
	// locally controlled here (single player, or the listen host's own pawn). A remote client's
	// prediction lives in another process and cannot be compared from inside this one.
	if (CVarTraceDebugHitZones.GetValueOnGameThread() != 0 && Character->IsLocallyControlled())
	{
		const bool bSameVictim = (LastPredictedVictim.Get() == Victim);
		const bool bSameZone = (LastPredictedZone == Zone);
		UE_LOG(LogTraceGame, Display,
			TEXT("HITZONE %s  %s: predicted %s on %s | server %s on %s (damage %.0f, rewind %.3fs)"),
			(bSameVictim && bSameZone) ? TEXT("AGREE   ") : TEXT("DISAGREE"),
			*GetNameSafe(Character),
			TraceHitZoneToString(LastPredictedZone), *GetNameSafe(LastPredictedVictim.Get()),
			TraceHitZoneToString(Zone), *GetNameSafe(Victim),
			FTraceHitZoneModel::DamageForZone(Zone),
			ServerNow - RewindTime);
	}

	// Unreliable and cosmetic: everyone but the shooter draws the railgun beam.
	const bool bImpacted = FVector::DistSquared(ShotOrigin, ImpactPoint) < static_cast<double>(ShotRange) * ShotRange * 0.998;
	MulticastFireEffects(FVector_NetQuantize(ShotOrigin), FVector_NetQuantize(ImpactPoint), bImpacted);
}

void UTraceWeaponComponent::MulticastFireEffects_Implementation(FVector_NetQuantize Origin, FVector_NetQuantize Impact, bool bImpacted)
{
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	// The shooter drew this tracer the instant it pulled the trigger. Drawing it again here would
	// double up the effect and, worse, draw the server's slightly different ray over the top of the
	// one the player already saw. This is the owner-skipping multicast the design calls for; on a
	// listen server the host's own pawn is locally controlled and is skipped for the same reason.
	if (Character->IsLocallyControlled())
	{
		return;
	}

	PlayLocalTracer(Origin, Impact, bImpacted);
}

#if !UE_BUILD_SHIPPING

// =================================================================================================
// Trace.TestRecoil — the unattended proof for spec v5 section 6.
//
// A screenshot cannot show a recoil pattern and a log line per shot cannot show whether the view
// came back. So this holds the trigger on the local player for a few seconds, samples the control
// rotation and the aim agreement throughout, and prints the four numbers that decide whether the
// feature is right:
//
//   PEAK CLIMB      how far above the original aim sustained fire took the view.
//   RESIDUAL PITCH  where the view sat once recovery had finished. Must land back on ~0.000 or the
//                   gun is stealing aim from the player one burst at a time.
//   YAW DRIFT       must be EXACTLY 0.000. This is the "recoil direction 100" claim, measured: a
//                   purely vertical kick cannot move the yaw, and the model has no yaw term at all.
//   MAX aimErr      angle between the camera's forward vector and GetAimDirection(), sampled while
//                   the view is being driven by recoil. The project's standing guarantee is
//                   0.0000 deg and recoil must not be the thing that breaks it.
//
// It drives the same StartFire/StopFire the input layer calls, so it exercises the shipping path
// rather than a test-only one, and it dumps Trace.ShotStats at the end so the client/server hit
// agreement for the same burst is printed beside the recoil numbers.
// =================================================================================================

namespace TraceRecoilTest
{
	ATraceCharacter* FindLocalTraceCharacter()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* TestWorld = Context.World();
			if (TestWorld == nullptr || !TestWorld->IsGameWorld())
			{
				continue;
			}

			if (APlayerController* LocalController = TestWorld->GetFirstPlayerController())
			{
				if (ATraceCharacter* LocalCharacter = Cast<ATraceCharacter>(LocalController->GetPawn()))
				{
					return LocalCharacter;
				}
			}
		}

		return nullptr;
	}

	struct FState
	{
		double Elapsed = 0.0;
		double FireSeconds = 1.6;
		double TotalSeconds = 4.0;
		double NextSampleAt = 0.0;

		bool bStarted = false;
		bool bStopped = false;

		double StartPitch = 0.0;
		double StartYaw = 0.0;
		double PeakClimb = 0.0;
		double MaxAimError = 0.0;
		int32 Samples = 0;
	};

	void Run(float FireSeconds, float SettleSeconds, float DelaySeconds)
	{
		TSharedRef<FState> State = MakeShared<FState>();
		State->FireSeconds = FMath::Max(0.1f, FireSeconds);
		State->TotalSeconds = State->FireSeconds + FMath::Max(0.5f, SettleSeconds);

		UE_LOG(LogTraceGame, Display,
			TEXT("[RecoilTest] in %.1fs: hold the trigger for %.2fs, then measure %.2fs of recovery."),
			DelaySeconds, State->FireSeconds, State->TotalSeconds - State->FireSeconds);

		// TWO TICKERS, AND THE SECOND ONE IS THE BUG FIX. FTSTicker::AddTicker's delay applies to
		// EVERY invocation, not just the first, so arming the sampler directly with a delay of 8
		// makes it run once every eight seconds — the first measured run held the trigger for thirty
		// seconds and never reached its own StopFire. So the delay arms a one-shot that then arms the
		// real per-frame sampler with no delay at all.
		//
		// The delay itself is what lets one unattended run take several samples at different points
		// in a match — one while the field is still empty, a later one mid-fight — instead of betting
		// the whole measurement on whichever two seconds the harness happened to pick.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float /*Delta*/) -> bool
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float DeltaTime) -> bool
			{
				ATraceCharacter* LocalCharacter = FindLocalTraceCharacter();
				UTraceWeaponComponent* Weapon = (LocalCharacter != nullptr)
					? LocalCharacter->FindComponentByClass<UTraceWeaponComponent>() : nullptr;
				APlayerController* LocalController = (LocalCharacter != nullptr)
					? Cast<APlayerController>(LocalCharacter->GetController()) : nullptr;

				if (Weapon == nullptr || LocalController == nullptr)
				{
					// No pawn yet (still in warmup, or on the menu map). Wait rather than reporting
					// a zero, which would read as "recoil does nothing".
					return true;
				}

				const FRotator ControlRotation = LocalController->GetControlRotation();
				const double Pitch = FRotator::NormalizeAxis(ControlRotation.Pitch);
				const double Yaw = FRotator::NormalizeAxis(ControlRotation.Yaw);

				if (!State->bStarted)
				{
					// POINT AT SOMEBODY FIRST. A burst fired at empty air proves the recoil model but
					// says nothing about the thing that actually has to survive it — the client and the
					// server resolving the same shot against the same body. Aimed once, at the start,
					// and deliberately never re-aimed: the whole point is that the climb then walks the
					// muzzle off the target, which is what recoil IS.
					//
					// The candidate is not merely the NEAREST enemy, it is the nearest one the shot
					// would actually reach. The first version took the nearest and measured ten
					// consecutive misses at 100% world truncation: on a field this dense the closest
					// body is usually behind a cover box, and "the client and the server agree that
					// nobody was hit" is a much weaker statement than the guarantee this is here to
					// re-measure. Candidates are therefore tested with the SAME resolver the shot
					// uses, so a target is only chosen if a round sent at it lands on it.
					const ATraceCharacter* AimTarget = nullptr;
					double BestDistanceSq = TNumericLimits<double>::Max();
					if (UWorld* TestWorld = LocalCharacter->GetWorld())
					{
						const FVector EyeLocation = LocalCharacter->GetPawnViewLocation();
						const float ProbeRange = FMath::Max(1.f, UTraceSettings::Get().HitscanRange);

						for (TActorIterator<ATraceCharacter> It(TestWorld); It; ++It)
						{
							ATraceCharacter* Candidate = *It;
							if (Candidate == nullptr || Candidate == LocalCharacter || !Candidate->IsAlive())
							{
								continue;
							}

							const double DistanceSq = FVector::DistSquared(
								Candidate->GetActorLocation(), LocalCharacter->GetActorLocation());
							if (DistanceSq >= BestDistanceSq)
							{
								continue;
							}

							const FVector CandidateChest = Candidate->GetActorLocation() + FVector(0.0, 0.0, 40.0);
							const FVector ProbeDirection = (CandidateChest - EyeLocation).GetSafeNormal();
							if (ProbeDirection.IsNearlyZero())
							{
								continue;
							}

							FVector ProbeImpact = FVector::ZeroVector;
							ETraceHitZone ProbeZone = ETraceHitZone::None;
							const ATraceCharacter* WouldHit = UTraceLagCompensationComponent::ResolveHitscan(
								TestWorld, LocalCharacter, EyeLocation, ProbeDirection, ProbeRange,
								0.f, ProbeImpact, ProbeZone);

							if (WouldHit == Candidate)
							{
								BestDistanceSq = DistanceSq;
								AimTarget = Candidate;
							}
						}
					}

					if (AimTarget != nullptr)
					{
						const FVector Chest = AimTarget->GetActorLocation() + FVector(0.0, 0.0, 40.0);
						LocalController->SetControlRotation((Chest - LocalCharacter->GetPawnViewLocation()).Rotation());
						UE_LOG(LogTraceGame, Display,
							TEXT("[RecoilTest] aiming at %s, %.0fuu away"),
							*GetNameSafe(AimTarget), FMath::Sqrt(BestDistanceSq));
					}
					else
					{
						UE_LOG(LogTraceGame, Display,
							TEXT("[RecoilTest] no living target in the world; firing at open air (hit agreement will have nothing to compare)."));
					}

					State->StartPitch = FRotator::NormalizeAxis(LocalController->GetControlRotation().Pitch);
					State->StartYaw = FRotator::NormalizeAxis(LocalController->GetControlRotation().Yaw);
					State->bStarted = true;
					Weapon->StartFire();
					UE_LOG(LogTraceGame, Display,
						TEXT("[RecoilTest] start: pitch %+.3f yaw %+.3f"), State->StartPitch, State->StartYaw);
					return true;   // sample from the next frame, once the aim has settled
				}

				State->Elapsed += DeltaTime;

				const double Climb = Pitch - State->StartPitch;
				State->PeakClimb = FMath::Max(State->PeakClimb, Climb);

				double AimError = 0.0;
				if (LocalCharacter->Camera != nullptr)
				{
					const FVector CameraForward = LocalCharacter->Camera->GetForwardVector();
					const FVector AimDirection = LocalCharacter->GetAimDirection();
					AimError = FMath::RadiansToDegrees(FMath::Acos(
						FMath::Clamp(FVector::DotProduct(CameraForward, AimDirection), -1.0, 1.0)));
					State->MaxAimError = FMath::Max(State->MaxAimError, AimError);
					++State->Samples;
				}

				if (State->Elapsed >= State->NextSampleAt)
				{
					State->NextSampleAt = State->Elapsed + 0.1;
					UE_LOG(LogTraceGame, Display,
						TEXT("[RecoilTest] t=%.2f %s pitch=%+.3f climb=%+.3f yawDrift=%+.4f aimErr=%.4fdeg"),
						State->Elapsed, State->bStopped ? TEXT("RECOVER") : TEXT("FIRING "),
						Pitch, Climb, Yaw - State->StartYaw, AimError);
				}

				if (!State->bStopped && State->Elapsed >= State->FireSeconds)
				{
					State->bStopped = true;
					Weapon->StopFire();
				}

				if (State->Elapsed < State->TotalSeconds)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display, TEXT("========== TRACE RECOIL TEST =========="));
				UE_LOG(LogTraceGame, Display,
					TEXT("RECOIL peak climb   : %+.3f deg above the original aim"), State->PeakClimb);
				UE_LOG(LogTraceGame, Display,
					TEXT("RECOIL residual     : %+.4f deg (must settle to ~0.000 — the gun must not keep the aim)"),
					Pitch - State->StartPitch);
				UE_LOG(LogTraceGame, Display,
					TEXT("RECOIL yaw drift    : %+.4f deg (MUST be 0.0000 — 'recoil direction 100' is purely vertical)"),
					Yaw - State->StartYaw);
				UE_LOG(LogTraceGame, Display,
					TEXT("RECOIL max aimErr   : %.4f deg over %d samples (crosshair vs bullet; the standing guarantee is 0.0000)"),
					State->MaxAimError, State->Samples);
				UE_LOG(LogTraceGame, Display, TEXT("======================================="));

				TraceShotStats::Dump();
				return false;   // the sampler is done
			}));

				return false;   // the delay shot is done; the sampler above is now armed
			}), DelaySeconds);
	}

	FAutoConsoleCommand CmdTestRecoil(
		TEXT("Trace.TestRecoil"),
		TEXT("Dev only. Trace.TestRecoil [FireSeconds] [SettleSeconds] [DelaySeconds] — hold the local player's trigger, then report peak climb, residual pitch, yaw drift and aimErr."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float FireSeconds = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 1.6f;
			const float SettleSeconds = (Args.Num() > 1) ? FCString::Atof(*Args[1]) : 2.4f;
			const float DelaySeconds = (Args.Num() > 2) ? FMath::Max(0.f, FCString::Atof(*Args[2])) : 0.f;
			Run(FireSeconds, SettleSeconds, DelaySeconds);
		}));
}

#endif // !UE_BUILD_SHIPPING
