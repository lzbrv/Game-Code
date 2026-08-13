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
#include "UObject/WeakObjectPtr.h"

#include "Settings/TraceUserSettings.h"   // ETraceInputAction

class AHUD;
class APawn;
class APlayerController;
class UCameraComponent;
class UFont;
class UTraceGameUserSettings;

/**
 * The settings / pause overlay.
 *
 * Lifecycle: Open*() -> Tick() once per frame from DrawHUD -> Close(), which fires OnClosed so the
 * host can put its input mode back. The host must stop routing its own input while IsOpen().
 */
class TRACE_API FTraceOptionsMenu
{
public:
	/**
	 * Clears the dev console command's pointer to this instance. See GActiveOptionsMenu in the .cpp.
	 *
	 * The only reason this class has a destructor at all — it owns nothing else that needs one.
	 */
	~FTraceOptionsMenu();

	/** Which page is on screen. Closed is a real state, not a null one — Tick() is a no-op in it. */
	enum class EPage : uint8
	{
		Closed = 0,
		/** RESUME / SETTINGS / VIDEO / RETURN TO TITLE / QUIT. In-match only; the title screen has none. */
		Root,
		Settings,
		/**
		 * Spec v11 §2 — resolution, resolution scale, quality, VSync, frame cap, auto-detect, FOV.
		 *
		 * ITS OWN PAGE, not a section of Settings. Settings is already sixteen rows of mouse and
		 * bindings and this adds twenty-four more, nineteen of them controls; one forty-row list would
		 * clamp the row pitch to its 18px floor and stop being readable at 720p. It also lets the pages
		 * be entered independently — the pause root gets a VIDEO row straight to here, because a player
		 * whose frame rate has collapsed should not walk through a page about mouse sensitivity first.
		 */
		Video
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

	/** Opens straight on the video page, with BACK closing the overlay. */
	void OpenVideo();

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

#if !UE_BUILD_SHIPPING
	/**
	 * Drives one row exactly as the arrow keys would: select it, adjust it, commit it, log it.
	 *
	 * Public only so the Trace.Menu.Nudge console command can reach it. It goes through the SAME
	 * MoveSelection / AdjustSelected path a key press does — not a shortcut into the settings class —
	 * because the thing it exists to verify is this file's write path, and a test that bypassed it
	 * would pass with every row on the page disconnected.
	 *
	 * @param RowsFromTop  index into Rows, counting headers and notes, so it can be read straight off
	 *                     a screenshot.
	 * @param Delta        arrow-key steps: -1 is one press of left.
	 */
	void DebugNudge(int32 RowsFromTop, int32 Delta);

	/**
	 * Screen rect of the row whose label is exactly @p Label, as of the last draw.
	 *
	 * Public only so ATraceMenuHUD's click harness (spec v15 §4) can park a real cursor on a real
	 * row and then click it through the real input pipeline. Returning the rect rather than a row
	 * index is deliberate: the thing being measured is the MOUSE path, and a harness that reached in
	 * by index would prove nothing about hit testing.
	 *
	 * @return false when the page has not been drawn yet (every rect is still invalid) or no row on
	 *         the current page carries that label.
	 */
	bool DebugGetRowRect(const TCHAR* Label, FBox2D& OutRect) const;
#endif

private:
#if !UE_BUILD_SHIPPING
	/**
	 * `-TraceMenuActivate=<ROW LABEL>`: presses one row, once, a few DRAWN FRAMES after the overlay
	 * opens — through MoveSelection/ActivateSelected, the same path Enter takes.
	 *
	 * WHY IT COUNTS DRAWS AND NOT SECONDS, which is the whole reason it has to exist: the pause menu
	 * stops the world in standalone, so every world timer freezes the instant the menu appears — and
	 * both -TraceExec and -TraceAutoShot are scheduled on world timers. There was therefore NO
	 * headless way to photograph a page one keystroke deeper than the in-match pause root, which is
	 * exactly where spec v20 §0.6's slider, key chips and KEYBIND/KEY lettering live. ATraceHUD's own
	 * auto-pause screenshot counts drawn frames for the identical reason.
	 *
	 *     -TraceAutoPause=6 -TraceMenuActivate=SETTINGS
	 *
	 * Dev-only, opt-in, and it drives the real rows rather than reaching past them, so a capture it
	 * produces is evidence about the menu and not about the harness.
	 */
	void TickAutoActivate();

	/** Drawn frames since the overlay opened. Reset by every Open*(). */
	int32 DrawsSinceOpen = 0;
	bool bAutoActivateDone = false;
#endif

private:

private:
	// ---- Rows -----------------------------------------------------------------------------------

