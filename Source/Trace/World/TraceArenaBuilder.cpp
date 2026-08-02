// Copyright (c) Trace. All Rights Reserved.

#include "World/TraceArenaBuilder.h"

#include "Trace.h"
#include "TraceTypes.h"
#include "Gameplay/TraceEndzone.h"
#include "World/TraceTeamPlayerStart.h"

#include "Components/BoxComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/EngineTypes.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                       // TActorIterator
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"            // NewObject, MakeUniqueObjectName

namespace TraceArenaConstants
{
	/** The engine basic shapes are 100 uu across and centred on their pivot. */
	static constexpr float ShapeUnit = 100.f;

	/** Floor top sits at local Z = 0. Everything else is measured from there. */
	static constexpr float PedestalHeight = 60.f;
	static constexpr float PedestalDiameter = 300.f;
	static constexpr float PedestalRingDiameter = 640.f;

	/** Core rest height above the pedestal top. It falls the last few uu onto the pedestal. */
	static constexpr float CoreDropHeight = 90.f;

	/** Thin decals laid on the floor. Kept a couple of uu up so they never z-fight the floor face. */
	static constexpr float GridZ = 2.f;
	static constexpr float GridThickness = 4.f;
	static constexpr float PatchZ = 3.f;
	static constexpr float PatchThickness = 6.f;
	static constexpr float GoalLineZ = 5.f;
	static constexpr float GoalLineThickness = 10.f;
	static constexpr float GoalLineWidth = 20.f;

	/** Bright strip running along the top inner edge of each wall - the Tron read from a distance. */
	static constexpr float WallTrimSize = 14.f;

	/** Player start capsules are ~92 uu half height; 100 keeps them clear of the floor. */
	static constexpr float PlayerStartZ = 100.f;

	/** How far the spawn line sits in front of a team's own goal line. */
	static constexpr float StartInsetFromEndzone = 400.f;

	/** Fraction of the half width the spawn fan covers. */
	static constexpr float StartSpreadFraction = 0.7f;

	// Palette. Dark, desaturated structure so the bright team colours and the grid carry the read.
	static const FLinearColor FloorColor(0.012f, 0.016f, 0.026f);
	static const FLinearColor WallColor(0.030f, 0.045f, 0.070f);
	static const FLinearColor GridColor(0.040f, 0.300f, 0.430f);
	static const FLinearColor CenterLineColor(0.200f, 0.800f, 1.000f);
	static const FLinearColor PedestalColor(0.080f, 0.120f, 0.170f);
	static const FLinearColor NeutralTrimColor(0.150f, 0.550f, 0.700f);

	/** Scales a team colour down for large floor areas; full strength is reserved for thin lines. */
	static FLinearColor Dim(const FLinearColor& Color, float Scale)
	{
		return FLinearColor(Color.R * Scale, Color.G * Scale, Color.B * Scale, 1.f);
	}
}

ATraceArenaBuilder::ATraceArenaBuilder()
{
	PrimaryActorTick.bCanEverTick = false;

	// Replicated so clients receive the actor and run BeginPlay locally; it owns no replicated
	// properties, and its transform is sent once in the spawn header. Movement replication is off
	// because the arena never moves - and if it did, every client's copy of the geometry would
	// still be sitting where it was built.
	bReplicates = true;
	SetReplicateMovement(false);
	SetCanBeDamaged(false);

	// Deliberately no relevancy tuning: the builder sits at the arena centre and the default net
	// cull distance (15000 uu) comfortably covers an 8000 uu field, so every player is always in
	// range. AActor's relevancy fields changed accessor form during the 5.x line; not touching them
	// keeps this compiling across 5.4-5.8 (build contract section 1).

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	// Explicitly Movable: a scene component may never be less mobile than its children, and every
	// piece of geometry we attach below is created Movable (runtime-spawned actors have no baked
	// lighting to gain from Static, and this project disables static lighting outright).
	Root->SetMobility(EComponentMobility::Movable);

	// Constructor-time FObjectFinders are what make these engine assets cook into a packaged build.
	// A bare runtime LoadObject would resolve to null once cooked (build contract section 2).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		CubeMesh = CubeFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		CylinderMesh = CylinderFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		BaseMaterial = MaterialFinder.Object;
	}
}

