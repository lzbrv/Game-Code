// Copyright (c) Trace. All Rights Reserved.

#include "World/TraceTeamPlayerStart.h"

#include "Components/CapsuleComponent.h"

ATraceTeamPlayerStart::ATraceTeamPlayerStart(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;

	// These are pure markers. They are spawned procedurally on the server only, so they must
	// never cost anything at runtime and must never collide with the pawns spawning on them.
	SetCanBeDamaged(false);

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Capsule->SetGenerateOverlapEvents(false);
		Capsule->SetCanEverAffectNavigation(false);
	}
}
