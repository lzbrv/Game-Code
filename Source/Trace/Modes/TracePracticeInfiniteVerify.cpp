// Trace — THE EVIDENCE FOR THE INFINITE-ABILITIES TOGGLE (spec v24 §5).
//
// ===================================================================================================
// THE REPORT, AND WHICH OF THE THREE IT TURNED OUT TO BE
// ===================================================================================================
//
// Verbatim: "Infinite abilities toggle in the practice range doesn't do anything." The spec asks
// which of three it is — never read, read on the wrong side, or read and then overwritten. It is the
// THIRD, and the other two are ruled out by construction rather than by opinion:
//
//   NEVER READ?           No. UTracePracticeRangeSubsystem::PollRange has always consulted
//                         bInfiniteAbilities and called ApplyInfiniteAbilities().
//
//   WRONG SIDE?           No. The subsystem's gate (TracePracticeRange::IsActive) answers true only
//                         where AGameModeBase exists, i.e. the server; and the write it makes,
//                         UTraceAbilityComponent::DebugSetActivatedCooldown, returns immediately
//                         without HasAuthorityOwner(). Cooldowns are server-authoritative here and
//                         the toggle was already on that side. A remote client could never have set
//                         it either — SetInfiniteAbilities is only reachable from a pad overlap that
//                         refuses without authority, or from a console command on the host.
//
//   READ AND OVERWRITTEN? *** YES. *** UTraceAbilityComponent::TryActivate() writes
//                         ActivatedCooldownEndMatchTime at the instant the ability fires and refuses
//                         on it from that instant. The toggle's only enforcement was a sweep on the
//                         range's 200 ms furniture timer, so the ordinary cooldown path overwrote the
//                         cheat on every single press and held it for up to a fifth of a second.
//                         "No cooldowns" was worth at most five activations a second, the HUD ring
//                         lit on every press, and the ability really was refused in between.
//
// THE FIX IS A TICK, AND THE ORDER IS THE POINT. UWorld::Tick runs TG_PrePhysics — where the E press
// is delivered and TryActivate writes the deadline — at LevelTick.cpp:1750, and
// FTickableGameObject::TickObjects at :1821, AFTER it, in the same frame. So the deadline the range
// erases is the one the press just wrote.
//
// ===================================================================================================
// WHY THIS HARNESS CAN GO RED, WHICH IS THE PART THAT MATTERS
// ===================================================================================================
//
//   RED ARM:    Trace.Practice.PollOnlyInfinite 1   then   Trace.Practice.InfiniteVerify
//   GREEN ARM:  Trace.Practice.PollOnlyInfinite 0   then   Trace.Practice.InfiniteVerify
//
// The arm restores the pre-v24 driver EXACTLY — the 5 Hz poll and nothing else — so the red arm is
// the build the owner reported rather than a simulation of it. One binary, both arms, minutes apart.
//
// *** IT PRESSES E FOR REAL. *** Every measurement here goes through TryActivate(), which is
// literally what the key press calls. The harness this replaces planted a cooldown with
// DebugSetActivatedCooldown(9.f) and then asserted that turning the toggle on removed it — but
// SetInfiniteAbilities() itself calls ApplyInfiniteAbilities(), whose whole job is to call
// DebugSetActivatedCooldown(0.f). That assertion undid its own plant in the same call stack and could
// not fail on any build, working or broken. It passed on the build the owner is complaining about.
//
// THE NUMBER THE TWO ARMS DISAGREE ON is "fired on X of N consecutive frames":
//   toggle OFF          1 of N   (one press, then a real 20 s cooldown — the ability IS unavailable)
//   toggle ON, RED      a handful of N (one per 200 ms poll; the rest are refused)
//   toggle ON, GREEN    N of N   (never refused, and the cooldown reads 0.00 s on every frame)

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                        // TActorIterator
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/TraceAbilityTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerState.h"
#include "Modes/TracePracticeActors.h"
#include "Modes/TracePracticeRange.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "World/TraceArenaBuilder.h"        // v24 §0: the endzone depth the footprint is derived from
#include "Trace.h"                              // LogTraceGame

// NAMED AFTER THE FILE, not anonymous — Scripts/check-jumbo-build-collisions.py gates the build on
// it, and this module has already lost a Windows build to two files sharing an anonymous symbol.
namespace TracePracticeInfiniteVerify
{
	// ---------------------------------------------------------------------------------------------
	// Plumbing
	// ---------------------------------------------------------------------------------------------

