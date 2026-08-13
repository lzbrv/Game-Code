// Trace — one status chip, as a UMG widget. Spec v17 §4 (step 4b).
//
// The UMG twin of ATraceHUD::DrawStatusChip. Same chip, same words, same colours, same direction of
// travel: it DRAINS toward gone, which is the second half of spec v16 §2's "separate from cooldowns"
// (the corner separates them in space; the direction separates them in motion, so the two never read
// as the same widget out of the corner of an eye).
//
// *** WHAT IS IN THE C++ AND WHAT IS IN THE .UASSET, because that split is the point of the step. ***
//
//   C++ (this class)      : which chips exist, their words, their numbers, their hue, the drain
//                           fraction. All of it arrives pre-computed in an FTraceHudCornerChip that
//                           ATraceHUD built from gameplay state — this class asks nothing of the game.
//   WBP_TraceHudStatusChip: the tree and the styling. Sizes, fonts, the panel fill, the trough behind
//                           the drain, paddings. Open it and move things; nothing here will mind.
//
// The BindWidget properties below are the contract between the two, and they are enforced twice: the
// Widget Blueprint compiler refuses to compile an asset that is missing one, and ValidateBindings()
// checks again at runtime before the corner is adopted — because a cooked asset from a stale
// generate can still arrive here with a null in it, and a corner with a hole in it is worse than the
// Canvas corner that has worked since v16.

#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"

#include "UI/Widgets/HUD/TraceHudCornerData.h"

#include "UI/Text/TraceAtlasTextSwap.h"

#include "TraceHudStatusChipWidget.generated.h"

class UBorder;
class UImage;
class UProgressBar;
class UTextBlock;

UCLASS()
class TRACE_API UTraceHudStatusChipWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/**
	 * Fills the chip in and makes it visible. Returns the line it would contribute to the spec v16
	 * §2 draw record — built HERE, from what was actually pushed into the widgets, so the record
	 * cannot certify itself.
	 */
	FString ApplyChip(const FTraceHudCornerChip& InChip);

	/**
	 * True when every BindWidget property resolved. @p OutMissing names the first that did not, so a
	 * broken asset produces "WBP_TraceHudStatusChip is missing 'DrainBar'" rather than a shrug.
	 *
	 * Non-static and called on a real instance rather than the CDO: BindWidget properties are only
	 * populated once a widget has been constructed from a Blueprint class.
	 */
	bool ValidateBindings(FString& OutMissing) const;

	/** Every BindWidget name this class needs, for the verifier and for the generator's manifest. */
	static const TArray<FName>& RequiredWidgetNames();

protected:
	/**
	 * The outline. A Border whose brush carries the chip's tint at 45% alpha, with its padding acting
	 * as the stroke width — Slate has no stroked-rect primitive either, and two nested Borders is the
	 * cheapest honest way to get one. (The Canvas pass draws the same outline as four thin rects.)
	 */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UBorder> ChipOutline;

	/** The dark fill inside the outline. Stays dark so six stacked chips never light the corner up. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UBorder> ChipFill;

	/** The saturated tab down the left edge. This is what carries the colour at a glance. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> ColorTab;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> LabelText;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> ReadoutText;

	/**
	 * The draining indicator. A ProgressBar because that is exactly the shape the Canvas pass builds
	 * by hand (a dark trough plus a coloured fill from the left) and because its trough, its corner
	 * radius and its height are then all styling a designer can reach.
	 */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UProgressBar> DrainBar;

	// SPEC v22 §A1 — the chip's two strings come off the glyph atlas, like the corner it sits under.
	bool bAtlasLabelsInstalled = false;

	UPROPERTY(Transient)
	TArray<FTraceAtlasLabel> AtlasLabels;

	void InstallAtlasLabels();
};