// -------------------------------------------------------------------------------------------------
// Contract surface
// -------------------------------------------------------------------------------------------------

ATraceArenaBuilder* ATraceArenaBuilder::Get(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<ATraceArenaBuilder> It(World); It; ++It)
	{
		ATraceArenaBuilder* Builder = *It;
		if (IsValid(Builder))
		{
			return Builder;
		}
	}

	return nullptr;
}

FVector ATraceArenaBuilder::GetCoreSpawnLocation() const
{
	// Just above the pedestal top: the Core is spawned/reset here and drops the last few uu onto it.
	// ATraceCore treats anything within ~75 uu of this point as "already home", so the resting
	// position on the pedestal must stay inside that tolerance - keep CoreDropHeight small.
	const FVector Local(0.f, 0.f, TraceArenaConstants::PedestalHeight + TraceArenaConstants::CoreDropHeight);
	return GetActorTransform().TransformPosition(Local);
}

FBox ATraceArenaBuilder::GetFieldBounds() const
{
	const FBox Local(
		FVector(-HalfLength(), -HalfWidth(), 0.f),
		FVector(HalfLength(), HalfWidth(), WallHeight));

	return Local.TransformBy(GetActorTransform());
}

// -------------------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------------------

void ATraceArenaBuilder::BeginPlay()
{
	Super::BeginPlay();

	// ATraceGameMode may force our BeginPlay early via DispatchBeginPlay so the Core and the player
	// starts exist before the first spawn; the guard makes that idempotent.
	if (!bArenaBuilt)
	{
		BuildArena();
	}
}

void ATraceArenaBuilder::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Only clean up when this actor specifically is destroyed. On level teardown or travel the
	// world destroys everything anyway, and touching actors mid-teardown is a good way to crash.
	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		for (const TObjectPtr<AActor>& Spawned : SpawnedActors)
		{
			AActor* Actor = Spawned.Get();
			if (IsValid(Actor))
			{
				Actor->Destroy();
			}
		}
	}

	SpawnedActors.Reset();

	Super::EndPlay(EndPlayReason);
}

// -------------------------------------------------------------------------------------------------
// Build
// -------------------------------------------------------------------------------------------------

void ATraceArenaBuilder::BuildArena()
{
	bArenaBuilt = true;

	// A dedicated server needs collision and triggers, nothing else: material shaders are not cooked
	// for server targets and nothing there ever renders.
	const bool bBuildVisuals = (GetNetMode() != NM_DedicatedServer);

	BuildFloorAndWalls(bBuildVisuals);

	if (bBuildVisuals)
	{
		BuildGrid();
	}

	BuildEndzones(bBuildVisuals);
	BuildPedestal(bBuildVisuals);

	if (HasAuthority())
	{
		// Only ChoosePlayerStart reads these, and that only ever runs on the server.
		BuildPlayerStarts();
	}

	BuildLighting();

	UE_LOG(LogTraceGame, Log, TEXT("Arena built (%.0f x %.0f uu, visuals=%s, authority=%s): %d components."),
		FieldLength, FieldWidth,
		bBuildVisuals ? TEXT("yes") : TEXT("no"),
		HasAuthority() ? TEXT("yes") : TEXT("no"),
		GetComponents().Num());
}

