// Trace — title screen implementation. See TraceMenuHUD.h.

#include "UI/TraceMenuHUD.h"

#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"             // FViewport::IsForegroundWindow
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreMiscDefines.h"     // FInputDeviceId
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "Settings/TraceUserSettings.h"
#include "TimerManager.h"
#include "Trace.h"                    // LogTraceGame
#include "UI/TraceAutoShot.h"
#include "UI/TraceMatchOptions.h"

// =================================================================================================
// Palette and layout
//
// Authored against a 1080p-tall viewport; every pixel constant below is multiplied by UIScale.
// The whole screen is two hues — a cyan that carries the interface and an amber that only ever
// marks danger or the Hard setting — over near-black. That restraint is the entire look: the
// moment a third colour appears, the grid stops reading as a grid and starts reading as noise.
// =================================================================================================

namespace TraceMenuStyle
{
	static constexpr float ReferenceHeight = 1080.f;

	static const FLinearColor Void      (0.006f, 0.011f, 0.022f, 1.00f);
	static const FLinearColor Cyan      (0.16f,  0.88f,  1.00f,  1.00f);
	static const FLinearColor CyanDeep  (0.04f,  0.34f,  0.48f,  1.00f);
	static const FLinearColor Amber     (1.00f,  0.46f,  0.08f,  1.00f);
	static const FLinearColor Ink       (0.90f,  0.97f,  1.00f,  1.00f);
	static const FLinearColor InkDim    (0.42f,  0.58f,  0.66f,  1.00f);

	/** Row geometry, in reference pixels. */
	static constexpr float RowHeight  = 62.f;
	static constexpr float RowSpacing = 78.f;
	static constexpr float RowPadX    = 30.f;

	/**
	 * Where the grid floor meets the dark. Shared by the backdrop glow and the grid itself so the
	 * glow can never end somewhere the horizon line is not.
	 */
	static constexpr float HorizonFraction = 0.66f;

	/**
	 * Seconds after the title screen appears during which Enter/Space will not activate a row.
	 *
	 * Shortened from 0.75s now that the mouse path — which was the actual bug — is fixed at its
	 * cause by press/release semantics plus foreground-click suppression. See
	 * ATraceMenuHUD::AcceptUnlockTime.
	 */
	static constexpr float ActivationGraceSeconds = 0.35f;

	static FLinearColor WithAlpha(const FLinearColor& C, float A)
	{
		return FLinearColor(C.R, C.G, C.B, A);
	}

	/** Colour that marks each difficulty, so the setting is legible without reading the word. */
	static FLinearColor DifficultyColor(ETraceBotDifficulty Difficulty)
	{
		switch (Difficulty)
		{
		case ETraceBotDifficulty::Easy: return FLinearColor(0.30f, 1.00f, 0.72f, 1.f);
		case ETraceBotDifficulty::Hard: return Amber;
		default:                        return Cyan;
		}
	}

	static FString DifficultyBlurb(ETraceBotDifficulty Difficulty)
	{
		switch (Difficulty)
		{
		case ETraceBotDifficulty::Easy:
			return TEXT("BOTS REACT SLOWLY AND SHOOT LOOSELY.  START HERE.");
		case ETraceBotDifficulty::Hard:
			return TEXT("BOTS REACT FAST, AIM TIGHT AND PUSH THE CORE.");
		default:
			return TEXT("BOTS PLAY THE SHIPPED TUNING.  A FAIR FIGHT.");
		}
	}
}

// =================================================================================================
// Stroke font
//
// Five letters and a space, drawn as line segments in a unit box (x and y both 0..1, y downward).
// The built-in engine fonts are bitmaps; at the size a title wants they are a blurry mess, and
// there is no .uasset budget for a real typeface (contract: no assets). Segments cost nothing,
// stay sharp at any resolution, and look like something a light cycle would drive on.
// =================================================================================================

namespace TraceStrokeFont
{
	struct FSeg { float X0, Y0, X1, Y1; };

	/** Glyph box aspect and letter tracking, both as multiples of cap height. */
	static constexpr float GlyphWidth = 0.62f;
	static constexpr float Tracking   = 0.30f;

	static const FSeg SegsT[] = { {0.f,0.f, 1.f,0.f}, {0.5f,0.f, 0.5f,1.f} };
	static const FSeg SegsR[] = {
		{0.f,1.f, 0.f,0.f}, {0.f,0.f, 0.74f,0.f}, {0.74f,0.f, 0.9f,0.14f},
		{0.9f,0.14f, 0.9f,0.4f}, {0.9f,0.4f, 0.74f,0.54f}, {0.74f,0.54f, 0.f,0.54f},
		{0.46f,0.54f, 0.95f,1.f} };
	static const FSeg SegsA[] = { {0.f,1.f, 0.5f,0.f}, {0.5f,0.f, 1.f,1.f}, {0.17f,0.66f, 0.83f,0.66f} };
	static const FSeg SegsC[] = {
		{1.f,0.f, 0.16f,0.f}, {0.16f,0.f, 0.f,0.16f}, {0.f,0.16f, 0.f,0.84f},
		{0.f,0.84f, 0.16f,1.f}, {0.16f,1.f, 1.f,1.f} };
	static const FSeg SegsE[] = { {1.f,0.f, 0.f,0.f}, {0.f,0.f, 0.f,1.f}, {0.f,1.f, 1.f,1.f}, {0.f,0.5f, 0.78f,0.5f} };

	/** Null segments + zero count is a legal glyph: it advances and draws nothing (space, unknown). */
	struct FGlyph { const FSeg* Segs; int32 Num; };