	enum class ERowKind : uint8
	{
		/** Section caption. Never selectable; navigation skips it. */
		Header,
		/**
		 * A line of explanatory prose under the row above it. Never selectable.
		 *
		 * Exists for exactly one row — RESOLUTION SCALE — and that is the point. Spec v11 §0 says the
		 * frame is GPU-bound per pixel, so that one slider is worth more than every other control on
		 * the page put together, and a player who does not know that will slide it back to 100 and
		 * work through the quality groups instead. A control that needs a sentence gets a sentence.
		 */
		Note,
		/** Continuous value with a draggable track. */
		Slider,
		/** On / off. */
		Toggle,
		/** One of an ordered, named set: window mode, resolution, a quality level. */
		Choice,
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
		OpenVideo,
		ReturnToTitle,
		Quit,
		ResetDefaults,
		/** Runs the engine's hardware benchmark and applies what it decides. Spec v11 §2.8. */
		AutoDetectQuality,
		/**
		 * Puts the VIDEO page back to the shipped defaults. DELIBERATELY SEPARATE from ResetDefaults,
		 * which is the controls page's own reset: a player who pressed a reset on a page about their
		 * display did not ask to lose their key bindings, and one row that quietly did both would be
		 * the most destructive control in this menu.
		 */
		ResetVideoDefaults,
		Back
	};

	/**
	 * Which setting a Slider / Toggle / Choice row drives. Kept as an enum so rows hold no pointers.
	 *
	 * Everything from ResolutionScale down lives on UTraceGameUserSettings rather than on
	 * UTraceUserSettings — see IsVideoSetting, and the file-scope comment in the .cpp about why the
	 * two are read and written through different paths.
	 */
	enum class ESetting : uint8
	{
		None = 0,
		Sensitivity,
		SensitivityY,
		InvertY,

		/**
		 * Spec v14 §3 — "Include a toggle in game settings to turn off all characters".
		 *
		 * INSERTED ABOVE THE VIDEO BLOCK, deliberately. IsVideoSetting() is `>= ResolutionScale`, so
		 * anything added below ResolutionScale silently becomes a "video setting" and gets read and
		 * written through UTraceGameUserSettings, which knows nothing about it — the row would draw
		 * 0.00 and adjusting it would do nothing at all. Nothing that is not video goes below this
		 * line.
		 *
		 * Its value lives in neither of the two settings objects this page normally talks to: see
		 * TraceCharacters in UI/TraceMatchOptions.h for where it persists and why.
		 */
		CharactersEnabled,

		// ---- Video. IsVideoSetting() is the boundary and depends on this ordering. ----
		ResolutionScale,
		OverallQuality,
		WindowMode,
		Resolution,
		VSync,
		FrameRateLimit,
		FieldOfView,

		// The nine scalability groups, in the order spec v11 §2.5 lists them.
		QualityViewDistance,
		QualityAntiAliasing,
		QualityPostProcess,
		QualityShadows,
		QualityGlobalIllumination,
		QualityReflections,
		QualityTextures,
		QualityEffects,
		QualityShading,

		Count
	};

	/** True for everything that lives on UTraceGameUserSettings instead of UTraceUserSettings. */
	static bool IsVideoSetting(ESetting Setting)
	{
		return Setting >= ESetting::ResolutionScale && Setting < ESetting::Count;
	}

	/** True for the nine Scalability::FQualityLevels groups, which share one get/set path. */
	static bool IsQualityGroup(ESetting Setting)
	{
		return Setting >= ESetting::QualityViewDistance && Setting <= ESetting::QualityShading;
	}

	/**
	 * The nine QualityXxx rows above are the nine ETraceQualityGroup entries in the same order, so
	 * the group is simply the offset from the first.
	 *
	 * Asserted rather than assumed, in the .cpp, where ETraceQualityGroup::Count is visible. ESetting
	 * is this file's identity for a row and ETraceQualityGroup is the settings file's identity for a
	 * setting; the two lists are maintained by different hands, and the failure mode of them drifting
	 * is a row that silently adjusts its neighbour — which looks like a renderer bug and would be
	 * hunted in entirely the wrong file.
	 */
	static int32 QualityGroupIndex(ESetting Setting)
	{
		return int32(Setting) - int32(ESetting::QualityViewDistance);
	}

	struct FRow
	{
		ERowKind Kind = ERowKind::Action;
		FString Label;
		EAction Action = EAction::None;
		ESetting Setting = ESetting::None;
		ETraceInputAction Binding = ETraceInputAction::Count;

