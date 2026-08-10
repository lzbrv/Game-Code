// Trace — see TraceMenuCanvasArt.h.

#include "UI/Widgets/Menu/TraceMenuCanvasArt.h"

#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

#define LOCTEXT_NAMESPACE "TraceMenuCanvasArt"

// Named after the file, not anonymous: ./Scripts/build.sh gates on
// Scripts/check-jumbo-build-collisions.py because two anonymous namespaces concatenated into one
// unity translation unit is MSVC C2084, and that failure only ever shows up on Windows.
namespace TraceMenuCanvasArtFile
{
	/** The white 1x1 Slate draws solid rectangles with. Resolved once; never null in practice. */
	static const FSlateBrush* SolidBrush()
	{
		static const FSlateBrush* Cached = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
		return Cached;
	}
}

// =================================================================================================
// The Slate leaf
// =================================================================================================

/**
 * Paints one ETraceMenuArtKind into whatever rectangle it is given.
 *
 * A leaf rather than a compound widget on purpose: it has no children, it never takes a click (the
 * whole title screen is HitTestInvisible — the HUD keeps the hit testing, see
 * ATraceMenuHUD::RowAtPoint), and a leaf is the cheapest thing Slate can lay out.
 */
class STraceMenuCanvasArt : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(STraceMenuCanvasArt) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		SetCanTick(false);
	}

	void SetParams(const FTraceMenuArtParams& InParams)
	{
		Params = InParams;
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		// Zero: this widget is always given a rectangle by its slot (a full-screen canvas anchor) and
		// has no opinion about how big that should be. Returning anything else would fight the slot.
		return FVector2D::ZeroVector;
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
		if (LocalSize.X <= 1.0 || LocalSize.Y <= 1.0)
		{
			return LayerId;
		}

		const float W = static_cast<float>(LocalSize.X);
		const float H = static_cast<float>(LocalSize.Y);

		// Reference pixels -> local units. The Canvas path multiplies its constants by
		// clamp(ViewH/1080, 0.5, 2); this does the same arithmetic from the geometry it was handed,
		// which is what makes the two renderers land in the same place at any window size.
		const float Scale = FMath::Clamp(H / 1080.f, 0.5f, 2.0f);

		int32 Layer = LayerId;
		switch (Params.Kind)
		{
		case ETraceMenuArtKind::Backdrop: Layer = PaintBackdrop(AllottedGeometry, OutDrawElements, Layer, W, H, Scale); break;
		case ETraceMenuArtKind::Bezel:    Layer = PaintBezel(AllottedGeometry, OutDrawElements, Layer, W, H, Scale);    break;
		case ETraceMenuArtKind::Cursor:   Layer = PaintCursor(AllottedGeometry, OutDrawElements, Layer, W, H, Scale);   break;
		default: break;
		}

		return FMath::Max(Layer, LayerId);
	}

