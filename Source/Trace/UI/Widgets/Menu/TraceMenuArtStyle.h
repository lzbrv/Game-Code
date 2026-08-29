// Trace — the artist's menu art, named once (spec v19 §5).
//
// -------------------------------------------------------------------------------------------------
// THIS FILE NO LONGER DECIDES THE TYPEFACE. Source/Trace/UI/Text DOES. (spec v22 §A1)
// -------------------------------------------------------------------------------------------------
// Read this before believing anything below about fonts.
//
// The brief said "use the exact font", the art arrived as ONE PNG, and for two specs the answer was
// a split: the four words baked into that PNG — PLAY, SETTINGS, KEYBIND, KEY — were used as sprites
// at the artist's own letterforms, and every other label (JOIN, DIFFICULTY, SCORING MODE, QUIT, key
// names, character names) was engine-rendered in an interim substitute, Lato Regular. Two faces, one
// column, one row apart. That was the most obvious "this isn't my art" tell on the screen.
//
// The owner then supplied SOFACHROME as a glyph atlas, and the split is over: the whole charset is
// available, so any string can be set in the artist's face.
//
// *** The atlas cannot be a UFont, so it is not reached through this file. *** An offline (bitmap)
// UFont returns nullptr from GetCompositeFont and Slate silently substitutes its last-resort face —
// measured, see TraceText.h. The glyphs are therefore drawn by hand, one quad each, by:
//
//     UI/Text/TraceText.h              layout, measurement, and which face is live
//     UI/Text/TraceCanvasText.h        the Canvas screens
//     UI/Text/TraceAtlasTextWidget.h   UMG — UTraceAtlasText, the drop-in for a UTextBlock
//
// WHAT IS LEFT HERE IS THE FALLBACK, and it is still load-bearing. When the atlas texture is missing
// or stale, or -TraceNoFontAtlas / `Trace.Text.Atlas 0` forces the degraded path, everything in
// UI/Text typesets MenuFont() below instead. A menu in the wrong font beats a menu with no text.
// `Trace.Text.Report` says which of the two is on screen at any moment.
//
// So MenuFontSourceFile is no longer "the one line that changes the screen" — it is the one line that
// changes the FALLBACK. Changing the real face means re-running Scripts/generate_font_atlas.py and
// Scripts/import-font-atlas.sh.
//
// The three copies problem this file used to have: Scripts/generate-menu-widgets.py bakes an
// FSlateFontInfo into every text block of WBP_TitleMenu, so for years the constant below decided
// NOTHING on screen — the generator kept its own hard-coded copy and the C++ one only fed a log
// line. It no longer does: the generator PARSES its font out of this header (see art_style_constant()
// there), so this is the only place in the project a FONT is named. If you add a third copy
// anywhere, you have re-broken it.
//
// -------------------------------------------------------------------------------------------------
// THE SPRITES
// -------------------------------------------------------------------------------------------------
// Cut from "UI Test Export_2.png" (32055 x 18006) by Scripts/slice-ui-assets.py and imported by
// Scripts/generate-menu-widgets.py. Every path here is SOFT — a missing texture must leave the menu
// drawable, not crash it, which is the same fallback rule the whole UMG migration lives under.
//
// -------------------------------------------------------------------------------------------------
// WHY THE FRAMES ARE 9-SLICED, AND WHERE THESE NUMBERS COME FROM
// -------------------------------------------------------------------------------------------------
// The sheet has three button STATES and one button SIZE. The menu needs a 720-wide row and, on the
// settings screen, a ~90-wide KEY chip. Stretching one bitmap across both would turn the artist's
// rounded corner into an oval, so the frame is drawn as a Slate Box brush: the four corners are drawn
// at a fixed size and only the flat middle stretches.
//
// *** CORRECTED IN SPEC v28 §1, AFTER MEASURING IT: A BOX BRUSH'S CORNER SIZE IS NOT Margin *
// ImageSize. *** It is Margin * the TEXTURE'S OWN PIXEL SIZE — SlateCore's ElementBatcher.cpp,
// AddBoxElement, `LeftMarginX = TextureWidth * Margin.Left`. ImageSize decides the brush's DESIRED
// size and takes no part in the slicing at all. Everything below that derives an ImageSize from the
// row height is therefore doing nothing to the corner; it is left in place because it is still the
// brush's desired size and removing it is a change to widget layout, not to this measurement.
//
// The consequence WAS real and WAS on screen, and the fix it named has been taken (release UI plan
// WP7). This plate shipped as a 512 x 153 texture, whose 44 x 44 slices did not fit the 72.5-unit
// plate image at RowHeight 60: Slate's overlap guard squashed both vertical slices to 36.24 and the
// artist's circular corner drew as an ellipse, 28 wide by 23.03 tall — a vertical squash of 0.8226.
// The source is now cut at 256 wide (same art, Scripts/slice-ui-assets.py TARGETS), so the cap
// slice is 22 px, 22 + 22 fits every plate the menu draws including the ~90 x 60 KEY chip, the
// overlap guard never fires, and the corner reaches the screen circular. ResolvePlateSilhouette()
// below is the arithmetic either way: it reads the LIVE texture size, so it reported the squash
// then and reports 1.0 now, with no edit here.
//
// The first four measurements below were taken off the sheet, row by row, not eyeballed. They are
// mirrored in Scripts/slice-ui-assets.py (which cuts to them) and in Scripts/generate-menu-widgets.py
// (which authors the brushes from them). Change one and change all three.
//
// Edge and Corner, added by spec v28 §1, are NOT mirrored anywhere and must not be: they are measured
// off the IMPORTED SPRITE's alpha rather than authored into it, so nothing downstream consumes them.
// They describe the art; the first four decide it.

