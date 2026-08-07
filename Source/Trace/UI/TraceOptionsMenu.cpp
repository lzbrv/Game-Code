// Trace — settings overlay implementation. See TraceOptionsMenu.h.

#include "UI/TraceOptionsMenu.h"

#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Trace.h"                       // LogTraceGame

// =================================================================================================
// Palette
//
// The same two hues as the title screen — a cyan that carries the interface and an amber that only
// ever means "danger, or something is waiting on you" — over near-black. Restated here rather than
// shared with TraceMenuHUD.cpp's TraceMenuStyle because this overlay also draws over the MATCH,
// where the title screen's palette namespace is not in scope, and a header shared between them
// would exist purely to hold six colours.
// =================================================================================================

namespace TraceOptionsStyle
{
	static const FLinearColor Cyan     (0.16f, 0.88f, 1.00f, 1.00f);
	static const FLinearColor Amber    (1.00f, 0.46f, 0.08f, 1.00f);
	static const FLinearColor Ink      (0.90f, 0.97f, 1.00f, 1.00f);
	static const FLinearColor InkDim   (0.42f, 0.58f, 0.66f, 1.00f);
	static const FLinearColor Panel    (0.004f, 0.014f, 0.026f, 0.96f);
	static const FLinearColor Trough   (0.03f, 0.06f, 0.08f, 0.90f);

	static FLinearColor WithAlpha(const FLinearColor& C, float A)
	{
		return FLinearColor(C.R, C.G, C.B, A);
	}
}

namespace
{
	/**
	 * Every key a player is allowed to bind, built once.
	 *
	 * EKeys::GetAllKeys() is a few hundred entries including every gamepad axis and every gesture,
	 * and this list is walked once per frame during a rebind capture. Filtering it up front keeps
	 * that walk to the ~150 real buttons and, more importantly, means an axis can never be captured
	 * as a binding — Dash on "MouseX" would fire every time the player looked around.
	 */
	const TArray<FKey>& BindableKeys()
	{
		static TArray<FKey> Keys;
		if (Keys.Num() == 0)
		{
			TArray<FKey> All;
			EKeys::GetAllKeys(All);
			Keys.Reserve(All.Num());
			for (const FKey& Key : All)
			{
				if (UTraceUserSettings::IsBindableKey(Key))
				{
					Keys.Add(Key);
				}
			}
		}
		return Keys;
	}

