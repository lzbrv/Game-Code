// Trace — Trace.Input.RestoreVerify. Nothing in this file is shipped gameplay.

// ===================================================================================================
// Trace.Input.RestoreVerify — a key pressed ON a closing page does not come back as a gameplay press
//
// THE BUG. SPACE is SELECT on the pause menu and JUMP in a match. Pressing it on RESUME closes the
// menu inside the same frame, the menu's OnClosed hands input back, and the v18 §1c re-delivery —
// which re-fires every hold-shaped action whose key is still down — found SPACE down and re-fired it
// as a jump. The player resumed and jumped in the same instant. Q did the same on the loadout page:
// it is TAB-left there and PARRY in a match.
//
// WHAT THIS DRIVES, all through the real viewport input path a keyboard takes:
//
//   LOADOUT ARM (must start with the loadout page up):
//     Q and CTRL go down ON the page, then the page locks in and the window closes.
//       -> PARRY withheld  (Q is the page's TAB-left key, pressed on the page)
//       -> CROUCH re-delivered (CTRL is not a key the page reads)
//   PAUSE ARM (in live play, a moment later):
//     TAB goes down, then ESC opens the pause menu, then SPACE goes down on RESUME.
//       -> the menu closes
//       -> JUMP withheld (SPACE is the menu's SELECT key, pressed on the menu)
//       -> SCOREBOARD re-delivered (TAB was held since BEFORE the menu: spec v18 §1c, unchanged)
//       -> the pawn is still on the ground a moment later
//     then SPACE is let go and pressed again in play
//       -> it jumps (the key was only taken away until it was released)
//   PAD ARM: ESC again, then pad A (SELECT on the menu, JUMP in play) on RESUME
//       -> pad A is taken from Enhanced Input until released; the pawn stays on the ground
//
// `red` runs it with Trace.Input.WithholdPageKeysOnRestore 0 — the behaviour before the fix — and
// must FAIL on exactly the withheld checks. A harness that cannot go red is not evidence.
//
// Headless recipe:
//   Arena -bots=4 -TraceExecAt=6 -TraceExec="Trace.Teams.Close|Trace.V10.After 1.5 Trace.Input.RestoreVerify"
// ===================================================================================================

#include "Containers/Ticker.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Settings/TraceUserSettings.h"   // ETraceInputAction — the restore record's bit order
#include "Trace.h"                        // LogTraceGame
#include "UI/TraceLoadoutSelect.h"        // IsArmed — the loadout page is the page that follows team select
#include "UnrealClient.h"                 // FViewport

#if !UE_BUILD_SHIPPING

// NAMED, not anonymous: this module builds as a unity blob (see Scripts/unity-hygiene.py).
namespace TraceInputRestoreVerify
{
	enum class EStage : uint8
	{
		LoadoutPress,      // Q + CTRL down on the loadout page
		LoadoutLock,       // lock in; the window closes
		LoadoutClosed,     // wait for the hand-back, judge it
		AwaitPawn,         // release; wait for a live pawn standing on something
		PauseOpen,         // TAB down, then ESC
		PauseEscUp,
		PauseSpace,        // menu up: SPACE on RESUME
		PauseClosed,       // wait for the hand-back, judge it
		PauseGround,       // did the pawn leave the ground? then SPACE up
		Repress,           // a FRESH SPACE, with the page gone: must jump
		RepressJudge,
		PadOpen,           // the pad: ESC again, then pad A (SELECT here, JUMP in play) on RESUME
		PadEscUp,
		PadConfirm,
		PadClosed,
		PadGround,
		Done,
	};

