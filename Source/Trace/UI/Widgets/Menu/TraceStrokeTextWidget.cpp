// Trace — see TraceStrokeTextWidget.h.

#include "UI/Widgets/Menu/TraceStrokeTextWidget.h"

#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"

#define LOCTEXT_NAMESPACE "TraceStrokeText"

// Named after the file for the unity/jumbo build; see Scripts/check-jumbo-build-collisions.py.
namespace TraceStrokeTextWidgetFile
{
	struct FSeg { float X0, Y0, X1, Y1; };

	/** Glyph box aspect and letter tracking, both as multiples of cap height. */
	static constexpr float GlyphWidth = 0.62f;
	static constexpr float Tracking   = 0.30f;

	static const FSeg SegsT[] = { {0.f,0.f, 1.f,0.f}, {0.5f,0.f, 0.5f,1.f} };
	static const FSeg SegsR[] = {
		{0.f,1.f, 0.f,0.f}, {0.f,0.f, 0.74f,0.f}, {0.74f,0.f, 0.9f,0.14f},
		{0.9f,0.14f, 0.9f,0.4f}, {0.9f,0.4f, 0.74f,0.54f}, {0.74f,0.54f, 0.f,0.54f},
		{0.46f,0.54f, 0.95f,1.f} };
	static const FSeg SegsA[] = { {0.f,1.f, 0.5f,0.f}, {0.5f,0.f, 1.f,1.f}, {0.17f,0.66f, 0.83f,0.66f} };
	static const FSeg SegsC[] = {
		{1.f,0.f, 0.16f,0.f}, {0.16f,0.f, 0.f,0.16f}, {0.f,0.16f, 0.f,0.84f},
		{0.f,0.84f, 0.16f,1.f}, {0.16f,1.f, 1.f,1.f} };
	static const FSeg SegsE[] = { {1.f,0.f, 0.f,0.f}, {0.f,0.f, 0.f,1.f}, {0.f,1.f, 1.f,1.f}, {0.f,0.5f, 0.78f,0.5f} };

	/** Null segments + zero count is a legal glyph: it advances and draws nothing (space, unknown). */
	struct FGlyph { const FSeg* Segs; int32 Num; };

	static FGlyph Find(TCHAR Char)
	{
		switch (Char)
		{
		case TEXT('T'): return { SegsT, UE_ARRAY_COUNT(SegsT) };
		case TEXT('R'): return { SegsR, UE_ARRAY_COUNT(SegsR) };
		case TEXT('A'): return { SegsA, UE_ARRAY_COUNT(SegsA) };
		case TEXT('C'): return { SegsC, UE_ARRAY_COUNT(SegsC) };
		case TEXT('E'): return { SegsE, UE_ARRAY_COUNT(SegsE) };
		default:        return { nullptr, 0 };
		}
	}
}

// =================================================================================================
// The Slate leaf
// =================================================================================================

class STraceStrokeText : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(STraceStrokeText) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		SetCanTick(false);
	}

	void SetParams(const FTraceStrokeTextParams& InParams)
	{
		Params = InParams;
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		return FVector2D::ZeroVector;
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
		if (LocalSize.X <= 1.0 || LocalSize.Y <= 1.0 || Params.Text.IsEmpty())
		{
			return LayerId;
		}

		const float W = static_cast<float>(LocalSize.X);
		const float H = static_cast<float>(LocalSize.Y);
		const float Scale = FMath::Clamp(H / 1080.f, 0.5f, 2.0f);

		const float CapHeight = H * Params.CapHeightFraction;
		const float TopY = H * Params.TopFraction;
		const float Thickness = FMath::Max(2.f, CapHeight * Params.ThicknessFraction);
		const float CX = W * 0.5f;

		DrawStrokeText(AllottedGeometry, OutDrawElements, LayerId, Params.Text, Params.Color, CX, TopY, CapHeight, Thickness);

		if (Params.bDrawRule)
		{
			const float MarkW = UTraceStrokeText::MeasureWidth(Params.Text, CapHeight);
			const float RuleY = TopY + CapHeight + (Params.RuleOffset * Scale);
			GlowLine(AllottedGeometry, OutDrawElements, LayerId,
				CX - MarkW * 0.5f, RuleY, CX + MarkW * 0.5f, RuleY,
				Params.Color, FMath::Max(1.f, Params.RuleThickness * Scale));
		}

		return LayerId + 1;
	}