	static FGlyph Find(TCHAR Char)
	{
		switch (Char)
		{
		case TEXT('T'): return { SegsT, UE_ARRAY_COUNT(SegsT) };
		case TEXT('R'): return { SegsR, UE_ARRAY_COUNT(SegsR) };
		case TEXT('A'): return { SegsA, UE_ARRAY_COUNT(SegsA) };
		case TEXT('C'): return { SegsC, UE_ARRAY_COUNT(SegsC) };
		case TEXT('E'): return { SegsE, UE_ARRAY_COUNT(SegsE) };
		default:        return { nullptr, 0 };
		}
	}
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void ATraceMenuHUD::BeginPlay()
{
	Super::BeginPlay();

	// The title screen chose Easy for the player; make that true of the settings straight away so
	// the value on screen is never a promise the match fails to keep. StartMatch() re-applies it
	// (and the arena's game mode applies whatever arrives in the URL), so this is belt and braces.
	TraceDifficulty::ApplyToSettings(Difficulty);

	// The mode goes the other way: it is READ from the settings rather than pushed to them, so a
	// player who has run three mode-B matches comes back to the title screen still on mode B. The
	// settings CDO is the single storage for the toggle (see TraceScoring::ApplyToSettings), so
	// there is nothing to reconcile — this is a load, not a second copy.
	ScoringMode = TraceScoring::GetCurrentSetting();

	UE_LOG(LogTraceGame, Log, TEXT("Title screen up. Difficulty %s, %s."),
		*TraceDifficulty::ToDisplayName(Difficulty), *TraceScoringModeLabel(ScoringMode));

	// See AcceptUnlockTime in the header. Long enough to outlast window creation and focus
	// acquisition, short enough that a player reaching straight for Enter never notices it.
	if (const UWorld* World = GetWorld())
	{
		TitleShownTime = World->GetTimeSeconds();
		AcceptUnlockTime = TitleShownTime + TraceMenuStyle::ActivationGraceSeconds;
	}

#if !UE_BUILD_SHIPPING
	TraceAutoShot::Arm(this, TEXT("Menu"));
	ArmAutoPlay();
	ArmAutoSettings();
#endif
}

#if !UE_BUILD_SHIPPING
namespace
{
	/**
	 * The scripted drive for -TraceAutoSettings, as (key, description) pairs.
	 *
	 * Chosen to touch one of each row kind exactly once, because a sequence that exercises three
	 * sliders proves no more than one that exercises one:
	 *   4 x Right  -> SENSITIVITY 1.00 -> 1.20        (Slider, and the held-key adjust path)
	 *   2 x Down   -> INVERT MOUSE Y                  (navigation, skipping nothing yet)
	 *   Enter      -> toggles it ON                   (Toggle)
	 *   2 x Down   -> MOVE FORWARD                    (navigation ACROSS the CONTROLS header)
	 *   Enter      -> arms the rebind capture         (Binding)
	 *   K          -> becomes the new MOVE FORWARD    (capture)
	 */
	struct FAutoSettingsKey { FKey (*Key)(); const TCHAR* What; };

	const TArray<FAutoSettingsKey>& AutoSettingsScript()
	{
		static const TArray<FAutoSettingsKey> Script =
		{
			{ []{ return EKeys::Right; }, TEXT("sensitivity +") },
			{ []{ return EKeys::Right; }, TEXT("sensitivity +") },
			{ []{ return EKeys::Right; }, TEXT("sensitivity +") },
			{ []{ return EKeys::Right; }, TEXT("sensitivity +") },
			{ []{ return EKeys::Down;  }, TEXT("-> vertical sensitivity") },
			{ []{ return EKeys::Down;  }, TEXT("-> invert mouse y") },
			{ []{ return EKeys::Enter; }, TEXT("toggle invert y") },
			{ []{ return EKeys::Down;  }, TEXT("-> move forward (across the CONTROLS header)") },
			{ []{ return EKeys::Enter; }, TEXT("arm rebind capture") },
			{ []{ return EKeys::K;     }, TEXT("bind move forward to K") },
		};
		return Script;
	}

	/** Feeds one key edge through the same entry point a physical keyboard uses. */
	void InjectKey(APlayerController* PC, const FKey& Key, bool bPressed)
	{
		if (PC == nullptr)
		{
			return;
		}

		// Internal id 0 rather than IPlatformInputDeviceMapper::GetDefaultInputDevice(): that lives in
		// the ApplicationCore module, which this module deliberately does not depend on (see
		// Trace.Build.cs). Desktop platforms map the keyboard and mouse to id 0, and this injector
		// only ever runs on one.
		const FInputKeyEventArgs Args(
			/*Viewport*/ nullptr,
			FInputDeviceId::CreateFromInternalId(0),
			Key,
			bPressed ? IE_Pressed : IE_Released,
			/*AmountDepressed*/ bPressed ? 1.f : 0.f,
			/*bIsTouchEvent*/ false,
			FPlatformTime::Cycles64());

		PC->InputKey(Args);
	}
}
#endif

#if !UE_BUILD_SHIPPING
void ATraceMenuHUD::ArmAutoPlay()
{
	float DelaySeconds = 0.f;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TraceAutoPlay="), DelaySeconds))
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// An explicit difficulty for the automated run, so a headless launch can exercise Hard without
	// somebody having to fake three key presses.
	FString DifficultyArg;
	if (FParse::Value(FCommandLine::Get(), TEXT("TraceMenuDifficulty="), DifficultyArg))
	{
		Difficulty = TraceDifficulty::FromUrlValue(DifficultyArg);
	}

	// And an explicit scoring mode, for the same reason: an A/B toggle whose second half can only be
	// reached by a human pressing an arrow key is an A/B toggle that never gets tested headlessly.
	// "-TraceMenuScoringMode=b" drives the whole menu -> mode B match -> results -> menu loop.
	FString ScoringModeArg;
	if (FParse::Value(FCommandLine::Get(), TEXT("TraceMenuScoringMode="), ScoringModeArg))
	{
		ScoringMode = TraceScoringModeFromUrlValue(ScoringModeArg);
		TraceScoring::ApplyToSettings(ScoringMode);
	}

	DelaySeconds = FMath::Max(0.01f, DelaySeconds);
	UE_LOG(LogTraceGame, Display, TEXT("[AutoPlay] Pressing PLAY in %.2fs at difficulty %s, %s."),
		DelaySeconds, *TraceDifficulty::ToDisplayName(Difficulty), *TraceScoringModeLabel(ScoringMode));

	World->GetTimerManager().SetTimer(AutoPlayTimer,
		FTimerDelegate::CreateWeakLambda(this, [this]() { StartMatch(); }), DelaySeconds, false);
}

void ATraceMenuHUD::ArmAutoSettings()
{
	float DelaySeconds = 0.f;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TraceAutoSettings="), DelaySeconds))
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	DelaySeconds = FMath::Max(0.01f, DelaySeconds);
	UE_LOG(LogTraceGame, Display, TEXT("[AutoSettings] Opening SETTINGS in %.2fs and driving it."), DelaySeconds);

	World->GetTimerManager().SetTimer(AutoSettingsTimer, FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		Selected = ETraceMenuRow::Settings;
		OpenOptions();

		AutoSettingsIndex = 0;
		bAutoSettingsAwaitingRelease = false;

		// 0.22s a step: comfortably longer than a frame so each press is seen, and comfortably
		// SHORTER than FTraceOptionsMenu::RepeatDelay (0.38s) so no held key auto-repeats and the
		// script's arithmetic stays exact.
		if (UWorld* TimerWorld = GetWorld())
		{
			TimerWorld->GetTimerManager().SetTimer(AutoSettingsStepTimer,
				FTimerDelegate::CreateWeakLambda(this, [this]() { AutoSettingsStep(); }), 0.22f, true);
		}
	}), DelaySeconds, false);
}

