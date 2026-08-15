// Trace — see TraceHardwareCursor.h for the whole argument.

#include "UI/TraceHardwareCursor.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "UnrealClient.h"                 // FViewport

#include "Trace.h"                        // LogTraceGame
#include "UI/TraceMenuHUD.h"              // ATraceMenuHUD::IsOptionsOpen — see PollMenuSurfaces

// Named after the file, not anonymous. Scripts/check-jumbo-build-collisions.py, and the unity build
// it exists to protect: two files' anonymous namespaces become one under UBT's jumbo compilation.
namespace TraceHardwareCursorFile
{
	/**
	 * How many frames a lease survives without a renewal.
	 *
	 * TWO, not one, and the reason is ordering rather than slack. FTSTicker runs early in
	 * FEngineLoop::Tick; a renewal comes from a HUD draw, which happens near the END of the same
	 * frame. So the tick that judges a lease is always looking at the PREVIOUS frame's renewal, and a
	 * grace of one would expire a lease that is being renewed perfectly reliably, every frame, for
	 * ever. Measured the obvious way: at one, the pointer strobed.
	 */
	constexpr uint64 GraceFrames = 2;

	/** THE RED ARM. See TraceHardwareCursor::IsRedArmed. */
	static int32 GHardwareCursorRedArm = 0;

	static FAutoConsoleVariableRef CVarHardwareCursorRedArm(
		TEXT("Trace.UI.HardwareCursorRedArm"),
		GHardwareCursorRedArm,
		TEXT("Dev only. Spec v24 s2 RED ARM. 1 puts the OS pointer back on top of the artist's ")
		TEXT("cursor by never suppressing it, so one binary produces both arms. See ")
		TEXT("Trace.UI.CursorReport."),
		ECVF_Cheat);

	/** The controller this file has written CurrentMouseCursor on, so it can put it back exactly. */
	static TWeakObjectPtr<APlayerController> Applied;

	/** What CurrentMouseCursor was before the lease was taken. Restored verbatim. */
	static EMouseCursor::Type SavedCursor = EMouseCursor::Default;

	/** True while Applied's cursor is ours. */
	static bool bSuppressing = false;

	/** GFrameCounter of the most recent renewal. */
	static uint64 LastRenewFrame = 0;

	/** Static string naming who renewed last. Never freed; every caller passes a literal. */
	static const TCHAR* LeaseOwner = TEXT("nobody");

	/** Set once the expiry ticker is registered, which happens on the first renewal of a process. */
	static FTSTicker::FDelegateHandle TickerHandle;

	// ---- What the log says, and why it says it on CHANGE rather than once ------------------------
	//
	// The OS pointer is composited by the window server and never appears in a captured frame — that
	// is the whole reason every screen in this project draws its own — so a screenshot cannot show
	// this working or failing. The only honest evidence is the value the engine hands the platform,
	// and the only way to see it move across a session (title -> settings -> JOIN -> travel) is a
	// line each time the answer changes. Once-only would have shown the first state and hidden every
	// handover after it, which is exactly the interesting part.
	static bool bLoggedAtLeastOnce = false;
	static bool bLastLoggedSuppressing = false;
	static const TCHAR* LastLoggedOwner = TEXT("");

	static const TCHAR* CursorName(EMouseCursor::Type InType)
	{
		switch (InType)
		{
		case EMouseCursor::None:                 return TEXT("None");
		case EMouseCursor::Default:              return TEXT("Default");
		case EMouseCursor::TextEditBeam:         return TEXT("TextEditBeam");
		case EMouseCursor::ResizeLeftRight:      return TEXT("ResizeLeftRight");
		case EMouseCursor::ResizeUpDown:         return TEXT("ResizeUpDown");
		case EMouseCursor::ResizeSouthEast:      return TEXT("ResizeSouthEast");
		case EMouseCursor::ResizeSouthWest:      return TEXT("ResizeSouthWest");
		case EMouseCursor::CardinalCross:        return TEXT("CardinalCross");
		case EMouseCursor::Crosshairs:           return TEXT("Crosshairs");
		case EMouseCursor::Hand:                 return TEXT("Hand");
		case EMouseCursor::GrabHand:             return TEXT("GrabHand");
		case EMouseCursor::GrabHandClosed:       return TEXT("GrabHandClosed");
		case EMouseCursor::SlashedCircle:        return TEXT("SlashedCircle");
		case EMouseCursor::EyeDropper:           return TEXT("EyeDropper");
		default:                                 return TEXT("(other)");
		}
	}

