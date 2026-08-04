// Copyright (c) Trace. All Rights Reserved.
//
// See TraceCore.h for the model. This file is the whole of it: there is no physics, no pickup
// volume and no flight path left to go wrong.

#include "Gameplay/TraceCore.h"

#include "Net/UnrealNetwork.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "World/TraceArenaBuilder.h"

#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/EngineTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                        // TActorIterator (character gather fallback)
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"        // GetServerWorldTimeSeconds()
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"             // FMath::SegmentDistToSegmentSafe
#include "UObject/ConstructorHelpers.h"

namespace TraceCoreTuning
{
	// ---------------------------------------------------------------------------------------------
	// PROMOTED. Pass hold, cooldown, range, aim cone, aim slack, chest offset and the turnover trace
	// grace all used to live here as compile-time constants, with a comment asking for exactly this:
	// "Every one of them is a designer knob and they should be promoted to UTraceSettings". They now
	// are - UTraceSettings, Category "Core|Pass" - and this file reads them at the point of use, so
	// they retune with PIE running. Nothing is left behind here except the two PRE-FIX values the
	// Trace.Pass.Fixes A/B switch replays, and the constants that are genuinely not designer-facing.
	// ---------------------------------------------------------------------------------------------

	/**
	 * PRE-FIX aim cone and slack. Read ONLY when Trace.Pass.Fixes is 0, which replays the exact
	 * behaviour the user reported as broken so the before/after numbers come from one binary.
	 * The live values are UTraceSettings::PassAimConeDegrees / PassAimSlack (9 deg / 70 uu).
	 */
	constexpr double PassAimConeDegrees = 6.0;
	constexpr double PassAimSlack = 40.0;

	/**
	 * Line-of-sight probe heights, as offsets from the receiver's capsule CENTRE. Chest, head,
	 * knees.
	 *
	 * A single chest-height ray against 176 uu / 352 uu / 616 uu cover boxes is a coin flip: a
	 * receiver whose head and shoulders are plainly visible over a 1x block fails the test because
	 * the one point being probed is behind it. ANY clear probe means line of sight, which is both
	 * more generous and much closer to what the player can actually see.
	 *
	 * Entry 0 is a placeholder: the chest probe is overridden at the call site with
	 * UTraceSettings::PassTargetChestOffsetZ, so the probed chest and the AIMED-AT chest are
	 * guaranteed to be the same point. Toggled by UTraceSettings::bPassMultiPointLos.
	 */
	constexpr double LosProbeOffsetsZ[] = { 20.0, 70.0, -45.0 };

	/**
	 * How long a kickoff waits before it is granted.
	 *
	 * Every caller of KickoffTo() teleports ten pawns immediately afterwards
	 * (ATraceGameMode::ResetPlayersToSpawns, which also clears every trail). Granting inside that
	 * window would lay a trail across the teleport and then have it wiped from under us.
	 */
	constexpr float KickoffDelaySeconds = 1.0f;

	/**
	 * Backstop: a Core that has been holderless this long with nobody owed it grants itself to the
	 * default team. The Core must never be idle - there is no way to pick it up any more, so a
	 * holderless Core is a dead match, not merely a quiet one.
	 */
	constexpr float MaxHolderlessSeconds = 5.0f;

	/**
	 * Last-ditch recovery when the Core has been parked OUT OF PLAY this long while a half is
	 * actually running. Long enough that no legitimate interval trips it, short enough that a
	 * mistake in the match-flow code costs one warning line rather than a dead match.
	 */
	constexpr float OutOfPlayRecoverySeconds = 15.0f;

	/** §1: the Core starts with Team A. Blue is Team A. */
	constexpr ETraceTeam DefaultKickoffTeam = ETraceTeam::Blue;

	// --- Cosmetics --------------------------------------------------------------------------------

	/** Orb centre above the holder's capsule centre. Capsule half-height is 88, so this clears the head. */
	constexpr double OrbHeight = 150.0;

	/**
	 * MEASURED. The first pass ran this at 0.55 (a 55uu orb) with a glow of 2.4, and captured frames
	 * of the holder's own third-person view show the result: a ~150px pure-white disc parked in the
	 * middle of the frame. Two separate faults in one object — it clipped every channel, so the team
	 * colour it exists to communicate was gone, and it was a large unlit emissive surface a few
	 * hundred uu from a camera, which is precisely the point-blank whiteout failure mode this build
	 * already has an open defect for. Smaller and dimmer, and hidden from its own holder (see
	 * ApplyAttachment).
	 */
	constexpr float OrbScale = 0.40f;

	/** The shaft that makes a holder findable across a 24000uu field. */
	constexpr double BeaconBottom = 205.0;
	constexpr double BeaconTop = 1150.0;
	constexpr double BeaconWidth = 26.0;

	/**
	 * Glow multipliers on M_TraceNeon.
	 *
	 * Deliberately restrained. An unlit emissive is distance-invariant, so anything attached to a
	 * pawn is a point-blank whiteout risk for whoever is fighting them; the orb sits 150uu above the
	 * capsule centre (i.e. above eye height) and the shaft starts higher still, and neither is
	 * pushed as hard as the arena trim. The PASS glow is the exception and is meant to be read as
	 * "that player is vulnerable right now".
	 */
	constexpr float OrbGlow = 1.25f;
	constexpr float BeaconGlow = 1.15f;
	constexpr float PassGlowMultiplier = 1.9f;

	/** Home-position tolerance: the Core will not bother re-parking itself inside this. */
	constexpr double HomeToleranceSq = 75.0 * 75.0;
}

namespace
{
	/** Divide-by-zero epsilon. Literal on purpose: the KINDA_SMALL_NUMBER family was re-spelled
	 *  during the 5.x line and this module must compile on 5.4 - 5.8. */
	constexpr double CoreGeometryEpsilon = 1.0e-8;

	/** True when both teams are known and different. Unknown teams are never enemies. */
	bool AreEnemies(ETraceTeam A, ETraceTeam B)
	{
		return A != ETraceTeam::None && B != ETraceTeam::None && A != B;
	}

	/** True when both teams are known and equal. */
	bool AreAllies(ETraceTeam A, ETraceTeam B)
	{
		return A != ETraceTeam::None && A == B;
	}
}

// =================================================================================================
// §4.1 PASS TUNABLES
//
// Every number the pass runs on is now a UTraceSettings property, read AT THE POINT OF USE so it
// retunes with PIE running - which is what the user asked for: "implement these as tunable
// variables so I can playtest and adjust numbers rather than you guessing at feel." See
// UTraceSettings, Category "Core|Pass". The TraceCoreTuning constants below survive only as the
// PRE-FIX values, which is what the A/B switch replays.
//
// Trace.Pass.Fixes is that switch. Setting it to 0 restores the exact pre-fix behaviour (no grace,
// 6-degree cone, 40 uu slack, single chest LOS ray, a full 2 s cooldown on a cancel, no sticky
// acquisition) so the before/after numbers in the report come from ONE binary rather than two -
// nothing else changed between the two runs, including the map, the bots and the RNG seed.
// =================================================================================================

static TAutoConsoleVariable<int32> CVarTracePassFixes(
	TEXT("Trace.Pass.Fixes"),
	1,
	TEXT("1 (default): apply the section 4.1 pass-reliability fixes and read UTraceSettings for their values. 0: exact pre-fix behaviour, for A/B measurement."),
	ECVF_Default);

namespace TraceCoreTuning
{
	/** Grace window, or 0 when the A/B switch is replaying pre-fix behaviour. */
	float ResolvedValidationGrace(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Clamp(Settings.PassValidationGraceSeconds, 0.f, 2.f)
			: 0.f;
	}

	/** Sticky acquisition window, or 0 when replaying pre-fix behaviour. */
	float ResolvedAcquireSticky(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Clamp(Settings.PassAcquireStickySeconds, 0.f, 2.f)
			: 0.f;
	}

	/** Cancel cooldown. Pre-fix this was the same two seconds a COMPLETED pass spends. */
	float ResolvedCancelCooldown(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Max(0.f, Settings.PassCancelCooldownSeconds)
			: FMath::Max(0.f, Settings.PassCooldownSeconds);
	}

	double ResolvedAimConeDegrees(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Clamp(static_cast<double>(Settings.PassAimConeDegrees), 0.5, 60.0)
			: PassAimConeDegrees;
	}

	double ResolvedAimSlack(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0)
			? FMath::Clamp(static_cast<double>(Settings.PassAimSlack), 0.0, 500.0)
			: PassAimSlack;
	}

	bool ResolvedMultiPointLos(const UTraceSettings& Settings)
	{
		return (CVarTracePassFixes.GetValueOnAnyThread() != 0) && Settings.bPassMultiPointLos;
	}
}

