// Copyright (c) Trace. All Rights Reserved.

#include "World/TraceBakedPiece.h"

#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"

ATraceBakedPiece::ATraceBakedPiece()
{
	PrimaryActorTick.bCanEverTick = false;

	// NOT replicated, and that is the same decision the procedural arena already makes for its
	// geometry: every machine loads the same .umap, so the pieces are already identical everywhere
	// and replicating a thousand static actors would buy nothing and cost real bandwidth. The
	// gameplay actors in the baked level (endzones, player starts) keep whatever replication their
	// own classes ask for.
	bReplicates = false;
	SetCanBeDamaged(false);

	PieceRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PieceRoot"));
	SetRootComponent(PieceRoot);

	// STATIC for exactly the reason ATraceArenaBuilder's root is (see the long note in its
	// constructor): a component may not be less mobile than its attach parent, so a Movable root here
	// would force every baked mesh below it into the renderer's dynamic half and every baked collision
	// box into the physics scene's dynamic broadphase. Nothing in the arena moves.
	//
	// This does NOT stop a designer dragging the actor in the editor — the Static restriction is on
	// moving a component in a GAME world, not on an editor transform.
	PieceRoot->SetMobility(EComponentMobility::Static);
}

void ATraceBakedPiece::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

#if WITH_EDITOR
	// After registration, not in PostLoad: instance components are attached and registered by then,
	// so counting them here cannot under-report a piece that is still being assembled.
	RefreshEditingNote();
#endif
}

#if WITH_EDITOR

void ATraceBakedPiece::RefreshEditingNote()
{
	// SPEC v17 §2(a). "659 of 835 instanced pieces are not individually selectable ... and this is
	// written down NOWHERE. Document it where a designer will actually meet it." A designer meets a
	// baked piece by clicking it and reading the Details panel, so that is where this goes — and it
	// reports THIS piece's real numbers, read off the components, rather than a remembered average
	// that would rot the first time anyone re-baked.
	int32 BatchComponents = 0;
	int32 BatchedBlocks = 0;
	int32 LooseBlocks = 0;
	int32 CollisionBoxes = 0;

	TArray<USceneComponent*> PieceComponents;
	GetComponents<USceneComponent>(PieceComponents);

	for (const USceneComponent* Component : PieceComponents)
	{
		if (const UInstancedStaticMeshComponent* Pool = Cast<UInstancedStaticMeshComponent>(Component))
		{
			++BatchComponents;
			BatchedBlocks += Pool->GetInstanceCount();
		}
		else if (Component->IsA<UStaticMeshComponent>())
		{
			++LooseBlocks;
		}
		else if (Component->IsA<UBoxComponent>())
		{
			++CollisionBoxes;
		}
	}

	FString Note = FString::Printf(
		TEXT("This whole actor is one piece of the arena: drag it, rotate it or delete it and every part ")
		TEXT("below moves with it.\n\nThis piece has %d individually selectable mesh component(s), %d ")
		TEXT("collision/standoff box(es)"),
		LooseBlocks, CollisionBoxes);

	if (BatchComponents > 0)
	{
		Note += FString::Printf(
			TEXT(", and %d block(s) BATCHED into %d instanced component(s).\n\n")
			TEXT("YOU CANNOT SELECT ONE OF THOSE %d BATCHED BLOCKS IN THE VIEWPORT. That is deliberate, not ")
			TEXT("a bug: loose components for every block cost 1.8-3.1 ms of game thread across the arena ")
			TEXT("(spec v7 s8), so repetition inside a piece is instanced. What you can do instead:\n")
			TEXT("  - change the whole batch: select the Instanced Static Mesh component in this panel and ")
			TEXT("change its Static Mesh or Material;\n")
			TEXT("  - move one block: expand that component's Instances array and edit the entry;\n")
			TEXT("  - get real per-block actors: raise BakeMaxPiecesPerName / lower InstanceCollapseThreshold ")
			TEXT("and re-bake, and expect the frame cost back."),
			BatchedBlocks, BatchComponents, BatchedBlocks);
	}
	else
	{
		Note += TEXT(" and no batched blocks - every mesh on this piece is separately selectable.");
	}

	EditingNote = Note;
}

#endif // WITH_EDITOR
