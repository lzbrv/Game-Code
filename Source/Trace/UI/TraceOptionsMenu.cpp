// Trace — settings overlay implementation. See TraceOptionsMenu.h.

#include "UI/TraceOptionsMenu.h"

#include "UI/TraceAbilityNames.h"   // the loadout page prints ability names

#include "Camera/CameraComponent.h"
#include "Containers/Ticker.h"          // FTSTicker - defer the viewport resize out of DrawHUD
#include "DynamicRHI.h"                  // RHIGetGPUFrameCycles
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"  // spec v28 §3a - the harness injects where FSceneViewport does
#include "InputKeyEventArgs.h"           // spec v28 §3a - a real FInputKeyEventArgs, not a call into the menu
#include "Framework/Application/SlateApplication.h"  // spec v28 §3a - inject ABOVE the viewport gate
#include "Widgets/SViewport.h"           // spec v28 §3a - the widget the synthetic click is aimed at
#include "UnrealClient.h"                // FViewport::GetMouseCaptureMode, the gate being measured
#include "Engine/Texture2D.h"            // the artist's sprites - see the art block below
#include "TextureResource.h"             // FTextureResource::TextureRHI - see LogReadiness
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"         // Trace.Menu.Settings / Trace.Menu.Video, the capture hooks
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"            // -TraceMenuActivate, the paused-world capture hook
#include "Misc/Parse.h"
#include "Scalability.h"
#include "Settings/TraceGameUserSettings.h"
#include "Settings/TraceGamepadInput.h"      // TracePadMenu::HasSeenPad — the legend's pad line
#include "Trace.h"                       // LogTraceGame
#include "UI/TraceMatchOptions.h"        // TraceCharacters - the spec v14 §3 toggle's storage
#include "UI/Text/TraceCanvasText.h" // spec v22 §A1 - this page types in the artist's face
#include "UI/Text/TraceGameText.h"   // TRACE_TEXT / TRACE_TEXTF - the owner's editable wording
#include "Audio/TraceAudio.h"           // spec v26 §9 - ButtonPress on the submenu rows too
#include "Audio/TraceMusicPlayer.h"     // UI plan WP3 - RefreshVolume, so a MUSIC drag is heard live
#include "GameFramework/PlayerState.h"  // UI plan WP2.4 - the name the submit path is replacing
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"  // WP11.1 - AmberLifted() for the slider thumb
#include "UI/Widgets/Menu/TraceMenuKit.h"       // the shared, guarded kit renderer: sprites, plates, trough
#include "UI/TraceHardwareCursor.h"        // UI QA finding 6 - one pointer, drawn in one place
#include "Gameplay/TraceMelee.h"       // kept for the transitive gameplay types; the v28 §10 row-label override it fed is deleted (v29 §5)

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
// TYPE — every string on this page goes through UI/Text (spec v22 §A1): TraceCanvasText blits one
// atlas quad per glyph in the artist's faces. With no atlas it types Lato at the same size and
// TraceText measures Lato, so the page degrades to "the wrong face, laid out correctly". Sizes are
// CAP HEIGHTS in the face being drawn (see TraceOptionsMenuText below), the kit's rule.
// =================================================================================================

// =================================================================================================
// SPEC v25 §1 — THE FOREGROUND-CANVAS ELEVATION IS REMOVED.
//
// `FTraceOverSlateCanvas`, `TraceOptionsMenuOverSlate`, `Trace.UI.ModalOverSlate` and
// `Trace.UI.ModalDPIGuard` all lived here. The whole argument — what the elevation was, what it
// crashed, the two arms that proved it, and what removing it costs — is written out in the header,
// above FTraceOptionsMenu. It is not repeated here because there is no longer any code here for it
// to explain: the panel draws on the canvas the host handed it, the way it did before spec v23 §A2.
// =================================================================================================

// =================================================================================================
// SPEC v26 §2 — THIS PAGE IS *TWO* TYPEFACES NOW, AND WHICH ONE IS A PROPERTY OF THE ROW
// =================================================================================================
//
//     "Make submenus (e.g. settings) use Erbaum bold rather than Sofachrome. Keep sofachrome for
//      main menu, headers, character names"
//
// v22 §A1 (the block above) made this page ONE typeface after it had shipped with two. That was the
// right fix for the defect it was aimed at — the artist's baked word sprites sitting four pixels from
// engine-font rows — and the owner is now asking for a different split, along a different seam:
//
//     the page's own TITLE and its section HEADERS  ->  Sofachrome     (ETraceTextWeight::Light)
//     everything a player reads, adjusts or binds   ->  Erbaum Bold    (ETraceTextWeight::Hud)
//
// The seam runs between OBJECTS, never inside one. A header owns its whole row and the rule beside
// it; a body row owns its label, its value, its arrows and its key chip. Nothing on this page mixes
// the two faces within a string or within a control, which is the thing that made the OLD two-face
// screen read as broken — SETTINGS and PLAY as baked sprites with their neighbours in Lato, one row
// apart in the same column.
//
// There is exactly ONE line where both faces appear, and it is deliberate: the VIDEO page's title
// line carries the word VIDEO (a header, centred, Sofachrome) and the live frame-time readout (body,
// right-aligned, Erbaum). Photographed at 1920x1080 — they sit at opposite ends of a 1020-wide panel
// and read as the two different things they are, a heading and an instrument.
//
// ---- WHAT WAS DECIDED PER SURFACE, AND WHY ------------------------------------------------------
//
//   SETTINGS / PAUSED / VIDEO (the panel title)   SOFACHROME. The spec calls this out by name: "the
//                                                 word SETTINGS at the top of the settings page stays
//                                                 Sofachrome while the rows under it become Erbaum".
//   CONTROLS / DISPLAY / MOUSE (section captions) SOFACHROME. Same object as the title, one level
//                                                 down — a caption with a rule through it, not a
//                                                 control. It is what "HEADERS anywhere" means.
//   KEYBIND / KEY (the two column captions)       SOFACHROME. The KEYBOARD page's first row, over
//                                                 the two columns every Binding row below is laid out
//                                                 in.
//   Row labels, values, key names, ON/OFF, the    ERBAUM BOLD. "settings / submenu body text, keybind
//   < > arrows, the resolution-scale note, the    rows, values" — this is the body of the page, and
//   footer key hints, the video perf readout      the arrows and the readout are furniture attached
//                                                 to it rather than headings of their own.
//   Action rows on the SETTINGS and VIDEO pages   ERBAUM BOLD. BACK, RESET DEFAULTS and AUTO-DETECT
//                                                 are controls on a submenu page, in the same column
//                                                 as the rows they sit under.
//   Action rows on the PAUSE ROOT page            SOFACHROME. *** THE ONE JUDGEMENT CALL. *** RESUME
//                                                 / SETTINGS / VIDEO / RETURN TO TITLE / QUIT are not
//                                                 settings; they are the in-match MAIN MENU, the same
//                                                 list of destinations the title screen draws in
//                                                 Sofachrome through UTraceMenuRow. Setting them in
//                                                 Erbaum would put the game's two top-level menus in
//                                                 two different faces. The rule that produces this is
//                                                 one line — FaceForAction() below — so an owner who
//                                                 disagrees changes it there and nowhere else.
//
// ---- WIDTHS DIFFER BY FACE --------------------------------------------------------------------------
//
// Erbaum measures the alphabet at 1823 px against Sofachrome's 2634 at em 96, so anything that
// measures a string MUST be handed the face it will be drawn in. TraceOptionsMenuText's measure and
// draw both take the face, and every call states it.
// =================================================================================================

namespace TraceOptionsMenuType
{
	/**
	 * The face a HEADER is set in: Sofachrome ExtraLight, the same sheet the title screen uses — the
	 * page title, the section captions, the KEYBIND / KEY column captions and the pause root's rows.
	 */
	static constexpr ETraceTextWeight HeaderFace = ETraceTextWeight::Light;

	/** The face BODY is set in: Erbaum Bold, the face the in-match HUD already uses (spec v26 §2). */
	static constexpr ETraceTextWeight BodyFace = ETraceTextWeight::Hud;
}

// =================================================================================================
// THE HANDMADE KIT, ON THIS SCREEN
//
// Every page of this overlay is built from the artist's kit through UI/Widgets/Menu/TraceMenuKit.h,
// the one Canvas renderer every other menu screen uses:
//
//   * a black scrim over whatever is behind (the match, or the title), and a borderless black panel
//     under the list — no cyan bezel, no corner ticks, no cyan anything (stylespec §0: cyan is the
//     pre-kit palette and is not a kit colour);
//   * the page title in white Sofachrome; section captions in white at half strength with a faint
//     hairline;
//   * every row is the artist's button plate in one of the kit's four states — DEFAULT navy with a
//     white word, HOVER (keyboard selection too) with the baked amber ring and the lifted olive word,
//     PRESSED while the mouse is down on it, DISABLED near-black with a grey ring;
//   * a value row is a label plate (the sheet's wide KEYBIND plate) with its control beside it: KEY
//     chips for a binding, the gold-edged value box for a choice or a toggle (with the '<' '>' that
//     step it), and for a slider the sheet's navy trough, its white blade handle and the value box;
//   * the footer is the kit's KEY legend — a [KEY] chip and its verb — the same one the loadout and
//     team pages draw;
//   * the pointer is the kit's white blade, drawn through TraceMenuKit::ShowCursor.
//
// The title screen's white plate outline and amber selection rail are owner-requested extras for the
// TITLE only and are deliberately not drawn here (TraceMenuKit::VisualsFor leaves them out too).
// =================================================================================================

/** Layout, in 1080p reference pixels: multiply by UIScale. */
namespace TraceOptionsMenuLayout
{
	static constexpr float TitleCap       = 30.f;    // the page title's cap height
	static constexpr float TitleTop       = 30.f;    // panel top to the title's cap top
	static constexpr float TitleBlock     = 90.f;    // panel top to the first row
	static constexpr float PreferredPitch = 46.f;    // row pitch when the page fits
	static constexpr float MinPitch       = 20.f;    // the floor it shrinks to when it does not
	static constexpr float RowFill        = 0.82f;   // plate height / pitch
	static constexpr float SideGutter     = 40.f;    // panel edge to the plates
	static constexpr float BottomPad      = 24.f;
	static constexpr float ColumnGap      = 18.f;    // label plate to its control
	static constexpr float BodyCap        = 0.34f;   // body caps / plate height (Erbaum Bold)
	static constexpr float HeaderCap      = 12.f;
	static constexpr float NoteCap        = 0.30f;   // note caps / row height
	static constexpr float ValueBoxFill   = 0.80f;   // value box height / row height
	static constexpr float SliderValueW   = 116.f;   // the slider's value box
	static constexpr float ValueMaxW      = 330.f;   // a choice / toggle / name box
	static constexpr float ChipMaxW       = 200.f;   // one KEY chip
	static constexpr float ChipGap        = 12.f;
	static constexpr float LegendChipH    = 28.f;
	static constexpr float LegendTopGap   = 18.f;
	static constexpr float LegendLineGap  = 38.f;
	static constexpr float RootRowW       = 520.f;   // the pause root: the title screen's row metrics
	static constexpr float RootRowH       = 60.f;
	static constexpr float RootPitch      = 71.f;
	static constexpr float PanelAlpha     = 0.90f;   // the black panel, over the kit's 0.82 scrim
	static constexpr float PanelMaxH      = 0.95f;   // of the view
}

/** The colours this page uses that the kit's state table does not already decide. */
namespace TraceOptionsMenuPalette
{
	static const FLinearColor Caption(1.f, 1.f, 1.f, 0.55f);
	static const FLinearColor Rule(1.f, 1.f, 1.f, 0.12f);
	/** Notes and inert furniture: the kit's disabled word grey. */
	static const FLinearColor Note(0.55f, 0.55f, 0.55f, 1.f);
	static const FLinearColor PanelFill(0.f, 0.f, 0.f, TraceOptionsMenuLayout::PanelAlpha);
}

/**
 * Text on this page, sized by CAP HEIGHT in the face it is drawn in (the kit's rule), with its caps
 * centred on a line. One measure and one draw, so a width and a draw can never disagree about the face.
 */
namespace TraceOptionsMenuText
{
	static TraceText::FStyle MakeStyle(float CapH, const FLinearColor& Color, ETraceTextWeight Weight)
	{
		return TraceText::FStyle(TraceText::SizeForCapHeight(FMath::Max(1.f, CapH), Weight), Color, Weight);
	}

	static float Width(const FString& Text, float CapH, ETraceTextWeight Weight)
	{
		return Text.IsEmpty() ? 0.f : TraceText::MeasureWidth(Text, MakeStyle(CapH, FLinearColor::White, Weight));
	}

	/** @p Text with its caps centred on @p CapCenterY, X per @p HAlign, shrunk to @p MaxW (0: no limit). */
	static float Draw(AHUD* HUD, const FString& Text, float X, float CapCenterY, float CapH,
		const FLinearColor& Color, ETraceTextWeight Weight, TraceText::EHAlign HAlign, float MaxW = 0.f)
	{
		if (HUD == nullptr || Text.IsEmpty())
		{
			return 0.f;
		}
		TraceText::FStyle Drawn = MakeStyle(CapH, Color, Weight);
		Drawn.HAlign = HAlign;
		if (MaxW > 0.f)
		{
			const float Natural = TraceText::MeasureWidth(Text, Drawn);
			if (Natural > MaxW && Natural > 0.f)
			{
				Drawn.Size *= MaxW / Natural;
			}
		}
		return TraceMenuKit::DrawTextCapCentered(HUD, Text, X, CapCenterY, Drawn);
	}
}

// =================================================================================================
// THE ARTIST'S ART, ON THIS SCREEN — spec v20 §0.6
//
// The artist's menu sheet was sliced into /Game/Trace/UI/Art and hung on a UMG title screen. This
// overlay could not reach any of it: it is a plain C++ class that paints through AHUD::DrawRect /
// DrawText / DrawLine from inside DrawHUD, and there is no Slate here to hang a Box brush on. So
// the same textures are drawn through AHUD::DrawTexture, which is the Canvas equivalent — same
// assets, same package, no new plumbing, and identical on both hosts.
//
// AND IT HAS TO BE BOTH HOSTS. This class draws the title screen's SETTINGS page and the in-match
// pause menu (Escape during a match) from one Draw(); the user asked for the art in-game, not only
// on the way in. Everything below is therefore in the shared path, and the pause root — RESUME /
// SETTINGS / VIDEO / RETURN TO TITLE / QUIT — is five Action rows that pick up the artist's button
// plates without a single line of host-specific code.
//
// THREE THINGS MAKE THIS SAFE TO PUT IN FRONT OF A PAUSED MATCH:
//
//  1. EVERY CALL SITE HAS A FALLBACK. Sprite() returns null if a texture is missing, if the package
//     failed to cook, or if the layer is switched off, and every caller then draws the exact
//     rectangle it drew before this change. A missing asset cannot produce a white box or an empty
//     row; it produces last week's screen. Grep this file for DrawTexture: not one of them stands
//     without an else.
//
//  2. NO GEOMETRY MOVES. Every sprite is fitted to a rectangle that was already being computed —
//     the row rect, the slider track, the key chip. FRow::Rect and FRow::Track are written from the
//     same expressions as before, so hit testing, slider dragging, Trace.Menu.Nudge, DebugGetRowRect
//     and the -TraceMenuClickTest harness all measure exactly what they measured before. Art shrinks
//     to fit a row; a row never grows to fit art.
//
//  3. THE TEXTURES ARE ROOTED ON FIRST USE. This class is deliberately not a UObject and holds no
//     UObject reference that outlives a frame (see the header). In a match nothing else in the world
//     references these textures, so a cached raw pointer would be collected out from under the pause
//     menu and a bare weak pointer would re-stream the art off disk mid-match. AddToRoot costs about
//     a megabyte for ten small UI textures and makes both failures impossible — and it is done in
//     the shared kit (TraceMenuKit::Sprite), not in a member, so the header's invariant still holds.
//
// ALL OF IT IS NOW DRAWN THROUGH THE SHARED KIT RENDERER (UI/Widgets/Menu/TraceMenuKit.h): the sprite
// cache, the render-resource guard, the plate and the slider trough. This page used to own copies of
// all four; the kit's are the same arithmetic, moved (Trace.UI.Kit.Verify proves the plate is the
// same pixels), so nothing on this page moved when it changed.
// =================================================================================================

namespace TraceOptionsMenuArt
{
	/**
	 * Off switch for the whole layer, so ONE binary produces both arms of a before/after.
	 *
	 * Spec v20 §4: a harness that cannot fail is not evidence. `Trace.Menu.Art 0` puts every fallback
	 * path on screen in the same session, which is both the red arm for these captures and the
	 * fastest way to confirm the fallbacks are still there.
	 */
	static int32 GEnabled = 1;

#if !UE_BUILD_SHIPPING
	// A CVar, not a console command: this file already fatals at module load if the two share a name
	// (see the Trace.Menu.Video block), and Trace.Menu.Art collides with neither command next door.
	static FAutoConsoleVariableRef CVarMenuArt(
		TEXT("Trace.Menu.Art"),
		GEnabled,
		TEXT("1 (default): the settings / pause overlay draws the artist's sprites. 0: the plain ")
		TEXT("rectangles it drew before spec v20. Both arms come out of one build, which is what makes ")
		TEXT("a before/after capture evidence rather than decoration."),
		ECVF_Default);
#endif

	/** The kit sprites this page draws: three button plates, the slider trough and blade, the value box. */
	static const ETraceKitSprite UsedSprites[] =
	{
		ETraceKitSprite::BtnDefault,
		ETraceKitSprite::BtnHover,
		ETraceKitSprite::BtnDisabled,
		ETraceKitSprite::SliderTrack,
		ETraceKitSprite::SliderHandle,
		ETraceKitSprite::ValueBox,
	};

	/**
	 * The overlay's FIRST drawn frame in this process: how many of UsedSprites were not drawable. -1
	 * until it has been drawn. Anything above zero is a frame of flat fallback plates in front of the
	 * player (TraceMenuKit::Prime in both HUDs' BeginPlay is what keeps it at zero); Trace.Menu.Verify
	 * reports it.
	 */
	static int32 GFirstDrawUnready = -1;

	/**
	 * The texture, or null — which every caller treats as "draw the rectangle you drew before".
	 *
	 * *** A LOADED TEXTURE IS NOT A DRAWABLE ONE, AND DRAWING ONE ANYWAY IS A CRASH. *** This page is
	 * where that was found and measured, and the finding now lives in TraceMenuKit::IsDrawable:
	 * `AHUD::DrawTexture` passes `Texture->GetResource()` straight into an FCanvasTileItem and checks
	 * only the UTexture (Engine HUD.cpp:986), so a texture that is LOADED but whose render resource
	 * has no RHI texture yet became a batched element the render thread died on — SIGSEGV in
	 * FBatchedElements::Draw at address 0x30, ~130 ms after this page first drew. Guarding on
	 * `GetResource() != nullptr` did NOT fix it (the resource object exists straight away); guarding
	 * on `FTextureResource::TextureRHI` did. Spec v23 §A2 exposed it: until then the Canvas title
	 * screen had always warmed these textures earlier in the same DrawHUD.
	 *
	 * The kit returns null for the one or two frames before the RHI texture exists, and the caller
	 * draws the plain rectangle it drew before spec v20 — the same path a missing file takes.
	 */
	static UTexture2D* Sprite(ETraceKitSprite Which)
	{
		if (GEnabled == 0)
		{
			return nullptr;
		}
		return TraceMenuKit::Sprite(Which);
	}

#if !UE_BUILD_SHIPPING
	/**
	 * SPEC v24 §1 — EVERY TEXTURE THIS PAGE CAN HAND THE CANVAS, AND WHETHER IT IS RENDERABLE YET.
	 *
	 * The v23 crash was a canvas batch holding a texture whose RHI resource did not exist, and it was
	 * closed by guarding the ten SPRITES. This page hands the canvas an eleventh and twelfth texture
	 * that no guard on this side covers: the Sofachrome GLYPH SHEETS, one per weight, drawn one tile
	 * per glyph by TraceCanvasText. They are read here, never written — the text renderer is not this
	 * agent's file — because a log line naming which of the twelve was unready on the frame before a
	 * SIGSEGV is the difference between fixing this bug and guessing at it again.
	 *
	 * Logged for the first few DRAWN frames of the overlay rather than once: the failure window is
	 * one or two frames wide, and a once-per-process line lands before the window opens.
	 */
	static void LogReadiness(int32 DrawsSinceOpen)
	{
		if (GFirstDrawUnready < 0)
		{
			GFirstDrawUnready = 0;
			for (const ETraceKitSprite Which : UsedSprites)
			{
				GFirstDrawUnready += TraceMenuKit::IsDrawable(TraceMenuKit::PeekSprite(Which)) ? 0 : 1;
			}
			UE_LOG(LogTraceGame, Display, TEXT("[Options] First frame this process: %d of %d sprites %s."),
				int32(UE_ARRAY_COUNT(UsedSprites)) - GFirstDrawUnready, int32(UE_ARRAY_COUNT(UsedSprites)),
				GFirstDrawUnready == 0 ? TEXT("drawable - no fallback frame")
					: TEXT("drawable - the rest drew their flat fallback plates this frame"));
		}

		if (DrawsSinceOpen > 4)
		{
			return;
		}

		auto State = [](const UTexture2D* Tex) -> const TCHAR*
		{
			if (Tex == nullptr)                       { return TEXT("absent"); }
			const FTextureResource* Res = Tex->GetResource();
			if (Res == nullptr)                       { return TEXT("NO-RESOURCE"); }
			if (!Res->TextureRHI.IsValid())           { return TEXT("NO-RHI"); }
			return TEXT("ready");
		};

		FString Line;
		for (int32 Index = 0; Index < int32(UE_ARRAY_COUNT(UsedSprites)); ++Index)
		{
			// PeekSprite, NOT Sprite(): Sprite() would LoadObject and flush async loading from inside
			// a draw pass, which is a thing this diagnostic must observe and not cause.
			Line += FString::Printf(TEXT("%d=%s "), Index, State(TraceMenuKit::PeekSprite(UsedSprites[Index])));
		}

		const UTexture2D* AtlasLight = TraceText::AtlasTexture(ETraceTextWeight::Light);
		const UTexture2D* AtlasBold  = TraceText::AtlasTexture(ETraceTextWeight::Bold);

		// HUD (Erbaum Bold) JOINED THIS LIST IN SPEC v26 §2, because this page now draws its whole
		// body out of that sheet. A readiness line that named only the two Sofachrome sheets would
		// have gone on reporting "ready" through exactly the frame where the settings rows came out
		// blank, which is the failure this diagnostic exists to catch.
		const UTexture2D* AtlasHud   = TraceText::AtlasTexture(ETraceTextWeight::Hud);

		UE_LOG(LogTraceGame, Display,
			TEXT("[Options] Readiness draw#%d: sprites %s| atlas(active=%d) light=%s bold=%s hud=%s"),
			DrawsSinceOpen, *Line, TraceText::IsAtlasActive() ? 1 : 0,
			State(AtlasLight), State(AtlasBold), State(AtlasHud));
	}
#endif

	/**
	 * Resolves every sprite once and says out loud what landed.
	 *
	 * `Trace.UI.VerifyMenuArt` only ever asked the UMG title screen's brushes whether they had a
	 * texture; it has no opinion about this screen at all, and its own message still says these
	 * sprites "belong to the settings screen, which is still Canvas". So this is the only thing that
	 * can tell a reader of a log whether the pause menu in front of them is wearing the art or its
	 * fallbacks — and it prints the count both ways round, which is what makes a red arm readable.
	 */
	static void LogOnce()
	{
		static bool bLogged = false;
		if (bLogged)
		{
			return;
		}
		bLogged = true;

		// Sprite() loads (and is gated by Trace.Menu.Art); PeekSprite() then says whether it LOADED. A
		// loaded sprite is usually not drawable on the first frame, and that is not a fault.
		int32 Resolved = 0;
		const int32 Wanted = int32(UE_ARRAY_COUNT(UsedSprites));
		for (const ETraceKitSprite Which : UsedSprites)
		{
			Sprite(Which);
			Resolved += (GEnabled != 0 && TraceMenuKit::PeekSprite(Which) != nullptr) ? 1 : 0;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Options] Menu art: %d of %d sprites resolved (Trace.Menu.Art = %d). %s"),
			Resolved, Wanted, GEnabled,
			(GEnabled == 0)
				? TEXT("Art is OFF: every control is drawing the plain rectangle it drew before spec v20.")
				: ((Resolved == Wanted)
					? TEXT("Plates, slider and chips are the artist's, through the shared kit renderer, on both hosts.")
					: TEXT("Some controls are drawing their fallback rectangles; see the warnings above.")));
	}
}

