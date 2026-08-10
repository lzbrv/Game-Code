// Copyright (c) Trace. All Rights Reserved.

#include "World/TraceCoreSpawn.h"

#include "Components/SceneComponent.h"

#if WITH_EDITORONLY_DATA
#include "Components/BillboardComponent.h"
#include "UObject/ConstructorHelpers.h"
#endif

ATraceCoreSpawn::ATraceCoreSpawn()
{
	PrimaryActorTick.bCanEverTick = false;

	// Static: nothing moves this in a game world. The editor may drag it freely — the Static
	// restriction is on moving a component during PLAY, not on an editor transform.
	SpawnRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SpawnRoot"));
	SpawnRoot->SetMobility(EComponentMobility::Static);
	SetRootComponent(SpawnRoot);

	// Never relevant to a client and never a network actor: the server reads its transform and spawns
	// the Core, and the CORE is what replicates. See the class comment.
	bReplicates = false;
	SetCanBeDamaged(false);

#if WITH_EDITORONLY_DATA
	// A marker with no visual is a marker nobody can find. The sprite is editor-only data, so it does
	// not exist in a cooked build at all and cannot cost a packaged game anything.
	Sprite = CreateDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (Sprite != nullptr)
	{
		static ConstructorHelpers::FObjectFinder<UTexture2D> IconFinder(
			TEXT("/Engine/EditorResources/Goal_Waypoint.Goal_Waypoint"));
		if (IconFinder.Succeeded())
		{
			Sprite->SetSprite(IconFinder.Object);
		}

		Sprite->SetupAttachment(SpawnRoot);
		Sprite->SetRelativeScale3D(FVector(2.f));
		Sprite->bIsScreenSizeScaled = true;
		// Belt and braces with the editor-only guard: a billboard that survived into a game world
		// would be a floating icon in the middle of the field, which is exactly the kind of thing
		// that ships.
		Sprite->SetHiddenInGame(true);
	}
#endif
}
