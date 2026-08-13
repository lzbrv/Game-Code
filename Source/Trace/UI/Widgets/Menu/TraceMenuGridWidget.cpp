// Trace — see TraceMenuGridWidget.h.

#include "UI/Widgets/Menu/TraceMenuGridWidget.h"

#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

#include "UI/Widgets/Menu/TraceMenuArtStyle.h"
#include "UI/Widgets/Menu/TraceMenuPalette.h"

#define LOCTEXT_NAMESPACE "TraceMenuGridWidget"

// Named after the file, not anonymous: ./Scripts/build.sh gates on
// Scripts/check-jumbo-build-collisions.py because two anonymous namespaces concatenated into one
// unity translation unit is MSVC C2084, and that failure only ever shows up on Windows.
namespace TraceMenuGridWidgetFile
{
	/**
	 * Draw it at all. Live — the next frame picks it up, because ApplyView re-asks every frame.
	 *
	 * -TraceNoMenuGrid beats this, for the same reason -TraceNoMenuUMG beats Trace.UI.UseUMG: a cvar
	 * set with -ExecCmds arrives after the first title screen has already been built, so the arm the
	 * contrast measurement is taken against has to be sayable up front.
	 */
	static int32 GMenuGrid = 1;
	static FAutoConsoleVariableRef CVarMenuGrid(
		TEXT("Trace.UI.MenuGrid"),
		GMenuGrid,
		TEXT("Draw the scrolling grid floor behind the UMG title screen. 1 (default) or 0.\n")
		TEXT("-TraceNoMenuGrid on the command line beats this."),
		ECVF_Default);

	/** Master multiplier on every alpha the grid uses. 0 is indistinguishable from off. */
	static float GMenuGridIntensity = 1.f;
	static FAutoConsoleVariableRef CVarMenuGridIntensity(
		TEXT("Trace.UI.MenuGridIntensity"),
		GMenuGridIntensity,
		TEXT("Multiplier on every alpha in the title screen's grid floor. 1 is the shipped level."),
		ECVF_Default);

	/** The white 1x1 Slate draws solid rectangles with. Resolved once; never null in practice. */
	static const FSlateBrush* SolidBrush()
	{
		static const FSlateBrush* Cached = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
		return Cached;
	}

	/**
	 * A colour's own hue at full value: divide through by its largest channel.
	 *
	 * This is the whole palette derivation. The artist's plate is their hue at ~9% value (it is a
	 * button, it is meant to be dark); a line drawn ON black has to be at full value or it is not a
	 * line. Dividing rather than picking a new colour is what guarantees the floor and the buttons
	 * cannot drift apart — change PlateFill and this follows it.
	 */
	static FLinearColor AtFullValue(const FLinearColor& InColor)
	{
		const float Peak = FMath::Max3(InColor.R, InColor.G, InColor.B);
		if (Peak <= UE_SMALL_NUMBER)
		{
			return FLinearColor::White;
		}
		return FLinearColor(InColor.R / Peak, InColor.G / Peak, InColor.B / Peak, 1.f);
	}

	/** The artist's button plate, RGB(29,41,81), with its brightness put back. sRGB(108,142,255). */
	static const FLinearColor& SteelLine()
	{
		static const FLinearColor Cached = AtFullValue(TraceMenuArtStyle::PlateFill);
		return Cached;
	}

	/**
	 * The artist's amber, RGB(116,58,0), the same way. sRGB(255,135,0).
	 *
	 * Measured against the wordmark's own lifted glow (TraceTitleMenuWidget's WordmarkGlow,
	 * RGB(255,140,40)) that is 5 bytes of green and 40 of blue apart — the same colour to the eye,
	 * arrived at from the constant that is actually named rather than by copying a literal.
	 */
	static const FLinearColor& HorizonAmber()
	{
		static const FLinearColor Cached = AtFullValue(TraceMenuArtStyle::Amber);
		return Cached;
	}

