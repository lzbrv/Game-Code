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
//         Music->Play(TraceSoundEvents::AmbienceMatch);   // match HUD BeginPlay (switched off: see below)
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
// *** THE TITLE BED IS ON; THE MATCH AMBIENCE IS OFF (since 2026-10-07). *** Both beds were
// switched off from 2026-09-04 (3f97019, the owner's request: UTraceAudioSettings::bMusicBedsEnabled
// =False, so Play() returned early) and back on 2026-10-05, again at the owner's request, by setting
// that flag to True in Config/DefaultGame.ini and in the header default. On 2026-10-07 the owner
// asked to "remove ambient match track", so AmbienceMatch has its own switch,
// UTraceAudioSettings::bMatchAmbienceEnabled, now False: Play(AmbienceMatch) fades out whatever bed
// is up and starts nothing. Nothing was unwired either time, so the three call sites below still
// run as described, and bMatchAmbienceEnabled=True (or `Trace.Music.Ambience 1`) puts the match
// ambience back. `Trace.Music.Beds 0` (or bMusicBedsEnabled=False) silences both beds; see
// AreBedsEnabled() and IsMatchAmbienceEnabled().
//
// The stingers are NOT beds and the switch never affected them — they never went through this
// subsystem.
//
// WIRED, AND HERE IS WHERE (this block said "NO CALL SITES YET, by design" while the subsystem
// waited a wave for them; that is no longer true and a stale justification is how this project has
// repeatedly fooled itself):
//
//   ATraceMenuHUD::BeginPlay   -> Play(MusicTitle)      — and by then usually a NO-OP; see below
//   ATraceHUD::BeginPlay       -> Play(AmbienceMatch)   — with the ambience on, cross-fades out of
//                                 MusicTitle; with it off (the shipped state since 2026-10-07),
//                                 MusicTitle fades out over the same 0.8 s and nothing fades in
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
// -> "stopped (fade 0.50s)" + StingerVictory -> "playing 'MusicTitle'". With the match ambience off
// the same lap reads "'AmbienceMatch' not started" -> (no stop: nothing is playing) + the stinger ->
// "playing 'MusicTitle'". Trace.Music.Verify (Audio/TraceMusicVerify.cpp) drives that lap and checks
// what the mixer is actually playing at each step.
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
	 *
	 * *** WHILE THE BEDS ARE DISABLED (UTraceAudioSettings::bMusicBedsEnabled False, or
	 * *** `Trace.Music.Beds 0`) THIS STARTS NOTHING. *** It stops whatever is playing over
	 * @p FadeSeconds and clears GetCurrentTrack() to NAME_None, so the subsystem never believes a
	 * bed is playing when none is. The same is true of @p Track == AmbienceMatch while the match
	 * ambience is switched off (bMatchAmbienceEnabled False, the shipped state since 2026-10-07, or
	 * `Trace.Music.Ambience 0`): the bed that was up fades out and nothing replaces it.
	 * Callers do not need to know: the contract "call it unconditionally, it does the right thing"
	 * is exactly what makes one flag able to turn the beds off without touching a call site.
	 */
	void Play(FName Track, float FadeSeconds = 0.8f);

	/** Fade whatever is playing to silence and stop it. Safe to call when nothing plays. */
	void Stop(float FadeSeconds = 0.8f);

	/**
	 * The track Play() would currently treat as already-playing. NAME_None when stopped — and always
	 * NAME_None while the beds are disabled, because Play() clears it rather than recording a track
	 * it did not start.
	 *
	 * *** THIS IS NOT A WAY TO ASK WHETHER THE BEDS ARE DISABLED. *** NAME_None also means "the beds
	 * are on and nothing has started one yet" — mid-travel, before the first BeginPlay, or after a
	 * device refused the component. Callers that want the SWITCH must ask AreBedsEnabled(); a
	 * results-screen log line that inferred "beds are off" from this was measured contradicting
	 * itself two seconds later, in the run that flipped `Trace.Music.Beds 1` mid-match.
	 */
	FName GetCurrentTrack() const { return CurrentTrack; }

	/**
	 * Are the two music beds allowed to play at all right now?
	 *
	 * The single source of truth for the switch the owner asked for: `Trace.Music.Beds` (-1 follow
	 * config / 0 force off / 1 force on) over UTraceAudioSettings::bMusicBedsEnabled. Static, and
	 * legal with no subsystem, because the answer is a setting and not a piece of subsystem state.
	 *
	 * It exists so that code OUTSIDE this file can say "the beds are off" and be RIGHT. Play() is
	 * still the only thing that consults it to decide what to do; everyone else is describing.
	 */
	static bool AreBedsEnabled();

	/**
	 * Will Play(AmbienceMatch) start anything right now? True only when the beds are enabled AND
	 * the match ambience is: `Trace.Music.Ambience` (-1 follow config / 0 off / 1 on) over
	 * UTraceAudioSettings::bMatchAmbienceEnabled, which is False since the owner asked on
	 * 2026-10-07 to "remove ambient match track". Static for the same reason as AreBedsEnabled().
	 */
	static bool IsMatchAmbienceEnabled();

	/**
	 * Re-applies MasterVolume x MusicVolumeScale to the playing component, for the audio settings
	 * page: a slider drag should be heard NOW, not on the next track change. A no-op when no bed is
	 * playing — which, with the match ambience switched off, is the whole match; see IsBedPlaying().
	 */
	void RefreshVolume();

	/**
	 * Is a bed actually sounding right now: the component Play() last started, still playing?
	 *
	 * The question the options menu's MUSIC row needs answered: RefreshVolume() is only audible when
	 * this is true. False on the title screen while the beds are off, during the 1.9 s between the
	 * whistle and the results screen's Play(MusicTitle), and FOR THE WHOLE MATCH while the match
	 * ambience is switched off (the shipped state since 2026-10-07). The MUSIC row then plays a
	 * music sample of its own (FTraceOptionsMenu::PreviewAudioChange) instead of moving nothing.
	 *
	 * Not GetCurrentTrack(): that names the track the subsystem believes it owns, and a component the
	 * device refused or somebody stopped can leave it set with nothing audible behind it.
	 */
	bool IsBedPlaying() const;

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
