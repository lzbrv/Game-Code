// Trace — Sofachrome on a Canvas (spec v22 §A1).
//
// The Canvas half of the two renderers. This is what the screens that still draw through AHUD use:
// TraceOptionsMenu, TraceCharacterSelect and the in-match HUD.
//
// It owns NO metrics and NO layout. Every call here asks TraceText::LayoutString() where the letters
// go and then blits one textured quad per glyph with AHUD::DrawTexture, whose U/V arguments are a
// normalised sub-rect — which is exactly a glyph cell. The UMG side does the identical loop with
// FSlateDrawElement::MakeBox. See TraceText.h for why the glyphs are drawn by hand at all.
//
// CALLING IT, from a DrawHUD / PostRender:
//
//     #include "UI/Text/TraceCanvasText.h"
//
//     TraceText::FStyle Style(28.f, TraceMenuArtStyle::WordDefault);
//     Style.HAlign = TraceText::EHAlign::Center;
//     TraceCanvasText::Draw(HUD, TEXT("SCORING MODE"), CenterX, RowY, Style);
//
// X and Y mean what they mean in AHUD::DrawText — the top-left of the line box — unless Style says
// otherwise, so swapping a DrawText call for this one does not move the caller's layout.
//
// ASKING FOR BOLD (spec v23 §A3). Everything here draws in the LIGHT cut unless the style says
// otherwise, which is the owner's instruction. The character names on the select screen are the one
// exception in the game:
//
//     TraceText::FStyle Style(NameSize, Color);
//     Style.Weight = ETraceTextWeight::Bold;
//     TraceCanvasText::Draw(HUD, Entry.Name, X, Y, Style);
//
// or, for the sugar overloads, DrawBold(...). Bold costs nothing in layout — both weights share
// every advance width — so a caller can add that line without re-measuring anything.
//
// THE FALLBACK IS INSIDE, not the caller's problem: with the atlas off (missing, stale,
// -TraceNoFontAtlas, or `Trace.Text.Atlas 0`) these functions typeset Lato through the same
// FCanvasTextItem path the rest of the Canvas menus use, and MEASURE it in Lato too, so a centred
// label stays centred in the degraded path.

#pragma once

#include "CoreMinimal.h"

#include "UI/Text/TraceText.h"

class AHUD;
class UCanvas;

namespace TraceCanvasText
{
	/**
	 * Draws @p Text and returns the width it occupied.
	 *
	 * @param HUD    the HUD currently rendering; nothing is drawn outside a draw pass.
	 * @param X, Y   the origin, interpreted per @p Style's alignment (default: top-left).
	 */
	TRACE_API float Draw(AHUD* HUD, const FString& Text, float X, float Y, const TraceText::FStyle& Style);

	/** As above, for callers that already hold the UCanvas (a UDebugDrawService delegate, say). */
	TRACE_API float Draw(UCanvas* Canvas, const FString& Text, float X, float Y, const TraceText::FStyle& Style);

	/** Sugar for the overwhelmingly common case. Colour and size, nothing else. */
	TRACE_API float Draw(AHUD* HUD, const FString& Text, float X, float Y, float Size, const FLinearColor& Color);

	/** Centred on @p CenterX. Identical to setting Style.HAlign = Center. */
	TRACE_API float DrawCentered(AHUD* HUD, const FString& Text, float CenterX, float Y,
		float Size, const FLinearColor& Color);

	/**
	 * The same draw in the BOLD cut — Sofachrome as drawn, rather than the eroded light default.
	 *
	 * This exists for the character names on the select screen and is not meant to spread: the
	 * owner asked for the game to be light and for those names to stay bold. Identical to setting
	 * Style.Weight = ETraceTextWeight::Bold, and it occupies exactly the same width as the light
	 * draw it replaces, so it can be swapped in without touching a layout.
	 */
	TRACE_API float DrawBold(AHUD* HUD, const FString& Text, float X, float Y,
		float Size, const FLinearColor& Color);

	/**
	 * The UCanvas the engine is drawing the HUD into, or null outside a draw pass.
	 *
	 * AHUD::Canvas is protected and AHUD exposes no accessor, so this is found where the engine put
	 * it — UGameViewportClient::Draw creates one UCanvas named "CanvasObject" in the transient
	 * package and hands that same object to AHUD::SetCanvas. Same lookup, same reasoning and the same
	 * guards as UI/TraceCharacterSelect.cpp, which needed it first.
	 */
	TRACE_API UCanvas* GameCanvas();
}
