// Trace — title screen implementation. See TraceMenuHUD.h.

#include "UI/TraceMenuHUD.h"

#include "Audio/TraceAudio.h"         // spec v26 §9 — ButtonPress, client-side
#include "Audio/TraceMusicPlayer.h"   // FX/audio plan §5.7 — the title loop, started in BeginPlay
#include "Blueprint/UserWidget.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "Engine/PendingNetGame.h"    // Trace.Menu.JoinVerify reads FWorldContext::PendingNetGame
#include "Engine/Texture2D.h"         // WP9 — the Canvas title's sprite cache
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"             // FViewport::IsForegroundWindow
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "HAL/IConsoleManager.h"      // FAutoConsoleVariableRef — spec v15 §4's red arm
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"      // GConfig — the WP8.2 version string reads ProjectVersion once
#include "Misc/CoreMiscDefines.h"     // FInputDeviceId
#include "Misc/Parse.h"
#include "HAL/PlatformApplicationMisc.h"   // D32-PADMENU — X pastes into the JOIN field
#include "HAL/PlatformTime.h"
#include "Settings/TraceGamepadInput.h"   // D32-PADMENU — TracePadMenu, the shared pad vocabulary
#include "Settings/TraceUserSettings.h"
#include "TimerManager.h"
#include "Trace.h"                    // LogTraceGame
#include "UI/Text/TraceCanvasText.h"   // spec v22 §A1 — this renderer types from the atlas
#include "UI/Text/TraceGameText.h"     // the editable wording, Config/TraceGameText.ini
#include "UI/TraceAutoShot.h"
#include "UI/TraceHardwareCursor.h"    // spec v24 §2 — one pointer on screen, not two
#include "UI/TraceMatchOptions.h"
#include "UI/TraceNetworking.h"
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"   // WP9 — the Canvas title draws the artist's sprites
#include "UI/Widgets/Menu/TraceMenuKit.h"        // ...through the shared, guarded kit renderer
#include "UI/Widgets/Menu/TraceMenuPalette.h"
#include "UI/Widgets/Menu/TraceTitleMenuWidget.h"

// The palette and the layout constants used to live here. They moved to
// UI/Widgets/Menu/TraceMenuPalette.h in spec v17 §4, because there are now two renderers for this
// screen and two copies of a palette is two palettes that drift. Nothing about the values changed.

// =================================================================================================
// Stroke font
//
// Five letters and a space, drawn as line segments in a unit box (x and y both 0..1, y downward).
// The built-in engine fonts are bitmaps; at the size a title wants they are a blurry mess. Segments
// cost nothing, stay sharp at any resolution, and look like something a light cycle would drive on.
//
// The comment here used to add "and there is no .uasset budget for a real typeface (contract: no
// assets)". THAT CONTRACT IS RETIRED — see spec v17 §0 and Trace.Build.cs. UTraceStrokeText
// (UI/Widgets/Menu/TraceStrokeTextWidget.h) is the same five glyphs for the UMG renderer.
//
// SINCE THE RELEASE PASS (UI plan WP9) THIS IS THE WORDMARK'S FALLBACK, NOT ITS RENDERER: the
// Canvas title and the travel card draw the artist's T_TraceWordmark sprite whenever it is loadable
// and drawable, and these glyphs (in white) are what a fresh checkout (or a failed import) shows
// instead of nothing.
// =================================================================================================

// Spec v22 §A1 — this renderer types in the artist's face too. See TraceMenuHUDType below.
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
// Spec v17 §4 — the UMG renderer, and the toggle that keeps Canvas alive
// =================================================================================================

// =================================================================================================
// SPEC v22 §A1 — THE CANVAS MENU TYPES IN THE ARTIST'S FACE
// =================================================================================================
//
// This renderer is not the default any more, but it is not dead either. Until spec v23 §A2 the JOIN
// prompt stood the UMG title screen down for the frames it was up, so the shipped, default menu
// dropped onto THIS code the moment a player pressed JOIN — photographed at 1920x1080 in
// v22integ_06_join_prompt.png: a screen of Sofachrome became a screen of the engine's stand-in font
// in one keypress. §A2 removed the stand-down; what still reaches this code is the JOIN PROMPT
// ITSELF, which is Canvas on either path, plus every frame of a genuine Canvas session.
//
// Same translation as the settings page, for the same reason, and the reasoning is written out once
// at the top of UI/TraceOptionsMenu.cpp: this file's layout vocabulary is (UFont*, Scale) and
// TraceText wants a point size, so SizeFor() MEASURES what that font at that scale draws and
// inverts TraceText::LineHeight to match it. Row heights and baselines therefore do not move; only
// the letterforms and the measured widths do.
// =================================================================================================

namespace TraceMenuHUDType
{
	/** The point size whose line height equals what @p Font at @p Scale draws. */
	static float SizeFor(AHUD* HUD, UFont* Font, float Scale)
	{
		float MeasuredW = 0.f;
		float MeasuredH = 0.f;
		if (HUD != nullptr)
		{
			HUD->GetTextSize(TEXT("Ag"), MeasuredW, MeasuredH, Font, Scale);
		}

		const float UnitLine = TraceText::LineHeight(1.f);
		if (MeasuredH > 1.f && UnitLine > KINDA_SMALL_NUMBER)
		{
			return MeasuredH / UnitLine;
		}
		return FMath::Max(1.f, 16.f * Scale);
	}

	static void Draw(AHUD* HUD, const FString& Text, const FLinearColor& Color,
		float X, float Y, UFont* Font, float Scale,
		TraceText::EHAlign HAlign = TraceText::EHAlign::Left)
	{
		TraceText::FStyle Style(SizeFor(HUD, Font, Scale), Color);
		Style.HAlign = HAlign;
		TraceCanvasText::Draw(HUD, Text, X, Y, Style);
	}
}

namespace TraceMenuHUDFile
{
	/**
	 * `Trace.UI.UseUMG 0|1` — which renderer draws the title screen.
	 *
	 * Either way the SCREEN BEHAVES THE SAME. `Trace.UI.VerifyMenu` measures the widget's laid-out
	 * row rectangles against the Canvas layout maths for the same frame and reports the worst error
	 * in pixels — hit testing, hover and the click harness all run off those rectangles, so if they
	 * agree the two renderers are interchangeable to the player's mouse. Last measured on spec v19
	 * §5: 6 of 6 rows, worst corner error 0.00 px, with the red arm (each row against its NEIGHBOUR)
	 * correctly failing at 355 px. -TraceMenuClickTest still reports one press per row,
	 * -TraceAutoSettings still opens the overlay and restores the config, -TraceAutoPlay still
	 * reaches the arena.
	 *
	 * Set it to 0 and everything falls back to the Canvas path with no other change. That is not a
	 * degraded mode: it is the shipped renderer, still compiled, still tested, and still what draws
	 * whenever the widget asset is missing or the wrong shape.
	 *
	 * IT IS WHAT DRAWS BEHIND A MODAL, AGAIN (spec v25 §1 reverts spec v23 §A2). The settings overlay
	 * and the JOIN prompt force this path for their frames: a Canvas modal is composited under Slate,
	 * so the widget has to stand down to be seen past. v23 §A2 avoided that by elevating the modals
	 * onto the engine's foreground canvas, and that elevation is what crashed the game on open. See
	 * the header of UI/TraceOptionsMenu.h.
	 */
	/**
	 * DEFAULT 1 — UMG. FLIPPED BY THE SPEC v20 INTEGRATION PASS. It was 0 from v17 to v19.
	 *
	 * *** THIS IS THE LINE THAT DECIDES WHETHER A PLAYER SEES THE ARTIST'S MENU. The sheet — the
	 * TRACE wordmark, the swoosh, the three button plates, the PLAY and SETTINGS lettering, the
	 * pointer — is drawn by the UMG tree ONLY. At 0 the title screen is the old stroked-vector one
	 * and none of the art is on screen. The user's report that opened spec v20 was, in their words,
	 * "the menu UI currently looks the same, not sure if anything was changed" — and it looked the
	 * same because of this number, not because the art was missing. ***
	 *
	 * The v17 retreat listed three measured reasons. ALL THREE ARE NOW CLOSED:
	 *
	 *   1. STARTUP — closed in v19. The cause was in Scripts/generate-menu-widgets.py: it retired
	 *      widgets by renaming them (TraceRetired_1, ...) inside the package, so their variable GUIDs
	 *      stayed in the .uasset and the widget compiler re-resolved and ensure()d over them on every
	 *      load. The generator now renames them into the TRANSIENT package. Re-measured headless on
	 *      /Game/Maps/MainMenu: pre-v19 asset 49 TraceRetired resolves, 13 ensures, 23.1 s wall;
	 *      regenerated asset 0 and 0 and 17.0 s, against 17.5 s for the Canvas path.
	 *   2. THE WORDMARK — closed in v19/v20. It is no longer stroked at all; it is the artist's own
	 *      sprite (T_TraceWordmark), navy in its amber glow as the handmade image draws it.
	 *   3. ONLY TWO OF FOUR SCREENS EXIST — CLOSED IN v20, and this was the whole of the reason the
	 *      switch stayed at 0. FTraceOptionsMenu now draws the artist's button plates, slider,
	 *      value chips, KEYBIND/KEY lettering and cursor on the SHARED options path, so the title
	 *      screen's SETTINGS page and the in-match pause SETTINGS page get the same treatment.
	 *      ATraceCharacterSelect was redesigned onto the same plates, chips and cursor.
	 *      Both are still CANVAS renderers, and v20 shipped with opening SETTINGS handing the screen
	 *      back to the Canvas path mid-session. SPEC v23 §A2 CLOSED THAT TOO: the modals draw on the
	 *      foreground canvas, in front of Slate, so the title screen no longer changes renderer
	 *      underneath them (see bUseWidgetThisFrame in DrawHUD).
	 *
	 * WHAT WAS WALKED BEFORE FLIPPING, at 1920x1080, every frame looked at (Saved/Screenshots/v20integ):
	 * title -> SETTINGS -> BACK -> character select -> live match -> Escape -> in-match SETTINGS.
	 *
	 * THE SHARED-SWITCH CONSEQUENCE, CHECKED RATHER THAN ASSUMED: TraceHUD.cpp's in-match ammo/status
	 * corner follows this variable whenever `Trace.UI.HUD.UseUMG` is -1 (its default), so this flip
	 * moves that corner onto its UMG path too. Photographed both arms in a live match on this build
	 * (v20integ_corner-umg1 / v20integ_corner-umg0): same ammo string, same 30 ticks, same [R] RELOAD,
	 * same box - the UMG one is a few pixels tighter and crisper. `Trace.UI.HUD.UseUMG 0` still A/Bs
	 * that corner back to Canvas alone if anyone wants it.
	 *
	 * Set this to 0 and everything falls back to the Canvas path with no other change. That is not a
	 * degraded mode: it is the shipped v16 renderer, still compiled, still tested, and still what
	 * draws whenever the widget asset is missing or the wrong shape.
	 */
	static int32 GUseUMG = 1;
	static FAutoConsoleVariableRef CVarUseUMG(
		TEXT("Trace.UI.UseUMG"),
		GUseUMG,
		TEXT("1 = DEFAULT since spec v20. Draw the title screen with the UMG widget\n")
		TEXT("    (/Game/Trace/UI/Menu/WBP_TitleMenu). THIS IS THE ONLY PATH THAT DRAWS THE\n")
		TEXT("    ARTIST'S MENU ART.\n")
		TEXT("0 = the original AHUD::DrawHUD Canvas path, which has none of that art on it. Both\n")
		TEXT("    are live; Canvas is also used automatically whenever the asset is missing or the\n")
		TEXT("    wrong shape, and for the settings and JOIN modals.\n")
		TEXT("The launch stall, the broken wordmark and the art-less OPTIONS / CHARACTER SELECT\n")
		TEXT("screens that kept this at 0 are all closed - see the comment above the variable.\n")
		TEXT("NOTE this switch is shared with the in-match HUD corner - see the comment above it."),
		ECVF_Default);

	/**
	 * Red arm for the one-pointer-one-owner guard in ATraceMenuHUD::DrawCursor. 1 restores the old
	 * behaviour: this HUD draws its cyan cross at the frozen cursor position even while the settings
	 * overlay is drawing the artist's arrow at the live one. Exists so the guard is falsifiable from
	 * a single binary, per spec v20 §4's red-arm rule.
	 */
	static int32 GCursorRedArm = 0;
	static FAutoConsoleVariableRef CVarCursorRedArm(
		TEXT("Trace.Menu.CursorRedArm"),
		GCursorRedArm,
		TEXT("1 = draw the title screen's own cyan cursor even while the settings overlay owns the\n")
		TEXT("    pointer, i.e. the pre-v20 double-cursor behaviour. 0 = DEFAULT, one pointer.\n")
		TEXT("Diagnostic only. See ATraceMenuHUD::DrawCursor."),
		ECVF_Cheat);

	/** Where the generated asset lives. Soft, by path: a missing asset must fall back, not fail. */
	static const TCHAR* TitleWidgetPath = TEXT("/Game/Trace/UI/Menu/WBP_TitleMenu.WBP_TitleMenu_C");

	/**
	 * Command line beats console variable, and that ordering is not arbitrary.
	 *
	 * A cvar set with -ExecCmds arrives during engine init, which is AFTER the first title screen has
	 * already decided which renderer to build. Step 6 hit exactly this with the input assets. So the
	 * switches are read from FCommandLine directly, where they are available before anything runs.
	 *
	 *   -TraceNoMenuUMG   force the Canvas path (the fallback drill: run this to prove rule 1)
	 *   -TraceMenuUMG     force the widget, even if the cvar says otherwise
	 */
	static bool WantsUMG()
	{
		if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoMenuUMG")))
		{
			return false;
		}
		if (FParse::Param(FCommandLine::Get(), TEXT("TraceMenuUMG")))
		{
			return true;
		}
		return GUseUMG != 0;
	}

	/**
	 * Release UI plan WP8.2 — "V 0.1.0   NET 3F9A1C2E", bottom-right of the title screen.
	 *
	 * Read once from the one place a version already exists — ProjectVersion in
	 * Config/DefaultGame.ini ([/Script/EngineSettings.GeneralProjectSettings]) — rather than a
	 * second constant that would drift from it.
	 *
	 * *** THE SECOND HALF IS THE CROSS-PLAY CHECK, AND IT IS HERE BECAUSE THIS IS THE ONLY PLACE A
	 * *** SHIPPING BUILD CAN SHOW IT. *** Two machines can only connect if their network
	 * compatibility values match (TraceNet::NetProtocolVersion documents what the value is made of).
	 * There is a console command that prints it — Trace.NetVersion — and a Display log line, and a
	 * SHIPPING GAME HAS NEITHER: logging is compiled out and the console is unavailable. The friends
	 * this project is played with run Shipping builds. So the value goes on the title screen, where
	 * comparing two machines is "read the bottom-right corner of both screens" and takes five
	 * seconds instead of an evening of "connection failed".
	 *
	 * The version half is dropped when the ini has no version (an empty corner beats "V "), but the
	 * NET half is ALWAYS drawn: it is the half that has a job.
	 *
	 * Both renderers show it from this one string: the Canvas path draws it in DrawHUD, and
	 * BuildMenuView hands it to the widget in the view (a Canvas draw alone cannot serve the UMG
	 * frames — AHUD's canvas composites UNDER Slate, which is the same fact that makes the modals
	 * stand the widget down).
	 */
	static const FString& ProjectVersionLabel()
	{
		static const FString Label = []() -> FString
		{
			FString Version;
			if (GConfig != nullptr)
			{
				GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"),
					TEXT("ProjectVersion"), Version, GGameIni);
			}
			Version.TrimStartAndEndInline();

			const FString NetLabel = TraceNet::GetNetVersionLabel();
			return Version.IsEmpty()
				? NetLabel
				: TRACE_TEXTF("MENU.VERSION_LINE", "V {0}   {1}", { Version, NetLabel });
		}();
		return Label;
	}
}

// =================================================================================================
// THE JOIN PROMPT AND THE TRAVEL CARD, ON THE HANDMADE KIT
// =================================================================================================
//
// Both used to be the pre-kit design — a flat dark rectangle with 1.6 px cyan edges, cyan title,
// cyan caret, cyan key hints — drawn over a Canvas title screen that had turned into a cyan Tron
// grid. They are the kit now: black, the artist's plates and words, the kit's [KEY] VERB legend, and
// the white blade pointer. Layout in 1080p reference px (x UIScale). Named namespaces, not anonymous:
// this module is a unity build.

namespace TraceMenuHUDJoin
{
	/** Indices into ATraceMenuHUD::JoinButtonRects. */
	static constexpr int32 Connect = 0;
	static constexpr int32 Back    = 1;
	static constexpr int32 ButtonCount = 2;

	static constexpr float PanelW       = 820.f;   // the black panel, at most (and 0.86 of the view)
	static constexpr float PanelTopFrac = 0.28f;   // of the view height
	static constexpr float PanelPadTop  = 34.f;
	static constexpr float PanelPadBottom = 30.f;
	static constexpr float PanelAlpha   = 0.90f;   // over the kit's 0.82 scrim, as the options pages

	static constexpr float TitleCap     = 30.f;    // Sofachrome, white — the options pages' title size
	static constexpr float SubtitleCap  = 12.f;
	static constexpr float SubtitleGap  = 18.f;    // title caps' bottom to the subtitle caps' top
	static constexpr float FieldTop     = 118.f;   // panel top to the field plate (its glow clears the subtitle)
	static constexpr float FieldH       = 64.f;
	static constexpr float FieldSide    = 60.f;    // panel edge to the field plate
	static constexpr float FieldTextCap = 22.f;    // Erbaum Bold, the settings pages' body face
	static constexpr float FieldTextPad = 28.f;    // plate edge to the first character
	static constexpr float NoteGap      = 24.f;    // field bottom to the note's cap centre
	static constexpr float NoteCap      = 12.f;
	static constexpr float ButtonsGap   = 50.f;    // field bottom to the buttons' top
	static constexpr float ButtonH      = 60.f;    // the title rows' height
	static constexpr float ButtonW      = 240.f;
	static constexpr float ButtonSpacing = 44.f;
	static constexpr float LegendGap    = 34.f;    // buttons' bottom to the legend's chips
	static constexpr float LegendChipH  = 28.f;    // the options pages' legend chip
	static constexpr float LegendLineGap = 38.f;
	static constexpr float MachineGap   = 28.f;    // last legend line to the "THIS MACHINE" caps centre
	static constexpr float MachineCap   = 11.f;
}

namespace TraceMenuHUDPointer
{
	/**
	 * The stand-in pointer for the frame or two before the blade sprite is drawable (and a build with
	 * no menu art): a small white gap-cross on the tip. White, like the blade — never the old cyan.
	 */
	static void DrawCross(AHUD* HUD, const FVector2D& Tip, float InUIScale)
	{
		if (HUD == nullptr)
		{
			return;
		}
		const float Arm = 9.f * InUIScale;
		const float Weight = FMath::Max(1.f, 1.5f * InUIScale);
		const FLinearColor Ink(1.f, 1.f, 1.f, 0.9f);
		HUD->DrawLine(Tip.X - Arm, Tip.Y, Tip.X - Arm * 0.35f, Tip.Y, Ink, Weight);
		HUD->DrawLine(Tip.X + Arm * 0.35f, Tip.Y, Tip.X + Arm, Tip.Y, Ink, Weight);
		HUD->DrawLine(Tip.X, Tip.Y - Arm, Tip.X, Tip.Y - Arm * 0.35f, Ink, Weight);
		HUD->DrawLine(Tip.X, Tip.Y + Arm * 0.35f, Tip.X, Tip.Y + Arm, Ink, Weight);
	}
}

namespace TraceMenuHUDTravel
{
	static constexpr float CaptionCap   = 22.f;    // white, Sofachrome
	static constexpr float CaptionY     = 0.47f;   // of the view height, the caption's cap centre
	static constexpr float ElapsedGap   = 40.f;    // caption to the elapsed line's cap centre
	static constexpr float ElapsedCap   = 13.f;
	static constexpr float LegendGap    = 34.f;    // elapsed line to the legend's chips
	static constexpr float LegendChipH  = 28.f;
	static constexpr float LegendLineGap = 38.f;

	/** "12s" — how long the join has been dialling. Ranen's words (MENU.TRAVEL_ELAPSED). */
	static FString ElapsedText(const AActor* Context, float StartRealTime)
	{
		const UWorld* World = (Context != nullptr) ? Context->GetWorld() : nullptr;
		const float Elapsed = (World != nullptr) ? FMath::Max(0.f, World->GetRealTimeSeconds() - StartRealTime) : 0.f;
		return TRACE_TEXTF("MENU.TRAVEL_ELAPSED", "{0}s", { FMath::FloorToInt(Elapsed) });
	}
}

// =================================================================================================
// RELEASE UI PLAN WP9 — THE CANVAS TITLE DRAWS THE ARTIST'S ART TOO
// =================================================================================================
//
// The Canvas renderer is what a player sees behind every SETTINGS/JOIN modal (spec v25 §1 stands the
// UMG widget down for those frames) and whenever the widget is off or missing. Until this pass it
// drew the stroke-vector wordmark on plateless rows — so opening a modal swapped 58.8% of the frame
// to what read as a DIFFERENT GAME under the scrim. The full Slate/UMG modal rebuild is deferred
// (TraceOptionsMenu.h documents why); the shipped mitigation is to draw the artist's sprites HERE
// too: wordmark, swoosh, and the button plates (9-sliced by the kit out of nine DrawTexture calls;
// at a fixed row height that is exactly the 3-slice this file used to draw). The stroke wordmark and
// the flat rects remain as the fallback whenever a texture is unavailable — the standing rule that a
// missing texture must leave the menu drawable.
//
// THE GUARD IS NOT OPTIONAL. "A LOADED TEXTURE IS NOT A DRAWABLE ONE": AHUD::DrawTexture hands
// Texture->GetResource() straight to an FCanvasTileItem, and a texture whose FTextureResource has no
// RHI texture yet is a SIGSEGV on the render thread ~130 ms later. The sprites, the guard and the
// plate are the shared kit renderer's now (UI/Widgets/Menu/TraceMenuKit.h) — this file used to carry
// its own copy of all three. TraceMenuKit::Sprite returns null until a texture is drawable, and every
// caller below keeps the stroke / rect fallback it had.

