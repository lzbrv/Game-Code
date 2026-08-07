#include "Gameplay/TraceWeaponComponent.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"      // EFirstPersonPrimitiveType (the knife viewmodel)
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"   // the knife in the third-person hand
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"                       // TActorIterator, for the recoil test harness
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"   // recoil: the camera boom's tick order
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"                 // TNumericLimits<double>
#include "Math/UnrealMathUtility.h"
#include "Net/UnrealNetwork.h"                  // DOREPLIFETIME

#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Movement/TraceCharacterMovementComponent.h"   // SetKnifeMovementProfileActive (spec v10 §1)
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceHitZones.h"
#include "Gameplay/TraceMelee.h"
#include "Gameplay/TraceMeleeArc.h"
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
	// TICKS ALWAYS, AS OF THE KNIFE (spec v10 §1). It used to tick only while the trigger was held
	// on the machine owning the input, and that is no longer sufficient in three separate ways:
	//
	//   * the SERVER has to sample this pawn's facing every frame, or a back-stab cannot be judged
	//     against the yaw the attacker actually saw (GetFacingYawAtTime);
	//   * the SWING has a wind-up, so the blade resolves on a later frame than the press;
	//   * EVERY rendering machine has to keep the knife rigs' visibility in step with a replicated
	//     selector, including the machines watching somebody else's pawn.
	//
	// The cost is one early-outing tick per pawn per frame, which is what
	// UTraceLagCompensationComponent already pays on the same actor.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// Replicated for its RPCs to route AND, since the knife, for two properties of its own:
	// EquippedWeapon and DeployEndServerTime.
	SetIsReplicatedByDefault(true);
}

void UTraceWeaponComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// TO EVERYONE, not COND_SkipOwner, and both for the same reason: the movement component reads
	// the selector on every machine to apply the knife's +30% ground speed, so a client that did not
	// receive its own value would be corrected into the speed instead of predicting it. The owner
	// having predicted the same value already makes the update a no-op, not a fight.
	DOREPLIFETIME(UTraceWeaponComponent, EquippedWeapon);
	DOREPLIFETIME(UTraceWeaponComponent, DeployEndServerTime);
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

	// --- SPEC v10 §1: the knife is out, so the gun is not ---------------------------------------
	if (IsKnifeEquipped())
	{
		return false;
	}

	// --- SPEC v10 §1: 0.2s of pullout, during which NEITHER weapon works -------------------------
	if (IsDeploying())
	{
		return false;
	}

	// --- SPEC v10 §6: "Don't let players shoot while in a dash animation. As soon as they end the
	//     dash, let them shoot again." ------------------------------------------------------------
	//
	// A GATE, NOT A COOLDOWN, and the distinction is the whole of the request: this is a pure
	// function of IsDashing(), so it opens on the exact frame the dash state clears. There is no
	// timer, nothing to expire, and nothing that could hold the trigger shut for a frame afterwards.
	//
	// The same gate applies to the knife (CanSwing) — a swing during a dash would be the same
	// exploit wearing a different weapon, and spec §1 folds the two together. Both gates go through
	// ATraceCharacter::AreWeaponActionsBlocked, which is the movement slice's own named accessor, so
	// there is ONE definition of "the dash blocks the trigger" rather than two IsDashing() calls
	// that can drift apart.
	if (Character->AreWeaponActionsBlocked())
	{
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

	// SPEC v10 §1 — MOUSE1 IS "ATTACK", NOT "SHOOT".
	//
	// Dispatching here rather than at the input layer is what let the knife land without touching
	// ATraceCharacter, ATracePlayerController or ATraceBotController: DoFirePressed already routes
	// here, so the knife inherits the human bind, the bot burst logic and the dead-player "put me
	// back in" path unchanged. The trigger stays HELD either way, so the tick below repeats a swing
	// at the 0.5 s cadence exactly as it repeats a shot at the fire interval.
	if (IsKnifeEquipped())
	{
		ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
		if (!StartSwing(&Refusal) && TraceMelee::IsDebugLoggingEnabled())
		{
			UE_LOG(LogTraceGame, Display, TEXT("[Knife] %s swing refused on press: %s"),
				*GetNameSafe(Character), LexToString(Refusal));
		}
		return;
	}

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
	// The tick is NOT switched off here, and as of the knife it is never switched off at all — see
	// the constructor. Recoil recovery already needed the component to keep ticking past the
	// release; the facing ring, the swing wind-up and the knife rigs' visibility need it to tick on
	// machines that never pressed anything.
	bTriggerHeld = false;
}

void UTraceWeaponComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		// The pawn is gone: genuine teardown, drop everything. The climb goes with it — there is no
		// view left to hand back to.
		bTriggerHeld = false;
		bSwingPendingResolve = false;
		ResetRecoil();
		SetComponentTickEnabled(false);
		return;
	}

	const AActor* OwnerActor = GetOwner();

	// --- SERVER: the victim-facing ring (spec v10 §1) --------------------------------------------
	//
	// Every frame, on every pawn, including remote clients' proxies and bots. This is the ONE piece
	// of history the knife needed that the gun's lag compensation does not keep — see
	// GetFacingYawAtTime. Eight bytes per frame per pawn.
	if (OwnerActor != nullptr && OwnerActor->HasAuthority())
	{
		RecordFacingSample(static_cast<float>(GetServerTimeSeconds()));
		TickBotKnife();
	}

	// --- EVERY MACHINE: the knife's movement profile ---------------------------------------------
	//
	// Deliberately before the locally-controlled gate and unconditional. The bit has to be right on
	// the server, on the owning client that predicts its own moves, AND on the machines simulating
	// this pawn — see RefreshMovementProfile. It is idempotent and only writes on a change.
	RefreshMovementProfile();

	// --- EVERY RENDERING MACHINE: the knife you can see ------------------------------------------
	//
	// Deliberately BEFORE the locally-controlled gate. The third-person knife in an enemy's hand is
	// the tell that they are 30% faster and cannot shoot back, and that tell has to appear on the
	// machines watching them, none of which control that pawn.
	UpdateKnifeVisuals(DeltaTime);

	if (!Character->IsLocallyControlled())
	{
		// Not ours: no input, no recoil, no swing. The climb goes with it.
		bTriggerHeld = false;
		bSwingPendingResolve = false;
		ResetRecoil();
		return;
	}

	if (!Character->IsAlive())
	{
		// Dead. Forget the climb rather than recovering it: the death camera owns the view now, and
		// a recovery that keeps writing control rotation would fight it for the whole respawn. A
		// swing that was mid-wind-up dies with the swinger for the same reason.
		ResetRecoil();
		bSwingPendingResolve = false;
	}
	else
	{
		TickRecoil(DeltaTime);
	}

	// The blade of a swing already committed to. Runs before the trigger check below so a held
	// trigger cannot start the next swing in the same frame the previous one lands.
	TickSwing(DeltaTime);

	// Attacking is gated separately from recoil, and deliberately does NOT clear bTriggerHeld while
	// ineligible: this component only hears about the trigger on press and release, so clearing it
	// means a player who is still physically holding the button gets nothing after passing the Core
	// away or after respawning — they would have to release and press again for no reason they can
	// see. Keep ticking, skip attacking, and resume the instant the gate reopens.
	if (bTriggerHeld && Character->IsAlive() && !Character->IsCarrier())
	{
		if (IsKnifeEquipped())
		{
			if (CanSwing())
			{
				StartSwing();
			}
		}
		else if (CanFire())
		{
			FireOnce();
		}
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

// =================================================================================================
// THE KNIFE  (spec v10 §1)
//
// Gameplay/TraceMelee.h is the design document — the carrier immunity, the back/front angle model,
// why the swing is a swept arc and why the yaw ring exists. This section is the STATE MACHINE:
//
//     press  -> StartSwing        gates, cooldown stamp, animation starts
//     +0.10s -> TickSwing         the blade resolves; the client predicts and sends ServerSwing
//     server -> ServerSwing       re-validates everything, rewinds, applies the damage
//     +0.50s -> the next swing is legal, measured from the PRESS
//
// and the swap, which is one replicated selector plus one replicated deadline.
// =================================================================================================

bool UTraceWeaponComponent::IsDeploying() const
{
	return GetServerTimeSeconds() < static_cast<double>(DeployEndServerTime);
}

float UTraceWeaponComponent::GetDeployRemaining() const
{
	return static_cast<float>(FMath::Max(0.0, static_cast<double>(DeployEndServerTime) - GetServerTimeSeconds()));
}

float UTraceWeaponComponent::GetSwingCooldownRemaining() const
{
	// The LOCAL clock, exactly as the fire-rate gate uses it, and for the same reason: a resync of
	// the shared clock must never be able to stall a weapon. The shared clock is for rewinding, not
	// for gating.
	const double Elapsed = GetLocalTimeSeconds() - LastLocalSwingTime;
	return static_cast<float>(FMath::Max(0.0, static_cast<double>(TraceMelee::GetSwingCooldownSeconds()) - Elapsed));
}

bool UTraceWeaponComponent::CanSwing(ETraceMeleeRefusal* OutRefusal) const
{
	if (OutRefusal != nullptr)
	{
		*OutRefusal = ETraceMeleeRefusal::None;
	}

	// One lambda so every refusal reports itself. A melee that silently does nothing is
	// indistinguishable from broken input — the exact failure the gun's carrier gate cost real
	// debugging time over.
	auto Refuse = [OutRefusal](ETraceMeleeRefusal Reason) -> bool
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = Reason;
		}
		return false;
	};

	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return Refuse(ETraceMeleeRefusal::NoPawn);
	}
	if (!Character->IsAlive())
	{
		return Refuse(ETraceMeleeRefusal::Dead);
	}
	if (Character->IsCarrier())
	{
		// The carrier cannot shoot (spec §4) and cannot swing, for the same reason and by the same
		// rule. See TraceMelee.h — this is the attacking half of the carrier's bargain, and the
		// defending half (the knife cannot HURT a carrier either) lives in TraceMelee::ResolveSwing.
		return Refuse(ETraceMeleeRefusal::Carrying);
	}
	if (!IsKnifeEquipped())
	{
		return Refuse(ETraceMeleeRefusal::WrongWeapon);
	}
	if (IsDeploying())
	{
		return Refuse(ETraceMeleeRefusal::Deploying);
	}
	if (Character->AreWeaponActionsBlocked())
	{
		// Spec §6, extended to the knife by §1's own note. A gate, not a cooldown: it opens on the
		// frame the dash state clears. Same accessor as CanFire — see there.
		return Refuse(ETraceMeleeRefusal::Dashing);
	}
	if (GetSwingCooldownRemaining() > 0.f)
	{
		return Refuse(ETraceMeleeRefusal::OnCooldown);
	}

	return true;
}

