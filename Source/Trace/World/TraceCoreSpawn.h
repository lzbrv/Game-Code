// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/ObjectPtr.h"

#include "TraceCoreSpawn.generated.h"

class UBillboardComponent;

/**
 * WHERE THE CORE STARTS AND RESETS TO — as a real, placed, draggable actor.
 *
 * SPEC v15 §1.4 ASKED FOR THIS AND SPEC v17 §2 CLOSED IT. Every other gameplay actor in the arena
 * became a placed actor when the map was baked: the endzone triggers, the goal volumes, the team
 * player starts, the whole lighting rig. The Core spawn did not. It stayed a PURE FUNCTION of the
 * builder's layout properties — ATraceArenaBuilder::GetCoreSpawnLocation() returns "centre of the
 * field, on top of the pedestal, plus a small drop height" — so a designer who moved the centre dais
 * in the baked level found the Core still spawning where the dais used to be, with nothing in the
 * outliner to grab and no way to say otherwise.
 *
 * WHAT IT DOES. Nothing at all by itself. It is a marker: ATraceArenaBuilder::GetCoreSpawnLocation()
 * looks for one of these in the level and, if it finds one, returns ITS WORLD LOCATION instead of the
 * derived point. ATraceGameMode spawns the Core there and ATraceCore::GetHomeLocation() resets to it,
 * both through that one function, so moving this actor moves the kickoff, the reset-after-score and
 * the "the Core is home" test together — which is the only way they can stay consistent.
 *
 * IT IS OPT-IN, WITH THE OLD PATH AS A LIVE FALLBACK (spec v17 §0 rule 1). A level with no
 * ATraceCoreSpawn in it — which is every level in this project until the bake puts one there, and
 * still /Game/Maps/Arena — derives the location exactly as it always has, and logs that it did. A
 * level with one uses it, and logs THAT. There is no toggle to get wrong: the actor's presence is
 * the switch.
 *
 * TWO OR MORE IS A MISTAKE AND IS REPORTED. The first one found wins (so the arena is never left
 * with no Core at all) and a warning names how many there were, because "the Core spawns in the
 * wrong half sometimes" is otherwise a very expensive thing to track down.
 *
 * DELIBERATELY NOT AN ATraceCore. Placing a real Core in the level would give the level an actor the
 * server has to reconcile with the one ATraceGameMode::SpawnCoreIfNeeded creates, and two Cores — one
 * replicated and one not — is a far worse failure than a misplaced spawn point. A marker has no
 * gameplay state, no replication and no lifetime to get wrong.
 *
 * NOT REPLICATED, on purpose. This is level geometry in the same sense a PlayerStart is: the server
 * reads it to decide where to spawn the Core, and the Core itself is what replicates. A client never
 * asks the question.
 */
UCLASS()
class TRACE_API ATraceCoreSpawn : public AActor
{
	GENERATED_BODY()

public:
	ATraceCoreSpawn();

	/**
	 * Root. Named SpawnRoot rather than Root: AActor has no member of that name, but
	 * ATraceArenaBuilder does, and MSVC C4458 shadowing has broken the Windows build here more than
	 * once — macOS structurally cannot catch it, so the only defence is not writing it.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Core Spawn")
	TObjectPtr<USceneComponent> SpawnRoot;

#if WITH_EDITORONLY_DATA
	/** Editor-only sprite, so the marker can be seen and clicked in an empty patch of field. */
	UPROPERTY()
	TObjectPtr<UBillboardComponent> Sprite;
#endif
};