bool ATraceMenuHUD::TryAdoptMenuWidget()
{
	if (!TraceMenuHUDFile::WantsUMG())
	{
		// Re-logged whenever the ANSWER changes, not just once. `Trace.UI.UseUMG 0` typed into the
		// console mid-session switches the screen back to Canvas without a restart, and a status line
		// still reading "UMG adopted" while Canvas is drawing is exactly the kind of stale report that
		// makes a fallback impossible to trust.
		if (!bMenuUmgDecisionLogged || bMenuUmgWantedLastDecision)
		{
			bMenuUmgDecisionLogged = true;
			bMenuUmgWantedLastDecision = false;
			MenuUmgStatus = (MenuWidget != nullptr)
				? FString(TEXT("CANVAS — the widget was adopted, then Trace.UI.UseUMG was set to 0. ")
					TEXT("It is standing down; set it back to 1 to bring it in again."))
				: FString(TEXT("CANVAS — Trace.UI.UseUMG is 0 (or -TraceNoMenuUMG was passed)."));
			UE_LOG(LogTraceGame, Display, TEXT("[MenuUI] %s"), *MenuUmgStatus);
		}
		return false;
	}

	if (MenuWidget != nullptr)
	{
		if (!bMenuUmgWantedLastDecision)
		{
			bMenuUmgWantedLastDecision = true;
			MenuUmgStatus = FString::Printf(TEXT("UMG — %s adopted, %d rows."),
				TraceMenuHUDFile::TitleWidgetPath, static_cast<int32>(ETraceMenuRow::Count));
			UE_LOG(LogTraceGame, Display, TEXT("[MenuUI] %s"), *MenuUmgStatus);
		}
		return true;
	}

	APlayerController* PC = GetOwningPlayerController();
	if (PC == nullptr)
	{
		// No local controller yet. Not a failure and not logged as one — try again next frame.
		return false;
	}

	// LoadClass, not a constructor FClassFinder. A finder would make the asset a HARD dependency of
	// this class: a fresh clone without the .uasset would fail to construct the HUD at all, which is
	// the exact opposite of the fallback spec v17 §0 rule 1 demands.
	UClass* WidgetClass = LoadClass<UTraceTitleMenuWidget>(nullptr, TraceMenuHUDFile::TitleWidgetPath);
	if (WidgetClass == nullptr)
	{
		if (!bMenuUmgDecisionLogged)
		{
			bMenuUmgDecisionLogged = true;
			MenuUmgStatus = FString::Printf(
				TEXT("CANVAS — %s did not load. Run Scripts/generate-menu-widgets.py to author it. ")
				TEXT("The game is drawing the original Canvas title screen and is fully playable."),
				TraceMenuHUDFile::TitleWidgetPath);
			UE_LOG(LogTraceGame, Warning, TEXT("[MenuUI] %s"), *MenuUmgStatus);
		}
		return false;
	}

	UTraceTitleMenuWidget* Created = CreateWidget<UTraceTitleMenuWidget>(PC, WidgetClass);
	if (Created == nullptr)
	{
		if (!bMenuUmgDecisionLogged)
		{
			bMenuUmgDecisionLogged = true;
			MenuUmgStatus = TEXT("CANVAS — CreateWidget failed for WBP_TitleMenu. Drawing Canvas instead.");
			UE_LOG(LogTraceGame, Error, TEXT("[MenuUI] %s"), *MenuUmgStatus);
		}
		return false;
	}

	// ALL-OR-NOTHING, and validating. A widget whose row count does not match ETraceMenuRow would
	// draw rows that nothing behind them can select — the same reasoning step 6 applies to the input
	// assets, and for the same reason: a silently half-adopted asset is worse than no asset.
	const int32 ExpectedRows = static_cast<int32>(ETraceMenuRow::Count);
	if (Created->GetRowCount() != ExpectedRows)
	{
		bMenuUmgDecisionLogged = true;
		MenuUmgStatus = FString::Printf(
			TEXT("CANVAS — WBP_TitleMenu offers %d row widget(s), C++ has %d (ETraceMenuRow::Count). ")
			TEXT("Re-run Scripts/generate-menu-widgets.py."),
			Created->GetRowCount(), ExpectedRows);
		UE_LOG(LogTraceGame, Error, TEXT("[MenuUI] %s"), *MenuUmgStatus);
		Created->MarkAsGarbage();
		return false;
	}

	// ZOrder 0 and HitTestInvisible: this widget is a picture. See the header of
	// UTraceTitleMenuWidget for why the clicks deliberately do not come through Slate.
	Created->SetVisibility(ESlateVisibility::HitTestInvisible);
	Created->AddToViewport(0);
	MenuWidget = Created;

	bMenuUmgDecisionLogged = true;
	bMenuUmgWantedLastDecision = true;
	MenuUmgStatus = FString::Printf(TEXT("UMG — %s adopted, %d rows."),
		TraceMenuHUDFile::TitleWidgetPath, ExpectedRows);
	UE_LOG(LogTraceGame, Display, TEXT("[MenuUI] %s"), *MenuUmgStatus);
	return true;
}

void ATraceMenuHUD::BuildMenuView(FTraceTitleMenuView& OutView) const
{
	// ---- WHAT THE WIDGET STOPS DRAWING WHILE A MODAL OWNS THE SCREEN -----------------------------
	//
	// KEPT, BUT NO LONGER REACHABLE ON THE MODAL FRAMES IT WAS WRITTEN FOR. Spec v23 §A2 built this
	// view while a modal was up, because the modal drew in front of Slate and the widget stayed put;
	// spec v25 §1 removed that elevation, so DrawHUD collapses the widget again for exactly those
	// frames and never calls this with bModalOwnsScreen true.
	//
	// It stays because it is the correct answer to the question either way — a frozen pointer and a
	// dimmable banner are stale on any frame a modal owns, whichever surface the modal is on — and
	// because deleting it would leave the next person to try a Slate options screen to rediscover it:
	//
	//   THE POINTER, because the widget's is FROZEN. DrawHUD deliberately stops sampling the mouse
	//   while a modal is open (a pointer resting over QUIT would otherwise re-select rows behind the
	//   panel), so LastCursorPos stops being true the instant the overlay opens, while the overlay
	//   draws the artist's arrow at the live position. This is the same "one pointer, one owner"
	//   argument DrawCursor makes for the Canvas path, arriving at the same answer for the widget.
	//
	//   THE FAILURE BANNER, because a Slate banner is the one element on this screen a modal's scrim
	//   could dim from in front. On the Canvas path the banner is drawn by DrawHUD; see the call site.
	const bool bModalOwnsScreen = OptionsMenu.IsOpen() || IsJoinPromptOpen();

	OutView.Now = Now;
	OutView.RowCount = FMath::Min(static_cast<int32>(ETraceMenuRow::Count), FTraceTitleMenuView::MaxRows);
	for (int32 Index = 0; Index < OutView.RowCount; ++Index)
	{
		const ETraceMenuRow Row = static_cast<ETraceMenuRow>(Index);
		BuildRowView(Row, Row == Selected, OutView.Rows[Index]);
	}

	OutView.Blurb = BuildBlurb();
	OutView.Tagline = TRACE_TEXT("MENU.TAGLINE", "5 V 5    -    ONE CORE    -    DASH THE TRAIL TO KILL THE CARRIER");
	OutView.AddressCaption = TRACE_TEXT("MENU.ADDRESS_CAPTION", "YOUR ADDRESS");
	OutView.AddressValue = TraceNet::GetHostEndpoint();

	// MEASURED CAVEAT, kept verbatim from the Canvas path. If something else already holds UDP 7777,
	// UIpNetDriver does not fail — it binds the next free port instead. The match is still joinable,
	// but the number in the chip is then a lie, and a host reciting it would send everybody to a port
	// nothing is listening on.
	OutView.PortWarning = TraceNet::IsDefaultPortFreeCached()
		? FString()
		: TRACE_TEXTF("NET.PORT_BUSY", "PORT {0} BUSY - USING ANOTHER PORT", { TraceNet::DefaultPort });

	// D30 — THE KEY LEGEND IS GONE, at the owner's request ("remove the text at the bottom: close
	// trace, w/s..."). It read
	//     W / S  OR  ARROWS   MOVE     A / D   CHANGE     ENTER   SELECT     ESC   QUIT
	// and it was the only line on this screen that told a player nothing they would not find by
	// pressing one key. The FIELD stays and is emptied rather than being deleted, for two reasons:
	// FooterKeysText is a required meta=(BindWidget) on UTraceTitleMenuWidget, so the block has to go
	// on existing in the asset whatever it says; and TraceText::Measure reports ONE line box for an
	// empty string (SplitLines yields a single empty line), so the footer stack keeps its height and
	// the hint below does not move up onto the blurb. The Canvas twin is ATraceMenuHUD::DrawFooter.
	//
	// D32-PADMENU FILLS IT AGAIN, BUT ONLY FOR A PAD, AND THAT IS NOT THE DELETED LINE COMING BACK.
	// What the owner asked to be rid of was a static keyboard legend telling every player, forever,
	// what W and S do. This appears only once a controller has actually been touched on this machine
	// (ShouldShowPadHints -> UTraceGamepadInputSubsystem::HasSeenGamepadInput), says only what a pad
	// cannot discover by pressing something, and vanishes again on a keyboard-only machine. The slot
	// it uses is the one the deletion deliberately left in the asset — see the paragraph above — so
	// no layout moves and the Canvas twin (DrawFooter) prints the same string in the same place.
	OutView.FooterKeys = ShouldShowPadHints()
		? FString(TRACE_TEXT("MENU.FOOTER_PAD_KEYS", "D-PAD   MOVE          A   SELECT          B   BACK"))
		: FString();

	// THE HINT LINE IS EMPTY, and stays in the view rather than being deleted. It read "PLAY ALSO
	// HOSTS - EVERY MATCH IS JOINABLE", which the PLAY blurb directly above it already says; the
	// co-developer's text pass removed it. Emptied, not deleted, for the same two reasons as
	// FooterKeys above: FooterHintText is a required BindWidget, and an empty string still measures
	// as one line box, so PlaceFooterBelowBlurb places the footer stack exactly where it was.
	OutView.FooterHint = FString();

	// WP8.2 — the version AND the network compatibility code, bottom-right. The widget draws it
	// because this Canvas cannot reach a UMG frame (AHUD's canvas composites under Slate); the Canvas
	// path draws the same string in DrawHUD.
	OutView.Version = TraceMenuHUDFile::ProjectVersionLabel();

	// ---- Failure banner ---------------------------------------------------------------------------
	{
		FString Headline;
		double AgeSeconds = 0.0;
		if (TraceNet::GetLastFailure(Headline, AgeSeconds) && !Headline.IsEmpty())
		{
			// A minute is a long time for a banner, and it is deliberate: the failure that matters
			// happens while the player is looking at a DIFFERENT screen.
			//
			// The headline only. The engine's error string is in the log and nowhere else (see
			// TraceNet::GetLastFailure); an emptied line (Ranen's "KEY =") shows no banner at all.
			constexpr double VisibleSeconds = 60.0;
			if (AgeSeconds <= VisibleSeconds && !bModalOwnsScreen)
			{
				OutView.bFailureVisible = true;
				OutView.FailureHeadline = Headline;
				OutView.FailureFade = static_cast<float>(FMath::Clamp((VisibleSeconds - AgeSeconds) / 6.0, 0.0, 1.0));
			}
		}
	}

	// ---- Travel overlay ---------------------------------------------------------------------------
	//
	// NOT REACHED ON A TRAVEL FRAME ANY MORE: DrawHUD stands the widget down for the whole of a travel
	// and draws the kit's black travel card on the Canvas (DrawTravelOverlay), so the connecting card
	// can carry the kit's ESC CANCEL legend and the pointer. Kept truthful anyway, from the same
	// state the card reads — the travel KIND, never the caption's wording.
	OutView.bTravelVisible = bTravelling;
	if (bTravelling)
	{
		OutView.TravelCaption = TravelCaption.IsEmpty() ? FString(TRACE_TEXT("MENU.TRAVEL_ENTERING_ARENA", "ENTERING THE ARENA")) : TravelCaption;
		OutView.TravelHint = IsJoinInFlight() ? TraceMenuHUDTravel::ElapsedText(this, TravelStartRealTime) : FString();
	}

	// ---- Cursor ------------------------------------------------------------------------------------
	//
	// The red arm is the one DrawCursor already owns: `Trace.Menu.CursorRedArm 1` puts the stale
	// second pointer back, on either renderer, so the guard can be shown to be load-bearing from one
	// binary rather than asserted.
	OutView.bCursorVisible = bHasCursor && !bTravelling
		&& (!bModalOwnsScreen || TraceMenuHUDFile::GCursorRedArm != 0);
	if (OutView.bCursorVisible && ViewW > 0.f && ViewH > 0.f)
	{
		// Fractions rather than pixels: the widget knows its own size and nothing here has to know
		// what the DPI scale did.
		OutView.CursorFraction = FVector2D(LastCursorPos.X / ViewW, LastCursorPos.Y / ViewH);
	}
}

void ATraceMenuHUD::PublishRowRectsFromWidget()
{
	if (MenuWidget == nullptr)
	{
		return;
	}

	const int32 RowCount = static_cast<int32>(ETraceMenuRow::Count);
	FBox2D Fetched[static_cast<int32>(ETraceMenuRow::Count)];
	for (int32 Index = 0; Index < RowCount; ++Index)
	{
		if (!MenuWidget->GetRowViewportRect(Index, Fetched[Index]))
		{
			// Not laid out yet. Leave RowRects and bRowRectsValid alone: the previous frame's rects
			// are still true, and on the very first frame there are none, which is the same state the
			// Canvas path is in before its first DrawMenuRows.
			return;
		}
	}

	for (int32 Index = 0; Index < RowCount; ++Index)
	{
		RowRects[Index] = Fetched[Index];
	}
	bRowRectsValid = true;
}

bool ATraceMenuHUD::GetUmgRowRect(int32 InRowIndex, FBox2D& OutRect) const
{
	return (MenuWidget != nullptr) && MenuWidget->GetRowViewportRect(InRowIndex, OutRect);
}

