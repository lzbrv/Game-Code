// Trace — the settings overlay, shared by the title screen and the in-game pause menu.
//
// WHY ONE CLASS FOR BOTH
// The player has to be able to fix their mouse from the title screen (before they are being shot at)
// and from inside a match (which is where they actually notice it is wrong). Those are two different
// HUDs on two different maps with two different player controllers. Writing the panel twice would
// mean two layouts, two hit-test implementations and two places to forget a new setting — so this is
// a plain C++ class that both AHUDs own an instance of and drive from their own DrawHUD.
//
// IT IS NOT A UOBJECT, deliberately. It holds no UObject references that outlive a frame, it never
// needs to replicate, and keeping it out of UHT means the two HUD headers can include it without
// gaining a reflected dependency. The AHUD it draws through is passed in per frame and never stored.
//
// INPUT IS POLLED, NOT BOUND.
// Every other input path in this project goes through a binding — Enhanced Input in the match,
// UInputComponent::BindKey on the title screen. This one polls APlayerController::
// WasInputKeyJustPressed instead, for three reasons that all point the same way:
//   1. Rebinding needs "tell me which key was just pressed, whatever it was". There is no binding
//      that answers that; FInputKeyBinding on EKeys::AnyKey does not report WHICH key fired.
//   2. The in-game menu pauses the world in standalone, and a bound delegate only runs while paused
//      if every single binding remembered to set bExecuteWhenPaused. Polling has no such trapdoor.
//   3. One implementation then serves both hosts, instead of one bound path and one polled path
//      that drift.
// The cost is that the two hosts must not ALSO route their own bindings here while this is open;
// both of them check IsOpen() first.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Math/Box2D.h"
#include "Templates/Function.h"

#include "Settings/TraceUserSettings.h"   // ETraceInputAction

class AHUD;
class APlayerController;
class UFont;

/**
 * The settings / pause overlay.
 *
 * Lifecycle: Open*() -> Tick() once per frame from DrawHUD -> Close(), which fires OnClosed so the
 * host can put its input mode back. The host must stop routing its own input while IsOpen().
 */
class TRACE_API FTraceOptionsMenu
{
public:
	/** Which page is on screen. Closed is a real state, not a null one — Tick() is a no-op in it. */
	enum class EPage : uint8
	{
		Closed = 0,
		/** RESUME / SETTINGS / RETURN TO TITLE / QUIT. In-match only; the title screen has no use for it. */
		Root,
		Settings
	};

	// ---- Host callbacks -------------------------------------------------------------------------
	//
	// All optional. An unset callback simply makes the corresponding row absent, which is how the
	// title screen gets a settings panel with no RESUME row without needing a second layout.

	/** Close the overlay and hand control back to the game. Presence of this adds the RESUME row. */
	TFunction<void()> OnResume;

	/** Leave the match. Presence of this adds the RETURN TO TITLE row. */
	TFunction<void()> OnReturnToTitle;

	/** Quit the application. Presence of this adds the QUIT row. */
	TFunction<void()> OnQuit;

	/** Always called when the overlay stops being open, whatever closed it. */
	TFunction<void()> OnClosed;

	// ---- Lifecycle ------------------------------------------------------------------------------

	/** Opens on the pause root. Use in-match. */
	void OpenRoot();

	/** Opens straight on the settings page, with BACK closing the overlay. Use on the title screen. */
	void OpenSettings();

	/** Closes, fires OnClosed, and abandons any rebind that was in progress. */
	void Close();

	bool IsOpen() const { return Page != EPage::Closed; }

	/** True while the overlay is waiting for the player to press the key they want to bind. */
	bool IsCapturingKey() const { return bCapturingKey; }

	/**
	 * Polls input and draws. Call exactly once per frame from the owning AHUD::DrawHUD while open.
	 *
	 * @param HUD       the drawing surface; every pixel goes through AHUD::DrawRect/DrawText/DrawLine
	 * @param PC        the controller whose key state is polled. May be null; the frame is then draw-only
	 * @param UIScale   the host's own layout scale, so the overlay matches the screen it sits on
	 */
	void Tick(AHUD* HUD, APlayerController* PC, float InViewW, float InViewH, float InUIScale, float InNow);

private:
	// ---- Rows -----------------------------------------------------------------------------------

	enum class ERowKind : uint8
	{
		/** Section caption. Never selectable; navigation skips it. */
		Header,
		/** Continuous value with a draggable track. */
		Slider,
		/** On / off. */
		Toggle,
		/** A rebindable action. */
		Binding,
		/** A button: RESUME, RESET, BACK... */
		Action
	};

	enum class EAction : uint8
	{
		None = 0,
		Resume,
		OpenSettings,
		ReturnToTitle,
		Quit,
		ResetDefaults,
		Back
	};

	/** Which continuous setting a Slider row drives. Kept as an enum so rows hold no pointers. */
	enum class ESetting : uint8
	{
		None = 0,
		Sensitivity,
		SensitivityY,
		InvertY
	};