namespace TraceOptionsMenuFile
{
	/**
	 * Every key the KEYBIND page is allowed to capture, built once.
	 *
	 * EKeys::GetAllKeys() is a few hundred entries including every gamepad axis and every gesture,
	 * and this list is walked once per frame during a rebind capture. Filtering it up front keeps
	 * that walk to the ~150 real buttons and, more importantly, means an axis can never be captured
	 * as a binding — Dash on "MouseX" would fire every time the player looked around.
	 *
	 * *** D31-PAD: IsBindableKeyboardKey, NOT IsBindableKey — THE PAD BUTTONS ARE GONE FROM HERE. ***
	 * They used to be in this list, because IsBindableKey accepts them and still does. Leaving them
	 * would mean the keybind page could put the A button into the KEYBOARD table while the controller
	 * page has it in the PAD table: one physical button mapped in two contexts at once, the
	 * higher-priority one consuming it, and the other page's row silently doing nothing at all. See
	 * IsBindableKeyboardKey in Settings/TraceUserSettings.h for the argument, and note that a saved
	 * .ini line naming a pad button is still honoured — this is about what a player may CREATE here,
	 * never about what loads.
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
				if (UTraceUserSettings::IsBindableKeyboardKey(Key))
				{
					Keys.Add(Key);
				}
			}
		}
		return Keys;
	}

	/**
	 * D31-PAD — every key the controller page is allowed to capture, built once.
	 *
	 * A SECOND LIST AND NOT A FILTER ON THE FIRST at capture time, for the same reason BindableKeys
	 * exists at all: this is walked once per frame during a capture, and EKeys::GetAllKeys() is a few
	 * hundred entries. It is also the thing that makes the two pages a PARTITION — the keyboard page
	 * can capture nothing that is on a pad and this page can capture nothing that is not, so one
	 * physical button can never end up in both tables and therefore never in both mapping contexts.
	 */
	const TArray<FKey>& PadBindableKeys()
	{
		static TArray<FKey> Keys;
		if (Keys.Num() == 0)
		{
			TArray<FKey> All;
			EKeys::GetAllKeys(All);
			Keys.Reserve(32);
			for (const FKey& Key : All)
			{
				if (UTraceUserSettings::IsBindablePadKey(Key))
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

	/**
	 * An action's row label on the KEYBOARD and CONTROLLER pages, THROUGH THE TEXT DOCUMENT.
	 *
	 * The table's DisplayName is a `const TCHAR*` in a table built once (TraceInputActions::All), so it
	 * cannot hold an editable string; its own comment names the place that can — where the label is
	 * COPIED into a row, once per rebuild. One literal key per action, so the document's scanner finds
	 * all twenty (Scripts/dump-game-text.py) and Config/TraceGameText.ini gets a line for each. The
	 * defaults ARE the table's DisplayNames; an action added to the table without a line here shows its
	 * DisplayName.
	 */
	FString ActionLabel(const FTraceInputActionInfo& Info)
	{
		switch (Info.Action)
		{
		case ETraceInputAction::MoveForward:      return TRACE_TEXT("OPTIONS.ACTION.MOVE_FORWARD", "MOVE FORWARD");
		case ETraceInputAction::MoveBack:         return TRACE_TEXT("OPTIONS.ACTION.MOVE_BACK", "MOVE BACK");
		case ETraceInputAction::MoveLeft:         return TRACE_TEXT("OPTIONS.ACTION.STRAFE_LEFT", "STRAFE LEFT");
		case ETraceInputAction::MoveRight:        return TRACE_TEXT("OPTIONS.ACTION.STRAFE_RIGHT", "STRAFE RIGHT");
		case ETraceInputAction::Jump:             return TRACE_TEXT("OPTIONS.ACTION.JUMP", "JUMP");
		case ETraceInputAction::Crouch:           return TRACE_TEXT("OPTIONS.ACTION.CROUCH_SLIDE", "CROUCH / SLIDE");
		case ETraceInputAction::Dash:             return TRACE_TEXT("OPTIONS.ACTION.DASH", "DASH");
		case ETraceInputAction::Parry:            return TRACE_TEXT("OPTIONS.ACTION.PARRY", "PARRY");
		case ETraceInputAction::Fire:             return TRACE_TEXT("OPTIONS.ACTION.FIRE", "FIRE");
		case ETraceInputAction::Pass:             return TRACE_TEXT("OPTIONS.ACTION.THROW_PASS_CORE", "THROW / PASS CORE");
		case ETraceInputAction::Scoreboard:       return TRACE_TEXT("OPTIONS.ACTION.SCOREBOARD", "SCOREBOARD");
		case ETraceInputAction::EquipKnife:       return TRACE_TEXT("OPTIONS.ACTION.KNIFE", "KNIFE");
		case ETraceInputAction::EquipGun:         return TRACE_TEXT("OPTIONS.ACTION.PISTOL", "PISTOL");
		case ETraceInputAction::Ability:          return TRACE_TEXT("OPTIONS.ACTION.ABILITY", "ABILITY");
		case ETraceInputAction::AbilitySecondary: return TRACE_TEXT("OPTIONS.ACTION.ABILITY_SECONDARY", "ABILITY (SECONDARY)");
		case ETraceInputAction::Reload:           return TRACE_TEXT("OPTIONS.ACTION.RELOAD", "RELOAD");
		case ETraceInputAction::PullCore:         return TRACE_TEXT("OPTIONS.ACTION.PULL_CORE", "PULL CORE");
		case ETraceInputAction::Melee:            return TRACE_TEXT("OPTIONS.ACTION.MELEE", "MELEE");
		case ETraceInputAction::EquipSmg:         return TRACE_TEXT("OPTIONS.ACTION.SMG", "SMG");
		case ETraceInputAction::Inspect:          return TRACE_TEXT("OPTIONS.ACTION.INSPECT_KNIFE", "INSPECT KNIFE");
		default:                                  return FString(Info.DisplayName);
		}
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
namespace TraceOptionsMenuFile
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

	FAutoConsoleCommand CmdMenuSettings(
		TEXT("Trace.Menu.Settings"),
		TEXT("Opens the SETTINGS page on whichever HUD is up. Works on the title screen and in a match. ")
		TEXT("Twin of Trace.Menu.Video, and it exists for the same reason: until spec v20 there was no ")
		TEXT("headless way to photograph the page that has the most art on it, so nobody could show a ")
		TEXT("before and an after of it. -TraceExec=\"Trace.Menu.Settings\"."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->OpenSettings();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Settings: no HUD is drawing an overlay yet."));
			}
		}));

	FAutoConsoleCommand CmdMenuCrosshair(
		TEXT("Trace.Menu.Crosshair"),
		TEXT("Spec v29 s3. Opens the CROSSHAIR settings page on whichever HUD is up. Works on the title ")
		TEXT("screen and in a match. Twin of Trace.Menu.Video and Trace.Menu.Settings, and it exists for ")
		TEXT("the same reason: a headless run has no keyboard, so without it there is no way to photograph ")
		TEXT("this page at all. -TraceExec=\"Trace.Menu.Crosshair\"."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->OpenCrosshair();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Crosshair: no HUD is drawing an overlay yet."));
			}
		}));

	FAutoConsoleCommand CmdMenuAudio(
		TEXT("Trace.Menu.Audio"),
		TEXT("UI plan WP3. Opens the AUDIO settings page on whichever HUD is up. Works on the title ")
		TEXT("screen and in a match. Triplet of Trace.Menu.Video and Trace.Menu.Crosshair, and it exists ")
		TEXT("for the same reason: a headless run has no keyboard, so without it there is no way to ")
		TEXT("photograph this page at all. -TraceExec=Trace.Menu.Audio."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->OpenAudio();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Audio: no HUD is drawing an overlay yet."));
			}
		}));

	FAutoConsoleCommand CmdMenuController(
		TEXT("Trace.Menu.Controller"),
		TEXT("D31-PAD. Opens the CONTROLLER settings page on whichever HUD is up. Works on the title ")
		TEXT("screen and in a match. Fourth of the Trace.Menu.* family and it exists for the same ")
		TEXT("reason: a headless run has no keyboard and no pad, so without it there is no way to ")
		TEXT("photograph this page at all. -TraceExec=Trace.Menu.Controller."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->OpenController();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Controller: no HUD is drawing an overlay yet."));
			}
		}));

	FAutoConsoleCommand CmdMenuKeyboard(
		TEXT("Trace.Menu.Keyboard"),
		TEXT("Opens the KEYBOARD binds page on whichever HUD is up. Works on the title screen and in a ")
		TEXT("match. The Trace.Menu.* family's reason: a headless run has no keyboard, so without it there ")
		TEXT("is no way to photograph this page. -TraceExec=Trace.Menu.Keyboard."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->OpenKeyboard();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Keyboard: no HUD is drawing an overlay yet."));
			}
		}));

	FAutoConsoleCommand CmdMenuVerify(
		TEXT("Trace.Menu.Verify"),
		TEXT("Checks the settings / pause overlay's own behaviour, driving it one drawn frame at a time: ")
		TEXT("a pointer resting over a row does not take the selection when a page opens; BACK lands on ")
		TEXT("the door the player came through; RESET and a saved-slot CLEAR need a second press; a click ")
		TEXT("on a choice row's '<' steps it DOWN; and with the world PAUSED the menu clock still runs, so a ")
		TEXT("held DOWN repeats. Uses its own pointer, restores everything it changes. Title screen or match."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->DebugBeginVerify();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[MenuVerify] No HUD is drawing an overlay yet."));
			}
		}));

	FAutoConsoleCommand CmdMenuLoadout(
		TEXT("Trace.Menu.Loadout"),
		TEXT("Trace.Menu.Loadout <1-5>. Opens SETTINGS > LOADOUTS with that slot's editor up, on whichever ")
		TEXT("HUD is drawing an overlay. The Trace.Menu.* family's reason: a headless run has no keyboard, ")
		TEXT("and the library editor is otherwise three presses deep. -TraceExec=\"Trace.Menu.Loadout 2\"."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const int32 Slot = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 5) : 1;
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->DebugOpenLoadoutEditor(Slot - 1);
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[Options] Trace.Menu.Loadout: no HUD is drawing an overlay yet."));
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

	FAutoConsoleCommand CmdRebindProof(
		TEXT("Trace.Keys.RebindProof"),
		TEXT("Spec v28 s3a. Counts how many complete mouse clicks it takes to open a rebind capture and ")
		TEXT("how many complete key presses it takes to land the key, by parking the real cursor on a real ")
		TEXT("key chip and injecting real edges through UGameViewportClient::InputKey. One and one is the ")
		TEXT("requirement. Runs on whichever overlay is up - title screen or in-match pause menu - and ")
		TEXT("restores every binding it touches."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (GActiveOptionsMenu != nullptr)
			{
				GActiveOptionsMenu->DebugBeginRebindProof();
			}
			else
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[RebindProof] No HUD is drawing an overlay yet, so there is nothing to drive."));
			}
		}));
}
#endif

FTraceOptionsMenu::~FTraceOptionsMenu()
{
	// SPEC v28 §3a — a last-resort restore. Close() is the ordinary path and every exit goes through
	// it, but an overlay destroyed WITH the panel still up (a HUD taken down by a travel) would
	// otherwise leave the viewport in CaptureDuringMouseDown for the rest of the process. It is a
	// no-op unless this instance is the one that raised the mode.
	SetPressDeliveryOverride(false);

#if !UE_BUILD_SHIPPING
	if (TraceOptionsMenuFile::GActiveOptionsMenu == this)
	{
		TraceOptionsMenuFile::GActiveOptionsMenu = nullptr;
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

// =================================================================================================
// SPEC v28 §3a — Trace.Keys.RebindProof: HOW MANY PRESSES DOES A REBIND ACTUALLY TAKE?
//
// The report is "when I click to rebind a key in settings, it's not working", restated by the note as
// "rebinding currently needs the button pressed twice before it registers". That is a COUNT, so the
// only honest answer is a count, taken the way a player produces it:
//
//   1. park the real OS cursor on a real key chip on a real row (PC->SetMouseLocation),
//   2. inject complete LEFT MOUSE down/up pairs through UGameViewportClient::InputKey — the exact
//      call FSceneViewport::OnMouseButtonDown makes for a physical mouse — until the row opens its
//      capture, counting the pairs,
//   3. inject complete pairs for the key being bound, the same way, until UTraceUserSettings says the
//      binding changed, counting those too.
//
// NOTHING HERE REACHES PAST THE INPUT PATH. It never sets bCapturingKey, never calls SetKey and never
// calls ActivateSelected; if it did, it would pass on a build whose input path is exactly what is
// broken, which is the failure mode this project has been caught by before (see the ClickTest's note
// about judging a click before it had been delivered).
//
// EVERY EDGE GETS ITS OWN DRAWN FRAME, AND THE JUDGEMENT COMES A FRAME AFTER THE RELEASE.
// APlayerController::InputKey does not run anything; it queues the event for the next
// ProcessInputStack, and this overlay then polls that state from the NEXT DrawHUD. Judging on the
// same frame as the release asks "did that work?" before the click has been delivered and manufactures
// the very bug it is measuring.
//
// It runs off DRAWN FRAMES rather than a timer because the in-match pause menu stops the world, so
// every world timer freezes the instant the overlay opens — the same reason TickAutoActivate counts
// draws.
//
// The bindings it moves are snapshotted, every slot, and put back in the Report stage.
// =================================================================================================

namespace TraceOptionsRebindProof
{
	/** Complete down/up pairs one stage is allowed before it is declared dead. */
	constexpr int32 MaxPairs = 5;

	/** The row the proof drives. PARRY, because spec v28 §3d ships it with BOTH slots occupied, so
	 *  the second chip is on screen and clickable without the harness having to arrange it first. */
	constexpr ETraceInputAction TargetAction = ETraceInputAction::Parry;

	/**
	 * Keys with no default bind anywhere in the table, so a pass cannot be an accident.
	 *
	 * J and H rather than the obvious J and K: the owner's own TraceUserSettings.ini has EQUIP GUN
	 * rebound to K, and a harness that stole a real player's real bind — even for four frames, even
	 * with a restore afterwards — would be writing noise into the log it is asking somebody to read.
	 */
	FKey KeyForPass(int32 Pass) { return (Pass == 0) ? EKeys::J : EKeys::H; }

	/**
	 * *** THE GATE THIS HARNESS EXISTS TO GET ABOVE. ***
	 *
	 * FSceneViewport::OnMouseButtonDown does NOT forward every press to the game. Its own rule is
	 *
	 *     bTemporaryCapture    = captureMode == CaptureDuringMouseDown (or the RMB variant)
	 *     bProcessInputPrimary = !IsCurrentlyGameViewport() || HasMouseCapture()
	 *                            || captureMode == CapturePermanently_IncludingInitialMouseDown
	 *     if (bTemporaryCapture || bProcessInputPrimary)  -> ViewportClient->InputKey(IE_Pressed)
	 *
	 * and the RELEASE is forwarded unconditionally. So under EMouseCaptureMode::NoCapture — which the
	 * title screen sets on purpose (ATraceMenuPlayerController::BeginPlay, to stop the stray "initial
	 * mouse down" replay) — a real mouse-down never reaches APlayerController at all until something
	 * else has given the viewport Slate mouse capture.
	 *
	 * Reproduced here rather than called, because FSceneViewport does not expose it. Printing it beside
	 * every click is the difference between "the click did nothing" and knowing WHY.
	 */
	bool WouldViewportForwardAPress(FString& OutWhy)
	{
		FViewport* Viewport = (GEngine != nullptr && GEngine->GameViewport != nullptr)
			? GEngine->GameViewport->Viewport : nullptr;
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || Viewport == nullptr)
		{
			OutWhy = TEXT("no game viewport");
			return false;
		}

		const EMouseCaptureMode Mode = GEngine->GameViewport->GetMouseCaptureMode();
		const bool bTemporaryCapture = (Mode == EMouseCaptureMode::CaptureDuringMouseDown)
			|| (Mode == EMouseCaptureMode::CaptureDuringRightMouseDown);
		const bool bHasCapture = Viewport->HasMouseCapture();
		const bool bInitialDown = (Mode == EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown);
		const bool bWould = bTemporaryCapture || bHasCapture || bInitialDown;

		OutWhy = FString::Printf(TEXT("captureMode=%d temporaryCapture=%d hasMouseCapture=%d initialDown=%d"),
			static_cast<int32>(Mode), bTemporaryCapture ? 1 : 0, bHasCapture ? 1 : 0, bInitialDown ? 1 : 0);
		return bWould;
	}

	/**
	 * One key or mouse edge, injected at the TOP of the chain — FSlateApplication — so it travels the
	 * whole route a physical device does, the viewport's own press gate included.
	 *
	 * The previous version of this went in at UGameViewportClient::InputKey, one level BELOW that gate,
	 * and that is precisely the "harness that reports a deleted asset as working" mistake this project
	 * has already paid for once: it measured the menu's logic on a build where the menu's logic was
	 * never the problem, and it would have reported a green pass on a title screen no click can reach.
	 */
	void InjectKey(const FKey& Key, bool bPressed)
	{
		if (!FSlateApplication::IsInitialized() || GEngine == nullptr || GEngine->GameViewport == nullptr)
		{
			return;
		}

		FSlateApplication& Slate = FSlateApplication::Get();

		// Slate refuses to route a key event to a widget with no focus, and an automated run very often
		// has no OS window focus at all. Forcing focus is the difference between "input is broken" and
		// "this process was in the background".
		if (TSharedPtr<SViewport> ViewportWidget = GEngine->GameViewport->GetGameViewportWidget())
		{
			Slate.SetAllUserFocus(ViewportWidget, EFocusCause::SetDirectly);
		}

		const FInputDeviceId Device = FInputDeviceId::CreateFromInternalId(0);

		if (Key.IsMouseButton())
		{
			const FVector2D Cursor = Slate.GetCursorPos();
			TSet<FKey> PressedButtons;
			if (bPressed)
			{
				PressedButtons.Add(Key);
			}

			const FPointerEvent MouseEvent(
				Device, /*PointerIndex*/ 0, Cursor, Cursor,
				PressedButtons, Key, /*WheelDelta*/ 0.f, FModifierKeysState());

			if (bPressed)
			{
				Slate.ProcessMouseButtonDownEvent(nullptr, MouseEvent);
			}
			else
			{
				Slate.ProcessMouseButtonUpEvent(MouseEvent);
			}
			return;
		}

		const FKeyEvent KeyEvent(Key, FModifierKeysState(), Device,
			/*bIsRepeat*/ false, /*CharacterCode*/ 0, /*KeyCode*/ 0);

		if (bPressed)
		{
			Slate.ProcessKeyDownEvent(KeyEvent);
		}
		else
		{
			Slate.ProcessKeyUpEvent(KeyEvent);
		}
	}
}

void FTraceOptionsMenu::DebugBeginRebindProof()
{
	if (RebindProofStage != ERebindProofStage::Idle)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[RebindProof] Already running."));
		return;
	}

	// Snapshot BEFORE the first injected edge: the very first pair already writes a binding.
	UTraceUserSettings& Settings = UTraceUserSettings::Get();
	RebindProofBefore.Reset();
	for (const FTraceInputActionInfo& Info : TraceInputActions::All())
	{
		for (int32 Slot = 0; Slot < UTraceUserSettings::MaxKeysPerAction; ++Slot)
		{
			RebindProofBefore.Add(Settings.GetKey(Info.Action, Slot));
		}
	}

	RebindProofStage = ERebindProofStage::WaitForPage;
	RebindProofWait = 0;
	RebindProofSubStep = 0;
	RebindProofClicks = 0;
	RebindProofPresses = 0;
	RebindProofClicksNeeded = 0;
	RebindProofPressesNeeded = 0;
	RebindProofAction = TraceOptionsRebindProof::TargetAction;
	RebindProofSlot = 0;

	UE_LOG(LogTraceGame, Display,
		TEXT("[RebindProof] ===== spec v28 s3a: counting the presses a rebind takes ====="));
	UE_LOG(LogTraceGame, Display,
		TEXT("[RebindProof] Target row '%s'. Pass 1 clicks CHIP 1 and binds '%s'; pass 2 clicks CHIP 2 ")
		TEXT("and binds '%s'. One click and one press each is the requirement."),
		TraceInputActions::Info(RebindProofAction).DisplayName,
		*UTraceUserSettings::DescribeKey(TraceOptionsRebindProof::KeyForPass(0)),
		*UTraceUserSettings::DescribeKey(TraceOptionsRebindProof::KeyForPass(1)));
}

void FTraceOptionsMenu::TickRebindProof(APlayerController* PC)
{
	// ---- `-TraceRebindProof=<drawn frames>` ------------------------------------------------------
	//
	// A LAUNCH FLAG AS WELL AS A CONSOLE COMMAND, and the reason is the in-match pause menu: it stops
	// the world, so -TraceExec and Trace.V10.After — both scheduled on WORLD timers — can never fire
	// once the overlay is up. Drawn frames are the only clock still running, which is the same
	// discovery TickAutoActivate and ATraceHUD's auto-pause capture each had to make.
	//
	//     -TraceAutoPause=6 -TraceRebindProof=40
	//
	// arms the pause menu the normal way (gameplay input suppressed, world paused, cursor back) and
	// then counts the presses a rebind takes ON THAT HOST — which is where a player actually rebinds
	// mid-match, and is a different input regime from the title screen in every respect that matters.
	{
		static bool bParsed = false;
		static int32 WantedDraws = -1;
		if (!bParsed)
		{
			bParsed = true;
			int32 Parsed = 0;
			if (FParse::Value(FCommandLine::Get(), TEXT("TraceRebindProof="), Parsed))
			{
				WantedDraws = FMath::Max(1, Parsed);
			}
		}

		if (WantedDraws > 0 && !bRebindProofArmedFromCommandLine && Page != EPage::Closed
			&& DrawsSinceOpen >= WantedDraws)
		{
			bRebindProofArmedFromCommandLine = true;
			UE_LOG(LogTraceGame, Display,
				TEXT("[RebindProof] -TraceRebindProof=%d: arming after %d drawn frames on the %s host."),
				WantedDraws, DrawsSinceOpen,
				(Page == EPage::Root) ? TEXT("in-match pause") : TEXT("settings"));
			DebugBeginRebindProof();
		}
	}

	if (RebindProofStage == ERebindProofStage::Idle)
	{
		return;
	}

	if (PC == nullptr)
	{
		return;
	}

	if (RebindProofWait > 0)
	{
		--RebindProofWait;
		return;
	}

	// The row, found by the ACTION rather than by a label string, so a DisplayName edit cannot
	// silently turn this harness into a no-op that reports nothing.
	int32 RowIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].Kind == ERowKind::Binding && Rows[Index].Binding == RebindProofAction)
		{
			RowIndex = Index;
			break;
		}
	}

	switch (RebindProofStage)
	{
	case ERebindProofStage::WaitForPage:
	{
		// The KEYBOARD page since the binds moved off SETTINGS (they made that page 43 rows long).
		if (Page != EPage::Keyboard)
		{
			OpenKeyboard();
			RebindProofWait = 3;
			return;
		}

		// A rect is only written by DrawRow, so an un-drawn page would have the harness clicking at
		// (0,0) and reporting a failure that is its own.
		if (RowIndex == INDEX_NONE || !Rows[RowIndex].KeyChip[RebindProofSlot].bIsValid)
		{
			RebindProofWait = 1;
			return;
		}

		const FVector2D Point = Rows[RowIndex].KeyChip[RebindProofSlot].GetCenter();
		PC->SetMouseLocation(FMath::RoundToInt(Point.X), FMath::RoundToInt(Point.Y));

		float ReadX = 0.f;
		float ReadY = 0.f;
		const bool bReadBack = PC->GetMousePosition(ReadX, ReadY);
		UE_LOG(LogTraceGame, Display,
			TEXT("[RebindProof] pass %d: cursor -> chip %d at (%.0f, %.0f); the menu reads it back as (%.0f, %.0f) valid=%d."),
			RebindProofSlot + 1, RebindProofSlot + 1, Point.X, Point.Y, ReadX, ReadY, bReadBack ? 1 : 0);

		if (!bReadBack)
		{
			// The one failure that would make every number below a lie. Say it; do not measure it.
			UE_LOG(LogTraceGame, Error,
				TEXT("[RebindProof] The viewport will not report a cursor position in this run, so no click ")
				TEXT("can be aimed. This is a HARNESS failure, not a menu failure."));
			RebindProofStage = ERebindProofStage::Report;
			return;
		}

		RebindProofStage = ERebindProofStage::ClickRow;
		RebindProofSubStep = 0;
		RebindProofWait = 1;   // a frame with the cursor parked before anything is pressed
		return;
	}

	case ERebindProofStage::ClickRow:
	{
		if (RebindProofSubStep == 0)
		{
			++RebindProofClicks;

			FString Why;
			const bool bWouldForward = TraceOptionsRebindProof::WouldViewportForwardAPress(Why);
			UE_LOG(LogTraceGame, Display,
				TEXT("[RebindProof] pass %d, click %d: LMB down (capturing=%d). The viewport %s forward this ")
				TEXT("PRESS to the game: %s."),
				RebindProofSlot + 1, RebindProofClicks, bCapturingKey ? 1 : 0,
				bWouldForward ? TEXT("WILL") : TEXT("will NOT"), *Why);
			TraceOptionsRebindProof::InjectKey(EKeys::LeftMouseButton, /*bPressed=*/true);
			RebindProofSubStep = 1;
			RebindProofWait = 1;
			return;
		}

		if (RebindProofSubStep == 1)
		{
			TraceOptionsRebindProof::InjectKey(EKeys::LeftMouseButton, /*bPressed=*/false);
			RebindProofSubStep = 2;
			RebindProofWait = 1;   // JUDGE a whole frame after the release. See the block comment.
			return;
		}

		// Judge.
		if (bCapturingKey && CapturingAction == RebindProofAction && CapturingSlot == RebindProofSlot)
		{
			RebindProofClicksNeeded = RebindProofClicks;
			UE_LOG(LogTraceGame, Display,
				TEXT("[RebindProof] pass %d: the capture OPENED on chip %d after %d click(s)."),
				RebindProofSlot + 1, CapturingSlot + 1, RebindProofClicks);
			RebindProofStage = ERebindProofStage::PressKey;
			RebindProofSubStep = 0;
			RebindProofWait = 1;
			return;
		}

		if (RebindProofClicks >= TraceOptionsRebindProof::MaxPairs)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[RebindProof] pass %d: %d complete clicks on chip %d and the capture never opened ")
				TEXT("(capturing=%d, capturedSlot=%d)."),
				RebindProofSlot + 1, RebindProofClicks, RebindProofSlot + 1,
				bCapturingKey ? 1 : 0, CapturingSlot);
			RebindProofClicksNeeded = 0;
			RebindProofStage = ERebindProofStage::Report;
			return;
		}

		// Same chip, same cursor, another complete click. Back to the press and NOT to the cursor
		// move: re-parking every attempt would hide a bug that only bites the first click.
		RebindProofSubStep = 0;
		return;
	}

	case ERebindProofStage::PressKey:
	{
		const FKey Wanted = TraceOptionsRebindProof::KeyForPass(RebindProofSlot);

		if (RebindProofSubStep == 0)
		{
			++RebindProofPresses;
			UE_LOG(LogTraceGame, Display,
				TEXT("[RebindProof] pass %d, key press %d: '%s' down (capturing=%d, frame=%llu, ignoreBefore=%llu)."),
				RebindProofSlot + 1, RebindProofPresses, *Wanted.GetFName().ToString(),
				bCapturingKey ? 1 : 0, static_cast<uint64>(GFrameCounter), IgnoreInputBeforeFrame);
			TraceOptionsRebindProof::InjectKey(Wanted, /*bPressed=*/true);
			RebindProofSubStep = 1;
			RebindProofWait = 1;
			return;
		}

		if (RebindProofSubStep == 1)
		{
			TraceOptionsRebindProof::InjectKey(Wanted, /*bPressed=*/false);
			RebindProofSubStep = 2;
			RebindProofWait = 1;
			return;
		}

		if (UTraceUserSettings::Get().GetKey(RebindProofAction, RebindProofSlot) == Wanted)
		{
			RebindProofPressesNeeded = RebindProofPresses;
			UE_LOG(LogTraceGame, Display,
				TEXT("[RebindProof] pass %d: '%s' LANDED on %s slot %d after %d press(es). The action now holds %s."),
				RebindProofSlot + 1, *Wanted.GetFName().ToString(),
				TraceInputActions::Info(RebindProofAction).DisplayName, RebindProofSlot + 1,
				RebindProofPresses, *UTraceUserSettings::Get().DescribeBinding(RebindProofAction));

			// Pass 1 proves §3a on the primary chip; pass 2 proves §3c — that the SECOND chip is
			// editable by the same one click and one press.
			if (RebindProofSlot + 1 < UTraceUserSettings::MaxKeysPerAction)
			{
				RebindProofSlot = 1;
				RebindProofClicks = 0;
				RebindProofPresses = 0;
				RebindProofStage = ERebindProofStage::WaitForPage;
				RebindProofSubStep = 0;
				RebindProofWait = 2;
				return;
			}

			RebindProofStage = ERebindProofStage::Report;
			return;
		}

		if (RebindProofPresses >= TraceOptionsRebindProof::MaxPairs)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[RebindProof] pass %d: %d complete presses of '%s' and %s slot %d is still '%s'."),
				RebindProofSlot + 1, RebindProofPresses, *Wanted.GetFName().ToString(),
				TraceInputActions::Info(RebindProofAction).DisplayName, RebindProofSlot + 1,
				*UTraceUserSettings::DescribeKey(UTraceUserSettings::Get().GetKey(RebindProofAction, RebindProofSlot)));
			RebindProofPressesNeeded = 0;
			RebindProofStage = ERebindProofStage::Report;
			return;
		}

		RebindProofSubStep = 0;
		return;
	}

	case ERebindProofStage::Report:
	default:
		break;
	}

	// ---- Report, restore, stop --------------------------------------------------------------
	RebindProofStage = ERebindProofStage::Idle;

	const bool bPass = (RebindProofClicksNeeded == 1) && (RebindProofPressesNeeded == 1);

	// Two calls rather than a ternary verbosity: UE_LOG's verbosity is a token the macro pastes into
	// a compile-time category check, not a value, so it cannot be an expression.
#define TRACE_REBINDPROOF_ARGS \
	bPass ? TEXT("ONE CLICK ARMS IT AND ONE PRESS BINDS IT") : TEXT("A REBIND STILL NEEDS MORE THAN ONE PRESS"), \
	RebindProofClicksNeeded, RebindProofPressesNeeded

#define TRACE_REBINDPROOF_TEXT \
	TEXT("[RebindProof] VERDICT: %s. Clicks to open the capture=%d, presses to land the key=%d ") \
	TEXT("(1 and 1 is the requirement; 0 means that stage never completed at all). Counts are from ") \
	TEXT("the LAST pass; every pass logged its own line above.")

	if (bPass)
	{
		UE_LOG(LogTraceGame, Display, TRACE_REBINDPROOF_TEXT, TRACE_REBINDPROOF_ARGS);
	}
	else
	{
		UE_LOG(LogTraceGame, Error, TRACE_REBINDPROOF_TEXT, TRACE_REBINDPROOF_ARGS);
	}

