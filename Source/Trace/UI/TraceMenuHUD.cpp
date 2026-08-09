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
#include "UI/TraceNetworking.h"

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

	/**
	 * Row geometry, in reference pixels.
	 *
	 * Spacing came down from 78 to 71 when JOIN was added: six rows at the old pitch put the bottom
	 * of the console panel under the footer's dark strip at 1080p, which is the kind of collision
	 * that only shows up on somebody else's monitor.
	 */
	static constexpr float RowHeight  = 60.f;
	static constexpr float RowSpacing = 71.f;
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

	// Bound here rather than in a module startup because both HUDs need it and neither owns the
	// other; the call is idempotent and the handlers outlive every world. Without it a failed join
	// is completely silent, which is the single thing that made the reported bug undiagnosable from
	// the player's chair.
	TraceNet::BindFailureHandlers();

	// The address the player typed last time, straight off disk. Empty on a fresh install, which is
	// exactly when the prompt falls back to showing an example instead.
	LastJoinAddress = TraceNet::LoadLastJoinAddress();

	UE_LOG(LogTraceGame, Log, TEXT("Title screen up. Difficulty %s, %s."),
		*TraceDifficulty::ToDisplayName(Difficulty), *TraceScoringModeLabel(ScoringMode));

	// The address other machines dial to reach this one, and whether we can actually bind it. Logged
	// at Display on every title screen: when a playtest fails to connect this is the first line
	// anyone will be asked for, and it must already be in the log they have.
	{
		TArray<FString> LocalAddresses;
		TraceNet::GetLocalAddresses(LocalAddresses);
		UE_LOG(LogTraceGame, Display, TEXT("[Net] This machine hosts on %s (UDP %d %s). All addresses: %s"),
			*TraceNet::GetHostEndpoint(), TraceNet::DefaultPort,
			TraceNet::IsListenPortAvailable() ? TEXT("free") : TEXT("IN USE"),
			*FString::Join(LocalAddresses, TEXT(", ")));
	}

	// See AcceptUnlockTime in the header. Long enough to outlast window creation and focus
	// acquisition, short enough that a player reaching straight for Enter never notices it.
	if (const UWorld* World = GetWorld())
	{
		TitleShownTime = World->GetTimeSeconds();
		AcceptUnlockTime = TitleShownTime + TraceMenuStyle::ActivationGraceSeconds;
	}

