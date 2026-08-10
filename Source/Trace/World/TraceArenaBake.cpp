// Copyright (c) Trace. All Rights Reserved.
//
// =================================================================================================
// SPEC v15 §1 — BAKING THE PROCEDURAL ARENA INTO A REAL, EDITABLE .umap
//
// WHAT THIS FILE IS. Two halves of one idea, and they are here rather than in TraceArenaBuilder.cpp
// only because that file is already 7,400 lines:
//
//   BakeArenaIntoLevel()  runs the arena build once, in the editor, and writes what it built into
//                         the current level as individually selectable, readably labelled actors.
//                         Editor only.
//   AdoptBakedArena()     is the other end of it: when a baked level is loaded, this is what the
//                         builder does INSTEAD of building, so the arena is not constructed twice.
//                         Shipped code — a baked level has to work in a packaged game.
//
// WHY BAKE AT ALL. /Game/Maps/Arena is an empty level and everything in a match is built at
// BeginPlay from this class. That is a good property for a prototype (no binary art in the repo, one
// place to change the layout) and a bad one for a game anybody else is going to work on: a designer
// cannot move a wall, cannot try a cover block 200 uu to the left, and cannot look at the thing
// without launching it. Spec v15 §1 is the migration off that, and the bar it sets is exact — "a
// level a human can open, click, and edit. Not a level with one actor in it that builds itself."
//
// THE THREE THINGS THAT MAKE OR BREAK IT, and where each is answered:
//   1. The geometry must EXPLODE into separate actors. The arena is ~1,450 blocks pooled into a few
//      dozen instanced components hanging off one actor, which is the least editable shape possible.
//      See the grouping block below: blocks are gathered back into logical PIECES and one actor is
//      emitted per piece.
//   2. Nothing baked may reference Content/Generated/, which is gitignored — a map that did would be
//      broken on every other machine. The two parent materials are required to exist at
//      /Game/Trace/Materials/Parents (committed; Scripts/generate_content.py authors them) and every
//      MID the build made is written out as a committed UMaterialInstanceConstant into
//      /Game/Trace/Materials/Instances — which is the layer a designer retunes (spec v17 §3).
//   3. The runtime build must not run on a baked level. See ATraceArenaBuilder::EnsureBuilt and
//      bLevelIsPreBaked.
// =================================================================================================

#include "World/TraceArenaBuilder.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "Core/TraceGameState.h"
#include "Gameplay/TraceEndzone.h"
#include "World/TraceBakedPiece.h"
#include "World/TraceCoreSpawn.h"          // spec v17 §2 - the Core spawn is a placed actor now
#include "World/TraceTeamPlayerStart.h"

#include "Components/BoxComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyAtmosphereComponent.h"   // ASkyAtmosphere, for labelling the baked sky
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                        // TActorIterator
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/Box.h"
#include "Math/Box2D.h"
#include "Scalability.h"                        // the quality-change subscription an adopted level still needs
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

// NAMED, not anonymous. Scripts/check-jumbo-build-collisions.py gates the build on this: UBT
// concatenates .cpp files into one translation unit, and two anonymous namespaces that each define a
// helper with the same name become one namespace with two definitions (MSVC C2084). Naming it after
// the file is the fix that does not just defer the next collision.
namespace TraceArenaBakeLocal
{
	/**
	 * Where the committed PARENT materials live (spec v17 §3's Materials/{Parents,Instances}).
	 * NOT Content/Generated - that is gitignored, and a baked map may not reference it.
	 */
	static const TCHAR* PromotedMaterialDir = TEXT("/Game/Trace/Materials/Parents");

	/** Where this file writes one UMaterialInstanceConstant per distinct tint the build asked for. */
	static const TCHAR* BakedInstanceDir = TEXT("/Game/Trace/Materials/Instances");

	/**
	 * How many blocks sharing one (mesh, material, shadow) key inside ONE piece are worth collapsing
	 * into an instanced component.
	 *
	 * THE TENSION THIS SETTLES. Spec v7 §8 rebuilt the whole arena on instanced meshes because ~1,450
	 * loose static mesh components cost 1.8-3.1 ms of game thread in draw submission alone; baking
	 * every block back out as its own component would hand all of that straight back. But an
	 * instanced component is also the thing a designer cannot click. So: a piece's DISTINCT parts (the
	 * dark body, the glowing lip) stay as their own named components, and its REPETITION - the eight
	 * corner ribs of a cover block, which nobody will ever select individually - collapses. Four is
	 * the smallest count where the collapse is worth the lost click.
	 */
	static constexpr int32 InstanceCollapseThreshold = 4;

	/** Hard cap on blocks in one baked piece, so a long run cannot produce one unusable mega-actor. */
	static constexpr int32 MaxBlocksPerPiece = 64;

	/** Actor tag on every piece and gameplay actor the bake emits. Also the "already baked" marker. */
	static const TCHAR* BakedTag = TEXT("TraceBakedArena");

	/** Distinguishes the two shapes of ATraceEndzone in a baked level, which C++ cannot re-derive. */
	static const TCHAR* GoalVolumeTag = TEXT("TraceBakedGoalVolume");
	static const TCHAR* EndzoneVolumeTag = TEXT("TraceBakedEndzoneVolume");

	/** Lets AdoptBakedArena find the pieces of the lighting rig ApplyFidelity has to re-tune. */
	static const TCHAR* FloorLampTag = TEXT("TraceBakedFloorLamp");
	static const TCHAR* KeyLightTag = TEXT("TraceBakedKeyLight");
	static const TCHAR* PostProcessTag = TEXT("TraceBakedPostProcess");

	/**
	 * "WallPosY" -> "Wall_Pos_Y", "GoalRingSpoke" -> "Goal_Ring_Spoke".
	 *
	 * Spec v15 §1.3: "Give every actor a readable label (Wall_North_03, not StaticMeshActor_417) -
	 * this is the entire difference between 'a human made this' and 'a script vomited this'." The
	 * debug names the build already passes to its factories ARE the readable names; they are just
	 * written in camel case, so this only has to put the word breaks back.
	 */
	static FString PrettyPieceName(FName PieceName)
	{
		const FString Raw = PieceName.ToString();

		// The four perimeter walls, by hand, because "Wall_Pos_Y" is a coordinate and "Wall_North" is
		// a place. Everything else reads fine mechanically.
		if (Raw == TEXT("WallPosY")) { return TEXT("Wall_North"); }
		if (Raw == TEXT("WallNegY")) { return TEXT("Wall_South"); }
		if (Raw == TEXT("WallPosX")) { return TEXT("Wall_East"); }
		if (Raw == TEXT("WallNegX")) { return TEXT("Wall_West"); }

		FString Out;
		Out.Reserve(Raw.Len() + 8);
		for (int32 Index = 0; Index < Raw.Len(); ++Index)
		{
			const TCHAR Ch = Raw[Index];
			const bool bBreak = (Index > 0)
				&& FChar::IsUpper(Ch)
				&& !FChar::IsUpper(Raw[Index - 1]);
			if (bBreak)
			{
				Out.AppendChar(TEXT('_'));
			}
			Out.AppendChar(Ch);
		}
		return Out;
	}