void ATraceMenuHUD::AutoSettingsStep()
{
	APlayerController* PC = GetOwningPlayerController();
	const TArray<FAutoSettingsKey>& Script = AutoSettingsScript();

	if (bAutoSettingsAwaitingRelease)
	{
		InjectKey(PC, Script[AutoSettingsIndex].Key(), /*bPressed=*/false);
		bAutoSettingsAwaitingRelease = false;
		++AutoSettingsIndex;
	}
	else if (Script.IsValidIndex(AutoSettingsIndex))
	{
		UE_LOG(LogTraceGame, Display, TEXT("[AutoSettings] step %d: %s"),
			AutoSettingsIndex, Script[AutoSettingsIndex].What);
		InjectKey(PC, Script[AutoSettingsIndex].Key(), /*bPressed=*/true);
		bAutoSettingsAwaitingRelease = true;
	}

	if (!Script.IsValidIndex(AutoSettingsIndex))
	{
		// Report the end state at Display. This line IS the test result: if it does not read
		// sensitivity=1.20 invertY=1 moveForward=K, the settings path did not work.
		const UTraceUserSettings& Settings = UTraceUserSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[AutoSettings] DONE. sensitivity=%.2f yScale=%.2f invertY=%d moveForward=%s dash=%s"),
			Settings.MouseSensitivity, Settings.MouseSensitivityYScale, Settings.bInvertMouseY ? 1 : 0,
			*UTraceUserSettings::DescribeKey(Settings.GetKey(ETraceInputAction::MoveForward)),
			*UTraceUserSettings::DescribeKey(Settings.GetKey(ETraceInputAction::Dash)));

		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(AutoSettingsStepTimer);
		}
	}
}
#endif

// =================================================================================================
// Input entry points
// =================================================================================================

void ATraceMenuHUD::MoveSelection(int32 Delta)
{
	// The overlay polls its own input (see FTraceOptionsMenu). These handlers are still wired to the
	// controller's key bindings and would otherwise move the selection UNDERNEATH the settings panel.
	if (OptionsMenu.IsOpen() || bTravelling || Delta == 0)
	{
		return;
	}

	const int32 Next = FMath::Clamp(static_cast<int32>(Selected) + Delta, 0, static_cast<int32>(ETraceMenuRow::Count) - 1);
	Selected = static_cast<ETraceMenuRow>(Next);
}

void ATraceMenuHUD::AdjustSelection(int32 Delta)
{
	if (OptionsMenu.IsOpen() || bTravelling || Delta == 0)
	{
		return;
	}

	if (Selected == ETraceMenuRow::Difficulty)
	{
		Difficulty = TraceDifficulty::Step(Difficulty, Delta);
		TraceDifficulty::ApplyToSettings(Difficulty);
		return;
	}

	if (Selected == ETraceMenuRow::Mode)
	{
		// Applied on every change rather than only on PLAY, so the value on screen is never a
		// promise the match fails to keep — the same reason the difficulty does it.
		ScoringMode = TraceScoringModeStep(ScoringMode, Delta);
		TraceScoring::ApplyToSettings(ScoringMode);
	}
}

bool ATraceMenuHUD::AcceptsActivation() const
{
	const UWorld* World = GetWorld();
	return (World == nullptr) || (World->GetTimeSeconds() >= AcceptUnlockTime);
}

void ATraceMenuHUD::ActivateSelection()
{
	if (OptionsMenu.IsOpen() || bTravelling)
	{
		return;
	}

	if (!AcceptsActivation())
	{
		// A press this early is not the player; it is the window taking focus. Swallow it rather
		// than skipping the title screen the player never got to see.
		UE_LOG(LogTraceGame, Display,
			TEXT("[MenuInput] Activation ignored: within the %.2fs grace period after the title screen appeared."),
			TraceMenuStyle::ActivationGraceSeconds);
		return;
	}

	switch (Selected)
	{
	case ETraceMenuRow::Play:
		StartMatch();
		break;

	case ETraceMenuRow::Difficulty:
		// Activating the row cycles it. Wraps, unlike the arrow keys: a click has no "other
		// direction" to offer, so stopping dead at HARD would just look broken.
		Difficulty = static_cast<ETraceBotDifficulty>((static_cast<int32>(Difficulty) + 1) % TraceDifficulty::Count);
		TraceDifficulty::ApplyToSettings(Difficulty);
		break;

	case ETraceMenuRow::Mode:
		// Same rule as DIFFICULTY: a click has no "other direction" to offer, so activating the row
		// wraps instead of stopping dead at the far end. With two modes this is simply a toggle,
		// which is exactly what an A/B switch should feel like to click.
		ScoringMode = static_cast<ETraceScoringMode>(
			(static_cast<int32>(ScoringMode) + 1) % TraceScoringModeCount);
		TraceScoring::ApplyToSettings(ScoringMode);
		break;

	case ETraceMenuRow::Settings:
		OpenOptions();
		break;

	case ETraceMenuRow::Quit:
		QuitGame();
		break;

	default:
		break;
	}
}

