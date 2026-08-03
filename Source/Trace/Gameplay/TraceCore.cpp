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
	// Spec §2/§4 numbers.
	//
	// These are compile-time constants rather than UTraceSettings entries only because
	// TraceSettings.h is owned by another slice this pass. Every one of them is a designer knob and
	// they should be promoted to UTraceSettings (Category = "Core") verbatim - the report names them.
	// ---------------------------------------------------------------------------------------------

	/** §4: hold the crosshair on the receiver this long to complete the transfer. */
	constexpr float PassHoldSeconds = 0.5f;

	/** §4: before another pass may be STARTED. Applied on completion and on cancellation alike. */
	constexpr float PassCooldownSeconds = 2.0f;

	/** §2: after the Core changes TEAM, the trace does not begin forming for this long. */
	constexpr float TransferGraceSeconds = 1.0f;

	/**
	 * §4 [ASSUMPTION]: maximum pass range.
	 *
	 * The field is 24000 x 12000, and the bots' own pass range works out at ~6600uu
	 * (BotPassRangeFieldFraction 0.55 of a 12000uu half-length). 8000 covers a third of the pitch
	 * and comfortably contains every pass a bot will attempt, without making a cross-map hail mary
	 * free.
	 */
	constexpr double PassMaxRange = 8000.0;

	/**
	 * §4 [ASSUMPTION]: "hover the crosshair over a teammate", generously.
	 *
	 * Two tests, either of which is enough: a flat angular cone (so a distant teammate is still
	 * acquirable when their capsule subtends almost nothing), and the aim ray passing within
	 * CapsuleRadius + PassAimSlack of the receiver's capsule axis (so a near teammate is acquirable
	 * anywhere on their body). The angular test is what stops it feeling frustrating at range; the
	 * capsule test is what stops it feeling sloppy up close.
	 */
	constexpr double PassAimConeDegrees = 6.0;
	constexpr double PassAimSlack = 40.0;

	/** Chest, not feet: the LOS probe and the aim point both use it. */
	constexpr double TargetChestOffsetZ = 20.0;

	/**
	 * How long a kickoff waits before it is granted.
	 *
	 * Every caller of ResetToCenter()/KickoffTo() teleports ten pawns immediately afterwards
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
	DOREPLIFETIME(ATraceCore, State);
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
		// if the GameMode has not called ResetToCenter()/KickoffTo() within a few seconds.
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
			bool bMatchLive = false;
			if (const ATraceGameState* GameState = GetWorld() ? GetWorld()->GetGameState<ATraceGameState>() : nullptr)
			{
				bMatchLive = (GameState->TraceMatchState == ETraceMatchState::InProgress);
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

bool ATraceCore::IsPickupLockedOutFor(const ATraceCharacter* /*Character*/) const
{
	// Nothing is picked up any more, so nothing can be locked out of it.
	return false;
}

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
	const float Hold = FMath::Max(0.01f, TraceCoreTuning::PassHoldSeconds);
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

bool ATraceCore::IsLegalPassTarget(const ATraceCharacter* Holder, const ATraceCharacter* Candidate, bool bRequireAim) const
{
	if (!IsValid(Holder) || !IsValid(Candidate) || Holder == Candidate)
	{
		return false;
	}

	// §9.3 [ASSUMPTION]: you cannot pass to a dead or respawning teammate.
	if (!Candidate->IsAlive())
	{
		return false;
	}

	if (!AreAllies(Holder->GetTeam(), Candidate->GetTeam()))
	{
		return false;
	}

	const FVector ViewLocation = Holder->GetPawnViewLocation();
	const FVector TargetChest = Candidate->GetActorLocation() + FVector(0.0, 0.0, TraceCoreTuning::TargetChestOffsetZ);

	if (FVector::DistSquared(ViewLocation, TargetChest) > FMath::Square(TraceCoreTuning::PassMaxRange))
	{
		return false;
	}

	// Line of sight, against WORLD GEOMETRY ONLY. A hitscan-style ECC_Visibility trace would let a
	// teammate standing between the two of them invalidate the pass, which is not what "can I see
	// them" means for a receiver.
	const UWorld* World = GetWorld();
	if (World != nullptr)
	{
		FCollisionObjectQueryParams ObjectParams;
		ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
		ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);

		FCollisionQueryParams QueryParams(FName(TEXT("TracePassLOS")), /*bTraceComplex=*/false);
		QueryParams.AddIgnoredActor(this);
		QueryParams.AddIgnoredActor(Holder);
		QueryParams.AddIgnoredActor(Candidate);

		if (World->LineTraceTestByObjectType(ViewLocation, TargetChest, ObjectParams, QueryParams))
		{
			return false;
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
		return false;   // Behind us.
	}

	// (a) angular cone - what makes a distant receiver acquirable at all.
	if (Cosine >= FMath::Cos(FMath::DegreesToRadians(TraceCoreTuning::PassAimConeDegrees)))
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

	const double Threshold = CapsuleRadius + TraceCoreTuning::PassAimSlack;
	return FVector::DistSquared(ClosestOnRay, ClosestOnCapsule) <= (Threshold * Threshold);
}