bool UTraceWeaponComponent::StartSwing(ETraceMeleeRefusal* OutRefusal)
{
	if (!CanSwing(OutRefusal))
	{
		return false;
	}

	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsLocallyControlled())
	{
		// Input is a local concept, exactly as it is for the trigger: a proxy copy of somebody
		// else's pawn must never swing. Note a bot IS locally controlled on the server, which is
		// how bots swing at all.
		if (OutRefusal != nullptr)
		{
			*OutRefusal = ETraceMeleeRefusal::NotLocallyControlled;
		}
		return false;
	}

	const double Now = GetLocalTimeSeconds();

	// THE COOLDOWN IS STAMPED AT THE PRESS, not at the resolve. The user's number is "0.5 seconds
	// after a knife swing before a player can knife again", and press-to-press is the interval a
	// player can actually observe — stamping at the resolve would make the real cadence 0.5 +
	// wind-up and no amount of tuning the cooldown would produce a 0.5 s rhythm.
	LastLocalSwingTime = Now;
	SwingAnimStartLocalTime = Now;
	SwingResolveAtLocalTime = Now + static_cast<double>(TraceMelee::GetSwingWindupSeconds());
	bSwingPendingResolve = true;

	if (TraceMelee::IsDebugLoggingEnabled())
	{
		UE_LOG(LogTraceGame, Display, TEXT("[Knife] %s swing START (blade resolves in %.3fs, next swing in %.3fs)"),
			*GetNameSafe(Character), TraceMelee::GetSwingWindupSeconds(), TraceMelee::GetSwingCooldownSeconds());
	}

	return true;
}

void UTraceWeaponComponent::TickSwing(float /*DeltaTime*/)
{
	if (!bSwingPendingResolve)
	{
		return;
	}
	if (GetLocalTimeSeconds() < SwingResolveAtLocalTime)
	{
		return;
	}
	bSwingPendingResolve = false;

	ATraceCharacter* Character = GetTraceCharacter();
	UWorld* World = GetWorld();
	if (Character == nullptr || World == nullptr)
	{
		return;
	}

	const FVector Origin = Character->GetMuzzleLocation();
	FVector Dir = Character->GetAimDirection().GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = Character->GetActorForwardVector();
	}

	// THE STAMP IS THE RESOLVE INSTANT, not the press. This is the moment the client saw the edge
	// cross the target, so it is the moment the server has to rewind to — stamping the press would
	// rewind the world to before the blade had started moving and hand the attacker a free
	// wind-up's worth of the victim's old position.
	const double SwingServerTime = GetServerTimeSeconds();

	// Cosmetic-only local resolve, exactly as FireOnce does one for the tracer: no damage is applied
	// on the client under any circumstances. On a client the lag-comp histories are empty, so this
	// resolves against the live interpolated poses the player can actually see — which is the right
	// answer for an effect.
	FTraceMeleeHit Predicted;
	TraceMelee::ResolveSwing(World, Character, Origin, Dir, static_cast<float>(SwingServerTime), Predicted);

	// The swinger draws their own slash immediately; MulticastSwingEffects skips them for it, the
	// same owner-skipping contract the tracer has.
	ATraceMeleeArc::Spawn(World, Origin, Dir, TraceMelee::GetSwingAxis(Character, Dir),
		TraceMelee::GetSwingArcDegrees(), TraceMelee::GetSwingRangeUU(),
		TraceTeamColor(Character->GetTeam()), Predicted.Victim != nullptr);

	ServerSwing(FVector_NetQuantize(Origin), FVector_NetQuantizeNormal(Dir), static_cast<float>(SwingServerTime));
}

// -------------------------------------------------------------------------------------------------
// The swap
// -------------------------------------------------------------------------------------------------

void UTraceWeaponComponent::DoSwapWeaponPressed()
{
	const ETraceEquippedWeapon Desired = IsKnifeEquipped()
		? ETraceEquippedWeapon::Gun
		: ETraceEquippedWeapon::Knife;

	ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
	if (!RequestEquip(Desired, &Refusal))
	{
		// A bind that is silent BY DESIGN is indistinguishable from a bind that is not wired up, and
		// this project has already paid for that lesson once with the carrier's gun.
		UE_LOG(LogTraceGame, Verbose, TEXT("[%s] Weapon swap refused: %s"),
			*GetNameSafe(GetOwner()), LexToString(Refusal));
	}
}

bool UTraceWeaponComponent::RequestEquip(ETraceEquippedWeapon Desired, ETraceMeleeRefusal* OutRefusal)
{
	if (OutRefusal != nullptr)
	{
		*OutRefusal = ETraceMeleeRefusal::None;
	}

	auto Refuse = [OutRefusal](ETraceMeleeRefusal Reason) -> bool
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = Reason;
		}
		return false;
	};

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return Refuse(ETraceMeleeRefusal::NoPawn);
	}
	if (!Character->IsAlive())
	{
		return Refuse(ETraceMeleeRefusal::Dead);
	}
	if (Character->IsCarrier())
	{
		// HANDS FULL. The Core is carried in both hands and in third person; a carrier who could put
		// a knife away and take it back out would be choosing between two weapons neither of which
		// they may use, and would be collecting the +30% movement bonus while doing it. See
		// TraceMelee.h. The weapon they were holding is untouched and comes back when they let go.
		return Refuse(ETraceMeleeRefusal::Carrying);
	}

	// --- THERE IS DELIBERATELY NO DASH GATE HERE, and that is a decision, not an omission. --------
	//
	// CanFire and CanSwing both refuse mid-dash (spec §6). A SWAP is neither: it fires no weapon, and
	// refusing it would mean a player who dashes in has to wait out the dash AND then the 0.2 s
	// pullout before the knife is up — a dash that costs 0.2 s of extra helplessness, which is the
	// opposite of what a mobility weapon is for.
	//
	// Checked for the three ways it could be an exploit, and it is none of them:
	//   * SHOOTING. A swap mid-dash cannot produce a shot mid-dash, because CanFire's own gate is
	//     independent of this one and still closed for the whole dash.
	//   * FREE SPEED. The knife's movement profile turns on at the press rather than at the end of
	//     the pullout, so a mid-dash swap does raise the air-strafe ceilings during the dash. It
	//     grants nothing, because those ceilings are a property of HOLDING the knife: pressing F a
	//     second before the dash produces the identical state. There is no window here that
	//     pre-swapping does not already open, and the dash's own speed is untouched either way
	//     (UTraceCharacterMovementComponent::GetMaxSpeed returns GetDashSpeed() before any
	//     multiplier is reached).
	//   * SWING CANCELLING. ApplyEquip drops a pending swing, but LastLocalSwingTime was stamped at
	//     the PRESS, so swapping out of a wind-up forfeits the swing and still pays the full 0.5 s
	//     cooldown. Cancelling is strictly worse than not cancelling; there is nothing to farm.
	//
	// The dash and the pullout simply overlap, and both clocks run to completion on their own.
	// ---------------------------------------------------------------------------------------------

	const AActor* OwnerActor = GetOwner();

	// The instant of the PRESS, on the shared clock. Both machines anchor the pullout to this, so
	// they compute the same deadline and the replicated value is a no-op rather than an extension.
	const double PressServerTime = GetServerTimeSeconds();
	const double DeployEnd = PressServerTime + static_cast<double>(TraceMelee::GetSwapSeconds());

	if (OwnerActor != nullptr && OwnerActor->HasAuthority())
	{
		// The server, or a listen host's own pawn, or a bot: no prediction needed, no RPC to send.
		ApplyEquip(Desired, DeployEnd);
		return true;
	}

	if (!Character->IsLocallyControlled())
	{
		return Refuse(ETraceMeleeRefusal::NotLocallyControlled);
	}

	// PREDICTED, exactly as the tracer is, AND ANCHORED so the prediction is right rather than
	// merely early. A pullout that waits for a round trip is a 0.2 s feature that costs 0.2 s + RTT;
	// a pullout the server re-anchors on arrival is a 0.2 s feature that costs 0.2 s + upstream lag,
	// which is what this measured at 0.294 s before the stamp existed. Sending the press instant and
	// having the server clamp-and-anchor it removes the lag from the number entirely. See
	// ServerRequestEquip's comment for the security argument, which is the shot's, unchanged.
	ApplyEquip(Desired, DeployEnd);
	ServerRequestEquip(Desired, static_cast<float>(PressServerTime));
	return true;
}