void ATraceMenuHUD::CancelPressed()
{
	if (OptionsMenu.IsOpen())
	{
		// The overlay owns Escape while it is up, and closes itself on it.
		return;
	}

	// Escape on the row it already highlights would be a trap, so move the highlight first: the
	// player sees what they are about to confirm.
	if (Selected != ETraceMenuRow::Quit)
	{
		Selected = ETraceMenuRow::Quit;
		return;
	}

	QuitGame();
}

bool ATraceMenuHUD::GetCursorPoint(FVector2D& OutPoint) const
{
	APlayerController* PC = GetOwningPlayerController();
	if (PC == nullptr)
	{
		return false;
	}

	float MouseX = 0.f;
	float MouseY = 0.f;
	if (!PC->GetMousePosition(MouseX, MouseY))
	{
		return false;
	}

	OutPoint = FVector2D(MouseX, MouseY);
	return true;
}

int32 ATraceMenuHUD::RowAtPoint(const FVector2D& Point) const
{
	if (!bRowRectsValid)
	{
		return INDEX_NONE;
	}

	for (int32 Index = 0; Index < static_cast<int32>(ETraceMenuRow::Count); ++Index)
	{
		if (RowRects[Index].bIsValid && RowRects[Index].IsInside(Point))
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

void ATraceMenuHUD::UpdateWindowFocus()
{
	// FViewport, not Slate: this module deliberately does not depend on Slate (see Trace.Build.cs),
	// and IsForegroundWindow is on the Engine-side viewport interface.
	bool bFocused = true;
	if (const UWorld* World = GetWorld())
	{
		if (const UGameViewportClient* GameViewport = World->GetGameViewport())
		{
			if (const FViewport* Viewport = GameViewport->Viewport)
			{
				bFocused = Viewport->IsForegroundWindow();
			}
		}
	}

	bWindowFocusedLastFrame = bFocused;
}

void ATraceMenuHUD::MousePressed()
{
	PressedRow = INDEX_NONE;

	if (OptionsMenu.IsOpen() || bTravelling || !bRowRectsValid)
	{
		return;
	}

	// THE STRAY ACTIVATION, caught at its cause.
	//
	// If the window was not foreground as of the last drawn frame, this down edge is the click that
	// is bringing it forward. macOS delivers that click to the application like any other, so it
	// used to land on whatever row was under the cursor — PLAY, at the default selection — and the
	// player watched the title screen skip itself. Arm nothing; the matching release will find no
	// armed row and do nothing either.
	if (!bWindowFocusedLastFrame)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[MenuInput] Ignored the mouse-down that brought the window to the foreground."));
		return;
	}

	// ...and the one that actually catches it. See bCursorHasMoved: the spurious click is a
	// complete down/up pair at a pointer that has not moved since the window appeared.
	if (!bCursorHasMoved)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[MenuInput] Ignored a click at (%.0f, %.0f): the cursor has not moved since the title screen appeared."),
			LastCursorPos.X, LastCursorPos.Y);
		return;
	}

	FVector2D Point = FVector2D::ZeroVector;
	if (!GetCursorPoint(Point))
	{
		return;
	}

	const int32 Row = RowAtPoint(Point);
	if (Row == INDEX_NONE)
	{
		return;
	}

	// Highlight on press so the row visibly depresses under the cursor; commit on release.
	Selected = static_cast<ETraceMenuRow>(Row);
	PressedRow = Row;
}

void ATraceMenuHUD::MouseReleased()
{
	const int32 Armed = PressedRow;
	PressedRow = INDEX_NONE;

	if (OptionsMenu.IsOpen() || bTravelling || Armed == INDEX_NONE)
	{
		return;
	}

	FVector2D Point = FVector2D::ZeroVector;
	if (!GetCursorPoint(Point))
	{
		return;
	}

	// Released somewhere else: the player changed their mind, which is exactly what dragging off a
	// button is for.
	if (RowAtPoint(Point) != Armed)
	{
		return;
	}

	Selected = static_cast<ETraceMenuRow>(Armed);
	ActivateSelection();
}

// =================================================================================================
// Actions
// =================================================================================================

void ATraceMenuHUD::StartMatch()
{
	if (bTravelling)
	{
		return;
	}
	bTravelling = true;

	// Applied here as well as carried in the URL: on a listen server the travel destination reads
	// the option and applies it itself, but in standalone this call is what makes the very first
	// match honour the setting even if the URL is ever dropped on the floor.
	TraceDifficulty::ApplyToSettings(Difficulty);
	TraceScoring::ApplyToSettings(ScoringMode);

	// NOTE THE SEPARATOR. UE URL options are chained with '?', NOT with '&' — writing "a=1&b=2"
	// parses as ONE option called "a" whose value is "1&b=2", so the first appears to work and the
	// second is silently ignored. That has already happened once in this project (see
	// ATraceGameMode::InitGame), and it is exactly the kind of bug that makes an A/B toggle look
	// like it does nothing.
	const FString Options = FString::Printf(TEXT("%s=%s?%s=%s"),
		TraceDifficulty::UrlOption, *TraceDifficulty::ToUrlValue(Difficulty),
		TraceScoringModeUrlOption, *TraceScoringModeToUrlValue(ScoringMode));

	UE_LOG(LogTraceGame, Log, TEXT("Title screen: PLAY -> %s?%s  (%s)"),
		TraceMaps::Arena, *Options, *TraceScoringModeLabel(ScoringMode));

	UGameplayStatics::OpenLevel(this, FName(TraceMaps::Arena), /*bAbsolute=*/true, Options);
}

void ATraceMenuHUD::OpenOptions()
{
	// The title screen has nothing to go back TO, so the settings page is the overlay's root and BACK
	// closes it outright. In a match the same overlay opens one page higher, on the pause menu.
	// OnResume / OnReturnToTitle are deliberately left unset: their absence is what removes those
	// rows, so there is no second layout to maintain.
	OptionsMenu.OnClosed = [this]()
	{
		// The menu controller already keeps the cursor visible and the viewport uncaptured, so there
		// is nothing to restore — but a caller that assumed otherwise would be a silent bug, and this
		// is the one place that assumption is written down.
		UE_LOG(LogTraceGame, Log, TEXT("Title screen: settings closed."));
	};

	OptionsMenu.OpenSettings();
}