bool ATraceMenuHUD::GetCanvasRowRect(int32 InRowIndex, FBox2D& OutRect) const
{
	const int32 RowCount = static_cast<int32>(ETraceMenuRow::Count);
	if (InRowIndex < 0 || InRowIndex >= RowCount || ViewW <= 0.f || ViewH <= 0.f)
	{
		return false;
	}

	const TraceMenuStyle::FConsoleLayout Layout =
		TraceMenuStyle::ComputeConsoleLayout(ViewW, ViewH, UIScale, RowCount);
	const float RowH = TraceMenuStyle::RowHeight * UIScale;
	const float Y = Layout.FirstRowY + InRowIndex * (TraceMenuStyle::RowSpacing * UIScale);

	OutRect = FBox2D(FVector2D(Layout.RowX, Y), FVector2D(Layout.RowX + Layout.RowW, Y + RowH));
	return true;
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void ATraceMenuHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Explicit, rather than trusting the travel to tear it down. A widget added to the game
	// viewport's screen layer outlives the world that made it in some paths, and a title screen
	// still painted over the arena would be a very loud bug for a very quiet omission.
	if (MenuWidget != nullptr)
	{
		MenuWidget->RemoveFromParent();
		MenuWidget = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void ATraceMenuHUD::BeginPlay()
{
	Super::BeginPlay();

	// The difficulty this machine last picked, not the default: a HARD player coming back from a
	// match (or from a failed join) finds HARD. Applied straight away so the value on screen is never a
	// promise the match fails to keep; StartMatch() re-applies it and the URL carries it.
	Difficulty = TraceDifficulty::GetSavedSetting();
	TraceDifficulty::ApplyToSettings(Difficulty);

	// Bound here rather than in a module startup because both HUDs need it and neither owns the
	// other; the call is idempotent and the handlers outlive every world. Without it a failed join
	// is completely silent, which is the single thing that made the reported bug undiagnosable from
	// the player's chair.
	TraceNet::BindFailureHandlers();

	// The address the player typed last time, straight off disk. Empty on a fresh install, which is
	// exactly when the prompt falls back to showing an example instead.
	LastJoinAddress = TraceNet::LoadLastJoinAddress();

	// A JOIN THAT JUST FAILED BRINGS THE PLAYER BACK TO ITS OWN PROMPT. A failed join reloads this map
	// with a fresh HUD, which used to land on PLAY under a banner across the wordmark — so Enter, the
	// natural "try again", HOSTED a match instead. Now the prompt is open again with the address they
	// were dialling and the reason under the field: retrying is Enter, fixing it is typing. The reason
	// is shown there instead of the banner, not as well (the engine's code is still in the log).
	{
		FString FailedHeadline;
		if (TraceNet::ConsumeFailedJoin(FailedHeadline))
		{
			Selected = ETraceMenuRow::Join;
			OpenJoinPrompt();
			JoinError = FailedHeadline;
			JoinErrorText = JoinEntry.GetText();
			TraceNet::ClearFailure();
			UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] The last JOIN failed (%s; the engine's reason is the [Net] error above); the prompt is open again on '%s'."),
				*FailedHeadline, *JoinEntry.GetText());
		}
	}

	// The handmade kit's sprites, loaded now rather than inside the first frame of SETTINGS: a sprite
	// loaded mid-draw stalls that frame and draws its flat fallback until its render resource lands.
	// See TraceMenuKit::Prime.
	TraceMenuKit::Prime();

	UE_LOG(LogTraceGame, Log, TEXT("Title screen up. Difficulty %s."),
		*TraceDifficulty::ToDisplayName(Difficulty));

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

	// ---- FX/AUDIO PLAN §5.7 — THE TITLE LOOP ----------------------------------------------------
	//
	// UTraceMusicSubsystem is a GAME INSTANCE subsystem, so the component it owns survives the
	// menu-level -> arena travel and back; Play() is a no-op when that track is already playing.
	// Both facts together are why this is an UNCONDITIONAL call in BeginPlay rather than a
	// "have we started yet" flag: returning to the title screen from a match re-runs this line and
	// simply re-asserts what is already true, and a map restart cannot leave the menu silent.
	//
	// Null-tested rather than assumed: Get() refuses on a dedicated server, and it returns null
	// before a game instance exists at all.
	//
	// *** THE BED IS CURRENTLY DISABLED AND THIS CALL IS STILL HERE ON PURPOSE. *** The owner asked
	// for both music beds off "until further notice", so UTraceAudioSettings::bMusicBedsEnabled is
	// False in Config/DefaultGame.ini and Play() starts nothing (Audio/TraceMusicPlayer.h). Leaving
	// the unconditional call in place is what makes turning it back on a one-line config edit — and
	// the "no-op when already playing" contract above is what makes that safe in both states.
	if (UTraceMusicSubsystem* Music = UTraceMusicSubsystem::Get(this))
	{
		Music->Play(TraceSoundEvents::MusicTitle);
	}

#if !UE_BUILD_SHIPPING
	TraceAutoShot::Arm(this, TEXT("Menu"));
	TraceAutoShot::ArmDeferredExec(this, TEXT("Menu"));
	ArmAutoPlay();
	ArmAutoJoin();
	ArmAutoSettings();
	ArmClickTest();
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
	 *   Enter      -> toggles it back OFF             (Toggle, and the one-way-toggle red arm)
	 *   2 x Down   -> MOVE FORWARD                    (navigation ACROSS the CONTROLS header)
	 *   Enter      -> arms the rebind capture         (Binding)
	 *   K          -> becomes the new MOVE FORWARD    (capture)
	 *
	 * *** THIS SCRIPT IS POSITIONAL AND MUST BE RE-WALKED WHENEVER A ROW IS ADDED OR REMOVED. ***
	 * It navigates by key presses, not by row identity, so a row inserted or deleted anywhere the
	 * script walks PAST silently re-aims every step after it — the run still "passes" while
	 * adjusting something nobody asked about. It was re-walked when spec v14 §3 added the MATCH /
	 * CHARACTERS row above MOUSE, which is where the leading Down and the two Enters come from. The
	 * log line each step prints is the check: if a step's description stops matching what the
	 * screenshot shows selected, this list is stale.
	 *
	 * RE-WALKED FOR SPEC v15 §5, which DELETED the SWAP WEAPON row. Verdict at the time: NOT
	 * AFFECTED — SWAP WEAPON was the twelfth binding, six rows BELOW MOVE FORWARD, so deleting it
	 * moved nothing the script ever selects.
	 *
	 * *** RE-WALKED AGAIN FOR THE RELEASE UI PLAN (WP2 CALL SIGN + WP3 AUDIO), AND THIS TIME IT WAS
	 * AFFECTED — TWICE OVER. *** Two passes have inserted rows ABOVE everything this script walks:
	 * spec v29 §3 put a CROSSHAIR door under VIDEO SETTINGS (that one shipped WITHOUT this comment
	 * being re-walked, which is exactly the silent re-aim the warning above predicts — the first Down
	 * had been landing on CROSSHAIR and calling it CHARACTERS ever since), and WP2/WP3 have now added
	 * a PLAYER section above the whole page plus a SOUND door in the middle of it.
	 *
	 * FTraceOptionsMenu::RebuildRows lays the settings page out as (the options-page kit pass moved
	 * the twenty key binds to a KEYBOARD page of their own and dropped the one-row captions)
	 *
	 *    0 CALL SIGN  <- the selection starts here     1 note
	 *    2 VIDEO  3 AUDIO  4 CROSSHAIR  5 KEYBOARD  6 CONTROLLER  7 LOADOUTS
	 *    8 spacer  9 ABILITIES  10 note  11 header MOUSE
	 *   12 SENSITIVITY  13 VERTICAL SENSITIVITY  14 INVERT MOUSE Y  15 spacer  16 RESET  17 BACK
	 *
	 * MoveSelection skips headers and notes, so ABILITIES is seven Downs from CALL SIGN, and MOVE
	 * FORWARD is the first row of the KEYBOARD page, six Ups from INVERT MOUSE Y and an Enter.
	 *
	 * *** THE CALL SIGN ROW IS NOT ACTIVATED BY THIS SCRIPT, DELIBERATELY. *** Enter on it hands the
	 * keyboard to FTraceTextEntry for the rest of the run (FTraceOptionsMenu::TickCallSignEntry), so
	 * every subsequent injected key would be typed into a name instead of driving a row — the script
	 * would still "pass" while proving nothing. The call sign has its own headless driver,
	 * Trace.CallSign.Set, which goes through the same storage and the same ServerChangeName the row's
	 * submit path uses.
	 *
	 * ---- FIXED SINCE: the options-page kit pass made the first pointer sample on a page a RECORD, not
	 * a move (FTraceOptionsMenu::PollMouse), and with the counts below the verdict now reads PASS on a
	 * headless run (slider moved, toggle returned, K landed). The history is kept below because it is
	 * the reason the fix exists.
	 *
	 * ---- MEASURED, AND IT IS WHY THE RE-WALK ALONE DOES NOT MAKE THE DONE LINE PASS ---------------
	 *
	 * *** THIS SCRIPT'S END-STATE ASSERTIONS DO NOT HOLD ON A HEADLESS RUN, AND THE CAUSE IS NOT THE
	 * ROW COUNTS. *** Run headlessly after the re-walk above
	 * (Saved/Logs/release/W3-UISETTINGS-autosettings.log), every step's DESCRIPTION matched the page
	 * — the layout was confirmed independently, row by row, with Trace.Menu.Nudge <n> 0
	 * (W3-UISETTINGS-rowdump.log) — and the DONE line still reported sensitivity=1.00 (expected 1.20)
	 * and moveForward=ENTER (expected K), with the four RIGHT presses landing on VERTICAL SENSITIVITY
	 * and the rebind capture opening on slot 1 rather than slot 0.
	 *
	 * The signature says what is happening: the two steps aimed at CHARACTERS produced NO
	 * `[Settings] Saved` line at all, which no Slider or Toggle row can do — so the highlight was not
	 * where the injected keys had put it. The remaining mover of `Selected` is
	 * FTraceOptionsMenu::PollMouse, which runs AFTER PollNavigation and drags the selection onto
	 * whatever row the pointer is over whenever the pointer appears to have moved. Headlessly the OS
	 * cursor sits wherever it sits — in the archived settings capture it is resting on INVERT MOUSE Y
	 * — and a row that slides under a resting pointer as the page is laid out looks exactly like a
	 * pointer that moved. That is the same class of defect the bCursorMoved guard was added for, one
	 * step further out, and it is a property of driving a MOUSE-AND-KEYBOARD menu with the keyboard
	 * only on a machine with no hand on the mouse.
	 *
	 * SO: the counts below are correct against the page and are worth keeping correct; the DONE
	 * line's numbers are NOT a usable pass/fail signal headlessly and must not be "fixed" by
	 * trial-and-error nudging of the counts, which would leave the descriptions lying about the rows
	 * again. What this run IS good evidence for is the input PATH — it drove fifteen real
	 * MoveSelection focus changes through the real polling code (Trace.Audio.Heard: UIHover x15).
	 * A trustworthy end-state assertion needs the pointer parked off the panel first, which is a
	 * change to this harness rather than to the page it drives, and it is left for whoever next
	 * needs the assertion rather than done blind here.
	 */
	struct FAutoSettingsKey { FKey (*Key)(); const TCHAR* What; };

	const TArray<FAutoSettingsKey>& AutoSettingsScript()
	{
		static const TArray<FAutoSettingsKey> Script =
		{
			// *** RE-WALKED AGAIN (the options-page kit pass): THE KEY BINDS ARE THEIR OWN PAGE NOW. ***
			// SETTINGS is CALL SIGN, then six doors with no captions (VIDEO, AUDIO, CROSSHAIR, KEYBOARD,
			// CONTROLLER, LOADOUTS), then ABILITIES, then the MOUSE block — so reaching ABILITIES costs
			// SEVEN Downs, and MOVE FORWARD is reached through the KEYBOARD door: six Ups from INVERT
			// MOUSE Y, Enter, and the page opens on MOVE FORWARD (its first row). The page no longer
			// lets a resting pointer take the selection when it opens, so these counts hold headlessly.
			{ []{ return EKeys::Down;  }, TEXT("-> video (from call sign)") },
			{ []{ return EKeys::Down;  }, TEXT("-> audio") },
			{ []{ return EKeys::Down;  }, TEXT("-> crosshair") },
			{ []{ return EKeys::Down;  }, TEXT("-> keyboard") },
			{ []{ return EKeys::Down;  }, TEXT("-> controller") },
			{ []{ return EKeys::Down;  }, TEXT("-> loadouts") },
			{ []{ return EKeys::Down;  }, TEXT("-> abilities (spec v14 3)") },

			// LEFT/RIGHT rather than ENTER. This is now belt-and-braces: ActivateSelected on a
			// Toggle USED to route through AdjustSelected(+1), which clamps, so ENTER could turn a
			// toggle on but never off. The first version of this step hit exactly that and produced
			// a run with no write and no log while looking like it had exercised the row.
			//
			// Working around it here instead of fixing it is why the bug survived to be reported by
			// a player as "the button to uninvert the mouse didn't work". ActivateSelected now
			// flips, so ENTER would work here too; these stay LEFT/RIGHT because a step that
			// asserts a specific end state is clearer than one that asserts a transition.
			{ []{ return EKeys::Left;  }, TEXT("abilities -> OFF") },
			{ []{ return EKeys::Right; }, TEXT("abilities -> ON (put back)") },
			{ []{ return EKeys::Down;  }, TEXT("-> sensitivity") },
			{ []{ return EKeys::Right; }, TEXT("sensitivity +") },
			{ []{ return EKeys::Right; }, TEXT("sensitivity +") },
			{ []{ return EKeys::Right; }, TEXT("sensitivity +") },
			{ []{ return EKeys::Right; }, TEXT("sensitivity +") },
			{ []{ return EKeys::Down;  }, TEXT("-> vertical sensitivity") },
			{ []{ return EKeys::Down;  }, TEXT("-> invert mouse y") },
			{ []{ return EKeys::Enter; }, TEXT("toggle invert y ON") },

			// THE RED ARM for the one-way-toggle bug. Pressing the same button a second time must
			// put the row back. Before the ActivateSelected fix this clamped and the row stayed ON,
			// so the DONE line below printed invertY=1 — which is the failure, and is what a player
			// actually hit when they could not turn the inverted mouse off again.
			{ []{ return EKeys::Enter; }, TEXT("toggle invert y OFF again (must actually flip)") },
			{ []{ return EKeys::Up;    }, TEXT("-> vertical sensitivity") },
			{ []{ return EKeys::Up;    }, TEXT("-> sensitivity") },
			{ []{ return EKeys::Up;    }, TEXT("-> abilities (across the MOUSE caption)") },
			{ []{ return EKeys::Up;    }, TEXT("-> loadouts") },
			{ []{ return EKeys::Up;    }, TEXT("-> controller") },
			{ []{ return EKeys::Up;    }, TEXT("-> keyboard") },
			{ []{ return EKeys::Enter; }, TEXT("open KEYBOARD (lands on move forward)") },
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

	// "-TraceMenuScoringMode=b" USED TO BE PARSED HERE, so a headless run could drive the whole
	// menu -> mode B match -> results -> menu loop without a human pressing an arrow key. There is
	// one ruleset now; the switch it drove no longer exists and the argument is silently ignored.

	DelaySeconds = FMath::Max(0.01f, DelaySeconds);
	UE_LOG(LogTraceGame, Display, TEXT("[AutoPlay] Pressing PLAY in %.2fs at difficulty %s."),
		DelaySeconds, *TraceDifficulty::ToDisplayName(Difficulty));

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

void ATraceMenuHUD::SnapshotUserSettings()
{
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();
	SavedMouseSensitivity = Settings.MouseSensitivity;
	SavedMouseSensitivityYScale = Settings.MouseSensitivityYScale;
	bSavedInvertMouseY = Settings.bInvertMouseY;

	const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();
	SavedBindings.Reset(Table.Num());
	for (int32 Index = 0; Index < Table.Num(); ++Index)
	{
		SavedBindings.Add(Settings.GetKey(static_cast<ETraceInputAction>(Index)));
	}

	bAutoSettingsSnapshotTaken = true;
	UE_LOG(LogTraceGame, Display,
		TEXT("[AutoSettings] Snapshotted the real settings (invertY=%d, %d bindings); they are put "
		     "back when the script finishes."),
		bSavedInvertMouseY ? 1 : 0, SavedBindings.Num());
}

void ATraceMenuHUD::RestoreUserSettings()
{
	if (!bAutoSettingsSnapshotTaken)
	{
		return;
	}
	bAutoSettingsSnapshotTaken = false;

	UTraceUserSettings& Settings = UTraceUserSettings::Get();
	Settings.MouseSensitivity = SavedMouseSensitivity;
	Settings.MouseSensitivityYScale = SavedMouseSensitivityYScale;
	Settings.bInvertMouseY = bSavedInvertMouseY;

	for (int32 Index = 0; Index < SavedBindings.Num(); ++Index)
	{
		if (SavedBindings[Index].IsValid())
		{
			Settings.SetKey(static_cast<ETraceInputAction>(Index), SavedBindings[Index]);
		}
	}

	// Save() rather than leaving it in memory: the script's own writes were flushed to disk, so
	// only a flushed restore actually undoes them.
	Settings.Save();

	UE_LOG(LogTraceGame, Display,
		TEXT("[AutoSettings] RESTORED. invertY=%d moveForward=%s — the run left no trace in the "
		     "player's config."),
		Settings.bInvertMouseY ? 1 : 0,
		*UTraceUserSettings::DescribeKey(Settings.GetKey(ETraceInputAction::MoveForward)));
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
		// Before the first injected key, not after: the script's very first steps already write.
		SnapshotUserSettings();

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
		// ---- THE VERDICT, JUDGED HERE RATHER THAN BY A READER ------------------------------------
		//
		// *** THIS USED TO PRINT NUMBERS AND LEAVE THE COMPARING TO A HUMAN, AND THAT IS WHY IT
		// STOPPED WORKING WITHOUT ANYBODY NOTICING. *** The old comment said "if it does not read
		// sensitivity=1.20 invertY=0 moveForward=K, the settings path did not work" — and for a
		// whole demo it read 1.50 / 1 / W and every run was still green, because a check whose
		// result only exists in prose is a check nothing can fail. See the row count above for what
		// had actually broken.
		//
		// THREE CLAIMS, EACH ABOUT A DIFFERENT ROW KIND, so one broken row kind cannot hide behind
		// two working ones:
		//
		//   SLIDER  four RIGHTs must raise MOUSE SENSITIVITY above where the snapshot found it. It
		//           is compared against the SNAPSHOT and not against a literal, because the default
		//           is a number somebody is entitled to change.
		//   TOGGLE  two ENTERs on INVERT MOUSE Y must leave it exactly where it started. One press
		//           turns it on, the second must turn it off — this is the red arm for the one-way
		//           toggle a player reported as "the button to uninvert the mouse didn't work", and
		//           invertY != the snapshot means it is back.
		//   CAPTURE the rebind must actually land: MOVE FORWARD on K.
		const UTraceUserSettings& Settings = UTraceUserSettings::Get();

		const bool bSliderMoved = Settings.MouseSensitivity > SavedMouseSensitivity + UE_KINDA_SMALL_NUMBER;
		const bool bToggleRestored = (Settings.bInvertMouseY == bSavedInvertMouseY);
		const bool bRebindLanded = (Settings.GetKey(ETraceInputAction::MoveForward) == EKeys::K);
		const bool bPassed = bSliderMoved && bToggleRestored && bRebindLanded;

		UE_LOG(LogTraceGame, Display,
			TEXT("[AutoSettings] DONE. sensitivity=%.2f (was %.2f) yScale=%.2f invertY=%d (was %d) ")
			TEXT("moveForward=%s dash=%s"),
			Settings.MouseSensitivity, SavedMouseSensitivity, Settings.MouseSensitivityYScale,
			Settings.bInvertMouseY ? 1 : 0, bSavedInvertMouseY ? 1 : 0,
			*UTraceUserSettings::DescribeKey(Settings.GetKey(ETraceInputAction::MoveForward)),
			*UTraceUserSettings::DescribeKey(Settings.GetKey(ETraceInputAction::Dash)));

		if (bPassed)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[AutoSettings] VERDICT: THE KEYBOARD STILL DRIVES THE SETTINGS OVERLAY. slider "
					"moved=1, toggle returned to its start=1, rebind landed on K=1."));
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[AutoSettings] VERDICT: THE KEYBOARD PATH IS BROKEN. slider moved=%d, toggle "
					"returned to its start=%d, rebind landed on K=%d."),
				bSliderMoved ? 1 : 0, bToggleRestored ? 1 : 0, bRebindLanded ? 1 : 0);
		}

		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(AutoSettingsStepTimer);
		}

		// Strictly AFTER the DONE line above, which is the assertion: restoring first would erase
		// the very state the run exists to report.
		RestoreUserSettings();
	}
}

// =================================================================================================
// -TraceMenuClickTest — SPEC v15 §4. See ArmClickTest() in the header for the argument.
//
// The two channels a click arrives on are driven separately, because that is how the engine
// delivers them and because only one of them was ever in doubt:
//
//   POSITION  APlayerController::SetMouseLocation -> FViewport::SetMouse, which writes the viewport's
//             cached cursor position — the exact value ATraceMenuHUD::GetCursorPoint reads back out
//             of APlayerController::GetMousePosition. It also asks the platform to move the real
//             pointer, which is a visible side effect of a dev-only switch and is why this is not
//             armed by default.
//   BUTTON    APlayerController::InputKey, the same entry point ATraceMenuHUD's own -TraceAutoSettings
//             script uses for the keyboard, and the same one a physical mouse ends up at.
//
// Steps are 0.10 s apart so several frames — and therefore several DrawHUD passes — separate the
// cursor move from the press and the press from the release. That matters: the hover pass, the
// window-focus sample and the cursor-movement test all live in DrawHUD, and a harness that pressed
// and released inside one frame would be measuring something no player can produce.
// =================================================================================================

#if !UE_BUILD_SHIPPING
/**
 * Spec v15 §4's red arm. 1 restores the foreground-window guard MousePressed used to have, so the
 * "two clicks" bug can be reproduced on demand in a shipped-fix build and the fix re-measured
 * against it.
 *
 * File-scope and distinctively named rather than an anonymous-namespace static: UBT compiles this
 * module as a unity/jumbo build, where two files' anonymous namespaces become one.
 */
static int32 GTraceMenuFocusGuardRedArm = 0;

static FAutoConsoleVariableRef CVarTraceMenuFocusGuardRedArm(
	TEXT("Trace.Menu.FocusGuardRedArm"),
	GTraceMenuFocusGuardRedArm,
	TEXT("Dev only. Spec v15 s4 RED ARM. 1 puts back the guard that dropped any menu mouse-down ")
	TEXT("arriving while the game window was not foreground — the thing that made every menu row ")
	TEXT("take two clicks. Run -TraceMenuClickTest with it on and off to compare."),
	ECVF_Cheat);

/**
 * Put the pointer somewhere, from the console, so a headless run can photograph what the menu draws
 * at a chosen cursor position: `Trace.Menu.CursorAt 200 540`.
 *
 * Added by the spec v20 integration pass for one specific reason. The double-cursor defect that
 * ATraceMenuHUD::DrawCursor now guards against is only VISIBLE when the frozen pointer lands outside
 * the settings panel, and every existing harness that moves the mouse (-TraceMenuClickTest) moves it
 * to the horizontal centre of the screen, which the panel always covers. Without this there was no
 * way to photograph either arm, and "I reasoned about it" is not what spec v20 §4 asks for.
 *
 * Goes through APlayerController::SetMouseLocation, the same call -TraceMenuClickTest uses, so the
 * menu's own sampling path sees an ordinary mouse move and nothing is faked past it.
 */
static FAutoConsoleCommandWithWorldAndArgs CmdMenuCursorAt(
	TEXT("Trace.Menu.CursorAt"),
	TEXT("Dev only. Trace.Menu.CursorAt <X> <Y> - move the pointer to viewport pixel (X, Y). Exists so ")
	TEXT("a capture can choose where the cursor is. See Trace.Menu.CursorRedArm."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() < 2 || World == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Menu] Trace.Menu.CursorAt needs an X and a Y."));
				return;
			}
			APlayerController* PC = World->GetFirstPlayerController();
			if (PC == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Menu] Trace.Menu.CursorAt: no player controller yet."));
				return;
			}
			const int32 X = FCString::Atoi(*Args[0]);
			const int32 Y = FCString::Atoi(*Args[1]);
			PC->SetMouseLocation(X, Y);
			UE_LOG(LogTraceGame, Display, TEXT("[Menu] Trace.Menu.CursorAt: pointer -> (%d, %d)."), X, Y);
		}));

/**
 * D32-PADMENU — Trace.Menu.Report.
 *
 * The title screen's state in one line, so a synthetic pad press can be shown to have moved exactly
 * what it claims to have moved. `Trace.Pad.Menu down down` followed by this is a measurement;
 * a screenshot of the same thing is a picture of a highlight nobody can count.
 *
 * READS ONLY. It is the only command this tranche adds that touches the title screen, and it cannot
 * change it — which is what lets it be run before AND after a press without the "after" reading
 * being something the report itself caused.
 */
static FAutoConsoleCommandWithWorldAndArgs CmdMenuReport(
	TEXT("Trace.Menu.Report"),
	TEXT("D32-PADMENU, dev only. Prints the highlighted title row, the difficulty, which modal is up "
		"and whether a controller has been seen. Reads only."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			APlayerController* const PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
			const ATraceMenuHUD* const MenuHUD = (PC != nullptr) ? Cast<ATraceMenuHUD>(PC->GetHUD()) : nullptr;
			if (MenuHUD == nullptr)
			{
				// NOT silence, and not an error either: run inside a match this is simply the wrong
				// screen, and saying so is what stops the next reader assuming the command is broken.
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Menu.Report] No title-screen HUD here — this command only means anything on "
						"the menu map."));
				return;
			}
			MenuHUD->LogMenuState((Args.Num() > 0) ? *Args[0] : TEXT("report"));
		}));
#endif

// NAMED, not anonymous: UBT compiles this module as a unity/jumbo build, so two files that each
// define something at the top of an anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
namespace TraceMenuClickTest
{
	/** Complete down/up pairs a single row is given before the phase is declared dead. */
	constexpr int32 MaxPairs = 4;

	/** Label of the overlay row phase 2 clicks. A toggle, so its effect is readable in one bool. */
	const TCHAR* const OverlayRow = TEXT("INVERT MOUSE Y");
}

void ATraceMenuHUD::ArmClickTest()
{
	float DelaySeconds = 0.f;
	if (!FParse::Value(FCommandLine::Get(), TEXT("TraceMenuClickTest="), DelaySeconds))
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// -TraceMenuFocusGuardRedArm as well as the cvar, because the cvar cannot be set reliably from a
	// launch: -ExecCmds splits on whitespace and "Trace.Menu.FocusGuardRedArm 1" arrives as two
	// arguments with the value on the floor. A bare switch has no such trap.
	if (FParse::Param(FCommandLine::Get(), TEXT("TraceMenuFocusGuardRedArm")))
	{
		GTraceMenuFocusGuardRedArm = 1;
		UE_LOG(LogTraceGame, Display,
			TEXT("[ClickTest] RED ARM ON: the foreground-window guard spec v15 §4 removed is back for "
			     "this run, and every row is expected to need more than one click."));
	}

	// Well clear of TraceMenuStyle::ActivationGraceSeconds and of the 0.75 s cursor-settling window,
	// so neither of those can be what the measurement reports. Both are legitimate and both are
	// meant to have expired long before a human has read the menu, let alone clicked it.
	DelaySeconds = FMath::Max(1.50f, DelaySeconds);
	UE_LOG(LogTraceGame, Display,
		TEXT("[ClickTest] Counting clicks-per-activation in %.2fs. One pair per row is the requirement."),
		DelaySeconds);

	World->GetTimerManager().SetTimer(ClickTestArmTimer,
		FTimerDelegate::CreateWeakLambda(this, [this]() { BeginClickTest(); }), DelaySeconds, false);
}

void ATraceMenuHUD::BeginClickTest()
{
	// Phase 2 writes INVERT MOUSE Y through the shipping save path. Same snapshot the
	// -TraceAutoSettings script takes, and for the same reason: a verification run that leaves the
	// developer's own mouse inverted gets reported as a game bug. See SnapshotUserSettings.
	SnapshotUserSettings();

	ClickTestBaselineDifficulty = Difficulty;
	bClickTestBaselineInvertY = UTraceUserSettings::Get().bInvertMouseY;

	ClickTestPhase = 0;
	ClickTestSubStep = 0;
	ClickTestDeadPairs = 0;
	ClickTestPairsUsed[0] = ClickTestPairsUsed[1] = ClickTestPairsUsed[2] = 0;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(ClickTestStepTimer,
			FTimerDelegate::CreateWeakLambda(this, [this]() { ClickTestStep(); }), 0.10f, true);
	}
}

bool ATraceMenuHUD::ClickTestRowCenter(int32 Phase, FVector2D& OutPoint)
{
	if (Phase == 0 || Phase == 1)
	{
		// DIFFICULTY, never PLAY: activating PLAY would travel and end the run mid-measurement.
		const int32 Row = static_cast<int32>(Phase == 0 ? ETraceMenuRow::Difficulty : ETraceMenuRow::Settings);
		if (!bRowRectsValid || !RowRects[Row].bIsValid)
		{
			return false;
		}
		OutPoint = RowRects[Row].GetCenter();
		return true;
	}

	FBox2D Rect(ForceInit);
	if (!OptionsMenu.DebugGetRowRect(TraceMenuClickTest::OverlayRow, Rect))
	{
		return false;
	}
	OutPoint = Rect.GetCenter();
	return true;
}

