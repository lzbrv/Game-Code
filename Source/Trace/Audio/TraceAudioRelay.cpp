// Copyright Trace. All Rights Reserved.

#include "Audio/TraceAudioRelay.h"

#include "Engine/World.h"
#include "EngineUtils.h"

#include "Audio/TraceAudio.h"
#include "Trace.h"

ATraceAudioRelay::ATraceAudioRelay()
{
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates = true;

	// bAlwaysRelevant: see the header. A relay that is culled is a relay whose multicast is silently
	// dropped for exactly the players who were about to walk into earshot.
	bAlwaysRelevant = true;

	// It never moves, has no replicated properties and is not saved into any map, so it needs neither
	// a movement channel nor a client-side load.
	SetReplicateMovement(false);
	bNetLoadOnClient = false;

	// One update a second is enough to keep the channel alive; the RPCs themselves are what carry the
	// traffic and they go out immediately regardless of this.
	SetNetUpdateFrequency(1.f);

	SetCanBeDamaged(false);
	SetActorEnableCollision(false);
	SetHidden(true);
	SetActorHiddenInGame(true);
}

void ATraceAudioRelay::MulticastPlaySound_Implementation(FName Event, FVector_NetQuantize Where)
{
	// Runs on EVERY machine, the authority included. The whole point of routing through here rather
	// than "play locally and also multicast" is that there is one code path and therefore no way for
	// a listen server to hear a game-side sound twice.
	TraceAudio::PlayResolvedAtLocation(this, Event, FVector(Where));
}

ATraceAudioRelay* ATraceAudioRelay::Find(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<ATraceAudioRelay> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It))
		{
			return *It;
		}
	}
	return nullptr;
}

ATraceAudioRelay* ATraceAudioRelay::GetOrSpawn(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	if (ATraceAudioRelay* Existing = Find(World))
	{
		return Existing;
	}

	// The authority test is here rather than at the caller because there are two callers (world begin
	// play, and the belt-and-braces retry in TraceAudio::PlayAt) and a client that spawned its own
	// relay would have an actor nobody replicates to and a multicast that reaches only itself.
	if (World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;          // never saved into a map
	SpawnParams.Name = FName(TEXT("TraceAudioRelay"));

	ATraceAudioRelay* Spawned = World->SpawnActor<ATraceAudioRelay>(
		ATraceAudioRelay::StaticClass(), FTransform::Identity, SpawnParams);

	if (Spawned == nullptr)
	{
		// Loud, because the consequence is "the three game-side sounds became client-side", which is
		// exactly the design mistake spec v26 §9 tells us not to make and is invisible from one
		// viewpoint.
		UE_LOG(LogTraceGame, Error,
			TEXT("[Audio] the game-side relay would not spawn. CoreTurnover / Dash / Parry will be "
			     "heard only on the machine that fired them."));
	}
	else
	{
		UE_LOG(LogTraceGame, Log, TEXT("[Audio] game-side relay up (%s)."), *Spawned->GetName());
	}

	return Spawned;
}