void UTraceWeaponComponent::ApplyEquip(ETraceEquippedWeapon Desired, double DeployEndSharedTime)
{
	const ETraceEquippedWeapon Previous = EquippedWeapon;

	EquippedWeapon = Desired;
	DeployEndServerTime = static_cast<float>(DeployEndSharedTime);

	// A swap cancels a swing that has not resolved yet. The alternative — letting the blade land
	// after the knife has been put away — is a hit from a weapon that is visibly not in the
	// player's hands, which is the least defensible thing a melee can do.
	bSwingPendingResolve = false;
	SwingAnimStartLocalTime = -1000.0;

	// The server does not receive its own OnRep, and on a listen host the presentation must still
	// follow. Calling it directly is what keeps the two paths identical.
	OnRep_EquippedWeapon();

	if (TraceMelee::IsDebugLoggingEnabled())
	{
		UE_LOG(LogTraceGame, Display, TEXT("[Knife] %s equip %s -> %s (pullout %.3fs, ends at shared t=%.3f)"),
			*GetNameSafe(GetOwner()), LexToString(Previous), LexToString(Desired),
			TraceMelee::GetSwapSeconds(), DeployEndServerTime);
	}
}

void UTraceWeaponComponent::OnRep_EquippedWeapon()
{
	// Which rig is visible is re-decided every tick from the replicated selector
	// (UpdateKnifeVisuals), so all this has to do about presentation is make sure a swap cannot
	// leave a half-swung blade frozen mid-arc on the machine that just learned about it.
	SwingAnimStartLocalTime = -1000.0;

	// The movement profile, on the very frame the selector changed rather than on the next tick. On
	// a simulated proxy that is what keeps the pawn's simulated speed matching the one the server
	// moved it at; on the owning client the predicted equip already called this, so it is a no-op.
	RefreshMovementProfile();
}

void UTraceWeaponComponent::RefreshMovementProfile()
{
	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	if (UTraceCharacterMovementComponent* Movement = Character->GetTraceMovement())
	{
		// TraceMelee::ShouldUseKnifeMovementProfile is the single definition of the answer, so the
		// movement component, the HUD and a test all ask the same question. It folds in the carrier
		// clause; see its comment for why a carrier holding a knife is holding a STOWED knife.
		const bool bActive = TraceMelee::ShouldUseKnifeMovementProfile(Character);
		if (Movement->IsKnifeMovementProfileActive() != bActive)
		{
			Movement->SetKnifeMovementProfileActive(bActive);

			if (TraceMelee::IsDebugLoggingEnabled())
			{
				UE_LOG(LogTraceGame, Display, TEXT("[Knife] %s movement profile -> %s"),
					*GetNameSafe(Character), bActive ? TEXT("KNIFE (fast)") : TEXT("base"));
			}
		}
	}
}

bool UTraceWeaponComponent::ServerRequestEquip_Validate(ETraceEquippedWeapon Desired, float ClientPressServerTime)
{
	// Validation failure disconnects the client, so this only rejects what is outright impossible to
	// reason about — a uint8 outside the enum, which no honest client can produce, and a stamp that
	// is not a number at all. A merely WRONG stamp is not a kick: it is clamped below, exactly as a
	// wrong shot timestamp is, because an honest client with a bad clock is not a cheater.
	if (!FMath::IsFinite(ClientPressServerTime))
	{
		return false;
	}
	return Desired == ETraceEquippedWeapon::Gun || Desired == ETraceEquippedWeapon::Knife;
}

void UTraceWeaponComponent::ServerRequestEquip_Implementation(ETraceEquippedWeapon Desired, float ClientPressServerTime)
{
	const AActor* OwnerActor = GetOwner();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || Character == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}

	// The SAME gates the client applied, re-asked here. The client's claim is only ever "I pressed
	// swap"; whether that is legal is decided once, here.
	if (!Character->IsAlive() || Character->IsCarrier())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerRequestEquip: refusing %s for %s"),
			LexToString(Desired), *GetNameSafe(OwnerActor));
		return;
	}

	// ANCHOR THE PULLOUT AT THE STAMPED PRESS, NOT AT ARRIVAL.
	//
	// Anchoring at arrival is what made a client's measured pullout 0.294 s against a specified
	// 0.2 s: the client started its 0.2 s at the press, the server started ITS 0.2 s when the RPC
	// landed one upstream-lag later, and the replicated deadline then pushed the client's out to
	// match the server's. Both machines now derive the identical shared-clock instant from the same
	// stamp, so the replicated value confirms the client's prediction instead of extending it.
	//
	// THE CLAMP IS THE SECURITY, and it is the shot's clamp unchanged. Whatever the client claims,
	// the press is pinned into [ServerNow - MaxRewindTime, ServerNow]: never in the future, so a
	// swap cannot be pre-booked to complete the instant it is asked for, and never further back than
	// a bullet may be rewound, so a backdated stamp cannot be used to skip the pullout outright. The
	// worst a liar buys is MaxRewindTime of head start, which is the same budget the gun already
	// grants and is bounded by the same setting.
	const UTraceSettings& Settings = UTraceSettings::Get();
	const double ServerNow = GetServerTimeSeconds();
	double PressServerTime = ServerNow;
	if (ClientPressServerTime > 0.f)
	{
		const double MaxRewind = FMath::Max(0.f, Settings.MaxRewindTime);
		PressServerTime = FMath::Clamp(static_cast<double>(ClientPressServerTime), ServerNow - MaxRewind, ServerNow);
	}

	ApplyEquip(Desired, PressServerTime + static_cast<double>(TraceMelee::GetSwapSeconds()));
}

// -------------------------------------------------------------------------------------------------
// The swing, on the authority
// -------------------------------------------------------------------------------------------------

bool UTraceWeaponComponent::ServerSwing_Validate(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientSwingServerTime)
{
	if (Origin.ContainsNaN() || Direction.ContainsNaN())
	{
		return false;
	}
	if (!FMath::IsFinite(ClientSwingServerTime))
	{
		return false;
	}
	return true;
}

