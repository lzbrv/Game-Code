// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — Trace.Audio.Loudness and Trace.Audio.Sides   (spec v29 §1a, §1b)
// ===================================================================================================
//
//   Trace.Audio.Sides      §1a. The nine spec v26 events, their sides, and a PASS/FAIL against the
//                          sides v26 shipped. "We kept the split" is a claim; this checks it.
//   Trace.Audio.Loudness   §1b. MEASURES every event and proves the footsteps are audibly quieter
//                          than all of them.
//   Trace.Audio.FootstepVolume <x>   the knob, from the console. 1.0 is Loudness's RED ARM.
//
// ---------------------------------------------------------------------------------------------------
// WHY MEASURING IS THE WHOLE POINT OF §1b, AND WHAT A NUMBER IN AN INI WOULD HAVE MISSED
// ---------------------------------------------------------------------------------------------------
//     "They must be audibly quieter than every other sound: give footsteps their own volume knob and
//      set it well below the bank default, then MEASURE it rather than trusting the number."
//
// A volume knob does not settle loudness, because the FILES ARE NOT EQUALLY LOUD. Measured from the
// samples themselves — this command from the imported asset, Scripts/import_sounds.py from the WAV,
// and the two agree:
//
//     loudest footstep    Step2         -20.06 dBFS peak
//     quietest other      ButtonPress   -27.21 dBFS peak
//
// AT UNITY GAIN THE FOOTSTEPS ARE THE LOUDER OF THE TWO, by 7.15 dB. A footstep knob of "0.5, that
// feels quiet" would have shipped §1b broken and every report would have said it was done. So this
// command multiplies each clip's measured peak by the gain the audio system will ACTUALLY hand the
// engine for that event — UTraceAudioSubsystem::VolumeFor, the same function PlayLocalNow and
// PlayWorldNow call, asked rather than re-derived — and compares the results.
//
// ---------------------------------------------------------------------------------------------------
// THE THREE INDEPENDENT SIGNALS, AND WHY THERE ARE THREE
// ---------------------------------------------------------------------------------------------------
//   1. THE ASSET'S OWN SAMPLES. USoundWave::GetImportedSoundWaveData gives the PCM the importer put
//      in the .uasset, so the peak is the audio the game will actually render — not the WAV on disk,
//      which is a different file that could have failed to import.
//   2. THE GAIN THE SYSTEM CHOOSES. VolumeFor(Event), asked of the live subsystem.
//   3. THE GAIN THE ENGINE RECEIVED. Each event is played for real and the returned
//      UAudioComponent's VolumeMultiplier is read back. Signals 2 and 3 disagreeing would mean the
//      knob is computed correctly and then lost somewhere in the play call, which is a failure no
//      amount of arithmetic on this side would ever notice.
//
// A red arm exists and it is one command: `Trace.Audio.FootstepVolume 1` puts the footsteps at the
// master volume and this harness MUST then fail, because at unity gain they really are louder than
// ButtonPress. A harness that cannot go red is not evidence.
//
// GetImportedSoundWaveData is WITH_EDITOR-only, which is fine and is stated in the output: every way
// this project runs the game is the editor binary with -game. In a cooked build signals 1 and 3 are
// unavailable and the command says so rather than inventing a number.
// ===================================================================================================

#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"           // FTSTicker — v31 §3's walk runs in REAL time, so a paused
                                         // world (an open pause menu) cannot strand it half finished
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"

#include "Audio/TraceAudio.h"
#include "Audio/TraceAudioWatch.h"       // v31 §3: CountFootstepsForSince — "this pawn's steps"
#include "Audio/TraceSoundBank.h"
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "Trace.h"

#if !UE_BUILD_SHIPPING

// Named after the file. Two anonymous namespaces in one unity translation unit are one namespace —
// see Scripts/check-jumbo-build-collisions.py and the four Windows-only breaks it exists to stop.
namespace TraceAudioLoudnessFile
{
	/**
	 * How far under the quietest other sound a footstep has to sit before this command will call it
	 * "audibly quieter".
	 *
	 * 6 dB is HALF THE AMPLITUDE, and it is the smallest gap anybody would describe that way without
	 * hedging. (The rule of thumb for "half as LOUD" is about 10 dB; the shipped setting clears that
	 * too, but the gate is the defensible minimum rather than the number we happen to have hit.)
	 *
	 * =============================================================================================
	 * *** SPEC v31 §3 MOVED THIS FROM 6.0 TO 4.0, ONCE, DELIBERATELY, AND HERE IS THE ARGUMENT. ***
	 * =============================================================================================
	 *
	 * The owner: "The footsteps don't seem to be in the game - if they are raise the volume by 5db".
	 * They were in the game (see UTraceAudioSettings::FootstepVolumeBoostDb for the measurement) and
	 * they went up by exactly 5 dB. That spends 5 dB of the 9.33 dB margin v29 §1b had banked, and
	 * 4.32 dB is what is left.
	 *
	 * A THRESHOLD THAT REFUSES THE THING THE OWNER ASKED FOR IS NOT EVIDENCE, IT IS AN OBSTACLE — but
	 * the honest fix is to move it exactly as far as the request costs and to say so, not to delete
	 * it. 4.0 dB is chosen, not fitted: it is what is left of "half the amplitude" after the owner's
	 * 5 dB, it is still a real gap (about a third of the amplitude down from ButtonPress), and it is
	 * still ~24 dB below a pistol shot, which is the comparison a player actually makes. Any further
	 * boost request must move this number again, on purpose, with the same paragraph.
	 *
	 * *** THE HARNESS CAN STILL GO RED, WHICH IS WHAT STOPS THIS FROM BEING A WEAKENING. *** The red
	 * arm is Trace.Audio.FootstepVolume 1: at unity gain the footsteps are 7.15 dB LOUDER than
	 * ButtonPress, i.e. the margin is NEGATIVE, and no positive threshold can hide a negative margin.
	 * Both 6.0 and 4.0 fail it identically. What changed is only the size of the band between "as
	 * shipped" and "broken", and that band is now 4.32 dB wide instead of 9.33.
	 */
	static constexpr double RequiredMarginDb = 4.0;

