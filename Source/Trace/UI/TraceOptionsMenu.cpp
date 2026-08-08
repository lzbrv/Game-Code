// Trace — settings overlay implementation. See TraceOptionsMenu.h.

#include "UI/TraceOptionsMenu.h"

#include "Camera/CameraComponent.h"
#include "Containers/Ticker.h"          // FTSTicker - defer the viewport resize out of DrawHUD
#include "DynamicRHI.h"                  // RHIGetGPUFrameCycles
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Scalability.h"
#include "Settings/TraceGameUserSettings.h"
#include "Trace.h"                       // LogTraceGame

// =================================================================================================
// WHERE THE VIDEO SETTINGS ACTUALLY LIVE — AND WHY NONE OF THEM LIVE HERE
//
// Every row on the VIDEO page is stored, validated, applied and persisted by
// UTraceGameUserSettings (Settings/TraceGameUserSettings.h). This file holds NO video state at all:
// no cached resolution list, no preset ladder, no frame-cap table, no field-of-view value. Each row
// is read live from that class on the frame it is drawn and written straight back on the frame it
// is changed.
//
// That is a deliberate line, and it is drawn where it is because both halves have been written by
// somebody who could plausibly have written the other. Two copies of "what resolutions exist" or
// "which levels count as Epic" would agree on the day they were written and diverge on the first
// day either was edited, and the symptom of that divergence is a menu row that quietly controls
// nothing — a failure this project has already been bitten by and now warns about in its own build
// notes. So: that class decides what a setting MEANS. This one decides what it LOOKS like, where it
// sits in the list, and what happens when a key is held down on it.
//
// The one thing this file argues about is ORDER, and that argument is spec v11 §0: the frame is
// GPU-bound per pixel, so RESOLUTION SCALE and AUTO-DETECT come first, above the window mode and
// above all nine quality groups. See RebuildRows.
// =================================================================================================

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
// Dev access — opening a page without a keyboard
//
// A headless run has no way to press anything, so there is no way to CAPTURE the video page, and a
// menu page that cannot be captured is a menu page nobody can be shown to have checked. This is the
// same class of hole -TraceAutoPause was added to fill for the pause root.
//
// A raw pointer to the last overlay that ticked. Both hosts call Tick() every frame whether the
// overlay is open or not, so this is always the one the player is looking at; on a travel the two
// HUDs overlap for a frame and last-writer-wins, which for a dev command is the right answer anyway.
// Cleared in the destructor so a HUD destroyed by a travel cannot leave this dangling.
//
// NOT a CVar, and NOT in the Trace.Video.* namespace. This project fatals at module load if a CVar
// and a console command share a name, and Trace.Video.* already contains a CVar next door
// (Trace.Video.FOVAutoApply). Trace.Menu.* is unambiguously this file's.
// =================================================================================================

#if !UE_BUILD_SHIPPING
namespace
{
	FTraceOptionsMenu* GActiveOptionsMenu = nullptr;

	FAutoConsoleCommand CmdMenuVideo(
		TEXT("Trace.Menu.Video"),
		TEXT("Opens the VIDEO settings page on whichever HUD is up. Works on the title screen and in ")
		TEXT("a match. Exists so a headless run can screenshot the page: -TraceExec=\"Trace.Menu.Video\"."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->OpenVideo();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Video: no HUD is drawing an overlay yet."));
			}
		}));

	FAutoConsoleCommand CmdMenuNudge(
		TEXT("Trace.Menu.Nudge"),
		TEXT("Trace.Menu.Nudge <rows-from-top> <delta>. Moves the selection and adjusts it, exactly as ")
		TEXT("the arrow keys would, then logs the row and its new value.\n")
		TEXT("This is the ONLY headless way to exercise the menu's own write path: every capture-only ")
		TEXT("test drives the settings through the Trace.Video.* commands instead, which proves the ")
		TEXT("settings class persists and proves nothing whatever about the rows on this page."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (GActiveOptionsMenu == nullptr || Args.Num() < 2)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Nudge <rows-from-top> <delta>"));
				return;
			}
			GActiveOptionsMenu->DebugNudge(FCString::Atoi(*Args[0]), FCString::Atoi(*Args[1]));
		}));
}
#endif

FTraceOptionsMenu::~FTraceOptionsMenu()
{
#if !UE_BUILD_SHIPPING
	if (GActiveOptionsMenu == this)
	{
		GActiveOptionsMenu = nullptr;
	}
#endif
}

#if !UE_BUILD_SHIPPING
void FTraceOptionsMenu::DebugNudge(int32 RowsFromTop, int32 Delta)
{
	if (!Rows.IsValidIndex(RowsFromTop))
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Options] Nudge: row %d is out of range (%d rows)."),
			RowsFromTop, Rows.Num());
		return;
	}
	if (!Rows[RowsFromTop].IsSelectable())
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[Options] Nudge: row %d ('%s') is not selectable."),
			RowsFromTop, *Rows[RowsFromTop].Label);
		return;
	}

	Selected = RowsFromTop;

	const int32 Steps = FMath::Abs(Delta);
	const int32 Dir = (Delta >= 0) ? 1 : -1;
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		AdjustSelected(Dir);
	}

	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float StepSize = 1.f;
	GetSettingValue(Rows[Selected].Setting, Value, Min, Max, StepSize);

	UE_LOG(LogTraceGame, Display, TEXT("[Options] Nudge: '%s' is now %s."),
		*Rows[Selected].Label, *FormatSettingValue(Rows[Selected].Setting, Value));
}
#endif

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

