// Copyright Trace. All Rights Reserved.
//
// ===================================================================================================
// Trace — Trace.Audio.GunLadder and Trace.Audio.Footsteps   (spec v29 §1b, §1c, §1d, §1e)
// ===================================================================================================
//
//   Trace.Audio.GunLadder   §1c/§1d/§1e. Fires the REAL gun and prints the CLIP NAME PER SHOT across a
//                           fast burst, then across a burst with a gap in it, then across an SMG
//                           burst — and asserts all three sequences.
//   Trace.Audio.Footsteps   §1b. Draws hundreds of footsteps and checks the randomisation: no clip
//                           twice running, every clip used, and no clip hogging the distribution.
//
// ---------------------------------------------------------------------------------------------------
// THE LADDER IS CHECKED TWICE, AND THE TWO CHECKS ANSWER DIFFERENT QUESTIONS
// ---------------------------------------------------------------------------------------------------
//   PHASE 0  THE RULE, with no world at all: FTracePistolLadder driven on synthetic timestamps. This
//            answers "is the rule written correctly", it runs in microseconds, and it can prove the
//            fortieth consecutive shot is still PistolShoot4 — which no real burst has the clip for.
//   PHASE 1+ THE GUN. The trigger is pressed through UTraceWeaponComponent::StartFire, the gun's own
//            rate gate decides when a round leaves, and the sequence is read back from the shot log
//            the audio system wrote while it was happening. This answers the different and more
//            important question: "does the shipped path actually produce that sequence".
//
// A harness that only did phase 0 would pass on a build where nothing was wired to the gun at all.
// One that only did phase 1 could not show what happens on shot 40. Both, or neither is worth much.
//
// ---------------------------------------------------------------------------------------------------
// *** THE THING THIS HARNESS FOUND, AND IT IS THE REASON PHASE 2 EXISTS ***
// ---------------------------------------------------------------------------------------------------
// The pistol's fire interval is 0.315789 s (190 RPM, spec v24 §4). The spec's reset window is 0.3 s.
// 0.3 < 0.3158, so a LITERAL reading resets the ladder on every shot and PistolShoot2/3/4 are never
// heard at any rate the gun can achieve. See UTraceAudioSettings::PistolLadderResetIntervalFloor for
// the fix and the reasoning; the short version is that the window now has a floor expressed as a
// MULTIPLE OF THE GUN'S OWN INTERVAL, so it tracks the fire rate instead of breaking again.
//
// THE RED ARM IS THAT LITERAL READING: `Trace.Audio.PistolResetFloor 0` puts the window back to a
// flat 0.3 s, and this harness must then report every shot as PistolShoot1 and FAIL. A second red arm
// switches the announcer off entirely (`Trace.Audio.ShotWatch 0` -> zero shots logged). Two arms,
// both of which break a different half of the claim.
//
// ---------------------------------------------------------------------------------------------------
// RUNNING IT — WITHOUT -nosound, AND ONE HARNESS PER BATCH
// ---------------------------------------------------------------------------------------------------
//     UE="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor"
//     "$UE" "$PWD/Trace.uproject" "/Game/Maps/Arena_Baked?game=/Script/Trace.TracePracticeGameMode" \
//         -game -windowed -ResX=1280 -ResY=720 -LogCmds="LogTraceGame Verbose" \
//         -TraceExec="Trace.Audio.GunLadder" -TraceExecAt=8 -TraceExecOn=Match
//
// SERVER ONLY. The clip is server state and the gunshot is a game-side sound the authority
// multicasts, so a client run would watch a replicated clip and announce nothing.
// ===================================================================================================

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"

#include "Audio/TraceAudio.h"
#include "Audio/TraceAudioWatch.h"
#include "Audio/TraceSoundBank.h"
#include "Audio/TraceSoundEvents.h"
#include "Core/TraceCharacter.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Trace.h"

#if !UE_BUILD_SHIPPING

// Named after the file. See Scripts/check-jumbo-build-collisions.py.
namespace TraceGunLadderVerifyFile
{
	/** How many rounds the fast burst fires. Six, because the interesting part starts at four. */
	static constexpr int32 FastBurstShots = 6;

	/** How many the burst after the gap fires. Three is enough to show 1 -> 2 -> 3 climbing again. */
	static constexpr int32 AfterGapShots = 3;

	/** How many SMG rounds §1d is checked over. */
	static constexpr int32 SmgBurstShots = 5;

	/** The gap, as a MULTIPLE of the effective reset window — relative, so it moves with the knob. */
	static constexpr double GapScale = 1.8;

	/** Give up rather than hang if the match never becomes drivable. */
	static constexpr double StageTimeoutSeconds = 25.0;