void ATraceMenuHUD::QuitGame()
{
	UE_LOG(LogTraceGame, Log, TEXT("Title screen: QUIT."));
	UKismetSystemLibrary::QuitGame(this, GetOwningPlayerController(), EQuitPreference::Quit, /*bIgnorePlatformRestrictions=*/false);
}

// =================================================================================================
// Draw
// =================================================================================================

void ATraceMenuHUD::DrawHUD()
{
	Super::DrawHUD();

	UWorld* World = GetWorld();
	if (Canvas == nullptr || World == nullptr || GEngine == nullptr)
	{
		return;
	}

	ViewW = static_cast<float>(Canvas->SizeX);
	ViewH = static_cast<float>(Canvas->SizeY);
	if (ViewW <= 0.f || ViewH <= 0.f)
	{
		return;
	}

	UIScale = FMath::Clamp(ViewH / TraceMenuStyle::ReferenceHeight, 0.5f, 2.0f);
	Now = World->GetTimeSeconds();

	// Sampled once per drawn frame so a mouse-down can ask "was the window already ours before this
	// click?". See MousePressed.
	UpdateWindowFocus();

	FontSmall  = GEngine->GetSmallFont();
	FontMedium = GEngine->GetMediumFont();
	FontLarge  = GEngine->GetLargeFont();

	// Mouse hover, but only once the cursor has actually moved. Without the movement test a cursor
	// parked over QUIT would silently override every keyboard press.
	//
	// Skipped entirely while the overlay is up: the pointer is over the SETTINGS panel then, and
	// tracking it here would quietly re-select whichever title row happened to be underneath, so
	// closing the overlay would drop the player on a different row than the one they left.
	if (APlayerController* PC = OptionsMenu.IsOpen() ? nullptr : GetOwningPlayerController())
	{
		float MouseX = 0.f;
		float MouseY = 0.f;
		if (PC->GetMousePosition(MouseX, MouseY))
		{
			const FVector2D Position(MouseX, MouseY);

			// The player has to move the mouse before a click counts. See bCursorHasMoved.
			//
			// The first 0.75s is ignored and the threshold is 30px, both learned the hard way: the
			// viewport is still resizing early on, so the same physical pointer reports several
			// different viewport coordinates in the opening frames. A 4px threshold with no settling
			// window was satisfied by that jitter alone and let two launches in ten through.
			if (bHasCursor
				&& (Now - TitleShownTime) > 0.75f
				&& FVector2D::Distance(Position, FirstCursorPos) > 30.f)
			{
				bCursorHasMoved = true;
			}

			if (!bHasCursor || FVector2D::Distance(Position, LastCursorPos) > 2.f)
			{
				if (!bHasCursor)
				{
					FirstCursorPos = Position;
				}
				bHasCursor = true;
				if (bRowRectsValid && !bTravelling)
				{
					for (int32 Index = 0; Index < static_cast<int32>(ETraceMenuRow::Count); ++Index)
					{
						if (RowRects[Index].bIsValid && RowRects[Index].IsInside(Position))
						{
							Selected = static_cast<ETraceMenuRow>(Index);
							break;
						}
					}
				}
			}
			LastCursorPos = Position;
		}
	}

	DrawBackdrop();
	DrawGridFloor();
	DrawWordmark();
	DrawMenuRows();
	DrawFooter();

	// After the footer, not before: the footer's dark strip runs to the bottom edge and would
	// otherwise swallow the frame's bottom rail and two of its corner ticks.
	DrawBezel();
	DrawCursor();
	DrawTravelOverlay();

	// Last of all, over everything, including the travel overlay. Tick() is a no-op while closed.
	OptionsMenu.Tick(this, GetOwningPlayerController(), ViewW, ViewH, UIScale, Now);
}

void ATraceMenuHUD::DrawBackdrop()
{
	// Opaque, and drawn first: the menu map is empty, and whatever the renderer decides to put
	// behind an empty map is not something the title screen should be at the mercy of.
	DrawRect(TraceMenuStyle::Void, 0.f, 0.f, ViewW, ViewH);

	// A cold glow sitting on the horizon, faked as a stack of strips because Canvas has no gradient.
	const float HorizonY = ViewH * TraceMenuStyle::HorizonFraction;
	const int32 Bands = 26;
	const float BandH = (ViewH * 0.26f) / Bands;
	for (int32 Index = 0; Index < Bands; ++Index)
	{
		const float T = static_cast<float>(Index) / static_cast<float>(Bands - 1);
		const float Alpha = 0.10f * T * T;
		DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::CyanDeep, Alpha),
			0.f, HorizonY - (ViewH * 0.26f) + Index * BandH, ViewW, BandH + 1.f);
	}
}

void ATraceMenuHUD::DrawGridFloor()
{
	const float HorizonY = ViewH * TraceMenuStyle::HorizonFraction;
	const float CX = ViewW * 0.5f;
	const float Thin = FMath::Max(1.f, 1.f * UIScale);

	// Rails converging on the vanishing point. Alpha falls off towards the edges so the grid
	// dissolves into the dark instead of ending in a hard line.
	const int32 Rails = 16;
	const float RailSpread = ViewW * 0.34f;
	for (int32 Index = -Rails; Index <= Rails; ++Index)
	{
		const float T = static_cast<float>(Index) / static_cast<float>(Rails);
		const float BottomX = CX + T * RailSpread * Rails * 0.14f;
		const float Alpha = 0.34f * (1.f - FMath::Abs(T) * 0.75f);
		DrawLine(CX, HorizonY, BottomX, ViewH, TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, Alpha), Thin);
	}

	// Rungs, scrolling towards the viewer. The exponent is what sells the perspective: evenly
	// spaced rungs read as a ladder, squared spacing reads as a floor running away from you.
	const int32 Rungs = 22;
	const float Scroll = FMath::Fmod(Now * 0.18f, 1.f);
	for (int32 Index = 0; Index < Rungs; ++Index)
	{
		const float T = (static_cast<float>(Index) + Scroll) / static_cast<float>(Rungs);
		if (T <= 0.f || T > 1.f)
		{
			continue;
		}
		const float Y = HorizonY + (ViewH - HorizonY) * FMath::Pow(T, 2.6f);
		const float Alpha = 0.32f * T;
		DrawLine(0.f, Y, ViewW, Y, TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, Alpha), Thin);
	}

	// The horizon itself, bright, because every straight edge in the frame should point at it.
	DrawGlowLine(0.f, HorizonY, ViewW, HorizonY, TraceMenuStyle::Cyan, FMath::Max(1.f, 1.2f * UIScale));

	// Scanlines over the whole frame. Cheap, and the single strongest cue that this is a screen
	// inside a machine rather than a slide.
	const float Step = FMath::Max(2.f, 4.f * UIScale);
	for (float Y = 0.f; Y < ViewH; Y += Step)
	{
		DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.16f), 0.f, Y, ViewW, 1.f);
	}
}