private:
	FTraceMenuArtParams Params;

	/** AHUD::DrawLine discards alpha; see the header. This is where that decision is enforced. */
	FLinearColor StrokeTint(const FLinearColor& InColor, float InAlpha) const
	{
		return FLinearColor(InColor.R, InColor.G, InColor.B, Params.bOpaqueLines ? 1.f : InAlpha);
	}

	void Rect(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float X, float Y, float SizeX, float SizeY, const FLinearColor& Tint) const
	{
		if (SizeX <= 0.f || SizeY <= 0.f || Tint.A <= 0.f)
		{
			return;
		}
		const FSlateBrush* Brush = TraceMenuCanvasArtFile::SolidBrush();
		if (Brush == nullptr)
		{
			return;
		}
		FSlateDrawElement::MakeBox(Elements, Layer,
			Geo.ToPaintGeometry(FVector2f(SizeX, SizeY), FSlateLayoutTransform(FVector2f(X, Y))),
			Brush, ESlateDrawEffect::None, Tint);
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

	/** The three-pass neon of ATraceMenuHUD::DrawGlowLine, alpha honoured or not per bOpaqueLines. */
	void GlowLine(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float X0, float Y0, float X1, float Y1, const FLinearColor& InColor, float Thickness) const
	{
		Line(Geo, Elements, Layer, X0, Y0, X1, Y1, StrokeTint(InColor, 0.18f), Thickness * 4.f);
		Line(Geo, Elements, Layer, X0, Y0, X1, Y1, StrokeTint(InColor, 0.45f), Thickness * 2.f);
		Line(Geo, Elements, Layer, X0, Y0, X1, Y1,
			FLinearColor(1.f, 1.f, 1.f, Params.bOpaqueLines ? 1.f : 0.92f), Thickness * 0.6f);
	}

	int32 PaintBackdrop(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Scale) const
	{
		// Opaque, and drawn first: the menu map is empty, and whatever the renderer decides to put
		// behind an empty map is not something the title screen should be at the mercy of.
		Rect(Geo, Elements, Layer, 0.f, 0.f, W, H, Params.VoidColor);

		// A cold glow sitting on the horizon, faked as a stack of strips. UMG could do this with a
		// gradient brush; it is kept as strips so the two renderers stay comparable.
		const float HorizonY = H * Params.HorizonFraction;
		const int32 Bands = FMath::Max(1, Params.GlowBands);
		const float GlowH = H * Params.GlowHeightFraction;
		const float BandH = GlowH / static_cast<float>(Bands);
		for (int32 Index = 0; Index < Bands; ++Index)
		{
			const float T = (Bands > 1) ? (static_cast<float>(Index) / static_cast<float>(Bands - 1)) : 1.f;
			const float BandAlpha = Params.GlowStrength * T * T;
			Rect(Geo, Elements, Layer, 0.f, HorizonY - GlowH + Index * BandH, W, BandH + 1.f,
				FLinearColor(Params.GlowColor.R, Params.GlowColor.G, Params.GlowColor.B, BandAlpha));
		}
		++Layer;

		const float CX = W * 0.5f;
		const float Thin = FMath::Max(1.f, 1.f * Scale);

		// Rails converging on the vanishing point. Alpha falls off towards the edges so the grid
		// dissolves into the dark instead of ending in a hard line — see bOpaqueLines in the header
		// for why the shipped Canvas screen does NOT show that falloff.
		const int32 Rails = FMath::Max(1, Params.RailCount);
		const float RailSpread = W * Params.RailSpreadFraction;
		for (int32 Index = -Rails; Index <= Rails; ++Index)
		{
			const float T = static_cast<float>(Index) / static_cast<float>(Rails);
			const float BottomX = CX + T * RailSpread * Rails * 0.14f;
			const float RailAlpha = 0.34f * (1.f - FMath::Abs(T) * 0.75f);
			Line(Geo, Elements, Layer, CX, HorizonY, BottomX, H, StrokeTint(Params.LineColor, RailAlpha), Thin);
		}

		// Rungs, scrolling towards the viewer.
		const int32 Rungs = FMath::Max(1, Params.RungCount);
		const float Scroll = FMath::Fmod(Params.TimeSeconds * Params.RungScrollSpeed, 1.f);
		for (int32 Index = 0; Index < Rungs; ++Index)
		{
			const float T = (static_cast<float>(Index) + Scroll) / static_cast<float>(Rungs);
			if (T <= 0.f || T > 1.f)
			{
				continue;
			}
			const float Y = HorizonY + (H - HorizonY) * FMath::Pow(T, Params.RungExponent);
			Line(Geo, Elements, Layer, 0.f, Y, W, Y, StrokeTint(Params.LineColor, 0.32f * T), Thin);
		}

		// The horizon itself, bright, because every straight edge in the frame should point at it.
		GlowLine(Geo, Elements, Layer, 0.f, HorizonY, W, HorizonY, Params.LineColor, FMath::Max(1.f, 1.2f * Scale));
		++Layer;

		// Scanlines over the whole frame. Cheap, and the single strongest cue that this is a screen
		// inside a machine rather than a slide.
		const float Step = FMath::Max(2.f, Params.ScanlineStep * Scale);
		for (float Y = 0.f; Y < H; Y += Step)
		{
			Rect(Geo, Elements, Layer, 0.f, Y, W, 1.f, FLinearColor(0.f, 0.f, 0.f, Params.ScanlineAlpha));
		}

		return Layer + 1;
	}

	int32 PaintBezel(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Scale) const
	{
		const float InsetX = W * Params.BezelInsetXFraction;
		const float InsetY = H * Params.BezelInsetYFraction;
		const float Left = InsetX;
		const float Right = W - InsetX;
		const float Top = InsetY;
		const float Bottom = H - InsetY;

		const float Thin = FMath::Max(1.f, 1.f * Scale);
		const FLinearColor Frame = StrokeTint(Params.LineColor, Params.BezelFrameAlpha);

		Line(Geo, Elements, Layer, Left, Top, Right, Top, Frame, Thin);
		Line(Geo, Elements, Layer, Left, Bottom, Right, Bottom, Frame, Thin);
		Line(Geo, Elements, Layer, Left, Top, Left, Bottom, Frame, Thin);
		Line(Geo, Elements, Layer, Right, Top, Right, Bottom, Frame, Thin);

		// Corner ticks: short, bright, and the only place the frame asserts itself.
		const float Tick = Params.BezelTickLength * Scale;
		const float TickT = FMath::Max(1.f, 2.f * Scale);
		const FLinearColor Bright = StrokeTint(Params.LineColor, Params.BezelTickAlpha);

		Line(Geo, Elements, Layer, Left, Top, Left + Tick, Top, Bright, TickT);
		Line(Geo, Elements, Layer, Left, Top, Left, Top + Tick, Bright, TickT);
		Line(Geo, Elements, Layer, Right - Tick, Top, Right, Top, Bright, TickT);
		Line(Geo, Elements, Layer, Right, Top, Right, Top + Tick, Bright, TickT);
		Line(Geo, Elements, Layer, Left, Bottom - Tick, Left, Bottom, Bright, TickT);
		Line(Geo, Elements, Layer, Left, Bottom, Left + Tick, Bottom, Bright, TickT);
		Line(Geo, Elements, Layer, Right, Bottom - Tick, Right, Bottom, Bright, TickT);
		Line(Geo, Elements, Layer, Right - Tick, Bottom, Right, Bottom, Bright, TickT);

		return Layer + 1;
	}

	int32 PaintCursor(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Scale) const
	{
		if (!Params.bCursorVisible)
		{
			return Layer;
		}

		const float PX = static_cast<float>(Params.CursorFraction.X) * W;
		const float PY = static_cast<float>(Params.CursorFraction.Y) * H;
		const float S = Params.CursorSize * Scale;
		const float T = FMath::Max(1.f, 1.5f * Scale);
		const FLinearColor Tint = StrokeTint(Params.LineColor, Params.CursorAlpha);

		Line(Geo, Elements, Layer, PX - S, PY, PX - S * 0.35f, PY, Tint, T);
		Line(Geo, Elements, Layer, PX + S * 0.35f, PY, PX + S, PY, Tint, T);
		Line(Geo, Elements, Layer, PX, PY - S, PX, PY - S * 0.35f, Tint, T);
		Line(Geo, Elements, Layer, PX, PY + S * 0.35f, PX, PY + S, Tint, T);

		return Layer + 1;
	}
};