#undef TRACE_REBINDPROOF_ARGS
#undef TRACE_REBINDPROOF_TEXT

	// Strictly AFTER the verdict — restoring first would erase the state the run exists to report.
	// Every action, every slot, cleared before written: see the same argument in
	// TraceUserSettingsVerify::Restore.
	{
		UTraceUserSettings& Settings = UTraceUserSettings::Get();
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			Settings.ClearKey(Info.Action);
		}

		int32 Flat = 0;
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			for (int32 Slot = 0; Slot < UTraceUserSettings::MaxKeysPerAction; ++Slot, ++Flat)
			{
				if (RebindProofBefore.IsValidIndex(Flat) && RebindProofBefore[Flat].IsValid())
				{
					Settings.SetKey(Info.Action, Slot, RebindProofBefore[Flat]);
				}
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[RebindProof] RESTORED. %s is %s again — the run left no trace in the player's config."),
			TraceInputActions::Info(RebindProofAction).DisplayName,
			*Settings.DescribeBinding(RebindProofAction));
	}
}

bool FTraceOptionsMenu::DebugGetRowRect(const TCHAR* Label, FBox2D& OutRect) const
{
	for (const FRow& Row : Rows)
	{
		// bIsValid, not just a label match: FRow::Rect is only written by DrawRow, so a page that has
		// been rebuilt but not yet drawn would otherwise hand back a zero rect and the harness would
		// click at (0,0) and report a failure that is its own.
		if (Row.Rect.bIsValid && Row.Label.Equals(Label, ESearchCase::IgnoreCase))
		{
			OutRect = Row.Rect;
			return true;
		}
	}
	return false;
}
#endif

// =================================================================================================
// Lifecycle
// =================================================================================================

/**
 * SPEC v28 §3a — the A/B arm for the swallowed-press fix.
 *
 * 0 restores the behaviour exactly as it shipped before v28: the overlay leaves the viewport's
 * capture mode alone, so on the title screen (EMouseCaptureMode::NoCapture) FSceneViewport drops the
 * mouse PRESS and forwards only the release — which is the reported "rebinding needs the button
 * pressed twice". That is the RED arm, and it exists because this project's standing rule is that a
 * harness which cannot go red is not evidence. Trace.Keys.RebindProof prints the viewport's verdict
 * beside every click, so the two arms are distinguishable in one line of log.
 *
 * *** ECVF_Cheat SINCE W9-SHIPGUARD. *** This line used to read "Not ECVF_Cheat: it changes no
 * gameplay rule, only whether a menu click is delivered", which is still an accurate description of
 * what the arm does and was the wrong reason to leave it unflagged. The flag is inert wherever
 * DISABLE_CHEAT_CVARS is 0 (every configuration except Shipping and Test), so the console and
 * -ExecCmds still reach this switch on every build the §3a harness runs on. What it closes is the
 * one path left into a SHIPPED build — Engine.ini's [ConsoleVariables] section, which
 * LoadConsoleVariablesFromINI applies with bAllowCheating = false and which is player-writable in a
 * packaged game. See Trace.Keys.LegacySteal in TraceUserSettings.cpp for the full argument; the two
 * v28 §3 arms are flagged together because they are one feature's A/B.
 */
static int32 GTraceMenuPressDelivery = 1;
static FAutoConsoleVariableRef CVarTraceMenuPressDelivery(
	TEXT("Trace.Menu.PressDelivery"),
	GTraceMenuPressDelivery,
	TEXT("Spec v28 sec 3a. 1 (default): while the settings/pause overlay is open the game viewport is "
	     "put in CaptureDuringMouseDown so a mouse PRESS reaches the game, and the previous mode is "
	     "restored on close. 0 is the RED arm - the pre-v28 behaviour, where the title screen's "
	     "NoCapture mode swallows the first press of every click."),
	ECVF_Cheat);

void FTraceOptionsMenu::SetPressDeliveryOverride(bool bEnable)
{
	if (GEngine == nullptr || GEngine->GameViewport == nullptr)
	{
		return;
	}

	UGameViewportClient& Viewport = *GEngine->GameViewport;

	if (!bEnable)
	{
		// Only ever undo what this class did. A host that was already forwarding presses never had
		// its mode touched, and one that changed the mode underneath us (a travel, another overlay)
		// gets to keep its own answer rather than ours.
		if (bMouseCaptureModeOverridden)
		{
			bMouseCaptureModeOverridden = false;
			Viewport.SetMouseCaptureMode(static_cast<EMouseCaptureMode>(PreviousMouseCaptureMode));
			UE_LOG(LogTraceGame, Display,
				TEXT("[Options] Mouse capture mode put back to %d on close."), PreviousMouseCaptureMode);
		}
		return;
	}

	if (GTraceMenuPressDelivery == 0 || bMouseCaptureModeOverridden)
	{
		return;
	}

	const EMouseCaptureMode Current = Viewport.GetMouseCaptureMode();

	// The three modes FSceneViewport::OnMouseButtonDown already forwards a press under. Touching the
	// mode there would be picking a fight with FInputModeGameAndUI for no gain — and the in-match
	// pause menu, which is one of those, is measured at one click already.
	if (Current == EMouseCaptureMode::CaptureDuringMouseDown
		|| Current == EMouseCaptureMode::CaptureDuringRightMouseDown
		|| Current == EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown)
	{
		return;
	}

	bMouseCaptureModeOverridden = true;
	PreviousMouseCaptureMode = static_cast<uint8>(Current);
	Viewport.SetMouseCaptureMode(EMouseCaptureMode::CaptureDuringMouseDown);

	UE_LOG(LogTraceGame, Display,
		TEXT("[Options] Mouse capture mode was %d, which makes FSceneViewport SWALLOW the press half of "
		     "every click (spec v28 s3a). Raised to CaptureDuringMouseDown for as long as this overlay is "
		     "open; it goes back on close."),
		static_cast<int32>(Current));
}

void FTraceOptionsMenu::OpenRoot()
{
	Page = EPage::Root;
	bSettingsIsRootPage = false;
	bCapturingKey = false;

	// The key press that opened us is still "just pressed" for the rest of this frame. See
	// IgnoreInputBeforeFrame in the header.
	IgnoreInputBeforeFrame = GFrameCounter + 1;

#if !UE_BUILD_SHIPPING
	// See TickAutoActivate: the capture hook is armed per opening, not per process.
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	// THE POINTER'S FIRST SAMPLE ON THIS PAGE IS NOT A MOVE. Forget where it was on the last visit,
	// so a pointer RESTING over some row cannot take the highlight off the row RebuildRows chose — see
	// PollMouse. (Escape then Enter used to "resume" into RETURN TO TITLE or QUIT this way.)
	bHasHoverCursorPos = false;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bLeaving = false;
	Disarm();

	// SPEC v28 §3a — before the first frame the player can click on. See SetPressDeliveryOverride.
	SetPressDeliveryOverride(true);

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Pause menu opened."));
}

void FTraceOptionsMenu::OpenSettings()
{
	Page = EPage::Settings;
	bSettingsIsRootPage = true;
	bCapturingKey = false;
	IgnoreInputBeforeFrame = GFrameCounter + 1;

#if !UE_BUILD_SHIPPING
	// See TickAutoActivate: the capture hook is armed per opening, not per process.
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	// THE POINTER'S FIRST SAMPLE ON THIS PAGE IS NOT A MOVE. Forget where it was on the last visit,
	// so a pointer RESTING over some row cannot take the highlight off the row RebuildRows chose — see
	// PollMouse. (Escape then Enter used to "resume" into RETURN TO TITLE or QUIT this way.)
	bHasHoverCursorPos = false;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bLeaving = false;
	Disarm();

	// SPEC v28 §3a — before the first frame the player can click on. See SetPressDeliveryOverride.
	SetPressDeliveryOverride(true);

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

#if !UE_BUILD_SHIPPING
	// See TickAutoActivate: the capture hook is armed per opening, not per process.
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	// THE POINTER'S FIRST SAMPLE ON THIS PAGE IS NOT A MOVE. Forget where it was on the last visit,
	// so a pointer RESTING over some row cannot take the highlight off the row RebuildRows chose — see
	// PollMouse. (Escape then Enter used to "resume" into RETURN TO TITLE or QUIT this way.)
	bHasHoverCursorPos = false;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bLeaving = false;
	Disarm();

	// SPEC v28 §3a — before the first frame the player can click on. See SetPressDeliveryOverride.
	SetPressDeliveryOverride(true);

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Video settings opened."));
}

void FTraceOptionsMenu::OpenCrosshair()
{
	Page = EPage::Crosshair;
	bCapturingKey = false;

	// Closed, not Settings: this entry point IS the top of the stack, so BACK has to close rather than
	// drop the player onto a settings page they never asked for. Same contract as OpenVideo.
	CrosshairReturnPage = EPage::Closed;
	IgnoreInputBeforeFrame = GFrameCounter + 1;

#if !UE_BUILD_SHIPPING
	// See TickAutoActivate: the capture hook is armed per opening, not per process.
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	// THE POINTER'S FIRST SAMPLE ON THIS PAGE IS NOT A MOVE. Forget where it was on the last visit,
	// so a pointer RESTING over some row cannot take the highlight off the row RebuildRows chose — see
	// PollMouse. (Escape then Enter used to "resume" into RETURN TO TITLE or QUIT this way.)
	bHasHoverCursorPos = false;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bLeaving = false;
	Disarm();

	// SPEC v28 §3a — before the first frame the player can click on. See SetPressDeliveryOverride.
	SetPressDeliveryOverride(true);

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Crosshair settings opened."));
}

void FTraceOptionsMenu::OpenAudio()
{
	Page = EPage::Audio;
	bCapturingKey = false;

	// Closed, not Settings: this entry point IS the top of the stack, so BACK has to close rather than
	// drop the player onto a settings page they never asked for. Same contract as OpenVideo.
	AudioReturnPage = EPage::Closed;
	IgnoreInputBeforeFrame = GFrameCounter + 1;

#if !UE_BUILD_SHIPPING
	// See TickAutoActivate: the capture hook is armed per opening, not per process.
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	// THE POINTER'S FIRST SAMPLE ON THIS PAGE IS NOT A MOVE. Forget where it was on the last visit,
	// so a pointer RESTING over some row cannot take the highlight off the row RebuildRows chose — see
	// PollMouse. (Escape then Enter used to "resume" into RETURN TO TITLE or QUIT this way.)
	bHasHoverCursorPos = false;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bLeaving = false;
	Disarm();

	// SPEC v28 §3a — before the first frame the player can click on. See SetPressDeliveryOverride.
	SetPressDeliveryOverride(true);

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Audio settings opened."));
}

#if !UE_BUILD_SHIPPING
void FTraceOptionsMenu::DebugOpenLoadoutEditor(int32 SlotIndex)
{
	OpenSettings();
	LoadoutsReturnPage = Page;
	Page = EPage::Loadouts;
	IgnoreInputBeforeFrame = GFrameCounter + 1;
	RebuildRows();
	LoadoutEditor.OpenLibrary(SlotIndex);
}
#endif

void FTraceOptionsMenu::OpenController()
{
	Page = EPage::Controller;
	bCapturingKey = false;
	bCapturingPadKey = false;

	// Closed, not Settings: this entry point IS the top of the stack, so BACK has to close rather than
	// drop the player onto a settings page they never asked for. Same contract as OpenVideo.
	ControllerReturnPage = EPage::Closed;
	IgnoreInputBeforeFrame = GFrameCounter + 1;

#if !UE_BUILD_SHIPPING
	// See TickAutoActivate: the capture hook is armed per opening, not per process.
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	// THE POINTER'S FIRST SAMPLE ON THIS PAGE IS NOT A MOVE. Forget where it was on the last visit,
	// so a pointer RESTING over some row cannot take the highlight off the row RebuildRows chose — see
	// PollMouse. (Escape then Enter used to "resume" into RETURN TO TITLE or QUIT this way.)
	bHasHoverCursorPos = false;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bLeaving = false;
	Disarm();

	// SPEC v28 §3a — before the first frame the player can click on. See SetPressDeliveryOverride.
	SetPressDeliveryOverride(true);

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Controller settings opened."));
}

void FTraceOptionsMenu::OpenKeyboard()
{
	Page = EPage::Keyboard;
	bCapturingKey = false;
	bCapturingPadKey = false;

	// Closed, not Settings: this entry point IS the top of the stack. Same contract as OpenController.
	KeyboardReturnPage = EPage::Closed;
	IgnoreInputBeforeFrame = GFrameCounter + 1;

#if !UE_BUILD_SHIPPING
	DrawsSinceOpen = 0;
	bAutoActivateDone = false;
#endif
	bHasHoverCursorPos = false;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bLeaving = false;
	Disarm();
	SetPressDeliveryOverride(true);

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Keyboard binds opened."));
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
	// D31-PAD — reset with its partner and never on its own. A stale bCapturingPadKey would make the
	// NEXT capture walk the pad list and refuse every key the player pressed on the keybind page.
	bCapturingPadKey = false;
	CapturingAction = ETraceInputAction::Count;

	// UI PLAN WP2.2 — an abandoned edit is an abandoned edit. Ending the field rather than committing
	// it is deliberate: the player closed the menu, which is not the gesture that means "save this
	// name". Nothing is lost that was not lost by the same rule for a half-finished rebind two lines
	// up, and the stored call sign is untouched.
	CallSignEntry.End();
	// SPEC v28 §3c — the chip column is part of "where the player was", so it resets with everything
	// else. Re-opening the page on the second chip of a row nobody is looking at would be a surprise.
	CapturingSlot = 0;
	SelectedBindingSlot = 0;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bAutoDetectPending = false;
	LastAdjustDir = 0;
	LastNavDir = 0;
	bHasHoverCursorPos = false;
	bLeaving = false;
	Disarm();

	// The loadout editor is part of this overlay: whatever closed the overlay closes it too, or it would
	// still be "open" (and drawing, and reading keys) the next time a page is.
	if (LoadoutEditor.IsLibraryOpen())
	{
		LoadoutEditor.CloseLibrary();
	}
	bLoadoutEditorWasOpen = false;

	// SPEC v28 §3a — the viewport goes back exactly as it was, BEFORE OnClosed fires. The host's own
	// callback can put a whole new input mode on (ATraceHUD's does), and it must be the one that wins.
	SetPressDeliveryOverride(false);

	UE_LOG(LogTraceGame, Display, TEXT("[Options] Closed."));

	if (OnClosed)
	{
		OnClosed();
	}
}

// =================================================================================================
// Rows
// =================================================================================================

// *** SPEC v29 §5 — THE `TraceOptionsBindingRowLabel` OVERRIDE IS DELETED. THE TABLE IS THE LABEL. ***
//
// It existed for one pass. Spec v28 §10 remapped 1 and 2 onto PISTOL and SMG behind two ConfigIds
// still named "EquipKnife"/"EquipGun", and rather than rename them (which costs every returning
// player that bind) the page overrode their DisplayName with "WEAPON 1 (PISTOL)" / "WEAPON 2 (SMG)"
// from the live dual-wield cvar.
//
// v29 §5 shifts the layout to 1 = STOW GUNS, 2 = PISTOL, 3 = SMG, and the §5 slice has already
// migrated the ConfigIds ("StowGuns"/"EquipPistol"/"EquipSmg") and written the correct DisplayNames
// into TraceInputActions::All(). The override was therefore printing the PREVIOUS pass's layout over
// the top of the current one: row 12 read "WEAPON 1 (PISTOL)" for what is now STOW GUNS, and row 13
// read "WEAPON 2 (SMG)" for what is now PISTOL — i.e. every weapon row on the keybind page named the
// wrong key. Deleting it is the entire fix; the strings below come straight from the table, which is
// the one place they are maintained.
//
// *** SPEC v31 §1 MOVED THE LAYOUT AGAIN AND THE DELETION STILL HOLDS — WHICH IS THE POINT. ***
// bDualWieldKnife is now OFF, the STOW state is gone, and the keys are 1 = PISTOL, 2 = SMG,
// 3 = KNIFE, with the ConfigIds migrated a second time ("StowGuns" -> "KnifeSlot", "EquipPistol" ->
// "PistolSlot", "EquipSmg" -> "SmgSlot") and the DisplayNames rewritten to KNIFE / PISTOL / SMG in
// TraceInputActions::All(). This page read every one of those changes without a line of its own
// changing, because it reads the table. Had the override survived v29 it would now be printing a
// THIRD stale layout over the top. (The two sentences that stood here before the v31 integration
// pass claimed the 1 key still selects the blade and that "STOW GUNS (KNIFE ONLY)" describes it —
// both were false the moment the switch was flipped, and neither was load-bearing.)

void FTraceOptionsMenu::RebuildRows(EAction SelectAction, int32 SelectSlot)
{
	Rows.Reset();

	// A new page, or the same page rebuilt: nothing stays armed across it.
	Disarm();

	// A HEADER OR A NOTE WHOSE WORDS THE DOCUMENT REMOVED IS NOT A ROW. "KEY =" is how a line is
	// taken off the screen (TraceGameText.h), and a note row with no words in it is still a full row
	// pitch of nothing — exactly the gap the removal was meant to close. The blank spacer row a page
	// WANTS (above RESET / BACK) is AddSpacer, so the two can never be confused.
	auto AddHeader = [this](const TCHAR* Label)
	{
		if (Label == nullptr || *Label == TEXT('\0'))
		{
			return;
		}
		FRow Row;
		Row.Kind = ERowKind::Header;
		Row.Label = Label;
		Rows.Add(MoveTemp(Row));
	};

	auto AddSpacer = [this]()
	{
		FRow Row;
		Row.Kind = ERowKind::Header;
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
		if (Label == nullptr || *Label == TEXT('\0'))
		{
			return;
		}
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
		if (OnResume)         { AddAction(*TRACE_TEXT("OPTIONS.PAUSE.RESUME", "RESUME"), EAction::Resume); }
		AddAction(*TRACE_TEXT("OPTIONS.PAUSE.SETTINGS", "SETTINGS"), EAction::OpenSettings);

		// Its own row on the pause root rather than only inside SETTINGS. Spec v11 §0: the player
		// this feature exists for is one whose frame rate has collapsed, and making them walk past
		// mouse sensitivity and eleven key bindings to reach the resolution scale is exactly the kind
		// of burial that left the collaborator with no way to improve anything.
		AddAction(*TRACE_TEXT("OPTIONS.PAUSE.VIDEO", "VIDEO"), EAction::OpenVideo);

		if (OnReturnToTitle)  { AddAction(*TRACE_TEXT("OPTIONS.PAUSE.RETURN_TO_TITLE", "RETURN TO TITLE"), EAction::ReturnToTitle); }
		if (OnQuit)           { AddAction(*TRACE_TEXT("OPTIONS.PAUSE.QUIT", "QUIT"), EAction::Quit); }
	}
	else if (Page == EPage::Video)
	{
		// ---- Performance first ------------------------------------------------------------------
		//
		// The order on this page is an argument, not a taxonomy. Spec v11 §0 measured the frame as
		// GPU-bound PER PIXEL — instancing the arena removed 893 draw calls and bought 1.4% — so the
		// number of pixels is the dominant term and the two controls that change it come first,
		// above the mode, the resolution and all nine quality groups.
		AddHeader(*TRACE_TEXT("OPTIONS.VIDEO.HDR_PERFORMANCE", "PERFORMANCE"));
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.VIDEO.ROW_RESOLUTION_SCALE", "RESOLUTION SCALE"), ESetting::ResolutionScale);
		AddAction(*TRACE_TEXT("OPTIONS.VIDEO.ROW_AUTO_DETECT_QUALITY", "AUTO-DETECT QUALITY"), EAction::AutoDetectQuality);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_OVERALL_QUALITY", "OVERALL QUALITY"), ESetting::OverallQuality);

		// ---- Display ----------------------------------------------------------------------------
		AddHeader(*TRACE_TEXT("OPTIONS.VIDEO.HDR_DISPLAY", "DISPLAY"));
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_WINDOW_MODE", "WINDOW MODE"), ESetting::WindowMode);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_RESOLUTION", "RESOLUTION"), ESetting::Resolution);
		AddValue(ERowKind::Toggle, *TRACE_TEXT("OPTIONS.VIDEO.ROW_VSYNC", "VSYNC"), ESetting::VSync);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_FRAME_RATE_LIMIT", "FRAME RATE LIMIT"), ESetting::FrameRateLimit);
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.VIDEO.ROW_FIELD_OF_VIEW", "FIELD OF VIEW"), ESetting::FieldOfView);

		// ---- The nine groups --------------------------------------------------------------------
		AddHeader(*TRACE_TEXT("OPTIONS.VIDEO.HDR_QUALITY", "QUALITY"));
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_VIEW_DISTANCE", "VIEW DISTANCE"), ESetting::QualityViewDistance);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_ANTI_ALIASING", "ANTI-ALIASING"), ESetting::QualityAntiAliasing);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_POST_PROCESSING", "POST PROCESSING"), ESetting::QualityPostProcess);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_SHADOWS", "SHADOWS"), ESetting::QualityShadows);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_GLOBAL_ILLUMINATION", "GLOBAL ILLUMINATION"), ESetting::QualityGlobalIllumination);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_REFLECTIONS", "REFLECTIONS"), ESetting::QualityReflections);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_TEXTURES", "TEXTURES"), ESetting::QualityTextures);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_EFFECTS", "EFFECTS"), ESetting::QualityEffects);
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.VIDEO.ROW_SHADING", "SHADING"), ESetting::QualityShading);

		AddSpacer();
		AddAction(*TRACE_TEXT("OPTIONS.ROW.RESET_TO_DEFAULTS", "RESET TO DEFAULTS"), EAction::ResetVideoDefaults);
		AddAction(*TRACE_TEXT("OPTIONS.ROW.BACK", "BACK"), EAction::Back);
	}
	else if (Page == EPage::Loadouts)
	{
		// FIVE ROWS, ONE PER SAVED SLOT, and each says what it holds rather than just its number — a
		// library where every row reads "LOADOUT 3" is a library the player has to open five times to
		// read. A filled slot shows its three abilities; an empty one says so.
		// NO "YOUR SAVED LOADOUTS" CAPTION: it sat directly under the page title LOADOUTS and said it
		// again.
		const UTraceUserSettings& Settings = UTraceUserSettings::Get();
		for (int32 Index = 0; Index < UTraceUserSettings::SavedLoadoutCount; ++Index)
		{
			const FTraceLoadout Saved = Settings.GetSavedLoadout(Index);

			FString Summary;
			if (Saved.IsEmpty())
			{
				Summary = TRACE_TEXT("OPTIONS.LOADOUTS.EMPTY", "EMPTY");
			}
			else
			{
				// ShortLabel, not Get: only the ACTIVATED ability has a name, so Get returns empty for
				// the other two and this row printed " /  / RIPPLE" — three abilities described by
				// one. ShortLabel falls back to the opening words of what an ability does.
				Summary = FString::Printf(TEXT("%s / %s / %s"),
					*TraceAbilityNames::ShortLabel(Saved.Movement, 22),
					*TraceAbilityNames::ShortLabel(Saved.Passive, 22),
					*TraceAbilityNames::ShortLabel(Saved.Activated, 22));
			}

			FRow Row;
			Row.Kind = ERowKind::Action;
			Row.Label = FString::Printf(TEXT("%d.  %s"), Index + 1, *Summary);
			Row.Action = EAction::EditLoadoutSlot;
			Row.SlotIndex = Index;
			Rows.Add(MoveTemp(Row));
		}

		AddSpacer();
		AddAction(*TRACE_TEXT("OPTIONS.ROW.BACK", "BACK"), EAction::Back);
	}
	else if (Page == EPage::Crosshair)
	{
		// ---- SPEC v29 §3 — SHAPE FIRST, THEN INK ------------------------------------------------
		//
		// The order is the order a player actually decides in, and it is not the order the fields are
		// declared in. Size, thickness and gap are the SHAPE — they are what the player is here to
		// change, they are the three that interact with each other (a big gap with short arms is a
		// different instrument from a small gap with long ones), and every one of them is visible in
		// the preview the moment it moves. Colour, opacity, dot and outline are then decisions about
		// the shape you have already settled on.
		//
		// The two toggles go LAST and together, because they are the two rows that answer "is there
		// LESS of it" rather than "how much".
		AddHeader(*TRACE_TEXT("OPTIONS.CROSSHAIR.HDR_SHAPE", "SHAPE"));
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.CROSSHAIR.ROW_SIZE", "SIZE"), ESetting::CrosshairSize);
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.CROSSHAIR.ROW_THICKNESS", "THICKNESS"), ESetting::CrosshairThickness);
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.CROSSHAIR.ROW_GAP", "GAP"), ESetting::CrosshairGap);

		AddHeader(*TRACE_TEXT("OPTIONS.CROSSHAIR.HDR_APPEARANCE", "APPEARANCE"));
		AddValue(ERowKind::Choice, *TRACE_TEXT("OPTIONS.CROSSHAIR.ROW_COLOUR", "COLOUR"), ESetting::CrosshairColor);
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.CROSSHAIR.ROW_OPACITY", "OPACITY"), ESetting::CrosshairOpacity);
		AddValue(ERowKind::Toggle, *TRACE_TEXT("OPTIONS.CROSSHAIR.ROW_CENTRE_DOT", "CENTRE DOT"), ESetting::CrosshairDot);
		AddValue(ERowKind::Toggle, *TRACE_TEXT("OPTIONS.CROSSHAIR.ROW_OUTLINE", "OUTLINE"), ESetting::CrosshairOutline);

		AddSpacer();
		AddAction(*TRACE_TEXT("OPTIONS.ROW.RESET_TO_DEFAULTS", "RESET TO DEFAULTS"), EAction::ResetCrosshairDefaults);
		AddAction(*TRACE_TEXT("OPTIONS.ROW.BACK", "BACK"), EAction::Back);
	}
	else if (Page == EPage::Audio)
	{
		// ---- UI PLAN WP3 — LOUD TO QUIET, MASTER FIRST ------------------------------------------
		//
		// The order is the order a player decides in and the order the values compose in: MASTER
		// multiplies both of the two under it, so it reads as the parent it is. EFFECTS before MUSIC
		// because effects are the game — a player who cannot hear the shot that killed them has a
		// problem, a player who finds the bed loud has a preference.
		AddHeader(*TRACE_TEXT("OPTIONS.AUDIO.HDR_VOLUME", "VOLUME"));
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.AUDIO.ROW_MASTER_VOLUME", "MASTER VOLUME"), ESetting::MasterVolume);
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.AUDIO.ROW_SOUND_EFFECTS", "SOUND EFFECTS"), ESetting::SfxVolume);
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.AUDIO.ROW_MUSIC", "MUSIC"), ESetting::MusicVolume);

		// NO "NO MUSIC YET" NOTE. The UI plan's draft carried one — deliberate honesty about a silent
		// slider — and the integration pass struck it, because music DOES ship this release: the
		// title loop, the match ambience bed and the two end stingers all exist as imported assets
		// (FX_AUDIO_PLAN §5.7). A note explaining an absence that is not there would be the exact
		// mistake the original note was written to avoid, one release later.

		AddSpacer();
		AddAction(*TRACE_TEXT("OPTIONS.ROW.RESET_TO_DEFAULTS", "RESET TO DEFAULTS"), EAction::ResetAudioDefaults);
		AddAction(*TRACE_TEXT("OPTIONS.ROW.BACK", "BACK"), EAction::Back);
	}
	else if (Page == EPage::Controller)
	{
		// ---- D31-PAD — THE STICKS FIRST, THEN THE BUTTONS ---------------------------------------
		//
		// The order is the order a player who has just plugged a pad in needs. A controller that is
		// bound perfectly and whose look stick is too fast, too slow or drifting is a controller the
		// owner will report as broken — so the analog feel comes first, above every bind, and the
		// binds are what a player scrolls down to once the thing feels right.
		//
		// MOVE AND LOOK HAVE NO BIND ROWS AT ALL. They are the two sticks, they are not per-action
		// buttons, and there is nothing to rebind about them but the numbers immediately below — the
		// LOOK STICK / MOVE STICK headers say which is which. (A note spelling that out, and one
		// explaining the dead zone, were removed by the co-developer's text pass.)
		// No CONTROLLER caption over this first row: the page is titled CONTROLLER directly above it.
		AddValue(ERowKind::Toggle, *TRACE_TEXT("OPTIONS.CONTROLLER.ROW_CONTROLLER_INPUT", "CONTROLLER INPUT"), ESetting::PadEnabled);

		AddHeader(*TRACE_TEXT("OPTIONS.CONTROLLER.HDR_LOOK_STICK", "LOOK STICK"));
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.CONTROLLER.ROW_LOOK_SPEED", "LOOK SPEED"),          ESetting::PadLookRate);
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.CONTROLLER.ROW_VERTICAL_SPEED", "VERTICAL SPEED"),      ESetting::PadLookYScale);
		AddValue(ERowKind::Toggle, *TRACE_TEXT("OPTIONS.CONTROLLER.ROW_INVERT_LOOK_Y", "INVERT LOOK Y"),       ESetting::PadInvertY);
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.CONTROLLER.ROW_LOOK_DEAD_ZONE", "LOOK DEAD ZONE"),      ESetting::PadLookDeadzone);

		AddHeader(*TRACE_TEXT("OPTIONS.CONTROLLER.HDR_MOVE_STICK", "MOVE STICK"));
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.CONTROLLER.ROW_MOVE_DEAD_ZONE", "MOVE DEAD ZONE"),      ESetting::PadMoveDeadzone);

		AddHeader(*TRACE_TEXT("OPTIONS.CONTROLLER.HDR_BUTTONS", "BUTTONS"));

		// EVERY ACTION EXCEPT THE FOUR KEYBOARD MOVE ROWS. Walking the shared table rather than a
		// hand-written list, so an action added to ETraceInputAction gets a row on this page for
		// free — the same argument the CONTROLS block on the settings page makes.
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			if (Info.Action == ETraceInputAction::MoveForward || Info.Action == ETraceInputAction::MoveBack
				|| Info.Action == ETraceInputAction::MoveLeft || Info.Action == ETraceInputAction::MoveRight)
			{
				continue;
			}

			FRow Row;
			Row.Kind = ERowKind::PadBinding;
			Row.Label = TraceOptionsMenuFile::ActionLabel(Info);   // one label per verb, shared with the keyboard page
			Row.Binding = Info.Action;
			Rows.Add(MoveTemp(Row));
		}

		AddSpacer();
		AddAction(*TRACE_TEXT("OPTIONS.ROW.RESET_TO_DEFAULTS", "RESET TO DEFAULTS"), EAction::ResetControllerDefaults);
		AddAction(*TRACE_TEXT("OPTIONS.ROW.BACK", "BACK"), EAction::Back);
	}
	else if (Page == EPage::Keyboard)
	{
		// EVERY ACTION, TWO KEY CHIPS EACH — the rows that used to fill the bottom half of SETTINGS.
		// Walking the shared table rather than a hand-written list, so an action added to
		// ETraceInputAction gets a row here for free. The first row captions the two columns (KEYBIND
		// over the labels, KEY over the chips) instead of carrying a word of its own.
		{
			FRow Captions;
			Captions.Kind = ERowKind::Header;
			Captions.Label = TRACE_TEXT("OPTIONS.SETTINGS.COL_KEYBIND", "KEYBIND");
			Captions.bColumnCaptions = true;
			Rows.Add(MoveTemp(Captions));
		}

		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			FRow Row;
			Row.Kind = ERowKind::Binding;
			Row.Label = TraceOptionsMenuFile::ActionLabel(Info);
			Row.Binding = Info.Action;
			Rows.Add(MoveTemp(Row));
		}

		AddSpacer();
		AddAction(*TRACE_TEXT("OPTIONS.ROW.RESET_TO_DEFAULTS", "RESET TO DEFAULTS"), EAction::ResetKeyboardDefaults);
		AddAction(*TRACE_TEXT("OPTIONS.ROW.BACK", "BACK"), EAction::Back);
	}
	else if (Page == EPage::Settings)
	{
		// ---- A SHORT PAGE: WHO YOU ARE, WHERE TO GO, THE MATCH SWITCH, THE MOUSE --------------------
		//
		// It was forty-three rows — five one-row section captions, six doors, three notes, the mouse and
		// all twenty keybinds — with no scrolling, so the pitch clamped to ~19 px at 1080p and the labels
		// shrank to 6-7 px caps (4-5 px at 720p). The keybinds are their own KEYBOARD page now, the six
		// doors are one uncaptioned group, and every door is labelled with the title of the page it
		// opens (the same text key, so a door and its page can never be called two different things).
		//
		// THE CALL SIGN FIRST, for the reason UI plan WP2 gave: the first thing a player opening
		// SETTINGS should be able to fix is the thing with their name on it. Its note names the cap,
		// the one refusal the field makes that a player could not otherwise see.
		AddValue(ERowKind::TextEntry, *TRACE_TEXT("OPTIONS.SETTINGS.ROW_CALL_SIGN", "CALL SIGN"), ESetting::CallSign);
		AddNote(*TRACE_TEXT("OPTIONS.SETTINGS.NOTE_CALL_SIGN",
			"SHOWN ON THE SCOREBOARD AND IN THE KILL FEED. A-Z, 0-9, SPACE, - _ . MAX 16."));

		// THE DOORS. VIDEO first (spec v11 §0: the player whose frame rate collapsed must not walk past
		// anything to reach it), and this is the ONLY route the title screen has to any of these pages.
		AddAction(*TRACE_TEXT("OPTIONS.TITLE.VIDEO", "VIDEO"), EAction::OpenVideo);
		AddAction(*TRACE_TEXT("OPTIONS.TITLE.AUDIO", "AUDIO"), EAction::OpenAudio);
		AddAction(*TRACE_TEXT("OPTIONS.TITLE.CROSSHAIR", "CROSSHAIR"), EAction::OpenCrosshair);
		AddAction(*TRACE_TEXT("OPTIONS.TITLE.KEYBOARD", "KEYBOARD"), EAction::OpenKeyboard);
		AddAction(*TRACE_TEXT("OPTIONS.TITLE.CONTROLLER", "CONTROLLER"), EAction::OpenController);
		AddAction(*TRACE_TEXT("OPTIONS.TITLE.LOADOUTS", "LOADOUTS"), EAction::OpenLoadouts);

		// ---- Match rules (spec v14 §3) ----------------------------------------------------------
		//
		// The one row on this page that changes what the GAME is rather than how it is driven. A HOST
		// setting, landing on the NEXT match. Labelled ABILITIES: the setting and its key are the old
		// characters switch, and there are no characters left to name.
		AddSpacer();
		AddValue(ERowKind::Toggle, *TRACE_TEXT("OPTIONS.SETTINGS.ROW_CHARACTERS", "ABILITIES"), ESetting::CharactersEnabled);
		AddNote(*TRACE_TEXT("OPTIONS.SETTINGS.NOTE_CHARACTERS_OFF",
			"OFF: EVERYONE PLAYS THE DEFAULT MANNEQUIN, NO ABILITIES, NO LOADOUT SCREEN."));

		AddHeader(*TRACE_TEXT("OPTIONS.SETTINGS.HDR_MOUSE", "MOUSE"));
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.SETTINGS.ROW_SENSITIVITY", "SENSITIVITY"), ESetting::Sensitivity);
		AddValue(ERowKind::Slider, *TRACE_TEXT("OPTIONS.SETTINGS.ROW_VERTICAL_SENSITIVITY", "VERTICAL SENSITIVITY"), ESetting::SensitivityY);
		AddValue(ERowKind::Toggle, *TRACE_TEXT("OPTIONS.SETTINGS.ROW_INVERT_MOUSE_Y", "INVERT MOUSE Y"), ESetting::InvertY);

		AddSpacer();
		AddAction(*TRACE_TEXT("OPTIONS.ROW.RESET_TO_DEFAULTS", "RESET TO DEFAULTS"), EAction::ResetDefaults);
		AddAction(*TRACE_TEXT("OPTIONS.ROW.BACK", "BACK"), EAction::Back);
	}

	// Before picking a selection, not after: a row that is greyed out right now is not somewhere the
	// highlight may land, and RESOLUTION is the first selectable row on the video page's DISPLAY
	// block whenever the window mode is not windowed fullscreen.
	RefreshRowStates();

	// Land on the row the caller asked for — the door the player came back through — or else on the
	// first thing that can actually be selected, so a page never opens with the highlight on a caption.
	Selected = 0;
	SelectedBindingSlot = 0;   // spec v28 §3c — and on its first chip
	const int32 Wanted = (SelectAction != EAction::None) ? FindActionRow(SelectAction, SelectSlot) : INDEX_NONE;
	if (Wanted != INDEX_NONE && Rows[Wanted].IsSelectable())
	{
		Selected = Wanted;
		return;
	}
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].IsSelectable())
		{
			Selected = Index;
			break;
		}
	}
}