	struct FRow
	{
		ERowKind Kind = ERowKind::Action;
		FString Label;
		EAction Action = EAction::None;
		ESetting Setting = ESetting::None;
		ETraceInputAction Binding = ETraceInputAction::Count;

		/** Screen rect of the whole row as of the last draw. Hit testing and hover use it. */
		FBox2D Rect = FBox2D(ForceInit);

		/** Screen rect of a slider's track. Click and drag map the cursor's X across this. */
		FBox2D Track = FBox2D(ForceInit);

		bool IsSelectable() const { return Kind != ERowKind::Header; }
	};

	TArray<FRow> Rows;
	int32 Selected = 0;

	/** Rebuilds Rows for the current page and puts the selection on the first selectable row. */
	void RebuildRows();

	// ---- Input ----------------------------------------------------------------------------------

	void PollInput(APlayerController* PC);
	void PollNavigation(APlayerController* PC);
	void PollMouse(APlayerController* PC);
	void PollKeyCapture(APlayerController* PC);

	/** Moves the selection by @p Delta, skipping headers and clamping at both ends. */
	void MoveSelection(int32 Delta);

	/** Left / right on the selected row. @p Delta is -1 or +1. */
	void AdjustSelected(int32 Delta);

	/** Enter / click on the selected row. */
	void ActivateSelected();

	/** Escape / BACK. Leaves the settings page for the root page, or closes when there is no root. */
	void GoBack();

	/** Applies a slider value from a normalised 0..1 position, for click-and-drag on the track. */
	void SetSettingNormalised(ESetting Setting, float Alpha);

	/** Current value, its range and its printed form, for both drawing and adjusting. */
	void GetSettingValue(ESetting Setting, float& OutValue, float& OutMin, float& OutMax, float& OutStep) const;

	// ---- Draw -----------------------------------------------------------------------------------

	void Draw(AHUD* HUD);
	void DrawRow(AHUD* HUD, FRow& Row, float X, float Y, float W, float H, bool bSelected);
	void DrawFrame(AHUD* HUD, float X, float Y, float W, float H);
	void DrawCursor(AHUD* HUD);

	/** Centred text helper; AHUD::DrawText is top-left anchored and has no measure-and-centre form. */
	void DrawTextCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale);
	void DrawTextRight(AHUD* HUD, const FString& Text, const FLinearColor& Color, float RightX, float Y, UFont* Font, float Scale);
	float MeasureWidth(AHUD* HUD, const FString& Text, UFont* Font, float Scale);
	float MeasureHeight(AHUD* HUD, const FString& Text, UFont* Font, float Scale);

	// ---- State ----------------------------------------------------------------------------------

	EPage Page = EPage::Closed;

	/** True on the title screen, where there is no match to go back to. BACK closes outright. */
	bool bSettingsIsRootPage = false;

	/** Waiting for the next key press to become a binding. */
	bool bCapturingKey = false;
	ETraceInputAction CapturingAction = ETraceInputAction::Count;

	/**
	 * Frame number before which no input is read.
	 *
	 * Both openings and rebind captures need it, for the same reason: the key press that OPENED the
	 * overlay (Escape) and the key press that STARTED a capture (Enter) are both still "just pressed"
	 * for the remainder of the frame they arrived on. Without this, Escape would open and immediately
	 * close the menu, and Enter would bind itself to whatever row the player was on.
	 */
	uint64 IgnoreInputBeforeFrame = 0;

	/** Cached per-frame view metrics, set at the top of Tick. */
	float ViewW = 0.f;
	float ViewH = 0.f;
	float UIScale = 1.f;
	float Now = 0.f;

	UFont* FontSmall = nullptr;
	UFont* FontMedium = nullptr;
	UFont* FontLarge = nullptr;

	// ---- Held-key repeat ------------------------------------------------------------------------
	//
	// A sensitivity slider with no key repeat takes 60 presses to cross its range, which is the kind
	// of detail that decides whether an options screen feels finished. One shared implementation
	// serves both axes: -1/+1 for adjust, -1/+1 for navigation.

	int32 LastAdjustDir = 0;
	float NextAdjustTime = 0.f;
	int32 LastNavDir = 0;
	float NextNavTime = 0.f;

	static constexpr float RepeatDelay = 0.38f;
	static constexpr float RepeatInterval = 0.055f;
	static constexpr float NavRepeatInterval = 0.12f;

	// ---- Mouse --------------------------------------------------------------------------------

	FVector2D CursorPos = FVector2D::ZeroVector;
	bool bHasCursor = false;
	bool bMouseWasDown = false;

	/** Row armed by the current mouse-down, or INDEX_NONE. Activation happens on release. */
	int32 PressedRow = INDEX_NONE;

	/** Set when the mouse-down landed on a slider track: every subsequent frame drags the value. */
	bool bDraggingSlider = false;
};