	static UWorld* AuthoritativeWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate != nullptr && Candidate->IsGameWorld() && Candidate->GetAuthGameMode() != nullptr)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/**
	 * The first HUMAN player's pawn, alive, locally controlled and not carrying the Core.
	 *
	 * A HUMAN and LOCALLY CONTROLLED both matter: StartFire() refuses a pawn this machine does not
	 * control (input is local), and the game mode re-fills BOTS' state several times a second, so a
	 * bot subject would be fighting the fill for the weapon selector this harness is setting.
	 */
	static ATraceCharacter* FindSubject(UWorld* World, FString& OutWhyNot)
	{
		OutWhyNot = TEXT("no human player controller with a pawn");
		if (World == nullptr)
		{
			OutWhyNot = TEXT("no world");
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = Cast<APlayerController>(It->Get());
			ATraceCharacter* Pawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;
			if (Pawn == nullptr || Pawn->Weapon == nullptr)
			{
				continue;
			}
			if (!Pawn->IsAlive() || Pawn->IsCarrier() || !Pawn->IsLocallyControlled())
			{
				OutWhyNot = FString::Printf(
					TEXT("%s is present but alive=%d carrying=%d locallyControlled=%d"),
					*GetNameSafe(Pawn), Pawn->IsAlive() ? 1 : 0, Pawn->IsCarrier() ? 1 : 0,
					Pawn->IsLocallyControlled() ? 1 : 0);
				continue;
			}
			return Pawn;
		}
		return nullptr;
	}

	// =============================================================================================
	// PHASE 0 — the rule, on synthetic time
	// =============================================================================================

	/** One expectation in the pure-rule check. */
	struct FRuleStep
	{
		double Gap;             // seconds since the previous shot
		FName  Expected;
		const TCHAR* Why;
	};

	/**
	 * Drives FTracePistolLadder over a scripted timeline and reports each mismatch.
	 *
	 * @return the number of steps that came out wrong.
	 */
	static int32 CheckRule(float ResetSeconds)
	{
		// The gaps are written as FRACTIONS OF THE RESET WINDOW, never as absolute seconds, so this
		// check keeps meaning the same thing if the window ever moves. "Half a window" is fast firing
		// by definition; "one and a half windows" is a break by definition.
		const double R = static_cast<double>(FMath::Max(0.001f, ResetSeconds));
		const double Fast = R * 0.5;
		const double Break = R * 1.5;

		const FRuleStep Script[] =
		{
			{ 0.0,   TraceSoundEvents::PistolShoot1, TEXT("the first shot of a match") },
			{ Fast,  TraceSoundEvents::PistolShoot2, TEXT("fast: the ladder climbs") },
			{ Fast,  TraceSoundEvents::PistolShoot3, TEXT("fast: the ladder climbs") },
			{ Fast,  TraceSoundEvents::PistolShoot4, TEXT("fast: shot 4") },
			{ Fast,  TraceSoundEvents::PistolShoot4, TEXT("fast: 'and every shot after'") },
			{ Fast,  TraceSoundEvents::PistolShoot4, TEXT("fast: still 4") },
			{ Break, TraceSoundEvents::PistolShoot1, TEXT("A BREAK: back to 1") },
			{ Fast,  TraceSoundEvents::PistolShoot2, TEXT("fast again: climbing from 1") },
			{ Fast,  TraceSoundEvents::PistolShoot3, TEXT("fast again") },
			{ Break, TraceSoundEvents::PistolShoot1, TEXT("another break") },
		};

		UE_LOG(LogTraceGame, Display,
			TEXT("[GunLadder] --- phase 0: the RULE, on synthetic time. reset window %.4fs; 'fast' is "
			     "%.4fs between shots and 'a break' is %.4fs. ---"), R, Fast, Break);

		FTracePistolLadder Ladder;
		double Now = 100.0;   // any monotonic origin; the rule only ever looks at differences
		int32 Wrong = 0;

		for (const FRuleStep& Step : Script)
		{
			Now += Step.Gap;
			const FName Got = Ladder.NextShot(Now, ResetSeconds);
			const bool bOk = (Got == Step.Expected);
			Wrong += bOk ? 0 : 1;

			if (bOk)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[GunLadder]   OK    +%.4fs -> %-12s   %s"),
					Step.Gap, *Got.ToString(), Step.Why);
			}
			else
			{
				UE_LOG(LogTraceGame, Error, TEXT("[GunLadder]   WRONG +%.4fs -> %-12s expected %-12s  %s"),
					Step.Gap, *Got.ToString(), *Step.Expected.ToString(), Step.Why);
			}
		}

		// "AND EVERY SHOT AFTER" TAKEN SERIOUSLY. No real clip is 200 rounds, so the only place this
		// can be shown is here. The script above ends ON A BREAK, i.e. back at shot 1, so the ladder
		// has to be walked up to 4 again before "every shot after" is a claim about anything — the
		// first version of this loop asserted from shot 2 and reported its own arithmetic as a bug.
		for (int32 Climb = 0; Climb < 3; ++Climb)
		{
			Now += Fast;
			Ladder.NextShot(Now, ResetSeconds);
		}

		int32 Held = 0;
		for (; Held < 200; ++Held)
		{
			Now += Fast;
			if (Ladder.NextShot(Now, ResetSeconds) != TraceSoundEvents::PistolShoot4)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[GunLadder]   WRONG a held burst left PistolShoot4 after %d more rounds."), Held);
				++Wrong;
				break;
			}
		}
		if (Held == 200)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[GunLadder]   OK    200 further fast rounds after shot 4 were ALL PistolShoot4 "
				     "('and every shot after')."));
		}

		return Wrong;
	}

	// =============================================================================================
	// PHASES 1..5 — the real gun
	// =============================================================================================

	struct FRun
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ATraceCharacter> Subject;
		int32 Stage = 0;
		double StageStartedReal = 0.0;
		double GapUntilReal = 0.0;
		bool bTriggerDown = false;
		int32 RuleFailures = 0;
		float ResetSeconds = 0.f;

		/** Where each phase's shots start in the watcher's log. */
		int32 FastBurstStart = 0;
		int32 AfterGapStart = 0;
		int32 SmgStart = 0;

		int32 Failures = 0;
		bool bAborted = false;
		FString AbortReason;
	};

	static TSharedPtr<FRun> GRun;

	/**
	 * One press-and-release per tick.
	 *
	 * NOT a held trigger, deliberately: spec v29 §2b makes the pistol semi-automatic this same patch,
	 * and a harness that held the button would measure one shot on a semi-auto gun and a full burst on
	 * an automatic one — i.e. it would go red for a change in somebody else's slice rather than for
	 * anything about the sound. Toggling gives a fresh trigger press every frame, which is legal for
	 * both, and the gun's own rate gate is left as the only thing deciding when a round leaves.
	 */
	static void PulseTrigger(FRun& Run, UTraceWeaponComponent* Weapon)
	{
		if (Weapon == nullptr)
		{
			return;
		}
		if (Run.bTriggerDown)
		{
			Weapon->StopFire();
		}
		else
		{
			Weapon->StartFire();
		}
		Run.bTriggerDown = !Run.bTriggerDown;
	}

	static void ReleaseTrigger(FRun& Run, UTraceWeaponComponent* Weapon)
	{
		if (Weapon != nullptr)
		{
			Weapon->StopFire();
		}
		Run.bTriggerDown = false;
	}

	/** Prints one burst's shots and checks them against @p Expected. Returns the mismatch count. */
	static int32 GradeBurst(const TArray<FTraceShotAudioRecord>& Log, int32 First, int32 Count,
		const TArray<FName>& Expected, const TCHAR* Label)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[GunLadder] --- %s ---"), Label);

		int32 Wrong = 0;
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			const int32 Slot = First + Index;
			if (Slot >= First + Count || !Log.IsValidIndex(Slot))
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[GunLadder]   MISSING shot %d of %d — the gun never produced it."),
					Index + 1, Expected.Num());
				++Wrong;
				continue;
			}

			const FTraceShotAudioRecord& Shot = Log[Slot];
			const bool bOk = (Shot.Event == Expected[Index]);
			Wrong += bOk ? 0 : 1;

			// THE LINE THE SPEC ASKS FOR: the clip name, per shot, with the gap that decided it.
			// A negative gap means "there was no previous shot" — the first pistol round of a run, or
			// any SMG round, which has no ladder and therefore no gap that decided anything.
			const FString Gap = (Shot.SinceLast >= 0.0)
				? FString::Printf(TEXT("%.4fs after the previous shot"), Shot.SinceLast)
				: FString(TEXT("no previous shot"));

			if (bOk)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[GunLadder]   OK    shot %d  %-12s  (%s)"),
					Index + 1, *Shot.Event.ToString(), *Gap);
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[GunLadder]   WRONG shot %d  %-12s  expected %-12s  (%s)"),
					Index + 1, *Shot.Event.ToString(), *Expected[Index].ToString(), *Gap);
			}
		}

		if (Count > Expected.Num())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[GunLadder]   %d extra shot(s) arrived in this window and were not graded."),
				Count - Expected.Num());
		}
		return Wrong;
	}

	static void Report(FRun& Run)
	{
		UWorld* World = Run.World.Get();
		UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World);

		if (Run.bAborted || Watch == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[GunLadder] ABORTED: %s. Nothing is claimed about the ladder."),
				Run.bAborted ? *Run.AbortReason : TEXT("the audio watcher went away"));
			UE_LOG(LogTraceGame, Error,
				TEXT("TRACE GUN LADDER VERDICT: NOT MEASURED."));
			GRun.Reset();
			return;
		}

		const TArray<FTraceShotAudioRecord>& Log = Watch->GetShotLog();

		TArray<FName> FastExpected;
		FastExpected.Add(TraceSoundEvents::PistolShoot1);
		FastExpected.Add(TraceSoundEvents::PistolShoot2);
		FastExpected.Add(TraceSoundEvents::PistolShoot3);
		FastExpected.Add(TraceSoundEvents::PistolShoot4);
		FastExpected.Add(TraceSoundEvents::PistolShoot4);
		FastExpected.Add(TraceSoundEvents::PistolShoot4);

		TArray<FName> GapExpected;
		GapExpected.Add(TraceSoundEvents::PistolShoot1);
		GapExpected.Add(TraceSoundEvents::PistolShoot2);
		GapExpected.Add(TraceSoundEvents::PistolShoot3);

		TArray<FName> SmgExpected;
		for (int32 Index = 0; Index < SmgBurstShots; ++Index)
		{
			SmgExpected.Add(TraceSoundEvents::SmgShoot1);
		}

		Run.Failures += Run.RuleFailures;
		Run.Failures += GradeBurst(Log, Run.FastBurstStart, Run.AfterGapStart - Run.FastBurstStart,
			FastExpected,
			TEXT("phase 1: A FAST BURST through the real gun. Every gap is under the reset window, so "
			     "the ladder must climb 1 2 3 4 and then hold at 4."));
		Run.Failures += GradeBurst(Log, Run.AfterGapStart, Run.SmgStart - Run.AfterGapStart,
			GapExpected,
			TEXT("phase 2: THE SAME TRIGGER, WITH A GAP LONGER THAN THE RESET IN IT. The first shot "
			     "after the gap must be PistolShoot1 again."));
		Run.Failures += GradeBurst(Log, Run.SmgStart, Log.Num() - Run.SmgStart, SmgExpected,
			TEXT("phase 3: THE SMG (§1d). SmgShoot1 on every round, no ladder, no reset."));

		// §1e, checked rather than assumed: every clip that played must be declared game-side, and the
		// authority must have SENT a multicast for it. A gunshot that was only heard locally is the
		// exact failure "gunshots should be global" names.
		int32 NotGlobal = 0;
		for (const FTraceShotAudioRecord& Shot : Log)
		{
			if (TraceSoundEvents::SideOf(Shot.Event) != ETraceSoundSide::World)
			{
				++NotGlobal;
			}
		}
		Run.Failures += NotGlobal;

		const UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		const int32 Multicasts = (Audio != nullptr) ? Audio->GetCounters().MulticastsSent : 0;

		UE_LOG(LogTraceGame, Display, TEXT("[GunLadder] --- §1e: are the gunshots GLOBAL? ---"));
		if (NotGlobal == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[GunLadder]   OK    all %d shot clip(s) are declared game-side; the authority has "
				     "sent %d multicast(s) this session, so other machines hear them at the muzzle."),
				Log.Num(), Multicasts);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[GunLadder]   WRONG %d shot(s) played a CLIENT-SIDE clip. §1e says gunshots are "
				     "global."), NotGlobal);
		}

		const UTraceAudioSettings& Settings = UTraceAudioSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[GunLadder] knobs: literal reset %.3fs, floor %.2fx the pistol's own interval, "
			     "EFFECTIVE %.4fs. (The literal 0.3s alone is SHORTER than the 0.3158s the pistol "
			     "fires at, which is why the floor exists — Trace.Audio.PistolResetFloor 0 is the red "
			     "arm and must turn every shot below into PistolShoot1.)"),
			Settings.PistolLadderResetSeconds, Settings.PistolLadderResetIntervalFloor,
			Settings.GetPistolLadderResetSeconds());

		// TWO CALLS, NOT A TERNARY VERBOSITY: UE_LOG pastes its second argument onto `ELogVerbosity::`.