void ATraceMenuHUD::ClickTestStep()
{
	APlayerController* PC = GetOwningPlayerController();
	UWorld* World = GetWorld();
	if (PC == nullptr || World == nullptr)
	{
		return;
	}

	// ---- Phase 3: report, put everything back, stop ----------------------------------------------
	if (ClickTestPhase >= 3)
	{
		World->GetTimerManager().ClearTimer(ClickTestStepTimer);

		OptionsMenu.Close();
		SetDifficulty(ClickTestBaselineDifficulty);   // saved too: phase 0's click saved the cycled value

		const bool bPass = (ClickTestPairsUsed[0] == 1) && (ClickTestPairsUsed[1] == 1) && (ClickTestPairsUsed[2] == 1);

		// Two calls rather than a ternary verbosity: UE_LOG's verbosity is a token the macro pastes
		// into a compile-time category check, not a value, so it cannot be an expression.
#define TRACE_CLICKTEST_ARGS \
	bPass ? TEXT("ONE PRESS = ONE ACTION") : TEXT("A MENU ROW NEEDS MORE THAN ONE CLICK"), \
	ClickTestPairsUsed[0], ClickTestPairsUsed[1], ClickTestPairsUsed[2]

#define TRACE_CLICKTEST_TEXT \
	TEXT("[ClickTest] VERDICT: %s. Clicks needed — title row=%d, SETTINGS row=%d, overlay row=%d ") \
	TEXT("(1 each is the requirement; 0 means the row never acted at all).")

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TRACE_CLICKTEST_TEXT, TRACE_CLICKTEST_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_CLICKTEST_TEXT, TRACE_CLICKTEST_ARGS);
		}

#undef TRACE_CLICKTEST_ARGS
#undef TRACE_CLICKTEST_TEXT

		// Strictly after the verdict, exactly as -TraceAutoSettings does it: restoring first would
		// erase the state the run exists to report.
		RestoreUserSettings();
		return;
	}

	// ---- Sub-step 0: park the cursor on the row --------------------------------------------------
	if (ClickTestSubStep == 0)
	{
		// Phase 2 needs the overlay up. If phase 1 could not open it with a click, open it here and
		// say so — the overlay row is still worth measuring, and silently skipping it would turn one
		// failure into two unanswered questions.
		if (ClickTestPhase == 2 && !OptionsMenu.IsOpen())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[ClickTest] The overlay is not open, so phase 1's click never landed. Opening it "
				     "directly so the overlay row can still be measured."));
			Selected = ETraceMenuRow::Settings;
			OpenOptions();
			return;   // next tick: the overlay draws, and its row rects become real
		}

		FVector2D Point = FVector2D::ZeroVector;
		if (!ClickTestRowCenter(ClickTestPhase, Point))
		{
			// Nothing has been drawn yet. Wait rather than click into the dark.
			return;
		}

		PC->SetMouseLocation(FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y));

		float ReadX = 0.f;
		float ReadY = 0.f;
		const bool bReadBack = PC->GetMousePosition(ReadX, ReadY);
		UE_LOG(LogTraceGame, Display,
			TEXT("[ClickTest] phase %d: cursor -> (%.0f, %.0f); the menu reads it back as (%.0f, %.0f) valid=%d."),
			ClickTestPhase, Point.X, Point.Y, ReadX, ReadY, bReadBack ? 1 : 0);

		if (!bReadBack)
		{
			// The one failure mode that would make every number below a lie. Say it, do not measure it.
			UE_LOG(LogTraceGame, Error,
				TEXT("[ClickTest] The viewport will not report a cursor position in this run, so no click "
				     "can be aimed. This is a harness failure, not a menu failure."));
			ClickTestPhase = 3;
			return;
		}

		ClickTestSubStep = 1;
		return;
	}

	// ---- Sub-step 1: press ------------------------------------------------------------------------
	if (ClickTestSubStep == 1)
	{
		// The two states that can silently eat a press are named on every attempt. Without this the
		// verdict says "two clicks" and leaves the next person to guess WHICH guard did it — which is
		// the position spec v15 §4 explicitly refuses to start from.
		UE_LOG(LogTraceGame, Display,
			TEXT("[ClickTest] phase %d, click %d: pressing. windowFocusedLastFrame=%d cursorHasMoved=%d"),
			ClickTestPhase, ClickTestDeadPairs + 1,
			bWindowFocusedLastFrame ? 1 : 0, bCursorHasMoved ? 1 : 0);

		InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/true);
		ClickTestSubStep = 2;
		return;
	}

	// ---- Sub-step 2: release --------------------------------------------------------------------
	if (ClickTestSubStep == 2)
	{
		InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/false);
		ClickTestSubStep = 3;
		return;
	}

	// ---- Sub-step 3: JUDGE, a whole step after the release ---------------------------------------
	//
	// SEPARATE FROM THE RELEASE, and the first version of this harness got it wrong in exactly the
	// way this project keeps getting caught by. APlayerController::InputKey does not run the bound
	// delegate; it queues the event for the next ProcessInputStack. Judging on the same call as the
	// release therefore asked "did that click work?" BEFORE the click had been delivered, so every
	// row appeared to need one more click than it does — the harness reported 2/2/3 on a build whose
	// title screen is single-click, which is the reported bug manufactured out of nothing. The log
	// gave it away: "[ClickTest] phase 0 ACTED" was printed BEFORE the "[MenuInput] LMB up" it was
	// supposedly judging.
	//
	// The overlay needs the extra step twice over: FTraceOptionsMenu polls IsInputKeyDown from
	// DrawHUD, so the release has to reach the input stack AND then be seen by a later draw.
	bool bActed = false;
	switch (ClickTestPhase)
	{
	case 0:  bActed = (Difficulty != ClickTestBaselineDifficulty); break;
	case 1:  bActed = OptionsMenu.IsOpen(); break;
	default: bActed = (UTraceUserSettings::Get().bInvertMouseY != bClickTestBaselineInvertY); break;
	}

	++ClickTestDeadPairs;

	if (bActed)
	{
		ClickTestPairsUsed[ClickTestPhase] = ClickTestDeadPairs;
		UE_LOG(LogTraceGame, Display, TEXT("[ClickTest] phase %d ACTED after %d click(s)."),
			ClickTestPhase, ClickTestDeadPairs);
	}
	else if (ClickTestDeadPairs >= TraceMenuClickTest::MaxPairs)
	{
		ClickTestPairsUsed[ClickTestPhase] = 0;
		UE_LOG(LogTraceGame, Error, TEXT("[ClickTest] phase %d did NOTHING after %d complete clicks."),
			ClickTestPhase, ClickTestDeadPairs);
	}
	else
	{
		// Same row, same cursor, another complete click. Back to the press, not to the cursor move:
		// re-parking the pointer every attempt would hide a bug that only bites the FIRST click.
		ClickTestSubStep = 1;
		return;
	}

	++ClickTestPhase;
	ClickTestSubStep = 0;
	ClickTestDeadPairs = 0;
}

// =================================================================================================
// Trace.Menu.JoinVerify — the JOIN prompt and the connecting card, one drawn frame at a time
// =================================================================================================
//
// THE STUCK STATE IT GUARDS. After CONNECT the card said "CONNECTING TO x" for the whole handshake
// (20 s to a dead address) with no way out, and CancelPressed had no travel guard: the first Escape
// moved the hidden highlight to QUIT and the second closed the game. Every step below goes through
// the entry point a player reaches — a real Escape key edge through APlayerController::InputKey (the
// same path -TraceAutoSettings drives), a real mouse click on the drawn legend and buttons — and
// checks the ENGINE's state, not just this HUD's: the pending net game must be gone and the queued
// travel URL empty, or the "cancelled" join would still connect behind the prompt.
//
// It dials 10.255.255.1 (unroutable, so nothing is ever joined) and puts the player's remembered JOIN
// address back at the end. Run on the title map:
//     -TraceExecOn=Menu -TraceExecAt=4 -TraceExec="Trace.Menu.JoinVerify"
// Against the pre-fix CancelPressed, step 4 fails ("still travelling, highlight on QUIT") and the
// harness stops there rather than pressing Escape a second time, which would have quit the game.

namespace TraceMenuJoinVerify
{
	static const TCHAR* const DeadAddress = TEXT("10.255.255.1:7777");
}

static FAutoConsoleCommandWithWorldAndArgs CmdMenuJoinVerify(
	TEXT("Trace.Menu.JoinVerify"),
	TEXT("Dev only. Drives the title screen's JOIN prompt and connecting card (Escape cancels a join, never ")
	TEXT("quits; the pending connection is really dropped; CONNECT / BACK / CANCEL take a click) and prints ")
	TEXT("a VERDICT. Run on the title map."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& /*Args*/, UWorld* World)
		{
			APlayerController* const PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
			ATraceMenuHUD* const MenuHUD = (PC != nullptr) ? Cast<ATraceMenuHUD>(PC->GetHUD()) : nullptr;
			if (MenuHUD == nullptr)
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[JoinVerify] No title-screen HUD here — run this on the menu map."));
				return;
			}
			MenuHUD->BeginJoinVerify();
		}));

/**
 * `Trace.Menu.JoinOnce <address>` — JOIN through the real prompt ONCE PER PROCESS, for a headless
 * capture of what a failed join comes back to. -TraceExec re-arms on every title screen, and a failed
 * join reloads the title, so a plain command would join again on the reloaded screen and hide the
 * very prompt the capture is for. Dev only.
 */
static FAutoConsoleCommandWithWorldAndArgs CmdMenuJoinOnce(
	TEXT("Trace.Menu.JoinOnce"),
	TEXT("Dev only. Trace.Menu.JoinOnce <address> - open the title's JOIN prompt on <address> and CONNECT, ")
	TEXT("once per process (a failed join reloads the title; the reloaded title does not join again)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			static bool bJoinedOnce = false;
			APlayerController* const PC = (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
			ATraceMenuHUD* const MenuHUD = (PC != nullptr) ? Cast<ATraceMenuHUD>(PC->GetHUD()) : nullptr;
			if (MenuHUD == nullptr || Args.Num() < 1 || bJoinedOnce)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[Menu] Trace.Menu.JoinOnce: %s."),
					bJoinedOnce ? TEXT("already joined once this process") : TEXT("needs the title map and an address"));
				return;
			}
			bJoinedOnce = true;
			MenuHUD->DebugJoin(Args[0]);
		}));

void ATraceMenuHUD::DebugJoin(const FString& Address)
{
	Selected = ETraceMenuRow::Join;
	OpenJoinPrompt();
	JoinEntry.SetText(Address);
	ConfirmJoin();
}

void ATraceMenuHUD::BeginJoinVerify()
{
	if (JoinVerifyStep != 0)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[JoinVerify] Already running."));
		return;
	}
	if (bTravelling || OptionsMenu.IsOpen())
	{
		UE_LOG(LogTraceGame, Error, TEXT("[JoinVerify] VERDICT: INCONCLUSIVE — the title is travelling or SETTINGS is open."));
		return;
	}

	JoinVerifySavedAddress = LastJoinAddress;
	JoinVerifyFailures = 0;
	JoinVerifyStep = 1;
	JoinVerifyStepTime = (GetWorld() != nullptr) ? GetWorld()->GetRealTimeSeconds() : 0.f;
	UE_LOG(LogTraceGame, Display, TEXT("[JoinVerify] ===== JOIN prompt and connecting card (dialling %s) ====="),
		TraceMenuJoinVerify::DeadAddress);
}

void ATraceMenuHUD::TickJoinVerify()
{
	if (JoinVerifyStep == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	APlayerController* PC = GetOwningPlayerController();
	FWorldContext* Context = (GEngine != nullptr && World != nullptr) ? GEngine->GetWorldContextFromWorld(World) : nullptr;
	if (World == nullptr || PC == nullptr || Context == nullptr)
	{
		return;
	}

	const float RealNow = World->GetRealTimeSeconds();
	const float StepAge = RealNow - JoinVerifyStepTime;
	const auto Advance = [this, RealNow]()
	{
		++JoinVerifyStep;
		JoinVerifyStepTime = RealNow;
	};
	const auto Check = [this](const TCHAR* What, bool bPass, const FString& Detail)
	{
		JoinVerifyFailures += bPass ? 0 : 1;
		UE_LOG(LogTraceGame, Display, TEXT("[JoinVerify]   %-4s %-62s %s"), bPass ? TEXT("ok") : TEXT("FAIL"), What, *Detail);
	};
	const auto Pending = [Context]() { return Context->PendingNetGame != nullptr; };
	const auto Queued = [Context]() { return !Context->TravelURL.IsEmpty(); };
	const auto DescribeState = [this, &Pending, &Queued]()
	{
		return FString::Printf(TEXT("travelling=%d kind=%d prompt=%d row=%d pendingNetGame=%d queuedURL=%d field='%s'"),
			bTravelling ? 1 : 0, static_cast<int32>(TravelKind), IsJoinPromptOpen() ? 1 : 0, static_cast<int32>(Selected),
			Pending() ? 1 : 0, Queued() ? 1 : 0, *JoinEntry.GetText());
	};
	const auto ClickAt = [PC](const FBox2D& Rect)
	{
		const FVector2D Center = Rect.GetCenter();
		PC->SetMouseLocation(FMath::RoundToInt(Center.X), FMath::RoundToInt(Center.Y));
	};

	switch (JoinVerifyStep)
	{
	// ---- 1. open the prompt on the dead address, and CONNECT ---------------------------------------
	case 1:
		Selected = ETraceMenuRow::Join;
		OpenJoinPrompt();
		JoinEntry.SetText(TraceMenuJoinVerify::DeadAddress);
		Advance();
		break;

	case 2:
		ConfirmJoin();
		Check(TEXT("CONNECT: the join is in flight and the connect is queued"),
			IsJoinInFlight() && Queued() && !IsJoinPromptOpen(), DescribeState());
		Advance();
		break;

	// ---- 2. a second later the engine is really dialling; the card offers the way out --------------
	case 3:
		if (StepAge < 1.0f)
		{
			break;
		}
		Check(TEXT("the engine is dialling (a pending net game exists)"), Pending(), DescribeState());
		Check(TEXT("the connecting card draws its CANCEL legend"), TravelCancelRect.bIsValid,
			TravelCancelRect.bIsValid ? TravelCancelRect.ToString() : FString(TEXT("no legend")));
		InjectKey(PC, EKeys::Escape, /*bPressed=*/true);
		Advance();
		break;

	case 4:
		InjectKey(PC, EKeys::Escape, /*bPressed=*/false);
		Advance();
		break;

	// ---- 3. ESCAPE CANCELLED IT: engine state gone, prompt back, highlight NOT on QUIT ---------------
	case 5:
		if (StepAge < 0.3f)
		{
			break;
		}
		{
			const bool bCancelled = !bTravelling && TravelKind == ETraceMenuTravel::None && !Pending() && !Queued();
			const bool bPromptBack = IsJoinPromptOpen() && JoinEntry.GetText() == TraceMenuJoinVerify::DeadAddress;
			const bool bNotQuit = Selected == ETraceMenuRow::Join;
			Check(TEXT("ESCAPE cancels the join (no pending game, nothing queued)"), bCancelled, DescribeState());
			Check(TEXT("...the prompt is back, holding the address that was dialled"), bPromptBack, DescribeState());
			Check(TEXT("...and the highlight is on JOIN, never walked to QUIT"), bNotQuit, DescribeState());
			if (!(bCancelled && bNotQuit))
			{
				// The pre-fix behaviour. Stop HERE: a second Escape on that build quits the game.
				JoinVerifyStep = 100;
				break;
			}
		}
		Advance();
		break;

	// ---- 4. the same-frame race: confirm and cancel before the engine ticks the travel -------------
	case 6:
		ConfirmJoin();
		CancelPressed();
		Check(TEXT("confirm + cancel in ONE frame: nothing left queued"), !bTravelling && !Queued(), DescribeState());
		Advance();
		break;

	case 7:
		if (StepAge < 1.0f)
		{
			break;
		}
		Check(TEXT("...and a second later no connection was ever started"), !Pending() && !bTravelling && IsJoinPromptOpen(),
			DescribeState());
		Advance();
		break;

	// ---- 5. the mouse: CONNECT, then a click on the card's CANCEL legend ----------------------------
	case 8:
		if (!JoinButtonRects[TraceMenuHUDJoin::Connect].bIsValid)
		{
			Check(TEXT("the prompt draws a CONNECT button"), false, TEXT("no rect"));
			JoinVerifyStep = 100;
			break;
		}
		ClickAt(JoinButtonRects[TraceMenuHUDJoin::Connect]);
		Advance();
		break;

	case 9:
		if (StepAge < 0.2f)
		{
			break;
		}
		InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/true);
		Advance();
		break;

	case 10:
		InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/false);
		Advance();
		break;

	case 11:
		if (StepAge < 1.0f)
		{
			break;
		}
		Check(TEXT("a click on CONNECT dials"), IsJoinInFlight() && (Pending() || Queued()), DescribeState());
		if (!TravelCancelRect.bIsValid)
		{
			Check(TEXT("the card's CANCEL legend is there to click"), false, TEXT("no rect"));
			JoinVerifyStep = 100;
			break;
		}
		ClickAt(TravelCancelRect);
		Advance();
		break;

	case 12:
		if (StepAge < 0.2f)
		{
			break;
		}
		InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/true);
		Advance();
		break;

	case 13:
		InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/false);
		Advance();
		break;

	case 14:
		if (StepAge < 0.3f)
		{
			break;
		}
		Check(TEXT("a click on CANCEL cancels the join"), !bTravelling && !Pending() && !Queued() && IsJoinPromptOpen(),
			DescribeState());
		if (JoinButtonRects[TraceMenuHUDJoin::Back].bIsValid)
		{
			ClickAt(JoinButtonRects[TraceMenuHUDJoin::Back]);
		}
		Advance();
		break;

	// ---- 6. BACK closes the prompt, onto JOIN ------------------------------------------------------
	case 15:
		if (StepAge < 0.2f)
		{
			break;
		}
		InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/true);
		Advance();
		break;

	case 16:
		InjectKey(PC, EKeys::LeftMouseButton, /*bPressed=*/false);
		Advance();
		break;

	case 17:
		if (StepAge < 0.3f)
		{
			break;
		}
		Check(TEXT("a click on BACK closes the prompt, highlight on JOIN"),
			!IsJoinPromptOpen() && !bTravelling && Selected == ETraceMenuRow::Join, DescribeState());
		JoinVerifyStep = 100;
		break;

	default:
		break;
	}

	if (JoinVerifyStep < 100)
	{
		return;
	}

	// ---- Put everything back, then the verdict ----------------------------------------------------
	if (bTravelling)
	{
		CancelJoin();
	}
	if (IsJoinPromptOpen())
	{
		CloseJoinPrompt(TEXT("JoinVerify done"));
	}
	LastJoinAddress = JoinVerifySavedAddress;
	TraceNet::SaveLastJoinAddress(JoinVerifySavedAddress);
	TraceNet::ForgetJoinAttempt();
	TraceNet::ClearFailure();
	JoinVerifyStep = 0;

	if (JoinVerifyFailures == 0)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[JoinVerify] VERDICT: PASS — Escape and CANCEL call a join off (the engine's pending connection is dropped), the prompt comes back, and nothing walks to QUIT."));
	}
	else
	{
		UE_LOG(LogTraceGame, Error, TEXT("[JoinVerify] VERDICT: FAIL — %d check(s) failed."), JoinVerifyFailures);
	}
}
#endif

// =================================================================================================
// Input entry points
// =================================================================================================

// =================================================================================================
// D32-PADMENU — the controller on the title screen
//
// See the block above the members in TraceMenuHUD.h for WHY this is polled while the keyboard next
// to it stays bound, and TracePadMenu in Settings/TraceGamepadInput.h for the button table.
//
// EVERY PATH BELOW ENDS IN A FUNCTION THE KEYBOARD ALREADY CALLS. MoveSelection, AdjustSelection,
// ActivateSelection and CancelPressed are unchanged, and so are their guards — the travel latch, the
// activation grace window, the "an overlay is open" early-outs. That is what makes this additive
// rather than a second, parallel menu that has to be kept in step with the first.
// =================================================================================================

#if !UE_BUILD_SHIPPING
void ATraceMenuHUD::LogMenuState(const TCHAR* Why) const
{
	// The label comes from BuildRowView rather than from a second switch, so a row renamed in one
	// place cannot be reported under its old name here.
	FTraceMenuRowView RowView;
	BuildRowView(Selected, true, RowView);

	UE_LOG(LogTraceGame, Display,
		TEXT("[Menu.Report] %s | row=%s (%d/%d) difficulty=%s | options=%d join=%d travelling=%d | ")
		TEXT("renderer=%s | pad: enabled=%d seenOnThisMachine=%d hints=%d | joinText='%s'"),
		Why, *RowView.Label, static_cast<int32>(Selected) + 1, static_cast<int32>(ETraceMenuRow::Count),
		*TraceDifficulty::ToDisplayName(Difficulty),
		OptionsMenu.IsOpen() ? 1 : 0, IsJoinPromptOpen() ? 1 : 0, bTravelling ? 1 : 0,
		bMenuUmgActive ? TEXT("UMG") : TEXT("Canvas"),
		TracePadMenu::IsEnabled() ? 1 : 0,
		TracePadMenu::HasSeenPad(this) ? 1 : 0,
		ShouldShowPadHints() ? 1 : 0,
		*JoinEntry.GetText());
}
#endif

bool ATraceMenuHUD::ShouldShowPadHints() const
{
	return TracePadMenu::HasSeenPad(this);
}

