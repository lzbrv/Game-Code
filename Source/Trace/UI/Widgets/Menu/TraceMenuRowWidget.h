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

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

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

	bool bSelected = false;

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

	/** Plate opacity when the row is / is not selected. Editable per asset, not per build. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	float SelectedPlateOpacity = 0.80f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	float UnselectedPlateOpacity = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	float SelectedEdgeOpacity = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	float UnselectedEdgeOpacity = 0.16f;

	/** Breaths per second of the selection bar. 0 freezes it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	float PulseSpeed = 4.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	FLinearColor AccentColor = FLinearColor(0.16f, 0.88f, 1.00f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	FLinearColor SelectedLabelColor = FLinearColor(0.90f, 0.97f, 1.00f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	FLinearColor UnselectedLabelColor = FLinearColor(0.42f, 0.58f, 0.66f, 1.f);

	/** Alpha the arrows drop to when there is nothing further in that direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row")
	float DisabledArrowAlpha = 0.20f;

protected:
	/** The dark plate the label sits on. Always opaque enough to lift the label off the grid. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> RowPlate;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> TopEdge;

	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> BottomEdge;

	/** Wash across the whole plate while selected. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> SelectionWash;

	/** The breathing bar on the leading edge. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> SelectionBar;

	/** Sits OUTSIDE the row's left edge, which is why the row must not clip its children. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> ChevronText;

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
