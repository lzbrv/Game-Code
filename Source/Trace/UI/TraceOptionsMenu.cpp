// Trace — settings overlay implementation. See TraceOptionsMenu.h.

#include "UI/TraceOptionsMenu.h"

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
#include "TextureResource.h"             // FTextureResource::TextureRHI - see IsDrawable
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"         // Trace.Menu.Settings / Trace.Menu.Video, the capture hooks
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"            // -TraceMenuActivate, the paused-world capture hook
#include "Misc/Parse.h"
#include "Scalability.h"
#include "Settings/TraceGameUserSettings.h"
#include "Trace.h"                       // LogTraceGame
#include "UI/TraceMatchOptions.h"        // TraceCharacters - the spec v14 §3 toggle's storage
#include "UI/Text/TraceCanvasText.h" // spec v22 §A1 - this page types in the artist's face
#include "Audio/TraceAudio.h"           // spec v26 §9 - ButtonPress on the submenu rows too
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
// Palette
//
// The same two hues as the title screen — a cyan that carries the interface and an amber that only
// ever means "danger, or something is waiting on you" — over near-black. Restated here rather than
// shared with TraceMenuHUD.cpp's TraceMenuStyle because this overlay also draws over the MATCH,
// where the title screen's palette namespace is not in scope, and a header shared between them
// would exist purely to hold six colours.
// =================================================================================================

// =================================================================================================
// SPEC v22 §A1 — THIS PAGE IS ONE TYPEFACE, AND IT IS THE ARTIST'S
// =================================================================================================
//
// The settings page had exactly the defect the spec calls the headline, one screen further in than
// anybody looked. Its CONTROLS header drew the artist's baked KEYBIND and KEY word sprites, and
// every other string on the page — SETTINGS, DISPLAY, MOUSE, MOVE FORWARD, the key names, the
// numbers — came out of AHUD::DrawText in the engine's stand-in font, in the same column, four
// pixels apart. Photographed at 1920x1080 in v22integ_03_settings_open.png before this change.
//
// Now every one of them goes through UI/Text: TraceCanvasText blits one atlas quad per glyph, and
// the word sprites are retired the same way the title row's PLAY and SETTINGS were.
//
// ---- THE ONE THING THAT IS NOT OBVIOUS: THE UNITS ------------------------------------------------
//
// This page's whole layout is expressed as (UFont*, Scale) pairs, where Scale is a MULTIPLIER on a
// bitmap font's natural size — `FontMedium, 1.5f * UIScale`. TraceText wants a point size in
// pixels. Converting with a constant would have been a guess, so SizeFor() measures instead: it
// asks the engine what line height that font at that scale actually produces, and returns the point
// size whose line height matches. TraceText::LineHeight is linear in size, so that inverse is exact.
//
// The consequence is the one that matters for a page this dense: every row keeps the height it had.
// MeasureHeight() below returns the same number it returned before this change, so the vertical
// rhythm of nineteen rows, their plates and their hit rects are all untouched. Only the letterforms
// and the WIDTHS move — and the widths are measured, not assumed, by the same call that draws.
//
// The fallback needs no branch here. With no atlas, TraceCanvasText types Lato at the same size and
// TraceText measures Lato, so this page degrades to "the wrong face, laid out correctly".
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
//   KEYBIND / KEY (the two column captions)       SOFACHROME. They are drawn INSTEAD of the word
//                                                 CONTROLS, on the header row, and they are headers
//                                                 for the two columns of every Binding row below.
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
// ---- THE UNITS DID NOT MOVE, AND THAT IS LOAD-BEARING --------------------------------------------
//
// Every face shares the 116 px line box (Scripts/import_font_atlas.py refuses to emit a metrics
// header whose sheets disagree about it), so SizeFor() and Height() are face-INDEPENDENT and the
// vertical rhythm of nineteen rows, their plates and their hit rects are all exactly what they were.
// WIDTHS are not: Erbaum measures the alphabet at 1823 px against Sofachrome's 2634 at em 96, so
// anything that measures a string MUST be handed the face it will be drawn in. That is why Width()
// and MeasureWidth() below take a weight and why it has no default — the four call sites that measure
// (the two header rules, the key chip's width, the arrow gutter) each state their face, and the
// compiler will not let a fifth forget.
// =================================================================================================

namespace TraceOptionsMenuType
{
	/**
	 * The face a HEADER is set in: Sofachrome ExtraLight, the same sheet the title screen uses.
	 *
	 * Named rather than written as ETraceTextWeight::Light at eleven call sites, because the reason a
	 * call passes it is "this is a header", not "this is the light weight". Re-pointing every header
	 * on this page at another sheet is then one edit here.
	 */
	static constexpr ETraceTextWeight HeaderFace = ETraceTextWeight::Light;

	/** The face BODY is set in: Erbaum Bold, the face the in-match HUD already uses (spec v25 §4). */
	static constexpr ETraceTextWeight BodyFace = ETraceTextWeight::Hud;

	/** The point size whose line height equals what @p Font at @p Scale draws. See the block above. */
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

