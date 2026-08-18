// Copyright Trace. All Rights Reserved.

#include "Audio/TraceAudio.h"

#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "UObject/UObjectGlobals.h"

#include "Audio/TraceAudioRelay.h"
#include "Audio/TraceSoundBank.h"
#include "Trace.h"

// Named after the file, per this project's jumbo-build rule (Scripts/check-jumbo-build-collisions.py):
// two anonymous namespaces in one unity translation unit are one namespace, and this project has
// already shipped a Windows-only break that way.
namespace TraceAudioLocal
{
	/** The world behind any context object, or null. Never asserts, never warns. */
	static UWorld* WorldOf(const UObject* WorldContext)
	{
		if (WorldContext == nullptr || GEngine == nullptr)
		{
			return nullptr;
		}
		return GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	}
}

// =================================================================================================
// UTraceAudioSubsystem
// =================================================================================================

bool UTraceAudioSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Game, PIE and dedicated-server worlds. Not the editor's preview/inactive worlds, which have no
	// players, no relay to spawn and no listener — creating one there would put a stray
	// always-relevant actor into every asset thumbnail scene.
	const UWorld* OuterWorld = Cast<UWorld>(Outer);
	if (OuterWorld == nullptr)
	{
		return false;
	}

	return OuterWorld->IsGameWorld();
}

void UTraceAudioSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// THE RELAY GOES UP NOW, NOT ON THE FIRST SOUND. See Audio/TraceAudioRelay.h: a multicast only
	// reaches connections that already hold a channel for the actor, so a lazily-spawned relay would
	// eat the first game-side event of every match.
	if (InWorld.GetNetMode() != NM_Client)
	{
		Relay = ATraceAudioRelay::GetOrSpawn(&InWorld);
	}

	// One line, once per world, naming what the audio system found. Cheap, and it is the difference
	// between "no sound" being a five-minute question and an hour-long one.
	const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();
	const UTraceSoundBank* ResolvedBank = GetBank();
	UE_LOG(LogTraceGame, Log,
		TEXT("[Audio] world '%s' netmode=%d: bank=%s (%d row(s)), enabled=%d, master=%.2f, "
		     "game-side radius %.0f uu + %.1fx falloff = %.0f uu, device=%s"),
		*InWorld.GetName(), static_cast<int32>(InWorld.GetNetMode()),
		ResolvedBank != nullptr ? *ResolvedBank->GetName() : TEXT("(none - convention paths only)"),
		ResolvedBank != nullptr ? ResolvedBank->Events.Num() : 0,
		Settings.bSoundEffectsEnabled ? 1 : 0, Settings.MasterVolume,
		Settings.WorldInnerRadiusUU, Settings.WorldFalloffScale, Settings.GetWorldFalloffDistanceUU(),
		InWorld.GetAudioDeviceRaw() != nullptr ? TEXT("present") : TEXT("NONE (-nosound, or a dedicated server)"));
}

void UTraceAudioSubsystem::Deinitialize()
{
	Relay = nullptr;
	Bank = nullptr;
	WorldAttenuation = nullptr;
	Resolved.Empty();
	Warned.Empty();
	bBankResolved = false;

	Super::Deinitialize();
}

UTraceAudioSubsystem* UTraceAudioSubsystem::Get(const UObject* WorldContext)
{
	UWorld* World = TraceAudioLocal::WorldOf(WorldContext);
	return World != nullptr ? World->GetSubsystem<UTraceAudioSubsystem>() : nullptr;
}

UTraceSoundBank* UTraceAudioSubsystem::GetBank()
{
	if (bBankResolved)
	{
		return Bank;
	}
	bBankResolved = true;

	const FSoftObjectPath& Path = UTraceAudioSettings::Get().SoundBankPath;
	if (!Path.IsValid())
	{
		return nullptr;
	}

	// TryLoad, not LoadObject: a bank that is not there is an ordinary state (a fresh clone that has
	// not run Scripts/import-sounds.sh) and must not produce an engine-level load warning per world.
	Bank = Cast<UTraceSoundBank>(Path.TryLoad());
	if (Bank == nullptr)
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("[Audio] no sound bank at %s - falling back to the convention path %s/S_<Event>. "
			     "Run ./Scripts/import-sounds.sh to build one."),
			*Path.ToString(), UTraceSoundBank::SoundPackageDir());
	}

	return Bank;
}