	/**
	 * True if either key is being held OR was pressed this frame.
	 *
	 * The held test alone is what the repeat logic wants, but it MISSES a press and release that both
	 * land inside one frame — measured during the scripted settings drive, where a synthetic tap that
	 * straddled a stalled frame was silently dropped. A human hand holds a key for ~80ms, so this is
	 * rare in practice, but at a low frame rate a quick tap on an arrow key would do nothing at all,
	 * and "the menu ignored me" is exactly the impression an options screen must never give.
	 *
	 * Adding the edge cannot double-apply: the caller acts once when the direction BECOMES non-zero,
	 * and a press-and-released key reports non-zero for one frame and zero the next.
	 */
	bool AnyDown(const APlayerController* PC, const FKey& A, const FKey& B)
	{
		if (PC == nullptr)
		{
			return false;
		}
		return PC->IsInputKeyDown(A) || PC->IsInputKeyDown(B)
			|| PC->WasInputKeyJustPressed(A) || PC->WasInputKeyJustPressed(B);
	}
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void FTraceOptionsMenu::OpenRoot()
{
	Page = EPage::Root;
	bSettingsIsRootPage = false;
	bCapturingKey = false;

	// The key press that opened us is still "just pressed" for the rest of this frame. See
	// IgnoreInputBeforeFrame in the header.
	IgnoreInputBeforeFrame = GFrameCounter + 1;

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Pause menu opened."));
}

void FTraceOptionsMenu::OpenSettings()
{
	Page = EPage::Settings;
	bSettingsIsRootPage = true;
	bCapturingKey = false;
	IgnoreInputBeforeFrame = GFrameCounter + 1;

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Settings opened."));
}

void FTraceOptionsMenu::Close()
{
	if (Page == EPage::Closed)
	{
		return;
	}

	Page = EPage::Closed;
	bCapturingKey = false;
	CapturingAction = ETraceInputAction::Count;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	LastAdjustDir = 0;
	LastNavDir = 0;

	UE_LOG(LogTraceGame, Display, TEXT("[Options] Closed."));

	if (OnClosed)
	{
		OnClosed();
	}
}

// =================================================================================================
// Rows
// =================================================================================================

void FTraceOptionsMenu::RebuildRows()
{
	Rows.Reset();

	auto AddHeader = [this](const TCHAR* Label)
	{
		FRow Row;
		Row.Kind = ERowKind::Header;
		Row.Label = Label;
		Rows.Add(MoveTemp(Row));
	};

	auto AddAction = [this](const TCHAR* Label, EAction Action)
	{
		FRow Row;
		Row.Kind = ERowKind::Action;
		Row.Label = Label;
		Row.Action = Action;
		Rows.Add(MoveTemp(Row));
	};

	if (Page == EPage::Root)
	{
		// Only offered when the host supplied somewhere to go. The title screen has no RESUME.
		if (OnResume)         { AddAction(TEXT("RESUME"), EAction::Resume); }
		AddAction(TEXT("SETTINGS"), EAction::OpenSettings);
		if (OnReturnToTitle)  { AddAction(TEXT("RETURN TO TITLE"), EAction::ReturnToTitle); }
		if (OnQuit)           { AddAction(TEXT("QUIT"), EAction::Quit); }
	}
	else if (Page == EPage::Settings)
	{
		AddHeader(TEXT("MOUSE"));

		{
			FRow Row;
			Row.Kind = ERowKind::Slider;
			Row.Label = TEXT("SENSITIVITY");
			Row.Setting = ESetting::Sensitivity;
			Rows.Add(MoveTemp(Row));
		}
		{
			FRow Row;
			Row.Kind = ERowKind::Slider;
			Row.Label = TEXT("VERTICAL SENSITIVITY");
			Row.Setting = ESetting::SensitivityY;
			Rows.Add(MoveTemp(Row));
		}
		{
			FRow Row;
			Row.Kind = ERowKind::Toggle;
			Row.Label = TEXT("INVERT MOUSE Y");
			Row.Setting = ESetting::InvertY;
			Rows.Add(MoveTemp(Row));
		}

		AddHeader(TEXT("CONTROLS"));

		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			FRow Row;
			Row.Kind = ERowKind::Binding;
			Row.Label = Info.DisplayName;
			Row.Binding = Info.Action;
			Rows.Add(MoveTemp(Row));
		}

		AddHeader(TEXT(""));
		AddAction(TEXT("RESET TO DEFAULTS"), EAction::ResetDefaults);
		AddAction(TEXT("BACK"), EAction::Back);
	}

	// Land on the first thing that can actually be selected, so a page never opens with the
	// highlight sitting on a caption.
	Selected = 0;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].IsSelectable())
		{
			Selected = Index;
			break;
		}
	}
}

// =================================================================================================
// Tick
// =================================================================================================

void FTraceOptionsMenu::Tick(AHUD* HUD, APlayerController* PC, float InViewW, float InViewH, float InUIScale, float InNow)
{
	if (Page == EPage::Closed || HUD == nullptr || InViewW <= 0.f || InViewH <= 0.f)
	{
		return;
	}

	ViewW = InViewW;
	ViewH = InViewH;
	UIScale = InUIScale;
	Now = InNow;

	if (GEngine != nullptr)
	{
		FontSmall  = GEngine->GetSmallFont();
		FontMedium = GEngine->GetMediumFont();
		FontLarge  = GEngine->GetLargeFont();
	}

	// Input first, then draw, so a value changed this frame is the value the player sees this frame.
	// The row rects the mouse tests against are from the PREVIOUS draw, which is correct: they are
	// where the player was actually looking when they clicked.
	PollInput(PC);

	// PollInput can close us (Escape, RESUME, QUIT). Drawing a closed overlay would leave a frame of
	// dimmed screen over a game that has already resumed.
	if (Page == EPage::Closed)
	{
		return;
	}

	Draw(HUD);
}

// =================================================================================================
// Input
// =================================================================================================