	/** First word of the pretty name, used as the World Outliner folder so the level reads as a plan. */
	static FString PieceCategory(const FString& PrettyName)
	{
		FString Head;
		FString Tail;
		return PrettyName.Split(TEXT("_"), &Head, &Tail) ? Head : PrettyName;
	}

	/** Package-safe: letters, digits and underscores only. */
	static FString SanitiseAssetName(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (const TCHAR Ch : In)
		{
			Out.AppendChar((FChar::IsAlnum(Ch) || Ch == TEXT('_')) ? Ch : TEXT('_'));
		}
		return Out.IsEmpty() ? FString(TEXT("Unnamed")) : Out;
	}

	/** One logical piece of the arena: a wall, a cover block, a pylon, a run of cove shells. */
	struct FBakePieceGroup
	{
		FName PieceName;
		ETraceBakedScoringTag ModeTag = ETraceBakedScoringTag::Always;
		TArray<int32> Records;
		FBox2D Footprint = FBox2D(ForceInit);
		FBox LocalBounds = FBox(ForceInit);
	};

	/**
	 * The key an instanced component batches on, exactly as FTraceInstancePool's is.
	 *
	 * Raw pointers, deliberately: this lives on the stack for the length of one bake, and every mesh
	 * and material in it is already kept alive by the builder's own UPROPERTYs or by the package it
	 * was just saved into. A TObjectPtr in a non-reflected struct buys no GC tracking anyway.
	 */
	struct FBakeInstanceKey
	{
		UStaticMesh* KeyMesh = nullptr;
		UMaterialInterface* KeyMaterial = nullptr;
		bool bCastShadow = false;

		bool operator==(const FBakeInstanceKey& Other) const
		{
			return KeyMesh == Other.KeyMesh && KeyMaterial == Other.KeyMaterial && bCastShadow == Other.bCastShadow;
		}
	};
}

// -------------------------------------------------------------------------------------------------
// The parts that ship: recognising a baked level and wiring it back up.
// -------------------------------------------------------------------------------------------------

FName ATraceArenaBuilder::TraceBakedArenaTag()
{
	return FName(TraceArenaBakeLocal::BakedTag);
}

bool ATraceArenaBuilder::IsLevelPreBaked() const
{
	if (bLevelIsPreBaked)
	{
		return true;
	}

	// THE SECOND, INDEPENDENT TRIGGER, and it is not belt-and-braces for its own sake. The property
	// above lives on ONE actor; a designer who deletes the builder and drags a fresh one in from the
	// Place Actors panel gets the default (false) and would silently double-build the level they are
	// standing in. Any baked piece at all is proof the level already contains an arena, and the bake
	// stamps a tag on every piece it emits, so the level is self-describing.
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	for (TActorIterator<ATraceBakedPiece> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It))
		{
			return true;
		}
	}

	return false;
}

void ATraceArenaBuilder::SetBakedActorsPresented(const TArray<TWeakObjectPtr<ATraceBakedPiece>>& Actors, bool bPresented)
{
	for (const TWeakObjectPtr<ATraceBakedPiece>& Weak : Actors)
	{
		ATraceBakedPiece* Piece = Weak.Get();
		if (!IsValid(Piece))
		{
			continue;
		}

		// Hidden AND collisionless, which is the pair SetPiecesPresented applies to components and for
		// the same reason: mode A's solid end wall must not still be standing in front of mode B's
		// ring, and mode B's approach ramp must not still be climbable in mode A.
		Piece->SetActorHiddenInGame(!bPresented);
		Piece->SetActorEnableCollision(bPresented);
	}
}