int32 FTraceOptionsMenu::FindActionRow(EAction Action, int32 SlotIndex) const
{
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FRow& Row = Rows[Index];
		if (Row.Kind == ERowKind::Action && Row.Action == Action
			&& (SlotIndex == INDEX_NONE || Row.SlotIndex == SlotIndex))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

FTraceOptionsMenu::EAction FTraceOptionsMenu::DoorFor(EPage Child)
{
	switch (Child)
	{
	case EPage::Settings:   return EAction::OpenSettings;
	case EPage::Video:      return EAction::OpenVideo;
	case EPage::Crosshair:  return EAction::OpenCrosshair;
	case EPage::Loadouts:   return EAction::OpenLoadouts;
	case EPage::Audio:      return EAction::OpenAudio;
	case EPage::Controller: return EAction::OpenController;
	case EPage::Keyboard:   return EAction::OpenKeyboard;
	default:                return EAction::None;
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
	TraceOptionsMenuFile::GActiveOptionsMenu = this;
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

	// ---- THE MENU'S OWN CLOCK — see Now in the header -----------------------------------------------
	//
	// NOT InNow. Both hosts pass UWorld::GetTimeSeconds, which stops dead while the world is paused,
	// and the in-match pause menu pauses the world in standalone. RealTimeSeconds keeps running through
	// a pause, is undilated, and is world-relative, so it stays small enough for the float timers
	// below. (FPlatformTime::Seconds is NOT a substitute: on this platform it carries a deliberately
	// large offset, and narrowed to float it would round every timer and pulse to whole seconds.)
	{
		const UWorld* ClockWorld = (PC != nullptr) ? PC->GetWorld() : nullptr;
		Now = (ClockWorld != nullptr) ? static_cast<float>(ClockWorld->GetRealTimeSeconds()) : InNow;
		bWorldPaused = (ClockWorld != nullptr) && ClockWorld->IsPaused();
	}

#if !UE_BUILD_SHIPPING
	// SPEC v28 §3a. BEFORE the closed-page early-out, because its first job is to OPEN the settings
	// page, and before PollInput below, because an edge it injects must not be read by the same frame
	// that queued it — see the block comment on TickRebindProof. Inert until armed.
	TickRebindProof(PC);

	// Trace.Menu.Verify: the same shape, for the same reasons. Inert until armed.
	TickVerify(PC);
#endif

	if (Page == EPage::Closed || HUD == nullptr || InViewW <= 0.f || InViewH <= 0.f)
	{
		return;
	}

	ViewW = InViewW;
	ViewH = InViewH;
	UIScale = InUIScale;

	// *** THE LOADOUT EDITOR OWNS THE FRAME WHILE IT IS OPEN. ***
	//
	// BEFORE everything below, and it returns rather than falling through: the editor reads arrows and
	// ENTER, and so does this menu's own row list. Both running would move the selection behind the
	// editor while the player builds a loadout. One screen reads the keys at a time. It runs on this
	// menu's clock, so its hover breath and its key repeat keep going in a paused match too.
	if (LoadoutEditor.IsLibraryOpen())
	{
		bLoadoutEditorWasOpen = true;
		LoadoutEditorSlot = LoadoutEditor.GetLibrarySlot();
		LoadoutEditor.TickLibrary(HUD, PC, InViewW, InViewH, InUIScale, Now,
			/*bInputAllowed=*/GFrameCounter >= IgnoreInputBeforeFrame);
		if (LoadoutEditor.IsLibraryOpen())
		{
			return;
		}
	}

	// THE EDITOR JUST CLOSED — by ENTER, by BACK (key, pad B or a click on its back mark), or by code.
	// Watched as an EDGE rather than read off TickLibrary's return, so no way out of the editor can
	// skip it. The rows are rebuilt so the slot shows what was just saved, with the highlight back on
	// THAT slot (it used to land on slot 1), and the page is drawn this same frame rather than leaving
	// one frame with nothing on screen.
	if (bLoadoutEditorWasOpen)
	{
		bLoadoutEditorWasOpen = false;

		// TWO frames, not one: the editor leaves on pad B, and TracePadMenu documents that one physical
		// press can be reported on two consecutive frames. A B seen again here would run GoBack and take
		// the player off the LOADOUTS page as well.
		IgnoreInputBeforeFrame = GFrameCounter + 2;
		RebuildRows(EAction::EditLoadoutSlot, LoadoutEditorSlot);

		// ...and the pointer, which may be resting over another slot, must not take the highlight back.
		bHasHoverCursorPos = false;
	}

	// RETURN TO TITLE / QUIT has been pressed and the host is taking the HUD away. No input; the page
	// stays on screen with the pressed row pressed. See bLeaving.
	if (bLeaving)
	{
		if (FPlatformTime::Seconds() - LeavingSinceReal > LeaveTimeoutSeconds)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Options] Still here %.0fs after leaving was requested; closing the overlay normally."),
				LeaveTimeoutSeconds);
			bLeaving = false;
			Close();
			return;
		}
		Draw(HUD, PC);
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

	// Before input, so a click cannot land on a row that stopped being meaningful last frame.
	RefreshRowStates();

	// UI PLAN WP2.2 — THE FIELD GETS THE KEYBOARD BEFORE THIS CLASS DOES.
	//
	// FTraceTextEntry's header states the rule for its hosts — "the host must not route its own
	// bindings while IsActive()" — and this class is a host exactly as the title screen is. It is not
	// a nicety here: the arrows are the caret's, Enter is submit, Escape is cancel and Backspace is a
	// deletion, and every one of those four is ALSO a control on this page. Both readers would fire.
	//
	// It returns true for the whole edit, including the frame that ends it, so the Enter that
	// committed the name cannot also activate the row underneath it.
	if (!TickCallSignEntry(PC))
	{
		// Input first, then draw, so a value changed this frame is the value the player sees this
		// frame. The row rects the mouse tests against are from the PREVIOUS draw, which is correct:
		// they are where the player was actually looking when they clicked.
		PollInput(PC);
	}

	// PollInput can close us (Escape, RESUME). Drawing a closed overlay would leave a frame of dimmed
	// screen over a game that has already resumed.
	if (Page == EPage::Closed)
	{
		return;
	}

	// An armed RESET / CLEAR lasts while the highlight stays on its row and the window is open.
	if (ArmedAction != EAction::None
		&& (FPlatformTime::Seconds() > ArmedUntilReal || !Rows.IsValidIndex(Selected) || !IsArmedRow(Rows[Selected])))
	{
		Disarm();
	}

	// The coalesced window resize, once the player has stopped moving through the list.
	if (bResolutionApplyPending && Now >= ResolutionApplyAtTime)
	{
		ApplyVideo(/*bResolutionAffecting=*/true, /*bPersist=*/true);
		RefreshRowStates();
	}

#if !UE_BUILD_SHIPPING
	// After input and before the draw, so the page a capture photographs is the page this frame
	// actually drew. Counted in DRAWN FRAMES - see the header for why seconds cannot work here.
	++DrawsSinceOpen;
	TickAutoActivate();

	// It presses a real row, and a real row can be BACK or QUIT. Same guard, same reason, as the one
	// after PollInput above.
	if (Page == EPage::Closed)
	{
		return;
	}
#endif

	Draw(HUD, PC);
}

#if !UE_BUILD_SHIPPING
void FTraceOptionsMenu::TickAutoActivate()
{
	// Parsed once per process. FParse over the whole command line every frame in front of a paused
	// match would be its own small crime.
	static bool bParsed = false;
	static FString WantedRow;
	if (!bParsed)
	{
		bParsed = true;
		FParse::Value(FCommandLine::Get(), TEXT("TraceMenuActivate="), WantedRow);
	}

	// Twelve drawn frames: long enough that the opening frame's IgnoreInputBeforeFrame guard and the
	// first layout have both passed, and comfortably inside ATraceHUD's own auto-pause capture, which
	// fires twenty draws after the menu appears. Order matters - the page has to change BEFORE the
	// screenshot, or the capture is of the root again.
	if (WantedRow.IsEmpty() || bAutoActivateDone || DrawsSinceOpen < 12)
	{
		return;
	}
	bAutoActivateDone = true;

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].IsSelectable() && Rows[Index].Label.Equals(WantedRow, ESearchCase::IgnoreCase))
		{
			Selected = Index;
			UE_LOG(LogTraceGame, Display,
				TEXT("[Options] -TraceMenuActivate: pressing row %d ('%s')."), Index, *Rows[Index].Label);

			// Through the real activation, not around it. A harness that set Page directly would
			// photograph a page no key press can reach.
			ActivateSelected();
			return;
		}
	}

	UE_LOG(LogTraceGame, Warning,
		TEXT("[Options] -TraceMenuActivate=%s: no selectable row on this page carries that label."),
		*WantedRow);
}
#endif

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

int32 FTraceOptionsMenu::ActiveBindingSlot() const
{
	return FMath::Clamp(SelectedBindingSlot, 0, UTraceUserSettings::MaxKeysPerAction - 1);
}

void FTraceOptionsMenu::PollKeyCapture(APlayerController* PC)
{
	// Escape is filtered out of BindableKeys precisely so it can mean "cancel" here and nothing else.
	if (PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		bCapturingKey = false;
		bCapturingPadKey = false;
		CapturingAction = ETraceInputAction::Count;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Rebind cancelled."));
		return;
	}

	// D31-PAD — a pad capture is cancelled by MENU/START as well as by Escape, because a player who
	// opened this page with a controller may have no keyboard within reach. It is the ONE pad button
	// the layout leaves unclaimed (Trace.Pad.Verify asserts that), which is exactly what makes it
	// available to mean "cancel" here and "pause" everywhere else.
	if (bCapturingPadKey && PC->WasInputKeyJustPressed(EKeys::Gamepad_Special_Right))
	{
		bCapturingKey = false;
		bCapturingPadKey = false;
		CapturingAction = ETraceInputAction::Count;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Controller rebind cancelled (MENU/START)."));
		return;
	}

	// The list this capture walks. THE TWO ARE DISJOINT: BindableKeys() holds no pad button and
	// PadBindableKeys() holds nothing else, so a keyboard key pressed over a pad row is ignored and
	// a pad button pressed over a keybind row is ignored. That partition is what stops one physical
	// button from landing in both tables and therefore in both mapping contexts.
	const TArray<FKey>& CaptureList = bCapturingPadKey ? TraceOptionsMenuFile::PadBindableKeys()
		: TraceOptionsMenuFile::BindableKeys();

	// First poll of a fresh capture: record what was already down. A key that was held before the
	// capture existed was never a choice made inside it.
	if (bCaptureNeedsHeldSnapshot)
	{
		bCaptureNeedsHeldSnapshot = false;
		KeysHeldWhenCaptureOpened.Reset();
		for (const FKey& Held : CaptureList)
		{
			if (PC->IsInputKeyDown(Held))
			{
				KeysHeldWhenCaptureOpened.Add(Held);
			}
		}
	}

	// Retire held-at-open keys as they come up, so the player's NEXT real press counts.
	for (int32 Index = KeysHeldWhenCaptureOpened.Num() - 1; Index >= 0; --Index)
	{
		if (!PC->IsInputKeyDown(KeysHeldWhenCaptureOpened[Index]))
		{
			KeysHeldWhenCaptureOpened.RemoveAtSwap(Index);
		}
	}

	for (const FKey& Key : CaptureList)
	{
		if (!PC->WasInputKeyJustPressed(Key))
		{
			continue;
		}

		// Was down before this capture existed, and has not been released since. Not a choice.
		if (KeysHeldWhenCaptureOpened.Contains(Key))
		{
			continue;
		}

		// SPEC v28 §3c — into the SLOT the player was pointing at, not always the first one.
		const ETraceInputAction Action = CapturingAction;
		const int32 Slot = FMath::Clamp(CapturingSlot, 0, UTraceUserSettings::MaxKeysPerAction - 1);
		const bool bPad = bCapturingPadKey;

		// *** SPEC v28 §3a — CLOSE THE CAPTURE BEFORE THE WRITE, NOT AFTER IT. ***
		//
		// SetKey below calls Save(), Save() broadcasts UTraceUserSettings::OnChanged, and
		// ATracePlayerController::ApplyControlSettings runs on that broadcast — SYNCHRONOUSLY, inside
		// this call. That is a lot of code to run while this object still says "I am waiting for a key",
		// and every line of it is a line that could re-enter the menu. Clearing the flags first makes
		// the capture over at the moment the key is decided, which is also what the player is told by
		// the row: the chip stops flashing PRESS A KEY on the same frame their key lands in it.
		bCapturingKey = false;
		bCapturingPadKey = false;
		CapturingAction = ETraceInputAction::Count;
		KeysHeldWhenCaptureOpened.Reset();
		bCaptureNeedsHeldSnapshot = false;

		if (bPad)
		{
			// D31-PAD — SetPadKey saves, and the save re-applies the pad's mapping context through
			// UTraceUserSettings::OnChanged, so the new button drives the game on the very next frame.
			// That is the same synchronous path the line below relies on for the keyboard.
			UTraceUserSettings::Get().SetPadKey(Action, Key);
			UE_LOG(LogTraceGame, Display, TEXT("[Options] Bound %s to controller '%s'."),
				TraceInputActions::Info(Action).DisplayName, *UTraceUserSettings::DescribePadKey(Key));
		}
		else
		{
			UTraceUserSettings::Get().SetKey(Action, Slot, Key);
			UE_LOG(LogTraceGame, Display, TEXT("[Options] Bound %s slot %d to '%s'. The action now holds %s."),
				TraceInputActions::Info(Action).DisplayName, Slot + 1, *UTraceUserSettings::DescribeKey(Key),
				*UTraceUserSettings::Get().DescribeBinding(Action));
		}

		// One more frame of quiet: the key that was just bound is still down, and if it happens to be
		// Enter or a mouse button the very next poll would read it as "activate this row again".
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		return;
	}
}

void FTraceOptionsMenu::PollNavigation(APlayerController* PC)
{
	// ---- D31-PAD — THE PAD DRIVES THIS OVERLAY TOO ----------------------------------------------
	//
	// Without this the controller settings page is a page a controller cannot reach: the pad opens the
	// pause menu (UTraceGamepadInputSubsystem turns MENU/START into Escape) and then cannot move the
	// highlight off RESUME. "Instead of a mouse or trackpad" is in the owner's sentence, so the
	// overlay has to be operable with nothing but the pad.
	//
	// THE STICK IS READ AS ITS FOUR DIGITAL KEYS (Gamepad_LeftStick_Up and friends), not as an axis.
	// The engine synthesises those from the stick with its own threshold, which means the repeat
	// logic below — written for a key that is either down or not — works unchanged, and a stick held
	// at half deflection does not scroll the list at half speed.
	//
	// NO CONFLICT WITH GAMEPLAY. ATracePlayerController::SetGameInputSuppressed makes every gameplay
	// handler early-return while this overlay is open, which is exactly what ETraceInputStates::Menu's
	// comment in Settings/TraceUserSettings.h says: a match action and the menu can never contend. So
	// the A button being both "select" here and JUMP in a match is not an overload.
	auto PadDown = [PC](const FKey& A, const FKey& B)
	{
		return PC->IsInputKeyDown(A) || PC->IsInputKeyDown(B)
			|| PC->WasInputKeyJustPressed(A) || PC->WasInputKeyJustPressed(B);
	};

	// ---- Vertical: move the selection -----------------------------------------------------------
	int32 NavDir = 0;
	if (TraceOptionsMenuFile::AnyDown(PC, EKeys::Down, EKeys::S)) { NavDir += 1; }
	if (TraceOptionsMenuFile::AnyDown(PC, EKeys::Up,   EKeys::W)) { NavDir -= 1; }
	if (PadDown(EKeys::Gamepad_DPad_Down, EKeys::Gamepad_LeftStick_Down)) { NavDir += 1; }
	if (PadDown(EKeys::Gamepad_DPad_Up,   EKeys::Gamepad_LeftStick_Up))   { NavDir -= 1; }
	NavDir = FMath::Clamp(NavDir, -1, 1);

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
	if (TraceOptionsMenuFile::AnyDown(PC, EKeys::Right, EKeys::D)) { AdjustDir += 1; }
	if (TraceOptionsMenuFile::AnyDown(PC, EKeys::Left,  EKeys::A)) { AdjustDir -= 1; }
	if (PadDown(EKeys::Gamepad_DPad_Right, EKeys::Gamepad_LeftStick_Right)) { AdjustDir += 1; }
	if (PadDown(EKeys::Gamepad_DPad_Left,  EKeys::Gamepad_LeftStick_Left))  { AdjustDir -= 1; }
	AdjustDir = FMath::Clamp(AdjustDir, -1, 1);

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
	// A is SELECT, which is the convention on every pad. Nothing else needs saying — the choke-point
	// comment in ActivateSelected has claimed "the gamepad face button" funnels here since spec v26
	// §9, and as of D31-PAD that is finally true rather than aspirational.
	if (PC->WasInputKeyJustPressed(EKeys::Enter) || PC->WasInputKeyJustPressed(EKeys::SpaceBar)
		|| PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Bottom))
	{
		ActivateSelected();
		return;
	}

	// B is BACK. Escape reaches here from the keyboard and, on a pad, from MENU/START by way of the
	// synthetic Escape UTraceGamepadInputSubsystem injects — so a pad has two ways back, which is
	// right: B is what a player's thumb reaches for, and MENU is what closes the whole overlay from
	// the pause root.
	if (PC->WasInputKeyJustPressed(EKeys::Escape)
		|| PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Right))
	{
		GoBack();
		return;
	}

	// Explicit unbind (and, on the LOADOUTS page, clear a saved slot). D31-PAD — Y does it on a pad,
	// because BKSP is not on a controller. See HandleClearPressed.
	if (PC->WasInputKeyJustPressed(EKeys::BackSpace) || PC->WasInputKeyJustPressed(EKeys::Delete)
		|| PC->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Top))
	{
		HandleClearPressed();
	}
}

void FTraceOptionsMenu::HandleClearPressed()
{
	if (!Rows.IsValidIndex(Selected))
	{
		return;
	}
	const FRow& Row = Rows[Selected];

	// Every options screen that lets you bind should let you UNbind, and without it there is no way to
	// express "I do not want a parry key" short of hiding it under some other one.
	if (Row.Kind == ERowKind::PadBinding)
	{
		// ClearPadKey, not SetPadKey(invalid): SetPadKey refuses an invalid key on purpose, because
		// "invalid" is also what an unparseable .ini line produces and a load path must never be able
		// to wipe a binding. Unbinding is a separate, explicit intent.
		//
		// THE WHOLE ROW, not a slot: MaxPadKeysPerAction is 1, so there is exactly one button.
		UTraceUserSettings::Get().ClearPadKey(Row.Binding);
		return;
	}

	if (Row.Kind == ERowKind::Binding)
	{
		// ClearKey, not SetKey: same reason. SPEC v28 §3c — ONE SLOT, the one the highlight is on.
		// Clearing both from a single Backspace would make the second bind impossible to remove on its
		// own, and would delete a key the player could not see themselves selecting.
		UTraceUserSettings::Get().ClearKey(Row.Binding, ActiveBindingSlot());
		return;
	}

	// A SAVED LOADOUT SLOT. It used to be impossible to empty one again: the case that clears a slot
	// existed and no row or key ever reached it. Two presses, because it cannot be undone — the first
	// turns the row into the question, the second answers it. An empty slot has nothing to clear.
	if (Row.Kind == ERowKind::Action && Row.Action == EAction::EditLoadoutSlot && Row.SlotIndex != INDEX_NONE
		&& !UTraceUserSettings::Get().GetSavedLoadout(Row.SlotIndex).IsEmpty())
	{
		if (ArmOrConfirm(EAction::ClearLoadoutSlot, Row.SlotIndex))
		{
			TraceAudio::PlayLocal2D(GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr,
				TraceSoundEvents::ButtonPress);
			ClearLoadoutSlot(Row.SlotIndex);
		}
	}
}

