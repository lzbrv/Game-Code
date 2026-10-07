// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace.Music.Verify — the music beds over a whole lap, judged on what the MIXER is playing
// ===================================================================================================
//
// menu -> PLAY -> match -> full time -> results -> back to the menu, driven by the harness itself
// through the game's own entry points (the menu's StartMatch, the game mode's FinishMatch, and the
// game's own return to the title screen after the results).
//
// WHY IT EXISTS. On 2026-10-07 the owner asked to "remove ambient match track". The change is one
// switch, UTraceAudioSettings::bMatchAmbienceEnabled (False), read by UTraceMusicSubsystem::Play.
// "The subsystem refused the track" is a claim about the subsystem's bookkeeping. What the player
// gets is what the audio device is rendering, so every judgement below is made on three separate
// readings, and a bed counts as sounding if ANY of them sees it:
//
//   the mixer's active sounds      FAudioDevice::GetActiveSounds(), read with the audio thread held
//   and their wave instances       (the same way au.Debug.ListWaves reads them)
//   every playing UAudioComponent  TObjectIterator, IsPlaying(), matched by the asset's name
//
// plus the subsystem's own GetCurrentTrack(), checked against what it should say.
//
// WHAT ONE LAP CHECKS
//   MENU     MusicTitle is playing, one voice, and nothing of AmbienceMatch.
//   PLAY     pressed through ATraceMenuHUD::DebugStartMatch (the same StartMatch the PLAY row calls).
//   MATCH    7 s from the match HUD's BeginPlay, four samples a second:
//              switch OFF  no AmbienceMatch at ANY sample, from the first frame; track None.
//              switch ON   AmbienceMatch playing, one voice, at every sample from +2 s; track
//                          AmbienceMatch.
//              both        MusicTitle silent from +2 s (the 0.8 s fade-out at PLAY).
//   WHISTLE  ATraceGameMode::DebugFinishMatch, with the local team as the winner, so the VICTORY
//            stinger plays. Checked: the stinger is heard within 2.5 s; any ambience is gone within
//            1.25 s (its stop fade is 0.5 s) and none is there at all with the switch off; MusicTitle
//            rises 0.4-4 s after the whistle and not before the stinger (the HUD times it to the
//            stinger's tail, 1.9 s for the 2.8 s stinger); one MusicTitle voice under the results.
//   RETURN   MusicTitle at every sample from there through the travel to the title screen.
//   MENU     3 s after arriving: MusicTitle still playing, one voice, the SAME component that rose
//            under the results and still counting up, so the menu did not restart it; no ambience.
//
// TWO LAPS BY DEFAULT, AND THE SECOND ONE IS THE CONTROL. Lap 1 follows the switch as configured.
// Lap 2 sets `Trace.Music.Ambience` to the opposite (1 if lap 1 expected it off, 0 if on) and
// expects the opposite. A harness that passed lap 1 only because AmbienceMatch could not play at all
// (no asset, a dead device, a broken census) fails lap 2. The cvar is put back as it was at the end.
// `laps=1` runs lap 1 only.
//
// RED ARM: put Play()'s match-ambience gate back the way it was before 2026-10-07 (delete the block
// under "THE MATCH AMBIENCE IS SWITCHED OFF" in TraceMusicPlayer.cpp) and lap 1 must FAIL on the
// match checks with the committed config. That is how this harness was proved.
//
// SOUND MUST BE ON. Under -nosound there is no audio device and the run FAILS at the first look;
// nothing is skipped. On a Mac with the lid shut use -DeterministicAudio (the offline renderer).
// The harness keeps time on FApp::GetCurrentTime(), so it runs on the fixed step under that too.
//
// Headless recipe (sound on, about 100 s for two laps):
//   UnrealEditor Trace.uproject /Game/Maps/MainMenu -game -log -RenderOffScreen -nosplash -unattended
//     -TraceExecOn=Menu -TraceExecAt=2 "-TraceExec=Trace.Music.Verify quit"
// -TraceExec runs again on every title screen; a call while a run is going is ignored, and `quit`
// ends the process two seconds after the verdict.
// ===================================================================================================

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "ActiveSound.h"
#include "Audio.h"                         // FWaveInstance::WaveData
#include "AudioDevice.h"
#include "AudioThread.h"                   // FAudioThreadSuspendContext
#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectIterator.h"

#include "Audio/TraceMusicPlayer.h"
#include "Audio/TraceSoundBank.h"
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceMatchTypes.h"
#include "Core/TracePlayerState.h"
#include "Trace.h"                         // LogTraceGame
#include "TraceTypes.h"
#include "UI/TraceHUD.h"
#include "UI/TraceMenuHUD.h"

// Named after the file, not anonymous: Scripts/check-jumbo-build-collisions.py.
namespace TraceMusicVerify
{
	/** How long the match is watched before the whistle, from the match HUD's BeginPlay. */
	constexpr double MatchWatchSeconds = 7.0;