void UTraceWeaponComponent::ServerSwing_Implementation(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, float ClientSwingServerTime)
{
	AActor* OwnerActor = GetOwner();
	UWorld* World = GetWorld();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || World == nullptr || Character == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}

	// ---- payload sanity (never check() on network input) ---------------------------------
	FVector Dir(Direction);
	const double DirLengthSq = Dir.SizeSquared();
	if (DirLengthSq < 0.25 || DirLengthSq > 4.0)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: rejecting non-unit direction from %s"), *GetNameSafe(OwnerActor));
		return;
	}
	Dir = Dir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return;
	}
	if (FMath::Abs(Origin.X) > MaxReasonableCoordinateUU || FMath::Abs(Origin.Y) > MaxReasonableCoordinateUU || FMath::Abs(Origin.Z) > MaxReasonableCoordinateUU)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: rejecting out-of-world origin from %s"), *GetNameSafe(OwnerActor));
		return;
	}

	// ---- state gate — the same four rules CanSwing applies on the client ------------------
	// AreWeaponActionsBlocked, not IsDashing, for the same one-definition reason CanSwing uses it —
	// and note this copy is the one that matters: the server's dash clock is authoritative, so a
	// modified client that skipped its own gate is refused here.
	if (!Character->IsAlive() || Character->IsCarrier() || !IsKnifeEquipped() || Character->AreWeaponActionsBlocked())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: state gate refused %s (alive=%d carrier=%d knife=%d dashing=%d)"),
			*GetNameSafe(OwnerActor), Character->IsAlive() ? 1 : 0, Character->IsCarrier() ? 1 : 0,
			IsKnifeEquipped() ? 1 : 0, Character->AreWeaponActionsBlocked() ? 1 : 0);
		return;
	}

	// The pullout, judged on the server's own clock. The client already refused to swing inside it;
	// this is the copy that a modified client cannot skip.
	if (IsDeploying())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: %s is still deploying (%.3fs left)"),
			*GetNameSafe(OwnerActor), GetDeployRemaining());
		return;
	}

	// ---- swing rate, with slack for honest jitter ----------------------------------------
	const double Cooldown = FMath::Max(0.05f, TraceMelee::GetSwingCooldownSeconds());
	const double LocalNow = GetLocalTimeSeconds();
	if ((LocalNow - LastAcceptedSwingTime) < Cooldown * (1.0 - SwingRateTolerance))
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: rate-limited %s (%.3fs since last accepted)"),
			*GetNameSafe(OwnerActor), LocalNow - LastAcceptedSwingTime);
		return;
	}
	LastAcceptedSwingTime = LocalNow;

	// ---- rewind window -------------------------------------------------------------------
	const UTraceSettings& Settings = UTraceSettings::Get();
	const double ServerNow = GetServerTimeSeconds();
	double RewindTime = ServerNow;
	if (Settings.bEnableLagCompensation && ClientSwingServerTime > 0.f)
	{
		// The identical clamp ServerFire applies. However stale or futuristic the client's stamp is,
		// we only ever look back at most MaxRewindTime and never forward at all.
		const double MaxRewind = FMath::Max(0.f, Settings.MaxRewindTime);
		RewindTime = FMath::Clamp(static_cast<double>(ClientSwingServerTime), ServerNow - MaxRewind, ServerNow);
	}

	// ---- blade origin sanity -------------------------------------------------------------
	//
	// TIGHTER THAN THE GUN'S, deliberately. ServerFire forgives 500 uu of muzzle error because a
	// bullet's range is 36000 and 500 uu of slop changes nothing about what it can reach. The blade
	// reaches 180 uu, so the same 500 uu would let a client swing from nearly four blade-lengths
	// away — i.e. it would be the dominant term in the weapon's range rather than a rounding
	// allowance. 200 uu still covers the capsule, the muzzle offset and a couple of frames of
	// movement at knife speed (1040 uu/s is 17 uu per frame at 60 Hz).
	constexpr double MaxSwingOriginErrorUU = 200.0;

	FVector SwingOrigin(Origin);
	FVector ReferencePoint = Character->GetMuzzleLocation();
	if (const UTraceLagCompensationComponent* AttackerLagComp = Character->FindComponentByClass<UTraceLagCompensationComponent>())
	{
		FTraceLagCompFrame AttackerFrame;
		if (AttackerLagComp->GetPoseAtTime(static_cast<float>(RewindTime), AttackerFrame))
		{
			// Comparing against the attacker's LIVE position would punish anyone with latency, who
			// legitimately swung from where they used to be.
			ReferencePoint = AttackerFrame.CapsuleCenter;
		}
	}

	if (FVector::DistSquared(SwingOrigin, ReferencePoint) > MaxSwingOriginErrorUU * MaxSwingOriginErrorUU)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ServerSwing: implausible blade origin from %s, snapping to server pose"),
			*GetNameSafe(OwnerActor));
		SwingOrigin = ReferencePoint;
	}

	// ---- resolve -------------------------------------------------------------------------
	FTraceMeleeHit Hit;
	TraceMelee::ResolveSwing(World, Character, SwingOrigin, Dir, static_cast<float>(RewindTime), Hit);

	bool bKilled = false;
	if (Hit.Victim != nullptr)
	{
		if (UTraceHealthComponent* VictimHealth = Hit.Victim->FindComponentByClass<UTraceHealthComponent>())
		{
			// TWO CAUSES, not one, for the same reason ServerFire passes "Headshot" instead of
			// letting the kill feed infer it from the victim's previous health: the approach angle
			// is known EXACTLY here and nowhere after. ApplyDamage takes a cause and the health
			// component clamps at zero, so a back-stab and a front swipe would otherwise arrive at
			// the death handler as one indistinguishable "Knife".
			const FName Cause = Hit.bBackstab ? TraceMelee::GetBackstabKillCause() : TraceMelee::GetKnifeKillCause();
			VictimHealth->ApplyDamage(Hit.Damage, Character->GetController(), Cause);

			// ApplyDamage no-ops against an invulnerable target, so read the result rather than
			// assuming the hit landed.
			bKilled = !VictimHealth->IsAlive();
		}

		if (ATracePlayerController* AttackerController = Cast<ATracePlayerController>(Character->GetController()))
		{
			// The zone rides along so the hitmarker stays positional, exactly as it does for a shot.
			// The knife's damage does NOT come from the zone — back or front decides that — but a
			// player reading their own hitmarker learns where the blade landed either way.
			AttackerController->ClientNotifyHit(bKilled, Hit.Zone);
		}

		UE_LOG(LogTraceGame, Verbose,
			TEXT("ServerSwing: %s %s %s for %.0f (approach %.1fdeg, rewind %.3fs)%s"),
			*GetNameSafe(Character), Hit.bBackstab ? TEXT("BACK-STABBED") : TEXT("cut"),
			*GetNameSafe(Hit.Victim), Hit.Damage, Hit.ApproachAngleDegrees, ServerNow - RewindTime,
			bKilled ? TEXT(" [KILL]") : TEXT(""));
	}
	else if (Hit.bBlockedByCarrierShield)
	{
		// Logged by name because "I clearly hit them and nothing happened" against a carrier is the
		// single most likely bug report this weapon will generate, and it is not a bug. See
		// TraceMelee.h.
		UE_LOG(LogTraceGame, Verbose,
			TEXT("ServerSwing: %s's blade stopped on a Core carrier's shield — carriers are immune to melee by design (spec v10 §1)."),
			*GetNameSafe(Character));
	}

	// Unreliable and cosmetic: everyone but the swinger draws the slash.
	MulticastSwingEffects(FVector_NetQuantize(SwingOrigin), FVector_NetQuantizeNormal(Dir), Hit.Victim != nullptr);
}

void UTraceWeaponComponent::MulticastSwingEffects_Implementation(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction, bool bConnected)
{
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	ATraceCharacter* Character = GetTraceCharacter();
	UWorld* World = GetWorld();
	if (Character == nullptr || World == nullptr)
	{
		return;
	}

	// The swinger drew this the instant the blade resolved locally. Drawing it again would double
	// the effect and, worse, draw the server's slightly different arc over the one the player
	// already saw — the same owner-skipping contract MulticastFireEffects honours.
	if (Character->IsLocallyControlled())
	{
		return;
	}

	// The swinging pawn also plays its own third-person blade animation from here, so a bystander
	// sees the arm move and the slash together rather than a slash from a static body.
	SwingAnimStartLocalTime = GetLocalTimeSeconds();

	const FVector Dir = FVector(Direction).GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return;
	}

	ATraceMeleeArc::Spawn(World, Origin, Dir, TraceMelee::GetSwingAxis(Character, Dir),
		TraceMelee::GetSwingArcDegrees(), TraceMelee::GetSwingRangeUU(),
		TraceTeamColor(Character->GetTeam()), bConnected);
}

// -------------------------------------------------------------------------------------------------
// The victim-facing ring — the one thing the knife could not borrow from the gun.
// -------------------------------------------------------------------------------------------------

void UTraceWeaponComponent::RecordFacingSample(float ServerTime)
{
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority() || !FMath::IsFinite(ServerTime))
	{
		return;
	}

	// Guard against being driven twice in one frame: two samples with the same timestamp would give
	// the interpolation below a zero-length span to work across.
	if (FacingHistory.Num() > 0 && ServerTime <= FacingHistory.Last().ServerTime)
	{
		return;
	}

	FTraceFacingSample& Sample = FacingHistory.AddDefaulted_GetRef();
	Sample.ServerTime = ServerTime;
	Sample.Yaw = static_cast<float>(OwnerActor->GetActorRotation().Yaw);

	// Trimmed against the SAME window the pose history uses, so a swing can never be rewound to an
	// instant where the position is known and the facing is not.
	const float Duration = FMath::Max(0.05f, UTraceSettings::Get().LagCompHistoryDuration);
	int32 FirstToKeep = 0;
	while (FirstToKeep + 1 < FacingHistory.Num()
		&& (ServerTime - FacingHistory[FirstToKeep + 1].ServerTime) > Duration)
	{
		++FirstToKeep;
	}
	if (FirstToKeep > 0)
	{
		FacingHistory.RemoveAt(0, FirstToKeep);
	}
	if (FacingHistory.Num() > MaxFacingSamples)
	{
		FacingHistory.RemoveAt(0, FacingHistory.Num() - MaxFacingSamples);
	}
}

bool UTraceWeaponComponent::GetFacingYawAtTime(const ATraceCharacter* Character, float ServerTime, float& OutYaw)
{
	if (Character == nullptr)
	{
		return false;
	}

	const UTraceWeaponComponent* Weapon = Character->FindComponentByClass<UTraceWeaponComponent>();
	if (Weapon == nullptr || Weapon->FacingHistory.Num() == 0)
	{
		// No history: a client (which records none), a pawn that just spawned, or a build with lag
		// compensation off. OutYaw is left exactly as the caller set it so their live-pose fallback
		// survives — see TraceMelee::ResolveSwing.
		return false;
	}

	const TArray<FTraceFacingSample>& History = Weapon->FacingHistory;

	if (ServerTime >= History.Last().ServerTime)
	{
		OutYaw = History.Last().Yaw;
		return true;
	}
	if (ServerTime < History[0].ServerTime)
	{
		// Older than anything retained. Refuse rather than invent a facing, exactly as
		// UTraceLagCompensationComponent::GetPoseAtTime refuses to invent a pose.
		return false;
	}

	for (int32 Index = History.Num() - 1; Index > 0; --Index)
	{
		const FTraceFacingSample& B = History[Index];
		const FTraceFacingSample& A = History[Index - 1];
		if (ServerTime >= A.ServerTime && ServerTime <= B.ServerTime)
		{
			const float Span = B.ServerTime - A.ServerTime;
			const float Alpha = (Span > 1.e-4f)
				? FMath::Clamp((ServerTime - A.ServerTime) / Span, 0.f, 1.f)
				: 0.f;

			// UnwindDegrees on the DELTA, not a plain lerp of the two yaws. A pawn crossing the
			// +/-180 seam would otherwise interpolate the long way round the circle, and for the
			// couple of frames that took, an attacker standing in the victim's back would be told
			// they were standing in their face.
			OutYaw = A.Yaw + FMath::UnwindDegrees(B.Yaw - A.Yaw) * Alpha;
			return true;
		}
	}

	OutYaw = History[0].Yaw;
	return true;
}