bool FTraceOptionsMenu::ArmOrConfirm(EAction Action, int32 Slot)
{
	const double RealNow = FPlatformTime::Seconds();
	if (ArmedAction == Action && ArmedSlot == Slot && RealNow <= ArmedUntilReal)
	{
		Disarm();
		return true;
	}

	ArmedAction = Action;
	ArmedSlot = Slot;
	ArmedUntilReal = RealNow + ArmWindowSeconds;
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Armed %s%s: press again within %.0fs to confirm."),
		(Action == EAction::ClearLoadoutSlot) ? TEXT("CLEAR slot ") : TEXT("RESET"),
		(Action == EAction::ClearLoadoutSlot) ? *FString::FromInt(Slot + 1) : TEXT(""), ArmWindowSeconds);
	return false;
}

bool FTraceOptionsMenu::IsArmedRow(const FRow& Row) const
{
	if (ArmedAction == EAction::None || Row.Kind != ERowKind::Action)
	{
		return false;
	}
	if (ArmedAction == EAction::ClearLoadoutSlot)
	{
		return Row.Action == EAction::EditLoadoutSlot && Row.SlotIndex == ArmedSlot;
	}
	return Row.Action == ArmedAction;
}

void FTraceOptionsMenu::ClearLoadoutSlot(int32 SlotIndex)
{
	UTraceUserSettings& Settings = UTraceUserSettings::Get();
	if (SlotIndex < 0 || SlotIndex >= UTraceUserSettings::SavedLoadoutCount)
	{
		return;
	}

	// The name goes with the contents, the way DiscardSavedLoadoutsIfStale keeps the two in step: a
	// name left behind would label an empty slot with a loadout it no longer holds.
	Settings.SetSavedLoadout(SlotIndex, FTraceLoadout());
	Settings.SetSavedLoadoutName(SlotIndex, FString());
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Saved loadout %d cleared."), SlotIndex + 1);

	// The row now reads EMPTY; the highlight stays on it.
	RebuildRows(EAction::EditLoadoutSlot, SlotIndex);
}