void FTraceOptionsMenu::OpenVideo()
{
	Page = EPage::Video;
	bCapturingKey = false;

	// Closed, not Settings: this entry point IS the top of the stack, so BACK has to close rather
	// than drop the player onto a settings page they never asked for.
	VideoReturnPage = EPage::Closed;
	IgnoreInputBeforeFrame = GFrameCounter + 1;

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Video settings opened."));
}

void FTraceOptionsMenu::Close()
{
	if (Page == EPage::Closed)
	{
		return;
	}

	// Before the page goes away: a resolution or window-mode change the player made and then escaped
	// out of is still a change they made. Dropping it would mean the value they can see in the .ini
	// next launch is not the one the window is running at.
	if (bResolutionApplyPending)
	{
		ApplyVideo(/*bResolutionAffecting=*/true, /*bPersist=*/true);
	}

	Page = EPage::Closed;
	bCapturingKey = false;
	CapturingAction = ETraceInputAction::Count;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bAutoDetectPending = false;
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

	auto AddNote = [this](const TCHAR* Label)
	{
		FRow Row;
		Row.Kind = ERowKind::Note;
		Row.Label = Label;
		Rows.Add(MoveTemp(Row));
	};

	auto AddValue = [this](ERowKind Kind, const TCHAR* Label, ESetting Setting)
	{
		FRow Row;
		Row.Kind = Kind;
		Row.Label = Label;
		Row.Setting = Setting;
		Rows.Add(MoveTemp(Row));
	};

	if (Page == EPage::Root)
	{
		// Only offered when the host supplied somewhere to go. The title screen has no RESUME.
		if (OnResume)         { AddAction(TEXT("RESUME"), EAction::Resume); }
		AddAction(TEXT("SETTINGS"), EAction::OpenSettings);

		// Its own row on the pause root rather than only inside SETTINGS. Spec v11 §0: the player
		// this feature exists for is one whose frame rate has collapsed, and making them walk past
		// mouse sensitivity and eleven key bindings to reach the resolution scale is exactly the kind
		// of burial that left the collaborator with no way to improve anything.
		AddAction(TEXT("VIDEO"), EAction::OpenVideo);

		if (OnReturnToTitle)  { AddAction(TEXT("RETURN TO TITLE"), EAction::ReturnToTitle); }
		if (OnQuit)           { AddAction(TEXT("QUIT"), EAction::Quit); }
	}
	else if (Page == EPage::Video)
	{
		// ---- Performance first ------------------------------------------------------------------
		//
		// The order on this page is an argument, not a taxonomy. Spec v11 §0 measured the frame as
		// GPU-bound PER PIXEL — instancing the arena removed 893 draw calls and bought 1.4% — so the
		// number of pixels is the dominant term and the two controls that change it come first,
		// above the mode, the resolution and all nine quality groups.
		AddHeader(TEXT("PERFORMANCE"));
		AddValue(ERowKind::Slider, TEXT("RESOLUTION SCALE"), ESetting::ResolutionScale);
		AddNote(TEXT("THE BIGGEST WIN IF THE GAME RUNS SLOW. THIS FRAME IS LIMITED BY PIXELS."));
		AddAction(TEXT("AUTO-DETECT QUALITY"), EAction::AutoDetectQuality);
		AddValue(ERowKind::Choice, TEXT("OVERALL QUALITY"), ESetting::OverallQuality);

		// ---- Display ----------------------------------------------------------------------------
		AddHeader(TEXT("DISPLAY"));
		AddValue(ERowKind::Choice, TEXT("WINDOW MODE"), ESetting::WindowMode);
		AddValue(ERowKind::Choice, TEXT("RESOLUTION"), ESetting::Resolution);
		AddValue(ERowKind::Toggle, TEXT("VSYNC"), ESetting::VSync);
		AddValue(ERowKind::Choice, TEXT("FRAME RATE LIMIT"), ESetting::FrameRateLimit);
		AddValue(ERowKind::Slider, TEXT("FIELD OF VIEW"), ESetting::FieldOfView);

		// ---- The nine groups --------------------------------------------------------------------
		AddHeader(TEXT("QUALITY"));
		AddValue(ERowKind::Choice, TEXT("VIEW DISTANCE"), ESetting::QualityViewDistance);
		AddValue(ERowKind::Choice, TEXT("ANTI-ALIASING"), ESetting::QualityAntiAliasing);
		AddValue(ERowKind::Choice, TEXT("POST PROCESSING"), ESetting::QualityPostProcess);
		AddValue(ERowKind::Choice, TEXT("SHADOWS"), ESetting::QualityShadows);
		AddValue(ERowKind::Choice, TEXT("GLOBAL ILLUMINATION"), ESetting::QualityGlobalIllumination);
		AddValue(ERowKind::Choice, TEXT("REFLECTIONS"), ESetting::QualityReflections);
		AddValue(ERowKind::Choice, TEXT("TEXTURES"), ESetting::QualityTextures);
		AddValue(ERowKind::Choice, TEXT("EFFECTS"), ESetting::QualityEffects);
		AddValue(ERowKind::Choice, TEXT("SHADING"), ESetting::QualityShading);

		AddHeader(TEXT(""));
		AddAction(TEXT("RESET TO DEFAULTS"), EAction::ResetVideoDefaults);
		AddAction(TEXT("BACK"), EAction::Back);
	}
	else if (Page == EPage::Settings)
	{
		// First row on the page, above the mouse. Same reasoning as the pause root's VIDEO entry —
		// and this is the ONLY route to the video page from the title screen, where there is no
		// pause root at all, so it cannot be buried at the bottom next to RESET.
		AddHeader(TEXT("DISPLAY"));
		AddAction(TEXT("VIDEO SETTINGS"), EAction::OpenVideo);

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

	// Before picking a selection, not after: a row that is greyed out right now is not somewhere the
	// highlight may land, and RESOLUTION is the first selectable row on the video page's DISPLAY
	// block whenever the window mode is not windowed fullscreen.
	RefreshRowStates();

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

void FTraceOptionsMenu::RefreshRowStates()
{
	// Only one rule so far, and it is worth stating rather than generalising: in windowed fullscreen
	// the window always takes the desktop's size, so a stored resolution is accepted, saved, and then
	// ignored by the platform. A row that takes input and changes nothing is the worst kind of
	// control, so it is greyed and its value reads DESKTOP. IsResolutionSelectable is the settings
	// class's own answer to that question, so the two files cannot disagree about it.
	const bool bResolutionMeaningful = (Video() == nullptr) || Video()->IsResolutionSelectable();

	for (FRow& Row : Rows)
	{
		if (Row.Setting == ESetting::Resolution)
		{
			Row.bEnabled = bResolutionMeaningful;
		}
	}

	// The selection may have been sitting on a row that just went grey — switching to windowed
	// fullscreen while RESOLUTION is highlighted does exactly that. Walk down, then up.
	if (Rows.IsValidIndex(Selected) && !Rows[Selected].IsSelectable())
	{
		for (int32 Index = Selected + 1; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index].IsSelectable()) { Selected = Index; return; }
		}
		for (int32 Index = Selected - 1; Index >= 0; --Index)
		{
			if (Rows[Index].IsSelectable()) { Selected = Index; return; }
		}
	}
}