// =================================================================================================
// The UWidget
// =================================================================================================

UTraceMenuCanvasArt::UTraceMenuCanvasArt()
{
	// The whole title screen is pass-through: the HUD owns hit testing (ATraceMenuHUD::RowAtPoint)
	// and the player controller owns the key bindings, exactly as they did before this widget
	// existed. A widget that took focus here would break the single-press contract of spec v15 §4.
	SetVisibilityInternal(ESlateVisibility::HitTestInvisible);

	VoidColor = FLinearColor(0.006f, 0.011f, 0.022f, 1.f);
	LineColor = FLinearColor(0.16f, 0.88f, 1.00f, 1.f);
	GlowColor = FLinearColor(0.04f, 0.34f, 0.48f, 1.f);
}

FTraceMenuArtParams UTraceMenuCanvasArt::BuildParams() const
{
	FTraceMenuArtParams Out;
	Out.Kind = Kind;
	Out.bOpaqueLines = bOpaqueLines;
	Out.VoidColor = VoidColor;
	Out.LineColor = LineColor;
	Out.GlowColor = GlowColor;
	Out.HorizonFraction = HorizonFraction;
	Out.GlowBands = GlowBands;
	Out.GlowHeightFraction = GlowHeightFraction;
	Out.GlowStrength = GlowStrength;
	Out.RailCount = RailCount;
	Out.RailSpreadFraction = RailSpreadFraction;
	Out.RungCount = RungCount;
	Out.RungScrollSpeed = RungScrollSpeed;
	Out.RungExponent = RungExponent;
	Out.ScanlineStep = ScanlineStep;
	Out.ScanlineAlpha = ScanlineAlpha;
	Out.BezelInsetXFraction = BezelInsetXFraction;
	Out.BezelInsetYFraction = BezelInsetYFraction;
	Out.BezelTickLength = BezelTickLength;
	Out.BezelFrameAlpha = BezelFrameAlpha;
	Out.BezelTickAlpha = BezelTickAlpha;
	Out.CursorSize = CursorSize;
	Out.CursorAlpha = CursorAlpha;
	Out.TimeSeconds = AnimationTime;
	Out.CursorFraction = CursorFraction;
	Out.bCursorVisible = bCursorVisible;
	return Out;
}

TSharedRef<SWidget> UTraceMenuCanvasArt::RebuildWidget()
{
	ArtWidget = SNew(STraceMenuCanvasArt);
	ArtWidget->SetParams(BuildParams());
	return ArtWidget.ToSharedRef();
}

void UTraceMenuCanvasArt::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (ArtWidget.IsValid())
	{
		ArtWidget->SetParams(BuildParams());
	}
}

void UTraceMenuCanvasArt::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	ArtWidget.Reset();
}

void UTraceMenuCanvasArt::SetAnimationTime(float InSeconds)
{
	AnimationTime = InSeconds;
	if (ArtWidget.IsValid())
	{
		ArtWidget->SetParams(BuildParams());
	}
}

void UTraceMenuCanvasArt::SetCursor(bool bInVisible, const FVector2D& InFraction)
{
	bCursorVisible = bInVisible;
	CursorFraction = InFraction;
	if (ArtWidget.IsValid())
	{
		ArtWidget->SetParams(BuildParams());
	}
}

#if WITH_EDITOR
const FText UTraceMenuCanvasArt::GetPaletteCategory()
{
	return LOCTEXT("TracePalette", "Trace");
}
#endif

#undef LOCTEXT_NAMESPACE