void ATraceMenuHUD::DrawBezel()
{
	// An earlier pass put a pair of counter-rotating wireframe hexagons behind the wordmark as a
	// nod to the Core. On screen it read as scribble crossing the letterforms — ornament competing
	// with the one thing that must be legible. This replaces it: a frame with corner ticks, which
	// gives the composition an edge to sit inside and stays out of the type's way entirely.
	const float InsetX = ViewW * 0.028f;
	const float InsetY = ViewH * 0.038f;
	const float Left = InsetX;
	const float Right = ViewW - InsetX;
	const float Top = InsetY;
	const float Bottom = ViewH - InsetY;

	const float Thin = FMath::Max(1.f, 1.f * UIScale);
	const FLinearColor Frame = TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.22f);

	DrawLine(Left, Top, Right, Top, Frame, Thin);
	DrawLine(Left, Bottom, Right, Bottom, Frame, Thin);
	DrawLine(Left, Top, Left, Bottom, Frame, Thin);
	DrawLine(Right, Top, Right, Bottom, Frame, Thin);

	// Corner ticks: short, bright, and the only place the frame asserts itself.
	const float Tick = 26.f * UIScale;
	const float TickT = FMath::Max(1.f, 2.f * UIScale);
	const FLinearColor Bright = TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.85f);

	DrawLine(Left, Top, Left + Tick, Top, Bright, TickT);
	DrawLine(Left, Top, Left, Top + Tick, Bright, TickT);
	DrawLine(Right - Tick, Top, Right, Top, Bright, TickT);
	DrawLine(Right, Top, Right, Top + Tick, Bright, TickT);
	DrawLine(Left, Bottom - Tick, Left, Bottom, Bright, TickT);
	DrawLine(Left, Bottom, Left + Tick, Bottom, Bright, TickT);
	DrawLine(Right, Bottom - Tick, Right, Bottom, Bright, TickT);
	DrawLine(Right - Tick, Bottom, Right, Bottom, Bright, TickT);
}

void ATraceMenuHUD::DrawWordmark()
{
	const float CX = ViewW * 0.5f;
	const float CapHeight = ViewH * 0.155f;
	const float TitleY = ViewH * 0.135f;
	const float Thickness = FMath::Max(2.f, CapHeight * 0.055f);

	DrawStrokeTextCentered(TEXT("TRACE"), TraceMenuStyle::Cyan, CX, TitleY, CapHeight, Thickness);

	// Rule + tagline. The rule is exactly as wide as the wordmark, which is the only reason the
	// block below it looks deliberate rather than dropped in.
	const float MarkW = MeasureStrokeText(TEXT("TRACE"), CapHeight);
	const float RuleY = TitleY + CapHeight + (28.f * UIScale);
	DrawGlowLine(CX - MarkW * 0.5f, RuleY, CX + MarkW * 0.5f, RuleY,
		TraceMenuStyle::Cyan, FMath::Max(1.f, 2.f * UIScale));

	DrawTextCentered(TEXT("5 V 5    -    ONE CORE    -    DASH THE TRAIL TO KILL THE CARRIER"),
		TraceMenuStyle::InkDim, CX, RuleY + (18.f * UIScale), FontSmall, 1.15f * UIScale);
}

void ATraceMenuHUD::DrawMenuRows()
{
	const int32 RowCount = static_cast<int32>(ETraceMenuRow::Count);

	const float CX = ViewW * 0.5f;
	const float RowW = FMath::Min(ViewW * 0.52f, 720.f * UIScale);
	const float Spacing = TraceMenuStyle::RowSpacing * UIScale;

	// The rows sit on an opaque console rather than straight on the grid. Without it the perspective
	// lines run right through the labels and the blurb underneath is simply unreadable — the grid
	// wins every legibility contest it is allowed to enter.
	const float PadX = 34.f * UIScale;
	const float PadTop = 22.f * UIScale;
	const float PanelW = RowW + PadX * 2.f;
	const float PanelX = CX - PanelW * 0.5f;
	const float PanelY = ViewH * 0.415f;
	const float PanelH = PadTop + (RowCount - 1) * Spacing + (TraceMenuStyle::RowHeight * UIScale) + (58.f * UIScale);

	DrawRect(FLinearColor(0.004f, 0.014f, 0.026f, 0.94f), PanelX, PanelY, PanelW, PanelH);

	const float Edge = FMath::Max(1.f, 1.2f * UIScale);
	const FLinearColor PanelEdge = TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.28f);
	DrawRect(PanelEdge, PanelX, PanelY, PanelW, Edge);
	DrawRect(PanelEdge, PanelX, PanelY + PanelH - Edge, PanelW, Edge);
	DrawRect(PanelEdge, PanelX, PanelY, Edge, PanelH);
	DrawRect(PanelEdge, PanelX + PanelW - Edge, PanelY, Edge, PanelH);

	const float FirstY = PanelY + PadTop;
	for (int32 Index = 0; Index < RowCount; ++Index)
	{
		const ETraceMenuRow Row = static_cast<ETraceMenuRow>(Index);
		RowRects[Index] = DrawRow(Row, CX, FirstY + Index * Spacing, RowW, Row == Selected);
	}
	bRowRectsValid = true;

	// One line of plain English under the rows, so "EASY" and "MODE B" mean something before you
	// commit to them. It describes whichever value row is SELECTED, because with two value rows a
	// blurb that only ever explained the difficulty would leave the more consequential of the two
	// undocumented — and the scoring mode is a whole different game, not a slider.
	const FString Blurb = (Selected == ETraceMenuRow::Mode)
		? FString(TraceScoringModeBlurb(ScoringMode))
		: TraceMenuStyle::DifficultyBlurb(Difficulty);

	DrawTextCentered(Blurb, TraceMenuStyle::InkDim,
		CX, PanelY + PanelH - (36.f * UIScale), FontSmall, 1.1f * UIScale);
}