void ATraceArenaBuilder::BuildFloorAndWalls(bool bBuildVisuals)
{
	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();

	// The slab reaches under the walls so nothing can squeeze through the seam at the corners.
	const FVector FloorSize(FieldLength + 2.f * WallThickness, FieldWidth + 2.f * WallThickness, FloorThickness);
	const FVector FloorCenter(0.f, 0.f, -FloorThickness * 0.5f);

	AddCollisionBlock(FloorCenter, FloorSize, TEXT("FloorCollision"));

	UMaterialInstanceDynamic* FloorMID = bBuildVisuals ? MakeMID(TraceArenaConstants::FloorColor) : nullptr;
	UMaterialInstanceDynamic* WallMID = bBuildVisuals ? MakeMID(TraceArenaConstants::WallColor) : nullptr;

	if (bBuildVisuals)
	{
		AddMeshBlock(CubeMesh, FloorCenter, FloorSize, FloorMID, /*bCastShadow=*/false, TEXT("Floor"));
	}

	// Four perimeter walls. The long pair overlaps the ends by a wall thickness so the corners are
	// solid without a fifth piece.
	const float WallCenterZ = WallHeight * 0.5f;

	struct FWallSpec
	{
		FVector Center;
		FVector Size;
		const TCHAR* Name;
	};

	const FWallSpec Walls[] =
	{
		{ FVector(0.f,  HalfY + WallThickness * 0.5f, WallCenterZ), FVector(FieldLength + 2.f * WallThickness, WallThickness, WallHeight), TEXT("WallPosY") },
		{ FVector(0.f, -HalfY - WallThickness * 0.5f, WallCenterZ), FVector(FieldLength + 2.f * WallThickness, WallThickness, WallHeight), TEXT("WallNegY") },
		{ FVector( HalfX + WallThickness * 0.5f, 0.f, WallCenterZ), FVector(WallThickness, FieldWidth, WallHeight), TEXT("WallPosX") },
		{ FVector(-HalfX - WallThickness * 0.5f, 0.f, WallCenterZ), FVector(WallThickness, FieldWidth, WallHeight), TEXT("WallNegX") }
	};

	for (const FWallSpec& Wall : Walls)
	{
		AddCollisionBlock(Wall.Center, Wall.Size, Wall.Name);

		if (bBuildVisuals)
		{
			AddMeshBlock(CubeMesh, Wall.Center, Wall.Size, WallMID, /*bCastShadow=*/true, Wall.Name);
		}
	}

	if (!bBuildVisuals)
	{
		return;
	}

	// Bright trim along the top inner edge of each wall. The end walls take the colour of the team
	// that defends that end, so you can read which way you are attacking from anywhere on the field.
	const float TrimZ = WallHeight - TraceArenaConstants::WallTrimSize * 0.5f;
	const float TrimInset = TraceArenaConstants::WallTrimSize * 0.5f;

	UMaterialInstanceDynamic* NeutralTrimMID = MakeMID(TraceArenaConstants::NeutralTrimColor);
	AddMeshBlock(CubeMesh, FVector(0.f, HalfY - TrimInset, TrimZ),
		FVector(FieldLength, TraceArenaConstants::WallTrimSize, TraceArenaConstants::WallTrimSize),
		NeutralTrimMID, false, TEXT("TrimPosY"));
	AddMeshBlock(CubeMesh, FVector(0.f, -HalfY + TrimInset, TrimZ),
		FVector(FieldLength, TraceArenaConstants::WallTrimSize, TraceArenaConstants::WallTrimSize),
		NeutralTrimMID, false, TEXT("TrimNegY"));

	const ETraceTeam EndTeams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : EndTeams)
	{
		const float Sign = TeamEndSign(Team);
		UMaterialInstanceDynamic* TeamTrimMID = MakeMID(TraceTeamColor(Team));

		AddMeshBlock(CubeMesh, FVector(Sign * (HalfX - TrimInset), 0.f, TrimZ),
			FVector(TraceArenaConstants::WallTrimSize, FieldWidth, TraceArenaConstants::WallTrimSize),
			TeamTrimMID, false, TEXT("EndTrim"));
	}
}

