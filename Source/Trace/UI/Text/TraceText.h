// Trace — SOFACHROME, drawn by hand (spec v22 §A1).
//
// =================================================================================================
// WHAT THIS IS, AND WHY IT IS NOT A UFont
// =================================================================================================
// The owner supplied Sofachrome and chose a bitmap-atlas route (docs/FONTS.md: the free desktop
// licence forbids embedding, and this repo is public). The obvious implementation — import the sheet
// as an "offline" UFont and hand it to Slate — DOES NOT WORK, and that was MEASURED on this project
// during spec v20 rather than assumed:
//
//     UFont::GetCompositeFont() returns nullptr unless FontCacheType == Runtime
//     (Engine/Private/Font.cpp). An FSlateFontInfo built on an offline font therefore carries no
//     composite font, and Slate SILENTLY SUBSTITUTES ITS LAST-RESORT FACE.
//
// Nothing logs, nothing crashes, and the title screen quietly draws in the wrong typeface while the
// Canvas screens look perfect. That is the worst of the two available bugs, so this module does not
// go near FSlateFontInfo for Sofachrome at all. It draws the glyphs itself, one textured quad each,
// from an ordinary UTexture2D.
//
// =================================================================================================
// ONE METRICS SOURCE, ONE LAYOUT PASS
// =================================================================================================
// Spec A1 requires the Canvas blitter and the UMG widget to read the SAME metrics. They do something
// stronger: they share the same LAYOUT FUNCTION. TraceText::LayoutString() is the only place in the
// project that includes TraceFontAtlasMetrics.h, and both renderers are thin loops over the quads it
// returns. Two paths cannot disagree about where a letter goes when neither of them decides.
//
//     TraceCanvasText::Draw()      -> LayoutString() -> one AHUD::DrawTexture per quad
//     STraceAtlasText::OnPaint()   -> LayoutString() -> one FSlateDrawElement::MakeBox per quad
//
// =================================================================================================
// TWO WEIGHTS, THROUGH THAT SAME ONE PASS (spec v23 §A3)
// =================================================================================================
// The owner wants everything light and the character names on the select screen bold. That is a
// property of the STYLE, not a second renderer: FStyle::Weight picks which sheet the quads sample,
// and LayoutString() is otherwise unchanged. Both weights are rasterised from the same em with the
// same advances and the same charset — Scripts/import_font_atlas.py REFUSES to emit the metrics
// header if they ever stop agreeing.
//
// ADVANCES ARE NOT SHARED, though, and that correction is spec v23's integration pass: the two
// weights are two different font files (Sofachrome W05 ExtraLight and Sofachrome Rg) and 94 of the
// 95 cells differ in width, bold being roughly half again as wide. Measurement is face-relative, so
// MeasureWidth() is right — but only if the caller hands it the weight it is going to DRAW in.
//
//     TraceText::FStyle Style(25.f, Color);
//     Style.Weight = ETraceTextWeight::Bold;      // the ONE line a caller adds
//     TraceCanvasText::Draw(HUD, Name, X, Y, Style);
//
// Cap height is the one metric that IS per weight (erosion shortens the light caps by ~10%), so
// CapHeight() and SizeForCapHeight() take a weight. Everything else takes it only for symmetry.
//
// =================================================================================================
// COORDINATES
// =================================================================================================
// A draw takes the TOP-LEFT of the line box, exactly like AHUD::DrawText, so a caller swapping a
// DrawText call for a TraceCanvasText::Draw call does not move its own layout. Inside, the atlas's
// cells are one full advance by one full line height with the glyph's bearings already baked in, so
// laying out a string is: blit cell, advance pen by the cell's width. There is no kerning table
// because there is no kerning — see the generated header's own comment.
//
// Ascent(), CapHeight() and SizeForCapHeight() exist for the one thing that is genuinely fiddly:
// sitting live type exactly where one of the artist's baked word sprites sat. Those sprites are
// placed by CAP height, and cap height is not derivable from Size without this table.
//
// =================================================================================================
// FALLBACK — MANDATORY, AND VISIBLE ON PURPOSE
// =================================================================================================
// If the texture is missing, or fails the staleness guard, or -TraceNoFontAtlas was passed, or
// `Trace.Text.Atlas 0` was typed, every function here switches to TraceMenuArtStyle::MenuFont (Lato)
// AND KEEPS WORKING — measurement included, so a caller's layout stays correct in the degraded path.
// A menu in the wrong font beats a menu with no text. `Trace.Text.Report` says which is live.