#if !UE_BUILD_SHIPPING
	TraceAutoShot::Arm(this, TEXT("Menu"));
	TraceAutoShot::ArmDeferredExec(this, TEXT("Menu"));
	ArmAutoPlay();
	ArmAutoJoin();
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
	 *   Down       -> CHARACTERS                      (spec v14 §3's new MATCH row)
	 *   Left, Right-> off, then back on               (Toggle, and it leaves the machine unchanged)
	 *   Down       -> SENSITIVITY
	 *   4 x Right  -> SENSITIVITY 1.00 -> 1.20        (Slider, and the held-key adjust path)
	 *   2 x Down   -> INVERT MOUSE Y                  (navigation)
	 *   Enter      -> toggles it ON                   (Toggle)
	 *   2 x Down   -> MOVE FORWARD                    (navigation ACROSS the CONTROLS header)
	 *   Enter      -> arms the rebind capture         (Binding)
	 *   K          -> becomes the new MOVE FORWARD    (capture)
	 *
	 * *** THIS SCRIPT IS POSITIONAL AND MUST BE RE-WALKED WHENEVER A ROW IS ADDED. *** It navigates
	 * by key presses, not by row identity, so inserting a row above SENSITIVITY silently re-aims
	 * every step after it — the run still "passes" while adjusting something nobody asked about. It
	 * was re-walked when spec v14 §3 added the MATCH / CHARACTERS row above MOUSE, which is where the
	 * leading Down and the two Enters come from. The log line each step prints is the check: if a
	 * step's description stops matching what the screenshot shows selected, this list is stale.
	 */
	struct FAutoSettingsKey { FKey (*Key)(); const TCHAR* What; };

	const TArray<FAutoSettingsKey>& AutoSettingsScript()
	{
		static const TArray<FAutoSettingsKey> Script =
		{
			{ []{ return EKeys::Down;  }, TEXT("-> characters (spec v14 3)") },

			// LEFT/RIGHT rather than ENTER, and this is worth knowing before writing another script
			// step: ActivateSelected on a Toggle routes through AdjustSelected(+1), so ENTER
			// INCREMENTS a toggle rather than flipping it. On a row that is already ON it clamps and
			// does nothing at all — which is exactly what the first version of this step did, and it
			// produced a run with no write and no log while looking like it had exercised the row.
			{ []{ return EKeys::Left;  }, TEXT("characters -> OFF") },
			{ []{ return EKeys::Right; }, TEXT("characters -> ON (put back)") },
			{ []{ return EKeys::Down;  }, TEXT("-> sensitivity") },
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

void ATraceMenuHUD::ArmAutoJoin()
{
	float DelaySeconds = 0.f;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TraceAutoJoin="), DelaySeconds))
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	FString AddressArg;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TraceJoinAddress="), AddressArg))
	{
		// No address given: fall back to the remembered one, so a repeat run of the acceptance test
		// needs one flag rather than two.
		AddressArg = LastJoinAddress;
	}
	if (AddressArg.IsEmpty())
	{
		AddressArg = FString::Printf(TEXT("127.0.0.1:%d"), TraceNet::DefaultPort);
	}

	DelaySeconds = FMath::Max(0.01f, DelaySeconds);
	UE_LOG(LogTraceGame, Display, TEXT("[AutoJoin] Will JOIN '%s' in %.2fs."), *AddressArg, DelaySeconds);

	World->GetTimerManager().SetTimer(AutoJoinTimer,
		FTimerDelegate::CreateWeakLambda(this, [this, AddressArg]()
	{
		// Through the real rows, not around them. Selecting JOIN, opening the real prompt and
		// filling the real field is what makes this a test of the menu rather than a test of
		// ClientTravel.
		Selected = ETraceMenuRow::Join;
		OpenJoinPrompt();
		JoinEntry.SetText(AddressArg);

		// Submitted on a SECOND timer rather than in this callback, so the filled prompt is on
		// screen for a couple of seconds. That is the only window in which -TraceAutoShot can
		// photograph it, and a screen with no automated capture is a screen that silently rots.
		if (UWorld* PromptWorld = GetWorld())
		{
			PromptWorld->GetTimerManager().SetTimer(AutoJoinSubmitTimer,
				FTimerDelegate::CreateWeakLambda(this, [this]() { ConfirmJoin(); }), 2.5f, false);
		}
		else
		{
			ConfirmJoin();
		}
	}), DelaySeconds, false);
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
	if (OptionsMenu.IsOpen() || IsJoinPromptOpen() || bTravelling || Delta == 0)
	{
		return;
	}

	const int32 Next = FMath::Clamp(static_cast<int32>(Selected) + Delta, 0, static_cast<int32>(ETraceMenuRow::Count) - 1);
	Selected = static_cast<ETraceMenuRow>(Next);
}

void ATraceMenuHUD::AdjustSelection(int32 Delta)
{
	if (OptionsMenu.IsOpen() || IsJoinPromptOpen() || bTravelling || Delta == 0)
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
	if (OptionsMenu.IsOpen() || IsJoinPromptOpen() || bTravelling)
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

	case ETraceMenuRow::Join:
		OpenJoinPrompt();
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
	if (OptionsMenu.IsOpen() || IsJoinPromptOpen())
	{
		// Both overlays own Escape while they are up and close themselves on it. Letting this
		// through as well would close the prompt AND move the selection to QUIT underneath it.
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

	if (OptionsMenu.IsOpen() || IsJoinPromptOpen() || bTravelling || !bRowRectsValid)
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

	if (OptionsMenu.IsOpen() || IsJoinPromptOpen() || bTravelling || Armed == INDEX_NONE)
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

	// A new attempt: whatever went wrong last time is no longer what is happening now.
	TraceNet::ClearFailure();

	// THE FIX (spec v5 §0). `listen` is a bare, valueless URL option, and its presence is the ONLY
	// thing that makes UEngine::LoadMap call UWorld::Listen and stand up a net driver on UDP 7777.
	// Without it the shipped build produced a standalone match with nothing bound, which is why two
	// machines that both pressed PLAY could never see each other however good their VPN was.
	//
	// It is unconditional on purpose. A listen server with no clients ticks identically to
	// standalone for the player sitting at it, so there is no single-player cost to pay and
	// therefore no reason to make hosting a mode somebody has to remember to choose.
	//
	// NOTE THE SEPARATOR. UE URL options are chained with '?', NOT with '&' — writing "a=1&b=2"
	// parses as ONE option called "a" whose value is "1&b=2", so the first appears to work and the
	// second is silently ignored. That has already happened once in this project (see
	// ATraceGameMode::InitGame), and it is exactly the kind of bug that makes an A/B toggle look
	// like it does nothing.
	//
	// "?characters=0|1" joins them for spec v14 §3's toggle, and it goes on the URL rather than being
	// left to the destination's own settings read for the same reason the mode does: the settings
	// page the player just used lives in THIS process, and a listen server that resolved the toggle
	// from its own ini would honour a value the player may have changed a second ago.
	const FString Options = FString::Printf(TEXT("%s=%s?%s=%s?%s=%d?listen"),
		TraceDifficulty::UrlOption, *TraceDifficulty::ToUrlValue(Difficulty),
		TraceScoringModeUrlOption, *TraceScoringModeToUrlValue(ScoringMode),
		TraceCharacters::UrlOption, TraceCharacters::GetEnabledSetting() ? 1 : 0);

	// Checked BEFORE travelling, and the reason is subtle enough to be worth spelling out.
	//
	// MEASURED, not assumed: with another copy of the game already on 7777, UIpNetDriver does NOT
	// fail — it walks up and binds the next free port, and the run logged "IpNetDriver listening on
	// port 7778". The match is perfectly joinable; it is just joinable at an address that is not the
	// one the title screen printed. A host reading ":7777" off their own screen and reciting it down
	// a voice call would send everybody to a port nothing is listening on, and the join would time
	// out for reasons neither end could see. So the warning is about the PORT MOVING, not about
	// hosting failing.
	//
	// The in-match HUD is the authority and is already correct: TraceNet::DescribeConnection reads
	// UNetDriver::LocalAddr, so the top-right chip shows the port that was actually bound.
	const bool bPortFree = TraceNet::IsListenPortAvailable();
	if (!bPortFree)
	{
		TraceNet::ReportFailure(
			FString::Printf(TEXT("UDP %d IS BUSY - HOSTING ON A DIFFERENT PORT"), TraceNet::DefaultPort),
			// ASCII only. These strings are drawn with the engine's built-in BITMAP fonts, whose
			// glyph pages do not cover an em dash — it comes out as a box or as nothing at all.
			TEXT("ANOTHER COPY OF TRACE ALREADY HOLDS 7777. THE MATCH IS STILL JOINABLE, BUT THE "
			     "ADDRESS TO SHARE IS THE ONE SHOWN TOP-RIGHT ON THE HUD - IT WILL NOT END IN 7777."));
	}

	TravelCaption = bPortFree
		? FString::Printf(TEXT("HOSTING ON %s"), *TraceNet::GetHostEndpoint())
		: FString(TEXT("PORT 7777 IS BUSY - CHECK THE HUD FOR THE REAL ADDRESS"));

	UE_LOG(LogTraceGame, Display, TEXT("Title screen: PLAY -> %s?%s  (%s)  hosting on %s, port %s"),
		TraceMaps::Arena, *Options, *TraceScoringModeLabel(ScoringMode),
		*TraceNet::GetHostEndpoint(), bPortFree ? TEXT("free") : TEXT("IN USE"));

	UGameplayStatics::OpenLevel(this, FName(TraceMaps::Arena), /*bAbsolute=*/true, Options);
}

// -------------------------------------------------------------------------------------------
// JOIN
// -------------------------------------------------------------------------------------------

void ATraceMenuHUD::OpenJoinPrompt()
{
	if (bTravelling || OptionsMenu.IsOpen())
	{
		return;
	}

	JoinError.Reset();
	JoinErrorText.Reset();

	// Pre-filled with the last address that worked, so the common case — the same four people
	// playing again tomorrow — is Enter, Enter. A fresh install gets an empty field and the example
	// text under it instead of a plausible-looking wrong address.
	JoinEntry.Begin(LastJoinAddress);

	UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] JOIN prompt opened (prefill '%s')."), *LastJoinAddress);
}

void ATraceMenuHUD::ConfirmJoin()
{
	const FString Typed = JoinEntry.GetText();
	const FString Address = TraceNet::NormalizeJoinAddress(Typed);

	if (Address.IsEmpty())
	{
		// Kept in the field rather than bounced back to the menu: an empty error that closes the
		// prompt is how a player concludes the button does nothing.
		JoinError = TEXT("ENTER AN ADDRESS, e.g.  100.101.102.103:7777");
		JoinErrorText = Typed;
		return;
	}

	APlayerController* PC = GetOwningPlayerController();
	if (PC == nullptr)
	{
		JoinError = TEXT("NO LOCAL PLAYER - CANNOT CONNECT");
		JoinErrorText = Typed;
		return;
	}

	// Remembered on the ATTEMPT, not on success. A join that times out is overwhelmingly likely to
	// be retried against the same address once the host's firewall is fixed, and making the player
	// retype it in exactly that situation is the opposite of helpful.
	LastJoinAddress = Address;
	TraceNet::SaveLastJoinAddress(Address);

	TraceNet::ClearFailure();
	JoinEntry.End();

	bTravelling = true;
	TravelCaption = FString::Printf(TEXT("CONNECTING TO %s"), *Address);

	UE_LOG(LogTraceGame, Display, TEXT("Title screen: JOIN -> ClientTravel('%s', TRAVEL_Absolute)."), *Address);

	// TRAVEL_Absolute: a join is not relative to the map we are standing on. With TRAVEL_Relative the
	// engine would resolve the address against the menu map's package path and produce nonsense.
	PC->ClientTravel(Address, TRAVEL_Absolute);
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
	if (APlayerController* PC = (OptionsMenu.IsOpen() || IsJoinPromptOpen()) ? nullptr : GetOwningPlayerController())
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

	// The address field polls the keyboard itself, for the reasons in FTraceTextEntry's header, and
	// has to be serviced before anything is drawn so that the caret and the text on screen are this
	// frame's, not last frame's.
	if (JoinEntry.IsActive())
	{
		JoinEntry.Poll(GetOwningPlayerController(), Now);

		if (JoinEntry.ConsumeSubmit())
		{
			ConfirmJoin();
		}
		else if (JoinEntry.ConsumeCancel())
		{
			UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] JOIN prompt cancelled."));
			JoinEntry.End();
			JoinError.Reset();
			JoinErrorText.Reset();
		}
		else if (!JoinError.IsEmpty() && JoinEntry.GetText() != JoinErrorText)
		{
			// Any edit clears the complaint. An error that outlives the text it was about is worse
			// than no error at all — the player fixes the address and the screen still calls it wrong.
			JoinError.Reset();
			JoinErrorText.Reset();
		}
	}

	DrawBackdrop();
	DrawGridFloor();
	DrawWordmark();
	DrawAddressChip();
	DrawMenuRows();
	DrawFooter();

	// After the footer, not before: the footer's dark strip runs to the bottom edge and would
	// otherwise swallow the frame's bottom rail and two of its corner ticks.
	DrawBezel();
	DrawCursor();
	DrawTravelOverlay();

	// Over everything except the settings overlay, which cannot be open at the same time. A no-op
	// while the field is inactive.
	DrawJoinPrompt();

	// AFTER the join prompt, not before. Measured from a screenshot: drawn earlier, the prompt's
	// 78%-black scrim sat on top of the banner and turned the one message the player most needs to
	// read into a dim brown smudge. The failure has to outrank every modal on this screen — the
	// commonest moment to see it is the moment you are about to retype the address that just failed.
	DrawFailureBanner();

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

	// Remembered for DrawAddressChip, which has to sit exactly under the tagline and must not
	// re-derive the wordmark's geometry to find out where that is.
	TaglineBottomY = RuleY + (18.f * UIScale) + MeasureHeight(TEXT("X"), FontSmall, 1.15f * UIScale);
}

void ATraceMenuHUD::DrawAddressChip()
{
	// THE SINGLE AFFORDANCE THAT REMOVES THE MOST COMMON FAILURE.
	//
	// The Demo 5 report was "we couldn't load into the same instance ... I'm unsure if we were
	// actually on a working network-client setup". Half of that uncertainty was not knowing what to
	// type at each other. So the address is on the title screen, before anything is pressed, at a
	// size somebody can read it off a screen and say it out loud — not buried in a doc, not behind
	// `tailscale ip -4`, not in a log.
	const FString Endpoint = TraceNet::GetHostEndpoint();
	const FString Caption = TEXT("YOUR ADDRESS");

	const float CX = ViewW * 0.5f;
	const float CaptionScale = 1.0f * UIScale;
	const float ValueScale = 1.45f * UIScale;

	const float CaptionW = MeasureWidth(Caption, FontSmall, CaptionScale);
	const float ValueW = MeasureWidth(Endpoint, FontMedium, ValueScale);
	const float ValueH = MeasureHeight(Endpoint, FontMedium, ValueScale);

	const float Gap = 14.f * UIScale;
	const float PadX = 18.f * UIScale;
	const float PadY = 7.f * UIScale;

	const float ChipW = CaptionW + Gap + ValueW + PadX * 2.f;
	const float ChipH = ValueH + PadY * 2.f;
	const float ChipX = CX - ChipW * 0.5f;
	const float ChipY = TaglineBottomY + (14.f * UIScale);

	DrawRect(FLinearColor(0.004f, 0.014f, 0.026f, 0.90f), ChipX, ChipY, ChipW, ChipH);

	const float Edge = FMath::Max(1.f, 1.2f * UIScale);
	const FLinearColor EdgeColor = TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.45f);
	DrawRect(EdgeColor, ChipX, ChipY, ChipW, Edge);
	DrawRect(EdgeColor, ChipX, ChipY + ChipH - Edge, ChipW, Edge);
	DrawRect(EdgeColor, ChipX, ChipY, Edge, ChipH);
	DrawRect(EdgeColor, ChipX + ChipW - Edge, ChipY, Edge, ChipH);

	const float CaptionY = ChipY + (ChipH - MeasureHeight(Caption, FontSmall, CaptionScale)) * 0.5f;
	DrawText(Caption, TraceMenuStyle::InkDim, ChipX + PadX, CaptionY, FontSmall, CaptionScale);

	// Bright cyan, and it is the only place on this screen a raw number is allowed to be the loudest
	// thing in its own box.
	DrawText(Endpoint, TraceMenuStyle::Cyan, ChipX + PadX + CaptionW + Gap, ChipY + PadY, FontMedium, ValueScale);

	// MEASURED CAVEAT. If something else already holds UDP 7777, UIpNetDriver does not fail — it
	// binds the next free port instead (observed: "IpNetDriver listening on port 7778"). The match is
	// still joinable, but the number in the chip above is then a lie, and a host reciting it would
	// send everybody to a port nothing is listening on. Saying so here, before PLAY, is cheaper than
	// the ten minutes of confusion on the other end.
	if (!TraceNet::IsDefaultPortFreeCached())
	{
		DrawTextCentered(TEXT("PORT 7777 IS BUSY ON THIS MACHINE - THE HUD WILL SHOW THE REAL PORT IN-GAME"),
			TraceMenuStyle::Amber, CX, ChipY + ChipH + (4.f * UIScale), FontSmall, 0.95f * UIScale);
	}
}

void ATraceMenuHUD::DrawFailureBanner()
{
	FString Headline;
	FString Detail;
	double AgeSeconds = 0.0;
	if (!TraceNet::GetLastFailure(Headline, Detail, AgeSeconds))
	{
		return;
	}

	// A minute is a long time for a banner, and it is deliberate. The failure that matters here
	// happens while the player is looking at a DIFFERENT screen — the join is in flight, the world is
	// being torn down — and they arrive back at the title screen some seconds later. A message that
	// had already expired by then would be exactly as useless as the silence it replaces.
	constexpr double VisibleSeconds = 60.0;
	if (AgeSeconds > VisibleSeconds)
	{
		return;
	}

	const float Fade = static_cast<float>(FMath::Clamp((VisibleSeconds - AgeSeconds) / 6.0, 0.0, 1.0));

	// Amber, not a new red. This screen has exactly two hues and amber is already the one that means
	// danger (see the palette note at the top of this file); introducing a third would cost more than
	// the extra half-step of urgency is worth.
	// Scales raised from 1.15 / 0.95 after reading a capture at 1280x720: the headline was a 9px
	// strip and the engine failure code under it was genuinely unreadable. This is the one message
	// on the screen that has to survive being photographed and pasted into a chat window.
	const float HeadScale = 1.45f * UIScale;
	const float DetailScale = 1.1f * UIScale;

	const float BannerY = ViewH * 0.05f;
	const float PadY = 11.f * UIScale;
	const float HeadH = MeasureHeight(Headline, FontMedium, HeadScale);
	const float DetailH = MeasureHeight(Detail, FontSmall, DetailScale);
	const float BannerH = HeadH + DetailH + PadY * 2.f + (4.f * UIScale);

	DrawRect(FLinearColor(0.18f, 0.05f, 0.00f, 0.90f * Fade), 0.f, BannerY, ViewW, BannerH);
	DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::Amber, 0.85f * Fade), 0.f, BannerY, ViewW, FMath::Max(1.f, 2.f * UIScale));
	DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::Amber, 0.85f * Fade), 0.f, BannerY + BannerH - FMath::Max(1.f, 2.f * UIScale), ViewW, FMath::Max(1.f, 2.f * UIScale));

	DrawTextCentered(Headline, TraceMenuStyle::WithAlpha(TraceMenuStyle::Amber, Fade),
		ViewW * 0.5f, BannerY + PadY, FontMedium, HeadScale);

	// The engine's own code and message, kept verbatim under the readable sentence. The player does
	// not need it; the person they paste their log to does.
	DrawTextCentered(Detail, TraceMenuStyle::WithAlpha(TraceMenuStyle::Ink, 0.8f * Fade),
		ViewW * 0.5f, BannerY + PadY + HeadH + (4.f * UIScale), FontSmall, DetailScale);
}