void ATraceMenuHUD::PollPadInput()
{
	APlayerController* const PC = GetOwningPlayerController();
	if (PC == nullptr)
	{
		return;
	}

	// ---- SAMPLE THE THREE BUTTONS EVERY FRAME, INCLUDING THE FRAMES NOTHING HERE ACTS ON THEM ----
	//
	// These are REMEMBERED edges (TracePadMenu::RisingEdge), so the memory has to be kept alive even
	// while this screen is not listening — otherwise the frame the settings overlay closes is a frame
	// where a still-held B looks like a brand new press and jumps the title highlight to QUIT. That
	// was not hypothetical: it is exactly the shape of the bug the double-A on frame 94 turned out to
	// be, seen from the other side.
	//
	// Sampled ONCE and passed down, never re-read: reading an edge consumes it.
	const bool bConfirm = TracePadMenu::RisingEdge(PC, TracePadMenu::ConfirmKey(), bPadConfirmWasDown);
	const bool bBack    = TracePadMenu::RisingEdge(PC, TracePadMenu::BackKey(),    bPadBackWasDown);
	const bool bAlt     = TracePadMenu::RisingEdge(PC, TracePadMenu::AltKey(),     bPadAltWasDown);

	// THE SETTINGS OVERLAY OWNS THE PAD WHILE IT IS UP. FTraceOptionsMenu::PollNavigation has read
	// the D-pad, the left stick and A/B/Y since D31-PAD; a second reader here would walk the title
	// selection underneath the panel, so that closing the overlay would drop the player on a row they
	// never chose. Exactly the reason DrawHUD stops sampling the mouse for the same frames.
	//
	// The repeat clocks are reset rather than left alone: a direction still held when the overlay
	// opened must not be read as "newly pressed" on the frame it closes.
	if (OptionsMenu.IsOpen())
	{
		PadNavX = 0;
		PadNavY = 0;
		return;
	}

	if (IsJoinPromptOpen())
	{
		PadNavX = 0;
		PadNavY = 0;
		PollPadJoinPrompt(bConfirm, bBack, bAlt);
		return;
	}

	PollPadTitle(PC, bConfirm, bBack);
}

void ATraceMenuHUD::PollPadTitle(APlayerController* PC, bool bConfirm, bool bBack)
{
	// ---- Move ------------------------------------------------------------------------------------
	//
	// The clock is TracePadMenu's, shared with the other pad-driven screens, because a menu that
	// scrolls at one speed here and another on the character select is a menu that feels unfinished.
	// The KEYBOARD is not on this clock and does not need to be: its repeat is the OS's, which is the
	// player's own system-wide setting and has always driven this screen.
	if (TracePadMenu::StepRepeat(TracePadMenu::NavY(PC), PadNavY, PadNextNavTime, Now))
	{
		const ETraceMenuRow Before = Selected;
		MoveSelection(PadNavY);

		// Logged only when the row actually changed. MoveSelection clamps, so a held direction at the
		// end of the list calls it every repeat while nothing moves, and a line per repeat would bury
		// the presses that did something.
		if (Selected != Before)
		{
			FTraceMenuRowView RowView;
			BuildRowView(Selected, true, RowView);
			UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] Pad %s -> %s."),
				(PadNavY > 0) ? TEXT("DOWN") : TEXT("UP"), *RowView.Label);
		}
	}

	// ---- Adjust (only DIFFICULTY answers, exactly as left/right on the keyboard) -----------------
	if (TracePadMenu::StepRepeat(TracePadMenu::NavX(PC), PadNavX, PadNextAdjustTime, Now))
	{
		AdjustSelection(PadNavX);
	}

	// ---- A: activate ------------------------------------------------------------------------------
	if (bConfirm)
	{
		FTraceMenuRowView RowView;
		BuildRowView(Selected, true, RowView);
		UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] Pad A (confirm) on %s."), *RowView.Label);

		ActivateSelection();
		return;
	}

	// ---- B: back ----------------------------------------------------------------------------------
	//
	// CancelPressed, which is Escape's handler — so B does on this screen exactly what Escape does:
	// move the highlight to QUIT, and quit only if it was already there. That two-step is deliberate
	// (see CancelPressed) and a pad gets the same protection rather than a shortcut out of the game.
	//
	// MENU/START also arrives here, as an Escape, because UTraceGamepadInputSubsystem::TickMenuButton
	// synthesises one and ATraceMenuPlayerController binds Escape to CancelPressed. So a pad has two
	// ways back, which is the same arrangement the options overlay settled on in D31-PAD.
	if (bBack)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] Pad B (back)."));
		CancelPressed();
	}
}

// -------------------------------------------------------------------------------------------------
// The JOIN prompt, and the one honest limit of this tranche
// -------------------------------------------------------------------------------------------------
//
// WHAT A PAD DOES AT A TEXT FIELD, DECIDED AND WRITTEN DOWN:
//
//   A   CONNECT to whatever is in the field. The field is pre-filled with the last address that was
//       tried (see OpenJoinPrompt), so "the same four people playing again tomorrow" is JOIN, A —
//       pad only, no keyboard, no clipboard.
//   B   CANCEL, back to the title rows. *** THIS IS THE ANTI-DEAD-END, and it is the one behaviour
//       here that is not optional. *** A pad player who opens this prompt and cannot leave it has a
//       game they must force-quit. MENU/START gets out too, by the synthetic Escape the prompt's own
//       FTraceTextEntry::Poll reads — two independent ways out, on purpose.
//   X   PASTE the clipboard over the field. This is what makes a NEW host reachable pad-only: the
//       address arrives in a chat window, the player copies it there and pastes it here.
//
// TYPING IS NOT SOLVED, AND IS NOT PRETENDED TO BE. An on-screen keyboard is out of scope for this
// tranche (stated in the brief). A pad alone cannot enter an address that is neither remembered nor
// on the clipboard; that needs a keyboard. (The panel used to say so; the co-developer's text pass cut
// the line.) A known limitation rather than a buried one.
//
// THE PASTE is FTraceTextEntry::PasteReplace: the field's own legal-character filter, so the pad and a
// typed Ctrl/Cmd+V cannot disagree about what an address may contain.

void ATraceMenuHUD::PollPadJoinPrompt(bool bConfirm, bool bBack, bool bAlt)
{
	// ---- B: cancel ---------------------------------------------------------------------------------
	//
	// FIRST, before anything that could act on the field. This is the escape hatch; if any line below
	// it ever throws or early-returns, the way out must already have been taken.
	if (bBack)
	{
		CloseJoinPrompt(TEXT("Pad B"));
		return;
	}

	// ---- X: paste ------------------------------------------------------------------------------------
	//
	// Through FTraceTextEntry::PasteReplace — the field's own filter (the same legal-character rule a
	// typed Ctrl/Cmd+V uses), the caret at the END of the pasted address, and the paste stamped so the
	// PASTED note shows. This used to be a second copy of the filter here, and it left the caret at the
	// start of the field and never showed the note.
	if (bAlt)
	{
		FString Clipboard;
		FPlatformApplicationMisc::ClipboardPaste(Clipboard);

		if (!JoinEntry.PasteReplace(Clipboard, Now))
		{
			// Said out loud rather than swallowed: a paste button that does nothing on an empty
			// clipboard is indistinguishable from a paste button that is broken.
			JoinError = TRACE_TEXT("MENU.JOIN_ERROR_CLIPBOARD_EMPTY", "NOTHING ADDRESS-LIKE ON THE CLIPBOARD");
			JoinErrorText = JoinEntry.GetText();
			UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] Pad X -> clipboard held nothing usable."));
			return;
		}

		JoinError.Reset();
		JoinErrorText.Reset();
		UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] Pad X -> pasted '%s' into the JOIN field."), *JoinEntry.GetText());
		return;
	}

	// ---- A: connect ------------------------------------------------------------------------------------
	//
	// Straight into ConfirmJoin, the very function Enter reaches through FTraceTextEntry's submit
	// edge — so an empty field produces the same "ENTER AN ADDRESS" complaint and the prompt stays
	// open, rather than a pad getting a different answer from a keyboard.
	if (bConfirm)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] Pad A -> JOIN connect ('%s')."), *JoinEntry.GetText());
		ConfirmJoin();
	}
}

void ATraceMenuHUD::MoveSelection(int32 Delta)
{
	// The overlay polls its own input (see FTraceOptionsMenu). These handlers are still wired to the
	// controller's key bindings and would otherwise move the selection UNDERNEATH the settings panel.
	if (OptionsMenu.IsOpen() || IsJoinPromptOpen() || bTravelling || Delta == 0)
	{
		return;
	}

	const int32 Next = FMath::Clamp(static_cast<int32>(Selected) + Delta, 0, static_cast<int32>(ETraceMenuRow::Count) - 1);

	// FX/AUDIO PLAN §5.1 — "UIHover: menu row focus change", client-side 2D (the ButtonPress
	// precedent is in ActivateSelection below).
	//
	// ON THE CHANGE, NEVER ON THE PRESS. The clamp above means holding DOWN at the bottom of the list
	// calls this function every frame while Selected does not move, and a sound on every one of those
	// frames would turn the end of the list into a buzz.
	if (Next != static_cast<int32>(Selected))
	{
		TraceAudio::PlayLocal2D(this, TraceSoundEvents::UIHover);
	}

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
		const ETraceBotDifficulty Next = TraceDifficulty::Step(Difficulty, Delta);

		// The same focus-change sound every other change on this screen makes, and only on a change:
		// holding RIGHT on HARD repeats this call while nothing moves.
		if (Next != Difficulty)
		{
			TraceAudio::PlayLocal2D(this, TraceSoundEvents::UIHover);
		}
		SetDifficulty(Next);
		return;
	}

	// SCORING MODE used to be the second value row and the other half of this function. It stepped
	// the A/B toggle and applied it on every change rather than only on PLAY, so the value on screen
	// was never a promise the match failed to keep. The endzone ruleset is gone, so DIFFICULTY is
	// the only row that answers a left/right key.
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

	// SPEC v26 §9 — "ButtonPress: a menu row is activated", CLIENT SIDE. A menu is by definition
	// local: there is no server, no pawn and no world position, so this is the 2D call and it never
	// sends anything.
	//
	// AFTER both refusals above, deliberately. A click swallowed by the open-overlay test or by the
	// title screen's grace window did NOT activate a row, and a click sound on a press that did
	// nothing is how a player learns to distrust the menu.
	TraceAudio::PlayLocal2D(this, TraceSoundEvents::ButtonPress);

	switch (Selected)
	{
	case ETraceMenuRow::Play:
		StartMatch();
		break;

	case ETraceMenuRow::Join:
		OpenJoinPrompt();
		break;

	case ETraceMenuRow::Practice:
		StartPracticeRange();
		break;

	case ETraceMenuRow::Difficulty:
		// Activating the row cycles it. Wraps, unlike the arrow keys: a click has no "other
		// direction" to offer, so stopping dead at HARD would just look broken.
		SetDifficulty(static_cast<ETraceBotDifficulty>((static_cast<int32>(Difficulty) + 1) % TraceDifficulty::Count));
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
	// ---- A TRAVEL OWNS ESCAPE, AND IT NEVER REACHES QUIT ----------------------------------------------
	//
	// THE STUCK STATE THIS FIXES. After CONNECT the card read "CONNECTING TO x" for as long as the
	// handshake took (20 s to a dead address) with no way out, and this function had no travel guard:
	// the first Escape / pad B / MENU silently moved the hidden highlight to QUIT, and the second one
	// CLOSED THE GAME. The natural "cancel" gesture quit the application.
	//
	// Now: during a JOIN, Escape cancels it and puts the prompt back; during PLAY or PRACTICE (a local
	// load that is gone within a frame) it does nothing. First, before every other branch, so no path
	// through this function can quit while a travel is under way. Pad B and MENU/START arrive here too.
	if (bTravelling)
	{
		if (IsJoinInFlight())
		{
			CancelJoin();
		}
		return;
	}

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
	// FViewport, not Slate. The comment here used to justify that with "this module deliberately does
	// not depend on Slate (see Trace.Build.cs)", which is FALSE as of spec v17 §4 — UMG, Slate and
	// SlateCore are all dependencies now. The real reason is simpler and still holds:
	// IsForegroundWindow is on the Engine-side viewport interface, which is where the answer lives.
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
	JoinPressedButton = INDEX_NONE;
	bTravelCancelArmed = false;

	if (OptionsMenu.IsOpen())
	{
		return;
	}

	// ---- THE JOIN PROMPT'S CONNECT / BACK, and the connecting card's CANCEL -----------------------
	//
	// Both used to ignore the mouse entirely: the prompt had nothing to click and the card could not
	// be left at all. Same contract as the rows: the press ARMS whatever is under the pointer, the
	// release on the same target fires (MouseReleased). No cursor-has-moved test here — that defence
	// is for the stray click a window can deliver the moment the title appears, and neither of these
	// can be on screen before the player has already done something.
	if (IsJoinPromptOpen())
	{
		FVector2D PromptPoint = FVector2D::ZeroVector;
		if (GetCursorPoint(PromptPoint))
		{
			JoinPressedButton = JoinButtonAtPoint(PromptPoint);
		}
		return;
	}
	if (bTravelling)
	{
		FVector2D CardPoint = FVector2D::ZeroVector;
		bTravelCancelArmed = IsJoinInFlight() && TravelCancelRect.bIsValid && GetCursorPoint(CardPoint)
			&& TravelCancelRect.IsInside(CardPoint);
		return;
	}

	if (!bRowRectsValid)
	{
		return;
	}

	// *** SPEC v15 §4 — "menu presses are a single press not two clicks". THIS IS WHERE THE FIRST
	// CLICK WAS GOING, and it was a guard this file added on purpose. ***
	//
	// A test used to stand here that dropped the mouse-down whenever the viewport had not reported
	// itself foreground as of the last drawn frame, on the theory that such a press must be the
	// click bringing the window forward. It is REMOVED, for two reasons that both had to hold:
	//
	//   IT NEVER CAUGHT THE BUG IT WAS ADDED FOR. Read the history on AcceptUnlockTime in the
	//   header: every one of the five logged self-activations arrived "while the viewport already
	//   reported itself foreground". The stray pair was the viewport leaving
	//   CapturePermanently_IncludingInitialMouseDown, fixed at its cause in
	//   ATraceMenuPlayerController::BeginPlay (NoCapture), and the discriminator that DOES catch it
	//   is the cursor-movement test below. This branch was defending nothing.
	//
	//   IT COST A REAL CLICK EVERY TIME, which IS the reported bug. Measured with
	//   -TraceMenuClickTest against a real window whose focus another application had taken:
	//   windowFocusedLastFrame=0 on every attempt, "Ignored the mouse-down that brought the window
	//   to the foreground" on every attempt, and the row never activated at all. In a real session
	//   the first physical click restores the key window and is swallowed here; the second lands
	//   with focus and works. Two clicks — on PLAY, on JOIN and on SETTINGS — every time the player
	//   had clicked away from the game, which on a windowed macOS build is most of the time.
	//
	// The focus state is still SAMPLED, and it is printed on the press below rather than acted on.
	// "Was the window ours when that click landed?" is a question this file has had to answer twice
	// now, and the answer must stay in the log; it must just not eat the click.
	//
#if !UE_BUILD_SHIPPING
	// THE RED ARM, kept rather than deleted, because a fix nobody can re-measure against the bug is
	// a fix that gets undone by the next person who reads the comment above and disagrees with it.
	//   Trace.Menu.FocusGuardRedArm 1
	// puts the removed guard back for this session, so -TraceMenuClickTest can be run both ways in
	// ONE build: 1 click per row with it off, and the row never activating at all with it on. Same
	// pattern as Trace.V13.Hotkeys' `toggle` arm.
	if (GTraceMenuFocusGuardRedArm != 0 && !bWindowFocusedLastFrame)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[MenuInput] [RED ARM] Ignored the mouse-down that brought the window to the foreground."));
		return;
	}
#endif

	// THE DEFENCE THAT ACTUALLY WORKS is next. See bCursorHasMoved: the spurious pair lands at a
	// pointer that has not moved since the title screen appeared, and a player always moves the
	// mouse onto a button before pressing it.
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

	UE_LOG(LogTraceGame, Display,
		TEXT("[MenuInput] Armed row %d at (%.0f, %.0f). windowFocused=%d — this press counts whether or "
		     "not the window was ours (spec v15 §4)."),
		Row, Point.X, Point.Y, bWindowFocusedLastFrame ? 1 : 0);
}

void ATraceMenuHUD::MouseReleased()
{
	const int32 Armed = PressedRow;
	PressedRow = INDEX_NONE;
	const int32 ArmedButton = JoinPressedButton;
	JoinPressedButton = INDEX_NONE;
	const bool bCancelArmed = bTravelCancelArmed;
	bTravelCancelArmed = false;

	if (OptionsMenu.IsOpen())
	{
		return;
	}

	if (IsJoinPromptOpen())
	{
		FVector2D PromptPoint = FVector2D::ZeroVector;
		if (ArmedButton == INDEX_NONE || !GetCursorPoint(PromptPoint) || JoinButtonAtPoint(PromptPoint) != ArmedButton)
		{
			return;
		}
		TraceAudio::PlayLocal2D(this, TraceSoundEvents::ButtonPress);
		if (ArmedButton == TraceMenuHUDJoin::Connect)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] CONNECT clicked ('%s')."), *JoinEntry.GetText());
			ConfirmJoin();
		}
		else
		{
			CloseJoinPrompt(TEXT("BACK clicked"));
		}
		return;
	}

	if (bTravelling)
	{
		FVector2D CardPoint = FVector2D::ZeroVector;
		if (bCancelArmed && IsJoinInFlight() && TravelCancelRect.bIsValid && GetCursorPoint(CardPoint)
			&& TravelCancelRect.IsInside(CardPoint))
		{
			UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] CANCEL clicked on the connecting card."));
			CancelJoin();
		}
		return;
	}

	if (Armed == INDEX_NONE)
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
	TravelKind = ETraceMenuTravel::Host;
	if (const UWorld* World = GetWorld())
	{
		TravelStartRealTime = World->GetRealTimeSeconds();
	}

	// Applied here as well as carried in the URL: on a listen server the travel destination reads
	// the option and applies it itself, but in standalone this call is what makes the very first
	// match honour the setting even if the URL is ever dropped on the floor.
	TraceDifficulty::ApplyToSettings(Difficulty);

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
	// ATraceGameMode::InitGame), and it is exactly the kind of bug that makes a toggle look like it
	// does nothing.
	//
	// "?mode=a|b" USED TO BE THE MIDDLE OPTION HERE, carrying the A/B scoring toggle across the
	// travel. The endzone ruleset was removed, so there is no mode to carry and no option to parse
	// at the other end (ATraceGameMode::ResolveScoringMode is gone with it).
	//
	// "?characters=0|1" carries spec v14 §3's toggle, and it goes on the URL rather than being left
	// to the destination's own settings read because the settings page the player just used lives in
	// THIS process, and a listen server that resolved the toggle from its own ini would honour a
	// value the player may have changed a second ago.
	const FString Options = FString::Printf(TEXT("%s=%s?%s=%d?listen"),
		TraceDifficulty::UrlOption, *TraceDifficulty::ToUrlValue(Difficulty),
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
	//
	// One line, the same one the title's address chip shows (NET.PORT_BUSY), and the HUD's HOSTING
	// chip carries the real address in the match. The explanation is for the log.
	const bool bPortFree = TraceNet::IsListenPortAvailable();
	const FString PortBusyLine = TRACE_TEXTF("NET.PORT_BUSY", "PORT {0} BUSY - USING ANOTHER PORT", { TraceNet::DefaultPort });
	if (!bPortFree)
	{
		TraceNet::ReportFailure(PortBusyLine,
			FString::Printf(TEXT("UDP %d is held by another process; the listen server will bind the next free "
				"port. The match is joinable at the address the HUD's HOSTING chip shows, not at :%d."),
				TraceNet::DefaultPort, TraceNet::DefaultPort));
	}

	TravelCaption = bPortFree
		? TRACE_TEXTF("MENU.TRAVEL_HOSTING_ON", "HOSTING ON {0}", { TraceNet::GetHostEndpoint() })
		: PortBusyLine;

	UE_LOG(LogTraceGame, Display, TEXT("Title screen: PLAY -> %s?%s  hosting on %s, port %s"),
		TraceMaps::Arena, *Options,
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
	JoinPressedButton = INDEX_NONE;
	JoinHoveredButton = INDEX_NONE;

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
		// The example address is the field's own placeholder, drawn right above this line.
		JoinError = TRACE_TEXT("MENU.JOIN_ERROR_NO_ADDRESS", "ENTER AN ADDRESS");
		JoinErrorText = Typed;
		return;
	}

	APlayerController* PC = GetOwningPlayerController();
	if (PC == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[MenuInput] JOIN refused: the title screen has no local player controller."));
		JoinError = TRACE_TEXT("NET.ERROR_CONNECTION_FAILED", "CONNECTION FAILED");
		JoinErrorText = Typed;
		return;
	}

	// Remembered on the ATTEMPT, not on success. A join that times out is overwhelmingly likely to
	// be retried against the same address once the host's firewall is fixed, and making the player
	// retype it in exactly that situation is the opposite of helpful.
	LastJoinAddress = Address;
	TraceNet::SaveLastJoinAddress(Address);

	TraceNet::ClearFailure();
	TraceNet::NoteJoinAttempt();
	JoinEntry.End();
	JoinPressedButton = INDEX_NONE;

	// AFTER the two early returns above, so a refused address cannot leave a join marked in flight.
	bTravelling = true;
	TravelKind = ETraceMenuTravel::Join;
	if (const UWorld* World = GetWorld())
	{
		TravelStartRealTime = World->GetRealTimeSeconds();
	}
	TravelCaption = TRACE_TEXTF("MENU.TRAVEL_CONNECTING_TO", "CONNECTING TO {0}", { Address });

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

void ATraceMenuHUD::StartPracticeRange()
{
	if (bTravelling)
	{
		return;
	}
	bTravelling = true;
	TravelKind = ETraceMenuTravel::Practice;
	if (const UWorld* World = GetWorld())
	{
		TravelStartRealTime = World->GetRealTimeSeconds();
	}

	// A new attempt: whatever went wrong last time is no longer what is happening now.
	TraceNet::ClearFailure();

	// =============================================================================================
	// SPEC v19 §2 — "?game=" IS THE WHOLE MECHANISM, AND IT IS A URL OPTION, NOT A SWITCH.
	//
	// TracePracticeRange::IsActive() asks exactly one question — "is the authoritative game mode an
	// ATracePracticeGameMode?" — and a game mode can only arrive on the travel URL. No setting, no
	// CVar and no ini key can turn a match into a range, which is what makes the range's cheats
	// structurally unable to leak into a real match rather than merely gated by a bool somebody
	// could flip. This row must therefore travel with ?game= and nothing else may set it.
	//
	// SEPARATED WITH '?', NOT '&'. UE chains URL options with '?'; "a=1&b=2" parses as ONE option
	// called "a" whose value is the whole rest of the string. StartMatch()'s comment records that
	// this has already cost this project real bugs, and this pass alone found two more of them in
	// harness launch URLs — a "?mode=b&bots=8" that silently ran mode A for an entire test slice.
	//
	// NO "listen". The range is a single-player room and standing a net driver up for it would put
	// a joinable server on 7777 that nobody should be able to join. It is also why IsActive()'s
	// GetAuthGameMode() answer — always false on a remote client — is the safe direction.
	// =============================================================================================
	const FString Options = TEXT("game=/Script/Trace.TracePracticeGameMode");

	TravelCaption = TRACE_TEXT("MENU.TRAVEL_ENTERING_PRACTICE", "ENTERING THE PRACTICE RANGE");

	UE_LOG(LogTraceGame, Display, TEXT("Title screen: PRACTICE -> %s?%s"), TraceMaps::Arena, *Options);

	UGameplayStatics::OpenLevel(this, FName(TraceMaps::Arena), /*bAbsolute=*/true, Options);
}

void ATraceMenuHUD::QuitGame()
{
	UE_LOG(LogTraceGame, Log, TEXT("Title screen: QUIT."));
	UKismetSystemLibrary::QuitGame(this, GetOwningPlayerController(), EQuitPreference::Quit, /*bIgnorePlatformRestrictions=*/false);
}

void ATraceMenuHUD::SetDifficulty(ETraceBotDifficulty InDifficulty)
{
	Difficulty = InDifficulty;
	TraceDifficulty::ApplyToSettings(Difficulty);

	// Saved on every change, so the next title screen — after a match, a failed join, RETURN TO TITLE
	// or a restart — shows what the player picked instead of quietly resetting to NORMAL.
	TraceDifficulty::SetSavedSetting(Difficulty);
}

void ATraceMenuHUD::CloseJoinPrompt(const TCHAR* Why)
{
	UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] JOIN prompt closed (%s)."), Why);
	JoinEntry.End();
	JoinError.Reset();
	JoinErrorText.Reset();
	JoinPressedButton = INDEX_NONE;
	JoinHoveredButton = INDEX_NONE;
}

