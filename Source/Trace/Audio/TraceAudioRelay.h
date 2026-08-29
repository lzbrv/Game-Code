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

class APawn;

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

	/**
	 * THE SAME MULTICAST, MINUS ONE MACHINE (FX_AUDIO_PLAN §1.6.2).
	 *
	 * A multicast cannot be addressed, so the exclusion is decided by the RECEIVER: every machine runs
	 * this body, and the one where @p Excluded is a locally-controlled PLAYER pawn plays nothing,
	 * because it already played its own predicted copy the instant the trigger went down. Everyone
	 * else — the listen server included, unless the shooter is the host — hears it exactly as they
	 * hear an ordinary game-side sound.
	 *
	 * WHY THE PAWN AND NOT A PLAYER-ID: the pawn reference already replicates and is already relevant
	 * on every machine that can hear the sound (it is the thing making it). A machine that has not
	 * heard of that pawn resolves it to null and plays the sound, which is the right answer — a
	 * listener too far away to have the shooter replicated is not the shooter.
	 *
	 * Unreliable, like its sibling, for the same reason: a late sound is worse than a lost one.
	 */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlaySoundExcluding(FName Event, FVector_NetQuantize Where, APawn* Excluded);

	/** The relay for @p World, or null. Server and clients both — clients get theirs by replication. */
	static ATraceAudioRelay* Find(const UWorld* World);

	/**
	 * AUTHORITY ONLY. Returns the existing relay, or spawns it. Called from
	 * UTraceAudioSubsystem::OnWorldBeginPlay; safe to call again (it never makes a second one).
	 */
	static ATraceAudioRelay* GetOrSpawn(UWorld* World);
};
