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
//
// =================================================================================================
// AND A SECOND, SMALLER FALLBACK: PER GLYPH (UI plan WP12)
// =================================================================================================
// The one above is all-or-nothing and answers "is the atlas usable at all". This one answers a
// different question, one codepoint at a time, while the atlas is perfectly healthy.
//
// The three licensed sheets are PRINTABLE ASCII. Every string this game authors is inside that set;
// the one class of string it does not author is a PLAYER NAME. "Björn" arriving over the network
// had no cell for its ö, so LayoutString advanced the pen by a space and drew nothing — a hole in
// the kill feed, in a name, which is the most conspicuous place in the game to have one.
//
// Extending Sofachrome or Erbaum to cover Latin-1 was ruled out: it rasterises more of two faces
// this public repository may not redistribute (docs/FONTS.md). Instead there is a FOURTH sheet,
// T_FontAtlasNames — Lato, which is OFL and whose .ttf is already committed — rasterised over
// Latin-1 plus a few typographic marks. A codepoint the DRAWING face has no cell for is drawn from
// that sheet, scaled by the same em and shifted so its baseline lands on the line's baseline; a
// codepoint missing from BOTH draws that sheet's '?' rather than advancing silently.
//
// Three consequences worth stating, because each is a decision:
//
//   * IT IS GLOBAL, not a names-only special case. One code path, in LayoutString(), so a
//     non-ASCII character cannot draw correctly in the kill feed and as a hole somewhere else.
//   * IT IS NOT A WEIGHT. Nothing can ASK to draw in it — ETraceTextWeight does not list it and
//     WeightFromName() cannot return it. It is reached only by a codepoint, never by a style.
//   * "Björn" therefore sets its ö in Lato inside an Erbaum line. That is what font fallback is,
//     it only ever happens on characters the licensed faces were never rasterised for, and only
//     in strings the game did not write. `Trace.Text.GlyphFallback 0` puts the holes back for a
//     same-binary before/after.
//
// A quad from that sheet is flagged FGlyphQuad::bFallback, and a renderer must sample the texture
// QuadTexture() hands back for it rather than the style's own sheet.

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

		/**
		 * True when this glyph came from the Latin-1 FALLBACK sheet rather than the style's own
		 * face — see the per-glyph fallback block at the top of this file.
		 *
		 * *** THE UVs ARE NORMALISED AGAINST A DIFFERENT TEXTURE WHEN THIS IS SET. *** A renderer
		 * that ignored it would address the fallback sheet's cells on the style's sheet and draw
		 * a smear of the wrong letters. Ask QuadTexture() rather than branching on it: it is the
		 * one place that knows which sheet each flag maps to.
		 *
		 * Pos is already shifted onto the drawing face's baseline, so nothing else about a
		 * fallback quad is special.
		 */
		bool bFallback = false;
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

	/**
	 * The TYPEFACE that @p Weight will actually draw in — "Sofachrome", "Erbaum", or "Lato" when the
	 * atlas is down. Spec v25 §4 asks for the face to be IDENTIFIED in a screenshot rather than
	 * asserted from a flag, and a caption that can only say "Sofachrome" cannot do that once one of
	 * the faces is a different family. Reports the EFFECTIVE face, so a weight that failed to load
	 * names the face that replaced it rather than the one that was asked for.
	 */
	TRACE_API FString FaceName(ETraceTextWeight Weight);

	/** The font FILE behind @p Weight ("Erbaum-Bold.otf"). The unambiguous form, for a log line. */
	TRACE_API const TCHAR* FaceSourceFile(ETraceTextWeight Weight);

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
	// THE PER-GLYPH FALLBACK SHEET (WP12) — what a renderer and a diagnostic need of it
	// =============================================================================================

	/**
	 * The sheet a quad should be sampled from. THE ONLY THING A RENDERER NEEDS TO KNOW about the
	 * per-glyph fallback.
	 *
	 * Pass the weight the quads were laid out in. A quad from the style's own face comes back with
	 * the same texture AtlasTexture() would hand over; one from the Latin-1 sheet comes back with
	 * that instead. Both renderers in this module call it per quad and change nothing else.
	 */
	TRACE_API UTexture2D* QuadTexture(const FGlyphQuad& Quad,
		ETraceTextWeight Weight = ETraceTextWeight::Light);

	/**
	 * True when the Latin-1 sheet resolved and is allowed to serve missing glyphs.
	 *
	 * False means the old behaviour: a codepoint outside printable ASCII advances the pen and draws
	 * nothing. Callers do not need to branch on it — it is here so `Trace.Text.Report` and the
	 * preview can state which of the two behaviours produced the pixels.
	 */
	TRACE_API bool IsGlyphFallbackActive();

	/** "Lato-Regular.ttf" — the file the fallback glyphs are rasterised from, for a caption. */
	TRACE_API const TCHAR* FallbackFaceSourceFile();

	/**
	 * True when @p Char will reach the screen as ITSELF: it has a cell in @p Weight's own sheet, or
	 * in the Latin-1 fallback sheet, or it is whitespace (which correctly draws nothing).
	 *
	 * False means the character has no cell ANYWHERE and will draw the fallback sheet's '?'. That
	 * is the only case worth a log line, and it is what TraceHUD's WarnIfUndrawable asks. Answering
	 * from here rather than from a hard-coded range keeps the charset in one place: this module is
	 * still the only thing that includes TraceFontAtlasMetrics.h.
	 */
	TRACE_API bool CanDraw(TCHAR Char, ETraceTextWeight Weight = ETraceTextWeight::Light);

	// =============================================================================================
	// METRICS — all in screen pixels at @p Size, and all correct in the fallback too
	// =============================================================================================

	/**
	 * Top of the line box to the next line's top. Identical in EVERY face, by construction —
	 * Scripts/import_font_atlas.py refuses to emit a metrics header whose sheets disagree about it,
	 * because this is the number one layout pass advances a multi-line string by.
	 */
	TRACE_API float LineHeight(float Size);

	/**
	 * Top of the line box down to the baseline.
	 *
	 * *** PER FACE SINCE v25. *** The line box is shared; how a face SPLITS it is not. Erbaum-Bold
	 * (the Hud face) splits the 116 px box as 93/23 where both Sofachrome cuts split it 95/21, so a
	 * caller lining a HUD row up by its baseline must pass Hud or sit 2 atlas px out. The default
	 * argument keeps every existing call site meaning exactly what it meant.
	 */
	TRACE_API float Ascent(float Size, ETraceTextWeight Weight = ETraceTextWeight::Light);

	/** Baseline down to the bottom of the line box. Per face, for the same reason as Ascent(). */
	TRACE_API float Descent(float Size, ETraceTextWeight Weight = ETraceTextWeight::Light);

	/**
	 * Baseline up to the top of a capital. This is what the artist's word sprites are placed by.
	 *
	 * *** DIFFERS BY FACE. *** The two Sofachrome cuts agree at 65 atlas px (em 96); Erbaum's caps
	 * are 70, because it is a different family and nothing makes families agree. Pass the weight you
	 * are about to draw in — at HUD sizes that is a whole pixel of cap height.
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
	 *
	 * Quads are NOT guaranteed to come from one texture: a codepoint the style's face has no cell
	 * for comes back flagged bFallback, addressing the Latin-1 sheet. Sample QuadTexture(), not
	 * AtlasTexture(), for each one.
	 */
	TRACE_API bool LayoutString(const FString& Text, const FStyle& Style, TArray<FGlyphQuad>& OutQuads);

	/**
	 * The offset from the caller's (X, Y) to the top-left of the first line's line box, for the
	 * alignment in @p Style. Exposed because the fallback renderers need exactly the same shift
	 * applied to a font that knows nothing about this module.
	 */
	TRACE_API FVector2f AlignOffset(const FVector2f& BlockSize, const FStyle& Style, float Size);
}