void ATraceMenuHUD::CancelJoin()
{
	if (!IsJoinInFlight())
	{
		return;
	}

	UWorld* World = GetWorld();
	const float Dialled = (World != nullptr) ? FMath::Max(0.f, World->GetRealTimeSeconds() - TravelStartRealTime) : 0.f;

	if (GEngine != nullptr && World != nullptr)
	{
		// BOTH halves, in this order. ClientTravel only QUEUED the address in the world context; the
		// engine builds the pending net game from it on its next tick. A cancel in the same frame as the
		// connect therefore finds no pending game at all, and without clearing the queued URL the join
		// would go ahead one tick later, behind a prompt that says it did not.
		if (FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
		{
			Context->TravelURL.Empty();
		}

		// Closes the half-open connection, destroys its net driver and drops the pending game.
		GEngine->CancelPending(World);
	}

	bTravelling = false;
	TravelKind = ETraceMenuTravel::None;
	TravelCaption.Reset();
	TravelCancelRect = FBox2D(ForceInit);
	bTravelCancelArmed = false;

	// The player called it off: nothing failed, and the next title screen must not treat this attempt
	// as a failed join.
	TraceNet::ForgetJoinAttempt();
	TraceNet::ClearFailure();

	UE_LOG(LogTraceGame, Display, TEXT("[MenuInput] JOIN cancelled after %.1fs; the prompt is open again on '%s'."),
		Dialled, *LastJoinAddress);

	// Back to the prompt, pre-filled with the address that was being dialled (ConfirmJoin remembered
	// it), because the commonest reason to cancel is a wrong address. Escape again returns to the rows.
	// FTraceTextEntry::Begin ignores the rest of this frame, so the Escape that cancelled cannot also
	// close the prompt it reopens.
	Selected = ETraceMenuRow::Join;
	OpenJoinPrompt();
}

int32 ATraceMenuHUD::JoinButtonAtPoint(const FVector2D& Point) const
{
	for (int32 Index = 0; Index < TraceMenuHUDJoin::ButtonCount; ++Index)
	{
		if (JoinButtonRects[Index].bIsValid && JoinButtonRects[Index].IsInside(Point))
		{
			return Index;
		}
	}
	return INDEX_NONE;
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

	// ---- D32-PADMENU — the controller, read once per drawn frame --------------------------------
	//
	// HERE, at the top, for the same reason the address field is serviced before anything is drawn:
	// a press has to be able to change what this frame shows. A pad that moved the highlight one
	// frame after the button went down would feel like the lag every other input on this screen does
	// not have.
	//
	// ABOVE the mouse-hover block below on purpose. Hover only re-selects when the pointer has
	// actually MOVED (see bCursorHasMoved), so on a machine whose mouse is sitting still the pad's
	// choice survives; on a machine where the player is using both, the last device to move wins,
	// which is the behaviour the keyboard has always had against the mouse here.
	PollPadInput();

	FontSmall  = GEngine->GetSmallFont();
	FontMedium = GEngine->GetMediumFont();
	FontLarge  = GEngine->GetLargeFont();

	// Mouse hover, but only once the cursor has actually moved. Without the movement test a cursor
	// parked over QUIT would silently override every keyboard press.
	//
	// Skipped entirely while the SETTINGS overlay is up: it samples and draws its own pointer, and
	// tracking it here would quietly re-select whichever title row happened to be underneath.
	//
	// SAMPLED while the JOIN prompt is up and while travelling — the prompt has CONNECT / BACK and the
	// connecting card has CANCEL, and both draw the live pointer (it used to freeze where it was when
	// the prompt opened, dimmed under the scrim) — but the ROWS only follow it when neither is up.
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
				if (bRowRectsValid && !bTravelling && !IsJoinPromptOpen())
				{
					for (int32 Index = 0; Index < static_cast<int32>(ETraceMenuRow::Count); ++Index)
					{
						if (RowRects[Index].bIsValid && RowRects[Index].IsInside(Position))
						{
							// FX/AUDIO PLAN §5.1 — the pointer's half of "menu row focus change".
							// Guarded on the row ACTUALLY changing: this block already only runs when
							// the cursor moved more than 2 px, but 2 px of travel inside one row must
							// not re-announce the row it is already on.
							if (Selected != static_cast<ETraceMenuRow>(Index))
							{
								TraceAudio::PlayLocal2D(this, TraceSoundEvents::UIHover);
							}

							Selected = static_cast<ETraceMenuRow>(Index);
							break;
						}
					}
				}
			}
			LastCursorPos = Position;

			// The JOIN buttons' half of "focus change": the hover sound when the pointer moves onto one.
			if (IsJoinPromptOpen())
			{
				const int32 Hovered = JoinButtonAtPoint(Position);
				if (Hovered != INDEX_NONE && Hovered != JoinHoveredButton)
				{
					TraceAudio::PlayLocal2D(this, TraceSoundEvents::UIHover);
				}
				JoinHoveredButton = Hovered;
			}
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
			CloseJoinPrompt(TEXT("Escape"));
		}
		else if (!JoinError.IsEmpty() && JoinEntry.GetText() != JoinErrorText)
		{
			// Any edit clears the complaint. An error that outlives the text it was about is worse
			// than no error at all — the player fixes the address and the screen still calls it wrong.
			JoinError.Reset();
			JoinErrorText.Reset();
		}
	}

	// ---- Which renderer draws this frame (spec v17 §4; spec v23 §A2 REVERTED by spec v25 §1) -------
	//
	// This is back to what it read before spec v23 §A2, and the revert is the settings-crash fix.
	//
	// v23 §A2 kept the UMG title screen up behind an open modal by drawing the modal on the engine's
	// FOREGROUND canvas (`FViewport::GetDebugCanvas()`), which Slate paints over the UI. That surface
	// is DEFERRED — the engine hands it to the render thread from its own paint pass — so everything
	// drawn into it outlived the frame that recorded it, still holding raw texture pointers and a
	// shared reference to the viewport. Opening SETTINGS crashed, three reports running. The full
	// argument, both arms of the measurement and all three captured callstacks are in the header of
	// UI/TraceOptionsMenu.h.
	//
	// So a Canvas modal is under Slate again, and the widget has to stand down for one. The cost was
	// the §A2 defect returning: for as long as SETTINGS or the JOIN prompt is open, the screen behind
	// it is the Canvas title screen rather than the artist's UMG one — measured at 1920x1080 as 58.8%
	// of the frame changing renderer on one keypress. That is the trade spec v25 §1 asks for, and the
	// release pass (UI plan WP9) shrank what it costs: the Canvas title now draws the artist's
	// wordmark, swoosh and button plates itself (through TraceMenuKit), so the swap is a renderer
	// change the player is not supposed to notice rather than a different-looking game under a scrim.
	//
	// STILL TAKEN HERE, NOT INSIDE EACH MODAL, and that has not stopped mattering: a frame that kept
	// the widget up while a modal drew underneath it would be a title screen with an INVISIBLE
	// settings panel behind it — a hard lock on a screen with no way out. One bool, one decision.
	//
	// SINCE THE HANDMADE-KIT PASS THE SWAP IS INVISIBLE, and a TRAVEL takes this path too. The Canvas
	// title below is the kit — pure black, the artist's navy wordmark and white swoosh, the plates, the
	// white blade — with no cyan grid, bezel or slate backdrop left in it, so opening SETTINGS or JOIN
	// dims the same screen instead of switching to a teal one. A travel draws the kit's black travel
	// card over it (DrawTravelOverlay), which is what lets the connecting card carry the ESC CANCEL
	// legend, the elapsed seconds and the pointer.
	const bool bWidgetAvailable = TryAdoptMenuWidget();
	const bool bModalOpen = OptionsMenu.IsOpen() || IsJoinPromptOpen() || bTravelling;
	const bool bUseWidgetThisFrame = bWidgetAvailable && !bModalOpen;
	bMenuUmgActive = bUseWidgetThisFrame;
	bMenuUmgAvailable = bWidgetAvailable;

	// Once per process. There is only one surface now, and saying so in the log is what stops the
	// next reader assuming the elevation is still there and re-deriving its removal from scratch.
	if (!bModalSurfaceLogged)
	{
		bModalSurfaceLogged = true;
		UE_LOG(LogTraceGame, Display,
			TEXT("[MenuUI] Modal surface: SCENE — Canvas modals draw under Slate (spec v25 §1 removed "
			     "the foreground-canvas elevation; the UMG title screen stands down while one is up)."));
	}

	if (bUseWidgetThisFrame)
	{
		MenuWidget->SetVisibility(ESlateVisibility::HitTestInvisible);

		FTraceTitleMenuView View;
		BuildMenuView(View);
		MenuWidget->ApplyView(View);

		// Hit testing stays here, off the rectangles Slate actually laid the rows out at. See
		// PublishRowRectsFromWidget and the header of UTraceTitleMenuWidget.
		PublishRowRectsFromWidget();
	}
	else
	{
		if (MenuWidget != nullptr)
		{
			MenuWidget->SetVisibility(ESlateVisibility::Collapsed);
		}

		// No grid floor and no bezel any more: both were the pre-kit cyan Tron screen, and this is the
		// screen every SETTINGS / JOIN modal shows through. Black, like the kit (stylespec §1).
		DrawBackdrop();
		DrawWordmark();
		DrawAddressChip();
		DrawMenuRows();
		DrawFooter();

		// Before the join prompt below and the options overlay at the end of DrawHUD, so a modal's
		// scrim dims it rather than losing it.
		DrawVersionString();

		DrawCursor();
		DrawTravelOverlay();
	}

	// Over everything except the settings overlay, which cannot be open at the same time. A no-op
	// while the field is inactive.
	DrawJoinPrompt();

	// AFTER the join prompt, not before. Measured from a screenshot: drawn earlier, the prompt's
	// 78%-black scrim sat on top of the banner and turned the one message the player most needs to
	// read into a dim brown smudge. The failure has to outrank every modal on this screen — the
	// commonest moment to see it is the moment you are about to retype the address that just failed.
	//
	// ONE BRANCH AGAIN (spec v25 §1). There used to be a second arm here that drew the banner inside
	// a foreground-canvas scope for the case "widget up AND a modal open". With the elevation removed
	// that case cannot occur — bUseWidgetThisFrame is false whenever bModalOpen is true, three lines
	// of decision above — so the arm was unreachable code holding the mechanism alive.
	if (!bUseWidgetThisFrame)
	{
		DrawFailureBanner();
	}

	// Last of all, over everything, including the travel overlay. Tick() is a no-op while closed.
	OptionsMenu.Tick(this, GetOwningPlayerController(), ViewW, ViewH, UIScale, Now);

#if !UE_BUILD_SHIPPING
	// `Trace.UI.Kit.Specimen 1` — every handmade-kit control on one black page, over all of the
	// above, for a screenshot. A no-op while the CVar is 0. See UI/Widgets/Menu/TraceMenuKit.h.
	TraceMenuKit::DrawSpecimenIfRequested(this, ViewW, ViewH, UIScale, Now);

	// `Trace.Menu.JoinVerify` — one step per drawn frame, after this frame's input and draw.
	TickJoinVerify();
#endif
}

void ATraceMenuHUD::DrawBackdrop()
{
	// Opaque, and drawn first: the menu map is empty, and whatever the renderer decides to put behind
	// an empty map is not something the title screen should be at the mercy of.
	//
	// THE KIT'S BLACK (stylespec §1), the same black the UMG title's Backdrop is. This used to be a
	// slate "Void" with a cyan haze, and under it DrawGridFloor drew a full cyan perspective grid and
	// DrawBezel a cyan frame with corner ticks — so the black title turned into a teal Tron screen the
	// moment SETTINGS or JOIN opened. Both are gone. The UMG title's steel/amber grid is owner-requested
	// and stays on that renderer; under a modal's 0.82 scrim it is all but invisible, so this path
	// does not reproduce it.
	TraceMenuKit::DrawBackground(this, ViewW, ViewH);
}

float ATraceMenuHUD::DrawTitleBlock()
{
	UTexture2D* Mark = TraceMenuKit::Sprite(ETraceKitSprite::Wordmark);
	if (Mark == nullptr || Mark->GetSizeX() <= 0 || Mark->GetSizeY() <= 0)
	{
		return -1.f;
	}

	// The SAME composition the UMG widget uses (TraceTitleLayout, one copy for both renderers): the
	// artist's navy wordmark in its amber glow, their white metal swoosh under it at the measured
	// offsets. White tint — the sprites carry their own colour, cut by Scripts/slice-ui-assets.py
	// notes 6 and 7 exactly as the artist drew them.
	const float CX = ViewW * 0.5f;
	const float MarkW = FMath::Min(TraceTitleLayout::MarkWidth * UIScale, ViewW * TraceTitleLayout::MarkMaxWidthFraction);
	const float MarkH = MarkW * (static_cast<float>(Mark->GetSizeY()) / static_cast<float>(Mark->GetSizeX()));
	const float MarkTop = TraceTitleLayout::MarkTopY * UIScale;
	const float TaglineTop = TraceTitleLayout::TaglineY * UIScale;

	DrawTexture(Mark, CX - MarkW * 0.5f, MarkTop, MarkW, MarkH,
		0.f, 0.f, 1.f, 1.f, FLinearColor::White, BLEND_Translucent);

	float Bottom = MarkTop + MarkH;

	UTexture2D* SwooshTex = TraceMenuKit::Sprite(ETraceKitSprite::Swoosh);
	if (SwooshTex != nullptr && SwooshTex->GetSizeX() > 0 && SwooshTex->GetSizeY() > 0)
	{
		const float SwooshAspect =
			static_cast<float>(SwooshTex->GetSizeY()) / static_cast<float>(SwooshTex->GetSizeX());
		float SwooshW = MarkW * TraceTitleLayout::SwooshWidthOfMark;
		const float SwooshTop = MarkTop + MarkH + MarkW * TraceTitleLayout::SwooshGapOfMark;

		// The same clamp the widget applies: whatever the sheet says, the flourish stops short of the
		// tagline.
		const float MaxSwooshH = FMath::Max(1.f, TaglineTop - TraceTitleLayout::SwooshClearOfTagline * UIScale - SwooshTop);
		if (SwooshW * SwooshAspect > MaxSwooshH)
		{
			SwooshW = MaxSwooshH / FMath::Max(SwooshAspect, KINDA_SMALL_NUMBER);
		}

		DrawTexture(SwooshTex,
			CX - MarkW * TraceTitleLayout::SwooshLeftOfMark - SwooshW * 0.5f, SwooshTop,
			SwooshW, SwooshW * SwooshAspect,
			0.f, 0.f, 1.f, 1.f,
			FLinearColor(1.f, 1.f, 1.f, TraceTitleLayout::SwooshOpacity), BLEND_Translucent);

		Bottom = SwooshTop + SwooshW * SwooshAspect;
	}

	return Bottom;
}

void ATraceMenuHUD::DrawWordmark()
{
	const float CX = ViewW * 0.5f;
	const FString& Tagline = TRACE_TEXT("MENU.TAGLINE", "5 V 5    -    ONE CORE    -    DASH THE TRAIL TO KILL THE CARRIER");

	// ---- THE ARTIST'S MARK ----------------------------------------------------------------------------
	//
	// This screen is what shows through every modal's scrim. The sprite path comes first; the stroke
	// wordmark below is the FALLBACK, kept per the standing rule that a missing texture must leave the
	// menu drawable — and it is still what a fresh checkout shows before the sprites are generated.
	if (DrawTitleBlock() >= 0.f)
	{
		const float TaglineTop = TraceTitleLayout::TaglineY * UIScale;
		DrawTextCentered(Tagline, TraceMenuKit::CaptionInk, CX, TaglineTop, FontSmall, 1.15f * UIScale);

		// Remembered for DrawAddressChip, which has to sit exactly under the tagline and must not
		// re-derive the wordmark's geometry to find out where that is.
		TaglineBottomY = TaglineTop + MeasureHeight(TEXT("X"), FontSmall, 1.15f * UIScale);
		return;
	}

	const float CapHeight = ViewH * 0.155f;
	const float TitleY = ViewH * 0.135f;
	const float Thickness = FMath::Max(2.f, CapHeight * 0.055f);

	// White, not the retired cyan: the kit has no cyan in it.
	DrawStrokeTextCentered(TRACE_TEXT("MENU.WORDMARK", "TRACE"), TraceMenuArtStyle::WordDefault, CX, TitleY, CapHeight, Thickness);

	const float TaglineTop = TitleY + CapHeight + (46.f * UIScale);
	DrawTextCentered(Tagline, TraceMenuKit::CaptionInk, CX, TaglineTop, FontSmall, 1.15f * UIScale);

	// Remembered for DrawAddressChip, same as the sprite arm above.
	TaglineBottomY = TaglineTop + MeasureHeight(TEXT("X"), FontSmall, 1.15f * UIScale);
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
	const FString Caption = TRACE_TEXT("MENU.ADDRESS_CAPTION", "YOUR ADDRESS");

	const float CX = ViewW * 0.5f;
	const float CaptionScale = 1.0f * UIScale;
	const float ValueScale = 1.45f * UIScale;

	const float CaptionW = MeasureWidth(Caption, FontSmall, CaptionScale);
	const float ValueW = MeasureWidth(Endpoint, FontMedium, ValueScale);
	const float ValueH = MeasureHeight(Endpoint, FontMedium, ValueScale);

	const float Gap = 14.f * UIScale;
	const float PadX = 20.f * UIScale;
	const float PadY = 7.f * UIScale;

	const float ChipW = CaptionW + Gap + ValueW + PadX * 2.f;
	const float ChipH = ValueH + PadY * 2.f;
	const float ChipX = CX - ChipW * 0.5f;
	const float ChipY = TaglineBottomY + (14.f * UIScale);

	// The artist's plate, as on the UMG chip (a button frame at chip height), not the flat
	// cyan-edged box it used to be. The kit's fallback when the texture is not drawable yet.
	if (!TraceMenuKit::DrawPlate(this, TraceMenuKit::Sprite(ETraceKitSprite::BtnDefault), TraceMenuArtStyle::ButtonFrame,
		ChipX, ChipY, ChipW, ChipH, ChipH, FLinearColor::White))
	{
		TraceMenuKit::DrawFallbackPlate(this, ETraceKitState::Default, ChipX, ChipY, ChipW, ChipH);
	}

	const float CaptionY = ChipY + (ChipH - MeasureHeight(Caption, FontSmall, CaptionScale)) * 0.5f;
	TraceMenuHUDType::Draw(this, Caption, TraceMenuKit::CaptionInk, ChipX + PadX, CaptionY, FontSmall, CaptionScale);

	// White: the only place on this screen a raw number is allowed to be the loudest thing in its box.
	TraceMenuHUDType::Draw(this, Endpoint, TraceMenuArtStyle::WordDefault, ChipX + PadX + CaptionW + Gap, ChipY + PadY, FontMedium, ValueScale);

	// MEASURED CAVEAT. If something else already holds UDP 7777, UIpNetDriver does not fail — it
	// binds the next free port instead (observed: "IpNetDriver listening on port 7778"). The match is
	// still joinable, but the number in the chip above is then a lie, and a host reciting it would
	// send everybody to a port nothing is listening on. Saying so here, before PLAY, is cheaper than
	// the ten minutes of confusion on the other end.
	if (!TraceNet::IsDefaultPortFreeCached())
	{
		DrawTextCentered(TRACE_TEXTF("NET.PORT_BUSY", "PORT {0} BUSY - USING ANOTHER PORT", { TraceNet::DefaultPort }),
			TraceMenuArtStyle::AmberLifted(), CX, ChipY + ChipH + (4.f * UIScale), FontSmall, 0.95f * UIScale);
	}
}