void ATraceArenaBuilder::BuildGrid()
{
	if (GridSpacing < 50.f)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("ATraceArenaBuilder: GridSpacing %.1f is too small; skipping the floor grid."), GridSpacing);
		return;
	}

	UMaterialInstanceDynamic* GridMID = MakeMID(TraceArenaConstants::GridColor);
	UMaterialInstanceDynamic* CenterMID = MakeMID(TraceArenaConstants::CenterLineColor);

	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();

	// Half the strips per axis, so the total stays at (2N+1) each way. With the shipped defaults
	// that is 15 + 7 = 22 components - the whole arena lands around 45, well inside budget.
	const int32 MaxHalfLines = FMath::Max(1, MaxGridLinesPerAxis / 2);
	const int32 HalfLinesX = FMath::Min(FMath::FloorToInt(HalfX / GridSpacing), MaxHalfLines);
	const int32 HalfLinesY = FMath::Min(FMath::FloorToInt(HalfY / GridSpacing), MaxHalfLines);

	// Lines of constant X, running the full width. The halfway line is wider and brighter.
	for (int32 Index = -HalfLinesX; Index <= HalfLinesX; ++Index)
	{
		const float X = Index * GridSpacing;
		if (FMath::IsNearlyEqual(FMath::Abs(X), HalfX, 1.f))
		{
			continue; // That is the wall, not a grid line.
		}

		const bool bCenter = (Index == 0);
		AddMeshBlock(CubeMesh,
			FVector(X, 0.f, TraceArenaConstants::GridZ),
			FVector(bCenter ? GridStripWidth * 3.f : GridStripWidth, FieldWidth, TraceArenaConstants::GridThickness),
			bCenter ? CenterMID : GridMID,
			/*bCastShadow=*/false,
			TEXT("GridX"));
	}

	// Lines of constant Y, running the full length.
	for (int32 Index = -HalfLinesY; Index <= HalfLinesY; ++Index)
	{
		const float Y = Index * GridSpacing;
		if (FMath::IsNearlyEqual(FMath::Abs(Y), HalfY, 1.f))
		{
			continue;
		}

		AddMeshBlock(CubeMesh,
			FVector(0.f, Y, TraceArenaConstants::GridZ),
			FVector(FieldLength, GridStripWidth, TraceArenaConstants::GridThickness),
			GridMID,
			/*bCastShadow=*/false,
			TEXT("GridY"));
	}
}

