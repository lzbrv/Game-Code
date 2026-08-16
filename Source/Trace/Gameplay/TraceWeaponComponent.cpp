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

#include "Abilities/Characters/TraceAbilityWeaponHooks.h"   // spec v14 §6: X's Sting bullets
#include "Abilities/TraceAbilityComponent.h"                 // spec v14 §6: the damage passives
#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Movement/TraceCharacterMovementComponent.h"   // SetKnifeMovementProfileActive (spec v10 §1)
#include "Gameplay/TraceCore.h"                        // spec v16 §1: the carrier has no gun and no ammo
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

// =================================================================================================
// AMMO — spec v16 §1. THREE ARMS, THREE SEPARATE SENTENCES THEY FALSIFY.
//
// None of these shares a name with a console COMMAND (that collision is fatal at module load in this
// engine version): the commands are all verbs — Trace.Ammo.Dump, .Test, .CarrierTest, .BotWatch —
// and these three are nouns.
// =================================================================================================

/**
 * MASTER ARM. 0 makes the clip infinite and never reloads.
 *
 * It is the arm that falsifies "30 bullets per clip, then the gun reloads" as a whole: with it at 0
 * a pawn can fire a hundred rounds without the count moving, which is exactly the build that existed
 * before this pass. Trace.Ammo.Test's red arm uses it to prove its own measurement is measuring
 * something.
 */
static TAutoConsoleVariable<int32> CVarAmmoEnabled(
	TEXT("Trace.Ammo.Enabled"), 1,
	TEXT("1 (shipped, spec v16 §1): 30 rounds per clip, auto-reload on empty, 0.5 s reload. "
	     "0: the RED arm — the clip is infinite and nothing ever reloads."),
	ECVF_Default);

/**
 * THE RELOAD GATE ARM. 0 lets a player fire straight through a reload.
 *
 * Separate from the master arm because "reloading takes .5 seconds" is only a rule if something is
 * refused during it. With the master arm at 0 there is no reload to fire through and the sentence is
 * untestable; with this one at 0 the reload still runs, still shows on the HUD and still refills —
 * the only thing that changes is whether CanFire() says no. One variable.
 */
static TAutoConsoleVariable<int32> CVarAmmoReloadBlocksFire(
	TEXT("Trace.Ammo.ReloadBlocksFire"), 1,
	TEXT("1 (shipped): firing is refused for the whole 0.5 s reload. 0: the RED arm — the reload still "
	     "runs and still refills, but the trigger works through it."),
	ECVF_Default);

/**
 * THE PREDICTION ARM. 0 makes an owning client WAIT for the server's count instead of predicting it.
 *
 * Spec v16 §1 asks for "server-authoritative, client-predicted for feel — the local player must see
 * the count drop on their own shot without waiting for a round trip", and a HOST-SIDE measurement
 * cannot see a prediction bug by definition: on the authority the predicted path is not even taken.
 * So the arm and its probe (Trace.Ammo.ClientPredictTest) both live on the CLIENT, and the arm turns
 * the feature off without turning the ammo system off — with it at 0 the clip still empties, still
 * reloads and is still authoritative, and the only thing that changes is WHEN the shooter's own
 * number moves.
 */
static TAutoConsoleVariable<int32> CVarAmmoPredict(
	TEXT("Trace.Ammo.Predict"), 1,
	TEXT("1 (shipped): an owning client decrements its own clip on its own shot, before the server has "
	     "heard about it. 0: the RED arm — the client waits for replication, so its count lags its own "
	     "muzzle flash by a round trip."),
	ECVF_Default);

/**
 * THE CARRIER ARM. 0 removes the carrier guard inside UTraceWeaponComponent::ConsumeRound.
 *
 * Spec v16 §1: "The Core carrier has no gun, so ammo must not be consumed or shown while carrying."
 * The SHIPPED path cannot reach the consumption code with a carrier at all — CanFire() and
 * ServerFire() both refuse one first — so this arm is about the SECOND lock, the one that survives a
 * future caller reaching ConsumeRound another way. Trace.Ammo.CarrierTest drives that lock directly
 * (see UTraceWeaponComponent::DebugConsumeRound) and checks the first lock separately.
 */
static TAutoConsoleVariable<int32> CVarAmmoCarrierGuard(
	TEXT("Trace.Ammo.CarrierGuard"), 1,
	TEXT("1 (shipped): a Core carrier's clip can never lose a round, even if something calls the "
	     "consumption path directly. 0: the RED arm — the guard is removed and "
	     "TraceAmmo::GetCarrierRoundsConsumed() moves."),
	ECVF_Default);

/**
 * The ammo counters. File-static for the same reason UTraceHealthComponent's vulnerable alarms are:
 * the facts being counted are about the RULES, not about any one pawn.
 *
 * The first must be zero for the life of a correct process. The rest are LIVENESS, and they are what
 * lets a harness distinguish "the carrier rule held" from "nothing ever fired a shot".
 */
static int32 GAmmoCarrierRoundsConsumed = 0;
static int32 GAmmoRoundsConsumed        = 0;
static int32 GAmmoReloadsCompleted      = 0;
static int32 GAmmoDryFireRefusals       = 0;
static int32 GAmmoReloadFireRefusals    = 0;

namespace TraceAmmo
{
	int32 GetClipSize()
	{
		// Derived from the knob, never hardcoded: spec v16 §1 says 30 and UTraceSettings::ClipSize
		// says 30. Read it here so a designer retuning it live is obeyed by every caller at once.
		return FMath::Clamp(UTraceSettings::Get().ClipSize, 1, 999);
	}

	float GetReloadSeconds()
	{
		return FMath::Clamp(UTraceSettings::Get().ReloadSeconds, 0.05f, 10.f);
	}

	bool IsEnabled()             { return CVarAmmoEnabled.GetValueOnAnyThread() != 0; }
	bool DoesReloadBlockFire()   { return CVarAmmoReloadBlocksFire.GetValueOnAnyThread() != 0; }
	bool IsCarrierGuardArmed()   { return CVarAmmoCarrierGuard.GetValueOnAnyThread() != 0; }
	bool IsPredictionEnabled()   { return CVarAmmoPredict.GetValueOnAnyThread() != 0; }

	int32 GetCarrierRoundsConsumed() { return GAmmoCarrierRoundsConsumed; }
	int32 GetRoundsConsumed()        { return GAmmoRoundsConsumed; }
	int32 GetReloadsCompleted()      { return GAmmoReloadsCompleted; }
	int32 GetDryFireRefusals()       { return GAmmoDryFireRefusals; }
	int32 GetReloadFireRefusals()    { return GAmmoReloadFireRefusals; }

	void ResetCounters()
	{
		GAmmoCarrierRoundsConsumed = 0;
		GAmmoRoundsConsumed = 0;
		GAmmoReloadsCompleted = 0;
		GAmmoDryFireRefusals = 0;
		GAmmoReloadFireRefusals = 0;
	}
}

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
	// the selector on every machine to apply the knife's +22% ground speed, so a client that did not
	// receive its own value would be corrected into the speed instead of predicting it. The owner
	// having predicted the same value already makes the update a no-op, not a fight.
	DOREPLIFETIME(UTraceWeaponComponent, EquippedWeapon);
	DOREPLIFETIME(UTraceWeaponComponent, DeployEndServerTime);

	// --- AMMO (spec v16 §1) ---------------------------------------------------------------------
	//
	// COND_OwnerOnly, and that is the OPPOSITE choice from the two above, deliberately. The selector
	// has to be on every machine because the movement component reads it on every machine; ammo is
	// read by exactly one thing — the owning player's own HUD, in the bottom right. Nothing in this
	// game draws another player's ammo, nothing gates on it, and the third-person rig has no
	// magazine to show. Sending four bytes per pawn to every client for a number none of them can
	// see would be a pure bandwidth cost.
	//
	// *** THE CONSEQUENCE, STATED SO THE NEXT READER DOES NOT HAVE TO FIND IT: *** on a simulated
	// proxy these stay at their defaults forever. GetClipAmmo() says so; do not build "he's out of
	// ammo, push him" on another pawn's count without widening this condition first.
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, ClipAmmo,            COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, ClipSerial,          COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, AbilityRoundsInClip, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UTraceWeaponComponent, ReloadEndServerTime, COND_OwnerOnly);
}

void UTraceWeaponComponent::BeginPlay()
{
	Super::BeginPlay();

	// --- AMMO: the pawn starts with a full clip (spec v16 §1) ------------------------------------
	//
	// *** BEFORE THE EARLY RETURN BELOW, AND THAT ORDER IS LOAD-BEARING. *** Everything after this
	// block is about the local human's camera boom and bails out for a bot, a proxy and a dedicated
	// server's copy of anybody. Seeding the clip down there would have left every bot in the match
	// with ClipAmmo at its class default of 0 — thirty players unable to fire a single round, which
	// is a bug that would have read as "the ammo system broke the bots".
	//
	// Authority only: a client receives the count by replication, and a local write here would just
	// be a number it briefly believed in before the server told it otherwise.
	if (const AActor* AmmoOwner = GetOwner(); AmmoOwner != nullptr && AmmoOwner->HasAuthority())
	{
		RefillClip(TraceAmmo::GetClipSize(), 0);
	}

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

	// --- SPEC v16 §1: AMMO. "30 bullets per clip, then the gun reloads." -------------------------
	//
	// TWO REFUSALS, NOT ONE, AND THEY ARE COUNTED SEPARATELY. An empty clip and a running reload feel
	// identical through the trigger and mean completely different things to anyone reading a log: the
	// first says the automatic reload has not started yet (or has been cancelled), the second says it
	// is doing its job. A single "no ammo" counter would have made those indistinguishable, which is
	// the shape of the bug where a gun stops working and nobody can say why.
	//
	// Both sit BELOW the dash/pullout/knife gates on purpose, so the counters only move for a shot
	// that ammo was genuinely the reason for.
	if (TraceAmmo::IsEnabled())
	{
		if (GetClipAmmo() <= 0)
		{
			++GAmmoDryFireRefusals;
			return false;
		}
		if (TraceAmmo::DoesReloadBlockFire() && IsReloading())
		{
			++GAmmoReloadFireRefusals;
			return false;
		}
	}

	// SPEC v18 §2 — the per-character fire rate. Roxie's Modded is "fire rate x1.65" and Slimeball's
	// stuck passive is "+30% fire rate"; both arrive here as a scale ON THE INTERVAL, i.e. a number
	// BELOW 1 for a faster gun (1/1.65 = 0.606, 1/1.30 = 0.769). The gun multiplies and never divides
	// — dividing by 1.65 here would make Roxie fire slower while her card, her HUD and her ability log
	// all said faster, which reads in a playtest as "the ability does nothing" rather than as a bug.
	//
	// One character-agnostic call, exactly like TraceAbilityWeaponHooks::OnBulletHit above it: the gun
	// must not learn the name of a character, or the third character that needs a fire-rate change
	// adds a third cast next to the first two. 1.0 for everybody else, including every Mannequin and
	// every bot, so nothing about the base gun's cadence moves.
	const double FireInterval = FMath::Max(0.01f, UTraceSettings::Get().FireInterval)
		* UTraceAbilityComponent::GetFireIntervalScaleFor(Character);
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

// =================================================================================================
// AMMO  (spec v16 §1)
//
//     press    -> CanFire            refuses an empty clip and a running reload
//     shot     -> ConsumeRound       one round, on the machine that owns the decision
//     empty    -> BeginReload        anchored at the SHOT, so both ends compute one deadline
//     +0.5s    -> RefillClip         30 ordinary rounds, and the serial moves
//     R        -> RequestReload      predicted locally, stamped, re-gated on the server
//
// The state, the prediction split and the reasoning behind each field are on the header. This is the
// state machine.
// =================================================================================================

int32 UTraceWeaponComponent::GetClipAmmo() const
{
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor != nullptr && OwnerActor->HasAuthority())
	{
		// The truth, and the only reading that matters for a bot, a listen host's own pawn, or a
		// remote client's pawn as the server sees it.
		return static_cast<int32>(ClipAmmo);
	}

	// A remote owning client. -1 means the first replication has not arrived yet, in which case the
	// replicated field is still the best answer available (and is the class default until then).
	return (PredictedClipAmmo >= 0) ? PredictedClipAmmo : static_cast<int32>(ClipAmmo);
}

int32 UTraceWeaponComponent::GetAbilityRoundsInClip() const
{
	return static_cast<int32>(AbilityRoundsInClip);
}

bool UTraceWeaponComponent::IsReloading() const
{
	// The SHARED clock, exactly as IsDeploying() uses it, and for the same reason: this deadline is
	// computed on one machine and honoured on two, so it has to be expressed in the one clock they
	// agree on. (The fire-RATE gate deliberately uses the local clock instead — a clock resync must
	// never be able to stall the trigger. A reload is a replicated deadline; a fire rate is not.)
	return ReloadEndServerTime > 0.f && GetServerTimeSeconds() < static_cast<double>(ReloadEndServerTime);
}

float UTraceWeaponComponent::GetReloadRemaining() const
{
	if (ReloadEndServerTime <= 0.f)
	{
		return 0.f;
	}
	return static_cast<float>(FMath::Max(0.0, static_cast<double>(ReloadEndServerTime) - GetServerTimeSeconds()));
}

bool UTraceWeaponComponent::ShouldShowAmmo() const
{
	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || !Character->IsAlive())
	{
		return false;
	}

	// *** SPEC v16 §1: "The Core carrier has no gun, so ammo must not be consumed or shown while
	// carrying." *** The HUD asks this rather than asking IsCarrier() itself, so that the "has no
	// gun" rule has ONE definition. The consumption half of the same sentence is enforced twice over
	// — CanFire()/ServerFire() refuse a carrier, and ConsumeRound carries its own guard.
	if (Character->IsCarrier())
	{
		return false;
	}

	// The knife has no magazine. Drawing "30" beside a blade would be worse than drawing nothing.
	return !IsKnifeEquipped();
}

void UTraceWeaponComponent::ConsumeRound()
{
	if (!TraceAmmo::IsEnabled())
	{
		// THE RED ARM. The clip never falls, so nothing ever empties and nothing ever reloads — the
		// build that existed before spec v16 §1, reachable in one CVar.
		return;
	}

	// *** SPEC v16 §1 / v14 §4, THE SECOND CARRIER LOCK. ***
	//
	// CanFire() and ServerFire() both refuse a carrier before a shot can ever reach this function, so
	// in the shipped build this branch is unreachable — which is exactly the argument
	// UTraceHealthComponent::ApplyVulnerable makes for ITS second carrier lock, and exactly why that
	// one exists: the rule has to survive a future caller who reaches the consumption path another
	// way. Both predicates are asked (the Core's authoritative pointer AND the pawn's replicated
	// mirror) so that a frame in which the two disagree cannot open the door.
	const ATraceCharacter* Character = GetTraceCharacter();
	const bool bCarrying = Character != nullptr
		&& (ATraceCore::IsCoreHolder(Character) || Character->IsCarrier());
	if (bCarrying)
	{
		if (TraceAmmo::IsCarrierGuardArmed())
		{
			return;
		}

		// The red arm reached a carrier. Counted and logged as an Error so a harness can prove the
		// arm actually disarmed something rather than reporting a green that had no rule to break.
		++GAmmoCarrierRoundsConsumed;
		UE_LOG(LogTraceGame, Error,
			TEXT("[Ammo] *** A ROUND WAS SPENT FROM THE CORE CARRIER %s's CLIP (round #%d). Spec v16 §1: the "
			     "carrier has no gun. This is only reachable with Trace.Ammo.CarrierGuard 0."),
			*GetNameSafe(GetOwner()), GAmmoCarrierRoundsConsumed);
	}

	const AActor* OwnerActor = GetOwner();
	const bool bAuthority = (OwnerActor != nullptr && OwnerActor->HasAuthority());

	bLastRoundWasAbilityRound = false;

	if (bAuthority)
	{
		if (ClipAmmo == 0)
		{
			return;
		}
		--ClipAmmo;

		// The ability-loaded rounds are spent FIRST and are simply the front of the clip. Recording
		// which kind of round just left is what lets UTraceAbilitySetX::NotifyBulletHit answer "was
		// that a bee round" a few statements later in ServerFire — including for the fifth and last
		// one, where "are there any left" would already say no.
		if (AbilityRoundsInClip > 0)
		{
			--AbilityRoundsInClip;
			bLastRoundWasAbilityRound = true;
		}

		++TotalRoundsConsumed;
		++GAmmoRoundsConsumed;
		return;
	}

	// A predicting client. Seed from the replicated truth on the first shot of the connection, so a
	// player who fires before their first ammo update does not start from -1.
	if (PredictedClipAmmo < 0)
	{
		PredictedClipAmmo = static_cast<int32>(ClipAmmo);
	}
	PredictedClipAmmo = FMath::Max(0, PredictedClipAmmo - 1);
	bLastRoundWasAbilityRound = (AbilityRoundsInClip > 0);

	// Counted on a client too, and the counter means something slightly different there: PREDICTED
	// rounds rather than spent ones. Said out loud because a client-side Trace.Ammo.Dump would
	// otherwise look like it was reporting authoritative numbers.
	++GAmmoRoundsConsumed;
}