	struct FRestoreRun
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<ATracePlayerController> Controller;
		EStage Stage = EStage::LoadoutPress;
		double StageStartReal = 0.0;
		int32 RestoreCountBefore = 0;
		int32 Passes = 0;
		int32 Failures = 0;
		bool bRed = false;
		int32 WithholdWas = 1;
		bool bGroundedBeforeResume = false;
		TArray<FKey> HeldByRun;
	};

	static IConsoleVariable* WithholdCVar()
	{
		return IConsoleManager::Get().FindConsoleVariable(TEXT("Trace.Input.WithholdPageKeysOnRestore"));
	}

	static uint64 BitOf(ETraceInputAction Action)
	{
		return uint64(1) << static_cast<uint32>(Action);
	}

	static void Report(FRestoreRun& Run, const TCHAR* Claim, bool bPass, const FString& Detail)
	{
		(bPass ? Run.Passes : Run.Failures) += 1;
		UE_LOG(LogTraceGame, Display, TEXT("[RestoreVerify]   %-4s %s  %s"), bPass ? TEXT("ok") : TEXT("FAIL"),
			Claim, *Detail);
	}

	/** One press or release through the viewport, as a real keyboard delivers it. */
	static void InjectKey(FRestoreRun& Run, const FKey& Key, bool bDown)
	{
		if (Run.Controller.Get() == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr)
		{
			return;
		}
		const FInputKeyEventArgs KeyArgs(GEngine->GameViewport->Viewport, FInputDeviceId::CreateFromInternalId(0),
			Key, bDown ? IE_Pressed : IE_Released, bDown ? 1.f : 0.f, /*bIsTouchEvent*/ false, FPlatformTime::Cycles64());
		GEngine->GameViewport->InputKey(KeyArgs);

		if (bDown)
		{
			Run.HeldByRun.AddUnique(Key);
		}
		else
		{
			Run.HeldByRun.Remove(Key);
		}
		UE_LOG(LogTraceGame, Display, TEXT("[RestoreVerify] %s %s"), *Key.ToString(), bDown ? TEXT("down") : TEXT("up"));
	}

	static void Advance(FRestoreRun& Run, EStage Next)
	{
		Run.Stage = Next;
		Run.StageStartReal = FPlatformTime::Seconds();
	}

	static double SinceStage(const FRestoreRun& Run)
	{
		return FPlatformTime::Seconds() - Run.StageStartReal;
	}

	/** The controller's pawn if it is a living Trace character, else null. */
	static const ATraceCharacter* LivingPawnOf(const ATracePlayerController& PC)
	{
		const ATraceCharacter* const Body = Cast<ATraceCharacter>(PC.GetPawn());
		return (Body != nullptr && Body->IsAlive()) ? Body : nullptr;
	}

	static FString DescribeRecord(const ATracePlayerController& PC)
	{
		return FString::Printf(TEXT("restores %d, redelivered mask 0x%llx, withheld mask 0x%llx, mappings ignored until release %d"),
			PC.GetInputRestoreCount(), PC.GetLastRestoreRedeliveredMask(), PC.GetLastRestoreWithheldMask(),
			PC.GetLastRestoreIgnoredKeyCount());
	}

	static void Finish(FRestoreRun& Run, bool bCompleted)
	{
		// Nothing the run pressed stays down, whatever stage it stopped at.
		const TArray<FKey> StillHeld = Run.HeldByRun;
		for (const FKey& HeldKey : StillHeld)
		{
			InjectKey(Run, HeldKey, false);
		}

		if (Run.bRed)
		{
			if (IConsoleVariable* const Var = WithholdCVar())
			{
				Var->Set(Run.WithholdWas, ECVF_SetByCode);
			}
		}

		const TCHAR* const Arm = Run.bRed ? TEXT(" [RED ARM: Trace.Input.WithholdPageKeysOnRestore 0 — expected to FAIL]")
		                                  : TEXT("");
		if (Run.Failures == 0 && bCompleted)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[RestoreVerify] ===== PASS (%d checks) — VERDICT: a key pressed on the closing page is not re-delivered; a key held from before still is =====%s"),
				Run.Passes, Arm);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[RestoreVerify] ===== *** FAIL *** %d of %d check(s)%s — VERDICT: FAIL =====%s"),
				Run.Failures, Run.Failures + Run.Passes, bCompleted ? TEXT("") : TEXT(", run did not finish"), Arm);
		}
	}

	/** One tick. Returns false when the run is over. */
	static bool Tick(FRestoreRun& Run)
	{
		ATracePlayerController* const PC = Run.Controller.Get();
		if (PC == nullptr || Run.World.Get() == nullptr)
		{
			Report(Run, TEXT("the controller lived to the end of the run"), false, TEXT("it went away"));
			return false;
		}

		switch (Run.Stage)
		{
		case EStage::LoadoutPress:
			Run.RestoreCountBefore = PC->GetInputRestoreCount();
			InjectKey(Run, EKeys::Q, true);             // the page's TAB-left; PARRY in a match
			InjectKey(Run, EKeys::LeftControl, true);   // not a key the page reads; CROUCH in a match
			Advance(Run, EStage::LoadoutLock);
			return true;

		case EStage::LoadoutLock:
			if (SinceStage(Run) < 0.4)
			{
				return true;
			}
			GEngine->Exec(Run.World.Get(), TEXT("Trace.Loadout.Press lock"));
			Advance(Run, EStage::LoadoutClosed);
			return true;

		case EStage::LoadoutClosed:
		{
			if (PC->GetInputRestoreCount() == Run.RestoreCountBefore)
			{
				if (SinceStage(Run) > 8.0)
				{
					Report(Run, TEXT("locking in closed the loadout window and handed input back"), false,
						FString::Printf(TEXT("no hand-back after 8 s; suppressed=%d"), PC->IsGameInputSuppressed() ? 1 : 0));
					return false;
				}
				return true;
			}
			const uint64 Redelivered = PC->GetLastRestoreRedeliveredMask();
			const uint64 Withheld = PC->GetLastRestoreWithheldMask();
			Report(Run, TEXT("Q pressed on the loadout page is NOT re-delivered as PARRY"),
				(Redelivered & BitOf(ETraceInputAction::Parry)) == 0 && (Withheld & BitOf(ETraceInputAction::Parry)) != 0,
				DescribeRecord(*PC));
			Report(Run, TEXT("CTRL pressed on the loadout page (not one of its keys) IS re-delivered as CROUCH"),
				(Redelivered & BitOf(ETraceInputAction::Crouch)) != 0, DescribeRecord(*PC));
			InjectKey(Run, EKeys::Q, false);
			InjectKey(Run, EKeys::LeftControl, false);
			Advance(Run, EStage::AwaitPawn);
			return true;
		}

		case EStage::AwaitPawn:
		{
			// Long enough for the crouch just re-delivered to be let go and for the pawn to settle.
			const ATraceCharacter* const Pawn = LivingPawnOf(*PC);
			const bool bStanding = Pawn != nullptr && Pawn->GetCharacterMovement() != nullptr
				&& Pawn->GetCharacterMovement()->IsMovingOnGround() && !Pawn->bIsCrouched;
			if (SinceStage(Run) < 1.5 || (!bStanding && SinceStage(Run) < 10.0))
			{
				return true;
			}
			if (!bStanding || PC->IsGameInputSuppressed())
			{
				Report(Run, TEXT("a live pawn standing on the ground, with input live, before the pause arm"), false,
					FString::Printf(TEXT("pawn %d, standing %d, suppressed %d"), Pawn != nullptr ? 1 : 0,
						bStanding ? 1 : 0, PC->IsGameInputSuppressed() ? 1 : 0));
				return false;
			}
			InjectKey(Run, EKeys::Tab, true);   // held from BEFORE the menu: SCOREBOARD, must come back
			Advance(Run, EStage::PauseOpen);
			return true;
		}

		case EStage::PauseOpen:
			if (SinceStage(Run) < 0.3)
			{
				return true;
			}
			InjectKey(Run, EKeys::Escape, true);
			Advance(Run, EStage::PauseEscUp);
			return true;

		case EStage::PauseEscUp:
			if (SinceStage(Run) < 0.05)
			{
				return true;
			}
			InjectKey(Run, EKeys::Escape, false);
			Advance(Run, EStage::PauseSpace);
			return true;

		case EStage::PauseSpace:
		{
			if (SinceStage(Run) < 0.5)
			{
				return true;
			}
			if (!PC->IsGameInputSuppressed())
			{
				Report(Run, TEXT("ESC opened the pause menu"), false, TEXT("gameplay input was never suppressed"));
				return false;
			}
			const ATraceCharacter* const Pawn = LivingPawnOf(*PC);
			Run.bGroundedBeforeResume = Pawn != nullptr && Pawn->GetCharacterMovement() != nullptr
				&& Pawn->GetCharacterMovement()->IsMovingOnGround();
			Run.RestoreCountBefore = PC->GetInputRestoreCount();
			InjectKey(Run, EKeys::SpaceBar, true);   // SELECT on RESUME; JUMP in a match
			Advance(Run, EStage::PauseClosed);
			return true;
		}

		case EStage::PauseClosed:
		{
			if (PC->GetInputRestoreCount() == Run.RestoreCountBefore)
			{
				if (SinceStage(Run) > 2.0)
				{
					Report(Run, TEXT("SPACE on RESUME closed the pause menu and handed input back"), false,
						FString::Printf(TEXT("still suppressed=%d after 2 s"), PC->IsGameInputSuppressed() ? 1 : 0));
					return false;
				}
				return true;
			}
			const uint64 Redelivered = PC->GetLastRestoreRedeliveredMask();
			const uint64 Withheld = PC->GetLastRestoreWithheldMask();
			Report(Run, TEXT("SPACE on RESUME closed the pause menu and handed input back"),
				!PC->IsGameInputSuppressed(), DescribeRecord(*PC));
			Report(Run, TEXT("SPACE pressed on RESUME is NOT re-delivered as JUMP"),
				(Redelivered & BitOf(ETraceInputAction::Jump)) == 0 && (Withheld & BitOf(ETraceInputAction::Jump)) != 0,
				DescribeRecord(*PC));
			Report(Run, TEXT("SPACE is taken from Enhanced Input until it is released (no Started on the first unpaused frame)"),
				PC->GetLastRestoreIgnoredKeyCount() > 0, DescribeRecord(*PC));
			Report(Run, TEXT("TAB held since before the menu IS re-delivered as SCOREBOARD (spec v18 1c)"),
				(Redelivered & BitOf(ETraceInputAction::Scoreboard)) != 0, DescribeRecord(*PC));
			Advance(Run, EStage::PauseGround);
			return true;
		}

		case EStage::PauseGround:
		{
			if (SinceStage(Run) < 0.35)
			{
				return true;
			}
			const ATraceCharacter* const Pawn = LivingPawnOf(*PC);
			const bool bFalling = Pawn != nullptr && Pawn->GetCharacterMovement() != nullptr
				&& Pawn->GetCharacterMovement()->IsFalling();
			const float UpSpeed = (Pawn != nullptr) ? static_cast<float>(Pawn->GetVelocity().Z) : 0.f;
			Report(Run, TEXT("the pawn did not leave the ground on RESUME"),
				Run.bGroundedBeforeResume && !bFalling,
				FString::Printf(TEXT("grounded before %d, falling 0.35 s after %d, vertical speed %.0f"),
					Run.bGroundedBeforeResume ? 1 : 0, bFalling ? 1 : 0, UpSpeed));
			InjectKey(Run, EKeys::SpaceBar, false);
			InjectKey(Run, EKeys::Tab, false);
			Advance(Run, EStage::Repress);
			return true;
		}

		case EStage::Repress:
		{
			// Letting go gives the key back. A jump pressed now, with nothing open, must be a jump —
			// or the fix has only swapped "jumps out of the menu" for "SPACE is dead after the menu".
			const ATraceCharacter* const Pawn = LivingPawnOf(*PC);
			const bool bLanded = Pawn != nullptr && Pawn->GetCharacterMovement() != nullptr
				&& Pawn->GetCharacterMovement()->IsMovingOnGround();
			if (SinceStage(Run) < 0.4 || (!bLanded && SinceStage(Run) < 3.0))
			{
				return true;
			}
			InjectKey(Run, EKeys::SpaceBar, true);
			Advance(Run, EStage::RepressJudge);
			return true;
		}

		case EStage::RepressJudge:
		{
			if (SinceStage(Run) < 0.3)
			{
				return true;
			}
			const ATraceCharacter* const Pawn = LivingPawnOf(*PC);
			const bool bFalling = Pawn != nullptr && Pawn->GetCharacterMovement() != nullptr
				&& Pawn->GetCharacterMovement()->IsFalling();
			Report(Run, TEXT("after SPACE is let go, a fresh SPACE in play jumps again"), bFalling,
				FString::Printf(TEXT("falling 0.3 s after the press %d, vertical speed %.0f"), bFalling ? 1 : 0,
					(Pawn != nullptr) ? static_cast<float>(Pawn->GetVelocity().Z) : 0.f));
			InjectKey(Run, EKeys::SpaceBar, false);
			Advance(Run, EStage::PadOpen);
			return true;
		}

		case EStage::PadOpen:
		{
			const ATraceCharacter* const Pawn = LivingPawnOf(*PC);
			const bool bLanded = Pawn != nullptr && Pawn->GetCharacterMovement() != nullptr
				&& Pawn->GetCharacterMovement()->IsMovingOnGround();
			if (SinceStage(Run) < 0.4 || (!bLanded && SinceStage(Run) < 3.0))
			{
				return true;
			}
			InjectKey(Run, EKeys::Escape, true);
			Advance(Run, EStage::PadEscUp);
			return true;
		}

		case EStage::PadEscUp:
			if (SinceStage(Run) < 0.05)
			{
				return true;
			}
			InjectKey(Run, EKeys::Escape, false);
			Advance(Run, EStage::PadConfirm);
			return true;

		case EStage::PadConfirm:
		{
			if (SinceStage(Run) < 0.5)
			{
				return true;
			}
			if (!PC->IsGameInputSuppressed())
			{
				Report(Run, TEXT("ESC opened the pause menu a second time"), false, TEXT("gameplay input was never suppressed"));
				return false;
			}
			const ATraceCharacter* const Pawn = LivingPawnOf(*PC);
			Run.bGroundedBeforeResume = Pawn != nullptr && Pawn->GetCharacterMovement() != nullptr
				&& Pawn->GetCharacterMovement()->IsMovingOnGround();
			Run.RestoreCountBefore = PC->GetInputRestoreCount();
			InjectKey(Run, EKeys::Gamepad_FaceButton_Bottom, true);   // pad A: SELECT on RESUME; JUMP in a match
			Advance(Run, EStage::PadClosed);
			return true;
		}

		case EStage::PadClosed:
		{
			if (PC->GetInputRestoreCount() == Run.RestoreCountBefore)
			{
				if (SinceStage(Run) > 2.0)
				{
					Report(Run, TEXT("pad A on RESUME closed the pause menu and handed input back"), false,
						FString::Printf(TEXT("still suppressed=%d after 2 s (CONTROLLER INPUT off?)"),
							PC->IsGameInputSuppressed() ? 1 : 0));
					return false;
				}
				return true;
			}
			Report(Run, TEXT("pad A is taken from Enhanced Input until it is released"),
				PC->GetLastRestoreIgnoredKeyCount() > 0, DescribeRecord(*PC));
			Advance(Run, EStage::PadGround);
			return true;
		}

		case EStage::PadGround:
		{
			if (SinceStage(Run) < 0.35)
			{
				return true;
			}
			const ATraceCharacter* const Pawn = LivingPawnOf(*PC);
			const bool bFalling = Pawn != nullptr && Pawn->GetCharacterMovement() != nullptr
				&& Pawn->GetCharacterMovement()->IsFalling();
			Report(Run, TEXT("the pawn did not leave the ground on pad-A RESUME"),
				Run.bGroundedBeforeResume && !bFalling,
				FString::Printf(TEXT("grounded before %d, falling 0.35 s after %d, vertical speed %.0f"),
					Run.bGroundedBeforeResume ? 1 : 0, bFalling ? 1 : 0,
					(Pawn != nullptr) ? static_cast<float>(Pawn->GetVelocity().Z) : 0.f));
			InjectKey(Run, EKeys::Gamepad_FaceButton_Bottom, false);
			Advance(Run, EStage::Done);
			return false;
		}

		default:
			return false;
		}
	}

	static void Start(const TArray<FString>& Args, UWorld* WorldPtr)
	{
		ATracePlayerController* const PC = (WorldPtr != nullptr)
			? Cast<ATracePlayerController>(WorldPtr->GetFirstPlayerController()) : nullptr;
		const ATracePlayerState* const LocalState = (PC != nullptr) ? PC->GetPlayerState<ATracePlayerState>() : nullptr;
		if (PC == nullptr || !PC->IsLocalController() || LocalState == nullptr || !LocalState->IsCharacterSelectOpen()
			|| !TraceLoadoutSelect::IsArmed() || !PC->IsGameInputSuppressed())
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[RestoreVerify] ===== *** FAIL *** INCONCLUSIVE: start this with the LOADOUT page up on the Arena "
				     "(Trace.Teams.Close, then this) — VERDICT: FAIL ====="));
			return;
		}

		TSharedRef<FRestoreRun> Run = MakeShared<FRestoreRun>();
		Run->World = WorldPtr;
		Run->Controller = PC;
		for (const FString& Arg : Args)
		{
			Run->bRed |= Arg.Equals(TEXT("red"), ESearchCase::IgnoreCase);
		}
		if (Run->bRed)
		{
			if (IConsoleVariable* const Var = WithholdCVar())
			{
				Run->WithholdWas = Var->GetInt();
				Var->Set(0, ECVF_SetByCode);
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[RestoreVerify] ===== keys pressed ON a closing page vs keys held from before it: loadout Q / CTRL, pause SPACE / TAB, pad A%s ====="),
			Run->bRed ? TEXT(" — RED ARM") : TEXT(""));
		Advance(*Run, EStage::LoadoutPress);

		// The core ticker, not a world timer: the pause menu stops the world, and this has to keep
		// running while it is up.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float /*Delta*/) -> bool
		{
			if (!Tick(*Run))
			{
				Finish(*Run, Run->Stage == EStage::Done);
				return false;
			}
			return true;
		}), 0.f);
	}

	static FAutoConsoleCommandWithWorldAndArgs CmdRestoreVerify(
		TEXT("Trace.Input.RestoreVerify"),
		TEXT("Dev only. Start with the loadout page up. Presses Q and CTRL on the page and locks in; then, in play, holds ")
		TEXT("TAB, opens the pause menu and presses SPACE on RESUME, then pad A on RESUME. Proves a key pressed ON the closing page is not ")
		TEXT("re-delivered as a gameplay press (Q -> parry, SPACE -> jump) while a key held from before, or one the ")
		TEXT("page does not read, still is. `red` runs the pre-fix behaviour and must FAIL."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Start));
}

#endif // !UE_BUILD_SHIPPING