	/** The 0.8 s fade at PLAY plus margin. From here the title must be silent and the ambience settled. */
	constexpr double MatchSettleSeconds = 2.0;

	/** One look at the mixer every this many seconds. */
	constexpr double SamplePeriodSeconds = 0.25;

	/** After the whistle: the stinger must have been heard by this. */
	constexpr double StingerWithinSeconds = 2.5;

	/** After the whistle: a playing ambience must be gone by this (its stop fade is 0.5 s). */
	constexpr double AmbienceGoneSeconds = 1.25;

	/** After the whistle: the window the title's rise has to land in. The HUD aims for 1.9 s. */
	constexpr double TitleRiseEarliestSeconds = 0.4;
	constexpr double TitleRiseLatestSeconds = 4.0;

	/** After the whistle: when the results screen's own census is taken. */
	constexpr double ResultsCensusSeconds = 5.0;

	/** On the menu: how long it is given to settle before it is looked at. */
	constexpr double MenuSettleSeconds = 1.5;
	constexpr double MenuBackSettleSeconds = 3.0;

	/** A looping voice's playback clock may lag the harness clock by this much and still be "the same". */
	constexpr double PlaybackSlackSeconds = 1.0;

	/** Upper bounds on every wait, so a stuck run ends in a FAIL rather than hanging. */
	constexpr double MenuWaitLimitSeconds = 30.0;
	constexpr double TravelWaitLimitSeconds = 60.0;
	constexpr double ReturnWaitLimitSeconds = 60.0;

	enum class EStage : uint8
	{
		WaitMenu,     // the first title screen, before lap 1
		Travelling,   // PLAY pressed, waiting for the match HUD
		MatchWatch,   // the match, then the whistle
		Results,      // the whistle to the results census
		Returning,    // the results screen and the travel back
		MenuBack,     // the title screen after the lap
		Done,
	};

	/** One look at what is sounding, from three separate places, plus what the subsystem believes. */
	struct FCensus
	{
		bool bDevice = false;

		// The mixer: active sounds, and the wave instances under them.
		int32 AmbienceSounds = 0;
		int32 AmbienceWaves = 0;
		int32 TitleSounds = 0;
		int32 TitleWaves = 0;
		int32 StingerSounds = 0;
		float TitlePlaybackSeconds = -1.f;

		// Every playing UAudioComponent.
		int32 AmbienceComponents = 0;
		int32 TitleComponents = 0;
		TWeakObjectPtr<UAudioComponent> TitleComponent;

		// UTraceMusicSubsystem::GetCurrentTrack().
		FName SubsystemTrack;

		int32 AmbienceSeen() const { return FMath::Max3(AmbienceSounds, AmbienceWaves, AmbienceComponents); }
		int32 TitleSeen() const { return FMath::Max3(TitleSounds, TitleWaves, TitleComponents); }

		/** Exactly one voice of a bed, in every reading. */
		bool AmbienceIsOneVoice() const { return AmbienceSounds == 1 && AmbienceWaves >= 1 && AmbienceComponents == 1; }
		bool TitleIsOneVoice() const { return TitleSounds == 1 && TitleWaves >= 1 && TitleComponents == 1; }

		FString Describe() const
		{
			return FString::Printf(
				TEXT("[mixer: AmbienceMatch %d sound(s)/%d wave(s), MusicTitle %d/%d (playback %.2fs), stinger %d; ")
				TEXT("playing components: AmbienceMatch %d, MusicTitle %d; subsystem track '%s']"),
				AmbienceSounds, AmbienceWaves, TitleSounds, TitleWaves, TitlePlaybackSeconds, StingerSounds,
				AmbienceComponents, TitleComponents, *SubsystemTrack.ToString());
		}
	};

	struct FRun
	{
		EStage Stage = EStage::WaitMenu;
		double StageStart = 0.0;
		double NextSample = 0.0;
		double MenuSeenAt = -1.0;

		int32 Lap = 1;
		int32 Laps = 2;
		bool bQuitAtEnd = false;

		/** Trace.Music.Ambience as it was before the run; put back by Finish(). */
		int32 OverrideAtStart = -1;
		bool bLap1Expect = false;
		bool bExpectAmbience = false;
		FString LapSummary[2];

		int32 Passes = 0;
		int32 Failures = 0;

		// ---- the match window, this lap ----
		double MatchAt = -1.0;
		int32 MatchSamples = 0;
		int32 SettledSamples = 0;
		int32 AmbienceHeardSamples = 0;        // any ambience, any sample in the window
		int32 AmbienceRightSettledSamples = 0; // settled samples where the ambience is exactly as expected
		int32 TitleHeardSettledSamples = 0;    // settled samples where the title still sounded
		int32 TrackRightSettledSamples = 0;
		bool bTitleAtFirstSample = false;
		FString MatchWorst;