void FTraceOptionsMenu::PollMouse(APlayerController* PC)
{
	bool bDown = PC->IsInputKeyDown(EKeys::LeftMouseButton);

	float MouseX = 0.f;
	float MouseY = 0.f;
	if (PC->GetMousePosition(MouseX, MouseY))
	{
		CursorPos = FVector2D(MouseX, MouseY);
		bHasCursor = true;
	}

#if !UE_BUILD_SHIPPING
	// Trace.Menu.Verify's own pointer: the whole function below runs on it exactly as on a real one.
	if (bDebugPointer)
	{
		CursorPos = DebugPointerPos;
		bHasCursor = true;
		bDown = bDebugPointerDown;
	}
#endif

	const bool bJustPressed = bDown && !bMouseWasDown;
	const bool bJustReleased = !bDown && bMouseWasDown;
	bMouseWasDown = bDown;

	if (!bHasCursor)
	{
		return;
	}

	// Hover follows the pointer, but ONLY when the pointer actually moved.
	//
	// It runs AFTER PollNavigation, so without this an arrow key moved Selected and the very same frame
	// a STATIONARY pointer resting over a row dragged it straight back.
	//
	// *** AND THE FIRST SAMPLE ON A PAGE IS NEVER A MOVE. *** Every Open*() forgets the last position
	// (bHasHoverCursorPos = false), so the first poll only RECORDS where the pointer is. It used to count
	// as a move — on the first opening because there was no previous sample, and on later ones because
	// the previous sample was from the last visit — so whatever row lay under the resting pointer took
	// the highlight the moment the menu appeared: Escape then Enter "resumed" into VIDEO, RETURN TO
	// TITLE or QUIT (photographed: the pause menu opened with no key pressed, RETURN TO TITLE lit).
	// Team select and the loadout page made the same rule; now this screen does too.
	const float CursorMoveThresholdSq = 4.f;   // 2 px; below that it is jitter, not intent
	const bool bCursorMoved = bHasHoverCursorPos
		&& FVector2D::DistSquared(CursorPos, LastHoverCursorPos) > CursorMoveThresholdSq;
	LastHoverCursorPos = CursorPos;
	bHasHoverCursorPos = true;

	int32 HoverRow = INDEX_NONE;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].IsSelectable() && Rows[Index].Rect.bIsValid && Rows[Index].Rect.IsInside(CursorPos))
		{
			HoverRow = Index;
			break;
		}
	}

	if (HoverRow != INDEX_NONE && !bDraggingSlider && bCursorMoved)
	{
		// FX_AUDIO_PLAN §5.1 — the pointer's half of "menu row focus change". Guarded on the row
		// ACTUALLY changing, or every 2 px of mouse travel across one row would re-announce it.
		if (Selected != HoverRow)
		{
			TraceAudio::PlayLocal2D(GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr,
				TraceSoundEvents::UIHover);
			SelectedBindingSlot = 0;
		}

		Selected = HoverRow;
	}

	if (bJustPressed)
	{
		PressedRow = HoverRow;
		bDraggingSlider = false;

		// A press SELECTS the row it lands on, moved or not. With the stricter move test above a click
		// on a row the pointer was already resting over would otherwise act on the keyboard's row: a
		// slider drag below reads Rows[Selected], and would have dragged the wrong slider.
		if (HoverRow != INDEX_NONE && Selected != HoverRow)
		{
			Selected = HoverRow;
			SelectedBindingSlot = 0;
		}

		// SPEC v28 §3c — WHICH CHIP DID THE PLAYER CLICK? "Both editable in the settings page" is a
		// hit test as much as a data model. The rects come from the last DrawRow, which is the right
		// frame to use: they are where the chips were when the player aimed at them.
		//
		// A click anywhere else on the row (the label, the gap) leaves the column alone rather than
		// resetting it to 0. Clicking the row you are already editing must not silently move you back
		// to its first bind — that would be a click that changed something invisible.
		if (HoverRow != INDEX_NONE && Rows[HoverRow].Kind == ERowKind::Binding)
		{
			for (int32 Slot = 0; Slot < UTraceUserSettings::MaxKeysPerAction; ++Slot)
			{
				const FBox2D& Chip = Rows[HoverRow].KeyChip[Slot];
				if (Chip.bIsValid && Chip.IsInside(CursorPos))
				{
					SelectedBindingSlot = Slot;
					break;
				}
			}
		}

		// Grabbing the track is a drag, not a click: the value follows the pointer from this frame on
		// and no activation happens on release. This is the single most useful interaction on the
		// whole screen, because "too sensitive" is found by sweeping, not by stepping.
		if (HoverRow != INDEX_NONE && Rows[HoverRow].Kind == ERowKind::Slider && Rows[HoverRow].Track.bIsValid)
		{
			const FBox2D& Track = Rows[HoverRow].Track;
			// Generous horizontal tolerance: the blade's half-width sits either side of the track.
			const float Slack = 12.f * UIScale;
			if (CursorPos.X >= Track.Min.X - Slack && CursorPos.X <= Track.Max.X + Slack)
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

			// THE ARROWS ARE BUTTONS. A choice (or toggle) row's '<' steps it DOWN and its '>' steps it
			// UP. Everywhere else on the row a click still steps forward and wraps at the top — which
			// used to be the ONLY thing a click could do, so stepping back from EPIC to HIGH took a
			// trip round every option.
			const FRow& Clicked = Rows[Armed];
			if ((Clicked.Kind == ERowKind::Choice || Clicked.Kind == ERowKind::Toggle)
				&& Clicked.ArrowLeft.bIsValid && Clicked.ArrowLeft.IsInside(CursorPos))
			{
				TraceAudio::PlayLocal2D(GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr,
					TraceSoundEvents::ButtonPress);
				AdjustSelected(-1);
				return;
			}
			if ((Clicked.Kind == ERowKind::Choice || Clicked.Kind == ERowKind::Toggle)
				&& Clicked.ArrowRight.bIsValid && Clicked.ArrowRight.IsInside(CursorPos))
			{
				TraceAudio::PlayLocal2D(GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr,
					TraceSoundEvents::ButtonPress);
				AdjustSelected(+1);
				return;
			}

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
			// FX_AUDIO_PLAN §5.1 — "UIHover: menu row focus change". On the CHANGE, never on the
			// press: this function is reached from a held arrow key's repeat as well as from a single
			// tap, and the guard below is what keeps a held key from firing sixty hovers a second at
			// the ends of the list, where Selected does not actually move.
			//
			// The pointer's own focus changes are voiced by PollMouse, which is the other place
			// Selected moves. Both of them are one keystroke-or-gesture per sound.
			if (Selected != Index)
			{
				TraceAudio::PlayLocal2D(GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr,
					TraceSoundEvents::UIHover);
			}

			Selected = Index;
			// SPEC v28 §3c — the chip column is the CURSOR's, not the row's. Landing on a keybind row
			// always starts on its first chip, so "down, enter" means the same thing on every row.
			SelectedBindingSlot = 0;
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

	// Spec v14 §3. Read live from its own storage — see TraceCharacters in UI/TraceMatchOptions.h.
	// It is neither a UTraceUserSettings value nor a video value, which is why it is answered here
	// and then falls through the IsVideoSetting() early-out below untouched.
	case ESetting::CharactersEnabled:
		OutValue = TraceCharacters::GetEnabledSetting() ? 1.f : 0.f;
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		break;

	// ---- SPEC v29 §3 — the crosshair --------------------------------------------------------
	//
	// RANGES COME FROM UTraceUserSettings' OWN CONSTANTS, never from numbers written down again
	// here — the same rule the video block below states. A menu that clamped a slider to a range
	// the settings class does not share is a menu that will one day refuse to reach a value the
	// settings class allows, or offer one it silently clamps away.
	//
	// The values are read through the CLAMPED accessors rather than off the raw fields, so a hand
	// edited .ini shows the row the value the game is actually using. Showing the raw number would
	// make the row disagree with the preview sitting next to it.

	case ESetting::CrosshairSize:
		OutValue = Settings.GetCrosshairSize();
		OutMin = UTraceUserSettings::MinCrosshairSize;
		OutMax = UTraceUserSettings::MaxCrosshairSize;
		// Whole reference pixels. The crosshair is snapped to integer pixels when it is drawn
		// (BuildCrosshairBars), so a step finer than 1 would give two slider positions that produce
		// the identical crosshair — a control that visibly does nothing on half its presses.
		OutStep = 1.f;
		break;

	case ESetting::CrosshairThickness:
		OutValue = Settings.GetCrosshairThickness();
		OutMin = UTraceUserSettings::MinCrosshairThickness;
		OutMax = UTraceUserSettings::MaxCrosshairThickness;
		// HALF pixels here, unlike SIZE, and the shipped default is why: it is 2.5, which a whole
		// pixel step could not express — the row would refuse to show a player the value they are on.
		OutStep = 0.5f;
		break;

	case ESetting::CrosshairGap:
		OutValue = Settings.GetCrosshairGap();
		OutMin = UTraceUserSettings::MinCrosshairGap;
		OutMax = UTraceUserSettings::MaxCrosshairGap;
		OutStep = 1.f;
		break;

	case ESetting::CrosshairColor:
		OutValue = float(FMath::Clamp(Settings.CrosshairColorIndex, 0,
			UTraceUserSettings::NumCrosshairColors() - 1));
		OutMin = 0.f;
		OutMax = float(FMath::Max(0, UTraceUserSettings::NumCrosshairColors() - 1));
		OutStep = 1.f;
		break;

	case ESetting::CrosshairOpacity:
		// *** IN PERCENT, NOT IN THE 0..1 THE SETTING STORES. *** A slider that stepped 0.05 through
		// 0.20..1.00 prints "0.85" and lands on values a player cannot report back or reproduce. The
		// conversion is one multiply here and one divide in SetSettingNormalised, and it is the only
		// row on any of these pages whose display unit differs from its storage unit — which is why
		// it is said twice, loudly, in both places.
		OutValue = Settings.GetCrosshairOpacity() * 100.f;
		OutMin = UTraceUserSettings::MinCrosshairOpacity * 100.f;
		OutMax = UTraceUserSettings::MaxCrosshairOpacity * 100.f;
		OutStep = 5.f;
		break;

	case ESetting::CrosshairDot:
		OutValue = Settings.bCrosshairCenterDot ? 1.f : 0.f;
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		break;

	case ESetting::CrosshairOutline:
		OutValue = Settings.bCrosshairOutline ? 1.f : 0.f;
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		break;

	// ---- UI PLAN WP3 — the three faders ------------------------------------------------------
	//
	// IN PERCENT, NOT IN THE 0..1 THE SETTING STORES — the same conversion CROSSHAIR OPACITY makes
	// four cases up, and for the same reason: "0.65" is a number a player cannot report back or
	// reproduce, "65%" is. The twin divide is in SetSettingNormalised and neither may move without
	// the other. That is now TWO rows on these pages whose display unit differs from their storage
	// unit, so the rule is stated once here and once there rather than four times.
	//
	// STEP 5 (i.e. 0.05 of the stored value), and the range starts at a real 0: silence has to be
	// reachable from the slider, and 21 stops is a fader a player can put anywhere without holding
	// the key down for a second.
	case ESetting::MasterVolume:
		OutValue = Settings.GetAudioMasterVolume() * 100.f;
		OutMin = UTraceUserSettings::MinAudioVolume * 100.f;
		OutMax = UTraceUserSettings::MaxAudioVolume * 100.f;
		OutStep = 5.f;
		break;

	case ESetting::SfxVolume:
		OutValue = Settings.GetAudioSfxVolume() * 100.f;
		OutMin = UTraceUserSettings::MinAudioVolume * 100.f;
		OutMax = UTraceUserSettings::MaxAudioVolume * 100.f;
		OutStep = 5.f;
		break;

	case ESetting::MusicVolume:
		OutValue = Settings.GetAudioMusicVolume() * 100.f;
		OutMin = UTraceUserSettings::MinAudioVolume * 100.f;
		OutMax = UTraceUserSettings::MaxAudioVolume * 100.f;
		OutStep = 5.f;
		break;

	// ---- D31-PAD — the controller page's six analog / toggle rows ---------------------------
	//
	// RANGES COME FROM UTraceUserSettings' OWN CONSTANTS, the rule the crosshair block above states.
	// The dead zones are shown as PERCENT because "0.20" means nothing to a player and "20%" means
	// "a fifth of the stick's travel"; the multiply here and the divide in SetSettingNormalised are
	// the pair, and neither may move without the other.
	case ESetting::PadEnabled:
		OutValue = Settings.bPadEnabled ? 1.f : 0.f;
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		break;

	case ESetting::PadLookRate:
		OutValue = Settings.GetPadLookRateX();
		OutMin = UTraceUserSettings::MinPadLookRate;
		OutMax = UTraceUserSettings::MaxPadLookRate;
		// TEN DEGREES PER SECOND A PRESS. The range is 660 wide, so a step of 1 would take 660
		// presses to cross and a step of 50 would give the player thirteen usable values.
		OutStep = 10.f;
		break;

	case ESetting::PadLookYScale:
		OutValue = FMath::Clamp(Settings.PadLookYScale,
			UTraceUserSettings::MinPadLookYScale, UTraceUserSettings::MaxPadLookYScale);
		OutMin = UTraceUserSettings::MinPadLookYScale;
		OutMax = UTraceUserSettings::MaxPadLookYScale;
		OutStep = 0.05f;
		break;

	case ESetting::PadInvertY:
		OutValue = Settings.bPadInvertLookY ? 1.f : 0.f;
		OutMin = 0.f;
		OutMax = 1.f;
		OutStep = 1.f;
		break;

	case ESetting::PadLookDeadzone:
		OutValue = Settings.GetPadLookDeadzone() * 100.f;
		OutMin = UTraceUserSettings::MinPadDeadzone * 100.f;
		OutMax = UTraceUserSettings::MaxPadDeadzone * 100.f;
		OutStep = 1.f;
		break;

	case ESetting::PadMoveDeadzone:
		OutValue = Settings.GetPadMoveDeadzone() * 100.f;
		OutMin = UTraceUserSettings::MinPadDeadzone * 100.f;
		OutMax = UTraceUserSettings::MaxPadDeadzone * 100.f;
		OutStep = 1.f;
		break;

	// UI PLAN WP2.2 — CallSign is a TextEntry row and has no numeric value. It never reaches here
	// (DrawRow and ActivateSelected both branch on the row KIND, and AdjustSelected returns before
	// this is called), and it is named rather than left to `default:` so a reader can see that the
	// omission was decided rather than forgotten.
	case ESetting::CallSign:
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
		return TRACE_TEXTF("OPTIONS.VALUE.PERCENT", "{0}%", { FMath::RoundToInt(Value) });

	case ESetting::FieldOfView:
		// No degree sign: AHUD::DrawText goes through the engine's bitmap fonts, whose glyph pages
		// are ASCII, and a missing glyph draws as a blank box that reads as a rendering fault.
		return TRACE_TEXTF("OPTIONS.VALUE.DEGREES", "{0} DEG", { FMath::RoundToInt(Value) });

	case ESetting::OverallQuality:
		return UTraceGameUserSettings::DescribeOverallQuality(ETraceVideoQuality(FMath::RoundToInt(Value)));

	case ESetting::WindowMode:
	{
		const TArray<EWindowMode::Type>& Modes = UTraceGameUserSettings::GetWindowModeOptions();
		const int32 Index = FMath::RoundToInt(Value);
		return Modes.IsValidIndex(Index)
			? UTraceGameUserSettings::DescribeWindowMode(Modes[Index])
			: FString(TRACE_TEXT("OPTIONS.VALUE.NOT_AVAILABLE", "N/A"));
	}

	case ESetting::Resolution:
	{
		const UTraceGameUserSettings* GUS = Video();
		if (GUS == nullptr)
		{
			return TRACE_TEXT("OPTIONS.VALUE.NOT_AVAILABLE", "N/A");
		}

		if (!GUS->IsResolutionSelectable())
		{
			// Not the stored size greyed out — that would still be a claim, and a false one. In
			// windowed fullscreen the window takes the desktop's size whatever this is set to, so
			// the row says what is actually true.
			return TRACE_TEXT("OPTIONS.VIDEO.VALUE_RESOLUTION_DESKTOP", "DESKTOP");
		}

		const TArray<FTraceResolutionOption>& Options = GUS->GetResolutionOptions();
		const int32 Index = FMath::RoundToInt(Value);

		// GetResolutionOptionIndex returned INDEX_NONE — the running mode is not one of the offered
		// ones. Print the truth about the window rather than the label of a row we are not on.
		if (GUS->GetResolutionOptionIndex() == INDEX_NONE)
		{
			const FIntPoint Current = GUS->GetScreenResolution();
			return TRACE_TEXTF("OPTIONS.VIDEO.VALUE_RESOLUTION_CUSTOM", "{0} x {1}  (CUSTOM)",
				{ Current.X, Current.Y });
		}

		return Options.IsValidIndex(Index) ? Options[Index].Label
			: FString(TRACE_TEXT("OPTIONS.VALUE.NOT_AVAILABLE", "N/A"));
	}

	case ESetting::VSync:
	case ESetting::InvertY:
	case ESetting::CharactersEnabled:
	// SPEC v29 §3 — the two crosshair toggles read like every other toggle on these pages. Sharing
	// this case rather than writing "ON"/"OFF" again is what stops one page's toggles from one day
	// saying ENABLED while another's say ON.
	case ESetting::CrosshairDot:
	case ESetting::CrosshairOutline:
	// D31-PAD — and the same argument holds a fourth time. MISSING THIS WAS A REAL DEFECT, caught in
	// the first capture of the controller page: both rows drew "1.00" and "0.00", which is the raw
	// slider value leaking through the default branch below. A toggle that prints a float reads as a
	// broken control, not as an off switch.
	case ESetting::PadEnabled:
	case ESetting::PadInvertY:
		return (Value >= 0.5f) ? TRACE_TEXT("OPTIONS.VALUE.ON", "ON")
			: TRACE_TEXT("OPTIONS.VALUE.OFF", "OFF");

	// ---- SPEC v29 §3 --------------------------------------------------------------------------
	//
	// "PX" and not "PIXELS": these are 1080p-REFERENCE pixels, which is what every layout number in
	// this project is in, and at 1920x1080 they are literal screen pixels. Spelling it out in the row
	// would be a claim that is exactly true at one resolution, so the unit is short and the honest
	// version lives in the header of Settings/TraceUserSettings.h.
	case ESetting::CrosshairSize:
	case ESetting::CrosshairGap:
		return TRACE_TEXTF("OPTIONS.VALUE.PIXELS", "{0} PX", { FMath::RoundToInt(Value) });

	case ESetting::CrosshairThickness:
		// One decimal, because the shipped default is 2.5 and rounding it to "2" or "3" on the row
		// would make the RESET row's result look like it had missed.
		return TRACE_TEXTF("OPTIONS.VALUE.PIXELS_DECIMAL", "{0} PX",
			{ FString::Printf(TEXT("%.1f"), Value) });

	case ESetting::CrosshairColor:
		// The palette's own names, from the settings class, for the same reason the video rows take
		// their labels from UTraceGameUserSettings: one spelling of "MAGENTA" in the game and in the
		// log that a bug report will be lined up against.
		return UTraceUserSettings::DescribeCrosshairColor(FMath::RoundToInt(Value));

	case ESetting::CrosshairOpacity:
	// UI PLAN WP3 — the three faders are already in percent too, for the same reason and by the same
	// conversion. Sharing this case is what stops one page printing "65%" while another prints "0.65".
	case ESetting::MasterVolume:
	case ESetting::SfxVolume:
	case ESetting::MusicVolume:
		// Already in percent — see GetSettingValue, which is the only place the conversion happens.
		return TRACE_TEXTF("OPTIONS.VALUE.PERCENT", "{0}%", { FMath::RoundToInt(Value) });

	// ---- D31-PAD ------------------------------------------------------------------------------
	//
	// UNITS ON EVERY ROW, because none of these numbers is self-explanatory. "220" tells a player
	// nothing; "220 DEG/S" tells them it is a turn rate and that doubling it doubles the turn. The
	// two dead zones read as a percentage of the stick's travel, which is the only form in which a
	// player can compare them to what their thumb is doing.
	case ESetting::PadLookRate:
		return TRACE_TEXTF("OPTIONS.VALUE.DEGREES_PER_SECOND", "{0} DEG/S", { FMath::RoundToInt(Value) });

	case ESetting::PadLookYScale:
		return TRACE_TEXTF("OPTIONS.VALUE.MULTIPLIER", "{0}x", { FString::Printf(TEXT("%.2f"), Value) });

	case ESetting::PadLookDeadzone:
	case ESetting::PadMoveDeadzone:
		// Already in percent — see GetSettingValue, which is the only place the conversion happens.
		return TRACE_TEXTF("OPTIONS.VALUE.PERCENT", "{0}%", { FMath::RoundToInt(Value) });

	case ESetting::CallSign:
		// UI PLAN WP2.2 — the row draws the name through the clamped accessor, so an empty stored
		// value reads PLAYER rather than as a blank chip that looks like a broken row. DrawRow calls
		// this for the un-edited state only; while the field is active it draws CallSignEntry's live
		// text and caret instead.
		return UTraceUserSettings::Get().GetCallSignOrDefault();

	case ESetting::FrameRateLimit:
	{
		const TArray<float>& Limits = UTraceGameUserSettings::GetFrameRateLimitOptions();
		const int32 Index = FMath::RoundToInt(Value);
		return Limits.IsValidIndex(Index)
			? UTraceGameUserSettings::DescribeFrameRateLimit(Limits[Index])
			: FString(TRACE_TEXT("OPTIONS.VALUE.NOT_AVAILABLE", "N/A"));
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

	// Spec v14 §3, and it is handled BEFORE the UTraceUserSettings block below because it does not
	// live there: it persists into GameUserSettings.ini through TraceCharacters, and falling into the
	// switch below would hit `default: return` and silently do nothing — which is precisely the
	// failure mode a settings row must never have.
	if (Setting == ESetting::CharactersEnabled)
	{
		TraceCharacters::SetEnabledSetting(Snapped >= 0.5f);
		return;
	}

	UTraceUserSettings& Settings = UTraceUserSettings::Get();
	switch (Setting)
	{
	case ESetting::Sensitivity:  Settings.MouseSensitivity = Snapped; break;
	case ESetting::SensitivityY: Settings.MouseSensitivityYScale = Snapped; break;
	case ESetting::InvertY:      Settings.bInvertMouseY = (Snapped >= 0.5f); break;

	// ---- SPEC v29 §3 --------------------------------------------------------------------------
	//
	// *** EVERY CROSSHAIR ROW MUST APPEAR HERE OR IT SILENTLY DOES NOTHING. *** The `default: return`
	// below is a real trapdoor and this page has fallen through it before: spec v14 §3's CHARACTERS
	// row is handled above precisely because landing here would have made it a control that took
	// input and changed no state. A row that draws a value, highlights, accepts arrow keys and
	// changes nothing is the worst failure an options screen has, because it looks like it worked.
	//
	// THESE ARE PLAIN ASSIGNMENTS OF Snapped, NOT INCREMENTS, and that is the other trapdoor. A
	// toggle written as a clamping increment can be turned on and never off — see the note in
	// ActivateSelected, which is where that shipped once and was reported as "the button to uninvert
	// it didn't work". Both toggles below take a value that came from the caller's full range, so
	// LEFT, RIGHT, ENTER and a click all reach both states.
	case ESetting::CrosshairSize:      Settings.CrosshairSize = Snapped; break;
	case ESetting::CrosshairThickness: Settings.CrosshairThickness = Snapped; break;
	case ESetting::CrosshairGap:       Settings.CrosshairGap = Snapped; break;
	case ESetting::CrosshairColor:     Settings.CrosshairColorIndex = FMath::RoundToInt(Snapped); break;

	// Back out of the row's percent into the 0..1 the setting stores. The ONE unit conversion on
	// these pages; its twin is in GetSettingValue and neither may move without the other.
	case ESetting::CrosshairOpacity:   Settings.CrosshairOpacity = Snapped * 0.01f; break;

	case ESetting::CrosshairDot:       Settings.bCrosshairCenterDot = (Snapped >= 0.5f); break;
	case ESetting::CrosshairOutline:   Settings.bCrosshairOutline = (Snapped >= 0.5f); break;

	// ---- UI PLAN WP3 --------------------------------------------------------------------------
	//
	// Back out of the row's percent into the 0..1 the setting stores — the twin of the multiply in
	// GetSettingValue, exactly as CROSSHAIR OPACITY's is three lines up. And they must be HERE or the
	// `default: return` below eats them: the trapdoor this switch's own comment describes, which this
	// page has already fallen through once.
	case ESetting::MasterVolume:       Settings.AudioMasterVolume = Snapped * 0.01f; break;
	case ESetting::SfxVolume:          Settings.AudioSfxVolume    = Snapped * 0.01f; break;
	case ESetting::MusicVolume:        Settings.AudioMusicVolume  = Snapped * 0.01f; break;

	// ---- D31-PAD ------------------------------------------------------------------------------
	//
	// *** EVERY CONTROLLER ROW MUST APPEAR HERE OR IT SILENTLY DOES NOTHING. *** The `default: return`
	// below is the trapdoor this switch's own comment describes, and this page has fallen through it
	// once already. Both toggles are plain assignments of Snapped and not clamping increments, for
	// the reason the crosshair block states: an increment can be turned on and never off.
	//
	// The caller — AdjustSelected, then Save() — is what re-applies the pad's mapping context, so a
	// dead zone dragged on this page is live on the frame the drag ends and not on the next launch.
	case ESetting::PadEnabled:       Settings.bPadEnabled     = (Snapped >= 0.5f); break;
	case ESetting::PadLookRate:      Settings.PadLookRate     = Snapped; break;
	case ESetting::PadLookYScale:    Settings.PadLookYScale   = Snapped; break;
	case ESetting::PadInvertY:       Settings.bPadInvertLookY = (Snapped >= 0.5f); break;

	// Back out of the row's percent into the 0..1 the setting stores — the twin of the multiply in
	// GetSettingValue, exactly as CROSSHAIR OPACITY's and the three faders' are above.
	case ESetting::PadLookDeadzone:  Settings.PadLookDeadzone = Snapped * 0.01f; break;
	case ESetting::PadMoveDeadzone:  Settings.PadMoveDeadzone = Snapped * 0.01f; break;

	default: return;
	}

	// ---- UI PLAN WP3 — THE BED IS RE-GAINED ON THE FRAME THE FADER MOVES ------------------------
	//
	// Every one-shot picks the new gain up by itself: it is chosen at PLAY time, in VolumeFor, and
	// nothing is holding an old one. The music bed is the exception and it is the exception by design
	// — it is a persistent component created once and left running across the menu->match travel, so
	// its VolumeMultiplier was set minutes ago and will not move on its own. RefreshVolume() re-runs
	// the gain arithmetic on the playing component, which is what makes a DRAG on the MUSIC row
	// audible while the pointer is still down instead of on the next track change.
	//
	// Called for MASTER as well as for MUSIC, because master multiplies the bed too.
	if (Setting == ESetting::MasterVolume || Setting == ESetting::MusicVolume)
	{
		if (UTraceMusicSubsystem* Music = UTraceMusicSubsystem::Get(
			GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr))
		{
			Music->RefreshVolume();
		}
	}

	// UI PLAN WP3 — the audible half of a volume control. See PreviewAudioChange; it is a no-op for
	// every row that is not a fader, and it is called from HERE rather than from AdjustSelected so
	// that a DRAG is audible too — a drag never goes through AdjustSelected at all.
	PreviewAudioChange(Setting);

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

	// SPEC v28 §3c — ON A BINDING ROW, LEFT AND RIGHT PICK THE CHIP.
	//
	// The horizontal axis had no meaning at all on these rows before (this function returned), so the
	// second bind costs the page no control it was already using and needs no new key to learn. The
	// move CLAMPS rather than wraps, like every other list on this page: holding right must arrive
	// somewhere and stay there.
	if (Row.Kind == ERowKind::Binding)
	{
		SelectedBindingSlot = FMath::Clamp(ActiveBindingSlot() + FMath::Clamp(Delta, -1, 1),
			0, UTraceUserSettings::MaxKeysPerAction - 1);
		return;
	}

	// D31-PAD — a pad row has ONE chip, so the horizontal axis has nothing to pick and does nothing.
	// Named rather than left to the kind test below, because "why does left/right work on the keybind
	// page and not this one" is a real question with a real answer: MaxPadKeysPerAction is 1.
	if (Row.Kind == ERowKind::PadBinding)
	{
		return;
	}

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

	// SPEC v26 §9 - ButtonPress, client-side. This is the settings/pause/video page's equivalent of
	// ATraceMenuHUD::ActivateSelection, and it is the ONE choke point every activation of a row on
	// these pages passes through: Enter, the gamepad face button and the mouse-release path all
	// funnel here (three call sites above), so one line covers all of them.
	//
	// A SLIDER IS EXCLUDED, and that is the same rule the main menu's grace-window gate encodes: Enter
	// on a continuous value deliberately does nothing (see ERowKind::Slider below), so a click that
	// made a noise while changing nothing would teach the player the sound means less than it does.
	//
	// PlayLocal2D rather than Play(Actor): this page has no actor and no world member - the HUD is a
	// draw-time parameter - and a menu click has no world position to be attenuated from anyway. The
	// call is silent-safe against a null world.
	//
	// FX_AUDIO_PLAN §5.1 — AND *BACK* IS NOT A BUTTON PRESS, IT IS A BACK. The palette carries a
	// separate UIBack for exactly this edge, and it is played by GoBack() rather than here, because
	// Escape reaches GoBack() without passing through this function at all. Playing ButtonPress here
	// as well would put two sounds on one keystroke for a player who clicked the row instead.
	const bool bIsBackRow = (Row.Kind == ERowKind::Action && Row.Action == EAction::Back);

	if (Row.Kind != ERowKind::Slider && !bIsBackRow)
	{
		TraceAudio::PlayLocal2D(GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr,
			TraceSoundEvents::ButtonPress);
	}

	switch (Row.Kind)
	{
	case ERowKind::Toggle:
	{
		// FLIP, do not increment. This used to be AdjustSelected(1), and AdjustSelected clamps at
		// Max — so pressing Enter or clicking a toggle that was already ON did nothing at all, and
		// the row could never be turned back off by the control a player actually reaches for. Only
		// the LEFT arrow could undo it.
		//
		// This shipped, and it was reported from the other side: "the mouse was inverted and the
		// button to uninvert it didn't work." INVERT MOUSE Y, VSYNC and CHARACTERS were all one-way.
		// The behaviour was even known — the -TraceAutoSettings script documents it and steers around
		// it with LEFT/RIGHT instead of pressing Enter, which is how it stayed invisible in testing.
		float Value = 0.f;
		float Min = 0.f;
		float Max = 1.f;
		float Step = 1.f;
		GetSettingValue(Row.Setting, Value, Min, Max, Step);

		AdjustSelected((Value >= 0.5f) ? -1 : 1);
		return;
	}

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

	case ERowKind::TextEntry:
		// UI PLAN WP2.2 — hand the keyboard over. Every key from here until Enter or Escape belongs to
		// the field; see TickCallSignEntry, which is what enforces that.
		BeginCallSignEntry();
		return;

	case ERowKind::Binding:
		bCapturingKey = true;
		CapturingAction = Row.Binding;
		// SPEC v28 §3c — the chip the player is pointing at. LEFT/RIGHT moved it, or the click that
		// got here set it from the chip it actually landed on (see PollMouse).
		CapturingSlot = ActiveBindingSlot();
		// The Enter (or click) that started the capture is still live this frame; without this it
		// would immediately become the new binding.
		IgnoreInputBeforeFrame = GFrameCounter + 1;

		// AND one frame is not enough on its own — see KeysHeldWhenCaptureOpened in the header.
		// The snapshot is taken by the first poll that HAS a PlayerController: this function is
		// reached from both Enter and the mouse and has none, and inventing one here would be a
		// second way to find the local player that could disagree with the one the polls use.
		KeysHeldWhenCaptureOpened.Reset();
		bCaptureNeedsHeldSnapshot = true;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Waiting for a key to bind to %s (slot %d of %d)."),
			TraceInputActions::Info(Row.Binding).DisplayName, CapturingSlot + 1,
			UTraceUserSettings::MaxKeysPerAction);
		return;

	case ERowKind::PadBinding:
		// D31-PAD — the SAME capture, with one flag set. See bCapturingPadKey in the header for why
		// this is one boolean rather than a second state machine: every line below is the Binding
		// case's, including spec v28 §3a's held-key snapshot, and only the key LIST and the SETTER
		// differ (PollKeyCapture branches on the flag for both).
		bCapturingKey = true;
		bCapturingPadKey = true;
		CapturingAction = Row.Binding;
		CapturingSlot = 0;   // MaxPadKeysPerAction is 1; there is no other slot to point at.
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		KeysHeldWhenCaptureOpened.Reset();
		bCaptureNeedsHeldSnapshot = true;
		UE_LOG(LogTraceGame, Display,
			TEXT("[Options] Waiting for a CONTROLLER button to bind to %s. Keyboard keys are refused ")
			TEXT("here — this row is the pad's."),
			TraceInputActions::Info(Row.Binding).DisplayName);
		return;

	default:
		break;
	}

	// THE RESETS ARE TWO PRESSES. Each throws away a page of the player's choices with no undo, and
	// the settings page's sat directly above BACK: one Enter or one misclick too many wiped the mouse
	// and all forty key slots. The first press turns the row into the question (DrawRow reads
	// IsArmedRow); only a second press on it, within ArmWindowSeconds, answers it.
	switch (Row.Action)
	{
	case EAction::ResetDefaults:
	case EAction::ResetKeyboardDefaults:
	case EAction::ResetVideoDefaults:
	case EAction::ResetCrosshairDefaults:
	case EAction::ResetAudioDefaults:
	case EAction::ResetControllerDefaults:
		if (!ArmOrConfirm(Row.Action, INDEX_NONE))
		{
			return;
		}
		break;
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

	case EAction::OpenLoadouts:
		// Remember where we came from, exactly as OpenVideo and OpenCrosshair do — the title screen and
		// the pause menu both reach this page and BACK has to undo the step the player actually took.
		LoadoutsReturnPage = Page;
		Page = EPage::Loadouts;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		break;

	case EAction::EditLoadoutSlot:
		// Hands the frame to the three-column editor. This menu draws nothing while it is up — see the
		// forward in Tick — so the two never read the same keys.
		if (Rows.IsValidIndex(Selected) && Rows[Selected].SlotIndex != INDEX_NONE)
		{
			LoadoutEditor.OpenLibrary(Rows[Selected].SlotIndex);
		}
		break;


	case EAction::OpenCrosshair:
		// Remember where we came from, exactly as OpenVideo does. Only the settings page carries this
		// row today, but "back" must mean the place the player actually came from rather than one
		// hardcoded parent — Trace.Menu.Crosshair can also land them here from nowhere.
		CrosshairReturnPage = Page;
		Page = EPage::Crosshair;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		break;

	case EAction::OpenAudio:
		// Remember where we came from, exactly as OpenVideo and OpenCrosshair do. Only the settings
		// page carries this row today, but "back" must mean the place the player actually came from
		// rather than one hardcoded parent — Trace.Menu.Audio can also land them here from nowhere.
		AudioReturnPage = Page;
		Page = EPage::Audio;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		break;

	case EAction::ResetAudioDefaults:
		// The three faders and nothing else — not the crosshair, not the bindings, not the mouse.
		// See EAction::ResetAudioDefaults in the header.
		UTraceUserSettings::Get().ResetAudioToDefaults();

		// The bed is a persistent component and does not re-read its gain on its own; the reset has to
		// reach it for the same reason a drag does. See SetSettingNormalised.
		if (UTraceMusicSubsystem* Music = UTraceMusicSubsystem::Get(
			GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr))
		{
			Music->RefreshVolume();
		}
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Audio volumes reset to defaults."));
		break;

	case EAction::OpenController:
		// Remember where we came from, exactly as the three doors above do. Only the settings page
		// carries this row today, but "back" must mean the place the player actually came from rather
		// than one hardcoded parent — Trace.Menu.Controller can also land them here from nowhere.
		ControllerReturnPage = Page;
		Page = EPage::Controller;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		break;

	case EAction::OpenKeyboard:
		// Remember where we came from, exactly as the doors above do.
		KeyboardReturnPage = Page;
		Page = EPage::Keyboard;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
		break;

	case EAction::ResetKeyboardDefaults:
		// Every action's keyboard keys and NOTHING else — not the mouse, not the pad.
		UTraceUserSettings::Get().ResetKeyboardToDefaults();
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Keyboard binds reset to defaults."));
		break;

	case EAction::ResetControllerDefaults:
		// The pad table and the five analog values, and NOTHING else — not the mouse, not a single
		// keyboard bind, not the crosshair. See EAction::ResetControllerDefaults in the header, and
		// Trace.Pad.Verify item 6, which asserts the converse as well.
		UTraceUserSettings::Get().ResetPadToDefaults();
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Controller layout reset to defaults."));
		break;

	case EAction::ResetCrosshairDefaults:
		// The crosshair's SEVEN fields and nothing else — not the mouse, not the bindings, not the
		// resolution. See EAction::ResetCrosshairDefaults in the header.
		UTraceUserSettings::Get().ResetCrosshairToDefaults();
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Crosshair reset to defaults."));
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
	case EAction::Quit:
	{
		// *** NOT Close() FIRST. *** Closing ran OnClosed — gameplay input back, the world unpaused,
		// the mouse recaptured — and the next frame began the blocking level load or the exit, so the
		// frame that stayed on screen through the whole load was a frozen frame of LIVE match with the
		// HUD and no menu, which reads as a hang. Now the overlay stays up, input off, the pressed row
		// drawn pressed, until the HUD is taken away; bLeaving's timeout closes it normally if the
		// travel never happens.
		const TFunction<void()>& Leave = (Row.Action == EAction::Quit) ? OnQuit : OnReturnToTitle;
		if (!Leave)
		{
			Close();
			break;
		}
		bLeaving = true;
		LeavingSinceReal = FPlatformTime::Seconds();
		PressedRow = Selected;
		Leave();
		break;
	}

	case EAction::ResetDefaults:
		// SETTINGS' own adjustable rows: the mouse. Not the key binds (their page has its own reset),
		// not the call sign (a name is not a setting) and not ABILITIES (a host rule with no single
		// shipped answer — its fallback is the project's config — which a player resetting their mouse
		// did not ask to change).
		UTraceUserSettings::Get().ResetMouseToDefaults();
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Mouse reset to defaults."));
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
	// FX_AUDIO_PLAN §5.1 — "UIBack: back/cancel activation", client-side 2D.
	//
	// THE ONE CHOKE POINT for going back: the BACK row's activation, Escape on any page, and the
	// pause root's Escape-means-resume all pass through here and nowhere else. Playing it in
	// ActivateSelected instead would miss Escape entirely, which is the way most players actually
	// leave a page.
	//
	// Before the branches, not inside them, because every branch below IS a back — including the one
	// that closes the overlay outright.
	TraceAudio::PlayLocal2D(GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr,
		TraceSoundEvents::UIBack);

	// BACK LANDS ON THE DOOR THE PLAYER CAME THROUGH. Every return below rebuilds the parent page with
	// the highlight on the row that opened this one (DoorFor), not on the parent's first row: coming
	// back from CONTROLLER used to put a keyboard or pad player on CALL SIGN at the top of SETTINGS, and
	// VIDEO -> BACK on RESUME, so they walked back down every time.
	const EPage LeftPage = Page;
	auto ReturnTo = [this, LeftPage](EPage Destination) -> bool
	{
		if (Destination != EPage::Root && Destination != EPage::Settings)
		{
			return false;
		}
		Page = Destination;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows(DoorFor(LeftPage));
		return true;
	};

	if (Page == EPage::Audio)
	{
		// UI PLAN WP3 — "Save() on page close, same as every other page." The keyboard path already
		// saves per press and the mouse path saves per release, so this is the belt-and-braces for a
		// value changed by any route this class grows later.
		UTraceUserSettings::Get().Save();
		if (!ReturnTo(AudioReturnPage))
		{
			Close();
		}
		return;
	}

	if (Page == EPage::Video)
	{
		// A queued resize must not be able to outlive the page that queued it. Leaving the page is
		// as much a commit as pressing enter is, and a mode change that lands two frames after the
		// menu shut would look like the game deciding on its own to resize.
		if (bResolutionApplyPending)
		{
			ApplyVideo(/*bResolutionAffecting=*/true, /*bPersist=*/true);
		}
		if (!ReturnTo(VideoReturnPage))
		{
			Close();
		}
		return;
	}

	// No queued apply to flush on any of these: every row on them is written and saved on the press or
	// the mouse-up that made it (and the pad / key saves re-apply the mapping contexts through
	// UTraceUserSettings::OnChanged); the loadout editor saves on the ENTER that made the slot.
	if (Page == EPage::Controller || Page == EPage::Keyboard || Page == EPage::Loadouts || Page == EPage::Crosshair)
	{
		const EPage ParentPage =
			(Page == EPage::Controller) ? ControllerReturnPage :
			(Page == EPage::Keyboard)   ? KeyboardReturnPage :
			(Page == EPage::Loadouts)   ? LoadoutsReturnPage :
			                              CrosshairReturnPage;
		if (!ReturnTo(ParentPage))
		{
			Close();
		}
		return;
	}

	if (Page == EPage::Settings && !bSettingsIsRootPage)
	{
		// Came in through the pause menu: step back to it rather than dropping the player straight
		// into a firefight they did not ask to return to.
		ReturnTo(EPage::Root);
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

	// Amber below 45 fps, white above. The threshold is a judgement, not a measurement: this is a
	// shooter, and the collaborator's report was about a build that was unplayable rather than one
	// that was merely not smooth. It exists so the player can tell at a glance whether a change they
	// just made moved them across the line.
	const FLinearColor Color = (PerfFps < 45.f) ? TraceMenuArtStyle::AmberLifted() : TraceMenuArtStyle::WordDefault;

	const FString Line = (PerfGpuMs > 0.01f)
		? TRACE_TEXTF("OPTIONS.VIDEO.PERF_READOUT_GPU", "{0} FPS    {1} MS    GPU {2} MS",
			{ FString::Printf(TEXT("%.0f"), PerfFps), FString::Printf(TEXT("%.2f"), PerfFrameMs),
			  FString::Printf(TEXT("%.2f"), PerfGpuMs) })
		: TRACE_TEXTF("OPTIONS.VIDEO.PERF_READOUT", "{0} FPS    {1} MS",
			{ FString::Printf(TEXT("%.0f"), PerfFps), FString::Printf(TEXT("%.2f"), PerfFrameMs) });

	// BODY, not a header (spec v26 §2): a live READOUT of the page's own effect, in the face the
	// in-match HUD reports numbers in. @p Y is the title's cap centre line: it shares the title's line.
	TraceOptionsMenuText::Draw(HUD, Line, RightX, Y, 12.f * UIScale, Color, TraceOptionsMenuType::BodyFace,
		TraceText::EHAlign::Right);
}

// =================================================================================================
// Draw
// =================================================================================================

namespace TraceOptionsMenuFile
{
	/** What the footer legend has to say, decided by the page and what it is doing right now. */
	struct FLegendAsk
	{
		bool bNone = false;          // the pause root (D30: its legend is blank on purpose)
		bool bKeyCapture = false;    // waiting for a key
		bool bPadCapture = false;    // waiting for a pad button
		bool bCallSign = false;      // typing a name
		bool bUnbind = false;        // BKSP / Y unbinds on this page
		bool bClear = false;         // BKSP / Y clears a saved slot on this page
		bool bEdit = false;          // ENTER opens an editor rather than selecting
		bool bPadLine = false;       // show the pad's line too
	};

	/** The legend's [KEY] VERB pairs, one list per device. Every word is the owner's to edit. */
	static void BuildLegend(const FLegendAsk& Ask, TArray<FTraceKitLegendItem>& OutKeys,
		TArray<FTraceKitLegendItem>& OutPad)
	{
		OutKeys.Reset();
		OutPad.Reset();
		if (Ask.bNone)
		{
			return;
		}

		const FString Cancel = TRACE_TEXT("OPTIONS.LEGEND.CANCEL", "CANCEL");
		if (Ask.bKeyCapture || Ask.bPadCapture)
		{
			OutKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_BACK", "ESC"), Cancel });
			if (Ask.bPadCapture)
			{
				// MENU/START is the one pad button the layout leaves unclaimed, so it is the pad's cancel.
				OutPad.Add({ TRACE_TEXT("OPTIONS.LEGEND.PAD_KEY_CANCEL", "MENU"), Cancel });
			}
			return;
		}

		if (Ask.bCallSign)
		{
			OutKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_SELECT", "ENTER"), TRACE_TEXT("OPTIONS.LEGEND.SAVE", "SAVE") });
			OutKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_BACK", "ESC"), Cancel });
			return;
		}

		const FString Move = TRACE_TEXT("OPTIONS.LEGEND.MOVE", "MOVE");
		const FString Select = Ask.bEdit ? TRACE_TEXT("OPTIONS.LEGEND.EDIT", "EDIT") : TRACE_TEXT("OPTIONS.LEGEND.SELECT", "SELECT");
		const FString Back = TRACE_TEXT("OPTIONS.LEGEND.BACK", "BACK");
		const FString ClearWord = Ask.bUnbind ? TRACE_TEXT("OPTIONS.LEGEND.UNBIND", "UNBIND")
			: (Ask.bClear ? TRACE_TEXT("OPTIONS.LEGEND.CLEAR", "CLEAR") : FString());

		OutKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_MOVE", "ARROWS"), Move });
		OutKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_SELECT", "ENTER"), Select });
		if (!ClearWord.IsEmpty())
		{
			OutKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_CLEAR", "BKSP"), ClearWord });
		}
		OutKeys.Add({ TRACE_TEXT("OPTIONS.LEGEND.KEY_BACK", "ESC"), Back });

		if (Ask.bPadLine)
		{
			OutPad.Add({ TRACE_TEXT("OPTIONS.LEGEND.PAD_KEY_MOVE", "D-PAD"), Move });
			OutPad.Add({ TRACE_TEXT("OPTIONS.LEGEND.PAD_KEY_SELECT", "A"), Select });
			if (!ClearWord.IsEmpty())
			{
				OutPad.Add({ TRACE_TEXT("OPTIONS.LEGEND.PAD_KEY_CLEAR", "Y"), ClearWord });
			}
			OutPad.Add({ TRACE_TEXT("OPTIONS.LEGEND.PAD_KEY_BACK", "B"), Back });
		}
	}
}

namespace TraceOptionsMenuFile
{
	static FLegendAsk AskFor(bool bRoot, bool bKeyCapture, bool bPadCapture, bool bCallSign, bool bKeyboardPage,
		bool bControllerPage, bool bLoadoutsPage, bool bSeenPad)
	{
		FLegendAsk Ask;
		Ask.bNone = bRoot && !bKeyCapture && !bPadCapture;
		Ask.bKeyCapture = bKeyCapture && !bPadCapture;
		Ask.bPadCapture = bPadCapture;
		Ask.bCallSign = bCallSign;
		Ask.bUnbind = bKeyboardPage || bControllerPage;
		Ask.bClear = bLoadoutsPage;
		Ask.bEdit = bLoadoutsPage;
		// The pad's line: always on the controller page (a player may have reached it with no keyboard),
		// elsewhere once a pad has been seen on this machine — the loadout page's rule.
		Ask.bPadLine = bControllerPage || bSeenPad;
		return Ask;
	}
}

float FTraceOptionsMenu::LegendHeight(APlayerController* PC) const
{
	namespace OL = TraceOptionsMenuLayout;

	TArray<FTraceKitLegendItem> Keys;
	TArray<FTraceKitLegendItem> PadKeys;
	TraceOptionsMenuFile::BuildLegend(TraceOptionsMenuFile::AskFor(Page == EPage::Root, bCapturingKey,
		bCapturingKey && bCapturingPadKey, CallSignEntry.IsActive(), Page == EPage::Keyboard,
		Page == EPage::Controller, Page == EPage::Loadouts, TracePadMenu::HasSeenPad(PC)), Keys, PadKeys);

	if (Keys.Num() == 0 && PadKeys.Num() == 0)
	{
		return 0.f;
	}
	const float Lines = OL::LegendChipH + ((Keys.Num() > 0 && PadKeys.Num() > 0) ? OL::LegendLineGap : 0.f);
	return (OL::LegendTopGap + Lines) * UIScale;
}

void FTraceOptionsMenu::DrawLegend(AHUD* HUD, APlayerController* PC, float CenterX, float Y, float MaxW)
{
	namespace OL = TraceOptionsMenuLayout;

	TArray<FTraceKitLegendItem> Keys;
	TArray<FTraceKitLegendItem> PadKeys;
	TraceOptionsMenuFile::BuildLegend(TraceOptionsMenuFile::AskFor(Page == EPage::Root, bCapturingKey,
		bCapturingKey && bCapturingPadKey, CallSignEntry.IsActive(), Page == EPage::Keyboard,
		Page == EPage::Controller, Page == EPage::Loadouts, TracePadMenu::HasSeenPad(PC)), Keys, PadKeys);

	// ONE SCALE FOR BOTH LINES, the loadout page's rule: two legends at two sizes would read as a mistake.
	const float FullChipH = OL::LegendChipH * UIScale;
	const float ChipH = TraceMenuKit::KeyLegendFit(FullChipH, MaxW,
		{ TraceMenuKit::KeyLegendWidth(Keys, FullChipH), TraceMenuKit::KeyLegendWidth(PadKeys, FullChipH) });

	float LineY = Y;
	if (Keys.Num() > 0)
	{
		TraceMenuKit::DrawKeyLegend(HUD, Keys, CenterX, LineY, ChipH, Now);
		LineY += OL::LegendLineGap * UIScale;
	}
	TraceMenuKit::DrawKeyLegend(HUD, PadKeys, CenterX, LineY, ChipH, Now);
}

float FTraceOptionsMenu::LabelColumnFraction() const
{
	// The narrow pages give the label a little more of the row, so a two-word label still sits at
	// its full size beside a slider.
	return (Page == EPage::Crosshair || Page == EPage::Audio) ? 0.42f : 0.40f;
}

void FTraceOptionsMenu::DrawPlateFor(AHUD* HUD, ETraceKitState State, float X, float Y, float W, float H) const
{
	// Trace.Menu.Art 0 is the flat-fallback arm: the kit's own stand-in plate, every control still there.
	if (TraceOptionsMenuArt::GEnabled == 0)
	{
		TraceMenuKit::DrawFallbackPlate(HUD, State, X, Y, W, H);
		return;
	}
	TraceMenuKit::DrawStatePlate(HUD, State, X, Y, W, H, Now);
}

void FTraceOptionsMenu::DrawValueBoxFor(AHUD* HUD, float X, float Y, float W, float H, bool bEnabled) const
{
	const FLinearColor Tint = bEnabled ? FLinearColor::White : FLinearColor(0.45f, 0.45f, 0.45f, 1.f);
	if (TraceOptionsMenuArt::GEnabled == 0)
	{
		HUD->DrawRect(TraceMenuArtStyle::PlateFill * Tint, X, Y, W, H);
		return;
	}
	TraceMenuKit::DrawValueBoxPlate(HUD, X, Y, W, H, Tint);
}

void FTraceOptionsMenu::Draw(AHUD* HUD, APlayerController* PC)
{
	namespace OL = TraceOptionsMenuLayout;

	// Once per process, and on the first frame the panel is up: a line in the log that says whether
	// this screen is wearing the art or its fallbacks.
	TraceOptionsMenuArt::LogOnce();

#if !UE_BUILD_SHIPPING
	// Spec v24 §1. After LogOnce, so the first line reports the state LogOnce's loads left behind.
	TraceOptionsMenuArt::LogReadiness(DrawsSinceOpen);
#endif

	const float S = UIScale;
	const bool bRoot = (Page == EPage::Root);

	// THE KIT'S SCRIM: black, over the match or the title. The arena is a field of bright emissive
	// strips and the panel has to be the only thing the eye can land on.
	TraceMenuKit::DrawScrim(HUD, ViewW, ViewH);

	// ---- Panel geometry -------------------------------------------------------------------------
	//
	// Sized to its CONTENT, then clamped to the screen: ask for a comfortable pitch, add it up, and
	// only then shrink the pitch if it does not fit. The legend's height is part of the sum, and is
	// zero on the pause root, which has no legend (D30) — so no dead band under QUIT.
	const float LegendH = LegendHeight(PC);
	const float TitleBlockH = OL::TitleBlock * S;
	const float BottomH = OL::BottomPad * S + LegendH;
	const int32 RowCount = FMath::Max(1, Rows.Num());
	const float PreferredPitch = (bRoot ? OL::RootPitch : OL::PreferredPitch) * S;

	const float PanelH = FMath::Min(ViewH * OL::PanelMaxH, TitleBlockH + RowCount * PreferredPitch + BottomH);
	const float RowsRegion = FMath::Max(1.f, PanelH - TitleBlockH - BottomH);
	const float Pitch = FMath::Clamp(RowsRegion / RowCount, OL::MinPitch * S, PreferredPitch);
	const float RowH = bRoot ? FMath::Min(OL::RootRowH * S, Pitch * 0.85f) : Pitch * OL::RowFill;

	// The pause root is the in-match MAIN MENU and wears the title screen's row metrics (60 tall on a
	// 71 pitch). The pages are as wide as their longest label-and-value pair needs at 720p.
	float PanelW = FMath::Min(ViewW * 0.74f, 880.f * S);
	if (bRoot)
	{
		PanelW = FMath::Min(ViewW * 0.90f, (OL::RootRowW + OL::SideGutter * 2.f) * S);
	}
	else if (Page == EPage::Video)
	{
		PanelW = FMath::Min(ViewW * 0.86f, 1020.f * S);
	}
	else if (Page == EPage::Crosshair)
	{
		// NARROWER, to buy the room the preview sits in beside it.
		PanelW = FMath::Min(ViewW * 0.52f, 620.f * S);
	}
	else if (Page == EPage::Audio)
	{
		PanelW = FMath::Min(ViewW * 0.60f, 680.f * S);
	}

	// ---- SPEC v29 §3 — the crosshair's live preview, BESIDE the panel ----------------------------
	//
	// Beside and not inside, because the panel is sized to its rows. The PAIR is centred, and the
	// preview is dropped when the pair does not fit (a preview overlapping the panel would be worse
	// than none).
	const bool bWantPreview = (Page == EPage::Crosshair);
	const float PreviewGap = 20.f * S;
	const float PreviewW = FMath::Min(ViewW * 0.28f, 320.f * S);
	const float PreviewH = FMath::Min(PreviewW, PanelH);
	const bool bDrawPreview = bWantPreview && (PanelW + PreviewGap + PreviewW) <= (ViewW * 0.96f);

	const float GroupW = bDrawPreview ? (PanelW + PreviewGap + PreviewW) : PanelW;
	const float PanelX = (ViewW - GroupW) * 0.5f;
	const float PanelY = (ViewH - PanelH) * 0.5f;
	const float CX = PanelX + PanelW * 0.5f;

	// A borderless black panel under the list, over the scrim. No bezel and no corner ticks: the kit
	// has none, and a flat rect with a coloured edge is exactly what stylespec §0 rules out.
	HUD->DrawRect(TraceOptionsMenuPalette::PanelFill, PanelX, PanelY, PanelW, PanelH);

	if (bDrawPreview)
	{
		DrawCrosshairPreview(HUD, PanelX + PanelW + PreviewGap, PanelY + (PanelH - PreviewH) * 0.5f, PreviewW, PreviewH);
	}

	// ---- Title ---------------------------------------------------------------------------------
	//
	// PAUSED only when the world really is paused. On a client, on a host with anybody connected, and
	// over the team or loadout screen (which keep the world running for their auto-pick clock) the
	// match goes on behind this panel, and a player reading PAUSED stands still while being shot.
	FString Title = TRACE_TEXT("OPTIONS.TITLE.SETTINGS", "SETTINGS");
	switch (Page)
	{
	case EPage::Root:
		Title = bWorldPaused ? TRACE_TEXT("OPTIONS.TITLE.PAUSED", "PAUSED") : TRACE_TEXT("OPTIONS.TITLE.MENU", "MENU");
		break;
	case EPage::Video:      Title = TRACE_TEXT("OPTIONS.TITLE.VIDEO", "VIDEO"); break;
	case EPage::Crosshair:  Title = TRACE_TEXT("OPTIONS.TITLE.CROSSHAIR", "CROSSHAIR"); break;
	case EPage::Loadouts:   Title = TRACE_TEXT("OPTIONS.TITLE.LOADOUTS", "LOADOUTS"); break;
	case EPage::Audio:      Title = TRACE_TEXT("OPTIONS.TITLE.AUDIO", "AUDIO"); break;
	case EPage::Controller: Title = TRACE_TEXT("OPTIONS.TITLE.CONTROLLER", "CONTROLLER"); break;
	case EPage::Keyboard:   Title = TRACE_TEXT("OPTIONS.TITLE.KEYBOARD", "KEYBOARD"); break;
	default: break;
	}

	// SOFACHROME, white — "the word SETTINGS at the top of the settings page stays Sofachrome while the
	// rows beneath it become Erbaum" (v26 §2).
	const float TitleCapH = OL::TitleCap * S;
	const float TitleMid = PanelY + OL::TitleTop * S + TitleCapH * 0.5f;
	TraceOptionsMenuText::Draw(HUD, Title, CX, TitleMid, TitleCapH, TraceMenuArtStyle::WordDefault,
		TraceOptionsMenuType::HeaderFace, TraceText::EHAlign::Center, PanelW - OL::SideGutter * 2.f * S);

	// The live readout, on the title line and only on the video page (spec v11 §2): every row there
	// is a control whose effect is visible in this number.
	if (Page == EPage::Video)
	{
		DrawPerfReadout(HUD, PanelX + PanelW - OL::SideGutter * S, TitleMid);
	}

	// ---- Rows ----------------------------------------------------------------------------------
	const float RowsTop = PanelY + TitleBlockH;
	const float RowX = PanelX + OL::SideGutter * S;
	const float RowW = PanelW - OL::SideGutter * 2.f * S;

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		DrawRow(HUD, Rows[Index], RowX, RowsTop + Index * Pitch + (Pitch - RowH) * 0.5f, RowW, RowH, Index == Selected);
	}

	// ---- Footer: the kit's KEY legend ----------------------------------------------------------
	if (LegendH > 0.f)
	{
		DrawLegend(HUD, PC, CX, PanelY + PanelH - OL::BottomPad * S - LegendH + OL::LegendTopGap * S,
			PanelW - OL::SideGutter * 2.f * S);
	}

	DrawCursor(HUD, PC);
}

void FTraceOptionsMenu::DrawRow(AHUD* HUD, FRow& Row, float X, float Y, float W, float H, bool bSelected)
{
	namespace OL = TraceOptionsMenuLayout;

	// Every rect this row owns is rewritten by this draw, or left invalid — a stale rect from another
	// page's layout is a click target with nothing under it.
	Row.Rect = FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H));
	Row.Track = FBox2D(ForceInit);
	Row.ArrowLeft = FBox2D(ForceInit);
	Row.ArrowRight = FBox2D(ForceInit);
	for (int32 Chip = 0; Chip < UTraceUserSettings::MaxKeysPerAction; ++Chip)
	{
		Row.KeyChip[Chip] = FBox2D(ForceInit);
	}

	const float S = UIScale;
	const float MidY = Y + H * 0.5f;
	const ETraceTextWeight Body = TraceOptionsMenuType::BodyFace;
	const float BodyCapH = H * OL::BodyCap;

	// DrawLabel sizes a word by its PLATE (caps = 0.37 of it); this is the plate height whose caps are
	// the body's 0.34 of the row, so every body word on the page shares one cap height.
	const float BodyPlateH = H * (OL::BodyCap / TraceMenuKit::LabelCapFraction);

	// A value row's two halves: the label plate (the sheet's wide KEYBIND plate), then its control.
	const float LabelW = W * LabelColumnFraction();
	const float ControlX = X + LabelW + OL::ColumnGap * S;
	const float ControlRight = X + W;
	const float ControlW = FMath::Max(0.f, ControlRight - ControlX);

	// The KEY chip columns: two, both ALWAYS reserved, so a row gaining its "+" chip when it is hovered
	// moves nothing (the primary chip used to jump ~90 px left and shrink under a sweeping pointer).
	const float ChipGapPx = OL::ChipGap * S;
	const float ChipW = FMath::Max(1.f, FMath::Min(OL::ChipMaxW * S, (ControlW - ChipGapPx) * 0.5f));
	const float ChipRightX = ControlRight - ChipW;             // slot 1
	const float ChipLeftX = ChipRightX - ChipGapPx - ChipW;    // slot 0, and the pad page's one chip

	// ---- Header --------------------------------------------------------------------------------
	//
	// A caption, not a control: Sofachrome in white at half strength, with a faint hairline running
	// to the edge. The KEYBOARD page's first row captions its two columns instead (KEYBIND over the
	// labels, KEY over the chips) — the sheet's own two words, which is where they came from.
	if (Row.Kind == ERowKind::Header)
	{
		const float CapH = FMath::Min(OL::HeaderCap * S, H * 0.42f);
		const float CapMid = Y + H * 0.62f;

		if (Row.bColumnCaptions)
		{
			TraceOptionsMenuText::Draw(HUD, Row.Label, X + LabelW * 0.5f, CapMid, CapH,
				TraceOptionsMenuPalette::Caption, TraceOptionsMenuType::HeaderFace, TraceText::EHAlign::Center);
			TraceOptionsMenuText::Draw(HUD, TRACE_TEXT("OPTIONS.SETTINGS.COL_KEY", "KEY"),
				ChipLeftX + (ChipRightX + ChipW - ChipLeftX) * 0.5f, CapMid, CapH,
				TraceOptionsMenuPalette::Caption, TraceOptionsMenuType::HeaderFace, TraceText::EHAlign::Center);
			return;
		}

		if (Row.Label.IsEmpty())
		{
			return;   // AddSpacer: a deliberate blank row
		}

		const float Drawn = TraceOptionsMenuText::Draw(HUD, Row.Label, X, CapMid, CapH,
			TraceOptionsMenuPalette::Caption, TraceOptionsMenuType::HeaderFace, TraceText::EHAlign::Left);
		const float RuleLeft = X + Drawn + 12.f * S;
		if (ControlRight > RuleLeft)
		{
			HUD->DrawRect(TraceOptionsMenuPalette::Rule, RuleLeft, CapMid, ControlRight - RuleLeft, FMath::Max(1.f, S));
		}
		return;
	}

	// ---- Note ----------------------------------------------------------------------------------
	//
	// A line of prose under the row it belongs to, in the kit's quiet grey and the body face, with no
	// plate. Not amber: amber is the hover ring's colour on this kit, and a note is not selectable.
	if (Row.Kind == ERowKind::Note)
	{
		TraceOptionsMenuText::Draw(HUD, Row.Label, X + 4.f * S, MidY, H * OL::NoteCap,
			TraceOptionsMenuPalette::Note, Body, TraceText::EHAlign::Left, W - 8.f * S);
		return;
	}

	// ---- The row's state: the kit's switch -------------------------------------------------------
	//
	// PRESSED while the mouse button is down on this row, and on the row that was pressed to leave
	// (RETURN TO TITLE / QUIT) for as long as the overlay waits for the travel.
	const bool bPressed = bSelected && ((PressedRow != INDEX_NONE && Rows.IsValidIndex(PressedRow)
		&& &Rows[PressedRow] == &Row && bMouseWasDown && !bDraggingSlider) || bLeaving);
	const ETraceKitState State = TraceMenuKit::StateFor(Row.bEnabled, bSelected, bPressed);
	const FTraceKitVisuals Visuals = TraceMenuKit::VisualsFor(State);

	// ---- Action rows: a whole kit button ---------------------------------------------------------
	if (Row.Kind == ERowKind::Action)
	{
		DrawPlateFor(HUD, State, X, Y, W, H);

		FString Text = Row.Label;
		if (IsArmedRow(Row))
		{
			// The two-step confirm: the row IS the question until it is answered or abandoned.
			Text = (ArmedAction == EAction::ClearLoadoutSlot)
				? TRACE_TEXTF("OPTIONS.LOADOUTS.CONFIRM_CLEAR", "CLEAR {0}?", { Row.SlotIndex + 1 })
				: FString(TRACE_TEXT("OPTIONS.ROW.RESET_CONFIRM", "CONFIRM RESET"));
		}
		else if (Row.Action == EAction::AutoDetectQuality && bAutoDetectPending)
		{
			// Swapped for the one frame before the benchmark blocks, so the stall is explained.
			Text = TRACE_TEXT("OPTIONS.VIDEO.AUTO_DETECT_MEASURING", "MEASURING THIS MACHINE...");
		}

		// Sofachrome at the kit's own size on the pause root (the in-match main menu); the body face
		// at the page's body size on the submenus. See FaceForAction.
		const ETraceTextWeight Face = FaceForAction();
		const float WordPlateH = (Face == TraceOptionsMenuType::HeaderFace) ? H : BodyPlateH;
		TraceMenuKit::DrawLabel(HUD, Text, X + W * 0.5f, MidY, WordPlateH, Visuals.Label, W - H * 0.6f, Face);
		return;
	}

	// ---- Value rows: the label plate ---------------------------------------------------------------
	DrawPlateFor(HUD, State, X, Y, LabelW, H);
	TraceMenuKit::DrawLabel(HUD, Row.Label, X + LabelW * 0.5f, MidY, BodyPlateH, Visuals.Label, LabelW - H * 0.6f, Body);

	// The value's own word: white, the lifted olive on the selected row (never cyan), grey when greyed.
	const FLinearColor ValueColor = !Row.bEnabled ? TraceMenuArtStyle::WordDisabled
		: (bSelected ? TraceMenuArtStyle::WordHoverLifted() : TraceMenuArtStyle::WordDefault);
	const float BoxH = H * OL::ValueBoxFill;
	const float BoxY = MidY - BoxH * 0.5f;

	// ---- KEY chips (the keyboard page) ------------------------------------------------------------
	//
	// SPEC v28 §3c — "up to TWO keybinds per action, both editable": two chips, the primary on the
	// left. A chip is the button plate at the sheet's KEY size: HOVER when it is the chip ENTER and
	// BKSP will act on (the active chip of the selected row) or the one waiting for a key; DEFAULT
	// holding a key; the dark DISABLED plate when it is empty (UNBOUND, or the "+" where a second
	// bind would go) — empty is what that plate says.
	if (Row.Kind == ERowKind::Binding)
	{
		const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();
		const int32 ActiveSlot = bSelected ? ActiveBindingSlot() : INDEX_NONE;

		for (int32 Chip = 0; Chip < UTraceUserSettings::MaxKeysPerAction; ++Chip)
		{
			const float ChipX = (Chip == 0) ? ChipLeftX : ChipRightX;
			const bool bWaiting = bCapturingKey && !bCapturingPadKey && CapturingAction == Row.Binding
				&& CapturingSlot == Chip;
			const FKey Key = UserSettings.GetKey(Row.Binding, Chip);
			const bool bActive = (Chip == ActiveSlot);
			const FBox2D ChipRect(FVector2D(ChipX, Y), FVector2D(ChipX + ChipW, Y + H));

			// UI PLAN WP6.3 — THE THROW ROW IS NOT UNBOUND WHILE FIRE HAS A KEY: fire throws (at a goal)
			// and passes (in an endzone) while carrying the Core, so this row is an optional second
			// route. Said as a note in the chip's column rather than as a chip — a sentence is not a key
			// — and it names FIRE's actual key rather than assuming LMB. With fire unbound too, the row
			// really is unbound and gets the normal UNBOUND chip.
			const bool bPassNote = (Chip == 0 && !bWaiting && !Key.IsValid() && Row.Binding == ETraceInputAction::Pass
				&& UserSettings.GetKey(ETraceInputAction::Fire, 0).IsValid());
			if (bPassNote)
			{
				// The note may run on into the second column while that column has nothing in it.
				const bool bSecondInUse = UserSettings.GetKey(Row.Binding, 1).IsValid() || ActiveSlot == 1
					|| (bCapturingKey && CapturingAction == Row.Binding);
				TraceOptionsMenuText::Draw(HUD,
					TRACE_TEXTF("OPTIONS.KEYBIND.PASS_VIA_FIRE", "{0}  (WHILE CARRYING)",
						{ UTraceUserSettings::DescribeKey(UserSettings.GetKey(ETraceInputAction::Fire, 0)) }),
					ChipX + H * 0.25f, MidY, BodyCapH * 0.85f,
					bActive ? TraceMenuArtStyle::WordHoverLifted() : TraceOptionsMenuPalette::Note,
					Body, TraceText::EHAlign::Left, (bSecondInUse ? ChipW : ChipW * 2.f + ChipGapPx) - H * 0.25f);
				Row.KeyChip[Chip] = ChipRect;   // still a live target: the row IS bindable
				continue;
			}

			// SLOT 1 IS DRAWN ONLY WHEN IT HAS SOMETHING TO SAY: a key, the capture, or the selected
			// row's "+" invitation (on the THROW row, only once the highlight is on it — its note uses
			// that column). The column is reserved either way, so nothing else on the row moves.
			const bool bInvite = bSelected && (Row.Binding != ETraceInputAction::Pass || bActive);
			if (Chip > 0 && !bWaiting && !Key.IsValid() && !bInvite)
			{
				continue;
			}

			FString ChipText;
			ETraceKitState ChipState = ETraceKitState::Default;
			FLinearColor ChipColor = FLinearColor::White;
			if (bWaiting)
			{
				ChipText = TRACE_TEXT("OPTIONS.KEYBIND.PRESS_A_KEY", "PRESS A KEY");
				ChipState = ETraceKitState::Hover;
				ChipColor = TraceMenuArtStyle::WordHoverLifted();
				ChipColor.A = 0.55f + 0.45f * FMath::Abs(FMath::Sin(Now * 4.5f));
			}
			else
			{
				ChipText = Key.IsValid() ? UTraceUserSettings::DescribeKey(Key)
					: ((Chip == 0) ? UTraceUserSettings::DescribeKey(Key) : FString(TRACE_TEXT("OPTIONS.KEYBIND.ADD_SECOND", "+")));
				ChipState = bActive ? ETraceKitState::Hover
					: (Key.IsValid() ? ETraceKitState::Default : ETraceKitState::Disabled);
				ChipColor = TraceMenuKit::VisualsFor(ChipState).Label;
			}

			DrawPlateFor(HUD, ChipState, ChipX, Y, ChipW, H);
			TraceMenuKit::DrawLabel(HUD, ChipText, ChipX + ChipW * 0.5f, MidY, BodyPlateH, ChipColor, ChipW - H * 0.5f, Body);
			Row.KeyChip[Chip] = ChipRect;
		}
		return;
	}

	// ---- D31-PAD — the controller page's one chip ---------------------------------------------------
	if (Row.Kind == ERowKind::PadBinding)
	{
		const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();
		const bool bWaiting = bCapturingKey && bCapturingPadKey && CapturingAction == Row.Binding;
		const FKey Key = UserSettings.GetPadKey(Row.Binding);
		// In the PRIMARY chip column — the one the keyboard page's first key sits in — so the two pages
		// read as one layout.
		const FBox2D ChipRect(FVector2D(ChipLeftX, Y), FVector2D(ChipLeftX + ChipW, Y + H));

		// The THROW row, on a pad: already reachable through FIRE's button while carrying — but only
		// if FIRE has one. Otherwise it used to read "UNBOUND (WHILE CARRYING)", which contradicts
		// itself, in grey prose that hid that this row was unbound as well.
		const FKey FirePad = UserSettings.GetPadKey(ETraceInputAction::Fire);
		if (!bWaiting && !Key.IsValid() && Row.Binding == ETraceInputAction::Pass && FirePad.IsValid())
		{
			TraceOptionsMenuText::Draw(HUD,
				TRACE_TEXTF("OPTIONS.PADBIND.PASS_ALREADY_BOUND", "{0}  (WHILE CARRYING)",
					{ UTraceUserSettings::DescribePadKey(FirePad) }),
				ChipLeftX + H * 0.25f, MidY, BodyCapH * 0.85f,
				bSelected ? TraceMenuArtStyle::WordHoverLifted() : TraceOptionsMenuPalette::Note,
				Body, TraceText::EHAlign::Left, ChipW * 2.f + ChipGapPx - H * 0.25f);
			Row.KeyChip[0] = ChipRect;
			return;
		}

		FString ChipText;
		ETraceKitState ChipState = ETraceKitState::Default;
		FLinearColor ChipColor = FLinearColor::White;
		if (bWaiting)
		{
			ChipText = TRACE_TEXT("OPTIONS.PADBIND.PRESS_A_BUTTON", "PRESS A BUTTON");
			ChipState = ETraceKitState::Hover;
			ChipColor = TraceMenuArtStyle::WordHoverLifted();
			ChipColor.A = 0.55f + 0.45f * FMath::Abs(FMath::Sin(Now * 4.5f));
		}
		else
		{
			ChipText = Key.IsValid() ? UTraceUserSettings::DescribePadKey(Key)
				: FString(TRACE_TEXT("OPTIONS.PADBIND.UNBOUND", "UNBOUND"));
			ChipState = bSelected ? ETraceKitState::Hover : (Key.IsValid() ? ETraceKitState::Default : ETraceKitState::Disabled);
			ChipColor = TraceMenuKit::VisualsFor(ChipState).Label;
		}

		DrawPlateFor(HUD, ChipState, ChipLeftX, Y, ChipW, H);
		TraceMenuKit::DrawLabel(HUD, ChipText, ChipLeftX + ChipW * 0.5f, MidY, BodyPlateH, ChipColor, ChipW - H * 0.5f, Body);
		Row.KeyChip[0] = ChipRect;
		return;
	}

	// ---- UI PLAN WP2.2 — the CALL SIGN row: a value box you type into ----------------------------
	//
	// The field is drawn as the JOIN prompt draws its own: the text left-aligned inside the box and a
	// caret measured off the SUBSTRING LEFT OF THE CARET (the faces are proportional). Wide enough for
	// all sixteen characters — a name the player cannot read back is one they cannot check.
	if (Row.Kind == ERowKind::TextEntry)
	{
		const bool bEditing = CallSignEntry.IsActive() && bSelected;
		const FString Shown = bEditing ? CallSignEntry.GetText() : FormatSettingValue(Row.Setting, 0.f);
		const float InsidePad = BoxH * 0.45f;
		const float BoxW = FMath::Min(ControlW,
			FMath::Max(OL::ValueMaxW * S, TraceOptionsMenuText::Width(Shown, BodyCapH, Body) + InsidePad * 2.f));
		const float BoxX = ControlRight - BoxW;
		DrawValueBoxFor(HUD, BoxX, BoxY, BoxW, BoxH, /*bEnabled=*/true);

		const float TextX = BoxX + InsidePad;
		if (bEditing && Shown.IsEmpty())
		{
			// Ghost text, dim enough that nobody mistakes it for a value they can press Enter on.
			FLinearColor Ghost = TraceOptionsMenuPalette::Note;
			Ghost.A = 0.5f;
			TraceOptionsMenuText::Draw(HUD, FString(UTraceUserSettings::DefaultCallSign), TextX, MidY, BodyCapH,
				Ghost, Body, TraceText::EHAlign::Left);
		}
		else
		{
			TraceOptionsMenuText::Draw(HUD, Shown, TextX, MidY, BodyCapH,
				bEditing ? TraceMenuArtStyle::WordDefault : ValueColor, Body, TraceText::EHAlign::Left, BoxW - InsidePad * 2.f);
		}

		if (bEditing && CallSignEntry.IsCaretVisible(Now))
		{
			const float CaretX = TextX + TraceOptionsMenuText::Width(Shown.Left(CallSignEntry.GetCaret()), BodyCapH, Body);
			HUD->DrawRect(TraceMenuArtStyle::WordDefault, CaretX + 1.f * S, MidY - BodyCapH * 0.75f,
				FMath::Max(2.f, 2.f * S), BodyCapH * 1.5f);
		}
		return;
	}

	// Every remaining kind reads its value the same way, which is what lets one path serve INVERT
	// MOUSE Y and VSYNC and another serve all thirteen enumerated video rows.
	float Value = 0.f;
	float Min = 0.f;
	float Max = 1.f;
	float Step = 1.f;
	GetSettingValue(Row.Setting, Value, Min, Max, Step);

	// ---- Choice and Toggle: the value box, with '<' '>' at its ends ---------------------------------
	//
	// The title's DIFFICULTY row is the same control, and it is a chip with the arrows inside it. They
	// are drawn on every live row (dim when it is not selected) and only toward an end that has
	// somewhere to go, and each is a CLICK TARGET: '<' steps down, '>' steps up (see PollMouse). The
	// box is a fixed column, so the lists do not twitch sideways as their values change length.
	if (Row.Kind == ERowKind::Toggle || Row.Kind == ERowKind::Choice)
	{
		const float BoxW = FMath::Min(ControlW, OL::ValueMaxW * S);
		const float BoxX = ControlRight - BoxW;
		DrawValueBoxFor(HUD, BoxX, BoxY, BoxW, BoxH, Row.bEnabled);

		const float ArrowW = BoxH;
		FLinearColor ArrowColor = bSelected ? TraceMenuKit::FurnitureSelected : TraceMenuKit::FurnitureUnselected;
		ArrowColor.A = bSelected ? 1.f : 0.45f;
		if (Row.bEnabled && Value > Min + UE_KINDA_SMALL_NUMBER)
		{
			TraceMenuKit::DrawLabel(HUD, TEXT("<"), BoxX + ArrowW * 0.5f, MidY, BodyPlateH, ArrowColor, 0.f, Body);
			Row.ArrowLeft = FBox2D(FVector2D(BoxX, Y), FVector2D(BoxX + ArrowW, Y + H));
		}
		if (Row.bEnabled && Value < Max - UE_KINDA_SMALL_NUMBER)
		{
			TraceMenuKit::DrawLabel(HUD, TEXT(">"), BoxX + BoxW - ArrowW * 0.5f, MidY, BodyPlateH, ArrowColor, 0.f, Body);
			Row.ArrowRight = FBox2D(FVector2D(BoxX + BoxW - ArrowW, Y), FVector2D(BoxX + BoxW, Y + H));
		}

		// ON is white like every other value — it used to be amber, which on this kit is the hover
		// colour, so an ON switch read as a selected one.
		TraceMenuKit::DrawLabel(HUD, FormatSettingValue(Row.Setting, Value), BoxX + BoxW * 0.5f, MidY, BodyPlateH,
			ValueColor, BoxW - ArrowW * 2.f, Body);
		return;
	}

	// ---- Slider: the sheet's trough, its white blade, and the value box -------------------------------
	//
	// On the sheet the slider is a thin navy rail with a gold halo, a white blade handle riding it, and
	// a gold-edged value box to its right holding the number. The box is ValuePlateToTrack times the
	// trough's height and the blade SliderHandleToTrack times it, all on one centre line.
	//
	// THE BLADE IS THE SAME PICTURE AS THE POINTER (UI QA finding 6b), which is why this page drew a
	// fader cap for a while. It is the kit's thumb (stylespec §3) and the package that brought this
	// page onto the kit was asked for it by name; what keeps the two apart is that the thumb is always
	// ON a trough and centred on the value's point, where the pointer is wherever the mouse is.
	const float Alpha = FMath::Clamp((Value - Min) / FMath::Max(UE_KINDA_SMALL_NUMBER, Max - Min), 0.f, 1.f);

	const float SliderBoxW = FMath::Min(ControlW * 0.5f, OL::SliderValueW * S);
	const float SliderBoxX = ControlRight - SliderBoxW;
	DrawValueBoxFor(HUD, SliderBoxX, BoxY, SliderBoxW, BoxH, /*bEnabled=*/true);
	TraceMenuKit::DrawLabel(HUD, FormatSettingValue(Row.Setting, Value), SliderBoxX + SliderBoxW * 0.5f, MidY, BodyPlateH,
		ValueColor, SliderBoxW - BoxH * 0.5f, Body);

	const float TrackH = BoxH / TraceMenuKit::ValuePlateToTrack;
	const float HandleW = static_cast<float>(TraceMenuKit::SliderHandleRect(0.f, 0.f, TrackH).GetSize().X);
	const float TrackLeft = ControlX;
	const float TrackRight = SliderBoxX - OL::ColumnGap * S;
	const float TrackW = FMath::Max(HandleW * 2.f, TrackRight - TrackLeft);

	// The blade travels between half its width in from each end, so at 0 % and 100 % it still sits
	// wholly on the trough and never over the value box (the old cap straddled the box at 100 %).
	const float RailLeft = TrackLeft + HandleW * 0.5f;
	const float RailW = FMath::Max(1.f, TrackW - HandleW);
	const float HandleX = RailLeft + RailW * Alpha;
	const float TrackY = MidY - TrackH * 0.5f;

	if (TraceOptionsMenuArt::GEnabled == 0)
	{
		HUD->DrawRect(TraceMenuArtStyle::PlateFill, TrackLeft, TrackY + TrackH * TraceMenuKit::TrackRailTopV,
			TrackW, FMath::Max(2.f, TrackH * TraceMenuKit::TrackRailV));
	}
	else
	{
		TraceMenuKit::DrawSliderTrack(HUD, TrackLeft, TrackY, TrackW, TrackH);
	}

	// THE FILL: a thin gold line along the trough's own rail band, up to the blade. Gold because that is
	// this control's accent on the sheet (the trough's halo and the value box's edge); thin, because the
	// thick cyan bar it replaces hid the artist's trough entirely.
	{
		FLinearColor Fill = TraceMenuArtStyle::ValueGlowLifted();
		Fill.A = bSelected ? 1.f : 0.7f;
		const float FillH = FMath::Max(2.f, TrackH * TraceMenuKit::TrackRailV * 0.45f);
		HUD->DrawRect(Fill, RailLeft, MidY - FillH * 0.5f, FMath::Max(0.f, HandleX - RailLeft), FillH);
	}

	if (TraceOptionsMenuArt::GEnabled == 0)
	{
		const FBox2D Blade = TraceMenuKit::SliderHandleRect(HandleX, MidY, TrackH);
		HUD->DrawRect(FLinearColor::White, HandleX - 1.5f * S, static_cast<float>(Blade.Min.Y), 3.f * S,
			static_cast<float>(Blade.GetSize().Y));
	}
	else
	{
		TraceMenuKit::DrawSliderHandle(HUD, HandleX, MidY, TrackH, FLinearColor::White);
	}

	// Stored AFTER drawing so the poll on the next frame drags against exactly what was on screen: the
	// blade's travel, so the pointer and the blade's centre agree at every value.
	Row.Track = FBox2D(FVector2D(RailLeft, Y), FVector2D(RailLeft + RailW, Y + H));
}