		/**
		 * False when the row is meaningless right now — currently only RESOLUTION in windowed
		 * fullscreen, which always takes the desktop's size. Greyed rather than removed: a row that
		 * vanishes and reappears makes the list jump under the selection, and the player has no way
		 * to learn that the mode they picked is what took their resolution choice away.
		 */
		bool bEnabled = true;

		/** Screen rect of the whole row as of the last draw. Hit testing and hover use it. */
		FBox2D Rect = FBox2D(ForceInit);

		/** Screen rect of a slider's track. Click and drag map the cursor's X across this. */
		FBox2D Track = FBox2D(ForceInit);

		bool IsSelectable() const
		{
			return Kind != ERowKind::Header && Kind != ERowKind::Note && bEnabled;
		}
	};

	TArray<FRow> Rows;
	int32 Selected = 0;

	/** Rebuilds Rows for the current page and puts the selection on the first selectable row. */
	void RebuildRows();

	/**
	 * Re-evaluates FRow::bEnabled and pulls the selection off any row that just became unselectable.
	 *
	 * Called once per frame rather than only on change: the enable state depends on the window mode,
	 * which the player can also change from the console or by dragging the window, and a stale grey
	 * row that still takes clicks is worse than a cheap per-frame recompute of twenty-two booleans.
	 */
	void RefreshRowStates();

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

	/** How the value reads on screen: "72%", "1920 x 1080", "EPIC", "UNLIMITED", "ON". */
	FString FormatSettingValue(ESetting Setting, float Value) const;

	// ---- Video ----------------------------------------------------------------------------------
	//
	// THIS PAGE OWNS NO VIDEO STATE. Every value on it is read live from UTraceGameUserSettings on
	// the frame it is drawn and written straight back on the frame it is changed — there is no cache
	// here to go stale, and no second copy of a resolution list, a preset ladder or a frame cap for
	// the two files to disagree about. What this class contributes is layout, input and the argument
	// about ORDER; the meaning of every setting belongs next door.

	/** The settings object, or null during shutdown. Every caller must null-check. */
	static UTraceGameUserSettings* Video();

	/**
	 * Pushes pending video changes into the renderer.
	 *
	 * @param bResolutionAffecting  true only for RESOLUTION and WINDOW MODE, which physically resize
	 *                              the window. Never true from a slider or a key repeat.
	 * @param bPersist              go through UTraceGameUserSettings::ApplyVideoSettings, which
	 *                              applies AND writes the .ini. False gives a live preview only —
	 *                              which is what every frame of a drag wants, because the alternative
	 *                              is one ini flush per frame.
	 */
	void ApplyVideo(bool bResolutionAffecting, bool bPersist);

	void RunAutoDetect();
	void ResetVideoToDefaults();

	// ---- Field of view --------------------------------------------------------------------------
	//
	// The VALUE lives in UTraceGameUserSettings, which persists it and pushes it onto live cameras.
	// What is added here is only the reassert: that class does it from a 1 Hz ticker, and a respawn
	// mid-second would leave the player looking through the constructor's 95 degrees for up to a
	// second before it snapped back. Tick runs every frame whether the overlay is open or not, and a
	// cached weak pointer plus one float compare closes that window for nothing.

	void MaintainFieldOfView(APlayerController* PC);

	// ---- Live performance readout ---------------------------------------------------------------

	/** Rolls real (never dilated, never paused) frame times into PerfFps / PerfFrameMs / PerfGpuMs. */
	void UpdatePerfReadout();

	void DrawPerfReadout(AHUD* HUD, float RightX, float Y);

	// ---- Draw -----------------------------------------------------------------------------------

	void Draw(AHUD* HUD);
	void DrawRow(AHUD* HUD, FRow& Row, float X, float Y, float W, float H, bool bSelected);
	void DrawFrame(AHUD* HUD, float X, float Y, float W, float H);
	void DrawCursor(AHUD* HUD);

	/**
	 * The artist's value chip (T_MenuValueBox), landed so its PLATE is exactly (X, Y, W, H).
	 *
	 * Spec v20 §0.6. Two callers want it — the key chip on a Binding row and the value column beside
	 * a Slider — and both of them already computed that rectangle for the plain cyan rect they used
	 * to draw, so nothing about the layout moves.
	 *
	 * @return false when the sprite is unavailable (missing, uncooked, or Trace.Menu.Art 0) and
	 *         NOTHING was drawn, which is the caller's cue to draw what it drew before. Every sprite
	 *         on this screen is optional in exactly this way; see the art block in the .cpp.
	 */
	bool DrawValueChip(AHUD* HUD, float X, float Y, float W, float H) const;