// =================================================================================================
// Tick
// =================================================================================================

void FTraceOptionsMenu::Tick(AHUD* HUD, APlayerController* PC, float InViewW, float InViewH, float InUIScale, float InNow)
{
#if !UE_BUILD_SHIPPING
	// Claimed every frame, open or not, so Trace.Menu.Video always reaches the overlay the player is
	// actually looking at. See GActiveOptionsMenu.
	GActiveOptionsMenu = this;
#endif

	// ---- Runs even while the overlay is CLOSED ---------------------------------------------------
	//
	// Both hosts call Tick unconditionally every frame and rely on it being a no-op while closed, so
	// this is the one hook the video settings have into an ordinary gameplay frame — and field of
	// view needs exactly that. ATraceCharacter::ATraceCharacter sets the camera's FOV once, in the
	// constructor, which means a fresh launch and every single respawn both come back at 95 degrees
	// no matter what the player saved. Reasserting it here costs one weak-pointer compare and one
	// float compare per frame, and it is the difference between the row persisting and the row
	// appearing to persist until the player next dies.
	MaintainFieldOfView(PC);

	if (Page == EPage::Closed || HUD == nullptr || InViewW <= 0.f || InViewH <= 0.f)
	{
		return;
	}

	// Sampled every frame the page is up, before anything else can spend time. See UpdatePerfReadout
	// for why this uses real time rather than the InNow the host passes in.
	UpdatePerfReadout();

	// Deferred from the click one frame ago so that "MEASURING…" was actually on screen for the
	// second the benchmark spends blocking the game thread. See bAutoDetectPending.
	if (bAutoDetectPending)
	{
		bAutoDetectPending = false;
		RunAutoDetect();
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

	// Before input, so a click cannot land on a row that stopped being meaningful last frame.
	RefreshRowStates();

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

	// The coalesced window resize, once the player has stopped moving through the list.
	if (bResolutionApplyPending && Now >= ResolutionApplyAtTime)
	{
		ApplyVideo(/*bResolutionAffecting=*/true, /*bPersist=*/true);
		RefreshRowStates();
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
			// The drag already wrote every intermediate value; the release only ends it — and writes
			// the one .ini flush the whole gesture is allowed.
			bDraggingSlider = false;

			const ESetting Dragged = Rows.IsValidIndex(Selected) ? Rows[Selected].Setting : ESetting::None;
			if (IsVideoSetting(Dragged))
			{
				ApplyVideo(/*bResolutionAffecting=*/false, /*bPersist=*/true);
			}
			else
			{
				UTraceUserSettings::Get().Save();
			}
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
	// Set before the switch, not in a default case, so that every early return below — and any case
	// somebody adds later that forgets one of the four — still hands back a coherent 0..1 range
	// instead of whatever the caller happened to have on its stack.
	OutValue = 0.f;
	OutMin = 0.f;
	OutMax = 1.f;
	OutStep = 1.f;

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
		break;
	}

	if (!IsVideoSetting(Setting))
	{
		return;
	}

	// ---- Video ----------------------------------------------------------------------------------
	//
	// Ranges come from UTraceGameUserSettings' own constants and option arrays, never from a number
	// written down again here. A menu that clamps a slider to a range the settings class does not
	// share is a menu that will one day refuse to reach a value the settings class allows.
	const UTraceGameUserSettings* GUS = Video();
	if (GUS == nullptr)
	{
		return;
	}

	static_assert(
		int32(ESetting::QualityShading) - int32(ESetting::QualityViewDistance) + 1 == int32(ETraceQualityGroup::Count),
		"ESetting's quality rows and ETraceQualityGroup have diverged. See QualityGroupIndex.");

	if (IsQualityGroup(Setting))
	{
		OutMin = float(UTraceGameUserSettings::MinQualityLevel);
		OutMax = float(UTraceGameUserSettings::MaxQualityLevel);
		OutStep = 1.f;
		OutValue = float(GUS->GetGroupQuality(ETraceQualityGroup(QualityGroupIndex(Setting))));
		return;
	}

	switch (Setting)
	{
	case ESetting::ResolutionScale:
		OutMin = float(UTraceGameUserSettings::MinResolutionScalePercent);
		OutMax = float(UTraceGameUserSettings::MaxResolutionScalePercent);
		// 5% steps: eleven stops across the range, every one of them a round number a player can
		// report back ("I'm at 70") and a second player can reproduce exactly.
		OutStep = 5.f;
		OutValue = float(GUS->GetResolutionScalePercent());
		break;

	case ESetting::OverallQuality:
	{
		const ETraceVideoQuality Quality = GUS->GetOverallQuality();
		const bool bCustom = (Quality == ETraceVideoQuality::Custom);

		OutMin = float(UTraceGameUserSettings::MinQualityLevel);
		OutStep = 1.f;
		// CUSTOM sits one past EPIC and is reachable only by BEING there — it is what the nine rows
		// below report when they disagree, not something a player can ask for. So the range stops at
		// EPIC unless we are already in it, and right-arrow on EPIC clamps rather than stepping into
		// a state that would mean nothing if it were set.
		OutMax = float(UTraceGameUserSettings::MaxQualityLevel + (bCustom ? 1 : 0));
		OutValue = float(int32(Quality));
		break;
	}

	case ESetting::WindowMode:
	{
		const TArray<EWindowMode::Type>& Modes = UTraceGameUserSettings::GetWindowModeOptions();
		OutMin = 0.f;
		OutMax = float(FMath::Max(0, Modes.Num() - 1));
		OutStep = 1.f;
		OutValue = float(FMath::Max(0, Modes.IndexOfByKey(GUS->GetWindowMode())));
		break;
	}

	case ESetting::Resolution:
	{
		const int32 Index = GUS->GetResolutionOptionIndex();
		OutMin = 0.f;
		OutMax = float(FMath::Max(0, GUS->GetResolutionOptions().Num() - 1));
		OutStep = 1.f;
		// INDEX_NONE means the current mode is not in the list — a hand-edited ini, or a monitor
		// change since it was written. Showing entry 0 would be a lie; FormatSettingValue prints the
		// real size instead, and the first arrow press moves to a mode that IS in the list.
		OutValue = float(Index == INDEX_NONE ? 0 : Index);
		break;
	}

	case ESetting::VSync:
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		OutValue = GUS->IsVSyncEnabled() ? 1.f : 0.f;
		break;

	case ESetting::FrameRateLimit:
		OutMin = 0.f;
		OutMax = float(FMath::Max(0, UTraceGameUserSettings::GetFrameRateLimitOptions().Num() - 1));
		OutStep = 1.f;
		OutValue = float(GUS->GetFrameRateLimitIndex());
		break;

	case ESetting::FieldOfView:
		OutMin = UTraceGameUserSettings::MinFieldOfView;
		OutMax = UTraceGameUserSettings::MaxFieldOfView;
		OutStep = 1.f;
		OutValue = GUS->GetFieldOfView();
		break;

	default:
		break;
	}
}

FString FTraceOptionsMenu::FormatSettingValue(ESetting Setting, float Value) const
{
	// Every label a video row prints comes from UTraceGameUserSettings' own Describe* helpers. This
	// file does not get to decide what "EPIC" or "UNLIMITED" is called — the settings class prints
	// the same strings into its log and its Trace.Video.Status output, and a menu that spelled them
	// differently would make a bug report and the log it came with impossible to line up.
	if (IsQualityGroup(Setting))
	{
		return UTraceGameUserSettings::DescribeQualityLevel(FMath::RoundToInt(Value));
	}

	switch (Setting)
	{
	case ESetting::ResolutionScale:
		return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Value));

	case ESetting::FieldOfView:
		// No degree sign: AHUD::DrawText goes through the engine's bitmap fonts, whose glyph pages
		// are ASCII, and a missing glyph draws as a blank box that reads as a rendering fault.
		return FString::Printf(TEXT("%d DEG"), FMath::RoundToInt(Value));

	case ESetting::OverallQuality:
		return UTraceGameUserSettings::DescribeOverallQuality(ETraceVideoQuality(FMath::RoundToInt(Value)));

	case ESetting::WindowMode:
	{
		const TArray<EWindowMode::Type>& Modes = UTraceGameUserSettings::GetWindowModeOptions();
		const int32 Index = FMath::RoundToInt(Value);
		return Modes.IsValidIndex(Index)
			? UTraceGameUserSettings::DescribeWindowMode(Modes[Index])
			: FString(TEXT("N/A"));
	}

	case ESetting::Resolution:
	{
		const UTraceGameUserSettings* GUS = Video();
		if (GUS == nullptr)
		{
			return TEXT("N/A");
		}

		if (!GUS->IsResolutionSelectable())
		{
			// Not the stored size greyed out — that would still be a claim, and a false one. In
			// windowed fullscreen the window takes the desktop's size whatever this is set to, so
			// the row says what is actually true.
			return TEXT("DESKTOP");
		}

		const TArray<FTraceResolutionOption>& Options = GUS->GetResolutionOptions();
		const int32 Index = FMath::RoundToInt(Value);

		// GetResolutionOptionIndex returned INDEX_NONE — the running mode is not one of the offered
		// ones. Print the truth about the window rather than the label of a row we are not on.
		if (GUS->GetResolutionOptionIndex() == INDEX_NONE)
		{
			const FIntPoint Current = GUS->GetScreenResolution();
			return FString::Printf(TEXT("%d x %d  (CUSTOM)"), Current.X, Current.Y);
		}

		return Options.IsValidIndex(Index) ? Options[Index].Label : FString(TEXT("N/A"));
	}

	case ESetting::VSync:
	case ESetting::InvertY:
		return (Value >= 0.5f) ? TEXT("ON") : TEXT("OFF");

	case ESetting::FrameRateLimit:
	{
		const TArray<float>& Limits = UTraceGameUserSettings::GetFrameRateLimitOptions();
		const int32 Index = FMath::RoundToInt(Value);
		return Limits.IsValidIndex(Index)
			? UTraceGameUserSettings::DescribeFrameRateLimit(Limits[Index])
			: FString(TEXT("N/A"));
	}

	default:
		return FString::Printf(TEXT("%.2f"), Value);
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

	if (IsVideoSetting(Setting))
	{
		UTraceGameUserSettings* GUS = Video();
		if (GUS == nullptr)
		{
			return;
		}

		if (IsQualityGroup(Setting))
		{
			GUS->SetGroupQuality(ETraceQualityGroup(QualityGroupIndex(Setting)), FMath::RoundToInt(Snapped));
		}
		else
		{
			switch (Setting)
			{
			case ESetting::ResolutionScale:
				GUS->SetResolutionScalePercent(FMath::RoundToInt(Snapped));
				break;

			case ESetting::OverallQuality:
				// Custom is silently ignored by SetOverallQuality, which is why the range in
				// GetSettingValue only reaches it when we are already there.
				GUS->SetOverallQuality(ETraceVideoQuality(FMath::RoundToInt(Snapped)));
				break;

			case ESetting::WindowMode:
			{
				const TArray<EWindowMode::Type>& Modes = UTraceGameUserSettings::GetWindowModeOptions();
				const int32 Index = FMath::RoundToInt(Snapped);
				if (Modes.IsValidIndex(Index))
				{
					GUS->SetWindowMode(Modes[Index]);
				}
				break;
			}

			case ESetting::Resolution:
				GUS->SetResolutionByOptionIndex(FMath::RoundToInt(Snapped));
				break;

			case ESetting::VSync:
				GUS->SetVSyncEnabled(Snapped >= 0.5f);
				break;

			case ESetting::FrameRateLimit:
				GUS->SetFrameRateLimitByIndex(FMath::RoundToInt(Snapped));
				// Cheap, single-cvar, and immediate — the settings class exposes this precisely so a
				// menu row does not have to drag the whole ApplyNonResolutionSettings machine behind
				// it just to change t.MaxFPS.
				GUS->ApplyFrameRateLimitNow();
				break;

			case ESetting::FieldOfView:
				// Applies to the live camera inside the setter. Nothing further to push.
				GUS->SetFieldOfView(Snapped);
				break;

			default:
				break;
			}
		}

		// RESOLUTION and WINDOW MODE do not apply here at all: they are queued, because applying
		// them re-creates the swap chain. See bResolutionApplyPending.
		if (Setting == ESetting::Resolution || Setting == ESetting::WindowMode)
		{
			bResolutionApplyPending = true;
			ResolutionApplyAtTime = Now + ResolutionApplyDelay;
			return;
		}

		// Everything else previews live, every step, and is NOT persisted here — same contract as
		// the mouse sliders below. The commit paths (AdjustSelected, and the mouse release in
		// PollMouse) go through ApplyVideoSettings, which writes the .ini once.
		ApplyVideo(/*bResolutionAffecting=*/false, /*bPersist=*/false);
		return;
	}

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
	if (Row.Kind != ERowKind::Slider && Row.Kind != ERowKind::Toggle && Row.Kind != ERowKind::Choice)
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

	if (IsVideoSetting(Row.Setting))
	{
		// The keyboard path commits: one press, one write. RESOLUTION and WINDOW MODE are excluded —
		// SetSettingNormalised queued those, and persisting here would apply the resize this press
		// was specifically trying not to trigger. Their commit happens when the coalesce expires, or
		// when the page is left.
		if (Row.Setting != ESetting::Resolution && Row.Setting != ESetting::WindowMode)
		{
			ApplyVideo(/*bResolutionAffecting=*/false, /*bPersist=*/true);
		}

		// The window mode decides whether RESOLUTION is a live row, and OVERALL QUALITY moves nine
		// other rows. Both are visible on screen right now, so both have to be re-evaluated now.
		RefreshRowStates();
		return;
	}

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

	case ERowKind::Choice:
	{
		// Enter WRAPS where the arrows clamp. Clamping is right for arrows — holding right must
		// arrive somewhere and stay there — but a click on a row whose value is already at the top
		// has to do something, or the row reads as broken. Wrapping is the only answer that does not
		// need a second control.
		float Value = 0.f;
		float Min = 0.f;
		float Max = 1.f;
		float Step = 1.f;
		GetSettingValue(Row.Setting, Value, Min, Max, Step);

		const int32 StepsAcross = FMath::RoundToInt((Max - Min) / FMath::Max(Step, UE_KINDA_SMALL_NUMBER));
		const bool bAtTop = (Value >= Max - UE_KINDA_SMALL_NUMBER);
		AdjustSelected(bAtTop ? -StepsAcross : 1);
		return;
	}

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

	case EAction::OpenVideo:
		// Remember where we came from — the pause root and the settings page both reach this row,
		// and BACK has to undo the step the player actually took.
		VideoReturnPage = Page;
		Page = EPage::Video;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		break;

	case EAction::AutoDetectQuality:
		// Deferred one frame so the "MEASURING…" state is drawn before the benchmark blocks. Cleared
		// and run at the top of the next Tick.
		bAutoDetectPending = true;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Hardware benchmark requested."));
		break;

	case EAction::ResetVideoDefaults:
		ResetVideoToDefaults();
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
	if (Page == EPage::Video)
	{
		// A queued resize must not be able to outlive the page that queued it. Leaving the page is
		// as much a commit as pressing enter is, and a mode change that lands two frames after the
		// menu shut would look like the game deciding on its own to resize.
		if (bResolutionApplyPending)
		{
			ApplyVideo(/*bResolutionAffecting=*/true, /*bPersist=*/true);
		}

		if (VideoReturnPage == EPage::Root || VideoReturnPage == EPage::Settings)
		{
			Page = VideoReturnPage;
			IgnoreInputBeforeFrame = GFrameCounter + 1;
			RebuildRows();
			return;
		}

		Close();
		return;
	}

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
// Video
// =================================================================================================

UTraceGameUserSettings* FTraceOptionsMenu::Video()
{
	return UTraceGameUserSettings::Get();
}

void FTraceOptionsMenu::ApplyVideo(bool bResolutionAffecting, bool bPersist)
{
	UTraceGameUserSettings* GUS = Video();
	if (GUS == nullptr)
	{
		return;
	}

	if (bPersist)
	{
		// ---- The resolution-affecting apply MUST NOT run inside a Canvas draw --------------------
		//
		// This whole menu is ticked from inside AHUD::DrawHUD (see ATraceMenuHUD::DrawHUD ->
		// OptionsMenu.Tick, and the same shape on ATraceHUD). ApplyVideoSettings(true) re-requests
		// the window, which tears down and rebuilds the viewport — and doing that with a live
		// FCanvas on the stack frees the memory the canvas is still drawing into. Measured: the
		// engine's own guard fires ~100 times ("Canvas Draw functions may only be called during the
		// handling of the DrawHUD event") and the next FCanvas::PushAbsoluteTransform walks a freed
		// allocation. Both WINDOW MODE and RESOLUTION crashed the process this way.
		//
		// The proof it is the call site and not the engine call: the identical
		// ApplyVideoSettings(true) driven from a console command — same function, same arguments,
		// but executed from a ticker instead of from DrawHUD — applies cleanly and the process
		// survives. So the fix is scheduling, not the call.
		//
		// Deferring to the next core tick puts it after the draw has finished. The weak pointer is
		// to the SETTINGS object, not to this menu: the menu is a plain struct owned by the HUD and
		// can be destroyed between scheduling and firing (closing the menu is one of the paths that
		// commits a pending resize), whereas the settings object is GC-tracked and outlives it.
		if (bResolutionAffecting)
		{
			bResolutionApplyPending = false;

			TWeakObjectPtr<UTraceGameUserSettings> WeakSettings(GUS);
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([WeakSettings](float /*Delta*/) -> bool
				{
					if (UTraceGameUserSettings* Settings = WeakSettings.Get())
					{
						Settings->ApplyVideoSettings(/*bResolutionAffecting=*/true);
					}
					return false;   // fire exactly once
				}),
				0.f);
			return;
		}

		// The non-resolution half is safe inline: it writes GameUserSettings.ini, re-pushes the FOV
		// onto live cameras and broadcasts, but never touches the viewport.
		GUS->ApplyVideoSettings(/*bResolutionAffecting=*/false);
		bResolutionApplyPending = false;
		return;
	}

	// ---- Preview only ---------------------------------------------------------------------------
	//
	// Every frame of a drag comes through here, and ApplyVideoSettings(false) would be wrong for it
	// twice over: it writes and flushes the .ini (one disk write per frame), and it goes through
	// ApplyNonResolutionSettings, which re-validates every setting, walks the audio device and calls
	// every console-variable sink on the way past. None of that belongs on a slider.
	//
	// Scalability::SetQualityLevels is the narrow path: it writes the group CVars and
	// r.ScreenPercentage and skips whatever has not actually moved. It is what makes RESOLUTION
	// SCALE respond under the cursor, which is the entire reason that row is at the top of the page.
	// The release then calls this again with bPersist and the value is written exactly once.
	Scalability::SetQualityLevels(GUS->ScalabilityQuality);
}

