// Trace — Sofachrome in UMG (spec v22 §A1). THE ONE THAT COULD NOT BE DONE WITH A FONT.
//
// =================================================================================================
// WHY THIS WIDGET EXISTS
// =================================================================================================
// Every other route into UMG text goes through FSlateFontInfo, and FSlateFontInfo cannot carry this
// typeface: an offline (bitmap) UFont returns nullptr from GetCompositeFont, and Slate then draws in
// its LAST-RESORT face without saying so. A UTextBlock pointed at the atlas would look like it
// worked and would be the wrong letterforms. Measured during spec v20 — see TraceText.h.
//
// So this widget's Slate leaf emits one FSlateDrawElement::MakeBox per glyph against the atlas
// texture and that glyph's UV rect. No font object is ever constructed, so there is nothing for
// Slate to substitute. That is the entire point of the class.
//
// =================================================================================================
// HOW TO USE IT — for the other agents' screens
// =================================================================================================
// In C++, building a widget tree (this is the usual case in this project, which keeps logic in C++
// UUserWidget subclasses):
//
//     #include "UI/Text/TraceAtlasTextWidget.h"
//
//     UTraceAtlasText* Label = WidgetTree->ConstructWidget<UTraceAtlasText>(
//         UTraceAtlasText::StaticClass(), TEXT("Label"));
//     Label->SetText(TEXT("PRACTICE"));
//     Label->SetSize(34.f);                                  // as FSlateFontInfo::Size
//     Label->SetColor(TraceMenuArtStyle::WordDefault);
//     Slot->SetContent(Label);
//
// It reports a real desired size (the measured string), so it drops into a horizontal/vertical box
// or an auto-sized slot exactly like a UTextBlock. In a slot bigger than the text, HAlign/VAlign
// decide where it sits — both default to Center, because that is what a menu row wants.
//
// *** REPLACING A BAKED WORD SPRITE (T_MenuWord_Play / _Settings / _Keybind / _Key). *** Those
// sprites are placed by CAP HEIGHT, not by font size or line box. To land live type exactly where
// one sat, size it from the sprite's cap height and align its cap line:
//
//     Label->SetSize(TraceText::SizeForCapHeight(SpriteCapHeightInPixels));
//     Label->VerticalAlignment = TraceText::EVAlign::CapTop;   // top edge = the cap line
//
// Everything degrades on its own: with the atlas off the leaf draws the same string, at the same
// size and position, in Lato via MakeText. A caller never branches on which face is live.

#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "Widgets/SLeafWidget.h"

#include "UI/Text/TraceText.h"

#include "TraceAtlasTextWidget.generated.h"

/** Everything the Slate leaf needs. Plain C++; the UWidget's UPROPERTYs are what a designer edits. */
struct FTraceAtlasTextParams
{
	FString Text;
	TraceText::FStyle Style;

	/** Where the string sits inside a slot LARGER than it. Irrelevant in an auto-sized slot. */
	TraceText::EHAlign SlotHAlign = TraceText::EHAlign::Center;
	TraceText::EVAlign SlotVAlign = TraceText::EVAlign::Center;
};

/**
 * The leaf. Declared here rather than hidden in the .cpp so `Trace.Text.Preview` can put one
 * straight into the viewport's Slate overlay — which proves the UMG path with the same paint code
 * the title screen runs, rather than with a second implementation that could pass while it fails.
 */
class TRACE_API STraceAtlasText : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(STraceAtlasText) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetParams(const FTraceAtlasTextParams& InParams);

	//~ Begin SWidget interface
	virtual FVector2D ComputeDesiredSize(float) const override;
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
		const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	//~ End SWidget interface

private:
	FTraceAtlasTextParams Params;
};

/**
 * A run of text in the artist's typeface, drawn glyph by glyph from the Sofachrome atlas.
 *
 * Falls back to TraceMenuArtStyle::MenuFont (Lato) when the atlas is unavailable or has been forced
 * off with -TraceNoFontAtlas; `Trace.Text.Report` names whichever is live.
 */
UCLASS(meta = (DisplayName = "Trace Atlas Text"))
class TRACE_API UTraceAtlasText : public UWidget
{
	GENERATED_BODY()

public:
	UTraceAtlasText();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Atlas Text")
	FString Text;

	/** The em, in pixels. Means exactly what FSlateFontInfo::Size means, so existing numbers carry over. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Atlas Text")
	float Size = 24.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Atlas Text")
	FLinearColor Color = FLinearColor::White;

	/** Extra pixels between glyphs at Size. Sofachrome is already wide; negative is legal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Atlas Text")
	float Tracking = 0.f;

	/**
	 * Which cut of Sofachrome. LIGHT unless you say otherwise — the owner wants the game light and
	 * bold reserved for the character names on the select screen. Changing it moves no glyph: both
	 * weights do NOT share advances (two different font files), so a label that flips weight
	 * does change its desired size and its slot will re-arrange around it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Atlas Text")
	ETraceTextWeight Weight = ETraceTextWeight::Light;

	/** Only meaningful in a slot bigger than the text; an auto-sized slot fits it exactly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Atlas Text")
	TEnumAsByte<EHorizontalAlignment> HorizontalAlignment = HAlign_Center;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace Atlas Text")
	TEnumAsByte<EVerticalAlignment> VerticalAlignment = VAlign_Center;

	UFUNCTION(BlueprintCallable, Category = "Trace Atlas Text")
	void SetText(const FString& InText);

	UFUNCTION(BlueprintCallable, Category = "Trace Atlas Text")
	void SetSize(float InSize);

	UFUNCTION(BlueprintCallable, Category = "Trace Atlas Text")
	void SetColor(const FLinearColor& InColor);

	/** Bold is for the character names on the select screen; everything else stays Light. */
	UFUNCTION(BlueprintCallable, Category = "Trace Atlas Text")
	void SetWeight(ETraceTextWeight InWeight);

	/**
	 * Sets the weight from a string — for a caller that carries one by NAME, out of a data asset,
	 * a config row or a designer-facing field. "Light" / "Bold", case-insensitive; anything else
	 * leaves the weight alone and warns, rather than silently picking one.
	 */
	UFUNCTION(BlueprintCallable, Category = "Trace Atlas Text")
	void SetWeightByName(const FString& InWeightName);

	/** Width this widget would occupy right now, in local units. Correct in the fallback too. */
	UFUNCTION(BlueprintCallable, Category = "Trace Atlas Text")
	float MeasureWidth() const;

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
	FTraceAtlasTextParams BuildParams() const;

	TSharedPtr<STraceAtlasText> TextWidget;
};
