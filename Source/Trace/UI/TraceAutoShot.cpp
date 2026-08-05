// Trace — automated screenshot harness implementation. See TraceAutoShot.h.

#include "UI/TraceAutoShot.h"

#if !UE_BUILD_SHIPPING

#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformFileManager.h"      // directory creation / file stat
#include "Misc/CommandLine.h"             // -TraceAutoShot=
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "Trace.h"                        // LogTraceGame
#include "UnrealClient.h"                 // FScreenshotRequest
#include "Engine/Engine.h"                // GEngine->Exec fallback for the deferred exec harness

namespace
{
	/**
	 * Process-wide harness bookkeeping.
	 *
	 * The arm-once guard is keyed on the *world*, not on a bool: a looping capture timer belongs to
	 * the world's timer manager and dies with it, so after a travel the freshly created HUD has to
	 * be allowed to arm again. Keying on the world gives exactly that while still stopping two HUDs
	 * inside one world (host + a second local player) from racing over the same filenames.
	 */
	struct FTraceAutoShotState
	{
		TWeakObjectPtr<UWorld> ArmedWorld;
		FTimerHandle CaptureTimer;
		FTimerHandle ConfirmTimer;
		FString PendingPath;
		int32 Index = 0;
	};

	FTraceAutoShotState& AutoShotState()
	{
		static FTraceAutoShotState State;
		return State;
	}

	void ConfirmCapture()
	{
		FTraceAutoShotState& State = AutoShotState();
		if (State.PendingPath.IsEmpty())
		{
			return;
		}

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.FileExists(*State.PendingPath))
		{
			UE_LOG(LogTraceGame, Display, TEXT("[AutoShot] Screenshot written (%lld bytes): %s"),
				PlatformFile.FileSize(*State.PendingPath), *State.PendingPath);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[AutoShot] Screenshot was requested but no file appeared at: %s"),
				*State.PendingPath);
		}
	}

	void TakeCapture(AHUD* OwnerHUD, FString Tag)
	{
		UWorld* World = (OwnerHUD != nullptr) ? OwnerHUD->GetWorld() : nullptr;
		if (World == nullptr)
		{
			return;
		}

		FTraceAutoShotState& State = AutoShotState();
		++State.Index;

		const FString FileName = FString::Printf(TEXT("TraceAutoShot_%s_%s_%02d.png"),
			*Tag, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), State.Index);
		State.PendingPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName);

		// FFileHelper does not reliably create the tree for us; make sure the directory is there
		// before the render thread tries to write into it.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(State.PendingPath));

		// bAddFilenameSuffix = false so the path we log is the path that gets written.
		// FScreenshotRequest treats a filename containing a slash as a complete path and leaves it
		// alone. bShowUI = false keeps engine on-screen debug text out of the capture; the HUD itself
		// is drawn into the scene's back buffer and IS captured, which is what we want to verify.
		FScreenshotRequest::RequestScreenshot(State.PendingPath, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);

		UE_LOG(LogTraceGame, Display, TEXT("[AutoShot] Screenshot requested: %s"), *State.PendingPath);

		// A screenshot on its own cannot tell you WHY it looks wrong. Pair every capture with the
		// view state that produced it, so an odd-looking frame can be diagnosed from the log alone.
		if (APlayerController* PC = OwnerHUD->GetOwningPlayerController())
		{
			FVector ViewLocation = FVector::ZeroVector;
			FRotator ViewRotation = FRotator::ZeroRotator;
			PC->GetPlayerViewPoint(ViewLocation, ViewRotation);

			const APawn* ViewPawn = PC->GetPawn();
			UE_LOG(LogTraceGame, Display,
				TEXT("[AutoShot] View: map=%s pawn=%s at %s | camera %s rot %s"),
				*World->GetMapName(),
				*GetNameSafe(ViewPawn),
				ViewPawn ? *ViewPawn->GetActorLocation().ToCompactString() : TEXT("<none>"),
				*ViewLocation.ToCompactString(),
				*ViewRotation.ToCompactString());
		}

		// The capture happens on the render thread at the end of a later frame, so confirm rather
		// than assume. Two seconds is generous even on a cold Metal pipeline.
		World->GetTimerManager().SetTimer(State.ConfirmTimer,
			FTimerDelegate::CreateWeakLambda(OwnerHUD, []() { ConfirmCapture(); }), 2.f, false);
	}
}

