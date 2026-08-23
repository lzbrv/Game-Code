// Trace — the evidence for the PRACTICE RANGE (spec v19 §2).
//
// ===================================================================================================
// TWO COMMANDS. THE SECOND ONE IS THE ONE THE SPEC IS SHOUTING ABOUT.
// ===================================================================================================
//
//   Trace.Practice.Verify    Does the range DO what was asked? Run it inside the range. It drives a
//                            target's damage and its respawn-in-place, racks the Core and watches it
//                            NOT move for four settle windows, turns the infinite-abilities toggle
//                            on against a live cooldown, and reopens the character select screen.
//
//   Trace.Practice.LeakTest  *** THE ONE THAT MATTERS. *** Run it in a REAL MATCH. Every range-only
//                            behaviour must be absent, and each is checked by MEASUREMENT rather
//                            than by assertion: a real cooldown is put on the local player and must
//                            still be running a second later; the character switch is actually
//                            attempted and the player must still be locked to what they had; and
//                            the world is swept for range furniture.
//
// ===================================================================================================
// WHY BOTH CAN GO RED, WHICH IS THE PART THAT MATTERS
// ===================================================================================================
//
//   Trace.Practice.LeakTest  RED ARM: `Trace.Practice.LeakArm 1`, then run it in the same real match.
//                            The arm forces TracePracticeRange::IsActive() true in every world, so
//                            the subsystem genuinely wakes up inside the match — it spawns the
//                            targets and the pads, it zeroes the cooldown, and it hands the
//                            character back. All four assertions then FAIL. That is only possible
//                            because the range's machinery lives in a world subsystem that exists
//                            everywhere and is gated, rather than on the game mode where it could
//                            not have been switched on to be caught. See the block at the top of
//                            TracePracticeRange.h.
//
//   Trace.Practice.Verify    RED ARM: run the identical command in a real match. There is no gate,
//                            no furniture, no toggle and no rack, and it reports FAIL — deliberately
//                            FAIL and not "skipped", because a harness that quietly declines to run
//                            is how this project has twice recorded a green that meant nothing.
//
// TIMING IS REAL TIME (FPlatformTime::Seconds), not world time. The character select screen can
// pause the world on a solo session, and a harness waiting on world time would wait for ever. Both
// commands unpause explicitly on entry.

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                        // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UObject/UObjectGlobals.h"             // GetDefault<> — the match's own answer, for the rule row

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterRoster.h"
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"                // the match phase and the warm-up deadline
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceMelee.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Modes/TracePracticeActors.h"
#include "Modes/TracePracticeGameMode.h"
#include "Modes/TracePracticeRange.h"
#include "Trace.h"                              // LogTraceGame

// NAMED, not anonymous — Scripts/check-jumbo-build-collisions.py gates the build on it, and four
// files in this module have already collided on a shared FindAuthoritativeWorld().
namespace TracePracticeVerify
{
	// ---------------------------------------------------------------------------------------------
	// Shared plumbing
	// ---------------------------------------------------------------------------------------------