#pragma once

#include "CoreMinimal.h"
#include "Math/Vector2D.h"

#include "UI/Text/TraceTextWeight.h"

class UTexture2D;

namespace TraceText
{
	/** Horizontal origin of a draw. Left means "X is the left edge", and so on. */
	enum class EHAlign : uint8
	{
		Left,
		Center,
		Right
	};

	/**
	 * Vertical origin of a draw.
	 *
	 * Top is the AHUD::DrawText convention and the default. Baseline is for lining live text up with
	 * something that was placed by its baseline; CapTop is for lining it up with a baked word sprite,
	 * whose top edge is the cap line and not the line box.
	 */
	enum class EVAlign : uint8
	{
		Top,
		Center,
		Bottom,
		Baseline,
		CapTop
	};

	/**
	 * Everything a draw needs beyond the string and the position.
	 *
	 * Size means what FSlateFontInfo::Size means — the em, in screen pixels — so the numbers already
	 * in this project's menus carry over unchanged. Lato at 24 and Sofachrome at 24 differ by 6% of
	 * cap height and less than a pixel of line height, which is why the fallback does not reflow.
	 */
	struct FStyle
	{
		float Size = 24.f;
		FLinearColor Color = FLinearColor::White;

		/** Extra space between glyphs, in pixels at Size. Sofachrome is wide; negative is legal. */
		float Tracking = 0.f;

		/**
		 * Which cut of Sofachrome. LIGHT BY DEFAULT — the owner's instruction is that the game is
		 * light and bold is the exception, so a caller opts IN to bold and never has to opt out.
		 * Changing this does not change any advance, so it cannot move a layout; see the header.
		 */
		ETraceTextWeight Weight = ETraceTextWeight::Light;

		EHAlign HAlign = EHAlign::Left;
		EVAlign VAlign = EVAlign::Top;

		FStyle() = default;
		explicit FStyle(float InSize) : Size(InSize) {}
		FStyle(float InSize, const FLinearColor& InColor) : Size(InSize), Color(InColor) {}
		FStyle(float InSize, const FLinearColor& InColor, ETraceTextWeight InWeight)
			: Size(InSize), Color(InColor), Weight(InWeight) {}
	};

	/** One glyph, positioned relative to the draw's origin. Pixels for Pos/Size, 0..1 for the UVs. */
	struct FGlyphQuad
	{
		FVector2f Pos = FVector2f::ZeroVector;
		FVector2f Size = FVector2f::ZeroVector;
		FVector2f UVMin = FVector2f::ZeroVector;
		FVector2f UVSize = FVector2f::ZeroVector;
	};

	// =============================================================================================
	// WHICH FACE IS LIVE
	// =============================================================================================

	/**
	 * True when Sofachrome is actually what will be drawn.
	 *
	 * False means every call below silently and correctly uses Lato instead. Callers do NOT need to
	 * branch on this — it is here so a verifier, a screenshot harness or `Trace.Text.Report` can
	 * state which path produced the pixels rather than guess.
	 */
	TRACE_API bool IsAtlasActive();

	/** "Sofachrome" or "Lato". Short enough to put on screen next to the specimen it labels. */
	TRACE_API FString FaceName();

	/** One paragraph: the face, the reason if it is the fallback, and how to force the other path. */
	TRACE_API FString DescribeFace();

	// =============================================================================================
	// WEIGHTS — asking for bold, including by name
	// =============================================================================================