void ATraceArenaBuilder::BuildEndzones(bool bBuildVisuals)
{
	UWorld* World = GetWorld();
	const float HalfX = HalfLength();
	const float HalfY = HalfWidth();
	const float Depth = FMath::Clamp(EndzoneDepth, 100.f, HalfX);

	const ETraceTeam Teams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : Teams)
	{
		const float Sign = TeamEndSign(Team);
		const float CenterX = Sign * (HalfX - Depth * 0.5f);

		if (bBuildVisuals)
		{
			// The patch is tinted with the colour of the team that DEFENDS this end. Their opponent
			// is the one who scores on it - see ATraceEndzone's class comment.
			UMaterialInstanceDynamic* PatchMID = MakeMID(TraceArenaConstants::Dim(TraceTeamColor(Team), 0.28f));
			AddMeshBlock(CubeMesh,
				FVector(CenterX, 0.f, TraceArenaConstants::PatchZ),
				FVector(Depth, FieldWidth, TraceArenaConstants::PatchThickness),
				PatchMID, /*bCastShadow=*/false, TEXT("EndzonePatch"));

			UMaterialInstanceDynamic* LineMID = MakeMID(TraceTeamColor(Team));
			AddMeshBlock(CubeMesh,
				FVector(Sign * (HalfX - Depth), 0.f, TraceArenaConstants::GoalLineZ),
				FVector(TraceArenaConstants::GoalLineWidth, FieldWidth, TraceArenaConstants::GoalLineThickness),
				LineMID, /*bCastShadow=*/false, TEXT("GoalLine"));
		}

		// The trigger itself is server-only: scoring is an authority decision, and a client copy
		// would be dead weight at best. Spawned deferred so OwningTeam is correct before BeginPlay.
		if (!HasAuthority() || World == nullptr)
		{
			continue;
		}

		const FTransform ZoneTransform(
			GetActorRotation(),
			GetActorTransform().TransformPosition(FVector(CenterX, 0.f, WallHeight * 0.5f)));

		ATraceEndzone* Zone = World->SpawnActorDeferred<ATraceEndzone>(
			ATraceEndzone::StaticClass(), ZoneTransform, /*Owner=*/nullptr, /*Instigator=*/nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

		if (Zone == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("ATraceArenaBuilder: failed to spawn the %s endzone."), *TraceTeamName(Team).ToString());
			continue;
		}

		Zone->ConfigureZone(Team, FVector(Depth * 0.5f, HalfY, WallHeight * 0.5f));
		Zone->FinishSpawning(ZoneTransform);

		// We are very often spawned from ATraceGameMode::BeginPlay, i.e. from inside the world's
		// own begin-play sweep, and an actor created during that sweep is not guaranteed to be
		// dispatched by it. The endzone does all of its wiring in BeginPlay, so force it here -
		// DispatchBeginPlay self-guards, and the world's later pass will simply skip it.
		if (!Zone->HasActorBegunPlay())
		{
			Zone->DispatchBeginPlay();
		}

		SpawnedActors.Add(Zone);
	}
}

void ATraceArenaBuilder::BuildPedestal(bool bBuildVisuals)
{
	const FVector PedestalSize(
		TraceArenaConstants::PedestalDiameter,
		TraceArenaConstants::PedestalDiameter,
		TraceArenaConstants::PedestalHeight);
	const FVector PedestalCenter(0.f, 0.f, TraceArenaConstants::PedestalHeight * 0.5f);

	// A box under a round pedestal: it is a hair wider at the corners than the mesh, which only
	// means the Core comes to rest fractionally early. Cheaper and more predictable than convex
	// collision, and the Core is the only thing that ever touches it.
	AddCollisionBlock(PedestalCenter, PedestalSize * FVector(0.94f, 0.94f, 1.f), TEXT("PedestalCollision"));

	if (!bBuildVisuals)
	{
		return;
	}

	UMaterialInstanceDynamic* PedestalMID = MakeMID(TraceArenaConstants::PedestalColor);
	AddMeshBlock(CylinderMesh, PedestalCenter, PedestalSize, PedestalMID, /*bCastShadow=*/true, TEXT("Pedestal"));

	// A flat ring on the floor around it, so the centre reads from across the field.
	UMaterialInstanceDynamic* RingMID = MakeMID(TraceArenaConstants::CenterLineColor);
	AddMeshBlock(CylinderMesh,
		FVector(0.f, 0.f, TraceArenaConstants::GridZ),
		FVector(TraceArenaConstants::PedestalRingDiameter, TraceArenaConstants::PedestalRingDiameter, TraceArenaConstants::GridThickness),
		RingMID, /*bCastShadow=*/false, TEXT("CenterRing"));
}

