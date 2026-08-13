// Trace — the two Sofachrome weights (spec v23 §A3).
//
// =================================================================================================
// WHAT THE OWNER ASKED FOR
// =================================================================================================
//   "the font type should be extra light, not regular/bold"
//   "leave the character names in the selection menu bold, though."
//
// So: LIGHT is the default for every string in the game, and BOLD is an opt-in that exactly one
// screen takes. A caller that says nothing gets light — that is why Light is the zero value, and
// why TraceText::FStyle default-constructs to it. Nobody has to be told to ask for light.
//
// =================================================================================================
// TWO REAL FONT FILES — AND THEY DO NOT SHARE ADVANCES
// =================================================================================================
// Each weight is rasterised from its own licensed file, --thin 0 for both:
//
//     Light = Sofachrome W05 ExtraLight.ttf   (weight 250)   T_FontAtlas       — THE DEFAULT
//     Bold  = Sofachrome Rg.otf               (weight 400)   T_FontAtlasBold   — character names
//
// This block used to say the opposite, and the correction is spec v23's integration pass. Until the
// real ExtraLight arrived the light cut was SYNTHESISED by eroding Regular, and two things followed
// from that which are now false:
//
//   * The advances were identical, because it was one font twice. THEY ARE NOT NOW: 94 of the 95
//     cells differ and bold is roughly half again as wide ("!" is 19 px light against 40 px bold at
//     em 96; "ABCDEFGHIJKLMNOPQRSTUVWXYZ" measured 932 px light against 1056 px bold on screen).
//     *** ANYTHING THAT MEASURES A STRING MUST PASS THE WEIGHT IT WILL DRAW IT IN. *** Layout and
//     MeasureWidth() are both face-relative and will answer correctly; a caller that measures light
//     and draws bold will overrun its column, which is exactly what happened to the character name
//     on the select screen.
//   * The cap heights differed, because erosion ate rows off every capital. They now MATCH at 65 px
//     — two real cuts of one family share it. CapHeight()/SizeForCapHeight() still take a weight,
//     because that is the honest signature and a third cut need not agree.
//
// Do not reintroduce --thin. Two attempts to synthesise a weight damaged the letterforms; the second
// deleted ( ) [ ] { } / and \ outright while the HUD draws '[E]'.
//
// =================================================================================================
// WHY ITS OWN HEADER
// =================================================================================================
// It is a UENUM so a designer can pick it in a UMG detail panel, a Blueprint can pass it, and
// TraceText::WeightFromName() can resolve it from a string — that is what "ask for bold by name"
// needs. TraceText.h is a plain non-UObject header and cannot host a UENUM, and putting it in
// TraceAtlasTextWidget.h would make the Canvas blitter depend on the UMG widget. A header carrying
// only an enum generates no class registration, so unlike a bare UCLASS it cannot break the link.

#pragma once

#include "CoreMinimal.h"

#include "TraceTextWeight.generated.h"

/**
 * Which cut of Sofachrome to draw.
 *
 * The values are indices into TraceFontAtlasMetrics::Faces and the two are static_asserted to agree
 * in TraceText.cpp, so adding a weight means adding it to Scripts/import_font_atlas.py's WEIGHTS
 * table and here, and the build breaks if only one of them is done.
 */
UENUM(BlueprintType)
enum class ETraceTextWeight : uint8
{
	/** The eroded cut. THE DEFAULT — every string in the game unless it says otherwise. */
	Light UMETA(DisplayName = "Light"),

	/** Sofachrome as drawn. The character names on the select screen, and nothing else. */
	Bold UMETA(DisplayName = "Bold"),

	Count UMETA(Hidden)
};