// -------------------------------------------------------------------------------------------------
// Bots. TEMPORARY AND HONESTLY SO — see the declaration's comment and the pass report.
// -------------------------------------------------------------------------------------------------

void UTraceWeaponComponent::TickBotKnife()
{
	if (!TraceMelee::IsBotAutoKnifeEnabled())
	{
		return;
	}

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsAlive() || Character->IsCarrier())
	{
		return;
	}

	// Bots only. A human's weapon choice is theirs; a server-side proxy of a remote human has no
	// controller of its own to consult here either way.
	AController* BotController = Character->GetController();
	if (BotController == nullptr || Cast<APlayerController>(BotController) != nullptr)
	{
		return;
	}

	// Four decisions a second, not sixty. The pullout is 0.2 s, so a bot re-deciding every frame at
	// a range boundary would live permanently inside a swap and never hold either weapon.
	const double Now = GetLocalTimeSeconds();
	if ((Now - LastBotSwapDecisionTime) < 0.25)
	{
		return;
	}
	LastBotSwapDecisionTime = Now;

	if (IsDeploying())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// Nearest living enemy who is not the carrier. Carriers are excluded because they are immune to
	// the knife — chasing one with a blade out would be a bot committing to a weapon that cannot
	// touch its target, which is worse than not using the knife at all.
	const ETraceTeam MyTeam = Character->GetTeam();
	double NearestDistanceSq = TNumericLimits<double>::Max();
	ATraceCharacter* Nearest = nullptr;

	for (TActorIterator<ATraceCharacter> It(World); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || Candidate == Character || !Candidate->IsAlive() || Candidate->IsCarrier())
		{
			continue;
		}
		if (MyTeam != ETraceTeam::None && Candidate->GetTeam() == MyTeam)
		{
			continue;
		}

		const double DistanceSq = FVector::DistSquared(Candidate->GetActorLocation(), Character->GetActorLocation());
		if (DistanceSq < NearestDistanceSq)
		{
			NearestDistanceSq = DistanceSq;
			Nearest = Candidate;
		}
	}

	const double Engage = TraceMelee::GetBotEngageRangeUU();
	const double Disengage = TraceMelee::GetBotDisengageRangeUU();

	// The band IS the rule: inside Engage the +30% makes the knife the correct chase tool, outside
	// Disengage it is not, and the gap between them is what stops a bot at the boundary thrashing.
	if (!IsKnifeEquipped())
	{
		if (NearestDistanceSq <= Engage * Engage)
		{
			RequestEquip(ETraceEquippedWeapon::Knife);
		}
		return;
	}

	if (NearestDistanceSq > Disengage * Disengage)
	{
		RequestEquip(ETraceEquippedWeapon::Gun);
		return;
	}

	// AND THEN ACTUALLY SWING IT. Spec v10 §1: "Bots must use it, or it will not be playtested" —
	// "at minimum swap to the knife to close distance AND swing in range".
	//
	// THIS BRANCH IS WHY THE FEATURE GETS PLAYTESTED AT ALL, and it was missing on the first
	// measured pass: bots swapped to the knife 7 times in a 45 s headless match and swung ZERO
	// times, with no refusals logged because StartSwing was simply never reached. The swap half of
	// the rule was implemented and the swing half was not, which reads in-game as bots jogging at
	// people with a blade out and politely declining to use it.
	//
	// A bot cannot inherit the human path here. A human swings because DoFirePressed routes a held
	// trigger into StartSwing; a bot's trigger is driven by ATraceBotController's shooting logic,
	// which reasons about GUN range and line of sight and has no reason to hold fire at 150 uu. So
	// the melee slice drives its own swing, exactly as it drives its own swap, and the bot
	// controller stays untouched — it is another agent's file and needs no knowledge of the knife.
	//
	// StartSwing re-checks every gate (alive, not carrying, not dashing, deployed, off cooldown), so
	// this is a request and not an assertion; the 0.5 s cooldown paces it without a timer here.
	if (Nearest != nullptr && TraceMelee::IsInSwingRange(Character, Nearest))
	{
		StartSwing();
	}
}

// -------------------------------------------------------------------------------------------------
// Presentation — the knife you can see. Cosmetic throughout; nothing here is read by the resolution.
// -------------------------------------------------------------------------------------------------

namespace TraceKnifeLayout
{
	// FIRST PERSON, in ATraceCharacter's viewmodel rig space: +X out of the lens, +Y right, +Z up,
	// origin at the top-rear of the gun's grip, and every Size is a WORLD size that AddKnifePart
	// divides by the 100 uu engine primitive.
	//
	// The framing and depth arithmetic in TraceCharacter.cpp's header both have to keep holding:
	//
	//   DEPTH. The deepest point of this rig is the blade tip at x = 23 uu. The gun's muzzle is at
	//   76 and is the piece that had to clear the capsule, so a 23 uu blade is a third of the way
	//   there — it can never intersect the world.
	//   FRAMING. The rig hangs where the gun hangs (the root's rest location is the gun's), so it
	//   sits in the same lower-right corner and clears the crosshair by the same margin.
	//
	// The blade is offset toward the right hand's block (which stays visible) rather than centred,
	// so it reads as being HELD rather than as floating where the gun used to be.
	constexpr float ShapeUnit = 100.f;

	struct FKnifePart
	{
		const TCHAR* Name;
		FVector Location;
		FRotator Rotation;
		FVector Size;
		bool bNeon;
	};

	const FKnifePart FirstPersonParts[] =
	{
		// Grip, guard, blade, tip: four masses, which is the least that reads as a knife rather than
		// as a stick. The 8 degree cant is what stops it looking like a prop welded to the lens.
		{ TEXT("KnifeGrip"),   FVector(0.4f, 0.2f, -4.4f),  FRotator(8.f, 0.f, 0.f),  FVector(7.4f, 2.6f, 3.0f),  false },
		{ TEXT("KnifePommel"), FVector(-3.6f, 0.2f, -5.0f), FRotator(8.f, 0.f, 0.f),  FVector(1.4f, 3.0f, 3.4f),  true  },
		{ TEXT("KnifeGuard"),  FVector(4.4f, 0.2f, -3.8f),  FRotator(8.f, 0.f, 0.f),  FVector(1.3f, 5.4f, 3.6f),  false },
		{ TEXT("KnifeBlade"),  FVector(12.6f, 0.2f, -2.7f), FRotator(4.f, 0.f, 0.f),  FVector(15.0f, 1.0f, 3.4f), false },
		// The edge is the only lit part, and it is a LINE rather than a face: this arena is black
		// surfaces and neon edges, and a glowing blade edge is the same language as everything else
		// in it. It is also the part that reads at a glance as "that is a knife, not a pistol".
		{ TEXT("KnifeEdge"),   FVector(12.8f, 0.2f, -4.2f), FRotator(4.f, 0.f, 0.f),  FVector(15.4f, 0.6f, 0.7f), true  },
		{ TEXT("KnifeTip"),    FVector(21.4f, 0.2f, -2.0f), FRotator(4.f, 0.f, 0.f),  FVector(3.6f, 0.9f, 1.9f),  false }
	};

	// THIRD PERSON, in hand_r socket space. Smaller numbers because this is a real-world-scale hand
	// rather than a viewmodel — the Mannequin's hand is about 10 uu across, and a blade the size of
	// the first-person one would be a sword.
	const FKnifePart HandParts[] =
	{
		{ TEXT("HandKnifeGrip"),  FVector(3.0f, 0.f, 0.f),  FRotator::ZeroRotator, FVector(9.0f, 2.4f, 2.4f),  false },
		{ TEXT("HandKnifeGuard"), FVector(8.0f, 0.f, 0.f),  FRotator::ZeroRotator, FVector(1.2f, 5.0f, 3.0f),  false },
		{ TEXT("HandKnifeBlade"), FVector(17.0f, 0.f, 0.f), FRotator::ZeroRotator, FVector(17.0f, 1.0f, 3.0f), false },
		{ TEXT("HandKnifeEdge"),  FVector(17.2f, 0.f, -1.6f), FRotator::ZeroRotator, FVector(17.4f, 0.6f, 0.7f), true }
	};

	/** Where the blade sits in the hand. Rotated so it runs along the fingers, edge outward. */
	const FVector HandOffset(-2.f, 4.f, 0.f);
	const FRotator HandRotation(0.f, 0.f, 0.f);

	/** Swing animation. Wind-up back and up, sweep across and down, then ease home. */
	constexpr float WindupYaw = 34.f;
	constexpr float WindupRoll = -26.f;
	constexpr float WindupBackUU = 5.5f;
	constexpr float SweepYaw = -58.f;
	constexpr float SweepRoll = 46.f;
	constexpr float SweepForwardUU = 9.0f;
	constexpr float SweepAcrossUU = -7.0f;
	constexpr float SweepDownUU = -3.0f;
}