		// ---- the whistle and the results, this lap ----
		double WhistleAt = -1.0;
		ETraceTeam LocalTeam = ETraceTeam::None;
		ETraceTeam Winner = ETraceTeam::None;
		double StingerAt = -1.0;
		double TitleRoseAt = -1.0;
		int32 ResultsSamples = 0;
		int32 AmbienceEarlyResultsSamples = 0; // before AmbienceGoneSeconds
		int32 AmbienceLateResultsSamples = 0;  // after it
		TWeakObjectPtr<UAudioComponent> ResultsTitle;

		// ---- the return, this lap ----
		int32 ReturnSamples = 0;
		int32 ReturnSilentSamples = 0;
	};

	/** The one run, so a second -TraceExec on the next title screen cannot start another. */
	static TSharedPtr<FRun> GActiveRun;

	static double Clock()
	{
		// App time, not FPlatformTime: under -UseFixedTimeStep (the offline renderer) the fades and the
		// HUD's schedule run on the fixed step, and so must the windows that judge them.
		return FApp::GetCurrentTime();
	}

	static IConsoleVariable* AmbienceOverrideVar()
	{
		return IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Music.Ambience"));
	}

	static void Report(FRun& Run, bool bPass, const FString& Claim, const FString& Detail)
	{
		(bPass ? Run.Passes : Run.Failures) += 1;
		UE_LOG(LogTraceGame, Display, TEXT("[MusicVerify]   %-4s lap %d  %s  %s"),
			bPass ? TEXT("ok") : TEXT("FAIL"), Run.Lap, *Claim, *Detail);
	}

	static void GoTo(FRun& Run, EStage NextStage)
	{
		Run.Stage = NextStage;
		Run.StageStart = Clock();
		Run.NextSample = Run.StageStart;
	}