	/**
	 * Deterministic 0..1 noise. Integer avalanche (Chris Wellons' lowbias32), no floating-point
	 * trigonometry: the same seed gives the same number on every platform and in every build, which
	 * is what lets a screenshot diff be evidence of MOVEMENT rather than evidence of a random number
	 * generator.
	 */
	static float Hash01(uint32 InSeed)
	{
		InSeed ^= InSeed >> 16;
		InSeed *= 0x7feb352dU;
		InSeed ^= InSeed >> 15;
		InSeed *= 0x846ca68bU;
		InSeed ^= InSeed >> 16;
		return static_cast<float>(InSeed & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
	}
}

// =================================================================================================
// The Slate leaf
// =================================================================================================

/**
 * Paints the grid floor into whatever rectangle it is given.
 *
 * A leaf rather than a compound widget on purpose: it has no children, it never takes a click (the
 * whole title screen is HitTestInvisible — ATraceMenuHUD keeps the hit testing, see
 * ATraceMenuHUD::RowAtPoint), and a leaf is the cheapest thing Slate can lay out.
 */
class STraceMenuGrid : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(STraceMenuGrid) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		// No tick: this widget owns no clock. UTraceTitleMenuWidget::ApplyView pushes ATraceMenuHUD's
		// time in once per drawn frame, which is the same clock the Canvas grid scrolls on.
		SetCanTick(false);
	}

	void SetParams(const FTraceMenuGridParams& InParams)
	{
		// Repainting is the POINT of this widget — a static grid is a failed grid — so the paint
		// invalidation is not optional and not left to whoever happens to be invalidating this frame.
		// Guarded on the time actually changing so a paused screen does not churn.
		const bool bMoved = !FMath::IsNearlyEqual(Params.TimeSeconds, InParams.TimeSeconds, UE_KINDA_SMALL_NUMBER)
			|| !FMath::IsNearlyEqual(Params.Intensity, InParams.Intensity, UE_KINDA_SMALL_NUMBER);
		Params = InParams;
		if (bMoved)
		{
			Invalidate(EInvalidateWidgetReason::Paint);
		}
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
		const float Scale = FMath::Clamp(H / TraceMenuStyle::ReferenceHeight, 0.5f, 2.0f);
		const float Master = FMath::Max(0.f, Params.Intensity);
		if (Master <= 0.f)
		{
			return LayerId;
		}

		int32 Layer = LayerId;
		PaintHaze(AllottedGeometry, OutDrawElements, Layer, W, H, Master);
		PaintRails(AllottedGeometry, OutDrawElements, Layer, W, H, Scale, Master);
		PaintRungs(AllottedGeometry, OutDrawElements, Layer, W, H, Scale, Master);
		PaintDust(AllottedGeometry, OutDrawElements, Layer, W, H, Scale, Master);
		++Layer;

		PaintHorizon(AllottedGeometry, OutDrawElements, Layer, W, H, Scale, Master);
		++Layer;

		PaintVignette(AllottedGeometry, OutDrawElements, Layer, W, H, Master);
		PaintScanlines(AllottedGeometry, OutDrawElements, Layer, W, H, Scale, Master);

		return FMath::Max(Layer + 1, LayerId);
	}