#define TRACE_GUNLADDER_VERDICT_TEXT \
	TEXT("TRACE GUN LADDER VERDICT: %s - %d problem(s) across the rule, the fast burst, the gapped " \
	     "burst, the SMG and the global check. %d shot(s) were logged.")
#define TRACE_GUNLADDER_VERDICT_ARGS \
	(Run.Failures == 0 ? TEXT("PASS") : TEXT("FAIL")), Run.Failures, Log.Num()

		if (Run.Failures == 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_GUNLADDER_VERDICT_TEXT, TRACE_GUNLADDER_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_GUNLADDER_VERDICT_TEXT, TRACE_GUNLADDER_VERDICT_ARGS);
		}

#undef TRACE_GUNLADDER_VERDICT_ARGS
#undef TRACE_GUNLADDER_VERDICT_TEXT

		GRun.Reset();
	}

	static void Abort(FRun& Run, const FString& Why)
	{
		Run.bAborted = true;
		Run.AbortReason = Why;
	}

	static bool Tick(float /*Delta*/)
	{
		TSharedPtr<FRun> Run = GRun;
		if (!Run.IsValid())
		{
			return false;
		}

		UWorld* World = Run->World.Get();
		UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World);
		if (World == nullptr || Watch == nullptr)
		{
			Abort(*Run, TEXT("the world or the audio watcher went away mid-run"));
			Report(*Run);
			return false;
		}

		const double NowReal = FPlatformTime::Seconds();
		FString WhyNot;
		ATraceCharacter* Subject = FindSubject(World, WhyNot);
		UTraceWeaponComponent* Weapon = (Subject != nullptr) ? Subject->Weapon : nullptr;

		if (Subject == nullptr || Weapon == nullptr)
		{
			// Staging is a legitimate state for the first seconds of a match, and this command has to
			// survive being launched from -TraceExec on frame one.
			if ((NowReal - Run->StageStartedReal) < StageTimeoutSeconds)
			{
				return true;
			}
			Abort(*Run, FString::Printf(TEXT("no drivable subject: %s"), *WhyNot));
			Report(*Run);
			return false;
		}
		Run->Subject = Subject;

		const int32 Logged = Watch->GetShotLog().Num();

		switch (Run->Stage)
		{
		case 0:
			// STAGE: the pistol, in hand, with a full clip and no reload running.
			if (Weapon->GetEquippedWeapon() != ETraceEquippedWeapon::Gun)
			{
				Weapon->RequestEquip(ETraceEquippedWeapon::Gun);
				return true;
			}
			if (Weapon->IsDeploying() || Weapon->IsReloading())
			{
				return true;
			}
			if (Weapon->GetClipAmmo() < (FastBurstShots + AfterGapShots))
			{
				Weapon->RequestReload();
				return true;
			}

			// THE LOG IS CLEARED HERE, not at arming: a bot's shot fired while this was staging would
			// otherwise be graded as the player's first round.
			Watch->ResetShotLog();
			Run->FastBurstStart = 0;
			UE_LOG(LogTraceGame, Display,
				TEXT("[GunLadder] --- phase 1: %s holds the PISTOL, clip %d. Pulsing the real trigger; "
				     "the gun's own rate gate (%.4fs) decides when a round leaves. ---"),
				*GetNameSafe(Subject), Weapon->GetClipAmmo(), Weapon->GetFireInterval());
			Run->Stage = 1;
			Run->StageStartedReal = NowReal;
			return true;

		case 1:
			// FAST BURST. Every gap is one fire interval, which is inside the reset window.
			if (Logged >= FastBurstShots)
			{
				ReleaseTrigger(*Run, Weapon);
				Run->AfterGapStart = Logged;
				Run->GapUntilReal = NowReal + static_cast<double>(Run->ResetSeconds) * GapScale;
				UE_LOG(LogTraceGame, Display,
					TEXT("[GunLadder] --- phase 2: trigger released. Waiting %.3fs, which is %.1fx the "
					     "%.4fs reset window, so the NEXT shot must be PistolShoot1. ---"),
					static_cast<double>(Run->ResetSeconds) * GapScale, GapScale, Run->ResetSeconds);
				Run->Stage = 2;
				return true;
			}
			if ((NowReal - Run->StageStartedReal) > StageTimeoutSeconds)
			{
				ReleaseTrigger(*Run, Weapon);
				// NOT "no round was fired" — rounds may well have left the clip. What is missing is the
				// ANNOUNCEMENT, which is a different claim, and Trace.Audio.ShotWatch 0 produces exactly
				// this state on purpose. Saying the wrong one would send the next reader to the gun.
				Abort(*Run, FString::Printf(
					TEXT("only %d of %d shots were ANNOUNCED in %.0fs (Trace.Audio.ShotWatch is %s). The "
					     "rounds may have left the clip; nothing turned them into a sound"),
					Logged, FastBurstShots, StageTimeoutSeconds,
					(IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Audio.ShotWatch")) != nullptr
						&& IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Audio.ShotWatch"))->GetInt() != 0)
						? TEXT("on") : TEXT("OFF")));
				Report(*Run);
				return false;
			}
			PulseTrigger(*Run, Weapon);
			return true;

		case 2:
			// THE GAP. Trigger fully released and nothing fired.
			if (NowReal < Run->GapUntilReal)
			{
				return true;
			}
			Run->Stage = 3;
			Run->StageStartedReal = NowReal;
			return true;

		case 3:
			// THE BURST AFTER THE GAP.
			if (Logged >= Run->AfterGapStart + AfterGapShots)
			{
				ReleaseTrigger(*Run, Weapon);
				Run->SmgStart = Logged;
				Run->Stage = 4;
				Run->StageStartedReal = NowReal;
				return true;
			}
			if ((NowReal - Run->StageStartedReal) > StageTimeoutSeconds)
			{
				ReleaseTrigger(*Run, Weapon);
				Abort(*Run, TEXT("the burst after the gap never produced its rounds"));
				Report(*Run);
				return false;
			}
			PulseTrigger(*Run, Weapon);
			return true;

		case 4:
			// STAGE THE SMG.
			if (Weapon->GetEquippedWeapon() != ETraceEquippedWeapon::Smg)
			{
				Weapon->RequestEquip(ETraceEquippedWeapon::Smg);
				return true;
			}
			if (Weapon->IsDeploying() || Weapon->IsReloading())
			{
				return true;
			}
			if (Weapon->GetClipAmmo() < SmgBurstShots)
			{
				Weapon->RequestReload();
				return true;
			}
			// The swap itself must not have been heard as a shot. The watcher re-seeds on a weapon
			// change instead of announcing the magazine exchange; if that guard were missing, the log
			// would have grown here and the phase-3 grading below would report the difference.
			Run->SmgStart = Watch->GetShotLog().Num();
			UE_LOG(LogTraceGame, Display,
				TEXT("[GunLadder] --- phase 3: the SMG, clip %d, interval %.4fs. §1d: every round is "
				     "SmgShoot1. ---"), Weapon->GetClipAmmo(), Weapon->GetFireInterval());
			Run->Stage = 5;
			Run->StageStartedReal = NowReal;
			return true;

		case 5:
			if (Logged >= Run->SmgStart + SmgBurstShots)
			{
				ReleaseTrigger(*Run, Weapon);
				Report(*Run);
				return false;
			}
			if ((NowReal - Run->StageStartedReal) > StageTimeoutSeconds)
			{
				ReleaseTrigger(*Run, Weapon);
				Abort(*Run, TEXT("the SMG burst never produced its rounds"));
				Report(*Run);
				return false;
			}
			PulseTrigger(*Run, Weapon);
			return true;

		default:
			ReleaseTrigger(*Run, Weapon);
			Report(*Run);
			return false;
		}
	}

	static void GunLadder()
	{
		UWorld* World = AuthoritativeWorld();
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[GunLadder] no authoritative game world. The clip is server state and the gunshot "
				     "is a game-side sound, so this must run on the server."));
			return;
		}
		if (GRun.IsValid())
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[GunLadder] a run is already in progress."));
			return;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Audio.GunLadder (spec v29 s1c/s1d/s1e) ================"));
		if (World->GetAudioDeviceRaw() == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[GunLadder] NO AUDIO DEVICE (-nosound, or a dedicated server). The LADDER is still "
				     "measured — it is decided before the device test — but nothing is audible, so this "
				     "run says nothing about whether a gunshot can be heard. Run it without -nosound."));
		}

		TSharedPtr<FRun> Run = MakeShared<FRun>();
		Run->World = World;
		Run->StageStartedReal = FPlatformTime::Seconds();
		Run->ResetSeconds = UTraceAudioSettings::Get().GetPistolLadderResetSeconds();

		// PHASE 0 RUNS RIGHT NOW, synchronously: the rule needs no pawn, and knowing whether the rule
		// itself is right BEFORE the gun starts is what tells a reader which half a later failure
		// belongs to.
		Run->RuleFailures = CheckRule(Run->ResetSeconds);

		GRun = Run;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.f);
	}

	// =============================================================================================
	// Trace.Audio.Footsteps — spec v29 §1b, the randomisation
	// =============================================================================================

	/** Everything phase 1 measured, carried into the combined verdict. */
	struct FWalkRun
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ATraceCharacter> Pawn;
		double StartedReal = 0.0;
		double DistanceUU = 0.0;
		int32 StepsAtStart = 0;
		int32 DrawProblems = 0;
		int32 Draws = 0;
		int32 Repeats = 0;
		int32 Unused = 0;
		int32 Lopsided = 0;
		int32 NotWorld = 0;
	};

	static TSharedPtr<FWalkRun> GWalk;

	/** How long the pawn walks. Long enough for a dozen strides at running speed. */
	static constexpr double WalkSeconds = 3.0;

	static bool WalkTick(float Delta)
	{
		TSharedPtr<FWalkRun> Run = GWalk;
		if (!Run.IsValid())
		{
			return false;
		}

		UWorld* World = Run->World.Get();
		ATraceCharacter* Pawn = Run->Pawn.Get();
		UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World);
		if (World == nullptr || Pawn == nullptr || Watch == nullptr)
		{
			GWalk.Reset();
			return false;
		}

		const double Elapsed = FPlatformTime::Seconds() - Run->StartedReal;
		if (Elapsed < WalkSeconds)
		{
			// The real movement path. The accumulator reads Velocity and IsMovingOnGround() off the
			// component this drives, so a step here is a step the game would have played.
			Pawn->AddMovementInput(Pawn->GetActorForwardVector().GetSafeNormal2D(), 1.f);
			if (const UCharacterMovementComponent* Move = Pawn->GetCharacterMovement())
			{
				Run->DistanceUU += Move->Velocity.Size2D() * static_cast<double>(FMath::Max(0.f, Delta));
			}
			return true;
		}

		// *** THIS PAWN'S STEPS, NOT THE WORLD'S. ***
		//
		// It read the LENGTH of the footstep log, which is world-wide. On the practice range, where
		// this was first written, the subject is the only thing walking and the two numbers are the
		// same number. In a REAL MATCH they are not: run against Arena_Baked?bots=6 and the walk
		// measured 43 steps for 2334 uu against 13.3 due — thirty of them belonged to six bots
		// jogging around the arena, and the harness reported FAIL for a system that was working.
		//
		// The dangerous direction is the other one. Six bots walking produce steps whatever the
		// subject's own accumulator does, so a completely dead stride on the subject would still have
		// landed inside the +/-50% band and PASSED. Filtering by pawn is what makes this measure its
		// own rule instead of the room's ambient step rate.
		const int32 Walked = Watch->CountFootstepsForSince(Pawn, Run->StepsAtStart);
		const int32 WorldWide = Watch->GetFootstepLog().Num() - Run->StepsAtStart;
		const float Stride = FMath::Max(1.f, UTraceAudioSettings::Get().FootstepStrideUU);
		const double Expected = Run->DistanceUU / Stride;

		UE_LOG(LogTraceGame, Display,
			TEXT("[Footsteps] --- the STRIDE, walked for real ---"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[Footsteps] %s walked %.0f uu in %.1fs and the accumulator played %d footstep(s) "
			     "FOR THIS PAWN (%d were played in the world over the same window, by everyone). "
			     "The stride knob is %.0f uu, so %.1f were due."),
			*GetNameSafe(Pawn), Run->DistanceUU, WalkSeconds, Walked, WorldWide, Stride, Expected);

		// A LOOSE BAND ON PURPOSE. This is asking "is the trigger wired and roughly in units of
		// distance", not "is the arithmetic exact to a frame": a walk that starts from a standstill
		// spends its first frames accelerating, and the accumulator quite correctly pays out nothing
		// for them.
		const bool bWalkOk = (Walked > 0)
			&& (Expected <= 0.5 || (Walked >= Expected * 0.5 && Walked <= Expected * 1.5 + 1.0));
		if (!bWalkOk)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[Footsteps] the stride accumulator produced %d step(s) for %.0f uu of walking. "
				     "Either nothing is driving it or it is not measuring distance."),
				Walked, Run->DistanceUU);
		}

		const int32 Problems = Run->DrawProblems + (bWalkOk ? 0 : 1);

		// TWO CALLS, NOT A TERNARY VERBOSITY: UE_LOG pastes its second argument onto `ELogVerbosity::`.