void FTraceOptionsMenu::RunAutoDetect()
{
	UTraceGameUserSettings* GUS = Video();
	if (GUS == nullptr)
	{
		return;
	}

	// Benchmarks, applies, persists and logs — all of it next door. This row is a button, not an
	// implementation. Note that auto-detect is the one path allowed to move RESOLUTION SCALE, which
	// is correct: the player asked the machine to decide, and on a weak GPU the render scale is the
	// biggest single part of that answer.
	GUS->RunAutoDetect();

	// Nine group rows, the overall row and the scale row have all potentially moved. They are read
	// live so they redraw correctly on their own; what has to be re-derived is which rows are still
	// selectable.
	RefreshRowStates();
}

void FTraceOptionsMenu::ResetVideoToDefaults()
{
	UTraceGameUserSettings* GUS = Video();
	if (GUS == nullptr)
	{
		return;
	}

	// VIDEO only. The controls page has its own RESET, and a player who pressed this one asked about
	// their display — silently clearing their key bindings from here would be the single most
	// destructive thing this menu could do.
	GUS->ResetVideoToDefaults();

	// Any queued resize is about a mode that no longer exists in the settings; the reset has already
	// applied the one that does.
	bResolutionApplyPending = false;
	RefreshRowStates();
}

// =================================================================================================
// Field of view
//
// The VALUE and its persistence belong to UTraceGameUserSettings. What is here is only the reassert.
// =================================================================================================