	/**
	 * The local controller this process's pointer belongs to, found from the engine rather than
	 * remembered from a caller.
	 *
	 * Found fresh every tick on purpose: a travel destroys the controller and builds a new one, and a
	 * remembered pointer is exactly how a lease outlives the screen that took it.
	 *
	 * The FIRST game/PIE world with a local controller wins. That is also what
	 * UGameViewportClient::GetCursor consults (GameInstance->GetFirstLocalPlayerController()), so the
	 * controller written here is the controller the engine will read back.
	 */
	static APlayerController* FindLocalController()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
			{
				continue;
			}
			UWorld* World = Context.World();
			if (World == nullptr)
			{
				continue;
			}
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				if (PC->IsLocalController())
				{
					return PC;
				}
			}
		}
		return nullptr;
	}

	/** Restore whatever was there before. Safe to call when nothing is held. */
	static void Restore()
	{
		if (!bSuppressing)
		{
			return;
		}
		bSuppressing = false;

		// A null weak pointer means the controller was destroyed — a travel, or shutdown. There is
		// nothing to put back and nothing is leaking: the new controller starts at its own default.
		if (APlayerController* PC = Applied.Get())
		{
			PC->CurrentMouseCursor = SavedCursor;
		}
		Applied.Reset();
	}

	/** Take the lease on @p PC, saving what was there. Idempotent while it is already held on @p PC. */
	static void Take(APlayerController* PC)
	{
		if (PC == nullptr)
		{
			return;
		}

		if (bSuppressing && Applied.Get() != PC)
		{
			// A different controller than the one we hold: put the old one back before touching this
			// one, so a travel cannot leave a stale value behind on a controller nobody looks at.
			Restore();
		}

		if (!bSuppressing)
		{
			SavedCursor = PC->CurrentMouseCursor;
			Applied = PC;
			bSuppressing = true;
		}

		// Re-asserted every frame rather than set once. Nothing else in this project writes
		// CurrentMouseCursor today, but a widget or a future input mode that did would otherwise win
		// silently and the two pointers would come back with no code change to blame.
		PC->CurrentMouseCursor = EMouseCursor::None;
	}

	/**
	 * The one surface that cannot renew its own lease this pass: the title screen's SETTINGS modal.
	 *
	 * FTraceOptionsMenu::DrawCursor draws the artist's arrow on both the title screen and the in-match
	 * pause menu, and UI/TraceOptionsMenu.cpp belongs to another slice of spec v24 — so instead of
	 * editing it, this asks the question from outside through ATraceMenuHUD::IsOptionsOpen(), which is
	 * already public and already documented as "true while the settings overlay owns the screen".
	 *
	 * THE JOIN PROMPT IS DELIBERATELY NOT LISTED HERE, and that is the whole reason this is a
	 * whitelist rather than "a modal is up". ATraceMenuHUD::DrawJoinPrompt draws a scrim, a panel and
	 * a text field and NO pointer of any kind — it is keyboard-only — while the title screen's own
	 * arrow is hidden for exactly those frames (see BuildMenuView's bModalOwnsScreen). Suppressing the
	 * hardware cursor there would leave the player with nothing to point with at all, which spec §2
	 * names as the worse bug. So JOIN gets the OS arrow back, and that is correct rather than missed.
	 *
	 * The in-match pause / settings overlay does NOT need to be here: FTraceCharacterSelect::Tick runs
	 * every frame of every match and is already told when something in front of it owns the keyboard.
	 */
	static bool PollMenuSurfaces(APlayerController* PC)
	{
		if (PC == nullptr)
		{
			return false;
		}
		const ATraceMenuHUD* MenuHUD = Cast<ATraceMenuHUD>(PC->GetHUD());
		return MenuHUD != nullptr && MenuHUD->IsOptionsOpen();
	}

	static bool Tick(float /*DeltaSeconds*/)
	{
		APlayerController* PC = FindLocalController();

		// The red arm falls THROUGH to the logging below rather than returning early. An arm that
		// silences the very line it is meant to flip is an arm that proves nothing — the first draft
		// of this function returned here and the red run simply went quiet, which reads exactly like
		// a run where nothing happened.
		const bool bRedArmed = GHardwareCursorRedArm != 0;

		const bool bLeaseFresh = (GFrameCounter - LastRenewFrame) <= GraceFrames;
		const bool bPolled = !bRedArmed && PollMenuSurfaces(PC);

		if (bPolled)
		{
			LeaseOwner = TEXT("settings modal (polled)");
		}

		if (!bRedArmed && (bLeaseFresh || bPolled) && PC != nullptr)
		{
			Take(PC);
		}
		else
		{
			Restore();
		}

		if (!bLoggedAtLeastOnce || bSuppressing != bLastLoggedSuppressing
			|| (bSuppressing && LeaseOwner != LastLoggedOwner))
		{
			bLoggedAtLeastOnce = true;
			bLastLoggedSuppressing = bSuppressing;
			LastLoggedOwner = LeaseOwner;

			UE_LOG(LogTraceGame, Display, TEXT("[Cursor] OS pointer %s — %s"),
				bSuppressing ? TEXT("HIDDEN") : TEXT("VISIBLE"),
				bSuppressing ? LeaseOwner
					: bRedArmed ? TEXT("RED ARM: Trace.UI.HardwareCursorRedArm is on, so the OS ")
					              TEXT("pointer is drawn over the artist's — this is the reported defect")
					: TEXT("nothing is drawing one of ours"));
		}
		return true;
	}

	static void EnsureTicker()
	{
		if (!TickerHandle.IsValid())
		{
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateStatic(&TraceHardwareCursorFile::Tick), 0.f);
		}
	}
}