bool UTraceWeaponComponent::IsViewModelHandPart(const UStaticMeshComponent* Part)
{
	if (Part == nullptr)
	{
		return false;
	}

	// See the declaration: identified by NAME because ATraceCharacter's parts table is private to
	// another ownership slice and ViewModelRoot is the only public handle on the rig. The failure
	// mode if that table is renamed is the GUN staying visible next to the knife — ugly and
	// immediately obvious — rather than the hands vanishing, which would look like a render bug.
	const FString Name = Part->GetName();
	return Name.Contains(TEXT("Hand"))
		|| Name.Contains(TEXT("Knuckle"))
		|| Name.Contains(TEXT("Forearm"))
		|| Name.Contains(TEXT("Cuff"));
}

UStaticMeshComponent* UTraceWeaponComponent::AddKnifePart(USceneComponent* AttachTo, const TCHAR* DebugName,
	UStaticMesh* Mesh, const FVector& Location, const FRotator& Rotation, const FVector& Size,
	bool bNeon, bool bFirstPerson)
{
	AActor* OwnerActor = GetOwner();
	if (AttachTo == nullptr || Mesh == nullptr || OwnerActor == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(
		OwnerActor, MakeUniqueObjectName(OwnerActor, UStaticMeshComponent::StaticClass(), FName(DebugName)));
	if (Part == nullptr)
	{
		return nullptr;
	}

	Part->SetMobility(EComponentMobility::Movable);
	Part->SetupAttachment(AttachTo);
	Part->SetStaticMesh(Mesh);
	Part->SetRelativeLocationAndRotation(Location, Rotation);
	Part->SetRelativeScale3D(Size / TraceKnifeLayout::ShapeUnit);

	// CONTRACT §7, and it matters as much here as it does for the gun: the capsule is the ONLY
	// collider on this actor. Hitscan resolution, the trail trip test and the lag-compensation
	// history all reason purely about the capsule, and a colliding blade would break all three —
	// besides being a permanent obstacle welded to a player's face in first person.
	Part->SetCollisionProfileName(TEXT("NoCollision"));
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCanEverAffectNavigation(false);
	Part->bReceivesDecals = false;

	if (bFirstPerson)
	{
		// NOBODY ELSE MAY EVER SEE THIS, and no shadow of any kind, so there is no path by which a
		// floating knife appears in another player's frame. Set BEFORE RegisterComponent so the
		// scene proxy is created with it rather than being rebuilt.
		Part->SetOnlyOwnerSee(true);
		Part->SetCastShadow(false);
		Part->bCastHiddenShadow = false;
		Part->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;
	}
	else
	{
		// The third-person blade is the opposite: everyone EXCEPT its owner, who is looking down the
		// first-person rig instead and must not see a second knife sticking out of their own wrist.
		Part->SetOwnerNoSee(true);
		Part->SetCastShadow(true);
	}

	// --- Material -------------------------------------------------------------------------------
	//
	// Reuse the material the viewmodel gun is already wearing where one exists, so the knife is made
	// of the same stuff as the gun it replaced AND inherits ApplyTeamColors' pushes for free — the
	// MIDs are shared objects owned by the character. Where there is no gun to borrow from (a
	// simulated proxy, which never builds a viewmodel) fall back to the generated Tron materials and
	// finally to BasicShapeMaterial, exactly as everything else in this project degrades.
	UMaterialInterface* Material = nullptr;
	if (const ATraceCharacter* Character = GetTraceCharacter())
	{
		if (Character->ViewModelRoot != nullptr)
		{
			TArray<USceneComponent*> Children;
			Character->ViewModelRoot->GetChildrenComponents(/*bIncludeAllDescendants=*/false, Children);
			for (USceneComponent* Child : Children)
			{
				UStaticMeshComponent* GunPart = Cast<UStaticMeshComponent>(Child);
				if (GunPart == nullptr || KnifeViewParts.Contains(GunPart) || KnifeHandParts.Contains(GunPart))
				{
					continue;
				}
				// "Neon" in the name is how the gun's own table marks its lit channels.
				const bool bPartIsNeon = GunPart->GetName().Contains(TEXT("Neon"));
				if (bPartIsNeon == bNeon)
				{
					Material = GunPart->GetMaterial(0);
					break;
				}
			}
		}
	}

	if (Material == nullptr)
	{
		Material = LoadObject<UMaterialInterface>(nullptr, bNeon
			? TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon")
			: TEXT("/Game/Generated/Materials/M_TraceSurface.M_TraceSurface"));
	}
	if (Material == nullptr)
	{
		Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}

	if (Material != nullptr)
	{
		if (UMaterialInstanceDynamic* MID = Part->CreateDynamicMaterialInstance(0, Material))
		{
			// Team-coloured, and the lit edge brighter than the body — the same read every neon
			// surface in this arena uses. Harmless on a material that has neither parameter.
			const ATraceCharacter* Character = GetTraceCharacter();
			const FLinearColor TeamColor = (Character != nullptr)
				? TraceTeamColor(Character->GetTeam())
				: FLinearColor(0.5f, 0.5f, 0.5f, 1.f);
			MID->SetVectorParameterValue(TEXT("Color"), bNeon ? TeamColor : (TeamColor * 0.16f));
			MID->SetScalarParameterValue(TEXT("Glow"), bNeon ? 2.6f : 0.f);
		}
	}

	Part->RegisterComponent();
	return Part;
}

void UTraceWeaponComponent::EnsureKnifeVisualsBuilt()
{
	if (bKnifeMeshUnavailable || (bKnifeViewBuilt && bKnifeHandBuilt))
	{
		return;
	}
	if (GetNetMode() == NM_DedicatedServer)
	{
		// Renders nothing, so there is nothing to build. Latch both so this stops being asked.
		bKnifeViewBuilt = true;
		bKnifeHandBuilt = true;
		return;
	}

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	if (KnifeCubeMesh == nullptr)
	{
		KnifeCubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (KnifeCubeMesh == nullptr)
		{
			// /Engine/BasicShapes ships with every install, so this is close to impossible — but an
			// invisible knife is a far better failure than a crash, and it is the contract every
			// other optional asset in this project honours. Logged once.
			bKnifeMeshUnavailable = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("Knife visuals skipped: /Engine/BasicShapes/Cube did not resolve. The weapon still works."));
			return;
		}
	}

	// --- First person ---------------------------------------------------------------------------
	//
	// Hung under ViewModelRoot, so it inherits the sway, the walk bob and the recoil settle the gun
	// already gets — for free, and without ATraceCharacter having to know the knife exists. The
	// intermediate KnifeViewRoot is what the swing animation writes to, so the swing composes with
	// the sway instead of fighting it.
	if (!bKnifeViewBuilt && Character->IsLocallyControlled() && Character->ViewModelRoot != nullptr)
	{
		KnifeViewRoot = NewObject<USceneComponent>(Character,
			MakeUniqueObjectName(Character, USceneComponent::StaticClass(), FName(TEXT("KnifeViewRoot"))));
		if (KnifeViewRoot != nullptr)
		{
			KnifeViewRoot->SetMobility(EComponentMobility::Movable);
			KnifeViewRoot->SetupAttachment(Character->ViewModelRoot);
			KnifeViewRoot->RegisterComponent();

			for (const TraceKnifeLayout::FKnifePart& Spec : TraceKnifeLayout::FirstPersonParts)
			{
				if (UStaticMeshComponent* Part = AddKnifePart(KnifeViewRoot, Spec.Name, KnifeCubeMesh,
					Spec.Location, Spec.Rotation, Spec.Size, Spec.bNeon, /*bFirstPerson=*/true))
				{
					Part->SetVisibility(false);
					KnifeViewParts.Add(Part);
				}
			}

			bKnifeViewBuilt = true;
			UE_LOG(LogTraceGame, Verbose, TEXT("%s built a first-person knife (%d parts)."),
				*GetNameSafe(Character), KnifeViewParts.Num());
		}
	}

	// --- Third person ---------------------------------------------------------------------------
	//
	// DoesSocketExist is the whole guard, and it is doing more work than it looks: the Mannequin is
	// imported per developer and is legitimately absent on a fresh clone, in which case the skeletal
	// mesh component exists but has no skeleton — attaching to a socket that does not exist would
	// silently attach at the component ORIGIN and leave a knife lying at the pawn's feet.
	if (!bKnifeHandBuilt)
	{
		USkeletalMeshComponent* BodyMesh = Character->GetMesh();
		if (BodyMesh != nullptr && BodyMesh->DoesSocketExist(TEXT("hand_r")))
		{
			KnifeHandRoot = NewObject<USceneComponent>(Character,
				MakeUniqueObjectName(Character, USceneComponent::StaticClass(), FName(TEXT("KnifeHandRoot"))));
			if (KnifeHandRoot != nullptr)
			{
				KnifeHandRoot->SetMobility(EComponentMobility::Movable);
				KnifeHandRoot->SetupAttachment(BodyMesh, TEXT("hand_r"));
				KnifeHandRoot->RegisterComponent();
				KnifeHandRoot->SetRelativeLocationAndRotation(
					TraceKnifeLayout::HandOffset, TraceKnifeLayout::HandRotation);

				for (const TraceKnifeLayout::FKnifePart& Spec : TraceKnifeLayout::HandParts)
				{
					if (UStaticMeshComponent* Part = AddKnifePart(KnifeHandRoot, Spec.Name, KnifeCubeMesh,
						Spec.Location, Spec.Rotation, Spec.Size, Spec.bNeon, /*bFirstPerson=*/false))
					{
						Part->SetVisibility(false);
						KnifeHandParts.Add(Part);
					}
				}

				bKnifeHandBuilt = true;
				UE_LOG(LogTraceGame, Verbose, TEXT("%s built a third-person knife (%d parts)."),
					*GetNameSafe(Character), KnifeHandParts.Num());
			}
		}
	}
}