void FTraceOptionsMenu::PollInput(APlayerController* PC)
{
	if (PC == nullptr || GFrameCounter < IgnoreInputBeforeFrame)
	{
		return;
	}

	if (bCapturingKey)
	{
		// *** SPEC v10 §8 — WHY MOUSE BUTTONS "COULD NOT BE BOUND". ***
		//
		// IsBindableKey never rejected them; it could not, the shipped defaults ARE LMB and RMB. The
		// defect was a STALE MOUSE EDGE manufactured on the way out of this capture.
		//
		// A capture is entered on a mouse RELEASE, so bMouseWasDown is false at that moment. PollMouse
		// then does not run for the whole capture — the branch below returns before it. The player
		// presses LMB to bind it, PollKeyCapture calls SetKey and closes the capture, and the player
		// is STILL PHYSICALLY HOLDING THE BUTTON. The first frame PollMouse runs again it compares
		// bDown=true against a bMouseWasDown that has been false since before the capture opened,
		// invents a press edge that never happened, arms PressedRow on the row under the cursor —
		// which is the binding row the player just used — and activates it on the real release. The
		// capture re-opens. Every subsequent click does it again, which reads exactly as "this row
		// refuses to take a mouse button".
		//
		// IgnoreInputBeforeFrame = GFrameCounter + 1 was the old defence and it is not enough by an
		// order of magnitude: it buys ONE frame, and a human holds a mouse button ~80 ms, about five.
		//
		// THE FIX IS AN INVARIANT, NOT A PATCH AT THE EXIT SITES. bMouseWasDown means "the button
		// state last frame", so it must be maintained on EVERY frame this function runs, including
		// the frames a capture is swallowing input. Then no edge can be synthesised across the
		// capture at all — not on the SetKey path, not on the Escape-cancel path, and not on any
		// third exit somebody adds later. Read it before PollKeyCapture, so the frame that closes the
		// capture is recorded with the button still down.
		bMouseWasDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);

		// Nothing may stay armed across a capture. Even with the edge fixed, a PressedRow armed by
		// the click that OPENED the capture would fire its activation on the next release.
		PressedRow = INDEX_NONE;
		bDraggingSlider = false;

		// A capture swallows everything. Navigating away mid-rebind would leave the player unsure
		// which action their next key press was about to land on.
		PollKeyCapture(PC);
		return;
	}

	PollNavigation(PC);
	PollMouse(PC);
}

void FTraceOptionsMenu::PollKeyCapture(APlayerController* PC)
{
	// Escape is filtered out of BindableKeys precisely so it can mean "cancel" here and nothing else.
	if (PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		bCapturingKey = false;
		CapturingAction = ETraceInputAction::Count;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Rebind cancelled."));
		return;
	}

	for (const FKey& Key : BindableKeys())
	{
		if (!PC->WasInputKeyJustPressed(Key))
		{
			continue;
		}

		UTraceUserSettings::Get().SetKey(CapturingAction, Key);
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Bound %s to '%s'."),
			TraceInputActions::Info(CapturingAction).DisplayName, *UTraceUserSettings::DescribeKey(Key));

		bCapturingKey = false;
		CapturingAction = ETraceInputAction::Count;

		// One more frame of quiet: the key that was just bound is still down, and if it happens to be
		// Enter or a mouse button the very next poll would read it as "activate this row again".
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		return;
	}
}

void FTraceOptionsMenu::PollNavigation(APlayerController* PC)
{
	// ---- Vertical: move the selection -----------------------------------------------------------
	int32 NavDir = 0;
	if (AnyDown(PC, EKeys::Down, EKeys::S)) { NavDir += 1; }
	if (AnyDown(PC, EKeys::Up,   EKeys::W)) { NavDir -= 1; }

	if (NavDir != 0)
	{
		if (NavDir != LastNavDir)
		{
			// Direction just became held: act immediately, then wait out the repeat delay.
			MoveSelection(NavDir);
			NextNavTime = Now + RepeatDelay;
		}
		else if (Now >= NextNavTime)
		{
			MoveSelection(NavDir);
			NextNavTime = Now + NavRepeatInterval;
		}
	}
	LastNavDir = NavDir;

	// ---- Horizontal: adjust the selected row ----------------------------------------------------
	int32 AdjustDir = 0;
	if (AnyDown(PC, EKeys::Right, EKeys::D)) { AdjustDir += 1; }
	if (AnyDown(PC, EKeys::Left,  EKeys::A)) { AdjustDir -= 1; }

	if (AdjustDir != 0)
	{
		if (AdjustDir != LastAdjustDir)
		{
			AdjustSelected(AdjustDir);
			NextAdjustTime = Now + RepeatDelay;
		}
		else if (Now >= NextAdjustTime)
		{
			AdjustSelected(AdjustDir);
			NextAdjustTime = Now + RepeatInterval;
		}
	}
	LastAdjustDir = AdjustDir;

	// ---- Buttons --------------------------------------------------------------------------------
	if (PC->WasInputKeyJustPressed(EKeys::Enter) || PC->WasInputKeyJustPressed(EKeys::SpaceBar))
	{
		ActivateSelected();
		return;
	}

	if (PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		GoBack();
		return;
	}

	// Explicit unbind. Every options screen that lets you bind should let you UNbind, and without it
	// there is no way to express "I do not want a parry key" short of hiding it under some other one.
	if (PC->WasInputKeyJustPressed(EKeys::BackSpace) || PC->WasInputKeyJustPressed(EKeys::Delete))
	{
		if (Rows.IsValidIndex(Selected) && Rows[Selected].Kind == ERowKind::Binding)
		{
			// ClearKey, not SetKey: SetKey refuses an invalid key on purpose, because "invalid" is
			// what an unparseable .ini entry looks like and it must never be able to wipe a binding.
			// Unbinding is a separate, explicit intent.
			UTraceUserSettings::Get().ClearKey(Rows[Selected].Binding);
		}
	}
}

