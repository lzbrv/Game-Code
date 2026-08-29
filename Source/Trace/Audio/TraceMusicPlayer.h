// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — THE MUSIC PLAYER (release FX/AUDIO plan §5.7)
// ===================================================================================================
//
// One subsystem, two calls (Get() answers null on a dedicated server — test it, every time):
//
//     if (UTraceMusicSubsystem* Music = UTraceMusicSubsystem::Get(this))
//     {
//         Music->Play(TraceSoundEvents::MusicTitle);      // menu HUD BeginPlay
//         Music->Play(TraceSoundEvents::AmbienceMatch);   // match HUD BeginPlay
//         Music->Stop(0.5f);                              // match-end banner site
//         Music->Play(TraceSoundEvents::MusicTitle, 1.4f); // …and back up under the results screen
//     }
//
// A GAME-INSTANCE subsystem, deliberately, where UTraceAudioSubsystem is a world subsystem: music
// must SURVIVE the menu-level -> match-level travel (the menu is its own level, Demo.12 canon), and
// anything owned by a world dies with it. The audio component is created against the AUDIO DEVICE
// (bPersistAcrossLevelTransition), so travel does not flush it either.
//
// WHY THIS DOES NOT GO THROUGH TraceAudio::Play. That API's whole design is "a call site cannot
// pick the wrong side, and one call = one play". Music is the opposite shape in every way that
// matters: it is a PERSISTENT component, not a fire-and-forget play; starting it twice must be a
// no-op, not a second copy; and replacing it must cross-fade, not cut. So MusicTitle/AmbienceMatch
// are declared Client in the event table (a stray Play() on them degrades to one machine hearing
// one copy, never a multicast) and THIS is the thing that actually plays them.
//
// VOLUME = MasterVolume x MusicVolumeScale (UTraceAudioSettings, Audio/TraceSoundBank.h) — the
// project's standing relative-value rule: turning the master down turns the music down with it.
//
// SILENT-SAFE, same discipline as UTraceAudioSubsystem::ResolveSound: a track with no asset logs
// ONCE per name and plays nothing; no audio device, a dedicated server and a null world are all
// quiet no-ops. Nothing here can crash a match and nothing here can fill a log.
//
// WIRED, AND HERE IS WHERE (this block said "NO CALL SITES YET, by design" while the subsystem
// waited a wave for them; that is no longer true and a stale justification is how this project has
// repeatedly fooled itself):
//
//   ATraceMenuHUD::BeginPlay   -> Play(MusicTitle)      — and by then usually a NO-OP; see below
//   ATraceHUD::BeginPlay       -> Play(AmbienceMatch)   — cross-fades out of MusicTitle
//   ATraceHUD::DrawMatchResult -> Stop(0.5f), then the victory/defeat stinger through
//                                 TraceAudio::PlayLocal2D, once per match, and THEN
//                                 Play(MusicTitle, 1.4f) once the stinger's tail is decaying
//
// *** THE MENU BED NOW STARTS ON THE RESULTS SCREEN, NOT AT THE TITLE SCREEN, AND THAT IS WHY THE
// MENU'S OWN Play() IS USUALLY A NO-OP. *** §5.7's two instructions (stop at full time, play on
// return) were both obeyed and still left 11-14 s of bed-less results screen between them, plus a
// further 2.2-2.5 s of travel — measured over two complete laps. DrawMatchResult now brings this
// bed up under the stinger's decay, so it is CONTINUOUS from the whistle through the travel into
// the menu, and ATraceMenuHUD's unconditional Play() lands on the already-playing track and does
// nothing. That is the "asking to play the track that is already playing is a no-op" contract below
// doing real work, not just tolerating a duplicate call.
//
// Measured end to end in one run (release-impl/fxhud/W4-FXHUD-music4.log): "playing 'AmbienceMatch'"
// -> "stopped (fade 0.50s)" + StingerVictory -> "playing 'MusicTitle'".
// ===================================================================================================

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceMusicPlayer.generated.h"

class UAudioComponent;
class USoundBase;

/**
 * The music player: one persistent 2D looping component, cross-faded between tracks.
 *
 * Everything is a no-op when it cannot work (no device, no world, dedicated server, missing asset),
 * and asking to play the track that is already playing is a no-op too — a HUD's BeginPlay can call
 * Play() unconditionally and a map restart will not restart the music.
 */
UCLASS()
class TRACE_API UTraceMusicSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	/** The subsystem behind @p WorldContext's game instance, or null. Null is legal everywhere. */
	static UTraceMusicSubsystem* Get(const UObject* WorldContext);

	/**
	 * Cross-fade to @p Track (an event name from Audio/TraceSoundEvents.h; the asset is
	 * /Game/Trace/Audio/S_<Track>). A NO-OP when @p Track is already the playing track. The
	 * outgoing track fades to silence over the same @p FadeSeconds the incoming one rises.
	 */
	void Play(FName Track, float FadeSeconds = 0.8f);

	/** Fade whatever is playing to silence and stop it. Safe to call when nothing plays. */
	void Stop(float FadeSeconds = 0.8f);

	/** The track Play() would currently treat as already-playing. NAME_None when stopped. */
	FName GetCurrentTrack() const { return CurrentTrack; }

	/**
	 * Re-applies MasterVolume x MusicVolumeScale to the playing component, for the audio settings
	 * page: a slider drag should be heard NOW, not on the next track change.
	 */
	void RefreshVolume();

private:
	/** MasterVolume x MusicVolumeScale, floored at 0. The one place the product is computed. */
	static float DesiredGain();

	/** The soft-path resolve, logging ONCE per missing track. Null is a legal answer. */
	USoundBase* ResolveTrack(FName Track);

	/** Fade @p Component to silence over @p FadeSeconds (0 = stop now) and forget it. */
	static void Retire(UAudioComponent* Component, float FadeSeconds);

	/** The playing component. Persistent across level travel; null when stopped. */
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> Active = nullptr;

	/** The outgoing component during a cross-fade. FadeOut stops it; GC then collects it. */
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> Fading = nullptr;

	/** What Active is playing. Compared by Play() for the "already playing that" no-op. */
	FName CurrentTrack;

	/** Tracks that have already produced their one "no asset" warning. */
	TSet<FName> Warned;
};