void ATraceArenaBuilder::AdoptBakedArena()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// Set FIRST, so the ApplyScoringMode call at the bottom is not swallowed by its own
	// "legal before the build" early-out. Nothing below constructs geometry; this flag means "the
	// arena exists", and on a baked level it did before this actor woke up.
	bArenaBuilt = true;

	// A baked level's pieces wear their own committed MaterialInstanceConstants and do not need
	// these two — but the ONE log line saying which material set the process resolved is worth
	// having on both paths, and it is what spec v17 §0 rule 1 asks the fallback to say out loud.
	ResolveArenaMaterials();
	UE_LOG(LogTraceGame, Log,
		TEXT("[Arena] Materials: surface '%s', neon '%s' (a baked level's geometry wears the committed ")
		TEXT("instances under /Game/Trace/Materials/Instances, not these)."),
		*GetPathNameSafe(SurfaceMaterial), *GetPathNameSafe(NeonMaterial));

	// Same question, same authority, same fallback as BuildArena: ATraceGameState if there is one,
	// the settings page if the world has not published yet.
	{
		const ETraceScoringMode PublishedMode = ATraceGameState::GetScoringModeFor(this);
		ScoringMode = (World->GetGameState() != nullptr) ? PublishedMode : UTraceSettings::Get().ScoringMode;
	}

	// --- The geometry, and the two things about it that are not just geometry ---------------------
	int32 PieceCount = 0;
	int32 TintedSurfaces = 0;

	for (TActorIterator<ATraceBakedPiece> It(World); It; ++It)
	{
		ATraceBakedPiece* Piece = *It;
		if (!IsValid(Piece))
		{
			continue;
		}

		++PieceCount;

		switch (Piece->ScoringModeTag)
		{
		case ETraceBakedScoringTag::EndzoneModeOnly:
			BakedEndzoneModeActors.Add(Piece);
			break;
		case ETraceBakedScoringTag::GoalModeOnly:
			BakedGoalModeActors.Add(Piece);
			break;
		default:
			break;
		}

		if (Piece->SideTints.Num() == 0)
		{
			continue;
		}

		// PROMOTING THE BAKED TINTS BACK TO MIDs. A committed material instance cannot be repainted,
		// and the half-time side switch (spec §1) has to repaint both endzones or a Blue player
		// spends the second half defending an orange-painted zone. So every surface the bake recorded
		// as side-tinted gets a dynamic instance created over it here, once, at load, and registered
		// into exactly the same SideMIDs array ApplyTeamSides already drives.
		TArray<UMeshComponent*> MeshComponents;
		Piece->GetComponents<UMeshComponent>(MeshComponents);

		for (const FTraceBakedSideTint& Tint : Piece->SideTints)
		{
			if (Tint.Material == nullptr)
			{
				continue;
			}

			for (UMeshComponent* MeshComponent : MeshComponents)
			{
				if (!IsValid(MeshComponent) || MeshComponent->GetMaterial(0) != Tint.Material)
				{
					continue;
				}

				UMaterialInstanceDynamic* MID = MeshComponent->CreateAndSetMaterialInstanceDynamicFromMaterial(0, Tint.Material);
				if (MID == nullptr)
				{
					continue;
				}

				// Held strongly, exactly as MakeSurfaceMID/MakeNeonMID hold theirs: SideMIDs is weak
				// by design and something has to keep these alive for the life of the match.
				TintMIDs.Add(MID);
				RegisterSideMID(Tint.EndSign, MID, Tint.bNeon, Tint.Intensity, Tint.BaseDim);
				++TintedSurfaces;
			}
		}
	}

	// --- The scoring volumes ----------------------------------------------------------------------
	//
	// VERIFIED AND WARNED ABOUT — NO LONGER OVERWRITTEN.  (spec v17 §2)
	//
	// THE HISTORY, BECAUSE THE OLD BEHAVIOUR LOOKED DELIBERATE. ATraceEndzone used to keep its shape
	// (bGoalVolume, bRingGoal, RingRadius, the armed flag) in plain C++ members, so none of it
	// survived being written into a .umap and this function REBUILT all of it on every load. Those
	// fields are UPROPERTYs now (TraceEndzone.h) and the trigger's box extent always was one, so the
	// shape does serialise. The comment that used to sit here claimed Arena_Baked.umap on disk
	// predated that change and still carried only OwningTeam — MEASURED AND FALSE: its four external
	// actor packages contain bGoalVolume, bRingGoal, RingRadius, bZoneActive and BoxExtent today.
	//
	// So overwriting was silently destroying work: a designer who selected Goal_Volume_Blue_01,
	// dragged its box out by 200 uu and saved got their edit back on the next load — with nothing in
	// the log to say why. Spec v17 §2 asks for verify-and-warn, and this is it:
	//
	//   * shape MISSING (a degenerate, near-zero box, which no real endzone can be) — the level was
	//     baked by a build that could not serialise it. REBUILT, and said out loud. This is the
	//     migration arm and it is what keeps every older map playable.
	//   * shape PRESENT and matching what this builder would make — nothing to do, logged at Verbose.
	//   * shape PRESENT and different — THE PLACED VALUE WINS. Warned, with both numbers, because the
	//     other reason the two can differ is that somebody changed a layout property or a settings
	//     knob (GoalWidthFieldFraction, EndzoneDepth) and the baked map has not been re-baked since.
	//     "The map is stale, re-run Scripts/bake-arena.sh --force" is exactly what that warning means.
	//
	// Note that OwningTeam is never touched either way: it always serialised, and it is what decides
	// which half of the field scores where.
	int32 VolumeCount = 0;
	int32 VolumesRebuilt = 0;
	int32 VolumesDiffering = 0;
	if (HasAuthority())
	{
		const float HalfY = HalfWidth();
		const float Depth = ClampedEndzoneDepth();
		const float Radius = GoalRingRadius();
		const float SlabDepth = FMath::Clamp(GoalRingDepth, 60.f, Depth);

		// 1 uu. Big enough to swallow float round-tripping through a package, far smaller than any
		// edit a person makes by dragging a box handle.
		constexpr float ShapeTolerance = 1.f;

		for (TActorIterator<ATraceEndzone> It(World); It; ++It)
		{
			ATraceEndzone* Zone = *It;
			if (!IsValid(Zone))
			{
				continue;
			}

			const bool bTaggedGoal = Zone->ActorHasTag(FName(TraceArenaBakeLocal::GoalVolumeTag));
			const bool bTaggedEndzone = Zone->ActorHasTag(FName(TraceArenaBakeLocal::EndzoneVolumeTag));

			if (bTaggedGoal || bTaggedEndzone)
			{
				const FVector Expected = bTaggedGoal
					? FVector(SlabDepth * 0.5f, Radius, Radius)
					: FVector(Depth * 0.5f, HalfY, WallHeight * 0.5f);
				const float ExpectedRadius = bTaggedGoal ? Radius : 0.f;

				const UBoxComponent* Box = Zone->Trigger;
				const FVector Placed = (Box != nullptr) ? Box->GetUnscaledBoxExtent() : FVector::ZeroVector;
				const float PlacedRadius = Zone->GetRingRadius();

				// A real endzone is thousands of uu across. Anything this small is not an edit, it is
				// a shape that never made it into the package.
				const bool bShapeUnset = Placed.GetMin() <= ShapeTolerance;

				if (bShapeUnset)
				{
					Zone->ConfigureZone(Zone->OwningTeam, Expected, bTaggedGoal);
					if (bTaggedGoal)
					{
						Zone->ConfigureRing(ExpectedRadius);
					}
					++VolumesRebuilt;

					UE_LOG(LogTraceGame, Display,
						TEXT("[Arena] %s carries no serialised shape (box extent %s), so it was rebuilt from the ")
						TEXT("builder's layout: extent %s%s. That means this level was baked before the shape ")
						TEXT("fields serialised — re-run Scripts/bake-arena.sh --force to make it editable."),
						*Zone->GetName(), *Placed.ToCompactString(), *Expected.ToCompactString(),
						bTaggedGoal ? *FString::Printf(TEXT(", ring radius %.0f"), ExpectedRadius) : TEXT(""));
				}
				else if (!Placed.Equals(Expected, ShapeTolerance)
					|| !FMath::IsNearlyEqual(PlacedRadius, ExpectedRadius, ShapeTolerance)
					|| Zone->IsGoalVolume() != bTaggedGoal)
				{
					++VolumesDiffering;

					// KEPT, NOT FIXED. This is the whole point of the change: the .umap is the
					// authority on a placed actor's shape.
					UE_LOG(LogTraceGame, Warning,
						TEXT("[Arena] %s is placed with a shape this builder would not make, and THE PLACED SHAPE ")
						TEXT("IS BEING KEPT. Placed: extent %s, ring radius %.0f, goal=%d. Builder would make: ")
						TEXT("extent %s, ring radius %.0f, goal=%d. If you edited it deliberately, ignore this. ")
						TEXT("If you did not, the map is stale against a layout or settings change — re-run ")
						TEXT("Scripts/bake-arena.sh --force."),
						*Zone->GetName(), *Placed.ToCompactString(), PlacedRadius, Zone->IsGoalVolume() ? 1 : 0,
						*Expected.ToCompactString(), ExpectedRadius, bTaggedGoal ? 1 : 0);
				}
				else
				{
					UE_LOG(LogTraceGame, Verbose,
						TEXT("[Arena] %s matches the builder's layout; left exactly as placed."), *Zone->GetName());
				}
			}
			else
			{
				// A hand-placed endzone in a baked level. Left exactly as the designer configured it,
				// but still armed/disarmed below, because a volume nobody arms is a volume that scores
				// in both modes.
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Arena] Baked level contains an untagged ATraceEndzone (%s). It is being armed with the ")
					TEXT("rest but its shape is whatever it was placed with, not what this builder would make."),
					*Zone->GetName());
			}

			ScoringVolumes.Add(Zone);
			++VolumeCount;
		}
	}

	// --- The lighting rig and the post-process volume ApplyFidelity re-tunes ----------------------
	for (TActorIterator<APointLight> It(World); It; ++It)
	{
		APointLight* Lamp = *It;
		if (IsValid(Lamp) && Lamp->ActorHasTag(FName(TraceArenaBakeLocal::FloorLampTag)))
		{
			if (UPointLightComponent* LampComponent = Cast<UPointLightComponent>(Lamp->GetLightComponent()))
			{
				FloorLamps.Add(LampComponent);
			}
		}
	}

	// Absolute, not relative, exactly as BuildFloorLamps sets them: ApplyFidelity scales against
	// these, so a re-tune cannot ratchet the lattice brighter every time the quality changes.
	BuiltFloorLampIntensity = FloorLampIntensity;
	BuiltFloorLampRadius = FloorLampRadius;

	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		ADirectionalLight* Light = *It;
		if (IsValid(Light) && Light->ActorHasTag(FName(TraceArenaBakeLocal::KeyLightTag)))
		{
			KeyLightComponent = Cast<UDirectionalLightComponent>(Light->GetLightComponent());
		}
	}

	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		APostProcessVolume* Volume = *It;
		if (IsValid(Volume) && Volume->ActorHasTag(FName(TraceArenaBakeLocal::PostProcessTag)))
		{
			ArenaPostProcess = Volume;
		}
	}

	// Subscribed here rather than in BeginPlay for the same reason BuildArena subscribes where it
	// does: a baked level never runs BuildArena, and a video-settings change still has to re-tune it.
	if (!ScalabilityChangedHandle.IsValid())
	{
		ScalabilityChangedHandle = Scalability::OnScalabilitySettingsChanged.AddWeakLambda(this,
			[this](const Scalability::FQualityLevels&)
			{
				ApplyFidelity();
			});
	}
	ApplyFidelity();

	ApplyScoringMode(ScoringMode);

	// Which Core spawn this level is playing with, resolved and logged HERE rather than left for the
	// GameMode's one-line "Core spawned at ..." — spec v17 §0 rule 1 wants the fallback to say so, and
	// "there is no marker so the old derived point was used" is exactly the sentence it has to say.
	{
		const ATraceCoreSpawn* Marker = FindPlacedCoreSpawn();
		UE_LOG(LogTraceGame, Display,
			TEXT("[Arena] Core spawn: %s, at %s."),
			(Marker != nullptr)
				? TEXT("the PLACED ATraceCoreSpawn in this level")
				: TEXT("DERIVED from the builder's layout (no ATraceCoreSpawn placed — this is the fallback)"),
			*GetCoreSpawnLocation().ToCompactString());
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[Arena] Baked level adopted, nothing built: %d baked pieces (%d endzone-mode, %d goal-mode), ")
		TEXT("%d scoring volumes (%d rebuilt from a shapeless legacy bake, %d kept a placed shape the builder ")
		TEXT("would not make), %d floor lamps, %d side-tinted surfaces promoted to dynamic instances. ")
		TEXT("Field %.0f x %.0f uu, mode %s."),
		PieceCount, BakedEndzoneModeActors.Num(), BakedGoalModeActors.Num(),
		VolumeCount, VolumesRebuilt, VolumesDiffering, FloorLamps.Num(), TintedSurfaces,
		FieldLength, FieldWidth, *TraceScoringModeLabel(ScoringMode));

	if (PieceCount == 0)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Arena] This level is marked pre-baked but contains no ATraceBakedPiece actors. ")
			TEXT("Nothing was built and there is no baked arena either — the level will be empty. ")
			TEXT("Clear bLevelIsPreBaked on the builder, or re-run Scripts/bake-arena.sh."));
	}
}