USoundBase* UTraceAudioSubsystem::ResolveSound(FName Event)
{
	// The cache holds the NULLS too. Without that, an event with no asset would hit TryLoad on every
	// shot — which is both a per-frame cost and, for a name that will never resolve, a per-frame
	// chance of a new engine warning. "Logs once" has to mean the lookup happens once as well.
	if (const TObjectPtr<USoundBase>* Cached = Resolved.Find(Event))
	{
		return Cached->Get();
	}

	USoundBase* Sound = nullptr;

	// 1. The bank. THE one place a name maps to a sound (spec v26 §9), and the only route a designer
	//    can change without a script.
	if (const UTraceSoundBank* ResolvedBank = GetBank())
	{
		Sound = ResolvedBank->Find(Event);
	}

	// 2. The convention path. This is not a nicety: it is what makes a half-imported project audible
	//    and what stops a cleared bank row becoming silence nobody can explain.
	if (Sound == nullptr)
	{
		const FSoftObjectPath ConventionPath(UTraceSoundBank::ConventionPathFor(Event));
		Sound = Cast<USoundBase>(ConventionPath.TryLoad());
	}

	Resolved.Add(Event, Sound);

	if (Sound == nullptr && LogOnceFor(Event))
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Audio] no sound for '%s' (%s). Looked in the bank and at %s. Nothing will play for "
			     "this event; this is the only line it will ever print. Fix: put %s.wav in Art/Sounds/ "
			     "and run ./Scripts/import-sounds.sh."),
			*Event.ToString(), TraceSoundEvents::SideName(TraceSoundEvents::SideOf(Event)),
			*UTraceSoundBank::ConventionPathFor(Event), *Event.ToString());
	}

	return Sound;
}

bool UTraceAudioSubsystem::LogOnceFor(FName Event)
{
	if (Warned.Contains(Event))
	{
		return false;
	}
	Warned.Add(Event);
	return true;
}

float UTraceAudioSubsystem::VolumeFor(FName Event) const
{
	const float Master = FMath::Max(0.f, UTraceAudioSettings::Get().MasterVolume);

	// The per-event trim MULTIPLIES the master (this project's standing rule: a value that modifies a
	// base is stored relative to it), so turning the master down turns everything down and no event
	// can escape it by carrying an absolute level of its own.
	const float Trim = (Bank != nullptr) ? Bank->VolumeFor(Event) : 1.f;
	return Master * Trim;
}

USoundAttenuation* UTraceAudioSubsystem::GetWorldAttenuation()
{
	if (WorldAttenuation != nullptr)
	{
		return WorldAttenuation;
	}

	// Built in code rather than imported as an asset, because the numbers belong to Project Settings
	// (where the owner can move them) and an asset would be a second place to change one value.
	WorldAttenuation = NewObject<USoundAttenuation>(this, NAME_None, RF_Transient);
	if (WorldAttenuation == nullptr)
	{
		return nullptr;
	}

	const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();

	FSoundAttenuationSettings& Shape = WorldAttenuation->Attenuation;
	Shape.bAttenuate = true;
	Shape.bSpatialize = true;
	Shape.AttenuationShape = EAttenuationShape::Sphere;

	// X is the sphere's radius for EAttenuationShape::Sphere: everyone inside it is at full volume.
	Shape.AttenuationShapeExtents = FVector(FMath::Max(1.f, Settings.WorldInnerRadiusUU), 0.f, 0.f);

	// DERIVED from the radius above, never typed in. See UTraceAudioSettings::WorldFalloffScale.
	Shape.FalloffDistance = Settings.GetWorldFalloffDistanceUU();

	// NaturalSound rather than Linear: a linear falloff on a 7 km arena is inaudible for most of its
	// range and then jumps, which reads as a bug rather than as distance.
	Shape.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
	Shape.dBAttenuationAtMax = -60.f;

	return WorldAttenuation;
}

