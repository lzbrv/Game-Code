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
// between three brushes for the three states they drew: default (blue fill), hover (amber ring) and
// disabled (near-black fill, grey ring). Those are real states now, not decoration — ATraceMenuHUD
// fills bSelected / bPressed / bEnabled from the actual mouse and the actual activation gate, and the
// row draws whichever one is true.
//
// THE WORD ON THE HOVER STATE IS THE SHEET'S GREEN — sRGB(85,107,47), spec v24 §3.
// It was white for two specs. That was spec v20 §0.5 reacting to a MIS-SAMPLED palette entry: what
// TraceMenuArtStyle called "the artist's hover word" was the amber halo around it, sRGB(115,82,50),
// which measured 1.9:1 against its plate and was rightly replaced. The three plates on the sheet
// that carry a real button label in the hover state all draw the word a flat green, to the byte; the
// re-measurement is in TraceMenuArtStyle.h. The plate, the ring, the rail and the word are now
// driven by ONE state through ONE switch (TraceMenuRowWidgetLocal::VisualsFor), which is §3's actual
// requirement: the outline and the label cannot disagree about being hovered.
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

#include "UI/Text/TraceAtlasTextSwap.h"

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

	// ---- SPEC v26 §7: A 2 PX WHITE OUTLINE ON THE DEFAULT (NON-HOVER) STATE -------------------------
	//
	//     "Add a 2pixel white outline to the default button state (non-hover)"
	//
	// THE WIDTH IS IN *DEVICE* PIXELS, AND THAT IS THE WHOLE OF THE INTERESTING PART.
	//
	// This widget is authored in a 1080-tall reference space and UMG scales it by the viewport's DPI
	// factor before it reaches the screen — the engine's default curve (BaseEngine.ini's
	// UIScaleCurve, keyed on the shortest side) is 0.666 at 720p, 1.0 at 1080p, 1.333 at 1440p and
	// 2.0 at 2160p. A Slate rounded box's outline Width is in LOCAL units, so a constant 2 written
	// here would draw 1.3 px at 720p, 2 px at 1080p and 4 px on a 4K display: precisely the "collapses
	// to 1 px / bloats on 4K" failure §7 asks to be avoided, and precisely the kind of thing that
	// looks right on the machine it was authored on.
	//
	// So the number below is what the OWNER asked for — a width in the pixels they can count in a
	// screenshot — and the local width is derived from it at draw time by dividing by the live layout
	// scale. Spec v24 §0's standing rule, applied to a unit rather than to a colour: the value that
	// modifies the base is stored RELATIVE to the base (here: relative to the DPI scale), so it
	// follows when the base moves rather than having to be re-tuned per resolution.
	//
	// The CORNER RADIUS goes the other way on purpose and is not a knob: it is a fraction of the row's
	// height derived from the artist's own plate geometry (TraceMenuArtStyle::ButtonFrame), because
	// the corner is part of the button's SHAPE and must scale with it, while the stroke is part of the
	// button's finish and must not. See OutlineCornerRadius() in the .cpp.

	/**
	 * The outline's width in DEVICE pixels, at every resolution. §7 asks for 2.
	 *
	 * EditAnywhere so it can be A/B'd in the editor without a rebuild; 0 switches the outline off,
	 * which is also the red arm for this feature.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Row|Art",
		meta = (ClampMin = "0.0", UIMax = "8.0"))
	float DefaultOutlineWidthPx = 2.f;

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

	/**
	 * The selection mark. Sits OUTSIDE the row's left edge, which is why the row must not clip its
	 * children.
	 *
	 * STILL CALLED ChevronSprite because the NAME IS A CONTRACT: it is a BindWidget, so it has to
	 * match the widget in WBP_MenuRow exactly, and that asset is authored by
	 * Scripts/generate-menu-widgets.py. Renaming it here without renaming it there fails the
	 * Blueprint compile. What it draws is no longer a sprite — see spec v22 §A4 at the top of
	 * TraceMenuRowWidget.cpp.
	 */
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

private:
	/**
	 * Whether the leading-edge selection rail has replaced the crescent WBP_MenuRow still ships in
	 * ChevronSprite's slot. Latched so the swap costs one brush assignment per row for the life of
	 * the menu instead of one per frame. Not serialised — the asset is authored with the artist's
	 * brush and every run re-applies the rail over it.
	 */
	bool bSelectionMarkApplied = false;

	// ---- SPEC v22 §A1: THE ROW IS ONE TYPEFACE ------------------------------------------------------
	//
	// "Retire the split ... set those labels as live text like every other label so the whole screen
	// is one typeface." Every string on this row — the word, the address readout, the value and its
	// two arrows — is drawn from the Sofachrome atlas by a UTraceAtlasText that has taken the
	// authored UTextBlock's place in its panel. The text blocks above are still the model and are
	// still what ApplyView writes to; see UI/Text/TraceAtlasTextSwap.h for why it is done that way
	// round.
	//
	// WordSprite is collapsed for good once this lands. The sprite stays in the repo and stays in the
	// asset, exactly as A1 asks — it is simply no longer how this row spells PLAY, because the row
	// can now spell anything in the same letterforms.

	/** Installed once, on the first ApplyView, when the widget tree is certain to exist. */
	bool bAtlasLabelsInstalled = false;

	UPROPERTY(Transient)
	TArray<FTraceAtlasLabel> AtlasLabels;

	/** Install the five swaps. Idempotent; a failure leaves the authored text blocks drawing. */
	void InstallAtlasLabels();

	// ---- SPEC v26 §7 -------------------------------------------------------------------------------
	//
	// NOT a BindWidget, and it cannot be one: BindWidget properties are bound BY NAME to widgets that
	// exist in WBP_MenuRow, and that asset is authored by Scripts/generate-menu-widgets.py, which this
	// agent does not own — adding a required binding here would fail the Blueprint's compile and take
	// the whole title screen down until somebody re-ran the generator in an editor. So the outline is
	// CONSTRUCTED at runtime into the same canvas panel the plate lives in, the same way
	// TraceAtlasTextSwap inserts the atlas labels, and a row whose tree is not what this expects
	// simply keeps the screen it had.

	/** The stroke. A textureless RoundedBox: transparent fill, white outline. */
	UPROPERTY(Transient)
	TObjectPtr<UImage> DefaultOutline = nullptr;

	/** Latched, like the atlas labels: one branch a frame after the first. */
	bool bDefaultOutlineInstalled = false;

	/** The local-unit width the brush currently carries, so the brush is rebuilt only when it moves. */
	float DefaultOutlineLocalWidth = -1.f;

	/** Build the outline image and put it over the plate. Idempotent; failure is silent and harmless. */
	void InstallDefaultOutline();

	/**
	 * Show / hide the outline and keep it 2 DEVICE pixels wide.
	 *
	 * @param bWanted true only in the state §7 names — the default, non-hover, enabled row.
	 */
	void ApplyDefaultOutline(bool bWanted);

	/**
	 * The layout scale between this row's local units and screen pixels.
	 *
	 * The row's OWN cached geometry first, so the answer stays right if this widget is ever put inside
	 * a scale box or a retainer as well as the viewport's DPI scaler; the viewport scale as the
	 * fallback for the frames before this row has been painted, where the cached geometry is still the
	 * identity and would claim 1.0 at every resolution.
	 */
	float LayoutScale() const;
};
