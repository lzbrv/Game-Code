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
#include "Settings/TraceUserSettings.h"   // UI plan WP3 - the player's three volume faders
#include "Audio/TraceSoundBank.h"
#include "Trace.h"

// Named after the file, per this project's jumbo-build rule (Scripts/check-jumbo-build-collisions.py):
// two anonymous namespaces in one unity translation unit are one namespace, and this project has
// already shipped a Windows-only break that way.
namespace TraceAudioLocal
{
	/**
	 * DEMO 29 — which unwired events this process has already explained.
	 *
	 * ONCE PER EVENT PER PROCESS, not once per call and not a rate limiter, exactly like the
	 * missing-sound warning: "DeathBurst is switched off" is a fact about the build, so it wants to
	 * be in the log one time. Without it a bot match would print twenty-nine identical lines for the
	 * death burst alone. A TSet and not a bool, so each unwired event gets its own sentence.
	 */
	static TSet<FName> GUnwiredExplained;

	/**
	 * Log, once, that @p Event's trigger fired and the sound deliberately did not follow.
	 *
	 * Log level and not Verbose: the whole point is that somebody reading an ordinary log after the
	 * owner says "the death sound is gone" finds the reason and the way back without having to know
	 * that a list exists.
	 */
	static void ExplainUnwiredOnce(FName Event)
	{
		if (GUnwiredExplained.Contains(Event))
		{
			return;
		}
		GUnwiredExplained.Add(Event);
		UE_LOG(LogTraceGame, Log,
			TEXT("[Audio] '%s' is UNWIRED: its trigger fired and the sound did not play. %s. ")
			TEXT("`Trace.Audio.UnwiredEvents 0` brings it (and every other unwired event) back."),
			*Event.ToString(), TraceSoundEvents::UnwiredReason(Event));
	}

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
	const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();

	// SPEC v29 §1b — FOOTSTEPS CARRY THEIR OWN KNOB, and it is asked for by FAMILY rather than by
	// name matching. GetFootstepVolume() is Master x FootstepVolumeScale, derived in one place, so
	// this line cannot drift from the number Trace.Audio.Loudness reports.
	//
	// Reading the family from the event TABLE and not from the string "Step" is the difference
	// between a rule and a coincidence: Step12 added tomorrow gets the knob by being declared a
	// footstep, and an event called "Steppe" never gets it by accident.
	const ETraceSoundFamily Family = TraceSoundEvents::FamilyOf(Event);
	const bool bFootstep = (Family == ETraceSoundFamily::Footstep);
	const float Base = bFootstep ? Settings.GetFootstepVolume() : FMath::Max(0.f, Settings.MasterVolume);

	// The per-event trim MULTIPLIES the base (this project's standing rule: a value that modifies a
	// base is stored relative to it), so turning the master down turns everything down and no event
	// can escape it by carrying an absolute level of its own.
	const float Trim = (Bank != nullptr) ? Bank->VolumeFor(Event) : 1.f;

	// ---- UI PLAN WP3 — THE PLAYER'S OWN FADERS, AND THE MUSIC BED'S DESIGNER TRIM ----------------
	//
	// TWO DIFFERENT KINDS OF NUMBER MEET HERE, and keeping them apart is the whole design:
	//
	//   MusicVolumeScale (UTraceAudioSettings, config=Game, checked in) is the DESIGNER saying how
	//   far under the effects a 64-second bed should sit. It ships at 0.7 and is the same on every
	//   machine — part of the mix, exactly like FootstepVolumeScale one line up.
	//
	//   The three UTraceUserSettings faders are the PLAYER saying how loud their machine should be.
	//   Per-machine, written at runtime, never in a diff.
	//
	// Both multiply, so a player at 100% hears precisely the designer's mix and a player at 0% hears
	// silence — and neither can be expressed by editing the other. Asked for BY FAMILY, from the
	// event table, for the same reason the footstep branch is: a stinger added tomorrow gets the
	// music fader by being DECLARED music (FX_AUDIO_PLAN §5.1 puts MusicTitle, AmbienceMatch and both
	// stingers in ETraceSoundFamily::Music), and an event whose name merely contains "music" never
	// gets it by accident.
	const bool bMusic = (Family == ETraceSoundFamily::Music);
	const float MusicTrim = bMusic ? FMath::Clamp(Settings.MusicVolumeScale, 0.f, 1.f) : 1.f;
	const float UserGain = UTraceUserSettings::Get().GetUserGainForFamily(bMusic);

