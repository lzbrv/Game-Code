// Trace — the artist's menu art, named once (spec v19 §5).
//
// -------------------------------------------------------------------------------------------------
// THE FONT IS NOT HERE, AND THAT IS THE POINT OF THIS FILE
// -------------------------------------------------------------------------------------------------
// The brief says "use the exact font". The art arrived as ONE PNG. A PNG can supply the four words
// that were drawn into it — PLAY, SETTINGS, KEYBIND, KEY — and it cannot supply a letter that is not
// on it, which is every other label this menu needs: JOIN, DIFFICULTY, SCORING MODE, QUIT, every key
// name on the settings screen, every character name. There is no way to typeset "SCORING MODE" out
// of a picture of the word "PLAY".
//
// So the four baked words ARE used, as sprites, at the artist's own letterforms — and everything else
// is engine-rendered in the closest thing this project has, which is Roboto Light. That is a stand-in
// and it is NOT the same typeface. It is declared in exactly one place, MenuFontAsset /
// MenuFontTypeface below, and swapping in the real .ttf or .otf is those two lines plus one import.
// Nothing else in the UI names a font.
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
	// THE FONT. Two lines. This is the whole substitution.
	// =============================================================================================

	/**
	 * Stand-in for the artist's typeface until we are sent the file.
	 *
	 * Roboto Light is the closest thing bundled with the engine: the sheet's face is a thin, wide,
	 * geometric display sans with flat-cut terminals, and of Roboto's shipped weights (Light, Regular,
	 * Medium, Bold, Black, BoldCondensed) the Light one is the only one that is not visibly heavier
	 * than the art. It is still the wrong typeface — its A is rounded and the sheet's is a hard
	 * triangle — and the difference is visible on any row next to the PLAY sprite.
	 */
	static const TCHAR* const MenuFontAsset = TEXT("/Engine/EngineFonts/Roboto");
	static const TCHAR* const MenuFontTypeface = TEXT("Light");

	/**
	 * The font, at @p InSize points. Resolved once and cached.
	 *
	 * Falls back to the engine's default font — loudly, once — rather than returning an empty
	 * FSlateFontInfo, which Slate draws as nothing at all. A menu with no words in it is a much worse
	 * failure than a menu in the wrong words.
	 */
	TRACE_API FSlateFontInfo MenuFont(float InSize);

	/** True when MenuFontAsset/MenuFontTypeface both resolved. Reported by `Trace.UI.VerifyMenuArt`. */
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

	/** The four words the sheet actually contains, lifted off their plates as white-on-transparent. */
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