private:
	FTraceMenuGridParams Params;

	/** Where the floor starts, in local units. */
	float HorizonY(float H) const { return H * Params.HorizonFraction; }

	/**
	 * THE READABILITY STRUCTURE, as one number.
	 *
	 * 1 from the horizon down to NearFadeStart, then falling to NearFadeFloor by NearFadeEnd. The
	 * Canvas original does the OPPOSITE (alpha = 0.32 * T, brightest at the bottom edge), which on
	 * this screen would put the grid's strongest pixels directly under the blurb and the two footer
	 * lines — the three strings here that have nothing opaque behind them.
	 *
	 * Smoothstepped rather than linear so the floor has no visible band across it where the fade
	 * starts; a hard edge there reads as a second horizon.
	 */
	float FloorFade(float Y, float H) const
	{
		const float Start = H * Params.NearFadeStart;
		const float End = H * Params.NearFadeEnd;
		if (Y <= Start || End <= Start)
		{
			return 1.f;
		}
		const float T = FMath::Clamp((Y - Start) / (End - Start), 0.f, 1.f);
		return FMath::Lerp(1.f, Params.NearFadeFloor, T * T * (3.f - 2.f * T));
	}

	void Rect(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float X, float Y, float SizeX, float SizeY, const FLinearColor& Tint) const
	{
		if (SizeX <= 0.f || SizeY <= 0.f || Tint.A <= 0.002f)
		{
			return;
		}
		const FSlateBrush* Brush = TraceMenuGridWidgetFile::SolidBrush();
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
		if (Tint.A <= 0.002f)
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

	static FLinearColor Tinted(const FLinearColor& InColor, float InAlpha)
	{
		return FLinearColor(InColor.R, InColor.G, InColor.B, FMath::Clamp(InAlpha, 0.f, 1.f));
	}

	// ---- The layers, bottom to top -----------------------------------------------------------------

	/**
	 * The haze sitting ON the horizon, faked as a stack of strips because a Slate box brush has no
	 * gradient. Amber, quadratic, and it stops below the tagline — see the header.
	 */
	void PaintHaze(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Master) const
	{
		const float Horizon = HorizonY(H);
		const int32 Bands = 26;
		const float HazeH = H * Params.HazeHeightFraction;
		const float BandH = HazeH / static_cast<float>(Bands);
		const FLinearColor& Amber = TraceMenuGridWidgetFile::HorizonAmber();

		for (int32 Index = 0; Index < Bands; ++Index)
		{
			const float T = static_cast<float>(Index) / static_cast<float>(Bands - 1);
			// Quadratic, so the band is already at ~0 alpha where the address chip and the tagline are
			// and only asserts itself in the last few strips before the horizon line.
			Rect(Geo, Elements, Layer, 0.f, Horizon - HazeH + Index * BandH, W, BandH + 1.f,
				Tinted(Amber, Params.HazeStrength * T * T * Master));
		}
	}

	/**
	 * Rails converging on the vanishing point, with a per-rail alpha jitter so the fan is uneven.
	 *
	 * The jitter seed is the rail's index and nothing else: rails do not move, so a rail that
	 * flickered would read as a rendering fault rather than as wear.
	 *
	 * *** DRAWN IN SEGMENTS, AND THAT IS NOT DECORATION. *** A rail runs the whole way from the
	 * horizon to the bottom edge, so it crosses the near-fade — the structure that keeps the grid's
	 * light AT the horizon and off the blurb and the two footer lines. One MakeLines call can carry
	 * only one alpha, so a whole-rail line has to pick a single point on that curve to sample: at the
	 * top the rail stays bright right down into the footer, and at the bottom (which is what the
	 * first draft of this file did) every rail comes out at 0.085 * 0.14 = 0.012 and the fan is
	 * invisible in the frame it is supposed to be the structure of. Segments let the fade actually
	 * apply along the rail, which is the only way both halves of the requirement hold at once.
	 */
	void PaintRails(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Scale, float Master) const
	{
		const float Horizon = HorizonY(H);
		const float CX = W * 0.5f;
		const float Thin = FMath::Max(1.f, 1.f * Scale);
		const int32 Rails = FMath::Max(1, Params.RailCount);
		const float RailSpread = W * Params.RailSpreadFraction;
		const FLinearColor& Steel = TraceMenuGridWidgetFile::SteelLine();

		// Eight is where the banding stops being visible against this fade curve; more is just more
		// draw elements. Checked by eye against 4 (visible steps) and 16 (identical to 8).
		const int32 Segments = 8;

		for (int32 Index = -Rails; Index <= Rails; ++Index)
		{
			const float T = static_cast<float>(Index) / static_cast<float>(Rails);
			const float BottomX = CX + T * RailSpread * Rails * 0.14f;

			// The Canvas original's edge falloff, kept: the grid dissolves into the dark instead of
			// ending in a hard line. (On Canvas it never actually shows, because AHUD::DrawLine
			// discards alpha — see TraceMenuCanvasArt.h. Here it does.)
			const float Falloff = 1.f - FMath::Abs(T) * 0.75f;
			const float Jitter = 0.55f + 0.75f * TraceMenuGridWidgetFile::Hash01(static_cast<uint32>(Index + 512) * 2654435761u);
			const float Level = Params.RailAlpha * Falloff * Jitter * Master;
			if (Level <= 0.002f)
			{
				continue;
			}

			for (int32 Seg = 0; Seg < Segments; ++Seg)
			{
				const float U0 = static_cast<float>(Seg) / static_cast<float>(Segments);
				const float U1 = static_cast<float>(Seg + 1) / static_cast<float>(Segments);
				const float Y0 = FMath::Lerp(Horizon, H, U0);
				const float Y1 = FMath::Lerp(Horizon, H, U1);

				Line(Geo, Elements, Layer,
					FMath::Lerp(CX, BottomX, U0), Y0,
					FMath::Lerp(CX, BottomX, U1), Y1,
					Tinted(Steel, Level * FloorFade((Y0 + Y1) * 0.5f, H)), Thin);
			}
		}
	}

	/**
	 * The rungs, scrolling toward the viewer, each broken into independently jittered cells.
	 *
	 * THE EXPONENT IS THE WHOLE ILLUSION, and the comment is the Canvas original's: "evenly spaced
	 * rungs read as a ladder, squared spacing reads as a floor running away from you."
	 *
	 * THE GRUNGE SEED IS THE RUNG'S TRAVELLING IDENTITY, not its slot in this loop. As Scroll wraps
	 * past 1 every rung inherits its neighbour's slot, so seeding on Index alone would nail the
	 * broken cells to the screen and the wear would sit still while the floor moved under it. Adding
	 * the wrap count gives each rung one identity for the whole trip from the horizon to the bottom
	 * edge, so its dropouts travel with it.
	 */
	void PaintRungs(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Scale, float Master) const
	{
		const float Horizon = HorizonY(H);
		const float Thin = FMath::Max(1.f, 1.f * Scale);
		const int32 Rungs = FMath::Max(1, Params.RungCount);
		const int32 Cells = FMath::Max(1, Params.RungCells);
		const FLinearColor& Steel = TraceMenuGridWidgetFile::SteelLine();

		const float Travel = Params.TimeSeconds * Params.RungScrollSpeed;
		const float Scroll = FMath::Frac(Travel);
		const int32 Wrap = FMath::FloorToInt(Travel);

		const float CellW = W / static_cast<float>(Cells);

		for (int32 Index = 0; Index < Rungs; ++Index)
		{
			const float T = (static_cast<float>(Index) + Scroll) / static_cast<float>(Rungs);
			if (T <= 0.f || T > 1.f)
			{
				continue;
			}

			const float Y = Horizon + (H - Horizon) * FMath::Pow(T, Params.RungExponent);

			// Near the horizon the rungs pile up on top of each other; drawing every one of them at
			// full strength turns the vanishing point into a bright smear, which is the one place on
			// this screen that must stay quiet because it is level with the row plates. Ramped in over
			// the first quarter of the run and then held.
			const float Emerge = FMath::Min(1.f, T * 4.f);
			const float Base = Params.RungAlpha * Emerge * FloorFade(Y, H) * Master;
			if (Base <= 0.002f)
			{
				continue;
			}

			const uint32 RungId = static_cast<uint32>(Index + Wrap) * 2246822519u;

			for (int32 Cell = 0; Cell < Cells; ++Cell)
			{
				const uint32 Seed = RungId ^ (static_cast<uint32>(Cell) * 3266489917u);

				// A near-dropout, not a hole: the cell is knocked down to a tenth rather than skipped,
				// so the line reads as worn rather than as a rendering error with pieces missing.
				const float Dropout = (TraceMenuGridWidgetFile::Hash01(Seed) < Params.RungDropoutChance) ? 0.10f : 1.f;
				const float CellJitter = 0.55f + 0.70f * TraceMenuGridWidgetFile::Hash01(Seed ^ 0x9e3779b9u);

				// A fraction of a pixel of vertical wobble and a small gap at each end. Sub-pixel is
				// deliberate: a whole pixel would read as a broken line, a third of one reads as a
				// stroke that was not drawn by a machine.
				const float Wobble = (TraceMenuGridWidgetFile::Hash01(Seed ^ 0x85ebca6bu) - 0.5f) * 1.2f * Scale;
				const float Gap = CellW * 0.04f * TraceMenuGridWidgetFile::Hash01(Seed ^ 0xc2b2ae35u);

				const float X0 = Cell * CellW + Gap;
				const float X1 = (Cell + 1) * CellW - Gap;

				Line(Geo, Elements, Layer, X0, Y + Wobble, X1, Y + Wobble,
					Tinted(Steel, Base * Dropout * CellJitter), Thin);
			}
		}
	}

	/**
	 * Dust on the floor, on the same perspective curve as the rungs and at a different speed, so the
	 * background has something moving in it that is not a straight line.
	 *
	 * Each mote keeps one identity for its whole trip (the same wrap-count trick as the rungs) and
	 * gets its lane, its phase offset and its size from that identity, so a mote never teleports
	 * sideways mid-flight and no two motes travel in step.
	 *
	 * DETERMINISTIC, deliberately: the same second of the same clock draws the same dust on every
	 * machine. That is what lets a diff of two screenshots be evidence that the SCROLL moved rather
	 * than evidence that a random number generator was called twice.
	 */
	void PaintDust(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Scale, float Master) const
	{
		const float Horizon = HorizonY(H);
		const int32 Motes = FMath::Max(0, Params.DustCount);
		const FLinearColor& Steel = TraceMenuGridWidgetFile::SteelLine();

		for (int32 Index = 0; Index < Motes; ++Index)
		{
			const uint32 Lane = static_cast<uint32>(Index) * 2654435761u;

			// Every mote runs at its own speed, between 0.6x and 1.4x the rungs'. Uniform dust reads
			// as a texture scrolling; varied dust reads as particles in a space.
			const float Speed = Params.RungScrollSpeed * (0.6f + 0.8f * TraceMenuGridWidgetFile::Hash01(Lane ^ 0x27d4eb2fu));
			const float Phase = TraceMenuGridWidgetFile::Hash01(Lane);
			const float Travel = Params.TimeSeconds * Speed + Phase;
			const float T = FMath::Frac(Travel);
			const int32 Wrap = FMath::FloorToInt(Travel);

			const uint32 MoteId = (Lane ^ (static_cast<uint32>(Wrap) * 2246822519u));

			const float Y = Horizon + (H - Horizon) * FMath::Pow(T, Params.RungExponent);

			// Spread with depth exactly as the rails do, so a mote appears to sit ON the floor rather
			// than to float in front of it: near the horizon everything is close to the centre line.
			const float Lateral = (TraceMenuGridWidgetFile::Hash01(MoteId ^ 0x165667b1u) * 2.f - 1.f);
			const float X = W * 0.5f + Lateral * W * 0.5f * FMath::Pow(T, 0.8f);

			const float Size = FMath::Max(1.f, (0.8f + 2.2f * T) * Scale);
			const float Alpha = Params.DustAlpha * FMath::Min(1.f, T * 5.f) * FloorFade(Y, H) * Master
				* (0.4f + 0.6f * TraceMenuGridWidgetFile::Hash01(MoteId ^ 0xd3a2646cu));

			Rect(Geo, Elements, Layer, X, Y, Size, FMath::Max(1.f, Size * 0.6f), Tinted(Steel, Alpha));
		}
	}

	/**
	 * The horizon itself, and the ONLY amber on this layer: the brightest line in the background is
	 * the same colour as the brightest thing in the title.
	 *
	 * Three passes, widest and faintest first, which is ATraceMenuHUD::DrawGlowLine's shape — except
	 * that its third pass is white and this one is not. A white core would be a third colour on a
	 * screen whose whole look is two, and at this alpha it read as a fluorescent tube rather than as
	 * a worn horizon.
	 */
	void PaintHorizon(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Scale, float Master) const
	{
		const float Y = HorizonY(H);
		const float Thin = FMath::Max(1.f, 1.2f * Scale);
		const FLinearColor& Amber = TraceMenuGridWidgetFile::HorizonAmber();

		Line(Geo, Elements, Layer, 0.f, Y, W, Y, Tinted(Amber, Params.HorizonHaloAlpha * Master), Thin * 4.f);
		Line(Geo, Elements, Layer, 0.f, Y, W, Y, Tinted(Amber, Params.HorizonMidAlpha * Master), Thin * 2.f);

		// The core is broken into the same cells the rungs are, on a seed that does not move: the
		// horizon is the one line in the frame that is supposed to be still, and a horizon that
		// twinkled would drag the eye straight off the rows.
		const int32 Cells = FMath::Max(1, Params.RungCells * 3);
		const float CellW = W / static_cast<float>(Cells);
		for (int32 Cell = 0; Cell < Cells; ++Cell)
		{
			const uint32 Seed = static_cast<uint32>(Cell) * 3266489917u;
			const float Wear = 0.35f + 0.65f * TraceMenuGridWidgetFile::Hash01(Seed);
			Line(Geo, Elements, Layer, Cell * CellW, Y, (Cell + 1) * CellW, Y,
				Tinted(Amber, Params.HorizonCoreAlpha * Wear * Master), Thin * 0.7f);
		}
	}

	/**
	 * Black down both edges, so the floor dissolves into the dark instead of running off the frame.
	 *
	 * It CANNOT cost any string on this screen its contrast, and that is structural rather than
	 * lucky: every text layer is above this widget in the canvas z-order, so the only thing this can
	 * darken is the grid itself — which is the background the text is measured against. It can only
	 * help. See UTraceTitleMenuWidget::InstallGridBackground().
	 */
	void PaintVignette(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Master) const
	{
		const int32 Steps = 18;
		const float BandW = (W * Params.VignetteWidthFraction) / static_cast<float>(Steps);
		for (int32 Index = 0; Index < Steps; ++Index)
		{
			const float T = 1.f - (static_cast<float>(Index) / static_cast<float>(Steps - 1));
			const float Alpha = Params.VignetteStrength * T * T * Master;
			Rect(Geo, Elements, Layer, Index * BandW, 0.f, BandW + 1.f, H, FLinearColor(0.f, 0.f, 0.f, Alpha));
			Rect(Geo, Elements, Layer, W - (Index + 1) * BandW - 1.f, 0.f, BandW + 1.f, H,
				FLinearColor(0.f, 0.f, 0.f, Alpha));
		}
	}

	/**
	 * Scanlines, with hashed per-line alpha that reseeds a few times a second.
	 *
	 * The Canvas original draws every line at one constant alpha, which reads as a CSS overlay laid
	 * on top of a picture. Hashing the alpha per line, and moving the hash slowly, is what makes the
	 * same 270 rectangles read as a worn tube. ScanlineNoiseHz is deliberately low — 9 reseeds a
	 * second is film grain; at 60 it is television static and it fights the type.
	 *
	 * Black, so like the vignette it can only ever RAISE the contrast of anything drawn above it.
	 */
	void PaintScanlines(const FGeometry& Geo, FSlateWindowElementList& Elements, int32 Layer,
		float W, float H, float Scale, float Master) const
	{
		const float Step = FMath::Max(2.f, static_cast<float>(Params.ScanlineStep) * Scale);
		const uint32 Seed = static_cast<uint32>(FMath::FloorToInt(Params.TimeSeconds * Params.ScanlineNoiseHz));
		const float Constant = 1.f - FMath::Clamp(Params.ScanlineNoise, 0.f, 1.f);

		int32 LineIndex = 0;
		for (float Y = 0.f; Y < H; Y += Step, ++LineIndex)
		{
			const float Noise = TraceMenuGridWidgetFile::Hash01((static_cast<uint32>(LineIndex) * 2654435761u) ^ (Seed * 40503u));
			const float Alpha = Params.ScanlineAlpha * (Constant + FMath::Clamp(Params.ScanlineNoise, 0.f, 1.f) * Noise) * Master;
			Rect(Geo, Elements, Layer, 0.f, Y, W, 1.f, FLinearColor(0.f, 0.f, 0.f, Alpha));
		}
	}
};

