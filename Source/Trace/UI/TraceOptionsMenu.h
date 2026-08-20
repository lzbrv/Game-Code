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
#include "UI/Text/TraceTextWeight.h"      // ETraceTextWeight - which FACE a string is set in (v26 §2)

class AHUD;
class APawn;
class APlayerController;
class UCameraComponent;
class UFont;
class UTraceGameUserSettings;

// =================================================================================================
// SPEC v25 §1 — THE FOREGROUND-CANVAS ELEVATION IS GONE. *** THIS IS THE SETTINGS CRASH. ***
//
// There used to be an `FTraceOverSlateCanvas` here: a scope that swapped the FCanvas inside the
// game's UCanvas for `FViewport::GetDebugCanvas()` — the engine's FOREGROUND canvas, which Slate
// paints on top of the UMG screens — so that opening SETTINGS did not make the UMG title screen
// stand down (spec v23 §A2). It shipped, it was reported as a crash, it was "fixed" twice, and both
// fixes were wrong because both of them accepted the elevation and argued about its arguments.
//
// IT WAS THE ELEVATION ITSELF. The foreground canvas is not a surface you may borrow. The engine
// builds it per frame in `FDebugCanvasDrawer::InitDebugCanvas` as a DEFERRED canvas
// (`CDM_DeferDrawing`, `Allow_DeleteOnRender`) and hands it to the RENDER thread later, from Slate's
// paint pass, via `FSceneViewport::PaintDebugCanvas` -> `BeginRenderingCanvas` ->
// `FSlateDrawElement::MakeCustom`. Everything drawn into it therefore outlives the frame that
// recorded it, holding raw resource pointers — textures, and a shared reference to the viewport —
// until somebody else decides to flush it. The engine knows this is sharp: it is the whole reason
// `FDebugCanvasDrawer::HandleReleaseFontResources` exists, and that hook only rescues the engine's
// OWN font atlas, not this project's sprites, its Sofachrome atlas, or the panel's tiles.
//
// MEASURED, spec v25 §1, one binary and one PIE harness, one switch apart:
//
//     elevation ON  (Trace.UI.ModalDPIGuard 0)  -> SETTINGS draws, then at PIE teardown
//                                                  "Assertion failed: GameViewport.IsUnique()"
//                                                  (SLevelViewport.cpp:5196) -> SIGSEGV. The EDITOR
//                                                  dies, which is what "crashes the entire game"
//                                                  looks like from the outside.
//     elevation OFF (-TraceModalUnderSlate)     -> SETTINGS draws, PIE-End, Test Completed Success.
//
// Two more captured stacks from the same defect, both on the render thread, both while the settings
// panel was up, are in the crash reports this fix was written from:
//     FBatchedElements::Draw <- FCanvasBatchedElementRenderItem::Render_RenderThread
//     GetMetalSurfaceFromRHITexture(FRHITexture*) <- SetShaderParameters   (a dead texture)
// Neither is editor-specific; both are what a deferred canvas does with pointers whose owner has
// moved on. The DPI comparison the previous fix added was never treating any of this — it only
// happened to switch the elevation OFF in PIE on a Retina display, which is why the PIE runs went
// green while the owner, whose two canvases agree about DPI, kept crashing.
//
// WHAT THIS COSTS, STATED PLAINLY. A Canvas modal is composited UNDER Slate again, so while SETTINGS
// or the JOIN prompt is up, ATraceMenuHUD stands its UMG title screen down and draws the Canvas one
// (see DrawHUD). That is the v23 §A2 defect coming back: the screen behind the panel changes
// renderer for as long as the panel is open. It is a cosmetic regression on one screen, and it is
// the trade spec v25 §1 asks for — a game that cannot open its settings is worse than a title screen
// that dims differently while they are open.
//
// DO NOT PUT THIS BACK by borrowing an engine canvas. If the modals must survive over UMG, the
// answer is to draw them AS Slate — a real widget, or a custom SWidget that owns its own FCanvas and
// its own resource lifetimes — not to write into a surface whose flush schedule belongs to somebody
// else.
// =================================================================================================

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
		Video,

		/**
		 * SPEC v29 §3 — "Add a page in settings to customize the crosshair."
		 *
		 * ITS OWN PAGE, for the same reason VIDEO is: the settings page is already nineteen rows of
		 * mouse and bindings, and seven more would push the pitch further into its 18px floor at 720p.
		 * It is also the only page here that wants LAYOUT of its own — a live preview needs a rectangle
		 * beside the list, which a section of a scrolling list cannot have.
		 *
		 * REACHED FROM THE SETTINGS PAGE AND NOT FROM THE PAUSE ROOT, which is the one place it differs
		 * from VIDEO. Video earned its root row by an argument spec v11 §0 measured: the player it
		 * exists for has a collapsed frame rate and must not be made to walk past mouse sensitivity
		 * first. A crosshair has no such emergency, and a five-row pause root is worth more than a
		 * sixth destination on it. It IS on the settings page, which is the only route the title screen
		 * has to anything.
		 */
		Crosshair
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

	/**
	 * SPEC v29 §3 — opens straight on the crosshair page, with BACK closing the overlay.
	 *
	 * Twin of OpenVideo, and it exists for the same headless reason: `Trace.Menu.Crosshair` is the
	 * only way a run with no keyboard can photograph this page, and a page nobody can photograph is a
	 * page nobody can be shown to have checked.
	 */
	void OpenCrosshair();

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

	/**
	 * SPEC v28 §3a — starts the press-counting proof on this overlay. See TickRebindProof.
	 *
	 * Public only so the Trace.Keys.RebindProof console command and the -TraceRebindProof launch flag
	 * can reach it, exactly as DebugNudge is public for Trace.Menu.Nudge.
	 */
	void DebugBeginRebindProof();
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

	// ---- SPEC v28 §3a — the press-counting harness ----------------------------------------------
	//
	// See TickRebindProof. All of this is dev-only and inert until the console command or the launch
	// flag arms it.

	enum class ERebindProofStage : uint8
	{
		Idle = 0,
		/** Waiting for the settings page to exist and to have been DRAWN at least once. */
		WaitForPage,
		/** Injecting complete LMB down/up pairs on the row until the capture opens. */
		ClickRow,
		/** Injecting complete key down/up pairs until the binding actually changes. */
		PressKey,
		Report,
	};

	ERebindProofStage RebindProofStage = ERebindProofStage::Idle;
	/** Drawn frames left before the harness takes its next action. Every edge gets its own frame. */
	int32 RebindProofWait = 0;
	int32 RebindProofSubStep = 0;
	int32 RebindProofClicks = 0;
	int32 RebindProofPresses = 0;
	int32 RebindProofClicksNeeded = 0;
	int32 RebindProofPressesNeeded = 0;
	/** Which action and slot the run is rebinding, and what it is putting there. */
	ETraceInputAction RebindProofAction = ETraceInputAction::Count;
	int32 RebindProofSlot = 0;
	/** Every slot of every action as it was before the run, put back by the Report stage. */
	TArray<FKey> RebindProofBefore;

	/** `-TraceRebindProof=<draws>` arms once per process, never once per page opening. */
	bool bRebindProofArmedFromCommandLine = false;
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
		/** SPEC v29 §3 — the CROSSHAIR row on the settings page. */
		OpenCrosshair,
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
		/**
		 * SPEC v29 §3 — puts the CROSSHAIR page back to its shipped defaults, and NOTHING ELSE.
		 *
		 * A third separate reset, for the reason ResetVideoDefaults gives above and which is now a
		 * pattern rather than a one-off: a reset row belongs to the page it is drawn on. A player who
		 * pressed reset on a page about their crosshair did not ask to lose their key bindings or their
		 * resolution, and it goes through UTraceUserSettings::ResetCrosshairToDefaults, which touches
		 * the seven crosshair fields and no others.
		 */
		ResetCrosshairDefaults,
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

		/**
		 * SPEC v29 §3 — the seven crosshair rows.
		 *
		 * *** ABOVE THE VIDEO BLOCK, AND THAT IS LOAD-BEARING, NOT TIDINESS. *** IsVideoSetting() is
		 * `>= ResolutionScale`, so anything placed below that line is read and written through
		 * UTraceGameUserSettings — which knows nothing about a crosshair. The rows would draw 0.00 and
		 * adjusting them would do nothing at all: a settings page whose controls are silently
		 * disconnected. CharactersEnabled's comment above says the same thing for the same reason.
		 *
		 * All seven live on UTraceUserSettings, which is where the player's own taste belongs (see the
		 * header of Settings/TraceUserSettings.h).
		 */
		CrosshairSize,
		CrosshairThickness,
		CrosshairGap,
		CrosshairColor,
		CrosshairOpacity,
		CrosshairDot,
		CrosshairOutline,

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

		/**
		 * SPEC v28 §3c — screen rect of each key chip on a Binding row, as of the last draw.
		 *
		 * "Both editable in the settings page" is a HIT TEST as much as it is a data model: with two
		 * chips on one row, a click has to be able to say WHICH of them the player meant. Written by
		 * DrawRow, read by PollMouse, invalid on every row that is not a Binding.
		 */
		FBox2D KeyChip[UTraceUserSettings::MaxKeysPerAction] = { FBox2D(ForceInit), FBox2D(ForceInit) };

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

	/** The slot a Binding row is currently editing, clamped to the row and to MaxKeysPerAction. */
	int32 ActiveBindingSlot() const;