	/** Full-scale decibels for a 0..1 amplitude, with a floor so silence prints instead of -inf. */
	static double Dbfs(double Linear)
	{
		return (Linear > 0.0) ? 20.0 * FMath::LogX(10.0, Linear) : -120.0;
	}

	/** The game world a console command should act on. */
	static UWorld* PlayableWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld()
				&& (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** What one event measured. */
	struct FMeasured
	{
		FName Event;
		ETraceSoundFamily Family = ETraceSoundFamily::Default;
		bool  bHaveSamples = false;
		double ClipPeak = 0.0;      // 0..1, from the asset's own PCM
		double ClipRms = 0.0;       // 0..1
		double Gain = 0.0;          // what VolumeFor answers
		double EngineGain = -1.0;   // what the played component reports; <0 = not read
		double EffectivePeakDb = -120.0;
		double EffectiveRmsDb = -120.0;
	};

	/**
	 * Peak and RMS of @p Sound's imported PCM, as 0..1 amplitudes. False when the samples are not
	 * reachable (a cooked build, or a USoundCue rather than a wave).
	 */
	static bool MeasureClip(USoundBase* Sound, double& OutPeak, double& OutRms)
	{
		OutPeak = 0.0;
		OutRms = 0.0;

#if WITH_EDITOR
		USoundWave* Wave = Cast<USoundWave>(Sound);
		if (Wave == nullptr)
		{
			return false;
		}

		TArray<uint8> Pcm;
		uint32 SampleRate = 0;
		uint16 Channels = 0;
		if (!Wave->GetImportedSoundWaveData(Pcm, SampleRate, Channels) || Pcm.Num() < 2)
		{
			return false;
		}

		// The importer's PCM is signed 16-bit interleaved, which is what every WAV in Art/Sounds is.
		const int32 Count = Pcm.Num() / 2;
		const int16* Samples = reinterpret_cast<const int16*>(Pcm.GetData());

		double Peak = 0.0;
		double SumSquares = 0.0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Value = static_cast<double>(Samples[Index]) / 32768.0;
			Peak = FMath::Max(Peak, FMath::Abs(Value));
			SumSquares += Value * Value;
		}

		OutPeak = Peak;
		OutRms = FMath::Sqrt(SumSquares / static_cast<double>(Count));
		return true;
#else
		(void)Sound;
		return false;
#endif
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Audio.Loudness
	// ---------------------------------------------------------------------------------------------

	static void Loudness()
	{
		UWorld* World = PlayableWorld();
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		if (World == nullptr || Audio == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Loudness] no game world / no audio subsystem."));
			return;
		}

		const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();
		FAudioDevice* Device = World->GetAudioDeviceRaw();

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Audio.Loudness (spec v29 s1b) ================"));
		// SPEC v31 §3: the boost is printed as its own term rather than folded silently into the
		// gain, because "x0.150 -> gain 0.2667" with no third column is the kind of line that reads
		// as an arithmetic bug and gets 'fixed' by somebody six months from now.
		UE_LOG(LogTraceGame, Display,
			TEXT("[Loudness] master %.3f   footstep x%.3f   v31 s3 boost %+.2f dB (x%.4f)   -> footstep "
			     "gain %.4f (%.2f dB under the master).  device=%s"),
			Settings.MasterVolume, Settings.FootstepVolumeScale,
			Settings.FootstepVolumeBoostDb, Settings.GetFootstepBoostLinear(),
			Settings.GetFootstepVolume(),
			Dbfs(FMath::Max(KINDA_SMALL_NUMBER, Settings.GetFootstepVolume())),
			Device != nullptr ? TEXT("present") : TEXT("NONE (-nosound; signal 3 unavailable)"));

#if !WITH_EDITOR
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Loudness] this is a cooked build: the imported PCM is not reachable, so the clip "
			     "peaks below are all 'n/a' and no verdict can be reached. Run the editor binary with "
			     "-game, which is how every script in Scripts/ runs this project."));
