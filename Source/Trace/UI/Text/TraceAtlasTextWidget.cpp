// Trace — see TraceAtlasTextWidget.h.

#include "UI/Text/TraceAtlasTextWidget.h"

#include "Engine/Texture2D.h"
#include "Rendering/DrawElements.h"
#include "Styling/SlateBrush.h"

#include "Trace.h"                                // LogTraceGame
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"

#define LOCTEXT_NAMESPACE "TraceAtlasText"

// Named after the file for the unity/jumbo build; see Scripts/check-jumbo-build-collisions.py.
namespace TraceAtlasTextWidgetFile
{
	static TraceText::EHAlign FromSlate(EHorizontalAlignment In)
	{
		switch (In)
		{
		case HAlign_Left:   return TraceText::EHAlign::Left;
		case HAlign_Right:  return TraceText::EHAlign::Right;
		default:            return TraceText::EHAlign::Center;
		}
	}

	static TraceText::EVAlign FromSlate(EVerticalAlignment In)
	{
		switch (In)
		{
		case VAlign_Top:    return TraceText::EVAlign::Top;
		case VAlign_Bottom: return TraceText::EVAlign::Bottom;
		default:            return TraceText::EVAlign::Center;
		}
	}
}

// =================================================================================================
// The Slate leaf — the only place in this project that draws a glyph into UMG
// =================================================================================================

void STraceAtlasText::Construct(const FArguments& InArgs)
{
	SetCanTick(false);
}

void STraceAtlasText::SetParams(const FTraceAtlasTextParams& InParams)
{
	Params = InParams;
	// The string and the size decide the desired size, so layout has to be told they moved.
	Invalidate(EInvalidateWidgetReason::Layout);
}

FVector2D STraceAtlasText::ComputeDesiredSize(float) const
{
	// A real desired size is what lets this drop into a box slot where a UTextBlock was. It is also
	// measured through TraceText, so it is right in BOTH faces — a fallback that reported the atlas's
	// widths would reflow every auto-sized slot on screen the moment the texture went missing.
	const FVector2f Size = TraceText::Measure(Params.Text, Params.Style);
	return FVector2D(FMath::Max(Size.X, Params.MinWidth), Size.Y);
}

int32 STraceAtlasText::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	if (Params.Text.IsEmpty() || Params.Style.Size <= 0.f)
	{
		return LayerId;
	}

	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	const FVector2f Block = TraceText::Measure(Params.Text, Params.Style);

	// Position the block inside the slot. The Style's own alignment describes the block's origin and
	// is left alone (LayoutString consumes it); this is the separate question of where the block goes
	// when the slot is bigger than the text.
	float OriginX = 0.f;
	switch (Params.SlotHAlign)
	{
	case TraceText::EHAlign::Center: OriginX = (static_cast<float>(LocalSize.X) - Block.X) * 0.5f; break;
	case TraceText::EHAlign::Right:  OriginX = static_cast<float>(LocalSize.X) - Block.X;          break;
	default:                                                                                       break;
	}

	float OriginY = 0.f;
	switch (Params.SlotVAlign)
	{
	case TraceText::EVAlign::Center: OriginY = (static_cast<float>(LocalSize.Y) - Block.Y) * 0.5f; break;
	case TraceText::EVAlign::Bottom: OriginY = static_cast<float>(LocalSize.Y) - Block.Y;          break;
	default:                                                                                       break;
	}

	const FLinearColor Tint = Params.Style.Color * InWidgetStyle.GetColorAndOpacityTint();

	return PaintString(Params.Text, Params.Style, FVector2f(OriginX, OriginY), AllottedGeometry,
		OutDrawElements, LayerId, Tint);
}

