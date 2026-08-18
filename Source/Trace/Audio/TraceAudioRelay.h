// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — THE GAME-SIDE CHANNEL (spec v26 §9)
// ===================================================================================================
//
//     "Game-side means everyone nearby hears it — a replicated/multicast sound at a world location."
//
// A multicast RPC has to live on a replicated ACTOR, and none of the three game-side events has an
// actor it can borrow: a turnover belongs to the Core (which is sometimes attached to a pawn and
// sometimes lying on the floor), a dash belongs to a pawn that may be network-irrelevant to the
// listener a moment later, and a parry belongs to a component. So the audio system owns exactly one
// tiny actor whose entire job is to carry the multicast.
//
// WHY IT IS ALWAYS RELEVANT AND WHY IT IS SPAWNED AT WORLD BEGIN PLAY, NOT LAZILY.
// A multicast only reaches connections that already have an open channel for the actor. Spawning the
// relay on the first turnover would mean the first turnover of the match is heard by nobody — the
// channel opens on the next replication tick, after the RPC has already been dropped. This project
// has shipped a "wired" hook that never fired; a lazily-spawned relay is the same bug with a delay
// fuse. So it exists from BeginPlay, is bAlwaysRelevant, and replicates nothing at all: no properties,
// no movement, no collision, no mesh. It costs one always-relevant actor channel per connection.
//
// The listener distance is the ATTENUATION's job, not relevancy's — see UTraceAudioSettings'
// inner-radius/falloff pair. "Nearby" is a volume curve, not a replication rule, because a player who
// walks INTO earshot half a second later should hear the tail of the sound rather than nothing.
// ===================================================================================================

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"   // FVector_NetQuantize — the multicast's payload
#include "GameFramework/Actor.h"
#include "UObject/ObjectMacros.h"

#include "TraceAudioRelay.generated.h"

/**
 * The one actor the game-side sounds travel on. Server-spawned, never placed, never visible.
 *
 * Nothing outside Source/Trace/Audio should touch this: call TraceAudio::Play() and let the event's
 * declared side pick the route.
 */
UCLASS(NotPlaceable, NotBlueprintable)
class TRACE_API ATraceAudioRelay : public AActor
{
	GENERATED_BODY()

public:
	ATraceAudioRelay();

	/**
	 * THE game-side play. Called on the server, runs on every machine — the server included, which
	 * is why the authority must NOT also play the sound locally. Unreliable on purpose: a sound
	 * effect that arrives late is worse than one that never arrives, and a saturated channel must
	 * drop hit confirmations last.
	 */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlaySound(FName Event, FVector_NetQuantize Where);

	/** The relay for @p World, or null. Server and clients both — clients get theirs by replication. */
	static ATraceAudioRelay* Find(const UWorld* World);

	/**
	 * AUTHORITY ONLY. Returns the existing relay, or spawns it. Called from
	 * UTraceAudioSubsystem::OnWorldBeginPlay; safe to call again (it never makes a second one).
	 */
	static ATraceAudioRelay* GetOrSpawn(UWorld* World);
};
