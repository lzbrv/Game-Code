// Trace — the title screen's non-typographic art, as a real UMG widget (spec v17 §4).
//
// WHAT THIS IS FOR
// The title screen is mostly type and rectangles, and UMG already has excellent widgets for both.
// Three parts of it are not: the perspective GRID FLOOR, the inset BEZEL with its corner ticks, and
// the drawn CURSOR. Those are stroked geometry, and expressing them as a hundred nested UImages
// would be a widget tree nobody could edit. So they stay as painted geometry — but as a widget with
// EditAnywhere properties, which means the horizon, the rail count, the scroll speed and every
// colour are things a designer changes in WBP_TitleMenu instead of in C++.
//
// One class with a Kind rather than three classes: the three share a palette, a paint helper and a
// resolution-independence rule, and splitting them would triple the boilerplate for no editing
// benefit. Kind is on the asset, so a designer can still drop a second backdrop in if they want one.
//
// ---------------------------------------------------------------------------------------------
// bOpaqueLines — READ THIS BEFORE "FIXING" THE LOOK
// ---------------------------------------------------------------------------------------------
// AHUD::DrawLine DISCARDS ALPHA. FCanvasLineItem defaults to SE_BLEND_Opaque, so every alpha the
// Canvas title screen passes to DrawLine — the grid's edge falloff, the bezel's 0.22, the two soft
// passes inside DrawGlowLine — is thrown away and the line draws at full strength. That is why the
// shipped screen has a hard-edged grid and a chunky wordmark rather than the dissolve and the glow
// the code says it wants.
//
// UMG has no such limitation. So this widget can render either:
//   bOpaqueLines = true  (DEFAULT) — force every stroke to alpha 1, reproducing the shipped screen
//                                    pixel for pixel. Spec v17 §0: a reorganisation must not change
//                                    how anything looks.
//   bOpaqueLines = false           — honour the alpha, which is the INTENT the Canvas comments
//                                    describe ("the grid dissolves into the dark instead of ending
//                                    in a hard line"). Spec v17 §4 asks for the intent to be
//                                    available; it does not ask for it to be switched on behind the
//                                    user's back.
// It is a checkbox on the widget in WBP_TitleMenu. Flip it, press Play, decide for yourself.

#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "Styling/SlateColor.h"

#include "TraceMenuCanvasArt.generated.h"

class STraceMenuCanvasArt;

/** Which piece of the title screen's stroked art this widget paints. */
UENUM(BlueprintType)
enum class ETraceMenuArtKind : uint8
{
	/** Void fill, the horizon glow stack, the perspective grid and the scanline overlay. */
	Backdrop UMETA(DisplayName = "Backdrop and grid floor"),
	/** The inset frame with corner ticks. Drawn ABOVE the footer strip, exactly as Canvas does. */
	Bezel    UMETA(DisplayName = "Bezel frame"),
	/** The drawn pointer. The OS cursor is invisible in captures and looks wrong here anyway. */
	Cursor   UMETA(DisplayName = "Cursor")
};

/**
 * Every number the three art kinds need, in one struct so the Slate widget can be handed a copy
 * rather than reaching back into the UObject during paint.
 *
 * Not a USTRUCT: nothing reflects it, and the UWidget's own UPROPERTYs are what a designer edits.
 */
struct FTraceMenuArtParams
{
	ETraceMenuArtKind Kind = ETraceMenuArtKind::Backdrop;
	bool bOpaqueLines = true;

	FLinearColor VoidColor = FLinearColor(0.006f, 0.011f, 0.022f, 1.f);
	FLinearColor LineColor = FLinearColor(0.16f, 0.88f, 1.00f, 1.f);
	FLinearColor GlowColor = FLinearColor(0.04f, 0.34f, 0.48f, 1.f);

	float HorizonFraction = 0.66f;
	int32 GlowBands = 26;
	float GlowHeightFraction = 0.26f;
	float GlowStrength = 0.10f;

	int32 RailCount = 16;
	float RailSpreadFraction = 0.34f;
	int32 RungCount = 22;
	float RungScrollSpeed = 0.18f;
	float RungExponent = 2.6f;

	float ScanlineStep = 4.f;
	float ScanlineAlpha = 0.16f;

	float BezelInsetXFraction = 0.028f;
	float BezelInsetYFraction = 0.038f;
	float BezelTickLength = 26.f;
	float BezelFrameAlpha = 0.22f;
	float BezelTickAlpha = 0.85f;

	float CursorSize = 9.f;
	float CursorAlpha = 0.9f;

	/** Seconds, for the scrolling rungs. Pushed in per frame; the widget owns no clock. */
	float TimeSeconds = 0.f;

	/** Cursor position as a 0..1 fraction of the widget, so nothing here depends on the DPI scale. */
	FVector2D CursorFraction = FVector2D(0.5, 0.5);
	bool bCursorVisible = false;
};

/**
 * Paints one of ETraceMenuArtKind. Sizes itself to whatever slot it is given and derives every
 * pixel from that, so it is resolution independent without knowing anything about the viewport.
 */
UCLASS(meta = (DisplayName = "Trace Menu Art"))
class TRACE_API UTraceMenuCanvasArt : public UWidget
{
	GENERATED_BODY()

public:
	UTraceMenuCanvasArt();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art")
	ETraceMenuArtKind Kind = ETraceMenuArtKind::Backdrop;

	/** See the long note at the top of this file. True reproduces the shipped Canvas screen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art")
	bool bOpaqueLines = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Palette")
	FLinearColor VoidColor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Palette")
	FLinearColor LineColor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Palette")
	FLinearColor GlowColor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HorizonFraction = 0.66f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	int32 GlowBands = 26;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	float GlowHeightFraction = 0.26f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	float GlowStrength = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	int32 RailCount = 16;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	float RailSpreadFraction = 0.34f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	int32 RungCount = 22;

	/** Rungs per second, towards the viewer. Zero freezes the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	float RungScrollSpeed = 0.18f;

	/** Squared-ish spacing is what sells the perspective; 1.0 reads as a ladder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	float RungExponent = 2.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	float ScanlineStep = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Grid")
	float ScanlineAlpha = 0.16f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Bezel")
	float BezelInsetXFraction = 0.028f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Bezel")
	float BezelInsetYFraction = 0.038f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Bezel")
	float BezelTickLength = 26.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Bezel")
	float BezelFrameAlpha = 0.22f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Bezel")
	float BezelTickAlpha = 0.85f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Cursor")
	float CursorSize = 9.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Menu Art|Cursor")
	float CursorAlpha = 0.9f;

	/** Drives the scrolling rungs. Pushed in from C++ every frame; this widget owns no clock. */
	void SetAnimationTime(float InSeconds);

	/** @param InFraction 0..1 across the widget. Fractions, so nothing here depends on DPI scale. */
	void SetCursor(bool bInVisible, const FVector2D& InFraction);

	//~ Begin UWidget interface
	virtual void SynchronizeProperties() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif
	//~ End UWidget interface

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	/** Everything the Slate side needs, rebuilt from the UPROPERTYs above. */
	FTraceMenuArtParams BuildParams() const;

	TSharedPtr<STraceMenuCanvasArt> ArtWidget;

	float AnimationTime = 0.f;
	bool bCursorVisible = false;
	FVector2D CursorFraction = FVector2D(0.5, 0.5);
};