#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Math/Vector2D.h"

namespace TraceMenuArtStyle
{
	// =============================================================================================
	// THE FONT. ONE constant (spec v20 §1). Everything under it is plumbing that a font swap does
	// not touch.
	// =============================================================================================

	/**
	 * *** THE FALLBACK FACE. Since spec v22 §A1 this is NOT what the menu normally draws in. ***
	 *
	 * The artist's face is Sofachrome and it arrives as a glyph atlas drawn by UI/Text, not as a font
	 * — see the header of this file. This constant decides what is used when that atlas is
	 * unavailable or has been switched off on purpose.
	 *
	 * A file name inside Art/Fonts/. Scripts/generate-menu-widgets.py reads this very line out of
	 * this header, imports that file to MenuFontAsset below, and bakes it into every text block of
	 * WBP_TitleMenu; this file loads the same asset for everything drawn on a Canvas. Drop the real
	 * .ttf/.otf into Art/Fonts/, change this string, re-run the generator, done.
	 *
	 * WHY LATO, AND WHY IT IS STILL WRONG. The user supplied twelve candidate fonts. None is the
	 * sheet's face, and that was measured rather than eyeballed: each baked word (SETTINGS, PLAY,
	 * KEYBIND, KEY) was segmented into letters and every candidate glyph scored by IoU at matched
	 * cap height. Lato-Regular won at 0.5645, Myriad Pro Regular 0.5595, everything else below 0.48.
	 * The artist's face is a hairline-weight, wide, squared-off geometric techno design; Lato is a
	 * humanist text face and is visibly heavier and rounder. It is the closest of the twelve that is
	 * also safe to ship — Myriad Pro, Proxima Nova and Trajan Pro 3 are COMMERCIAL and must never be
	 * committed to this repository. So this is an interim substitute, and it is a substitute for
	 * Roboto Light, which was a substitute already.
	 */
	static const TCHAR* const MenuFontSourceFile = TEXT("Lato-Regular.ttf");

	/**
	 * Where the generator lands it. Fixed on purpose: the ASSET name does not change when the FILE
	 * does, so a font swap never touches C++ that loads it, a .uasset reference, or a redirector.
	 */
	static const TCHAR* const MenuFontAsset = TEXT("/Game/Trace/UI/Fonts/F_TraceMenu.F_TraceMenu");

	/**
	 * The typeface inside that font asset. A UFont imported from a single .ttf carries exactly one,
	 * and UE 5.8 names it "Default"; if a future font file carries several, name the one you want
	 * here. Getting it wrong is not silent — MenuFont() below says so in the log and then draws in
	 * the font's first face, which is what Slate would have done anyway.
	 */
	static const TCHAR* const MenuFontTypeface = TEXT("Default");

	/**
	 * Fallback, used only when MenuFontAsset fails to load — nobody re-ran the generator, the import
	 * failed, the asset was not cooked. A menu in the wrong font beats a menu with no text, and this
	 * is the font the whole menu was drawn in before spec v20, so falling back is a visible
	 * regression rather than an invisible one.
	 */
	static const TCHAR* const MenuFontFallbackAsset = TEXT("/Engine/EngineFonts/Roboto");
	static const TCHAR* const MenuFontFallbackTypeface = TEXT("Light");

