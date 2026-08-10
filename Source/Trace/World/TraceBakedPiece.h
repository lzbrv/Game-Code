// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/ObjectPtr.h"

#include "TraceBakedPiece.generated.h"

class UMaterialInterface;

/**
 * Which scoring shape a baked piece belongs to.
 *
 * The procedural arena builds BOTH shapes and presents one (see the two-modes note in
 * TraceArenaBuilder.h). The bake has to preserve that, and it cannot do it the way the runtime
 * builder does - by remembering component pointers in FTraceModePiece arrays - because the pieces
 * are now separate actors serialised into a .umap. So the tag travels WITH the actor, and
 * ATraceArenaBuilder::AdoptBakedArena rebuilds the two lists from it on load.
 */
UENUM()
enum class ETraceBakedScoringTag : uint8
{
	/** Present in both modes. Almost everything: floor, walls, cover, banks, flanks. */
	Always,

	/** Mode A only: the full-width endzone paint, the goal line, the solid end wall, the gate. */
	EndzoneModeOnly,

	/** Mode B only: the perforated end wall, the ring, the alcove and the approach ramp. */
	GoalModeOnly
};

/**
 * One surface on a baked piece whose colour follows the half-time side switch.
 *
 * WHY THE KEY IS THE MATERIAL AND NOT A COMPONENT INDEX. The runtime arena repaints by pushing
 * parameters into the MIDs it built (ATraceArenaBuilder::ApplyTeamSides); a baked piece wears
 * committed MaterialInstanceConstants instead, which cannot be repainted. AdoptBakedArena therefore
 * has to promote them back to MIDs at load, and it needs to know WHICH components to promote.
 * Keying on the material asset rather than on a component index means a designer can add, delete or
 * reorder components on a baked piece - which is the entire point of baking it - without silently
 * repainting the wrong surface at half time.
 */
USTRUCT()
struct FTraceBakedSideTint
{
	GENERATED_BODY()

	/** The committed instance this surface wears. Every component using it is promoted to a MID. */
	UPROPERTY(EditAnywhere, Category = "Trace|Baked")
	TObjectPtr<UMaterialInterface> Material;

	/** Which end this surface belongs to: -1 or +1 along X. Matches FTraceSideMID::EndSign. */
	UPROPERTY(EditAnywhere, Category = "Trace|Baked")
	float EndSign = 0.f;

	/** Neon MIDs take Color/Glow; surface MIDs take BaseColor/Emissive. Different pushes. */
	UPROPERTY(EditAnywhere, Category = "Trace|Baked")
	bool bNeon = false;

	/** Glow for a neon MID, or emissive strength for a surface MID. */
	UPROPERTY(EditAnywhere, Category = "Trace|Baked")
	float Intensity = 0.f;

	/** Surface MIDs only: how far the base colour is dimmed from the team colour. */
	UPROPERTY(EditAnywhere, Category = "Trace|Baked")
	float BaseDim = 0.f;
};