#endif

		TArray<FMeasured> Rows;
		Rows.Reserve(TraceSoundEvents::All().Num());

		for (const FTraceSoundEvent& Row : TraceSoundEvents::All())
		{
			FMeasured Entry;
			Entry.Event = Row.Name;
			Entry.Family = Row.Family;

			USoundBase* Sound = Audio->ResolveSound(Row.Name);
			Entry.bHaveSamples = MeasureClip(Sound, Entry.ClipPeak, Entry.ClipRms);

			// SIGNAL 2: the gain this system will choose, asked of the system.
			Entry.Gain = static_cast<double>(Audio->VolumeFor(Row.Name));

			// SIGNAL 3: the gain the ENGINE was handed. Played for real; the component the engine
			// gives back reports the multiplier it received. Side-agnostic on purpose — this is a
			// measurement of GAIN, not of routing, and Trace.Audio.Report is what checks routing.
			if (Device != nullptr && Sound != nullptr)
			{
				UAudioComponent* Component = (Row.Side == ETraceSoundSide::World)
					? Audio->PlayWorldNow(Row.Name, FVector::ZeroVector)
					: Audio->PlayLocalNow(Row.Name);
				if (Component != nullptr)
				{
					Entry.EngineGain = static_cast<double>(Component->VolumeMultiplier);
					// Stop it again immediately. Twenty-eight overlapping one-shots is a mess and,
					// worse, would leave the mixer busy for the next harness in the batch.
					Component->Stop();
				}
			}

			Entry.EffectivePeakDb = Dbfs(Entry.ClipPeak * Entry.Gain);
			Entry.EffectiveRmsDb = Dbfs(Entry.ClipRms * Entry.Gain);
			Rows.Add(Entry);
		}

		UE_LOG(LogTraceGame, Display, TEXT(""));
		UE_LOG(LogTraceGame, Display, TEXT("[Loudness] %-14s %-9s %9s %9s %8s %8s %11s %10s"),
			TEXT("EVENT"), TEXT("FAMILY"), TEXT("CLIP PK"), TEXT("CLIP RMS"), TEXT("GAIN"),
			TEXT("ENGINE"), TEXT("EFF PEAK"), TEXT("EFF RMS"));

		double LoudestStepPeak = -1000.0;
		FName LoudestStep;
		double QuietestOtherPeak = 1000.0;
		FName QuietestOther;
		int32 Unmeasured = 0;
		int32 GainMismatches = 0;

		for (const FMeasured& Entry : Rows)
		{
			if (!Entry.bHaveSamples)
			{
				++Unmeasured;
			}

			// Signals 2 and 3 must agree. A disagreement means the knob is right and the play call
			// lost it, which is invisible to any amount of arithmetic on this side.
			const bool bGainRead = Entry.EngineGain >= 0.0;
			const bool bGainAgrees = !bGainRead
				|| FMath::IsNearlyEqual(Entry.EngineGain, Entry.Gain, 1.0e-4);
			if (!bGainAgrees)
			{
				++GainMismatches;
			}

			UE_LOG(LogTraceGame, Display, TEXT("[Loudness] %-14s %-9s %9s %9s %8.4f %8s %11s %10s%s"),
				*Entry.Event.ToString(), TraceSoundEvents::FamilyName(Entry.Family),
				Entry.bHaveSamples ? *FString::Printf(TEXT("%.2f"), Dbfs(Entry.ClipPeak)) : TEXT("n/a"),
				Entry.bHaveSamples ? *FString::Printf(TEXT("%.2f"), Dbfs(Entry.ClipRms)) : TEXT("n/a"),
				Entry.Gain,
				bGainRead ? *FString::Printf(TEXT("%.4f"), Entry.EngineGain) : TEXT("-"),
				Entry.bHaveSamples ? *FString::Printf(TEXT("%.2f"), Entry.EffectivePeakDb) : TEXT("n/a"),
				Entry.bHaveSamples ? *FString::Printf(TEXT("%.2f"), Entry.EffectiveRmsDb) : TEXT("n/a"),
				bGainAgrees ? TEXT("") : TEXT("   *** ENGINE GAIN DISAGREES ***"));

			if (!Entry.bHaveSamples)
			{
				continue;
			}

			if (Entry.Family == ETraceSoundFamily::Footstep)
			{
				if (Entry.EffectivePeakDb > LoudestStepPeak)
				{
					LoudestStepPeak = Entry.EffectivePeakDb;
					LoudestStep = Entry.Event;
				}
			}
			else if (Entry.EffectivePeakDb < QuietestOtherPeak)
			{
				QuietestOtherPeak = Entry.EffectivePeakDb;
				QuietestOther = Entry.Event;
			}
		}

		UE_LOG(LogTraceGame, Display, TEXT(""));

		if (LoudestStep.IsNone() || QuietestOther.IsNone())
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[Loudness] VERDICT: NOT MEASURED - %d event(s) had no reachable samples. Nothing "
				     "is claimed about s1b."), Unmeasured);
			return;
		}

		// THE COMPARISON IS LOUDEST-FOOTSTEP AGAINST QUIETEST-OTHER, not average against average.
		// "Quieter than every other sound" is a statement about the worst pair, and an average would
		// let one very loud footstep hide behind ten quiet ones.
		const double Margin = QuietestOtherPeak - LoudestStepPeak;
		const bool bPass = (Margin >= RequiredMarginDb) && (GainMismatches == 0);

		UE_LOG(LogTraceGame, Display,
			TEXT("[Loudness] loudest footstep  %-12s %8.2f dBFS effective peak"),
			*LoudestStep.ToString(), LoudestStepPeak);
		UE_LOG(LogTraceGame, Display,
			TEXT("[Loudness] quietest other    %-12s %8.2f dBFS effective peak"),
			*QuietestOther.ToString(), QuietestOtherPeak);
		// THE "AT UNITY" TERM READS THE DERIVED GAIN, NOT THE RAW KNOB. It used to read
		// FootstepVolumeScale, which was the same number only for as long as the scale WAS the whole
		// gain. Spec v31 §3 added the +5 dB boost, so the two parted company: the counterfactual
		// being described is "what if the gain were 1.0", and the gain is GetFootstepVolume().
		UE_LOG(LogTraceGame, Display,
			TEXT("[Loudness] margin %.2f dB (need %.2f; v29 s1b required 6.00 and v31 s3's +%.2f dB "
			     "spent the difference - see RequiredMarginDb). At UNITY footstep gain the margin "
			     "would be %.2f dB, i.e. the footsteps would be the LOUDER of the two - which is why "
			     "this is measured and not assumed."),
			Margin, RequiredMarginDb, Settings.FootstepVolumeBoostDb,
			Margin + Dbfs(FMath::Max(KINDA_SMALL_NUMBER, Settings.GetFootstepVolume())));

		// TWO CALLS AND NOT A TERNARY VERBOSITY: UE_LOG pastes its second argument onto
		// `ELogVerbosity::`, so an expression there does not compile.