	/**
	 * True when @p Weight's own sheet is what will be drawn for it.
	 *
	 * False means that weight is being SUBSTITUTED — either the whole atlas path is down (Lato), or
	 * that one sheet failed to resolve and it is drawing in the default weight instead. Callers do
	 * not need to branch on it; it is here so a verifier can state which sheet made the pixels.
	 */
	TRACE_API bool IsWeightActive(ETraceTextWeight Weight);

	/** "Light" / "Bold" — the weight as drawn, so a caption can name what is on screen. */
	TRACE_API const TCHAR* WeightName(ETraceTextWeight Weight);

	/**
	 * Resolves a weight from a string, for callers that carry one by NAME — a config value, a
	 * console argument, a data asset column. Case-insensitive; matches the names above.
	 *
	 * Unrecognised input returns @p Fallback rather than guessing, so a typo in a knob degrades to
	 * the default weight instead of silently picking the wrong one. Pass bOutMatched to tell a typo
	 * apart from a deliberate "Light".
	 */
	TRACE_API ETraceTextWeight WeightFromName(const FString& Name,
		ETraceTextWeight Fallback = ETraceTextWeight::Light, bool* bOutMatched = nullptr);

	/**
	 * The imported sheet for @p Weight, or null when it did not resolve. Only the two renderers
	 * need this — and they must pass the weight they are about to lay out, or every glyph samples
	 * the right cell of the wrong sheet and the text draws in the other weight.
	 */
	TRACE_API UTexture2D* AtlasTexture(ETraceTextWeight Weight = ETraceTextWeight::Light);

	// =============================================================================================
	// METRICS — all in screen pixels at @p Size, and all correct in the fallback too
	// =============================================================================================

	/** Top of the line box to the next line's top. Identical in both weights, by construction. */
	TRACE_API float LineHeight(float Size);

	/** Top of the line box down to the baseline. Identical in both weights, by construction. */
	TRACE_API float Ascent(float Size);

	/**
	 * Baseline up to the top of a capital. This is what the artist's word sprites are placed by.
	 *
	 * *** THE ONE METRIC THAT REALLY DIFFERS BY WEIGHT. *** Synthesising the light cut erodes rows
	 * off the top of every capital, so light caps are ~10% shorter than bold ones at the same size
	 * (59 vs 65 atlas px at an em of 96). Pass the weight you are about to draw in.
	 */
	TRACE_API float CapHeight(float Size, ETraceTextWeight Weight = ETraceTextWeight::Light);

	/** The Size whose caps come out @p InCapHeight tall, in @p Weight. The inverse of CapHeight(). */
	TRACE_API float SizeForCapHeight(float InCapHeight,
		ETraceTextWeight Weight = ETraceTextWeight::Light);

	/** Width the string occupies. Multi-line strings report their widest line. */
	TRACE_API float MeasureWidth(const FString& Text, const FStyle& Style);
	TRACE_API float MeasureWidth(const FString& Text, float Size);

	/** Width and height together; height is LineHeight() times the number of lines. */
	TRACE_API FVector2f Measure(const FString& Text, const FStyle& Style);

	// =============================================================================================
	// LAYOUT — the one pass both renderers run
	// =============================================================================================

	/**
	 * Fills @p OutQuads with one quad per drawable glyph, positioned relative to the draw origin
	 * that @p Style's alignment implies. Handles '\n'.
	 *
	 * Returns false when the atlas is not live, in which case OutQuads is emptied and the caller must
	 * take its own fallback path (both renderers in this module do). Whitespace produces no quad but
	 * still advances the pen.
	 */
	TRACE_API bool LayoutString(const FString& Text, const FStyle& Style, TArray<FGlyphQuad>& OutQuads);

	/**
	 * The offset from the caller's (X, Y) to the top-left of the first line's line box, for the
	 * alignment in @p Style. Exposed because the fallback renderers need exactly the same shift
	 * applied to a font that knows nothing about this module.
	 */
	TRACE_API FVector2f AlignOffset(const FVector2f& BlockSize, const FStyle& Style, float Size);
}
