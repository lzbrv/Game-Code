// Trace — one row of the title menu, as a UMG widget (spec v17 §4).
//
// THE ARCHITECTURE, restated because it is the part that is not negotiable:
//   * ALL the behaviour stays in C++. This class decides nothing. It is handed an FTraceMenuRowView
//     that ATraceMenuHUD built from the state it already owned before UMG existed — which row is
//     selected, what the value reads, whether the arrows are live — and it puts that on screen.
//   * The .uasset (Content/Trace/UI/Menu/WBP_MenuRow) carries the TREE and the STYLING only: where
//     the label sits, what font it is, how big the plate is. No graph, no logic.
//   * Every UPROPERTY(meta=(BindWidget)) below MUST exist in that asset with EXACTLY this name.
//     That is the safety net, not a formality: a renamed or deleted widget fails the Blueprint
//     compile loudly instead of drawing nothing at runtime.
//
// WHY TWO VALUE BLOCKS. The Canvas row has two mutually exclusive right-hand readouts and they are
// deliberately different sizes: PLAY and JOIN carry an ADDRESS in the small font (an IPv4 address
// plus a port is twenty characters and at the label's size it collides with "JOIN" on a 1280-wide
// window), while DIFFICULTY and SCORING MODE carry a VALUE in the row font with arrows either side.
// One text block trying to be both would have to be restyled from C++ every frame, which is exactly
// the styling-in-code this migration exists to remove.
//
// -------------------------------------------------------------------------------------------------
// SPEC v19 §5 — THE ROW IS NOW THE ARTIST'S BUTTON
// -------------------------------------------------------------------------------------------------
// The plate used to be three coloured rectangles. It is now ONE sprite off the artist's sheet, swapped
// between three brushes for the three states they drew: default (blue fill, white word), hover (amber
// ring, gold word) and disabled (near-black fill, grey ring). Those are real states now, not decoration
// — ATraceMenuHUD fills bSelected / bPressed / bEnabled from the actual mouse and the actual
// activation gate, and the row draws whichever one is true.
//
// PRESSED IS THE ONE STATE THE SHEET DOES NOT CONTAIN. There are three plates on it and a press is
// not one of them, so pressed is the HOVER plate at 72% brightness. That is a stand-in and it is
// marked as one (TraceMenuArtStyle::PressedTint); a fourth plate would replace it with no code change.
//
// THE WORD IS A SEPARATE LAYER FROM THE PLATE, and that is what makes the sheet go as far as it does.
// PLAY and SETTINGS are on the sheet, so those two rows draw the artist's own lettering, in all three
// states, by tinting one white sprite. JOIN, DIFFICULTY, SCORING MODE and QUIT are not on the sheet
// and cannot be, so they are engine-rendered in the stand-in font — see TraceMenuArtStyle.h, which is
// the only place a font is named.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateBrush.h"

#include "TraceMenuRowWidget.generated.h"

class UImage;
class UTextBlock;

/**
 * Everything ATraceMenuHUD tells a row about itself for one frame.
 *
 * A view, not a model: the row keeps no copy and decides nothing from it.
 */
struct FTraceMenuRowView
{
	FString Label;

	/** Right-aligned ADDRESS readout, small font. Empty hides it. PLAY and JOIN only. */
	FString Status;

	/** Right-aligned VALUE, row font, with arrows. Empty hides it. DIFFICULTY and MODE only. */
	FString Value;
	FLinearColor ValueColor = FLinearColor::White;

	/** Hover / keyboard selection. On this menu they are the same thing — see ATraceMenuHUD::DrawHUD. */
	bool bSelected = false;

	/** The mouse is DOWN on this row and has not been let go yet. See ATraceMenuHUD::PressedRow. */
	bool bPressed = false;

	/**
	 * False when activating this row would do nothing right now.
	 *
	 * Two real causes, not a decoration: the 0.35 s grace period after the title screen appears (see
	 * ATraceMenuHUD::AcceptsActivation, which swallows a press that early) and the whole screen going
	 * inert once a travel has started. Both used to be invisible — the row looked live and silently
	 * refused. Now it wears the artist's disabled plate for exactly as long as it is refusing.
	 */
	bool bEnabled = true;