private:
	FTraceStrokeTextParams Params;

	FLinearColor StrokeTint(const FLinearColor& InColor, float InAlpha) const
	{
		// AHUD::DrawLine discards alpha; see UTraceMenuCanvasArt's header for the whole argument.
		// The wordmark's colour alpha is always honoured — the travel overlay fades it to 0.55 and
		// that is a value, not a workaround. Only the GLOW's internal alphas are affected.
		return FLinearColor(InColor.R, InColor.G, InColor.B,
			Params.bOpaqueLines ? InColor.A : (InColor.A * InAlpha));
	}

	void Line(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float X0, float Y0, float X1, float Y1, const FLinearColor& Tint, float Thickness) const
	{
		if (Tint.A <= 0.f)
		{
			return;
		}
		TArray<FVector2f> Points;
		Points.Reserve(2);
		Points.Add(FVector2f(X0, Y0));
		Points.Add(FVector2f(X1, Y1));
		FSlateDrawElement::MakeLines(Elements, Layer, Geo.ToPaintGeometry(), MoveTemp(Points),
			ESlateDrawEffect::None, Tint, /*bAntialias=*/true, FMath::Max(1.f, Thickness));
	}

	void GlowLine(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float X0, float Y0, float X1, float Y1, const FLinearColor& InColor, float Thickness) const
	{
		Line(Geo, Elements, Layer, X0, Y0, X1, Y1, StrokeTint(InColor, 0.18f), Thickness * 4.f);
		Line(Geo, Elements, Layer, X0, Y0, X1, Y1, StrokeTint(InColor, 0.45f), Thickness * 2.f);
		Line(Geo, Elements, Layer, X0, Y0, X1, Y1,
			FLinearColor(1.f, 1.f, 1.f, Params.bOpaqueLines ? InColor.A : (InColor.A * 0.92f)),
			Thickness * 0.6f);
	}

	void DrawStrokeText(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		const FString& InText, const FLinearColor& InColor, float CenterX, float Y, float Height, float Thickness) const
	{
		const float GlyphW = TraceStrokeTextWidgetFile::GlyphWidth * Height;
		const float Advance = GlyphW + TraceStrokeTextWidgetFile::Tracking * Height;

		float PenX = CenterX - UTraceStrokeText::MeasureWidth(InText, Height) * 0.5f;

		for (int32 Index = 0; Index < InText.Len(); ++Index)
		{
			const TraceStrokeTextWidgetFile::FGlyph Glyph = TraceStrokeTextWidgetFile::Find(InText[Index]);
			for (int32 SegIndex = 0; SegIndex < Glyph.Num; ++SegIndex)
			{
				const TraceStrokeTextWidgetFile::FSeg& Seg = Glyph.Segs[SegIndex];
				GlowLine(Geo, Elements, Layer,
					PenX + Seg.X0 * GlyphW, Y + Seg.Y0 * Height,
					PenX + Seg.X1 * GlyphW, Y + Seg.Y1 * Height,
					InColor, Thickness);
			}
			PenX += Advance;
		}
	}
};

// =================================================================================================
// The UWidget
// =================================================================================================

UTraceStrokeText::UTraceStrokeText()
{
	SetVisibilityInternal(ESlateVisibility::HitTestInvisible);
	Color = FLinearColor(0.16f, 0.88f, 1.00f, 1.f);
}

float UTraceStrokeText::MeasureWidth(const FString& InText, float InCapHeight)
{
	const int32 Num = InText.Len();
	if (Num <= 0)
	{
		return 0.f;
	}
	return Num * (TraceStrokeTextWidgetFile::GlyphWidth * InCapHeight)
		+ (Num - 1) * (TraceStrokeTextWidgetFile::Tracking * InCapHeight);
}

FTraceStrokeTextParams UTraceStrokeText::BuildParams() const
{
	FTraceStrokeTextParams Out;
	Out.Text = Text;
	Out.Color = Color;
	Out.CapHeightFraction = CapHeightFraction;
	Out.TopFraction = TopFraction;
	Out.ThicknessFraction = ThicknessFraction;
	Out.bDrawRule = bDrawRule;
	Out.RuleOffset = RuleOffset;
	Out.RuleThickness = RuleThickness;
	Out.bOpaqueLines = bOpaqueLines;
	return Out;
}

TSharedRef<SWidget> UTraceStrokeText::RebuildWidget()
{
	StrokeWidget = SNew(STraceStrokeText);
	StrokeWidget->SetParams(BuildParams());
	return StrokeWidget.ToSharedRef();
}

void UTraceStrokeText::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (StrokeWidget.IsValid())
	{
		StrokeWidget->SetParams(BuildParams());
	}
}

void UTraceStrokeText::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	StrokeWidget.Reset();
}

void UTraceStrokeText::SetStrokeColor(const FLinearColor& InColor)
{
	Color = InColor;
	if (StrokeWidget.IsValid())
	{
		StrokeWidget->SetParams(BuildParams());
	}
}

#if WITH_EDITOR
const FText UTraceStrokeText::GetPaletteCategory()
{
	return LOCTEXT("TracePalette", "Trace");
}
#endif

#undef LOCTEXT_NAMESPACE