void UTraceWeaponComponent::RefillClip(int32 Rounds, int32 AbilityRounds)
{
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority())
	{
		// Clients receive a refill by replication and predict their own through TickReload. A local
		// write here would be a number they briefly believed in before the server contradicted it.
		return;
	}

	const int32 NewRounds = FMath::Clamp(Rounds, 0, 255);
	ClipAmmo = static_cast<uint8>(NewRounds);
	AbilityRoundsInClip = static_cast<uint8>(FMath::Clamp(AbilityRounds, 0, NewRounds));
	ReloadEndServerTime = -1.f;

	// *** THE SERIAL IS THE POINT OF THIS FUNCTION. *** It is what tells an owning client "this is a
	// NEW clip, throw your prediction away" as distinct from "I am simply behind your shots". Without
	// it, OnRep_Ammo could not tell a refill from a stale packet and would have to choose between
	// refusing refills and refunding bullets. Wraps at 255, which is harmless — the reconcile only
	// ever asks whether it CHANGED, never by how much.
	++ClipSerial;

	// Deliberately does NOT call OnRep_Ammo() by hand, which is the opposite of what this project
	// does for Health and the vulnerable mark. Those OnReps do presentation work every machine needs;
	// this one only reconciles the CLIENT's prediction mirror, and the authority has no mirror to
	// reconcile — it reads ClipAmmo directly (see GetClipAmmo).
}

void UTraceWeaponComponent::BeginReload(double AnchorSharedTime)
{
	if (!TraceAmmo::IsEnabled())
	{
		return;
	}

	const double Now = GetServerTimeSeconds();

	// *** THE CLAMP IS THE SECURITY, AND ITS COST IS WORTH STATING IN NUMBERS. ***
	//
	// The anchor comes from the machine that pressed the key or fired the shot, so it is client data
	// and is pinned into [Now - MaxRewindTime, Now] exactly as ServerFire pins a shot's timestamp:
	// never in the future, so a reload cannot be pre-booked, and never further back than a bullet may
	// be rewound.
	//
	// What a liar buys with the full 0.25 s of back-dating is half a reload. Against a 30-round clip
	// at FireInterval 0.40 that is 0.25 s off a 12.5 s cycle — about 2% more sustained DPS, which is
	// an order of magnitude less than the 20% the fire-rate gate already forgives honest jitter
	// (FireRateTolerance). Anchoring is what stops a 40 ms client's reload from actually taking
	// 0.54 s, which is a 9% error in one of the three numbers spec v16 §1 gives; refusing to anchor
	// to avoid a 2% exploit would be trading a real defect for a smaller one.
	const double MaxRewind = FMath::Max(0.f, UTraceSettings::Get().MaxRewindTime);
	const double Anchor = FMath::Clamp(AnchorSharedTime, Now - MaxRewind, Now);

	ReloadEndServerTime = static_cast<float>(Anchor + static_cast<double>(TraceAmmo::GetReloadSeconds()));
	bPredictedRefillPending = false;
}

void UTraceWeaponComponent::CancelReload()
{
	ReloadEndServerTime = -1.f;
	bPredictedRefillPending = false;
}

void UTraceWeaponComponent::TickReload()
{
	const AActor* OwnerActor = GetOwner();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || Character == nullptr)
	{
		return;
	}

	const bool bAuthority = OwnerActor->HasAuthority();

	// A SIMULATED PROXY OWNS NO AMMO DECISION AT ALL. It has no replicated count (COND_OwnerOnly), so
	// letting it run this state machine would have it "reloading" a clip it cannot see, on numbers
	// that are class defaults. Authority decides; the owning client predicts; everyone else watches.
	if (!bAuthority && !Character->IsLocallyControlled())
	{
		return;
	}

	if (!TraceAmmo::IsEnabled())
	{
		// THE RED ARM: the mechanic is ABSENT, not half-present. Any reload already in flight is
		// abandoned rather than left to complete, so a red run cannot report a reload it inherited
		// from the green build that preceded it.
		if (ReloadEndServerTime > 0.f)
		{
			CancelReload();
		}
		return;
	}

	// --- THE TWO CANCELLATIONS. Spec v16 §1 [ASSUMPTION]: "reloading is cancelled by death and by
	//     picking up the Core." ---------------------------------------------------------------------
	const bool bDead = !Character->IsAlive();
	const bool bCarrying = Character->IsCarrier();
	if (bDead || bCarrying)
	{
		if (ReloadEndServerTime > 0.f)
		{
			CancelReload();
		}

		// A NEW LIFE STARTS WITH A FULL CLIP, and it is done from the death side rather than from a
		// respawn hook because this component has no respawn hook to hang it on — ATraceCharacter is
		// another ownership slice. Refilling while dead is equivalent and needs nothing from anyone
		// else. The guard is what keeps it from bumping the serial (and therefore replicating) sixty
		// times a second for the whole respawn countdown.
		if (bAuthority && bDead
			&& (static_cast<int32>(ClipAmmo) != TraceAmmo::GetClipSize() || AbilityRoundsInClip != 0))
		{
			RefillClip(TraceAmmo::GetClipSize(), 0);
		}
		return;
	}

	// --- COMPLETION --------------------------------------------------------------------------------
	if (ReloadEndServerTime > 0.f && GetServerTimeSeconds() >= static_cast<double>(ReloadEndServerTime))
	{
		if (bAuthority)
		{
			RefillClip(TraceAmmo::GetClipSize(), 0);
			++TotalReloadsCompleted;
			++GAmmoReloadsCompleted;

			UE_LOG(LogTraceGame, Verbose, TEXT("[Ammo] %s reloaded: %d rounds."),
				*GetNameSafe(GetOwner()), TraceAmmo::GetClipSize());
		}
		else
		{
			// THE PREDICTED REFILL. The client's gun comes back up on ITS deadline rather than a ping
			// later, and bPredictedRefillPending is what stops the server's last pre-refill packet
			// from yanking the count back to empty for that ping — see OnRep_Ammo.
			PredictedClipAmmo = TraceAmmo::GetClipSize();
			bPredictedRefillPending = true;
			ReloadEndServerTime = -1.f;
			++GAmmoReloadsCompleted;
		}
	}

	// --- THE AUTOMATIC RELOAD. "30 bullets per clip, then the gun reloads." ------------------------
	//
	// The ordinary path starts this at the SHOT that emptied the clip (ServerFire / FireOnce), which
	// is what gets both machines onto one deadline. This is the safety net for every other way a pawn
	// can end up standing there with an empty gun: a reload cancelled by picking up the Core and then
	// passing it on, a death mid-reload, a clip emptied by something other than the trigger.
	//
	// *** IT IS ALSO THE WHOLE OF "BOTS MUST RELOAD". *** A bot's pawn is authoritative and locally
	// controlled in the same process, so it runs this identical branch. There is not one line of AI
	// code in the ammo feature, and a bot that dry-fires forever is impossible by construction rather
	// than by a rule somebody remembered to add to the bot controller.
	if (GetClipAmmo() <= 0 && !IsReloading() && !IsKnifeEquipped())
	{
		BeginReload(GetServerTimeSeconds());
	}
}

bool UTraceWeaponComponent::RequestReload()
{
	const AActor* OwnerActor = GetOwner();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || Character == nullptr || !TraceAmmo::IsEnabled())
	{
		return false;
	}

	// Every refusal below is a rule; see the header for why each one is where it is.
	if (!Character->IsAlive() || Character->IsCarrier())
	{
		return false;
	}
	if (IsKnifeEquipped() || IsDeploying())
	{
		return false;
	}
	if (IsReloading())
	{
		return false;
	}
	if (GetClipAmmo() >= TraceAmmo::GetClipSize())
	{
		// A FULL CLIP: NOTHING AT ALL. [ASSUMPTION], spec v16 §1. Restarting the timer instead would
		// let a player weld their own gun shut for half a second by leaning on R, which no shooter
		// does and which would read as the bind being broken.
		return false;
	}

	// *** AN ABILITY-LOADED CLIP IS NOT MANUALLY RELOADABLE. [ASSUMPTION], spec v16 §1. ***
	//
	// X's Sting puts five bee rounds in the gun off a 25 s cooldown, and R would otherwise throw them
	// on the floor on a mis-press — a whole ability lost to a reflex, with no undo and no feedback.
	// The spec's own sentence assumes they get FIRED ("after shooting those, his next reload is
	// normal"), so refusing here is the reading that matches it. Reverse it by deleting this block if
	// playtesting wants a dump.
	if (AbilityRoundsInClip > 0)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Ammo] %s: manual reload refused — the clip holds %d ability round(s) and they must be fired."),
			*GetNameSafe(GetOwner()), static_cast<int32>(AbilityRoundsInClip));
		return false;
	}

	const double PressTime = GetServerTimeSeconds();

	if (OwnerActor->HasAuthority())
	{
		BeginReload(PressTime);
		return true;
	}

	if (!Character->IsLocallyControlled())
	{
		// A proxy copy of somebody else's pawn. Input is a local concept.
		return false;
	}

	// Predict, then ask. Both ends anchor at PressTime, so the server's copy of the deadline arrives
	// as a no-op rather than as an extension.
	BeginReload(PressTime);
	ServerRequestReload(static_cast<float>(PressTime));
	return true;
}

bool UTraceWeaponComponent::ServerRequestReload_Validate(float ClientPressServerTime)
{
	// Validation failure disconnects the client, so only reject what is impossible to reason about.
	return FMath::IsFinite(ClientPressServerTime);
}

void UTraceWeaponComponent::ServerRequestReload_Implementation(float ClientPressServerTime)
{
	const AActor* OwnerActor = GetOwner();
	ATraceCharacter* Character = GetTraceCharacter();
	if (OwnerActor == nullptr || Character == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}
	if (!TraceAmmo::IsEnabled())
	{
		return;
	}

	// RE-GATED FROM SCRATCH, never trusted. The client gated on its own copy of all of this before it
	// sent the RPC; that gate is for FEEL, this one is the rule. A client that has been corrected
	// into a state where the reload is illegal simply gets nothing.
	if (!Character->IsAlive() || Character->IsCarrier() || IsKnifeEquipped() || IsDeploying())
	{
		return;
	}
	if (IsReloading() || ClipAmmo >= static_cast<uint8>(TraceAmmo::GetClipSize()) || AbilityRoundsInClip > 0)
	{
		return;
	}

	BeginReload(static_cast<double>(ClientPressServerTime));
}

void UTraceWeaponComponent::LoadAbilityClip(int32 RoundCount)
{
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || !OwnerActor->HasAuthority())
	{
		return;
	}

	// REPLACES, never adds — spec v16 §1's [ASSUMPTION], stated there: "Sting overwrites whatever was
	// in the clip (20 rounds left -> 5 bee rounds)". RefillClip clears any running reload too, so an
	// ability cast during a reload puts the gun up immediately instead of stranding the player behind
	// a timer they can no longer see the point of.
	const int32 Rounds = FMath::Clamp(RoundCount, 0, TraceAmmo::GetClipSize());
	RefillClip(Rounds, Rounds);

	UE_LOG(LogTraceGame, Log, TEXT("[Ammo] %s: the clip is now %d ability round(s)."),
		*GetNameSafe(GetOwner()), Rounds);
}