void FTraceOptionsMenu::DrawCrosshairPreview(AHUD* HUD, float X, float Y, float W, float H)
{
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();

	// ---- The two surfaces ----------------------------------------------------------------------
	//
	// Split down the middle, and both colours are taken from the thing they stand for rather than
	// invented: the left is the arena's black floor and the right is (193, 252, 253) — the lit cyan
	// surface named in ATraceHUD::DrawAimReticle's own note as the one a white reticle disappeared
	// against. A crosshair legible over one and invisible over the other is the failure this exists
	// to show. (That cyan is the ARENA's, shown as a sample, not a colour of this menu.)
	//
	// Integer-snapped; a half-pixel seam under the crosshair would read as part of it.
	const float BoxX = FMath::RoundToFloat(X);
	const float BoxY = FMath::RoundToFloat(Y);
	const float BoxW = FMath::RoundToFloat(W);
	const float BoxH = FMath::RoundToFloat(H);
	const float HalfW = FMath::RoundToFloat(BoxW * 0.5f);

	HUD->DrawRect(FLinearColor(0.004f, 0.014f, 0.026f, 1.f), BoxX, BoxY, HalfW, BoxH);
	HUD->DrawRect(FLinearColor(0.757f, 0.988f, 0.992f, 1.f), BoxX + HalfW, BoxY, BoxW - HalfW, BoxH);

	// ---- The crosshair, from the SAME geometry the HUD draws -----------------------------------
	//
	// UTraceUserSettings::BuildCrosshairBars, not a copy of it, at the first-person scale (UIScale and
	// nothing else) — the crosshair the player aims a gun with.
	const float CenterX = BoxX + HalfW;
	const float CenterY = BoxY + FMath::RoundToFloat(BoxH * 0.5f);

	FTraceCrosshairBar Bars[TraceCrosshairMaxBars];
	const int32 NumBars = Settings.BuildCrosshairBars(CenterX, CenterY, UIScale, Bars);

	const FLinearColor Ink = Settings.GetCrosshairColor();
	const FLinearColor Outline = Settings.GetCrosshairOutlineColor();

	// *** DrawRect AND NEVER DrawLine. *** AHUD::DrawLine discards alpha, so the OPACITY row would have
	// no visible effect here and the OUTLINE row's "off" (alpha zero) would draw a solid black box.
	if (Outline.A > UE_KINDA_SMALL_NUMBER)
	{
		for (int32 Index = 0; Index < NumBars; ++Index)
		{
			const FTraceCrosshairBar& B = Bars[Index];
			HUD->DrawRect(Outline, B.X - 1.f, B.Y - 1.f, B.W + 2.f, B.H + 2.f);
		}
	}
	for (int32 Index = 0; Index < NumBars; ++Index)
	{
		const FTraceCrosshairBar& B = Bars[Index];
		HUD->DrawRect(Ink, B.X, B.Y, B.W, B.H);
	}

	// ---- Caption -------------------------------------------------------------------------------
	//
	// One word, on the DARK half so it is legible whatever the preview's own colours are doing. The
	// strip of numbers that used to run along the bottom repeated the rows beside it and is gone.
	const float CapH = 10.f * UIScale;
	TraceOptionsMenuText::Draw(HUD, TRACE_TEXT("OPTIONS.CROSSHAIR.PREVIEW_CAPTION", "PREVIEW"),
		BoxX + 12.f * UIScale, BoxY + 12.f * UIScale + CapH * 0.5f, CapH, TraceOptionsMenuPalette::Caption,
		TraceOptionsMenuType::HeaderFace, TraceText::EHAlign::Left);
}

