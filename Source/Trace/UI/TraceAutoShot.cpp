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

	/**
	 * *** WHY THERE ARE TWO ROUNDS AND NOT ONE LONGER COMMAND LIST. ***
	 *
	 * Every command of one round runs inside ONE timer callback, back to back, in the same frame
	 * (see ArmDeferredExec: "one timer, not one per command"). That is exactly right for a batch of
	 * verifiers which only read state, and exactly wrong for a pair where the first command CHANGES
	 * state the second one has to observe. The measured case is the character census
	 * (PIPELINE_DESIGN.md §9.3): `Trace.Characters.Select 4` asks for a character, the pawn's body is
	 * applied on a later POLL tick (UpdateCharacterBodyMesh), and `Trace.Characters.BodyMesh` run in
	 * the same callback photographs the body the pawn was already wearing — a FAIL on a healthy
	 * build, or worse, a PASS that proves nothing.
	 *
	 * So a round is "commands that may run together", and a run that needs a settle gap between two
	 * groups asks for a second round at its own -TraceExec2At. Two is enough for every case this
	 * project has: the third group is a screenshot, and -TraceAutoShot already owns its own clock.
	 *
	 * The rounds are INDEPENDENT — each has its own armed-world guard and its own timer handle — so
	 * arming round 2 without round 1 is legal, and a run that gives neither pays two FParse calls.
	 */
	struct FTraceDeferredExecRound
	{
		TWeakObjectPtr<UWorld> ArmedWorld;
		FTimerHandle Timer;
	};

	/** Round 0 = -TraceExec/-TraceExecOn/-TraceExecAt; round 1 = the -TraceExec2 family. */
	FTraceDeferredExecRound& DeferredExecRound(int32 Round)
	{
		static FTraceDeferredExecRound Rounds[2];
		return Rounds[FMath::Clamp(Round, 0, 1)];
	}

	/**
	 * Arms ONE round. Returns the delay it armed at, or a negative number when it armed nothing —
	 * which is what lets round 2's default delay be stated relative to round 1's rather than as a
	 * second magic number that could silently be the earlier of the two.
	 */
	float ArmOneDeferredExecRound(AHUD* OwnerHUD, UWorld* World, const FString& TagCopy,
		int32 Round, float DefaultDelaySeconds)
	{
		// The switch names are the ONLY thing that differs between the rounds, so they are the only
		// thing branched on. Everything below this point is the single implementation both rounds run,
		// which is what stops round 2 from drifting into a near-copy of round 1 with its own bugs.
		const TCHAR* const ListSwitch = (Round == 0) ? TEXT("TraceExec=")   : TEXT("TraceExec2=");
		const TCHAR* const TagSwitch  = (Round == 0) ? TEXT("TraceExecOn=") : TEXT("TraceExec2On=");
		const TCHAR* const AtSwitch   = (Round == 0) ? TEXT("TraceExecAt=") : TEXT("TraceExec2At=");

		// FParse::Value matches the switch name up to and including its '=', so "TraceExec=" cannot
		// be satisfied by "-TraceExec2=", by "-TraceExecOn=" or by "-TraceExecAt=". The four-switch
		// family is unambiguous by construction and needs no ordering rule on the command line.
		FString CommandList;
		if (!FParse::Value(FCommandLine::Get(), ListSwitch, CommandList) || CommandList.IsEmpty())
		{
			// Switch absent: one FParse and out, same as the screenshot harness.
			return -1.f;
		}

		// Which HUD this round is aimed at. Defaulting to "Match" is the safe half of the choice:
		// almost every command worth deferring needs a live match, and firing one at the title screen
		// produces a confusing "no world" refusal that reads like the command is broken.
		FString WantTag;
		if (!FParse::Value(FCommandLine::Get(), TagSwitch, WantTag) || WantTag.IsEmpty())
		{
			WantTag = TEXT("Match");
		}
		if (!TagCopy.Equals(WantTag, ESearchCase::IgnoreCase))
		{
			return -1.f;
		}

		FTraceDeferredExecRound& State = DeferredExecRound(Round);
		if (State.ArmedWorld.Get() == World)
		{
			return -1.f;
		}
		State.ArmedWorld = World;

		float DelaySeconds = DefaultDelaySeconds;
		FParse::Value(FCommandLine::Get(), AtSwitch, DelaySeconds);
		DelaySeconds = FMath::Max(0.01f, DelaySeconds);

		TArray<FString> Commands;
		CommandList.ParseIntoArray(Commands, TEXT("|"), /*InCullEmpty=*/true);

		UE_LOG(LogTraceGame, Display, TEXT("[AutoExec] Armed (%s, round %d): %d command(s) in %.2fs."),
			*TagCopy, Round + 1, Commands.Num(), DelaySeconds);

		// One timer per ROUND, not one per command: the commands of a round are run in the order
		// given, and several of them (Trace.ModeB.Verify, Trace.Trail.TestHeadGap) schedule
		// multi-second work of their own that would interleave badly if they were started at
		// staggered times. Commands that must NOT share a frame go in the other round.
		World->GetTimerManager().SetTimer(State.Timer,
			FTimerDelegate::CreateWeakLambda(OwnerHUD, [OwnerHUD, Commands, Round]()
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
				UE_LOG(LogTraceGame, Display, TEXT("[AutoExec] %d> %s"), Round + 1, *Trimmed);
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

		return DelaySeconds;
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
		// alone.
		//
		// *** bShowUI IS TRUE, AND IT WAS FALSE UNTIL SPEC v17 §4. MEASURED, NOT ASSUMED. ***
		//
		// The old comment here said "bShowUI = false keeps engine on-screen debug text out of the
		// capture; the HUD itself is drawn into the scene's back buffer and IS captured". The first
		// half is still true and the second half stopped being true in this pass. bShowUI=false does
		// not filter debug text — it excludes the ENTIRE SLATE LAYER. That was invisible while every
		// pixel of UI in this project came from AHUD::DrawHUD (Canvas, drawn into the scene), which
		// is why nobody noticed. Spec v17 §4 moved the title menu and the HUD's ammo/status corner
		// onto UMG, and a UMG widget IS a Slate widget composited after the scene.
		//
		// The step-4b agent reproduced the failure rather than reasoning about it: a frame whose draw
		// record said the corner had drawn showed an EMPTY corner, with the Canvas score panel and
		// health bar plainly visible in the same image. Left at false, -TraceAutoShot would silently
		// photograph the game with its new menu and its new HUD corner MISSING, and every screenshot
		// it produced would be evidence for a conclusion that is not true. This project has been
		// burned three times by self-certifying evidence; a harness that cannot see what it is
		// testing is exactly that.
		//
		// Cost of the change: engine on-screen debug text (stat displays, "Ctrl+Shift+, "-style
		// overlays) can now appear in a capture. That is a cosmetic nuisance in a harness that runs
		// -unattended with no debug displays enabled, and it is the strictly smaller risk.
		// ATraceHUD's own Trace.HUD.V16Shots harness made the identical change for the identical
		// reason, and its Canvas-path frames were checked to be unaffected.
		FScreenshotRequest::RequestScreenshot(State.PendingPath, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);

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

	const FString TagCopy(Tag != nullptr ? Tag : TEXT(""));

	// ROUND 1 first, because round 2's DEFAULT is stated relative to it. 8 s is the shipped default
	// and the number every existing -TraceExec run on this project was written against.
	constexpr float FirstRoundDefaultSeconds = 8.f;
	const float FirstDelay = ArmOneDeferredExecRound(OwnerHUD, World, TagCopy, 0, FirstRoundDefaultSeconds);

	// *** ROUND 2 DEFAULTS TO TEN SECONDS AFTER ROUND 1, NOT TO A NUMBER OF ITS OWN. ***
	// The entire reason round 2 exists is to land AFTER round 1 has settled, so a fixed default would
	// be a standing invitation to write -TraceExecAt=40 -TraceExec2="..." and get the two rounds in
	// the wrong order — silently, with a plausible-looking log. Deriving it means the ordering holds
	// whatever round 1 was moved to, and an author who wants a different gap says so with
	// -TraceExec2At. Ten seconds is the census gap of PIPELINE_DESIGN.md §9.3 (20 -> 30), which is
	// the run this round was added for.
	//
	// When round 1 armed nothing (absent, or aimed at the other HUD) there is nothing to be after, so
	// round 2 falls back to round 1's own default and behaves exactly like a lone -TraceExec.
	constexpr float SecondRoundGapSeconds = 10.f;
	const float SecondRoundDefault = (FirstDelay > 0.f)
		? (FirstDelay + SecondRoundGapSeconds)
		: FirstRoundDefaultSeconds;
	ArmOneDeferredExecRound(OwnerHUD, World, TagCopy, 1, SecondRoundDefault);
}

#endif // !UE_BUILD_SHIPPING