// =================================================================================================
// The UWidget
// =================================================================================================

UTraceMenuGrid::UTraceMenuGrid()
{
	// The whole title screen is pass-through: ATraceMenuHUD owns hit testing and the player controller
	// owns the key bindings, exactly as they did before any widget existed. A widget that took input
	// here would break the single-press contract of spec v15 §4.
	SetVisibilityInternal(ESlateVisibility::HitTestInvisible);
}

bool UTraceMenuGrid::WantsGrid()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoMenuGrid")))
	{
		return false;
	}
	return TraceMenuGridWidgetFile::GMenuGrid != 0;
}

float UTraceMenuGrid::GridIntensity()
{
	return FMath::Max(0.f, TraceMenuGridWidgetFile::GMenuGridIntensity);
}

FString UTraceMenuGrid::Describe()
{
	const FTraceMenuGridParams Defaults;
	const FColor Steel = TraceMenuGridWidgetFile::SteelLine().ToFColor(true);
	const FColor Amber = TraceMenuGridWidgetFile::HorizonAmber().ToFColor(true);

	return FString::Printf(
		TEXT("steel sRGB(%d,%d,%d) from PlateFill, amber sRGB(%d,%d,%d) from Amber; ")
		TEXT("rails %.3f rungs %.3f horizon %.2f (Canvas original: 0.340 / 0.320 / pure cyan); ")
		TEXT("scroll %.2f rungs/s, near-fade %.2f->%.2f down to %.2f; on=%s intensity=%.2f"),
		Steel.R, Steel.G, Steel.B, Amber.R, Amber.G, Amber.B,
		Defaults.RailAlpha, Defaults.RungAlpha, Defaults.HorizonCoreAlpha,
		Defaults.RungScrollSpeed, Defaults.NearFadeStart, Defaults.NearFadeEnd, Defaults.NearFadeFloor,
		WantsGrid() ? TEXT("yes") : TEXT("NO"), GridIntensity());
}

FTraceMenuGridParams UTraceMenuGrid::BuildParams() const
{
	FTraceMenuGridParams Out;
	Out.TimeSeconds = TimeSeconds;
	Out.Intensity = GridIntensity();
	return Out;
}

TSharedRef<SWidget> UTraceMenuGrid::RebuildWidget()
{
	GridWidget = SNew(STraceMenuGrid);
	GridWidget->SetParams(BuildParams());
	return GridWidget.ToSharedRef();
}

void UTraceMenuGrid::SynchronizeProperties()
{
	Super::SynchronizeProperties();

	if (GridWidget.IsValid())
	{
		GridWidget->SetParams(BuildParams());
	}
}

void UTraceMenuGrid::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	GridWidget.Reset();
}

void UTraceMenuGrid::SetAnimationTime(float InSeconds)
{
	TimeSeconds = InSeconds;
	if (GridWidget.IsValid())
	{
		GridWidget->SetParams(BuildParams());
	}
}

#if WITH_EDITOR
const FText UTraceMenuGrid::GetPaletteCategory()
{
	return LOCTEXT("TracePalette", "Trace");
}
#endif

#undef LOCTEXT_NAMESPACE