	/** Arrows are drawn dim at the ends of the range so the player can see there is no further. */
	bool bShowArrows = false;
	bool bCanLeft = false;
	bool bCanRight = false;
};

/**
 * One title-menu row. Instantiated six times inside WBP_TitleMenu, in a vertical box.
 *
 * Its cached geometry IS the row's hit rect — ATraceMenuHUD reads it back every frame and keeps
 * using its own RowAtPoint / press-arms-release-fires logic, so the single-press contract of spec
 * v15 §4 is literally the same code it was before this widget existed.
 */
UCLASS()
class TRACE_API UTraceMenuRow : public UUserWidget
{
	GENERATED_BODY()

public:
	/** @param InNow world seconds, for the selected row's breathing pulse. */
	void ApplyView(const FTraceMenuRowView& InView, float InNow);

	/**
	 * Every sprite slot this row is supposed to carry, and whether it actually arrived. Read by
	 * `Trace.UI.VerifyMenuArt` through UTraceTitleMenuWidget::CountResolvedArt.
	 *
	 * IT COUNTS SLOT BY SLOT RATHER THAN ROW BY ROW, and that is the point. A row whose HOVER plate
	 * failed to load looks perfect until the cursor touches it and then flashes a white rectangle —
	 * the state a player only meets at the moment they are about to click. Counting "does this row
	 * have art" would have called such a row resolved.
	 *
	 * @param InLabel  what to call this row in the missing-sprite list, e.g. "row 0 (PLAY)".
	 * @return the number of this row's slots that resolved; @p OutTotal is grown by how many there are.
	 */
	int32 CountResolvedArt(int32& OutTotal, TArray<FString>& OutMissing, const FString& InLabel) const;

	// ---- The three states the artist drew, as brushes the asset carries -----------------------------
	//
	// Authored by Scripts/generate-menu-widgets.py, editable in the editor afterwards. They are
	// Box-draw brushes with the margin and image size TraceMenuArtStyle::ButtonFrame derives, which is
	// what lets one 512 px texture be a 720-wide row without the corner going oval.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row|Art")
	FSlateBrush PlateDefaultBrush;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row|Art")
	FSlateBrush PlateHoverBrush;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row|Art")
	FSlateBrush PlateDisabledBrush;

	/**
	 * PER INSTANCE: this row's label as one of the four words the sheet actually contains.
	 *
	 * Set on RowPlay and RowSettings only. Leave it empty and the row types its label instead — which
	 * is what JOIN, DIFFICULTY, SCORING MODE and QUIT do, because those words are not on the sheet.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row|Art")
	FSlateBrush WordBrush;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row|Art")
	bool bLabelIsSprite = false;

	/** Breaths per second of the hover glow. 0 freezes it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	float PulseSpeed = 4.5f;

	/** Alpha the arrows drop to when there is nothing further in that direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	float DisabledArrowAlpha = 0.20f;

protected:
	/**
	 * The artist's button plate. ONE image, three brushes.
	 *
	 * Its slot is deliberately BIGGER than the row rect, by TraceMenuArtStyle::ButtonFrame's glow
	 * inset on every side: the sprite carries 128 sheet pixels of halo outside the plate, and drawing
	 * it inside the row rect would shrink the plate itself by a fifth.
	 */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> PlateSprite;

	/** The artist's lettering, on the two rows whose word is on the sheet. Tinted per state. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> WordSprite;

	/** Sits OUTSIDE the row's left edge, which is why the row must not clip its children. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> ChevronSprite;

	/** Behind the DIFFICULTY / SCORING MODE readout. The artist's slider chip, doing a second job. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> ValueChip;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> LabelText;

	/** Small-font address readout. See "WHY TWO VALUE BLOCKS" at the top of this file. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> ValueText;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> LeftArrowText;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> RightArrowText;
};