		// Only reachable with no HUD or a font the engine could not measure. 16 px is the engine's own
		// medium font line box, so the page comes out readable rather than microscopic.
		return FMath::Max(1.f, 16.f * Scale);
	}

	/**
	 * Width @p Text occupies IN @p Weight.
	 *
	 * The weight is not optional and must be the one the caller is about to DRAW in: the three faces
	 * share no advances at all (see the §2 block above), so measuring in one and drawing in another
	 * puts a rule through a word or leaves a chip a third too wide.
	 */
	static float Width(AHUD* HUD, const FString& Text, UFont* Font, float Scale, ETraceTextWeight Weight)
	{
		TraceText::FStyle Style(SizeFor(HUD, Font, Scale));
		Style.Weight = Weight;
		return TraceText::MeasureWidth(Text, Style);
	}

	/**
	 * The LINE BOX, which is what every caller on this page uses it for.
	 *
	 * NO WEIGHT, deliberately, and it is not an oversight: the line box is identical in all three
	 * faces by construction — Scripts/import_font_atlas.py refuses to emit a metrics header whose
	 * sheets disagree about it — so this answer cannot depend on the face. That is exactly why §2's
	 * face split moves no row on this page.
	 */
	static float Height(AHUD* HUD, UFont* Font, float Scale)
	{
		return TraceText::LineHeight(SizeFor(HUD, Font, Scale));
	}

	static void Draw(AHUD* HUD, const FString& Text, const FLinearColor& Color,
		float X, float Y, UFont* Font, float Scale, ETraceTextWeight Weight,
		TraceText::EHAlign HAlign = TraceText::EHAlign::Left)
	{
		TraceText::FStyle Style(SizeFor(HUD, Font, Scale), Color);
		Style.Weight = Weight;
		Style.HAlign = HAlign;
		TraceCanvasText::Draw(HUD, Text, X, Y, Style);
	}
}

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
//     a megabyte for ten small UI textures and makes both failures impossible — and it is done here,
//     at file scope, rather than in a member, so the header's invariant still holds.
//
// CANVAS HAS NO 9-SLICE. FCanvasTileItem stretches the whole bitmap, so the 160x91 chip drawn 120x17
// would come out with its corner squashed 7:1 — which reads as sloppy art rather than as a missing
// engine feature. Everything with a corner is therefore drawn as a THREE-slice in X: left cap,
// stretched middle, right cap, with the cap's on-screen width derived from the sprite's HEIGHT
// scale, which is what keeps the corner circular at every row width. Three and not nine because the
// vertical scale is the reference — nothing here is ever stretched past its natural aspect in Y.
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

	enum class ESprite : uint8
	{
		PlateDefault = 0,
		PlateHover,
		PlateDisabled,
		SliderTrack,
		SliderHandle,
		ValueBox,
		WordKeybind,
		WordKey,
		Cursor,
		Chevron,
		Count
	};

	static const TCHAR* const SpritePaths[int32(ESprite::Count)] =
	{
		TEXT("/Game/Trace/UI/Art/T_MenuBtn_Default.T_MenuBtn_Default"),
		TEXT("/Game/Trace/UI/Art/T_MenuBtn_Hover.T_MenuBtn_Hover"),
		TEXT("/Game/Trace/UI/Art/T_MenuBtn_Disabled.T_MenuBtn_Disabled"),
		TEXT("/Game/Trace/UI/Art/T_MenuSliderTrack.T_MenuSliderTrack"),
		TEXT("/Game/Trace/UI/Art/T_MenuSliderHandle.T_MenuSliderHandle"),
		TEXT("/Game/Trace/UI/Art/T_MenuValueBox.T_MenuValueBox"),
		TEXT("/Game/Trace/UI/Art/T_MenuWord_Keybind.T_MenuWord_Keybind"),
		TEXT("/Game/Trace/UI/Art/T_MenuWord_Key.T_MenuWord_Key"),
		TEXT("/Game/Trace/UI/Art/T_MenuCursor.T_MenuCursor"),
		TEXT("/Game/Trace/UI/Art/T_MenuBack.T_MenuBack"),
	};

	static TWeakObjectPtr<UTexture2D> GCache[int32(ESprite::Count)];

	/** Set only on a genuine load failure, so a collected texture is re-fetched but a missing one is not re-hunted every frame. */
	static bool GFailed[int32(ESprite::Count)] = {};

	/**
	 * *** A LOADED TEXTURE IS NOT A DRAWABLE ONE, AND DRAWING ONE ANYWAY IS A CRASH. ***
	 *
	 * `AHUD::DrawTexture` passes `Texture->GetResource()` STRAIGHT into an FCanvasTileItem and checks
	 * only the UTexture (Engine HUD.cpp:986). A texture that is LOADED but whose render resource has
	 * no RHI texture yet therefore becomes a batched element the render thread cannot draw, and it
	 * dies on it: SIGSEGV in FBatchedElements::Draw at address 0x30, on the render thread, ~130 ms
	 * after this page first drew.
	 *
	 * MEASURED, in this order, because the first two answers were wrong:
	 *   - `Trace.Menu.Art 0` (no textures at all) ran clean, and so did the JOIN prompt, which draws
	 *     rects and atlas text and no textures. So it was the sprites, not the surface.
	 *   - guarding on `GetResource() != nullptr` did NOT fix it: the resource object exists straight
	 *     away. It is `FTextureResource::TextureRHI` that arrives later, from the render thread.
	 *   - guarding on that fixed it. Same binary, same arm, six captures, no crash.
	 *
	 * THIS IS A LATENT BUG SPEC v23 §A2 EXPOSED, not one it introduced. LoadObject returns the object
	 * as soon as the package is in memory and the RHI texture lands a frame or two later. Until v23
	 * this overlay could not be on screen without the CANVAS title screen being on screen underneath
	 * it — that is the whole defect §A2 fixed — and that screen draws TraceMenuCanvasArt out of the
	 * same /Game/Trace/UI/Art package, earlier in the same DrawHUD, which happened to warm these ten
	 * textures before this page ever asked for one. Take the Canvas title screen away and this page
	 * is the first thing in the process to touch that package.
	 *
	 * The guard costs one pointer compare per sprite per frame and fails the way every other sprite
	 * failure on this page fails: Sprite() returns null, the caller draws the plain rectangle it drew
	 * before spec v20, for the one or two frames before the RHI texture exists.
	 */
	static bool IsDrawable(const UTexture2D* Tex)
	{
		if (Tex == nullptr)
		{
			return false;
		}
		const FTextureResource* Resource = Tex->GetResource();
		if (Resource == nullptr || !Resource->TextureRHI.IsValid())
		{
			// Loud once per sprite: this is the state that used to crash, so a build that starts
			// hitting it a lot is a build whose art is arriving later than this page draws.
			UE_LOG(LogTraceGame, Verbose, TEXT("[Options] '%s' is loaded but has no RHI texture yet; ")
				TEXT("drawing the plain rectangle this frame."), *Tex->GetName());
			return false;
		}
		return true;
	}

	/** The texture, or null — which every caller treats as "draw the rectangle you drew before". */
	static UTexture2D* Sprite(ESprite Which)
	{
		if (GEnabled == 0)
		{
			return nullptr;
		}

		const int32 Index = int32(Which);
		if (UTexture2D* Cached = GCache[Index].Get())
		{
			return IsDrawable(Cached) ? Cached : nullptr;
		}
		if (GFailed[Index])
		{
			return nullptr;
		}

		UTexture2D* Loaded = LoadObject<UTexture2D>(nullptr, SpritePaths[Index]);
		if (Loaded == nullptr)
		{
			// Once. A warning per frame per sprite in front of a paused match is its own defect.
			GFailed[Index] = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Options] Menu art '%s' did not load; that control keeps its plain rectangle."),
				SpritePaths[Index]);
			return nullptr;
		}

		// See point 3 in the block above: nothing else in a match holds these.
		Loaded->AddToRoot();
		GCache[Index] = Loaded;
		return IsDrawable(Loaded) ? Loaded : nullptr;
	}

	/** Plain stretch. For alpha masks and for anything whose corners are not being distorted. */
	static void Draw(AHUD* HUD, UTexture2D* Tex, float X, float Y, float W, float H, const FLinearColor& Tint)
	{
		HUD->DrawTexture(Tex, X, Y, W, H, 0.f, 0.f, 1.f, 1.f, Tint, BLEND_Translucent);
	}

	/**
	 * Three-slice in X: the two caps keep their shape, only the middle stretches.
	 *
	 * @param CapU   the cap as a fraction of the sprite's WIDTH (a texture coordinate)
	 * @param CapPx  the cap's width on screen, derived from the sprite's height scale by the caller
	 */
	static void Draw3H(AHUD* HUD, UTexture2D* Tex, float X, float Y, float W, float H,
		float CapU, float CapPx, const FLinearColor& Tint)
	{
		// A row narrower than two caps is not a layout this screen produces, but clamping is one line
		// and the alternative is the two caps drawing over each other back to front.
		const float Cap = FMath::Min(CapPx, W * 0.5f);
		const float MidW = W - Cap * 2.f;

		HUD->DrawTexture(Tex, X, Y, Cap, H, 0.f, 0.f, CapU, 1.f, Tint, BLEND_Translucent);
		if (MidW > 0.f)
		{
			HUD->DrawTexture(Tex, X + Cap, Y, MidW, H, CapU, 0.f, 1.f - CapU * 2.f, 1.f, Tint, BLEND_Translucent);
		}
		HUD->DrawTexture(Tex, X + W - Cap, Y, Cap, H, 1.f - CapU, 0.f, CapU, 1.f, Tint, BLEND_Translucent);
	}

	/**
	 * A sprite that was cut as a PLATE plus a margin of glow, in the sheet's own pixels.
	 *
	 * Mirrored from TraceMenuArtStyle::FSpriteFrame rather than included: that header describes Slate
	 * Box brushes, which do not exist on this side of the fence, and the numbers below are the sheet's
	 * and not the engine's. If the slicer's crop boxes change, both copies change.
	 */
	struct FPlateFrame
	{
		float PlateW;
		float PlateH;
		/** Sheet pixels of glow kept OUTSIDE the plate on every side. */
		float Glow;
		/** Sheet pixels from the sprite's edge to where the corner curve is fully open. */
		float Cap;

		float SpriteW() const { return PlateW + Glow * 2.f; }
	};

	/** The wide button: plate 4723x1230 inside a 4979x1486 crop, corner open by 428. */
	static const FPlateFrame ButtonFrame = { 4723.f, 1230.f, 128.f, 428.f };

	/** The chip beside a slider: ring 1034x538 inside a 1154x656 crop, corner open by 150. */
	static const FPlateFrame ValueFrame  = { 1034.f,  538.f,  60.f, 150.f };

	/**
	 * Draws @p Tex so that its PLATE lands exactly on (X, Y, W, H), with the glow overhanging outside.
	 *
	 * Forget the overhang and the plate comes out a fifth small inside its own row — which is the
	 * mistake the UMG row widget documents having made once already.
	 */
	static void DrawPlate(AHUD* HUD, UTexture2D* Tex, const FPlateFrame& Frame,
		float X, float Y, float W, float H, const FLinearColor& Tint)
	{
		// Height is the reference scale: the glow and the corner are square in the sheet, so scaling
		// both by H/PlateH is what keeps the corner circular however wide the row is.
		const float Scale = H / Frame.PlateH;
		const float GlowPx = Frame.Glow * Scale;
		const float CapPx = Frame.Cap * Scale;
		const float CapU = Frame.Cap / Frame.SpriteW();

		Draw3H(HUD, Tex, X - GlowPx, Y - GlowPx, W + GlowPx * 2.f, H + GlowPx * 2.f, CapU, CapPx, Tint);
	}

	// Sprite aspects, measured off the PNGs rather than guessed, so nothing here is stretched.
	static constexpr float CursorAspect  = 64.f / 87.f;
	static constexpr float HandleAspect  = 64.f / 87.f;
	static constexpr float ChevronAspect = 96.f / 125.f;
	static constexpr float KeybindAspect = 256.f / 42.f;
	static constexpr float KeyAspect     = 128.f / 47.f;

	/**
	 * The tip of T_MenuCursor, as a fraction of the sprite, measured from its alpha: the arrow's point
	 * is at about (11.5, 6.5) of 64x87.
	 *
	 * It matters because PollMouse hit-tests at CursorPos. A centre-anchored arrow would draw its
	 * point about eleven pixels away from the pixel it is about to click, and every click in every
	 * screenshot would look like it landed on the wrong row.
	 */
	static constexpr float CursorTipU = 0.180f;
	static constexpr float CursorTipV = 0.075f;

	/** The slider sprite is a trough: its solid rail occupies rows 6..17 of its 23. */
	static constexpr float TrackRailTopV = 6.f / 23.f;
	static constexpr float TrackRailV    = 11.f / 23.f;

	/**
	 * Draws T_MenuSliderTrack as a trough, AVOIDING THE HANDLE THAT IS BAKED INTO IT.
	 *
	 * MEASURED, because it cost a capture to find: the slicer's crop kept the artist's own handle
	 * blade inside the track sprite. Columns 37..78 of its 512 — 7.2% to 15.2% along — are a bright
	 * white diagonal, and a plain stretch therefore paints a SECOND, immovable handle at a fixed
	 * tenth of every slider, next to the real one. The first capture of this work had two blades on
	 * every row and it read as a rendering bug.
	 *
	 * So the middle is sampled from a clean band in the sprite's uniform centre rather than from the
	 * span between the caps. The caps themselves (16 px at each end, the artist's soft lip) are clear
	 * of the blade and are drawn as they were cut.
	 *
	 * The real fix is a re-cut in Scripts/slice-ui-assets.py, which is not this agent's file — this
	 * is a faithful presentation of the sprite as shipped, not a workaround hiding a bad asset.
	 */
	static void DrawTrough(AHUD* HUD, UTexture2D* Tex, float X, float Y, float W, float H, const FLinearColor& Tint)
	{
		constexpr float CapU = 16.f / 512.f;   // the lip, and it is clean
		constexpr float MidU = 0.30f;          // a band of the uniform centre, well past the blade
		constexpr float MidUW = 0.40f;

		const float Cap = FMath::Min(H * (16.f / 23.f), W * 0.5f);
		const float MidW = W - Cap * 2.f;

		HUD->DrawTexture(Tex, X, Y, Cap, H, 0.f, 0.f, CapU, 1.f, Tint, BLEND_Translucent);
		if (MidW > 0.f)
		{
			HUD->DrawTexture(Tex, X + Cap, Y, MidW, H, MidU, 0.f, MidUW, 1.f, Tint, BLEND_Translucent);
		}
		HUD->DrawTexture(Tex, X + W - Cap, Y, Cap, H, 1.f - CapU, 0.f, CapU, 1.f, Tint, BLEND_Translucent);
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
		for (int32 Index = 0; Index < int32(ESprite::Count); ++Index)
		{
			// GCache directly, NOT Sprite(): Sprite() would LoadObject and flush async loading from
			// inside a draw pass, which is a thing this diagnostic must observe and not cause.
			Line += FString::Printf(TEXT("%d=%s "), Index, State(GCache[Index].Get()));
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

		int32 Resolved = 0;
		for (int32 Index = 0; Index < int32(ESprite::Count); ++Index)
		{
			if (Sprite(ESprite(Index)) != nullptr)
			{
				++Resolved;
			}
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Options] Menu art: %d of %d sprites resolved (Trace.Menu.Art = %d). %s"),
			Resolved, int32(ESprite::Count), GEnabled,
			(GEnabled == 0)
				? TEXT("Art is OFF: every control is drawing the plain rectangle it drew before spec v20.")
				: ((Resolved == int32(ESprite::Count))
					? TEXT("Plates, slider, chips, KEYBIND/KEY and the cursor are the artist's, on both hosts.")
					: TEXT("Some controls are drawing their fallback rectangles; see the warnings above.")));
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
		if (Page != EPage::Settings)
		{
			OpenSettings();
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
 * Not ECVF_Cheat: it changes no gameplay rule, only whether a menu click is delivered.
 */
static int32 GTraceMenuPressDelivery = 1;
static FAutoConsoleVariableRef CVarTraceMenuPressDelivery(
	TEXT("Trace.Menu.PressDelivery"),
	GTraceMenuPressDelivery,
	TEXT("Spec v28 sec 3a. 1 (default): while the settings/pause overlay is open the game viewport is "
	     "put in CaptureDuringMouseDown so a mouse PRESS reaches the game, and the previous mode is "
	     "restored on close. 0 is the RED arm - the pre-v28 behaviour, where the title screen's "
	     "NoCapture mode swallows the first press of every click."),
	ECVF_Default);

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
	// SPEC v28 §3a — before the first frame the player can click on. See SetPressDeliveryOverride.
	SetPressDeliveryOverride(true);

	RebuildRows();
	UE_LOG(LogTraceGame, Display, TEXT("[Options] Crosshair settings opened."));
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
	// SPEC v28 §3c — the chip column is part of "where the player was", so it resets with everything
	// else. Re-opening the page on the second chip of a row nobody is looking at would be a surprise.
	CapturingSlot = 0;
	SelectedBindingSlot = 0;
	PressedRow = INDEX_NONE;
	bDraggingSlider = false;
	bAutoDetectPending = false;
	LastAdjustDir = 0;
	LastNavDir = 0;

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
		AddHeader(TEXT("SHAPE"));
		AddValue(ERowKind::Slider, TEXT("SIZE"), ESetting::CrosshairSize);
		AddValue(ERowKind::Slider, TEXT("THICKNESS"), ESetting::CrosshairThickness);
		AddValue(ERowKind::Slider, TEXT("GAP"), ESetting::CrosshairGap);

		AddHeader(TEXT("APPEARANCE"));
		AddValue(ERowKind::Choice, TEXT("COLOUR"), ESetting::CrosshairColor);
		AddValue(ERowKind::Slider, TEXT("OPACITY"), ESetting::CrosshairOpacity);
		AddValue(ERowKind::Toggle, TEXT("CENTRE DOT"), ESetting::CrosshairDot);
		AddValue(ERowKind::Toggle, TEXT("OUTLINE"), ESetting::CrosshairOutline);

		// Not decoration. The preview beside this list draws at ACTUAL SIZE, which at 1080p is a cross
		// about twenty pixels across sitting in a box ten times that — and a player who does not know
		// it is 1:1 reads that as the preview being broken. The note is how they are told.
		AddNote(TEXT("PREVIEW IS ACTUAL SIZE, OVER THE TWO SURFACES THE ARENA IS MADE OF."));

		AddHeader(TEXT(""));
		AddAction(TEXT("RESET TO DEFAULTS"), EAction::ResetCrosshairDefaults);
		AddAction(TEXT("BACK"), EAction::Back);
	}
	else if (Page == EPage::Settings)
	{
		// First row on the page, above the mouse. Same reasoning as the pause root's VIDEO entry —
		// and this is the ONLY route to the video page from the title screen, where there is no
		// pause root at all, so it cannot be buried at the bottom next to RESET.
		AddHeader(TEXT("DISPLAY"));
		AddAction(TEXT("VIDEO SETTINGS"), EAction::OpenVideo);

		// SPEC v29 §3. Beside VIDEO SETTINGS rather than in a section of its own: both rows are doors
		// to a page about how the game LOOKS, and this is the only route the title screen has to
		// either of them — there is no pause root there to hang a shortcut on.
		AddAction(TEXT("CROSSHAIR"), EAction::OpenCrosshair);

		// ---- Match rules (spec v14 §3) ----------------------------------------------------------
		//
		// Above MOUSE and above CONTROLS, because it is the only row on this page that changes what
		// the GAME is rather than how it is driven, and because the one thing a player is looking for
		// when they come here about characters is the switch that turns them off.
		//
		// The note is not optional. This row cannot retro-apply to a match already being served by
		// somebody else's machine, and a toggle that appears to do nothing is worse than no toggle —
		// the player needs to be told it is a HOST setting and that it lands on the next match.
		AddHeader(TEXT("MATCH"));
		AddValue(ERowKind::Toggle, TEXT("CHARACTERS"), ESetting::CharactersEnabled);
		AddNote(TEXT("OFF: EVERYONE PLAYS THE DEFAULT MANNEQUIN, NO ABILITIES, NO SELECT SCREEN."));
		AddNote(TEXT("APPLIES TO MATCHES YOU HOST, FROM THE NEXT MATCH. GOALS MODE ONLY."));

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
			Row.Label = Info.DisplayName;   // spec v29 §5: straight from the table, no override
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
	SelectedBindingSlot = 0;   // spec v28 §3c — and on its first chip
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

#if !UE_BUILD_SHIPPING
	// SPEC v28 §3a. BEFORE the closed-page early-out, because its first job is to OPEN the settings
	// page, and before PollInput below, because an edge it injects must not be read by the same frame
	// that queued it — see the block comment on TickRebindProof. Inert until armed.
	TickRebindProof(PC);
#endif

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

#if !UE_BUILD_SHIPPING
	// After input and before the draw, so the page a capture photographs is the page this frame
	// actually drew. Counted in DRAWN FRAMES - see the header for why seconds cannot work here.
	++DrawsSinceOpen;
	TickAutoActivate();

	// It presses a real row, and a real row can be BACK or QUIT. Same guard, same reason, as the one
	// after PollInput above: drawing a closed overlay leaves a frame of dimmed screen over a game
	// that has already resumed.
	if (Page == EPage::Closed)
	{
		return;
	}
#endif

	// ---- SPEC v23 §A2 — IN FRONT OF SLATE, ON BOTH HOSTS -----------------------------------------
	//
	// Held for the draw only, never across the input poll above: PollInput can close the page, and a
	// scope that outlived it would leave the game's canvas pointer swapped on a frame that drew
	// nothing.
	//
	// This one line is what reaches the IN-MATCH pause menu as well, without a line changing in
	// TraceHUD.cpp. Both hosts call this Tick, so both of them get a panel that draws over their UMG
	// screens instead of under them — the title screen's over WBP_TitleMenu, the pause menu's over
	// the in-match corner widgets. The owner asked for the art to reach the in-game menus; this is
	// the same requirement one layer down, and it would have been wrong to fix it only on the way in.
	//
	// Nothing below branches on the answer. When it is false the panel draws precisely where it drew
	// before, and it is the HOST that adapts (see ATraceMenuHUD::DrawHUD).
	Draw(HUD);
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
		CapturingAction = ETraceInputAction::Count;
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Rebind cancelled."));
		return;
	}

	// First poll of a fresh capture: record what was already down. A key that was held before the
	// capture existed was never a choice made inside it.
	if (bCaptureNeedsHeldSnapshot)
	{
		bCaptureNeedsHeldSnapshot = false;
		KeysHeldWhenCaptureOpened.Reset();
		for (const FKey& Held : BindableKeys())
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

	for (const FKey& Key : BindableKeys())
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

		// *** SPEC v28 §3a — CLOSE THE CAPTURE BEFORE THE WRITE, NOT AFTER IT. ***
		//
		// SetKey below calls Save(), Save() broadcasts UTraceUserSettings::OnChanged, and
		// ATracePlayerController::ApplyControlSettings runs on that broadcast — SYNCHRONOUSLY, inside
		// this call. That is a lot of code to run while this object still says "I am waiting for a key",
		// and every line of it is a line that could re-enter the menu. Clearing the flags first makes
		// the capture over at the moment the key is decided, which is also what the player is told by
		// the row: the chip stops flashing PRESS A KEY on the same frame their key lands in it.
		bCapturingKey = false;
		CapturingAction = ETraceInputAction::Count;
		KeysHeldWhenCaptureOpened.Reset();
		bCaptureNeedsHeldSnapshot = false;

		UTraceUserSettings::Get().SetKey(Action, Slot, Key);
		UE_LOG(LogTraceGame, Display, TEXT("[Options] Bound %s slot %d to '%s'. The action now holds %s."),
			TraceInputActions::Info(Action).DisplayName, Slot + 1, *UTraceUserSettings::DescribeKey(Key),
			*UTraceUserSettings::Get().DescribeBinding(Action));

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
			//
			// SPEC v28 §3c — ONE SLOT, the one the highlight is on. Clearing both from a single
			// Backspace would make the second bind impossible to remove on its own, and would delete a
			// key the player could not see themselves selecting.
			UTraceUserSettings::Get().ClearKey(Rows[Selected].Binding, ActiveBindingSlot());
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

	// Hover follows the pointer, but ONLY when the pointer actually moved.
	//
	// The guard is not about window-activation clicks (an earlier comment here claimed that and used
	// it to justify having no guard). It is about the keyboard: this runs AFTER PollNavigation, so
	// without it an arrow key moved Selected and the very same frame a STATIONARY cursor resting
	// over a row dragged it straight back. The measured symptom was that the arrow keys did nothing
	// whatsoever whenever the pointer happened to be over the list — and it is also why the
	// -TraceAutoSettings script landed on different rows depending on viewport size.
	const float CursorMoveThresholdSq = 4.f;   // 2 px; below that it is jitter, not intent
	const bool bCursorMoved = !bHasHoverCursorPos
		|| FVector2D::DistSquared(CursorPos, LastHoverCursorPos) > CursorMoveThresholdSq;
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

	// bCursorMoved, or the keyboard cannot win an argument with a resting pointer. A click still
	// selects regardless — that path reads HoverRow directly below.
	if (HoverRow != INDEX_NONE && !bDraggingSlider && bCursorMoved)
	{
		Selected = HoverRow;
	}

	if (bJustPressed)
	{
		PressedRow = HoverRow;
		bDraggingSlider = false;

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
	case ESetting::CharactersEnabled:
	// SPEC v29 §3 — the two crosshair toggles read like every other toggle on these pages. Sharing
	// this case rather than writing "ON"/"OFF" again is what stops one page's toggles from one day
	// saying ENABLED while another's say ON.
	case ESetting::CrosshairDot:
	case ESetting::CrosshairOutline:
		return (Value >= 0.5f) ? TEXT("ON") : TEXT("OFF");

	// ---- SPEC v29 §3 --------------------------------------------------------------------------
	//
	// "PX" and not "PIXELS": these are 1080p-REFERENCE pixels, which is what every layout number in
	// this project is in, and at 1920x1080 they are literal screen pixels. Spelling it out in the row
	// would be a claim that is exactly true at one resolution, so the unit is short and the honest
	// version lives in the header of Settings/TraceUserSettings.h.
	case ESetting::CrosshairSize:
	case ESetting::CrosshairGap:
		return FString::Printf(TEXT("%d PX"), FMath::RoundToInt(Value));

	case ESetting::CrosshairThickness:
		// One decimal, because the shipped default is 2.5 and rounding it to "2" or "3" on the row
		// would make the RESET row's result look like it had missed.
		return FString::Printf(TEXT("%.1f PX"), Value);

	case ESetting::CrosshairColor:
		// The palette's own names, from the settings class, for the same reason the video rows take
		// their labels from UTraceGameUserSettings: one spelling of "MAGENTA" in the game and in the
		// log that a bug report will be lined up against.
		return UTraceUserSettings::DescribeCrosshairColor(FMath::RoundToInt(Value));

	case ESetting::CrosshairOpacity:
		// Already in percent — see GetSettingValue, which is the only place the conversion happens.
		return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Value));

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
	if (Row.Kind != ERowKind::Slider)
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

	case EAction::OpenCrosshair:
		// Remember where we came from, exactly as OpenVideo does. Only the settings page carries this
		// row today, but "back" must mean the place the player actually came from rather than one
		// hardcoded parent — Trace.Menu.Crosshair can also land them here from nowhere.
		CrosshairReturnPage = Page;
		Page = EPage::Crosshair;
		IgnoreInputBeforeFrame = GFrameCounter + 1;
		RebuildRows();
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

	if (Page == EPage::Crosshair)
	{
		// No queued apply to flush, unlike the video page: every crosshair row is written and saved on
		// the press or the mouse-up that made it, because none of them re-creates a swap chain.
		if (CrosshairReturnPage == EPage::Root || CrosshairReturnPage == EPage::Settings)
		{
			Page = CrosshairReturnPage;
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

	// BODY, not a header (spec v26 §2): this is a live READOUT of the page's own effect — the same
	// class of thing as a row's value, and it is set in the same face the in-match HUD reports numbers
	// in, which is the face a player is already reading frame times in during a match.
	DrawTextRight(HUD, Line, Color, RightX, Y, FontSmall, 1.1f * UIScale, TraceOptionsMenuType::BodyFace);
}

// =================================================================================================
// Draw
// =================================================================================================

void FTraceOptionsMenu::Draw(AHUD* HUD)
{
	// Once per process, and on the first frame the panel is up rather than at module load: it forces
	// the ten UI textures resident before the player can move a selection, and it puts a line in the
	// log that says whether this screen is wearing the art or its fallbacks.
	TraceOptionsMenuArt::LogOnce();

#if !UE_BUILD_SHIPPING
	// Spec v24 §1. After LogOnce, so the first line reports the state LogOnce's loads left behind.
	TraceOptionsMenuArt::LogReadiness(DrawsSinceOpen);
#endif

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
	else if (Page == EPage::Crosshair)
	{
		// NARROWER THAN THE SETTINGS PAGE, to buy the room the preview sits in. Nine rows of short
		// labels and short values ("SIZE  11 PX") do not need 880 px, and the preview is worth more
		// than the whitespace it costs.
		PanelW = FMath::Min(ViewW * 0.50f, 560.f * UIScale);
	}

	// ---- SPEC v29 §3 — the live preview, BESIDE the panel ---------------------------------------
	//
	// Beside and not inside, because the panel is sized to its ROWS: its height is RowCount * Pitch,
	// and there is no row shape that can be four times its neighbours' height without breaking the
	// pitch every hit rect on the page is computed from. A box to the right of the list needs none of
	// that machinery and gets a preview big enough to judge a crosshair in.
	//
	// THE PAIR IS CENTRED, NOT THE PANEL. Otherwise the list stays put and the preview hangs off one
	// side, which reads as an element that escaped its layout rather than as a two-column page.
	//
	// AND IT IS CONDITIONAL. At a small window the pair does not fit, and a preview that overlapped
	// the panel would be worse than none — so the fit is tested and the page falls back to exactly
	// the single centred panel every other page draws. Measured: it fits at 1280x720 (UIScale 0.667:
	// 373 + 13 + 213 = 599 of 1229 available) and at 1920x1080 (560 + 20 + 320 = 900 of 1843).
	const bool bWantPreview = (Page == EPage::Crosshair);
	const float PreviewGap = 20.f * UIScale;
	const float PreviewW = FMath::Min(ViewW * 0.28f, 320.f * UIScale);
	const float PreviewH = FMath::Min(PreviewW, PanelH);

	const bool bDrawPreview = bWantPreview
		&& (PanelW + PreviewGap + PreviewW) <= (ViewW * 0.96f);

	const float GroupW = bDrawPreview ? (PanelW + PreviewGap + PreviewW) : PanelW;
	const float PanelX = (ViewW - GroupW) * 0.5f;
	const float PanelY = (ViewH - PanelH) * 0.5f;

	// The panel's own centre, not the screen's. Everything centred below — the title, the footer hint
	// — belongs to the PANEL, and on the crosshair page the two are no longer the same pixel.
	const float CX = PanelX + PanelW * 0.5f;

	HUD->DrawRect(TraceOptionsStyle::Panel, PanelX, PanelY, PanelW, PanelH);
	DrawFrame(HUD, PanelX, PanelY, PanelW, PanelH);

	if (bDrawPreview)
	{
		// After the panel, so the preview's own frame is never drawn under it, and before the rows so
		// nothing about it can move a row rect. See DrawCrosshairPreview.
		DrawCrosshairPreview(HUD, PanelX + PanelW + PreviewGap,
			PanelY + (PanelH - PreviewH) * 0.5f, PreviewW, PreviewH);
	}

	// ---- Title ---------------------------------------------------------------------------------
	FString Title = TEXT("SETTINGS");
	if (Page == EPage::Root)            { Title = TEXT("PAUSED"); }
	else if (Page == EPage::Video)      { Title = TEXT("VIDEO"); }
	else if (Page == EPage::Crosshair)  { Title = TEXT("CROSSHAIR"); }

	// SOFACHROME, and the spec names this string: "the word SETTINGS at the top of the settings page
	// stays Sofachrome while the rows beneath it become Erbaum" (v26 §2). PAUSED and VIDEO are the
	// same object on the other two pages.
	const float TitleY = PanelY + (22.f * UIScale);
	DrawTextCentered(HUD, Title, TraceOptionsStyle::Cyan, CX, TitleY, FontLarge, 1.9f * UIScale,
		TraceOptionsMenuType::HeaderFace);

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
	else if (Page == EPage::Video || Page == EPage::Crosshair)
	{
		// No BKSP/UNBIND on either page — there is nothing to unbind — and the hint says so rather
		// than offering a key that does nothing.
		Hint = TEXT("ARROWS  MOVE / ADJUST          ENTER  SELECT          ESC  BACK");
	}
	else
	{
		Hint = TEXT("ARROWS  MOVE / ADJUST      ENTER  SELECT      BKSP  UNBIND      ESC  BACK");
	}

	// BODY (spec v26 §2). The footer is a key legend — "BKSP UNBIND" is the same kind of string as the
	// key names in the rows above it, and it is read at a glance rather than scanned as a heading.
	DrawTextCentered(HUD, Hint, TraceOptionsStyle::InkDim, CX,
		PanelY + PanelH - (30.f * UIScale), FontSmall, 1.0f * UIScale, TraceOptionsMenuType::BodyFace);

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
		if (Row.Label.IsEmpty())
		{
			return;
		}

		const float Gap = 10.f * UIScale;
		float RuleLeft = 0.f;
		float RuleRight = X + W;
		bool bWordsDrawn = false;

		// THE CONTROLS HEADER IS WHERE THE ARTIST'S LETTERING BELONGS. KEYBIND and KEY were cut off
		// the sheet as a left/right pair on one row — a wide label plate beside a narrow key chip —
		// which is exactly the two columns every Binding row underneath this one is laid out in. They
		// are drawn INSTEAD of the word "CONTROLS", not beside it: at a 24px row there is no room for
		// three labels, and "KEYBIND ......... KEY" is what those columns actually are.
		//
		// FRow::Label is untouched. DebugGetRowRect and the click harness match on it, and nothing
		// about this is allowed to be a contract change.
		// SPEC v22 §A1 RETIRED THE SPRITES HERE TOO. This branch used to blit the artist's baked
		// T_MenuWord_Keybind and T_MenuWord_Key, which is why the shipped settings page had TWO faces
		// on it: those two words in the artist's squared lettering and the nineteen rows under them in
		// the engine's stand-in font, four pixels apart. The page can type in the artist's face now,
		// so the header is typed like everything else and the two-column KEYBIND / KEY layout — which
		// is the part that was actually worth keeping — survives unchanged.
		//
		// The sprites stay in the repo and stay loadable, exactly as A1 asks. Nothing draws them.
		if (Row.Label.Equals(TEXT("CONTROLS"), ESearchCase::IgnoreCase))
		{
			const FLinearColor WordTint = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.80f);
			const float ValueRightHdr = X + W - PadX;

			// SOFACHROME (spec v26 §2). These two are HEADERS in the strongest sense available on this
			// page: they are drawn instead of the word CONTROLS, and they name the two COLUMNS that
			// every Binding row below them is laid out in. The key names under KEY are Erbaum; the
			// word KEY is not.
			TraceOptionsMenuType::Draw(HUD, TEXT("KEYBIND"), WordTint, X, TextY, FontSmall, 1.0f * UIScale,
				TraceOptionsMenuType::HeaderFace);
			TraceOptionsMenuType::Draw(HUD, TEXT("KEY"), WordTint, ValueRightHdr, TextY, FontSmall,
				1.0f * UIScale, TraceOptionsMenuType::HeaderFace, TraceText::EHAlign::Right);

			// The rule has to stop short at BOTH ends, or it strikes straight through KEY. Measured in
			// the face the words were just DRAWN in — Erbaum is a third narrower than Sofachrome, so
			// measuring in the wrong one is exactly the strike-through this line exists to avoid.
			RuleLeft = X + MeasureWidth(HUD, TEXT("KEYBIND"), FontSmall, 1.0f * UIScale,
				TraceOptionsMenuType::HeaderFace) + Gap;
			RuleRight = ValueRightHdr - MeasureWidth(HUD, TEXT("KEY"), FontSmall, 1.0f * UIScale,
				TraceOptionsMenuType::HeaderFace) - Gap;
			bWordsDrawn = true;
		}

		if (!bWordsDrawn)
		{
			// DISPLAY, MOUSE, GAMEPLAY... — "HEADERS anywhere" (spec v26 §2). Same treatment as the
			// panel title one level up, and measured in the same face for the same reason.
			TraceOptionsMenuType::Draw(HUD, Row.Label,
				TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.75f),
				X, TextY, FontSmall, 1.0f * UIScale, TraceOptionsMenuType::HeaderFace);

			RuleLeft = X + MeasureWidth(HUD, Row.Label, FontSmall, 1.0f * UIScale,
				TraceOptionsMenuType::HeaderFace) + Gap;
		}

		if (RuleRight > RuleLeft)
		{
			HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.20f),
				RuleLeft, Y + H * 0.5f, RuleRight - RuleLeft, FMath::Max(1.f, 1.f * UIScale));
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
		// BODY (spec v26 §2). A Note is a SENTENCE about the control above it — prose, indented under
		// its row, the least heading-like string on the page.
		TraceOptionsMenuType::Draw(HUD, Row.Label,
			TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Amber, 0.78f),
			X + PadX, TextY, FontSmall, 1.0f * UIScale, TraceOptionsMenuType::BodyFace);
		return;
	}

	// ---- Plate ---------------------------------------------------------------------------------
	//
	// The artist's own button plate, one sprite per state, landed exactly on the row rect the mouse
	// already hit-tests. This is what makes a row here the same object as a row on the title screen —
	// and because the pause root is nothing but Action rows, it is also what puts the art in front of
	// a player who pressed Escape mid-match.
	//
	// The three states are the sheet's three states and they differ in the PLATE, not in the word:
	// default is a bare navy plate, hover adds the amber ring, disabled is near-black with a grey one.
	// So the selected row is marked by its ring and by a BRIGHTER label, never by a dimmer one — spec
	// v20 §0.5 is what happens when a decorative hover tint gets promoted to a selection indicator.
	bool bPlateDrawn = false;
	{
		const TraceOptionsMenuArt::ESprite Which = !Row.bEnabled
			? TraceOptionsMenuArt::ESprite::PlateDisabled
			: (bSelected ? TraceOptionsMenuArt::ESprite::PlateHover : TraceOptionsMenuArt::ESprite::PlateDefault);

		if (UTexture2D* Plate = TraceOptionsMenuArt::Sprite(Which))
		{
			// Unselected rows are knocked back rather than the selected row being knocked forward: a
			// page of thirty plates all at full strength is a wall, and the eye needs the selected one
			// to be the brightest thing in the list.
			const FLinearColor Tint = !Row.bEnabled
				? FLinearColor(0.80f, 0.80f, 0.80f, 0.75f)
				: (bSelected ? FLinearColor::White : FLinearColor(0.78f, 0.78f, 0.78f, 0.90f));

			TraceOptionsMenuArt::DrawPlate(HUD, Plate, TraceOptionsMenuArt::ButtonFrame, X, Y, W, H, Tint);
			bPlateDrawn = true;
		}
	}
	if (!bPlateDrawn)
	{
		HUD->DrawRect(FLinearColor(0.f, 0.02f, 0.04f, bSelected ? 0.85f : 0.45f), X, Y, W, H);
	}

	if (bSelected)
	{
		const float Pulse = 0.72f + 0.28f * FMath::Sin(Now * 4.5f);

		// A wash, never a dim: this only ever ADDS light to the selected plate. The pulse rides on the
		// wash and on the marker, not on the plate itself, so a before/after capture of the selected
		// row cannot be moved by more than a few percent by the phase it was caught at.
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.10f * Pulse), X, Y, W, H);

		// A SOLID RAIL ON THE LEADING EDGE — the same mark, in the same place, as the UMG title row
		// (spec v22 §A4, UI/Widgets/Menu/TraceMenuRowWidget.cpp), and for the same reason.
		//
		// This slot drew the artist's T_MenuBack crescent until this pass. A4 replaced it on the title
		// screen because a tapered stroke at ~20 px stops reading as a pointer and starts reading as a
		// stray ")" — and then left this page still drawing it, so pressing SETTINGS swapped the mark
		// for the thing A4 had just removed. Photographed at 1920x1080 beside VIDEO SETTINGS in
		// v22integ_03_settings_open.png before this change.
		//
		// A filled bar has no small-size failure mode; this is a rectangle rather than the title row's
		// rounded capsule only because there is no Slate brush on a Canvas and a 9-px-wide rounded end
		// is under one pixel of difference at this size. Height is tied to the row, not to a constant,
		// so it tracks the 18px row this page clamps to at 720p as well as the full-height one.
		const float MarkW = FMath::Max(3.f, 5.f * UIScale);
		const float MarkH = H * 0.66f;
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, Pulse),
			X - MarkW - (6.f * UIScale), Y + (H - MarkH) * 0.5f, MarkW, MarkH);
	}

	// A greyed row is not a selected row and cannot be, so this collapses to two states, not four.
	const FLinearColor LabelColor = !Row.bEnabled
		? TraceOptionsStyle::WithAlpha(TraceOptionsStyle::InkDim, 0.45f)
		: (bSelected ? TraceOptionsStyle::Ink : TraceOptionsStyle::InkDim);

	// Action rows are buttons; centring their label is what makes them read as one.
	if (Row.Kind == ERowKind::Action)
	{
		// Sofachrome on the pause ROOT, Erbaum on the settings and video pages. See FaceForAction().
		const ETraceTextWeight ActionFace = FaceForAction();

		// AUTO-DETECT is the row a confused player on a weak machine should press, so it is the only
		// button on the page drawn in amber with a plate behind it — everything else here is a list.
		if (Row.Action == EAction::AutoDetectQuality)
		{
			const bool bMeasuring = bAutoDetectPending;
			HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Amber, bSelected ? 0.30f : 0.16f), X, Y, W, H);

			const FString Text = bMeasuring ? TEXT("MEASURING THIS MACHINE...") : Row.Label;
			DrawTextCentered(HUD, Text, bMeasuring ? FLinearColor::White : TraceOptionsStyle::Amber,
				X + W * 0.5f, TextY, FontMedium, LabelScale, ActionFace);
			return;
		}

		DrawTextCentered(HUD, Row.Label, LabelColor, X + W * 0.5f, TextY, FontMedium, LabelScale, ActionFace);
		return;
	}

	// THE ROW LABEL — MOUSE SENSITIVITY, MOVE FORWARD, RESOLUTION SCALE. Erbaum Bold: this is the
	// "settings / submenu body text, keybind rows" the owner asked for, and it is the string that
	// makes the change visible (spec v26 §2).
	TraceOptionsMenuType::Draw(HUD, Row.Label, LabelColor, X + PadX, TextY, FontMedium, LabelScale,
		TraceOptionsMenuType::BodyFace);

	// ---- Value ---------------------------------------------------------------------------------
	const float ValueRight = X + W - PadX;
	const float ValueColW = 96.f * UIScale;

	if (Row.Kind == ERowKind::Binding)
	{
		// ---- SPEC v28 §3c — TWO CHIPS, LAID OUT RIGHT TO LEFT ------------------------------------
		//
		// "Up to TWO keybinds per action, both editable in the settings page." Two chips is what makes
		// the second bind a thing the player can SEE; without it a slot they cannot point at is a slot
		// that does not exist as far as the page is concerned.
		//
		// SLOT 1 IS DRAWN ONLY WHEN IT HAS SOMETHING TO SAY: it holds a key, or the row is selected (so
		// the player can see where a second bind would go and aim at it), or it is the slot being
		// captured right now. Seventeen rows each carrying a permanent empty box would make the page read
		// as half-broken, and sixteen of the seventeen ship with one key.
		const UTraceUserSettings& UserSettings = UTraceUserSettings::Get();
		const int32 ActiveSlot = bSelected ? ActiveBindingSlot() : 0;

		const float ChipGap = 6.f * UIScale;
		const float ChipY = Y + H * 0.14f;
		const float ChipH = H * 0.72f;

		// Right to left, so slot 1 keeps the position the single chip has always had when there is no
		// second bind — nothing on this page moves for a player who never uses the feature.
		float NextRight = ValueRight;

		for (int32 Slot = UTraceUserSettings::MaxKeysPerAction - 1; Slot >= 0; --Slot)
		{
			Row.KeyChip[Slot] = FBox2D(ForceInit);

			const bool bWaiting = bCapturingKey && CapturingAction == Row.Binding && CapturingSlot == Slot;
			const FKey Key = UserSettings.GetKey(Row.Binding, Slot);

			const bool bShow = (Slot == 0) || bWaiting || Key.IsValid() || bSelected;
			if (!bShow)
			{
				continue;
			}

			FString ValueText;
			FLinearColor ValueColor;
			if (bWaiting)
			{
				ValueText = TEXT("PRESS A KEY");
				ValueColor = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Amber, 0.6f + 0.4f * FMath::Sin(Now * 9.f));
			}
			else if (Key.IsValid())
			{
				ValueText = UTraceUserSettings::DescribeKey(Key);
				ValueColor = TraceOptionsStyle::Ink;
			}
			else if (Slot == 0)
			{
				// Amber for UNBOUND: it is not an error, but the player should not be able to miss it.
				ValueText = UTraceUserSettings::DescribeKey(Key);
				ValueColor = TraceOptionsStyle::Amber;
			}
			else
			{
				// An empty SECOND slot on the selected row. "+" and not "UNBOUND": the first chip already
				// says whether the action works at all, and this one is an invitation rather than a state.
				ValueText = TEXT("+");
				ValueColor = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::InkDim, 0.55f);
			}

			// UNCHANGED ARITHMETIC for the primary chip when it is alone. The chip rect this screen has
			// always drawn is exactly the shape of T_MenuValueBox, so the sprite is a one-for-one swap onto
			// a rectangle that already existed. The minimum width is the only number that moved: 120px was
			// chosen for one chip in the column, and two of those would not fit the column at 720p, so the
			// FLOOR drops to 84 and the text still sizes the box whenever it needs more.
			//
			// MEASURED IN THE FACE IT IS DRAWN IN (spec v26 §2): the chip is sized to its key name, so
			// measuring "LEFT SHIFT" in Sofachrome and setting it in Erbaum would leave a chip a third
			// wider than the word inside it on every keybind row on the page.
			const float MinChipW = (Slot == 0 && !Row.KeyChip[1].bIsValid) ? (120.f * UIScale) : (84.f * UIScale);
			const float PlateW = FMath::Max(MeasureWidth(HUD, ValueText, FontMedium, LabelScale,
				TraceOptionsMenuType::BodyFace) + (16.f * UIScale), MinChipW);
			const float ChipX = NextRight - PlateW;

			const bool bChipDrawn = DrawValueChip(HUD, ChipX, ChipY, PlateW, ChipH);

			// The cyan wash the chip used to BE, kept on top of the sprite at a whisper. The artist's chip
			// is the same navy as the plate it sits on — its amber ring is what separates them, and a
			// little light inside it is what stops the key name reading as a hole in the row.
			//
			// SPEC v28 §3c: the ACTIVE chip on a selected row is washed harder than its neighbour. That
			// difference is the only thing telling the player which of the two Enter and Backspace are
			// about, so it is not decoration.
			const bool bActiveChip = bSelected && (Slot == ActiveSlot);
			const float Wash = bWaiting ? 0.22f : (bActiveChip ? 0.16f : (bChipDrawn ? 0.05f : 0.10f));
			HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, Wash), ChipX, ChipY, PlateW, ChipH);

			// THE KEYBIND ROW'S KEY NAME. Erbaum Bold — "keybind rows" is one of the three surfaces §2
			// names, and this is the string on them a player actually reads.
			DrawTextCentered(HUD, ValueText, ValueColor, ChipX + PlateW * 0.5f, TextY, FontMedium, LabelScale,
				TraceOptionsMenuType::BodyFace);

			// The rect PollMouse hit-tests against. Written after the draw so it is exactly what was drawn.
			Row.KeyChip[Slot] = FBox2D(FVector2D(ChipX, ChipY), FVector2D(ChipX + PlateW, ChipY + ChipH));

			NextRight = ChipX - ChipGap;
		}
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
			ValueRight, TextY, FontMedium, LabelScale, TraceOptionsMenuType::BodyFace);
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
		// The value and both arrows are BODY (spec v26 §2), and ValueW is measured in that same face —
		// it is what positions the '<', so a measurement in the wrong face parks the left arrow inside
		// the word it is supposed to sit outside.
		const float ArrowColW = 20.f * UIScale;
		const float ValueW = MeasureWidth(HUD, ValueText, FontMedium, LabelScale,
			TraceOptionsMenuType::BodyFace);
		const float ValueTextRight = ValueRight - ArrowColW;

		DrawTextRight(HUD, ValueText, ValueColor, ValueTextRight, TextY, FontMedium, LabelScale,
			TraceOptionsMenuType::BodyFace);

		if (bSelected && Row.bEnabled)
		{
			const FLinearColor ArrowColor = TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.9f);
			if (Value > Min + UE_KINDA_SMALL_NUMBER)
			{
				DrawTextRight(HUD, TEXT("<"), ArrowColor, ValueTextRight - ValueW - (6.f * UIScale),
					TextY, FontMedium, LabelScale, TraceOptionsMenuType::BodyFace);
			}
			if (Value < Max - UE_KINDA_SMALL_NUMBER)
			{
				DrawTextRight(HUD, TEXT(">"), ArrowColor, ValueRight, TextY, FontMedium, LabelScale,
					TraceOptionsMenuType::BodyFace);
			}
		}
		return;
	}

	// ---- Slider --------------------------------------------------------------------------------
	const float Alpha = FMath::Clamp((Value - Min) / FMath::Max(UE_KINDA_SMALL_NUMBER, Max - Min), 0.f, 1.f);

	const FString ValueText = FormatSettingValue(Row.Setting, Value);
	const FLinearColor SliderValueColor = bSelected ? TraceOptionsStyle::Ink : TraceOptionsStyle::InkDim;

	// The sheet drew this control as a slider WITH a numbered chip beside it — the slicer erased the
	// "13" that was in it and kept the chip. So the chip goes back where the artist put it, behind the
	// value, in the column the track already stops short of (TrackRight below is its left edge).
	if (DrawValueChip(HUD, ValueRight - ValueColW, Y + H * 0.14f, ValueColW, H * 0.72f))
	{
		// Centred in the chip rather than right-aligned to the panel, because it now sits inside a
		// box and a number pinned to one wall of its box looks like a mistake.
		DrawTextCentered(HUD, ValueText, SliderValueColor, ValueRight - ValueColW * 0.5f, TextY, FontMedium, LabelScale,
			TraceOptionsMenuType::BodyFace);
	}
	else
	{
		DrawTextRight(HUD, ValueText, SliderValueColor, ValueRight, TextY, FontMedium, LabelScale,
			TraceOptionsMenuType::BodyFace);
	}

	const float TrackRight = ValueRight - ValueColW;
	const float TrackLeft = X + W * 0.48f;
	const float TrackW = FMath::Max(20.f * UIScale, TrackRight - TrackLeft);
	const float TrackH = FMath::Max(3.f, 6.f * UIScale);
	const float TrackY = Y + (H - TrackH) * 0.5f;

	bool bTrackDrawn = false;
	if (UTexture2D* TrackTex = TraceOptionsMenuArt::Sprite(TraceOptionsMenuArt::ESprite::SliderTrack))
	{
		// The sprite is a TROUGH — 23 rows with the solid rail occupying its middle eleven and a halo
		// above and below — so drawing it at the 6px the plain bar used would throw the shape away. It
		// takes the height the row can spare instead. TrackLeft and TrackW are untouched, which is the
		// only thing that matters: Row.Track is still computed from them at the bottom of this
		// function, and that rect is what a drag maps the pointer across.
		const float SpriteH = FMath::Clamp(H * 0.60f, TrackH, 21.f * UIScale);
		const float SpriteY = Y + (H - SpriteH) * 0.5f;
		TraceOptionsMenuArt::DrawTrough(HUD, TrackTex, TrackLeft, SpriteY, TrackW, SpriteH, FLinearColor::White);

		// THE FILL IS STILL A PLAIN BAR, deliberately. The sheet has no filled-track sprite (the
		// slicer's own note says the artist drew an empty trough), and tinting this one cannot make a
		// fill: a Canvas tint MULTIPLIES, so a navy trough times cyan is a darker navy trough. So the
		// rail is drawn INSIDE the artist's trough, inset to the sprite's own solid band, and the
		// artist's lip and halo frame it.
		const float RailTop = SpriteY + SpriteH * TraceOptionsMenuArt::TrackRailTopV;
		const float RailH = FMath::Max(2.f, SpriteH * TraceOptionsMenuArt::TrackRailV);
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, bSelected ? 0.95f : 0.55f),
			TrackLeft, RailTop, TrackW * Alpha, RailH);

		bTrackDrawn = true;
	}
	if (!bTrackDrawn)
	{
		HUD->DrawRect(TraceOptionsStyle::Trough, TrackLeft, TrackY, TrackW, TrackH);
		HUD->DrawRect(TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, bSelected ? 0.95f : 0.55f),
			TrackLeft, TrackY, TrackW * Alpha, TrackH);
	}

	// Handle, so the value has a thing to grab as well as a bar to read.
	const float HandleW = FMath::Max(4.f, 6.f * UIScale);
	const float HandleH = H * 0.56f;
	bool bHandleDrawn = false;
	if (UTexture2D* HandleTex = TraceOptionsMenuArt::Sprite(TraceOptionsMenuArt::ESprite::SliderHandle))
	{
		// The artist subtracted the rail from underneath the blade, so this composites over the trough
		// with no stub showing through. It is a white alpha mask, so the tint is the whole state.
		const float SpriteHandleH = FMath::Min(H * 0.92f, 26.f * UIScale);
		const float SpriteHandleW = SpriteHandleH * TraceOptionsMenuArt::HandleAspect;
		TraceOptionsMenuArt::Draw(HUD, HandleTex,
			TrackLeft + TrackW * Alpha - SpriteHandleW * 0.5f, Y + (H - SpriteHandleH) * 0.5f,
			SpriteHandleW, SpriteHandleH,
			bSelected ? FLinearColor::White : TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.85f));
		bHandleDrawn = true;
	}
	if (!bHandleDrawn)
	{
		HUD->DrawRect(bSelected ? FLinearColor::White : TraceOptionsStyle::Cyan,
			TrackLeft + TrackW * Alpha - HandleW * 0.5f, Y + (H - HandleH) * 0.5f, HandleW, HandleH);
	}

	// Stored AFTER drawing so the poll on the next frame drags against exactly what was on screen.
	// UNCHANGED BY SPEC v20: the sprite above is fitted to TrackLeft/TrackW, never the other way
	// round, so this is the same rectangle it has always been and a drag lands where it always did.
	Row.Track = FBox2D(FVector2D(TrackLeft, TrackY - H * 0.4f), FVector2D(TrackLeft + TrackW, TrackY + H * 0.4f));
}