void ATraceMenuHUD::DrawFailureBanner()
{
	FString Headline;
	double AgeSeconds = 0.0;
	if (!TraceNet::GetLastFailure(Headline, AgeSeconds) || Headline.IsEmpty())
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
	// Scale raised from 1.15 after reading a capture at 1280x720: the headline was a 9px strip. This
	// is the one message on the screen that has to survive being photographed and pasted into a chat.
	//
	// ONE LINE. The engine's own code and message used to be drawn under the headline, upper-cased and
	// cut at 140 characters ("CONNECTIONTIMEOUT: UNETCONNECTION::TICK: ..."), edge to edge. It is in
	// the log (TraceNet::ReportFailure), which is where the person a player sends it to will look.
	const float HeadScale = 1.45f * UIScale;

	const float BannerY = ViewH * 0.05f;
	const float PadY = 11.f * UIScale;
	const float HeadH = MeasureHeight(Headline, FontMedium, HeadScale);
	const float BannerH = HeadH + PadY * 2.f;

	DrawRect(FLinearColor(0.18f, 0.05f, 0.00f, 0.90f * Fade), 0.f, BannerY, ViewW, BannerH);
	DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::Amber, 0.85f * Fade), 0.f, BannerY, ViewW, FMath::Max(1.f, 2.f * UIScale));
	DrawRect(TraceMenuStyle::WithAlpha(TraceMenuStyle::Amber, 0.85f * Fade), 0.f, BannerY + BannerH - FMath::Max(1.f, 2.f * UIScale), ViewW, FMath::Max(1.f, 2.f * UIScale));

	DrawTextCentered(Headline, TraceMenuStyle::WithAlpha(TraceMenuStyle::Amber, Fade),
		ViewW * 0.5f, BannerY + PadY, FontMedium, HeadScale);
}

void ATraceMenuHUD::DrawJoinPrompt()
{
	if (!JoinEntry.IsActive())
	{
		JoinButtonRects[TraceMenuHUDJoin::Connect] = FBox2D(ForceInit);
		JoinButtonRects[TraceMenuHUDJoin::Back] = FBox2D(ForceInit);
		return;
	}

	namespace MJ = TraceMenuHUDJoin;
	const float S = UIScale;

	// ---- ON THE HANDMADE KIT ---------------------------------------------------------------------
	//
	// This was the pre-kit design: a flat dark rectangle with 1.6 px cyan edges, a cyan title, a
	// cyan-edged field with a cyan caret, and key hints as one long string — with no pointer (the OS
	// arrow came back) and nothing a mouse could press. Now it is built from the kit like the options
	// pages it sits beside: the kit's black scrim and a black panel with no edges, the field as the
	// artist's glowing HOVER plate (it has the keyboard, so it is the focused control), CONNECT and BACK
	// as real kit buttons, a [KEY] VERB legend, and the white blade pointer.
	//
	// The screen behind it is the Canvas title, which is the kit too now (DrawBackdrop), so opening
	// JOIN dims the same black screen instead of switching it to a teal one.
	TraceMenuKit::DrawScrim(this, ViewW, ViewH);

	const float CX = ViewW * 0.5f;
	const float PanelW = FMath::Min(MJ::PanelW * S, ViewW * 0.86f);
	const float PanelX = CX - PanelW * 0.5f;
	const float PanelY = ViewH * MJ::PanelTopFrac;

	// ---- The legend first, because the panel's height depends on how many lines it has ------------
	const FString ConnectWord = TRACE_TEXT("MENU.JOIN_CONNECT", "CONNECT");
	const FString BackWord = TRACE_TEXT("MENU.JOIN_BACK", "BACK");
	const FString PasteWord = TRACE_TEXT("MENU.JOIN_PASTE", "PASTE");

	TArray<FTraceKitLegendItem> Keys;
	Keys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_SELECT", "ENTER"), ConnectWord });
	Keys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_BACK", "ESC"), BackWord });
	Keys.Add({ TRACE_TEXT("MENU.JOIN_KEY_PASTE", "CTRL+V"), PasteWord });

	// Only once a controller has been seen (ShouldShowPadHints): a keyboard-only player is not told
	// about buttons they do not have.
	TArray<FTraceKitLegendItem> PadKeys;
	if (ShouldShowPadHints())
	{
		PadKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.PAD_KEY_SELECT", "A"), ConnectWord });
		PadKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.PAD_KEY_BACK", "B"), BackWord });
		PadKeys.Add({ TRACE_TEXT("MENU.JOIN_PAD_KEY_PASTE", "X"), PasteWord });
	}

	const float FullChipH = MJ::LegendChipH * S;
	const float LegendMaxW = PanelW - 2.f * 40.f * S;
	const float ChipH = TraceMenuKit::KeyLegendFit(FullChipH, LegendMaxW,
		{ TraceMenuKit::KeyLegendWidth(Keys, FullChipH), TraceMenuKit::KeyLegendWidth(PadKeys, FullChipH) });
	const int32 LegendLines = (TraceMenuKit::KeyLegendWidth(Keys, ChipH) > 0.f ? 1 : 0)
		+ (TraceMenuKit::KeyLegendWidth(PadKeys, ChipH) > 0.f ? 1 : 0);

	// ---- Vertical layout, top to bottom, in one place --------------------------------------------
	const float TitleMid = PanelY + (MJ::PanelPadTop + MJ::TitleCap * 0.5f) * S;
	const float SubtitleMid = TitleMid + (MJ::TitleCap * 0.5f + MJ::SubtitleGap + MJ::SubtitleCap * 0.5f) * S;
	const float FieldY = PanelY + MJ::FieldTop * S;
	const float FieldH = MJ::FieldH * S;
	const float NoteMid = FieldY + FieldH + MJ::NoteGap * S;
	const float ButtonsY = FieldY + FieldH + MJ::ButtonsGap * S;
	const float ButtonH = MJ::ButtonH * S;
	const float LegendY = ButtonsY + ButtonH + MJ::LegendGap * S;
	const float LegendBottom = LegendY + ((LegendLines > 0) ? (ChipH + (LegendLines - 1) * MJ::LegendLineGap * S) : 0.f);
	const float MachineMid = LegendBottom + MJ::MachineGap * S;
	const float PanelH = (MachineMid + MJ::MachineCap * 0.5f * S + MJ::PanelPadBottom * S) - PanelY;

	// A black panel with NO coloured edges (stylespec §0), over the scrim, as the options pages do it.
	DrawRect(FLinearColor(0.f, 0.f, 0.f, MJ::PanelAlpha), PanelX, PanelY, PanelW, PanelH);

	// ---- Title and subtitle: Sofachrome, the page-title face -------------------------------------
	TraceMenuKit::DrawCapText(this, TRACE_TEXT("MENU.JOIN_TITLE", "JOIN A GAME"), CX, TitleMid, MJ::TitleCap * S,
		TraceMenuArtStyle::WordDefault, ETraceTextWeight::Light, TraceText::EHAlign::Center, PanelW - 80.f * S);
	TraceMenuKit::DrawCapText(this, TRACE_TEXT("MENU.JOIN_SUBTITLE", "TYPE THE HOST'S ADDRESS"), CX, SubtitleMid,
		MJ::SubtitleCap * S, TraceMenuKit::CaptionInk, ETraceTextWeight::Light, TraceText::EHAlign::Center, PanelW - 80.f * S);

	// ---- The field: the artist's HOVER plate, because it is the focused control -------------------
	const float FieldX = PanelX + MJ::FieldSide * S;
	const float FieldW = PanelW - 2.f * MJ::FieldSide * S;
	if (!TraceMenuKit::DrawPlate(this, TraceMenuKit::Sprite(ETraceKitSprite::BtnHover), TraceMenuArtStyle::ButtonFrame,
		FieldX, FieldY, FieldW, FieldH, FieldH, FLinearColor::White))
	{
		TraceMenuKit::DrawFallbackPlate(this, ETraceKitState::Hover, FieldX, FieldY, FieldW, FieldH);
	}

	// The address in the settings pages' body face (Erbaum Bold): digits and dots have to be read
	// exactly, and it is the face that reads them best. White, shrunk to fit rather than clipped.
	const FString Typed = JoinEntry.GetText();
	const float FieldMid = FieldY + FieldH * 0.5f;
	const float TextX = FieldX + MJ::FieldTextPad * S;
	const float TextRoom = FieldW - 2.f * MJ::FieldTextPad * S;
	const FString Shown = Typed.IsEmpty()
		? TRACE_TEXTF("MENU.JOIN_FIELD_PLACEHOLDER", "100.101.102.103:{0}", { TraceNet::DefaultPort })
		: Typed;

	TraceText::FStyle FieldStyle = TraceMenuKit::CapStyle(MJ::FieldTextCap * S,
		Typed.IsEmpty() ? FLinearColor(1.f, 1.f, 1.f, 0.35f) : TraceMenuArtStyle::WordDefault, ETraceTextWeight::Hud);
	FieldStyle.HAlign = TraceText::EHAlign::Left;
	{
		const float Natural = TraceText::MeasureWidth(Shown, FieldStyle);
		if (Natural > TextRoom && Natural > 0.f)
		{
			FieldStyle.Size *= TextRoom / Natural;
		}
	}
	TraceMenuKit::DrawTextCapCentered(this, Shown, TextX, FieldMid, FieldStyle);

	// Caret: white, measured off the substring LEFT of it in the same style, so it sits on the
	// character it is editing however the field shrank.
	if (JoinEntry.IsCaretVisible(Now))
	{
		const float CaretCap = TraceText::CapHeight(FieldStyle.Size, FieldStyle.Weight);
		const float CaretX = TextX + (Typed.IsEmpty() ? 0.f : TraceText::MeasureWidth(Typed.Left(JoinEntry.GetCaret()), FieldStyle));
		DrawRect(TraceMenuArtStyle::WordDefault, CaretX + FMath::Max(1.f, 1.f * S), FieldMid - CaretCap * 0.8f,
			FMath::Max(2.f, 2.f * S), CaretCap * 1.6f);
	}

	// ---- Error, or the confirmation that replaces it, or the port note ---------------------------
	if (!JoinError.IsEmpty())
	{
		TraceMenuKit::DrawCapText(this, JoinError, CX, NoteMid, MJ::NoteCap * S, TraceMenuArtStyle::AmberLifted(),
			ETraceTextWeight::Hud, TraceText::EHAlign::Center, FieldW);
	}
	else if (JoinEntry.WasRecentlyPasted(Now))
	{
		TraceMenuKit::DrawCapText(this, TRACE_TEXT("MENU.JOIN_PASTED_NOTE", "PASTED FROM CLIPBOARD"), CX, NoteMid,
			MJ::NoteCap * S, TraceMenuArtStyle::WordHoverLifted(), ETraceTextWeight::Hud, TraceText::EHAlign::Center, FieldW);
	}
	else
	{
		TraceMenuKit::DrawCapText(this, TRACE_TEXTF("MENU.JOIN_PORT_NOTE", "PORT {0} IS ADDED FOR YOU IF YOU LEAVE IT OFF",
			{ TraceNet::DefaultPort }), CX, NoteMid, MJ::NoteCap * S, TraceMenuKit::CaptionInk, ETraceTextWeight::Hud,
			TraceText::EHAlign::Center, FieldW);
	}

	// ---- CONNECT and BACK: kit buttons the mouse can press ---------------------------------------
	//
	// Hover follows the pointer; a press held on a button draws it PRESSED; the release on the same
	// button fires it (MouseReleased). An emptied label (Ranen's "KEY =") draws no button and leaves
	// nothing to click.
	const float ButtonW = FMath::Min(MJ::ButtonW * S, (FieldW - MJ::ButtonSpacing * S) * 0.5f);
	const float ButtonsX = CX - (ButtonW * 2.f + MJ::ButtonSpacing * S) * 0.5f;
	const FString ButtonWords[MJ::ButtonCount] = { ConnectWord, BackWord };
	for (int32 Index = 0; Index < MJ::ButtonCount; ++Index)
	{
		const float BX = ButtonsX + Index * (ButtonW + MJ::ButtonSpacing * S);
		const FBox2D Rect(FVector2D(BX, ButtonsY), FVector2D(BX + ButtonW, ButtonsY + ButtonH));
		const bool bUnderPointer = bHasCursor && Rect.IsInside(LastCursorPos);
		const ETraceKitState State = TraceMenuKit::StateFor(/*bEnabled=*/true, bUnderPointer,
			bUnderPointer && JoinPressedButton == Index);
		JoinButtonRects[Index] = TraceMenuKit::DrawButton(this, State, BX, ButtonsY, ButtonW, ButtonH, ButtonWords[Index], Now)
			? Rect : FBox2D(ForceInit);
	}

	// ---- The legend: [ENTER] CONNECT  [ESC] BACK  [CTRL+V] PASTE, and the pad's line under it -----
	float LineY = LegendY;
	if (TraceMenuKit::KeyLegendWidth(Keys, ChipH) > 0.f)
	{
		TraceMenuKit::DrawKeyLegend(this, Keys, CX, LineY, ChipH, Now);
		LineY += MJ::LegendLineGap * S;
	}
	TraceMenuKit::DrawKeyLegend(this, PadKeys, CX, LineY, ChipH, Now);

	// Deliberately repeated here as well as on the title screen behind it. Somebody in this prompt is
	// mid-conversation with the person they are trying to reach, and "what's yours?" is the very next
	// question — having it on screen saves a round trip through Escape.
	TraceMenuKit::DrawCapText(this, TRACE_TEXTF("MENU.JOIN_THIS_MACHINE", "THIS MACHINE IS {0}", { TraceNet::GetHostEndpoint() }),
		CX, MachineMid, MJ::MachineCap * S, TraceMenuKit::CaptionInk, ETraceTextWeight::Light, TraceText::EHAlign::Center,
		PanelW - 80.f * S);

	// ---- The pointer, on top of everything the prompt drew ----------------------------------------
	//
	// Live, not frozen: DrawHUD keeps sampling it while the prompt is up. ShowCursor renews the lease
	// that hides the OS arrow, so there is exactly one pointer and it is the artist's.
	if (bHasCursor
		&& !TraceMenuKit::ShowCursor(this, GetOwningPlayerController(), TEXT("JOIN prompt"), LastCursorPos, UIScale))
	{
		TraceMenuHUDPointer::DrawCross(this, LastCursorPos, UIScale);
	}
}

void ATraceMenuHUD::DrawMenuRows()
{
	const int32 RowCount = static_cast<int32>(ETraceMenuRow::Count);

	const float CX = ViewW * 0.5f;
	const float Spacing = TraceMenuStyle::RowSpacing * UIScale;

	// The arithmetic lives in TraceMenuStyle::ComputeConsoleLayout (spec v17 §4) so that
	// GetCanvasRowRect (which the UMG verifier compares against) and this draw cannot disagree.
	//
	// NO CONSOLE PANEL behind the rows, on either renderer: every row is an opaque navy plate (the
	// kit's own fallback rectangle while a sprite is not drawable yet) on black, exactly as on the UMG
	// screen, whose ConsolePanel is a CLEAR border. The old cyan-edged console went with the cyan grid
	// it was there to hide; the panel rect is still what places the blurb.
	const TraceMenuStyle::FConsoleLayout Layout =
		TraceMenuStyle::ComputeConsoleLayout(ViewW, ViewH, UIScale, RowCount);
	const float RowW = Layout.RowW;
	const float PanelY = Layout.PanelY;
	const float PanelH = Layout.PanelH;

	const float FirstY = Layout.FirstRowY;
	for (int32 Index = 0; Index < RowCount; ++Index)
	{
		const ETraceMenuRow Row = static_cast<ETraceMenuRow>(Index);
		RowRects[Index] = DrawRow(Row, CX, FirstY + Index * Spacing, RowW, Row == Selected);
	}
	bRowRectsValid = true;

	// Remembered, not recomputed: DrawFooter() places the footer hint under this line rather than at
	// a fixed height, so that adding a menu row cannot slide the blurb onto it. See BlurbBottomY.
	// (D30 deleted the KEY LEGEND that used to be the first of the two footer lines; the hint under
	// it is still there and can still be collided with, so this measurement still earns its keep.)
	const FString Blurb = BuildBlurb();
	const float BlurbY = PanelY + PanelH - (36.f * UIScale);
	DrawTextCentered(Blurb, TraceMenuKit::CaptionInk, CX, BlurbY, FontSmall, 1.1f * UIScale);
	BlurbBottomY = BlurbY + MeasureHeight(Blurb, FontSmall, 1.1f * UIScale);
}

FString ATraceMenuHUD::BuildBlurb() const
{
	// One line of plain English under the rows, so "EASY" and "MODE B" mean something before you
	// commit to them. It describes whichever row is SELECTED, when that row has something worth
	// saying — the two multiplayer rows have the most: PLAY silently became "host a server", and a
	// player who is not told that will keep asking somebody else to host.
	switch (Selected)
	{
	case ETraceMenuRow::Play:
		// WP8.1 — points at the chip instead of repeating its number. The chip is the address's one
		// authoritative appearance on this screen.
		return TRACE_TEXT("MENU.BLURB_PLAY", "HOSTS A GAME ON YOUR ADDRESS ABOVE.  OTHERS PICK JOIN AND TYPE IT.");

	case ETraceMenuRow::Join:
		return LastJoinAddress.IsEmpty()
			? FString(TRACE_TEXT("MENU.BLURB_JOIN_NO_ADDRESS", "CONNECT TO SOMEBODY ELSE'S GAME.  YOU WILL NEED THEIR ADDRESS."))
			: TRACE_TEXTF("MENU.BLURB_JOIN_REMEMBERED",
				"CONNECT TO SOMEBODY ELSE'S GAME.  ENTER RECONNECTS TO {0}.", { LastJoinAddress });

	// PRACTICE, SETTINGS AND QUIT SAY NOTHING.
	//
	// QUIT (D30): "CLOSE TRACE." was the "close trace" in the owner's "remove the text at the bottom:
	// close trace, w/s...", the one blurb that only said its own row's label back. PRACTICE's and
	// SETTINGS' sentences were removed by the co-developer's text pass (SETTINGS' was out of date as
	// well: that page has long since had video, crosshair, loadouts, audio and controller doors).
	//
	// EMPTY, not a deleted case: both renderers place the footer under the blurb's MEASURED bottom,
	// and both measure an empty string as one line box (ATraceMenuHUD::MeasureHeight ignores the text
	// outright; TraceText::Measure splits "" into one empty line). Returning nothing therefore leaves
	// the footer exactly where it is instead of letting it jump a line as the selection moves.
	//
	// A plain empty string rather than a lookup with an empty default: those keys would only ever
	// hold nothing, and the co-developer deleted their lines, so they are not registered at all — a
	// dump will not write empty BLURB_* lines back into his file.
	case ETraceMenuRow::Practice:
	case ETraceMenuRow::Settings:
	case ETraceMenuRow::Quit:
		return FString();

	default:
		return TraceDifficulty::ToBlurb(Difficulty);
	}
}

void ATraceMenuHUD::BuildRowView(ETraceMenuRow Row, bool bSelected, FTraceMenuRowView& OutView) const
{
	OutView = FTraceMenuRowView();
	OutView.bSelected = bSelected;

	// Spec v19 §5: the artist's sheet has a PRESSED-looking hover state and a DISABLED state, so both
	// are now wired to the things that were already true and simply never drawn.
	//
	// PRESSED is the row a mouse-down armed and has not released — the same PressedRow the
	// press-arms/release-fires contract of spec v15 §4 has always kept.
	//
	// DISABLED is the two moments this screen genuinely refuses a press: the 0.35 s grace period after
	// the title appears (AcceptsActivation swallows an Enter that early — see AcceptUnlockTime) and
	// after PLAY or JOIN has been taken and the level is loading. Both used to look completely live
	// while doing nothing, which is the worst thing a button can do.
	//
	// NOT THE GRACE PERIOD ANY MORE. It used to be `!bTravelling && AcceptsActivation()`, so for the
	// first 0.35 s of every title screen (every launch, every return from a match) all six rows drew the
	// near-black DISABLED plate and then popped to navy: the whole menu blinked grey to blue. The grace
	// period still swallows an early Enter (ActivateSelection) — it just is not DRAWN, because it is a
	// guard against a stray key, not a state the player needs to see.
	OutView.bPressed = (PressedRow == static_cast<int32>(Row));
	OutView.bEnabled = !bTravelling;

	switch (Row)
	{
	case ETraceMenuRow::Play:       OutView.Label = TRACE_TEXT("MENU.ROW_PLAY", "PLAY");         break;
	case ETraceMenuRow::Join:       OutView.Label = TRACE_TEXT("MENU.ROW_JOIN", "JOIN");         break;
	case ETraceMenuRow::Practice:   OutView.Label = TRACE_TEXT("MENU.ROW_PRACTICE", "PRACTICE");     break;
	case ETraceMenuRow::Difficulty: OutView.Label = TRACE_TEXT("MENU.ROW_DIFFICULTY", "DIFFICULTY");   break;
	case ETraceMenuRow::Settings:   OutView.Label = TRACE_TEXT("MENU.ROW_SETTINGS", "SETTINGS");     break;
	case ETraceMenuRow::Quit:       OutView.Label = TRACE_TEXT("MENU.ROW_QUIT", "QUIT");         break;
	default:                        OutView.Label = TEXT("");             break;
	}

	// The JOIN row carries a right-aligned status word instead of a stepper. Small font, not
	// the row font: an IPv4 address plus a port is twenty characters, and at the label's size it
	// would collide with "JOIN" on a 1280-wide window. It is a readout, not a value to change.
	//
	// PLAY carried a "HOST <address>" readout too, until WP8.1: that was the address's third
	// appearance on one screen, one row under the chip that is its single authoritative source. The
	// row label and the blurb carry the hosting fact; the chip carries the number. JOIN's remembered
	// target stays — a reconnect affordance, not a repetition.
	if (Row == ETraceMenuRow::Join)
	{
		OutView.Status = LastJoinAddress.IsEmpty() ? FString(TRACE_TEXT("MENU.ROW_JOIN_STATUS_EMPTY", "ENTER AN ADDRESS")) : LastJoinAddress;
	}

	// DIFFICULTY is the VALUE row: right-aligned value, arrows either side, dimmed at the ends of
	// the range. It shared this block with SCORING MODE until the endzone ruleset was removed.
	if (Row == ETraceMenuRow::Difficulty)
	{
		OutView.Value = TraceDifficulty::ToDisplayName(Difficulty);

		// The row's own word colour, never a colour of its own: white, and the hover olive on the
		// selected row, exactly as the label beside it (the sheet sets the number in its value box in
		// that olive). It was the pre-kit cyan; the kit has no cyan in it. The WORDS carry the setting.
		OutView.ValueColor = bSelected ? TraceMenuArtStyle::WordHoverLifted() : TraceMenuArtStyle::WordDefault;

		OutView.bShowArrows = true;
		OutView.bCanLeft = (Difficulty != ETraceBotDifficulty::Easy);
		OutView.bCanRight = (Difficulty != ETraceBotDifficulty::Hard);
	}
}