	/**
	 * The font, at @p InSize points. Resolved once and cached.
	 *
	 * Falls back to MenuFontFallbackAsset — loudly, once — rather than returning an empty
	 * FSlateFontInfo. A menu with no words in it is a much worse failure than a menu in the wrong
	 * words, and an empty FSlateFontInfo is not even that: Slate silently substitutes its last-resort
	 * face, so nothing looks broken and the design is simply gone.
	 */
	TRACE_API FSlateFontInfo MenuFont(float InSize);

	/**
	 * True only when the INTENDED font resolved: MenuFontAsset loaded AND it contains
	 * MenuFontTypeface. False while the menu is drawing in the Roboto fallback, which is the state
	 * this is here to catch — a fallback looks like a working menu.
	 */
	TRACE_API bool IsMenuFontResolved();

	/** One line naming what is actually being drawn with, for the verifier and the log. */
	TRACE_API FString DescribeMenuFont();

	// =============================================================================================
	// Sprite paths. /Game/Trace/UI/Art, written by Scripts/generate-menu-widgets.py.
	// =============================================================================================

	static const TCHAR* const ArtDir = TEXT("/Game/Trace/UI/Art");

	/** Empty button plate, one per state. 9-sliced; see FrameImageSize(). */
	static const TCHAR* const BtnDefault  = TEXT("/Game/Trace/UI/Art/T_MenuBtn_Default.T_MenuBtn_Default");
	static const TCHAR* const BtnHover    = TEXT("/Game/Trace/UI/Art/T_MenuBtn_Hover.T_MenuBtn_Hover");
	static const TCHAR* const BtnDisabled = TEXT("/Game/Trace/UI/Art/T_MenuBtn_Disabled.T_MenuBtn_Disabled");

	/**
	 * The four words the sheet actually contains, lifted off their plates as white-on-transparent.
	 *
	 * *** RETIRED AS A SOURCE OF TEXT (spec v22 §A1). *** These existed only because the atlas did
	 * not, and a picture of the word PLAY cannot typeset SCORING MODE. Now that the whole charset is
	 * available, setting PLAY and SETTINGS as sprites while their neighbours are live text is what
	 * PUT two typefaces in one column. Draw them as live text like every other label:
	 *
	 *     Label->SetSize(TraceText::SizeForCapHeight(<the cap height the sprite occupied>));
	 *     Label->VerticalAlignment = ... EVAlign::CapTop;   // a word sprite's top edge IS its cap line
	 *
	 * They are deliberately KEPT in the repository and kept named here: they are the artist's own
	 * raster of those four words and the reference the atlas is checked against. Do not delete them;
	 * just stop typesetting with them.
	 */
	static const TCHAR* const WordPlay     = TEXT("/Game/Trace/UI/Art/T_MenuWord_Play.T_MenuWord_Play");
	static const TCHAR* const WordSettings = TEXT("/Game/Trace/UI/Art/T_MenuWord_Settings.T_MenuWord_Settings");
	static const TCHAR* const WordKeybind  = TEXT("/Game/Trace/UI/Art/T_MenuWord_Keybind.T_MenuWord_Keybind");
	static const TCHAR* const WordKey      = TEXT("/Game/Trace/UI/Art/T_MenuWord_Key.T_MenuWord_Key");

	static const TCHAR* const Wordmark     = TEXT("/Game/Trace/UI/Art/T_TraceWordmark.T_TraceWordmark");
	static const TCHAR* const Swoosh       = TEXT("/Game/Trace/UI/Art/T_MenuSwoosh.T_MenuSwoosh");
	static const TCHAR* const Cursor       = TEXT("/Game/Trace/UI/Art/T_MenuCursor.T_MenuCursor");
	static const TCHAR* const Chevron      = TEXT("/Game/Trace/UI/Art/T_MenuBack.T_MenuBack");
	static const TCHAR* const ValueBox     = TEXT("/Game/Trace/UI/Art/T_MenuValueBox.T_MenuValueBox");