void TraceAutoShot::Arm(AHUD* OwnerHUD, const TCHAR* Tag)
{
	// Only the machine that actually owns a viewport can produce a frame.
	if (OwnerHUD == nullptr)
	{
		return;
	}

	UWorld* World = OwnerHUD->GetWorld();
	APlayerController* PC = OwnerHUD->GetOwningPlayerController();
	if (World == nullptr || PC == nullptr || !PC->IsLocalController())
	{
		return;
	}

	float DelaySeconds = 0.f;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TraceAutoShot="), DelaySeconds))
	{
		// Switch absent: this is the entire cost of the harness in a normal run.
		return;
	}

	FTraceAutoShotState& State = AutoShotState();
	if (State.ArmedWorld.Get() == World)
	{
		return;
	}
	State.ArmedWorld = World;

	// Zero or negative is legal and means "next tick"; the timer manager rejects <= 0, so floor it.
	DelaySeconds = FMath::Max(0.01f, DelaySeconds);

	float RepeatSeconds = 0.f;
	const bool bRepeat = FParse::Value(FCommandLine::Get(), TEXT("TraceAutoShotRepeat="), RepeatSeconds)
		&& RepeatSeconds > 0.01f;

	const FString TagCopy(Tag != nullptr ? Tag : TEXT("Shot"));

	UE_LOG(LogTraceGame, Display, TEXT("[AutoShot] Armed (%s): first capture in %.2fs%s"),
		*TagCopy, DelaySeconds,
		bRepeat ? *FString::Printf(TEXT(", then every %.2fs"), RepeatSeconds) : TEXT(""));

	World->GetTimerManager().SetTimer(State.CaptureTimer,
		FTimerDelegate::CreateWeakLambda(OwnerHUD, [OwnerHUD, TagCopy]() { TakeCapture(OwnerHUD, TagCopy); }),
		bRepeat ? RepeatSeconds : DelaySeconds, bRepeat, DelaySeconds);
}

void TraceAutoShot::ArmDeferredExec(AHUD* OwnerHUD, const TCHAR* Tag)
{
	if (OwnerHUD == nullptr)
	{
		return;
	}

	UWorld* World = OwnerHUD->GetWorld();
	APlayerController* PC = OwnerHUD->GetOwningPlayerController();
	if (World == nullptr || PC == nullptr || !PC->IsLocalController())
	{
		return;
	}

	FString CommandList;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TraceExec="), CommandList) || CommandList.IsEmpty())
	{
		// Switch absent: one FParse and out, same as the screenshot harness.
		return;
	}

	// Which HUD this run is aimed at. Defaulting to "Match" is the safe half of the choice: almost
	// every command worth deferring needs a live match, and firing one at the title screen produces
	// a confusing "no world" refusal that reads like the command is broken.
	FString WantTag;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TraceExecOn="), WantTag) || WantTag.IsEmpty())
	{
		WantTag = TEXT("Match");
	}
	const FString TagCopy(Tag != nullptr ? Tag : TEXT(""));
	if (!TagCopy.Equals(WantTag, ESearchCase::IgnoreCase))
	{
		return;
	}

	static TWeakObjectPtr<UWorld> ArmedExecWorld;
	if (ArmedExecWorld.Get() == World)
	{
		return;
	}
	ArmedExecWorld = World;

	float DelaySeconds = 8.f;
	FParse::Value(FCommandLine::Get(), TEXT("TraceExecAt="), DelaySeconds);
	DelaySeconds = FMath::Max(0.01f, DelaySeconds);

	TArray<FString> Commands;
	CommandList.ParseIntoArray(Commands, TEXT("|"), /*InCullEmpty=*/true);

	UE_LOG(LogTraceGame, Display, TEXT("[AutoExec] Armed (%s): %d command(s) in %.2fs."),
		*TagCopy, Commands.Num(), DelaySeconds);

	// One timer, not one per command: the commands are run in the order given, and several of them
	// (Trace.ModeB.Verify, Trace.Trail.TestHeadGap) schedule multi-second work of their own that
	// would interleave badly if they were started at staggered times.
	static FTimerHandle ExecTimer;
	World->GetTimerManager().SetTimer(ExecTimer,
		FTimerDelegate::CreateWeakLambda(OwnerHUD, [OwnerHUD, Commands]()
	{
		UWorld* ExecWorld = OwnerHUD->GetWorld();
		APlayerController* ExecPC = OwnerHUD->GetOwningPlayerController();
		for (const FString& Command : Commands)
		{
			const FString Trimmed = Command.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				continue;
			}
			UE_LOG(LogTraceGame, Display, TEXT("[AutoExec] > %s"), *Trimmed);
			// Through the player controller when there is one: several Trace.* commands resolve
			// "the local pawn" from the executing controller, and GEngine->Exec with a null player
			// hands them nothing to work with.
			if (ExecPC != nullptr)
			{
				ExecPC->ConsoleCommand(Trimmed, /*bWriteToLog=*/true);
			}
			else if (GEngine != nullptr)
			{
				GEngine->Exec(ExecWorld, *Trimmed);
			}
		}
	}), DelaySeconds, false);
}

#endif // !UE_BUILD_SHIPPING