/**
 * ONE EDITABLE PIECE OF THE BAKED ARENA — spec v15 §1.
 *
 * WHAT IT IS FOR. `/Game/Maps/Arena_Baked` is the procedural arena run once and written down. The
 * thing that makes it worth writing down is that a designer can click a wall and move it, and that
 * is exactly what this class buys: every logical piece of the arena (a wall, a cover block, a pylon,
 * a terrace) is one of these actors, with a readable label, its own transform, and its own
 * components — the dark body, the neon trim, the collision box, the pawn standoff shell. Drag it and
 * all four move together, which is what "a wall" means to a person.
 *
 * WHY NOT AStaticMeshActor. A static mesh actor is exactly one mesh and no collision box. Almost
 * nothing in this arena is exactly one mesh: ATraceArenaBuilder::AddNeonBlock alone emits a body, a
 * lip, a skirt, a mid band and eight corner ribs, plus a BlockAll box and a pawn-only standoff
 * shell — twelve components that are one THING. Baking those as twelve static mesh actors would
 * technically be "exploded" and would in practice be unusable: moving a cover block would mean
 * box-selecting twelve actors and hoping you got the standoff shell.
 *
 * SO IT IS DELIBERATELY A PLAIN ACTOR with instance components. Everything the bake attaches is
 * added through AddInstanceComponent, which is the same route the editor's own "Add Component"
 * button takes, so the components serialise with the actor, appear in the Details panel, and can be
 * individually selected, retargeted or deleted.
 *
 * TWO PIECES OF STATE TRAVEL WITH THE ACTOR because the runtime builder can no longer hold them in
 * memory across a save: ScoringModeTag (which of the A/B shapes this belongs to) and SideTints (which
 * of its materials the half-time switch repaints). ATraceArenaBuilder::AdoptBakedArena reads both
 * back on load — see the skip-the-build note on ATraceArenaBuilder::bLevelIsPreBaked.
 *
 * ==================================================================================================
 * THE ONE THING THIS IS NOT: YOU CANNOT SELECT A SINGLE BLOCK INSIDE A PIECE  (spec v17 §2)
 * ==================================================================================================
 * MEASURED, on the shipped /Game/Maps/Arena_Baked: of 835 blocks emitted, 659 are INSTANCES inside a
 * UInstancedStaticMeshComponent rather than components of their own. Every piece is still its own
 * clickable, labelled, movable actor — that part is real and it is what the bake was for — but the
 * repetition INSIDE a piece is batched, so you can grab "Bank_Terrace_04" and you cannot grab tile 12
 * of it.
 *
 * WHY, AND IT IS NOT AN OVERSIGHT. Spec v7 §8 rebuilt the arena on instanced meshes because ~1,450
 * loose static mesh components cost 1.8–3.1 ms of game thread in draw submission alone. Baking every
 * block back out as its own component would hand all of that straight back, on a frame this project
 * has repeatedly measured as tight. So the bake splits the difference: a piece's DISTINCT parts (the
 * dark body, the glowing lip, the collision box) stay as their own named components you can click,
 * and its REPETITION (the eight corner ribs of a cover block, the hundreds of 5 uu shells that make
 * the wall cove read as curved) collapses into one instanced component. The threshold is four; see
 * InstanceCollapseThreshold in TraceArenaBake.cpp.
 *
 * WHAT TO DO INSTEAD, in rough order of how often you will want it:
 *   * Move / rotate / delete the WHOLE PIECE — works, that is the normal edit.
 *   * Change what a batch is made of — select the UInstancedStaticMeshComponent in the Details panel
 *     and change its Static Mesh or its Material. Every instance in it follows.
 *   * Move ONE instance — expand the instanced component's Instances array in the Details panel and
 *     edit that entry's transform. Fiddly, but it is there.
 *   * Need real per-block actors for a section? Raise BakeMaxPiecesPerName on the builder, or lower
 *     InstanceCollapseThreshold, and re-bake. Expect the frame cost back.
 *
 * EVERY PIECE REPORTS ITS OWN SITUATION in the Details panel — see EditingNote below, which is where
 * a designer actually meets this rather than in a spec nobody opens.
 */
UCLASS()
class TRACE_API ATraceBakedPiece : public AActor
{
	GENERATED_BODY()

public:
	ATraceBakedPiece();

	/**
	 * Attach parent for every baked component. Named PieceRoot, not Root: AActor has no member of
	 * that name, but ATraceArenaBuilder does, and this project has broken the Windows build four
	 * times on MSVC C4458 shadowing. A distinct name costs nothing and cannot be the next one.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Baked")
	TObjectPtr<USceneComponent> PieceRoot;

	/** Which scoring shape presents this piece. Always for everything that is not endzone furniture. */
	UPROPERTY(EditAnywhere, Category = "Trace|Baked")
	ETraceBakedScoringTag ScoringModeTag = ETraceBakedScoringTag::Always;

	/** Surfaces on this piece that the half-time side switch repaints. Usually empty. */
	UPROPERTY(EditAnywhere, Category = "Trace|Baked")
	TArray<FTraceBakedSideTint> SideTints;

#if WITH_EDITORONLY_DATA
	/**
	 * WHAT YOU CAN AND CANNOT CLICK ON THIS PIECE, in plain words, in the Details panel.
	 *
	 * THIS IS THE ANSWER TO SPEC v17 §2's "this is written down nowhere". The batching caveat above
	 * used to live only in a C++ comment and a spec document, so the first person to discover it was
	 * whoever spent twenty minutes trying to select one tile of a terrace. Now the piece says so
	 * itself, at the moment you select it, with ITS OWN numbers rather than the arena's average.
	 *
	 * Transient and rebuilt from the actual components every time the actor registers, so it can
	 * never go stale against an edit and it never dirties the package — you will not be asked to save
	 * a level just because you looked at it.
	 */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Trace|Baked",
		meta = (DisplayName = "Editing Note", MultiLine = true))
	FString EditingNote;
#endif

	//~ AActor
	virtual void PostRegisterAllComponents() override;
	//~ End AActor

#if WITH_EDITOR
	/** Recomputes EditingNote from the components actually on this actor. Editor only. */
	void RefreshEditingNote();
#endif
};