void FTraceOptionsMenu::PollMouse(APlayerController* PC)
{
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (PC->GetMousePosition(MouseX, MouseY))
	{
		CursorPos = FVector2D(MouseX, MouseY);
		bHasCursor = true;
	}

	const bool bDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);
	const bool bJustPressed = bDown && !bMouseWasDown;
	const bool bJustReleased = !bDown && bMouseWasDown;
	bMouseWasDown = bDown;

	if (!bHasCursor)
	{
		return;
	}

	// Hover follows the pointer whenever it is over a selectable row. Unlike the title screen this
	// does not need a "has the cursor moved" guard: the overlay is only ever opened by a deliberate
	// key press or click, so there is no window-activation click to defend against.
	int32 HoverRow = INDEX_NONE;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].IsSelectable() && Rows[Index].Rect.bIsValid && Rows[Index].Rect.IsInside(CursorPos))
		{
			HoverRow = Index;
			break;
		}
	}

	if (HoverRow != INDEX_NONE && !bDraggingSlider)
	{
		Selected = HoverRow;
	}

	if (bJustPressed)
	{
		PressedRow = HoverRow;
		bDraggingSlider = false;

		// Grabbing the track is a drag, not a click: the value follows the pointer from this frame on
		// and no activation happens on release. This is the single most useful interaction on the
		// whole screen, because "too sensitive" is found by sweeping, not by stepping.
		if (HoverRow != INDEX_NONE && Rows[HoverRow].Kind == ERowKind::Slider && Rows[HoverRow].Track.bIsValid)
		{
			const FBox2D& Track = Rows[HoverRow].Track;
			// Generous vertical tolerance: the track is a few pixels tall and the row is not.
			if (CursorPos.X >= Track.Min.X - 4.f && CursorPos.X <= Track.Max.X + 4.f)
			{
				bDraggingSlider = true;
			}
		}
	}

	if (bDown && bDraggingSlider && Rows.IsValidIndex(Selected) && Rows[Selected].Track.bIsValid)
	{
		const FBox2D& Track = Rows[Selected].Track;
		const float Width = FMath::Max(1.f, Track.Max.X - Track.Min.X);
		SetSettingNormalised(Rows[Selected].Setting, (CursorPos.X - Track.Min.X) / Width);
	}

	if (bJustReleased)
	{
		const int32 Armed = PressedRow;
		PressedRow = INDEX_NONE;

		if (bDraggingSlider)
		{
			// The drag already wrote every intermediate value; the release only ends it.
			bDraggingSlider = false;
			UTraceUserSettings::Get().Save();
			return;
		}

		// Press and release must land on the same row — ordinary button behaviour, and the same rule
		// the title screen uses.
		if (Armed != INDEX_NONE && Armed == HoverRow)
		{
			Selected = Armed;
			ActivateSelected();
		}
	}
}

void FTraceOptionsMenu::MoveSelection(int32 Delta)
{
	if (Rows.Num() == 0 || Delta == 0)
	{
		return;
	}

	int32 Index = Selected;
	for (int32 Guard = 0; Guard < Rows.Num(); ++Guard)
	{
		Index += Delta;
		if (!Rows.IsValidIndex(Index))
		{
			// Clamp rather than wrap. On a fifteen-row list, wrapping from BACK to SENSITIVITY reads
			// as the menu having jumped somewhere on its own.
			return;
		}
		if (Rows[Index].IsSelectable())
		{
			Selected = Index;
			return;
		}
	}
}

void FTraceOptionsMenu::GetSettingValue(ESetting Setting, float& OutValue, float& OutMin, float& OutMax, float& OutStep) const
{
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();

	switch (Setting)
	{
	case ESetting::Sensitivity:
		OutValue = Settings.MouseSensitivity;
		OutMin = UTraceUserSettings::MinSensitivity;
		OutMax = UTraceUserSettings::MaxSensitivity;
		OutStep = 0.05f;
		break;

	case ESetting::SensitivityY:
		OutValue = Settings.MouseSensitivityYScale;
		OutMin = UTraceUserSettings::MinSensitivityYScale;
		OutMax = UTraceUserSettings::MaxSensitivityYScale;
		OutStep = 0.05f;
		break;

	case ESetting::InvertY:
		OutValue = Settings.bInvertMouseY ? 1.f : 0.f;
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		break;

	default:
		OutValue = 0.f;
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		break;
	}
}

