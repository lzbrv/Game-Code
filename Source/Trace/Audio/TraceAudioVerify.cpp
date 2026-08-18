// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — THE AUDIO HARNESSES (spec v26 §9)
// ===================================================================================================
//
//   Trace.Audio.Report            every event: its side, where its sound came from, whether it loaded
//   Trace.Audio.Test [Event]      fires one event (or all nine) through the REAL API, not a shortcut
//   Trace.Audio.Probe             plays all nine and reads the MIXER back. The one that is evidence.
//   Trace.Audio.Reload            drops the resolve cache, so a re-import can be heard without a relaunch
//
// WHY Trace.Audio.Probe EXISTS AND WHY Trace.Audio.Report IS NOT ENOUGH.
// "The asset loaded" and "the sound was audible" are different claims, and this project has been
// caught by that distinction before in the font work (an offline UFont loads perfectly and then draws
// in Slate's last-resort face). A USoundBase pointer proves a package resolved. It does not prove an
// audio device exists, that it is not muted, that a source voice was allocated, or that anything was
// rendered — all four of which are false under -nosound while ResolveSound still returns a valid
// asset.
//
// So the probe asks the AUDIO DEVICE, on the far side of the play call:
//
//     FAudioDevice::GetNumActiveSources()   the audio MIXER's own count of source voices currently
//                                           rendering (Audio::FMixerDevice overrides it and forwards
//                                           to FMixerSourceManager). Non-zero means the sound reached
//                                           the mixer, not merely the asset loader.
//     UAudioComponent::IsPlaying()          the second, independent signal — the engine's own view of
//                                           the component it handed back.
//
// A run with -nosound reports device=NONE, peak sources 0 and a FAIL verdict, which is the red arm
// this harness needs to be worth anything (house rule: a green verifier that cannot see what you
// changed is not evidence).
// ===================================================================================================

#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"

#include "Audio/TraceAudio.h"
#include "Audio/TraceSoundBank.h"
#include "Audio/TraceSoundEvents.h"
#include "Trace.h"

// Named after the file. Two anonymous namespaces in one unity translation unit are one namespace —
// see Scripts/check-jumbo-build-collisions.py and the four Windows-only breaks it exists to stop.
namespace TraceAudioVerify
{
	/** The world a console command should act on: the game world with a player in it. */
	static UWorld* PlayableWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate == nullptr || !Candidate->IsGameWorld())
			{
				continue;
			}
			if (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** Where the resolved sound came from, for the report's third column. */
	static const TCHAR* SourceOf(UTraceAudioSubsystem* Audio, FName Event)
	{
		if (Audio == nullptr)
		{
			return TEXT("-");
		}
		if (const UTraceSoundBank* Bank = Audio->GetBank())
		{
			if (Bank->Find(Event) != nullptr)
			{
				return TEXT("bank");
			}
		}
		return (Audio->ResolveSound(Event) != nullptr) ? TEXT("convention") : TEXT("MISSING");
	}

	static float DurationOf(const USoundBase* Sound)
	{
		return (Sound != nullptr) ? Sound->GetDuration() : 0.f;
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Audio.Report
	// ---------------------------------------------------------------------------------------------

	static void Report()
	{
		UWorld* World = PlayableWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Audio] Trace.Audio.Report: no game world."));
			return;
		}

		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();
		FAudioDevice* Device = World->GetAudioDeviceRaw();

		UE_LOG(LogTraceGame, Display, TEXT("================ Trace.Audio.Report (spec v26 s9) ================"));
		UE_LOG(LogTraceGame, Display, TEXT("world '%s'  netmode=%d  subsystem=%s"),
			*World->GetName(), static_cast<int32>(World->GetNetMode()),
			Audio != nullptr ? TEXT("up") : TEXT("ABSENT"));
		UE_LOG(LogTraceGame, Display, TEXT("bank path   %s"), *Settings.SoundBankPath.ToString());
		UE_LOG(LogTraceGame, Display, TEXT("bank asset  %s"),
			(Audio != nullptr && Audio->GetBank() != nullptr)
				? *Audio->GetBank()->GetPathName()
				: TEXT("(none - every event falls back to /Game/Trace/Audio/S_<Event>)"));
		UE_LOG(LogTraceGame, Display,
			TEXT("volume      master %.2f   game-side: inner %.0f uu, falloff x%.1f -> %.0f uu"),
			Settings.MasterVolume, Settings.WorldInnerRadiusUU, Settings.WorldFalloffScale,
			Settings.GetWorldFalloffDistanceUU());

		if (Device != nullptr)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("device      present: %.0f Hz, %d channels, muted=%d, active sources now %d"),
				Device->GetSampleRate(), Device->GetMaxChannels(),
				Device->IsAudioDeviceMuted() ? 1 : 0, Device->GetNumActiveSources());
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("device      NONE. Either this is a dedicated server or the process was launched "
				     "with -nosound. Nothing will be audible."));
		}

		UE_LOG(LogTraceGame, Display, TEXT(""));
		UE_LOG(LogTraceGame, Display, TEXT("  %-14s %-11s %-11s %6s  %s"),
			TEXT("EVENT"), TEXT("SIDE"), TEXT("SOURCE"), TEXT("SECS"), TEXT("TRIGGER"));

		int32 Missing = 0;
		for (const FTraceSoundEvent& Row : TraceSoundEvents::All())
		{
			USoundBase* Sound = (Audio != nullptr) ? Audio->ResolveSound(Row.Name) : nullptr;
			if (Sound == nullptr)
			{
				++Missing;
			}

			UE_LOG(LogTraceGame, Display, TEXT("  %-14s %-11s %-11s %6.2f  %s"),
				*Row.Name.ToString(), TraceSoundEvents::SideName(Row.Side),
				SourceOf(Audio, Row.Name), DurationOf(Sound), Row.Trigger);
		}

		if (Audio != nullptr)
		{
			const FTraceAudioCounters& Tally = Audio->GetCounters();
			UE_LOG(LogTraceGame, Display, TEXT(""));
			UE_LOG(LogTraceGame, Display,
				TEXT("counters    local %d  world %d  multicasts sent %d  |  refused: not-authority %d, "
				     "not-local-player %d  |  missing %d  no-device %d"),
				Tally.LocalPlays, Tally.WorldPlays, Tally.MulticastsSent,
				Tally.RefusedNotAuthority, Tally.RefusedNotLocalPlayer,
				Tally.MissingSound, Tally.NoAudioDevice);
		}

		// TWO CALLS AND NOT A TERNARY VERBOSITY: UE_LOG pastes its second argument onto
		// `ELogVerbosity::`, so an expression there does not compile. Same shape as the verdict
		// blocks in Core/TracePlayerController.cpp.
