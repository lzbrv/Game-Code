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
	 * The UCanvas the engine is drawing the HUD into, or null outside a draw pass.
	 *
	 * AHUD::Canvas is protected and AHUD exposes no accessor, so this is found where the engine put
	 * it — UGameViewportClient::Draw creates one UCanvas named "CanvasObject" in the transient
	 * package and hands that same object to AHUD::SetCanvas. Same lookup, same reasoning and the same
	 * guards as UI/TraceCharacterSelect.cpp, which needed it first.
	 */
	TRACE_API UCanvas* GameCanvas();
}