	/** The world a player is looking at: the game (or PIE) world. */
	static UWorld* ViewedWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* const Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld()
				&& (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** The local player's HUD, if it is of @p HUDClass and its BeginPlay has run. */
	template <typename HUDClass>
	static HUDClass* LiveHUDOf(UWorld* InWorld)
	{
		APlayerController* const LocalPC = (InWorld != nullptr) ? InWorld->GetFirstPlayerController() : nullptr;
		HUDClass* const Found = (LocalPC != nullptr) ? Cast<HUDClass>(LocalPC->GetHUD()) : nullptr;
		return (Found != nullptr && Found->HasActorBegunPlay()) ? Found : nullptr;
	}

	static FName AssetNameOf(FName Event)
	{
		return FName(*UTraceSoundBank::SoundAssetNameFor(Event));
	}

	static FCensus TakeCensus(UWorld* InWorld)
	{
		FCensus Out;
		const FName AmbienceAsset = AssetNameOf(TraceSoundEvents::AmbienceMatch);
		const FName TitleAsset = AssetNameOf(TraceSoundEvents::MusicTitle);
		const FName VictoryAsset = AssetNameOf(TraceSoundEvents::StingerVictory);
		const FName DefeatAsset = AssetNameOf(TraceSoundEvents::StingerDefeat);

		if (const UTraceMusicSubsystem* const Music = UTraceMusicSubsystem::Get(InWorld))
		{
			Out.SubsystemTrack = Music->GetCurrentTrack();
		}

		FAudioDevice* AudioDevice = (InWorld != nullptr) ? InWorld->GetAudioDeviceRaw() : nullptr;
		if (AudioDevice == nullptr && GEngine != nullptr)
		{
			AudioDevice = GEngine->GetMainAudioDeviceRaw();
		}
		if (AudioDevice != nullptr)
		{
			Out.bDevice = true;

			// The audio thread owns these lists. Holding it is what au.Debug.ListWaves does too, and while
			// it is held the game thread counts as the audio thread for GetActiveSounds()'s check.
			FAudioThreadSuspendContext AudioThreadHold;
			for (const FActiveSound* const EachSound : AudioDevice->GetActiveSounds())
			{
				const USoundBase* const Asset = (EachSound != nullptr) ? EachSound->GetSound() : nullptr;
				if (Asset == nullptr)
				{
					continue;
				}
				int32 WaveCount = 0;
				for (const auto& Entry : EachSound->GetWaveInstances())
				{
					if (Entry.Value != nullptr && Entry.Value->WaveData != nullptr)
					{
						++WaveCount;
					}
				}
				const FName AssetName = Asset->GetFName();
				if (AssetName == AmbienceAsset)
				{
					++Out.AmbienceSounds;
					Out.AmbienceWaves += WaveCount;
				}
				else if (AssetName == TitleAsset)
				{
					++Out.TitleSounds;
					Out.TitleWaves += WaveCount;
					Out.TitlePlaybackSeconds = FMath::Max(Out.TitlePlaybackSeconds, EachSound->PlaybackTime);
				}
				else if (AssetName == VictoryAsset || AssetName == DefeatAsset)
				{
					++Out.StingerSounds;
				}
			}
		}

		for (TObjectIterator<UAudioComponent> It; It; ++It)
		{
			UAudioComponent* const Component = *It;
			if (!IsValid(Component) || Component->IsTemplate() || Component->Sound == nullptr || !Component->IsPlaying())
			{
				continue;
			}
			const FName AssetName = Component->Sound->GetFName();
			if (AssetName == AmbienceAsset)
			{
				++Out.AmbienceComponents;
			}
			else if (AssetName == TitleAsset)
			{
				++Out.TitleComponents;
				Out.TitleComponent = Component;
			}
		}
		return Out;
	}

	/** au.Debug.* into the log at the moments a reader will want to check by eye. */
	static void Dump(UWorld* InWorld, bool bWithComponents)
	{
		if (GEngine == nullptr)
		{
			return;
		}
		GEngine->Exec(InWorld, TEXT("au.Debug.ListWaves"));
		if (bWithComponents)
		{
			GEngine->Exec(InWorld, TEXT("au.Debug.ListAudioComponents"));
		}
	}

	static void ResetLap(FRun& Run)
	{
		const int32 KeepLap = Run.Lap;
		const int32 KeepLaps = Run.Laps;
		const bool bKeepQuit = Run.bQuitAtEnd;
		const int32 KeepOverride = Run.OverrideAtStart;
		const bool bKeepLap1 = Run.bLap1Expect;
		const int32 KeepPasses = Run.Passes;
		const int32 KeepFailures = Run.Failures;
		const FString KeepSummary0 = Run.LapSummary[0];
		const FString KeepSummary1 = Run.LapSummary[1];

		Run = FRun();
		Run.Lap = KeepLap;
		Run.Laps = KeepLaps;
		Run.bQuitAtEnd = bKeepQuit;
		Run.OverrideAtStart = KeepOverride;
		Run.bLap1Expect = bKeepLap1;
		Run.Passes = KeepPasses;
		Run.Failures = KeepFailures;
		Run.LapSummary[0] = KeepSummary0;
		Run.LapSummary[1] = KeepSummary1;
	}

	/** Set this lap's switch, decide what it expects, and press PLAY. False ends the run. */
	static bool StartLap(FRun& Run, UWorld* InWorld, ATraceMenuHUD* MenuHUD)
	{
		IConsoleVariable* const OverrideVar = AmbienceOverrideVar();
		if (OverrideVar == nullptr)
		{
			Report(Run, false, TEXT("Trace.Music.Ambience exists"), TEXT("the cvar is not registered"));
			return false;
		}

		FString How;
		if (Run.Lap == 1)
		{
			How = (OverrideVar->GetInt() >= 0)
				? FString::Printf(TEXT("Trace.Music.Ambience %d (set before the run)"), OverrideVar->GetInt())
				: FString::Printf(TEXT("bMatchAmbienceEnabled=%s (config; Trace.Music.Ambience -1)"),
					UTraceAudioSettings::Get().bMatchAmbienceEnabled ? TEXT("True") : TEXT("False"));
		}
		else
		{
			// THE CONTROL: the opposite of lap 1, through the runtime override.
			const int32 Opposite = Run.bLap1Expect ? 0 : 1;
			OverrideVar->Set(Opposite, ECVF_SetByConsole);
			How = FString::Printf(TEXT("Trace.Music.Ambience %d (the control: the opposite of lap 1)"), Opposite);
			if (OverrideVar->GetInt() != Opposite)
			{
				Report(Run, false, TEXT("the control lap can set Trace.Music.Ambience"),
					FString::Printf(TEXT("asked for %d, it reads %d"), Opposite, OverrideVar->GetInt()));
				return false;
			}
		}

		Run.bExpectAmbience = UTraceMusicSubsystem::IsMatchAmbienceEnabled();
		if (Run.Lap == 1)
		{
			Run.bLap1Expect = Run.bExpectAmbience;
		}
		else
		{
			Report(Run, Run.bExpectAmbience != Run.bLap1Expect, TEXT("the control lap flips the switch"),
				FString::Printf(TEXT("lap 1 expected the ambience %s, lap 2 expects it %s"),
					Run.bLap1Expect ? TEXT("ON") : TEXT("OFF"), Run.bExpectAmbience ? TEXT("ON") : TEXT("OFF")));
		}
		Run.LapSummary[FMath::Clamp(Run.Lap - 1, 0, 1)] = FString::Printf(TEXT("lap %d ambience %s via %s"),
			Run.Lap, Run.bExpectAmbience ? TEXT("ON") : TEXT("OFF"), *How);

		UE_LOG(LogTraceGame, Display,
			TEXT("[MusicVerify] lap %d of %d: %s, so the match should have %s. Pressing PLAY."),
			Run.Lap, Run.Laps, *How,
			Run.bExpectAmbience ? TEXT("AmbienceMatch playing (one voice)") : TEXT("NO music bed at all"));
		Dump(InWorld, /*bWithComponents=*/false);
		MenuHUD->DebugStartMatch();
		GoTo(Run, EStage::Travelling);
		return true;
	}

	/** The checks shared by the first title screen and the one each lap returns to. */
	static void JudgeMenu(FRun& Run, const FCensus& Seen, const TCHAR* Where)
	{
		Report(Run, Seen.TitleIsOneVoice(), FString::Printf(TEXT("%s: MusicTitle is playing, one voice"), Where),
			Seen.Describe());
		Report(Run, Seen.SubsystemTrack == TraceSoundEvents::MusicTitle,
			FString::Printf(TEXT("%s: the music subsystem says MusicTitle"), Where), Seen.Describe());
		Report(Run, Seen.AmbienceSeen() == 0, FString::Printf(TEXT("%s: no AmbienceMatch"), Where), Seen.Describe());
	}

	static void JudgeMatch(FRun& Run)
	{
		if (Run.bExpectAmbience)
		{
			Report(Run, Run.SettledSamples > 0 && Run.AmbienceRightSettledSamples == Run.SettledSamples,
				TEXT("match (switch ON): AmbienceMatch playing, one voice, at every sample from +2 s"),
				FString::Printf(TEXT("%d of %d samples. %s"), Run.AmbienceRightSettledSamples, Run.SettledSamples, *Run.MatchWorst));
		}
		else
		{
			Report(Run, Run.MatchSamples > 0 && Run.AmbienceHeardSamples == 0,
				TEXT("match (switch OFF): no AmbienceMatch at any sample, from the first frame"),
				FString::Printf(TEXT("heard in %d of %d samples. %s"), Run.AmbienceHeardSamples, Run.MatchSamples, *Run.MatchWorst));
		}
		Report(Run, Run.SettledSamples > 0 && Run.TitleHeardSettledSamples == 0,
			TEXT("match: MusicTitle faded out at PLAY (silent at every sample from +2 s)"),
			FString::Printf(TEXT("still sounding in %d of %d samples; it %s at the first sample (the fade-out)"),
				Run.TitleHeardSettledSamples, Run.SettledSamples,
				Run.bTitleAtFirstSample ? TEXT("was still there") : TEXT("was already gone")));
		Report(Run, Run.SettledSamples > 0 && Run.TrackRightSettledSamples == Run.SettledSamples,
			FString::Printf(TEXT("match: the music subsystem says '%s'"),
				Run.bExpectAmbience ? TEXT("AmbienceMatch") : TEXT("None")),
			FString::Printf(TEXT("%d of %d samples"), Run.TrackRightSettledSamples, Run.SettledSamples));
	}

	static bool BlowWhistle(FRun& Run, UWorld* InWorld)
	{
		ATraceGameMode* const Rules = (InWorld != nullptr) ? InWorld->GetAuthGameMode<ATraceGameMode>() : nullptr;
		APlayerController* const LocalPC = (InWorld != nullptr) ? InWorld->GetFirstPlayerController() : nullptr;
		const ATracePlayerState* const LocalState = (LocalPC != nullptr) ? LocalPC->GetPlayerState<ATracePlayerState>() : nullptr;
		if (Rules == nullptr)
		{
			Report(Run, false, TEXT("the whistle can be blown"), TEXT("no ATraceGameMode here (a client cannot end the match)"));
			return false;
		}
		Run.LocalTeam = (LocalState != nullptr) ? LocalState->Team : ETraceTeam::None;
		Run.Winner = (Run.LocalTeam != ETraceTeam::None) ? Run.LocalTeam : ETraceTeam::Blue;
		UE_LOG(LogTraceGame, Display,
			TEXT("[MusicVerify] lap %d: full time now, %s wins (local team %s), so the %s stinger should play."),
			Run.Lap, *TraceTeamName(Run.Winner).ToString(), *TraceTeamName(Run.LocalTeam).ToString(),
			(Run.LocalTeam != ETraceTeam::None) ? TEXT("VICTORY") : TEXT("no"));
		Rules->DebugFinishMatch(Run.Winner, ETraceMatchEndReason::Clock);
		GoTo(Run, EStage::Results);
		Run.WhistleAt = Run.StageStart;
		return true;
	}

	static void JudgeResults(FRun& Run, const FCensus& Seen)
	{
		const double StingerDelay = (Run.StingerAt >= 0.0) ? Run.StingerAt - Run.WhistleAt : -1.0;
		const double RiseDelay = (Run.TitleRoseAt >= 0.0) ? Run.TitleRoseAt - Run.WhistleAt : -1.0;

		if (Run.LocalTeam != ETraceTeam::None)
		{
			Report(Run, StingerDelay >= 0.0 && StingerDelay <= StingerWithinSeconds,
				TEXT("full time: the VICTORY stinger is heard"),
				FString::Printf(TEXT("first heard %.2fs after the whistle"), StingerDelay));
		}
		else
		{
			Report(Run, StingerDelay < 0.0, TEXT("full time: no team, so no stinger"),
				FString::Printf(TEXT("first heard %.2fs after the whistle (-1 = never)"), StingerDelay));
		}

		if (Run.bExpectAmbience)
		{
			Report(Run, Run.AmbienceLateResultsSamples == 0,
				TEXT("full time: the ambience stops (gone within 1.25 s)"),
				FString::Printf(TEXT("still sounding in %d of the later results samples"), Run.AmbienceLateResultsSamples));
		}
		else
		{
			Report(Run, Run.AmbienceEarlyResultsSamples == 0 && Run.AmbienceLateResultsSamples == 0,
				TEXT("full time: no AmbienceMatch on the results screen"),
				FString::Printf(TEXT("heard in %d of %d results samples"),
					Run.AmbienceEarlyResultsSamples + Run.AmbienceLateResultsSamples, Run.ResultsSamples));
		}

		Report(Run, RiseDelay >= TitleRiseEarliestSeconds && RiseDelay <= TitleRiseLatestSeconds
				&& (Run.StingerAt < 0.0 || Run.TitleRoseAt >= Run.StingerAt),
			TEXT("results: MusicTitle rises under the stinger's tail, as before"),
			FString::Printf(TEXT("rose %.2fs after the whistle (window %.1f-%.1f s; the HUD aims for 1.9 s), stinger at %.2fs"),
				RiseDelay, TitleRiseEarliestSeconds, TitleRiseLatestSeconds, StingerDelay));
		Report(Run, Seen.TitleIsOneVoice() && Seen.SubsystemTrack == TraceSoundEvents::MusicTitle,
			TEXT("results: one MusicTitle voice under the results screen, and the subsystem says MusicTitle"),
			Seen.Describe());
		Run.ResultsTitle = Seen.TitleComponent;
	}

	static void JudgeMenuBack(FRun& Run, const FCensus& Seen)
	{
		JudgeMenu(Run, Seen, TEXT("back on the menu"));

		const UAudioComponent* const MenuVoice = Seen.TitleComponent.Get();
		const UAudioComponent* const ResultsVoice = Run.ResultsTitle.Get();
		const bool bSameVoice = (MenuVoice != nullptr && MenuVoice == ResultsVoice);
		const double SinceRise = (Run.TitleRoseAt >= 0.0) ? Clock() - Run.TitleRoseAt : -1.0;
		Report(Run, bSameVoice && SinceRise > 0.0 && Seen.TitlePlaybackSeconds >= SinceRise - PlaybackSlackSeconds,
			TEXT("back on the menu: the SAME MusicTitle voice that rose under the results (the menu did not restart it)"),
			FString::Printf(TEXT("component %s the results one; its playback is %.2fs, %.2fs since it rose"),
				bSameVoice ? TEXT("is") : TEXT("is NOT"), Seen.TitlePlaybackSeconds, SinceRise));
		Report(Run, Run.ReturnSamples > 0 && Run.ReturnSilentSamples == 0,
			TEXT("results -> menu: MusicTitle at every sample through the travel"),
			FString::Printf(TEXT("silent in %d of %d samples"), Run.ReturnSilentSamples, Run.ReturnSamples));
	}

	static void Finish(FRun& Run, const TCHAR* Why)
	{
		if (IConsoleVariable* const OverrideVar = AmbienceOverrideVar())
		{
			if (OverrideVar->GetInt() != Run.OverrideAtStart)
			{
				OverrideVar->Set(Run.OverrideAtStart, ECVF_SetByConsole);
			}
		}

		const bool bPass = (Run.Failures == 0 && Run.Passes > 0 && Why == nullptr);
		UE_LOG(LogTraceGame, Display, TEXT("[MusicVerify] %s%s%s"),
			*Run.LapSummary[0], Run.LapSummary[1].IsEmpty() ? TEXT("") : TEXT("; "), *Run.LapSummary[1]);
		UE_LOG(LogTraceGame, Display,
			TEXT("[MusicVerify] ===== VERDICT: %s — %d ok, %d FAIL%s%s. Trace.Music.Ambience is back at %d. ====="),
			bPass ? TEXT("PASS") : TEXT("FAIL"), Run.Passes, Run.Failures,
			(Why != nullptr) ? TEXT(", stopped early: ") : TEXT(""), (Why != nullptr) ? Why : TEXT(""),
			Run.OverrideAtStart);

		Run.Stage = EStage::Done;
		if (Run.bQuitAtEnd && GEngine != nullptr)
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float /*Delta*/) -> bool
			{
				if (GEngine != nullptr)
				{
					GEngine->Exec(ViewedWorld(), TEXT("quit"));
				}
				return false;
			}), 2.f);
		}
	}

	/** One frame. False when the run is over. */
	static bool Tick(FRun& Run)
	{
		UWorld* const Viewed = ViewedWorld();
		const double NowTime = Clock();
		const double InStage = NowTime - Run.StageStart;
		const bool bSampleDue = (NowTime >= Run.NextSample);
		if (bSampleDue)
		{
			Run.NextSample = NowTime + SamplePeriodSeconds;
		}

		switch (Run.Stage)
		{
		case EStage::WaitMenu:
		{
			ATraceMenuHUD* const MenuHUD = LiveHUDOf<ATraceMenuHUD>(Viewed);
			if (MenuHUD == nullptr)
			{
				if (InStage > MenuWaitLimitSeconds)
				{
					Finish(Run, TEXT("no title screen"));
					return false;
				}
				return true;
			}
			if (Run.MenuSeenAt < 0.0)
			{
				Run.MenuSeenAt = NowTime;
			}
			if (NowTime - Run.MenuSeenAt < MenuSettleSeconds)
			{
				return true;
			}
			const FCensus Seen = TakeCensus(Viewed);
			if (!Seen.bDevice)
			{
				Report(Run, false, TEXT("an audio device exists"), TEXT("none: is this -nosound? Nothing can be judged"));
				Finish(Run, TEXT("no audio device"));
				return false;
			}
			JudgeMenu(Run, Seen, TEXT("title screen"));
			if (!StartLap(Run, Viewed, MenuHUD))
			{
				Finish(Run, TEXT("could not start the lap"));
				return false;
			}
			return true;
		}

		case EStage::Travelling:
		{
			if (LiveHUDOf<ATraceHUD>(Viewed) != nullptr)
			{
				GoTo(Run, EStage::MatchWatch);
				Run.MatchAt = Run.StageStart;
				// No "seconds after PLAY" here: the app clock does not count the level load (the engine
				// clamps that frame's delta), so the number would read as a near-instant travel.
				UE_LOG(LogTraceGame, Display, TEXT("[MusicVerify] lap %d: the match HUD is up; watching the match for %.0fs."),
					Run.Lap, MatchWatchSeconds);
				return true;
			}
			if (InStage > TravelWaitLimitSeconds)
			{
				Report(Run, false, TEXT("PLAY reaches a match"), FString::Printf(TEXT("no match HUD after %.0fs"), InStage));
				Finish(Run, TEXT("no match"));
				return false;
			}
			return true;
		}

		case EStage::MatchWatch:
		{
			if (bSampleDue)
			{
				const FCensus Seen = TakeCensus(Viewed);
				const bool bSettled = (InStage >= MatchSettleSeconds);
				if (Run.MatchSamples == 0)
				{
					Run.bTitleAtFirstSample = (Seen.TitleSeen() > 0);
				}
				++Run.MatchSamples;
				if (Seen.AmbienceSeen() > 0)
				{
					++Run.AmbienceHeardSamples;
					if (!Run.bExpectAmbience && Run.MatchWorst.IsEmpty())
					{
						Run.MatchWorst = FString::Printf(TEXT("First heard at +%.2fs: %s"), InStage, *Seen.Describe());
					}
				}
				if (bSettled)
				{
					++Run.SettledSamples;
					const bool bAmbienceRight = Run.bExpectAmbience ? Seen.AmbienceIsOneVoice() : (Seen.AmbienceSeen() == 0);
					if (bAmbienceRight)
					{
						++Run.AmbienceRightSettledSamples;
					}
					else if (Run.bExpectAmbience && Run.MatchWorst.IsEmpty())
					{
						Run.MatchWorst = FString::Printf(TEXT("First wrong at +%.2fs: %s"), InStage, *Seen.Describe());
					}
					if (Seen.TitleSeen() > 0)
					{
						++Run.TitleHeardSettledSamples;
					}
					const FName WantTrack = Run.bExpectAmbience ? TraceSoundEvents::AmbienceMatch : FName(NAME_None);
					if (Seen.SubsystemTrack == WantTrack)
					{
						++Run.TrackRightSettledSamples;
					}
				}
				if (Run.MatchSamples == 17)   // ~4 s in: one by-eye record for the log
				{
					UE_LOG(LogTraceGame, Display, TEXT("[MusicVerify] lap %d: match at +%.2fs %s"), Run.Lap, InStage, *Seen.Describe());
					Dump(Viewed, /*bWithComponents=*/true);
				}
			}
			if (InStage >= MatchWatchSeconds)
			{
				JudgeMatch(Run);
				if (!BlowWhistle(Run, Viewed))
				{
					Finish(Run, TEXT("no whistle"));
					return false;
				}
			}
			return true;
		}

		case EStage::Results:
		{
			if (bSampleDue)
			{
				const FCensus Seen = TakeCensus(Viewed);
				++Run.ResultsSamples;
				if (Seen.StingerSounds > 0 && Run.StingerAt < 0.0)
				{
					Run.StingerAt = NowTime;
				}
				if (Seen.TitleSeen() > 0 && Run.TitleRoseAt < 0.0)
				{
					Run.TitleRoseAt = NowTime;
				}
				if (Seen.AmbienceSeen() > 0)
				{
					(InStage < AmbienceGoneSeconds ? Run.AmbienceEarlyResultsSamples : Run.AmbienceLateResultsSamples) += 1;
				}
				if (InStage >= ResultsCensusSeconds)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[MusicVerify] lap %d: results at +%.2fs %s"), Run.Lap, InStage, *Seen.Describe());
					Dump(Viewed, /*bWithComponents=*/false);
					JudgeResults(Run, Seen);
					GoTo(Run, EStage::Returning);
				}
			}
			return true;
		}

		case EStage::Returning:
		{
			ATraceMenuHUD* const MenuHUD = LiveHUDOf<ATraceMenuHUD>(Viewed);
			if (MenuHUD != nullptr)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[MusicVerify] lap %d: back on the title screen (%.1fs after the results census)."),
					Run.Lap, InStage);
				GoTo(Run, EStage::MenuBack);
				return true;
			}
			if (bSampleDue && Viewed != nullptr)
			{
				const FCensus Seen = TakeCensus(Viewed);
				++Run.ReturnSamples;
				if (Seen.TitleSeen() == 0)
				{
					++Run.ReturnSilentSamples;
				}
			}
			if (InStage > ReturnWaitLimitSeconds)
			{
				Report(Run, false, TEXT("the results screen returns to the menu"), FString::Printf(TEXT("still not there after %.0fs"), InStage));
				Finish(Run, TEXT("no return to the menu"));
				return false;
			}
			return true;
		}

		case EStage::MenuBack:
		{
			if (InStage < MenuBackSettleSeconds)
			{
				return true;
			}
			const FCensus Seen = TakeCensus(Viewed);
			UE_LOG(LogTraceGame, Display, TEXT("[MusicVerify] lap %d: menu at +%.2fs %s"), Run.Lap, InStage, *Seen.Describe());
			Dump(Viewed, /*bWithComponents=*/true);
			JudgeMenuBack(Run, Seen);

			if (Run.Lap >= Run.Laps)
			{
				Finish(Run, nullptr);
				return false;
			}
			ATraceMenuHUD* const MenuHUD = LiveHUDOf<ATraceMenuHUD>(Viewed);
			if (MenuHUD == nullptr)
			{
				Finish(Run, TEXT("the title screen went away before the next lap"));
				return false;
			}
			const int32 NextLap = Run.Lap + 1;
			ResetLap(Run);
			Run.Lap = NextLap;
			if (!StartLap(Run, Viewed, MenuHUD))
			{
				Finish(Run, TEXT("could not start the control lap"));
				return false;
			}
			return true;
		}

		case EStage::Done:
		default:
			return false;
		}
	}

	static void Start(const TArray<FString>& Args, UWorld* InWorld)
	{
		if (GActiveRun.IsValid() && GActiveRun->Stage != EStage::Done)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[MusicVerify] already running (lap %d); this call is ignored."), GActiveRun->Lap);
			return;
		}
		if (LiveHUDOf<ATraceMenuHUD>(InWorld) == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[MusicVerify] start this on the title screen: it presses PLAY itself. ===== VERDICT: FAIL ====="));
			return;
		}
		if (!UTraceMusicSubsystem::AreBedsEnabled())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[MusicVerify] the music beds are off (bMusicBedsEnabled / Trace.Music.Beds), so there is no title ")
				TEXT("music to check. Turn them on first. ===== VERDICT: FAIL ====="));
			return;
		}

		TSharedRef<FRun> Run = MakeShared<FRun>();
		for (const FString& Arg : Args)
		{
			int32 LapArg = 0;
			if (FParse::Value(*Arg, TEXT("laps="), LapArg))
			{
				Run->Laps = FMath::Clamp(LapArg, 1, 2);
			}
			Run->bQuitAtEnd |= Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase);
		}
		if (const IConsoleVariable* const OverrideVar = AmbienceOverrideVar())
		{
			Run->OverrideAtStart = OverrideVar->GetInt();
		}
		GoTo(*Run, EStage::WaitMenu);
		GActiveRun = Run;

		UE_LOG(LogTraceGame, Display,
			TEXT("[MusicVerify] ===== the music beds over %d lap(s): menu -> PLAY -> match -> full time -> results -> menu. ")
			TEXT("bMusicBedsEnabled=%s, bMatchAmbienceEnabled=%s, Trace.Music.Ambience %d ====="),
			Run->Laps, UTraceAudioSettings::Get().bMusicBedsEnabled ? TEXT("True") : TEXT("False"),
			UTraceAudioSettings::Get().bMatchAmbienceEnabled ? TEXT("True") : TEXT("False"), Run->OverrideAtStart);

		// The core ticker, not a world timer: the run has to outlive two level travels per lap.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float /*Delta*/) -> bool
		{
			return Tick(*Run);
		}), 0.f);
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdMusicVerify(
		TEXT("Trace.Music.Verify"),
		TEXT("Dev only. Start on the title screen with sound on. Drives menu -> PLAY -> match -> full time -> results -> ")
		TEXT("menu and checks, on the mixer: MusicTitle on the menu; in the match no AmbienceMatch while the match ambience ")
		TEXT("is switched off (bMatchAmbienceEnabled / Trace.Music.Ambience), one voice of it while on; the title fades ")
		TEXT("out at PLAY; the stinger at full time; MusicTitle rising under the results and carrying into the menu ")
		TEXT("unrestarted. Lap 2 flips Trace.Music.Ambience to the opposite as a control. laps=1 skips it; quit exits ")
		TEXT("after the verdict."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Start));
}

#endif // !UE_BUILD_SHIPPING
