// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — THE SOUND API (spec v26 §9). Three calls. Read this before wiring a trigger.
// ===================================================================================================
//
// A CALL SITE IS ONE LINE AND CANNOT PICK THE WRONG SIDE:
//
//     TraceAudio::Play(Pawn, TraceSoundEvents::Dash);            // game-side: the table says so
//     TraceAudio::Play(PlayerController, TraceSoundEvents::Headshot);   // client-side: ditto
//     TraceAudio::PlayAt(this, TraceSoundEvents::CoreTurnover, Where);  // game-side, explicit point
//     TraceAudio::PlayLocal2D(this, TraceSoundEvents::ButtonPress);     // client-side, no world pos
//
// There is deliberately NO "PlayMulticast" and NO "bLocalOnly" argument. Whether everyone hears an
// event or only you is declared once, in Audio/TraceSoundEvents.h, and Play() reads it. That is the
// half of §9 a verifier checks by listening from a second viewpoint, and the shape of this API is
// what makes it impossible to get wrong at a call site in somebody else's file.
//
// ---------------------------------------------------------------------------------------------------
// WHAT EACH CALL DOES, EXACTLY
// ---------------------------------------------------------------------------------------------------
//
//   Play(Actor, Event)
//       GAME-SIDE event  : does nothing unless Actor->HasAuthority(). On the authority it multicasts
//                          at Actor's location, so every machine — the server included — plays it
//                          once, attenuated by distance. Calling it on a client is a no-op, not a
//                          local play: the server's multicast is already on its way.
//       CLIENT-SIDE event: does nothing unless Actor is the pawn or controller of a PLAYER ON THIS
//                          MACHINE. Bots are excluded (an AIController is "local" on the server, and
//                          without this test a host would hear every bot's jump). No RPC is sent, in
//                          either direction, ever.
//
//   PlayAt(WorldContext, Event, WorldLocation)
//       The game-side call for an event with no actor to hand — a turnover happens at a POINT.
//       Authority only. Never use it for a client-side event: it refuses and says so once.
//
//   PlayLocal2D(WorldContext, Event)
//       The client-side call for an event with no world position — a menu button. Non-spatialised,
//       local, no RPC. Never use it for a game-side event: it refuses and says so once.
//
// ---------------------------------------------------------------------------------------------------
// SILENT-SAFE, WHICH IS A REQUIREMENT AND NOT A HOPE
// ---------------------------------------------------------------------------------------------------
// Every entry point survives a null actor, a null world, a missing bank, a missing sound, a missing
// audio device and a dedicated server. A sound that cannot be found logs ONCE — per event name, per
// process — at Warning, and plays nothing. Nothing here can crash a match and nothing here can fill
// a log: the "once" is a TSet on the subsystem, not a rate limiter.
// ===================================================================================================

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "Audio/TraceSoundEvents.h"

#include "TraceAudio.generated.h"

class AActor;
class ATraceAudioRelay;
class UAudioComponent;
class USoundAttenuation;
class USoundBase;
class UTraceSoundBank;

/**
 * What the audio system has actually done this session. Read by Trace.Audio.Report and by
 * Trace.Audio.Probe, which is what makes those harnesses evidence rather than assertions.
 */
struct FTraceAudioCounters
{
	/** Client-side plays that reached the engine on this machine. */
	int32 LocalPlays = 0;
	/** Game-side plays that reached the engine on this machine (via a multicast, or standalone). */
	int32 WorldPlays = 0;
	/** Multicasts this machine SENT. Non-zero only on the authority. */
	int32 MulticastsSent = 0;
	/** Game-side calls refused because this machine is not the authority. Expected on clients. */
	int32 RefusedNotAuthority = 0;
	/** Client-side calls refused because the actor is not this machine's player. Expected constantly. */
	int32 RefusedNotLocalPlayer = 0;
	/** Calls that found no sound at all. Each distinct event contributes one log line, ever. */
	int32 MissingSound = 0;
	/** Plays the engine declined (no audio device — a dedicated server, or -nosound). */
	int32 NoAudioDevice = 0;
};

/**
 * The per-world audio system: resolves the bank, owns the game-side relay, keeps the "logged once"
 * set, and is the only thing in the project that talks to UGameplayStatics' sound calls.
 *
 * A WORLD subsystem and not a game-instance one, because the relay is an actor and actors belong to
 * worlds; travelling from the menu to the arena must get a fresh relay, not a stale pointer.
 */