FBox2D ATraceMenuHUD::DrawRow(ETraceMenuRow Row, float CenterX, float Y, float Width, bool bSelected)
{
	const float RowH = TraceMenuStyle::RowHeight * UIScale;
	const float X = CenterX - Width * 0.5f;
	const float PadX = TraceMenuStyle::RowPadX * UIScale;

	// WHAT the row says is decided in exactly one place, BuildRowView, because the UMG renderer says
	// the same things from the same call (spec v17 §4). This function is purely HOW it looks on a
	// Canvas.
	FTraceMenuRowView RowView;
	BuildRowView(Row, bSelected, RowView);

	// ---- The artist's plate, 9-sliced by the kit ------------------------------------------------------
	//
	// The selected row wears the HOVER plate (ring and all) exactly as the UMG row does — selection and
	// hover are one state on this screen. Plate and word come from the kit's one state switch (the same
	// one the UMG row uses), so the two renderers cannot pick different plates or words for the same
	// row. A flat white tint: this renderer has never breathed the selected plate (P03's decision).
	// The kit's own fallback rectangle stands in while a texture is not drawable yet.
	const ETraceKitState RowState = TraceMenuKit::StateFor(/*bEnabled=*/true, bSelected);
	const FTraceKitVisuals RowVisuals = TraceMenuKit::VisualsFor(RowState);
	if (!TraceMenuKit::DrawPlate(this, TraceMenuKit::Sprite(RowVisuals.Plate), TraceMenuArtStyle::ButtonFrame,
		X, Y, Width, RowH, RowH, FLinearColor::White))
	{
		TraceMenuKit::DrawFallbackPlate(this, RowState, X, Y, Width, RowH);
	}

	if (bSelected)
	{
		// THE TITLE'S AMBER SELECTION RAIL — owner-requested, title only — from the ONE definition the
		// UMG row uses (TraceMenuStyle::SelectionRail*): two thirds of the row tall, 0.15 of it wide,
		// 14 px outside the plate, breathing on the kit's hover pulse. It used to be a 6x60 bar 24 px
		// out on a different pulse, so the same selection looked different on the two renderers.
		const float RailW = RowH * TraceMenuStyle::SelectionRailWidthOfRow;
		const float RailH = RowH * TraceMenuStyle::SelectionRailHeightOfRow;
		const float RailX = X - TraceMenuStyle::SelectionRailGap * UIScale - RailW;
		const float Breath = TraceMenuKit::HoverPulse(Now);
		const FLinearColor RailAmber = TraceMenuArtStyle::AmberLifted();
		DrawRect(FLinearColor(RailAmber.R * Breath, RailAmber.G * Breath, RailAmber.B * Breath, 1.f),
			RailX, Y + (RowH - RailH) * 0.5f, RailW, RailH);
	}

	// The word colour, from the same switch as the plate above: the hover olive on the selected row,
	// the sheet's white on every other.
	const float LabelScale = 1.55f * UIScale;
	const float LabelY = Y + (RowH - MeasureHeight(RowView.Label, FontMedium, LabelScale)) * 0.5f;
	TraceMenuHUDType::Draw(this, RowView.Label, RowVisuals.Label, X + PadX, LabelY, FontMedium, LabelScale);

	// The row's own FURNITURE (the JOIN readout, the DIFFICULTY value's arrows) in the kit's furniture
	// colour, as on the UMG row — white on the selected row, 0.85 white otherwise. Never cyan.
	const FLinearColor RowFurniture = RowVisuals.Furniture;

	if (!RowView.Status.IsEmpty())
	{
		const float ValueScale = 1.05f * UIScale;
		const FLinearColor StatusColor(RowFurniture.R, RowFurniture.G, RowFurniture.B, bSelected ? 0.88f : 0.70f);

		const float StatusW = MeasureWidth(RowView.Status, FontSmall, ValueScale);
		const float StatusY = Y + (RowH - MeasureHeight(RowView.Status, FontSmall, ValueScale)) * 0.5f;
		TraceMenuHUDType::Draw(this, RowView.Status, StatusColor, X + Width - PadX - StatusW, StatusY, FontSmall, ValueScale);
	}

	// ---- The value, its arrows, and the artist's value box behind them --------------------------------
	//
	// As the UMG row lays it out: the value box (T_MenuValueBox, gold-edged) sits 8 px in from the
	// plate's right end, 34 px tall, holding "<  VALUE  >" with 16 px of padding and 14 px between the
	// arrows and the value.
	if (!RowView.Value.IsEmpty())
	{
		const float ArrowS = 8.f * UIScale;
		const float ArrowT = FMath::Max(1.f, 2.f * UIScale);
		const float ArrowGap = 14.f * UIScale;
		const float BoxPad = 16.f * UIScale;
		const float BoxH = 34.f * UIScale;

		const float ValueW = MeasureWidth(RowView.Value, FontMedium, LabelScale);
		const float BoxRight = X + Width - 8.f * UIScale;
		const float RightArrowX = BoxRight - BoxPad - ArrowS;
		const float ValueRight = RightArrowX - ArrowGap;
		const float LeftArrowX = ValueRight - ValueW - ArrowGap - ArrowS;
		const float BoxLeft = LeftArrowX - BoxPad;

		TraceMenuKit::DrawValueBoxPlate(this, BoxLeft, Y + (RowH - BoxH) * 0.5f, BoxRight - BoxLeft, BoxH);

		const float ValueY = Y + (RowH - MeasureHeight(RowView.Value, FontMedium, LabelScale)) * 0.5f;
		TraceMenuHUDType::Draw(this, RowView.Value, RowView.ValueColor, ValueRight - ValueW, ValueY, FontMedium, LabelScale);

		if (RowView.bShowArrows)
		{
			// Dimmed at the ends of the range so the player can see there is nothing further that way.
			const float ArrowY = Y + RowH * 0.5f;
			const FLinearColor LeftInk(RowFurniture.R, RowFurniture.G, RowFurniture.B, RowView.bCanLeft ? 0.95f : 0.20f);
			const FLinearColor RightInk(RowFurniture.R, RowFurniture.G, RowFurniture.B, RowView.bCanRight ? 0.95f : 0.20f);

			DrawLine(LeftArrowX, ArrowY, LeftArrowX + ArrowS, ArrowY - ArrowS, LeftInk, ArrowT);
			DrawLine(LeftArrowX, ArrowY, LeftArrowX + ArrowS, ArrowY + ArrowS, LeftInk, ArrowT);
			DrawLine(RightArrowX, ArrowY - ArrowS, RightArrowX + ArrowS, ArrowY, RightInk, ArrowT);
			DrawLine(RightArrowX, ArrowY + ArrowS, RightArrowX + ArrowS, ArrowY, RightInk, ArrowT);
		}
	}

	return FBox2D(FVector2D(X, Y), FVector2D(X + Width, Y + RowH));
}

void ATraceMenuHUD::DrawFooter()
{
	const float CX = ViewW * 0.5f;

	// D30 — THE KEYBOARD KEY LEGEND IS GONE. The line that used to sit on the first baseline
	// ("W / S OR ARROWS MOVE ... ESC QUIT") was removed at the owner's request, and the hint line
	// under it has since been removed too (see the end of this function). The MATHS below is
	// deliberately untouched — Y is still that first line's position — because the UMG twin
	// (UTraceTitleMenuWidget::PlaceFooterBelowBlurb) lays its footer out at exactly KeysY, and the two
	// renderers have to keep landing in the same place. Emptying a string on one side and moving the
	// line on the other is how they would drift apart.
	//
	// D32-PADMENU — THE FOOTER IS THEREFORE ONE LINE OR TWO, AND THE SECOND ONE IS THE PAD'S. Y is no
	// longer a vacant baseline: on a machine where a controller has been seen it carries D-PAD / A / B,
	// and the dark band below grows up to meet it. See ShouldShowPadHints for why that is not the
	// deleted legend coming back, and BuildMenuView for the identical decision on the UMG side.
	//
	// The line has to clear the bezel's bottom rail, which sits 3.8% of the height up from the edge;
	// anchoring off the bottom in reference pixels alone puts it under the rail.
	float Y = ViewH - (100.f * UIScale) - (ViewH * 0.02f);

	// ---- ...AND it has to clear the CONSOLE BLURB (spec v20 §0.4) --------------------------------
	//
	// The line above is anchored to the viewport bottom; the blurb is anchored to the bottom of the
	// rows panel, which grows with the row count. At seven rows on a 1080-high screen they met, and
	// the screen printed "HOSTS A GAME ON <addr>. OTHERS PICK JOIN AND TYPE THAT." straight through
	// "W/S OR ARROWS MOVE ... ESC QUIT" on one baseline. That is the defect the spec lists as "the
	// footer text is drawn twice, overlapping itself". Deleting the key legend removes one of the two
	// strings that could collide, but NOT the collision: the blurb can still reach the hint, so the
	// measurement stays. It is also still reachable here because this Canvas renderer is the fallback
	// whenever the widget asset is missing or -TraceNoMenuUMG is set.
	//
	// Taking the MEASURED bottom of the blurb (DrawMenuRows ran earlier this frame and recorded it)
	// rather than a second hard-coded constant is what stops another row from recreating it. The
	// FMath::Max keeps the old position whenever there is already room, so nothing moves on the
	// layouts that were fine — only the crowded ones change.
	// 26, not 8: the dark strip below is opaque and starts 22px above whatever the topmost footer
	// line is. Clearing only the text would tuck the blurb under the strip instead of under the
	// words - the same line lost, by a different mechanism. 22 puts the strip's top edge on the
	// blurb's last row of pixels; the extra 4 is the visible gap.
	const float MinGap = 26.f * UIScale;
	if (BlurbBottomY > 0.f)
	{
		Y = FMath::Max(Y, BlurbBottomY + MinGap);
	}

	// Never off the bottom edge. The 46px floor was sized for the hint line that sat 24px under Y and
	// is kept so the pad legend does not move; if the panel is so tall that even this cannot fit, the
	// footer wins and the blurb is what gets overlapped.
	Y = FMath::Min(Y, ViewH - (46.f * UIScale));

	// D32-PADMENU — the pad legend goes on the baseline the deleted keyboard legend used to hold (Y),
	// which is exactly the slot the UMG twin keeps for it (BuildMenuView's FooterKeys). Drawn only
	// once a controller has been seen; see ShouldShowPadHints. The grid runs all the way to the
	// bottom edge, so it gets its own dark strip, hugging the line by 22px — legibility beats
	// atmosphere every time.
	//
	// THE HINT LINE THAT USED TO SIT 24px UNDER Y IS GONE: the co-developer's text pass removed "PLAY
	// ALSO HOSTS - EVERY MATCH IS JOINABLE", which the PLAY blurb already says. So the strip now exists
	// only for the pad legend — a dark band with nothing in it would be exactly the empty chrome a
	// removed line must not leave behind — and on a keyboard-only machine this footer draws nothing,
	// like the UMG twin, which never had a band.
	const FString& PadLegend = TRACE_TEXT("MENU.FOOTER_PAD_KEYS", "D-PAD   MOVE          A   SELECT          B   BACK");
	const bool bPadLegend = ShouldShowPadHints() && !PadLegend.IsEmpty();
	if (!bPadLegend)
	{
		return;
	}

	// NO BAND BEHIND IT ANY MORE. The dark strip and its cyan rule existed to lift the line off the
	// cyan grid floor; the grid is gone (the backdrop is the kit's black), so the line sits on black
	// like the UMG twin's.
	DrawTextCentered(PadLegend, TraceMenuStyle::WithAlpha(TraceMenuKit::CaptionInk, 0.75f), CX, Y, FontSmall, 1.f * UIScale);
}

void ATraceMenuHUD::DrawVersionString()
{
	// WP8.2. Bottom-right, small and dim — a build identifier for screenshots and bug reports, and
	// (the NET half) the one thing two players must compare before they try to connect. Drawn after
	// the footer strip so it sits ON it, and before the modals' scrims/panels in this frame's order,
	// so a modal dims it proportionally rather than losing it.
	const FString& Label = TraceMenuHUDFile::ProjectVersionLabel();
	if (Label.IsEmpty())
	{
		return;
	}
	TraceMenuHUDType::Draw(this, Label,
		TraceMenuStyle::WithAlpha(TraceMenuKit::CaptionInk, 0.55f),
		ViewW - (24.f * UIScale), ViewH - (28.f * UIScale),
		FontSmall, 0.9f * UIScale, TraceText::EHAlign::Right);
}

void ATraceMenuHUD::DrawCursor()
{
	// The OS cursor is drawn by the platform and is invisible in captures, so the menu draws its
	// own. It is also simply nicer: a system arrow on this screen would look like a mistake.
	if (!bHasCursor || bTravelling)
	{
		return;
	}

	// ---- ONE POINTER, ONE OWNER (spec v20 integration) -------------------------------------------
	//
	// While the settings overlay is up, FTraceOptionsMenu draws the ARTIST'S arrow at the live mouse
	// position — and this function would draw a SECOND pointer, the old cyan cross, at a position
	// that is deliberately frozen (the sample above is skipped while the overlay is open, so that
	// closing it does not re-select whichever title row the pointer happened to be over). Two
	// pointers, one of them stale and lying about where the mouse is.
	//
	// This was invisible until spec v20 because both marks were the same cyan cross, and it is STILL
	// invisible in most captures because OptionsMenu.Tick() paints its 880x972 panel into this same
	// Canvas AFTER this call and covers the stale mark. It shows wherever the frozen point falls
	// outside that panel — roughly half the screen at 1920x1080 — which is exactly the case where a
	// player leaves the pointer at a screen edge and opens SETTINGS from the keyboard.
	//
	// Red arm: `Trace.Menu.CursorRedArm 1` restores the old unconditional draw, so one binary
	// produces both arms and the guard below can be shown to be load-bearing rather than asserted.
	if (OptionsMenu.IsOpen() && TraceMenuHUDFile::GCursorRedArm == 0)
	{
		return;
	}

	// The JOIN prompt draws the pointer itself, AFTER its panel, so it sits on top of the prompt rather
	// than under its scrim. Drawing it here too would be a second, dimmed pointer.
	if (IsJoinPromptOpen())
	{
		return;
	}

	// ---- HIDE THE OS POINTER WHILE WE DRAW OUR OWN (spec v24 §2, integration) --------------------
	//
	// Past this point this function IS drawing a pointer, so the hardware arrow must go, exactly as it
	// does on the UMG title screen, the settings modal and character select. This is the Canvas
	// fallback path: Trace.UI.UseUMG 0, a missing WBP asset, or a modal that declined to elevate over
	// Slate. UTraceTitleMenuWidget::ApplyView never runs there, so nothing else renews the lease and
	// the player got the system arrow on top of ours.
	//
	// Placed AFTER both early-outs on purpose. The lease is a statement that a pointer is being
	// drawn THIS FRAME, so it must not be renewed on the frames this function returns without
	// drawing one (travelling, no cursor, or the options overlay owning the screen — that last case
	// renews through FTraceOptionsMenu's own surface instead). The lease expires two frames after
	// the last renewal, so leaving this screen hands the arrow back with no exit path to forget.
	TraceHardwareCursor::EnsureRunning();
	TraceHardwareCursor::RenewSuppression(GetOwningPlayerController(), TEXT("title screen (Canvas)"));

	// ---- ONE SCREEN, ONE POINTER, ON BOTH RENDERERS (UI QA finding 6) ---------------------------
	//
	// The artist's white blade, through TraceHardwareCursor::DrawPointer — the same pointer the UMG
	// title, the options pages and the select screens draw. The small white cross is the FALLBACK for
	// a build with no menu art and for the frame or two before the sprite's RHI texture lands: a title
	// screen with no pointer at all is unusable with a mouse.
	if (TraceHardwareCursor::DrawPointer(this, LastCursorPos, UIScale))
	{
		return;
	}
	TraceMenuHUDPointer::DrawCross(this, LastCursorPos, UIScale);
}

void ATraceMenuHUD::DrawTravelOverlay()
{
	TravelCancelRect = FBox2D(ForceInit);
	if (!bTravelling)
	{
		return;
	}

	namespace MT = TraceMenuHUDTravel;
	const float S = UIScale;
	const float CX = ViewW * 0.5f;

	// ---- ONE BLACK CARD ---------------------------------------------------------------------------
	//
	// It was a 72 % scrim with a second, smaller TRACE over the tagline — the six rows still readable
	// underneath and looking pressable — and on this renderer a stroke-vector cyan TRACE. Now: opaque
	// black (the kit), the artist's wordmark and swoosh in exactly the place the title had them, so the
	// mark does not jump when the card comes up, and the caption in white.
	TraceMenuKit::DrawBackground(this, ViewW, ViewH);
	if (DrawTitleBlock() < 0.f)
	{
		const float CapHeight = ViewH * 0.10f;
		DrawStrokeTextCentered(TRACE_TEXT("MENU.WORDMARK", "TRACE"), TraceMenuArtStyle::WordDefault,
			CX, ViewH * 0.20f, CapHeight, FMath::Max(2.f, CapHeight * 0.055f));
	}

	// The caption names what is actually happening — "HOSTING ON 100.x.y.z:7777" or "CONNECTING TO
	// <addr>" — rather than one generic line for two very different operations. A join that hangs for
	// fifteen seconds and then fails needs the player to have seen the address it was dialling.
	const FString Caption = TravelCaption.IsEmpty() ? FString(TRACE_TEXT("MENU.TRAVEL_ENTERING_ARENA", "ENTERING THE ARENA")) : TravelCaption;
	const float CaptionMid = ViewH * MT::CaptionY;
	TraceMenuKit::DrawCapText(this, Caption, CX, CaptionMid, MT::CaptionCap * S, TraceMenuArtStyle::WordDefault,
		ETraceTextWeight::Light, TraceText::EHAlign::Center, ViewW - 160.f * S);

	// ---- ONLY A JOIN WAITS, AND ONLY A JOIN CAN BE CALLED OFF --------------------------------------
	//
	// Decided by the travel KIND (IsJoinInFlight), never by the caption's wording — the caption is
	// Ranen's editable MENU.TRAVEL_CONNECTING_TO, and the old `StartsWith("CONNECTING")` test would have
	// silently dropped the hint the day it was reworded. A local PLAY / PRACTICE load is gone within a
	// frame and gets neither line.
	//
	// Elapsed seconds instead of "THIS CAN TAKE A FEW SECONDS": a join to a dead address sits here for
	// the whole connect timeout, and a counter that keeps moving is what tells the player the game has
	// not hung. And the way out, which did not exist: [ESC] CANCEL (and [B] CANCEL once a pad has been
	// seen). The legend is clickable too.
	if (!IsJoinInFlight())
	{
		return;
	}

	const float ElapsedMid = CaptionMid + MT::ElapsedGap * S;
	TraceMenuKit::DrawCapText(this, MT::ElapsedText(this, TravelStartRealTime), CX, ElapsedMid, MT::ElapsedCap * S,
		TraceMenuKit::CaptionInk, ETraceTextWeight::Light, TraceText::EHAlign::Center);

	const FString CancelWord = TRACE_TEXT("OPTIONS.LEGEND.CANCEL", "CANCEL");
	TArray<FTraceKitLegendItem> Keys;
	Keys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_BACK", "ESC"), CancelWord });
	TArray<FTraceKitLegendItem> PadKeys;
	if (ShouldShowPadHints())
	{
		PadKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.PAD_KEY_BACK", "B"), CancelWord });
	}

	const float ChipH = MT::LegendChipH * S;
	const float KeysW = TraceMenuKit::KeyLegendWidth(Keys, ChipH);
	const float PadW = TraceMenuKit::KeyLegendWidth(PadKeys, ChipH);
	float LineY = ElapsedMid + MT::LegendGap * S;
	const float LegendTop = LineY;
	if (KeysW > 0.f)
	{
		TraceMenuKit::DrawKeyLegend(this, Keys, CX, LineY, ChipH, Now);
		LineY += MT::LegendLineGap * S;
	}
	if (PadW > 0.f)
	{
		TraceMenuKit::DrawKeyLegend(this, PadKeys, CX, LineY, ChipH, Now);
		LineY += MT::LegendLineGap * S;
	}

	// The legend is the card's one control: a click on it cancels (MousePressed / MouseReleased).
	const float LegendW = FMath::Max(KeysW, PadW);
	if (LegendW > 0.f)
	{
		const float Slop = 8.f * S;
		TravelCancelRect = FBox2D(FVector2D(CX - LegendW * 0.5f - Slop, LegendTop - Slop),
			FVector2D(CX + LegendW * 0.5f + Slop, LineY - MT::LegendLineGap * S + ChipH + Slop));
	}

	if (bHasCursor
		&& !TraceMenuKit::ShowCursor(this, GetOwningPlayerController(), TEXT("connecting card"), LastCursorPos, UIScale))
	{
		TraceMenuHUDPointer::DrawCross(this, LastCursorPos, UIScale);
	}
}

// =================================================================================================
// Helpers
// =================================================================================================

// All three go through UI/Text now — spec v22 §A1. The (UFont*, Scale) signatures are this file's
// layout vocabulary and are kept; TraceMenuHUDType::SizeFor translates them.

void ATraceMenuHUD::DrawTextCentered(const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale)
{
	TraceMenuHUDType::Draw(this, Text, Color, CenterX, Y, Font, Scale, TraceText::EHAlign::Center);
}

float ATraceMenuHUD::MeasureWidth(const FString& Text, UFont* Font, float Scale)
{
	return TraceText::MeasureWidth(Text, TraceMenuHUDType::SizeFor(this, Font, Scale));
}

float ATraceMenuHUD::MeasureHeight(const FString& Text, UFont* Font, float Scale)
{
	// The LINE BOX, independent of @p Text, so a row's height cannot change with its contents.
	(void)Text;
	return TraceText::LineHeight(TraceMenuHUDType::SizeFor(this, Font, Scale));
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