void FTraceOptionsMenu::SetSettingNormalised(ESetting Setting, float Alpha)
{
	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float Step = 1.f;
	GetSettingValue(Setting, Value, Min, Max, Step);

	const float Raw = Min + FMath::Clamp(Alpha, 0.f, 1.f) * (Max - Min);

	// Snapped to the step so a drag produces the same set of values the arrow keys do — otherwise the
	// printed number never lands on a round figure and two players who both "set it to 1.0" have
	// different sensitivities.
	const float Snapped = FMath::Clamp(FMath::RoundToFloat(Raw / Step) * Step, Min, Max);

	UTraceUserSettings& Settings = UTraceUserSettings::Get();
	switch (Setting)
	{
	case ESetting::Sensitivity:  Settings.MouseSensitivity = Snapped; break;
	case ESetting::SensitivityY: Settings.MouseSensitivityYScale = Snapped; break;
	case ESetting::InvertY:      Settings.bInvertMouseY = (Snapped >= 0.5f); break;
	default: return;
	}

	// Deliberately no Save() here: a drag would otherwise write and flush the .ini every frame. The
	// mouse-up path saves once, and the keyboard path in AdjustSelected saves per press.
	UTraceUserSettings::OnChanged().Broadcast();
}

void FTraceOptionsMenu::AdjustSelected(int32 Delta)
{
	if (!Rows.IsValidIndex(Selected) || Delta == 0)
	{
		return;
	}

	FRow& Row = Rows[Selected];
	if (Row.Kind != ERowKind::Slider && Row.Kind != ERowKind::Toggle)
	{
		return;
	}

	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float Step = 1.f;
	GetSettingValue(Row.Setting, Value, Min, Max, Step);

	const float Range = FMath::Max(UE_KINDA_SMALL_NUMBER, Max - Min);
	SetSettingNormalised(Row.Setting, ((Value + Delta * Step) - Min) / Range);

	UTraceUserSettings::Get().Save();
}

void FTraceOptionsMenu::ActivateSelected()
{
	if (!Rows.IsValidIndex(Selected))
	{
		return;
	}

	FRow& Row = Rows[Selected];

	switch (Row.Kind)
	{
	case ERowKind::Toggle:
		// A toggle has one other state, so activating it is unambiguous.
		AdjustSelected(1);
		return;

	case ERowKind::Slider:
		// Nothing sensible for Enter to do to a continuous value. Left/right and the mouse own it.
		return;

	case ERowKind::Binding:
		bCapturingKey = true;
		CapturingAction = Row.Binding;
		// The Enter (or click) that started the capture is still live this frame; without this it
		// would immediately become the new binding.
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Waiting for a key to bind to %s."),
			TraceInputActions::Info(Row.Binding).DisplayName);
		return;

	default:
		break;
	}

	switch (Row.Action)
	{
	case EAction::Resume:
		Close();
		if (OnResume) { OnResume(); }
		break;

	case EAction::OpenSettings:
		Page = EPage::Settings;
		bSettingsIsRootPage = false;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		break;

	case EAction::ReturnToTitle:
		Close();
		if (OnReturnToTitle) { OnReturnToTitle(); }
		break;

	case EAction::Quit:
		Close();
		if (OnQuit) { OnQuit(); }
		break;

	case EAction::ResetDefaults:
		UTraceUserSettings::Get().ResetToDefaults();
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Reset to defaults."));
		break;

	case EAction::Back:
		GoBack();
		break;

	default:
		break;
	}
}

void FTraceOptionsMenu::GoBack()
{
	if (Page == EPage::Settings && !bSettingsIsRootPage)
	{
		// Came in through the pause menu: step back to it rather than dropping the player straight
		// into a firefight they did not ask to return to.
		Page = EPage::Root;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		return;
	}

	const bool bWasRoot = (Page == EPage::Root);
	Close();

	// Escape out of the pause root means "resume", which is what every game does.
	if (bWasRoot && OnResume)
	{
		OnResume();
	}
}

// =================================================================================================
// Draw
// =================================================================================================