int32 STraceAtlasText::PaintString(const FString& InText, const TraceText::FStyle& InStyle,
	const FVector2f& InOrigin, const FGeometry& InGeometry, FSlateWindowElementList& OutDrawElements,
	int32 InLayerId, const FLinearColor& InTint)
{
	if (InText.IsEmpty() || InStyle.Size <= 0.f)
	{
		return InLayerId;
	}

	// ---------------------------------------------------------------------------------------------
	// THE ATLAS PATH — one MakeBox per glyph, which is what bypasses FSlateFontInfo entirely.
	// ---------------------------------------------------------------------------------------------
	// A local array, never a shared one: this runs on the game thread for a widget and on the Slate
	// loading thread for the loading card, and TraceCanvasText's reusable array is game-thread only.
	TArray<TraceText::FGlyphQuad> Quads;
	// The sheet for THIS STYLE'S WEIGHT — see the same line in TraceCanvasText.cpp. The layout pass
	// below uses the same Style, so the cells it returns and this texture are always the same cut.
	UTexture2D* Atlas = TraceText::AtlasTexture(InStyle.Weight);

	if (Atlas != nullptr && TraceText::LayoutString(InText, InStyle, Quads))
	{
		// ONE brush, re-pointed per glyph. FSlateBoxPayload::SetBrush copies the margin, the UV
		// region and the resource proxy out of the brush at submission time and explicitly does NOT
		// keep the pointer ("Do not store the brush", DrawElementPayloads.h) — so mutating and
		// resubmitting one brush is correct, and 95 brushes would be 95 resource-handle lookups.
		FSlateBrush Brush;
		Brush.SetResourceObject(Atlas);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.Tiling = ESlateBrushTileType::NoTile;
		// Zero margin keeps this a plain quad rather than a 9-slice; a sliced glyph would stretch
		// its own middle and pull the neighbouring cell in through the gutter.
		Brush.Margin = FMargin(0.f);

		// Which sheet the brush is currently pointed at. Since UI plan WP12 a string may draw from
		// two — the style's face, and the Latin-1 fallback for a codepoint that face has no cell for
		// — so the resource is re-pointed when (and only when) the sheet actually changes. Every
		// quad already carries UVs normalised against ITS OWN sheet's dimensions.
		const UTexture2D* Bound = Atlas;

		for (const TraceText::FGlyphQuad& Quad : Quads)
		{
			UTexture2D* Sheet = TraceText::QuadTexture(Quad, InStyle.Weight);
			if (Sheet == nullptr)
			{
				continue;
			}
			if (Sheet != Bound)
			{
				Brush.SetResourceObject(Sheet);
				Bound = Sheet;
			}

			Brush.ImageSize = FVector2f(Quad.Size.X, Quad.Size.Y);
			Brush.SetUVRegion(FBox2f(
				Quad.UVMin,
				Quad.UVMin + Quad.UVSize));

			FSlateDrawElement::MakeBox(
				OutDrawElements,
				InLayerId,
				InGeometry.ToPaintGeometry(
					FVector2f(Quad.Size.X, Quad.Size.Y),
					FSlateLayoutTransform(FVector2f(InOrigin.X + Quad.Pos.X, InOrigin.Y + Quad.Pos.Y))),
				&Brush,
				ESlateDrawEffect::None,
				InTint);
		}

		return InLayerId + 1;
	}

	// ---------------------------------------------------------------------------------------------
	// THE FALLBACK — Lato, through the ordinary Slate text path. Same string, same size, same place.
	// ---------------------------------------------------------------------------------------------
	const FVector2f Block = TraceText::Measure(InText, InStyle);
	const FVector2f Offset = TraceText::AlignOffset(Block, InStyle, InStyle.Size);

	FSlateDrawElement::MakeText(
		OutDrawElements,
		InLayerId,
		InGeometry.ToPaintGeometry(
			FVector2f(Block.X, Block.Y),
			FSlateLayoutTransform(FVector2f(InOrigin.X + Offset.X, InOrigin.Y + Offset.Y))),
		InText,
		TraceMenuArtStyle::MenuFont(InStyle.Size),
		ESlateDrawEffect::None,
		InTint);

	return InLayerId + 1;
}

// =================================================================================================
// The UWidget
// =================================================================================================

UTraceAtlasText::UTraceAtlasText()
{
	bIsVariable = true;
	SetVisibilityInternal(ESlateVisibility::SelfHitTestInvisible);
}