void FTraceOptionsMenu::MaintainFieldOfView(APlayerController* PC)
{
	const UTraceGameUserSettings* GUS = Video();
	if (PC == nullptr || GUS == nullptr)
	{
		return;
	}

	APawn* ControlledPawn = PC->GetPawn();
	if (ControlledPawn == nullptr)
	{
		// Between death and respawn, or on the title screen, where there is no pawn at all. Drop the
		// cache so the next pawn is looked up fresh rather than inheriting a stale camera.
		FovPawn = nullptr;
		FovCamera = nullptr;
		return;
	}

	if (ControlledPawn != FovPawn.Get())
	{
		FovPawn = ControlledPawn;
		FovCamera = ControlledPawn->FindComponentByClass<UCameraComponent>();
	}

	UCameraComponent* Cam = FovCamera.Get();
	if (Cam == nullptr)
	{
		return;
	}

	// The same write UTraceGameUserSettings::ApplyFieldOfViewToWorlds does, at frame rate instead of
	// at 1 Hz. That ticker is what makes the setting survive a respawn at all, but it can be up to a
	// second late, and a second of the wrong field of view immediately after respawning is a second
	// of a shooter feeling wrong at exactly the moment the player is trying to re-orient.
	//
	// Idempotent and self-limiting: one float compare when nothing has changed, one SetFieldOfView on
	// the single frame after a respawn when it has. The two writers cannot fight — they write the
	// same value from the same source.
	//
	// NOT APlayerCameraManager::SetFOV, which was the obvious answer and is a dead end: in UE 5.8
	// LockedFOV is read by GetFOVAngle() and by nothing else in the view pipeline, so it reports a
	// number without changing a single pixel. The camera component is where the projection actually
	// comes from.
	const float DesiredFOV = GUS->GetFieldOfView();
	if (!FMath::IsNearlyEqual(Cam->FieldOfView, DesiredFOV, 0.01f))
	{
		Cam->SetFieldOfView(DesiredFOV);
	}
}