#define TRACE_LOUDNESS_VERDICT_TEXT \
	TEXT("TRACE LOUDNESS VERDICT: %s - the loudest footstep is %.2f dB %s the quietest other sound " \
	     "(need %.2f dB quieter). %d gain mismatch(es), %d event(s) unmeasured.")
#define TRACE_LOUDNESS_VERDICT_ARGS \
	(bPass ? TEXT("PASS") : TEXT("FAIL")), FMath::Abs(Margin), \
	(Margin >= 0.0 ? TEXT("QUIETER THAN") : TEXT("LOUDER THAN")), RequiredMarginDb, \
	GainMismatches, Unmeasured

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TRACE_LOUDNESS_VERDICT_TEXT, TRACE_LOUDNESS_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_LOUDNESS_VERDICT_TEXT, TRACE_LOUDNESS_VERDICT_ARGS);
		}

#undef TRACE_LOUDNESS_VERDICT_ARGS
#undef TRACE_LOUDNESS_VERDICT_TEXT
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Audio.FootstepVolume — the knob, and Loudness's RED ARM
	// ---------------------------------------------------------------------------------------------

	static void SetFootstepVolume(const TArray<FString>& Args)
	{
		UTraceAudioSettings* Mutable = GetMutableDefault<UTraceAudioSettings>();
		if (Mutable == nullptr)
		{
			return;
		}

		if (Args.Num() == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[Loudness] footstep volume is x%.4f of the master (= gain %.4f). Pass a value to "
				     "change it; 1 is Trace.Audio.Loudness's RED ARM and must make it FAIL."),
				Mutable->FootstepVolumeScale, Mutable->GetFootstepVolume());
			return;
		}

		const float Wanted = FMath::Clamp(FCString::Atof(*Args[0]), 0.f, 1.f);
		const float Before = Mutable->FootstepVolumeScale;
		Mutable->FootstepVolumeScale = Wanted;

		UE_LOG(LogTraceGame, Display,
			TEXT("[Loudness] footstep volume x%.4f -> x%.4f (gain %.4f, which INCLUDES v31 s3's "
			     "%+.2f dB boost). This is a live default and is NOT written to any ini."),
			Before, Mutable->FootstepVolumeScale, Mutable->GetFootstepVolume(),
			Mutable->FootstepVolumeBoostDb);
	}

	// ---------------------------------------------------------------------------------------------
	// *** Trace.Audio.FootstepWalk — SPEC v31 §3. "DO THEY PLAY AT ALL, FROM MY OWN MACHINE?" ***
	// ---------------------------------------------------------------------------------------------
	//
	// §3 is explicit that this question has to be answered BEFORE the +5 dB, and that "measured
	// quieter" is not "audible". Nothing that existed could answer it end to end:
	//
	//   Trace.Audio.Loudness    proves the LEVEL is right. It never walks a pawn, so a footstep
	//                           system that was completely unwired would still pass it.
	//   Trace.Audio.Footsteps   proves the STRIDE and the RANDOMISER. Its own comment says the walk
	//                           is not what is under test, and it counts entries in the watcher's
	//                           log — which is what the watcher DECIDED, not what the engine PLAYED.
	//   Trace.Audio.Heard       proves what this machine played, but only for whatever happened to
	//                           have been played before you typed it.
	//
	// The gap between the second and the third is exactly where "the sound is dead" hides: the
	// watcher can log eleven steps and hand them to a multicast that plays nothing, and every
	// existing command still says PASS. So this one walks the LOCAL PLAYER'S OWN PAWN through the
	// shipping movement path and then reads UTraceAudioSubsystem::PlaysByEvent — which is bumped
	// inside PlayWorldNow, i.e. at the instant a sound is handed to THIS machine's audio engine,
	// after the side gate, the settings gate, the device test and the resolve.
	//
	// THE LOCAL PLAYER'S PAWN AND NOT "A DRIVABLE PAWN", which is the difference between this and
	// Trace.Audio.Footsteps' FindSubject. The owner's complaint is "I cannot hear them", and a bot
	// forty metres away being audible is not an answer to it.
	//
	// FOUR NUMBERS COME OUT AND THEY ARE INDEPENDENT, so a failure says WHERE it failed:
	//   announced   the watcher decided this pawn took a step        (stride/trigger alive)
	//   sent        multicasts this machine put on the wire          (authority/relay alive)
	//   played      Step* plays that reached this machine's engine    (the ear, and the answer)
	//   audible     one step replayed at the listener, component read back: still playing? at what
	//               gain? what effective dBFS?                        (the LEVEL, at the ear)

	/** State for the walk, which spans frames. One at a time; a second call replaces the first. */
	struct FFootstepWalkRun
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ATraceCharacter> Pawn;
		double StartedReal = 0.0;
		double Seconds = 3.0;
		double DistanceUU = 0.0;
		int32 AnnouncedAtStart = 0;
		int32 MulticastsAtStart = 0;
		int32 WorldPlaysAtStart = 0;
		int32 StepPlaysAtStart = 0;
	};

	static TSharedPtr<FFootstepWalkRun> GFootstepWalk;

	/** Every Step* play this machine's engine has accepted, summed out of PlaysByEvent. */
	static int32 CountStepPlays(const UTraceAudioSubsystem& Audio)
	{
		int32 Total = 0;
		for (const TPair<FName, int32>& Pair : Audio.GetPlaysByEvent())
		{
			if (TraceSoundEvents::FamilyOf(Pair.Key) == ETraceSoundFamily::Footstep)
			{
				Total += Pair.Value;
			}
		}
		return Total;
	}

	/** The pawn the person at the keyboard is driving, or null with a reason. */
	static ATraceCharacter* LocalPlayerPawn(UWorld* World, FString& OutWhyNot)
	{
		if (World == nullptr)
		{
			OutWhyNot = TEXT("no game world");
			return nullptr;
		}

		// GetFirstLocalPlayerFromController rather than iterating every controller: "the player's own
		// machine" means the viewport this process is rendering, and on a listen server the bots'
		// AIControllers are local too — which is precisely the confusion this command exists to avoid.
		const APlayerController* PC = World->GetFirstPlayerController();
		if (PC == nullptr || !PC->IsLocalController())
		{
			OutWhyNot = TEXT("no LOCAL player controller in this world (a dedicated server has none)");
			return nullptr;
		}

		ATraceCharacter* Pawn = Cast<ATraceCharacter>(PC->GetPawn());
		if (Pawn == nullptr)
		{
			OutWhyNot = TEXT("the local controller has no ATraceCharacter possessed yet");
			return nullptr;
		}
		return Pawn;
	}

	/** Plays one footstep at the listener and reports what the engine did with it. */
	static void ProbeOneStepAtTheEar(UWorld* World, UTraceAudioSubsystem& Audio, ATraceCharacter* Pawn)
	{
		const FName Clip = TraceSoundEvents::FootstepAt(0);
		if (Clip.IsNone() || Pawn == nullptr)
		{
			return;
		}

		// AT THE PAWN, so the attenuation shape sees the same ~0 uu it sees for a real step of your
		// own. Deliberately NOT at the feet: this probe is about GAIN, and putting it a metre away
		// would fold a (tiny, but non-zero) distance term into the number being reported.
		UAudioComponent* Component = Audio.PlayWorldNow(Clip, Pawn->GetActorLocation());
		if (Component == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[FootstepWalk] audible: the engine REFUSED a direct play of '%s'. That is the "
				     "'no sound at all' failure - check device, bank and bSoundEffectsEnabled above."),
				*Clip.ToString());
			return;
		}

		const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();
		const double Gain = static_cast<double>(Component->VolumeMultiplier);

		UE_LOG(LogTraceGame, Display,
			TEXT("[FootstepWalk] audible: '%s' handed to the engine, playing=%d, engine gain %.4f "
			     "(the system chose %.4f; they must match). Master %.3f x scale %.3f x %+.2f dB."),
			*Clip.ToString(), Component->IsPlaying() ? 1 : 0, Gain, Settings.GetFootstepVolume(),
			Settings.MasterVolume, Settings.FootstepVolumeScale, Settings.FootstepVolumeBoostDb);

		if (!FMath::IsNearlyEqual(Gain, static_cast<double>(Settings.GetFootstepVolume()), 1.0e-3))
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[FootstepWalk] audible: GAIN MISMATCH. The knob is computed correctly and then "
				     "lost between VolumeFor and the play call."));
		}
	}

	static bool FootstepWalkTick(float Delta)
	{
		TSharedPtr<FFootstepWalkRun> Run = GFootstepWalk;
		if (!Run.IsValid())
		{
			return false;
		}

		UWorld* World = Run->World.Get();
		ATraceCharacter* Pawn = Run->Pawn.Get();
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World);
		if (World == nullptr || Pawn == nullptr || Audio == nullptr || Watch == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[FootstepWalk] the world or the pawn went away mid-walk; nothing is claimed."));
			GFootstepWalk.Reset();
			return false;
		}

		const double Elapsed = FPlatformTime::Seconds() - Run->StartedReal;
		if (Elapsed < Run->Seconds)
		{
			// THE SHIPPING MOVEMENT PATH. AddMovementInput is what the player's own W key reaches, and
			// the stride accumulator reads Velocity and IsMovingOnGround() off the component this
			// drives — so a step counted here is a step the game would have played for a human.
			Pawn->AddMovementInput(Pawn->GetActorForwardVector().GetSafeNormal2D(), 1.f);
			if (const UCharacterMovementComponent* Move = Pawn->GetCharacterMovement())
			{
				// SPEED x TIME, never the change in position: a teleport (a respawn, a range reset)
				// would otherwise be counted as walking. Same rule the accumulator itself uses.
				Run->DistanceUU += Move->Velocity.Size2D() * static_cast<double>(FMath::Max(0.f, Delta));
			}
			return true;
		}

		const int32 Announced = Watch->CountFootstepsForSince(Pawn, Run->AnnouncedAtStart);
		const int32 Sent = Audio->GetCounters().MulticastsSent - Run->MulticastsAtStart;
		const int32 WorldPlays = Audio->GetCounters().WorldPlays - Run->WorldPlaysAtStart;
		const int32 StepPlays = CountStepPlays(*Audio) - Run->StepPlaysAtStart;

		const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();
		const double Stride = static_cast<double>(FMath::Max(1.f, Settings.FootstepStrideUU));
		const double Due = Run->DistanceUU / Stride;

		UE_LOG(LogTraceGame, Display,
			TEXT("[FootstepWalk] %s walked %.0f uu in %.1fs. Stride %.0f uu, so %.1f step(s) were due."),
			*GetNameSafe(Pawn), Run->DistanceUU, Run->Seconds, Stride, Due);
		UE_LOG(LogTraceGame, Display,
			TEXT("[FootstepWalk]   announced %d   multicasts sent %d   world plays here %d   "
			     "Step* plays that REACHED THIS MACHINE'S ENGINE %d"),
			Announced, Sent, WorldPlays, StepPlays);

		ProbeOneStepAtTheEar(World, *Audio, Pawn);

		// THREE SEPARATE FAILURES, NAMED SEPARATELY, because "footsteps are broken" is not actionable
		// and each of these has a different owner.
		const bool bTriggerOk = (Announced > 0);
		const bool bEarOk = (StepPlays > 0);
		const bool bRateOk = (Due <= 0.5) || (Announced >= Due * 0.5 && Announced <= Due * 1.5 + 1.0);

		if (!bTriggerOk)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[FootstepWalk] THE TRIGGER IS DEAD: the pawn walked and the watcher announced "
				     "nothing. Look at UTraceAudioWatchSubsystem::TickFootsteps - authority, "
				     "IsMovingOnGround, FootstepMinSpeedUU, Trace.Audio.FootstepWatch."));
		}
		if (bTriggerOk && !bEarOk)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[FootstepWalk] ANNOUNCED BUT NEVER HEARD: %d step(s) were decided and none "
				     "reached this machine's audio engine. That is the multicast/relay/device half, "
				     "not the stride - run Trace.Audio.Report."), Announced);
		}
		if (bTriggerOk && !bRateOk)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[FootstepWalk] the CADENCE is off: %d step(s) for %.1f due. The trigger works "
				     "but it is not measuring distance the way FootstepStrideUU says."),
				Announced, Due);
		}

		const bool bPass = bTriggerOk && bEarOk && bRateOk;

		// TWO CALLS AND NOT A TERNARY VERBOSITY: UE_LOG pastes its second argument onto
		// `ELogVerbosity::`, so an expression there does not compile. Same reason as everywhere else
		// in this file.
