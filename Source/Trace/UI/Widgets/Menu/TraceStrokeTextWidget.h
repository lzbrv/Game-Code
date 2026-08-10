// Trace — the TRACE wordmark, as a UMG widget (spec v17 §4).
//
// The wordmark is not typeset, it is DRAWN: five letters built out of line segments in a unit box.
// The reason has not changed since the Canvas version — the engine's built-in fonts are bitmaps and
// go to mush somewhere around 3x, and this title wants a cap height of 15% of the screen. Segments
// cost nothing and stay sharp at any resolution.
//
// The comment this replaced justified it with "there is no .uasset budget for a real typeface
// (contract: no assets)". THAT CONTRACT IS RETIRED — spec v17 §0 — and the project now commits
// .uassets freely. The stroke font stays because it is better at this size, not because it is
// forced. If somebody imports a real display face, this widget is what they delete.
//
// The RULE under the wordmark is drawn here rather than placed as a separate widget, because its
// width is defined as "exactly as wide as the wordmark" and that is the only reason the block below
// it looks deliberate rather than dropped in. Two widgets could not keep that promise without one
// of them reading the other's geometry.

#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"

#include "TraceStrokeTextWidget.generated.h"

class STraceStrokeText;

/** Everything the Slate leaf needs. Plain C++; the UWidget's UPROPERTYs are what a designer edits. */
struct FTraceStrokeTextParams
{
	FString Text = TEXT("TRACE");
	FLinearColor Color = FLinearColor(0.16f, 0.88f, 1.00f, 1.f);

	/** Cap height and top edge, as fractions of the widget's own height. */
	float CapHeightFraction = 0.155f;
	float TopFraction = 0.135f;

	/** Stroke weight as a fraction of the cap height. */
	float ThicknessFraction = 0.055f;

	bool bDrawRule = true;
	/** Gap between the wordmark's baseline and the rule, in reference (1080-tall) pixels. */
	float RuleOffset = 28.f;
	float RuleThickness = 2.f;

	/** See UTraceMenuCanvasArt's header: AHUD::DrawLine discards alpha and this reproduces that. */
	bool bOpaqueLines = true;
};

/**
 * Draws stroke-font text centred horizontally in its slot, with an optional rule under it.
 *
 * Everything is a fraction of the widget's own height, so this is resolution independent without
 * knowing anything about the viewport — give it a full-screen canvas slot and it lands exactly
 * where ATraceMenuHUD::DrawWordmark puts it.
 */
UCLASS(meta = (DisplayName = "Trace Stroke Text"))
class TRACE_API UTraceStrokeText : public UWidget
{
	GENERATED_BODY()

public:
	UTraceStrokeText();

	/** Only T, R, A, C and E have glyphs. Anything else advances and draws nothing — by design. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Stroke Text")
	FString Text = TEXT("TRACE");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Stroke Text")
	FLinearColor Color;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Stroke Text")
	float CapHeightFraction = 0.155f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Stroke Text")
	float TopFraction = 0.135f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Stroke Text")
	float ThicknessFraction = 0.055f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Stroke Text")
	bool bDrawRule = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Stroke Text")
	float RuleOffset = 28.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Stroke Text")
	float RuleThickness = 2.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Stroke Text")
	bool bOpaqueLines = true;

	/** Fades the whole mark. The travel overlay draws its wordmark at 0.55. */
	void SetStrokeColor(const FLinearColor& InColor);

	/** Width the current settings would occupy, in the widget's own local units. */
	static float MeasureWidth(const FString& InText, float InCapHeight);

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
	FTraceStrokeTextParams BuildParams() const;

	TSharedPtr<STraceStrokeText> StrokeWidget;
};