void ATraceArenaBuilder::BuildPlayerStarts()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const int32 PerTeam = FMath::Clamp(StartsPerTeam, 1, 16);
	const float HalfX = HalfLength();
	const float Spread = HalfWidth() * TraceArenaConstants::StartSpreadFraction;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient; // Purely runtime scaffolding; never save it into a level.

	const ETraceTeam Teams[] = { ETraceTeam::Blue, ETraceTeam::Orange };
	for (const ETraceTeam Team : Teams)
	{
		const float Sign = TeamEndSign(Team);

		// A team spawns in front of the endzone it defends and faces the centre of the field.
		const float LineX = Sign * FMath::Max(0.f, HalfX - EndzoneDepth - TraceArenaConstants::StartInsetFromEndzone);
		const float FacingYaw = (Sign < 0.f) ? 0.f : 180.f;

		for (int32 Index = 0; Index < PerTeam; ++Index)
		{
			const float Alpha = (PerTeam == 1) ? 0.5f : static_cast<float>(Index) / static_cast<float>(PerTeam - 1);
			const float Y = FMath::Lerp(-Spread, Spread, Alpha);

			const FVector Location = GetActorTransform().TransformPosition(
				FVector(LineX, Y, TraceArenaConstants::PlayerStartZ));
			const FRotator Rotation = GetActorRotation() + FRotator(0.f, FacingYaw, 0.f);

			ATraceTeamPlayerStart* Start = World->SpawnActor<ATraceTeamPlayerStart>(
				ATraceTeamPlayerStart::StaticClass(), Location, Rotation, SpawnParams);

			if (Start == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("ATraceArenaBuilder: failed to spawn a %s player start."), *TraceTeamName(Team).ToString());
				continue;
			}

			Start->Team = Team;
			SpawnedActors.Add(Start);
		}
	}

	UE_LOG(LogTraceGame, Verbose, TEXT("Arena spawned %d player starts per team."), PerTeam);
}

void ATraceArenaBuilder::BuildLighting()
{
	UWorld* World = GetWorld();
	if (World == nullptr || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// Both lights are local cosmetics, spawned independently on the server and on every client, so
	// each machine must be told explicitly NOT to replicate its copy. ALight (and ASkyLight) turn
	// replication and bAlwaysRelevant on in their own constructors, which on a listen server would
	// push the host's sun and sky light to every client ON TOP of the ones that client just built
	// for itself: two suns at SunIntensity each, with r.DefaultFeature.AutoExposure=False to hide
	// behind, plus a duplicate sky recapture. Gating the spawn on net mode instead would be wrong -
	// clients genuinely need their own lights.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	const FVector SkyPosition = GetActorTransform().TransformPosition(FVector(0.f, 0.f, WallHeight * 2.f));

	const FTransform SunTransform(GetActorRotation() + SunRotation, SkyPosition);
	if (ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), SunTransform, SpawnParams))
	{
		// Purely local: see the note above. Both calls are safe on a client (SetReplicates(false) is
		// always permitted regardless of role).
		Sun->SetReplicates(false);
		Sun->SetReplicateMovement(false);

		// FindComponentByClass rather than a typed accessor: it cannot be wrong about the getter's
		// name or return type on any 5.x engine.
		if (UDirectionalLightComponent* SunComponent = Sun->FindComponentByClass<UDirectionalLightComponent>())
		{
			// Runtime-spawned lights must be Movable. A Stationary/Static light has no baked data in
			// a level nobody ever built lighting for, and the project disables static lighting
			// entirely (r.AllowStaticLighting=False), so Movable is the only correct answer.
			SunComponent->SetMobility(EComponentMobility::Movable);
			SunComponent->SetIntensity(SunIntensity);
			SunComponent->SetLightColor(FLinearColor(1.f, 0.97f, 0.92f));
		}
		SpawnedActors.Add(Sun);
	}

	if (ASkyLight* Sky = World->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FTransform(SkyPosition), SpawnParams))
	{
		// Purely local: see the note above.
		Sky->SetReplicates(false);
		Sky->SetReplicateMovement(false);

		if (USkyLightComponent* SkyComponent = Sky->FindComponentByClass<USkyLightComponent>())
		{
			SkyComponent->SetMobility(EComponentMobility::Movable);

			// Intensity is set through the public field plus MarkRenderStateDirty rather than a
			// setter: ULightComponentBase::Intensity is stable across the whole 5.x line, whereas the
			// sky light's typed SetIntensity lives on USkyLightComponent rather than ULightComponent
			// and we cannot compile-check it here.
			SkyComponent->Intensity = SkyLightIntensity;
			SkyComponent->MarkRenderStateDirty();

			// NOTE: with no sky sphere or atmosphere in the level there is very little for the sky
			// light to capture, so it contributes mostly a faint fill - the directional light does
			// the real work. RecaptureSky() is still required after a runtime spawn, otherwise the
			// component never captures at all. If the arena ever reads too dark, raise SunIntensity
			// rather than fighting this.
			SkyComponent->RecaptureSky();
		}
		SpawnedActors.Add(Sky);
	}
}