void UTraceWeaponComponent::OnRep_Ammo()
{
	// CLIENTS ONLY (replication callbacks never fire on the authority), and its whole job is to
	// reconcile the prediction mirror with the truth. Three cases, and the third is the interesting
	// one.
	if (PredictedClipAmmo < 0)
	{
		// First update of the connection. Adopt everything; there is nothing predicted to protect.
		PredictedClipAmmo = static_cast<int32>(ClipAmmo);
		PredictedClipSerial = ClipSerial;
		bPredictedRefillPending = false;
		return;
	}

	if (ClipSerial != PredictedClipSerial)
	{
		// A NEW CLIP. The server reloaded, an ability replaced the clip, or this pawn respawned. The
		// prediction is about a magazine that no longer exists, so it is discarded outright.
		PredictedClipSerial = ClipSerial;
		PredictedClipAmmo = static_cast<int32>(ClipAmmo);
		bPredictedRefillPending = false;
		return;
	}

	if (bPredictedRefillPending)
	{
		// We have finished a reload the server has not told us about yet, so every packet still in
		// flight describes the OLD, nearly empty clip under the OLD serial. Taking it would drop the
		// count back to 1 or 2 for a ping — at the exact moment the player is reading the number to
		// decide whether to re-engage. Wait for the serial to move.
		return;
	}

	// SAME CLIP, and our shots are ahead of the server's knowledge of them, so the server's count can
	// only be >= ours. Take the LOWER: a stale packet must never hand a round back. The cost of being
	// wrong here is that a shot the server REJECTED (rate limit, a state change) leaves the client one
	// round pessimistic until the next refill re-seats both — which is the right direction to be
	// wrong in, because it can only ever make the client reload early, never fire a round the server
	// will not honour.
	PredictedClipAmmo = FMath::Min(PredictedClipAmmo, static_cast<int32>(ClipAmmo));
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

	// --- AMMO: the reload state machine (spec v16 §1) ---------------------------------------------
	//
	// DELIBERATELY ABOVE THE locally-controlled GATE. The server has to run this for every pawn it
	// owns, and most of those — every remote human — are not locally controlled here. Putting it
	// below would have left exactly the players with the worst latency as the ones whose guns never
	// reloaded. TickReload decides for itself which machines own a decision.
	TickReload();

	// --- EVERY MACHINE: the knife's movement profile ---------------------------------------------
	//
	// Deliberately before the locally-controlled gate and unconditional. The bit has to be right on
	// the server, on the owning client that predicts its own moves, AND on the machines simulating
	// this pawn — see RefreshMovementProfile. It is idempotent and only writes on a change.
	RefreshMovementProfile();

	// --- EVERY RENDERING MACHINE: the knife you can see ------------------------------------------
	//
	// Deliberately BEFORE the locally-controlled gate. The third-person knife in an enemy's hand is
	// the tell that they are 22% faster and cannot shoot back, and that tell has to appear on the
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
	// =============================================================================================
	// SPEC v25 §5 — "Remove gun recoil, keep the firing rate."
	// =============================================================================================
	//
	// THIS EARLY RETURN IS THE REMOVAL, and bRecoilEnabled is now false in both the header default
	// and DefaultGame.ini. It is the only door into the aim punch: this function is called from
	// exactly one place (FireOnce, after the shot has been sampled and sent) and it is the only
	// caller of AddRecoilPitch that ever ADDS pitch. With the flag off nothing ever writes to the
	// control rotation from this component, RecoilAppliedPitch never leaves 0.0, and TickRecoil
	// returns on its first line. Measure it with Trace.TestRecoil: peak climb, residual and yaw
	// drift must all read 0.000.
	//
	// THE VIEWMODEL KICK IS A DIFFERENT MECHANISM AND IS DELIBERATELY STILL RUNNING. FireOnce calls
	// Character->NotifyWeaponFired() a few lines before it calls this; that jolts the first-person
	// gun MESH (ATraceCharacter::ViewModelKick) and settles it. It never touches the control
	// rotation, so it cannot move the crosshair or the round. §5 asks for the thing that moves your
	// AIM, which is this function, not that one. The muzzle flash and the tracer likewise stay.
	//
	// FireInterval (190 RPM) is not read anywhere in this file's recoil path and did not change.
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

	// --- AMMO: the CLIENT-PREDICTED half (spec v16 §1) -------------------------------------------
	//
	// *** ONLY OFF THE AUTHORITY, AND THAT CONDITION IS NOT AN OPTIMISATION. *** On a listen host's
	// own pawn and on every bot, this function and ServerFire_Implementation both run in this same
	// process — ServerFire is a local call there, not an RPC — so consuming in both would spend two
	// rounds per trigger pull and halve the clip. Each machine consumes on exactly one side of that
	// pair: the authority in ServerFire, a remote client here.
	//
	// The count therefore drops on the shooter's HUD in the same frame as the muzzle flash, which is
	// the "client-predicted for feel" half of §1, and the server's copy arrives ~RTT/2 later and
	// reconciles through OnRep_Ammo rather than overwriting.
	const AActor* FireOwner = GetOwner();
	if ((FireOwner == nullptr || !FireOwner->HasAuthority()) && TraceAmmo::IsPredictionEnabled())
	{
		ConsumeRound();

		// The automatic reload, anchored at THIS SHOT rather than at "now". The server anchors its own
		// at the same (clamped) stamp when it processes the same round, so both machines compute one
		// deadline — without that, a 40 ms client would come off a 0.5 s reload and find the server
		// still refusing to fire for another 40 ms, every single clip.
		if (TraceAmmo::IsEnabled() && GetClipAmmo() <= 0 && !IsReloading())
		{
			BeginReload(FireServerTime);
		}
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
	//
	// SPEC v25 §5 KEEPS THIS. It moves the first-person gun MESH, not the control rotation — the
	// crosshair does not move, the camera does not move, and the round has already been sampled and
	// sent above regardless. "Remove recoil" means the aim punch (ApplyRecoilKick, at the bottom of
	// this function, now disabled); removing the mesh animation as well would delete the gun's only
	// feedback that it fired while removing no aim penalty at all. Same reasoning keeps the muzzle
	// flash and the tracer (PlayLocalTracer, just above), which the spec names as out of scope.
	Character->NotifyWeaponFired();

	ServerFire(FVector_NetQuantize(Origin), FVector_NetQuantizeNormal(Dir), static_cast<float>(FireServerTime));

	// UPWARDS RECOIL, LAST — AND THE ORDER IS THE WHOLE DESIGN (spec v5 section 6).
	//
	// *** SPEC v25 SECTION 5 DISABLES THIS. *** ApplyRecoilKick() returns on its first line while
	// bRecoilEnabled is false (header default AND DefaultGame.ini, both now false), so the call
	// below is inert and no pitch is ever written to the control rotation. The call is left in
	// place, not deleted, because the flag is the A/B arm the recoil was built with and the ordering
	// note below is what has to stay true if it is ever switched back on. THE FIRE RATE ABOVE IS
	// UNCHANGED: FireInterval is still 0.315789 s = 190 RPM.
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

	// ---- AMMO (spec v16 §1): the SERVER's copy of the gate the client fired through ---------
	//
	// The client gated on its own predicted clip before it sent this; that gate is for feel, and a
	// modified client simply would not run it. Re-asked here, this is the rule — without it "30
	// bullets per clip" would be a client-side suggestion.
	if (TraceAmmo::IsEnabled())
	{
		if (ClipAmmo == 0)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: %s fired on an empty clip"), *GetNameSafe(OwnerActor));
			if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedState; }
			return;
		}

		// THE GRACE EXISTS FOR CLOCK ESTIMATION, NOT FOR CHEATS, and it is the same argument
		// FireRateTolerance makes. Both ends anchor the reload at one stamped instant, so they agree
		// to within each machine's error in its estimate of the shared clock — tens of milliseconds.
		// Refusing a shot inside that error would drop the first round of a burst after every single
		// reload, for honest players only. What 50 ms of forgiveness buys a liar is one extra round
		// per reload at most, because the fire-rate gate is 0.32 s wide.
		constexpr double ReloadGateGraceSeconds = 0.05;
		if (TraceAmmo::DoesReloadBlockFire()
			&& ReloadEndServerTime > 0.f
			&& GetServerTimeSeconds() < static_cast<double>(ReloadEndServerTime) - ReloadGateGraceSeconds)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("ServerFire: %s fired %.3fs into a reload"),
				*GetNameSafe(OwnerActor), TraceAmmo::GetReloadSeconds() - GetReloadRemaining());
			if (bCollectStats) { ++TraceShotStats::GStats.ServerRejectedState; }
			return;
		}
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	// ---- fire rate, with slack for honest jitter -----------------------------------------
	//
	// SPEC v18 §2: scaled by the SAME per-character seam CanFire() uses, and it has to be. If only the
	// client's gate scaled, a Roxie with Modded up would fire every 0.242 s and this validation would
	// reject every second shot as rate-limited — which reads as the gun eating bullets, i.e. strictly
	// worse than the ability not working at all. The server re-derives the scale from its own
	// authoritative ability state rather than trusting anything in the RPC payload, so a client that
	// lied about being Roxie gains nothing.
	const double FireInterval = FMath::Max(0.01f, Settings.FireInterval)
		* UTraceAbilityComponent::GetFireIntervalScaleFor(Character);
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

	// ---- AMMO: the AUTHORITATIVE round, spent (spec v16 §1) --------------------------------
	//
	// After the rate gate (so a rejected shot costs nothing) and after RewindTime is resolved (so the
	// automatic reload can be anchored at the same clamped instant the bullet is resolved at, which is
	// the same instant the client anchored ITS predicted reload at in FireOnce). Before the trace,
	// because a round leaves the gun whether or not it finds anybody.
	ConsumeRound();
	if (TraceAmmo::IsEnabled() && ClipAmmo == 0 && !IsReloading())
	{
		BeginReload(RewindTime);
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
			// NOT const: the ability passives below may modify it (spec v14 §6). The ZONE is still
			// the only thing that decides the base number — see spec section 6, head 100 / body 40 /
			// legs 25 — and a character with no passives leaves it exactly as it was.
			float Damage = FTraceHitZoneModel::DamageForZone(Zone);

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

			// SPEC v14 §6 — THE ABILITY PASSIVE PIPELINE, FOLDED IN AT THE ONE PLACE A BULLET'S
			// NUMBER IS DECIDED.
			//
			// This runs the INSTIGATOR's outgoing passives and the TARGET's incoming ones — Chut's
			// Chud is "30% less damage from body shots", and a body shot is this line. It is a no-op
			// for a Mannequin and for every player who has not picked a character, so mode A and bots
			// are unaffected.
			//
			// *** X's VULNERABLE IS NOT APPLIED HERE, AND MUST NOT BE. *** "+25% damage from all
			// sources" is one multiplication inside UTraceHealthComponent::ApplyDamage, which this
			// call is about to reach; UTraceAbilitySetX deliberately leaves
			// GetIncomingDamageMultiplier() at 1 so that the amplification cannot happen twice on one
			// bullet. See the block comment at the top of TraceHealthComponent.h.
			FTraceAbilityDamageContext AbilityContext;
			AbilityContext.Instigator   = Character;
			AbilityContext.Target       = Victim;
			AbilityContext.Cause        = DamageCause;
			AbilityContext.bHeadshot    = (Zone == ETraceHitZone::Head);
			AbilityContext.bMelee       = false;
			AbilityContext.bFromAbility = false;
			Damage = UTraceAbilityComponent::ModifyDamageThroughPassives(Damage, AbilityContext);

			VictimHealth->ApplyDamage(Damage, Character->GetController(), DamageCause);

			// ApplyDamage no-ops against an invulnerable target, so read the result rather than
			// assuming the hit landed.
			bKilled = !VictimHealth->IsAlive();

			// SPEC v14 §6 — X's STING: "His NEXT FIVE BULLETS apply vulnerable on hit, at NORMAL
			// damage." AFTER the damage, deliberately: the delivering bullet must not be amplified by
			// the mark it delivers, and being on this side of ApplyDamage is what guarantees it
			// rather than a comment asking the next reader to be careful.
			//
			// One character-agnostic call. See Abilities/Characters/TraceAbilityWeaponHooks.h for why
			// the gun does not know X's class name.
			TraceAbilityWeaponHooks::OnBulletHit(Character, Victim, Zone == ETraceHitZone::Head);
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

	// ---------------------------------------------------------------------------------------------
	// NO SLASH ACTOR IS SPAWNED HERE ANY MORE. SPEC v12 §2, verbatim: "Take the 3d 'swipe' line
	// animation of the knife out of the player's view."
	//
	// This call site was the swinger's OWN copy of ATraceMeleeArc — a fan of emissive chords swept
	// across 140 degrees at blade-tip radius, drawn 140 uu in front of the camera, which is what the
	// user was looking at when they asked for it to go. It is deleted rather than made conditional:
	// there is no state in which the swinger wants a lateral arc drawn over their own crosshair now
	// that the motion is a thrust.
	//
	// WHAT REPLACES IT is the viewmodel itself — UpdateKnifeVisuals now drives KnifeViewRoot forward
	// and back rather than across, so the swinger's read is the blade going out and coming home. That
	// is procedural, and deliberately so; see the note there.
	//
	// THE THIRD-PERSON SLASH IS UNTOUCHED. MulticastSwingEffects still spawns one for everybody
	// ELSE, because a victim with no first-person blade to look at needs some tell that a hundred
	// damage just came from a direction, and it is drawn with the same GetSwingArcDegrees() the
	// server actually resolved — drawn volume and lethal volume stay equal. Reshaping that actor into
	// a thrust belongs to whoever owns TraceMeleeArc.cpp; see the pass report.
	//
	// THE LOCAL RESOLVE ABOVE IS KEPT even though nothing cosmetic consumes it now. It is the client
	// half of the "compare the two resolutions line for line" contract in TraceMelee.h's FTraceMeleeHit
	// comment, and it is the only way a mispredicted swing is visible in a log at all — the server's
	// verdict alone cannot show a disagreement. It costs one 15-ray sweep per 0.5 s per player and
	// applies no damage on any client, ever.
	// ---------------------------------------------------------------------------------------------
	if (TraceMelee::IsDebugLoggingEnabled())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Knife] %s predicted swing: victim %s, %s, %.0f damage (client-side, cosmetic only)"),
			*GetNameSafe(Character), *GetNameSafe(Predicted.Victim),
			Predicted.bBackstab ? TEXT("BACKSTAB") : TEXT("front"), Predicted.Damage);
	}

	ServerSwing(FVector_NetQuantize(Origin), FVector_NetQuantizeNormal(Dir), static_cast<float>(SwingServerTime));
}

// -------------------------------------------------------------------------------------------------
// The swap
//
// DoSwapWeaponPressed() lived here and is deleted (spec v15 §5) — see the note at its old declaration
// in TraceWeaponComponent.h. The toggle verb survives once, in TraceMelee::RequestSwapWeapon.
// -------------------------------------------------------------------------------------------------