void UTraceWeaponComponent::SetGunViewModelHidden(bool bHidden)
{
	if (bGunViewModelHidden == bHidden)
	{
		return;
	}

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || Character->ViewModelRoot == nullptr)
	{
		return;
	}

	bGunViewModelHidden = bHidden;

	TArray<USceneComponent*> Children;
	Character->ViewModelRoot->GetChildrenComponents(/*bIncludeAllDescendants=*/false, Children);

	for (USceneComponent* Child : Children)
	{
		UStaticMeshComponent* Part = Cast<UStaticMeshComponent>(Child);
		if (Part == nullptr || KnifeViewParts.Contains(Part))
		{
			continue;
		}
		if (IsViewModelHandPart(Part))
		{
			// The hands and forearms stay. A knife held by nothing is a worse read than no knife.
			continue;
		}
		Part->SetVisibility(!bHidden);
	}
}

void UTraceWeaponComponent::UpdateKnifeVisuals(float /*DeltaTime*/)
{
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	EnsureKnifeVisualsBuilt();

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	const bool bKnife = IsKnifeEquipped();
	const bool bAlive = Character->IsAlive();

	// --- First person ---------------------------------------------------------------------------
	//
	// ATraceCharacter::SetViewModelVisible drives its OWN parts list and never touches ours, so the
	// two rigs' visibility is decided independently and re-asserted every tick. Re-asserting rather
	// than latching is what makes this robust against the character re-showing the gun on a state
	// change it owns (a respawn, or handing the Core away), which is a path this component cannot
	// hook.
	if (bKnifeViewBuilt)
	{
		// IsViewModelVisible() is the character's own settled answer to "is the first-person rig on
		// screen", which folds in the third-person carry blend and the corpse hiding. Asking it is
		// what keeps the knife from appearing over the carrier's shoulder camera.
		const bool bWantView = bKnife && bAlive && Character->IsLocallyControlled() && Character->IsViewModelVisible();

		if (bWantView != bKnifeViewVisible)
		{
			bKnifeViewVisible = bWantView;
			for (UStaticMeshComponent* Part : KnifeViewParts)
			{
				if (Part != nullptr)
				{
					Part->SetVisibility(bWantView);
				}
			}
		}

		// The gun goes away exactly when the knife comes out, and comes back when it does not.
		SetGunViewModelHidden(bKnife && bAlive);

		if (bWantView && KnifeViewRoot != nullptr)
		{
			// --- The swing, as a transform ------------------------------------------------------
			//
			// THIS IS THE SWINGER'S ENTIRE READ, and it is why the blade resolves on a wind-up
			// instead of on the press: the damage lands as the edge crosses the middle of the sweep,
			// which is the frame a player would point at and say "there".
			//
			// Written to KnifeViewRoot, a CHILD of ViewModelRoot, so it composes with the sway and
			// bob the character is writing to the parent rather than overwriting them. And nothing
			// in TraceMelee::ResolveSwing reads any of it — the arc that was actually cut is pure
			// arithmetic on the aim ray, so the blade may lag and overshoot as much as it likes.
			const double Elapsed = GetLocalTimeSeconds() - SwingAnimStartLocalTime;
			const double AnimLength = static_cast<double>(TraceMelee::GetSwingAnimSeconds());

			FVector Offset = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;

			if (Elapsed >= 0.0 && Elapsed <= AnimLength)
			{
				const float WindupEnd = FMath::Clamp(
					TraceMelee::GetSwingWindupSeconds() / FMath::Max(0.01f, TraceMelee::GetSwingAnimSeconds()), 0.05f, 0.6f);
				const float Alpha = static_cast<float>(Elapsed / FMath::Max(0.01, AnimLength));

				if (Alpha < WindupEnd)
				{
					// Wind-up: back, up and cocked. Eased IN, so it accelerates into the sweep.
					const float T = FMath::Clamp(Alpha / WindupEnd, 0.f, 1.f);
					const float Eased = T * T;
					Offset = FVector(-TraceKnifeLayout::WindupBackUU * Eased, 0.f, 1.5f * Eased);
					Rotation = FRotator(0.f, TraceKnifeLayout::WindupYaw * Eased, TraceKnifeLayout::WindupRoll * Eased);
				}
				else
				{
					// Sweep and settle: one eased-out arc from the cocked pose through the strike
					// and back to rest. One curve, not two, so there is no visible seam at the
					// moment of impact.
					const float T = FMath::Clamp((Alpha - WindupEnd) / FMath::Max(0.01f, 1.f - WindupEnd), 0.f, 1.f);
					const float Strike = FMath::Sin(T * PI);                 // 0 -> 1 -> 0
					const float Travel = 1.f - FMath::Square(1.f - T);       // 0 -> 1, eased out

					Offset = FVector(
						FMath::Lerp(-TraceKnifeLayout::WindupBackUU, 0.f, Travel) + TraceKnifeLayout::SweepForwardUU * Strike,
						TraceKnifeLayout::SweepAcrossUU * Strike,
						FMath::Lerp(1.5f, 0.f, Travel) + TraceKnifeLayout::SweepDownUU * Strike);
					Rotation = FRotator(
						0.f,
						FMath::Lerp(TraceKnifeLayout::WindupYaw, 0.f, Travel) + TraceKnifeLayout::SweepYaw * Strike,
						FMath::Lerp(TraceKnifeLayout::WindupRoll, 0.f, Travel) + TraceKnifeLayout::SweepRoll * Strike);
				}
			}

			KnifeViewRoot->SetRelativeLocationAndRotation(Offset, Rotation);
		}
	}

	// --- Third person ---------------------------------------------------------------------------
	//
	// No animation here on purpose. The imported Mannequin set has no melee sequence (the same
	// absence that forced UpdateCrouchPresentation to pose the slide by hand), so the arm will not
	// swing — ATraceMeleeArc draws the cut instead, which is the information a victim actually needs.
	// What this rig carries is the STATE: a visible blade means that player is 30% faster and cannot
	// shoot, and that is worth reading across the arena.
	if (bKnifeHandBuilt)
	{
		const bool bWantHand = bKnife && bAlive;
		if (bWantHand != bKnifeHandVisible)
		{
			bKnifeHandVisible = bWantHand;
			for (UStaticMeshComponent* Part : KnifeHandParts)
			{
				if (Part != nullptr)
				{
					Part->SetVisibility(bWantHand);
				}
			}
		}
	}
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

// =================================================================================================
// Trace.TestKnife — the unattended proof for spec v10 §1.
//
// The user gave THREE numbers and every one of them is a timing: 0.2 s of pullout each way, 0.5 s
// between swings, and 100/30 damage decided by an angle. None of those can be checked by looking at
// a screenshot, and none of them can be checked by reading the header — the whole point of the
// project rule about the .ini is that the header is not the authority. So this drives the SHIPPING
// input path (StartSwing / RequestEquip, the same functions mouse1 and the swap bind call) and
// reports what the clock actually did:
//
//   PULLOUT gun->knife  seconds from the swap request until an attack was first permitted.
//   PULLOUT knife->gun  the same, the other way. The user asked for one number; if these two differ
//                       by more than a frame, they are not one number.
//   SWING-TO-SWING      min / mean / max of the press-to-press interval over a held burst. This is
//                       the interval a player can observe, which is why the cooldown is stamped at
//                       the press rather than at the resolve.
//   DAMAGE PROBE        for the nearest living enemy, the approach angle and therefore the damage a
//                       swing would score right now — measured through the same TraceMelee::
//                       IsBackstab the server uses, not a re-derivation of it.
//
// It also prints the equipped weapon at every transition, so a run in which the swap silently did
// nothing is distinguishable from one in which it worked and the timing was wrong.
// =================================================================================================

namespace TraceKnifeTest
{
	struct FState
	{
		enum class EPhase : uint8
		{
			ToKnife,
			Swinging,
			ToGun,
			Done
		};

		EPhase Phase = EPhase::ToKnife;
		double PhaseStart = 0.0;
		double Elapsed = 0.0;

		double SwingSeconds = 3.0;
		double TimeoutSeconds = 2.0;

		float PulloutToKnife = -1.f;
		float PulloutToGun = -1.f;

		double LastSwingAt = -1.0;
		TArray<double> SwingIntervals;
		int32 SwingCount = 0;

		bool bStarted = false;
		bool bAborted = false;
		FString AbortReason;
	};

	void Report(const TSharedRef<FState>& State, UTraceWeaponComponent* Weapon, ATraceCharacter* Character)
	{
		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE KNIFE TEST =========="));

		if (State->bAborted)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("KNIFE aborted     : %s"), *State->AbortReason);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("KNIFE pullout     : gun->knife %.4fs | knife->gun %.4fs   (asked for ~0.200s, setting is %.3fs)"),
			State->PulloutToKnife, State->PulloutToGun, TraceMelee::GetSwapSeconds());

		if (State->SwingIntervals.Num() > 0)
		{
			double Sum = 0.0;
			double Min = TNumericLimits<double>::Max();
			double Max = 0.0;
			for (double Interval : State->SwingIntervals)
			{
				Sum += Interval;
				Min = FMath::Min(Min, Interval);
				Max = FMath::Max(Max, Interval);
			}
			UE_LOG(LogTraceGame, Display,
				TEXT("KNIFE swing gap   : %d intervals | min %.4fs  mean %.4fs  max %.4fs   (asked for 0.500s, setting is %.3fs)"),
				State->SwingIntervals.Num(), Min, Sum / State->SwingIntervals.Num(), Max,
				TraceMelee::GetSwingCooldownSeconds());
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("KNIFE swing gap   : NO SWINGS LANDED — %d starts. A knife that never swings is the failure this test exists to catch."),
				State->SwingCount);
		}

		// Damage probe. Analytic, against the real world, through the same predicate the server uses.
		if (Character != nullptr && Character->GetWorld() != nullptr)
		{
			const ATraceCharacter* Nearest = nullptr;
			double NearestDistanceSq = TNumericLimits<double>::Max();
			for (TActorIterator<ATraceCharacter> It(Character->GetWorld()); It; ++It)
			{
				const ATraceCharacter* Candidate = *It;
				if (Candidate == nullptr || Candidate == Character || !Candidate->IsAlive())
				{
					continue;
				}
				const double DistanceSq = FVector::DistSquared(Candidate->GetActorLocation(), Character->GetActorLocation());
				if (DistanceSq < NearestDistanceSq)
				{
					NearestDistanceSq = DistanceSq;
					Nearest = Candidate;
				}
			}

			if (Nearest != nullptr)
			{
				double Angle = -1.0;
				const bool bBackstab = TraceMelee::IsBackstab(
					Character->GetActorLocation(), Nearest->GetActorLocation(),
					static_cast<float>(Nearest->GetActorRotation().Yaw), &Angle);

				UE_LOG(LogTraceGame, Display,
					TEXT("KNIFE probe       : nearest is %s at %.0fuu | approach %.1fdeg (threshold %.0f) -> %s, %.0f damage | carrier=%d (immune)"),
					*GetNameSafe(Nearest), FMath::Sqrt(NearestDistanceSq), Angle,
					TraceMelee::GetBackstabHalfAngleDegrees(),
					bBackstab ? TEXT("BACKSTAB") : TEXT("front"),
					TraceMelee::DamageForApproach(bBackstab),
					Nearest->IsCarrier() ? 1 : 0);
			}
			else
			{
				UE_LOG(LogTraceGame, Display, TEXT("KNIFE probe       : no living target in the world."));
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("KNIFE final state : equipped %s | deploying %d | cooldown %.3fs"),
			(Weapon != nullptr) ? LexToString(Weapon->GetEquippedWeapon()) : TEXT("-"),
			(Weapon != nullptr && Weapon->IsDeploying()) ? 1 : 0,
			(Weapon != nullptr) ? Weapon->GetSwingCooldownRemaining() : 0.f);
		UE_LOG(LogTraceGame, Display, TEXT("======================================"));

		// The pure model, run alongside so one command answers both halves of the feature.
		TraceRunMeleeSelfTest();
	}

	void Run(float SwingSeconds, float DelaySeconds)
	{
		TSharedRef<FState> State = MakeShared<FState>();
		State->SwingSeconds = FMath::Max(1.0f, SwingSeconds);

		UE_LOG(LogTraceGame, Display,
			TEXT("[KnifeTest] in %.1fs: swap to the knife, swing for %.1fs, swap back, and report every timing."),
			DelaySeconds, State->SwingSeconds);

		// TWO TICKERS, for the reason spelled out in Trace.TestRecoil: FTSTicker's delay applies to
		// EVERY invocation, not just the first, so a delay on the sampler itself would make it run
		// once per delay period instead of once per frame.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float /*Delta*/) -> bool
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
					[State](float DeltaTime) -> bool
					{
						ATraceCharacter* Character = TraceRecoilTest::FindLocalTraceCharacter();
						UTraceWeaponComponent* Weapon = (Character != nullptr)
							? Character->FindComponentByClass<UTraceWeaponComponent>() : nullptr;

						if (Weapon == nullptr || Character == nullptr)
						{
							// No pawn yet (warm-up, or the menu map). Wait rather than reporting
							// zeroes, which would read as "the knife does nothing".
							return true;
						}

						// The WORLD clock, because that is the clock the gates are measured against
						// (UTraceWeaponComponent::GetLocalTimeSeconds). Measuring the cooldown with
						// a wall clock and gating it with a world clock would silently report the
						// time dilation as a knife bug.
						const UWorld* TestWorld = Character->GetWorld();
						const double Now = (TestWorld != nullptr) ? TestWorld->GetTimeSeconds() : 0.0;

						if (!State->bStarted)
						{
							State->bStarted = true;
							State->PhaseStart = Now;

							if (Character->IsCarrier())
							{
								State->bAborted = true;
								State->AbortReason = TEXT("the local player is carrying the Core; carriers cannot swap or swing by design (spec v10 s1).");
								Report(State, Weapon, Character);
								return false;
							}

							UE_LOG(LogTraceGame, Display, TEXT("[KnifeTest] start: equipped %s. Requesting the knife."),
								LexToString(Weapon->GetEquippedWeapon()));

							ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
							if (!Weapon->RequestEquip(ETraceEquippedWeapon::Knife, &Refusal))
							{
								State->bAborted = true;
								State->AbortReason = FString::Printf(TEXT("the swap to the knife was refused: %s"), LexToString(Refusal));
								Report(State, Weapon, Character);
								return false;
							}
							return true;
						}

						State->Elapsed += DeltaTime;

						switch (State->Phase)
						{
						case FState::EPhase::ToKnife:
						{
							// The pullout is over the instant an attack is permitted — which is the
							// only definition of it a player can feel.
							if (Weapon->IsKnifeEquipped() && Weapon->CanSwing())
							{
								State->PulloutToKnife = static_cast<float>(Now - State->PhaseStart);
								UE_LOG(LogTraceGame, Display, TEXT("[KnifeTest] knife up after %.4fs; swinging for %.1fs."),
									State->PulloutToKnife, State->SwingSeconds);
								State->Phase = FState::EPhase::Swinging;
								State->PhaseStart = Now;
							}
							else if ((Now - State->PhaseStart) > State->TimeoutSeconds)
							{
								State->bAborted = true;
								State->AbortReason = TEXT("the knife never became usable within 2s of the swap.");
								Report(State, Weapon, Character);
								return false;
							}
							break;
						}

						case FState::EPhase::Swinging:
						{
							// Ask EVERY frame, exactly as a held trigger does. Most frames are
							// refused by the cooldown, and the intervals between the ones that are
							// not are the number this test exists to produce.
							if (Weapon->StartSwing())
							{
								++State->SwingCount;
								if (State->LastSwingAt > 0.0)
								{
									State->SwingIntervals.Add(Now - State->LastSwingAt);
								}
								State->LastSwingAt = Now;
							}

							if ((Now - State->PhaseStart) >= State->SwingSeconds)
							{
								UE_LOG(LogTraceGame, Display, TEXT("[KnifeTest] %d swings; requesting the gun."), State->SwingCount);
								State->Phase = FState::EPhase::ToGun;
								State->PhaseStart = Now;
								Weapon->RequestEquip(ETraceEquippedWeapon::Gun);
							}
							break;
						}

						case FState::EPhase::ToGun:
						{
							if (!Weapon->IsKnifeEquipped() && Weapon->CanFire())
							{
								State->PulloutToGun = static_cast<float>(Now - State->PhaseStart);
								State->Phase = FState::EPhase::Done;
								Report(State, Weapon, Character);
								return false;
							}
							if ((Now - State->PhaseStart) > State->TimeoutSeconds)
							{
								State->bAborted = true;
								State->AbortReason = TEXT("the gun never became usable within 2s of the swap back.");
								Report(State, Weapon, Character);
								return false;
							}
							break;
						}

						default:
							return false;
						}

						return true;
					}));

				return false;   // the delay shot is done; the sampler above is now armed
			}), DelaySeconds);
	}

	FAutoConsoleCommand CmdTestKnife(
		TEXT("Trace.TestKnife"),
		TEXT("Dev only. Trace.TestKnife [SwingSeconds] [DelaySeconds] — swap to the knife, swing, swap back, and report the measured pullout and swing-to-swing timings."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const float SwingSeconds = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 3.f;
			const float DelaySeconds = (Args.Num() > 1) ? FMath::Max(0.f, FCString::Atof(*Args[1])) : 0.f;
			Run(SwingSeconds, DelaySeconds);
		}));
}

#endif // !UE_BUILD_SHIPPING