	/** Centred text helper; AHUD::DrawText is top-left anchored and has no measure-and-centre form. */
	void DrawTextCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale);
	void DrawTextRight(AHUD* HUD, const FString& Text, const FLinearColor& Color, float RightX, float Y, UFont* Font, float Scale);
	float MeasureWidth(AHUD* HUD, const FString& Text, UFont* Font, float Scale);
	float MeasureHeight(AHUD* HUD, const FString& Text, UFont* Font, float Scale);

	// ---- State ----------------------------------------------------------------------------------

	EPage Page = EPage::Closed;

	/** True on the title screen, where there is no match to go back to. BACK closes outright. */
	bool bSettingsIsRootPage = false;

	/**
	 * Where BACK goes from the VIDEO page.
	 *
	 * Video is reachable from three places — the pause root, the settings page, and directly — and
	 * "back" has to mean the place the player actually came from rather than one hardcoded parent.
	 * Closed means "this page was the entry point, so BACK closes the overlay".
	 */
	EPage VideoReturnPage = EPage::Settings;

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

	/**
	 * Where the pointer was on the previous poll, so hover can tell "the player moved onto this row"
	 * from "the pointer happens to be resting there".
	 *
	 * Without this, PollMouse sets Selected = HoverRow EVERY frame, and it runs after PollNavigation
	 * — so an arrow key moved the selection and the pointer immediately dragged it back, meaning the
	 * keyboard did nothing at all whenever the cursor happened to sit over a row. Measured three
	 * times through the real viewport input path, and it is also what made the -TraceAutoSettings
	 * script land on the wrong rows depending on viewport size.
	 */
	FVector2D LastHoverCursorPos = FVector2D::ZeroVector;
	bool bHasHoverCursorPos = false;

	/** Row armed by the current mouse-down, or INDEX_NONE. Activation happens on release. */
	int32 PressedRow = INDEX_NONE;

	/** Set when the mouse-down landed on a slider track: every subsequent frame drags the value. */
	bool bDraggingSlider = false;

	// ---- Video state ----------------------------------------------------------------------------
	//
	// Three flags and two weak pointers. Everything else about the video settings is read from
	// UTraceGameUserSettings at the moment it is needed — see the block above ApplyVideo.

	/**
	 * Set by the AUTO-DETECT row, consumed at the top of the NEXT Tick.
	 *
	 * RunHardwareBenchmark blocks the game thread for the better part of a second. Running it inline
	 * from the click would freeze on the frame BEFORE any feedback was drawn, so the player sees a
	 * hitch with no explanation and presses it again. Deferring by one frame means "MEASURING…" is
	 * on screen for the whole stall.
	 */
	bool bAutoDetectPending = false;

	/**
	 * A pending window resize, coalesced.
	 *
	 * RESOLUTION and WINDOW MODE are the only two rows whose application physically re-creates the
	 * swap chain, and they are also rows a player will hold an arrow key on to walk down a list of
	 * twelve modes. Resizing the window once per repeat tick — every 55 ms — makes the machine
	 * unusable for a second and can drop the mouse capture. So the value changes instantly and
	 * visibly, and the resize happens once, a beat after the player stops moving.
	 */
	bool bResolutionApplyPending = false;
	float ResolutionApplyAtTime = 0.f;

	/** How long the coalesce waits. Long enough to walk a list, short enough to feel like a result. */
	static constexpr float ResolutionApplyDelay = 0.35f;

	/** Resolved once per pawn rather than per frame; FindComponentByClass walks the component array. */
	TWeakObjectPtr<APawn> FovPawn;
	TWeakObjectPtr<UCameraComponent> FovCamera;

	// ---- Live performance readout ---------------------------------------------------------------
	//
	// Spec v11 §2: "a player changing settings can see the effect immediately rather than guessing."
	//
	// Measured from FPlatformTime::Seconds and NOT from the Now the host passes in, which is
	// UWorld::GetTimeSeconds — world time is dilated and stops dead if anything ever pauses, and a
	// frame-rate readout that reports the frame rate of a paused world is worse than none. Real time
	// is the only clock that answers the question the player is asking.

	double PerfLastRealTime = 0.0;
	float PerfWindowSeconds = 0.f;
	int32 PerfWindowFrames = 0;

	float PerfFps = 0.f;
	float PerfFrameMs = 0.f;
	/** From RHIGetGPUFrameCycles. Zero when the RHI does not report it; the readout then hides it. */
	float PerfGpuMs = 0.f;
};