	/**
	 * Placed: the Canvas options pages draw the TRACK (TraceOptionsMenu.cpp).
	 *
	 * THE HANDLE IS NO LONGER DRAWN BY ANYTHING, and the path is kept as the artist's record rather
	 * than deleted. T_MenuSliderHandle is the same 64x87 diagonal blade as T_MenuCursor with the rail
	 * subtracted from under it, so on a page carrying both there were two identical blades and one of
	 * them was the mouse pointer (UI QA finding 6b). The thumb is drawn as a vertical fader cap now;
	 * see DrawSliderRow. A re-cut that produced a vertical thumb sprite would make this live again.
	 */
	static const TCHAR* const SliderTrack  = TEXT("/Game/Trace/UI/Art/T_MenuSliderTrack.T_MenuSliderTrack");
	static const TCHAR* const SliderHandle = TEXT("/Game/Trace/UI/Art/T_MenuSliderHandle.T_MenuSliderHandle");

	// =============================================================================================
	// Colours, sampled off the sheet rather than invented.
	//
	// These are the artist's, to the byte. The three button states differ in the plate (which is
	// baked into the sprite) and in the colour of the WORD, which is a tint — which is exactly why
	// one white word sprite can serve all three states.
	// =============================================================================================

	/** Plate fill, RGB(29,41,81). Here for anything that has to sit ON a button and match it. */
	static const FLinearColor PlateFill = FLinearColor::FromSRGBColor(FColor(29, 41, 81));

	/** Default state: white lettering. RGB(255,255,255). */
	static const FLinearColor WordDefault = FLinearColor::White;

	/**
	 * Hover state: the sheet's GREEN, sRGB(85,107,47).
	 *
	 * ---------------------------------------------------------------------------------------------
	 * THIS CONSTANT WAS WRONG, AND HOW IT GOT WRONG IS WORTH THE PARAGRAPH (spec v24 §3)
	 * ---------------------------------------------------------------------------------------------
	 *     "The button's hover state doesn't fully work — the orange outline shows up while hovering,
	 *      but not the green text from my assets"
	 *
	 * It used to read RGB(115,82,50) and call itself "the sheet's muted gold". That is not a colour
	 * the artist ever put on a word. It is the AMBER HALO that bleeds off the hover ring and pools
	 * around the lettering, and it got in here because whoever sampled it took the brightest pixel
	 * inside the plate — on the one plate whose baked word is a dim state ANNOTATION rather than a
	 * button label. Everything downstream then inherited a bad measurement, including spec v20 §0.5,
	 * which correctly found the resulting colour illegible and replaced it with white. The word came
	 * out white, the ring stayed amber, and the artist's green was never on the screen at all.
	 *
	 * RE-MEASURED, on the three plates of "UI Test Export_2.png" that carry a real BUTTON label in
	 * the hover state. Their words are a flat, unshaded fill and all three agree to the byte:
	 *
	 *     SETTINGS hover  (sheet x 2000..7200,  y 1300..2600)   sRGB(85,107,47)  331,550 px
	 *     KEYBIND  hover  (sheet x 18800..23200, y 8300..9600)  sRGB(85,107,47)  325,901 px
	 *     KEY      hover  (sheet x 23900..25600, y 8300..9600)  sRGB(85,107,47)  102,267 px
	 *
	 * The odd one out is the generic state-swatch plate the slicer cuts T_MenuBtn_Hover from, whose
	 * word is the literal string "HOVER" at sRGB(68,76,56) — a legend, drawn dimmer than the buttons
	 * it labels. Sampling that plate is how this went wrong the first time, so it is named here.
	 *
	 * ---------------------------------------------------------------------------------------------
	 * WHAT IT MEASURES AGAINST THE PLATE, STATED RATHER THAN DISCOVERED LATER
	 * ---------------------------------------------------------------------------------------------
	 * On PlateFill, sRGB(29,41,81), this green is a WCAG contrast ratio of 2.34:1. That is low, and
	 * it is the artist's own composition — the same 2.34:1 their own sheet shows. It is deliberately
	 * NOT "corrected" here: this file is the artist's palette sampled to the byte, and the moment a
	 * value in it stops being what the sheet says, the file stops being worth reading. The selection
	 * is carried by three signals besides the word anyway — the amber ring on T_MenuBtn_Hover, the
	 * amber rail on the row's leading edge, and the plate's pulse — none of which changed.
	 *
	 * Shipped as WordHoverLifted() per the release art bible §2.5 — same hue, artist's own
	 * transformation precedent (AmberLifted). This constant stays as the artist record it derives
	 * from; nothing draws a word in it directly any more.
	 */
	static const FLinearColor WordHover = FLinearColor::FromSRGBColor(FColor(85, 107, 47));