static TAutoConsoleVariable<int32> CVarTracePassStats(
	TEXT("Trace.PassStats"),
	0,
	TEXT("1: accumulate pass acquisition / cancellation statistics. Trace.PassStats.Dump prints them."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarTracePassStatsInterval(
	TEXT("Trace.PassStats.Interval"),
	20.f,
	TEXT("Seconds between automatic Trace.PassStats dumps. 0 disables the automatic dump."),
	ECVF_Default);

namespace TracePassStats
{
	/**
	 * "Sometimes the pass option doesn't show up" is a claim about how OFTEN, so this counts how
	 * often. Sampled server-side at 10 Hz for whoever is holding the Core: does FindPassTargetFor()
	 * return anybody, and when it does not, which test refused each teammate?
	 *
	 * That last column is the whole diagnosis. "No receiver" caused by everyone being genuinely
	 * behind cover is a map problem; "no receiver" caused by the 6-degree cone is a tuning problem;
	 * and they are indistinguishable from inside the game.
	 */
	struct FStats
	{
		// --- availability, sampled while somebody is holding the Core ---
		//
		// TWO numbers, because they answer two different questions and only the second one is the
		// bug report. RawSamplesWithTarget asks the instantaneous rule "is any teammate legal RIGHT
		// NOW"; SamplesWithTarget asks FindPassTargetFor(), which is what ATraceHUD polls to decide
		// whether to draw the pass affordance at all and therefore what the player means by "the
		// pass option". The gap between them is exactly what the sticky window closes.
		int32 CarrySamples = 0;
		int32 RawSamplesWithTarget = 0;
		int32 SamplesWithTarget = 0;
		int32 SamplesWithoutTarget = 0;

		/** Per-teammate refusal tally over those samples. Indexed by ETracePassRejectReason. */
		int32 RefusalCount[8] = {};
		int32 TeammatesConsidered = 0;
		int32 TeammatesLegal = 0;

		// --- the state machine ---
		int32 PassesBegun = 0;
		int32 PassesCompleted = 0;
		int32 CancelReleased = 0;
		int32 CancelLostTarget = 0;
		int32 CancelHolderGone = 0;

		/** Times the grace window absorbed a blink of illegality that would previously have cancelled. */
		int32 GraceSaves = 0;
		/** Times the grace window expired and the pass cancelled anyway. */
		int32 GraceExpired = 0;
		/** Total seconds of in-flight pass time spent inside the grace window. */
		double GraceSecondsAccrued = 0.0;

		/** Times the sticky window kept a receiver the instantaneous test had dropped. */
		int32 StickySaves = 0;

		double LastSampleTime = 0.0;
		double LastDumpTime = 0.0;
	};

	static FStats GStats;

	static const TCHAR* RefusalName(int32 Index);

	static float Percent(int32 Part, int32 Whole)
	{
		return (Whole > 0) ? (100.f * static_cast<float>(Part) / static_cast<float>(Whole)) : 0.f;
	}

	static void Reset()
	{
		const double Keep = GStats.LastDumpTime;
		GStats = FStats();
		GStats.LastDumpTime = Keep;
	}

	static void Dump()
	{
		const FStats& S = GStats;
		UE_LOG(LogTraceGame, Display, TEXT("========== TRACE PASS STATS =========="));
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT availability: %d samples while carrying | a receiver was offered on %d (%.1f%%), none on %d (%.1f%%)"),
			S.CarrySamples,
			S.SamplesWithTarget, Percent(S.SamplesWithTarget, S.CarrySamples),
			S.SamplesWithoutTarget, Percent(S.SamplesWithoutTarget, S.CarrySamples));
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT rawrule     : the instantaneous rule alone offered a receiver on %d (%.1f%%) - the gap to the line above is what the sticky window recovers"),
			S.RawSamplesWithTarget, Percent(S.RawSamplesWithTarget, S.CarrySamples));
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT teammates   : %d evaluated, %d legal (%.1f%%)"),
			S.TeammatesConsidered, S.TeammatesLegal, Percent(S.TeammatesLegal, S.TeammatesConsidered));

		FString Refusals;
		for (int32 Index = 1; Index < 8; ++Index)
		{
			if (S.RefusalCount[Index] > 0)
			{
				Refusals += FString::Printf(TEXT("  %-20s %6d (%.1f%%)\n"),
					RefusalName(Index), S.RefusalCount[Index],
					Percent(S.RefusalCount[Index], S.TeammatesConsidered));
			}
		}
		UE_LOG(LogTraceGame, Display, TEXT("PASSSTAT refusals by test:\n%s"), *Refusals);

		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT state       : begun %d, completed %d (%.1f%%) | cancels: released %d, lost target %d, holder gone %d"),
			S.PassesBegun, S.PassesCompleted, Percent(S.PassesCompleted, S.PassesBegun),
			S.CancelReleased, S.CancelLostTarget, S.CancelHolderGone);
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT grace       : saves %d, expired %d | %.2fs total spent inside the grace window"),
			S.GraceSaves, S.GraceExpired, S.GraceSecondsAccrued);
		UE_LOG(LogTraceGame, Display,
			TEXT("PASSSTAT sticky      : %d acquisitions kept alive by the sticky window"), S.StickySaves);
		UE_LOG(LogTraceGame, Display, TEXT("======================================"));
	}
} // namespace TracePassStats

static FAutoConsoleCommand GTracePassStatsDumpCmd(
	TEXT("Trace.PassStats.Dump"),
	TEXT("Prints the accumulated pass acquisition / cancellation statistics gathered while Trace.PassStats is 1."),
	FConsoleCommandDelegate::CreateStatic([]() { TracePassStats::Dump(); }));

static FAutoConsoleCommand GTracePassStatsResetCmd(
	TEXT("Trace.PassStats.Reset"),
	TEXT("Clears the accumulated pass statistics."),
	FConsoleCommandDelegate::CreateStatic([]() { TracePassStats::Reset(); }));


// =================================================================================================
// Construction
// =================================================================================================

ATraceCore::ATraceCore()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;

	// The Core is a status: everybody has to know who holds it, from anywhere on a 24000uu field,
	// or the beacon it drives is pointless.
	bAlwaysRelevant = true;

	// Movement replication is off for good. Attached => attachment replicates the transform;
	// holderless => every machine computes the home location identically.
	SetReplicateMovement(false);
	SetCanBeDamaged(false);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Root->SetMobility(EComponentMobility::Movable);

	// NO COLLISION ANYWHERE ON THIS ACTOR. There is nothing to run into, nothing to catch, and
	// nothing that may ever eat a bullet meant for a player.
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCollisionProfileName(TEXT("NoCollision"));
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->SetCastShadow(false);
	Mesh->bReceivesDecals = false;
	Mesh->SetRelativeScale3D(FVector(TraceCoreTuning::OrbScale));

	// HIDDEN FROM THE HOLDER'S OWN CAMERA, AND ONLY FROM THEIRS.
	//
	// This marker exists so that everyone ELSE can find the holder; the holder already knows, from
	// the HUD banner, the shield indicator and the third-person pull-back. Left visible to them it
	// is a bright emissive object suspended a few hundred uu in front of their own lens - the same
	// class of defect as the arena trim whiteout, and captured frames confirmed it: the orb filled
	// the centre of the holder's screen.
	//
	// SetOwnerNoSee resolves through the ACTOR OWNER CHAIN, and GrantTo() SetOwner()s this actor to
	// its holder, so "the owner" is exactly the one player who should not see it. Every other client
	// draws the full beacon. ApplyAttachment() re-dirties the render state whenever the holder
	// changes, because the proxy caches that chain when it is built.
	Mesh->SetOwnerNoSee(true);

	Beacon = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Beacon"));
	Beacon->SetupAttachment(Root);
	Beacon->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Beacon->SetCollisionProfileName(TEXT("NoCollision"));
	Beacon->SetGenerateOverlapEvents(false);
	Beacon->SetCanEverAffectNavigation(false);
	Beacon->SetCastShadow(false);
	Beacon->bReceivesDecals = false;
	Beacon->SetOwnerNoSee(true);   // Same reason as the orb above.

	// Constructor-time FObjectFinders are what make these engine assets cook into a packaged build;
	// a bare runtime LoadObject would resolve to null once cooked.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMeshFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMeshFinder.Succeeded())
	{
		Mesh->SetStaticMesh(SphereMeshFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMeshFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMeshFinder.Succeeded())
	{
		Beacon->SetStaticMesh(CylinderMeshFinder.Object);
	}

	// Same material policy as the trail and the arena: the generated unlit neon material if the
	// content script has been run, the lit engine basic material otherwise. No .uasset we author by
	// hand is ever a hard requirement.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		BaseMaterial = NeonFinder.Object;
		bMaterialIsNeon = true;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BaseMaterial == nullptr && BasicFinder.Succeeded())
	{
		BaseMaterial = BasicFinder.Object;
		bMaterialIsNeon = false;
	}
}

void ATraceCore::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceCore, Carrier);
	DOREPLIFETIME(ATraceCore, bPassActive);
	DOREPLIFETIME(ATraceCore, PassTarget);
	DOREPLIFETIME(ATraceCore, PassStartServerTime);
	DOREPLIFETIME(ATraceCore, PassCooldownEndServerTime);
}

void ATraceCore::BeginPlay()
{
	Super::BeginPlay();

	SpawnHomeLocation = GetActorLocation();

	// Shader work is pointless (and unreliable - shaders are not cooked for server targets) on a
	// dedicated server.
	if (BaseMaterial != nullptr && GetNetMode() != NM_DedicatedServer)
	{
		if (Mesh != nullptr)
		{
			MeshMID = Mesh->CreateDynamicMaterialInstance(0, BaseMaterial);
		}
		if (Beacon != nullptr)
		{
			BeaconMID = Beacon->CreateDynamicMaterialInstance(0, BaseMaterial);
		}
	}

	if (HasAuthority())
	{
		// Nobody has it yet and nobody is owed it. The Tick backstop grants it to the default team
		// if the GameMode has not called KickoffTo() within a few seconds.
		PendingGrantTime = GetServerTimeSeconds() + TraceCoreTuning::MaxHolderlessSeconds;
	}

	ApplyAttachment();
	UpdateVisuals();
}

void ATraceCore::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ATraceCharacter* Bound = BoundDeathHolder.Get())
	{
		if (Bound->Health != nullptr)
		{
			Bound->Health->OnDeath.RemoveDynamic(this, &ATraceCore::OnHolderDeath);
		}
	}
	BoundDeathHolder = nullptr;

	Super::EndPlay(EndPlayReason);
}


// =================================================================================================
// Tick
// =================================================================================================

