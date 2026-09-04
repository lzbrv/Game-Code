// Copyright Trace. All Rights Reserved.

#include "Audio/TraceMusicPlayer.h"

#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/IConsoleManager.h"   // Trace.Music.Beds — the runtime half of the switch
#include "Sound/SoundBase.h"

#include "Audio/TraceSoundBank.h"
#include "Settings/TraceUserSettings.h"   // UI plan WP3 - the player's master and music faders
#include "Trace.h"

bool UTraceMusicSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// A dedicated server has nobody to play music to. Everything else — client, listen host,
	// standalone, PIE — gets the subsystem; whether it can actually PLAY is decided per call
	// (CreateSound2D answers null without a device, and every path here survives that).
	return Super::ShouldCreateSubsystem(Outer) && !IsRunningDedicatedServer();
}

void UTraceMusicSubsystem::Deinitialize()
{
	// The components are created against the audio device with bIgnoreForFlushing, which is what
	// lets them survive level travel — and also what would let them outlive this subsystem if the
	// game instance died without this stop. Immediate, not faded: the process is going away.
	Retire(Active, 0.f);
	Retire(Fading, 0.f);
	Active = nullptr;
	Fading = nullptr;
	CurrentTrack = NAME_None;

	Super::Deinitialize();
}

UTraceMusicSubsystem* UTraceMusicSubsystem::Get(const UObject* WorldContext)
{
	if (WorldContext == nullptr || GEngine == nullptr)
	{
		return nullptr;
	}
	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	const UGameInstance* GameInstance = (World != nullptr) ? World->GetGameInstance() : nullptr;
	return (GameInstance != nullptr) ? GameInstance->GetSubsystem<UTraceMusicSubsystem>() : nullptr;
}

namespace TraceMusicFile
{
	// =============================================================================================
	// *** THE BEDS ARE OFF UNTIL FURTHER NOTICE, AND THIS IS THE SWITCH. ***
	// =============================================================================================
	//
	// Two ways to turn them back on, both live at once and either is enough:
	//
	//     Config/DefaultGame.ini   [/Script/Trace.TraceAudioSettings]  bMusicBedsEnabled=True
	//     console                  Trace.Music.Beds 1
	//
	// THE CVAR IS A THREE-STATE OVERRIDE, NOT A SECOND COPY OF THE SETTING, and that distinction is
	// the whole reason it is an int and not a bool:
	//
	//     -1  follow UTraceAudioSettings::bMusicBedsEnabled   (the default — no opinion)
	//      0  force the beds off
	//      1  force the beds on
	//
	// A plain bool cvar defaulting to false would have SHADOWED the config setting: the owner would
	// flip bMusicBedsEnabled=True, hear nothing, and have no way to tell which of the two switches
	// was winning. With -1 as the default the cvar is silent until somebody sets it.
	int32 GBedsOverride = -1;
	FAutoConsoleVariableRef CVarMusicBeds(
		TEXT("Trace.Music.Beds"),
		GBedsOverride,
		TEXT("The two music beds (MusicTitle, AmbienceMatch). -1 = follow bMusicBedsEnabled in ")
		TEXT("Config/DefaultGame.ini (the default), 0 = force off, 1 = force on. Stingers, UI, ")
		TEXT("weapons and every other sound are unaffected either way."),
		ECVF_Default);

	/** True when a bed is allowed to play right now. The ONE place that question is answered. */
	bool BedsEnabled()
	{
		if (GBedsOverride >= 0)
		{
			return GBedsOverride != 0;
		}
		return UTraceAudioSettings::Get().bMusicBedsEnabled;
	}
}

bool UTraceMusicSubsystem::AreBedsEnabled()
{
	// The public face of TraceMusicFile::BedsEnabled(). One implementation, two names, because the
	// gate has to be file-local (it is the thing Play() consults) and the ANSWER has to be readable
	// from outside (the results-screen log has to be able to say "the beds are off" truthfully).
	return TraceMusicFile::BedsEnabled();
}

float UTraceMusicSubsystem::DesiredGain()
{
	// Master x MusicVolumeScale — both knobs relative, per the standing rule spelled out on
	// MusicVolumeScale (Audio/TraceSoundBank.h). Computed here and nowhere else, so Play and
	// RefreshVolume cannot disagree about what the gain is.
	const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();

	// ---- UI PLAN WP3 — AND THE PLAYER'S OWN TWO FADERS -------------------------------------------
	//
	// *** THE BED DOES NOT PASS THROUGH UTraceAudioSubsystem::VolumeFor. *** It is a persistent 2D
	// component created here and handed a VolumeMultiplier directly, precisely so it can outlive the
	// menu->match travel that destroys the world subsystem. That is the right design for a bed and it
	// has one consequence: the WP3 user gain wired into VolumeFor reaches the stingers (PlayLocal2D)
	// and would MISS the loop, so a MUSIC slider that moved nothing audible is exactly what a player
	// would have got. The same two terms are therefore applied here.
	//
	// It is the same product either way — Master x MusicVolumeScale x (userMaster x userMusic) — so
	// the two paths agree about what a music event's gain is; they only differ in where the component
	// lives. RefreshVolume() re-runs this, which is what makes a drag on the slider audible live.
	const UTraceUserSettings& User = UTraceUserSettings::Get();

	return FMath::Max(0.f, Settings.MasterVolume)
		* FMath::Max(0.f, Settings.MusicVolumeScale)
		* User.GetUserGainForFamily(/*bIsMusic=*/true);
}