void FTraceOptionsMenu::Draw(AHUD* HUD)
{
	// Dim whatever is behind us. In a match that is the arena; on the title screen it is the grid.
	// Either way the panel has to be the only thing the eye can land on, and the arena in particular
	// is a field of bright emissive strips that a translucent panel loses to.
	HUD->DrawRect(FLinearColor(0.f, 0.008f, 0.018f, 0.88f), 0.f, 0.f, ViewW, ViewH);

	// ---- Panel geometry -------------------------------------------------------------------------
	//
	// The panel is sized to its CONTENT, not to the screen. Both pages share this class, and they are
	// wildly different shapes: the settings page has sixteen rows, the pause root has four. A fixed
	// 88%-of-height panel fitted the settings page and left the pause menu as four buttons stranded
	// at the top of an enormous empty box — which is exactly what the first capture showed.
	//
	// So: ask for a comfortable row pitch, add it up, and only then clamp to the screen. The clamp
	// bites on the settings page at 720p and nowhere else, and when it bites the pitch shrinks to fit
	// rather than the last rows falling off the bottom.
	const float TitleBlockH = 82.f * UIScale;      // title + rule + the gap under it
	const float FooterH     = 44.f * UIScale;      // the key-hint line
	const float PadY        = 14.f * UIScale;

	const int32 RowCount = FMath::Max(1, Rows.Num());
	const float PreferredPitch = 44.f * UIScale;

	const float MaxPanelH = ViewH * 0.90f;
	const float PanelH = FMath::Min(MaxPanelH, TitleBlockH + RowCount * PreferredPitch + FooterH + PadY);

	// Whatever height survived the clamp is what the rows get to share.
	const float RowsRegion = FMath::Max(1.f, PanelH - TitleBlockH - FooterH - PadY);
	const float Pitch = FMath::Clamp(RowsRegion / RowCount, 18.f * UIScale, PreferredPitch);
	const float RowH = Pitch * 0.86f;

	// A page of four buttons does not want to be as wide as a page of sixteen labelled settings.
	const float PanelW = (Page == EPage::Root)
		? FMath::Min(ViewW * 0.46f, 520.f * UIScale)
		: FMath::Min(ViewW * 0.74f, 880.f * UIScale);

	const float PanelX = (ViewW - PanelW) * 0.5f;
	const float PanelY = (ViewH - PanelH) * 0.5f;
	const float CX = ViewW * 0.5f;

	HUD->DrawRect(TraceOptionsStyle::Panel, PanelX, PanelY, PanelW, PanelH);
	DrawFrame(HUD, PanelX, PanelY, PanelW, PanelH);

	// ---- Title ---------------------------------------------------------------------------------
	const FString Title = (Page == EPage::Root) ? TEXT("PAUSED") : TEXT("SETTINGS");
	const float TitleY = PanelY + (22.f * UIScale);
	DrawTextCentered(HUD, Title, TraceOptionsStyle::Cyan, CX, TitleY, FontLarge, 1.9f * UIScale);

	const float RuleY = TitleY + (46.f * UIScale);
	HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.45f),
		PanelX + (28.f * UIScale), RuleY, PanelW - (56.f * UIScale), FMath::Max(1.f, 1.5f * UIScale));

	// ---- Rows ----------------------------------------------------------------------------------
	const float RowsTop = RuleY + (14.f * UIScale);
	const float RowX = PanelX + (28.f * UIScale);
	const float RowW = PanelW - (56.f * UIScale);

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		DrawRow(HUD, Rows[Index], RowX, RowsTop + Index * Pitch, RowW, RowH, Index == Selected);
	}

	// ---- Footer --------------------------------------------------------------------------------
	FString Hint;
	if (bCapturingKey)
	{
		Hint = TEXT("PRESS ANY KEY TO BIND          ESC   CANCEL");
	}
	else if (Page == EPage::Root)
	{
		Hint = TEXT("W / S  OR  ARROWS   MOVE          ENTER   SELECT          ESC   RESUME");
	}
	else
	{
		Hint = TEXT("ARROWS  MOVE / ADJUST      ENTER  SELECT      BKSP  UNBIND      ESC  BACK");
	}

	DrawTextCentered(HUD, Hint, TraceOptionsStyle::InkDim, CX,
		PanelY + PanelH - (30.f * UIScale), FontSmall, 1.0f * UIScale);

	DrawCursor(HUD);
}