// -------------------------------------------------------------------------------------------------
// The editor-only half: producing the baked level in the first place.
// -------------------------------------------------------------------------------------------------

#if WITH_EDITOR

void ATraceArenaBuilder::EnsureCoreSpawnActor()
{
	// SPEC v17 §2, CLOSING SPEC v15 §1.4: "The Core spawn is still not a placed actor."
	//
	// Every other gameplay position in the arena became a real actor when the map was baked — the
	// endzone triggers, the goal volumes, the ten team player starts, the whole lighting rig. The
	// Core's did not: it stayed a pure function of the layout properties, so a designer who dragged
	// the centre dais in the baked level left the Core spawning over thin air with nothing in the
	// World Outliner to grab.
	//
	// IDEMPOTENT AND POSITION-PRESERVING. If a marker already exists it is left exactly where it is,
	// which is what makes this safe to run on a level somebody has already edited AND what makes a
	// re-bake keep a moved spawn point. The location used for a NEW marker is whatever
	// GetCoreSpawnLocation() answers right now, which with no marker present is the derived point —
	// so placing it changes nothing about where the Core starts. It only writes the answer down.
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	if (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::EditorPreview)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[Bake] EnsureCoreSpawnActor is an editor-only tool: it places a permanent actor in the ")
			TEXT("level. Run it from Scripts/bake-arena.sh --add-core-spawn, or from the Details panel of a ")
			TEXT("builder in a level with PIE stopped."));
		return;
	}

	if (const ATraceCoreSpawn* Existing = FindPlacedCoreSpawn())
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Bake] This level already has a Core spawn marker ('%s' at %s); left exactly where it is."),
			*Existing->GetActorLabel(), *Existing->GetActorLocation().ToCompactString());
		return;
	}

	const FVector CoreSpawnLocation = GetCoreSpawnLocation();

	FActorSpawnParameters MarkerParams;
	MarkerParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ATraceCoreSpawn* Marker = World->SpawnActor<ATraceCoreSpawn>(
		ATraceCoreSpawn::StaticClass(), CoreSpawnLocation, FRotator::ZeroRotator, MarkerParams);

	if (Marker == nullptr)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[Bake] Could not spawn the ATraceCoreSpawn marker. The level still plays — ")
			TEXT("GetCoreSpawnLocation() falls back to the derived point and says so — but the Core spawn ")
			TEXT("will not be editable in this map."));
		return;
	}

	Marker->Tags.Add(TraceBakedArenaTag());
	Marker->SetActorLabel(TEXT("Core_Spawn"));
	Marker->SetFolderPath(TEXT("Arena/Spawns"));
	PlacedCoreSpawn = Marker;

	UE_LOG(LogTraceGame, Display,
		TEXT("[Bake] Core spawn written down as a placed ATraceCoreSpawn at %s (World Outliner: ")
		TEXT("Arena/Spawns/Core_Spawn). Move that actor and the kickoff, the reset-after-score and the ")
		TEXT("'the Core is home' test all move with it. Save the level to keep it."),
		*CoreSpawnLocation.ToCompactString());
}