bool UTraceWeaponComponent::RequestEquipIfDifferent(ETraceEquippedWeapon Desired, ETraceMeleeRefusal* OutRefusal)
{
	if (OutRefusal != nullptr)
	{
		*OutRefusal = ETraceMeleeRefusal::None;
	}

	// THE ONLY LINE THAT DIFFERS FROM RequestEquip, and it is checked BEFORE the legality gates on
	// purpose. "Already holding it" is not a refusal — nothing was asked for — so a dead or carrying
	// player pressing the bind for the weapon they nominally hold produces None rather than Dead or
	// Carrying. The caller logs refusals, and a refusal reason for a press that asked for nothing is
	// noise that reads like a bug report.
	//
	// EquippedWeapon is replicated, so the client's answer and the server's are the same answer; this
	// is not a second opinion about legality, and no state is duplicated to reach it.
	if (EquippedWeapon == Desired)
	{
		return false;
	}

	return RequestEquip(Desired, OutRefusal);
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
		// they may use, and would be collecting the +22% movement bonus while doing it. See
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
	// movement at knife speed (976 uu/s is 16 uu per frame at 60 Hz).
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

			// SPEC v14 §6 — the ability passive pipeline, exactly as ServerFire does it. Chut's Chud
			// is "30% less damage from body shots AND MELEES", so the knife has to pass through it
			// too, and bMelee is the flag that tells it apart from a bullet.
			//
			// X's vulnerable is again NOT here: it is one multiplication inside
			// UTraceHealthComponent::ApplyDamage, which the next line reaches. A knife on a marked
			// target is amplified there, once.
			//
			// The carrier is unaffected by any of this: TraceMelee::ResolveSwing already refused to
			// return a carrier as a victim (Hit.bBlockedByCarrierShield below), so this branch is
			// unreachable for one — which is the proven 5/5 rule, untouched.
			FTraceAbilityDamageContext AbilityContext;
			AbilityContext.Instigator   = Character;
			AbilityContext.Target       = Hit.Victim;
			AbilityContext.Cause        = Cause;
			AbilityContext.bHeadshot    = (Hit.Zone == ETraceHitZone::Head);
			AbilityContext.bMelee       = true;
			AbilityContext.bFromAbility = false;
			const float MeleeDamage = UTraceAbilityComponent::ModifyDamageThroughPassives(Hit.Damage, AbilityContext);

			VictimHealth->ApplyDamage(MeleeDamage, Character->GetController(), Cause);

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
	// *** DEMO 19 ITEM 2: "Don't let the bots attack." ***
	//
	// FIRST, before the cvar and before anything is measured, because this is a statement about the
	// BODY rather than about the feature. A practice-range target is possessed by a controller that
	// steers nothing and presses nothing — but this function never asked the controller what it
	// wanted. It fires for every pawn that is not player-controlled, so the range's five stationary
	// targets were drawing the blade and swinging it at anyone who came within 153 uu: which is
	// exactly the distance you have to close to knife one. See SetAutonomousAttacksAllowed.
	if (!bAutonomousAttacksAllowed)
	{
		return;
	}

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

	// The band IS the rule: inside Engage the +22% makes the knife the correct chase tool, outside
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

	// ---------------------------------------------------------------------------------------------
	// THE STAB  (spec v12 §2: "Can you make the knife animation a stab instead of a swipe?")
	//
	// SAY THIS PLAINLY: THIS IS A PROCEDURAL VIEWMODEL MOTION, NOT AN AUTHORED ANIMATION. Epic's
	// Mannequin ships no stab, no slash and no melee sequence of any kind — the imported set is
	// Death, Jump, Pistol, Rifle and Unarmed locomotion, verified previously, and it is the same
	// absence that forced UpdateCrouchPresentation to pose the slide by hand. There is no sequence to
	// play, so the motion is arithmetic on KnifeViewRoot's relative transform, three keyframes deep.
	//
	// WHAT CHANGED FROM THE SWIPE IT REPLACES, and it is the whole point of the request: the old
	// motion's signature was SweepYaw -58 and SweepRoll +46, i.e. the blade travelled ACROSS the
	// screen and rolled over as it went. A stab has no lateral signature at all. Everything below is
	// dominated by +X — straight out of the lens, down the aim ray — with only a few degrees of yaw
	// and pitch so it reads as an arm rather than as a mesh sliding on rails. Roll is gone entirely;
	// roll is what made it a slash.
	//
	// FAST OUT, SLOWER RETURN, which is the shape the spec asked for and also the shape of a real
	// thrust: the arm commits in a snap and recovers under control. Out is a quarter of the
	// post-windup time, the return is the other three quarters — see UpdateKnifeVisuals.
	//
	// DEPTH ARITHMETIC, which has to keep holding (see the layout note at the top of this namespace):
	// the deepest point of the rig is the blade tip at x = 21.4 + 3.6/2 ~= 23 uu. A 16 uu thrust puts
	// it at ~39 uu. The gun's muzzle sits at 76 uu and is the piece that had to clear the capsule, so
	// the fully extended blade is still barely half way there and can never intersect the world.
	// Raising ThrustForwardUU past ~50 would start to matter; do not.

	/** Cock: the blade draws back and the tip lifts, ready to go. Eased IN, so it loads the thrust. */
	constexpr float CockBackUU = 4.6f;
	constexpr float CockUpUU = 1.4f;
	constexpr float CockInboardUU = 1.2f;
	constexpr float CockPitch = -13.f;
	constexpr float CockYaw = 8.f;

	/** Thrust: out along the lens axis and slightly toward the crosshair. The lateral terms are small
	 *  ON PURPOSE — anything larger reads as the swipe this replaced. */
	constexpr float ThrustForwardUU = 16.0f;
	constexpr float ThrustInboardUU = -2.4f;
	constexpr float ThrustUpUU = 1.0f;
	constexpr float ThrustPitch = 6.f;
	constexpr float ThrustYaw = -5.f;

	/** Fraction of the post-cock time spent going OUT. The rest is the return. 0.25 => 1:3, fast:slow. */
	constexpr float ThrustOutFraction = 0.25f;

	/**
	 * THE STAB CURVE, and the only copy of it.
	 *
	 * @param Alpha       0..1 through the whole animation.
	 * @param CockEnd     where the cock beat ends, as a fraction of Alpha (the wind-up over the anim).
	 * @param OutOffset   relative translation for KnifeViewRoot, in rig space (+X out of the lens).
	 * @param OutRotation relative rotation. Roll is always zero — roll is what made the old motion a
	 *                    swipe, and its absence is half of what makes this one a stab.
	 *
	 * Shared by UpdateKnifeVisuals (which renders it) and Trace.Knife.StabProfile (which measures
	 * it). One derivation on purpose: the profile command's whole job is to say whether the motion is
	 * forward-dominant and fast-out/slow-return, and it could not answer that about a curve it had
	 * its own copy of.
	 */
	inline void ComputeStabPose(float Alpha, float CockEnd, FVector& OutOffset, FRotator& OutRotation)
	{
		// The two poses the motion interpolates between, named once so the three beats cannot
		// disagree about where "cocked" and "extended" are.
		const FVector CockOffset(-CockBackUU, CockInboardUU, CockUpUU);
		const FRotator CockRotation(CockPitch, CockYaw, 0.f);
		const FVector ThrustOffset(ThrustForwardUU, ThrustInboardUU, ThrustUpUU);
		const FRotator ThrustRotation(ThrustPitch, ThrustYaw, 0.f);

		Alpha = FMath::Clamp(Alpha, 0.f, 1.f);
		CockEnd = FMath::Clamp(CockEnd, 0.05f, 0.6f);

		if (Alpha < CockEnd)
		{
			const float T = FMath::Clamp(Alpha / CockEnd, 0.f, 1.f);
			const float Eased = T * T;                                   // ease in: loads the thrust
			OutOffset = FMath::Lerp(FVector::ZeroVector, CockOffset, Eased);
			OutRotation = FMath::Lerp(FRotator::ZeroRotator, CockRotation, Eased);
			return;
		}

		// Where we are inside the post-cock portion, 0..1.
		const float Post = FMath::Clamp((Alpha - CockEnd) / FMath::Max(0.01f, 1.f - CockEnd), 0.f, 1.f);
		const float OutEnd = FMath::Clamp(ThrustOutFraction, 0.05f, 0.9f);

		if (Post < OutEnd)
		{
			const float T = FMath::Clamp(Post / OutEnd, 0.f, 1.f);
			const float Eased = 1.f - FMath::Square(1.f - T);            // ease out: snap, then settle
			OutOffset = FMath::Lerp(CockOffset, ThrustOffset, Eased);
			OutRotation = FMath::Lerp(CockRotation, ThrustRotation, Eased);
			return;
		}

		const float T = FMath::Clamp((Post - OutEnd) / FMath::Max(0.01f, 1.f - OutEnd), 0.f, 1.f);
		const float Eased = T * T * (3.f - 2.f * T);                     // smoothstep home
		OutOffset = FMath::Lerp(ThrustOffset, FVector::ZeroVector, Eased);
		OutRotation = FMath::Lerp(ThrustRotation, FRotator::ZeroRotator, Eased);
	}
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
		// Ask the owner first. The child-walk below identifies the gun's lit channels by finding
		// "Neon" in a COMPONENT NAME, which only ever worked because the gun was a table of cubes
		// named after their function. The railgun's parts are named after their meshes, so on that
		// rig the search matches nothing and the knife drops all the way to BasicShapeMaterial —
		// visible as a knife that is not made of the same stuff as everything around it.
		Material = bNeon ? Character->GetViewModelNeonMID() : Character->GetViewModelBodyMID();
	}

	const ATraceCharacter* OwningCharacter = GetTraceCharacter();
	if (Material == nullptr && OwningCharacter != nullptr && OwningCharacter->ViewModelRoot != nullptr)
	{
		TArray<USceneComponent*> Children;
		OwningCharacter->ViewModelRoot->GetChildrenComponents(/*bIncludeAllDescendants=*/false, Children);
		for (USceneComponent* Child : Children)
		{
			UStaticMeshComponent* GunPart = Cast<UStaticMeshComponent>(Child);
			if (GunPart == nullptr || KnifeViewParts.Contains(GunPart) || KnifeHandParts.Contains(GunPart))
			{
				continue;
			}
			// "Neon" in the name is how the CUBE gun's table marks its lit channels. Kept as a
			// second chance for a rig whose MIDs were never made (a simulated proxy builds no
			// viewmodel), not as the primary route.
			const bool bPartIsNeon = GunPart->GetName().Contains(TEXT("Neon"));
			if (bPartIsNeon == bNeon)
			{
				Material = GunPart->GetMaterial(0);
				break;
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

#if !UE_BUILD_SHIPPING
/**
 * Restores the EXACT pre-v12 SetGunViewModelHidden — the early-return latch that caused "the knife
 * and gun can be held at the same time".
 *
 * A TEST HOOK, defaulting to the fix being on, and it exists for the same reason
 * Trace.Knife.CarrierImmune does: a harness that has never gone RED is not evidence. With this at 1,
 * Trace.Knife.DualWeaponTest reproduces the user's bug on demand, on the shipped build, and prints
 * the exact census that proves it. Never ship 1.
 */
static TAutoConsoleVariable<int32> CVarKnifeLegacyGunHideLatch(
	TEXT("Trace.Knife.LegacyGunHideLatch"), 0,
	TEXT("Dev only. 1 restores the pre-v12 latched gun-hiding, i.e. PUTS THE DUAL-WEAPON BUG BACK, so "
	     "Trace.Knife.DualWeaponTest can be shown failing on a working build. Never ship 1."),
	ECVF_Cheat);
#endif

void UTraceWeaponComponent::RefreshGunPartCache(const ATraceCharacter& Character)
{
	// ViewModelRoot's direct children are the gun's parts, the hands/forearms, and KnifeViewRoot
	// (a USceneComponent, so the cast drops it). ATraceCharacter builds its rig ONCE, lazily, the
	// first frame the pawn turns out to be locally controlled — which is usually AFTER this component
	// has started ticking — so the cache is keyed on the child count and rebuilds itself when the rig
	// appears underneath it. Without that, an empty cache taken on frame one would be permanent and
	// the gun would never be hidden at all.
	const int32 ChildCount = Character.ViewModelRoot->GetNumChildrenComponents();
	if (ChildCount == CachedViewModelChildCount && CachedGunParts.Num() > 0)
	{
		return;
	}
	CachedViewModelChildCount = ChildCount;
	CachedGunParts.Reset();

	TArray<USceneComponent*> Children;
	Character.ViewModelRoot->GetChildrenComponents(/*bIncludeAllDescendants=*/false, Children);

	for (USceneComponent* Child : Children)
	{
		UStaticMeshComponent* Part = Cast<UStaticMeshComponent>(Child);
		if (Part == nullptr || KnifeViewParts.Contains(Part) || KnifeHandParts.Contains(Part))
		{
			continue;
		}
		if (IsViewModelHandPart(Part))
		{
			// The hands and forearms stay with either weapon. A knife held by nothing is a worse
			// read than no knife, so they are not in this list and are never written to.
			continue;
		}
		CachedGunParts.Add(Part);
	}
}

void UTraceWeaponComponent::SetGunViewModelHidden(bool bHidden)
{
	// ===============================================================================================
	// SPEC v12 §7 — "There's a bug where the knife and gun can be held at the same time."
	//
	// DIAGNOSED, REPRODUCED AND FIXED HERE. It was never a state bug: EquippedWeapon is a single
	// replicated selector and cannot hold both. It was this function, and it had two defects that
	// compounded.
	//
	// DEFECT 1 — THE LATCH. This used to open with `if (bGunViewModelHidden == bHidden) return;`,
	// which made it a one-shot edge trigger. But it is not the only writer of those components'
	// visibility: ATraceCharacter::SetViewModelVisible(true) sets EVERY part of ViewModelParts
	// visible, gun included, and it runs whenever the rig comes back — the carry blend returning to
	// first person, or a respawn. At that moment bGunViewModelHidden was still true, so this
	// function's next call early-returned and the gun stayed on screen NEXT TO THE KNIFE. That is
	// the user's bug, exactly, and the comment three lines up in UpdateKnifeVisuals claimed the code
	// was "re-asserting rather than latching" while this function did the opposite. Reproduction:
	// equip the knife, go third person and come back (Trace.ForceThirdPerson 1 then 0, which is the
	// same SetViewModelVisible path the Core carry uses), and both weapons are drawn. Measured, in
	// exactly that order, by Trace.Knife.DualWeaponTest.
	//
	// DEFECT 2 — RE-SHOWING A GUN THAT SHOULD BE GONE. The old body wrote SetVisibility(!bHidden)
	// unconditionally, so on the frame a knife-holding player DIED, UpdateKnifeVisuals' call with
	// bHidden=false put the gun back on screen over the death camera — after ATraceCharacter had just
	// hidden the whole rig for the corpse. Two owners, opposite intentions, last writer wins.
	//
	// THE RULE THAT REPLACES BOTH, and it is the only rule this function has:
	//
	//     a gun part is visible  <=>  the character wants the rig on screen  AND  no knife is out.
	//
	// The character's half is asked for, every time, rather than remembered — IsViewModelVisible() is
	// its own settled answer, folding in the carry blend, the corpse and the respawn. So this can
	// only ever REMOVE the gun from a rig the character is showing; it can never resurrect one the
	// character has hidden. Re-asserted every tick from UpdateKnifeVisuals, from a cached part list so
	// there is no per-frame allocation, and SetVisibility itself is a no-op when nothing changed.
	// ===============================================================================================

#if !UE_BUILD_SHIPPING
	// THE RED ARM. Byte for byte the behaviour described above as DEFECT 1 and DEFECT 2: latch on the
	// requested state, then write the raw !bHidden with no regard for what the character wants. See
	// CVarKnifeLegacyGunHideLatch.
	if (CVarKnifeLegacyGunHideLatch.GetValueOnAnyThread() != 0)
	{
		if (bGunViewModelHidden == bHidden)
		{
			return;
		}
		ATraceCharacter* LegacyCharacter = GetTraceCharacter();
		if (LegacyCharacter == nullptr || LegacyCharacter->ViewModelRoot == nullptr)
		{
			return;
		}
		bGunViewModelHidden = bHidden;
		RefreshGunPartCache(*LegacyCharacter);
		for (const TObjectPtr<UStaticMeshComponent>& Part : CachedGunParts)
		{
			if (Part != nullptr)
			{
				Part->SetVisibility(!bHidden);
			}
		}
		return;
	}
#endif

	ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr || Character->ViewModelRoot == nullptr)
	{
		return;
	}

	bGunViewModelHidden = bHidden;

	RefreshGunPartCache(*Character);

	const bool bWantGunParts = Character->IsViewModelVisible() && !bHidden;

	for (const TObjectPtr<UStaticMeshComponent>& Part : CachedGunParts)
	{
		if (Part != nullptr)
		{
			Part->SetVisibility(bWantGunParts);
		}
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
		//
		// UNCONDITIONAL, EVERY TICK, AND THAT IS THE FIX FOR SPEC v12 §7. ATraceCharacter is the
		// other writer of these components and it re-shows the whole rig on its own schedule (the
		// carry blend, a respawn); an edge-triggered call would miss that and leave a gun on screen
		// beside the knife. See SetGunViewModelHidden, which now states the rule instead of
		// remembering an edge.
		SetGunViewModelHidden(bKnife && bAlive);

		if (bWantView && KnifeViewRoot != nullptr)
		{
			// --- The stab, as a transform  (spec v12 §2) ----------------------------------------
			//
			// THIS IS THE SWINGER'S ENTIRE READ now that the 3D slash line has been taken out of
			// their view, and it is PROCEDURAL: there is no stab animation in the imported Mannequin
			// set (Death, Jump, Pistol, Rifle, Unarmed locomotion — nothing melee), so the motion is
			// this arithmetic and nothing else. See TraceKnifeLayout for the constants and for the
			// depth check that keeps the extended blade inside the capsule's clearance.
			//
			// THREE BEATS, and the middle one is the request:
			//   COCK    0 .. windup. Blade draws back and the tip lifts. Eased IN (T^2), so it
			//           accelerates into the thrust instead of arriving at the cocked pose and
			//           stopping. This is also the window the blade resolves in.
			//   THRUST  a quarter of what is left. Eased OUT, so it snaps to full extension and
			//           decelerates there — a punch, not a slide.
			//   RETURN  the remaining three quarters, smoothstepped home. FAST OUT, SLOWER RETURN is
			//           what §2 asked for, and 1:3 is the ratio that reads as a recovery rather than
			//           as a rewind.
			//
			// Written to KnifeViewRoot, a CHILD of ViewModelRoot, so it composes with the sway and
			// bob the character is writing to the parent rather than overwriting them. And nothing
			// in TraceMelee::ResolveSwing reads any of it — the arc that was actually cut is pure
			// arithmetic on the aim ray, so the blade may lag and overshoot as much as it likes. That
			// separation is why the animation could be replaced outright without touching one line of
			// hit resolution.
			const double Elapsed = GetLocalTimeSeconds() - SwingAnimStartLocalTime;
			const double AnimLength = static_cast<double>(TraceMelee::GetSwingAnimSeconds());

			FVector Offset = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;

			if (Elapsed >= 0.0 && Elapsed <= AnimLength)
			{
				// ONE derivation, shared with Trace.Knife.StabProfile — which walks this same curve at
				// 1 ms and reports whether the motion is actually a thrust. A test that re-derived the
				// curve would be measuring its own copy, which is how "the animation is a stab" becomes
				// a claim instead of a number.
				TraceKnifeLayout::ComputeStabPose(
					static_cast<float>(Elapsed / FMath::Max(0.01, AnimLength)),
					FMath::Clamp(TraceMelee::GetSwingWindupSeconds()
						/ FMath::Max(0.01f, TraceMelee::GetSwingAnimSeconds()), 0.05f, 0.6f),
					Offset, Rotation);
			}

			KnifeViewRoot->SetRelativeLocationAndRotation(Offset, Rotation);
		}
	}

	// --- Third person ---------------------------------------------------------------------------
	//
	// No animation here on purpose. The imported Mannequin set has no melee sequence (the same
	// absence that forced UpdateCrouchPresentation to pose the slide by hand), so the arm will not
	// swing — ATraceMeleeArc draws the cut instead, which is the information a victim actually needs.
	// What this rig carries is the STATE: a visible blade means that player is 22% faster and cannot
	// shoot, and that is worth reading across the arena.
	//
	// THERE IS NO THIRD-PERSON GUN MESH ANYWHERE IN THIS PROJECT, which is why spec v12 §7's third
	// suspect ("the third-person attachment showing both to other players") could be ruled out rather
	// than fixed: ATraceCharacter builds a first-person viewmodel and nothing else, so the only weapon
	// another player can ever see on your body is this knife. The dual-weapon bug was first-person
	// only, and Trace.Knife.DualWeaponTest reports both censuses so that stays checkable rather than
	// remembered.
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

void UTraceWeaponComponent::GetViewModelCensus(int32& OutGunVisible, int32& OutKnifeVisible,
	int32& OutHandVisible, int32& OutBodyKnife) const
{
	OutGunVisible = 0;
	OutKnifeVisible = 0;
	OutHandVisible = 0;
	OutBodyKnife = 0;

	const ATraceCharacter* Character = GetTraceCharacter();
	if (Character == nullptr)
	{
		return;
	}

	// The knife rigs first, from our own lists.
	for (const TObjectPtr<UStaticMeshComponent>& Part : KnifeViewParts)
	{
		if (Part != nullptr && Part->IsVisible())
		{
			++OutKnifeVisible;
		}
	}
	for (const TObjectPtr<UStaticMeshComponent>& Part : KnifeHandParts)
	{
		if (Part != nullptr && Part->IsVisible())
		{
			++OutBodyKnife;
		}
	}

	// The gun, walked live rather than through CachedGunParts. A census that trusted the same cache
	// the fix writes through could agree with a broken fix; this asks the rig itself.
	if (Character->ViewModelRoot == nullptr)
	{
		return;
	}
	TArray<USceneComponent*> Children;
	Character->ViewModelRoot->GetChildrenComponents(/*bIncludeAllDescendants=*/false, Children);
	for (USceneComponent* Child : Children)
	{
		const UStaticMeshComponent* Part = Cast<UStaticMeshComponent>(Child);
		if (Part == nullptr || KnifeViewParts.Contains(Part) || !Part->IsVisible())
		{
			continue;
		}
		if (IsViewModelHandPart(Part))
		{
			++OutHandVisible;
		}
		else
		{
			++OutGunVisible;
		}
	}
}

// =================================================================================================
// Trace.Knife.StabProfile — the unattended proof for spec v12 §2.
//
// Verbatim: "Take the 3d 'swipe' line animation of the knife out of the player's view. Can you make
// the knife animation a stab instead of a swipe?"
//
// "IT LOOKS LIKE A STAB NOW" IS NOT A MEASUREMENT, and this project has been burned by exactly that
// class of claim. A stab and a swipe are distinguishable as numbers, so this walks the shipped curve
// — TraceKnifeLayout::ComputeStabPose, the same function the viewmodel renders from, not a copy — at
// 1 ms over the whole animation and reports four things, each with a threshold:
//
//   FORWARD DOMINANCE   peak |+X| against peak |Y|. A thrust travels down the lens; a swipe travels
//                       across it. The old motion's peaks were 9.0 forward against 7.0 lateral, i.e.
//                       barely forward-biased at all, on top of a 58 degree yaw sweep. Requires the
//                       forward peak to be at least 3x the lateral one.
//   ROLL                must be exactly 0. Roll is what reads as a slash; the old motion rolled 46
//                       degrees through the strike.
//   YAW SWEEP           total yaw excursion. The old motion swept 34 -> -58, i.e. 92 degrees across
//                       the screen. Requires under 25.
//   OUT vs RETURN       time from the cocked pose to full extension, against the time from full
//                       extension back to rest. §2 asks for fast out and slower return, so the
//                       return must be strictly longer than the out-stroke.
//
// It also prints the extended blade's depth so the framing arithmetic in TraceKnifeLayout stays
// honest: the tip must stay well short of the 76 uu the gun's muzzle needed to clear the capsule.
//
// World-free and instant, so it runs anywhere — including in the same -ExecCmds list as the angle
// test, before a pawn exists.
// =================================================================================================

int32 TraceRunStabProfile()
{
	int32 Failures = 0;

	const float AnimSeconds = TraceMelee::GetSwingAnimSeconds();
	const float CockEnd = FMath::Clamp(
		TraceMelee::GetSwingWindupSeconds() / FMath::Max(0.01f, AnimSeconds), 0.05f, 0.6f);

	constexpr int32 Samples = 1000;                     // 1 ms at a 0.32 s animation, near enough
	float PeakForward = 0.f;
	float PeakBackward = 0.f;
	float PeakLateral = 0.f;
	float PeakRoll = 0.f;
	float MinYaw = 0.f;
	float MaxYaw = 0.f;
	float ForwardPeakAlpha = 0.f;

	for (int32 Index = 0; Index <= Samples; ++Index)
	{
		const float Alpha = static_cast<float>(Index) / static_cast<float>(Samples);

		FVector Offset = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		TraceKnifeLayout::ComputeStabPose(Alpha, CockEnd, Offset, Rotation);

		if (static_cast<float>(Offset.X) > PeakForward)
		{
			PeakForward = static_cast<float>(Offset.X);
			ForwardPeakAlpha = Alpha;
		}
		PeakBackward = FMath::Max(PeakBackward, -static_cast<float>(Offset.X));
		PeakLateral = FMath::Max(PeakLateral, FMath::Abs(static_cast<float>(Offset.Y)));
		PeakRoll = FMath::Max(PeakRoll, FMath::Abs(static_cast<float>(Rotation.Roll)));
		MinYaw = FMath::Min(MinYaw, static_cast<float>(Rotation.Yaw));
		MaxYaw = FMath::Max(MaxYaw, static_cast<float>(Rotation.Yaw));
	}

	// The two stroke durations, in seconds, from the curve's own structure.
	const float CockSeconds = CockEnd * AnimSeconds;
	const float OutSeconds = ForwardPeakAlpha * AnimSeconds - CockSeconds;
	const float ReturnSeconds = AnimSeconds - ForwardPeakAlpha * AnimSeconds;
	const float YawSweep = MaxYaw - MinYaw;

	UE_LOG(LogTraceGame, Display, TEXT("========== TRACE KNIFE STAB PROFILE (spec v12 s2) =========="));
	UE_LOG(LogTraceGame, Display,
		TEXT("[StabProfile] PROCEDURAL viewmodel motion — the Mannequin set ships no melee sequence, so "
		     "there is no authored stab to play and this curve IS the animation."));
	UE_LOG(LogTraceGame, Display,
		TEXT("[StabProfile] anim %.3fs = cock %.3fs + out %.3fs + return %.3fs (full extension at alpha %.3f)"),
		AnimSeconds, CockSeconds, OutSeconds, ReturnSeconds, ForwardPeakAlpha);
	UE_LOG(LogTraceGame, Display,
		TEXT("[StabProfile] travel: forward +%.2fuu, back -%.2fuu, lateral %.2fuu | roll %.2fdeg | yaw sweep %.2fdeg"),
		PeakForward, PeakBackward, PeakLateral, PeakRoll, YawSweep);

	auto Check = [&Failures](bool bCondition, const TCHAR* What, const FString& Detail)
	{
		if (bCondition)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[StabProfile] PASS  %-28s %s"), What, *Detail);
		}
		else
		{
			++Failures;
			UE_LOG(LogTraceGame, Error, TEXT("[StabProfile] FAIL  %-28s %s"), What, *Detail);
		}
	};

	Check(PeakForward >= 3.f * PeakLateral, TEXT("forward-dominant"),
		FString::Printf(TEXT("forward %.2fuu vs lateral %.2fuu (needs >= 3x — a swipe is lateral-dominant)"),
			PeakForward, PeakLateral));

	Check(FMath::IsNearlyZero(PeakRoll, 0.01f), TEXT("no roll"),
		FString::Printf(TEXT("peak roll %.3fdeg (a slash rolls; the motion this replaced rolled 46)"), PeakRoll));

	Check(YawSweep < 25.f, TEXT("no lateral sweep"),
		FString::Printf(TEXT("yaw excursion %.2fdeg (needs < 25; the motion this replaced swept 92)"), YawSweep));

	Check(ReturnSeconds > OutSeconds, TEXT("fast out, slower return"),
		FString::Printf(TEXT("out %.3fs vs return %.3fs (spec v12 s2 asks for a fast thrust and a slower recovery)"),
			OutSeconds, ReturnSeconds));

	// Depth. The blade tip's rest position is x = 21.4 + 3.6/2 = 23.2 uu in rig space.
	constexpr float BladeTipRestUU = 23.2f;
	constexpr float MuzzleClearanceUU = 76.f;
	const float ExtendedTip = BladeTipRestUU + PeakForward;
	Check(ExtendedTip < MuzzleClearanceUU, TEXT("blade stays inside clearance"),
		FString::Printf(TEXT("extended tip at %.1fuu vs the gun muzzle's %.0fuu, which is the depth that had to clear the capsule"),
			ExtendedTip, MuzzleClearanceUU));

	if (Failures == 0)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[StabProfile] 0 failures — the motion is a thrust, not a swipe."));
	}
	else
	{
		UE_LOG(LogTraceGame, Error, TEXT("[StabProfile] %d failure(s)."), Failures);
	}
	UE_LOG(LogTraceGame, Display, TEXT("============================================================"));

	return Failures;
}

static FAutoConsoleCommand GTraceStabProfileCmd(
	TEXT("Trace.Knife.StabProfile"),
	TEXT("Dev only. Spec v12 s2. Walks the shipped first-person knife motion at 1ms and reports whether "
	     "it is a forward thrust (fast out, slower return, no roll, no lateral sweep) rather than a swipe."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceRunStabProfile(); }));