void FTraceOptionsMenu::DrawRow(AHUD* HUD, FRow& Row, float X, float Y, float W, float H, bool bSelected)
{
	Row.Rect = FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));
	Row.Track = FBox2D(ForceInit);

	const float PadX = 16.f * UIScale;
	const float LabelScale = FMath::Min(1.15f, H / (26.f * UIScale)) * UIScale;
	const float TextY = Y + (H - MeasureHeight(HUD, TEXT("X"), FontMedium, LabelScale)) * 0.5f;

	// ---- Header --------------------------------------------------------------------------------
	if (Row.Kind == ERowKind::Header)
	{
		if (!Row.Label.IsEmpty())
		{
			HUD->DrawText(Row.Label, TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.75f),
				X, TextY, FontSmall, 1.0f * UIScale);

			const float LabelW = MeasureWidth(HUD, Row.Label, FontSmall, 1.0f * UIScale);
			HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.20f),
				X + LabelW + (10.f * UIScale), Y + H * 0.5f, W - LabelW - (10.f * UIScale), FMath::Max(1.f, 1.f * UIScale));
		}
		return;
	}

	// ---- Plate ---------------------------------------------------------------------------------
	HUD->DrawRect(FLinearColor(0.f, 0.02f, 0.04f, bSelected ? 0.85f : 0.45f), X, Y, W, H);

	if (bSelected)
	{
		const float Pulse = 0.72f + 0.28f * FMath::Sin(Now * 4.5f);
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.10f * Pulse), X, Y, W, H);
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, Pulse), X, Y, 4.f * UIScale, H);
	}

	const FLinearColor LabelColor = bSelected ? TraceOptionsStyle::Ink : TraceOptionsStyle::InkDim;

	// Action rows are buttons; centring their label is what makes them read as one.
	if (Row.Kind == ERowKind::Action)
	{
		DrawTextCentered(HUD, Row.Label, LabelColor, X + W * 0.5f, TextY, FontMedium, LabelScale);
		return;
	}

	HUD->DrawText(Row.Label, LabelColor, X + PadX, TextY, FontMedium, LabelScale);

	// ---- Value ---------------------------------------------------------------------------------
	const float ValueRight = X + W - PadX;
	const float ValueColW = 96.f * UIScale;

	if (Row.Kind == ERowKind::Binding)
	{
		const bool bWaiting = bCapturingKey && CapturingAction == Row.Binding;
		const FKey Key = UTraceUserSettings::Get().GetKey(Row.Binding);

		FString ValueText;
		FLinearColor ValueColor;
		if (bWaiting)
		{
			ValueText = TEXT("PRESS A KEY");
			ValueColor = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Amber, 0.6f + 0.4f * FMath::Sin(Now * 9.f));
		}
		else
		{
			ValueText = UTraceUserSettings::DescribeKey(Key);
			// Amber for UNBOUND: it is not an error, but the player should not be able to miss it.
			ValueColor = Key.IsValid() ? TraceOptionsStyle::Ink : TraceOptionsStyle::Amber;
		}

		const float PlateW = FMath::Max(MeasureWidth(HUD, ValueText, FontMedium, LabelScale) + (16.f * UIScale), 120.f * UIScale);
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, bWaiting ? 0.22f : 0.10f),
			ValueRight - PlateW, Y + H * 0.14f, PlateW, H * 0.72f);
		DrawTextCentered(HUD, ValueText, ValueColor, ValueRight - PlateW * 0.5f, TextY, FontMedium, LabelScale);
		return;
	}

	if (Row.Kind == ERowKind::Toggle)
	{
		const bool bOn = UTraceUserSettings::Get().bInvertMouseY;
		const FString ValueText = bOn ? TEXT("ON") : TEXT("OFF");
		DrawTextRight(HUD, ValueText, bOn ? TraceOptionsStyle::Amber : TraceOptionsStyle::InkDim,
			ValueRight, TextY, FontMedium, LabelScale);
		return;
	}

	// ---- Slider --------------------------------------------------------------------------------
	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float Step = 1.f;
	GetSettingValue(Row.Setting, Value, Min, Max, Step);

	const float Alpha = FMath::Clamp((Value - Min) / FMath::Max(UE_KINDA_SMALL_NUMBER, Max - Min), 0.f, 1.f);

	const FString ValueText = FString::Printf(TEXT("%.2f"), Value);
	DrawTextRight(HUD, ValueText, bSelected ? TraceOptionsStyle::Ink : TraceOptionsStyle::InkDim,
		ValueRight, TextY, FontMedium, LabelScale);

	const float TrackRight = ValueRight - ValueColW;
	const float TrackLeft = X + W * 0.48f;
	const float TrackW = FMath::Max(20.f * UIScale, TrackRight - TrackLeft);
	const float TrackH = FMath::Max(3.f, 6.f * UIScale);
	const float TrackY = Y + (H - TrackH) * 0.5f;

	HUD->DrawRect(TraceOptionsStyle::Trough, TrackLeft, TrackY, TrackW, TrackH);
	HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, bSelected ? 0.95f : 0.55f),
		TrackLeft, TrackY, TrackW * Alpha, TrackH);

	// Handle, so the value has a thing to grab as well as a bar to read.
	const float HandleW = FMath::Max(4.f, 6.f * UIScale);
	const float HandleH = H * 0.56f;
	HUD->DrawRect(bSelected ? FLinearColor::White : TraceOptionsStyle::Cyan,
		TrackLeft + TrackW * Alpha - HandleW * 0.5f, Y + (H - HandleH) * 0.5f, HandleW, HandleH);

	// Stored AFTER drawing so the poll on the next frame drags against exactly what was on screen.
	Row.Track = FBox2D(FVector2D(TrackLeft, TrackY - H * 0.4f), FVector2D(TrackLeft + TrackW, TrackY + H * 0.4f));
}

