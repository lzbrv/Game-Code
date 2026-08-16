// Trace — the faces the game draws type in (spec v23 §A3, third face by v25 §4).
//
// =================================================================================================
// WHAT THE OWNER ASKED FOR
// =================================================================================================
//   v23:  "the font type should be extra light, not regular/bold"
//         "leave the character names in the selection menu bold, though."
//   v25:  "Use Erbaum Bold for in game hud and character ability descriptions"
//
// So: LIGHT is the default for every string in the game, and everything else is an opt-in that a
// named screen takes. A caller that says nothing gets light — that is why Light is the zero value,
// and why TraceText::FStyle default-constructs to it. Nobody has to be told to ask for light.
//
// =================================================================================================
// THREE REAL FONT FILES — AND THEY SHARE NEITHER ADVANCES NOR BASELINES
// =================================================================================================
// Each face is rasterised from its own licensed file, --thin 0 for all three:
//
//     Light = Sofachrome W05 ExtraLight.ttf   (weight 250)   T_FontAtlas       — THE DEFAULT
//     Bold  = Sofachrome Rg.otf               (weight 400)   T_FontAtlasBold   — character names
//     Hud   = Erbaum-Bold.otf                 (weight 700)   T_FontAtlasHud    — match HUD + ability text
//
// This block used to say the opposite, and the correction is spec v23's integration pass. Until the
// real ExtraLight arrived the light cut was SYNTHESISED by eroding Regular, and two things followed
// from that which are now false:
//
//   * The advances were identical, because it was one font twice. THEY ARE NOT NOW: 94 of the 95
//     cells differ between the two Sofachrome cuts and bold is roughly half again as wide ("!" is
//     19 px light against 40 px bold at em 96). Erbaum is narrower than either — the alphabet
//     measures 1823 px against light's 2634. *** ANYTHING THAT MEASURES A STRING MUST PASS THE
//     WEIGHT IT WILL DRAW IT IN. *** Layout and MeasureWidth() are both face-relative and will
//     answer correctly; a caller that measures light and draws bold will overrun its column, which
//     is exactly what happened to the character name on the select screen.
//   * The cap heights differed, because erosion ate rows off every capital. The two Sofachrome cuts
//     now MATCH at 65 px; Erbaum's is 70 px, because it is a different family and there is no reason
//     it would agree. CapHeight()/SizeForCapHeight() take a weight for exactly that.
//
// *** AND, NEW IN v25: THE BASELINE MOVES TOO. *** Every face shares the 116 px line box — that is
// enforced by Scripts/import_font_atlas.py and it is what lets one layout pass serve all three — but
// Erbaum splits it 93/23 where both Sofachrome cuts split it 95/21. TraceText::Ascent() therefore
// takes a weight, and EVAlign::Baseline / EVAlign::CapTop draws must pass the one they are drawing.
//
// "Weight" is a slight misnomer now: Hud is a different FAMILY, not a heavier cut of Sofachrome.
// The name stays because this is the axis the runtime, the UMG detail panel and every existing
// caller already select on, and splitting it into family+weight would double every signature in
// TraceText.h to serve one caller.
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
 * Which face to draw.
 *
 * The values are indices into TraceFontAtlasMetrics::Faces and the two are static_asserted to agree
 * in TraceText.cpp, so adding a face means adding it to Scripts/import_font_atlas.py's WEIGHTS
 * table and here, and the build breaks if only one of them is done.
 *
 * APPEND, never insert: a value is what a saved knob, a data-asset column and a Blueprint pin all
 * carry, and moving one would silently re-point every one of them at a different sheet.
 */
UENUM(BlueprintType)
enum class ETraceTextWeight : uint8
{
	/** Sofachrome ExtraLight. THE DEFAULT — every string in the game unless it says otherwise. */
	Light UMETA(DisplayName = "Light"),

	/** Sofachrome Regular. The character names on the select screen, and nothing else. */
	Bold UMETA(DisplayName = "Bold"),

	/** Erbaum Bold. The in-match HUD and the character-select ability DESCRIPTIONS (spec v25 §4). */
	Hud UMETA(DisplayName = "Hud (Erbaum Bold)"),

	Count UMETA(Hidden)
};