	UWorld* FindPracticeVerifyWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		UWorld* Fallback = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* const Candidate = Context.World();
			if (Candidate == nullptr || !Candidate->IsGameWorld())
			{
				continue;
			}
			if (Candidate->GetAuthGameMode() != nullptr)
			{
				return Candidate;   // the server's world, which is the only one that can answer
			}
			if (Fallback == nullptr)
			{
				Fallback = Candidate;
			}
		}
		return Fallback;
	}

	void UnpauseFor(UWorld* WorldPtr)
	{
		if (WorldPtr == nullptr)
		{
			return;
		}
		if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
		{
			FirstPC->SetPause(false);
		}
	}

	/** Local player's Trace player state on the server, or null. */
	ATracePlayerState* LocalTraceState(UWorld* WorldPtr)
	{
		const APlayerController* const FirstPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		return (FirstPC != nullptr) ? Cast<ATracePlayerState>(FirstPC->PlayerState) : nullptr;
	}

	ATraceCharacter* LocalPawn(UWorld* WorldPtr)
	{
		APlayerController* const FirstPC = (WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		return (FirstPC != nullptr) ? Cast<ATraceCharacter>(FirstPC->GetPawn()) : nullptr;
	}

	/** Range furniture anywhere in @p WorldPtr: pads plus dummy controllers. */
	int32 CountRangeFurniture(UWorld* WorldPtr, int32& OutPads, int32& OutDummies)
	{
		OutPads = 0;
		OutDummies = 0;
		if (WorldPtr == nullptr)
		{
			return 0;
		}

		for (TActorIterator<ATracePracticePad> It(WorldPtr); It; ++It)
		{
			++OutPads;
		}
		for (TActorIterator<ATracePracticeDummyController> It(WorldPtr); It; ++It)
		{
			++OutDummies;
		}
		return OutPads + OutDummies;
	}

	struct FPracticeTally
	{
		int32 Passed = 0;
		int32 Failed = 0;

		void Report(bool bCondition, const TCHAR* Tag, const TCHAR* What)
		{
			if (bCondition)
			{
				++Passed;
				UE_LOG(LogTraceGame, Display, TEXT("[%s] PASS  %s"), Tag, What);
			}
			else
			{
				++Failed;
				UE_LOG(LogTraceGame, Error, TEXT("[%s] FAIL  %s"), Tag, What);
			}
		}
	};

	// =============================================================================================
	// Trace.Practice.LeakTest — THE RANGE MUST NOT LEAK
	// =============================================================================================

	struct FLeakRun
	{
		TWeakObjectPtr<UWorld> WorldPtr;
		FPracticeTally Tally;

		/** 0 = waiting for a player state, 1 = baseline planted and cheats asked for, 2 = judging. */
		int32 Phase = 0;

		double StartRealTime = 0.0;
		double PlantedRealTime = 0.0;

		/** Cooldown we deliberately put on the local player, in seconds. */
		static constexpr float PlantedCooldownSeconds = 6.f;

		/**
		 * Real seconds we let the range's 5 Hz poll run for before judging. ~10 polls of the range
		 * and ~8 of the game mode's select poll, so a cheat that was going to apply has applied.
		 */
		static constexpr double SettleRealSeconds = 2.0;

		/**
		 * How long to wait for the local player to have a PlayerState.
		 *
		 * -ExecCmds fires on the FIRST TICK of the loaded map, which is before the listen/standalone
		 * player has finished logging in. Judging then would measure an empty world and report a
		 * green that means "there was nobody to cheat for" — the exact shape of manufactured
		 * evidence this project has already been caught by.
		 */
		static constexpr double ReadyWaitSeconds = 20.0;

		/** What SetInfiniteAbilities() answered when we asked it to cheat. */
		bool bInfiniteAsked = false;

		/** What ReopenCharacterSelect() answered when we asked it to unlock us. */
		bool bSwitchAsked = false;

		/** The character and lock the local player had BEFORE we tried to take them away. */
		ETraceCharacterId CharacterBefore = ETraceCharacterId::None;
		bool bLockedBefore = false;
	};

	TSharedPtr<FLeakRun> GLeakRun;

	bool TickLeakRun(float /*Unused*/);

	void RunLeakTest()
	{
		UWorld* const WorldPtr = FindPracticeVerifyWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[PRACTICELEAK] no game world; nothing to test."));
			return;
		}

		UnpauseFor(WorldPtr);

		const bool bIsRangeMode = (WorldPtr->GetAuthGameMode<ATracePracticeGameMode>() != nullptr);

		UE_LOG(LogTraceGame, Display,
			TEXT("================================================================================"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[PRACTICELEAK] SPEC v19 §2: the practice range must NOT leak into a real match."));
		UE_LOG(LogTraceGame, Display,
			TEXT("[PRACTICELEAK] game mode is '%s'; red arm Trace.Practice.LeakArm is %s."),
			*GetNameSafe(WorldPtr->GetAuthGameMode()),
			TracePracticeRange::IsLeakArmed() ? TEXT("*** ON ***") : TEXT("off"));

		if (bIsRangeMode)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[PRACTICELEAK] *** THIS IS THE PRACTICE RANGE. *** The leak test only means "
				     "anything in a REAL match — open /Game/Maps/Arena_Baked with the ordinary game "
				     "mode and run it there. Refusing rather than reporting a green that proves "
				     "nothing."));
			return;
		}

		TSharedPtr<FLeakRun> Run = MakeShared<FLeakRun>();
		Run->WorldPtr = WorldPtr;
		Run->StartRealTime = FPlatformTime::Seconds();

		GLeakRun = Run;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickLeakRun), 0.f);
	}

	/**
	 * Phase 0: establish the baseline, then ask the range to cheat.
	 *
	 * Split out of the console command because -ExecCmds fires on the FIRST TICK of the loaded map,
	 * before the player has logged in. See FLeakRun::ReadyWaitSeconds.
	 */
	void PlantLeakBaseline(FLeakRun& Run, UWorld* WorldPtr)
	{
		// A player who is STILL CHOOSING cannot be "freed to choose", so the range would refuse the
		// switch for a reason that has nothing to do with the gate — and the red arm would then
		// report a green that means "the harness asked at the wrong moment". So: if the local player
		// holds nothing yet, lock them onto the first character before asking anything.
		//
		// FirstId (Rocco) SPECIFICALLY, and that is not arbitrary. Elle is the one character that
		// overrides GetCharacterOwnedCooldownRemaining(), so her ring can read non-zero for reasons
		// the range never touches — measuring the leak on her would let assertion B pass in the red
		// arm for the wrong reason.
		ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
		if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(TraceState))
		{
			if (Abilities->GetCharacterId() == ETraceCharacterId::None
				&& UTraceAbilityComponent::AreCharactersEnabled(WorldPtr))
			{
				Abilities->ServerSetCharacter(static_cast<ETraceCharacterId>(TraceCharacterRoster::FirstId));
				if (TraceState != nullptr)
				{
					TraceState->ServerMarkCharacterResolved(/*bLocked=*/true, /*bWasChosen=*/false);
					TraceState->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
				}
			}

			// ---- plant a REAL cooldown, so "infinite abilities is off" is measured, not asserted --
			Abilities->DebugSetActivatedCooldown(FLeakRun::PlantedCooldownSeconds);
			Run.CharacterBefore = Abilities->GetCharacterId();
		}
		if (TraceState != nullptr)
		{
			Run.bLockedBefore = TraceState->bCharacterLocked;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[PRACTICELEAK] baseline: character=%s locked=%s, %.0fs cooldown planted."),
			TraceCharacterIdToString(Run.CharacterBefore),
			Run.bLockedBefore ? TEXT("yes") : TEXT("no"), FLeakRun::PlantedCooldownSeconds);

		// ---- now ask the range to cheat, exactly as a player standing on a pad would --------------
		if (UTracePracticeRangeSubsystem* Range = UTracePracticeRangeSubsystem::Get(WorldPtr))
		{
			Run.bInfiniteAsked = Range->SetInfiniteAbilities(true);
			Run.bSwitchAsked = Range->ReopenCharacterSelect(TraceState);
		}

		Run.PlantedRealTime = FPlatformTime::Seconds();
		Run.Phase = 1;
	}

	bool TickLeakRun(float /*Unused*/)
	{
		TSharedPtr<FLeakRun> Run = GLeakRun;
		if (!Run.IsValid())
		{
			return false;
		}

		UWorld* const WorldPtr = Run->WorldPtr.Get();
		if (WorldPtr == nullptr)
		{
			GLeakRun.Reset();
			return false;
		}

		const double SinceStart = FPlatformTime::Seconds() - Run->StartRealTime;

		// ---- phase 0: wait for somebody to cheat FOR ---------------------------------------------
		if (Run->Phase == 0)
		{
			const bool bReady = (LocalTraceState(WorldPtr) != nullptr)
				&& (UTraceAbilityComponent::Get(LocalTraceState(WorldPtr)) != nullptr);

			if (!bReady)
			{
				if (SinceStart < FLeakRun::ReadyWaitSeconds)
				{
					return true;
				}

				UE_LOG(LogTraceGame, Error,
					TEXT("[PRACTICELEAK] VERDICT: *** INVALID *** — no local player with an ability "
					     "component after %.0fs. Nothing was measured; this is NOT a pass."),
					FLeakRun::ReadyWaitSeconds);
				GLeakRun.Reset();
				return false;
			}

			PlantLeakBaseline(*Run, WorldPtr);
			return true;
		}

		// Let the range's 5 Hz poll and the game mode's 4 Hz select poll each run several times. If
		// the cheats were going to apply, they have had every chance to.
		if (FPlatformTime::Seconds() - Run->PlantedRealTime < FLeakRun::SettleRealSeconds)
		{
			return true;
		}

		const UTracePracticeRangeSubsystem* const Range = UTracePracticeRangeSubsystem::Get(WorldPtr);
		ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
		const UTraceAbilityComponent* const Abilities = UTraceAbilityComponent::Get(TraceState);

		// ---- A. NO RANGE FURNITURE ---------------------------------------------------------------
		int32 PadCount = 0;
		int32 DummyCount = 0;
		CountRangeFurniture(WorldPtr, PadCount, DummyCount);

		Run->Tally.Report(PadCount == 0, TEXT("PRACTICELEAK"),
			*FString::Printf(TEXT("no practice pads in a real match (found %d). The no-turnover spot "
			                      "IS a pad, so this is that cheat's absence."), PadCount));
		Run->Tally.Report(DummyCount == 0, TEXT("PRACTICELEAK"),
			*FString::Printf(TEXT("no practice targets in a real match (found %d)."), DummyCount));

		// ---- B. INFINITE ABILITIES IS OFF, AND THE COOLDOWN PROVES IT ----------------------------
		const bool bInfiniteOn = (Range != nullptr) && Range->IsInfiniteAbilitiesOn();
		const float CooldownNow = (Abilities != nullptr) ? Abilities->GetActivatedCooldownRemaining() : -1.f;

		Run->Tally.Report(!Run->bInfiniteAsked && !bInfiniteOn, TEXT("PRACTICELEAK"),
			TEXT("the infinite-abilities toggle REFUSED outside the range and is still off."));
		Run->Tally.Report(CooldownNow > 0.f, TEXT("PRACTICELEAK"),
			*FString::Printf(TEXT("the planted %.0fs cooldown is still running after %.1fs (%.2fs left). "
			                      "A leaked range would have zeroed it."),
				FLeakRun::PlantedCooldownSeconds, FLeakRun::SettleRealSeconds, CooldownNow));

		// ---- C. NO FREE CHARACTER SWITCHING ------------------------------------------------------
		const ETraceCharacterId CharacterNow = (Abilities != nullptr) ? Abilities->GetCharacterId() : ETraceCharacterId::None;
		const bool bLockedNow = (TraceState != nullptr) && TraceState->bCharacterLocked;
		const bool bSelectOpen = (TraceState != nullptr) && TraceState->IsCharacterSelectOpen();

		Run->Tally.Report(!Run->bSwitchAsked, TEXT("PRACTICELEAK"),
			TEXT("the character switch REFUSED outside the range."));
		Run->Tally.Report(CharacterNow == Run->CharacterBefore, TEXT("PRACTICELEAK"),
			*FString::Printf(TEXT("the player still holds the character they had (%s)."),
				TraceCharacterIdToString(CharacterNow)));
		Run->Tally.Report(bLockedNow == Run->bLockedBefore && !bSelectOpen, TEXT("PRACTICELEAK"),
			TEXT("the player's lock is untouched and no select screen was forced open."));

		// ---- D. THE RANGE NEVER BUILT ------------------------------------------------------------
		Run->Tally.Report(Range == nullptr || !Range->IsBuilt(), TEXT("PRACTICELEAK"),
			TEXT("the range subsystem is inert in this world."));

		const bool bArmed = TracePracticeRange::IsLeakArmed();
		const bool bAllGreen = (Run->Tally.Failed == 0);

		UE_LOG(LogTraceGame, Display,
			TEXT("--------------------------------------------------------------------------------"));
		if (bArmed)
		{
			// THE RED ARM'S VERDICT IS INVERTED ON PURPOSE. With the gate forced open the cheats
			// really are in this match, so a clean sheet here means the harness cannot see them and
			// is therefore worthless as evidence of anything.
			if (bAllGreen)
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[PRACTICELEAK] RED ARM VERDICT: *** BROKEN HARNESS *** — Trace.Practice.LeakArm "
					     "is ON, so the range's cheats ARE in this match, and every assertion still "
					     "passed. This harness cannot detect a leak and its green arm proves nothing."));
			}
			else
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[PRACTICELEAK] RED ARM VERDICT: correct — %d/%d assertions FAILED with the gate "
					     "forced open. The harness can see a leak, so its green arm is evidence. Set "
					     "Trace.Practice.LeakArm 0 and run again."),
					Run->Tally.Failed, Run->Tally.Failed + Run->Tally.Passed);
			}
		}
		else if (bAllGreen)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[PRACTICELEAK] VERDICT: *** PASS *** — %d/%d. Infinite abilities, free character "
				     "switching and the no-turnover spot are all absent from a real match."),
				Run->Tally.Passed, Run->Tally.Passed);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[PRACTICELEAK] VERDICT: *** FAIL *** — %d of %d assertions failed. A range "
				     "affordance has escaped into a real match."),
				Run->Tally.Failed, Run->Tally.Failed + Run->Tally.Passed);
		}

		GLeakRun.Reset();
		return false;
	}

	// =============================================================================================
	// Trace.Practice.Verify — the range does what §2 asked for
	// =============================================================================================

	struct FRangeRun
	{
		TWeakObjectPtr<UWorld> WorldPtr;
		FPracticeTally Tally;

		int32 Step = 0;
		double StepStartRealTime = 0.0;

		/** The target under test, and where it is supposed to come back to. */
		TWeakObjectPtr<ATracePracticeDummyController> Subject;
		FVector SubjectPost = FVector::ZeroVector;
		float SubjectHealthBefore = 0.f;

		/** Where the target was at the START of the stationary window, so the drift is measured. */
		FVector SubjectMark = FVector::ZeroVector;
		bool bSubjectMarked = false;

		/** Where the Core sat the moment it was racked, so "it stayed put" is a measurement. */
		FVector RackedCoreLocation = FVector::ZeroVector;

		/** Where the player was before the harness walked them onto the rack. */
		FVector PlayerHomeLocation = FVector::ZeroVector;

		ETraceCharacterId CharacterBefore = ETraceCharacterId::None;

		/** The DIFFERENT character the switch is driven to, so "switched" is a measurement. */
		ETraceCharacterId SwitchTarget = ETraceCharacterId::None;

		/** Where the player stood when the switch began, so "without leaving the range" is measured. */
		FVector PlayerLocationAtSwitch = FVector::ZeroVector;

		/**
		 * Demo 27, "Don't have a match start timer in the practice range" — THE SYMPTOM, sampled.
		 *
		 * Set on any tick that finds the pair the HUD's "MATCH STARTS IN" banner needs: the phase
		 * still WaitingForPlayers AND a warm-up deadline published on the game state. The harness is
		 * armed from -ExecCmds on the FIRST TICK of the loaded map, so it is watching before the
		 * range is furnished and therefore across the entire window in which a countdown could run;
		 * the old behaviour held that pair for five seconds and ~350 ticks, so this cannot miss it.
		 */
		bool bSawStartCountdown = false;

		/** Real seconds each waiting step is allowed. */
		static constexpr double ShortWait = 0.8;
		static constexpr double RespawnWait = 8.0;

		/**
		 * How long to wait for the range to furnish itself and the player to log in.
		 *
		 * -ExecCmds fires on the FIRST TICK of the loaded map, before the arena is built and before
		 * the subsystem's 5 Hz poll has run once. Judging then would report the range empty about a
		 * range that had not been asked to exist.
		 */
		static constexpr double ReadyWaitSeconds = 25.0;

		/**
		 * How long the racked Core is watched for.
		 *
		 * Trace.ModeB.TurnoverSettleSeconds is 0.15 s and the loose-Core reset timer is longer still,
		 * so four seconds is many settle windows and comfortably more than the arrival latch needs.
		 */
		static constexpr double RackWatchSeconds = 4.0;
	};

	TSharedPtr<FRangeRun> GRangeRun;

	bool TickRangeRun(float /*Unused*/);

	void RunRangeVerify()
	{
		UWorld* const WorldPtr = FindPracticeVerifyWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[PRACTICE] no game world; nothing to test."));
			return;
		}

		UnpauseFor(WorldPtr);

		UE_LOG(LogTraceGame, Display,
			TEXT("================================================================================"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[PRACTICE] SPEC v19 §2: stationary targets that take damage and respawn in place, a "
			     "spot the Core can be left on, infinite abilities, and character switching without "
			     "leaving the range."));

		UTracePracticeRangeSubsystem* const Range = UTracePracticeRangeSubsystem::Get(WorldPtr);

		// THE GATE IS THE FIRST ASSERTION AND IT FAILS RATHER THAN SKIPS. Running this command in a
		// real match is the cheapest red arm this feature has, and a harness that quietly declined to
		// run there would turn that arm into silence.
		FPracticeTally GateTally;
		const bool bGateOpen = TracePracticeRange::IsActive(WorldPtr);
		GateTally.Report(bGateOpen, TEXT("PRACTICE"),
			TEXT("the practice range is active in this world (open it with "
			     "?game=/Script/Trace.TracePracticeGameMode)."));

		if (!bGateOpen || Range == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[PRACTICE] VERDICT: *** FAIL *** — this world is not a practice range, so none of "
				     "the range's affordances exist here. That is the correct answer for a real match "
				     "and the reason this command doubles as a red arm."));
			return;
		}

		TSharedPtr<FRangeRun> Run = MakeShared<FRangeRun>();
		Run->WorldPtr = WorldPtr;
		Run->Tally = GateTally;
		Run->StepStartRealTime = FPlatformTime::Seconds();

		GRangeRun = Run;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickRangeRun), 0.f);
	}

	/** Advances to @p NextStep and restarts its wait clock. */
	void GoToStep(FRangeRun& Run, int32 NextStep)
	{
		Run.Step = NextStep;
		Run.StepStartRealTime = FPlatformTime::Seconds();
	}

	bool TickRangeRun(float /*Unused*/)
	{
		TSharedPtr<FRangeRun> Run = GRangeRun;
		if (!Run.IsValid())
		{
			return false;
		}

		UWorld* const WorldPtr = Run->WorldPtr.Get();
		UTracePracticeRangeSubsystem* const Range = UTracePracticeRangeSubsystem::Get(WorldPtr);
		if (WorldPtr == nullptr || Range == nullptr)
		{
			GRangeRun.Reset();
			return false;
		}

		const double SinceStep = FPlatformTime::Seconds() - Run->StepStartRealTime;
		ATraceCharacter* const PlayerPawn = LocalPawn(WorldPtr);
		ATraceCore* const TheCore = ATraceCore::Get(WorldPtr);

		// Demo 27's symptom, sampled on EVERY tick of the run rather than at one chosen moment: a
		// countdown that ran and finished before the step machine looked would be a countdown this
		// harness reported as absent. See FRangeRun::bSawStartCountdown.
		if (const ATraceGameState* const PhaseState = WorldPtr->GetGameState<ATraceGameState>())
		{
			if (PhaseState->TraceMatchState == ETraceMatchState::WaitingForPlayers
				&& PhaseState->MatchEndServerTime > 0.f)
			{
				Run->bSawStartCountdown = true;
			}
		}

		switch (Run->Step)
		{
		// -----------------------------------------------------------------------------------------
		case 0:   // the furniture exists at all
		{
			// -ExecCmds fires on the FIRST TICK of the loaded map: the arena has not been built, the
			// player has not logged in and the subsystem's 5 Hz poll has not run once. Judging then
			// would report "the range is empty" about a range that had not been asked to exist yet —
			// so wait for it, and only call it a failure once the wait is genuinely over.
			//
			// "Furnished" now includes "and play has actually started". The targets appear on the
			// subsystem's poll and the whistle follows on ATracePracticeGameMode::PokeMatchStart's
			// 1 Hz re-ask, so there is up to a second in which the range is fully built and not yet
			// live — judging inside it would report a countdown-free range as not running. Waiting
			// for the pair costs nothing (the wait is bounded by ReadyWaitSeconds either way) and is
			// what makes the "came up live" row below a measurement rather than a race.
			const ATraceGameState* const ReadyState = WorldPtr->GetGameState<ATraceGameState>();
			const bool bFurnished = Range->IsBuilt() && Range->GetDummyCount() > 0
				&& LocalPawn(WorldPtr) != nullptr
				&& ReadyState != nullptr && ReadyState->TraceMatchState == ETraceMatchState::InProgress;
			if (!bFurnished && SinceStep < FRangeRun::ReadyWaitSeconds)
			{
				return true;
			}

			int32 PadCount = 0;
			int32 DummyCount = 0;
			CountRangeFurniture(WorldPtr, PadCount, DummyCount);

			Run->Tally.Report(DummyCount > 0, TEXT("PRACTICE"),
				*FString::Printf(TEXT("the range has %d stationary targets."), DummyCount));
			Run->Tally.Report(PadCount >= 3, TEXT("PRACTICE"),
				*FString::Printf(TEXT("the range has %d pads (core rack, infinite abilities, change "
				                      "character)."), PadCount));

			// -----------------------------------------------------------------------------------
			// DEMO 27: "Don't have a match start timer in the practice range." TWO ROWS, and the
			// first one is the rule.
			// -----------------------------------------------------------------------------------
			//
			// THE RULE, and it is a COMPARISON so that "returns zero" cannot pass because everything
			// returns zero: the range's authoritative mode must answer 0 warm-up seconds while a
			// plain ATraceGameMode — the class-default object, asked the same question — must still
			// answer the match's UTraceSettings::WarmupDuration. Frame-rate independent, and it goes
			// red the day somebody zeroes WarmupDuration globally and calls the range fixed.
			//
			// RED ARM for both rows: Trace.Practice.StartCountdown 1, then run this command in the
			// range as usual. It gives the range the match's warm-up back and nothing else, and these
			// two rows — and only these two — must go red. (Running Verify in a real match, this
			// command's other red arm, cannot exercise them: it stops at the gate above.)
			const ATraceGameMode* const AuthMode = WorldPtr->GetAuthGameMode<ATraceGameMode>();
			const float RangeWarmup = (AuthMode != nullptr) ? AuthMode->GetWarmupSeconds() : -1.f;
			const float MatchWarmup = GetDefault<ATraceGameMode>()->GetWarmupSeconds();

			Run->Tally.Report(RangeWarmup == 0.f && MatchWarmup > 0.f, TEXT("PRACTICE"),
				*FString::Printf(TEXT("no match-start countdown here (range warm-up %.2fs) while a "
				                      "match still has one (%.2fs)."), RangeWarmup, MatchWarmup));

			// THE MATCH CLOCK, WHICH IS THE SAME COMPLAINT WEARING A DIFFERENT HAT. The range's
			// constructor always asked for no period structure and never got it: UE config sections
			// are inherited, so `[/Script/Trace.TraceGameMode] HalfDuration=480` landed on the
			// subclass too and the range ran 480 s halves - it announced "One 480 s half" itself
			// while the HUD counted down - and would have ended after eight minutes of practice.
			// Compared against the MATCH's answer for the same reason the warm-up rows are: a row
			// that only asserted "the range's half is huge" would pass if every mode's half were
			// huge, which measures nothing.
			const float RangeHalf = (AuthMode != nullptr) ? AuthMode->GetHalfSeconds() : -1.f;
			const float MatchHalf = GetDefault<ATraceGameMode>()->GetHalfSeconds();

			Run->Tally.Report(RangeHalf > MatchHalf * 100.f && MatchHalf > 0.f, TEXT("PRACTICE"),
				*FString::Printf(TEXT("no match clock here (range half %.0fs) while a match still "
				                      "runs one (%.0fs)."), RangeHalf, MatchHalf));

			// THE SYMPTOM, sampled from the first tick of the map (see FRangeRun::bSawStartCountdown):
			// no tick of this run ever found the phase/deadline pair the HUD draws "MATCH STARTS IN"
			// from — and by now, with the targets standing, the range is already live rather than
			// still waiting to be.
			const bool bLiveNow = (ReadyState != nullptr)
				&& ReadyState->TraceMatchState == ETraceMatchState::InProgress;

			Run->Tally.Report(!Run->bSawStartCountdown && bLiveNow, TEXT("PRACTICE"),
				*FString::Printf(TEXT("the range came up live: no countdown was ever published "
				                      "(seen=%s) and play is running now (%s)."),
					Run->bSawStartCountdown ? TEXT("yes") : TEXT("no"),
					bLiveNow ? TEXT("in progress") : TEXT("NOT in progress")));

			for (TActorIterator<ATracePracticeDummyController> It(WorldPtr); It; ++It)
			{
				if (It->GetPawn() != nullptr && It->GetHomePost() != nullptr)
				{
					Run->Subject = *It;
					Run->SubjectPost = It->GetHomePost()->GetActorLocation();
					break;
				}
			}

			if (!Run->Subject.IsValid())
			{
				Run->Tally.Report(false, TEXT("PRACTICE"), TEXT("a target with a pawn and a post exists."));
				GoToStep(*Run, 90);
				return true;
			}

			GoToStep(*Run, 1);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 1:   // "stationary" — measured, not asserted
		{
			ATracePracticeDummyController* const DummyCtrl = Run->Subject.Get();
			const ATraceCharacter* const DummyPawn = (DummyCtrl != nullptr)
				? Cast<ATraceCharacter>(DummyCtrl->GetPawn()) : nullptr;
			if (DummyPawn == nullptr)
			{
				Run->Tally.Report(false, TEXT("PRACTICE"), TEXT("the target still has a pawn."));
				GoToStep(*Run, 90);
				return true;
			}

			if (!Run->bSubjectMarked)
			{
				Run->SubjectMark = DummyPawn->GetActorLocation();
				Run->bSubjectMarked = true;
			}

			if (SinceStep < FRangeRun::ShortWait)
			{
				return true;
			}

			const double Drift = FVector::Dist2D(Run->SubjectMark, DummyPawn->GetActorLocation());
			Run->Tally.Report(Drift < 20.0, TEXT("PRACTICE"),
				*FString::Printf(TEXT("the target is STATIONARY: it moved %.1f uu in %.1fs."),
					Drift, FRangeRun::ShortWait));

			Run->SubjectHealthBefore = (DummyPawn->Health != nullptr) ? DummyPawn->Health->Health : 0.f;
			GoToStep(*Run, 2);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 2:   // it takes damage
		{
			ATracePracticeDummyController* const DummyCtrl = Run->Subject.Get();
			ATraceCharacter* const DummyPawn = (DummyCtrl != nullptr)
				? Cast<ATraceCharacter>(DummyCtrl->GetPawn()) : nullptr;
			if (DummyPawn == nullptr || DummyPawn->Health == nullptr)
			{
				Run->Tally.Report(false, TEXT("PRACTICE"), TEXT("the target has a health component."));
				GoToStep(*Run, 90);
				return true;
			}

			// The shipped damage entry point, credited to the local player, so the whole pipeline
			// (invulnerability, the regen rescind, the death broadcast) runs exactly as a bullet's
			// would.
			APlayerController* const FirstPC = WorldPtr->GetFirstPlayerController();
			DummyPawn->Health->ApplyDamage(35.f, FirstPC, FName(TEXT("Bullet")));

			const float HealthAfter = DummyPawn->Health->Health;
			Run->Tally.Report(HealthAfter < Run->SubjectHealthBefore, TEXT("PRACTICE"),
				*FString::Printf(TEXT("the target TAKES DAMAGE: %.0f -> %.0f."),
					Run->SubjectHealthBefore, HealthAfter));

			// ...and now kill it, to measure the respawn.
			DummyPawn->Health->Kill(FirstPC, FName(TEXT("Bullet")));
			GoToStep(*Run, 3);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 3:   // it respawns IN PLACE
		{
			ATracePracticeDummyController* const DummyCtrl = Run->Subject.Get();
			if (DummyCtrl == nullptr)
			{
				Run->Tally.Report(false, TEXT("PRACTICE"), TEXT("the target's controller survived its death."));
				GoToStep(*Run, 90);
				return true;
			}

			const ATraceCharacter* const FreshPawn = Cast<ATraceCharacter>(DummyCtrl->GetPawn());
			const bool bBackUp = (FreshPawn != nullptr) && FreshPawn->IsAlive();

			if (!bBackUp && SinceStep < FRangeRun::RespawnWait)
			{
				return true;
			}

			Run->Tally.Report(bBackUp, TEXT("PRACTICE"),
				*FString::Printf(TEXT("the target RESPAWNED within %.0fs of dying."), FRangeRun::RespawnWait));

			if (bBackUp)
			{
				const double AwayFromPost = FVector::Dist2D(FreshPawn->GetActorLocation(), Run->SubjectPost);
				Run->Tally.Report(AwayFromPost < 250.0, TEXT("PRACTICE"),
					*FString::Printf(TEXT("it respawned IN PLACE: %.0f uu from the post it died on "
					                      "(a team pad would be thousands)."), AwayFromPost));
			}

			GoToStep(*Run, 4);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 4:   // walk onto the rack carrying the Core
		{
			if (PlayerPawn == nullptr || TheCore == nullptr)
			{
				Run->Tally.Report(false, TEXT("PRACTICE"), TEXT("there is a local pawn and a Core to test with."));
				GoToStep(*Run, 90);
				return true;
			}

			ATracePracticePad* RackPad = nullptr;
			for (TActorIterator<ATracePracticePad> It(WorldPtr); It; ++It)
			{
				if (It->GetPadRole() == ETracePracticePadRole::CoreRack)
				{
					RackPad = *It;
					break;
				}
			}

			if (RackPad == nullptr)
			{
				Run->Tally.Report(false, TEXT("PRACTICE"), TEXT("the range has a CORE RACK pad."));
				GoToStep(*Run, 90);
				return true;
			}

			Run->PlayerHomeLocation = PlayerPawn->GetActorLocation();

			// Give the player the Core, then WALK THEM ONTO THE PAD. Deliberately the real trigger
			// and not a direct call into the subsystem: the thing a player does is stand on the disc,
			// and a harness that called the handler would not have tested the disc.
			TheCore->TryPickup(PlayerPawn);
			PlayerPawn->TeleportTo(RackPad->GetActorLocation() + FVector(0.f, 0.f, 120.f),
				PlayerPawn->GetActorRotation(), /*bIsATest=*/false, /*bNoCheck=*/true);

			GoToStep(*Run, 5);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 5:   // ...and it goes onto the rack
		{
			if (SinceStep < FRangeRun::ShortWait)
			{
				return true;
			}

			const bool bRacked = Range->IsCoreOnRack() && (TheCore != nullptr) && !TheCore->IsHeld();
			Run->Tally.Report(bRacked, TEXT("PRACTICE"),
				TEXT("stepping onto the CORE RACK with the Core LEFT IT THERE (nobody is holding it)."));

			if (!bRacked)
			{
				GoToStep(*Run, 8);
				return true;
			}

			Run->RackedCoreLocation = TheCore->GetActorLocation();
			GoToStep(*Run, 6);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 6:   // *** THE NO-TURNOVER ASSERTION ***
		{
			if (SinceStep < FRangeRun::RackWatchSeconds)
			{
				return true;
			}

			const bool bStillLoose = (TheCore != nullptr) && !TheCore->IsHeld();
			const double Moved = (TheCore != nullptr)
				? FVector::Dist(TheCore->GetActorLocation(), Run->RackedCoreLocation) : 1e9;

			Run->Tally.Report(bStillLoose, TEXT("PRACTICE"),
				*FString::Printf(TEXT("*** NO TURNOVER: %.0fs after being racked, the Core is still "
				                      "nobody's. ***"), FRangeRun::RackWatchSeconds));
			Run->Tally.Report(Moved < 120.0, TEXT("PRACTICE"),
				*FString::Printf(TEXT("and it is still where it was put (%.0f uu of drift)."), Moved));

			GoToStep(*Run, 7);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 7:   // step off and back on to collect it
		{
			if (PlayerPawn == nullptr)
			{
				GoToStep(*Run, 8);
				return true;
			}

			if (SinceStep < FRangeRun::ShortWait * 0.5)
			{
				// Step OFF first — the pad reports the entry edge, so a collect needs a fresh entry.
				PlayerPawn->TeleportTo(Run->PlayerHomeLocation, PlayerPawn->GetActorRotation(),
					/*bIsATest=*/false, /*bNoCheck=*/true);
				return true;
			}

			if (SinceStep < FRangeRun::ShortWait * 2.0)
			{
				if (Run->RackedCoreLocation != FVector::ZeroVector)
				{
					PlayerPawn->TeleportTo(Run->RackedCoreLocation + FVector(0.f, 0.f, 120.f),
						PlayerPawn->GetActorRotation(), /*bIsATest=*/false, /*bNoCheck=*/true);
					Run->RackedCoreLocation = FVector::ZeroVector;
				}
				return true;
			}

			Run->Tally.Report((TheCore != nullptr) && TheCore->GetCarrier() == PlayerPawn, TEXT("PRACTICE"),
				TEXT("walking back onto the rack COLLECTED the Core again."));

			PlayerPawn->TeleportTo(Run->PlayerHomeLocation, PlayerPawn->GetActorRotation(),
				/*bIsATest=*/false, /*bNoCheck=*/true);
			GoToStep(*Run, 8);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 8:   // infinite abilities, against a live cooldown
		{
			ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
			UTraceAbilityComponent* const Abilities = UTraceAbilityComponent::Get(TraceState);
			if (Abilities == nullptr)
			{
				Run->Tally.Report(false, TEXT("PRACTICE"), TEXT("the local player has an ability component."));
				GoToStep(*Run, 90);
				return true;
			}

			Run->CharacterBefore = Abilities->GetCharacterId();

			Abilities->DebugSetActivatedCooldown(9.f);
			const float Planted = Abilities->GetActivatedCooldownRemaining();
			Run->Tally.Report(Planted > 1.f, TEXT("PRACTICE"),
				*FString::Printf(TEXT("a %.1fs cooldown is running with the toggle OFF."), Planted));

			Range->SetInfiniteAbilities(true);
			GoToStep(*Run, 9);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 9:
		{
			if (SinceStep < FRangeRun::ShortWait)
			{
				return true;
			}

			ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
			const UTraceAbilityComponent* const Abilities = UTraceAbilityComponent::Get(TraceState);
			const float CooldownNow = (Abilities != nullptr) ? Abilities->GetActivatedCooldownRemaining() : -1.f;

			Run->Tally.Report(Range->IsInfiniteAbilitiesOn(), TEXT("PRACTICE"),
				TEXT("the INFINITE ABILITIES toggle is on inside the range."));
			Run->Tally.Report(CooldownNow <= 0.01f, TEXT("PRACTICE"),
				*FString::Printf(TEXT("...and the 9s cooldown is gone (%.2fs left). The framework's E "
				                      "timer is what TryActivate() refuses on."), CooldownNow));

			// Put it back. A harness must not leave the range in a state the next player did not ask
			// for, and "OFF" is what a range opens with.
			Range->SetInfiniteAbilities(false);
			GoToStep(*Run, 10);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 10:  // BASELINE: a player who has already picked, which is who walks onto the pad
		{
			// *** THIS STEP EXISTS BECAUSE ITS ABSENCE MADE THE NEXT TWO VACUOUS. ***
			//
			// Measured, in Saved/Logs/practice-verify-range.log: the range sets CharacterSelectTimeout
			// to 0 (no auto-assign — nothing is stalling, so nagging a player reading the cards would be
			// wrong), the harness runs early as the etiquette requires, and the local player therefore
			// still had NO character and an OPEN select screen. ReopenCharacterSelect refused, correctly
			// and silently, because a player who is still choosing cannot be freed to choose — and the
			// screen-is-open assertion that followed then passed on a screen the range had never
			// touched. A green for the wrong reason.
			//
			// So: put the player in the state a switch is actually about — holding a character, locked,
			// screen closed — through the shipped setter. This is the same baseline
			// PlantLeakBaseline() establishes for the leak test, and for the same reason.
			ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
			UTraceAbilityComponent* const Abilities = UTraceAbilityComponent::Get(TraceState);
			if (TraceState == nullptr || Abilities == nullptr)
			{
				Run->Tally.Report(false, TEXT("PRACTICE"),
					TEXT("the local player has a player state and an ability component."));
				GoToStep(*Run, 90);
				return true;
			}

			if (!UTraceAbilityComponent::AreCharactersEnabled(WorldPtr))
			{
				Run->Tally.Report(false, TEXT("PRACTICE"),
					TEXT("characters are enabled for this session, so a switch is a thing that exists."));
				GoToStep(*Run, 90);
				return true;
			}

			if (Abilities->GetCharacterId() == ETraceCharacterId::None)
			{
				Abilities->ServerSetCharacter(static_cast<ETraceCharacterId>(TraceCharacterRoster::FirstId));
			}
			TraceState->ServerMarkCharacterResolved(/*bLocked=*/true, /*bWasChosen=*/true);
			TraceState->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);

			Run->CharacterBefore = Abilities->GetCharacterId();

			// Somebody OTHER than who they are now, so "they switched" cannot be satisfied by nothing
			// happening. The roster is FirstId..LastId with no gaps.
			const uint8 BeforeId = static_cast<uint8>(Run->CharacterBefore);
			const uint8 TargetId = (BeforeId == TraceCharacterRoster::FirstId)
				? static_cast<uint8>(TraceCharacterRoster::FirstId + 1)
				: TraceCharacterRoster::FirstId;
			Run->SwitchTarget = static_cast<ETraceCharacterId>(TargetId);

			Run->Tally.Report(Run->CharacterBefore != ETraceCharacterId::None, TEXT("PRACTICE"),
				*FString::Printf(TEXT("baseline: the player is locked to %s with the select screen shut "
				                      "— the state a switch is actually about."),
					TraceCharacterIdToString(Run->CharacterBefore)));

			if (const ATraceCharacter* SwitchPawn = LocalPawn(WorldPtr))
			{
				Run->PlayerLocationAtSwitch = SwitchPawn->GetActorLocation();
			}

			GoToStep(*Run, 11);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 11:  // the CHANGE CHARACTER affordance is accepted
		{
			ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
			if (TraceState == nullptr)
			{
				Run->Tally.Report(false, TEXT("PRACTICE"), TEXT("the local player has a player state."));
				GoToStep(*Run, 90);
				return true;
			}

			Run->Tally.Report(Range->ReopenCharacterSelect(TraceState), TEXT("PRACTICE"),
				TEXT("the range accepted a character switch from a player who was locked in."));
			GoToStep(*Run, 12);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 12:  // ...the shipped screen reopens, and a DIFFERENT character is picked on it
		{
			// ATraceGameMode::PollCharacterSelect runs at 4 Hz, so give it several passes.
			if (SinceStep < FRangeRun::ShortWait * 2.0)
			{
				return true;
			}

			ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
			const bool bScreenOpen = (TraceState != nullptr) && TraceState->IsCharacterSelectOpen();
			const bool bUnlocked = (TraceState != nullptr) && !TraceState->bCharacterLocked;

			Run->Tally.Report(bScreenOpen && bUnlocked, TEXT("PRACTICE"),
				TEXT("the SHIPPED character select screen reopened, unlocked, without leaving the "
				     "range — the pick then goes through ATraceGameMode::RequestCharacter as usual."));

			if (!bScreenOpen || !bUnlocked)
			{
				GoToStep(*Run, 90);
				return true;
			}

			// THE REAL PICK, through the real RPC the select screen sends. Not ServerSetCharacter:
			// that is the writer, and going straight to it would skip ATraceGameMode::RequestCharacter,
			// which is where the per-team uniqueness rule and the re-lock live. Calling a Server RPC on
			// the server runs it immediately and locally, so this is the screen's exact path.
			TraceState->ServerRequestCharacter(static_cast<uint8>(Run->SwitchTarget));

			GoToStep(*Run, 13);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 13:  // ...and they are now that character, still standing in the range
		{
			if (SinceStep < FRangeRun::ShortWait)
			{
				return true;
			}

			ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
			const UTraceAbilityComponent* const Abilities = UTraceAbilityComponent::Get(TraceState);
			const ETraceCharacterId CharacterNow = (Abilities != nullptr)
				? Abilities->GetCharacterId() : ETraceCharacterId::None;

			Run->Tally.Report(CharacterNow == Run->SwitchTarget && CharacterNow != Run->CharacterBefore,
				TEXT("PRACTICE"),
				*FString::Printf(TEXT("the player CHANGED CHARACTER: %s -> %s (asked for %s)."),
					TraceCharacterIdToString(Run->CharacterBefore), TraceCharacterIdToString(CharacterNow),
					TraceCharacterIdToString(Run->SwitchTarget)));

			// "WITHOUT LEAVING THE RANGE", measured rather than assumed: same world, gate still open,
			// the furniture still standing, and the player still on the spot they switched from. A
			// switch that worked by travelling to a fresh map would fail every one of these.
			int32 PadCount = 0;
			int32 DummyCount = 0;
			CountRangeFurniture(WorldPtr, PadCount, DummyCount);

			const ATraceCharacter* const SwitchedPawn = LocalPawn(WorldPtr);
			const double MovedDuringSwitch = (SwitchedPawn != nullptr)
				? FVector::Dist(SwitchedPawn->GetActorLocation(), Run->PlayerLocationAtSwitch) : 1e9;

			Run->Tally.Report(TracePracticeRange::IsActive(WorldPtr) && Range->IsBuilt()
				&& PadCount >= 3 && DummyCount > 0 && MovedDuringSwitch < 1500.0,
				TEXT("PRACTICE"),
				*FString::Printf(TEXT("...WITHOUT LEAVING THE RANGE: same world, gate still open, %d pads "
				                      "and %d targets still standing, player %.0f uu from where they "
				                      "switched."), PadCount, DummyCount, MovedDuringSwitch));

			GoToStep(*Run, 90);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		default:  // verdict
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("--------------------------------------------------------------------------------"));
			if (Run->Tally.Failed == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[PRACTICE] VERDICT: *** PASS *** — %d/%d. Targets stand still, take damage and "
					     "come back where they fell; the Core can be left on the rack; abilities can be "
					     "made free; the character can be changed without leaving."),
					Run->Tally.Passed, Run->Tally.Passed);
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[PRACTICE] VERDICT: *** FAIL *** — %d of %d assertions failed."),
					Run->Tally.Failed, Run->Tally.Failed + Run->Tally.Passed);
			}

			GRangeRun.Reset();
			return false;
		}
		}
	}

	// =============================================================================================
	// Trace.Practice.KnifeTest — "KNIFE BACKSTABS DON'T WORK" (spec v21 demo 19, item 3)
	// =============================================================================================
	//
	// WHY THIS EXISTS WHEN Trace.Knife.AngleTest ALREADY PASSES. AngleTest sweeps the PURE PREDICATE
	// TraceMelee::IsBackstab around a hypothetical victim at the origin and checks the arithmetic. It
	// has never once applied damage, and it cannot: it builds no attacker, swings no blade and never
	// reaches ResolveSwing, ServerSwing or UTraceHealthComponent::ApplyDamage. The same is true of
	// Trace.TestKnife's "DAMAGE PROBE", which reports the damage a swing *would* score by calling
	// IsBackstab itself. Both of them can be green on a build where nobody has ever taken 100 damage
	// from behind, which is exactly the shape of the two harnesses this project has already been
	// burned by.
	//
	// So this one measures the ONE number the user can see: A DUMMY'S HEALTH BAR. It stands the local
	// player behind a target, presses the shipped input path (RequestEquip -> StartSwing -> TickSwing
	// -> ServerSwing), and reads the health that came off. Then it does the same from the front. If
	// those two numbers are equal the harness is not measuring its own rule and says so.
	//
	// RED ARM: Trace.Practice.KnifeArm 1 makes the "back" stab be delivered from DIRECTLY IN FRONT of
	// the target while every assertion still asks for a back-stab. All the back-stab assertions must
	// then FAIL. An arm that leaves the verdict green means this harness cannot tell back from front.

	/** How far from the target's capsule centre the harness stands to stab, in uu. */
	constexpr float KnifeStandOffUU = 110.f;

	/** Real seconds allowed for the blade to resolve and the damage to land. Windup is 0.1 s. */
	constexpr double KnifeResolveWait = 0.45;

	/** Real seconds between the two stabs. The swing cooldown is 0.5 s and is stamped at the press. */
	constexpr double KnifeCooldownWait = 0.85;

	TAutoConsoleVariable<int32> CVarPracticeKnifeArm(
		TEXT("Trace.Practice.KnifeArm"),
		0,
		TEXT("DEV ONLY. RED ARM for Trace.Practice.KnifeTest. 1 delivers the 'back' stab from directly "
		     "IN FRONT of the target while the assertions still ask for a back-stab, so every back-stab "
		     "assertion must FAIL. If they pass, the harness cannot tell back from front and its green "
		     "arm proves nothing."),
		ECVF_Cheat);

	struct FKnifeRun
	{
		TWeakObjectPtr<UWorld> WorldPtr;
		FPracticeTally Tally;

		int32 Step = 0;
		double StepStartRealTime = 0.0;

		/** The two victims. A back-stab is meant to be lethal, so the front stab needs its own body. */
		TWeakObjectPtr<ATraceCharacter> BackVictim;
		TWeakObjectPtr<ATraceCharacter> FrontVictim;

		/** Where the player was before the harness walked them into stabbing range. */
		FVector PlayerHome = FVector::ZeroVector;
		FRotator PlayerHomeRotation = FRotator::ZeroRotator;

		float HealthBefore = 0.f;

		/** Health that actually came off, per approach. -1 until measured. */
		float BackDamage = -1.f;
		float FrontDamage = -1.f;
		bool bBackKilled = false;
		bool bFrontKilled = false;

		/** What the pure predicate said about the spot the harness stood on. */
		double BackPredicateAngle = -1.0;
		double FrontPredicateAngle = -1.0;
		bool bBackPredicate = false;
		bool bFrontPredicate = false;

		/** The victim's facing at the moment of the back stab, both ways of asking. */
		float VictimActorYaw = 0.f;
		float VictimRingYaw = 0.f;
		bool bVictimRingHadHistory = false;

		/** Refusals, so "the blade never swung" is distinguishable from "it swung and did 30". */
		bool bBackSwingAccepted = false;
		bool bFrontSwingAccepted = false;
		bool bKnifeEquipped = false;

		static constexpr double ReadyWaitSeconds = 25.0;
	};

	TSharedPtr<FKnifeRun> GKnifeRun;

	bool TickKnifeRun(float /*Unused*/);

	/** Every living ATraceCharacter that is not @p Exclude, nearest first is not required. */
	void CollectVictims(UWorld* WorldPtr, const ATraceCharacter* Exclude, TArray<ATraceCharacter*>& Out)
	{
		Out.Reset();
		if (WorldPtr == nullptr)
		{
			return;
		}
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* const Candidate = *It;
			if (Candidate == nullptr || Candidate == Exclude || !Candidate->IsAlive())
			{
				continue;
			}
			if (Candidate->Health == nullptr || Candidate->IsCarrier())
			{
				continue;   // a carrier is immune to melee BY DESIGN — see TraceMelee.h
			}
			Out.Add(Candidate);
		}
	}

	/**
	 * Stands @p Attacker at @p StandOff uu from @p Victim on the side asked for, looking at it.
	 *
	 * bBehind chooses the side by the VICTIM'S OWN FORWARD, which is the only thing the back/front
	 * rule reads: behind means "on the far side from where the victim is looking", so the approach
	 * vector attacker->victim agrees with the victim's forward. See TraceMelee::IsBackstab.
	 */
	void StandToStab(ATraceCharacter& Attacker, const ATraceCharacter& Victim, bool bBehind)
	{
		const FVector VictimLocation = Victim.GetActorLocation();
		const FVector VictimForward = FRotator(0.f, Victim.GetActorRotation().Yaw, 0.f).Vector();
		const FVector Offset = bBehind ? -VictimForward : VictimForward;

		const FVector StandPoint = VictimLocation + Offset * KnifeStandOffUU;
		Attacker.TeleportTo(StandPoint, Attacker.GetActorRotation(), /*bIsATest=*/false, /*bNoCheck=*/true);

		// Aim at the victim's capsule centre from the eye, so the swept arc really crosses the body
		// rather than passing over its head. The back/front verdict is PLANAR and cannot see this
		// pitch — which is the point: the harness must not be able to buy a verdict with its aim.
		const FRotator LookAt = (VictimLocation - Attacker.GetPawnViewLocation()).Rotation();
		if (AController* Ctrl = Attacker.GetController())
		{
			Ctrl->SetControlRotation(LookAt);
		}
		Attacker.SetActorRotation(FRotator(0.f, LookAt.Yaw, 0.f));
	}

	void RunKnifeTest()
	{
		UWorld* const WorldPtr = FindPracticeVerifyWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[KNIFEBACK] no game world; nothing to test."));
			return;
		}

		UnpauseFor(WorldPtr);

		UE_LOG(LogTraceGame, Display,
			TEXT("================================================================================"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[KNIFEBACK] DEMO 19 item 3: \"Knife backstabs don't work\". Measured as HEALTH TAKEN "
			     "OFF A BODY through the shipped input path, not as an angle."));
		UE_LOG(LogTraceGame, Display,
			TEXT("[KNIFEBACK] settings in force: back %.0f dmg inside +/-%.0fdeg of the rear axis, "
			     "front %.0f dmg, reach %.0fuu, arc %.0fdeg. Red arm Trace.Practice.KnifeArm is %s."),
			TraceMelee::GetBackstabDamage(), TraceMelee::GetBackstabHalfAngleDegrees(),
			TraceMelee::GetFrontDamage(), TraceMelee::GetSwingRangeUU(), TraceMelee::GetSwingArcDegrees(),
			CVarPracticeKnifeArm.GetValueOnAnyThread() != 0 ? TEXT("*** ON ***") : TEXT("off"));

		TSharedPtr<FKnifeRun> Run = MakeShared<FKnifeRun>();
		Run->WorldPtr = WorldPtr;
		Run->StepStartRealTime = FPlatformTime::Seconds();

		GKnifeRun = Run;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickKnifeRun), 0.f);
	}

	void GoToKnifeStep(FKnifeRun& Run, int32 NextStep)
	{
		Run.Step = NextStep;
		Run.StepStartRealTime = FPlatformTime::Seconds();
	}

	bool TickKnifeRun(float /*Unused*/)
	{
		TSharedPtr<FKnifeRun> Run = GKnifeRun;
		if (!Run.IsValid())
		{
			return false;
		}

		UWorld* const WorldPtr = Run->WorldPtr.Get();
		if (WorldPtr == nullptr)
		{
			GKnifeRun.Reset();
			return false;
		}

		const double SinceStep = FPlatformTime::Seconds() - Run->StepStartRealTime;
		ATraceCharacter* const PlayerPawn = LocalPawn(WorldPtr);
		UTraceWeaponComponent* const Weapon = (PlayerPawn != nullptr)
			? PlayerPawn->FindComponentByClass<UTraceWeaponComponent>() : nullptr;

		switch (Run->Step)
		{
		// -----------------------------------------------------------------------------------------
		case 0:   // two bodies and a player holding a knife
		{
			TArray<ATraceCharacter*> Victims;
			CollectVictims(WorldPtr, PlayerPawn, Victims);

			const bool bReady = (PlayerPawn != nullptr) && PlayerPawn->IsAlive()
				&& (Weapon != nullptr) && Victims.Num() >= 2;
			if (!bReady && SinceStep < FKnifeRun::ReadyWaitSeconds)
			{
				return true;
			}

			Run->Tally.Report(PlayerPawn != nullptr && Weapon != nullptr, TEXT("KNIFEBACK"),
				TEXT("there is a local pawn with a weapon component to swing."));
			Run->Tally.Report(Victims.Num() >= 2, TEXT("KNIFEBACK"),
				*FString::Printf(TEXT("there are at least two stabbable bodies (found %d). A back-stab is "
				                      "meant to be lethal, so the front stab needs its own."), Victims.Num()));

			if (PlayerPawn == nullptr || Weapon == nullptr || Victims.Num() < 2)
			{
				GoToKnifeStep(*Run, 90);
				return true;
			}

			Run->BackVictim = Victims[0];
			Run->FrontVictim = Victims[1];
			Run->PlayerHome = PlayerPawn->GetActorLocation();
			Run->PlayerHomeRotation = PlayerPawn->GetActorRotation();

			// The shipped swap, the same call the swap bind makes. It costs the 0.2 s pullout, which
			// the next step waits out through the shipped IsDeploying() rather than a private timer.
			Weapon->RequestEquip(ETraceEquippedWeapon::Knife);
			GoToKnifeStep(*Run, 1);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 1:   // stand behind the first body, knife out
		{
			ATraceCharacter* const Victim = Run->BackVictim.Get();
			if (PlayerPawn == nullptr || Weapon == nullptr || Victim == nullptr || Victim->Health == nullptr)
			{
				Run->Tally.Report(false, TEXT("KNIFEBACK"), TEXT("the target survived long enough to be stabbed."));
				GoToKnifeStep(*Run, 90);
				return true;
			}

			if (Weapon->IsDeploying() && SinceStep < 1.5)
			{
				return true;
			}

			// *** SPEC v28 §10 INTEGRATION — ASK "IS THERE A BLADE IN HAND", NOT "IS THE SELECTOR ON
			// THE KNIFE". *** This read Weapon->IsKnifeEquipped(), i.e. the weapon SELECTOR. Under
			// §10's dual wield the knife is permanently in the off hand and the selector never sits
			// on it at all, so RequestEquip(Knife) above is a successful no-op and IsKnifeEquipped()
			// is correctly false - this assertion would have failed a build behaving exactly as §10
			// specifies. TraceMelee::IsKnifeInHand() is the question this step actually means ("is
			// there a blade available to swing") and it is TRUE under BOTH positions of the
			// bDualWieldKnife switch, so this harness measures the backstab under either.
			//
			// The §10 owner found this and reported it rather than reaching into another slice's
			// harness; this is that one line. The stab itself, and every assertion below it, is
			// untouched - the swing path does not care which hand holds the blade.
			Run->bKnifeEquipped = TraceMelee::IsKnifeInHand(PlayerPawn);
			Run->Tally.Report(Run->bKnifeEquipped, TEXT("KNIFEBACK"),
				TEXT("the player has the KNIFE IN HAND and can swing it (true whether the knife is the "
				     "selected weapon, as before spec v28 §10, or permanently in the off hand, as after)."));

			// *** THE RED ARM. *** Armed, the "back" stab is delivered from the victim's FACE while
			// every assertion below still asks for a back-stab.
			const bool bArmed = CVarPracticeKnifeArm.GetValueOnAnyThread() != 0;
			StandToStab(*PlayerPawn, *Victim, /*bBehind=*/!bArmed);

			// The pure predicate, asked about the spot the harness is really standing on. This is the
			// cross-check between "the geometry is a back-stab" and "the damage was a back-stab" — and
			// the two disagreeing is the whole diagnosis this command exists to produce.
			Run->VictimActorYaw = static_cast<float>(Victim->GetActorRotation().Yaw);
			Run->VictimRingYaw = Run->VictimActorYaw;
			Run->bVictimRingHadHistory = UTraceWeaponComponent::GetFacingYawAtTime(
				Victim, static_cast<float>(WorldPtr->GetGameState() != nullptr
					? WorldPtr->GetGameState()->GetServerWorldTimeSeconds()
					: WorldPtr->GetTimeSeconds()),
				Run->VictimRingYaw);

			Run->bBackPredicate = TraceMelee::IsBackstab(PlayerPawn->GetMuzzleLocation(),
				Victim->GetActorLocation(), Run->VictimActorYaw, &Run->BackPredicateAngle);

			UE_LOG(LogTraceGame, Display,
				TEXT("[KNIFEBACK] stand %s: attacker %s, victim %s, victim yaw %.1f (ring %.1f, history=%s), "
				     "approach %.1fdeg, predicate says %s, gap %.0fuu."),
				bArmed ? TEXT("IN FRONT (RED ARM)") : TEXT("BEHIND"),
				*PlayerPawn->GetActorLocation().ToCompactString(),
				*Victim->GetActorLocation().ToCompactString(),
				Run->VictimActorYaw, Run->VictimRingYaw,
				Run->bVictimRingHadHistory ? TEXT("yes") : TEXT("no"),
				Run->BackPredicateAngle, Run->bBackPredicate ? TEXT("BACKSTAB") : TEXT("front"),
				FVector::Dist(PlayerPawn->GetActorLocation(), Victim->GetActorLocation()));

			Run->HealthBefore = Victim->Health->Health;
			GoToKnifeStep(*Run, 2);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 2:   // swing
		{
			ATraceCharacter* const Victim = Run->BackVictim.Get();
			if (Weapon == nullptr || Victim == nullptr)
			{
				GoToKnifeStep(*Run, 90);
				return true;
			}

			// One frame of settle after the teleport before the press, so the movement component has
			// published the new location into the lag-compensation history the server rewinds into.
			if (SinceStep < 0.15)
			{
				return true;
			}

			ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
			Run->bBackSwingAccepted = Weapon->StartSwing(&Refusal);
			Run->Tally.Report(Run->bBackSwingAccepted, TEXT("KNIFEBACK"),
				*FString::Printf(TEXT("the swing was accepted (refusal code %d)."), static_cast<int32>(Refusal)));

			GoToKnifeStep(*Run, 3);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 3:   // *** THE ASSERTION THE USER WOULD MAKE: it took a hundred off ***
		{
			if (SinceStep < KnifeResolveWait)
			{
				return true;
			}

			ATraceCharacter* const Victim = Run->BackVictim.Get();
			const bool bVictimGone = (Victim == nullptr) || !Victim->IsAlive();
			const float HealthAfter = (Victim != nullptr && Victim->Health != nullptr)
				? Victim->Health->Health : 0.f;

			Run->bBackKilled = bVictimGone || HealthAfter <= 0.f;
			Run->BackDamage = Run->bBackKilled
				? Run->HealthBefore                        // clamped at zero; it took everything it could
				: (Run->HealthBefore - HealthAfter);

			UE_LOG(LogTraceGame, Display,
				TEXT("[KNIFEBACK] BACK stab: %.0f -> %.0f health, %.0f damage, %s."),
				Run->HealthBefore, HealthAfter, Run->BackDamage,
				Run->bBackKilled ? TEXT("KILLED") : TEXT("survived"));

			GoToKnifeStep(*Run, 4);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 4:   // the same swing from the front, on a fresh body
		{
			ATraceCharacter* const Victim = Run->FrontVictim.Get();
			if (PlayerPawn == nullptr || Weapon == nullptr || Victim == nullptr || Victim->Health == nullptr)
			{
				Run->Tally.Report(false, TEXT("KNIFEBACK"),
					TEXT("a second body survived to take the FRONT stab (without it, back and front "
					     "cannot be compared and this run proves nothing)."));
				GoToKnifeStep(*Run, 90);
				return true;
			}

			if (SinceStep < KnifeCooldownWait)
			{
				return true;
			}

			StandToStab(*PlayerPawn, *Victim, /*bBehind=*/false);

			Run->bFrontPredicate = TraceMelee::IsBackstab(PlayerPawn->GetMuzzleLocation(),
				Victim->GetActorLocation(), static_cast<float>(Victim->GetActorRotation().Yaw),
				&Run->FrontPredicateAngle);

			UE_LOG(LogTraceGame, Display,
				TEXT("[KNIFEBACK] stand IN FRONT: approach %.1fdeg, predicate says %s."),
				Run->FrontPredicateAngle, Run->bFrontPredicate ? TEXT("BACKSTAB") : TEXT("front"));

			Run->HealthBefore = Victim->Health->Health;
			GoToKnifeStep(*Run, 5);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 5:
		{
			if (SinceStep < 0.15)
			{
				return true;
			}

			ETraceMeleeRefusal Refusal = ETraceMeleeRefusal::None;
			Run->bFrontSwingAccepted = (Weapon != nullptr) && Weapon->StartSwing(&Refusal);
			Run->Tally.Report(Run->bFrontSwingAccepted, TEXT("KNIFEBACK"),
				*FString::Printf(TEXT("the FRONT swing was accepted (refusal code %d)."), static_cast<int32>(Refusal)));

			GoToKnifeStep(*Run, 6);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 6:
		{
			if (SinceStep < KnifeResolveWait)
			{
				return true;
			}

			ATraceCharacter* const Victim = Run->FrontVictim.Get();
			const bool bVictimGone = (Victim == nullptr) || !Victim->IsAlive();
			const float HealthAfter = (Victim != nullptr && Victim->Health != nullptr)
				? Victim->Health->Health : 0.f;

			Run->bFrontKilled = bVictimGone || HealthAfter <= 0.f;
			Run->FrontDamage = Run->bFrontKilled ? Run->HealthBefore : (Run->HealthBefore - HealthAfter);

			UE_LOG(LogTraceGame, Display,
				TEXT("[KNIFEBACK] FRONT stab: %.0f -> %.0f health, %.0f damage, %s."),
				Run->HealthBefore, HealthAfter, Run->FrontDamage,
				Run->bFrontKilled ? TEXT("KILLED") : TEXT("survived"));

			// Put the player back where they were standing; a harness must not leave the range
			// arranged around itself.
			if (PlayerPawn != nullptr)
			{
				PlayerPawn->TeleportTo(Run->PlayerHome, Run->PlayerHomeRotation,
					/*bIsATest=*/false, /*bNoCheck=*/true);
			}

			GoToKnifeStep(*Run, 7);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 7:   // judgement
		{
			const float WantBack = TraceMelee::GetBackstabDamage();
			const float WantFront = TraceMelee::GetFrontDamage();

			// A. the geometry really was a back-stab. If this fails the harness stood in the wrong
			//    place and every number below it is about something else.
			Run->Tally.Report(Run->bBackPredicate, TEXT("KNIFEBACK"),
				*FString::Printf(TEXT("the spot the harness stabbed from IS behind the target: approach "
				                      "%.1fdeg, threshold %.0fdeg."),
					Run->BackPredicateAngle, TraceMelee::GetBackstabHalfAngleDegrees()));

			// B. *** THE USER'S COMPLAINT. ***
			Run->Tally.Report(FMath::IsNearlyEqual(Run->BackDamage, WantBack, 0.51f), TEXT("KNIFEBACK"),
				*FString::Printf(TEXT("*** A STAB FROM BEHIND TOOK %.0f DAMAGE (want %.0f). *** This is "
				                      "the number \"backstabs don't work\" is about."),
					Run->BackDamage, WantBack));

			Run->Tally.Report(Run->bBackKilled, TEXT("KNIFEBACK"),
				*FString::Printf(TEXT("...and it KILLED a %.0f-health target in one hit."), WantBack));

			// C. the front stab is the control. A harness whose two arms give the same number is not
			//    measuring its rule — this project's house rule, applied to itself.
			Run->Tally.Report(FMath::IsNearlyEqual(Run->FrontDamage, WantFront, 0.51f), TEXT("KNIFEBACK"),
				*FString::Printf(TEXT("a stab from the FRONT took %.0f damage (want %.0f)."),
					Run->FrontDamage, WantFront));

			Run->Tally.Report(!FMath::IsNearlyEqual(Run->BackDamage, Run->FrontDamage, 0.51f), TEXT("KNIFEBACK"),
				*FString::Printf(TEXT("back (%.0f) and front (%.0f) are DIFFERENT numbers — the harness "
				                      "can tell the two approaches apart."),
					Run->BackDamage, Run->FrontDamage));

			const bool bArmed = CVarPracticeKnifeArm.GetValueOnAnyThread() != 0;
			const bool bAllGreen = (Run->Tally.Failed == 0);

			UE_LOG(LogTraceGame, Display,
				TEXT("--------------------------------------------------------------------------------"));
			if (bArmed)
			{
				if (bAllGreen)
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("[KNIFEBACK] RED ARM VERDICT: *** BROKEN HARNESS *** — Trace.Practice.KnifeArm "
						     "is ON, so the \"back\" stab was delivered to the target's FACE, and every "
						     "assertion still passed. This harness cannot tell back from front."));
				}
				else
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[KNIFEBACK] RED ARM VERDICT: correct — %d/%d assertions FAILED with the stab "
						     "moved to the target's face. The harness can see the difference, so its green "
						     "arm is evidence. Set Trace.Practice.KnifeArm 0 and run again."),
						Run->Tally.Failed, Run->Tally.Failed + Run->Tally.Passed);
				}
			}
			else if (bAllGreen)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[KNIFEBACK] VERDICT: *** PASS *** — %d/%d. A stab from behind takes %.0f and "
					     "kills; the same stab from the front takes %.0f."),
					Run->Tally.Passed, Run->Tally.Passed, Run->BackDamage, Run->FrontDamage);
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[KNIFEBACK] VERDICT: *** FAIL *** — %d of %d. back=%.0f front=%.0f "
					     "(want %.0f / %.0f), predicate said %s."),
					Run->Tally.Failed, Run->Tally.Failed + Run->Tally.Passed,
					Run->BackDamage, Run->FrontDamage, WantBack, WantFront,
					Run->bBackPredicate ? TEXT("BACKSTAB") : TEXT("front"));
			}

			GKnifeRun.Reset();
			return false;
		}

		// -----------------------------------------------------------------------------------------
		default:
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[KNIFEBACK] VERDICT: *** INVALID *** — the run could not be set up, so nothing was "
				     "measured. %d of %d assertions failed on the way. This is NOT a pass."),
				Run->Tally.Failed, Run->Tally.Failed + Run->Tally.Passed);
			GKnifeRun.Reset();
			return false;
		}
		}
	}

	// =============================================================================================
	// Trace.Practice.BotAttackTest — "DON'T LET THE BOTS ATTACK" (spec v21 demo 19, item 2)
	// =============================================================================================
	//
	// WHAT THE USER SAW, and therefore what this measures. The range's five targets were arming
	// themselves and swinging: UTraceWeaponComponent::TickBotKnife runs on the server for every pawn
	// whose controller is not an APlayerController and asks that controller nothing, so a target drew
	// the blade at 500 uu and swung it at 153 uu. A dummy is on ETraceTeam::None, and TickBotKnife's
	// teammate skip is guarded by `MyTeam != ETraceTeam::None` — so it never applies to a dummy and
	// EVERY other body in the world is an enemy to it, the other four targets included.
	//
	// So there are two independent signals here and the harness reads both:
	//   1. HOW MANY TARGETS HAVE A KNIFE OUT. The row is 260 uu wide-spaced, inside the 500 uu engage
	//      range, so an armed row draws on ITSELF even with no player nearby. This is the signal that
	//      does not depend on the harness standing anywhere in particular.
	//   2. HEALTH OFF THE PLAYER, standing inside knifing distance of the middle target. This is the
	//      user's actual complaint — "the bots attack ME" — and it is the number they can see.
	//
	// RED ARM: Trace.Practice.BotArm 1 puts the autonomy bit BACK on every target and the assertions
	// are unchanged, so they must all FAIL. An arm that leaves the verdict green would mean the
	// harness cannot see a bot attacking and its green arm proves nothing — which is exactly the
	// failure mode this project has shipped twice.

	/** How far from the target's capsule centre the harness parks the player, in uu. */
	constexpr float BotAttackStandOffUU = 110.f;

	/**
	 * Real seconds the player stands there being a target of opportunity.
	 *
	 * TickBotKnife decides 4x a second, the pullout is 0.2 s and the swing cooldown 0.5 s, so this is
	 * roughly a dozen decisions and several complete swings — long enough that "nothing happened"
	 * means the rule held rather than that the clock ran out.
	 */
	constexpr double BotAttackWatchSeconds = 3.5;

	TAutoConsoleVariable<int32> CVarPracticeBotArm(
		TEXT("Trace.Practice.BotArm"),
		0,
		TEXT("DEV ONLY. RED ARM for Trace.Practice.BotAttackTest. 1 puts the autonomous-attack bit back "
		     "on every practice target while the assertions still demand that no target attacks, so "
		     "every one of them must FAIL. If they pass, the harness cannot see a bot attacking."),
		ECVF_Cheat);

	struct FBotAttackRun
	{
		TWeakObjectPtr<UWorld> WorldPtr;
		FPracticeTally Tally;

		int32 Step = 0;
		double StepStartRealTime = 0.0;

		/** The target the player is parked next to. */
		TWeakObjectPtr<ATraceCharacter> NearTarget;

		float PlayerHealthBefore = 0.f;

		/** Peak count of targets seen holding a knife at any one sample. */
		int32 PeakArmedTargets = 0;

		/** Targets present when the watch began, so "they all died" is distinguishable from "quiet". */
		int32 TargetsWatched = 0;

		bool bArmed = false;

		static constexpr double ReadyWaitSeconds = 25.0;
	};

	TSharedPtr<FBotAttackRun> GBotAttackRun;

	bool TickBotAttackRun(float /*Unused*/);

	/** Every living range target, i.e. an ATraceCharacter possessed by a dummy controller. */
	void CollectRangeTargets(UWorld* WorldPtr, TArray<ATraceCharacter*>& Out)
	{
		Out.Reset();
		if (WorldPtr == nullptr)
		{
			return;
		}
		for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
		{
			ATraceCharacter* const Candidate = *It;
			if (Candidate == nullptr || !Candidate->IsAlive())
			{
				continue;
			}
			if (Cast<ATracePracticeDummyController>(Candidate->GetController()) != nullptr)
			{
				Out.Add(Candidate);
			}
		}
	}

	/** How many of @p Targets are holding the knife right now. */
	int32 CountArmedTargets(const TArray<ATraceCharacter*>& Targets)
	{
		int32 Count = 0;
		for (const ATraceCharacter* const EachTarget : Targets)
		{
			if (TraceMelee::IsKnifeEquipped(EachTarget))
			{
				++Count;
			}
		}
		return Count;
	}

	void RunBotAttackTest()
	{
		UWorld* const WorldPtr = FindPracticeVerifyWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[BOTATTACK] no game world; nothing to test."));
			return;
		}

		UnpauseFor(WorldPtr);

		UE_LOG(LogTraceGame, Display,
			TEXT("================================================================================"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[BOTATTACK] DEMO 19 item 2: \"Don't let the bots attack\". Measured as KNIVES DRAWN by "
			     "the range's targets and HEALTH TAKEN OFF THE PLAYER standing inside their reach."));
		UE_LOG(LogTraceGame, Display,
			TEXT("[BOTATTACK] bot engage range %.0fuu, swing reach %.0fuu x %.2f, targets spaced 260uu. "
			     "Red arm Trace.Practice.BotArm is %s."),
			TraceMelee::GetBotEngageRangeUU(), TraceMelee::GetSwingRangeUU(),
			TraceMelee::GetBotSwingRangeFraction(),
			CVarPracticeBotArm.GetValueOnAnyThread() != 0 ? TEXT("*** ON ***") : TEXT("off"));

		TSharedPtr<FBotAttackRun> Run = MakeShared<FBotAttackRun>();
		Run->WorldPtr = WorldPtr;
		Run->StepStartRealTime = FPlatformTime::Seconds();

		GBotAttackRun = Run;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickBotAttackRun), 0.f);
	}

	void GoToBotAttackStep(FBotAttackRun& Run, int32 NextStep)
	{
		Run.Step = NextStep;
		Run.StepStartRealTime = FPlatformTime::Seconds();
	}

	bool TickBotAttackRun(float /*Unused*/)
	{
		TSharedPtr<FBotAttackRun> Run = GBotAttackRun;
		if (!Run.IsValid())
		{
			return false;
		}

		UWorld* const WorldPtr = Run->WorldPtr.Get();
		if (WorldPtr == nullptr)
		{
			GBotAttackRun.Reset();
			return false;
		}

		const double SinceStep = FPlatformTime::Seconds() - Run->StepStartRealTime;

		APlayerController* const PC = WorldPtr->GetFirstPlayerController();
		ATraceCharacter* const PlayerPawn = (PC != nullptr) ? Cast<ATraceCharacter>(PC->GetPawn()) : nullptr;

		TArray<ATraceCharacter*> Targets;
		CollectRangeTargets(WorldPtr, Targets);

		switch (Run->Step)
		{
		// -----------------------------------------------------------------------------------------
		case 0:   // wait for a pawn and a furnished row, then apply the arm and park the player
		{
			const bool bReady = (PlayerPawn != nullptr) && (PlayerPawn->Health != nullptr) && Targets.Num() >= 2;
			if (!bReady && SinceStep < FBotAttackRun::ReadyWaitSeconds)
			{
				return true;
			}

			Run->Tally.Report(PlayerPawn != nullptr && PlayerPawn->Health != nullptr, TEXT("BOTATTACK"),
				TEXT("there is a local pawn with health to lose."));
			Run->Tally.Report(Targets.Num() >= 2, TEXT("BOTATTACK"),
				*FString::Printf(TEXT("the target row is furnished (found %d). An armed row draws on "
				                      "ITSELF, which is the signal that needs no player."), Targets.Num()));

			if (PlayerPawn == nullptr || PlayerPawn->Health == nullptr || Targets.Num() < 2)
			{
				GoToBotAttackStep(*Run, 90);
				return true;
			}

			// *** THE RED ARM, AND ONLY THE RED ARM, TOUCHES THE BIT. ***
			//
			// Armed, every target gets its autonomy back and the assertions below are unchanged, so
			// they must all fail. UNARMED, THIS LOOP DOES NOT RUN: the green arm has to observe what
			// ATracePracticeDummyController::OnPossess actually left on these pawns, because that is
			// the fix under test. A harness that re-applied the fix and then checked it would be
			// measuring its own setup call — which is the shape of the two harnesses this project has
			// already shipped and had to throw away.
			Run->bArmed = CVarPracticeBotArm.GetValueOnAnyThread() != 0;
			if (Run->bArmed)
			{
				for (ATraceCharacter* const EachTarget : Targets)
				{
					if (UTraceWeaponComponent* TargetWeapon = EachTarget->FindComponentByClass<UTraceWeaponComponent>())
					{
						TargetWeapon->SetAutonomousAttacksAllowed(true);
					}
				}
			}

			// Park the player inside knifing reach of the middle target, in its FACE — the front is
			// where a bot that is attacking you would be met, and it keeps the 30-damage front stab
			// (not the 100 back-stab) as the thing being detected, so a survivable hit still registers.
			ATraceCharacter* const Near = Targets[Targets.Num() / 2];
			Run->NearTarget = Near;
			const FVector TargetForward = FRotator(0.f, Near->GetActorRotation().Yaw, 0.f).Vector();
			PlayerPawn->TeleportTo(Near->GetActorLocation() + TargetForward * BotAttackStandOffUU,
				PlayerPawn->GetActorRotation(), /*bIsATest=*/false, /*bNoCheck=*/true);

			Run->PlayerHealthBefore = PlayerPawn->Health->Health;
			Run->TargetsWatched = Targets.Num();
			Run->PeakArmedTargets = CountArmedTargets(Targets);

			UE_LOG(LogTraceGame, Display,
				TEXT("[BOTATTACK] player parked %.0fuu from a target (reach is %.0fuu), health %.0f, "
				     "%d targets watched. Arm is %s."),
				BotAttackStandOffUU,
				TraceMelee::GetSwingRangeUU() * TraceMelee::GetBotSwingRangeFraction(),
				Run->PlayerHealthBefore, Run->TargetsWatched,
				Run->bArmed ? TEXT("*** ON ***") : TEXT("off"));

			GoToBotAttackStep(*Run, 1);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 1:   // stand there and watch
		{
			Run->PeakArmedTargets = FMath::Max(Run->PeakArmedTargets, CountArmedTargets(Targets));

			if (SinceStep < BotAttackWatchSeconds)
			{
				return true;
			}

			const float HealthNow = (PlayerPawn != nullptr && PlayerPawn->Health != nullptr)
				? PlayerPawn->Health->Health
				: 0.f;
			const float HealthLost = Run->PlayerHealthBefore - HealthNow;
			const bool bPlayerDied = (PlayerPawn == nullptr) || !PlayerPawn->IsAlive();

			UE_LOG(LogTraceGame, Display,
				TEXT("[BOTATTACK] after %.1fs: peak %d of %d targets holding a knife, player health "
				     "%.0f -> %.0f (%.0f lost)%s."),
				BotAttackWatchSeconds, Run->PeakArmedTargets, Run->TargetsWatched,
				Run->PlayerHealthBefore, HealthNow, HealthLost,
				bPlayerDied ? TEXT(", PLAYER DIED") : TEXT(""));

			Run->Tally.Report(Run->PeakArmedTargets == 0, TEXT("BOTATTACK"),
				*FString::Printf(TEXT("no target ever drew the knife (peak %d of %d holding one)."),
					Run->PeakArmedTargets, Run->TargetsWatched));

			Run->Tally.Report(HealthLost <= 0.f && !bPlayerDied, TEXT("BOTATTACK"),
				*FString::Printf(TEXT("the player standing inside their reach took NOTHING (%.0f health "
				                      "lost). This is the number \"don't let the bots attack\" is about."),
					HealthLost));

			GoToBotAttackStep(*Run, 90);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		default:   // verdict
		{
			// Disarm again on the way out whatever the arm said, so a red run does not leave a live
			// row of knife-fighting targets behind it for the next command to trip over.
			for (ATraceCharacter* const EachTarget : Targets)
			{
				if (UTraceWeaponComponent* TargetWeapon = EachTarget->FindComponentByClass<UTraceWeaponComponent>())
				{
					TargetWeapon->SetAutonomousAttacksAllowed(false);
				}
			}

			const bool bGreen = Run->Tally.Failed == 0;

			if (Run->bArmed)
			{
				// THE ARM'S OWN VERDICT. Armed, the assertions above are supposed to fail.
				if (bGreen)
				{
					UE_LOG(LogTraceGame, Error,
						TEXT("[BOTATTACK] RED ARM VERDICT: *** BROKEN HARNESS *** — Trace.Practice.BotArm "
						     "was ON, every target had its autonomy back, and the harness still reported "
						     "everything green. It cannot see a bot attacking, so its green arm is not "
						     "evidence. Set Trace.Practice.BotArm 0 and run again."));
				}
				else
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[BOTATTACK] RED ARM VERDICT: *** CORRECTLY FAILED *** — %d of %d assertions "
						     "failed with the targets re-armed. The harness can see a bot attacking, so a "
						     "green run with the arm off means something."),
						Run->Tally.Failed, Run->Tally.Passed + Run->Tally.Failed);
				}
			}
			else
			{
				UE_LOG(LogTraceGame, Display, TEXT("[BOTATTACK] VERDICT: %s — %d of %d."),
					bGreen ? TEXT("*** PASS ***") : TEXT("*** FAIL ***"),
					Run->Tally.Passed, Run->Tally.Passed + Run->Tally.Failed);
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("================================================================================"));

			GBotAttackRun.Reset();
			return false;
		}
		}
	}

	// =============================================================================================
	// Registration
	// =============================================================================================

	FAutoConsoleCommand CmdPracticeBotAttackTest(
		TEXT("Trace.Practice.BotAttackTest"),
		TEXT("DEMO 19 item 2. Parks the local player inside knifing reach of a practice target and "
		     "watches for 3.5 s: counts targets that draw a knife and health taken off the player. "
		     "RED ARM: Trace.Practice.BotArm 1 gives every target its autonomy back and every "
		     "assertion must FAIL."),
		FConsoleCommandDelegate::CreateStatic(&RunBotAttackTest));

	FAutoConsoleCommand CmdPracticeKnifeTest(
		TEXT("Trace.Practice.KnifeTest"),
		TEXT("DEMO 19 item 3. Stands the local player behind a stationary target, swings the knife "
		     "through the shipped input path and reports the HEALTH THAT CAME OFF; then repeats from "
		     "the front. RED ARM: Trace.Practice.KnifeArm 1 delivers the 'back' stab to the target's "
		     "face and every back-stab assertion must FAIL."),
		FConsoleCommandDelegate::CreateStatic(&RunKnifeTest));

	FAutoConsoleCommand CmdPracticeVerify(
		TEXT("Trace.Practice.Verify"),
		TEXT("SPEC v19 §2. Drives the practice range end to end: a target's damage and its "
		     "respawn-in-place, the Core left on the rack and watched for four settle windows, the "
		     "infinite-abilities toggle against a live cooldown, and the character switch. Run it in "
		     "a real match and it FAILS, which is its red arm."),
		FConsoleCommandDelegate::CreateStatic(&RunRangeVerify));

	FAutoConsoleCommand CmdPracticeLeakTest(
		TEXT("Trace.Practice.LeakTest"),
		TEXT("SPEC v19 §2, THE IMPORTANT ONE. Run in a REAL match: proves infinite abilities, free "
		     "character switching and the no-turnover spot are all absent, by measurement. RED ARM: "
		     "Trace.Practice.LeakArm 1 forces the range's gate open in every world and every "
		     "assertion must then FAIL."),
		FConsoleCommandDelegate::CreateStatic(&RunLeakTest));
}

#endif   // !UE_BUILD_SHIPPING
