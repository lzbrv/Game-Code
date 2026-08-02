// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"      // EComponentMobility, EEndPlayReason
#include "GameFramework/Actor.h"
#include "Math/Box.h"                // FBox
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"              // ETraceTeam

#include "TraceArenaBuilder.generated.h"

class ADirectionalLight;
class ASkyLight;
class ATraceEndzone;
class ATraceTeamPlayerStart;
class UBoxComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Builds the entire playfield from C++ at BeginPlay, so /Game/Maps/Arena can be a completely
 * empty level and the project never ships an authored .uasset (build contract section 2).
 *
 * What it makes: an 8000 x 4000 uu floor, four perimeter walls, a Tron-ish grid of thin bright
 * floor strips, two endzones at the +X and -X ends with team-tinted floor patches, a centre
 * pedestal for the Core, five ATraceTeamPlayerStarts per team, and a directional + sky light so
 * an empty map is not a black void.
 *
 * NET MODEL - the important part:
 *  - The GameMode spawns exactly one of these on the server. The actor replicates so that clients
 *    get a copy, but it has no replicated properties: every client runs BeginPlay and builds the
 *    same geometry locally from the same constants. Replicating ~40 static meshes as individual
 *    actors would cost real bandwidth and buy nothing, and clients need the collision locally
 *    anyway or predicted movement would fall through the floor.
 *  - Consequence: the layout constants below must be identical on both ends. They are compiled
 *    defaults, so they are - but if you ever expose them to config or a Blueprint subclass, the
 *    server and clients must resolve the same values.
 *  - Server-only pieces: the ATraceEndzone triggers (scoring is a server decision) and the
 *    ATraceTeamPlayerStarts (only ChoosePlayerStart ever reads them).
 *  - Client-and-listen-server-only pieces: the two lights. A dedicated server builds collision
 *    only - no meshes, no materials, no lights.
 */
UCLASS()
class TRACE_API ATraceArenaBuilder : public AActor
{
	GENERATED_BODY()

public:
	ATraceArenaBuilder();

	//~ Contract surface (spec section 7)

	/** First arena builder in @p World, or null. Cheap enough for occasional use, not per-tick. */
	static ATraceArenaBuilder* Get(UWorld* World);

	/** World-space point the Core spawns at and resets to: just above the centre pedestal. */
	FVector GetCoreSpawnLocation() const;

	/** World-space playable volume: floor to wall top, inside faces of the four walls. */
	FBox GetFieldBounds() const;

	//~ End contract surface

	//~ AActor
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AActor

	// ---------------------------------------------------------------------------------------------
	// Layout. All values in unreal units, all local to this actor's transform, so the whole arena
	// can be moved by moving the builder. Editable for tuning via a Blueprint subclass assigned to
	// ATraceGameMode::ArenaBuilderClass - see the net note in the class comment before changing any
	// of them at runtime.
	// ---------------------------------------------------------------------------------------------

	/** Length of the field along X (goal to goal). */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FieldLength = 8000.f;

	/** Width of the field along Y (sideline to sideline). */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FieldWidth = 4000.f;

	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float WallHeight = 700.f;

	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float WallThickness = 100.f;

	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float FloorThickness = 60.f;

	/** How far each endzone reaches in from its end wall. */
	UPROPERTY(EditAnywhere, Category = "Trace|Arena")
	float EndzoneDepth = 900.f;

	/** Spacing of the floor grid strips. Clamped so a small value cannot spawn thousands of them. */
	UPROPERTY(EditAnywhere, Category = "Trace|Grid")
	float GridSpacing = 500.f;

	/** Width of a grid strip (they are 4 uu tall slabs laid on the floor). */
	UPROPERTY(EditAnywhere, Category = "Trace|Grid")
	float GridStripWidth = 10.f;

	/** Hard cap on grid strips per axis. Guards the component budget against a silly GridSpacing. */
	UPROPERTY(EditAnywhere, Category = "Trace|Grid")
	int32 MaxGridLinesPerAxis = 33;

	UPROPERTY(EditAnywhere, Category = "Trace|Spawns")
	int32 StartsPerTeam = 5;

	/**
	 * Directional light intensity in lux. Tuned low on purpose: Config/DefaultEngine.ini disables
	 * auto exposure (r.DefaultFeature.AutoExposure=False), so the tonemapper sees scene radiance
	 * unscaled and a physically-sized sun would blow the whole frame to white. ~PI lux makes a
	 * lit surface read at roughly its own albedo.
	 */
	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	float SunIntensity = 3.5f;

	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	float SkyLightIntensity = 1.f;

	UPROPERTY(EditAnywhere, Category = "Trace|Lighting")
	FRotator SunRotation = FRotator(-55.f, -35.f, 0.f);

protected:
	/** Attach parent for every piece of built geometry. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Arena")
	TObjectPtr<USceneComponent> Root;

	// --- Build steps ---------------------------------------------------------------------------
	void BuildArena();
	void BuildFloorAndWalls(bool bBuildVisuals);
	void BuildGrid();
	void BuildEndzones(bool bBuildVisuals);
	void BuildPedestal(bool bBuildVisuals);
	void BuildPlayerStarts();
	void BuildLighting();

	// --- Primitive helpers ---------------------------------------------------------------------

	/**
	 * Adds a visual-only mesh block. @p Size is the desired world size in uu; the engine basic
	 * shapes are 100 uu and centred on their pivot, so the scale is just Size/100.
	 * Returns null (harmlessly) if the mesh asset failed to resolve.
	 */
	UStaticMeshComponent* AddMeshBlock(UStaticMesh* Mesh, const FVector& LocalCenter, const FVector& Size,
		UMaterialInstanceDynamic* MID, bool bCastShadow, const TCHAR* DebugName);

	/**
	 * Adds an invisible box collider. Collision deliberately lives in box components rather than in
	 * the static meshes: it costs less, and it means the floor and walls still exist even if
	 * /Engine/BasicShapes ever fails to resolve (a dedicated server build, for instance) - players
	 * would see nothing but would not fall out of the world.
	 */
	UBoxComponent* AddCollisionBlock(const FVector& LocalCenter, const FVector& Size, const TCHAR* DebugName);

	/** Creates a dynamic instance of the engine basic-shape material tinted @p Color. */
	UMaterialInstanceDynamic* MakeMID(const FLinearColor& Color);

	// --- Derived layout ------------------------------------------------------------------------
	float HalfLength() const { return FieldLength * 0.5f; }
	float HalfWidth() const { return FieldWidth * 0.5f; }

	/** +1 for the team defending the +X end, -1 for the other. Blue defends -X. */
	static float TeamEndSign(ETraceTeam Team);

private:
	/** Engine basic shapes, resolved in the constructor so the cooker keeps them (contract §2). */
	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CylinderMesh;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseMaterial;

	/** Keeps the tint instances alive independently of the components they are assigned to. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> TintMIDs;

	/** Actors we spawned, so a destroyed builder takes its arena with it instead of orphaning it. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> SpawnedActors;

	/** BeginPlay can be forced early by the GameMode (DispatchBeginPlay); never build twice. */
	bool bArenaBuilt = false;
};