void ATraceCore::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Carrier / State / bPassActive are independent properties and can land in any order, so every
	// machine reconciles from Tick as well as from the OnReps.
	if (!bAppliedEver || AppliedHolder.Get() != Carrier || bAppliedPassActive != bPassActive)
	{
		ApplyAttachment();
		UpdateVisuals();
	}

	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	// ---- 1. A queued fallback from DropAt(). --------------------------------------------------
	//
	// Deferred by exactly one tick on purpose. ATraceGameMode::NotifyCharacterDied() calls DropAt()
	// from inside the health component's OnDeath broadcast, BEFORE our own OnHolderDeath() listener
	// on the same broadcast has run - and ours is the one that knows who the killer was. Resolving
	// DropAt immediately would hand the Core to the nearest enemy and then immediately re-hand it to
	// the killer, which is two transfers, two grace periods and two packets for one death.
	if (bFallbackQueued)
	{
		bFallbackQueued = false;
		if (Carrier == nullptr)
		{
			ResolveFallback(FallbackTeam);
		}
	}

	// ---- 2. Holder sanity. The Core may never ride a corpse or a destroyed pawn. ---------------
	if (Carrier != nullptr && (!IsValid(Carrier) || !Carrier->IsAlive()))
	{
		const ETraceTeam LostTeam = IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::None;
		UE_LOG(LogTraceGame, Verbose, TEXT("Core: holder became invalid; applying fallback."));
		ReleaseHolder();
		ResolveFallback(LostTeam);
	}

	// ---- 3. Holderless: resolve whoever is owed it, or fall back to the default team. ----------
	if (Carrier == nullptr)
	{
		// Deliberately unowned (half-time interval, post-match). Do nothing — except keep the
		// last-ditch recovery below alive, because "out of play" during an IN PROGRESS half would
		// be a dead match, and this class's one hard promise is that the Core never goes missing.
		if (bOutOfPlay)
		{
			// "LIVE" MUST EXCLUDE HALF TIME, and the obvious test does not.
			// ATraceGameState.h states it outright: TraceMatchState stays InProgress THROUGH the
			// interval on purpose. So `== InProgress` alone is true during half time, and this
			// recovery would fire 15 s into a 12 s break — granting the Core to Blue, starting a
			// trace and teleporting pawns in the middle of the side switch. Four seconds of margin,
			// on a break length that is `config` AND settable per-run via ?breaklen= and
			// -TraceHalfTimeSeconds=. Any longer break trips it.
			//
			// IsHalfTimeBreak() is the question this code was always asking; the constant's own doc
			// comment says "while a half is actually RUNNING", which is not what the enum means.
			bool bMatchLive = false;
			if (const ATraceGameState* GameState = GetWorld() ? GetWorld()->GetGameState<ATraceGameState>() : nullptr)
			{
				bMatchLive = (GameState->TraceMatchState == ETraceMatchState::InProgress)
					&& !GameState->IsHalfTimeBreak();
			}

			if (bMatchLive && (Now - PendingGrantTime) >= TraceCoreTuning::OutOfPlayRecoverySeconds)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("Core: out of play for %.0fs while the match is running - forcing a kickoff so play can continue."),
					TraceCoreTuning::OutOfPlayRecoverySeconds);
				bOutOfPlay = false;
				PendingGrantTeam = TraceCoreTuning::DefaultKickoffTeam;
			}
		}
		else if (Now >= PendingGrantTime)
		{
			if (PendingGrantTeam == ETraceTeam::None)
			{
				PendingGrantTeam = TraceCoreTuning::DefaultKickoffTeam;
			}
			if (!TryResolvePendingGrant())
			{
				// Nobody alive on that side yet (everyone is on a respawn timer). Try again shortly;
				// the Core waits rather than vanishing.
				PendingGrantTime = Now + 0.25f;
			}
		}

		// Park it at home so a holderless Core is somewhere sensible rather than on a corpse.
		if (FVector::DistSquared(GetActorLocation(), GetHomeLocation()) > TraceCoreTuning::HomeToleranceSq)
		{
			SetActorLocation(GetHomeLocation(), false, nullptr, ETeleportType::TeleportPhysics);
		}
		return;
	}

	// ---- 4. Held: run the pass state machine and keep the trace alive. -------------------------
	ServerTickPass(DeltaSeconds);
	EnforceHolderTrailState();
	SamplePassAvailabilityStats();
}

void ATraceCore::SamplePassAvailabilityStats()
{
	if (!HasAuthority() || CVarTracePassStats.GetValueOnAnyThread() == 0 || !IsValid(Carrier) || !Carrier->IsAlive())
	{
		return;
	}

	// 10 Hz. The point is a distribution over a match, not a per-frame trace storm - and each sample
	// runs a handful of line traces per teammate.
	const float Now = GetServerTimeSeconds();
	if ((Now - TracePassStats::GStats.LastSampleTime) < 0.1f)
	{
		return;
	}
	TracePassStats::GStats.LastSampleTime = Now;

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	bool bAnyLegal = false;
	for (const ATraceCharacter* Candidate : Candidates)
	{
		if (Candidate == Carrier || !IsValid(Candidate))
		{
			continue;
		}
		// Only count TEAMMATES: an enemy failing "not an ally" says nothing about whether the pass
		// option should have shown up, and there are five of them per sample drowning the histogram.
		if (!AreAllies(Carrier->GetTeam(), Candidate->GetTeam()))
		{
			continue;
		}

		++TracePassStats::GStats.TeammatesConsidered;

		ETracePassRejectReason Code = ETracePassRejectReason::None;
		if (IsLegalPassTarget(Carrier, Candidate, /*bRequireAim=*/true, /*OutRejectReason=*/nullptr, &Code))
		{
			++TracePassStats::GStats.TeammatesLegal;
			bAnyLegal = true;
		}
		else
		{
			const int32 Index = FMath::Clamp(static_cast<int32>(Code), 0, 7);
			++TracePassStats::GStats.RefusalCount[Index];
		}
	}

	++TracePassStats::GStats.CarrySamples;
	if (bAnyLegal)
	{
		++TracePassStats::GStats.RawSamplesWithTarget;
	}

	// THE NUMBER THE BUG REPORT IS ABOUT. FindPassTargetFor() is what ATraceHUD polls to decide
	// whether to show the pass affordance and what the bots ask before committing a pass, so this -
	// not the raw rule above - is "did the pass option show up".
	if (FindPassTargetFor(Carrier) != nullptr)
	{
		++TracePassStats::GStats.SamplesWithTarget;
	}
	else
	{
		++TracePassStats::GStats.SamplesWithoutTarget;
	}

	const float Interval = CVarTracePassStatsInterval.GetValueOnAnyThread();
	if (Interval > 0.f)
	{
		if (TracePassStats::GStats.LastDumpTime <= 0.0)
		{
			TracePassStats::GStats.LastDumpTime = Now;
		}
		else if ((Now - TracePassStats::GStats.LastDumpTime) >= Interval)
		{
			TracePassStats::GStats.LastDumpTime = Now;
			TracePassStats::Dump();
		}
	}
}


// =================================================================================================
// Queries
// =================================================================================================

ATraceCharacter* ATraceCore::GetCarrier() const
{
	return Carrier;
}

bool ATraceCore::IsHeld() const
{
	return IsValid(Carrier);
}

ETraceTeam ATraceCore::GetHolderTeam() const
{
	return IsValid(Carrier) ? Carrier->GetTeam() : ETraceTeam::None;
}

FVector ATraceCore::GetHomeLocation() const
{
	// Resolved lazily: the GameMode may spawn the arena builder and the Core in either order.
	if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(GetWorld()))
	{
		return Arena->GetCoreSpawnLocation();
	}
	return SpawnHomeLocation;
}

// IsPickupLockedOutFor() IS DELETED. Zero callers, and it returned false unconditionally: there is
// no loose Core and nothing is ever picked up, so there was nothing to be locked out of.

float ATraceCore::GetServerTimeSeconds() const
{
	if (const UWorld* World = GetWorld())
	{
		// Already replicated and smoothed. Returns double on 5.3+, so narrow explicitly.
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			return static_cast<float>(GameState->GetServerWorldTimeSeconds());
		}
		return static_cast<float>(World->GetTimeSeconds());
	}
	return 0.f;
}

ATraceCore* ATraceCore::Get(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	if (const ATraceGameState* GameState = World->GetGameState<ATraceGameState>())
	{
		return GameState->Core;
	}
	return nullptr;
}

bool ATraceCore::IsShieldSuppressedFor(const AActor* Character)
{
	if (Character == nullptr)
	{
		return false;
	}

	const ATraceCore* Core = ATraceCore::Get(Character->GetWorld());

	// The whole risk beat, in one line: the shield is down for the holder, and only the holder, and
	// only for as long as their pass is being held.
	return Core != nullptr && Core->bPassActive && Core->Carrier == Character;
}

bool ATraceCore::IsCoreHolder(const AActor* Character)
{
	if (Character == nullptr)
	{
		return false;
	}

	const ATraceCore* Core = ATraceCore::Get(Character->GetWorld());
	return Core != nullptr && Core->Carrier == Character;
}

bool ATraceCore::IsTraceInvulnerableFor(const AActor* Character)
{
	// Identical condition to IsShieldSuppressedFor by design: §4 says the two flip "simultaneously",
	// so they are literally the same fact read twice rather than two pieces of state to keep in sync.
	return IsShieldSuppressedFor(Character);
}

bool ATraceCore::IsPassActive() const
{
	return bPassActive || bLocalPassPredicted;
}

ATraceCharacter* ATraceCore::GetEffectivePassTarget() const
{
	if (bPassActive)
	{
		return PassTarget;
	}
	return bLocalPassPredicted ? LocalPassPredictTarget.Get() : nullptr;
}

float ATraceCore::GetPassProgress() const
{
	const float Hold = FMath::Max(0.01f, UTraceSettings::Get().PassHoldSeconds);
	const float Now = GetServerTimeSeconds();

	// Server state wins the moment it exists; prediction only covers the round trip before it does.
	if (bPassActive)
	{
		return FMath::Clamp((Now - PassStartServerTime) / Hold, 0.f, 1.f);
	}
	if (bLocalPassPredicted)
	{
		return FMath::Clamp((Now - LocalPassPredictStartTime) / Hold, 0.f, 1.f);
	}
	return 0.f;
}