// =================================================================================================
// UI PLAN WP2 — the call sign
// =================================================================================================

void FTraceOptionsMenu::BeginCallSignEntry()
{
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();

	// SEEDED WITH THE RAW STORED VALUE, not with GetCallSignOrDefault(). A player who has never set a
	// name sees the row read PLAYER and then opens an EMPTY field — because PLAYER is what the game
	// calls them, not what they typed, and pre-filling it would make their first keystroke a
	// correction of a word they never chose. A player who HAS set a name gets it back to edit.
	CallSignEntry.Begin(UTraceUserSettings::SanitizeCallSign(Settings.CallSign),
		ETraceTextCharset::CallSign, UTraceUserSettings::MaxCallSignLength);

	UE_LOG(LogTraceGame, Display, TEXT("[Options] Call sign entry opened (current '%s')."),
		*Settings.GetCallSignOrDefault());
}

bool FTraceOptionsMenu::TickCallSignEntry(APlayerController* PC)
{
	if (!CallSignEntry.IsActive())
	{
		return false;
	}

	// The field reads the keyboard itself — the reasons are in FTraceTextEntry's header — and it has
	// to be serviced BEFORE anything is drawn so the caret and the text on screen are this frame's.
	// ATraceMenuHUD does exactly this with its JOIN field, one line of its DrawHUD.
	CallSignEntry.Poll(PC, Now);

	if (CallSignEntry.ConsumeSubmit())
	{
		CommitCallSign();
		CallSignEntry.End();
		return true;
	}

	if (CallSignEntry.ConsumeCancel())
	{
		// Discarded, not written. Escape means "forget what I typed" everywhere else in this overlay
		// (a rebind capture, a page), and a field that saved on Escape would be the one control here
		// where it meant the opposite.
		CallSignEntry.End();
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Call sign entry cancelled."));
		return true;
	}

	// STILL ACTIVE: this class must not route its own bindings this frame. The arrow keys are the
	// caret's, Enter is submit, Escape is cancel and Backspace is a deletion — every one of them is
	// also a control on this page, and both readers would fire without this.
	return true;
}

void FTraceOptionsMenu::CommitCallSign()
{
	UTraceUserSettings& Settings = UTraceUserSettings::Get();

	// Sanitised on the way in even though the field enforced the same alphabet while typing. The
	// field is the UI and the accessor is the contract, and a commit path that trusted the UI would
	// be the one place a pasted string could get past the rules — see SanitizeCallSign.
	Settings.CallSign = UTraceUserSettings::SanitizeCallSign(CallSignEntry.GetText());
	Settings.Save();

	const FString Effective = Settings.GetCallSignOrDefault();

	// ---- UI PLAN WP2.4 — THE LIVE HALF ----------------------------------------------------------
	//
	// The other half is one line in ATracePlayerController::BeginPlay (owned by the restructure
	// tranche), which is what makes a name apply to a match you JOIN. This is what makes it apply to
	// the match you are already IN: a player who opens the pause menu mid-match, sets a name and gets
	// it on the scoreboard only after a re-travel has been given a setting that appears not to work.
	//
	// ServerChangeName, not SetPlayerName. It is the engine's own rename path — APlayerController ->
	// AGameModeBase::ChangeName -> APlayerState::SetPlayerName, replicated to everybody — where a
	// local SetPlayerName would be this client lying to itself and to nobody else. On the title
	// screen there is no controller with a PlayerState to rename and this is simply skipped; the
	// stored value is what the next match reads.
	APlayerController* const LocalPC = (GEngine != nullptr && GEngine->GetCurrentPlayWorld() != nullptr)
		? GEngine->GetCurrentPlayWorld()->GetFirstPlayerController()
		: nullptr;

	const APlayerState* const State = (LocalPC != nullptr) ? LocalPC->PlayerState : nullptr;
	const FString Was = (State != nullptr) ? State->GetPlayerName() : FString(TEXT("<none>"));

	if (LocalPC != nullptr && Was != Effective)
	{
		LocalPC->ServerChangeName(Effective);
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[Options] Call sign committed: typed='%s' stored='%s' effective='%s' (player state was '%s')."),
		*CallSignEntry.GetText(), *Settings.CallSign, *Effective, *Was);
}

// =================================================================================================
// UI PLAN WP3 — the fader's own click
// =================================================================================================

void FTraceOptionsMenu::PreviewAudioChange(ESetting Setting)
{
	// MASTER and EFFECTS only. The MUSIC row is answered by the music itself: RefreshVolume() has
	// already moved the playing bed by the time this runs, so a one-shot on top of it would be a
	// second sound answering a question the first one is answering better.
	if (Setting != ESetting::MasterVolume && Setting != ESetting::SfxVolume)
	{
		return;
	}

	// REAL time. The in-match pause menu stops the world, so a throttle measured against world time
	// would never expire there — the first click would be the only one a paused player ever heard.
	const double RealNow = FPlatformTime::Seconds();
	if ((RealNow - LastAudioPreviewRealTime) < AudioPreviewMinInterval)
	{
		return;
	}
	LastAudioPreviewRealTime = RealNow;

	// PlayLocalNow rather than TraceAudio::PlayLocal2D, because this one call wants the COMPONENT
	// discarded rather than the convenience: the point is that it goes through
	// UTraceAudioSubsystem::VolumeFor, i.e. through the very fader the player is dragging, so what
	// they hear IS the level they are choosing rather than a fixed-level preview of it.
	if (UTraceAudioSubsystem* Audio = UTraceAudioSubsystem::Get(
		GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr))
	{
		Audio->PlayLocalNow(TraceSoundEvents::ButtonPress);
	}
}

void FTraceOptionsMenu::DrawCursor(AHUD* HUD, APlayerController* PC)
{
	// The OS cursor does not appear in captured frames, and in the match it is hidden outright until
	// this overlay releases it — so the overlay draws its own: the kit's white blade, TIP-ANCHORED on
	// the point PollMouse hit-tests, through TraceMenuKit::ShowCursor (which also keeps the OS arrow
	// hidden for as long as ours is drawn — stylespec §9, the pointer rule every kit screen follows).
	if (!bHasCursor)
	{
		return;
	}

	if (TraceMenuKit::ShowCursor(HUD, PC, TEXT("settings overlay"), CursorPos, UIScale))
	{
		return;
	}

	// The fallback every Canvas surface keeps, for a build with no menu art or the frame or two before
	// the sprite's texture lands.
	const float Size = 9.f * UIScale;
	const float Thick = FMath::Max(1.f, 1.5f * UIScale);
	const FLinearColor Color = TraceMenuArtStyle::WordDefault;

	HUD->DrawLine(CursorPos.X - Size, CursorPos.Y, CursorPos.X - Size * 0.35f, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X + Size * 0.35f, CursorPos.Y, CursorPos.X + Size, CursorPos.Y, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y - Size, CursorPos.X, CursorPos.Y - Size * 0.35f, Color, Thick);
	HUD->DrawLine(CursorPos.X, CursorPos.Y + Size * 0.35f, CursorPos.X, CursorPos.Y + Size, Color, Thick);
}

ETraceTextWeight FTraceOptionsMenu::FaceForAction() const
{
	// THE ONE JUDGEMENT CALL IN SPEC v26 §2's SPLIT, ISOLATED TO ONE EXPRESSION.
	//
	// An Action row is a button, and this class draws buttons on two very different pages:
	//
	//   * the PAUSE ROOT — RESUME / SETTINGS / VIDEO / RETURN TO TITLE / QUIT. That is not a settings
	//     page; it is the in-match MAIN MENU, the same list of destinations the title screen puts on
	//     screen through UTraceMenuRow. §2 keeps "main menu rows" in Sofachrome, so these stay in it;
	//
	//   * the submenus — BACK, RESET, the doors, the loadout slots. Controls on a submenu, in the same
	//     column and rhythm as the rows around them; they get the body face like everything else there.
	//
	// An owner who reads it the other way changes this one return.
	return (Page == EPage::Root) ? TraceOptionsMenuType::HeaderFace : TraceOptionsMenuType::BodyFace;
}

#if !UE_BUILD_SHIPPING
// =================================================================================================
// Trace.Menu.Verify — this overlay's own behaviour, one drawn frame at a time
// =================================================================================================
//
// Every check drives the REAL path: the real PollMouse (fed its own pointer, so a headless run
// neither needs nor moves the OS mouse), real key edges injected through Slate for BKSP and the held
// DOWN (the route a keyboard takes), and ActivateSelected / GoBack — the functions Enter, a click and
// Escape all end in. It restores every setting it touches.
//
// Each check was seen to FAIL against the code it replaced before it was trusted (the commit that
// added it says how).

void FTraceOptionsMenu::DebugBeginVerify()
{
	if (VerifyStep != 0)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[MenuVerify] Already running."));
		return;
	}
	VerifyStep = 1;
	VerifyWait = 0;
	VerifyFailures = 0;
	VerifyChecks = 0;
	UE_LOG(LogTraceGame, Display,
		TEXT("[MenuVerify] ===== the settings / pause overlay: pointer on open, BACK, confirms, arrows, paused clock ====="));
}

void FTraceOptionsMenu::VerifyCheck(const TCHAR* Label, bool bPass, const FString& Detail)
{
	++VerifyChecks;
	VerifyFailures += bPass ? 0 : 1;
	UE_LOG(LogTraceGame, Display, TEXT("[MenuVerify]   %-4s %-70s %s"), bPass ? TEXT("ok") : TEXT("FAIL"), Label, *Detail);
}

void FTraceOptionsMenu::TickVerify(APlayerController* PC)
{
	if (VerifyStep == 0 || PC == nullptr)
	{
		return;
	}
	if (VerifyWait > 0)
	{
		--VerifyWait;
		return;
	}

	UTraceUserSettings& Settings = UTraceUserSettings::Get();
	UWorld* VerifyWorld = PC->GetWorld();

	auto FirstSelectable = [this]() -> int32
	{
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index].IsSelectable())
			{
				return Index;
			}
		}
		return INDEX_NONE;
	};
	auto RowName = [this](int32 Index) -> FString
	{
		return Rows.IsValidIndex(Index) ? FString::Printf(TEXT("'%s'"), *Rows[Index].Label) : FString(TEXT("<none>"));
	};
	auto Next = [this](int32 Step, int32 Wait)
	{
		VerifyStep = Step;
		VerifyWait = Wait;
	};

	switch (VerifyStep)
	{
	// ---- A. A POINTER RESTING OVER A ROW DOES NOT TAKE THE SELECTION WHEN A PAGE OPENS --------------
	case 1:
		bDebugPointer = true;
		bDebugPointerDown = false;
		DebugPointerPos = FVector2D(2.f, 2.f);   // off every row
		OpenSettings();
		Next(2, 3);
		return;

	case 2:
		VerifyIndex = FindActionRow(EAction::Back);
		if (VerifyIndex == INDEX_NONE || !Rows[VerifyIndex].Rect.bIsValid)
		{
			VerifyCheck(TEXT("the settings page drew its rows"), false, TEXT("no BACK rect"));
			Next(30, 0);
			return;
		}
		// Park the pointer on BACK while the overlay is CLOSED, then open it again: the pointer is at
		// rest over a row the moment the page appears — the pause menu's Escape-then-Enter case.
		DebugPointerPos = Rows[VerifyIndex].Rect.GetCenter();
		Close();
		Next(3, 2);
		return;

	case 3:
		OpenSettings();
		Next(4, 4);
		return;

	case 4:
	{
		const int32 First = FirstSelectable();
		VerifyCheck(TEXT("A. a pointer resting on a row does not take the selection on open"),
			Selected == First,
			FString::Printf(TEXT("selected %s; the page opens on %s; the pointer rests on %s"),
				*RowName(Selected), *RowName(First), *RowName(VerifyIndex)));

		// ...and a pointer that really moves still hovers — a fix that switched hover off would pass
		// the check above.
		VerifyIndexB = FindActionRow(EAction::OpenAudio);
		if (Rows.IsValidIndex(VerifyIndexB))
		{
			DebugPointerPos = Rows[VerifyIndexB].Rect.GetCenter();
		}
		Next(5, 2);
		return;
	}

	case 5:
		VerifyCheck(TEXT("A. ...and a pointer that moves onto a row still selects it"),
			Selected == VerifyIndexB && VerifyIndexB != INDEX_NONE,
			FString::Printf(TEXT("selected %s, pointer on %s"), *RowName(Selected), *RowName(VerifyIndexB)));

		// Through the door, the way Enter goes.
		ActivateSelected();
		Next(6, 3);
		return;

	// ---- E. RESET IS TWO PRESSES ------------------------------------------------------------------
	case 6:
	{
		VerifyCheck(TEXT("the AUDIO door opens the AUDIO page"), Page == EPage::Audio, TEXT(""));

		VerifySavedVolume = Settings.AudioMasterVolume;
		Settings.AudioMasterVolume = 0.40f;

		const int32 ResetRow = FindActionRow(EAction::ResetAudioDefaults);
		if (ResetRow == INDEX_NONE)
		{
			VerifyCheck(TEXT("E. the AUDIO page has its RESET row"), false, TEXT(""));
		}
		else
		{
			Selected = ResetRow;
			ActivateSelected();
			const bool bAsked = FMath::IsNearlyEqual(Settings.AudioMasterVolume, 0.40f) && IsArmedRow(Rows[Selected]);
			VerifyCheck(TEXT("E. RESET: the first press only asks (row armed, nothing reset)"), bAsked,
				FString::Printf(TEXT("master %.2f, armed %d"), Settings.AudioMasterVolume, IsArmedRow(Rows[Selected]) ? 1 : 0));

			ActivateSelected();
			VerifyCheck(TEXT("E. RESET: the second press resets"),
				!FMath::IsNearlyEqual(Settings.AudioMasterVolume, 0.40f) && ArmedAction == EAction::None,
				FString::Printf(TEXT("master %.2f"), Settings.AudioMasterVolume));
		}

		Settings.AudioMasterVolume = VerifySavedVolume;
		Settings.Save();

		// ---- B. BACK LANDS ON THE DOOR --------------------------------------------------------------
		GoBack();
		Next(7, 2);
		return;
	}

	case 7:
		VerifyCheck(TEXT("B. BACK from AUDIO lands on the AUDIO door, not the top of the page"),
			Page == EPage::Settings && Rows.IsValidIndex(Selected) && Rows[Selected].Action == EAction::OpenAudio,
			FString::Printf(TEXT("page %d, selected %s"), int32(Page), *RowName(Selected)));

		// ---- G. THE CHOICE ARROWS ARE CLICK TARGETS -----------------------------------------------
		Selected = FindActionRow(EAction::OpenCrosshair);
		ActivateSelected();
		Next(8, 3);
		return;

	case 8:
		VerifyIndex = INDEX_NONE;
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index].Kind == ERowKind::Choice && Rows[Index].Setting == ESetting::CrosshairColor)
			{
				VerifyIndex = Index;
			}
		}
		VerifySavedColour = Settings.CrosshairColorIndex;
		if (VerifyIndex == INDEX_NONE || UTraceUserSettings::NumCrosshairColors() < 3)
		{
			VerifyCheck(TEXT("G. the CROSSHAIR page has a COLOUR choice with three colours"), false, TEXT(""));
			Next(16, 0);
			return;
		}
		Settings.CrosshairColorIndex = 2;   // both ends have somewhere to go, so both arrows are drawn
		Next(9, 2);
		return;

	case 9:
		if (!Rows[VerifyIndex].ArrowLeft.bIsValid || !Rows[VerifyIndex].ArrowRight.bIsValid)
		{
			VerifyCheck(TEXT("G. COLOUR draws a '<' and a '>'"), false, TEXT("an arrow rect is missing"));
			Next(16, 0);
			return;
		}
		DebugPointerPos = Rows[VerifyIndex].ArrowLeft.GetCenter();
		Next(10, 1);
		return;

	case 10: bDebugPointerDown = true;  Next(11, 1); return;
	case 11: bDebugPointerDown = false; Next(12, 2); return;

	case 12:
		VerifyCheck(TEXT("G. a click on COLOUR's '<' steps it DOWN (it used to go forward)"),
			Settings.CrosshairColorIndex == 1, FString::Printf(TEXT("colour index 2 -> %d"), Settings.CrosshairColorIndex));
		DebugPointerPos = Rows[VerifyIndex].ArrowRight.GetCenter();
		Next(13, 2);
		return;

	case 13: bDebugPointerDown = true;  Next(14, 1); return;
	case 14: bDebugPointerDown = false; Next(15, 2); return;

	case 15:
		VerifyCheck(TEXT("G. ...and its '>' steps it UP"), Settings.CrosshairColorIndex == 2,
			FString::Printf(TEXT("colour index 1 -> %d"), Settings.CrosshairColorIndex));
		Next(16, 0);
		return;

	// ---- D. A SAVED SLOT CAN BE CLEARED, IN TWO PRESSES --------------------------------------------
	case 16:
		Settings.CrosshairColorIndex = VerifySavedColour;
		Settings.Save();
		DebugPointerPos = FVector2D(2.f, 2.f);
		if (Page != EPage::Settings)
		{
			GoBack();
		}
		Selected = FindActionRow(EAction::OpenLoadouts);
		ActivateSelected();
		Next(17, 3);
		return;

	case 17:
	{
		constexpr int32 SlotUnderTest = UTraceUserSettings::SavedLoadoutCount - 1;
		VerifySavedLoadout = Settings.GetSavedLoadout(SlotUnderTest);
		VerifySavedLoadoutName = Settings.GetSavedLoadoutName(SlotUnderTest);
		Settings.SetSavedLoadout(SlotUnderTest, FTraceLoadout::Uniform(ETraceCharacterId::Rocco));
		RebuildRows(EAction::EditLoadoutSlot, SlotUnderTest);
		VerifyIndex = Selected;

		TraceOptionsRebindProof::InjectKey(EKeys::BackSpace, /*bPressed=*/true);
		Next(18, 1);
		return;
	}

	case 18: TraceOptionsRebindProof::InjectKey(EKeys::BackSpace, false); Next(19, 1); return;

	case 19:
	{
		constexpr int32 SlotUnderTest = UTraceUserSettings::SavedLoadoutCount - 1;
		const bool bAsked = !Settings.GetSavedLoadout(SlotUnderTest).IsEmpty() && Rows.IsValidIndex(Selected)
			&& IsArmedRow(Rows[Selected]) && Rows[Selected].SlotIndex == SlotUnderTest;
		VerifyCheck(TEXT("D. CLEAR: the first BKSP on a saved slot asks (it used to do nothing)"), bAsked,
			FString::Printf(TEXT("slot %d %s, row %s armed %d"), SlotUnderTest + 1,
				Settings.GetSavedLoadout(SlotUnderTest).IsEmpty() ? TEXT("EMPTY") : TEXT("filled"), *RowName(Selected),
				(Rows.IsValidIndex(Selected) && IsArmedRow(Rows[Selected])) ? 1 : 0));
		TraceOptionsRebindProof::InjectKey(EKeys::BackSpace, true);
		Next(20, 1);
		return;
	}

	case 20: TraceOptionsRebindProof::InjectKey(EKeys::BackSpace, false); Next(21, 1); return;

	case 21:
	{
		constexpr int32 SlotUnderTest = UTraceUserSettings::SavedLoadoutCount - 1;
		VerifyCheck(TEXT("D. CLEAR: the second BKSP empties it, and the highlight stays on it"),
			Settings.GetSavedLoadout(SlotUnderTest).IsEmpty() && Rows.IsValidIndex(Selected)
				&& Rows[Selected].SlotIndex == SlotUnderTest,
			FString::Printf(TEXT("slot %d %s, selected %s"), SlotUnderTest + 1,
				Settings.GetSavedLoadout(SlotUnderTest).IsEmpty() ? TEXT("EMPTY") : TEXT("still filled"), *RowName(Selected)));

		Settings.SetSavedLoadout(SlotUnderTest, VerifySavedLoadout);
		Settings.SetSavedLoadoutName(SlotUnderTest, VerifySavedLoadoutName);

		// ---- B. CLOSING THE LOADOUT EDITOR LANDS ON THE SLOT IT EDITED -----------------------------
		Selected = FindActionRow(EAction::EditLoadoutSlot, 2);
		ActivateSelected();
		Next(22, 3);
		return;
	}

	case 22:
		VerifyCheck(TEXT("the loadout editor opens on slot 3"),
			LoadoutEditor.IsLibraryOpen() && LoadoutEditor.GetLibrarySlot() == 2, TEXT(""));
		LoadoutEditor.CloseLibrary();
		Next(23, 3);
		return;

	case 23:
		VerifyCheck(TEXT("B. closing the editor lands back on slot 3 (it used to land on slot 1)"),
			Page == EPage::Loadouts && Rows.IsValidIndex(Selected) && Rows[Selected].SlotIndex == 2,
			FString::Printf(TEXT("selected %s"), *RowName(Selected)));

		// ---- F. A PAUSED WORLD DOES NOT FREEZE THE MENU ---------------------------------------------
		Close();
		PC->SetPause(true);
		OpenVideo();
		Next(24, 3);
		return;

	case 24:
		VerifyIndex = Selected;
		VerifyRealStart = FPlatformTime::Seconds();
		VerifyMenuStart = Now;
		VerifyWorldStart = (VerifyWorld != nullptr) ? VerifyWorld->GetTimeSeconds() : 0.0;
		TraceOptionsRebindProof::InjectKey(EKeys::Down, true);
		Next(25, 0);
		return;

	case 25:
		// HELD for 1.3 s of REAL time: the repeat delay is 0.38 s and the interval 0.12 s, so a running
		// clock moves about eight rows. A frozen one moves exactly one.
		if (FPlatformTime::Seconds() - VerifyRealStart < 1.3)
		{
			return;
		}
		TraceOptionsRebindProof::InjectKey(EKeys::Down, false);
		Next(26, 2);
		return;

	case 26:
	{
		int32 Moves = 0;
		for (int32 Index = VerifyIndex + 1; Index <= Selected && Rows.IsValidIndex(Index); ++Index)
		{
			Moves += Rows[Index].IsSelectable() ? 1 : 0;
		}
		const bool bPaused = VerifyWorld != nullptr && VerifyWorld->IsPaused()
			&& FMath::IsNearlyEqual(VerifyWorld->GetTimeSeconds(), VerifyWorldStart);
		VerifyCheck(TEXT("F. the world is really paused (its clock did not move)"), bPaused,
			bPaused ? TEXT("") : TEXT("INCONCLUSIVE: this world would not pause, so F proves nothing here"));
		VerifyCheck(TEXT("F. ...and the menu's clock kept running"), Now - VerifyMenuStart >= 1.0f,
			FString::Printf(TEXT("menu clock +%.2fs over %.2fs real"), Now - VerifyMenuStart,
				FPlatformTime::Seconds() - VerifyRealStart));
		VerifyCheck(TEXT("F. ...so a held DOWN repeats (it used to move one row)"), Moves >= 3,
			FString::Printf(TEXT("%d row(s) in 1.3 s, %s -> %s"), Moves, *RowName(VerifyIndex), *RowName(Selected)));
		VerifyCheck(TEXT("F. ...and the root is titled PAUSED only while paused"), bWorldPaused == bPaused, TEXT(""));

		PC->SetPause(false);
		Close();
		Next(30, 1);
		return;
	}

	case 30:
	default:
	{
		bDebugPointer = false;
		bDebugPointerDown = false;
		if (IsOpen())
		{
			Close();
		}

		VerifyCheck(TEXT("the overlay's first frame had every sprite drawable (TraceMenuKit::Prime)"),
			TraceOptionsMenuArt::GFirstDrawUnready == 0,
			FString::Printf(TEXT("%d not drawable on the first frame"), TraceOptionsMenuArt::GFirstDrawUnready));

		VerifyStep = 0;
		if (VerifyFailures == 0)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[MenuVerify] ===== PASS — %d checks: the pointer waits to be moved, BACK keeps your place, ")
				TEXT("RESET and CLEAR ask first, the arrows step both ways, and a paused world does not freeze the menu ====="),
				VerifyChecks);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TEXT("[MenuVerify] ===== *** FAIL *** %d of %d check(s) — see above ====="),
				VerifyFailures, VerifyChecks);
		}
		return;
	}
	}
}
#endif