ATraceAudioRelay* UTraceAudioSubsystem::GetRelay()
{
	if (IsValid(Relay))
	{
		return Relay;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	// A client never spawns one; it finds the replicated one. A server that somehow got here before
	// OnWorldBeginPlay (a sound fired from a BeginPlay ordering we did not predict) still gets a
	// relay rather than a silent game-side event.
	Relay = (World->GetNetMode() == NM_Client) ? ATraceAudioRelay::Find(World)
	                                           : ATraceAudioRelay::GetOrSpawn(World);
	return Relay;
}

UAudioComponent* UTraceAudioSubsystem::PlayLocalNow(FName Event)
{
	if (!UTraceAudioSettings::Get().bSoundEffectsEnabled)
	{
		return nullptr;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	if (World->GetAudioDeviceRaw() == nullptr)
	{
		++Tally.NoAudioDevice;
		if (!bNoAudioDeviceLogged)
		{
			bNoAudioDeviceLogged = true;
			UE_LOG(LogTraceGame, Log,
				TEXT("[Audio] this process has no audio device (a dedicated server, or -nosound). "
				     "Every play is a no-op from here; this is the only line it will print."));
		}
		return nullptr;
	}

	USoundBase* Sound = ResolveSound(Event);
	if (Sound == nullptr)
	{
		++Tally.MissingSound;
		return nullptr;
	}

	// SpawnSound2D rather than PlaySound2D purely so the harnesses get a handle back:
	// Trace.Audio.Probe asks the returned component whether it is still playing, which is one more
	// piece of evidence than "the call did not crash". bAutoDestroy keeps the ownership simple.
	UAudioComponent* Component = UGameplayStatics::SpawnSound2D(World, Sound, VolumeFor(Event),
		/*PitchMultiplier=*/1.f, /*StartTime=*/0.f, /*ConcurrencySettings=*/nullptr,
		/*bPersistAcrossLevelTransition=*/false, /*bAutoDestroy=*/true);

	++Tally.LocalPlays;
	++PlaysByEvent.FindOrAdd(Event);
	UE_LOG(LogTraceGame, Verbose, TEXT("[Audio] client-side '%s' played locally."), *Event.ToString());
	return Component;
}

UAudioComponent* UTraceAudioSubsystem::PlayWorldNow(FName Event, const FVector& WorldLocation)
{
	if (!UTraceAudioSettings::Get().bSoundEffectsEnabled)
	{
		return nullptr;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	// A dedicated server reaches here — the multicast body runs on the authority too — and has no
	// device. That is not a failure; it is the server not hearing its own multicast.
	if (World->GetAudioDeviceRaw() == nullptr)
	{
		++Tally.NoAudioDevice;
		return nullptr;
	}

	USoundBase* Sound = ResolveSound(Event);
	if (Sound == nullptr)
	{
		++Tally.MissingSound;
		return nullptr;
	}

	UAudioComponent* Component = UGameplayStatics::SpawnSoundAtLocation(World, Sound, WorldLocation,
		FRotator::ZeroRotator, VolumeFor(Event), /*PitchMultiplier=*/1.f, /*StartTime=*/0.f,
		GetWorldAttenuation(), /*ConcurrencySettings=*/nullptr, /*bAutoDestroy=*/true);

	++Tally.WorldPlays;
	++PlaysByEvent.FindOrAdd(Event);
	UE_LOG(LogTraceGame, Verbose, TEXT("[Audio] game-side '%s' played at %s."),
		*Event.ToString(), *WorldLocation.ToCompactString());
	return Component;
}

void UTraceAudioSubsystem::ForgetResolvedSounds()
{
	Resolved.Empty();
	Warned.Empty();
	Bank = nullptr;
	bBankResolved = false;
	WorldAttenuation = nullptr;
}

// =================================================================================================
// namespace TraceAudio — the three calls a trigger site makes
// =================================================================================================

namespace TraceAudio
{
	bool IsLocalPlayerActor(const AActor* Actor)
	{
		if (!IsValid(Actor))
		{
			return false;
		}

		const AController* AsController = Cast<AController>(Actor);
		if (AsController == nullptr)
		{
			if (const APawn* AsPawn = Cast<APawn>(Actor))
			{
				AsController = AsPawn->GetController();
			}
		}

		// A PLAYER controller specifically. An AIController is a "local controller" on the server, so
		// testing only for locality would give a listen-server host every bot's client-side sound —
		// see the header.
		const APlayerController* AsPlayer = Cast<APlayerController>(AsController);
		return (AsPlayer != nullptr) && AsPlayer->IsLocalController();
	}

	void PlayResolvedAtLocation(const UObject* WorldContext, FName Event, const FVector& WorldLocation)
	{
		if (UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(WorldContext))
		{
			Audio->PlayWorldNow(Event, WorldLocation);
		}
	}

	void PlayAt(const UObject* WorldContext, FName Event, const FVector& WorldLocation)
	{
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(WorldContext);
		if (Audio == nullptr)
		{
			return;
		}

		// A client-side event sent down the game-side route would be heard by everyone, which is the
		// exact design mistake §9 names. Refused, and said once.
		if (TraceSoundEvents::SideOf(Event) != ETraceSoundSide::World)
		{
			// ONCE per process, not once per call: this is a wiring mistake, so it wants to be seen
			// exactly one time and then never fill a match's log.
			static bool bWarnedClientEventOnWorldRoute = false;
			if (!bWarnedClientEventOnWorldRoute)
			{
				bWarnedClientEventOnWorldRoute = true;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Audio] TraceAudio::PlayAt refused '%s': it is declared client-side in "
					     "Audio/TraceSoundEvents.h. Use TraceAudio::Play or PlayLocal2D."),
					*Event.ToString());
			}
			++Audio->Counters().RefusedNotAuthority;
			return;
		}

		UWorld* World = Audio->GetWorld();
		if (World == nullptr)
		{
			return;
		}

		// Only the authority multicasts. A client that reached here is a call site that forgot its
		// HasAuthority() test, and the right answer is silence on this machine — the server's
		// multicast is already on its way and playing here as well would double it.
		if (World->GetNetMode() == NM_Client)
		{
			++Audio->Counters().RefusedNotAuthority;
			return;
		}

		if (ATraceAudioRelay* Relay = Audio->GetRelay())
		{
			++Audio->Counters().MulticastsSent;
			Relay->MulticastPlaySound(Event, FVector_NetQuantize(WorldLocation));
			return;
		}

		// No relay. Degraded, and loud about it — see ATraceAudioRelay::GetOrSpawn, which has already
		// logged the Error. Playing locally is better than silence for a standalone session and is
		// impossible to mistake for correct, because the Error is in the log.
		Audio->PlayWorldNow(Event, WorldLocation);
	}

	void PlayLocal2D(const UObject* WorldContext, FName Event)
	{
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(WorldContext);
		if (Audio == nullptr)
		{
			return;
		}

		// The mirror of PlayAt's guard: a game-side event played only here would be heard by one
		// player instead of the room.
		if (TraceSoundEvents::SideOf(Event) == ETraceSoundSide::World)
		{
			static bool bWarnedWorldEventOnLocalRoute = false;
			if (!bWarnedWorldEventOnLocalRoute)
			{
				bWarnedWorldEventOnLocalRoute = true;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Audio] TraceAudio::PlayLocal2D refused '%s': it is declared game-side in "
					     "Audio/TraceSoundEvents.h. Use TraceAudio::Play or PlayAt."),
					*Event.ToString());
			}
			++Audio->Counters().RefusedNotLocalPlayer;
			return;
		}

		Audio->PlayLocalNow(Event);
	}

	void Play(const AActor* Actor, FName Event)
	{
		if (!IsValid(Actor))
		{
			return;
		}

		if (TraceSoundEvents::SideOf(Event) == ETraceSoundSide::World)
		{
			// GAME-SIDE. The authority decides, once, and everybody hears it.
			if (!Actor->HasAuthority())
			{
				if (UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(Actor))
				{
					++Audio->Counters().RefusedNotAuthority;
				}
				return;
			}
			PlayAt(Actor, Event, Actor->GetActorLocation());
			return;
		}

		// CLIENT-SIDE. Only this machine's player, and no RPC in either direction.
		if (!IsLocalPlayerActor(Actor))
		{
			if (UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(Actor))
			{
				++Audio->Counters().RefusedNotLocalPlayer;
			}
			return;
		}

		if (UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(Actor))
		{
			Audio->PlayLocalNow(Event);
		}
	}
}