FTraceAtlasTextParams UTraceAtlasText::BuildParams() const
{
	FTraceAtlasTextParams Out;
	Out.Text = Text;
	Out.Style.Size = Size;
	Out.Style.Color = Color;
	Out.Style.Tracking = Tracking;
	Out.Style.Weight = Weight;
	Out.Style.bTabularDigits = bTabularDigits;

	// The block is laid out from its own top-left; where that block lands inside the slot is the
	// SlotH/VAlign question, handled in OnPaint. Keeping them separate is what makes a
	// centred-in-slot label still able to be, say, cap-aligned to a sprite.
	Out.Style.HAlign = TraceText::EHAlign::Left;
	Out.Style.VAlign = TraceText::EVAlign::Top;

	Out.SlotHAlign = TraceAtlasTextWidgetFile::FromSlate(HorizontalAlignment);
	Out.SlotVAlign = TraceAtlasTextWidgetFile::FromSlate(VerticalAlignment);
	Out.MinWidth = FMath::Max(0.f, MinWidth);
	return Out;
}

TSharedRef<SWidget> UTraceAtlasText::RebuildWidget()
{
	TextWidget = SNew(STraceAtlasText);
	TextWidget->SetParams(BuildParams());
	return TextWidget.ToSharedRef();
}

void UTraceAtlasText::SynchronizeProperties()
{
	Super::SynchronizeProperties();
	if (TextWidget.IsValid())
	{
		TextWidget->SetParams(BuildParams());
	}
}

void UTraceAtlasText::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	TextWidget.Reset();
}

void UTraceAtlasText::SetText(const FString& InText)
{
	Text = InText;
	SynchronizeProperties();
}

void UTraceAtlasText::SetSize(float InSize)
{
	Size = InSize;
	SynchronizeProperties();
}

void UTraceAtlasText::SetColor(const FLinearColor& InColor)
{
	Color = InColor;
	SynchronizeProperties();
}

void UTraceAtlasText::SetTabularDigits(bool bInTabular)
{
	if (bTabularDigits != bInTabular)
	{
		bTabularDigits = bInTabular;
		SynchronizeProperties();
	}
}

void UTraceAtlasText::SetWeight(ETraceTextWeight InWeight)
{
	Weight = InWeight;
	SynchronizeProperties();
}

void UTraceAtlasText::SetWeightByName(const FString& InWeightName)
{
	bool bMatched = false;
	const ETraceTextWeight Resolved = TraceText::WeightFromName(InWeightName, Weight, &bMatched);
	if (!bMatched)
	{
		// Not silent: a misspelled weight would otherwise draw light and look exactly like a screen
		// that was never asked to be bold, which is the failure this whole module is built to avoid.
		//
		// The valid names are built from the face table rather than listed, so a fourth face cannot
		// be added and leave this message quietly recommending three.
		FString Valid;
		for (int32 Index = 0; Index < static_cast<int32>(ETraceTextWeight::Count); ++Index)
		{
			Valid += FString::Printf(TEXT("%s%s"), Valid.IsEmpty() ? TEXT("") : TEXT(", "),
				TraceText::WeightName(static_cast<ETraceTextWeight>(Index)));
		}

		UE_LOG(LogTraceGame, Warning,
			TEXT("[Text] SetWeightByName(\"%s\") on '%s' matched no weight; leaving it %s. "
				 "Valid names: %s."),
			*InWeightName, *GetName(), TraceText::WeightName(Weight), *Valid);
		return;
	}

	SetWeight(Resolved);
}

float UTraceAtlasText::MeasureWidth() const
{
	return TraceText::MeasureWidth(Text, BuildParams().Style);
}

float UTraceAtlasText::MeasureWidthOf(const FString& InText) const
{
	TraceText::FStyle MeasureStyle(Size, Color, Weight);
	MeasureStyle.Tracking = Tracking;
	MeasureStyle.bTabularDigits = bTabularDigits;
	return TraceText::MeasureWidth(InText, MeasureStyle);
}

void UTraceAtlasText::SetMinWidth(float InMinWidth, EHorizontalAlignment InAlign)
{
	const float Wanted = FMath::Max(0.f, InMinWidth);
	if (FMath::IsNearlyEqual(MinWidth, Wanted, 0.01f) && HorizontalAlignment == InAlign)
	{
		return;
	}
	MinWidth = Wanted;
	HorizontalAlignment = InAlign;
	SynchronizeProperties();
}

#if WITH_EDITOR
const FText UTraceAtlasText::GetPaletteCategory()
{
	return LOCTEXT("Trace", "Trace");
}
#endif

#undef LOCTEXT_NAMESPACE