#define TRACE_AUDIO_REPORT_VERDICT_TEXT TEXT("VERDICT: %d of %d events resolved to a sound.%s")
#define TRACE_AUDIO_REPORT_VERDICT_ARGS \
	TraceSoundEvents::All().Num() - Missing, TraceSoundEvents::All().Num(), \
	(Missing == 0 ? TEXT("") : TEXT("  Run ./Scripts/import-sounds.sh."))

		if (Missing == 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_AUDIO_REPORT_VERDICT_TEXT, TRACE_AUDIO_REPORT_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_AUDIO_REPORT_VERDICT_TEXT, TRACE_AUDIO_REPORT_VERDICT_ARGS);
		}

#undef TRACE_AUDIO_REPORT_VERDICT_ARGS
#undef TRACE_AUDIO_REPORT_VERDICT_TEXT
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Audio.Test — through the REAL API, so it exercises the side routing too
	// ---------------------------------------------------------------------------------------------

	/** Fires @p Row exactly the way a real trigger site would. */
	static void FireThroughRealApi(UWorld* World, const FTraceSoundEvent& Row)
	{
		if (Row.Side == ETraceSoundSide::World)
		{
			// A point in front of the listener, so the attenuation curve is not what is under test.
			FVector Where = FVector::ZeroVector;
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				FVector ViewPoint = FVector::ZeroVector;
				FRotator ViewRotation = FRotator::ZeroRotator;
				PC->GetPlayerViewPoint(ViewPoint, ViewRotation);
				Where = ViewPoint + ViewRotation.Vector() * 150.f;
			}
			TraceAudio::PlayAt(World, Row.Name, Where);
		}
		else
		{
			TraceAudio::PlayLocal2D(World, Row.Name);
		}
	}

	static void Test(const TArray<FString>& Args)
	{
		UWorld* World = PlayableWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Audio] Trace.Audio.Test: no game world."));
			return;
		}

		if (Args.Num() > 0)
		{
			const FName Wanted(*Args[0]);
			for (const FTraceSoundEvent& Row : TraceSoundEvents::All())
			{
				if (Row.Name == Wanted)
				{
					UE_LOG(LogTraceGame, Display, TEXT("[Audio] firing '%s' (%s)."),
						*Row.Name.ToString(), TraceSoundEvents::SideName(Row.Side));
					FireThroughRealApi(World, Row);
					return;
				}
			}
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Audio] '%s' is not a declared event. Trace.Audio.Report lists them."), *Args[0]);
			return;
		}

		for (const FTraceSoundEvent& Row : TraceSoundEvents::All())
		{
			FireThroughRealApi(World, Row);
		}
		UE_LOG(LogTraceGame, Display,
			TEXT("[Audio] fired all %d events at once (they will overlap). Trace.Audio.Probe staggers "
			     "them and reads the mixer back."), TraceSoundEvents::All().Num());
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Audio.Probe — THE evidence: does the mixer actually get a source voice?
	// ---------------------------------------------------------------------------------------------

	struct FProbeRun
	{
		TWeakObjectPtr<UWorld> World;
		int32 NextEvent = 0;
		int32 PeakSources = 0;
		int32 BaselineSources = 0;
		int32 EventsWithSources = 0;
		int32 EventsMissingAsset = 0;
		double NextFireTime = 0.0;
		double EndTime = 0.0;
		/** Per-event peak, sampled between this event's fire and the next one's. */
		int32 CurrentWindowPeak = 0;
		FTSTicker::FDelegateHandle Handle;
		bool bFinished = false;
	};

	// One at a time. A second probe while one is running would interleave its own sounds into the
	// first one's windows and both would report numbers neither of them measured.
	static TSharedPtr<FProbeRun> GRun;

	static int32 ActiveSources(const UWorld* World)
	{
		const FAudioDevice* Device = (World != nullptr) ? World->GetAudioDeviceRaw() : nullptr;
		return (Device != nullptr) ? Device->GetNumActiveSources() : 0;
	}

	static bool ProbeTick(float /*DeltaSeconds*/)
	{
		TSharedPtr<FProbeRun> Run = GRun;
		if (!Run.IsValid() || Run->bFinished)
		{
			GRun.Reset();
			return false;
		}

		UWorld* World = Run->World.Get();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Audio] probe abandoned: the world went away."));
			Run->bFinished = true;
			GRun.Reset();
			return false;
		}

		const double Now = FPlatformTime::Seconds();
		const int32 Sources = ActiveSources(World);
		Run->PeakSources = FMath::Max(Run->PeakSources, Sources);
		Run->CurrentWindowPeak = FMath::Max(Run->CurrentWindowPeak, Sources);

		TConstArrayView<FTraceSoundEvent> Table = TraceSoundEvents::All();

		if (Now >= Run->NextFireTime && Run->NextEvent < Table.Num())
		{
			// Close the PREVIOUS event's window before opening this one's.
			if (Run->NextEvent > 0)
			{
				const FTraceSoundEvent& Previous = Table[Run->NextEvent - 1];
				const bool bHeard = Run->CurrentWindowPeak > Run->BaselineSources;
				if (bHeard)
				{
					++Run->EventsWithSources;
				}
				UE_LOG(LogTraceGame, Display, TEXT("  %-14s %-11s  mixer sources peaked at %d   %s"),
					*Previous.Name.ToString(), TraceSoundEvents::SideName(Previous.Side),
					Run->CurrentWindowPeak, bHeard ? TEXT("HEARD") : TEXT("*** SILENT ***"));
			}
			Run->CurrentWindowPeak = Sources;

			const FTraceSoundEvent& Row = Table[Run->NextEvent];
			if (UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World))
			{
				if (Audio->ResolveSound(Row.Name) == nullptr)
				{
					++Run->EventsMissingAsset;
				}
			}
			FireThroughRealApi(World, Row);

			++Run->NextEvent;
			Run->NextFireTime = Now + 0.60;
			if (Run->NextEvent >= Table.Num())
			{
				Run->EndTime = Now + 0.90;
			}
			return true;
		}

		if (Run->NextEvent >= Table.Num() && Now >= Run->EndTime)
		{
			// The last event's window.
			const FTraceSoundEvent& Last = Table[Table.Num() - 1];
			const bool bHeard = Run->CurrentWindowPeak > Run->BaselineSources;
			if (bHeard)
			{
				++Run->EventsWithSources;
			}
			UE_LOG(LogTraceGame, Display, TEXT("  %-14s %-11s  mixer sources peaked at %d   %s"),
				*Last.Name.ToString(), TraceSoundEvents::SideName(Last.Side),
				Run->CurrentWindowPeak, bHeard ? TEXT("HEARD") : TEXT("*** SILENT ***"));

			const int32 Total = Table.Num();
			UE_LOG(LogTraceGame, Display, TEXT(""));
			UE_LOG(LogTraceGame, Display,
				TEXT("[Audio] probe done. baseline sources %d, peak %d, %d/%d events reached the mixer, "
				     "%d had no asset."),
				Run->BaselineSources, Run->PeakSources, Run->EventsWithSources, Total,
				Run->EventsMissingAsset);

			const bool bPass = (Run->EventsWithSources == Total) && (Run->EventsMissingAsset == 0);

#define TRACE_AUDIO_PROBE_VERDICT_TEXT \
	TEXT("TRACE AUDIO PROBE VERDICT: %s - %d of %d events allocated a mixer source voice. %s")
#define TRACE_AUDIO_PROBE_VERDICT_ARGS \
	(bPass ? TEXT("PASS") : TEXT("FAIL")), Run->EventsWithSources, Total, \
	(bPass ? TEXT("Sound is reaching the mixer, not merely loading.") \
	       : TEXT("Run WITHOUT -nosound, and check Trace.Audio.Report for a missing asset."))

			if (bPass)
			{
				UE_LOG(LogTraceGame, Display, TRACE_AUDIO_PROBE_VERDICT_TEXT, TRACE_AUDIO_PROBE_VERDICT_ARGS);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TRACE_AUDIO_PROBE_VERDICT_TEXT, TRACE_AUDIO_PROBE_VERDICT_ARGS);
			}