	/**
	 * Disabled state: the sheet draws the word WHITE on a near-black plate with a grey ring, so the
	 * plate carries the whole "you cannot press this" signal. Reproduced as drawn, then dropped to 55%
	 * so a dead row cannot out-shout a live one — the one deliberate departure from the sheet on this
	 * screen, and it is a tint, not a redraw.
	 */
	static const FLinearColor WordDisabled = FLinearColor(0.55f, 0.55f, 0.55f, 1.f);

	/** Pressed: the hover plate, knocked down. The sheet has no pressed state; see UTraceMenuRow. */
	static constexpr float PressedTint = 0.72f;

	/** The amber the sheet outlines a hover button and the TRACE mark with. RGB(116,58,0). */
	static const FLinearColor Amber = FLinearColor::FromSRGBColor(FColor(116, 58, 0));

	/**
	 * Amber's HUE at full brightness: sRGB(116,58,0) -> sRGB(255,128,0). The ratio is exactly the
	 * artist's; only the level is not.
	 *
	 * WHY THIS IS NEEDED AT ALL. The sheet's amber is a GLOW. It gets its apparent brightness from the
	 * bloom around it rather than from the value itself, so anything that draws a FLAT shape in it —
	 * a 9 px selection rail, the wordmark's rebuilt outer glow — ships as a dim brown smear at the
	 * sampled value. Both of those places therefore want the same hue at a level that reads on black.
	 *
	 * WHY IT IS A FUNCTION IN THE PALETTE FILE, when the file's own rule is that it contains only what
	 * the sheet says. Because this is not a second, invented COLOUR — it is a stated TRANSFORMATION of
	 * the artist's one, and it moves when Amber does. That is exactly spec v24 §0's distinction. The
	 * two callers used to carry the answer as pasted literals instead: the row's selection rail as
	 * FColor(255,127,0) with a comment deriving it, and the title screen's wordmark glow as
	 * FColor(255,140,40) with a comment CLAIMING to derive it and in fact not — 116:58:0 does not
	 * normalise to 255:140:40, so the mark's halo had drifted off the artist's hue without anybody
	 * being able to see it in a diff. Deriving it here makes the claim true and keeps it true.
	 *
	 * Normalised in sRGB BYTES, not in linear: the ratio the eye and the sheet agree on is the byte
	 * one. Doing it in linear would produce a visibly different, more saturated colour.
	 */
	TRACE_API FLinearColor AmberLifted();

	/**
	 * WordHover's HUE at full brightness: sRGB(85,107,47) -> sRGB(203,255,112), #CBFF70.
	 *
	 * The same stated transformation as AmberLifted, applied to the artist's other glow colour, for
	 * the same reason and under the same rule — a TRANSFORMATION of the sheet's colour, not an
	 * invented one, and it moves when WordHover does. The artist's hover green is a flat fill at
	 * 2.34:1 against PlateFill (measured, see WordHover above): legible on their backlit sheet,
	 * not on a screen. Byte-normalised it keeps the exact hue and clears the plate at 12.2:1.
	 *
	 * This is what the hovered/selected WORD is drawn in, on both renderers (release art bible
	 * §2.5). WordHover itself stays untouched above — it is the artist record this derives from.
	 */
	TRACE_API FLinearColor WordHoverLifted();

	// =============================================================================================
	// 9-slice geometry, in SHEET pixels. Mirrored in Scripts/slice-ui-assets.py.
	// =============================================================================================

	/** One button state as it was cut: a plate, plus a margin of glow on every side. */
	struct FSpriteFrame
	{
		float PlateW = 0.f;
		float PlateH = 0.f;
		/** Sheet pixels of glow kept outside the plate on every side. */
		float Glow = 0.f;
		/** Sheet pixels from the sprite's edge to where the corner curve is fully open. */
		float Cap = 0.f;

		/**
		 * Sheet pixels from the sprite's edge to the plate's own 50%-ALPHA edge — i.e. to the line a
		 * viewer would point at and call the edge of the button.
		 *
		 * NOT the same number as Glow, and the difference is why it is measured rather than assumed.
		 * Glow is the CROP: how much halo Scripts/slice-ui-assets.py kept outside the nominal plate
		 * rectangle, and it is the number the widget generator offsets the plate image by. The artist's
		 * plate edge is soft over about ten sheet pixels, so the half-coverage contour — where the
		 * silhouette visually is — sits a little inside the nominal edge.
		 *
		 * MEASURED off Content/Trace/UI/Art/Source/T_MenuBtn_Default.png (at its original 512 x 153
		 * cut; the sprite has since been re-cut to 256-wide — WP7 — which changes none of these
		 * numbers, because they are stated in SHEET pixels and a resize is a uniform scale), by
		 * taking the alpha's 0.5 crossing along the flat middle of each side and scaling by
		 * the sheet/sprite ratio: left 122.7, right 122.0, top 122.6, bottom 122.9 sheet px. The same
		 * pass recovers the plate as 4724.6 x 1230.8 sheet px, which is the 4723 x 1230 above to within
		 * a fifth of a sprite pixel — so the two measurements agree and only this one is new.
		 *
		 * Zero means "not measured for this frame"; ResolvePlateSilhouette() falls back to Glow.
		 */
		float Edge = 0.f;