// -------------------------------------------------------------------------------------------------
// Primitive helpers
// -------------------------------------------------------------------------------------------------

UStaticMeshComponent* ATraceArenaBuilder::AddMeshBlock(UStaticMesh* Mesh, const FVector& LocalCenter, const FVector& Size,
	UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName)
{
	// Null-checked per the asset rules: a missing engine shape must degrade to "invisible", never
	// to a crash. Collision lives in separate box components, so the arena still plays.
	if (Mesh == nullptr || Root == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
		this, MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), FName(DebugName)));
	if (Component == nullptr)
	{
		return nullptr;
	}

	// Everything is configured BEFORE RegisterComponent: a scene component may be freely posed
	// while unregistered, and registering once with the final state avoids a redundant render and
	// physics update per piece.
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetupAttachment(Root);
	Component->SetStaticMesh(Mesh);
	Component->SetRelativeLocation(LocalCenter);
	Component->SetRelativeScale3D(Size / TraceArenaConstants::ShapeUnit);
	Component->SetCollisionProfileName(TEXT("NoCollision"));
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(bCastShadow);

	if (MID != nullptr)
	{
		Component->SetMaterial(0, MID);
	}

	Component->RegisterComponent();
	return Component;
}

UBoxComponent* ATraceArenaBuilder::AddCollisionBlock(const FVector& LocalCenter, const FVector& Size, const TCHAR* DebugName)
{
	if (Root == nullptr)
	{
		return nullptr;
	}

	UBoxComponent* Component = NewObject<UBoxComponent>(
		this, MakeUniqueObjectName(this, UBoxComponent::StaticClass(), FName(DebugName)));
	if (Component == nullptr)
	{
		return nullptr;
	}

	Component->SetMobility(EComponentMobility::Movable);
	Component->SetupAttachment(Root);
	Component->SetRelativeLocation(LocalCenter);
	Component->SetBoxExtent(Size * 0.5f, /*bUpdateOverlaps=*/false);
	// BlockAll: WorldStatic object type blocking every channel. That single profile covers all
	// three things the arena has to stop - pawn movement sweeps, the Core's projectile sweeps and
	// hitscan line traces (ECC_Visibility) - without hand-rolling a response table.
	Component->SetCollisionProfileName(TEXT("BlockAll"));
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetHiddenInGame(true);

	Component->RegisterComponent();
	return Component;
}

UMaterialInstanceDynamic* ATraceArenaBuilder::MakeMID(const FLinearColor& Color)
{
	if (BaseMaterial == nullptr)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	if (MID == nullptr)
	{
		return nullptr;
	}

	MID->SetVectorParameterValue(TEXT("Color"), Color);     // BasicShapeMaterial's real parameter.
	MID->SetVectorParameterValue(TEXT("BaseColor"), Color); // Harmless no-op if absent.

	TintMIDs.Add(MID);
	return MID;
}

// -------------------------------------------------------------------------------------------------
// Derived layout
// -------------------------------------------------------------------------------------------------

float ATraceArenaBuilder::TeamEndSign(ETraceTeam Team)
{
	// Blue defends the -X end and spawns there; Orange defends +X. Blue therefore attacks towards
	// +X (the Orange endzone) and vice versa. Everything else in this file derives from this line.
	return (Team == ETraceTeam::Blue) ? -1.f : 1.f;
}