#define TRACE_FOOTSTEPWALK_VERDICT_TEXT \
	TEXT("TRACE FOOTSTEPWALK VERDICT: %s - the local player walked %.0f uu, %d footstep(s) were " \
	     "announced for THEM and %d Step* play(s) reached THIS machine's engine at gain %.4f " \
	     "(%.2f dB under the master).")
#define TRACE_FOOTSTEPWALK_VERDICT_ARGS \
	(bPass ? TEXT("PASS") : TEXT("FAIL")), Run->DistanceUU, Announced, StepPlays, \
	Settings.GetFootstepVolume(), \
	Dbfs(FMath::Max(KINDA_SMALL_NUMBER, Settings.GetFootstepVolume()))

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TRACE_FOOTSTEPWALK_VERDICT_TEXT, TRACE_FOOTSTEPWALK_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_FOOTSTEPWALK_VERDICT_TEXT, TRACE_FOOTSTEPWALK_VERDICT_ARGS);
		}

#undef TRACE_FOOTSTEPWALK_VERDICT_ARGS
#undef TRACE_FOOTSTEPWALK_VERDICT_TEXT

		GFootstepWalk.Reset();
		return false;
	}

	static void FootstepWalk(const TArray<FString>& Args)
	{
		UWorld* World = PlayableWorld();
		UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World);

		if (World == nullptr || Audio == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[FootstepWalk] no game world / no audio subsystem."));
			return;
		}
		if (Watch == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[FootstepWalk] no audio WATCH subsystem. It is authority-only, so this command "
				     "has to run on the server (standalone or listen), not on a connected client."));
			return;
		}

		FString WhyNot;
		ATraceCharacter* Pawn = LocalPlayerPawn(World, WhyNot);
		if (Pawn == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[FootstepWalk] no local player pawn: %s."), *WhyNot);
			return;
		}

		TSharedPtr<FFootstepWalkRun> Run = MakeShared<FFootstepWalkRun>();
		Run->World = World;
		Run->Pawn = Pawn;
		Run->StartedReal = FPlatformTime::Seconds();
		Run->Seconds = (Args.Num() > 0) ? FMath::Clamp(FCString::Atod(*Args[0]), 0.5, 30.0) : 3.0;

		// BASELINES, NOT RESETS. Clearing PlaysByEvent would make this command destroy the evidence
		// Trace.Audio.Heard is there to read; taking a delta costs one int and leaves the session's
		// history intact.
		Run->AnnouncedAtStart = Watch->GetFootstepLog().Num();
		Run->MulticastsAtStart = Audio->GetCounters().MulticastsSent;
		Run->WorldPlaysAtStart = Audio->GetCounters().WorldPlays;
		Run->StepPlaysAtStart = CountStepPlays(*Audio);

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Audio.FootstepWalk (spec v31 s3) ================"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[FootstepWalk] walking %s (the LOCAL player's pawn) forward for %.1fs with sound on, "
			     "then reading what reached this machine's audio engine. device=%s"),
			*GetNameSafe(Pawn), Run->Seconds,
			World->GetAudioDeviceRaw() != nullptr ? TEXT("present") : TEXT("NONE (-nosound: this "
			     "command cannot answer the question - relaunch WITHOUT -nosound)"));

		GFootstepWalk = Run;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&FootstepWalkTick), 0.f);
	}

	// ---------------------------------------------------------------------------------------------
	// Trace.Audio.Sides — spec v29 §1a, checked rather than asserted
	// ---------------------------------------------------------------------------------------------

	/** What spec v26 §9 shipped. THE REFERENCE, typed out once, from the v26 spec text. */
	struct FSideExpectation
	{
		FName Event;
		ETraceSoundSide Side;
	};

	static void Sides()
	{
		// "Core Turnover, dash, and parry should be game-side. The rest should be client side."
		// Copied from the v26 quotation at the top of Audio/TraceSoundEvents.h, deliberately as a
		// SECOND copy: a check that reads the table it is checking cannot fail.
		const FSideExpectation Expected[] =
		{
			{ TraceSoundEvents::CoreTurnover, ETraceSoundSide::World },
			{ TraceSoundEvents::Dash,         ETraceSoundSide::World },
			{ TraceSoundEvents::Parry,        ETraceSoundSide::World },
			{ TraceSoundEvents::Bodyshot,     ETraceSoundSide::Client },
			{ TraceSoundEvents::Headshot,     ETraceSoundSide::Client },
			{ TraceSoundEvents::CorePickup,   ETraceSoundSide::Client },
			{ TraceSoundEvents::Jump,         ETraceSoundSide::Client },
			{ TraceSoundEvents::WallJump,     ETraceSoundSide::Client },
			{ TraceSoundEvents::ButtonPress,  ETraceSoundSide::Client },
		};

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Audio.Sides (spec v29 s1a) ================"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[Sides] \"replace the old sounds with the new versions ... keeping the same sounds "
			     "client side vs global\". SEVEN WAVs were replaced this patch; these nine rows are "
			     "what must NOT have moved."));

		int32 Moved = 0;
		for (const FSideExpectation& Row : Expected)
		{
			const ETraceSoundSide Actual = TraceSoundEvents::SideOf(Row.Event);
			const bool bSame = (Actual == Row.Side);
			Moved += bSame ? 0 : 1;

			if (bSame)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[Sides]   KEPT   %-14s %s"),
					*Row.Event.ToString(), TraceSoundEvents::SideName(Actual));
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[Sides]   MOVED  %-14s was %s, is now %s"),
					*Row.Event.ToString(), TraceSoundEvents::SideName(Row.Side),
					TraceSoundEvents::SideName(Actual));
			}
		}

		// The v29 additions, listed rather than asserted: they had no previous side to keep.
		UE_LOG(LogTraceGame, Display, TEXT("[Sides] --- new in v29, for the reader's eye ---"));
		for (const FTraceSoundEvent& Row : TraceSoundEvents::All())
		{
			bool bIsOld = false;
			for (const FSideExpectation& Old : Expected)
			{
				bIsOld = bIsOld || (Old.Event == Row.Name);
			}
			if (!bIsOld)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[Sides]   NEW    %-14s %-11s %-9s %s"),
					*Row.Name.ToString(), TraceSoundEvents::SideName(Row.Side),
					TraceSoundEvents::FamilyName(Row.Family), Row.Trigger);
			}
		}