		/**
		 * The plate's CORNER RADIUS, in sheet pixels. Not Cap: Cap is the 9-slice margin, deliberately
		 * cut wider than the corner so the arc is safely inside the slice.
		 *
		 * MEASURED, and it is a genuine circular arc rather than a squircle — which is worth knowing,
		 * because a Slate rounded box can only draw circular corners and could not have followed a
		 * superellipse at any radius. Fitting the 0.5-alpha contour of all four corners of
		 * T_MenuBtn_Default.png (its original 512-wide cut; sheet-pixel result unchanged by the WP7
		 * re-cut) gives 28.04 / 27.96 / 28.04 / 27.98 sprite px at an RMS residual of
		 * 0.05 px, and a free superellipse exponent lands on 2.00. 28.0 sprite px is 272.4 sheet px,
		 * which is 0.2211 of the plate's height.
		 *
		 * Zero means "not measured"; ResolvePlateSilhouette() falls back to Cap - Glow, which is what
		 * this file used to assume the corner was (300 sheet px — 10% too round).
		 */
		float Corner = 0.f;

		float SpriteW() const { return PlateW + Glow * 2.f; }
		float SpriteH() const { return PlateH + Glow * 2.f; }

		/** Edge, or Glow when nobody has measured this frame's silhouette. */
		float EdgeOrGlow() const { return (Edge > 0.f) ? Edge : Glow; }

		/** Corner, or the pre-measurement assumption, when nobody has measured this frame. */
		float CornerOrCap() const { return (Corner > 0.f) ? Corner : FMath::Max(0.f, Cap - Glow); }

		/** Slate Box-brush margin, normalised, which is what Slate wants. */
		FMargin BrushMargin() const
		{
			return FMargin(Cap / SpriteW(), Cap / SpriteH(), Cap / SpriteW(), Cap / SpriteH());
		}

		/**
		 * How far OUTSIDE the plate rectangle the sprite has to be drawn so the plate lands exactly on
		 * it. The glow lives in that overhang; forget it and the plate comes out 20% small.
		 */
		float GlowInset(float InPlateHeightOnScreen) const
		{
			return InPlateHeightOnScreen * (Glow / PlateH);
		}

		/**
		 * Brush ImageSize for a plate drawn @p InPlateHeightOnScreen tall.
		 *
		 * *** THIS DOES NOT DECIDE THE CORNER'S SIZE. *** It used to say it did; spec v28 §1 measured
		 * it and it does not. Slate slices a Box brush against the TEXTURE's pixel size, not against
		 * ImageSize — see the block at the top of this file. What ImageSize still does is give the
		 * brush a desired size, which is what an auto-sized slot would lay out to, so it is derived at
		 * the sheet's own aspect ratio and kept.
		 */
		FVector2D ImageSize(float InPlateHeightOnScreen) const
		{
			const float Height = InPlateHeightOnScreen + GlowInset(InPlateHeightOnScreen) * 2.f;
			return FVector2D(Height * (SpriteW() / SpriteH()), Height);
		}
	};

	/**
	 * The wide button. Plate 4723 x 1230; the corner is fully open 300 rows down; the amber hover ring
	 * sits 34 px out and its halo is gone by ~108, so 128 px of glow keeps all of it.
	 *
	 * The last two are spec v28 §1's measurement of the DEFAULT plate's actual silhouette — see Edge
	 * and Corner above. They are the numbers a stroke has to follow to hug this button.
	 */
	static const FSpriteFrame ButtonFrame = { 4723.f, 1230.f, 128.f, 428.f, 122.6f, 272.4f };

	/**
	 * The chip beside a slider. Ring 1034 x 538 inside a 1154 x 656 crop, corner open by 60 rows.
	 *
	 * Edge and Corner are deliberately UNMEASURED: nothing draws a stroke against this frame, and a
	 * number nobody has checked is worse than an honest fallback. Measure them the way ButtonFrame's
	 * were if a chip ever needs an outline.
	 */
	static const FSpriteFrame ValueFrame = { 1034.f, 538.f, 60.f, 150.f };