// =================================================================================================
// Trace.Knife.DualWeaponTest — the unattended proof for spec v12 §7.
//
// Verbatim: "There's a bug where the knife and gun can be held at the same time."
//
// WHY A HARNESS AND NOT A SCREENSHOT. The two rigs are OnlyOwnerSee first-person primitives, so the
// only camera that can photograph them is the local player's, and a -RenderOffScreen run has no
// window to photograph. More importantly a screenshot cannot be made to go RED on demand — and the
// whole reason this file exists is that a fix nobody watched fail is not a fix. So this counts the
// components that are actually VISIBLE at each step, through UTraceWeaponComponent::
// GetViewModelCensus, which reads the components' own flags rather than either rig's opinion.
//
// TWO ARMS, RED FIRST, exactly as Trace.Knife.CarrierImmunityTest does it. Arm 0 restores the old
// latched SetGunViewModelHidden via Trace.Knife.LegacyGunHideLatch and MUST reproduce the bug; arm 1
// is the shipped code and must be clean. If red does not go red the verdict is NOT PROVEN, because a
// green arm on its own only demonstrates that the harness can print zero.
//
// THE REPRODUCTION, and it is the shipping path rather than a poke at the members:
//
//   1  swap to the knife and wait out the 0.2 s pullout   -> expect gun 0, knife >0
//   2  Trace.ForceThirdPerson 1, wait for the view blend  -> ATraceCharacter hides the whole rig,
//                                                            exactly as picking up the Core does
//   3  Trace.ForceThirdPerson 0, wait for the view blend  -> ATraceCharacter SHOWS the whole rig
//                                                            again, gun parts included
//   4  census                                             -> THE BUG: gun >0 AND knife >0
//   5  swap to the gun and back to the knife four times, half of them sampled INSIDE the 0.2 s
//      pullout — spec v12 §7's second suspect was a race in exactly that window
//
// Step 2/3 is not a contrivance: Trace.ForceThirdPerson feeds ATraceCharacter::WantsFirstPersonView,
// which is the SAME input the Core carry uses, and UpdateViewBlend's SetViewModelVisible call is the
// same line either way. Carrying the Core is simply the version of this that needs a Core, a pickup
// and a live match; the cvar is the version an unattended run can drive in four seconds.
//
// AS MEASURED — see the pass report for the full logs. On a listen server the RED arm reports the
// gun and the knife drawn together the instant the camera comes back to first person, and the GREEN
// arm reports zero across every census point. Both numbers are logged at Display on purpose.
// =================================================================================================

namespace TraceDualWeaponTest
{
	/**
	 * The pawn the local player is looking out of.
	 *
	 * Its own copy rather than TraceRecoilTest's, purely because that namespace is defined further
	 * down this file and this harness is not worth a forward declaration for.
	 */
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
		enum class EPhase : uint8
		{
			ToKnife,        // swap, and wait out the pullout
			ThirdPerson,    // force the camera out, wait for the blend
			BackToFirst,    // release it, wait for the blend — this is where the bug lands
			SwapCycle,      // repeated swaps, including one caught mid-pullout
			Done
		};

		EPhase Phase = EPhase::ToKnife;
		double PhaseStart = 0.0;
		int32 CycleStep = 0;

		int32 Failures = 0;
		bool bAborted = false;
		FString AbortReason;

		/** Worst simultaneous count seen anywhere in the run. Non-zero is the bug, by definition. */
		int32 WorstBothVisible = 0;

		/**
		 * 0 = RED (the pre-v12 latch restored, MUST reproduce the bug), 1 = GREEN (shipped).
		 *
		 * Both arms run, red first, and the verdict is NOT PROVEN unless red went red. That is the
		 * project rule about harnesses that have never failed, applied to the one bug in this pass
		 * whose only symptom is a pixel.
		 */
		int32 Arm = 0;

		int32 RedFailures = -1;
		int32 RedWorst = 0;
	};

	/** One census line. Returns true when BOTH weapons were on screen at once. */
	bool Sample(const TSharedRef<FState>& State, const UTraceWeaponComponent* Weapon,
		const ATraceCharacter* Character, const TCHAR* What)
	{
		if (Weapon == nullptr || Character == nullptr)
		{
			return false;
		}

		int32 Gun = 0;
		int32 Knife = 0;
		int32 Hands = 0;
		int32 BodyKnife = 0;
		Weapon->GetViewModelCensus(Gun, Knife, Hands, BodyKnife);

		const bool bBoth = (Gun > 0 && Knife > 0);
		if (bBoth)
		{
			State->WorstBothVisible = FMath::Max(State->WorstBothVisible, FMath::Min(Gun, Knife));
			++State->Failures;
		}

		const FString Line = FString::Printf(
			TEXT("[DualWeapon] %s  %-46s equipped=%-5s deploying=%d rigVisible=%d | 1P gun=%d knife=%d hands=%d | 3P knife=%d"),
			bBoth ? TEXT("*** BOTH ***") : TEXT("ok          "),
			What, LexToString(Weapon->GetEquippedWeapon()), Weapon->IsDeploying() ? 1 : 0,
			Character->IsViewModelVisible() ? 1 : 0, Gun, Knife, Hands, BodyKnife);

		if (bBoth)
		{
			UE_LOG(LogTraceGame, Error, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogTraceGame, Display, TEXT("%s"), *Line);
		}
		return bBoth;
	}

	/** Puts the world back the way it was found, on every exit path. */
	void RestoreWorld()
	{
		if (IConsoleVariable* ForceThird = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.ForceThirdPerson")))
		{
			ForceThird->Set(0, ECVF_SetByConsole);
		}
		if (IConsoleVariable* Legacy = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Knife.LegacyGunHideLatch")))
		{
			Legacy->Set(0, ECVF_SetByConsole);   // never leave the bug switched on
		}
	}

	void Run(float DelaySeconds, int32 Arm, int32 RedFailures, int32 RedWorst);

	/** Ends one arm, and either chains into the next or prints the combined verdict. */
	void Report(const TSharedRef<FState>& State)
	{
		const bool bRed = (State->Arm == 0);

		UE_LOG(LogTraceGame, Display, TEXT("---------- arm %d (%s): %d census points with BOTH weapons drawn ----------"),
			State->Arm, bRed ? TEXT("RED — the pre-v12 latch restored") : TEXT("GREEN — shipped"), State->Failures);

		if (State->bAborted)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[DualWeapon] arm %d ABORTED: %s"), State->Arm, *State->AbortReason);
			RestoreWorld();
			UE_LOG(LogTraceGame, Warning,
				TEXT("[DualWeapon] RESULT: *** NOT PROVEN *** — the arm did not complete, so neither number means anything."));
			return;
		}

		if (bRed)
		{
			// Chain straight into the green arm, carrying the red numbers so one verdict line can
			// compare them. The latch goes off here and RestoreWorld puts it off again at the end.
			RestoreWorld();
			Run(1.0f, /*Arm=*/1, State->Failures, State->WorstBothVisible);
			return;
		}

		RestoreWorld();

		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE DUAL-WEAPON TEST (spec v12 s7) =========="));
		UE_LOG(LogTraceGame, Display,
			TEXT("[DualWeapon] RED   arm (bug restored) : %d census points with both weapons drawn, worst overlap %d parts."),
			State->RedFailures, State->RedWorst);
		UE_LOG(LogTraceGame, Display,
			TEXT("[DualWeapon] GREEN arm (shipped code) : %d census points with both weapons drawn, worst overlap %d parts."),
			State->Failures, State->WorstBothVisible);

		const bool bRedReproduced = (State->RedFailures > 0);
		const bool bGreenClean = (State->Failures == 0);

		if (bRedReproduced && bGreenClean)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[DualWeapon] RESULT: FIXED, PROVEN — the bug reproduces with the old latch and is gone without it."));
		}
		else if (!bRedReproduced)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[DualWeapon] RESULT: *** NOT PROVEN *** — the RED arm did not reproduce the bug, so the GREEN "
				     "arm's zero proves nothing. The reproduction, not the fix, is what needs looking at."));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[DualWeapon] RESULT: *** FAIL *** — the gun and the knife are still drawn together on the "
				     "shipped path (%d census points). This is spec v12 s7, unfixed."),
				State->Failures);
		}
		UE_LOG(LogTraceGame, Display, TEXT("=========================================================="));
	}

	void Run(float DelaySeconds, int32 Arm, int32 RedFailures, int32 RedWorst)
	{
		TSharedRef<FState> State = MakeShared<FState>();
		State->Arm = Arm;
		State->RedFailures = RedFailures;
		State->RedWorst = RedWorst;

		// Arm 0 puts the bug back. Every exit path below clears it again — including the aborts.
		if (IConsoleVariable* Legacy = IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Knife.LegacyGunHideLatch")))
		{
			Legacy->Set(Arm == 0 ? 1 : 0, ECVF_SetByConsole);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[DualWeapon] arm %d (%s) in %.1fs: equip the knife, force third person and back (the "
			     "carry-blend path), then swap repeatedly — counting VISIBLE viewmodel parts at every step."),
			Arm, (Arm == 0) ? TEXT("RED, bug restored, MUST FAIL") : TEXT("GREEN, shipped"), DelaySeconds);

		// Two tickers, for the reason spelled out in Trace.TestRecoil: FTSTicker's delay applies to
		// every invocation, not only the first.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State](float /*Delta*/) -> bool
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
					[State](float /*DeltaTime*/) -> bool
					{
						ATraceCharacter* Character = FindLocalTraceCharacter();
						UTraceWeaponComponent* Weapon = (Character != nullptr)
							? Character->FindComponentByClass<UTraceWeaponComponent>() : nullptr;
						if (Weapon == nullptr || Character == nullptr)
						{
							return true;   // no pawn yet — wait rather than reporting zeroes
						}

						const UWorld* TestWorld = Character->GetWorld();
						const double Now = (TestWorld != nullptr) ? TestWorld->GetTimeSeconds() : 0.0;
						if (State->PhaseStart <= 0.0)
						{
							State->PhaseStart = Now;

							if (Character->IsCarrier())
							{
								State->bAborted = true;
								State->AbortReason = TEXT("the local player is carrying the Core; carriers cannot swap by design.");
								Report(State);
								return false;
							}

							ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
							if (!Weapon->RequestEquip(ETraceEquippedWeapon::Knife, &Refusal))
							{
								State->bAborted = true;
								State->AbortReason = FString::Printf(
									TEXT("the swap to the knife was refused: %s"), LexToString(Refusal));
								Report(State);
								return false;
							}
							Sample(State, Weapon, Character, TEXT("t0: knife requested, mid-pullout"));
							return true;
						}

						const double InPhase = Now - State->PhaseStart;

						switch (State->Phase)
						{
						case FState::EPhase::ToKnife:
							// The pullout, plus a beat for the visuals to settle.
							if (InPhase < static_cast<double>(TraceMelee::GetSwapSeconds()) + 0.35)
							{
								return true;
							}
							Sample(State, Weapon, Character, TEXT("knife deployed, first person"));

							// The carry-blend path, without needing a Core. See the block comment.
							{
								static IConsoleVariable* ForceThird =
									IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.ForceThirdPerson"));
								if (ForceThird == nullptr)
								{
									State->bAborted = true;
									State->AbortReason = TEXT("Trace.ForceThirdPerson is missing; the rig hide/show path cannot be driven.");
									Report(State);
									return false;
								}
								ForceThird->Set(1, ECVF_SetByConsole);
							}
							State->Phase = FState::EPhase::ThirdPerson;
							State->PhaseStart = Now;
							return true;

						case FState::EPhase::ThirdPerson:
							// ViewBlendSeconds is ATraceCharacter's, and private; 1.2 s is comfortably
							// past it for any sane value and the census below proves the rig actually
							// went away rather than assuming it.
							if (InPhase < 1.2)
							{
								return true;
							}
							Sample(State, Weapon, Character, TEXT("third person: whole rig should be hidden"));
							{
								static IConsoleVariable* ForceThird =
									IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.ForceThirdPerson"));
								if (ForceThird != nullptr)
								{
									ForceThird->Set(0, ECVF_SetByConsole);
								}
							}
							State->Phase = FState::EPhase::BackToFirst;
							State->PhaseStart = Now;
							return true;

						case FState::EPhase::BackToFirst:
							if (InPhase < 1.2)
							{
								return true;
							}
							// *** THE BUG'S MOMENT. ATraceCharacter has just re-shown every part of
							// its rig, gun included, while the knife is still the equipped weapon.
							Sample(State, Weapon, Character,
								TEXT("BACK TO FIRST PERSON — the reported bug lands here"));
							State->Phase = FState::EPhase::SwapCycle;
							State->PhaseStart = Now;
							State->CycleStep = 0;
							return true;

						case FState::EPhase::SwapCycle:
						{
							// Four swaps. The odd steps are sampled at 0.10 s — INSIDE the 0.2 s
							// pullout — because spec v12 §7's second suspect was a race between the
							// replicated selector and the local prediction during exactly that window.
							const double StepLength = (State->CycleStep % 2 == 0) ? 0.10 : 0.45;
							if (InPhase < StepLength)
							{
								return true;
							}

							const bool bMidPullout = (State->CycleStep % 2 == 0);
							Sample(State, Weapon, Character, bMidPullout
								? TEXT("swap cycle: sampled INSIDE the 0.2s pullout")
								: TEXT("swap cycle: sampled after the pullout"));

							if (++State->CycleStep >= 8)
							{
								State->Phase = FState::EPhase::Done;
								Report(State);
								return false;
							}

							ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
							Weapon->RequestEquip(Weapon->IsKnifeEquipped()
								? ETraceEquippedWeapon::Gun : ETraceEquippedWeapon::Knife, &Refusal);
							State->PhaseStart = Now;
							return true;
						}

						case FState::EPhase::Done:
						default:
							return false;
						}
					}), 0.f);
				return false;
			}), FMath::Max(0.f, DelaySeconds));
	}
}

static FAutoConsoleCommand GTraceDualWeaponTestCmd(
	TEXT("Trace.Knife.DualWeaponTest"),
	TEXT("Dev only. Spec v12 s7. Equips the knife, drives the rig through the carry-blend hide/show "
	     "and four swaps (two sampled mid-pullout), and counts the viewmodel parts actually VISIBLE at "
	     "each step. Any census with a gun part and a knife part both drawn is the bug."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const float Delay = (Args.Num() > 0) ? FMath::Max(0.f, FCString::Atof(*Args[0])) : 8.f;
		TraceDualWeaponTest::Run(Delay, /*Arm=*/0, /*RedFailures=*/-1, /*RedWorst=*/0);
	}));

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

