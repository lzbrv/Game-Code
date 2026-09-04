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
// SPEC v28 §2 — Trace.Audio.TurnoverArm drives the SHIPPING rules, not the audio API: the Core's own
// drop, the Core's own grant, and the health component's own death. See the block by CmdTurnoverArm.
#include "Core/TraceCharacter.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Trace.h"
#include "TraceTypes.h"

#if !UE_BUILD_SHIPPING

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

			// DEMO 29 items 9 and 11: an unwired event still RESOLVES - its WAV is on disk and its
			// USoundWave is in the bank, which is what the rest of this row measures - but it is not
			// allowed to sound. Marking the row is the difference between this report saying "the
			// bank is wired" (true) and a reader taking it to mean "you will hear this" (not true).
			const bool bUnwired = TraceSoundEvents::IsUnwired(Row.Name);
			UE_LOG(LogTraceGame, Display, TEXT("  %-14s %-11s %-11s %6.2f  %s%s"),
				*Row.Name.ToString(), TraceSoundEvents::SideName(Row.Side),
				SourceOf(Audio, Row.Name), DurationOf(Sound),
				bUnwired ? TEXT("[UNWIRED] ") : TEXT(""), Row.Trigger);
		}

		if (Audio != nullptr)
		{
			const FTraceAudioCounters& Tally = Audio->GetCounters();
			UE_LOG(LogTraceGame, Display, TEXT(""));
			UE_LOG(LogTraceGame, Display,
				TEXT("counters    local %d  world %d  multicasts sent %d  |  refused: not-authority %d, "
				     "not-local-player %d, unwired %d  |  missing %d  no-device %d"),
				Tally.LocalPlays, Tally.WorldPlays, Tally.MulticastsSent,
				Tally.RefusedNotAuthority, Tally.RefusedNotLocalPlayer, Tally.RefusedUnwired,
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

	// =============================================================================================
	// Trace.Audio.TurnoverArm — SPEC v28 §2's RED ARM, AND IT MEASURES ITS OWN RULE
	// =============================================================================================
	//
	//   "The core turnover sound should play globally when the core is dropped by a team or a carrier
	//    is killed (a turnover), NOT when a team picks up a core which was locked out."
	//
	// The spec's instruction is "force a turnover and hear it once; then pick the Core up and hear
	// nothing", and a harness that only counted the first half would pass on a build that played the
	// sound on every event in the game. So this drives FOUR moments and asserts a count for each:
	//
	//   A  the carrier DROPS it (throws)                  CoreTurnover must go up by exactly 1
	//   B  the thrown Core lands and the lockout opens     ... by exactly 0   <- the OLD trigger
	//   C  somebody PICKS THE CORE UP afterwards           ... by exactly 0   <- the owner's "NOT"
	//   D  the carrier is KILLED                           ... by exactly 1   <- new in v28
	//
	// THE ARM IS Trace.Audio.TurnoverEdge, AND THE TWO ARMS DISAGREE ON THREE OF THE FOUR ROWS. At
	// edge 0 (pre-v28) A is 0, B is 1 and D is 0, so this harness goes red — which is what makes a
	// green run at edge 1 evidence rather than an assertion. C is 0 in both arms, and that is stated
	// rather than hidden: the pickup never made a sound, so the "NOT" half of the sentence is a
	// regression guard, not a fix.
	//
	// It reads UTraceAudioSubsystem::GetPlaysByEvent(), which is bumped inside PlayLocalNow /
	// PlayWorldNow — i.e. AFTER the side gate, the settings gate, the device test and the resolve. A
	// row that moves means a real trigger handed a real sound to the engine on this machine.
	//
	// RUN IT WITHOUT -nosound. With -nosound nothing reaches the engine, every row reads 0, and the
	// verdict is a FAIL that says so.
	//
	//     "$UE" "$PWD/Trace.uproject" \
	//         "/Game/Maps/Arena_Baked?game=/Script/Trace.TracePracticeGameMode" \
	//         -game -windowed -ResX=1280 -ResY=720 -LogCmds="LogTraceGame Verbose" \
	//         -TraceExec="Trace.Audio.TurnoverArm" -TraceExecAt=10 -TraceExecOn=Match

	/**
	 * Which arm is live, read off the console rather than from a copy.
	 *
	 * Trace.Audio.TurnoverEdge is declared in Gameplay/TraceCore.cpp, next to the rule it switches. It
	 * is looked up by NAME here instead of being re-declared: two TAutoConsoleVariables with the same
	 * name is a registration warning and, worse, a second default that can silently disagree with the
	 * first. -1 means "the CVar is not registered", which would mean the Core slice is not in this
	 * build at all.
	 */
	static int32 TraceAudioTurnoverEdgeValue()
	{
		if (const IConsoleVariable* CVar =
			IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Audio.TurnoverEdge")))
		{
			return CVar->GetInt();
		}
		return -1;
	}

	/** One asserted moment. */
	struct FTurnoverArmStep
	{
		const TCHAR* Label = TEXT("");
		int32 Expected = 0;
		int32 Observed = -1;
		bool  bRun = false;
	};

	struct FTurnoverArmRun
	{
		TWeakObjectPtr<UWorld> World;
		FTSTicker::FDelegateHandle Handle;
		double StartTime = 0.0;
		int32 Stage = 0;
		int32 Baseline = 0;
		bool bFinished = false;
		bool bAborted = false;
		FTurnoverArmStep Steps[4];
	};

	static TSharedPtr<FTurnoverArmRun> GTurnoverArm;

	/** How many CoreTurnover plays have reached the engine on this machine so far. */
	static int32 TurnoverPlays(UWorld* World)
	{
		if (const UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World))
		{
			if (const int32* Found = Audio->GetPlaysByEvent().Find(TraceSoundEvents::CoreTurnover))
			{
				return *Found;
			}
		}
		return 0;
	}

	static ATraceCharacter* ArmPawn(UWorld* World)
	{
		APlayerController* PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
		return (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
	}

	/** Closes the step that was open, recording what actually happened between then and now. */
	static void CloseStep(FTurnoverArmRun& Run, int32 Index, int32 NowCount)
	{
		if (Index >= 0 && Index < UE_ARRAY_COUNT(Run.Steps))
		{
			Run.Steps[Index].Observed = NowCount - Run.Baseline;
			Run.Steps[Index].bRun = true;

			const FTurnoverArmStep& Step = Run.Steps[Index];
			if (Step.Observed == Step.Expected)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[TurnoverArm]   OK    %-46s expected %d, heard %d"),
					Step.Label, Step.Expected, Step.Observed);
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[TurnoverArm]   WRONG %-46s expected %d, heard %d"),
					Step.Label, Step.Expected, Step.Observed);
			}
		}
		Run.Baseline = NowCount;
	}

	static bool TurnoverArmTick(float /*Delta*/)
	{
		TSharedPtr<FTurnoverArmRun> Run = GTurnoverArm;
		if (!Run.IsValid() || Run->bFinished)
		{
			return false;
		}

		UWorld* World = Run->World.Get();
		ATraceCore* Core = (World != nullptr) ? ATraceCore::Get(World) : nullptr;
		ATraceCharacter* Pawn = ArmPawn(World);

		if (World == nullptr || Core == nullptr || !IsValid(Pawn) || !Core->HasAuthority())
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[TurnoverArm] ABORTED: world/core/pawn/authority went away mid-run. Nothing is "
				     "claimed about the rule."));
			Run->bFinished = true;
			Run->bAborted = true;
			GTurnoverArm.Reset();
			return false;
		}

		const double Elapsed = FPlatformTime::Seconds() - Run->StartTime;
		const int32 Now = TurnoverPlays(World);

		switch (Run->Stage)
		{
		case 0:
			// SETUP. Put the Core in the local pawn's hands. This plays CorePickup, never CoreTurnover.
			if (Elapsed < 0.2) { return true; }
			Core->GrantTo(Pawn, ETraceCoreGrantReason::Debug);
			UE_LOG(LogTraceGame, Display,
				TEXT("[TurnoverArm] setup: Core granted to %s. edge=%d (1 = spec v28 §2, 0 = the pre-v28 "
				     "red arm). CoreTurnover plays so far: %d."),
				*GetNameSafe(Pawn), TraceAudioTurnoverEdgeValue(), Now);
			Run->Baseline = TurnoverPlays(World);
			++Run->Stage;
			return true;

		case 1:
			// A — THE DROP. ThrowFromHolder is the shipping drop: it is the exact function
			// RequestPassInput's release branch calls one line further in, so this is the player's own
			// path minus the key and the RPC. -1 means "full charge", which keeps the throw comparable
			// with every other diagnostic in the project.
			if (Elapsed < 1.2) { return true; }
			if (!IsValid(Core->GetCarrier()))
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[TurnoverArm] the grant did not stick (the practice range's Core RACK takes it "
					     "back). Re-granting and waiting."));
				Core->GrantTo(Pawn, ETraceCoreGrantReason::Debug);
				return true;
			}
			UE_LOG(LogTraceGame, Display, TEXT("[TurnoverArm] A: the carrier DROPS the Core (throws it)."));
			Core->ThrowFromHolder(Pawn, -1.f);
			++Run->Stage;
			return true;

		case 2:
			// Close A a beat after the throw so the multicast has been dispatched and counted.
			if (Elapsed < 1.6) { return true; }
			CloseStep(*Run, 0, Now);
			++Run->Stage;
			return true;

		case 3:
			// B — THE LANDING. Long enough for the Core to fly, land, settle and register the turnover.
			// This is the edge the sound used to be on, and it must now be silent.
			if (Elapsed < 5.2) { return true; }
			UE_LOG(LogTraceGame, Display,
				TEXT("[TurnoverArm] B: the throw has landed. turnover window active=%d, %.2fs left - the "
				     "lockout HAS opened, so a sound here would be the pre-v28 edge."),
				Core->IsTurnoverActive() ? 1 : 0, Core->GetTurnoverSecondsRemaining());
			CloseStep(*Run, 1, Now);
			++Run->Stage;
			return true;

		case 4:
			// C — THE PICKUP. "NOT when a team picks up a core which was locked out."
			if (Elapsed < 5.6) { return true; }
			UE_LOG(LogTraceGame, Display,
				TEXT("[TurnoverArm] C: somebody PICKS THE CORE UP off the ground."));
			Core->GrantTo(Pawn, ETraceCoreGrantReason::Debug);
			++Run->Stage;
			return true;

		case 5:
			if (Elapsed < 6.4) { return true; }
			CloseStep(*Run, 2, Now);
			++Run->Stage;
			return true;

		case 6:
		{
			// D — THE KILL. The real death path: UTraceHealthComponent::Kill fires OnDeath, which is
			// what ATraceGameMode::NotifyCharacterDied (-> DropAt) and ATraceCore::OnHolderDeath both
			// listen to. Two handlers, one death, and the de-dup in AnnounceTurnoverSound is what makes
			// the expected answer 1 rather than 2 — so this row also tests the de-dup.
			if (Elapsed < 6.8) { return true; }
			if (!IsValid(Core->GetCarrier()))
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[TurnoverArm] nobody is carrying the Core before the kill step; re-granting."));
				Core->GrantTo(Pawn, ETraceCoreGrantReason::Debug);
				return true;
			}
			ATraceCharacter* Victim = Core->GetCarrier();
			UE_LOG(LogTraceGame, Display, TEXT("[TurnoverArm] D: the CARRIER (%s) is KILLED."),
				*GetNameSafe(Victim));
			if (UTraceHealthComponent* Health = Victim->Health)
			{
				Health->Kill(nullptr, TEXT("Trace.Audio.TurnoverArm"));
			}
			++Run->Stage;
			return true;
		}

		case 7:
			if (Elapsed < 7.6) { return true; }
			CloseStep(*Run, 3, Now);
			++Run->Stage;
			return true;

		default:
			break;
		}

		// ---- VERDICT --------------------------------------------------------------------------
		int32 Correct = 0;
		for (const FTurnoverArmStep& Step : Run->Steps)
		{
			Correct += (Step.bRun && Step.Observed == Step.Expected) ? 1 : 0;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[TurnoverArm] ---- spec v28 §2: which EVENT the CoreTurnover sound belongs to ----"));
		for (const FTurnoverArmStep& Step : Run->Steps)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[TurnoverArm]   %-46s expected %d  heard %d"),
				Step.Label, Step.Expected, Step.bRun ? Step.Observed : -1);
		}

		if (Correct == UE_ARRAY_COUNT(Run->Steps))
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("TRACE TURNOVER ARM VERDICT: PASS - %d of %d moments sounded exactly as spec v28 §2 "
				     "asks (edge=%d)."),
				Correct, static_cast<int32>(UE_ARRAY_COUNT(Run->Steps)), TraceAudioTurnoverEdgeValue());
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("TRACE TURNOVER ARM VERDICT: FAIL - %d of %d moments sounded as spec v28 §2 asks "
				     "(edge=%d). At edge 0 this SHOULD fail; that is the red arm."),
				Correct, static_cast<int32>(UE_ARRAY_COUNT(Run->Steps)), TraceAudioTurnoverEdgeValue());
		}

		Run->bFinished = true;
		GTurnoverArm.Reset();
		return false;
	}

	static void TurnoverArm()
	{
		UWorld* World = PlayableWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[TurnoverArm] no game world."));
			return;
		}

		ATraceCore* Core = ATraceCore::Get(World);
		if (Core == nullptr || !Core->HasAuthority())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[TurnoverArm] no Core, or this machine is not the server. Run it on the host: the "
				     "sound is game-side and only the authority may announce it."));
			return;
		}
		if (GTurnoverArm.IsValid() && !GTurnoverArm->bFinished)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[TurnoverArm] a run is already in progress."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Audio.TurnoverArm (spec v28 s2) ================"));
		if (World->GetAudioDeviceRaw() == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[TurnoverArm] NO AUDIO DEVICE (-nosound, or a dedicated server). Every row will "
				     "read 0 and the verdict will FAIL - that is the harness's own red arm, not a "
				     "statement about the rule. Relaunch without -nosound."));
		}

		TSharedPtr<FTurnoverArmRun> Run = MakeShared<FTurnoverArmRun>();
		Run->World = World;
		Run->StartTime = FPlatformTime::Seconds();
		Run->Steps[0] = { TEXT("A  the carrier DROPS the Core (throws it)"),      1, -1, false };
		Run->Steps[1] = { TEXT("B  the throw LANDS and the lockout opens"),       0, -1, false };
		Run->Steps[2] = { TEXT("C  somebody PICKS THE CORE UP afterwards"),       0, -1, false };
		Run->Steps[3] = { TEXT("D  the CARRIER IS KILLED"),                       1, -1, false };
		GTurnoverArm = Run;

		Run->Handle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&TurnoverArmTick), 0.f);
	}

	FAutoConsoleCommand CmdTurnoverArm(
		TEXT("Trace.Audio.TurnoverArm"),
		TEXT("SPEC v28 s2. Drives four moments - the drop, the landing, the pickup and a carrier's ")
		TEXT("death - and asserts how many times the CoreTurnover sound reached the engine at each. ")
		TEXT("Set Trace.Audio.TurnoverEdge 0 first for the RED arm, which must FAIL."),
		FConsoleCommandDelegate::CreateStatic(&TurnoverArm));
}

#endif // !UE_BUILD_SHIPPING