	// =============================================================================================
	// SPEC v28 §1 — WHERE A 9-SLICED PLATE'S SILHOUETTE ACTUALLY LANDS
	// =============================================================================================
	//
	//     "Change the white outline to 1px and make sure it hugs the buttons, right now it looks
	//      terrible."
	//
	// IT DID NOT HUG BECAUSE THE ROW RECTANGLE IS NOT THE PLATE. Spec v26 §7 anchored the stroke to
	// the row's own rect, on the reasoning that WBP_MenuRow pushes the plate image outward by exactly
	// the glow overhang so the plate lands back on that rect. That reasoning has one wrong step in it,
	// and the gap it leaves is measurable: on the shipped 1920x1080 capture the stroke runs down
	// x = 600..601 and the navy plate does not begin until x = 607 — five pixels of pure background
	// between the line and the button it is supposed to be drawn around, with the same thing 4 px deep
	// at the top and bottom.
	//
	// THE WRONG STEP IS THAT SLATE DOES NOT SIZE A BOX BRUSH'S CORNERS FROM ImageSize. It sizes them
	// from the TEXTURE'S OWN PIXEL SIZE (SlateCore's ElementBatcher.cpp, AddBoxElement:
	// `LeftMarginX = TextureWidth * Margin.Left`). ImageSize decides the widget's DESIRED size and
	// nothing else here. At the plate's original 512-wide cut the 9-slice corners were therefore
	// drawn 44 x 44 LOCAL UNITS — 512 x 0.0860 and 153 x 0.2880 — no matter how tall the row was or
	// what DPI scale it was drawn at.
	//
	// AND AT A 60-UNIT ROW THAT DID NOT FIT. The plate image is RowHeight plus two glow insets tall,
	// i.e. 72.5 local units, and the top and bottom slices wanted 44 each. Slate's own overlap guard
	// then fired — `if (BottomMarginY < TopMarginY) { TopMarginY = LocalSize.Y / 2; ... }` — and
	// squashed both slices to 36.24. Two things followed, and both were visible:
	//
	//   * the plate's edges moved INWARD from the row rect: 6.92 local units on the left and right,
	//     4.60 on the top and bottom. That is the gap above;
	//   * the artist's circular 28-px corner drew as an ELLIPSE, 28 wide by 23.03 tall, because
	//     the vertical slice was compressed by 36.24/44.06 = 0.8226 and the horizontal one was not.
	//
	// Neither number moved with the resolution — they are local units, and the DPI scale multiplies
	// the row and the plate together — so it was one shape to follow, not one per screen.
	//
	// THE RE-CUT THIS BLOCK ASKED FOR HAS BEEN TAKEN (release UI plan WP7): the three T_MenuBtn_*
	// sources are now cut 256 wide, the cap slice is 22 px, 22 + 22 fits the 72.5 available (and the
	// KEY chip's 60), the overlap guard never fires and the corner ships circular. The arithmetic
	// below is deliberately kept live rather than deleted — it reads the LIVE brush and texture, so
	// it is what PROVES the squash is gone (Squash() == 1.0) and what would catch it coming back.
	// Known trade, stated: at 4K (DPI 2) the 76 px-tall source upscales ~2x into a softer,
	// glow-edged plate; the corner SHAPE wins over edge crispness. If a 4K capture ever reads badly,
	// the recorded fallback is TWO sources (256 for rows, 512 kept for plates drawn >= 88 px tall) —
	// not taken preemptively.
	//
	// THIS FUNCTION IS THAT ARITHMETIC, restated from Slate's own source so a stroke can be laid on
	// the answer instead of on a guess. It is deliberately not a table of the numbers above: feed it
	// the live brush and the live slot and it follows a re-slice, a different row height, or a fixed
	// generator with no edit here. If the squash ever stops happening, Squash() returns 1 and the
	// caller's ellipse quietly becomes the circle it should always have been.

	/** Where a plate's own silhouette lands, in the local units of the rectangle it was measured in. */
	struct FPlateSilhouette
	{
		/** False when the inputs could not describe a plate; the caller must draw nothing. */
		bool bValid = false;

		FVector2D Min = FVector2D::ZeroVector;
		FVector2D Max = FVector2D::ZeroVector;