#define TRACE_FOOTSTEPS_VERDICT_TEXT \
	TEXT("TRACE FOOTSTEPS VERDICT: %s - %d draw(s): %d immediate repeat(s) (must be 0), %d clip(s) " \
	     "never drawn, %d lopsided, %d not game-side; and %d real step(s) over %.0f uu of walking.")
#define TRACE_FOOTSTEPS_VERDICT_ARGS \
	(Problems == 0 ? TEXT("PASS") : TEXT("FAIL")), Run->Draws, Run->Repeats, Run->Unused, \
	Run->Lopsided, Run->NotWorld, Walked, Run->DistanceUU

		if (Problems == 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_FOOTSTEPS_VERDICT_TEXT, TRACE_FOOTSTEPS_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_FOOTSTEPS_VERDICT_TEXT, TRACE_FOOTSTEPS_VERDICT_ARGS);
		}

#undef TRACE_FOOTSTEPS_VERDICT_ARGS
#undef TRACE_FOOTSTEPS_VERDICT_TEXT

		GWalk.Reset();
		return false;
	}

	static void StartWalkPhase(UWorld* World, ATraceCharacter* Pawn, int32 DrawProblems, int32 Draws,
		int32 Repeats, int32 Unused, int32 Lopsided, int32 NotWorld)
	{
		UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World);
		if (Watch == nullptr || Pawn == nullptr)
		{
			return;
		}

		TSharedPtr<FWalkRun> Run = MakeShared<FWalkRun>();
		Run->World = World;
		Run->Pawn = Pawn;
		Run->StartedReal = FPlatformTime::Seconds();
		Run->StepsAtStart = Watch->GetFootstepLog().Num();
		Run->DrawProblems = DrawProblems;
		Run->Draws = Draws;
		Run->Repeats = Repeats;
		Run->Unused = Unused;
		Run->Lopsided = Lopsided;
		Run->NotWorld = NotWorld;
		GWalk = Run;

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&WalkTick), 0.f);
	}

	static void Footsteps(const TArray<FString>& Args)
	{
		UWorld* World = AuthoritativeWorld();
		UTraceAudioWatchSubsystem* Watch = UTraceAudioWatchSubsystem::Get(World);
		if (World == nullptr || Watch == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Footsteps] no authoritative game world. Footsteps are a game-side sound the "
				     "server multicasts, so this must run on the server."));
			return;
		}

		FString WhyNot;
		ATraceCharacter* Subject = FindSubject(World, WhyNot);
		if (Subject == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[Footsteps] no drivable pawn: %s"), *WhyNot);
			return;
		}

		const int32 Draws = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 22, 500) : 220;

		UE_LOG(LogTraceGame, Display,
			TEXT("================ Trace.Audio.Footsteps (spec v29 s1b) ================"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[Footsteps] drawing %d steps for %s through the SHIPPING chooser. The walk itself is "
			     "not what is under test — the RANDOMISATION is — so the strides are driven directly "
			     "rather than by making a pawn walk %.0f uu %d times."),
			Draws, *GetNameSafe(Subject), UTraceAudioSettings::Get().FootstepStrideUU, Draws);

		Watch->ResetFootstepLog();
		for (int32 Step = 0; Step < Draws; ++Step)
		{
			Watch->AnnounceFootstep(Subject);
		}

		const TArray<FName>& Log = Watch->GetFootstepLog();
		const int32 ClipCount = TraceSoundEvents::FootstepCount();

		TArray<int32> Histogram;
		Histogram.SetNumZeroed(ClipCount);
		int32 Repeats = 0;

		for (int32 Index = 0; Index < Log.Num(); ++Index)
		{
			if (Index > 0 && Log[Index] == Log[Index - 1])
			{
				++Repeats;
				UE_LOG(LogTraceGame, Error,
					TEXT("[Footsteps]   REPEAT at %d: %s twice running."), Index, *Log[Index].ToString());
			}
			for (int32 Clip = 0; Clip < ClipCount; ++Clip)
			{
				if (TraceSoundEvents::FootstepAt(Clip) == Log[Index])
				{
					++Histogram[Clip];
					break;
				}
			}
		}

		int32 Unused = 0;
		int32 Lopsided = 0;
		const double Expected = static_cast<double>(Log.Num()) / FMath::Max(1, ClipCount);

		UE_LOG(LogTraceGame, Display, TEXT("[Footsteps] %-9s %6s   %s"),
			TEXT("CLIP"), TEXT("DRAWN"), TEXT("share (expected 1/11 = 9.1%)"));
		for (int32 Clip = 0; Clip < ClipCount; ++Clip)
		{
			const double Share = (Log.Num() > 0) ? 100.0 * Histogram[Clip] / Log.Num() : 0.0;
			UE_LOG(LogTraceGame, Display, TEXT("[Footsteps] %-9s %6d   %5.1f%%"),
				*TraceSoundEvents::FootstepAt(Clip).ToString(), Histogram[Clip], Share);

			// "VARIED" HAS TO MEAN ALL OF THEM. A chooser that only ever picked three clips would pass
			// a no-repeat check with full marks, which is why the histogram is graded and not merely
			// printed. The bands are deliberately loose — this is a randomness sanity check, not a
			// statistics exam, and a tight band would make the harness flaky.
			if (Histogram[Clip] == 0)
			{
				++Unused;
			}
			else if (Histogram[Clip] < Expected * 0.4 || Histogram[Clip] > Expected * 2.0)
			{
				++Lopsided;
			}
		}

		// §1b's other half: a WORLD sound. Checked from the table, so a future edit that made them
		// local would be caught here rather than by somebody noticing in a match.
		int32 NotWorld = 0;
		for (int32 Clip = 0; Clip < ClipCount; ++Clip)
		{
			if (TraceSoundEvents::SideOf(TraceSoundEvents::FootstepAt(Clip)) != ETraceSoundSide::World)
			{
				++NotWorld;
			}
		}

		const UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(World);
		UE_LOG(LogTraceGame, Display,
			TEXT("[Footsteps] side: %d of %d clips are game-side (other players hear yours). The "
			     "authority has sent %d multicast(s) this session."),
			ClipCount - NotWorld, ClipCount,
			(Audio != nullptr) ? Audio->GetCounters().MulticastsSent : 0);

		const int32 DrawProblems = Repeats + Unused + Lopsided + NotWorld;

		// =========================================================================================
		// PHASE 2 — THE STRIDE. Everything above tested the CHOOSER; this tests the TRIGGER.
		//
		// A chooser that is perfectly random and a trigger that never fires would sail through phase 1
		// with full marks, which is the same gap Trace.Audio.Integ exists to close for the v26 nine.
		// So the pawn is now WALKED, through UPawn::AddMovementInput and the real character movement
		// component, and the footsteps that come out are the accumulator's own.
		//
		// AddMovementInput rather than a simulated keypress: the input path can be suppressed (the
		// character-select screen does exactly that, and it has already cost this project a harness
		// that reported an unwired call site), and the stride accumulator reads the movement
		// component's VELOCITY — which is what AddMovementInput drives. Nothing about the accumulator
		// is bypassed.
		// =========================================================================================
		StartWalkPhase(World, Subject, DrawProblems, Log.Num(), Repeats, Unused, Lopsided, NotWorld);
	}

	FAutoConsoleCommand CmdGunLadder(
		TEXT("Trace.Audio.GunLadder"),
		TEXT("Spec v29 s1c/s1d/s1e. Checks the pistol ladder as a RULE on synthetic time, then fires ")
		TEXT("the REAL gun and prints the clip name per shot across a fast burst, a burst with a gap ")
		TEXT("longer than the reset in it, and an SMG burst. RED ARMS: Trace.Audio.PistolResetFloor 0 ")
		TEXT("(the literal 0.3s, which the 190 RPM pistol can never beat) and Trace.Audio.ShotWatch 0."),
		FConsoleCommandDelegate::CreateStatic(&GunLadder));

	FAutoConsoleCommand CmdFootsteps(
		TEXT("Trace.Audio.Footsteps"),
		TEXT("Spec v29 s1b. Draws N footsteps (default 220) through the shipping chooser and checks ")
		TEXT("that no clip repeats twice running, that all eleven are used, that none dominates, and ")
		TEXT("that they are game-side. RED ARM: Trace.Audio.FootstepRepeatGuard 0."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Footsteps));
}

#endif // !UE_BUILD_SHIPPING