ATraceCharacter* ATraceCore::FindPassTargetFor(const ATraceCharacter* Holder) const
{
	if (!IsValid(Holder) || !Holder->IsAlive())
	{
		return nullptr;
	}

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
		const FVector TargetChest = Candidate->GetActorLocation() + FVector(0.0, 0.0, TraceCoreTuning::TargetChestOffsetZ);
		const FVector ToTarget = (TargetChest - ViewLocation).GetSafeNormal();
		const double Cosine = FVector::DotProduct(ToTarget, AimDirection);

		if (Cosine > BestCosine)
		{
			BestCosine = Cosine;
			Best = Candidate;
		}
	}

	return Best;
}


// =================================================================================================
// Pass: input and state machine
// =================================================================================================

void ATraceCore::RequestPassInput(bool bPressed)
{
	// Only the holder can pass, and only a living one.
	if (!IsValid(Carrier) || !Carrier->IsAlive())
	{
		return;
	}

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
	if (Carrier->IsLocallyControlled())
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

	ServerSetPassInput(bPressed);
}

void ATraceCore::ServerSetPassInput_Implementation(bool bPressed)
{
	// Network input: this RPC is routed by ownership (SetOwner(Carrier) in GrantTo), so only the
	// holding connection can reach it at all. Re-check anyway - ownership replication and the
	// client's own idea of who holds the Core can disagree for a frame.
	if (!IsValid(Carrier))
	{
		return;
	}

	bPassInputHeld = bPressed;
	ServerTickPass(0.f);
}