// =================================================================================================
// AMMO'S SELF-TESTS  (spec v16 §1)
//
//   Trace.Ammo.Dump         what this process actually resolved, plus the live state of the local
//                           pawn's clip. The .ini can win over the header, so this is the only
//                           honest way to read the numbers.
//
//   Trace.Ammo.Test         the mechanic. RED ARM FIRST (Trace.Ammo.Enabled 0): the clip must refuse
//                           to fall, or the green arm's arithmetic is a measurement of nothing.
//                           Then: one round per shot THROUGH THE REAL TRIGGER, the automatic reload
//                           at zero, fire refused for the whole 0.5 s (with Trace.Ammo.ReloadBlocksFire
//                           0 as its own A/B), the refill to a full clip, and the manual R rules.
//
//   Trace.Ammo.CarrierTest  *** "The Core carrier has no gun, so ammo must not be consumed or shown
//                           while carrying." *** Both locks, red-armed, plus a live non-carrier
//                           CONTROL — a clip that cannot be moved at all would otherwise report the
//                           carrier rule holding when nothing had been tested.
//
//   Trace.Ammo.BotWatch     "Bots must respect it and reload; a bot that dry-fires forever will read
//                           as broken." Watches every bot's real clip for N seconds and reports what
//                           it saw. INVALID rather than PASS when the bots did not fire enough to
//                           make the question meaningful.
//
// TWO WAYS OF DRIVING A SHOT, AND EACH IS USED WHERE IT IS HONEST.
//   * THE REAL TRIGGER (StartFire, and the component's own tick) is used to prove the shipping path
//     spends exactly one round per shot. It is the only thing that proves the input path is wired.
//   * THE CONSUMPTION FUNCTION (DebugConsumeRound) is used for the parts a real trigger cannot
//     reach in reasonable time or at all: emptying a 30-round clip would take 12 s of live fire in a
//     match where a bot may kill the subject halfway through, and the carrier's ConsumeRound guard
//     is by construction unreachable through the trigger, because CanFire() refuses a carrier first.
//     Both drive the SAME function a shot drives; neither invents a number.
// =================================================================================================

namespace TraceAmmoTest
{
	IConsoleVariable* FindArm(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}

	void SetArm(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = FindArm(Name))
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	/** Every arm these commands touch, back to shipped. Called on EVERY exit path, aborts included. */
	void RestoreArms()
	{
		SetArm(TEXT("Trace.Ammo.Enabled"), 1);
		SetArm(TEXT("Trace.Ammo.ReloadBlocksFire"), 1);
		SetArm(TEXT("Trace.Ammo.CarrierGuard"), 1);
	}

	/** The project's usual checklist, so a FAIL is impossible to skim past in a headless log. */
	struct FChecklist
	{
		const TCHAR* Tag = TEXT("AMMO");
		int32 Passed = 0;
		int32 Failed = 0;
		bool  bInvalid = false;
		FString InvalidReason;

		void Check(bool bCondition, const FString& Name, const FString& Detail)
		{
			if (bCondition) { ++Passed; } else { ++Failed; }
			UE_LOG(LogTraceGame, Display, TEXT("[%s] %s  %s  |  %s"),
				Tag, bCondition ? TEXT("PASS") : TEXT("*** FAIL ***"), *Name, *Detail);
		}

		void Invalidate(const FString& Reason)
		{
			bInvalid = true;
			InvalidReason = Reason;
		}