#define TRACE_SIDES_VERDICT_TEXT \
	TEXT("TRACE SIDES VERDICT: %s - %d of %d spec v26 events still route exactly as they did.")
#define TRACE_SIDES_VERDICT_ARGS \
	(Moved == 0 ? TEXT("PASS") : TEXT("FAIL")), \
	static_cast<int32>(UE_ARRAY_COUNT(Expected)) - Moved, static_cast<int32>(UE_ARRAY_COUNT(Expected))

		if (Moved == 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_SIDES_VERDICT_TEXT, TRACE_SIDES_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_SIDES_VERDICT_TEXT, TRACE_SIDES_VERDICT_ARGS);
		}

#undef TRACE_SIDES_VERDICT_ARGS
#undef TRACE_SIDES_VERDICT_TEXT
	}

	FAutoConsoleCommand CmdLoudness(
		TEXT("Trace.Audio.Loudness"),
		TEXT("Spec v29 s1b. MEASURES every sound: the imported clip's own peak/RMS, the gain the audio ")
		TEXT("system chooses for it, and the gain the engine actually received - then proves the ")
		TEXT("footsteps are at least 6 dB under the quietest other sound. RED ARM: ")
		TEXT("Trace.Audio.FootstepVolume 1, which must make this FAIL."),
		FConsoleCommandDelegate::CreateStatic(&Loudness));

	FAutoConsoleCommand CmdFootstepVolume(
		TEXT("Trace.Audio.FootstepVolume"),
		TEXT("Spec v29 s1b. Reads or sets the footstep volume as a MULTIPLE OF THE MASTER. With no ")
		TEXT("argument it prints the current value. 1 is Trace.Audio.Loudness's red arm."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&SetFootstepVolume));

	FAutoConsoleCommand CmdFootstepWalk(
		TEXT("Trace.Audio.FootstepWalk"),
		TEXT("Spec v31 s3. Walks the LOCAL PLAYER's own pawn for N seconds (default 3) and reports, ")
		TEXT("separately: how many footsteps the watcher announced for them, how many multicasts went ")
		TEXT("out, and how many Step* plays actually REACHED THIS MACHINE'S audio engine - then plays ")
		TEXT("one at the ear and reads the component's gain back. This is the command that answers ")
		TEXT("\"are the footsteps in the game at all\"; Trace.Audio.Loudness only answers \"at what ")
		TEXT("level\". Must be run WITHOUT -nosound."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FootstepWalk));

	/**
	 * SPEC v29 §1c's RED ARM. Set the floor to 0 and the reset window falls back to the owner's
	 * literal 0.3 s — which the 190 RPM pistol can never fire inside, so every shot becomes
	 * PistolShoot1 and Trace.Audio.GunLadder FAILS. That failure is the evidence the collision
	 * documented on UTraceAudioSettings::PistolLadderResetIntervalFloor is real.
	 */
	static void SetPistolResetFloor(const TArray<FString>& Args)
	{
		UTraceAudioSettings* Mutable = GetMutableDefault<UTraceAudioSettings>();
		if (Mutable == nullptr)
		{
			return;
		}

		if (Args.Num() > 0)
		{
			Mutable->PistolLadderResetIntervalFloor = FMath::Clamp(FCString::Atof(*Args[0]), 0.f, 10.f);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[GunLadder] pistol ladder reset: literal %.3fs, floor %.3fx the gun's own interval, "
			     "EFFECTIVE %.4fs. %s"),
			Mutable->PistolLadderResetSeconds, Mutable->PistolLadderResetIntervalFloor,
			Mutable->GetPistolLadderResetSeconds(),
			(Mutable->GetPistolLadderResetSeconds() <= Mutable->PistolLadderResetSeconds + KINDA_SMALL_NUMBER)
				? TEXT("*** THIS IS THE RED ARM: the window is now the literal 0.3s, which is SHORTER "
				       "than the 0.3158s the pistol fires at, so the ladder can never climb. ***")
				: TEXT(""));
	}

	FAutoConsoleCommand CmdPistolResetFloor(
		TEXT("Trace.Audio.PistolResetFloor"),
		TEXT("Spec v29 s1c. Reads or sets the pistol ladder's reset FLOOR as a multiple of the gun's ")
		TEXT("own fire interval. 0 restores the literal 0.3s from the spec, which the 190 RPM pistol ")
		TEXT("cannot fire inside - that is Trace.Audio.GunLadder's red arm."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&SetPistolResetFloor));

	// =============================================================================================
	// *** Trace.Audio.Heard — WHAT THIS MACHINE ACTUALLY PLAYED. THE SECOND-VIEWPOINT INSTRUMENT. ***
	// =============================================================================================
	//
	// Spec v29 §1e says gunshots are GLOBAL: "world sound at the muzzle, so other players hear where a
	// shot came from". Every check written for it so far runs on ONE machine and proves the SENDING
	// half — the events are declared world-side, and the authority reports N multicasts sent. That is
	// necessary and it is not sufficient: a multicast that no client plays satisfies all of it.
	//
	// This prints the RECEIVING half. UTraceAudioSubsystem::PlaysByEvent is bumped inside PlayLocalNow
	// and PlayWorldNow — i.e. at the moment a sound is handed to the engine ON THIS MACHINE — so
	// running this on a CLIENT and seeing PistolShoot1 with a non-zero count is a second viewpoint
	// hearing a shot that a different machine fired. The counters beside it are what tell the two
	// roles apart without having to be told which process you are looking at: an authority reports
	// multicastsSent > 0, a client reports multicastsSent == 0 and worldPlays > 0.
	static void Heard()
	{
		UWorld* World = PlayableWorld();
		UTraceAudioSubsystem* Audio = (World != nullptr) ? World->GetSubsystem<UTraceAudioSubsystem>() : nullptr;
		if (Audio == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Heard] no audio subsystem on this machine."));
			return;
		}

		const FTraceAudioCounters& Counters = Audio->GetCounters();
		const ENetMode Mode = (World != nullptr) ? World->GetNetMode() : NM_Standalone;
		const TCHAR* Role =
			(Mode == NM_Client)       ? TEXT("CLIENT (a second viewpoint)") :
			(Mode == NM_ListenServer) ? TEXT("LISTEN SERVER (the authority, and also a viewpoint)") :
			(Mode == NM_DedicatedServer) ? TEXT("DEDICATED SERVER (no listener)") : TEXT("STANDALONE");

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Audio.Heard — this machine is the %s ================"), Role);
		UE_LOG(LogTraceGame, Display,
			TEXT("[Heard] localPlays=%d worldPlays=%d multicastsSent=%d refusedNotAuthority=%d "
			     "refusedNotLocalPlayer=%d missingSound=%d noAudioDevice=%d"),
			Counters.LocalPlays, Counters.WorldPlays, Counters.MulticastsSent,
			Counters.RefusedNotAuthority, Counters.RefusedNotLocalPlayer,
			Counters.MissingSound, Counters.NoAudioDevice);

		// Sorted by name so two processes' output can be diffed line for line.
		TArray<FName> Events;
		Audio->GetPlaysByEvent().GetKeys(Events);
		Events.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

		if (Events.Num() == 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Heard] NOTHING has been played on this machine yet. On a client that means no "
				     "multicast has arrived AND been played — which is the failure §1e would have."));
			return;
		}

		int32 GunshotPlays = 0;
		int32 FootstepPlays = 0;
		for (const FName& Event : Events)
		{
			const int32 Count = Audio->GetPlaysByEvent().FindRef(Event);
			const ETraceSoundFamily Family = TraceSoundEvents::FamilyOf(Event);
			if (Family == ETraceSoundFamily::Gunshot)
			{
				GunshotPlays += Count;
			}
			else if (Family == ETraceSoundFamily::Footstep)
			{
				FootstepPlays += Count;
			}
			UE_LOG(LogTraceGame, Display, TEXT("[Heard]   %-16s %5d   (%s, %s)"),
				*Event.ToString(), Count,
				TraceSoundEvents::FamilyName(Family),
				(TraceSoundEvents::SideOf(Event) == ETraceSoundSide::World) ? TEXT("world") : TEXT("client"));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Heard] TOTALS: %d gunshot(s) and %d footstep(s) were played on this machine, "
			     "across %d distinct event(s)."),
			GunshotPlays, FootstepPlays, Events.Num());
	}

	FAutoConsoleCommand CmdHeard(
		TEXT("Trace.Audio.Heard"),
		TEXT("Spec v29 s1e. Prints what THIS machine has actually played, per event, plus its audio ")
		TEXT("counters and its net role. Run it on a CLIENT to prove a second viewpoint hears the ")
		TEXT("gunshots and footsteps another machine fired - the sending half is all any single-process ")
		TEXT("check can show."),
		FConsoleCommandDelegate::CreateStatic(&Heard));

	FAutoConsoleCommand CmdSides(
		TEXT("Trace.Audio.Sides"),
		TEXT("Spec v29 s1a. Checks the nine spec v26 events against the sides v26 shipped, so ")
		TEXT("\"we replaced the WAVs and kept the client/global split\" is verified rather than ")
		TEXT("asserted, and lists the v29 additions with their sides and families."),
		FConsoleCommandDelegate::CreateStatic(&Sides));
}

#endif // !UE_BUILD_SHIPPING