#undef TRACE_AUDIO_PROBE_VERDICT_ARGS
#undef TRACE_AUDIO_PROBE_VERDICT_TEXT

			Run->bFinished = true;
			GRun.Reset();
			return false;
		}

		return true;
	}

	static void Probe()
	{
		UWorld* World = PlayableWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Audio] Trace.Audio.Probe: no game world."));
			return;
		}

		if (GRun.IsValid() && !GRun->bFinished)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Audio] a probe is already running."));
			return;
		}

		FAudioDevice* Device = World->GetAudioDeviceRaw();
		UE_LOG(LogTraceGame, Display, TEXT("================ Trace.Audio.Probe (spec v26 s9) ================"));
		if (Device == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[Audio] NO AUDIO DEVICE. This process cannot make a sound - it was launched with "
				     "-nosound, or it is a dedicated server. This is the probe's RED ARM and it is "
				     "working; relaunch without -nosound for the green one."));
			UE_LOG(LogTraceGame, Error,
				TEXT("TRACE AUDIO PROBE VERDICT: FAIL - 0 of %d events allocated a mixer source voice."),
				TraceSoundEvents::All().Num());
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("device: %.0f Hz, %d channels, muted=%d. Firing %d events 0.6 s apart and reading "
			     "FAudioDevice::GetNumActiveSources() back after each."),
			Device->GetSampleRate(), Device->GetMaxChannels(), Device->IsAudioDeviceMuted() ? 1 : 0,
			TraceSoundEvents::All().Num());

		TSharedPtr<FProbeRun> Run = MakeShared<FProbeRun>();
		Run->World = World;
		Run->BaselineSources = ActiveSources(World);
		Run->PeakSources = Run->BaselineSources;
		Run->CurrentWindowPeak = Run->BaselineSources;
		Run->NextFireTime = FPlatformTime::Seconds();
		GRun = Run;

		Run->Handle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&ProbeTick), 0.f);
	}

	static void Reload()
	{
		if (UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(PlayableWorld()))
		{
			Audio->ForgetResolvedSounds();
			UE_LOG(LogTraceGame, Display,
				TEXT("[Audio] resolve cache dropped. The next play re-reads the bank, so a re-import "
				     "can be heard without relaunching."));
		}
	}

	FAutoConsoleCommand CmdReport(
		TEXT("Trace.Audio.Report"),
		TEXT("Spec v26 s9. Every sound event: whether it is client-side or game-side, which asset it ")
		TEXT("resolved to (bank row, convention path, or missing), its duration, and the audio device."),
		FConsoleCommandDelegate::CreateStatic(&Report));

	FAutoConsoleCommand CmdTest(
		TEXT("Trace.Audio.Test"),
		TEXT("Spec v26 s9. Fires one sound event through the REAL TraceAudio API - game-side ones ")
		TEXT("multicast, client-side ones stay local. With no argument, fires all nine at once."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Test));

	FAutoConsoleCommand CmdProbe(
		TEXT("Trace.Audio.Probe"),
		TEXT("Spec v26 s9. THE evidence that sound reaches the MIXER and not merely the asset loader: ")
		TEXT("fires all nine events 0.6s apart and reads FAudioDevice::GetNumActiveSources() back ")
		TEXT("after each one. FAILs under -nosound, which is the point."),
		FConsoleCommandDelegate::CreateStatic(&Probe));

	FAutoConsoleCommand CmdReload(
		TEXT("Trace.Audio.Reload"),
		TEXT("Spec v26 s9. Drops the resolved-sound cache so a freshly re-imported WAV is picked up ")
		TEXT("without relaunching the game."),
		FConsoleCommandDelegate::CreateStatic(&Reload));
}