		void Report()
		{
			if (bInvalid)
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: INVALID — %s (%d passed, %d failed)"),
					Tag, *InvalidReason, Passed, Failed);
			}
			else if (Failed == 0)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[%s] VERDICT: PASS — %d checks, 0 failed."), Tag, Passed);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[%s] VERDICT: *** FAIL *** — %d passed, %d FAILED."),
					Tag, Passed, Failed);
			}
		}
	};

	UWorld* FindAuthoritativeWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetAuthGameMode() != nullptr)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/**
	 * A pawn this harness may drive: alive, holding the gun, not carrying the Core, LOCALLY
	 * CONTROLLED (StartFire refuses anything else — input is a local concept) and authoritative.
	 *
	 * The local human first, then any bot. A human is the better subject in a headless run because
	 * nothing else is steering them: a bot's own TickBotKnife would draw the blade the moment an
	 * enemy came within BotEngageRangeUU and quietly turn "the clip did not fall" into a true
	 * statement about a pawn holding a knife.
	 */
	ATraceCharacter* FindDrivableSubject(UWorld* World)
	{
		auto IsUsable = [](const ATraceCharacter* Candidate) -> bool
		{
			return Candidate != nullptr && Candidate->IsAlive() && !Candidate->IsCarrier()
				&& Candidate->Weapon != nullptr && !Candidate->Weapon->IsKnifeEquipped()
				&& Candidate->IsLocallyControlled() && Candidate->HasAuthority();
		};

		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (ATraceCharacter* HumanPawn = Cast<ATraceCharacter>(PC->GetPawn()); IsUsable(HumanPawn))
			{
				return HumanPawn;
			}
		}

		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			if (ATraceCharacter* Candidate = *It; IsUsable(Candidate))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** True when @p Character is driven by AI rather than by a human. Used by Trace.Ammo.BotWatch. */
	bool IsBotControlled(const ATraceCharacter* Character)
	{
		const AController* Controller = (Character != nullptr) ? Character->GetController() : nullptr;
		return Controller != nullptr && Cast<const APlayerController>(Controller) == nullptr;
	}

	// =============================================================================================
	// Trace.Ammo.Dump
	// =============================================================================================

	void RunDump(UWorld* World)
	{
		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE AMMO (spec v16 §1) =========="));
		UE_LOG(LogTraceGame, Display, TEXT("CLIP    size       : %d rounds (UTraceSettings::ClipSize)"),
			TraceAmmo::GetClipSize());
		UE_LOG(LogTraceGame, Display, TEXT("RELOAD  time       : %.3fs (UTraceSettings::ReloadSeconds)"),
			TraceAmmo::GetReloadSeconds());
		UE_LOG(LogTraceGame, Display,
			TEXT("CYCLE   sustained  : %.2fs of fire per clip at FireInterval %.2fs, then %.2fs reloading"),
			TraceAmmo::GetClipSize() * FMath::Max(0.01f, UTraceSettings::Get().FireInterval),
			FMath::Max(0.01f, UTraceSettings::Get().FireInterval), TraceAmmo::GetReloadSeconds());
		UE_LOG(LogTraceGame, Display,
			TEXT("ARMS    enabled=%d reloadBlocksFire=%d carrierGuard=%d predict=%d (all 1 = shipped)"),
			TraceAmmo::IsEnabled() ? 1 : 0, TraceAmmo::DoesReloadBlockFire() ? 1 : 0,
			TraceAmmo::IsCarrierGuardArmed() ? 1 : 0, TraceAmmo::IsPredictionEnabled() ? 1 : 0);
		UE_LOG(LogTraceGame, Display,
			TEXT("ALARM   carrierRoundsConsumed=%d (MUST be 0) | liveness: rounds=%d reloads=%d "
			     "refusedEmpty=%d refusedReloading=%d"),
			TraceAmmo::GetCarrierRoundsConsumed(), TraceAmmo::GetRoundsConsumed(),
			TraceAmmo::GetReloadsCompleted(), TraceAmmo::GetDryFireRefusals(),
			TraceAmmo::GetReloadFireRefusals());

		if (World != nullptr)
		{
			UE_LOG(LogTraceGame, Display, TEXT("NET     mode       : %d (0 standalone, 1 dedicated, 2 listen, 3 client)"),
				static_cast<int32>(World->GetNetMode()));

			if (const APlayerController* PC = World->GetFirstPlayerController())
			{
				if (const ATraceCharacter* Pawn = Cast<ATraceCharacter>(PC->GetPawn());
					Pawn != nullptr && Pawn->Weapon != nullptr)
				{
					const UTraceWeaponComponent* Weapon = Pawn->Weapon;
					UE_LOG(LogTraceGame, Display,
						TEXT("LOCAL   %s: clip %d/%d (%d ability round(s)) | reloading=%d (%.3fs left) | "
						     "shown=%d canFire=%d"),
						*GetNameSafe(Pawn), Weapon->GetClipAmmo(), Weapon->GetClipSize(),
						Weapon->GetAbilityRoundsInClip(), Weapon->IsReloading() ? 1 : 0,
						Weapon->GetReloadRemaining(), Weapon->ShouldShowAmmo() ? 1 : 0,
						Weapon->CanFire() ? 1 : 0);
				}
			}
		}
		UE_LOG(LogTraceGame, Display, TEXT("============================================="));
	}

	// =============================================================================================
	// Trace.Ammo.Test
	// =============================================================================================

	struct FAmmoTestState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;
		TWeakObjectPtr<ATraceCharacter> Subject;

		/** Set by the red arm. Without it the green arm's numbers are uninformative. */
		bool bRedReproduced = false;

		/** Live-fire bookkeeping (step 2). */
		int32 LiveFireClipBefore = 0;
		int32 LiveFireConsumedBefore = 0;
		double LiveFireStartRealTime = 0.0;

		/** Snapshots the later steps compare against. */
		int32 ClipBeforeDrain = 0;
		int32 DrainCalls = 0;
		double ReloadObservedAtRealTime = 0.0;
	};

	void RunAmmoTest()
	{
		UWorld* World = FindAuthoritativeWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[AMMO] no authoritative game world — the clip is server state, so this must run on the server."));
			return;
		}

		TSharedPtr<FAmmoTestState> State = MakeShared<FAmmoTestState>();
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[AMMO] ===== spec v16 §1: '30 bullets per clip, then the gun reloads. R to reload.' "
			     "'Reloading takes .5seconds'. Resolved: %d rounds, %.2fs reload. arm 0 = RED "
			     "(Trace.Ammo.Enabled 0) must leave the clip untouched. ====="),
			TraceAmmo::GetClipSize(), TraceAmmo::GetReloadSeconds());

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreArms();
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			// ---- staging -------------------------------------------------------------------
			if (State->Step == 0)
			{
				ATraceCharacter* Subject = FindDrivableSubject(TickWorld);
				if (Subject == nullptr)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(TEXT("no drivable subject: this needs a live, locally controlled, "
						                            "authoritative pawn holding the gun and not carrying the Core"));
						State->List.Report();
						RestoreArms();
						return false;
					}
					return true;
				}

				State->Subject = Subject;
				UE_LOG(LogTraceGame, Display, TEXT("[AMMO] staged on %s (clip %d/%d)."),
					*GetNameSafe(Subject), Subject->Weapon->GetClipAmmo(), TraceAmmo::GetClipSize());

				// The resolved knobs are checked once, here, because every later assertion is stated
				// in terms of them and a wrong ClipSize would make the rest pass against the wrong game.
				State->List.Check(TraceAmmo::GetClipSize() == 30,
					TEXT("'30 bullets per clip'"),
					FString::Printf(TEXT("resolved %d"), TraceAmmo::GetClipSize()));
				State->List.Check(FMath::IsNearlyEqual(TraceAmmo::GetReloadSeconds(), 0.5f, 0.001f),
					TEXT("'Reloading takes .5seconds'"),
					FString::Printf(TEXT("resolved %.3fs"), TraceAmmo::GetReloadSeconds()));

				State->Step = 1;
				return true;
			}

			// *** THE SUBJECT IS RE-VALIDATED ON EVERY TICK, NOT JUST AT STAGING. *** A pawn that died,
			// picked up the Core or drew the knife between two steps would turn the rest of this run
			// into a set of true statements about the wrong situation — the exact shape of the carrier
			// harness that once printed PASS while striking a corpse.
			ATraceCharacter* Subject = State->Subject.Get();
			UTraceWeaponComponent* Weapon = (Subject != nullptr) ? Subject->Weapon : nullptr;
			if (Subject == nullptr || Weapon == nullptr || !Subject->IsAlive()
				|| Subject->IsCarrier() || Weapon->IsKnifeEquipped())
			{
				State->List.Invalidate(FString::Printf(
					TEXT("the subject stopped being testable mid-run (alive=%d carrying=%d knife=%d) — rerun in a "
					     "quieter match"),
					(Subject != nullptr && Subject->IsAlive()) ? 1 : 0,
					(Subject != nullptr && Subject->IsCarrier()) ? 1 : 0,
					(Weapon != nullptr && Weapon->IsKnifeEquipped()) ? 1 : 0));
				State->List.Report();
				RestoreArms();
				return false;
			}

			// ---- step 1: THE RED ARM ---------------------------------------------------------
			if (State->Step == 1)
			{
				SetArm(TEXT("Trace.Ammo.Enabled"), 0);

				const int32 Before = Weapon->GetClipAmmo();
				for (int32 Index = 0; Index < 10; ++Index)
				{
					Weapon->DebugConsumeRound();
				}
				const int32 After = Weapon->GetClipAmmo();

				State->bRedReproduced = (After == Before);
				State->List.Check(State->bRedReproduced,
					TEXT("RED ARM: with the mechanic disarmed, ten rounds cost nothing"),
					FString::Printf(TEXT("clip %d -> %d — this is the build that existed before §1"), Before, After));

				SetArm(TEXT("Trace.Ammo.Enabled"), 1);

				// LIVE FIRE, through the real trigger, starts now and is measured next step.
				State->LiveFireClipBefore = Weapon->GetClipAmmo();
				int32 ReloadsBefore = 0;
				Weapon->DebugGetAmmoTotals(State->LiveFireConsumedBefore, ReloadsBefore);
				State->LiveFireStartRealTime = NowReal;
				Weapon->StartFire();

				State->Step = 2;
				// Long enough for three shots at the shipped 0.40 s interval, short enough that the
				// subject is unlikely to be killed in the middle of it.
				State->NextStepRealTime = NowReal + 1.4;
				return true;
			}

			// ---- step 2: ONE ROUND PER SHOT, THROUGH THE SHIPPING INPUT PATH -----------------
			if (State->Step == 2)
			{
				Weapon->StopFire();

				int32 ConsumedNow = 0;
				int32 ReloadsNow = 0;
				Weapon->DebugGetAmmoTotals(ConsumedNow, ReloadsNow);

				const int32 RoundsSpent = ConsumedNow - State->LiveFireConsumedBefore;
				const int32 ClipDrop = State->LiveFireClipBefore - Weapon->GetClipAmmo();

				// *** THE EXPECTED SHOT COUNT IS DERIVED FROM THE CLOCK, NOT FROM ANY AMMO COUNTER. ***
				//
				// This was a vacuous check once and the fix's own sabotage run is what caught it: with
				// ConsumeRound() removed from ServerFire, BOTH the clip and the component's round
				// counter stayed still, "0 == 0" was true, and "exactly one round per shot" printed
				// PASS on a build where no shot cost anything. Two numbers written by the same function
				// cannot disagree, so they cannot witness each other.
				//
				// Elapsed wall time over FireInterval is an independent witness: the trigger was held
				// for a measured duration and the gun's cadence is a published knob. +/-1 covers the
				// ticker's granularity at each end of the burst.
				const double FireInterval = FMath::Max(0.01, static_cast<double>(UTraceSettings::Get().FireInterval));
				const double HeldSeconds = NowReal - State->LiveFireStartRealTime;
				const int32 ExpectedShots = FMath::FloorToInt(HeldSeconds / FireInterval);
				const bool bPlausibleCount = (ClipDrop >= ExpectedShots - 1) && (ClipDrop <= ExpectedShots + 1);

				State->List.Check(ClipDrop > 0 && bPlausibleCount,
					TEXT("the REAL trigger actually spends rounds: StartFire -> FireOnce -> ServerFire -> the clip"),
					FString::Printf(TEXT("clip fell by %d in %.2fs at FireInterval %.2f, so ~%d shot(s) were "
					                     "expected (+/-1)"),
						ClipDrop, HeldSeconds, FireInterval, ExpectedShots));

				State->List.Check(ClipDrop > 0 && RoundsSpent == ClipDrop,
					TEXT("exactly ONE round leaves the clip per shot"),
					FString::Printf(TEXT("clip fell by %d for %d recorded round(s) — %d/%d left. The `> 0` is "
					                     "load-bearing: without it a gun that consumed nothing would report both "
					                     "numbers as 0 and PASS"),
						ClipDrop, RoundsSpent, Weapon->GetClipAmmo(), TraceAmmo::GetClipSize()));

				// ---- drain the rest through the same consumption function a shot uses ----
				State->ClipBeforeDrain = Weapon->GetClipAmmo();
				State->DrainCalls = 0;
				while (Weapon->GetClipAmmo() > 0 && State->DrainCalls <= 1000)
				{
					Weapon->DebugConsumeRound();
					++State->DrainCalls;
				}

				State->List.Check(State->DrainCalls == State->ClipBeforeDrain,
					TEXT("the clip holds exactly the rounds it says it holds"),
					FString::Printf(TEXT("%d consumption(s) took %d rounds to zero"),
						State->DrainCalls, State->ClipBeforeDrain));

				State->Step = 3;
				State->NextStepRealTime = NowReal + 0.1;   // let one TickReload run
				return true;
			}

			// ---- step 3: "then the gun reloads" — automatic, and it refuses fire -------------
			if (State->Step == 3)
			{
				const bool bReloading = Weapon->IsReloading();
				const float Remaining = Weapon->GetReloadRemaining();
				const bool bRefused = !Weapon->CanFire();

				State->List.Check(bReloading,
					TEXT("'30 bullets per clip, THEN THE GUN RELOADS' — with no key pressed"),
					FString::Printf(TEXT("reloading=%d with %.3fs left of %.2fs"),
						bReloading ? 1 : 0, Remaining, TraceAmmo::GetReloadSeconds()));

				State->List.Check(bRefused,
					TEXT("firing is refused while the reload runs"),
					FString::Printf(TEXT("CanFire()=%d, empty-clip refusals %d, mid-reload refusals %d"),
						Weapon->CanFire() ? 1 : 0, TraceAmmo::GetDryFireRefusals(),
						TraceAmmo::GetReloadFireRefusals()));

				State->ReloadObservedAtRealTime = NowReal;
				State->Step = 4;
				State->NextStepRealTime = NowReal + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.25;
				return true;
			}

			// ---- step 4: the refill, then the MANUAL reload rules ----------------------------
			if (State->Step == 4)
			{
				State->List.Check(Weapon->GetClipAmmo() == TraceAmmo::GetClipSize() && !Weapon->IsReloading(),
					TEXT("the reload finishes and the clip is full again"),
					FString::Printf(TEXT("clip %d/%d, reloading=%d, ~%.2fs after it started"),
						Weapon->GetClipAmmo(), TraceAmmo::GetClipSize(), Weapon->IsReloading() ? 1 : 0,
						NowReal - State->ReloadObservedAtRealTime));

				// R WITH A FULL CLIP DOES NOTHING. [ASSUMPTION], spec v16 §1.
				const bool bFullReload = Weapon->RequestReload();
				State->List.Check(!bFullReload && !Weapon->IsReloading(),
					TEXT("R with a FULL clip does nothing at all"),
					FString::Printf(TEXT("RequestReload()=%d, reloading=%d — it must not restart the timer"),
						bFullReload ? 1 : 0, Weapon->IsReloading() ? 1 : 0));

				// A partial clip, then R.
				for (int32 Index = 0; Index < 5; ++Index)
				{
					Weapon->DebugConsumeRound();
				}
				const int32 PartialClip = Weapon->GetClipAmmo();

				// THE CONTROL FOR THE NEXT CHECK: the gun must be able to fire RIGHT NOW, or "refused
				// during a reload" would be true for some entirely unrelated reason.
				const bool bCanFireBefore = Weapon->CanFire();

				const bool bManualReload = Weapon->RequestReload();
				State->List.Check(bManualReload && Weapon->IsReloading(),
					TEXT("R with a partial clip starts a reload"),
					FString::Printf(TEXT("clip was %d/%d, RequestReload()=%d, %.3fs left"),
						PartialClip, TraceAmmo::GetClipSize(), bManualReload ? 1 : 0,
						Weapon->GetReloadRemaining()));

				// ---- THE RELOAD GATE'S OWN A/B, on a NON-empty clip so the empty-clip refusal
				//      cannot be what is being measured. One arm, nothing else changed. ----
				const bool bRefusedArmed = !Weapon->CanFire();
				SetArm(TEXT("Trace.Ammo.ReloadBlocksFire"), 0);
				const bool bAllowedDisarmed = Weapon->CanFire();
				SetArm(TEXT("Trace.Ammo.ReloadBlocksFire"), 1);

				State->List.Check(bCanFireBefore && bRefusedArmed && bAllowedDisarmed,
					TEXT("the mid-reload refusal is the RELOAD's doing and nothing else's"),
					FString::Printf(TEXT("canFire before the reload=%d | during, armed=%d | during, "
					                     "Trace.Ammo.ReloadBlocksFire 0=%d (want 1 / 0 / 1)"),
						bCanFireBefore ? 1 : 0, bRefusedArmed ? 0 : 1, bAllowedDisarmed ? 1 : 0));

				State->Step = 5;
				State->NextStepRealTime = NowReal + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.25;
				return true;
			}

			// ---- step 5: the manual reload also refills, and the verdict ---------------------
			State->List.Check(Weapon->GetClipAmmo() == TraceAmmo::GetClipSize() && !Weapon->IsReloading(),
				TEXT("the manual reload refills the clip too"),
				FString::Printf(TEXT("clip %d/%d, reloading=%d"),
					Weapon->GetClipAmmo(), TraceAmmo::GetClipSize(), Weapon->IsReloading() ? 1 : 0));

			State->List.Check(TraceAmmo::GetCarrierRoundsConsumed() == 0,
				TEXT("no round was ever taken from a Core carrier's clip during this run"),
				FString::Printf(TEXT("alarm=%d (Trace.Ammo.CarrierTest is what red-arms this properly)"),
					TraceAmmo::GetCarrierRoundsConsumed()));

			if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — Trace.Ammo.Enabled 0 failed to stop "
				                            "the clip falling, so nothing above is a measurement of the ammo code"));
			}

			State->List.Report();
			RestoreArms();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Ammo.CarrierTest
	// =============================================================================================

	struct FCarrierAmmoState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;
		TWeakObjectPtr<ATraceCharacter> CarrierPawn;
		TWeakObjectPtr<ATraceCharacter> ControlPawn;

		bool bRedReproduced = false;
		bool bControlWorked = false;
	};

	void RunCarrierTest()
	{
		UWorld* World = FindAuthoritativeWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[AMMOCARRIER] no authoritative game world."));
			return;
		}

		TSharedPtr<FCarrierAmmoState> State = MakeShared<FCarrierAmmoState>();
		State->List.Tag = TEXT("AMMOCARRIER");
		State->Deadline = FPlatformTime::Seconds() + 90.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[AMMOCARRIER] ===== spec v16 §1: 'The Core carrier has no gun, so ammo must not be consumed or "
			     "shown while carrying.' TWO locks: (1) CanFire()/ServerFire refuse a carrier outright, (2) "
			     "ConsumeRound refuses one even when called directly. arm 0 (Trace.Ammo.CarrierGuard 0) must "
			     "reproduce on lock 2, or arm 1's clean run is uninformative. ====="));

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreArms();
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			ATraceCore* CoreActor = ATraceCore::Get(TickWorld);

			// ---- staging: a live carrier and a live non-carrier control ----------------------
			if (State->Step == 0)
			{
				ATraceCharacter* CarrierPawn = (CoreActor != nullptr) ? CoreActor->Carrier : nullptr;
				ATraceCharacter* ControlPawn = nullptr;

				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					ATraceCharacter* Candidate = *It;
					if (Candidate == nullptr || !Candidate->IsAlive() || Candidate->Weapon == nullptr
						|| !Candidate->HasAuthority())
					{
						continue;
					}
					if (Candidate == CarrierPawn || Candidate->IsCarrier())
					{
						continue;
					}
					ControlPawn = Candidate;
					break;
				}

				// GrantTo, not TryPickup: TryPickup is a no-op while there is already a holder, which is
				// exactly the case that has to be corrected when the Core is loose or in flight.
				if (CarrierPawn == nullptr && CoreActor != nullptr && ControlPawn != nullptr)
				{
					CoreActor->GrantTo(ControlPawn, ETraceCoreGrantReason::Debug);
					CarrierPawn = CoreActor->Carrier;
					ControlPawn = nullptr;   // it is the carrier now; find another next tick
				}

				if (CarrierPawn == nullptr || CarrierPawn->Weapon == nullptr
					|| ControlPawn == nullptr || ControlPawn->Weapon == nullptr)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(FString::Printf(
							TEXT("could not stage: carrier=%s control=%s — this needs a live match with at least "
							     "two pawns"),
							*GetNameSafe(CarrierPawn), *GetNameSafe(ControlPawn)));
						State->List.Report();
						RestoreArms();
						return false;
					}
					return true;
				}

				State->CarrierPawn = CarrierPawn;
				State->ControlPawn = ControlPawn;
				UE_LOG(LogTraceGame, Display, TEXT("[AMMOCARRIER] staged: CARRIER %s | CONTROL %s"),
					*GetNameSafe(CarrierPawn), *GetNameSafe(ControlPawn));
				State->Step = 1;
				return true;
			}

			ATraceCharacter* Carrier = State->CarrierPawn.Get();
			ATraceCharacter* Control = State->ControlPawn.Get();
			UTraceWeaponComponent* CarrierWeapon = (Carrier != nullptr) ? Carrier->Weapon : nullptr;
			UTraceWeaponComponent* ControlWeapon = (Control != nullptr) ? Control->Weapon : nullptr;
			if (CarrierWeapon == nullptr || ControlWeapon == nullptr
				|| !Carrier->IsAlive() || !Control->IsAlive())
			{
				State->List.Invalidate(TEXT("a participant went away or died mid-test"));
				State->List.Report();
				RestoreArms();
				return false;
			}

			// *** THE LIVE-CARRIER GATE, RE-ASSERTED EVERY TICK — BUT ONLY WHILE THE SUBJECT IS THE
			// CARRIER. *** Bots pass and score, so a subject who stopped carrying between staging and
			// the strike would make steps 1 and 2 an ordinary ammo test that passes for entirely the
			// wrong reason.
			//
			// IT IS SKIPPED FOR STEP 3, AND THAT IS A BUG THIS HARNESS ALREADY HAD ONCE. Step 2 ends by
			// GRANTING the Core to the control on purpose (that is the cancellation under test), and
			// the gate below would then dutifully take it straight back — so step 3 measured a
			// cancelled reload on a pawn that was no longer carrying anything, and printed
			// "reloading=0, carrying=0", which is a PASS that says nothing. The check now runs while
			// the control is still holding the Core and asserts BOTH facts in one statement.
			if (State->Step < 3)
			{
				if (CoreActor != nullptr && CoreActor->Carrier != Carrier)
				{
					CoreActor->TryPickup(Carrier);
				}
				if (CoreActor == nullptr || CoreActor->Carrier != Carrier || Control->IsCarrier())
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(TEXT("could not keep the Core on the carrier (and off the control)"));
						State->List.Report();
						RestoreArms();
						return false;
					}
					return true;
				}
			}

			// ---- step 1: LOCK 1 — the shipped path refuses a carrier the trigger, and RED lock 2 ----
			if (State->Step == 1)
			{
				State->List.Check(!CarrierWeapon->CanFire(),
					TEXT("LOCK 1: a carrier cannot fire at all, so no round can reach the clip"),
					FString::Printf(TEXT("CanFire()=%d on a live carrier (spec §4's own rule, re-checked here "
					                     "because it is the FIRST reason ammo is not consumed)"),
						CarrierWeapon->CanFire() ? 1 : 0));

				State->List.Check(!CarrierWeapon->ShouldShowAmmo(),
					TEXT("'...or SHOWN while carrying'"),
					FString::Printf(TEXT("ShouldShowAmmo()=%d on the carrier, %d on the control"),
						CarrierWeapon->ShouldShowAmmo() ? 1 : 0, ControlWeapon->ShouldShowAmmo() ? 1 : 0));

				// THE RED ARM for lock 2. Driven through DebugConsumeRound because the shipped path
				// cannot reach ConsumeRound with a carrier at all — lock 1 stops it first — so a
				// trigger-driven red arm would watch the clip hold and prove nothing about lock 2.
				SetArm(TEXT("Trace.Ammo.CarrierGuard"), 0);
				const int32 AlarmBefore = TraceAmmo::GetCarrierRoundsConsumed();
				const int32 ClipBefore = CarrierWeapon->GetClipAmmo();
				CarrierWeapon->DebugConsumeRound();
				const int32 ClipAfter = CarrierWeapon->GetClipAmmo();
				const int32 AlarmAfter = TraceAmmo::GetCarrierRoundsConsumed();
				SetArm(TEXT("Trace.Ammo.CarrierGuard"), 1);

				State->bRedReproduced = (ClipAfter == ClipBefore - 1) && (AlarmAfter > AlarmBefore);
				State->List.Check(State->bRedReproduced,
					TEXT("RED: with the guard removed a carrier's clip DOES lose a round"),
					FString::Printf(TEXT("clip %d -> %d, alarm +%d — this is the failure the shipped guard prevents"),
						ClipBefore, ClipAfter, AlarmAfter - AlarmBefore));

				State->Step = 2;
				return true;
			}

			// ---- step 2: LOCK 2 GREEN, plus the control, plus the pickup cancellation ----------
			if (State->Step == 2)
			{
				RestoreArms();

				const int32 AlarmBefore = TraceAmmo::GetCarrierRoundsConsumed();
				const int32 ClipBefore = CarrierWeapon->GetClipAmmo();
				CarrierWeapon->DebugConsumeRound();
				CarrierWeapon->DebugConsumeRound();
				CarrierWeapon->DebugConsumeRound();
				const int32 ClipAfter = CarrierWeapon->GetClipAmmo();

				State->List.Check(ClipAfter == ClipBefore && TraceAmmo::GetCarrierRoundsConsumed() == AlarmBefore,
					TEXT("LOCK 2 (GREEN): three direct consumptions cost a carrier nothing"),
					FString::Printf(TEXT("clip %d -> %d, alarm +%d (must be +0)"),
						ClipBefore, ClipAfter, TraceAmmo::GetCarrierRoundsConsumed() - AlarmBefore));

				// ---- THE CONTROL. The IDENTICAL call on a non-carrier must really take a round, or
				//      this harness has proved something about itself and nothing about the carrier. ----
				const int32 ControlBefore = ControlWeapon->GetClipAmmo();
				ControlWeapon->DebugConsumeRound();
				const int32 ControlAfter = ControlWeapon->GetClipAmmo();
				State->bControlWorked = (ControlAfter == ControlBefore - 1);

				State->List.Check(State->bControlWorked,
					TEXT("CONTROL: the identical call DOES spend a non-carrier's round"),
					FString::Printf(TEXT("clip %d -> %d on %s"),
						ControlBefore, ControlAfter, *GetNameSafe(Control)));

				// ---- "reloading is cancelled by picking up the Core" ----
				ControlWeapon->RequestReload();
				State->List.Check(ControlWeapon->IsReloading(),
					TEXT("the control starts a reload (the fixture for the cancellation below)"),
					FString::Printf(TEXT("reloading=%d, %.3fs left"),
						ControlWeapon->IsReloading() ? 1 : 0, ControlWeapon->GetReloadRemaining()));

				if (CoreActor != nullptr)
				{
					CoreActor->GrantTo(Control, ETraceCoreGrantReason::Debug);
				}

				State->Step = 3;
				State->NextStepRealTime = NowReal + 0.15;   // let one TickReload see the new carrier
				return true;
			}

			// ---- step 3: the cancellation landed, and the verdict ------------------------------
			//
			// BOTH FACTS IN ONE ASSERTION. "Not reloading" on its own is satisfied by a pawn that never
			// started one, by one whose reload simply finished, and by a pawn that is not carrying at
			// all — so the carrying half is what makes this a statement about the CANCELLATION. The
			// timing is deliberate too: the check runs ~0.15 s into a 0.5 s reload, so a reload that
			// merely ran to completion cannot be mistaken for one that was cancelled.
			{
				const bool bStillCarrying = Control->IsCarrier()
					|| (CoreActor != nullptr && CoreActor->Carrier == Control);
				State->List.Check(bStillCarrying && !ControlWeapon->IsReloading(),
					TEXT("picking up the Core CANCELS a reload in progress"),
					FString::Printf(TEXT("carrying=%d reloading=%d, ~0.35s of the %.2fs reload was still owed"),
						bStillCarrying ? 1 : 0, ControlWeapon->IsReloading() ? 1 : 0,
						TraceAmmo::GetReloadSeconds()));

				// Put the Core back where the harness found it. It is a legal game state either way, but
				// leaving a test's staging behind in a live match is how the NEXT test gets a surprise.
				if (CoreActor != nullptr && Carrier->IsAlive())
				{
					CoreActor->GrantTo(Carrier, ETraceCoreGrantReason::Debug);
				}
			}

			if (!State->bControlWorked)
			{
				State->List.Invalidate(TEXT("the CONTROL consumption on a non-carrier did not move a clip — the "
				                            "fixture cannot spend a round at all, so nothing it says about the "
				                            "carrier means anything"));
			}
			else if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — Trace.Ammo.CarrierGuard 0 failed to "
				                            "let a carrier's clip fall, so the green arm proves nothing"));
			}

			State->List.Report();
			RestoreArms();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Ammo.BotWatch
	// =============================================================================================

	/**
	 * Two halves, because "bots must reload" needs both an OBSERVATION and an EXPERIMENT.
	 *
	 * The observation alone is not enough, and a 120 s run proved it: nine bots spent 21 rounds
	 * between them, which is fewer than one clip, so "no bot reloaded" was a fact about how much the
	 * bots shoot and not about the code. Waiting for a bot to fire thirty rounds in a live match is
	 * waiting on the AI, not on the feature.
	 *
	 * So the experiment DRAINS one live bot's clip through the same consumption function a shot uses,
	 * and then watches that bot put the gun back together with no bot-side code involved at all. The
	 * observation still runs alongside and carries the alarm the experiment cannot: whether any bot,
	 * anywhere, was ever caught standing at zero rounds with no reload running.
	 */
	struct FBotWatchState
	{
		int32 Step = 0;
		double ObserveSeconds = 0.0;
		double ObserveUntilRealTime = 0.0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;

		/** Sampled every tick across every live bot. The "dry-fires forever" alarm. */
		int32 StuckSamples = 0;
		int32 BotSamples = 0;
		int32 BotsSeen = 0;
		int32 BotRoundsAtEnd = 0;

		/** Process-wide deltas. These survive a bot dying and respawning; per-pawn totals do not. */
		int32 GlobalRoundsAtStart = 0;
		int32 GlobalReloadsAtStart = 0;

		TWeakObjectPtr<ATraceCharacter> Victim;
		int32 VictimClipBeforeDrain = 0;
		int32 VictimDrainCalls = 0;
		bool bRedReproduced = false;
	};

	/** A live bot that could reload right now: alive, gun in hand, not carrying the Core. */
	ATraceCharacter* FindDrainableBot(UWorld* World)
	{
		for (TActorIterator<ATraceCharacter> It(World); It; ++It)
		{
			ATraceCharacter* Candidate = *It;
			if (Candidate == nullptr || !Candidate->HasAuthority() || Candidate->Weapon == nullptr
				|| !IsBotControlled(Candidate))
			{
				continue;
			}
			if (Candidate->IsAlive() && !Candidate->IsCarrier() && !Candidate->Weapon->IsKnifeEquipped())
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	void RunBotWatch(const TArray<FString>& Args)
	{
		UWorld* World = FindAuthoritativeWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[AMMOBOTS] no authoritative game world."));
			return;
		}

		const double ObserveSeconds = (Args.Num() > 0)
			? FMath::Clamp(FCString::Atod(*Args[0]), 2.0, 600.0) : 20.0;

		TSharedPtr<FBotWatchState> State = MakeShared<FBotWatchState>();
		State->List.Tag = TEXT("AMMOBOTS");
		State->ObserveSeconds = ObserveSeconds;
		State->ObserveUntilRealTime = FPlatformTime::Seconds() + ObserveSeconds;
		State->Deadline = FPlatformTime::Seconds() + ObserveSeconds + 60.0;
		State->GlobalRoundsAtStart = TraceAmmo::GetRoundsConsumed();
		State->GlobalReloadsAtStart = TraceAmmo::GetReloadsCompleted();

		UE_LOG(LogTraceGame, Display,
			TEXT("[AMMOBOTS] ===== spec v16 §1: 'Bots must respect it and reload; a bot that dry-fires forever "
			     "will read as broken.' Observing every bot for %.0fs, then DRAINING one bot's clip and watching "
			     "it reload itself. ====="), ObserveSeconds);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				RestoreArms();
				return false;
			}

			// --- the observation runs on EVERY tick, through every phase ---------------------
			{
				int32 BotCount = 0;
				int32 RoundsNow = 0;
				for (TActorIterator<ATraceCharacter> It(TickWorld); It; ++It)
				{
					const ATraceCharacter* Candidate = *It;
					if (Candidate == nullptr || !Candidate->HasAuthority() || Candidate->Weapon == nullptr
						|| !IsBotControlled(Candidate))
					{
						continue;
					}
					++BotCount;

					int32 Rounds = 0;
					int32 Reloads = 0;
					Candidate->Weapon->DebugGetAmmoTotals(Rounds, Reloads);
					RoundsNow += Rounds;

					// *** THE DRY-FIRE ALARM. *** A living, gun-holding, non-carrying bot at zero rounds
					// with no reload running is exactly the "reads as broken" case §1 names. Sampled
					// rather than asserted once, because it is a STATE a bot can only be caught in while
					// it lasts — and if the automatic reload works it never lasts a whole frame.
					if (Candidate->IsAlive() && !Candidate->IsCarrier()
						&& !Candidate->Weapon->IsKnifeEquipped())
					{
						++State->BotSamples;
						if (Candidate->Weapon->GetClipAmmo() <= 0 && !Candidate->Weapon->IsReloading())
						{
							++State->StuckSamples;
						}
					}
				}
				State->BotsSeen = FMath::Max(State->BotsSeen, BotCount);
				State->BotRoundsAtEnd = RoundsNow;
			}

			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			// --- phase 0: just observe ------------------------------------------------------
			if (State->Step == 0)
			{
				if (NowReal < State->ObserveUntilRealTime)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[AMMOBOTS] observation over %.0fs: %d bot(s) | process-wide rounds +%d, reloads +%d | "
					     "bot-pawn rounds now %d | dry-stuck %d of %d samples"),
					State->ObserveSeconds,
					State->BotsSeen,
					TraceAmmo::GetRoundsConsumed() - State->GlobalRoundsAtStart,
					TraceAmmo::GetReloadsCompleted() - State->GlobalReloadsAtStart,
					State->BotRoundsAtEnd, State->StuckSamples, State->BotSamples);

				State->Step = 1;
				return true;
			}

			ATraceCharacter* Victim = State->Victim.Get();

			// --- phase 1: pick a bot and RED-ARM the drain ----------------------------------
			if (State->Step == 1)
			{
				Victim = FindDrainableBot(TickWorld);
				if (Victim == nullptr)
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(TEXT("no live, gun-holding, non-carrying bot to drain — this needs "
						                            "a match with bots in it"));
						State->List.Report();
						RestoreArms();
						return false;
					}
					return true;
				}
				State->Victim = Victim;

				SetArm(TEXT("Trace.Ammo.Enabled"), 0);
				const int32 RedBefore = Victim->Weapon->GetClipAmmo();
				for (int32 Index = 0; Index < RedBefore + 5; ++Index)
				{
					Victim->Weapon->DebugConsumeRound();
				}
				State->bRedReproduced = (Victim->Weapon->GetClipAmmo() == RedBefore)
					&& !Victim->Weapon->IsReloading();
				SetArm(TEXT("Trace.Ammo.Enabled"), 1);

				State->List.Check(State->bRedReproduced,
					TEXT("RED ARM: with the mechanic disarmed a bot's clip cannot empty, so it never reloads"),
					FString::Printf(TEXT("%s: clip %d after %d consumptions, reloading=%d"),
						*GetNameSafe(Victim), Victim->Weapon->GetClipAmmo(), RedBefore + 5,
						Victim->Weapon->IsReloading() ? 1 : 0));

				State->Step = 2;
				return true;
			}

			// The bot has to still be a legal subject for the rest of this to mean anything — one that
			// died, took the Core or drew the knife would be a true statement about the wrong pawn.
			if (Victim == nullptr || Victim->Weapon == nullptr || !Victim->IsAlive()
				|| Victim->IsCarrier() || Victim->Weapon->IsKnifeEquipped())
			{
				State->List.Invalidate(TEXT("the drained bot stopped being a legal subject mid-test (died, took "
				                            "the Core, or drew the knife) — rerun"));
				State->List.Report();
				RestoreArms();
				return false;
			}

			// --- phase 2: drain it for real -------------------------------------------------
			if (State->Step == 2)
			{
				State->VictimClipBeforeDrain = Victim->Weapon->GetClipAmmo();
				State->VictimDrainCalls = 0;
				while (Victim->Weapon->GetClipAmmo() > 0 && State->VictimDrainCalls <= 1000)
				{
					Victim->Weapon->DebugConsumeRound();
					++State->VictimDrainCalls;
				}

				State->List.Check(State->VictimDrainCalls == State->VictimClipBeforeDrain
					&& Victim->Weapon->GetClipAmmo() == 0,
					TEXT("a bot's clip is the same finite clip a human's is"),
					FString::Printf(TEXT("%s: %d consumption(s) took %d rounds to zero"),
						*GetNameSafe(Victim), State->VictimDrainCalls, State->VictimClipBeforeDrain));

				State->Step = 3;
				State->NextStepRealTime = NowReal + 0.15;   // let one TickReload run on that pawn
				return true;
			}

			// --- phase 3: it reloads itself -------------------------------------------------
			if (State->Step == 3)
			{
				State->List.Check(Victim->Weapon->IsReloading(),
					TEXT("'Bots must respect it and reload' — the bot starts a reload with nobody telling it to"),
					FString::Printf(TEXT("%s: reloading=%d, %.3fs left. There is no bot-side ammo code at all; "
					                     "TickReload runs on its pawn because the pawn is authoritative and "
					                     "locally controlled in this process"),
						*GetNameSafe(Victim), Victim->Weapon->IsReloading() ? 1 : 0,
						Victim->Weapon->GetReloadRemaining()));

				State->Step = 4;
				State->NextStepRealTime = NowReal + static_cast<double>(TraceAmmo::GetReloadSeconds()) + 0.25;
				return true;
			}

			// --- phase 4: and comes back with a full clip, then the verdict ------------------
			State->List.Check(Victim->Weapon->GetClipAmmo() == TraceAmmo::GetClipSize(),
				TEXT("...and comes back with a full clip"),
				FString::Printf(TEXT("%s: clip %d/%d"), *GetNameSafe(Victim),
					Victim->Weapon->GetClipAmmo(), TraceAmmo::GetClipSize()));

			State->List.Check(State->StuckSamples == 0,
				TEXT("no bot was EVER caught standing at zero rounds with no reload running"),
				FString::Printf(TEXT("%d dry-stuck sample(s) of %d live bot samples across the whole run — this "
				                     "is the 'dry-fires forever' failure, sampled every tick on every bot"),
					State->StuckSamples, State->BotSamples));

			// LIVENESS, and it is reported rather than asserted. Bots in a headless match shoot far less
			// than one clip's worth in a minute (measured: 21 rounds across 9 bots in 120 s), so
			// "no bot organically emptied a clip" is a fact about the AI and not about this feature. The
			// experiment above is what makes the verdict mean something; this is context.
			UE_LOG(LogTraceGame, Display,
				TEXT("[AMMOBOTS] context: %d bot(s) seen, %d round(s) on their current pawns, process-wide +%d "
				     "rounds and +%d reloads since this command started (the drain and reload above are inside "
				     "those two numbers)"),
				State->BotsSeen, State->BotRoundsAtEnd,
				TraceAmmo::GetRoundsConsumed() - State->GlobalRoundsAtStart,
				TraceAmmo::GetReloadsCompleted() - State->GlobalReloadsAtStart);

			if (State->BotSamples == 0)
			{
				State->List.Invalidate(TEXT("no live bot was ever sampled — the dry-fire alarm had nothing to "
				                            "watch, so its zero means nothing"));
			}
			else if (!State->bRedReproduced)
			{
				State->List.Invalidate(TEXT("the RED arm did not reproduce — Trace.Ammo.Enabled 0 failed to stop "
				                            "the bot's clip emptying, so the reload below proves nothing"));
			}

			State->List.Report();
			RestoreArms();
			return false;
		}));
	}

	// =============================================================================================
	// Trace.Ammo.ClientPredictTest — spec v16 §1's prediction clause, measured where it exists
	//
	// "Server-authoritative, client-predicted for feel — the local player must see the count drop on
	// their own shot without waiting for a round trip, exactly like the existing fire path."
	//
	// *** THIS CANNOT BE RUN ON THE HOST, AND REFUSES TO BE. *** On the authority FireOnce does not
	// take the predicted branch at all (ServerFire spends the round instead), so a host-side run would
	// watch the count drop for entirely the wrong reason and report the feature working on a build
	// that had no prediction in it. It runs on a JOINED CLIENT or not at all.
	//
	// THE MEASUREMENT IS ONE SHOT, READ ON THE NEXT TICK. A burst would be ambiguous — at 0.40 s
	// between rounds and a 200 ms round trip, "the count is one behind" is only about half an
	// interval and reads as noise. One round, sampled ~16 ms later, is unambiguous: predicted means
	// -1 now, replicated means -0 now and -1 a round trip later. Run the client under
	// NetEmulation.PktLag 150 or more and the two answers are half a second apart.
	// =============================================================================================

	struct FPredictTestState
	{
		int32 Step = 0;
		double NextStepRealTime = 0.0;
		double Deadline = 0.0;
		FChecklist List;
		TWeakObjectPtr<ATraceCharacter> Subject;
		int32 ClipBefore = 0;
		int32 ClipImmediatelyAfter = 0;
		double FiredAtRealTime = 0.0;
	};

	void RunClientPredictTest()
	{
		// Any game world; the point is that it must NOT be authoritative.
		UWorld* World = nullptr;
		if (GEngine != nullptr)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (UWorld* Candidate = Context.World(); Candidate != nullptr && Candidate->IsGameWorld())
				{
					World = Candidate;
					break;
				}
			}
		}

		if (World == nullptr || World->GetNetMode() != NM_Client)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[AMMOPREDICT] *** CLIENT ONLY. *** netmode=%d. On the server (or a listen host's own pawn) "
				     "the predicted branch in FireOnce is never taken — ServerFire spends the round — so a run "
				     "here would report prediction working on a build that has none. Join a host and run it there."),
				(World != nullptr) ? static_cast<int32>(World->GetNetMode()) : -1);
			return;
		}

		TSharedPtr<FPredictTestState> State = MakeShared<FPredictTestState>();
		State->List.Tag = TEXT("AMMOPREDICT");
		State->Deadline = FPlatformTime::Seconds() + 60.0;

		UE_LOG(LogTraceGame, Display,
			TEXT("[AMMOPREDICT] ===== spec v16 §1: 'client-predicted for feel — the local player must see the "
			     "count drop on their own shot without waiting for a round trip.' Trace.Ammo.Predict is %d "
			     "(1 = shipped, 0 = the RED arm: wait for replication). ====="),
			TraceAmmo::IsPredictionEnabled() ? 1 : 0);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[State, WeakWorld = TWeakObjectPtr<UWorld>(World)](float) -> bool
		{
			UWorld* TickWorld = WeakWorld.Get();
			const double NowReal = FPlatformTime::Seconds();
			if (TickWorld == nullptr)
			{
				return false;
			}
			if (NowReal < State->NextStepRealTime)
			{
				return true;
			}

			ATraceCharacter* Subject = State->Subject.Get();
			if (State->Step == 0)
			{
				const APlayerController* PC = TickWorld->GetFirstPlayerController();
				Subject = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
				if (Subject == nullptr || Subject->Weapon == nullptr || !Subject->IsAlive()
					|| Subject->IsCarrier() || Subject->Weapon->IsKnifeEquipped()
					|| !Subject->Weapon->CanFire())
				{
					if (NowReal > State->Deadline)
					{
						State->List.Invalidate(TEXT("no local pawn that could fire right now — this needs a live, "
						                            "gun-holding, non-carrying pawn off cooldown"));
						State->List.Report();
						return false;
					}
					return true;
				}
				State->Subject = Subject;

				State->ClipBefore = Subject->Weapon->GetClipAmmo();
				State->FiredAtRealTime = NowReal;
				Subject->Weapon->StartFire();     // exactly one round: StopFire lands next tick
				State->Step = 1;
				return true;
			}

			if (Subject == nullptr || Subject->Weapon == nullptr || !Subject->IsAlive())
			{
				State->List.Invalidate(TEXT("the local pawn went away mid-test"));
				State->List.Report();
				return false;
			}

			// ---- the next tick: the shot has been fired and NOTHING has come back from the server ----
			if (State->Step == 1)
			{
				Subject->Weapon->StopFire();
				State->ClipImmediatelyAfter = Subject->Weapon->GetClipAmmo();

				const double ElapsedMs = (NowReal - State->FiredAtRealTime) * 1000.0;
				const bool bDroppedNow = (State->ClipImmediatelyAfter == State->ClipBefore - 1);

				State->List.Check(bDroppedNow,
					TEXT("the SHOOTER's own count drops on their own shot, with no round trip"),
					FString::Printf(TEXT("clip %d -> %d, %.1f ms after the trigger. Trace.Ammo.Predict=%d. A "
					                     "replication-driven count would still read %d here and only fall a "
					                     "round trip later"),
						State->ClipBefore, State->ClipImmediatelyAfter, ElapsedMs,
						TraceAmmo::IsPredictionEnabled() ? 1 : 0, State->ClipBefore));

				State->Step = 2;
				State->NextStepRealTime = NowReal + 1.5;   // comfortably more than any sane round trip
				return true;
			}

			// ---- 1.5 s later: the server's copy has landed and must AGREE, not refund ----
			const int32 ClipAfterSettle = Subject->Weapon->GetClipAmmo();

			State->List.Check(ClipAfterSettle <= State->ClipBefore - 1,
				TEXT("the server's copy CONFIRMS the predicted round rather than handing it back"),
				FString::Printf(TEXT("clip %d immediately after the shot, %d once replication settled. A count "
				                     "that climbed back to %d would be the 30 -> 29 -> 30 -> 29 bounce "
				                     "OnRep_Ammo's min-rule exists to prevent"),
					State->ClipImmediatelyAfter, ClipAfterSettle, State->ClipBefore));

			UE_LOG(LogTraceGame, Display,
				TEXT("[AMMOPREDICT] netmode=%d | clip before %d, immediately after %d, settled %d | "
				     "reloading=%d | predict arm=%d"),
				static_cast<int32>(TickWorld->GetNetMode()), State->ClipBefore, State->ClipImmediatelyAfter,
				ClipAfterSettle, Subject->Weapon->IsReloading() ? 1 : 0,
				TraceAmmo::IsPredictionEnabled() ? 1 : 0);

			State->List.Report();
			return false;
		}));
	}

	FAutoConsoleCommandWithWorld CmdAmmoDump(
		TEXT("Trace.Ammo.Dump"),
		TEXT("Dev only. Log the ammo values this process resolved RIGHT NOW, the alarms, and the local pawn's live clip."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&RunDump));

	FAutoConsoleCommand CmdAmmoTest(
		TEXT("Trace.Ammo.Test"),
		TEXT("Dev only, SERVER. Spec v16 §1: prove one round per shot through the real trigger, the automatic "
		     "reload at zero, fire refused for the whole 0.5 s, the refill, and the manual R rules. Red-arms "
		     "itself with Trace.Ammo.Enabled 0 first and reports INVALID if that arm did not reproduce."),
		FConsoleCommandDelegate::CreateStatic(&RunAmmoTest));

	FAutoConsoleCommand CmdAmmoCarrierTest(
		TEXT("Trace.Ammo.CarrierTest"),
		TEXT("Dev only, SERVER. Spec v16 §1: the Core carrier consumes no ammo, shows no ammo, and has any "
		     "reload cancelled by the pickup. Red-arms the consumption guard and carries a live non-carrier "
		     "CONTROL, so a clean run cannot be a fixture that never fired."),
		FConsoleCommandDelegate::CreateStatic(&RunCarrierTest));

	FAutoConsoleCommand CmdAmmoClientPredictTest(
		TEXT("Trace.Ammo.ClientPredictTest"),
		TEXT("Dev only, *** CLIENT ONLY ***. Spec v16 §1: prove the shooter's own ammo count drops on their own "
		     "shot with no round trip, and that the server's copy then confirms it rather than refunding the "
		     "round. Red arm: Trace.Ammo.Predict 0. Run the client under NetEmulation.PktLag 150+."),
		FConsoleCommandDelegate::CreateStatic(&RunClientPredictTest));

	FAutoConsoleCommand CmdAmmoBotWatch(
		TEXT("Trace.Ammo.BotWatch"),
		TEXT("Dev only, SERVER. Trace.Ammo.BotWatch [seconds=60]: watch every bot's real clip and prove bots "
		     "spend rounds, complete reloads, and are never caught dry with no reload running."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunBotWatch));
}

#endif // !UE_BUILD_SHIPPING