void ATraceArenaBuilder::BakeArenaIntoLevel()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// Same gate as the preview buttons, and a sharper reason: this one spawns hundreds of
	// non-transient actors, and doing that into a running match would be unrecoverable.
	if (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::EditorPreview)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[Bake] BakeArenaIntoLevel is an editor-only tool. Run it from Scripts/bake-arena.sh, or from ")
			TEXT("the Details panel of a builder placed in a level with PIE stopped."));
		return;
	}

	if (!GetActorTransform().GetScale3D().Equals(FVector::OneVector, KINDA_SMALL_NUMBER))
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[Bake] The builder is scaled (%s). Every baked piece's transform is derived from this actor's, ")
			TEXT("and the emit path composes rotation and translation only, so a scaled builder would bake a ")
			TEXT("wrong-sized arena. Reset the scale to 1 and bake again."),
			*GetActorTransform().GetScale3D().ToString());
		return;
	}

	// --- 1. The materials must be committed and outside Content/Generated -------------------------
	UMaterialInterface* PromotedSurface = LoadObject<UMaterialInterface>(nullptr,
		*FString::Printf(TEXT("%s/M_TraceSurface.M_TraceSurface"), TraceArenaBakeLocal::PromotedMaterialDir));
	UMaterialInterface* PromotedNeon = LoadObject<UMaterialInterface>(nullptr,
		*FString::Printf(TEXT("%s/M_TraceNeon.M_TraceNeon"), TraceArenaBakeLocal::PromotedMaterialDir));

	if (PromotedSurface == nullptr || PromotedNeon == nullptr)
	{
		// REFUSED, not warned. Spec v15 §1.2 is unambiguous: "nothing the baked map references may
		// live under Content/Generated/". Baking against the generated originals would produce a map
		// that renders correctly on this machine and as grey default material on every other one,
		// which is the worst possible failure because it looks like success here.
		UE_LOG(LogTraceGame, Error,
			TEXT("[Bake] Refusing to bake: %s/M_TraceSurface and M_TraceNeon do not both exist. ")
			TEXT("They are the COMMITTED copies of the two generated materials, and a baked map may not ")
			TEXT("reference Content/Generated/ (it is gitignored, so the map would be broken on every other ")
			TEXT("machine). Scripts/bake-arena.py creates them before calling this; run that instead."),
			TraceArenaBakeLocal::PromotedMaterialDir);
		return;
	}

	// --- 2. Run the real build, transiently, and watch it -----------------------------------------
	DestroyBuiltArena();
	BakeRecords.Reset();

	bBakingToLevel = true;
	bBuildingEditorPreview = true;   // components and MIDs stay transient; only the actors persist
	BuildArena();
	bBuildingEditorPreview = false;
	bBakingToLevel = false;

	if (BakeRecords.Num() == 0)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[Bake] The build recorded nothing. Nothing was emitted."));
		DestroyBuiltArena();
		return;
	}

	// --- 3. One committed material instance per distinct tint the build asked for -----------------
	//
	// MIDs are made per BUILD STEP and handed to every block in it (see the pool note in the header),
	// so this is a few dozen assets, not fourteen hundred.
	TMap<TWeakObjectPtr<UMaterialInstanceDynamic>, UMaterialInterface*> BakedMaterials;
	TSet<FString> UsedAssetNames;
	int32 MaterialsWritten = 0;

	for (const FTraceBakeRecord& Record : BakeRecords)
	{
		UMaterialInstanceDynamic* MID = Record.BlockMID.Get();
		if (MID == nullptr || BakedMaterials.Contains(Record.BlockMID))
		{
			continue;
		}

		// Which of the two generated materials this tint is an instance of. Anything else is the
		// BasicShapeMaterial fallback path, which is an engine asset and needs no promotion.
		UMaterialInterface* NewParent = nullptr;
		const TCHAR* Prefix = TEXT("MI_Trace");
		if (MID->Parent == SurfaceMaterial)
		{
			NewParent = PromotedSurface;
			Prefix = TEXT("MI_Surface");
		}
		else if (MID->Parent == NeonMaterial)
		{
			NewParent = PromotedNeon;
			Prefix = TEXT("MI_Neon");
		}

		if (NewParent == nullptr)
		{
			BakedMaterials.Add(Record.BlockMID, MID->Parent);
			continue;
		}

		FString AssetName = FString::Printf(TEXT("%s_%s"), Prefix,
			*TraceArenaBakeLocal::SanitiseAssetName(TraceArenaBakeLocal::PrettyPieceName(Record.PieceName)));
		int32 Suffix = 2;
		while (UsedAssetNames.Contains(AssetName))
		{
			AssetName = FString::Printf(TEXT("%s_%s_%02d"), Prefix,
				*TraceArenaBakeLocal::SanitiseAssetName(TraceArenaBakeLocal::PrettyPieceName(Record.PieceName)), Suffix++);
		}
		UsedAssetNames.Add(AssetName);

		const FString PackageName = FString::Printf(TEXT("%s/%s"), TraceArenaBakeLocal::BakedInstanceDir, *AssetName);
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

		// RE-BAKE, AND THE CRASH IT CAUSED. The obvious code here is CreatePackage + NewObject, and
		// it works exactly once. On the second bake the asset already exists on disk, so
		// CreatePackage hands back the STUB package the asset registry has for it - present but only
		// partially loaded - and SavePackage's validator turns that into a fatal error:
		// "Asset ... cannot be saved as it has only been partially loaded", i.e. an appError and a
		// SIGSEGV in the middle of the bake. So: load the existing asset if there is one and
		// overwrite it in place, and only create when there is genuinely nothing there.
		UMaterialInstanceConstant* Committed = LoadObject<UMaterialInstanceConstant>(
			nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);

		UPackage* Package = nullptr;
		if (Committed != nullptr)
		{
			Package = Committed->GetPackage();
			Package->FullyLoad();
		}
		else
		{
			Package = CreatePackage(*PackageName);
			if (Package != nullptr)
			{
				// FullyLoad on a brand-new package is a no-op; on one the registry half-knows about
				// it is the difference between saving and crashing.
				Package->FullyLoad();
				Committed = NewObject<UMaterialInstanceConstant>(Package, FName(*AssetName), RF_Public | RF_Standalone);
			}
		}

		if (Package == nullptr || Committed == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[Bake] Could not create or load %s."), *ObjectPath);
			continue;
		}

		Committed->SetParentEditorOnly(NewParent);

		// COPIED WHOLESALE rather than replayed parameter by parameter. A MID stores its overrides in
		// exactly these two arrays, so this is the complete set by construction - and a hand-written
		// list of parameter names here would silently go stale the day MakeSurfaceMID gains a
		// parameter, which is precisely how the arena would come out of the bake the wrong colour.
		Committed->ScalarParameterValues = MID->ScalarParameterValues;
		Committed->VectorParameterValues = MID->VectorParameterValues;
		Committed->PostEditChange();
		Committed->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, Committed, *FileName, SaveArgs))
		{
			UE_LOG(LogTraceGame, Error, TEXT("[Bake] Failed to save %s."), *FileName);
		}
		else
		{
			++MaterialsWritten;
		}

		BakedMaterials.Add(Record.BlockMID, Committed);
	}

	// --- 4. Group the flat record list back into logical pieces -----------------------------------
	//
	// THE PROBLEM, PRECISELY. The build emits blocks, not things: ATraceArenaBuilder::AddNeonBlock
	// makes twelve components for one cover block and passes the same debug name to all of them, and
	// the next cover block passes that same name again. So neither the name alone nor the order alone
	// says where one piece ends and the next begins.
	//
	// THE RULE, AND WHY IT IS THIS ONE. A new piece starts when the debug name changes, when the
	// scoring-mode tag changes, or when the block's footprint leaves the piece being built (inflated
	// by BakePieceGroupSlack). That works because a piece's parts all sit inside its own footprint -
	// the widest is the pawn standoff shell at 26 uu proud on each side, and the corner ribs are
	// centred exactly on the corners - while the next piece with the same name is thousands of uu
	// away. The alternative was threading a "which piece am I" argument through forty-odd call sites
	// in a 7,400-line file, which is more code, more risk, and something a later edit would forget.
	TArray<TraceArenaBakeLocal::FBakePieceGroup> Groups;
	TArray<int32> LampRecords;

	// Axis-aligned local-space bound of one recorded block. The rotation of a yawed block is ignored
	// on purpose: the grouping test wants a conservative bound, and the emit step uses the exact
	// transform anyway.
	auto BlockBounds = [](const FTraceBakeRecord& Record) -> FBox
	{
		FVector Centre = FVector::ZeroVector;
		FVector Extent = FVector::ZeroVector;

		if (Record.Kind == FTraceBakeRecord::EKind::MeshBlock)
		{
			// The engine basic shapes are 100 uu across and centred on their pivot, so scale x 50 is
			// the half size.
			Centre = Record.Transform.GetTranslation();
			Extent = Record.Transform.GetScale3D().GetAbs() * 50.f;
		}
		else if (const USceneComponent* Source = Record.SourceComponent.Get())
		{
			Centre = Source->GetRelativeLocation();
			if (const UBoxComponent* AsBox = Cast<UBoxComponent>(Source))
			{
				Extent = AsBox->GetUnscaledBoxExtent();
			}
		}

		return FBox(Centre - Extent, Centre + Extent);
	};

	for (int32 Index = 0; Index < BakeRecords.Num(); ++Index)
	{
		const FTraceBakeRecord& Record = BakeRecords[Index];

		// Lights are actors of their own, not components on a piece - see the emit step.
		if (Record.Kind == FTraceBakeRecord::EKind::PointLight)
		{
			LampRecords.Add(Index);
			continue;
		}

		const FBox BlockBox = BlockBounds(Record);
		const FBox2D Footprint(FVector2D(BlockBox.Min.X, BlockBox.Min.Y), FVector2D(BlockBox.Max.X, BlockBox.Max.Y));

		TraceArenaBakeLocal::FBakePieceGroup* Current = (Groups.Num() > 0) ? &Groups.Last() : nullptr;
		const bool bContinues = (Current != nullptr)
			&& Current->PieceName == Record.PieceName
			&& Current->ModeTag == Record.ModeTag
			&& Current->Records.Num() < TraceArenaBakeLocal::MaxBlocksPerPiece
			&& Current->Footprint.ExpandBy(BakePieceGroupSlack).IsInside(Footprint.GetCenter());

		if (!bContinues)
		{
			Current = &Groups.AddDefaulted_GetRef();
			Current->PieceName = Record.PieceName;
			Current->ModeTag = Record.ModeTag;
			Current->Footprint = Footprint;
		}
		else
		{
			Current->Footprint += Footprint;
		}

		Current->Records.Add(Index);
		Current->LocalBounds += BlockBox;
	}

	// --- 5. Which names are worth one actor per piece, and which are one actor per run ------------
	//
	// Spec v15 §1.3's [ASSUMPTION], applied: "collapse runs of identical instanced pieces into one
	// actor per instance only where the instance count is sane". Nobody is ever going to click one of
	// the 5 uu shells that make the wall cove read as curved, and there are thousands of them.
	TMap<FName, int32> GroupsPerName;
	for (const TraceArenaBakeLocal::FBakePieceGroup& Group : Groups)
	{
		GroupsPerName.FindOrAdd(Group.PieceName) += 1;
	}

	TSet<FName> CollapsedNames;
	for (const TPair<FName, int32>& Pair : GroupsPerName)
	{
		if (Pair.Value > BakeMaxPiecesPerName)
		{
			CollapsedNames.Add(Pair.Key);
		}
	}

	// --- 6. Emit ----------------------------------------------------------------------------------
	const FTransform BuilderTransform = GetActorTransform();
	const FQuat BuilderRotation = BuilderTransform.GetRotation();

	TMap<FName, int32> LabelCounters;
	int32 PieceActors = 0;
	int32 MeshComponentsEmitted = 0;
	int32 InstancedComponentsEmitted = 0;
	int32 CollisionComponentsEmitted = 0;

	FActorSpawnParameters PieceParams;
	PieceParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	for (const TraceArenaBakeLocal::FBakePieceGroup& Group : Groups)
	{
		const FString PrettyName = TraceArenaBakeLocal::PrettyPieceName(Group.PieceName);
		const bool bCollapse = CollapsedNames.Contains(Group.PieceName);

		// Pivot: centre of the footprint, base of the piece. Dragging a wall by its foot is what a
		// person expects; dragging it by an arbitrary interior point is what a script produces.
		const FVector PivotLocal(
			Group.Footprint.GetCenter().X,
			Group.Footprint.GetCenter().Y,
			Group.LocalBounds.Min.Z);

		const FTransform PieceTransform(BuilderRotation, BuilderTransform.TransformPosition(PivotLocal), FVector::OneVector);

		ATraceBakedPiece* Piece = World->SpawnActor<ATraceBakedPiece>(ATraceBakedPiece::StaticClass(), PieceTransform, PieceParams);
		if (Piece == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[Bake] Failed to spawn a piece actor for %s."), *PrettyName);
			continue;
		}

		++PieceActors;
		Piece->ScoringModeTag = Group.ModeTag;
		Piece->Tags.Add(TraceBakedArenaTag());

		int32& Counter = LabelCounters.FindOrAdd(Group.PieceName);
		++Counter;
		Piece->SetActorLabel(FString::Printf(TEXT("%s_%02d"), *PrettyName, Counter));
		Piece->SetFolderPath(FName(*FString::Printf(TEXT("Arena/%s"), *TraceArenaBakeLocal::PieceCategory(PrettyName))));

		// Batch the piece's blocks by (mesh, material, shadow), exactly as the runtime pools do, so
		// the emit step can decide per key whether repetition is worth an instanced component.
		TArray<TraceArenaBakeLocal::FBakeInstanceKey> Keys;
		TArray<TArray<FTransform>> KeyTransforms;

		for (const int32 RecordIndex : Group.Records)
		{
			const FTraceBakeRecord& Record = BakeRecords[RecordIndex];

			if (Record.Kind == FTraceBakeRecord::EKind::MeshBlock)
			{
				UStaticMesh* BlockMesh = Record.BlockMesh.Get();
				if (BlockMesh == nullptr)
				{
					continue;
				}

				UMaterialInterface* const* Found = BakedMaterials.Find(Record.BlockMID);
				TraceArenaBakeLocal::FBakeInstanceKey Key;
				Key.KeyMesh = BlockMesh;
				Key.KeyMaterial = (Found != nullptr) ? *Found : nullptr;
				Key.bCastShadow = Record.bCastShadow;

				int32 KeyIndex = Keys.IndexOfByKey(Key);
				if (KeyIndex == INDEX_NONE)
				{
					KeyIndex = Keys.Add(Key);
					KeyTransforms.AddDefaulted();
				}

				FTransform Relative = Record.Transform;
				Relative.SetTranslation(Record.Transform.GetTranslation() - PivotLocal);
				KeyTransforms[KeyIndex].Add(Relative);
				continue;
			}

			// Collision boxes and pawn standoff shells: copied component-for-component, INCLUDING
			// their per-channel responses. That is what preserves BuildCentreDais's pedestal, whose
			// box deliberately ignores ECC_Pawn so the middle of the field is not a thing you bounce
			// off - a state no amount of re-deriving from the record would have recovered.
			const UBoxComponent* Source = Cast<UBoxComponent>(Record.SourceComponent.Get());
			if (Source == nullptr)
			{
				continue;
			}

			const FString ComponentName = (Record.Kind == FTraceBakeRecord::EKind::PawnStandoff)
				? FString::Printf(TEXT("%s_Standoff"), *PrettyName)
				: FString::Printf(TEXT("%s_Collision"), *PrettyName);

			UBoxComponent* Box = NewObject<UBoxComponent>(Piece,
				MakeUniqueObjectName(Piece, UBoxComponent::StaticClass(), FName(*ComponentName)));
			if (Box == nullptr)
			{
				continue;
			}

			Box->SetMobility(EComponentMobility::Static);
			Box->SetupAttachment(Piece->PieceRoot);
			Box->SetRelativeLocation(Source->GetRelativeLocation() - PivotLocal);
			Box->SetRelativeRotation(Source->GetRelativeRotation());
			Box->SetBoxExtent(Source->GetUnscaledBoxExtent(), /*bUpdateOverlaps=*/false);
			Box->SetCollisionProfileName(Source->GetCollisionProfileName());
			Box->SetCollisionEnabled(Source->GetCollisionEnabled());
			Box->SetCollisionObjectType(Source->GetCollisionObjectType());
			Box->SetCollisionResponseToChannels(Source->GetCollisionResponseToChannels());
			Box->SetGenerateOverlapEvents(false);
			Box->SetCanEverAffectNavigation(false);
			Box->SetHiddenInGame(true);
			Box->RegisterComponent();
			Piece->AddInstanceComponent(Box);
			++CollisionComponentsEmitted;
		}

		for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
		{
			const TraceArenaBakeLocal::FBakeInstanceKey& Key = Keys[KeyIndex];
			const TArray<FTransform>& Transforms = KeyTransforms[KeyIndex];
			const bool bInstance = bCollapse || Transforms.Num() >= TraceArenaBakeLocal::InstanceCollapseThreshold;

			if (bInstance)
			{
				UInstancedStaticMeshComponent* Pool = NewObject<UInstancedStaticMeshComponent>(Piece,
					MakeUniqueObjectName(Piece, UInstancedStaticMeshComponent::StaticClass(), FName(*PrettyName)));
				if (Pool == nullptr)
				{
					continue;
				}

				Pool->SetMobility(EComponentMobility::Static);
				Pool->SetupAttachment(Piece->PieceRoot);
				Pool->SetStaticMesh(Key.KeyMesh);
				Pool->SetCollisionProfileName(TEXT("NoCollision"));
				Pool->SetGenerateOverlapEvents(false);
				Pool->SetCanEverAffectNavigation(false);
				Pool->SetCastShadow(Key.bCastShadow);
				if (Key.KeyMaterial != nullptr)
				{
					Pool->SetMaterial(0, Key.KeyMaterial);
				}
				Pool->RegisterComponent();
				for (const FTransform& Relative : Transforms)
				{
					Pool->AddInstance(Relative, /*bWorldSpace=*/false);
				}
				Piece->AddInstanceComponent(Pool);
				++InstancedComponentsEmitted;
				continue;
			}

			for (const FTransform& Relative : Transforms)
			{
				UStaticMeshComponent* Block = NewObject<UStaticMeshComponent>(Piece,
					MakeUniqueObjectName(Piece, UStaticMeshComponent::StaticClass(), FName(*PrettyName)));
				if (Block == nullptr)
				{
					continue;
				}

				Block->SetMobility(EComponentMobility::Static);
				Block->SetupAttachment(Piece->PieceRoot);
				Block->SetStaticMesh(Key.KeyMesh);
				Block->SetRelativeTransform(Relative);
				Block->SetCollisionProfileName(TEXT("NoCollision"));
				Block->SetGenerateOverlapEvents(false);
				Block->SetCanEverAffectNavigation(false);
				Block->SetCastShadow(Key.bCastShadow);
				if (Key.KeyMaterial != nullptr)
				{
					Block->SetMaterial(0, Key.KeyMaterial);
				}
				Block->RegisterComponent();
				Piece->AddInstanceComponent(Block);
				++MeshComponentsEmitted;
			}
		}

		// Which of this piece's materials the half-time switch repaints. Recorded per PIECE rather
		// than globally so a designer can delete a piece and take its repaint entry with it.
		for (const int32 RecordIndex : Group.Records)
		{
			const FTraceBakeRecord& Record = BakeRecords[RecordIndex];
			UMaterialInstanceDynamic* MID = Record.BlockMID.Get();
			if (MID == nullptr)
			{
				continue;
			}

			UMaterialInterface* const* Found = BakedMaterials.Find(Record.BlockMID);
			if (Found == nullptr || *Found == nullptr)
			{
				continue;
			}

			for (const FTraceSideMID& Side : SideMIDs)
			{
				if (Side.MID.Get() != MID)
				{
					continue;
				}

				const bool bAlready = Piece->SideTints.ContainsByPredicate(
					[Found](const FTraceBakedSideTint& Existing) { return Existing.Material == *Found; });
				if (bAlready)
				{
					break;
				}

				FTraceBakedSideTint& Tint = Piece->SideTints.AddDefaulted_GetRef();
				Tint.Material = *Found;
				Tint.EndSign = Side.EndSign;
				Tint.bNeon = Side.bNeon;
				Tint.Intensity = Side.Intensity;
				Tint.BaseDim = Side.BaseDim;
				break;
			}
		}
	}

	// --- 7. The floor-lamp lattice, as real placed lights -----------------------------------------
	int32 LampActors = 0;
	for (const int32 RecordIndex : LampRecords)
	{
		const UPointLightComponent* Source = Cast<UPointLightComponent>(BakeRecords[RecordIndex].SourceComponent.Get());
		if (Source == nullptr)
		{
			continue;
		}

		const FVector WorldLocation = BuilderTransform.TransformPosition(Source->GetRelativeLocation());
		APointLight* Lamp = World->SpawnActor<APointLight>(APointLight::StaticClass(), WorldLocation, FRotator::ZeroRotator, PieceParams);
		if (Lamp == nullptr)
		{
			continue;
		}

		if (UPointLightComponent* LampComponent = Cast<UPointLightComponent>(Lamp->GetLightComponent()))
		{
			// Movable for the same reason every light this project makes is: static lighting is
			// disabled outright (r.AllowStaticLighting=False) and nothing here has baked data.
			LampComponent->SetMobility(EComponentMobility::Movable);
			LampComponent->SetIntensity(Source->Intensity);
			LampComponent->SetLightColor(Source->GetLightColor());
			LampComponent->SetAttenuationRadius(Source->AttenuationRadius);
			LampComponent->SetCastShadows(false);
			LampComponent->SetVolumetricScatteringIntensity(0.f);
		}

		Lamp->Tags.Add(TraceBakedArenaTag());
		Lamp->Tags.Add(FName(TraceArenaBakeLocal::FloorLampTag));
		Lamp->SetActorLabel(FString::Printf(TEXT("Floor_Lamp_%02d"), ++LampActors));
		Lamp->SetFolderPath(TEXT("Arena/Lighting"));
	}

	// --- 7b. The Core spawn, as a real placed actor  (spec v17 §2, closing spec v15 §1.4) ---------
	//
	// Every other gameplay position in the arena is an actor in the saved level: the endzone
	// triggers, the goal volumes, the team player starts. The Core's was not — it was a pure
	// function of the layout properties, so moving the centre dais in the baked level left the Core
	// spawning over thin air with nothing in the outliner to drag.
	//
	// Emitted at whatever GetCoreSpawnLocation() answers RIGHT NOW, which on a fresh bake is the
	// derived point. So a bake changes nothing about where the Core starts; it just writes the answer
	// down where a person can move it. Re-baking a level that already has a marker picks the marker's
	// own position back up, which is exactly what a designer expects a re-bake to preserve.
	EnsureCoreSpawnActor();
	const int32 CoreSpawnActors = (FindPlacedCoreSpawn() != nullptr) ? 1 : 0;

	// --- 8. Label and tag the gameplay actors the build already spawned for real ------------------
	//
	// These are ordinary spawned actors that SpawnedActorFlags() made non-transient for this run, so
	// they are already in the level and will serialise with it. All that is left is to make them
	// findable (by AdoptBakedArena) and readable (by a person opening the outliner).
	TMap<FName, int32> ActorCounters;
	int32 GameplayActors = 0;

	for (const TObjectPtr<AActor>& Spawned : SpawnedActors)
	{
		AActor* Actor = Spawned.Get();
		if (!IsValid(Actor))
		{
			continue;
		}

		++GameplayActors;
		Actor->Tags.Add(TraceBakedArenaTag());

		FString Label;
		if (ATraceEndzone* Zone = Cast<ATraceEndzone>(Actor))
		{
			const bool bGoal = Zone->IsGoalVolume();
			Zone->Tags.Add(FName(bGoal ? TraceArenaBakeLocal::GoalVolumeTag : TraceArenaBakeLocal::EndzoneVolumeTag));
			Label = FString::Printf(TEXT("%s_%s"), bGoal ? TEXT("Goal_Volume") : TEXT("Endzone_Volume"),
				*TraceTeamName(Zone->OwningTeam).ToString());
			Actor->SetFolderPath(TEXT("Arena/Scoring"));
		}
		else if (const ATraceTeamPlayerStart* Start = Cast<ATraceTeamPlayerStart>(Actor))
		{
			Label = FString::Printf(TEXT("PlayerStart_%s"), *TraceTeamName(Start->Team).ToString());
			Actor->SetFolderPath(TEXT("Arena/Spawns"));
		}
		else
		{
			// The lighting rig. "DirectionalLight_01" is a class name with a number after it, which
			// is exactly the failure spec v15 §1.3 calls out, so each of these gets a name that says
			// what it DOES. The four directional lights are otherwise indistinguishable actors, so
			// they carry the name BuildLighting's FLightSpec table gave them, as a tag.
			Label = Actor->GetClass()->GetName();
			Label.RemoveFromStart(TEXT("A"));

			if (Actor->IsA<ADirectionalLight>())
			{
				for (const FName& Tag : Actor->Tags)
				{
					if (Tag != TraceBakedArenaTag())
					{
						Label = TraceArenaBakeLocal::PrettyPieceName(Tag);
						break;
					}
				}
			}
			else if (Actor->IsA<ASkyAtmosphere>())  { Label = TEXT("Sky_Atmosphere"); }
			else if (Actor->IsA<ASkyLight>())       { Label = TEXT("Sky_Light"); }
			else if (Actor->IsA<AExponentialHeightFog>()) { Label = TEXT("Height_Fog"); }

			Actor->SetFolderPath(TEXT("Arena/Lighting"));
		}

		int32& Counter = ActorCounters.FindOrAdd(FName(*Label));
		++Counter;
		Actor->SetActorLabel(FString::Printf(TEXT("%s_%02d"), *Label, Counter));
	}

	// The two pieces of the rig ApplyFidelity drives by pointer rather than by search. Tagged from
	// the handles the build itself set, so "which directional light is the key light" is answered by
	// the code that decided it rather than guessed at load.
	if (const UDirectionalLightComponent* Key = KeyLightComponent.Get())
	{
		if (AActor* KeyOwner = Key->GetOwner())
		{
			KeyOwner->Tags.Add(FName(TraceArenaBakeLocal::KeyLightTag));
			KeyOwner->SetActorLabel(TEXT("Key_Light"));
		}
	}
	if (APostProcessVolume* Volume = ArenaPostProcess.Get())
	{
		Volume->Tags.Add(FName(TraceArenaBakeLocal::PostProcessTag));
		Volume->SetActorLabel(TEXT("Arena_PostProcess"));
	}

	// --- 9. Hand the level over -------------------------------------------------------------------
	//
	// SpawnedActors is CLEARED WITHOUT DESTROYING, and the order matters: DestroyBuiltArena below
	// destroys everything in it, which is exactly right for a preview and exactly wrong here - those
	// actors are the baked level's endzones, player starts and lights.
	SpawnedActors.Reset();

	// Read BEFORE the teardown, which empties BakeRecords. The first version of this logged the
	// count afterwards and cheerfully reported "from 0 recorded blocks", which is the kind of log
	// line that makes a reader distrust every other number next to it.
	const int32 RecordedBlocks = BakeRecords.Num();
	DestroyBuiltArena();

	bLevelIsPreBaked = true;
	Tags.AddUnique(TraceBakedArenaTag());
	SetActorLabel(TEXT("Arena_Builder"));
	SetFolderPath(TEXT("Arena"));

	UE_LOG(LogTraceGame, Display,
		TEXT("[Bake] Arena baked into the current level: %d piece actors + %d floor lamps + %d gameplay actors ")
		TEXT("+ %d Core spawn + this builder = %d actors, from %d recorded blocks. Components: %d static meshes, ")
		TEXT("%d instanced batches, %d collision/standoff boxes. Materials: %d committed instances under %s ")
		TEXT("(parents: %s)."),
		PieceActors, LampActors, GameplayActors, CoreSpawnActors,
		PieceActors + LampActors + GameplayActors + CoreSpawnActors + 1,
		RecordedBlocks, MeshComponentsEmitted, InstancedComponentsEmitted, CollisionComponentsEmitted,
		MaterialsWritten, TraceArenaBakeLocal::BakedInstanceDir, TraceArenaBakeLocal::PromotedMaterialDir);

	for (const FName& Collapsed : CollapsedNames)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[Bake] '%s' produced %d pieces, over the %d limit, so each of them batches ALL its blocks ")
			TEXT("into instanced components rather than loose static meshes. They are still %d separate, ")
			TEXT("individually clickable actors — only their sub-blocks are batched."),
			*TraceArenaBakeLocal::PrettyPieceName(Collapsed), GroupsPerName[Collapsed], BakeMaxPiecesPerName,
			GroupsPerName[Collapsed]);
	}

	// The per-name census, because "517 actors" is the first question and "of what" is the second,
	// and because it is the only way to tell whether the grouping rule above did something sensible.
	GroupsPerName.ValueSort([](int32 Left, int32 Right) { return Left > Right; });
	int32 Listed = 0;
	for (const TPair<FName, int32>& Pair : GroupsPerName)
	{
		if (Listed++ >= 20)
		{
			break;
		}
		UE_LOG(LogTraceGame, Display, TEXT("[Bake]   %-28s %d"),
			*TraceArenaBakeLocal::PrettyPieceName(Pair.Key), Pair.Value);
	}

	BakeRecords.Reset();
}

#endif // WITH_EDITOR