// =================================================================================================
// Live performance readout
// =================================================================================================

void FTraceOptionsMenu::UpdatePerfReadout()
{
	const double RealNow = FPlatformTime::Seconds();

	if (PerfLastRealTime <= 0.0)
	{
		// First frame on the page. There is no interval yet, and inventing one from a zero would
		// print an infinite frame rate for a quarter of a second.
		PerfLastRealTime = RealNow;
		return;
	}

	const float Delta = float(RealNow - PerfLastRealTime);
	PerfLastRealTime = RealNow;

	// A hitch of half a second — a resolution change, or the benchmark — is not the frame rate and
	// must not be averaged into it, or the readout spends the next window claiming 2 fps.
	if (Delta <= 0.f || Delta > 0.5f)
	{
		return;
	}

	PerfWindowSeconds += Delta;
	++PerfWindowFrames;

	// A quarter of a second: long enough that the number stops flickering, short enough that letting
	// go of the resolution-scale slider shows a new number before the player has moved their hand.
	if (PerfWindowSeconds >= 0.25f && PerfWindowFrames > 0)
	{
		PerfFrameMs = (PerfWindowSeconds / float(PerfWindowFrames)) * 1000.f;
		PerfFps = float(PerfWindowFrames) / PerfWindowSeconds;
		PerfWindowSeconds = 0.f;
		PerfWindowFrames = 0;

		// The RHI's own timer. Reported directly rather than through FStatUnitData, which is only
		// filled while the `stat unit` overlay is enabled and would otherwise need this page to
		// switch on an engine overlay it then has to draw around. Zero on an RHI that does not
		// implement it, and the readout simply omits the column in that case rather than printing a
		// confident 0.00.
		PerfGpuMs = float(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()));
	}
}