float ATraceCore::GetPassCooldownRemaining() const
{
	return FMath::Max(0.f, PassCooldownEndServerTime - GetServerTimeSeconds());
}


// =================================================================================================
// Pass: target acquisition
// =================================================================================================

void ATraceCore::GatherCharacters(TArray<ATraceCharacter*>& OutCharacters) const
{
	OutCharacters.Reset();

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (const ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
	{
		for (const TWeakObjectPtr<ATraceCharacter>& Weak : GameMode->GetTrackedCharacters())
		{
			if (ATraceCharacter* Character = Weak.Get())
			{
				OutCharacters.Add(Character);
			}
		}

		if (OutCharacters.Num() > 0)
		{
			return;
		}
	}

	// The GameMode list is the fast path and is authority-only; clients (which run this for local
	// pass prediction) always land here.
	for (TActorIterator<ATraceCharacter> It(World); It; ++It)
	{
		if (ATraceCharacter* Character = *It)
		{
			OutCharacters.Add(Character);
		}
	}
}

const TCHAR* TracePassStats::RefusalName(int32 Index)
{
	switch (static_cast<ETracePassRejectReason>(Index))
	{
	case ETracePassRejectReason::InvalidOrSelf:     return TEXT("invalid or self");
	case ETracePassRejectReason::Dead:              return TEXT("dead");
	case ETracePassRejectReason::NotAnAlly:         return TEXT("not an ally");
	case ETracePassRejectReason::OutOfRange:        return TEXT("out of range");
	case ETracePassRejectReason::NoLineOfSight:     return TEXT("no line of sight");
	case ETracePassRejectReason::Behind:            return TEXT("behind us");
	case ETracePassRejectReason::NotUnderCrosshair: return TEXT("not under crosshair");
	default:                                        return TEXT("legal");
	}
}

bool ATraceCore::IsLegalPassTarget(const ATraceCharacter* Holder, const ATraceCharacter* Candidate, bool bRequireAim,
	const TCHAR** OutRejectReason, ETracePassRejectReason* OutRejectCode) const
{
	// Every `return false` below names itself. Costs nothing when the caller does not ask (the
	// gameplay path passes null), and turns "no receiver found" — which is what a carrier who cannot
	// get rid of the Core actually experiences — into a specific broken test.
	//
	// The CODE is what the grace window reads: the state machine has to know whether the refusal was
	// a transient geometric blink (ride it out) or a fact about the receiver (cancel now). The string
	// stays for Trace.DebugPassTargets, derived from the code so the two can never disagree.
	const auto Reject = [OutRejectReason, OutRejectCode](ETracePassRejectReason Code) -> bool
	{
		if (OutRejectCode != nullptr)
		{
			*OutRejectCode = Code;
		}
		if (OutRejectReason != nullptr)
		{
			*OutRejectReason = TracePassStats::RefusalName(static_cast<int32>(Code));
		}
		return false;
	};

	if (OutRejectReason != nullptr)
	{
		*OutRejectReason = TEXT("legal");
	}
	if (OutRejectCode != nullptr)
	{
		*OutRejectCode = ETracePassRejectReason::None;
	}

	if (!IsValid(Holder) || !IsValid(Candidate) || Holder == Candidate)
	{
		return Reject(ETracePassRejectReason::InvalidOrSelf);
	}

	// §9.3 [ASSUMPTION]: you cannot pass to a dead or respawning teammate.
	if (!Candidate->IsAlive())
	{
		return Reject(ETracePassRejectReason::Dead);
	}

	if (!AreAllies(Holder->GetTeam(), Candidate->GetTeam()))
	{
		return Reject(ETracePassRejectReason::NotAnAlly);
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	const FVector ViewLocation = Holder->GetPawnViewLocation();
	const FVector TargetChest = Candidate->GetActorLocation()
		+ FVector(0.0, 0.0, static_cast<double>(Settings.PassTargetChestOffsetZ));

	if (FVector::DistSquared(ViewLocation, TargetChest) > FMath::Square(static_cast<double>(Settings.PassMaxRange)))
	{
		return Reject(ETracePassRejectReason::OutOfRange);
	}

	// Line of sight, against WORLD GEOMETRY ONLY - and "world geometry" means ECC_Visibility, not an
	// object-type query.
	//
	// THIS USED TO BE LineTraceTestByObjectType({WorldStatic, WorldDynamic}), AND THAT SILENTLY KILLED
	// EVERY PASS INVOLVING AN ENDZONE. An object-type query matches on the component's object type and
	// ignores its response channels entirely, so the ATraceEndzone trigger - a QueryOnly box whose
	// object type is ECC_WorldDynamic and which responds to nothing but ECC_Pawn - was a solid wall to
	// this one query and nothing else in the game. A carrier standing in a zone hit it immediately
	// (bFindInitialOverlaps defaults true), and a pass INTO a zone hit it on entry, so "complete a pass
	// to a teammate standing in the enemy endzone" could never fire. Widening the endzones to the full
	// field width made the dead area twice as large.
	//
	// ECC_Visibility is what the original comment actually wanted: arena geometry blocks it, the
	// endzone trigger ignores it by construction (TraceEndzone.cpp sets every channel to Ignore, then
	// opens ECC_Pawn alone), and the "Pawn" capsule profile ignores it too - so a teammate standing in
	// between still cannot invalidate the pass, which was the whole reason the object query was chosen.
	//
	// §4.1 FIX - PROBE MORE THAN ONE POINT. A single chest ray is a coin flip against the new cover
	// density: a receiver whose head and shoulders are plainly visible over a 1x (176 uu) block still
	// fails, because the one point being probed is the one point behind it. Any clear probe now means
	// line of sight, which is both more generous and much closer to what the passer can actually see.
	// The chest is probed first so the common case is still exactly one trace.
	const UWorld* World = GetWorld();
	if (World != nullptr)
	{
		FCollisionQueryParams QueryParams(FName(TEXT("TracePassLOS")), /*bTraceComplex=*/false);
		QueryParams.AddIgnoredActor(this);
		QueryParams.AddIgnoredActor(Holder);
		QueryParams.AddIgnoredActor(Candidate);

		const FVector CandidateLocation = Candidate->GetActorLocation();
		const int32 ProbeCount = TraceCoreTuning::ResolvedMultiPointLos(Settings)
			? static_cast<int32>(UE_ARRAY_COUNT(TraceCoreTuning::LosProbeOffsetsZ))
			: 1;

		bool bHaveLineOfSight = false;
		for (int32 ProbeIndex = 0; ProbeIndex < ProbeCount && !bHaveLineOfSight; ++ProbeIndex)
		{
			// Probe 0 is the chest and must be the SAME point the aim test and the range test used,
			// or the pass can be aimed at a spot line of sight was never checked against.
			const double ProbeZ = (ProbeIndex == 0)
				? static_cast<double>(Settings.PassTargetChestOffsetZ)
				: TraceCoreTuning::LosProbeOffsetsZ[ProbeIndex];
			const FVector ProbePoint = CandidateLocation + FVector(0.0, 0.0, ProbeZ);
			bHaveLineOfSight = !World->LineTraceTestByChannel(ViewLocation, ProbePoint, ECC_Visibility, QueryParams);
		}

		if (!bHaveLineOfSight)
		{
			return Reject(ETracePassRejectReason::NoLineOfSight);
		}
	}

	// --- "hover the crosshair over them", generously (see TraceCoreTuning) ----------------------

	if (!bRequireAim)
	{
		return true;
	}

	const FVector AimDirection = Holder->GetAimDirection();
	FVector ToTarget = TargetChest - ViewLocation;
	const double Distance = ToTarget.Size();
	if (Distance <= CoreGeometryEpsilon)
	{
		return true;   // Standing inside each other. Nothing sensible to measure; accept.
	}
	ToTarget /= Distance;

	const double Cosine = FVector::DotProduct(ToTarget, AimDirection);
	if (Cosine <= 0.0)
	{
		return Reject(ETracePassRejectReason::Behind);
	}

	// (a) angular cone - what makes a distant receiver acquirable at all.
	const double ConeDegrees = TraceCoreTuning::ResolvedAimConeDegrees(Settings);
	if (Cosine >= FMath::Cos(FMath::DegreesToRadians(ConeDegrees)))
	{
		return true;
	}

	// (b) the aim ray passing through the receiver's capsule - what makes a near receiver
	//     acquirable anywhere on their body, where the cone above has collapsed to nothing.
	double CapsuleRadius = 34.0;
	double CapsuleHalfHeight = 88.0;
	if (const UCapsuleComponent* Capsule = Candidate->GetCapsuleComponent())
	{
		CapsuleRadius = Capsule->GetScaledCapsuleRadius();
		CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	}

	const FVector RayEnd = ViewLocation + AimDirection * (Distance + CapsuleHalfHeight);
	const FVector CapsuleBottom = Candidate->GetActorLocation() - FVector(0.0, 0.0, CapsuleHalfHeight);
	const FVector CapsuleTop = Candidate->GetActorLocation() + FVector(0.0, 0.0, CapsuleHalfHeight);

	FVector ClosestOnRay = FVector::ZeroVector;
	FVector ClosestOnCapsule = FVector::ZeroVector;
	FMath::SegmentDistToSegmentSafe(ViewLocation, RayEnd, CapsuleBottom, CapsuleTop, ClosestOnRay, ClosestOnCapsule);

	const double Threshold = CapsuleRadius + TraceCoreTuning::ResolvedAimSlack(Settings);
	if (FVector::DistSquared(ClosestOnRay, ClosestOnCapsule) > (Threshold * Threshold))
	{
		return Reject(ETracePassRejectReason::NotUnderCrosshair);
	}

	return true;
}

ATraceCharacter* ATraceCore::FindPassTargetFor(const ATraceCharacter* Holder) const
{
	if (!IsValid(Holder) || !Holder->IsAlive())
	{
		return nullptr;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	const FVector ViewLocation = Holder->GetPawnViewLocation();
	const FVector AimDirection = Holder->GetAimDirection();

	ATraceCharacter* Best = nullptr;
	double BestCosine = -1.0;

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (!IsLegalPassTarget(Holder, Candidate))
		{
			continue;
		}

		// Among everyone who qualifies, take whoever is nearest the crosshair. With two teammates
		// overlapping on screen this is the one the player is obviously pointing at.
		const FVector TargetChest = Candidate->GetActorLocation()
			+ FVector(0.0, 0.0, static_cast<double>(Settings.PassTargetChestOffsetZ));
		const FVector ToTarget = (TargetChest - ViewLocation).GetSafeNormal();
		const double Cosine = FVector::DotProduct(ToTarget, AimDirection);

		if (Cosine > BestCosine)
		{
			BestCosine = Cosine;
			Best = Candidate;
		}
	}

	// --- §4.1 FIX: sticky acquisition ------------------------------------------------------------
	//
	// Without this the answer above flickers, and the flicker IS the reported bug. ATraceHUD polls
	// this at 20 Hz purely to decide whether to draw the receiver highlight, so a teammate who is
	// jogging past a lane rail makes the pass option itself blink in and out - "sometimes the pass
	// option doesn't show up". It also costs real passes: the button is only sampled when it is
	// pressed, so a press that lands on a flickered-out frame acquires nobody at all.
	//
	// Identity, team, life and range are re-tested every single time (bRequireAim false skips only
	// the aim cone, and the LOS probe inside is what the window is forgiving). So a receiver who dies
	// or is genuinely miles away is dropped immediately; only the two flickery geometric tests get
	// held over, and only for Trace.Pass.StickySeconds.
	const float StickyWindow = TraceCoreTuning::ResolvedAcquireSticky(Settings);
	const float Now = GetServerTimeSeconds();

	if (Best != nullptr)
	{
		StickyAcquireHolder = const_cast<ATraceCharacter*>(Holder);
		StickyAcquireTarget = Best;
		StickyAcquireServerTime = Now;
		return Best;
	}

	if (StickyWindow > 0.f
		&& StickyAcquireHolder.Get() == Holder
		&& (Now - StickyAcquireServerTime) <= StickyWindow)
	{
		if (ATraceCharacter* Held = StickyAcquireTarget.Get())
		{
			// Everything except the aim cone and line of sight, re-checked for real.
			if (IsValid(Held) && Held != Holder && Held->IsAlive()
				&& AreAllies(Holder->GetTeam(), Held->GetTeam())
				&& FVector::DistSquared(Holder->GetPawnViewLocation(),
					Held->GetActorLocation() + FVector(0.0, 0.0, static_cast<double>(Settings.PassTargetChestOffsetZ)))
					<= FMath::Square(static_cast<double>(Settings.PassMaxRange)))
			{
				if (HasAuthority() && CVarTracePassStats.GetValueOnAnyThread() != 0)
				{
					++TracePassStats::GStats.StickySaves;
				}
				return Held;
			}
		}
	}

	return nullptr;
}


// =================================================================================================
// Pass: input and state machine
// =================================================================================================

void ATraceCore::RequestPassInput(bool bPressed, ATraceCharacter* Requester)
{
	if (!IsValid(Requester))
	{
		return;
	}

	// bPassInputHeld is ONE latch shared by the whole match, so "safe to call on a non-holder" —
	// which this function used to claim — was wrong in both directions. Who is asking decides
	// whether the latch may move.
	if (bPressed)
	{
		// Only the living holder may arm it.
		//
		// IsValid(Carrier) FIRST, explicitly. The old form (`Requester != Carrier || !Carrier->...`)
		// was correct only because || short-circuits when Carrier is null and Requester is validated
		// non-null above — i.e. it was one clause reorder away from being a client crash. Stating the
		// null check is free and removes the trap.
		if (!IsValid(Carrier) || Requester != Carrier || !Carrier->IsAlive())
		{
			return;
		}
		PassInputInstigator = Requester;
	}
	else
	{
		// A RELEASE is honoured from the holder OR from whoever armed it, and the second case is the
		// one that matters: a completed pass moves the Core to the receiver BEFORE the player's
		// finger leaves the button, so the passer is no longer the holder when their release lands.
		// (Tracked per machine; nothing here is replicated.)
		if (Requester != Carrier && Requester != PassInputInstigator.Get())
		{
			return;
		}
		PassInputInstigator = nullptr;
	}

	// No "is anyone holding the Core" check here any more. A PRESS already implies one — it is only
	// reached when Requester is the living Carrier. A RELEASE must go through even when nobody is
	// holding the Core (one arriving during a kickoff window), because a release that is dropped is
	// exactly what leaves the latch set.

	if (HasAuthority())
	{
		bPassInputHeld = bPressed;
		// Resolve on the same frame the button was pressed rather than waiting a tick: the pass
		// window is the moment the shield drops, and a frame of latency on that is a frame of free
		// invulnerability.
		ServerTickPass(0.f);
		return;
	}

	// --- Owning client: predict, then tell the server. -----------------------------------------
	//
	// The prediction is presentation only (the HUD ring, the receiver highlight). It deliberately
	// does NOT predict the shield drop or the trace hardening: those are damage rules, they are
	// resolved on the server, and a client that mispredicted them would be showing itself a
	// safety it does not have.
	// Requester, not Carrier: the release half of this runs after a completed pass has already moved
	// Carrier to the receiver, and it is OUR prediction that has to be unwound, not theirs.
	if (Requester->IsLocallyControlled())
	{
		if (bPressed)
		{
			if (GetPassCooldownRemaining() <= 0.f)
			{
				if (ATraceCharacter* Predicted = FindPassTargetFor(Carrier))
				{
					bLocalPassPredicted = true;
					LocalPassPredictStartTime = GetServerTimeSeconds();
					LocalPassPredictTarget = Predicted;
				}
			}
		}
		else
		{
			bLocalPassPredicted = false;
			LocalPassPredictTarget = nullptr;
		}
	}

	ServerSetPassInput(bPressed, Requester);
}

void ATraceCore::ServerSetPassInput_Implementation(bool bPressed, ATraceCharacter* Requester)
{
	// Network input: this RPC is routed by ownership (SetOwner(Carrier) in GrantTo), so only the
	// holding connection can reach it at all. It is re-validated anyway, by running the SAME
	// function the client ran - on the server HasAuthority() is true, so this lands in the branch
	// above and applies the identical press/release ownership rules. One copy of the rules, and a
	// client that lies about Requester is refused by them exactly as a local call would be.
	RequestPassInput(bPressed, Requester);
}

void ATraceCore::ServerTickPass(float /*DeltaSeconds*/)
{
	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	const bool bCollectPassStats = (CVarTracePassStats.GetValueOnAnyThread() != 0);

	if (!IsValid(Carrier) || !Carrier->IsAlive())
	{
		if (bPassActive && bCollectPassStats)
		{
			++TracePassStats::GStats.CancelHolderGone;
		}
		CancelPass(TEXT("holder gone"));
		ClearPassInput();
		return;
	}

	// --- An active pass: validate every frame, then complete on time. --------------------------
	if (bPassActive)
	{
		if (!bPassInputHeld)
		{
			// §4 [ASSUMPTION]: releasing early cancels, with the same instant restoration.
			if (bCollectPassStats)
			{
				++TracePassStats::GStats.CancelReleased;
			}
			CancelPass(TEXT("released"));
			return;
		}

		// Bots have no hands. Hold their crosshair on the receiver first, so what is validated below
		// is the aim an AI holder is actually being given.
		DriveBotAimAtPassTarget();

		// "Looking away cancels." Re-tested from the SERVER's copy of the holder's aim, never from
		// anything the client sent - the shield is down for as long as this stays true, so a client
		// that could assert it would be asserting its own invulnerability window.
		const AController* HolderController = Carrier->GetController();
		const bool bHolderIsAI = (HolderController != nullptr) && !HolderController->IsPlayerController();

		ETracePassRejectReason RejectCode = ETracePassRejectReason::None;
		const bool bStillLegal = IsLegalPassTarget(Carrier, PassTarget, /*bRequireAim=*/!bHolderIsAI,
			/*OutRejectReason=*/nullptr, &RejectCode);

		// --- §4.1 FIX: LINE-OF-SIGHT / AIM GRACE ------------------------------------------------
		//
		// The measured failure this exists for: a pass cancelled 24 ms before it would have completed
		// because the receiver crossed behind a lane rail. Both remaining tests here are sampled once
		// per frame against geometry, and a receiver who is RUNNING - which is every receiver worth
		// passing to - blinks through them repeatedly over a 0.5 s hold. One frame of occlusion must
		// not be the end of the pass.
		//
		// Only the transient geometric refusals are graced. A receiver who dies, who is no longer an
		// ally or who has been destroyed cancels on the very frame it happens, exactly as before,
		// because granting the Core to a corpse is a real bug and a rail is not.
		const float GraceSeconds = TraceCoreTuning::ResolvedValidationGrace(UTraceSettings::Get());

		if (bStillLegal)
		{
			if (PassGraceStartServerTime > 0.f)
			{
				// Rode it out. This is the counter that says how often the fix earns its keep.
				if (bCollectPassStats)
				{
					++TracePassStats::GStats.GraceSaves;
					TracePassStats::GStats.GraceSecondsAccrued += FMath::Max(0.f, Now - PassGraceStartServerTime);
				}
				PassGraceStartServerTime = 0.f;
			}
		}
		else
		{
			const bool bTransient = IsTransientPassRejection(RejectCode);
			if (!bTransient || GraceSeconds <= 0.f)
			{
				if (bCollectPassStats)
				{
					++TracePassStats::GStats.CancelLostTarget;
					if (bTransient && PassGraceStartServerTime > 0.f)
					{
						++TracePassStats::GStats.GraceExpired;
					}
				}
				PassGraceStartServerTime = 0.f;
				CancelPass(TEXT("looked away or target invalid"));
				return;
			}

			if (PassGraceStartServerTime <= 0.f)
			{
				PassGraceStartServerTime = Now;
			}
			else if ((Now - PassGraceStartServerTime) > GraceSeconds)
			{
				if (bCollectPassStats)
				{
					++TracePassStats::GStats.CancelLostTarget;
					++TracePassStats::GStats.GraceExpired;
					TracePassStats::GStats.GraceSecondsAccrued += GraceSeconds;
				}
				PassGraceStartServerTime = 0.f;
				CancelPass(TEXT("receiver stayed out of sight past the grace window"));
				return;
			}
			// Inside the grace window: fall through and keep the pass alive, including completing it
			// if the timer runs out here. Completing during a blink of occlusion is correct - the
			// player held a legal receiver for the full 0.5 s and a rail passing across them at the
			// last moment is not a decision they made.
		}

		if ((Now - PassStartServerTime) >= FMath::Max(0.05f, UTraceSettings::Get().PassHoldSeconds))
		{
			ATraceCharacter* Receiver = PassTarget;

			UE_LOG(LogTraceGame, Log, TEXT("Core: pass completed %s -> %s"),
				*GetNameSafe(Carrier), *GetNameSafe(Receiver));

			// Cooldown lands on the passer's side of the transfer, but the Core is about to change
			// hands so it is really only meaningful if the pass is somehow refused below.
			PassCooldownEndServerTime = Now + FMath::Max(0.f, UTraceSettings::Get().PassCooldownSeconds);

			if (bCollectPassStats)
			{
				++TracePassStats::GStats.PassesCompleted;
			}

			CancelPass(nullptr);          // Silent: this is a completion, not an abort.
			GrantTo(Receiver, ETraceCoreGrantReason::Pass);
		}
		return;
	}

	// --- No active pass: start one if the button is down and everything lines up. --------------
	if (!bPassInputHeld)
	{
		return;
	}

	if (Now < PassCooldownEndServerTime)
	{
		return;
	}

	if (ATraceCharacter* Target = FindPassTargetFor(Carrier))
	{
		BeginPass(Target);
	}
}

void ATraceCore::BeginPass(ATraceCharacter* Target)
{
	if (!HasAuthority() || !IsValid(Target) || bPassActive)
	{
		return;
	}

	bPassActive = true;
	PassTarget = Target;
	PassStartServerTime = GetServerTimeSeconds();
	PassGraceStartServerTime = 0.f;

	if (CVarTracePassStats.GetValueOnAnyThread() != 0)
	{
		++TracePassStats::GStats.PassesBegun;
	}

	// THE RISK BEAT. Both halves of §4 happen right here, on one frame, from one bool:
	//   - the trace hardens (ApplyTraceInvulnerability, read back by UTraceTrailComponent), and
	//   - the shield drops (nothing to do: UTraceHealthComponent reads IsShieldSuppressedFor()).
	ApplyTraceInvulnerability();
	UpdateVisuals();

	// The pass window is 0.5s long and it decides whether the holder can be shot. It does not wait
	// for the next scheduled net update.
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Verbose, TEXT("Core: pass started %s -> %s (shield down, trace invulnerable)"),
		*GetNameSafe(Carrier), *GetNameSafe(Target));
}

void ATraceCore::CancelPass(const TCHAR* Reason)
{
	if (!HasAuthority())
	{
		return;
	}

	// THIS USED TO START WITH `bPassInputHeld = false;` AND THAT WAS THE BUG.
	//
	// bPassInputHeld is not pass state. It is a mirror of a PHYSICAL BUTTON, and only the player who
	// is holding that button (or losing the pawn that owns it) may change it. Clearing it here meant
	// that any cancel - a receiver stepping behind a lane rail for one frame, the crosshair drifting
	// off for one frame - silently disarmed a mouse button the player was still holding down. From
	// then on ServerTickPass hit `if (!bPassInputHeld) return;` and refused to acquire ANYBODY, no
	// matter how perfectly the crosshair sat on a teammate, until the player physically let go and
	// pressed again. Measured: a pass cancelled 24ms before it would have completed, then six
	// further seconds of holding the button with a legal receiver on the crosshair and nothing
	// happening. The player is left holding the Core - and the camera is `!bIsCarrier` and nothing
	// else, so "the Core will not leave" and "I am stuck in third person" are the same event.
	//
	// Churn is already prevented, and by the right mechanism: a cancel with a Reason spends
	// PassCooldownSeconds below, so a held button re-acquires on a cooldown rather than every frame.
	// The latch is cleared where it actually belongs - on a release, and on a possession change
	// (ClearPassInput, called from ReleaseHolder and GrantTo).

	if (!bPassActive)
	{
		return;
	}

	if (Reason != nullptr)
	{
		// Display, unlike its "pass started" counterpart. A cancel that the player did not ask for is
		// the failure signature of this whole area — it is what leaves them holding the Core, and
		// therefore stuck in third person — and it names its own reason. At most a few lines a
		// minute, since only one player can be passing at a time.
		UE_LOG(LogTraceGame, Display, TEXT("Core: pass cancelled (%s) - shield restored, trace vulnerable"), Reason);

		// A cancelled attempt still spends A cooldown. Without one, tapping the button is a free way
		// to churn the shield state (and the packets that carry it) every frame.
		//
		// §4.1 FIX: it is no longer the SAME cooldown a completed pass spends. Two full seconds of
		// lockout after an unwanted cancel is a large part of "passing is inconsistent": the player
		// holds mouse1 on a teammate, watches the ring fill, watches it die because the receiver
		// clipped a rail, and then gets nothing for two more seconds while still holding the button
		// on a perfectly legal target. 0.25 s still bounds the churn to one cycle per
		// (grace + cooldown) = 0.4 s, which is far below anything a player experiences as a lockout,
		// and it is what "make re-acquisition generous after a cancel" means in practice.
		PassCooldownEndServerTime = GetServerTimeSeconds()
			+ TraceCoreTuning::ResolvedCancelCooldown(UTraceSettings::Get());
	}

	bPassActive = false;
	PassTarget = nullptr;
	PassGraceStartServerTime = 0.f;

	// Instant restoration, both halves together (§9.2: the implemented answer is "instant").
	ApplyTraceInvulnerability();
	UpdateVisuals();
	ForceNetUpdate();
}

void ATraceCore::ClearPassInput()
{
	// The one legitimate reason to forget a held button without hearing a release: the pawn that was
	// holding it is not the holder any more. Whoever has the Core now has not pressed anything, and
	// a latch inherited from the previous holder would start a pass they never asked for.
	bPassInputHeld = false;
	PassInputInstigator = nullptr;
}

void ATraceCore::DriveBotAimAtPassTarget()
{
	if (!bPassActive || !IsValid(Carrier) || !IsValid(PassTarget))
	{
		return;
	}

	AController* HolderController = Carrier->GetController();
	if (HolderController == nullptr || HolderController->IsPlayerController())
	{
		return;   // A human holds their own crosshair; that is the mechanic.
	}

	const FVector ViewLocation = Carrier->GetPawnViewLocation();
	const FVector TargetChest = PassTarget->GetActorLocation()
		+ FVector(0.0, 0.0, static_cast<double>(UTraceSettings::Get().PassTargetChestOffsetZ));

	const FVector ToTarget = TargetChest - ViewLocation;
	if (!ToTarget.IsNearlyZero())
	{
		// Snapping is fine: a carrying pawn is on orient-to-movement, so its body does not follow
		// the control rotation and nothing visibly jerks.
		HolderController->SetControlRotation(ToTarget.Rotation());
	}
}

void ATraceCore::ApplyTraceInvulnerability()
{
	// Nothing to push: UTraceTrailComponent asks ATraceCore::IsTraceInvulnerableFor() directly, so
	// there is exactly one copy of the fact and it is the replicated one. This hook exists as the
	// single named place the rule is applied, and to force a visual refresh on the trail so the
	// hardening is legible the frame it happens.
	if (IsValid(Carrier) && Carrier->Trail != nullptr)
	{
		Carrier->Trail->NotifyInvulnerabilityChanged();
	}
}


// =================================================================================================
// Transfers
// =================================================================================================

void ATraceCore::GrantTo(ATraceCharacter* NewHolder, ETraceCoreGrantReason Reason)
{
	if (!HasAuthority())
	{
		return;
	}

	if (!IsValid(NewHolder) || !NewHolder->IsAlive())
	{
		return;
	}

	if (NewHolder == Carrier)
	{
		return;
	}

	ATraceCharacter* Previous = Carrier;
	const ETraceTeam PreviousTeam = IsValid(Previous) ? Previous->GetTeam() : ETraceTeam::None;
	const ETraceTeam NewTeam = NewHolder->GetTeam();

	// §2 [ASSUMPTION]: the 1s grace is a TEAM change rule. A completed teammate pass reads as
	// continuous possession, so the trace keeps forming without a gap; anything that takes the Core
	// across to the other side buys that side a second before their trace exists.
	const bool bTeamChanged = !AreAllies(PreviousTeam, NewTeam);

	CancelPass(nullptr);
	ReleaseHolder();

	// Explicit, and no longer a side effect of CancelPass: the incoming holder inherits no button.
	ClearPassInput();

	Carrier = NewHolder;
	PendingGrantTeam = ETraceTeam::None;
	bFallbackQueued = false;
	bOutOfPlay = false;

	// Ownership is what lets the holding CLIENT send ServerSetPassInput() to this actor. Without it
	// the RPC is silently dropped by the net driver as "not owned by that connection".
	SetOwner(NewHolder);

	// The receiver may not pass back instantly; the cooldown belongs to whoever holds the Core.
	PassCooldownEndServerTime = GetServerTimeSeconds()
		+ ((Reason == ETraceCoreGrantReason::Pass)
			? FMath::Max(0.f, UTraceSettings::Get().PassCooldownSeconds)
			: 0.f);

	ApplyAttachment();

	// Grace BEFORE SetCarrying: SetCarrying(true) starts the trail emitting, and the trail must
	// already know it is not allowed to lay a point yet.
	if (NewHolder->Trail != nullptr)
	{
		// Spec v3 section 1: 1.0 -> 0.4 s, and now a UTraceSettings knob rather than a constant.
		NewHolder->Trail->SetEmitGrace(bTeamChanged
			? FMath::Max(0.f, UTraceSettings::Get().CoreTurnoverGraceSeconds) : 0.f);
	}

	// SetCarrying() is the SOLE writer of ATracePlayerState::bIsCarrier — it sets the flag and
	// force-updates it. This used to write the mirror a second time right here, which is the
	// duplicated-state shape the audit flagged.
	NewHolder->SetCarrying(true);

	// Cache the holder's PlayerState. Not an optimisation: the PlayerState OUTLIVES the pawn, and
	// ReleaseHolder() has a path where the pawn is already invalid by the time it runs. Without a
	// handle taken while the pawn was alive there is nothing left to clear the mirror through, and a
	// phantom carrier sticks on the scoreboard for the rest of the match. See ReleaseHolder().
	HolderPlayerState = NewHolder->GetPlayerState<ATracePlayerState>();

	if (NewHolder->Trail != nullptr)
	{
		NewHolder->Trail->SetEmitting(true);
	}

	// The kill path: whoever kills the holder gets the Core, and this binding is how we learn about
	// it WITH the killer attached. The GameMode's own death handling cannot tell us that - it calls
	// the location-based legacy DropAt() - so the Core listens to the health component directly.
	if (NewHolder->Health != nullptr)
	{
		NewHolder->Health->OnDeath.AddUniqueDynamic(this, &ATraceCore::OnHolderDeath);
		BoundDeathHolder = NewHolder;
	}

	OnRep_Carrier();
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Log, TEXT("Core granted to %s (reason %d, team change %s)"),
		*GetNameSafe(NewHolder), static_cast<int32>(Reason), bTeamChanged ? TEXT("yes") : TEXT("no"));

	// A pass completed to a teammate who is ALREADY STANDING IN THE ENEMY ENDZONE scores, and so
	// does a Core taken from a carrier you killed while standing in it yourself. Neither generates
	// a begin-overlap — the receiver never moved — so the endzone test has to be driven off the
	// POSSESSION change, which is this function, rather than off the movement that usually causes
	// it. The GameMode owns the test; it reads the endzone volume itself, so this stays correct
	// however the zones are sized.
	//
	// LAST STATEMENT IN THE FUNCTION, deliberately: a score resets the field (kickoff, every pawn
	// teleported, this Core released again), so nothing here may touch member state afterwards.
	if (UWorld* World = GetWorld())
	{
		if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
		{
			// Ordered to match ETraceCoreGrantReason exactly; clamped so adding a reason without
			// touching this cannot read off the end.
			static const TCHAR* GrantReasonNames[] =
			{
				TEXT("kickoff"), TEXT("completed pass"), TEXT("kill steal"), TEXT("fallback"), TEXT("debug grant")
			};
			const int32 ReasonIndex = FMath::Clamp(static_cast<int32>(Reason), 0,
				static_cast<int32>(UE_ARRAY_COUNT(GrantReasonNames)) - 1);

			GameMode->CheckEndzoneScoreForCarrier(NewHolder, GrantReasonNames[ReasonIndex]);
		}
	}
}