void FTraceOptionsMenu::DrawCrosshairPreview(AHUD* HUD, float X, float Y, float W, float H)
{
	const UTraceUserSettings& Settings = UTraceUserSettings::Get();

	// ---- The two surfaces ----------------------------------------------------------------------
	//
	// Split down the middle, and both colours are taken from the thing they stand for rather than
	// invented: the left is the arena's black floor and the right is (193, 252, 253) — the lit cyan
	// surface named in ATraceHUD::DrawAimReticle's own note as the one a white reticle disappeared
	// against. That failure is the reason the outline exists at all, so it is the exact case a
	// crosshair preview has to be able to show.
	//
	// Integer-snapped like everything else that has to be pixel-crisp; a half-pixel seam under the
	// crosshair would be a grey band that looks like part of the crosshair.
	const float BoxX = FMath::RoundToFloat(X);
	const float BoxY = FMath::RoundToFloat(Y);
	const float BoxW = FMath::RoundToFloat(W);
	const float BoxH = FMath::RoundToFloat(H);
	const float HalfW = FMath::RoundToFloat(BoxW * 0.5f);

	HUD->DrawRect(FLinearColor(0.004f, 0.014f, 0.026f, 1.f), BoxX, BoxY, HalfW, BoxH);
	HUD->DrawRect(FLinearColor(0.757f, 0.988f, 0.992f, 1.f), BoxX + HalfW, BoxY, BoxW - HalfW, BoxH);

	// The same instrument-panel bezel the settings panel wears, so the preview reads as part of the
	// page rather than as a texture dropped onto it.
	DrawFrame(HUD, BoxX, BoxY, BoxW, BoxH);

	// ---- The crosshair, from the SAME geometry the HUD draws -----------------------------------
	//
	// UTraceUserSettings::BuildCrosshairBars, not a copy of it. PixelScale is UIScale and nothing
	// else: scale 1.0 is the FIRST-PERSON crosshair, which is the one the player is aiming a gun
	// with and therefore the one this page is about. The third-person carry view multiplies it by
	// UTraceSettings::ThirdPersonCrosshairScale, which is a designer's knob and not on this page.
	const float CenterX = BoxX + HalfW;
	const float CenterY = BoxY + FMath::RoundToFloat(BoxH * 0.5f);

	FTraceCrosshairBar Bars[TraceCrosshairMaxBars];
	const int32 NumBars = Settings.BuildCrosshairBars(CenterX, CenterY, UIScale, Bars);

	const FLinearColor Ink = Settings.GetCrosshairColor();
	const FLinearColor Outline = Settings.GetCrosshairOutlineColor();

	// *** DrawRect AND NEVER DrawLine. *** AHUD::DrawLine goes through the batched-element path,
	// which DISCARDS the alpha it is handed — every line comes out fully opaque. The OPACITY row on
	// this very page would then be a control whose effect could not be seen in the preview beside it,
	// and the OUTLINE row's "off" (an alpha of zero) would draw a solid black box. Rects carry alpha.
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
	// BODY (spec v26 §2): this is a label on a control, on a submenu, in the same rhythm as the rows
	// to its left. Erbaum Bold, like every other string on this page that is not a heading.
	//
	// Drawn on the DARK half so it is legible whatever the preview's own colours are doing, and
	// pinned to the bottom of the box rather than under it, because it belongs to the box.
	const float CaptionScale = 0.95f * UIScale;
	TraceOptionsMenuType::Draw(HUD, TEXT("PREVIEW"),
		TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Cyan, 0.85f),
		BoxX + (10.f * UIScale), BoxY + (8.f * UIScale), FontSmall, CaptionScale,
		TraceOptionsMenuType::BodyFace);

	// The live numbers, so the preview is readable as evidence in a screenshot and not only by eye.
	// One line, in the same face, derived from the same accessors the crosshair above was built from.
	//
	// ON ITS OWN DARK STRIP, spanning the full width. The line is longer than the dark half of the
	// box, so without the strip its tail would be near-white ink on the lit cyan surface — invisible,
	// which is the exact defect the cyan half is here to demonstrate. A caption that fell into it
	// would be an unintentional demonstration.
	const FString Readout = FString::Printf(TEXT("%d / %.1f / %d  %s  %d%%"),
		FMath::RoundToInt(Settings.GetCrosshairSize()),
		Settings.GetCrosshairThickness(),
		FMath::RoundToInt(Settings.GetCrosshairGap()),
		*UTraceUserSettings::DescribeCrosshairColor(Settings.CrosshairColorIndex),
		FMath::RoundToInt(Settings.GetCrosshairOpacity() * 100.f));

	const float ReadoutH = TraceOptionsMenuType::Height(HUD, FontSmall, CaptionScale);
	const float StripH = ReadoutH + (10.f * UIScale);
	const float StripY = BoxY + BoxH - StripH;

	HUD->DrawRect(FLinearColor(0.f, 0.02f, 0.04f, 0.88f), BoxX, StripY, BoxW, StripH);

	TraceOptionsMenuType::Draw(HUD, Readout,
		TraceOptionsStyle::WithAlpha(TraceOptionsStyle::Ink, 0.90f),
		BoxX + (10.f * UIScale), StripY + (5.f * UIScale),
		FontSmall, CaptionScale, TraceOptionsMenuType::BodyFace);
}