FBox2D ATraceMenuHUD::DrawRow(ETraceMenuRow Row, float CenterX, float Y, float Width, bool bSelected)
{
	const float RowH = TraceMenuStyle::RowHeight * UIScale;
	const float X = CenterX - Width * 0.5f;
	const float PadX = TraceMenuStyle::RowPadX * UIScale;

	// Plate. Always opaque enough to lift the label off the grid; brighter when selected.
	DrawRect(FLinearColor(0.f, 0.02f, 0.04f, bSelected ? 0.80f : 0.55f), X, Y, Width, RowH);

	const float Edge = FMath::Max(1.f, 1.2f * UIScale);
	DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, bSelected ? 0.55f : 0.16f), X, Y, Width, Edge);
	DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, bSelected ? 0.55f : 0.16f), X, Y + RowH - Edge, Width, Edge);

	if (bSelected)
	{
		// Breathing selection bar on the leading edge, plus a wash across the plate.
		const float Pulse = 0.72f + 0.28f * FMath::Sin(Now * 4.5f);
		DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.10f * Pulse), X, Y, Width, RowH);
		DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, Pulse), X, Y, 5.f * UIScale, RowH);

		// Chevron, drawn rather than typed — the bitmap fonts have no glyph worth using here.
		const float ChevX = X - (18.f * UIScale);
		const float ChevY = Y + RowH * 0.5f;
		const float ChevS = 10.f * UIScale;
		const float ChevT = FMath::Max(1.f, 2.5f * UIScale);
		DrawLine(ChevX - ChevS, ChevY - ChevS, ChevX, ChevY, TraceMenuStyle::Cyan, ChevT);
		DrawLine(ChevX, ChevY, ChevX - ChevS, ChevY + ChevS, TraceMenuStyle::Cyan, ChevT);
	}

	const FLinearColor LabelColor = bSelected ? TraceMenuStyle::Ink : TraceMenuStyle::InkDim;
	const float LabelScale = 1.55f * UIScale;

	FString Label;
	switch (Row)
	{
	case ETraceMenuRow::Play:       Label = TEXT("PLAY");         break;
	case ETraceMenuRow::Difficulty: Label = TEXT("DIFFICULTY");   break;
	case ETraceMenuRow::Mode:       Label = TEXT("SCORING MODE"); break;
	case ETraceMenuRow::Settings:   Label = TEXT("SETTINGS");     break;
	case ETraceMenuRow::Quit:       Label = TEXT("QUIT");         break;
	default:                        Label = TEXT("");             break;
	}

	const float LabelY = Y + (RowH - MeasureHeight(Label, FontMedium, LabelScale)) * 0.5f;
	DrawText(Label, LabelColor, X + PadX, LabelY, FontMedium, LabelScale);

	// The two VALUE rows share one renderer: right-aligned value, arrows either side, dimmed at the
	// ends of the range. Written once because two copies of this maths is two copies to keep in
	// alignment, and a title screen where one row's arrows sit two pixels off the other's is the
	// kind of thing that reads as sloppy without anyone being able to say why.
	if (Row == ETraceMenuRow::Difficulty || Row == ETraceMenuRow::Mode)
	{
		const bool bIsModeRow = (Row == ETraceMenuRow::Mode);

		// "A - ENDZONES" rather than the full "MODE A - ENDZONES": the row already says SCORING MODE
		// on its left, and repeating the word inside the value pushes it into the label at 720p.
		const FString Value = bIsModeRow
			? FString::Printf(TEXT("%s - %s"), TraceScoringModeLetter(ScoringMode), TraceScoringModeName(ScoringMode))
			: TraceDifficulty::ToDisplayName(Difficulty);

		// Mode B wears the amber that this screen reserves for "this is not the default" — the same
		// colour HARD gets, for the same reason. Mode A is the shipped game and takes the plain cyan.
		const FLinearColor ValueColor = bIsModeRow
			? (TraceIsGoalMode(ScoringMode) ? TraceMenuStyle::Amber : TraceMenuStyle::Cyan)
			: TraceMenuStyle::DifficultyColor(Difficulty);

		const float ValueW = MeasureWidth(Value, FontMedium, LabelScale);
		const float ValueRight = X + Width - PadX;
		const float ValueY = Y + (RowH - MeasureHeight(Value, FontMedium, LabelScale)) * 0.5f;

		DrawText(Value, ValueColor, ValueRight - ValueW, ValueY, FontMedium, LabelScale);

		// Arrows on both sides of the value, dimmed at the ends of the range so the player can see
		// there is nothing further in that direction.
		const float ArrowY = Y + RowH * 0.5f;
		const float ArrowS = 8.f * UIScale;
		const float ArrowT = FMath::Max(1.f, 2.f * UIScale);
		const bool bCanLeft  = bIsModeRow
			? (ScoringMode != ETraceScoringMode::EndzoneStatusCore)
			: (Difficulty != ETraceBotDifficulty::Easy);
		const bool bCanRight = bIsModeRow
			? (ScoringMode != ETraceScoringMode::ThrownCoreAndGoals)
			: (Difficulty != ETraceBotDifficulty::Hard);

		const float LeftX = ValueRight - ValueW - (22.f * UIScale);
		DrawLine(LeftX, ArrowY, LeftX + ArrowS, ArrowY - ArrowS,
			TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, bCanLeft ? 0.95f : 0.20f), ArrowT);
		DrawLine(LeftX, ArrowY, LeftX + ArrowS, ArrowY + ArrowS,
			TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, bCanLeft ? 0.95f : 0.20f), ArrowT);

		const float RightX = ValueRight + (14.f * UIScale);
		DrawLine(RightX, ArrowY - ArrowS, RightX + ArrowS, ArrowY,
			TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, bCanRight ? 0.95f : 0.20f), ArrowT);
		DrawLine(RightX, ArrowY + ArrowS, RightX + ArrowS, ArrowY,
			TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, bCanRight ? 0.95f : 0.20f), ArrowT);
	}

	return FBox2D(FVector2D(X, Y), FVector2D(X + Width, Y + RowH));
}