void ATraceCore::ReleaseHolder()
{
	if (!HasAuthority())
	{
		return;
	}

	ATraceCharacter* Previous = Carrier;

	if (ATraceCharacter* Bound = BoundDeathHolder.Get())
	{
		if (Bound->Health != nullptr)
		{
			Bound->Health->OnDeath.RemoveDynamic(this, &ATraceCore::OnHolderDeath);
		}
	}
	BoundDeathHolder = nullptr;

	Carrier = nullptr;
	SetOwner(nullptr);

	// Possession is gone, so the held-button state that belonged to it is gone too. Every path that
	// takes the Core off somebody funnels through here - a completed pass, a kill, a disconnect, a
	// score, half time - which is why this is the one place it needs saying.
	ClearPassInput();

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

	// CLEAR THE MIRROR BEFORE THE EARLY-OUT, through the handle cached in GrantTo.
	//
	// The bug this fixes: ATracePlayerState OUTLIVES the pawn. If the holder's pawn became invalid
	// before release — which Tick() step 2 exists specifically to handle, and which a GC'd pawn or a
	// level teardown also produces — the `!IsValid(Previous)` return below fired first and the mirror
	// was never cleared. Nothing else could clear it either: ATracePlayerState::CopyProperties
	// deliberately does not carry bIsCarrier across, and SetCarrying() early-outs when the pawn's own
	// flag already matches, so a fresh pawn (default false) never writes it. The result was a
	// permanent phantom carrier on the scoreboard (ATraceHUD reads Member->bIsCarrier) for the rest
	// of the match.
	if (ATracePlayerState* CachedState = HolderPlayerState.Get())
	{
		CachedState->bIsCarrier = false;
		CachedState->ForceNetUpdate();
	}
	HolderPlayerState = nullptr;

	if (!IsValid(Previous))
	{
		return;
	}

	// See the declaration: this is what lets OnHolderDeath still recognise the death that is
	// currently unwinding, after the GameMode's own death path has already released the holder.
	RecentlyReleasedHolder = Previous;

	// Sole writer of the mirror for the live-pawn case (see ATraceCharacter::SetCarrying). The
	// explicit PlayerState write that used to sit here is gone with the other duplicates.
	Previous->SetCarrying(false);

	// Stop laying, but do NOT wipe what is already there: an expiring trace is counterplay the enemy
	// team has already earned, and popping it out of existence reads worse than letting it fade.
	// Death is the one case that clears instantly, and ATraceCharacter::HandleDeath does that.
	if (Previous->Trail != nullptr)
	{
		Previous->Trail->SetEmitting(false);
	}
}