#if !UE_BUILD_SHIPPING
	/**
	 * SPEC v28 §3a — `Trace.Keys.RebindProof`, and the reason it drives the menu from Tick().
	 *
	 * The claim is "ONE press binds a key", and the only honest way to count presses is to make them
	 * REAL: this parks the OS cursor on a real row, injects mouse and key edges through
	 * UGameViewportClient::InputKey — the same call FSceneViewport makes for a physical device — and
	 * then asks the SETTINGS OBJECT what it holds. It never touches bCapturingKey, SetKey or
	 * ActivateSelected directly, because a harness that reached past the input path would pass on a
	 * build where the input path is exactly what is broken.
	 *
	 * DRIVEN FROM Tick AND NOT FROM A TIMER, which is the part the pause menu forces: the in-match
	 * overlay stops the world, so every world timer freezes the instant it opens. Drawn frames are
	 * the only clock that still runs, and they are also the clock the menu's own polling runs on.
	 */
	void TickRebindProof(APlayerController* PC);
#endif

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

	// ---- Crosshair (spec v29 §3) ----------------------------------------------------------------
	//
	// THIS PAGE OWNS NO CROSSHAIR STATE EITHER. Every row reads UTraceUserSettings live on the frame
	// it is drawn and writes straight back on the frame it changes, exactly as the video page does
	// with UTraceGameUserSettings — so there is no cached copy here to go stale, and the preview below
	// cannot be showing a value the game is not using.

	/**
	 * The live preview: the real crosshair, at its real size, over the two backgrounds it has to
	 * survive.
	 *
	 * THE SHAPE COMES FROM UTraceUserSettings::BuildCrosshairBars — the SAME function
	 * ATraceHUD::DrawAimReticle calls — rather than from a copy of the arithmetic here. A preview with
	 * its own geometry is a preview that agrees with the game until somebody edits one of the two,
	 * and then lies about the setting it exists to show.
	 *
	 * TWO BACKGROUNDS, SPLIT DOWN THE MIDDLE, and that is the whole point of previewing a crosshair in
	 * a menu rather than just printing the numbers. This arena is a black floor under saturated cyan
	 * neon, and the failure mode that produced "there is no crosshair" was a reticle that was legible
	 * over one and invisible over the other. The cross sits on the seam, so a colour or an opacity
	 * that only works over half the arena is visibly wrong here instead of in a firefight.
	 *
	 * DRAWN AT ACTUAL SIZE (PixelScale = UIScale, first-person scale 1.0), not magnified. The player
	 * is choosing how big their crosshair should be on THIS screen, and a preview at 3x would answer a
	 * question nobody asked. The caption says so, because a small preview otherwise reads as a bug.
	 */
	void DrawCrosshairPreview(AHUD* HUD, float X, float Y, float W, float H);

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

	// ---- Text ------------------------------------------------------------------------------------
	//
	// SPEC v26 §2 — @p Weight is which FACE the string is set in, and it has NO DEFAULT.
	//
	// This page draws headers in Sofachrome and its body in Erbaum Bold, and the three faces share no
	// advances (Erbaum measures the alphabet a third narrower than Sofachrome ExtraLight). A defaulted
	// face would therefore let a future call site measure in one face and draw in another and produce a
	// layout that is wrong by a third — silently, in a screenshot, rather than in a compile. Making it
	// explicit costs one argument per call and makes that class of mistake unexpressible. The names to
	// pass are TraceOptionsMenuType::HeaderFace / ::BodyFace, in the .cpp beside the reasoning.

	/**
	 * Which face an ACTION row's label is set in — the one place §2's split needed a judgement.
	 *
	 * Sofachrome on the pause root (it is the in-match main menu), Erbaum on the settings and video
	 * pages (they are submenus). The whole argument is at the definition in the .cpp.
	 */
	ETraceTextWeight FaceForAction() const;

	/** Centred text helper; AHUD::DrawText is top-left anchored and has no measure-and-centre form. */
	void DrawTextCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale,
		ETraceTextWeight Weight);
	void DrawTextRight(AHUD* HUD, const FString& Text, const FLinearColor& Color, float RightX, float Y, UFont* Font, float Scale,
		ETraceTextWeight Weight);
	float MeasureWidth(AHUD* HUD, const FString& Text, UFont* Font, float Scale, ETraceTextWeight Weight);

	/** The line box. Face-independent by construction — every atlas shares it — so it takes no weight. */
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

	/**
	 * SPEC v29 §3 — where BACK goes from the CROSSHAIR page. Same contract as VideoReturnPage.
	 *
	 * Settings by default because that is the only row that reaches it; Closed when the page was
	 * entered directly (OpenCrosshair, or the Trace.Menu.Crosshair console command), so BACK closes
	 * the overlay instead of dropping the player onto a page they never opened.
	 */
	EPage CrosshairReturnPage = EPage::Settings;

	// ---- SPEC v28 §3a — THE SWALLOWED FIRST PRESS -----------------------------------------------
	//
	// *** THIS IS THE "REBINDING NEEDS TWO PRESSES" BUG, AND IT IS NOT IN THIS CLASS'S LOGIC. ***
	//
	// FSceneViewport::OnMouseButtonDown does not forward every press to the game. Its rule is
	//
	//     bTemporaryCapture    = captureMode == CaptureDuringMouseDown (or the RMB variant)
	//     bProcessInputPrimary = !IsCurrentlyGameViewport() || HasMouseCapture()
	//                            || captureMode == CapturePermanently_IncludingInitialMouseDown
	//     if (bTemporaryCapture || bProcessInputPrimary) -> ViewportClient->InputKey(IE_Pressed)
	//
	// while the RELEASE is forwarded unconditionally. The TITLE SCREEN sets
	// EMouseCaptureMode::NoCapture on purpose (ATraceMenuPlayerController::BeginPlay, to kill the
	// stray "initial mouse down" the old CapturePermanently_IncludingInitialMouseDown replayed), so
	// none of those three terms is true and the FIRST press a player makes is dropped on the floor.
	//
	// MEASURED, Trace.Keys.RebindProof, one binary, two hosts:
	//     title screen      click 1: "will NOT forward ... captureMode=0 hasMouseCapture=0"
	//                       -> five complete clicks on a keybind chip, capture never opened
	//     in-match pause    click 1: "WILL forward ... captureMode=3" -> 1 click, 1 press, bound
	// The in-match host differs by exactly one thing: FInputModeGameAndUI sets CaptureDuringMouseDown
	// there, so the press survives. That is the whole defect.
	//
	// THE FIX IS TO STOP SWALLOWING PRESSES WHILE THE OVERLAY IS UP, and to put the viewport back
	// exactly as it was when it closes. CaptureDuringMouseDown is the mode the in-match host already
	// proves correct and is NOT the mode whose exit replayed the stray click, so nothing spec v15 §4
	// fixed comes back. Restoring the previous value rather than assuming NoCapture is what keeps this
	// honest on a host that never had the problem.

	/** Set while this overlay has raised the viewport's capture mode. Drives the restore on close. */
	bool bMouseCaptureModeOverridden = false;

	/** Whatever the viewport was in before the overlay raised it. Put back verbatim by Close(). */
	uint8 PreviousMouseCaptureMode = 0;

	/**
	 * Raises the viewport's capture mode so a press is delivered, or puts it back.
	 *
	 * A no-op when the viewport would already forward a press — which is every in-match opening — so
	 * the pause menu's input mode is never fought over.
	 */
	void SetPressDeliveryOverride(bool bEnable);

	/** Waiting for the next key press to become a binding. */
	bool bCapturingKey = false;
	ETraceInputAction CapturingAction = ETraceInputAction::Count;

	/** SPEC v28 §3c — which of the action's two slots the pending capture will write. */
	int32 CapturingSlot = 0;

	/**
	 * SPEC v28 §3c — which key chip the selection is on, for the Binding row under `Selected`.
	 *
	 * Kept as ONE member rather than per-row state, and reset by MoveSelection, because it is the
	 * cursor's column and not a property of the row: walking away from a row and back must not
	 * remember that the player was editing its second bind three rows ago. LEFT/RIGHT move it (a
	 * Binding row has no other use for the horizontal axis) and a click sets it from the chip that
	 * was actually hit.
	 */
	int32 SelectedBindingSlot = 0;

	/**
	 * Frame number before which no input is read.
	 *
	 * Both openings and rebind captures need it, for the same reason: the key press that OPENED the
	 * overlay (Escape) and the key press that STARTED a capture (Enter) are both still "just pressed"
	 * for the remainder of the frame they arrived on. Without this, Escape would open and immediately
	 * close the menu, and Enter would bind itself to whatever row the player was on.
	 */
	uint64 IgnoreInputBeforeFrame = 0;

	/**
	 * Bindable keys that were ALREADY HELD when the rebind capture opened.
	 *
	 * A single-frame IgnoreInputBeforeFrame is not enough on its own. In the in-match pause menu the
	 * viewport runs a different mouse-capture mode, so the click that opens a capture can still be
	 * physically down several frames later — and a key that was down before the capture existed was
	 * never a choice the player made inside it. Anything in here is refused until it is seen RELEASED
	 * at least once, which turns "whatever happened to be down" into "the next key you actually press".
	 */
	TArray<FKey> KeysHeldWhenCaptureOpened;

	/** Set when a capture opens; the first poll that has a PlayerController fills the list above. */
	bool bCaptureNeedsHeldSnapshot = false;

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