	return Base * Trim * MusicTrim * UserGain;
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

bool UTraceAudioSubsystem::IsBigWorldEvent(FName Event)
{
	// FX_AUDIO_PLAN §1.6.5. THE WHOLE LIST, and it is a code set rather than a knob on purpose: "this
	// one is important" is not a claim a call site gets to make, or every call site eventually makes
	// it. Three events, each of which the far side of the arena is meant to hear.
	return Event == TraceSoundEvents::MortimerQuake
		|| Event == TraceSoundEvents::RoxieRocketBurst
		|| Event == TraceSoundEvents::Goal;
}

USoundAttenuation* UTraceAudioSubsystem::GetBigWorldAttenuation()
{
	if (BigWorldAttenuation != nullptr)
	{
		return BigWorldAttenuation;
	}

	BigWorldAttenuation = NewObject<USoundAttenuation>(this, NAME_None, RF_Transient);
	if (BigWorldAttenuation == nullptr)
	{
		return nullptr;
	}

	const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();

	FSoundAttenuationSettings& Shape = BigWorldAttenuation->Attenuation;
	Shape.bAttenuate = true;
	Shape.bSpatialize = true;
	Shape.AttenuationShape = EAttenuationShape::Sphere;

	// DERIVED from the ordinary shape's numbers rather than typed in, for the same reason
	// GetWorldFalloffDistanceUU() is derived: the owner retunes ONE pair in Project Settings and both
	// shapes follow. x2 on the inner radius (1200 -> 2400) and x8 rather than x5 on the falloff, per
	// §1.6.5.
	constexpr float BigInnerScale = 2.f;
	constexpr float BigFalloffScale = 8.f;
	const float InnerRadius = FMath::Max(1.f, Settings.WorldInnerRadiusUU) * BigInnerScale;

	Shape.AttenuationShapeExtents = FVector(InnerRadius, 0.f, 0.f);
	Shape.FalloffDistance = InnerRadius * BigFalloffScale;
	Shape.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
	Shape.dBAttenuationAtMax = -60.f;

	return BigWorldAttenuation;
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

	// DEMO 29 items 9 and 11. Ahead of the device test and the resolve so an unwired event costs
	// nothing and, more importantly, so PlaysByEvent never counts it: Trace.Audio.EventPlays is the
	// ledger that answers "did this sound?", and a silent event that appears in it with a count
	// would be a lie in the one instrument built to catch exactly this class of bug.
	if (TraceSoundEvents::IsUnwired(Event))
	{
		++Tally.RefusedUnwired;
		TraceAudioLocal::ExplainUnwiredOnce(Event);
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

	// DEMO 29 items 9 and 11, and this is the copy that matters most: every game-side route funnels
	// through here (Play, PlayAt's multicast body, PlayReplicatedLocal, PlayPredictedLocal), so one
	// test covers all four and a machine RECEIVING a multicast for an unwired event stays silent
	// even when the sender is an older build that still broadcasts it.
	if (TraceSoundEvents::IsUnwired(Event))
	{
		++Tally.RefusedUnwired;
		TraceAudioLocal::ExplainUnwiredOnce(Event);
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

	// FX_AUDIO_PLAN §1.6.5: the shape is chosen from the EVENT, here, once — not by the call site and
	// not by a parameter. Every game-side route (Play, PlayAt, the relay's multicast,
	// PlayReplicatedLocal, PlayPredictedLocal) funnels through this function, so the three big events
	// carry their reach whichever door they came in by.
	USoundAttenuation* const Shape = IsBigWorldEvent(Event) ? GetBigWorldAttenuation() : GetWorldAttenuation();

	UAudioComponent* Component = UGameplayStatics::SpawnSoundAtLocation(World, Sound, WorldLocation,
		FRotator::ZeroRotator, VolumeFor(Event), /*PitchMultiplier=*/1.f, /*StartTime=*/0.f,
		Shape, /*ConcurrencySettings=*/nullptr, /*bAutoDestroy=*/true);

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
	BigWorldAttenuation = nullptr;   // rebuilt on next use, so Trace.Audio.Reload picks up retuned radii
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

		// DEMO 29 items 9 and 11. PlayWorldNow would refuse this on every machine anyway; catching it
		// HERE, on the authority, is what stops the multicast being SENT — twenty-nine unreliable
		// RPCs per match for DeathBurst alone, to say something no receiver is allowed to play.
		if (TraceSoundEvents::IsUnwired(Event))
		{
			++Audio->Counters().RefusedUnwired;
			TraceAudioLocal::ExplainUnwiredOnce(Event);
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

	// =============================================================================================
	// THE FOUR NARROW BYPASSES — FX_AUDIO_PLAN §1.6. See the header for what each one is FOR; the
	// bodies below are only about being safe when they are called wrongly.
	// =============================================================================================

	void PlayPredictedLocal(const AActor* Shooter, FName Event, const FVector& Where)
	{
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(Shooter);
		if (Audio == nullptr)
		{
			return;
		}

		// THE ONE GUARD, and it is the client-side gate's guard for the client-side gate's reason: a
		// bot's pawn is "locally controlled" on a listen server, so a plain locality test would give
		// the host a point-blank copy of every bot's gunshot on top of the multicast it already gets.
		if (!IsLocalPlayerActor(Shooter))
		{
			++Audio->Counters().RefusedNotLocalPlayer;
			return;
		}

		// Spatialised, at the muzzle, with no side check: this is deliberately the same PlayWorldNow
		// the relay's multicast calls, so the predicted copy and everybody else's copy are the same
		// sound with the same gain and the same attenuation curve. The exclusion (below) is what stops
		// this machine hearing it twice.
		Audio->PlayWorldNow(Event, Where);
	}

	void PlayAtExcluding(const UObject* WorldContext, FName Event, const FVector& Where, APawn* Excluded)
	{
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(WorldContext);
		if (Audio == nullptr)
		{
			return;
		}

		if (Excluded == nullptr)
		{
			// Not fatal — it is PlayAt with extra steps — but it is always a mistake at a call site
			// whose whole point is the exclusion, so it says so once and then behaves.
			static bool bWarnedNullExclusion = false;
			if (!bWarnedNullExclusion)
			{
				bWarnedNullExclusion = true;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Audio] PlayAtExcluding('%s') was given a null Excluded pawn — every machine "
					     "will play it, including the one that predicted it. Use TraceAudio::PlayAt if "
					     "that is what you meant."),
					*Event.ToString());
			}
			PlayAt(WorldContext, Event, Where);
			return;
		}

		if (TraceSoundEvents::SideOf(Event) != ETraceSoundSide::World)
		{
			static bool bWarnedClientEventOnExcludingRoute = false;
			if (!bWarnedClientEventOnExcludingRoute)
			{
				bWarnedClientEventOnExcludingRoute = true;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Audio] TraceAudio::PlayAtExcluding refused '%s': it is declared client-side in "
					     "Audio/TraceSoundEvents.h, so there is nobody to exclude it FROM."),
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

		// Authority only, exactly like PlayAt: a client that reached here forgot its HasAuthority()
		// test, and the server's multicast is already on its way.
		if (World->GetNetMode() == NM_Client)
		{
			++Audio->Counters().RefusedNotAuthority;
			return;
		}

		if (ATraceAudioRelay* Relay = Audio->GetRelay())
		{
			++Audio->Counters().MulticastsSent;
			Relay->MulticastPlaySoundExcluding(Event, FVector_NetQuantize(Where), Excluded);
			return;
		}

		// No relay — the Error is already in the log from GetOrSpawn. Standalone still wants the
		// sound, and standalone is exactly the case where "everyone except the predictor" means
		// "nobody", so the honest degradation is silence on this machine if the excluded pawn is ours.
		if (!IsLocalPlayerActor(Excluded))
		{
			Audio->PlayWorldNow(Event, Where);
		}
	}

	void PlayReplicatedLocal(const UObject* WorldContext, FName Event, const FVector& Where)
	{
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(WorldContext);
		if (Audio == nullptr)
		{
			return;
		}

		// A WORLD-side event down this route is a doctrine break rather than a crash: the actor's
		// replication already put this call on every machine, so whoever ALSO calls Play() on the same
		// event will multicast it on top and everyone hears it twice. Said once, and then played,
		// because silence would be a worse way to find out.
		if (TraceSoundEvents::SideOf(Event) == ETraceSoundSide::World)
		{
			static bool bWarnedWorldEventOnReplicatedRoute = false;
			if (!bWarnedWorldEventOnReplicatedRoute)
			{
				bWarnedWorldEventOnReplicatedRoute = true;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Audio] TraceAudio::PlayReplicatedLocal('%s'): that event is declared GAME-side "
					     "in Audio/TraceSoundEvents.h. Events that ride a replicated actor must be "
					     "declared client-side (§1.6.3) or a stray Play() will multicast them as well."),
					*Event.ToString());
			}
		}

		// No authority test on purpose: the point of this call is that it runs on the authority AND on
		// every client, once each, from code they all run.
		Audio->PlayWorldNow(Event, Where);
	}

	UAudioComponent* StartLoopOn(USceneComponent* AttachTo, FName Event, float FadeInSeconds)
	{
		if (!IsValid(AttachTo))
		{
			return nullptr;
		}

		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(AttachTo);
		if (Audio == nullptr)
		{
			return nullptr;
		}

		if (!UTraceAudioSettings::Get().bSoundEffectsEnabled)
		{
			return nullptr;
		}

		// DEMO 29 items 9 and 11. None of the three events unwired TODAY is a loop, so this test
		// costs one compare and changes nothing — it is here so that "an unwired event cannot be
		// heard" is a property of the module rather than a property of which three names happen to
		// be on the list. Returning null is the same answer a dedicated server gets, and every
		// caller already survives it.
		if (TraceSoundEvents::IsUnwired(Event))
		{
			++Audio->Counters().RefusedUnwired;
			TraceAudioLocal::ExplainUnwiredOnce(Event);
			return nullptr;
		}

		UWorld* World = Audio->GetWorld();
		if (World == nullptr || World->GetAudioDeviceRaw() == nullptr)
		{
			// A dedicated server has no device. Returning null rather than a dead component keeps the
			// caller's "if (Loop != nullptr) Loop->FadeOut(...)" honest on every machine.
			return nullptr;
		}

		USoundBase* Sound = Audio->ResolveSound(Event);
		if (Sound == nullptr)
		{
			return nullptr;
		}

		const float Volume = Audio->VolumeFor(Event);

		// bAutoDestroy FALSE: the caller owns this one. A looping component that destroyed itself
		// would leave the caller holding a dangling pointer at exactly the moment it wants to fade it
		// out. bStopWhenAttachedToDestroyed TRUE so a loop can never outlive the pawn it hangs on —
		// the corpse case the §1.2 router's detach rule is also about.
		UAudioComponent* Loop = UGameplayStatics::SpawnSoundAttached(Sound, AttachTo, NAME_None,
			FVector::ZeroVector, EAttachLocation::KeepRelativeOffset,
			/*bStopWhenAttachedToDestroyed=*/true, Volume, /*PitchMultiplier=*/1.f, /*StartTime=*/0.f,
			Audio->GetWorldAttenuation(), /*ConcurrencySettings=*/nullptr, /*bAutoDestroy=*/false);

		if (Loop == nullptr)
		{
			++Audio->Counters().NoAudioDevice;
			return nullptr;
		}

		if (FadeInSeconds > 0.f)
		{
			// FadeIn re-starts the sound from the top with a volume ramp; it has been playing for a
			// fraction of a millisecond at this point, so there is nothing to hear in the restart, and
			// the ramp is what stops a state that switches on during a fight from clicking.
			Loop->FadeIn(FadeInSeconds, /*FadeVolumeLevel=*/1.f);
		}

		// Counted like every other play that reached the engine, so Trace.Audio.Report and
		// Trace.Audio.Integ see loops as well as one-shots.
		Audio->CountAttachedPlay(Event);
		UE_LOG(LogTraceGame, Verbose, TEXT("[Audio] loop '%s' attached to %s (gain %.3f, fade %.2fs)."),
			*Event.ToString(), *GetNameSafe(AttachTo->GetOwner()), Volume, FadeInSeconds);
		return Loop;
	}
}