void ATraceCore::KickoffTo(ETraceTeam ReceivingTeam)
{
	if (!HasAuthority())
	{
		return;
	}

	CancelPass(nullptr);
	ReleaseHolder();

	// None means OUT OF PLAY, and it must NOT be quietly rewritten into a real team: the half-time
	// interval and the post-match screen both need the Core parked with nobody holding it, and
	// ATraceGameMode calls this exact function to say so.
	bOutOfPlay = (ReceivingTeam == ETraceTeam::None);
	PendingGrantTeam = ReceivingTeam;
	PendingGrantTime = GetServerTimeSeconds() + TraceCoreTuning::KickoffDelaySeconds;

	SetActorLocation(GetHomeLocation(), false, nullptr, ETeleportType::TeleportPhysics);
	SetActorRotation(FRotator::ZeroRotator);

	OnRep_Carrier();
	ForceNetUpdate();

	if (bOutOfPlay)
	{
		UE_LOG(LogTraceGame, Log, TEXT("Core: parked out of play (no holder)."));
	}
	else
	{
		UE_LOG(LogTraceGame, Log, TEXT("Core: kickoff queued for %s in %.1fs"),
			*TraceTeamName(PendingGrantTeam).ToString(), TraceCoreTuning::KickoffDelaySeconds);
	}
}