void FTraceOptionsMenu::DrawPerfReadout(AHUD* HUD, float RightX, float Y)
{
	if (PerfFps <= 0.f)
	{
		return;
	}

	// Amber below 45 fps, cyan above. The threshold is a judgement, not a measurement: this is a
	// shooter, and the collaborator's report was about a build that was unplayable rather than one
	// that was merely not smooth. It exists so the player can tell at a glance whether a change they
	// just made moved them across the line, which is the entire point of putting this here.
	const FLinearColor Color = (PerfFps < 45.f) ? TraceOptionsStyle::Amber : TraceOptionsStyle::Cyan;

	const FString Line = (PerfGpuMs > 0.01f)
		? FString::Printf(TEXT("%.0f FPS    %.2f MS    GPU %.2f MS"), PerfFps, PerfFrameMs, PerfGpuMs)
		: FString::Printf(TEXT("%.0f FPS    %.2f MS"), PerfFps, PerfFrameMs);

	DrawTextRight(HUD, Line, Color, RightX, Y, FontSmall, 1.1f * UIScale);
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

	// A page of four buttons does not want to be as wide as a page of sixteen labelled settings — and
	// the video page is wider still, because its values are words rather than numbers: "WINDOWED
	// FULLSCREEN" and "GLOBAL ILLUMINATION" have to fit on one line at 720p without the label and
	// the value colliding in the middle.
	float PanelW = FMath::Min(ViewW * 0.74f, 880.f * UIScale);
	if (Page == EPage::Root)
	{
		PanelW = FMath::Min(ViewW * 0.46f, 520.f * UIScale);
	}
	else if (Page == EPage::Video)
	{
		PanelW = FMath::Min(ViewW * 0.86f, 1020.f * UIScale);
	}

	const float PanelX = (ViewW - PanelW) * 0.5f;
	const float PanelY = (ViewH - PanelH) * 0.5f;
	const float CX = ViewW * 0.5f;

	HUD->DrawRect(TraceOptionsStyle::Panel, PanelX, PanelY, PanelW, PanelH);
	DrawFrame(HUD, PanelX, PanelY, PanelW, PanelH);

	// ---- Title ---------------------------------------------------------------------------------
	FString Title = TEXT("SETTINGS");
	if (Page == EPage::Root)       { Title = TEXT("PAUSED"); }
	else if (Page == EPage::Video) { Title = TEXT("VIDEO"); }

	const float TitleY = PanelY + (22.f * UIScale);
	DrawTextCentered(HUD, Title, TraceOptionsStyle::Cyan, CX, TitleY, FontLarge, 1.9f * UIScale);

	// The live readout, on the title line and only on the video page. Spec v11 §2: the collaborator
	// could tell the build was slow and had no way to measure it, so every row on this page is a
	// control whose effect is visible in the number sitting three inches above it. It is drawn last
	// in reading order but first in importance, which is why it shares the title's line rather than
	// being another row at the bottom of twenty-two.
	if (Page == EPage::Video)
	{
		DrawPerfReadout(HUD, PanelX + PanelW - (28.f * UIScale), TitleY + (14.f * UIScale));
	}

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
	else if (Page == EPage::Video)
	{
		// No BKSP/UNBIND on this page — there is nothing to unbind — and the hint says so rather
		// than offering a key that does nothing.
		Hint = TEXT("ARROWS  MOVE / ADJUST          ENTER  SELECT          ESC  BACK");
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

	// ---- Note ----------------------------------------------------------------------------------
	//
	// Amber, indented under the row it belongs to, with no plate. Amber is the palette's "this is
	// the one you want" colour and it is spent here deliberately: on a page of nineteen controls
	// the eye needs to be told which one is worth more than the other eighteen.
	if (Row.Kind == ERowKind::Note)
	{
		HUD->DrawText(Row.Label, TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Amber, 0.78f),
			X + PadX, TextY, FontSmall, 1.0f * UIScale);
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

	// A greyed row is not a selected row and cannot be, so this collapses to two states, not four.
	const FLinearColor LabelColor = !Row.bEnabled
		? TraceOptionsStyle::WithAlpha(TraceOptionsStyle::InkDim, 0.45f)
		: (bSelected ? TraceOptionsStyle::Ink : TraceOptionsStyle::InkDim);

	// Action rows are buttons; centring their label is what makes them read as one.
	if (Row.Kind == ERowKind::Action)
	{
		// AUTO-DETECT is the row a confused player on a weak machine should press, so it is the only
		// button on the page drawn in amber with a plate behind it — everything else here is a list.
		if (Row.Action == EAction::AutoDetectQuality)
		{
			const bool bMeasuring = bAutoDetectPending;
			HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Amber, bSelected ? 0.30f : 0.16f), X, Y, W, H);

			const FString Text = bMeasuring ? TEXT("MEASURING THIS MACHINE...") : Row.Label;
			DrawTextCentered(HUD, Text, bMeasuring ? FLinearColor::White : TraceOptionsStyle::Amber,
				X + W * 0.5f, TextY, FontMedium, LabelScale);
			return;
		}

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

	// Every remaining kind reads its value the same way, which is what lets one Toggle path serve
	// INVERT MOUSE Y and VSYNC and one Choice path serve all thirteen enumerated video rows.
	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float Step = 1.f;
	GetSettingValue(Row.Setting, Value, Min, Max, Step);

	if (Row.Kind == ERowKind::Toggle)
	{
		const bool bOn = (Value >= 0.5f);
		DrawTextRight(HUD, FormatSettingValue(Row.Setting, Value),
			bOn ? TraceOptionsStyle::Amber : TraceOptionsStyle::InkDim,
			ValueRight, TextY, FontMedium, LabelScale);
		return;
	}

	if (Row.Kind == ERowKind::Choice)
	{
		const FString ValueText = FormatSettingValue(Row.Setting, Value);

		// Arrow glyphs, drawn only on the selected row and only on the side there is somewhere to go.
		// This is the one affordance that tells a player a row is a LIST rather than a label, and
		// without it the quality groups look like readouts.
		const FLinearColor ValueColor = !Row.bEnabled
			? TraceOptionsStyle::WithAlpha(TraceOptionsStyle::InkDim, 0.45f)
			: (bSelected ? TraceOptionsStyle::Ink : TraceOptionsStyle::InkDim);

		// The arrow columns are reserved WHETHER OR NOT an arrow is drawn in them. If the value moved
		// right every time it hit the end of its range, every list on the page would twitch sideways
		// as the player walked it, which reads as a layout bug rather than as an end stop.
		const float ArrowColW = 20.f * UIScale;
		const float ValueW = MeasureWidth(HUD, ValueText, FontMedium, LabelScale);
		const float ValueTextRight = ValueRight - ArrowColW;

		DrawTextRight(HUD, ValueText, ValueColor, ValueTextRight, TextY, FontMedium, LabelScale);

		if (bSelected && Row.bEnabled)
		{
			const FLinearColor ArrowColor = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.9f);
			if (Value > Min + UE_KINDA_SMALL_NUMBER)
			{
				DrawTextRight(HUD, TEXT("<"), ArrowColor, ValueTextRight - ValueW - (6.f * UIScale),
					TextY, FontMedium, LabelScale);
			}
			if (Value < Max - UE_KINDA_SMALL_NUMBER)
			{
				DrawTextRight(HUD, TEXT(">"), ArrowColor, ValueRight, TextY, FontMedium, LabelScale);
			}
		}
		return;
	}

	// ---- Slider --------------------------------------------------------------------------------
	const float Alpha = FMath::Clamp((Value - Min) / FMath::Max(UE_KINDA_SMALL_NUMBER, Max - Min), 0.f, 1.f);

	const FString ValueText = FormatSettingValue(Row.Setting, Value);
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