bool FTraceOptionsMenu::DrawValueChip(AHUD* HUD, float X, float Y, float W, float H) const
{
	UTexture2D* Chip = TraceOptionsMenuArt::Sprite(TraceOptionsMenuArt::ESprite::ValueBox);
	if (Chip == nullptr)
	{
		return false;
	}

	TraceOptionsMenuArt::DrawPlate(HUD, Chip, TraceOptionsMenuArt::ValueFrame, X, Y, W, H, FLinearColor::White);
	return true;
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

	// The artist's pointer, everywhere this overlay is — which is the title screen's SETTINGS page AND
	// the in-match pause menu. Spec v20 §0.8: until now it existed only on the UMG title screen.
	//
	// TIP-ANCHORED, NOT CENTRED. PollMouse hit-tests at CursorPos, so an arrow whose middle sat on the
	// hit point would draw its point about eleven pixels away from the pixel it is about to click, and
	// every click in every screenshot would look like it landed on the wrong row.
	if (UTexture2D* Arrow = TraceOptionsMenuArt::Sprite(TraceOptionsMenuArt::ESprite::Cursor))
	{
		const float CursorH = 30.f * UIScale;
		const float CursorW = CursorH * TraceOptionsMenuArt::CursorAspect;
		TraceOptionsMenuArt::Draw(HUD, Arrow,
			CursorPos.X - CursorW * TraceOptionsMenuArt::CursorTipU,
			CursorPos.Y - CursorH * TraceOptionsMenuArt::CursorTipV,
			CursorW, CursorH, FLinearColor::White);
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

// ALL FOUR go through UI/Text now — spec v22 §A1. The (UFont*, Scale) signatures stay because they
// are this page's layout vocabulary and forty call sites speak it; TraceOptionsMenuType::SizeFor
// translates. See the block at the top of this file for why the translation is a measurement.

// SPEC v26 §2 added the FACE to three of the four. It is a required argument rather than a defaulted
// one on purpose: a default would let a new call site measure Sofachrome and draw Erbaum — a third
// narrower — and the symptom of that is a rule struck through a header or a key chip that no longer
// fits its key, which is a bug you find in a screenshot rather than in a compile.

ETraceTextWeight FTraceOptionsMenu::FaceForAction() const
{
	// THE ONE JUDGEMENT CALL IN SPEC v26 §2's SPLIT, ISOLATED TO ONE EXPRESSION.
	//
	// An Action row is a button, and this class draws buttons on two very different pages:
	//
	//   * the PAUSE ROOT — RESUME / SETTINGS / VIDEO / RETURN TO TITLE / QUIT. That is not a settings
	//     page; it is the in-match MAIN MENU, the same list of destinations the title screen puts on
	//     screen through UTraceMenuRow. §2 keeps "main menu rows" in Sofachrome, so these stay in it:
	//     setting them in Erbaum would give the game's two top-level menus two different faces, and a
	//     player who opened the pause menu would see a screen that did not match the one they had
	//     launched from thirty seconds earlier;
	//
	//   * SETTINGS and VIDEO — BACK, RESET DEFAULTS, AUTO-DETECT. These are controls on a submenu, in
	//     the same column and the same rhythm as the rows above them, and they get the body face like
	//     everything else on those pages.
	//
	// An owner who reads it the other way changes this one return.
	return (Page == EPage::Root) ? TraceOptionsMenuType::HeaderFace : TraceOptionsMenuType::BodyFace;
}

float FTraceOptionsMenu::MeasureWidth(AHUD* HUD, const FString& Text, UFont* Font, float Scale,
	ETraceTextWeight Weight)
{
	return TraceOptionsMenuType::Width(HUD, Text, Font, Scale, Weight);
}

float FTraceOptionsMenu::MeasureHeight(AHUD* HUD, const FString& Text, UFont* Font, float Scale)
{
	// The LINE BOX, deliberately independent of @p Text — every caller on this page uses it to sit a
	// row's baseline, and a row whose height depended on whether its label happened to contain a
	// descender would jitter as the value changed. Measured to equal what the old bitmap path
	// returned, which is what keeps nineteen rows in exactly the places they were.
	(void)Text;
	return TraceOptionsMenuType::Height(HUD, Font, Scale);
}

void FTraceOptionsMenu::DrawTextCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale,
	ETraceTextWeight Weight)
{
	TraceOptionsMenuType::Draw(HUD, Text, Color, CenterX, Y, Font, Scale, Weight, TraceText::EHAlign::Center);
}

void FTraceOptionsMenu::DrawTextRight(AHUD* HUD, const FString& Text, const FLinearColor& Color, float RightX, float Y, UFont* Font, float Scale,
	ETraceTextWeight Weight)
{
	TraceOptionsMenuType::Draw(HUD, Text, Color, RightX, Y, Font, Scale, Weight, TraceText::EHAlign::Right);
}