void ATraceCore::ResolveFallback(ETraceTeam LostTeam)
{
	if (!HasAuthority())
	{
		return;
	}

	const ETraceTeam ReceivingTeam = TraceOpposingTeam(LostTeam);

	// A death is play, so whatever "out of play" state a previous interval left behind is over.
	bOutOfPlay = false;

	// §2 [ASSUMPTION]: no attributable enemy killer -> nearest living enemy gets it.
	if (ReceivingTeam != ETraceTeam::None)
	{
		PendingGrantTeam = ReceivingTeam;
		PendingGrantTime = GetServerTimeSeconds();
		if (TryResolvePendingGrant())
		{
			return;
		}

		// Every enemy is on a respawn timer. The Core waits for them rather than vanishing; Tick
		// retries until somebody on that side is alive.
		UE_LOG(LogTraceGame, Log, TEXT("Core: no living %s player to receive it; holding for their next spawn."),
			*TraceTeamName(ReceivingTeam).ToString());
		return;
	}

	// The previous holder had no team at all (a spectator-slot pawn, or the Core was already
	// holderless). Fall back to the default kickoff rather than leaving it stranded.
	PendingGrantTeam = TraceCoreTuning::DefaultKickoffTeam;
	PendingGrantTime = GetServerTimeSeconds();
}

bool ATraceCore::TryResolvePendingGrant()
{
	if (!HasAuthority() || PendingGrantTeam == ETraceTeam::None || IsValid(Carrier))
	{
		return false;
	}

	TArray<ATraceCharacter*> Candidates;
	GatherCharacters(Candidates);

	const FVector Reference = GetActorLocation();
	ATraceCharacter* Best = nullptr;
	double BestDistSq = TNumericLimits<double>::Max();

	for (ATraceCharacter* Candidate : Candidates)
	{
		if (!IsValid(Candidate) || !Candidate->IsAlive() || Candidate->GetTeam() != PendingGrantTeam)
		{
			continue;
		}

		const double DistSq = FVector::DistSquared(Reference, Candidate->GetActorLocation());
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Candidate;
		}
	}

	if (Best == nullptr)
	{
		return false;
	}

	GrantTo(Best, ETraceCoreGrantReason::Kickoff);
	return true;
}