void ATraceMenuHUD::DrawJoinPrompt()
{
	if (!JoinEntry.IsActive())
	{
		return;
	}

	// Dim everything behind it. The prompt has swallowed the keyboard — including the W/A/S/D that
	// normally move the selection — so the screen has to say plainly that the menu is not listening.
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.78f), 0.f, 0.f, ViewW, ViewH);

	const float CX = ViewW * 0.5f;
	const float PanelW = FMath::Min(ViewW * 0.72f, 900.f * UIScale);
	const float PanelH = 300.f * UIScale;
	const float PanelX = CX - PanelW * 0.5f;
	const float PanelY = ViewH * 0.30f;

	DrawRect(FLinearColor(0.004f, 0.016f, 0.030f, 0.97f), PanelX, PanelY, PanelW, PanelH);

	const float Edge = FMath::Max(1.f, 1.6f * UIScale);
	const FLinearColor EdgeColor = TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.65f);
	DrawRect(EdgeColor, PanelX, PanelY, PanelW, Edge);
	DrawRect(EdgeColor, PanelX, PanelY + PanelH - Edge, PanelW, Edge);
	DrawRect(EdgeColor, PanelX, PanelY, Edge, PanelH);
	DrawRect(EdgeColor, PanelX + PanelW - Edge, PanelY, Edge, PanelH);

	DrawTextCentered(TEXT("JOIN A GAME"), TraceMenuStyle::Cyan, CX, PanelY + (22.f * UIScale), FontMedium, 1.5f * UIScale);
	DrawTextCentered(TEXT("TYPE THE HOST'S ADDRESS"), TraceMenuStyle::InkDim,
		CX, PanelY + (58.f * UIScale), FontSmall, 1.f * UIScale);

	// ---- The field -----------------------------------------------------------------------------
	const float FieldW = PanelW - (72.f * UIScale);
	const float FieldH = 56.f * UIScale;
	const float FieldX = CX - FieldW * 0.5f;
	const float FieldY = PanelY + (92.f * UIScale);

	DrawRect(FLinearColor(0.00f, 0.03f, 0.05f, 0.95f), FieldX, FieldY, FieldW, FieldH);
	const FLinearColor FieldEdge = TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.55f);
	DrawRect(FieldEdge, FieldX, FieldY, FieldW, Edge);
	DrawRect(FieldEdge, FieldX, FieldY + FieldH - Edge, FieldW, Edge);
	DrawRect(FieldEdge, FieldX, FieldY, Edge, FieldH);
	DrawRect(FieldEdge, FieldX + FieldW - Edge, FieldY, Edge, FieldH);

	const FString Typed = JoinEntry.GetText();
	const float TextScale = 1.5f * UIScale;
	const float TextX = FieldX + (18.f * UIScale);
	const float TextY = FieldY + (FieldH - MeasureHeight(TEXT("0"), FontMedium, TextScale)) * 0.5f;

	if (Typed.IsEmpty())
	{
		// Ghost text, dim enough that nobody mistakes it for a value they can press Enter on.
		DrawText(FString::Printf(TEXT("100.101.102.103:%d"), TraceNet::DefaultPort),
			TraceMenuStyle::WithAlpha(TraceMenuStyle::InkDim, 0.35f), TextX, TextY, FontMedium, TextScale);
	}
	else
	{
		DrawText(Typed, TraceMenuStyle::Ink, TextX, TextY, FontMedium, TextScale);
	}

	// Caret. Measured off the substring LEFT of the caret rather than assuming a fixed advance —
	// the engine fonts are proportional, and a caret that drifts off the character it is editing is
	// worse than no caret at all.
	if (JoinEntry.IsCaretVisible(Now))
	{
		const FString LeftOfCaret = Typed.Left(JoinEntry.GetCaret());
		const float CaretX = TextX + MeasureWidth(LeftOfCaret, FontMedium, TextScale);
		DrawRect(TraceMenuStyle::Cyan, CaretX, FieldY + (10.f * UIScale),
			FMath::Max(2.f, 2.f * UIScale), FieldH - (20.f * UIScale));
	}

	// ---- Error, or the reassurance that replaces it ----------------------------------------------
	const float NoteY = FieldY + FieldH + (12.f * UIScale);
	if (!JoinError.IsEmpty())
	{
		DrawTextCentered(JoinError, TraceMenuStyle::Amber, CX, NoteY, FontSmall, 1.05f * UIScale);
	}
	else if (JoinEntry.WasRecentlyPasted(Now))
	{
		DrawTextCentered(TEXT("PASTED FROM CLIPBOARD"), TraceMenuStyle::Cyan, CX, NoteY, FontSmall, 1.05f * UIScale);
	}
	else
	{
		DrawTextCentered(FString::Printf(TEXT("PORT %d IS ADDED FOR YOU IF YOU LEAVE IT OFF"), TraceNet::DefaultPort),
			TraceMenuStyle::WithAlpha(TraceMenuStyle::InkDim, 0.75f), CX, NoteY, FontSmall, 1.05f * UIScale);
	}

	// ---- Keys, and this machine's own address ----------------------------------------------------
	DrawTextCentered(TEXT("ENTER   CONNECT          ESC   CANCEL          CTRL / CMD + V   PASTE          BACKSPACE   DELETE"),
		TraceMenuStyle::InkDim, CX, PanelY + PanelH - (66.f * UIScale), FontSmall, 1.f * UIScale);

	// Deliberately repeated here as well as on the title screen behind it. Somebody in this prompt is
	// mid-conversation with the person they are trying to reach, and "what's yours?" is the very next
	// question — having it on screen saves a round trip through Escape.
	DrawTextCentered(FString::Printf(TEXT("THIS MACHINE IS %s"), *TraceNet::GetHostEndpoint()),
		TraceMenuStyle::WithAlpha(TraceMenuStyle::InkDim, 0.7f),
		CX, PanelY + PanelH - (40.f * UIScale), FontSmall, 1.f * UIScale);
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
	// 0.395 rather than 0.415: JOIN made this a six-row panel, and at 1080p the old anchor pushed its
	// bottom edge 7px under the footer's dark strip.
	const float PanelY = ViewH * 0.395f;
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
	// commit to them. It describes whichever row is SELECTED — every row now has something worth
	// saying, and the two multiplayer rows have the most: PLAY silently became "host a server", and
	// a player who is not told that will keep asking somebody else to host.
	FString Blurb;
	switch (Selected)
	{
	case ETraceMenuRow::Play:
		Blurb = FString::Printf(TEXT("HOSTS A GAME ON %s.  OTHERS PICK JOIN AND TYPE THAT."),
			*TraceNet::GetHostEndpoint());
		break;

	case ETraceMenuRow::Join:
		Blurb = LastJoinAddress.IsEmpty()
			? FString(TEXT("CONNECT TO SOMEBODY ELSE'S GAME.  YOU WILL NEED THEIR ADDRESS."))
			: FString::Printf(TEXT("CONNECT TO SOMEBODY ELSE'S GAME.  ENTER RECONNECTS TO %s."), *LastJoinAddress);
		break;

	case ETraceMenuRow::Mode:
		Blurb = FString(TraceScoringModeBlurb(ScoringMode));
		break;

	case ETraceMenuRow::Settings:
		Blurb = TEXT("MOUSE SENSITIVITY, INVERT Y AND EVERY KEY BINDING.");
		break;

	case ETraceMenuRow::Quit:
		Blurb = TEXT("CLOSE TRACE.");
		break;

	default:
		Blurb = TraceMenuStyle::DifficultyBlurb(Difficulty);
		break;
	}

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
	case ETraceMenuRow::Join:       Label = TEXT("JOIN");         break;
	case ETraceMenuRow::Difficulty: Label = TEXT("DIFFICULTY");   break;
	case ETraceMenuRow::Mode:       Label = TEXT("SCORING MODE"); break;
	case ETraceMenuRow::Settings:   Label = TEXT("SETTINGS");     break;
	case ETraceMenuRow::Quit:       Label = TEXT("QUIT");         break;
	default:                        Label = TEXT("");             break;
	}

	const float LabelY = Y + (RowH - MeasureHeight(Label, FontMedium, LabelScale)) * 0.5f;
	DrawText(Label, LabelColor, X + PadX, LabelY, FontMedium, LabelScale);

	// The two NETWORK rows carry a right-aligned status word instead of a stepper. Small font, not
	// the row font: an IPv4 address plus a port is twenty characters, and at the label's size it
	// would collide with "JOIN" on a 1280-wide window. It is a readout, not a value to change.
	if (Row == ETraceMenuRow::Play || Row == ETraceMenuRow::Join)
	{
		const bool bIsPlayRow = (Row == ETraceMenuRow::Play);

		const FString Value = bIsPlayRow
			? FString::Printf(TEXT("HOST  %s"), *TraceNet::GetHostEndpoint())
			: (LastJoinAddress.IsEmpty() ? FString(TEXT("ENTER AN ADDRESS")) : LastJoinAddress);

		const float ValueScale = 1.05f * UIScale;
		const FLinearColor ValueColor = bSelected
			? TraceMenuStyle::WithAlpha(TraceMenuStyle::Cyan, 0.95f)
			: TraceMenuStyle::WithAlpha(TraceMenuStyle::InkDim, 0.85f);

		const float ValueW = MeasureWidth(Value, FontSmall, ValueScale);
		const float ValueY = Y + (RowH - MeasureHeight(Value, FontSmall, ValueScale)) * 0.5f;
		DrawText(Value, ValueColor, X + Width - PadX - ValueW, ValueY, FontSmall, ValueScale);
	}

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

	DrawTextCentered(TEXT("PLAY ALSO HOSTS - EVERY MATCH IS JOINABLE   -   OTHERS PICK JOIN AND TYPE YOUR ADDRESS ABOVE"),
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

	// The caption names what is actually happening — "HOSTING ON 100.x.y.z:7777" or "CONNECTING TO
	// <addr>" — rather than one generic line for two very different operations. A join that hangs
	// for fifteen seconds and then fails needs the player to have seen the address it was dialling.
	const FString Caption = TravelCaption.IsEmpty() ? FString(TEXT("ENTERING THE ARENA")) : TravelCaption;
	DrawTextCentered(Caption, TraceMenuStyle::Ink, ViewW * 0.5f, ViewH * 0.55f,
		FontMedium, 1.4f * UIScale);

	// Only a join can sit here for a noticeable time; a local map load is instant. Say so, so that
	// three seconds of nothing does not read as a hang.
	if (TravelCaption.StartsWith(TEXT("CONNECTING")))
	{
		DrawTextCentered(TEXT("THIS CAN TAKE A FEW SECONDS.  A FAILURE WILL BE REPORTED, NOT SWALLOWED."),
			TraceMenuStyle::InkDim, ViewW * 0.5f, ViewH * 0.55f + (34.f * UIScale), FontSmall, 1.05f * UIScale);
	}
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