void TraceHardwareCursor::EnsureRunning()
{
	TraceHardwareCursorFile::EnsureTicker();
}

void TraceHardwareCursor::RenewSuppression(APlayerController* InPC, const TCHAR* InOwner)
{
	TraceHardwareCursorFile::EnsureTicker();

	if (TraceHardwareCursorFile::GHardwareCursorRedArm != 0)
	{
		TraceHardwareCursorFile::Restore();
		return;
	}

	if (InPC == nullptr)
	{
		// A caller that has momentarily lost its controller pointer has NOT stopped drawing its
		// pointer, and letting the lease lapse for that reason would strobe the OS arrow back on for
		// a frame. Ask the engine for the same controller the ticker would have used.
		InPC = TraceHardwareCursorFile::FindLocalController();
	}

	if (InPC == nullptr || !InPC->IsLocalController())
	{
		return;
	}

	TraceHardwareCursorFile::LastRenewFrame = GFrameCounter;
	TraceHardwareCursorFile::LeaseOwner = (InOwner != nullptr) ? InOwner : TEXT("(unnamed)");

	// Applied here as well as in the ticker so the first frame a menu comes up is already right. The
	// ticker's job is the RELEASE edge, which is the one nobody can be trusted to remember.
	TraceHardwareCursorFile::Take(InPC);
}

void TraceHardwareCursor::ReleaseNow()
{
	TraceHardwareCursorFile::LastRenewFrame = 0;
	TraceHardwareCursorFile::LeaseOwner = TEXT("nobody");
	TraceHardwareCursorFile::Restore();
}