void ATraceMenuHUD::DrawFooter()
{
	const float CX = ViewW * 0.5f;
	// Both lines have to clear the bezel's bottom rail, which sits 3.8% of the height up from the
	// edge; anchoring off the bottom in reference pixels alone puts the second line under it.
	const float Y = ViewH - (100.f * UIScale) - (ViewH * 0.02f);

	// The grid runs all the way to the bottom edge, so the key hints get their own dark strip.
	// Same reasoning as the console panel above: legibility beats atmosphere every time.
	const float BandY = Y - (22.f * UIScale);
	DrawRect(FLinearColor(0.004f, 0.014f, 0.026f, 0.92f), 0.f, BandY, ViewW, ViewH - BandY);
	DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.24f), 0.f, BandY, ViewW, FMath::Max(1.f, 1.f * UIScale));

	DrawTextCentered(TEXT("W / S  OR  ARROWS   MOVE          A / D   CHANGE          ENTER   SELECT          ESC   QUIT"),
		TraceMenuStyle::InkDim, CX, Y, FontSmall, 1.05f * UIScale);

	DrawTextCentered(TEXT("MOUSE WORKS TOO   -   SENSITIVITY, INVERT Y AND KEY BINDINGS LIVE UNDER SETTINGS"),
		TraceMenuStyle::WithAlpha(TraceMenuStyle::InkDim, 0.6f),
		CX, Y + (24.f * UIScale), FontSmall, 1.f * UIScale);
}

void ATraceMenuHUD::DrawCursor()
{
	// The OS cursor is drawn by the platform and is invisible in captures, so the menu draws its
	// own. It is also simply nicer: a system arrow on this screen would look like a mistake.
	if (!bHasCursor || bTravelling)
	{
		return;
	}

	const float S = 9.f * UIScale;
	const float T = FMath::Max(1.f, 1.5f * UIScale);
	const FLinearColor Color = TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.9f);

	DrawLine(LastCursorPos.X - S, LastCursorPos.Y, LastCursorPos.X - S * 0.35f, LastCursorPos.Y, Color, T);
	DrawLine(LastCursorPos.X + S * 0.35f, LastCursorPos.Y, LastCursorPos.X + S, LastCursorPos.Y, Color, T);
	DrawLine(LastCursorPos.X, LastCursorPos.Y - S, LastCursorPos.X, LastCursorPos.Y - S * 0.35f, Color, T);
	DrawLine(LastCursorPos.X, LastCursorPos.Y + S * 0.35f, LastCursorPos.X, LastCursorPos.Y + S, Color, T);
}

void ATraceMenuHUD::DrawTravelOverlay()
{
	if (!bTravelling)
	{
		return;
	}

	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.72f), 0.f, 0.f, ViewW, ViewH);
	DrawStrokeTextCentered(TEXT("TRACE"), TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.55f),
		ViewW * 0.5f, ViewH * 0.36f, ViewH * 0.10f, FMath::Max(2.f, ViewH * 0.10f * 0.055f));

	DrawTextCentered(TEXT("ENTERING THE ARENA"), TraceMenuStyle::Ink, ViewW * 0.5f, ViewH * 0.55f,
		FontMedium, 1.4f * UIScale);
}

// =================================================================================================
// Helpers
// =================================================================================================

void ATraceMenuHUD::DrawTextCentered(const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale)
{
	DrawText(Text, Color, CenterX - MeasureWidth(Text, Font, Scale) * 0.5f, Y, Font, Scale);
}

float ATraceMenuHUD::MeasureWidth(const FString& Text, UFont* Font, float Scale)
{
	float OutWidth = 0.f;
	float OutHeight = 0.f;
	GetTextSize(Text, OutWidth, OutHeight, Font, Scale);
	return OutWidth;
}

float ATraceMenuHUD::MeasureHeight(const FString& Text, UFont* Font, float Scale)
{
	float OutWidth = 0.f;
	float OutHeight = 0.f;
	GetTextSize(Text, OutWidth, OutHeight, Font, Scale);
	return OutHeight;
}

void ATraceMenuHUD::DrawGlowLine(float X0, float Y0, float X1, float Y1, const FLinearColor& Color, float Thickness)
{
	DrawLine(X0, Y0, X1, Y1, TraceMenuStyle::WithAlpha(Color, 0.18f), Thickness * 4.f);
	DrawLine(X0, Y0, X1, Y1, TraceMenuStyle::WithAlpha(Color, 0.45f), Thickness * 2.f);
	DrawLine(X0, Y0, X1, Y1, FLinearColor(1.f, 1.f, 1.f, 0.92f), Thickness * 0.6f);
}

float ATraceMenuHUD::MeasureStrokeText(const FString& Text, float Height)
{
	const int32 Num = Text.Len();
	if (Num <= 0)
	{
		return 0.f;
	}
	return Num * (TraceStrokeFont::GlyphWidth * Height) + (Num - 1) * (TraceStrokeFont::Tracking * Height);
}

void ATraceMenuHUD::DrawStrokeTextCentered(const FString& Text, const FLinearColor& Color, float CenterX, float Y, float Height, float Thickness)
{
	const float GlyphW = TraceStrokeFont::GlyphWidth * Height;
	const float Advance = GlyphW + TraceStrokeFont::Tracking * Height;

	float PenX = CenterX - MeasureStrokeText(Text, Height) * 0.5f;

	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TraceStrokeFont::FGlyph Glyph = TraceStrokeFont::Find(Text[Index]);
		for (int32 SegIndex = 0; SegIndex < Glyph.Num; ++SegIndex)
		{
			const TraceStrokeFont::FSeg& Seg = Glyph.Segs[SegIndex];
			DrawGlowLine(
				PenX + Seg.X0 * GlyphW, Y + Seg.Y0 * Height,
				PenX + Seg.X1 * GlyphW, Y + Seg.Y1 * Height,
				Color, Thickness);
		}
		PenX += Advance;
	}
}
