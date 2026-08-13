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
// A Box brush's corner size is Margin * Brush.ImageSize, so ImageSize is not decoration — it is what
// decides how big the corner comes out. FrameImageSize() derives it from the height the plate is
// asked to be, at the sheet's own aspect ratio, which is the only way the corner stays the shape the
// artist drew at every row size.
//
// All four measurements below were taken off the sheet, row by row, not eyeballed. They are mirrored
// in Scripts/slice-ui-assets.py (which cuts to them) and in Scripts/generate-menu-widgets.py (which
// authors the brushes from them). Change one and change all three.

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

	/** Imported and NOT yet placed: they belong to the settings screen, which is still Canvas. */
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

	/** Hover state: the sheet's muted gold, RGB(115,82,50). Dimmer than you would guess; measured. */
	static const FLinearColor WordHover = FLinearColor::FromSRGBColor(FColor(115, 82, 50));

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

		float SpriteW() const { return PlateW + Glow * 2.f; }
		float SpriteH() const { return PlateH + Glow * 2.f; }

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
		 * This is the number that decides the corner's on-screen size (Slate draws each corner at
		 * Margin * ImageSize). Deriving it from the sheet's aspect ratio is what keeps the corner
		 * circular instead of oval at every row width.
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
	 */
	static const FSpriteFrame ButtonFrame = { 4723.f, 1230.f, 128.f, 428.f };

	/** The chip beside a slider. Ring 1034 x 538 inside a 1154 x 656 crop, corner open by 60 rows. */
	static const FSpriteFrame ValueFrame = { 1034.f, 538.f, 60.f, 150.f };
}