bool TraceHardwareCursor::IsRedArmed()
{
	return TraceHardwareCursorFile::GHardwareCursorRedArm != 0;
}

FString TraceHardwareCursor::Describe()
{
	using namespace TraceHardwareCursorFile;

	APlayerController* PC = Applied.Get();
	if (PC == nullptr)
	{
		PC = FindLocalController();
	}

	FString Out = FString::Printf(
		TEXT("redarm=%d  suppressing=%d  owner=%s  frames since renewal=%llu (grace %llu)"),
		GHardwareCursorRedArm, bSuppressing ? 1 : 0, LeaseOwner,
		static_cast<unsigned long long>(GFrameCounter - LastRenewFrame),
		static_cast<unsigned long long>(GraceFrames));

	if (PC == nullptr)
	{
		return Out + TEXT("  | no local controller");
	}

	Out += FString::Printf(TEXT("\n    controller: bShowMouseCursor=%d  CurrentMouseCursor=%s  GetMouseCursor()=%s"),
		PC->bShowMouseCursor ? 1 : 0, CursorName(PC->CurrentMouseCursor), CursorName(PC->GetMouseCursor()));

	// The value that actually decides what the player sees. Everything above is an input to it.
	UGameViewportClient* GameViewport = (PC->GetWorld() != nullptr) ? PC->GetWorld()->GetGameViewport() : nullptr;
	FViewport* Viewport = (GameViewport != nullptr) ? GameViewport->Viewport : nullptr;
	const bool bSlateActive = FSlateApplication::IsInitialized() && FSlateApplication::Get().IsActive();

	if (GameViewport != nullptr && Viewport != nullptr)
	{
		float MouseX = 0.f;
		float MouseY = 0.f;
		PC->GetMousePosition(MouseX, MouseY);
		const EMouseCursor::Type Handed =
			GameViewport->GetCursor(Viewport, FMath::RoundToInt(MouseX), FMath::RoundToInt(MouseY));

		Out += FString::Printf(
			TEXT("\n    viewport: SlateActive=%d  HasFocus=%d  HasMouseCapture=%d")
			TEXT("\n    ENGINE HANDS THE PLATFORM: %s   <- this is what the player sees"),
			bSlateActive ? 1 : 0, Viewport->HasFocus() ? 1 : 0, Viewport->HasMouseCapture() ? 1 : 0,
			CursorName(Handed));
	}
	else
	{
		Out += FString::Printf(TEXT("\n    viewport: none (SlateActive=%d)"), bSlateActive ? 1 : 0);
	}

	return Out;
}

#if !UE_BUILD_SHIPPING
namespace TraceHardwareCursorCommands
{
	/**
	 * `Trace.UI.CursorReport` — the measurement behind spec v24 §2.
	 *
	 * A screenshot cannot settle this: the operating system's pointer is composited by the window
	 * server and never appears in a captured frame, which is the very reason every menu in this
	 * project draws its own. So the proof has to be the value the engine hands the platform, and this
	 * prints exactly that, alongside every input the engine's decision is made from.
	 *
	 * Run it with `Trace.UI.HardwareCursorRedArm 1` for the other arm.
	 */
	static FAutoConsoleCommand CmdCursorReport(
		TEXT("Trace.UI.CursorReport"),
		TEXT("Dev only. Prints who owns the hardware pointer right now and what cursor the engine is ")
		TEXT("handing the platform. See Trace.UI.HardwareCursorRedArm."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			UE_LOG(LogTraceGame, Display, TEXT("=============== Trace.UI.CursorReport ==============="));
			UE_LOG(LogTraceGame, Display, TEXT("[Cursor] %s"), *TraceHardwareCursor::Describe());
			UE_LOG(LogTraceGame, Display, TEXT("====================================================="));
		}));
}
#endif