UCLASS()
class TRACE_API UTraceAudioSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	/** The subsystem for @p WorldContext's world, or null. Null is legal everywhere. */
	static UTraceAudioSubsystem* Get(const UObject* WorldContext);

	/**
	 * The sound for @p Event: the bank's row, else the convention path /Game/Trace/Audio/S_<Event>,
	 * else null having logged ONCE. Cached either way, including the null — a missing sound must not
	 * cost a package lookup per shot.
	 */
	USoundBase* ResolveSound(FName Event);

	/** Client-side, non-spatialised. Returns the component, or null when nothing played. */
	UAudioComponent* PlayLocalNow(FName Event);

	/** Game-side tail: the actual play, at a point, with attenuation. No side check, no RPC. */
	UAudioComponent* PlayWorldNow(FName Event, const FVector& WorldLocation);

	/** The relay, spawning it on the authority if BeginPlay has not run yet. May be null. */
	ATraceAudioRelay* GetRelay();

	/** The attenuation shape every game-side sound uses. Built once from UTraceAudioSettings. */
	USoundAttenuation* GetWorldAttenuation();

	/** Live counters. Mutable on purpose: the call sites bump them. */
	FTraceAudioCounters& Counters() { return Tally; }
	const FTraceAudioCounters& GetCounters() const { return Tally; }

	/**
	 * How many times each event actually REACHED THE ENGINE on this machine, keyed by event name.
	 *
	 * Added by the v26 integration pass and it answers a question the aggregate counters cannot:
	 * "local 22, world 8" proves sound is flowing but not WHICH events, so a trigger that was never
	 * wired hides behind eight plays of a trigger that was. Trace.Audio.Integ's verdict is this map
	 * read back after the real triggers have been driven, which is why the harness can go red for a
	 * single missing call site instead of only for a silent process.
	 *
	 * Bumped in PlayLocalNow/PlayWorldNow, i.e. AFTER the side gate, the settings gate, the device
	 * test and the resolve — an entry here means the sound was handed to the engine, not merely asked
	 * for. Never reset except by Trace.Audio.Integ's own arming.
	 */
	const TMap<FName, int32>& GetPlaysByEvent() const { return PlaysByEvent; }

	/** Clears GetPlaysByEvent(). Used by Trace.Audio.Integ so a run measures its own drive. */
	void ResetPlaysByEvent() { PlaysByEvent.Reset(); }

	/** Drops the resolve cache and the "already logged" set. Used by Trace.Audio.Reload. */
	void ForgetResolvedSounds();

	/** The bank asset, resolving it on first use. Null when there is none, which is survivable. */
	UTraceSoundBank* GetBank();

	/**
	 * THE GAIN this system would hand the engine for @p Event, right now.
	 *
	 * Public since spec v29 §1b, and only because that section says MEASURE rather than trust. The
	 * footstep knob is worth nothing as a number in an ini; what matters is the gain that actually
	 * reaches a play call, and Trace.Audio.Loudness reads it from here — the same function
	 * PlayLocalNow and PlayWorldNow call — rather than recomputing master x scale and grading its own
	 * arithmetic. A harness that re-derives the value it is checking cannot fail.
	 *
	 * It is const and has no side effects: asking does not play anything.
	 */
	float VolumeFor(FName Event) const;

private:

	/** Logs @p Message for @p Event exactly once per process. Returns true the first time. */
	bool LogOnceFor(FName Event);

	UPROPERTY(Transient)
	TObjectPtr<UTraceSoundBank> Bank = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<USoundAttenuation> WorldAttenuation = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<ATraceAudioRelay> Relay = nullptr;

	/** Resolved sounds, INCLUDING the nulls. See ResolveSound. */
	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<USoundBase>> Resolved;

	/** Names that have already produced their one warning. */
	TSet<FName> Warned;

	/** Per-event play counts on THIS machine. See GetPlaysByEvent. */
	TMap<FName, int32> PlaysByEvent;

	bool bBankResolved = false;
	bool bNoAudioDeviceLogged = false;

	FTraceAudioCounters Tally;
};

namespace TraceAudio
{
	/**
	 * THE CALL. Routes by the event's declared side — see the header comment above.
	 *
	 * @param Actor  the thing the sound belongs to. For a game-side event, its location and its
	 *               authority; for a client-side event, the test for "is this my player".
	 */
	TRACE_API void Play(const AActor* Actor, FName Event);

	/** Game-side, at an explicit point. Authority only. Refuses a client-side event. */
	TRACE_API void PlayAt(const UObject* WorldContext, FName Event, const FVector& WorldLocation);

	/** Client-side, non-spatialised. Never RPCs. Refuses a game-side event. */
	TRACE_API void PlayLocal2D(const UObject* WorldContext, FName Event);

	/**
	 * The tail of the multicast: play @p Event at @p WorldLocation on THIS machine, with no side
	 * check and no RPC. ATraceAudioRelay calls it; gameplay code should not.
	 */
	TRACE_API void PlayResolvedAtLocation(const UObject* WorldContext, FName Event, const FVector& WorldLocation);

	/**
	 * True when @p Actor is a pawn or controller driven by a PLAYER on this machine.
	 *
	 * Bots are excluded deliberately: APawn::IsLocallyControlled() is true for every AI-controlled
	 * pawn on the server, so a plain "is it local" test would give a listen-server host every bot's
	 * jump, pickup and hitmarker. "Client side means only the local player hears it" — the local
	 * PLAYER.
	 */
	TRACE_API bool IsLocalPlayerActor(const AActor* Actor);
}