	UWorld* FindInfiniteVerifyWorld()
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
			if (TracePracticeRange::IsActive(Candidate))
			{
				return Candidate;
			}
			if (Fallback == nullptr)
			{
				Fallback = Candidate;
			}
		}
		return Fallback;
	}

	struct FInfiniteTally
	{
		int32 Passed = 0;
		int32 Failed = 0;

		void Report(bool bCondition, const TCHAR* What)
		{
			if (bCondition)
			{
				++Passed;
				UE_LOG(LogTraceGame, Display, TEXT("[PRACTICEINF] PASS  %s"), What);
			}
			else
			{
				++Failed;
				UE_LOG(LogTraceGame, Error, TEXT("[PRACTICEINF] FAIL  %s"), What);
			}
		}
	};

	ATracePlayerState* LocalTraceState(UWorld* WorldPtr)
	{
		const APlayerController* const FirstPC =
			(WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		return (FirstPC != nullptr) ? Cast<ATracePlayerState>(FirstPC->PlayerState) : nullptr;
	}

	ATraceCharacter* LocalPawn(UWorld* WorldPtr)
	{
		APlayerController* const FirstPC =
			(WorldPtr != nullptr) ? WorldPtr->GetFirstPlayerController() : nullptr;
		return (FirstPC != nullptr) ? Cast<ATraceCharacter>(FirstPC->GetPawn()) : nullptr;
	}

	UTraceAbilityComponent* LocalAbilities(UWorld* WorldPtr)
	{
		return UTraceAbilityComponent::Get(LocalTraceState(WorldPtr));
	}

	/** The pad with @p Role, or null. */
	ATracePracticePad* FindPad(UWorld* WorldPtr, ETracePracticePadRole Role)
	{
		if (WorldPtr == nullptr)
		{
			return nullptr;
		}
		for (TActorIterator<ATracePracticePad> It(WorldPtr); It; ++It)
		{
			if (It->GetPadRole() == Role)
			{
				return *It;
			}
		}
		return nullptr;
	}

	// =============================================================================================
	// The run
	// =============================================================================================

	struct FInfiniteRun
	{
		TWeakObjectPtr<UWorld> WorldPtr;
		FInfiniteTally Tally;

		int32 Step = 0;
		double StepStartRealTime = 0.0;

		/** Ticker calls spent inside the current burst. */
		int32 BurstFrame = 0;

		/** Whether the red arm was on when the run started, so the verdict can say which build it is. */
		bool bPollOnlyArmed = false;

		// ---- ARM: toggle OFF ----------------------------------------------------------------------
		bool bReadyBeforeOff = false;      ///< the ability was ready before the first press
		bool bFiredOff = false;            ///< the first press was accepted
		float CooldownAfterOff = -1.f;     ///< seconds left the instant after it fired
		bool bNextFrameOff = false;        ///< the very next frame's press was accepted
		int32 RefiresOff = 0;              ///< presses accepted across the burst
		float MaxCooldownSeenOff = 0.f;    ///< worst cooldown observed during the burst

		// ---- ARM: toggle ON -----------------------------------------------------------------------
		float CooldownWhenToggledOn = -1.f;  ///< the 20 s left from the OFF arm, after the toggle
		bool bFiredOn = false;
		float CooldownAfterOn = -1.f;
		bool bNextFrameOn = false;
		int32 RefiresOn = 0;
		float MaxCooldownSeenOn = 0.f;

		/** Which driver applied the cheat during the ON burst. See UTracePracticeRangeSubsystem. */
		int32 AppliesAtOnStart = 0;
		int32 TickAppliesAtOnStart = 0;
		int32 AppliesDuringOn = 0;
		int32 TickAppliesDuringOn = 0;

		// ---- the pads ------------------------------------------------------------------------------
		FVector PlayerHome = FVector::ZeroVector;
		bool bToggleBeforePad = false;
		bool bToggleAfterPad = false;
		FString PadLabelAfterTouch;
		bool bPadLitAfterTouch = false;
		bool bSelectOpenAfterSwapPad = false;

		/**
		 * Presses in a burst. Frames, not seconds, so a headless run's frame rate cannot change the
		 * denominator. Long enough to span several 200 ms polls at any plausible rate, which is what
		 * makes the RED arm's handful of successes distinguishable from zero AND from all of them.
		 */
		static constexpr int32 BurstFrames = 40;

		/** Real seconds a settling step is given. Two range polls plus slack. */
		static constexpr double SettleWait = 0.6;

		/** Real seconds to wait for the range to furnish itself and the player to log in. */
		static constexpr double ReadyWaitSeconds = 30.0;

		/** The subject. Chut's Chud is the cleanest cooldown in the roster — see PickSubject. */
		static constexpr ETraceCharacterId Subject = ETraceCharacterId::Chut;
	};

	TSharedPtr<FInfiniteRun> GInfiniteRun;

	bool TickInfiniteRun(float Unused);

	void GoToStep(FInfiniteRun& Run, int32 NextStep)
	{
		Run.Step = NextStep;
		Run.StepStartRealTime = FPlatformTime::Seconds();
		Run.BurstFrame = 0;
	}

	// ---------------------------------------------------------------------------------------------
	// WHY CHUT.
	//
	// The measurement has to be about the COOLDOWN and nothing else, so the subject must be a
	// character whose ability set never refuses for a reason of its own. Chut's Chud qualifies and
	// almost nobody else does: UTraceAbilitySetChut::ActivateAbility() writes one number and returns
	// true unconditionally on authority, it does not override CanActivate(), and it does not override
	// GetCharacterOwnedCooldownRemaining(). Its 20 s (UTraceSettings::ChudCooldownSeconds) is long
	// enough that the OFF arm cannot pass by accident.
	//
	// X would have been the wrong choice and is worth naming: UTraceAbilitySetX::CanActivate() refuses
	// with "BEES ALREADY LOADED" while the clip is loaded, so X is NOT re-firable back to back even
	// with every cooldown in the game at zero — and a harness that measured him would report the fix
	// failing when it was working. Elle is worse again: she is the one character who overrides
	// GetCharacterOwnedCooldownRemaining(), so her ring reads non-zero for reasons the toggle does not
	// and must not reach.
	// ---------------------------------------------------------------------------------------------
	bool PickSubject(UWorld* WorldPtr)
	{
		ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
		UTraceAbilityComponent* const Abilities = UTraceAbilityComponent::Get(TraceState);
		if (TraceState == nullptr || Abilities == nullptr)
		{
			return false;
		}

		if (Abilities->GetCharacterId() != FInfiniteRun::Subject)
		{
			Abilities->ServerSetCharacter(FInfiniteRun::Subject);
		}

		// The state a player who walks onto the pad is actually in: holding a character, locked, no
		// select screen open. Without this the range's own select poll can reopen the screen underneath
		// the run and pause the world.
		TraceState->ServerMarkCharacterResolved(/*bLocked=*/true, /*bWasChosen=*/true);
		TraceState->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
		return Abilities->GetCharacterId() == FInfiniteRun::Subject;
	}

	void RunInfiniteVerify()
	{
		UWorld* const WorldPtr = FindInfiniteVerifyWorld();
		if (WorldPtr == nullptr)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[PRACTICEINF] no game world; nothing to test."));
			return;
		}

		if (APlayerController* FirstPC = WorldPtr->GetFirstPlayerController())
		{
			FirstPC->SetPause(false);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("================================================================================"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[PRACTICEINF] SPEC v24 §5: with the INFINITE ABILITIES toggle OFF an ability must go "
			     "on cooldown and be unavailable; with it ON it must be immediately reusable. Both arms, "
			     "same harness, real E presses through UTraceAbilityComponent::TryActivate()."));

		UTracePracticeRangeSubsystem* const Range = UTracePracticeRangeSubsystem::Get(WorldPtr);

		// THE GATE FAILS RATHER THAN SKIPS. Running this in a real match is the cheapest red arm the
		// feature has, and a harness that quietly declined there would turn that arm into silence.
		FInfiniteTally GateTally;
		const bool bGateOpen = TracePracticeRange::IsActive(WorldPtr);
		GateTally.Report(bGateOpen,
			TEXT("the practice range is active in this world (open it with "
			     "?game=/Script/Trace.TracePracticeGameMode)."));

		if (!bGateOpen || Range == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[PRACTICEINF] VERDICT: *** FAIL *** — this world is not a practice range, so the "
				     "toggle does not exist here. That is the correct answer for a real match."));
			return;
		}

		TSharedPtr<FInfiniteRun> Run = MakeShared<FInfiniteRun>();
		Run->WorldPtr = WorldPtr;
		Run->Tally = GateTally;
		Run->StepStartRealTime = FPlatformTime::Seconds();
		Run->bPollOnlyArmed = TracePracticeRange::IsInfinitePollOnlyArmed();

		UE_LOG(LogTraceGame, Display,
			TEXT("[PRACTICEINF] driver under test: %s"),
			Run->bPollOnlyArmed
				? TEXT("*** RED ARM — Trace.Practice.PollOnlyInfinite 1: the 5 Hz furniture poll ONLY, "
				       "which is the pre-v24 build the owner reported. This run MUST fail. ***")
				: TEXT("the shipped v24 driver: the per-frame tick, which runs after the press in the "
				       "same frame."));

		GInfiniteRun = Run;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickInfiniteRun), 0.f);
	}

	bool TickInfiniteRun(float /*Unused*/)
	{
		TSharedPtr<FInfiniteRun> Run = GInfiniteRun;
		if (!Run.IsValid())
		{
			return false;
		}

		UWorld* const WorldPtr = Run->WorldPtr.Get();
		UTracePracticeRangeSubsystem* const Range = UTracePracticeRangeSubsystem::Get(WorldPtr);
		if (WorldPtr == nullptr || Range == nullptr)
		{
			GInfiniteRun.Reset();
			return false;
		}

		const double SinceStep = FPlatformTime::Seconds() - Run->StepStartRealTime;
		UTraceAbilityComponent* const Abilities = LocalAbilities(WorldPtr);
		ATraceCharacter* const PlayerPawn = LocalPawn(WorldPtr);

		switch (Run->Step)
		{
		// -----------------------------------------------------------------------------------------
		case 0:   // wait for a range, a pawn and an ability component, then take the subject
		{
			const bool bReady = Range->IsBuilt() && PlayerPawn != nullptr && PlayerPawn->IsAlive()
				&& Abilities != nullptr;
			if (!bReady && SinceStep < FInfiniteRun::ReadyWaitSeconds)
			{
				return true;
			}

			Run->Tally.Report(bReady,
				TEXT("the range is furnished and the local player has a pawn and an ability component."));
			if (!bReady)
			{
				GoToStep(*Run, 90);
				return true;
			}

			Run->Tally.Report(PickSubject(WorldPtr),
				TEXT("the subject is CHUT, whose Chud is the roster's one purely cooldown-gated ability "
				     "(20 s, no CanActivate() refusal, no character-owned timer)."));
			Run->PlayerHome = PlayerPawn->GetActorLocation();

			// The toggle is OFF as the range opens; say so rather than assume it.
			Range->SetInfiniteAbilities(false);
			GoToStep(*Run, 1);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 1:   // let the ability set rebuild after the character assignment
		{
			if (SinceStep < FInfiniteRun::SettleWait)
			{
				return true;
			}
			GoToStep(*Run, 2);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 2:   // *** ARM: TOGGLE OFF. Press E once for real. ***
		{
			if (Abilities == nullptr)
			{
				GoToStep(*Run, 90);
				return true;
			}

			Run->bReadyBeforeOff = Abilities->GetActivatedCooldownRemaining() <= 0.01f;
			Run->bFiredOff = Abilities->TryActivate();
			Run->CooldownAfterOff = Abilities->GetActivatedCooldownRemaining();

			GoToStep(*Run, 3);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 3:   // ...and now press it on every one of the next BurstFrames frames
		{
			if (Abilities == nullptr)
			{
				GoToStep(*Run, 90);
				return true;
			}

			const float CooldownNow = Abilities->GetActivatedCooldownRemaining();
			Run->MaxCooldownSeenOff = FMath::Max(Run->MaxCooldownSeenOff, CooldownNow);

			const bool bAccepted = Abilities->TryActivate();
			if (Run->BurstFrame == 0)
			{
				Run->bNextFrameOff = bAccepted;
			}
			if (bAccepted)
			{
				++Run->RefiresOff;
			}

			if (++Run->BurstFrame < FInfiniteRun::BurstFrames)
			{
				return true;
			}

			// ---- THE RED-ARM VALIDITY ASSERTIONS -------------------------------------------------
			//
			// These do not test the fix. They test that this harness can SEE the fix — the spec's own
			// rule that a red and a green arm reporting the same numbers means the harness is measuring
			// nothing. If the toggle-OFF arm cannot show an ability going on cooldown and becoming
			// unavailable, nothing below it means anything.
			Run->Tally.Report(Run->bReadyBeforeOff,
				TEXT("BASELINE: the ability was READY before the first press."));
			Run->Tally.Report(Run->bFiredOff,
				TEXT("BASELINE: with the toggle OFF, the press was ACCEPTED (so a cooldown was started "
				     "by the shipping path and not by a plant)."));
			Run->Tally.Report(Run->CooldownAfterOff > 1.f,
				*FString::Printf(TEXT("BASELINE: with the toggle OFF the ability WENT ON COOLDOWN — "
				                      "%.2f s left the instant it fired."), Run->CooldownAfterOff));
			Run->Tally.Report(!Run->bNextFrameOff,
				TEXT("BASELINE: with the toggle OFF the very next frame's press was REFUSED — the "
				     "ability is genuinely UNAVAILABLE."));
			Run->Tally.Report(Run->RefiresOff == 0,
				*FString::Printf(TEXT("BASELINE: with the toggle OFF, %d of %d further presses were "
				                      "accepted (must be 0)."),
					Run->RefiresOff, FInfiniteRun::BurstFrames));

			GoToStep(*Run, 4);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 4:   // *** ARM: TOGGLE ON. ***
		{
			Range->SetInfiniteAbilities(true);
			GoToStep(*Run, 5);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 5:   // give whichever driver is live at least three 200 ms polls to clear the leftover
		{
			if (SinceStep < FInfiniteRun::SettleWait)
			{
				return true;
			}

			if (Abilities == nullptr)
			{
				GoToStep(*Run, 90);
				return true;
			}

			Run->CooldownWhenToggledOn = Abilities->GetActivatedCooldownRemaining();
			Run->AppliesAtOnStart = Range->GetInfiniteApplyCount();
			Run->TickAppliesAtOnStart = Range->GetInfiniteTickApplyCount();

			Run->Tally.Report(Range->IsInfiniteAbilitiesOn(),
				TEXT("the INFINITE ABILITIES toggle reports ON inside the range."));

			// Worth its own line: this half works even on the RED arm, and it is the half the previous
			// harness measured. Clearing a cooldown that is ALREADY running is easy; not letting the
			// next press start one is the thing that was broken.
			Run->Tally.Report(Run->CooldownWhenToggledOn <= 0.01f,
				*FString::Printf(TEXT("turning the toggle ON cleared the %.0f s that was already "
				                      "running (%.2f s left)."),
					Run->CooldownAfterOff, Run->CooldownWhenToggledOn));

			Run->bFiredOn = Abilities->TryActivate();
			Run->CooldownAfterOn = Abilities->GetActivatedCooldownRemaining();
			GoToStep(*Run, 6);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 6:   // ...the same burst, the same denominator, the toggle ON
		{
			if (Abilities == nullptr)
			{
				GoToStep(*Run, 90);
				return true;
			}

			const float CooldownNow = Abilities->GetActivatedCooldownRemaining();
			Run->MaxCooldownSeenOn = FMath::Max(Run->MaxCooldownSeenOn, CooldownNow);

			const bool bAccepted = Abilities->TryActivate();
			if (Run->BurstFrame == 0)
			{
				Run->bNextFrameOn = bAccepted;
			}
			if (bAccepted)
			{
				++Run->RefiresOn;
			}

			if (++Run->BurstFrame < FInfiniteRun::BurstFrames)
			{
				return true;
			}

			Run->AppliesDuringOn = Range->GetInfiniteApplyCount() - Run->AppliesAtOnStart;
			Run->TickAppliesDuringOn = Range->GetInfiniteTickApplyCount() - Run->TickAppliesAtOnStart;

			// ---- THE RULE ------------------------------------------------------------------------
			Run->Tally.Report(Run->bFiredOn,
				TEXT("with the toggle ON, the press was accepted."));
			Run->Tally.Report(Run->bNextFrameOn,
				TEXT("*** THE RULE: with the toggle ON the ability was IMMEDIATELY REUSABLE — the very "
				     "next frame's press was accepted, where the OFF arm's was refused. ***"));
			Run->Tally.Report(Run->RefiresOn >= FInfiniteRun::BurstFrames - 1,
				*FString::Printf(TEXT("*** THE RULE: with the toggle ON, %d of %d consecutive presses "
				                      "were accepted (the OFF arm managed %d). ***"),
					Run->RefiresOn, FInfiniteRun::BurstFrames, Run->RefiresOff));
			Run->Tally.Report(Run->MaxCooldownSeenOn <= 0.01f,
				*FString::Printf(TEXT("...and no press ever left a cooldown a reader could see: worst "
				                      "reading during the burst %.2f s, against %.2f s with the toggle "
				                      "OFF. That is the HUD ring as well as TryActivate()."),
					Run->MaxCooldownSeenOn, Run->MaxCooldownSeenOff));

			UE_LOG(LogTraceGame, Display,
				TEXT("[PRACTICEINF] driver during the ON burst: %d applies, %d of them from the "
				     "per-frame tick. %d frames of presses."),
				Run->AppliesDuringOn, Run->TickAppliesDuringOn, FInfiniteRun::BurstFrames);

			GoToStep(*Run, 7);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 7:   // the OTHER half of the toggle: the dash pool
		{
			// The toggle's own log line promises "the E cooldown and the dash pool refill
			// continuously", so the dash pool is a claim this harness has to check rather than trust.
			const UTraceCharacterMovementComponent* const PlayerMove =
				(PlayerPawn != nullptr) ? PlayerPawn->GetTraceMovement() : nullptr;
			if (PlayerMove == nullptr)
			{
				Run->Tally.Report(false, TEXT("the local pawn has a Trace movement component."));
				GoToStep(*Run, 8);
				return true;
			}

			const int32 Charges = PlayerMove->GetDashCharges();
			const int32 MaxCharges = PlayerMove->GetMaxDashCharges();
			Run->Tally.Report(Charges >= MaxCharges,
				*FString::Printf(TEXT("with the toggle ON the dash pool is full (%d/%d charges)."),
					Charges, MaxCharges));

			GoToStep(*Run, 8);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 8:   // *** THE PAD IS THE PLAYER'S ONLY INTERFACE TO THIS TOGGLE. Walk onto it. ***
		{
			// Nothing had ever driven the toggle the way a PLAYER drives it. Both the old harness and
			// the console command call SetInfiniteAbilities() directly, so a pad whose overlap never
			// reached the subsystem would have read exactly like the bug being fixed here and no arm
			// would have caught it.
			ATracePracticePad* const Pad = FindPad(WorldPtr, ETracePracticePadRole::InfiniteAbilities);
			if (Pad == nullptr || PlayerPawn == nullptr)
			{
				Run->Tally.Report(false, TEXT("the range has an INFINITE ABILITIES pad to walk onto."));
				GoToStep(*Run, 11);
				return true;
			}

			Run->bToggleBeforePad = Range->IsInfiniteAbilitiesOn();

			// Step OFF anything first — the pad reports the ENTRY edge, so a touch needs a fresh entry.
			PlayerPawn->TeleportTo(Run->PlayerHome, PlayerPawn->GetActorRotation(),
				/*bIsATest=*/false, /*bNoCheck=*/true);
			GoToStep(*Run, 9);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 9:   // step onto it
		{
			// Past ATracePracticePad's 0.6 s retrigger lockout before the entry, so the touch cannot be
			// swallowed by a lockout the harness itself opened.
			if (SinceStep < 0.8)
			{
				return true;
			}

			ATracePracticePad* const Pad = FindPad(WorldPtr, ETracePracticePadRole::InfiniteAbilities);
			if (Pad != nullptr && PlayerPawn != nullptr)
			{
				PlayerPawn->TeleportTo(Pad->GetActorLocation() + FVector(0.f, 0.f, 100.f),
					PlayerPawn->GetActorRotation(), /*bIsATest=*/false, /*bNoCheck=*/true);
			}
			GoToStep(*Run, 10);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 10:  // ...and it flipped, and it says so
		{
			if (SinceStep < FInfiniteRun::SettleWait)
			{
				return true;
			}

			Run->bToggleAfterPad = Range->IsInfiniteAbilitiesOn();
			if (const ATracePracticePad* Pad = FindPad(WorldPtr, ETracePracticePadRole::InfiniteAbilities))
			{
				Run->PadLabelAfterTouch = Pad->GetPadLabel();
				Run->bPadLitAfterTouch = Pad->IsPadLit();
			}

			Run->Tally.Report(Run->bToggleAfterPad != Run->bToggleBeforePad,
				*FString::Printf(TEXT("WALKING ONTO the INFINITE ABILITIES pad flipped it: %s -> %s."),
					Run->bToggleBeforePad ? TEXT("ON") : TEXT("OFF"),
					Run->bToggleAfterPad ? TEXT("ON") : TEXT("OFF")));

			Run->Tally.Report(
				Run->PadLabelAfterTouch.Contains(Run->bToggleAfterPad ? TEXT("ON") : TEXT("OFF"))
					&& Run->bPadLitAfterTouch == Run->bToggleAfterPad,
				*FString::Printf(TEXT("...and the pad SAYS what it now is: label '%s', lit=%s."),
					*Run->PadLabelAfterTouch, Run->bPadLitAfterTouch ? TEXT("yes") : TEXT("no")));

			if (PlayerPawn != nullptr)
			{
				PlayerPawn->TeleportTo(Run->PlayerHome, PlayerPawn->GetActorRotation(),
					/*bIsATest=*/false, /*bNoCheck=*/true);
			}
			GoToStep(*Run, 11);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 11:  // the console form of the same toggle
		{
			if (GEngine != nullptr)
			{
				GEngine->Exec(WorldPtr, TEXT("Trace.Practice.InfiniteAbilities 1"));
			}
			const bool bOnByConsole = Range->IsInfiniteAbilitiesOn();

			if (GEngine != nullptr)
			{
				GEngine->Exec(WorldPtr, TEXT("Trace.Practice.InfiniteAbilities 0"));
			}
			const bool bOffByConsole = !Range->IsInfiniteAbilitiesOn();

			Run->Tally.Report(bOnByConsole && bOffByConsole,
				TEXT("Trace.Practice.InfiniteAbilities 1 / 0 drives the same toggle the pad does."));

			GoToStep(*Run, 12);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 12:  // *** SPEC v24 §0 — THE RANGE'S FOOTPRINT IS A RATIO NOW, SO MEASURE THE RATIO ***
		{
			// Both of these were absolutes whose only justification was a relationship to something
			// else, and both are now computed from that something else. So what is asserted here is
			// the RELATIONSHIP and not the number: if the arena's endzone gets deeper or shallower
			// tomorrow, these two lines still have to hold, and the constants they used to be would
			// not have. Measured off the furniture that is actually standing in the world, never off
			// the constants that produced it.
			const ATraceArenaBuilder* const Arena = ATraceArenaBuilder::Get(WorldPtr);
			const AActor* const StartPost = Range->GetPlayerStartPost();
			if (Arena == nullptr || StartPost == nullptr)
			{
				Run->Tally.Report(false, TEXT("the arena and the range's spawn line both exist."));
				GoToStep(*Run, 13);
				return true;
			}

			const float EndzoneDepth = Arena->ClampedEndzoneDepth();
			const float BuiltStandback = Range->GetBuiltTargetStandbackUU();

			Run->Tally.Report(BuiltStandback > 0.f && BuiltStandback < EndzoneDepth,
				*FString::Printf(TEXT("v24 §0: the firing distance is DERIVED from the arena's own "
				                      "endzone depth — %.1f uu of a %.0f uu endzone (%.3f x), so "
				                      "'the range is shallower than one end of the field' stays true "
				                      "when the field changes."),
					BuiltStandback, EndzoneDepth, BuiltStandback / FMath::Max(1.f, EndzoneDepth)));

			// The row's apparent width, measured off where the targets actually stand.
			const FVector SpawnPoint = StartPost->GetActorLocation();
			double WidestAngleDeg = -1.0;
			int32 TargetsSeen = 0;
			for (TActorIterator<ATracePracticeDummyController> It(WorldPtr); It; ++It)
			{
				const APawn* const DummyPawn = It->GetPawn();
				if (DummyPawn == nullptr)
				{
					continue;
				}
				++TargetsSeen;
				const FVector Delta = DummyPawn->GetActorLocation() - SpawnPoint;
				if (Delta.X > 1.0)
				{
					WidestAngleDeg = FMath::Max(WidestAngleDeg,
						FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(Delta.Y), Delta.X)));
				}
			}

			Run->Tally.Report(TargetsSeen > 0 && FMath::IsNearlyEqual(WidestAngleDeg, 13.30, 0.6),
				*FString::Printf(TEXT("v24 §0: the target row's lateral spacing is DERIVED from the "
				                      "firing distance — the outermost of %d targets measures %.2f "
				                      "degrees off the crosshair, which is the 13.3 the design comment "
				                      "claims, and it stays that whatever the firing distance becomes."),
					TargetsSeen, WidestAngleDeg));

			GoToStep(*Run, 13);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 13:  // THE OTHER PAD NOBODY HAD EVER WALKED ON: CHANGE CHARACTER. Done last — it opens a
		          // screen, and a screen can pause the world.
		{
			ATracePracticePad* const Pad = FindPad(WorldPtr, ETracePracticePadRole::CharacterSwap);
			if (Pad == nullptr || PlayerPawn == nullptr)
			{
				Run->Tally.Report(false, TEXT("the range has a CHANGE CHARACTER pad to walk onto."));
				GoToStep(*Run, 90);
				return true;
			}

			// Locked in and screen shut, which is the state a switch is actually about.
			PickSubject(WorldPtr);
			PlayerPawn->TeleportTo(Pad->GetActorLocation() + FVector(0.f, 0.f, 100.f),
				PlayerPawn->GetActorRotation(), /*bIsATest=*/false, /*bNoCheck=*/true);
			GoToStep(*Run, 14);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 14:
		{
			// ATraceGameMode::PollCharacterSelect runs at 4 Hz, so give it several passes.
			if (SinceStep < FInfiniteRun::SettleWait * 2.0)
			{
				return true;
			}

			const ATracePlayerState* const TraceState = LocalTraceState(WorldPtr);
			Run->bSelectOpenAfterSwapPad = (TraceState != nullptr) && TraceState->IsCharacterSelectOpen();

			Run->Tally.Report(Run->bSelectOpenAfterSwapPad,
				TEXT("WALKING ONTO the CHANGE CHARACTER pad reopened the shipped select screen."));

			GoToStep(*Run, 90);
			return true;
		}

		// -----------------------------------------------------------------------------------------
		case 90:  // verdict
		default:
		{
			// A harness must not leave the range in a state the next player did not ask for.
			Range->SetInfiniteAbilities(false);

			UE_LOG(LogTraceGame, Display,
				TEXT("--------------------------------------------------------------------------------"));
			UE_LOG(LogTraceGame, Display,
				TEXT("[PRACTICEINF] THE NUMBERS, side by side. Presses accepted out of %d consecutive "
				     "frames:   toggle OFF = %d   |   toggle ON = %d."),
				FInfiniteRun::BurstFrames, Run->RefiresOff, Run->RefiresOn);
			UE_LOG(LogTraceGame, Display,
				TEXT("[PRACTICEINF] Cooldown left the instant the ability fired:   toggle OFF = %.2f s   "
				     "|   toggle ON = %.2f s.   Worst reading across the burst: OFF %.2f s | ON %.2f s."),
				Run->CooldownAfterOff, Run->CooldownAfterOn,
				Run->MaxCooldownSeenOff, Run->MaxCooldownSeenOn);
			UE_LOG(LogTraceGame, Display,
				TEXT("[PRACTICEINF] Reusable on the very next frame:   toggle OFF = %s   |   "
				     "toggle ON = %s."),
				Run->bNextFrameOff ? TEXT("yes") : TEXT("NO"),
				Run->bNextFrameOn ? TEXT("yes") : TEXT("NO"));

			if (Run->Tally.Failed == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[PRACTICEINF] VERDICT: *** PASS *** — %d/%d. %s"),
					Run->Tally.Passed, Run->Tally.Passed,
					Run->bPollOnlyArmed
						? TEXT("*** BUT THE RED ARM WAS ON. A pass here means Trace.Practice.PollOnlyInfinite "
						       "is not restoring the old driver, so the green arm below it proves NOTHING. "
						       "Treat this run as INVALID. ***")
						: TEXT("The toggle is enforced at every frame the player can press in."));
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[PRACTICEINF] VERDICT: *** FAIL *** — %d of %d assertions failed. %s"),
					Run->Tally.Failed, Run->Tally.Failed + Run->Tally.Passed,
					Run->bPollOnlyArmed
						? TEXT("This is the RED ARM (Trace.Practice.PollOnlyInfinite 1) and a failure here "
						       "is the REPRODUCTION: it is the build in which the toggle 'doesn't do "
						       "anything'.")
						: TEXT("The shipped driver is not holding the cooldown down."));
			}

			GInfiniteRun.Reset();
			return false;
		}
		}
	}

	FAutoConsoleCommand CmdPracticeInfiniteVerify(
		TEXT("Trace.Practice.InfiniteVerify"),
		TEXT("SPEC v24 §5. Presses E for real with the range's infinite-abilities toggle OFF and then "
		     "ON, and reports both arms' numbers. Also walks the player onto the INFINITE ABILITIES "
		     "and CHANGE CHARACTER pads, which is the only interface a player has to either. Red-arm "
		     "it with Trace.Practice.PollOnlyInfinite 1."),
		FConsoleCommandDelegate::CreateStatic(&RunInfiniteVerify));
}

#endif   // !UE_BUILD_SHIPPING