		/** The drawn corner, which is an ellipse whenever Slate has squashed one of the slices. */
		double RadiusX = 0.0;
		double RadiusY = 0.0;

		/**
		 * False when the corner arc runs past the end of its 9-slice and into the stretched middle. The
		 * corner is then not an ellipse either and no rounded box can follow it — worth saying out loud
		 * rather than drawing a shape that is wrong in a way nobody can name.
		 */
		bool bCornerInsideSlice = true;

		FVector2D Size() const { return Max - Min; }
		FVector2D Center() const { return (Min + Max) * 0.5; }

		/** RadiusY / RadiusX. 1.0 when the artist's circular corner is being drawn as a circle. */
		double Squash() const
		{
			return (RadiusX > UE_DOUBLE_KINDA_SMALL_NUMBER) ? (RadiusY / RadiusX) : 1.0;
		}
	};

	/**
	 * Resolve @p InFrame's silhouette inside the rectangle its sprite is drawn in.
	 *
	 * @param InFrame        the sheet geometry of the plate being drawn.
	 * @param InImageMin     top-left of the IMAGE WIDGET's rect (not the plate's), in any one space.
	 * @param InImageMax     bottom-right of the same rect, in the same space.
	 * @param InTextureSize  the imported texture's pixel size. This is what Slate slices with.
	 * @param InBrushMargin  the brush's normalised Box margin, straight off the brush.
	 * @return the plate's rect and drawn corner radii, in the space @p InImageMin was given in.
	 */
	TRACE_API FPlateSilhouette ResolvePlateSilhouette(
		const FSpriteFrame& InFrame,
		const FVector2D& InImageMin,
		const FVector2D& InImageMax,
		const FVector2D& InTextureSize,
		const FMargin& InBrushMargin);

	// =============================================================================================
	// THE POINTER'S OWN GEOMETRY — named ONCE (spec v24 §0, applied to this area)
	// =============================================================================================
	//
	// T_MenuCursor is drawn by three screens, and until this pass each carried its own copy of the
	// sprite's proportions as bare numbers: FTraceOptionsMenu had `64.f / 87.f` with a tip of
	// (0.180, 0.075), FTraceCharacterSelect had `64.f / 87.f` again with a DIFFERENT tip,
	// (12/64, 6/87) = (0.1875, 0.0690). Two screens, two answers, for one picture — and a re-slice
	// that changed the sprite would have moved neither.
	//
	// THE SECOND HALF OF THAT JOB WAS FINISHED BY THE UI QA PASS. Sharing the numbers left three
	// separate DRAWS, and one of them (the Canvas title screen) was not drawing this sprite at all —
	// it drew a nine-pixel cyan cross, so the same screen looked different on its two renderers.
	// UI/TraceHardwareCursor.h now owns the pointer end to end: it reads the four constants below,
	// loads the sprite once, decides the tint, and every Canvas surface calls its DrawPointer. The
	// constants stay HERE because this file is where the sprite's geometry is measured; what moved is
	// the drawing, not the measurement.
	//
	// §0's rule is that a value derived from a base must be expressed RELATIVE to that base so it
	// follows when the base moves. Here the base is the sprite's own pixel size, so that is what is
	// stated and everything else is arithmetic on it. A future re-cut edits the two sizes below and
	// every screen follows with no other edit.
	//
	// Measured off Content/Trace/UI/Art/Source/T_MenuCursor.png's ALPHA, not eyeballed: ink begins on
	// row 6 spanning columns 9..15, and the first fully-opaque pixel is (11, 10). The arrow's point
	// is therefore at about (11.5, 6.5) in the sprite's own 64 x 87.

	static constexpr float CursorSpriteW = 64.f;
	static constexpr float CursorSpriteH = 87.f;
	static constexpr float CursorTipX = 11.5f;
	static constexpr float CursorTipY = 6.5f;

	/** Width per unit of height. A pointer drawn H tall is this wide, and is never stretched. */
	static constexpr float CursorAspect = CursorSpriteW / CursorSpriteH;

	/**
	 * Where the arrow's POINT sits inside the sprite, as a fraction of it.
	 *
	 * Load-bearing rather than decorative: every screen hit-tests at the cursor position, so an arrow
	 * anchored anywhere but its point draws about eleven pixels from the pixel it is about to click,
	 * and every click in every screenshot looks like it landed on the wrong row.
	 */
	static constexpr float CursorTipU = CursorTipX / CursorSpriteW;
	static constexpr float CursorTipV = CursorTipY / CursorSpriteH;
}