USoundBase* UTraceMusicSubsystem::ResolveTrack(FName Track)
{
	// The CONVENTION PATH only, not the bank: the bank is a world subsystem's concern and music
	// outlives worlds. One spelling of the path, shared with everything else via
	// UTraceSoundBank::ConventionPathFor. TryLoad, not LoadObject, for the same reason
	// UTraceAudioSubsystem::GetBank uses it: missing is an ordinary state, not an engine warning.
	const FSoftObjectPath Path(UTraceSoundBank::ConventionPathFor(Track));
	USoundBase* Sound = Cast<USoundBase>(Path.TryLoad());

	if (Sound == nullptr && !Warned.Contains(Track))
	{
		Warned.Add(Track);
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Music] no sound for '%s' at %s. Nothing will play for this track; this is the "
			     "only line it will ever print. Fix: put %s.wav in Art/Sounds/ and run "
			     "./Scripts/import-sounds.sh."),
			*Track.ToString(), *Path.ToString(), *Track.ToString());
	}

	return Sound;
}

void UTraceMusicSubsystem::Retire(UAudioComponent* Component, float FadeSeconds)
{
	if (Component == nullptr)
	{
		return;
	}

	if (Component->IsPlaying() && FadeSeconds > 0.f)
	{
		// FadeOut to zero STOPS the component when the fade lands — no tick, no timer, no second
		// bookkeeping pass needed here. bAutoDestroy is false, so the UObject waits for GC.
		Component->FadeOut(FadeSeconds, 0.f);
	}
	else
	{
		Component->Stop();
	}
}

void UTraceMusicSubsystem::Play(FName Track, float FadeSeconds)
{
	if (Track.IsNone())
	{
		return;
	}

	// ---- THE BEDS ARE OFF (TraceMusicFile::BedsEnabled, above) ----------------------------------
	//
	// FIRST, BEFORE ANY STATE IS TOUCHED, so a refused Play cannot leave this subsystem believing a
	// bed is playing: CurrentTrack, Active and Fading are all still whatever they were, and the
	// Stop() below makes that "nothing" in every order of events.
	//
	// The Stop() is NOT redundant with the early return. It is what makes the RUNTIME toggle honest:
	// `Trace.Music.Beds 0` typed while a bed is up leaves that component playing until something
	// asks for a track, and the next ask is the next HUD's BeginPlay — a whole match away. Stopping
	// here means the first Play() after the switch is thrown ends the bed instead of ignoring it,
	// and it costs nothing on the ordinary path because Stop() early-returns when nothing plays.
	if (!TraceMusicFile::BedsEnabled())
	{
		Stop(FadeSeconds);
		return;
	}

	// THE NO-OP THAT MAKES CALL SITES SIMPLE: every menu HUD BeginPlay can ask for MusicTitle and
	// only the first one starts anything. "Still the current track" is only honest while the
	// component is actually playing — a component the device refused, or one somebody stopped
	// through the debugger, must not block the restart.
	if (Track == CurrentTrack && Active != nullptr && Active->IsPlaying())
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = (GameInstance != nullptr) ? GameInstance->GetWorld() : nullptr;
	if (World == nullptr)
	{
		// Mid-travel there is briefly no world to create a component through. The next HUD's
		// BeginPlay (which is where the §5.7 call sites live) retries naturally.
		return;
	}

	USoundBase* Sound = ResolveTrack(Track);
	if (Sound == nullptr)
	{
		// Logged once by ResolveTrack. Whatever is currently playing keeps playing: a missing new
		// track must degrade to "the old music continues", not to sudden silence.
		return;
	}

	// A third track requested while a cross-fade is still running retires the oldest immediately.
	// Two components is the whole budget — this is a cross-fade, not a mixer.
	Retire(Fading, 0.f);
	Fading = Active;
	Active = nullptr;
	Retire(Fading, FadeSeconds);

	// Gain rides VolumeMultiplier; the fade rides the component's fader. Keeping them on separate
	// axes is what lets RefreshVolume move the gain mid-fade without fighting the envelope.
	UAudioComponent* Component = UGameplayStatics::CreateSound2D(World, Sound,
		/*VolumeMultiplier=*/DesiredGain(), /*PitchMultiplier=*/1.f, /*StartTime=*/0.f,
		/*ConcurrencySettings=*/nullptr, /*bPersistAcrossLevelTransition=*/true, /*bAutoDestroy=*/false);
	if (Component == nullptr)
	{
		// No audio device (-nosound), or the world refuses playback. Quiet no-op; CurrentTrack
		// stays cleared so a later Play can try again.
		CurrentTrack = NAME_None;
		return;
	}

	// FadeIn STARTS playback. Zero-or-negative fade degrades to a plain start, and the fader's
	// target is 1.0 — full VolumeMultiplier — not a second copy of the gain.
	if (FadeSeconds > 0.f)
	{
		Component->FadeIn(FadeSeconds, 1.f);
	}
	else
	{
		Component->Play();
	}

	Active = Component;
	CurrentTrack = Track;

	UE_LOG(LogTraceGame, Log, TEXT("[Music] playing '%s' (fade %.2fs, gain %.2f)"),
		*Track.ToString(), FMath::Max(0.f, FadeSeconds), DesiredGain());
}

void UTraceMusicSubsystem::Stop(float FadeSeconds)
{
	if (Active == nullptr && Fading == nullptr)
	{
		return;
	}

	Retire(Fading, 0.f);
	Retire(Active, FadeSeconds);
	Fading = nullptr;
	Active = nullptr;
	CurrentTrack = NAME_None;

	UE_LOG(LogTraceGame, Log, TEXT("[Music] stopped (fade %.2fs)"), FMath::Max(0.f, FadeSeconds));
}

void UTraceMusicSubsystem::RefreshVolume()
{
	if (Active != nullptr)
	{
		// The playing component only. Fading is heading to silence and staying there; moving its
		// gain mid-fade would just make the fade audible twice.
		Active->SetVolumeMultiplier(DesiredGain());
	}
}