void ATraceCore::ServerTickPass(float /*DeltaSeconds*/)
{
	if (!HasAuthority())
	{
		return;
	}

	const float Now = GetServerTimeSeconds();

	if (!IsValid(Carrier) || !Carrier->IsAlive())
	{
		CancelPass(TEXT("holder gone"));
		return;
	}

	// --- An active pass: validate every frame, then complete on time. --------------------------
	if (bPassActive)
	{
		if (!bPassInputHeld)
		{
			// §4 [ASSUMPTION]: releasing early cancels, with the same instant restoration.
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

		if (!IsLegalPassTarget(Carrier, PassTarget, /*bRequireAim=*/!bHolderIsAI))
		{
			CancelPass(TEXT("looked away or target invalid"));
			return;
		}

		if ((Now - PassStartServerTime) >= TraceCoreTuning::PassHoldSeconds)
		{
			ATraceCharacter* Receiver = PassTarget;

			UE_LOG(LogTraceGame, Log, TEXT("Core: pass completed %s -> %s"),
				*GetNameSafe(Carrier), *GetNameSafe(Receiver));

			// Cooldown lands on the passer's side of the transfer, but the Core is about to change
			// hands so it is really only meaningful if the pass is somehow refused below.
			PassCooldownEndServerTime = Now + TraceCoreTuning::PassCooldownSeconds;

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

	bPassInputHeld = false;

	if (!bPassActive)
	{
		return;
	}

	if (Reason != nullptr)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("Core: pass cancelled (%s) - shield restored, trace vulnerable"), Reason);

		// A cancelled attempt still spends the cooldown. Without this, tapping the button is a free
		// way to churn the shield state (and the packets that carry it) every frame.
		PassCooldownEndServerTime = GetServerTimeSeconds() + TraceCoreTuning::PassCooldownSeconds;
	}

	bPassActive = false;
	PassTarget = nullptr;

	// Instant restoration, both halves together (§9.2: the implemented answer is "instant").
	ApplyTraceInvulnerability();
	UpdateVisuals();
	ForceNetUpdate();
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
	const FVector TargetChest = PassTarget->GetActorLocation() + FVector(0.0, 0.0, TraceCoreTuning::TargetChestOffsetZ);

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

	Carrier = NewHolder;
	State = ECoreState::Carried;
	PendingGrantTeam = ETraceTeam::None;
	bFallbackQueued = false;
	bOutOfPlay = false;

	// Ownership is what lets the holding CLIENT send ServerSetPassInput() to this actor. Without it
	// the RPC is silently dropped by the net driver as "not owned by that connection".
	SetOwner(NewHolder);

	// The receiver may not pass back instantly; the cooldown belongs to whoever holds the Core.
	PassCooldownEndServerTime = GetServerTimeSeconds()
		+ ((Reason == ETraceCoreGrantReason::Pass) ? TraceCoreTuning::PassCooldownSeconds : 0.f);

	ApplyAttachment();

	// Grace BEFORE SetCarrying: SetCarrying(true) starts the trail emitting, and the trail must
	// already know it is not allowed to lay a point yet.
	if (NewHolder->Trail != nullptr)
	{
		NewHolder->Trail->SetEmitGrace(bTeamChanged ? TraceCoreTuning::TransferGraceSeconds : 0.f);
	}

	NewHolder->SetCarrying(true);
	if (ATracePlayerState* HolderState = NewHolder->GetPlayerState<ATracePlayerState>())
	{
		HolderState->bIsCarrier = true;
	}
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
	State = ECoreState::Loose;
	SetOwner(nullptr);

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

	if (!IsValid(Previous))
	{
		return;
	}

	// See the declaration: this is what lets OnHolderDeath still recognise the death that is
	// currently unwinding, after the GameMode's own death path has already released the holder.
	RecentlyReleasedHolder = Previous;

	Previous->SetCarrying(false);
	if (ATracePlayerState* PreviousState = Previous->GetPlayerState<ATracePlayerState>())
	{
		PreviousState->bIsCarrier = false;
	}

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

void ATraceCore::Throw(const FVector& /*Direction*/, float /*Speed*/)
{
	if (!HasAuthority())
	{
		return;
	}

	// Nothing is thrown any more. This is the legacy "pass" entry point
	// (ATraceCharacter::PerformPass, and through it every bot's ConsiderPass()), so it is
	// re-expressed as a pass INPUT: press, and let the state machine hold it.
	//
	// The supplied direction is deliberately ignored. The server evaluates the holder's own aim
	// (ATraceCharacter::GetAimDirection, which on the server reads the replicated control rotation),
	// which is the only aim it is entitled to trust. Bots set their control rotation onto the
	// receiver immediately before calling this, so they acquire on the first frame; the state
	// machine then holds their crosshair there for them (DriveBotAimAtPassTarget).
	bPassInputHeld = true;
	ServerTickPass(0.f);

	// Single shot. This entry point is a PRESS with no matching release (ATraceCharacter::DoPass is
	// bound to a pressed-only action, and a bot never lets go of anything), so if nothing was
	// acquired the button must not stay latched - otherwise the holder would silently pass to the
	// first teammate who later wandered across their crosshair. Once a pass IS running the flag
	// stays set and the state machine owns it until the hold completes or the holder looks away.
	if (!bPassActive)
	{
		bPassInputHeld = false;
	}
}

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

void ATraceCore::ResetToCenter()
{
	if (!HasAuthority())
	{
		return;
	}

	// §1 [ASSUMPTION] - American-football kickoff. Every caller of this is a score, a match start or
	// a half-time reset, and in the scoring case the outgoing holder IS the team that just scored,
	// so the team that was scored on is simply their opponent. That means the kickoff rule needs no
	// change at the call site.
	const ETraceTeam ScoringTeam = GetHolderTeam();
	const ETraceTeam ReceivingTeam = (ScoringTeam != ETraceTeam::None)
		? TraceOpposingTeam(ScoringTeam)
		: TraceCoreTuning::DefaultKickoffTeam;

	KickoffTo(ReceivingTeam);
}


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