void FTraceOptionsMenu::DrawFrame(AHUD* HUD, float X, float Y, float W, float H)
{
	// Same instrument-panel frame as the title screen's bezel, at panel scale, so the overlay reads
	// as part of the same machine rather than as a dialog box dropped on top of it.
	const float Thin = FMath::Max(1.f, 1.f * UIScale);
	const FLinearColor Frame = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.30f);

	HUD->DrawRect(Frame, X, Y, W, Thin);
	HUD->DrawRect(Frame, X, Y + H - Thin, W, Thin);
	HUD->DrawRect(Frame, X, Y, Thin, H);
	HUD->DrawRect(Frame, X + W - Thin, Y, Thin, H);

	const float Tick = 24.f * UIScale;
	const float TickT = FMath::Max(1.f, 2.f * UIScale);
	const FLinearColor Bright = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.9f);

	HUD->DrawRect(Bright, X, Y, Tick, TickT);
	HUD->DrawRect(Bright, X, Y, TickT, Tick);
	HUD->DrawRect(Bright, X + W - Tick, Y, Tick, TickT);
	HUD->DrawRect(Bright, X + W - TickT, Y, TickT, Tick);
	HUD->DrawRect(Bright, X, Y + H - TickT, Tick, TickT);
	HUD->DrawRect(Bright, X, Y + H - Tick, TickT, Tick);
	HUD->DrawRect(Bright, X + W - Tick, Y + H - TickT, Tick, TickT);
	HUD->DrawRect(Bright, X + W - TickT, Y + H - Tick, TickT, Tick);
}

void FTraceOptionsMenu::DrawCursor(AHUD* HUD)
{
	// The OS cursor does not appear in captured frames, and in the match it is hidden outright until
	// this overlay releases it — so the overlay draws its own. Same shape as the title screen's.
	if (!bHasCursor)
	{
		return;
	}

	const float S = 9.f * UIScale;
	const float T = FMath::Max(1.f, 1.5f * UIScale);
	const FLinearColor Color = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.95f);

	HUD->DrawLine(CursorPos.X - S, CursorPos.Y, CursorPos.X - S * 0.35f, CursorPos.Y, Color, T);
	HUD->DrawLine(CursorPos.X + S * 0.35f, CursorPos.Y, CursorPos.X + S, CursorPos.Y, Color, T);
	HUD->DrawLine(CursorPos.X, CursorPos.Y - S, CursorPos.X, CursorPos.Y - S * 0.35f, Color, T);
	HUD->DrawLine(CursorPos.X, CursorPos.Y + S * 0.35f, CursorPos.X, CursorPos.Y + S, Color, T);
}

// =================================================================================================
// Text helpers
// =================================================================================================

float FTraceOptionsMenu::MeasureWidth(AHUD* HUD, const FString& Text, UFont* Font, float Scale)
{
	float OutWidth = 0.f;
	float OutHeight = 0.f;
	HUD->GetTextSize(Text, OutWidth, OutHeight, Font, Scale);
	return OutWidth;
}

float FTraceOptionsMenu::MeasureHeight(AHUD* HUD, const FString& Text, UFont* Font, float Scale)
{
	float OutWidth = 0.f;
	float OutHeight = 0.f;
	HUD->GetTextSize(Text, OutWidth, OutHeight, Font, Scale);
	return OutHeight;
}

void FTraceOptionsMenu::DrawTextCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale)
{
	HUD->DrawText(Text, Color, CenterX - MeasureWidth(HUD, Text, Font, Scale) * 0.5f, Y, Font, Scale);
}

void FTraceOptionsMenu::DrawTextRight(AHUD* HUD, const FString& Text, const FLinearColor& Color, float RightX, float Y, UFont* Font, float Scale)
{
	HUD->DrawText(Text, Color, RightX - MeasureWidth(HUD, Text, Font, Scale), Y, Font, Scale);
}
