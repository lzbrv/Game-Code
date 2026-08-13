// Trace — putting Sofachrome where a UTextBlock used to be (spec v22 §A1, integration pass).
//
// =================================================================================================
// WHY THIS FILE EXISTS
// =================================================================================================
// Spec A1 has two halves. The first — a real Sofachrome renderer for both Canvas and UMG — landed as
// UI/Text/TraceText.*, TraceCanvasText.* and TraceAtlasTextWidget.*. The second is the one the
// player actually sees:
//
//     "Then RETIRE THE SPLIT ... set those labels as live text like every other label so the whole
//      screen is one typeface."
//
// That half did not land: `grep -rn TraceCanvasText Source/` outside UI/Text/ was empty, and the
// shipped title screen still drew PLAY and SETTINGS as baked word sprites with JOIN, PRACTICE,
// DIFFICULTY, SCORING MODE and QUIT beside them in Lato. This file is how the UMG screens cross
// over.
//
// =================================================================================================
// THE PROBLEM IT SOLVES, AND WHY IT IS NOT A ONE-LINER
// =================================================================================================
// UTraceAtlasText is a drop-in for a UTextBlock in every way except the one that matters here: the
// title screen's text blocks are `meta = (BindWidget)` properties bound BY NAME AND TYPE to
// WBP_TitleMenu / WBP_MenuRow, which are authored by Scripts/generate-menu-widgets.py. Changing
// their declared type would mean regenerating both .uassets in the editor, and a half-written
// generator run leaves the project with no title screen at all.
//
// So the widgets stay exactly as authored and the atlas label TAKES THE TEXT BLOCK'S PLACE IN ITS
// PARENT PANEL AT RUNTIME:
//
//     * the UTextBlock is removed from its panel and kept as the DATA MODEL. Every existing
//       `LabelText->SetText(...)` / `SetColorAndOpacity(...)` / `SetVisibility(...)` call in the
//       menu keeps working, untouched, and keeps meaning what it meant.
//     * a UTraceAtlasText is inserted AT THE SAME INDEX, and its slot's layout properties are copied
//       off the text block's old slot by reflection — so anchors, offsets, alignment, auto-size,
//       Z-order, box padding and fill all survive whatever panel type this happens to be. A
//       horizontal box keeps its order (that is the whole reason for the index), which is what stops
//       "<  NORMAL  >" coming out as "NORMAL  <  >".
//     * Mirror() copies text, colour and visibility across once per frame.
//
// The result is that the SCREEN changes typeface and the CODE that drives it does not.
//
// =================================================================================================
// IT CANNOT MAKE THE MENU WORSE THAN IT FOUND IT
// =================================================================================================
// Install() returns null and changes nothing if anything is missing — no source, no parent panel, no
// widget tree. Mirror() on a null record is a no-op, and the caller's original UTextBlock line is
// still there and still runs. And if the ATLAS is missing, UTraceAtlasText itself already draws the
// same string at the same size and position in Lato (see TraceText.h), so the failure mode of the
// whole chain is "the menu you had before", never "a menu with no words in it".

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectPtr.h"

#include "TraceAtlasTextSwap.generated.h"

class UTextBlock;
class UTraceAtlasText;
class UUserWidget;
class UWidget;

/**
 * One swapped label: the text block that is still the model, and the atlas widget that draws it.
 *
 * A USTRUCT with real UPROPERTYs rather than two raw pointers, because the atlas widget's only other
 * referrer is the panel slot it was inserted into and the day somebody rebuilds that tree is the day
 * a raw pointer here becomes a crash on the title screen.
 */
USTRUCT()
struct FTraceAtlasLabel
{
	GENERATED_BODY()

	/** The authored UTextBlock, detached from its panel. Still the thing the menu code writes to. */
	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> Source = nullptr;

	/** What actually draws, in the artist's face. */
	UPROPERTY(Transient)
	TObjectPtr<UTraceAtlasText> Atlas = nullptr;

	/** The authored point size, kept so the shrink-to-fit below never compounds across frames. */
	UPROPERTY(Transient)
	float BaseSize = 0.f;

	/**
	 * Shrink the size when the string would be wider than this, in the panel's LOCAL units. 0 = off.
	 *
	 * Sofachrome is a wide face — the same string measures about a third wider than it does in Lato
	 * at the same size — so a label that fitted its plate in the fallback face can run into the value
	 * chip beside it in the real one. This is that guard, and it is a MEASUREMENT (UTraceAtlasText
	 * reports the width it will occupy) rather than a hand-tuned smaller number per string.
	 */
	UPROPERTY(Transient)
	float MaxWidth = 0.f;

	bool IsValid() const { return Source != nullptr && Atlas != nullptr; }
};

namespace TraceAtlasTextSwap
{
	/**
	 * Puts an atlas label in @p Source's place inside its parent panel and returns the record.
	 *
	 * @param Owner      the UUserWidget whose WidgetTree will own the new widget.
	 * @param Source     the authored text block. Detached from its panel; NOT destroyed.
	 * @param InMaxWidth see FTraceAtlasLabel::MaxWidth. 0 for "never shrink".
	 * @param SizeScale  multiplies the authored point size. 1 keeps the size the generator chose.
	 *
	 * Everything about the new widget — size, colour, justification — is READ OFF @p Source, so a
	 * change to the generator's type scale still lands on the screen without an edit here.
	 *
	 * Safe to call more than once: a source that has already been swapped returns the same record.
	 */
	TRACE_API FTraceAtlasLabel Install(UUserWidget* Owner, UTextBlock* Source,
		float InMaxWidth = 0.f, float SizeScale = 1.f);

	/** Copies text, colour and visibility from the model to the atlas widget. Call once per frame. */
	TRACE_API void Mirror(const FTraceAtlasLabel& Label);

	/** Mirror() over a whole array. */
	TRACE_API void MirrorAll(const TArray<FTraceAtlasLabel>& Labels);

	/**
	 * THE WIDGET THAT IS ACTUALLY IN THE PANEL for @p Source — the atlas label if it was swapped,
	 * otherwise @p Source itself.
	 *
	 * Any code that asks a swapped text block for its `Slot` or its `GetCachedGeometry()` has to go
	 * through this, because a swapped text block has neither: it was detached, so its slot is null
	 * and its cached geometry is whatever it was on the frame it stopped being drawn. The title
	 * screen's footer stack measures exactly those two things — it puts the key hints under the
	 * blurb by measuring where the blurb ENDED UP rather than recomputing it — so without this it
	 * would quietly go back to the authored y and stack the footer on top of itself.
	 *
	 * Returns @p Source unchanged when nothing was swapped, so a call site reads the same either way.
	 */
	TRACE_API UWidget* Live(const TArray<FTraceAtlasLabel>& Labels, UTextBlock* Source);
}