void ATraceCore::OnHolderDeath(AActor* Victim, AController* Killer, FName Cause)
{
	if (!HasAuthority())
	{
		return;
	}

	ATraceCharacter* VictimCharacter = Cast<ATraceCharacter>(Victim);
	if (!IsValid(VictimCharacter))
	{
		return;
	}

	// The victim must be the holder, or the holder the GameMode's death path released moments ago
	// (see RecentlyReleasedHolder). The `Carrier == nullptr` requirement is what makes the second
	// case safe: if somebody else has already been given the Core, this death cannot move it.
	const bool bWasHolder = (VictimCharacter == Carrier)
		|| (Carrier == nullptr && VictimCharacter == RecentlyReleasedHolder.Get());

	if (!bWasHolder)
	{
		return;
	}

	const ETraceTeam LostTeam = VictimCharacter->GetTeam();

	// Whoever killed the holder takes the Core. This one branch covers all three §2 cases:
	//   - "breaks your trace":   UTraceTrailComponent kills through the tripper's controller.
	//   - "kills the carrier":   any legal bullet, once the shield is down.
	//   - "intercepts the core": which, with no physics, means killing them mid-pass (§2 note).
	ATraceCharacter* KillerCharacter = (Killer != nullptr) ? Cast<ATraceCharacter>(Killer->GetPawn()) : nullptr;

	// This death is being resolved authoritatively here and now, so the coarse fallback the
	// GameMode's DropAt() queued a moment ago must not also fire on the next tick.
	bFallbackQueued = false;

	ReleaseHolder();

	if (IsValid(KillerCharacter)
		&& KillerCharacter->IsAlive()
		&& AreEnemies(LostTeam, KillerCharacter->GetTeam()))
	{
		UE_LOG(LogTraceGame, Log, TEXT("Core: %s killed the holder (%s) and takes the Core."),
			*GetNameSafe(KillerCharacter), *Cause.ToString());

		GrantTo(KillerCharacter, ETraceCoreGrantReason::Kill);
		return;
	}

	// Suicide, fall damage, a team kill, or a killer who died in the same exchange.
	ResolveFallback(LostTeam);
}


// =================================================================================================
// Legacy shims (see the file header)
// =================================================================================================

void ATraceCore::TryPickup(ATraceCharacter* Character)
{
	if (!HasAuthority() || !IsValid(Character))
	{
		return;
	}

	// Under the status model there is no pickup: the Core is essentially always held, so a version
	// of this that refused whenever somebody had it would refuse every time and Trace.DebugTakeCore
	// — its only remaining caller, and the tool used to inspect the carrier's view and their trace —
	// would never fire. So it is an unconditional debug grant, and it takes the Core off whoever has
	// it. Nothing in the shipping game reaches this.
	if (IsValid(Carrier))
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("Core: debug grant to %s takes it from %s."),
			*GetNameSafe(Character), *GetNameSafe(Carrier));
	}

	GrantTo(Character, ETraceCoreGrantReason::Debug);
}

// Throw() IS DELETED. It was the legacy pass entry point, documented as being reached from
// ATraceCharacter::PerformPass "and therefore the bots' DoPass()" — a map to a road that no longer
// existed: PerformPass called RequestPassInput directly and never went through here, and the bots go
// through ApplyPassInput -> DoPassPressed -> RequestPassInput. Zero callers, and all three of the
// functions it named are deleted too. The one door is RequestPassInput().

void ATraceCore::DropAt(const FVector& /*Location*/, const FVector& /*Impulse*/)
{
	if (!HasAuthority() || !IsValid(Carrier))
	{
		return;
	}

	// The holder lost the Core with nobody credited: a disconnect (ATraceGameMode::Logout), or the
	// GameMode's death path, which runs before our own OnHolderDeath listener knows the killer.
	//
	// QUEUED, not resolved, precisely because of that second caller - see the note in Tick(). If a
	// killer does exist, OnHolderDeath fires later in the same broadcast, transfers the Core and
	// clears this flag, so the death produces exactly one transfer.
	FallbackTeam = Carrier->GetTeam();
	bFallbackQueued = true;

	ReleaseHolder();
	ForceNetUpdate();
}

// ResetToCenter() IS DELETED. Zero callers — every score, match start and half-time reset calls
// KickoffTo() directly, which is the entry point this merely wrapped.

// =================================================================================================
// Presentation
// =================================================================================================

void ATraceCore::ApplyAttachment()
{
	AppliedHolder = Carrier;
	bAppliedPassActive = bPassActive;
	bAppliedEver = true;

	if (IsValid(Carrier))
	{
		if (GetAttachParentActor() != Carrier)
		{
			AttachToActor(Carrier, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		}

		// Straight up the capsule axis, so the holder's yaw cannot swing the orb around and the
		// beacon is a true vertical wherever they are facing.
		SetActorRelativeLocation(FVector(0.0, 0.0, TraceCoreTuning::OrbHeight));
		SetActorRelativeRotation(FRotator::ZeroRotator);
	}
	else
	{
		if (GetAttachParentActor() != nullptr)
		{
			DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}
	}

	// The beacon is placed in the actor's own space, once, from its two end heights.
	if (Beacon != nullptr && Beacon->GetStaticMesh() != nullptr)
	{
		const FBoxSphereBounds Bounds = Beacon->GetStaticMesh()->GetBounds();
		const double MeshHeight = FMath::Max(1.0, 2.0 * Bounds.BoxExtent.Z);
		const double MeshWidth = FMath::Max(1.0, 2.0 * Bounds.BoxExtent.X);

		const double Height = TraceCoreTuning::BeaconTop - TraceCoreTuning::BeaconBottom;
		const double Centre = (TraceCoreTuning::BeaconTop + TraceCoreTuning::BeaconBottom) * 0.5
			- TraceCoreTuning::OrbHeight;   // Relative to the actor, which sits at OrbHeight.

		Beacon->SetRelativeLocation(FVector(0.0, 0.0, Centre));
		Beacon->SetRelativeScale3D(FVector(
			TraceCoreTuning::BeaconWidth / MeshWidth,
			TraceCoreTuning::BeaconWidth / MeshWidth,
			Height / MeshHeight));

		// Only shown while somebody is holding it: a holderless Core is a kickoff, not a target.
		Beacon->SetVisibility(IsValid(Carrier));
	}

	// FPrimitiveSceneProxy caches the actor owner chain when it is BUILT, and that chain is what
	// bOwnerNoSee is resolved against. The chain changes every time the Core changes hands (GrantTo
	// calls SetOwner), and SetOwnerNoSee(true) would early-out because the flag itself is unchanged
	// - so the proxy has to be rebuilt explicitly or the previous holder would keep the Core hidden
	// from themselves while the new one stared straight at it.
	if (Mesh != nullptr)
	{
		Mesh->MarkRenderStateDirty();
	}
	if (Beacon != nullptr)
	{
		Beacon->MarkRenderStateDirty();
	}
}

void ATraceCore::UpdateVisuals()
{
	FLinearColor Color = TraceTeamColor(GetHolderTeam());
	Color.A = 1.f;

	const bool bColorChanged = !bColorApplied || !Color.Equals(AppliedColor, 0.001f);
	AppliedColor = Color;
	bColorApplied = true;

	// The pass window is the one moment an enemy can actually shoot the holder. Making the orb
	// visibly hotter for exactly those 0.5s is the read that turns the risk beat into something a
	// defender can act on rather than something only the passer knows about.
	const float GlowScale = bPassActive ? TraceCoreTuning::PassGlowMultiplier : 1.f;

	auto Push = [this, &Color, GlowScale, bColorChanged](UMaterialInstanceDynamic* Material, float BaseGlow)
	{
		if (Material == nullptr)
		{
			return;
		}
		if (bColorChanged)
		{
			Material->SetVectorParameterValue(TEXT("Color"), Color);
			Material->SetVectorParameterValue(TEXT("BaseColor"), Color);   // No-op if absent.
		}
		if (bMaterialIsNeon)
		{
			Material->SetScalarParameterValue(TEXT("Glow"), BaseGlow * GlowScale);
		}
	};

	Push(MeshMID, TraceCoreTuning::OrbGlow);
	Push(BeaconMID, TraceCoreTuning::BeaconGlow);
}

void ATraceCore::EnforceHolderTrailState()
{
	// Self-heal. Several foreign systems switch trails off wholesale - the GameMode clears every
	// player's trail on every score (ResetPlayersToSpawns) and on every death - and under the status
	// model there is no pickup event left to switch the holder's trail back on. So the Core, which
	// is the thing that knows who the holder is, re-asserts it every tick. SetEmitting() early-outs
	// when the state is unchanged, so in the normal case this costs one bool compare.
	if (!IsValid(Carrier) || !Carrier->IsAlive() || Carrier->Trail == nullptr)
	{
		return;
	}

	if (!Carrier->Trail->IsEmitting())
	{
		Carrier->Trail->SetEmitting(true);
	}
}

void ATraceCore::OnRep_Carrier()
{
	ApplyAttachment();
	UpdateVisuals();

	// Server truth about who holds it supersedes any local pass prediction.
	bLocalPassPredicted = false;
	LocalPassPredictTarget = nullptr;
}

void ATraceCore::OnRep_Owner()
{
	Super::OnRep_Owner();

	if (Mesh != nullptr)
	{
		Mesh->MarkRenderStateDirty();
	}
	if (Beacon != nullptr)
	{
		Beacon->MarkRenderStateDirty();
	}
}

void ATraceCore::OnRep_PassState()
{
	// The authoritative answer has arrived; stop predicting either way.
	bLocalPassPredicted = false;
	LocalPassPredictTarget = nullptr;

	UpdateVisuals();

	if (IsValid(Carrier) && Carrier->Trail != nullptr)
	{
		Carrier->Trail->NotifyInvulnerabilityChanged();
	}
}
