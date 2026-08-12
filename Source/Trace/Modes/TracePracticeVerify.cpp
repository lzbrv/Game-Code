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

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterRoster.h"
#include "Core/TraceGameMode.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
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

		switch (Run->Step)
		{
		// -----------------------------------------------------------------------------------------
		case 0:   // the furniture exists at all
		{
			// -ExecCmds fires on the FIRST TICK of the loaded map: the arena has not been built, the
			// player has not logged in and the subsystem's 5 Hz poll has not run once. Judging then
			// would report "the range is empty" about a range that had not been asked to exist yet —
			// so wait for it, and only call it a failure once the wait is genuinely over.
			const bool bFurnished = Range->IsBuilt() && Range->GetDummyCount() > 0
				&& LocalPawn(WorldPtr) != nullptr;
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
	// Registration
	// =============================================================================================

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
