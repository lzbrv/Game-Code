// Copyright (c) Trace. All Rights Reserved.

#include "World/TraceBakedPiece.h"

#include "Components/SceneComponent.h"

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
