// Trace — see TraceText.h. THE ONLY FILE IN THE PROJECT THAT READS TraceFontAtlasMetrics.h.
//
// That is not a style rule, it is the mechanism behind spec A1's "ONE source": the Canvas blitter
// and the Slate leaf both call LayoutString() below, so neither of them owns an opinion about where
// a letter goes and neither of them can drift from the other.

#include "UI/Text/TraceText.h"

#include "Engine/Texture2D.h"
#include "EngineFontServices.h"      // FEngineFontServices — measuring the Lato fallback
#include "Fonts/FontMeasure.h"       // FSlateFontMeasure
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/GCObject.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR
#include "TextureCompiler.h"         // FTextureCompilingManager — see WaitForTextureBuild() below
#endif

#include "Trace.h"                                // LogTraceGame
#include "UI/Text/TraceFontAtlasMetrics.h"        // <<< the one include of the generated table
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"    // the mandated fallback face

// Named after the file rather than anonymous: two anonymous namespaces merged into one unity
// translation unit is MSVC C2084 on Windows only. Scripts/check-jumbo-build-collisions.py gates on it.
namespace TraceTextFile
{
	namespace Metrics = TraceFontAtlasMetrics;

	/**
	 * Runtime override for the atlas, so the degraded path can be reached without relaunching.
	 *
	 * -TraceNoFontAtlas (spec A1, mandatory) is the launch-time form and is checked separately; this
	 * is the same switch for a running game, which is what makes a red arm / green arm pair possible
	 * inside ONE process and one screenshot session. Both are ORed: either one forces Lato.
	 */
	static TAutoConsoleVariable<int32> CVarAtlas(
		TEXT("Trace.Text.Atlas"),
		1,
		TEXT("1 = draw menu type from the Sofachrome glyph atlas (default).\n")
		TEXT("0 = force the Lato fallback, exactly as -TraceNoFontAtlas does at launch.\n")
		TEXT("Use Trace.Text.Report to see which is live."),
		ECVF_Default);

	/**
	 * Keeps the sheet alive.
	 *
	 * A TStrongObjectPtr in a static would work until module shutdown ordering bit somebody; an
	 * FGCObject is the pattern the engine itself uses for exactly this and it releases cleanly.
	 */
	class FAtlasRef : public FGCObject
	{
	public:
		TObjectPtr<UTexture2D> Texture;

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			Collector.AddReferencedObject(Texture);
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("TraceText::Atlas");
		}
	};

	static FAtlasRef& Refs()
	{
		static FAtlasRef Instance;
		return Instance;
	}

	static bool bResolved = false;
	/** The texture loaded AND passed the staleness guard. Does not account for the two overrides. */
	static bool bAtlasUsable = false;
	static FString FailureReason;

	static bool ForcedOffAtLaunch()
	{
		// Parsed once. FParse::Param on every draw would be a string scan per glyph run.
		static const bool bForced = FParse::Param(FCommandLine::Get(), TEXT("TraceNoFontAtlas"));
		return bForced;
	}

	/**
	 * Waits for the texture's platform data, in an editor build only.
	 *
	 * *** THIS IS NOT BELT AND BRACES, IT IS LOAD-BEARING, AND IT COST THIS PASS A FALSE FAILURE. ***
	 * In an editor build a texture is COMPILED ASYNCHRONOUSLY after it loads. Until that finishes the
	 * UTexture2D stands in for itself with the engine's 32x32 placeholder, and UTexture2D::GetSizeX()
	 * quietly answers for the placeholder instead — the engine's own comment on that branch reads
	 * "any calculation that actually uses this is garbage" (Texture2D.cpp, under IsDefaultTexture()).
	 *
	 * Measured here: the first run of the staleness guard read 32x32 for a sheet that is 2048x1024 on
	 * disk and correct in the asset, declared the metrics out of step, and fell back to Lato. The
	 * atlas was never the problem. Two things follow, and the second matters more:
	 *
	 *   1. The guard must not compare against a placeholder, or it fails textures that are fine.
	 *   2. NOTHING must draw from a placeholder either. Every UV rect below is computed for a
	 *      2048x1024 sheet; sampled against a 32x32 stand-in, every glyph on the title screen would
	 *      be a smear of the wrong texture. Waiting is the fix for both.
	 *
	 * Blocking is the right call precisely here: this is a menu texture, resolved once, on the frame
	 * the menu first needs it. A cooked build has its mips inline and compiles nothing, so this whole
	 * function compiles out.
	 */
	static void WaitForTextureBuild(UTexture2D* Texture)
	{
#if WITH_EDITOR
		if (Texture != nullptr && Texture->IsDefaultTexture())
		{
			FTextureCompilingManager::Get().FinishCompilation({ Texture });
		}
#endif
	}

	/**
	 * Loads the sheet and checks it against the numbers baked into the generated header.
	 *
	 * THE STALENESS GUARD is the point of this function. Somebody re-runs
	 * Scripts/generate_font_atlas.py at a different em or with a different charset, re-imports the
	 * texture, and forgets to re-run Scripts/import_font_atlas.py — now the cell table describes a
	 * sheet that no longer exists and every UV rect lands on the wrong letter. That failure draws
	 * *something*, so it survives review. Refusing the atlas outright turns it into a legible log
	 * line and a menu in Lato.
	 */
	static void Resolve()
	{
		if (bResolved)
		{
			return;
		}
		bResolved = true;

		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, Metrics::TextureAsset);
		if (Texture == nullptr)
		{
			FailureReason = FString::Printf(
				TEXT("%s did not load. Run Scripts/import-font-atlas.sh to import it."),
				Metrics::TextureAsset);
			UE_LOG(LogTraceGame, Error, TEXT("[Text] %s Falling back to %s."),
				*FailureReason, TraceMenuArtStyle::MenuFontSourceFile);
			return;
		}

		// Before ANY question is asked of the texture's dimensions. See the comment on this function.
		WaitForTextureBuild(Texture);

		const int32 Width = static_cast<int32>(Texture->GetSizeX());
		const int32 Height = static_cast<int32>(Texture->GetSizeY());
		if (Width != Metrics::AtlasWidth || Height != Metrics::AtlasHeight)
		{
			FailureReason = FString::Printf(
				TEXT("%s is %dx%d but TraceFontAtlasMetrics.h describes a %dx%d sheet — the metrics ")
				TEXT("and the texture are out of step, so every glyph would sample the wrong cell. ")
				TEXT("Re-run Scripts/import-font-atlas.sh (it does both halves together)."),
				Metrics::TextureAsset, Width, Height, Metrics::AtlasWidth, Metrics::AtlasHeight);
			UE_LOG(LogTraceGame, Error, TEXT("[Text] %s"), *FailureReason);
			return;
		}

		Refs().Texture = Texture;
		bAtlasUsable = true;

		UE_LOG(LogTraceGame, Display,
			TEXT("[Text] Sofachrome is live: %s (%dx%d, %d glyphs, em %.0fpx, cap %.0fpx). Menu type ")
			TEXT("is drawn one quad per glyph — no UFont and no FSlateFontInfo is involved."),
			Metrics::TextureAsset, Width, Height, Metrics::NumGlyphs, Metrics::EmSize, Metrics::CapHeight);
	}

	/** Atlas pixels -> screen pixels. */
	static float ScaleFor(float Size)
	{
		return FMath::Max(0.f, Size) / Metrics::EmSize;
	}

	static const Metrics::FCell* FindCell(TCHAR Char)
	{
		const int32 Code = static_cast<int32>(Char);
		if (Code < Metrics::FirstCode || Code > Metrics::LastCode)
		{
			return nullptr;
		}
		return &Metrics::Cells[Code - Metrics::FirstCode];
	}

	/** The cell an unmapped character advances by, so foreign text still occupies sane space. */
	static const Metrics::FCell& SpaceCell()
	{
		return Metrics::Cells[TEXT(' ') - Metrics::FirstCode];
	}

	// ---------------------------------------------------------------------------------------------
	// The fallback face. Everything below has to keep working when the atlas stands down, and
	// MEASUREMENT most of all — a caller that centres a row on a width measured in one face and then
	// draws it in another gets a menu that is subtly, unattributably off.
	// ---------------------------------------------------------------------------------------------

	static TSharedPtr<FSlateFontMeasure> Measurer()
	{
		if (!FEngineFontServices::IsInitialized())
		{
			return nullptr;
		}
		return FEngineFontServices::Get().GetFontMeasure();
	}

	static float FallbackLineWidth(const FString& Line, float Size)
	{
		const TSharedPtr<FSlateFontMeasure> Measure = Measurer();
		if (!Measure.IsValid())
		{
			// No font services yet (very early, or a commandlet). A rough monospace guess beats
			// returning 0, which would stack every centred label on top of itself.
			return Line.Len() * Size * 0.5f;
		}
		return static_cast<float>(
			Measure->Measure(Line, TraceMenuArtStyle::MenuFont(Size), 1.f).X);
	}

	static void SplitLines(const FString& Text, TArray<FString>& OutLines)
	{
		Text.ParseIntoArray(OutLines, TEXT("\n"), /*InCullEmpty=*/false);
		if (OutLines.Num() == 0)
		{
			OutLines.Add(FString());
		}
	}

	static float AtlasLineWidth(const FString& Line, float Size, float Tracking)
	{
		const float Scale = ScaleFor(Size);
		float Width = 0.f;
		int32 Drawn = 0;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			const Metrics::FCell* Cell = FindCell(Line[Index]);
			Width += (Cell != nullptr ? Cell->USize : SpaceCell().USize) * Scale;
			++Drawn;
		}
		// Tracking is between glyphs, not after the last one — otherwise a centred string drifts
		// left by half a track and nobody can find the half pixel.
		return Width + (Drawn > 1 ? Tracking * (Drawn - 1) : 0.f);
	}
}

// =================================================================================================
// Which face is live
// =================================================================================================

bool TraceText::IsAtlasActive()
{
	TraceTextFile::Resolve();
	return TraceTextFile::bAtlasUsable
		&& !TraceTextFile::ForcedOffAtLaunch()
		&& TraceTextFile::CVarAtlas.GetValueOnAnyThread() != 0;
}

FString TraceText::FaceName()
{
	if (IsAtlasActive())
	{
		// The name of the typeface, not of the asset: this string is what a screenshot is captioned
		// with, and "T_FontAtlas" would not tell a reviewer which letterforms they are looking at.
		FString Source(TraceFontAtlasMetrics::SourceFont);
		Source.RemoveFromEnd(TEXT(".otf"));
		Source.RemoveFromEnd(TEXT(".ttf"));
		Source.RemoveFromEnd(TEXT(" Rg"));
		return Source;
	}

	FString Fallback(TraceMenuArtStyle::MenuFontSourceFile);
	Fallback.RemoveFromEnd(TEXT(".ttf"));
	Fallback.RemoveFromEnd(TEXT(".otf"));
	Fallback.RemoveFromEnd(TEXT("-Regular"));
	return Fallback;
}

UTexture2D* TraceText::AtlasTexture()
{
	TraceTextFile::Resolve();
	return TraceTextFile::Refs().Texture;
}

FString TraceText::DescribeFace()
{
	TraceTextFile::Resolve();

	if (IsAtlasActive())
	{
		return FString::Printf(
			TEXT("SOFACHROME — the artist's face, from the glyph atlas %s (%dx%d, %d glyphs, em %.0f, ")
			TEXT("cap %.0f). Every letter is one textured quad drawn by Source/Trace/UI/Text; no UFont ")
			TEXT("and no FSlateFontInfo is involved, which is the whole point — an offline UFont ")
			TEXT("cannot drive UMG. Force the fallback with -TraceNoFontAtlas or `Trace.Text.Atlas 0`."),
			TraceFontAtlasMetrics::TextureAsset, TraceFontAtlasMetrics::AtlasWidth,
			TraceFontAtlasMetrics::AtlasHeight, TraceFontAtlasMetrics::NumGlyphs,
			TraceFontAtlasMetrics::EmSize, TraceFontAtlasMetrics::CapHeight);
	}

	FString Why;
	if (!TraceTextFile::bAtlasUsable)
	{
		Why = FString::Printf(TEXT("the atlas did not resolve — %s"), *TraceTextFile::FailureReason);
	}
	else if (TraceTextFile::ForcedOffAtLaunch())
	{
		Why = TEXT("-TraceNoFontAtlas was passed on the command line, so this is the degraded path ON PURPOSE");
	}
	else
	{
		Why = TEXT("`Trace.Text.Atlas 0` was set, so this is the degraded path ON PURPOSE");
	}

	return FString::Printf(TEXT("%s — THE FALLBACK, not Sofachrome. Reason: %s. Details of the ")
		TEXT("fallback face: %s"),
		*FaceName(), *Why, *TraceMenuArtStyle::DescribeMenuFont());
}

// =================================================================================================
// Metrics
// =================================================================================================

float TraceText::LineHeight(float Size)
{
	// Deliberately NOT branched on IsAtlasActive(). The two faces' line heights agree to under a
	// pixel at every size this menu uses (Lato's is 1.20 em, the atlas's is 116/96 = 1.208), and a
	// caller whose row pitch changed when the font fell back would be a worse bug than the 0.6% here.
	return TraceFontAtlasMetrics::LineHeight * TraceTextFile::ScaleFor(Size);
}

float TraceText::Ascent(float Size)
{
	return TraceFontAtlasMetrics::Ascent * TraceTextFile::ScaleFor(Size);
}

float TraceText::CapHeight(float Size)
{
	return TraceFontAtlasMetrics::CapHeight * TraceTextFile::ScaleFor(Size);
}

float TraceText::SizeForCapHeight(float InCapHeight)
{
	return FMath::Max(0.f, InCapHeight) * (TraceFontAtlasMetrics::EmSize / TraceFontAtlasMetrics::CapHeight);
}

float TraceText::MeasureWidth(const FString& Text, const FStyle& Style)
{
	TArray<FString> Lines;
	TraceTextFile::SplitLines(Text, Lines);

	const bool bAtlas = IsAtlasActive();
	float Widest = 0.f;
	for (const FString& Line : Lines)
	{
		const float Width = bAtlas
			? TraceTextFile::AtlasLineWidth(Line, Style.Size, Style.Tracking)
			: TraceTextFile::FallbackLineWidth(Line, Style.Size);
		Widest = FMath::Max(Widest, Width);
	}
	return Widest;
}

float TraceText::MeasureWidth(const FString& Text, float Size)
{
	return MeasureWidth(Text, FStyle(Size));
}

FVector2f TraceText::Measure(const FString& Text, const FStyle& Style)
{
	TArray<FString> Lines;
	TraceTextFile::SplitLines(Text, Lines);
	return FVector2f(MeasureWidth(Text, Style), LineHeight(Style.Size) * Lines.Num());
}

// =================================================================================================
// Alignment and layout
// =================================================================================================

FVector2f TraceText::AlignOffset(const FVector2f& BlockSize, const FStyle& Style, float Size)
{
	float X = 0.f;
	switch (Style.HAlign)
	{
	case EHAlign::Center: X = -BlockSize.X * 0.5f; break;
	case EHAlign::Right:  X = -BlockSize.X;        break;
	default:                                       break;
	}

	float Y = 0.f;
	switch (Style.VAlign)
	{
	case EVAlign::Center:   Y = -BlockSize.Y * 0.5f;  break;
	case EVAlign::Bottom:   Y = -BlockSize.Y;         break;
	case EVAlign::Baseline: Y = -Ascent(Size);        break;
	// A baked word sprite's top edge is the CAP LINE, not the line box, so live text that must sit
	// where one sat has to be lifted by the gap between them.
	case EVAlign::CapTop:   Y = -(Ascent(Size) - CapHeight(Size)); break;
	default:                                          break;
	}

	return FVector2f(X, Y);
}

bool TraceText::LayoutString(const FString& Text, const FStyle& Style, TArray<FGlyphQuad>& OutQuads)
{
	OutQuads.Reset();

	if (!IsAtlasActive() || Text.IsEmpty() || Style.Size <= 0.f)
	{
		return false;
	}

	TArray<FString> Lines;
	TraceTextFile::SplitLines(Text, Lines);

	const float Scale = TraceTextFile::ScaleFor(Style.Size);
	const float Height = LineHeight(Style.Size);
	const FVector2f Block(MeasureWidth(Text, Style), Height * Lines.Num());
	const FVector2f Origin = AlignOffset(Block, Style, Style.Size);

	constexpr float AtlasW = static_cast<float>(TraceFontAtlasMetrics::AtlasWidth);
	constexpr float AtlasH = static_cast<float>(TraceFontAtlasMetrics::AtlasHeight);

	OutQuads.Reserve(Text.Len());

	for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
	{
		const FString& Line = Lines[LineIndex];

		// Every line is aligned inside the BLOCK, so a centred two-line label centres both lines
		// rather than centring the longest and left-aligning the rest.
		float PenX = Origin.X;
		const float LineWidth = TraceTextFile::AtlasLineWidth(Line, Style.Size, Style.Tracking);
		switch (Style.HAlign)
		{
		case EHAlign::Center: PenX += (Block.X - LineWidth) * 0.5f; break;
		case EHAlign::Right:  PenX += (Block.X - LineWidth);        break;
		default:                                                    break;
		}

		const float PenY = Origin.Y + Height * LineIndex;

		for (int32 CharIndex = 0; CharIndex < Line.Len(); ++CharIndex)
		{
			const TCHAR Char = Line[CharIndex];
			const TraceFontAtlasMetrics::FCell* Cell = TraceTextFile::FindCell(Char);
			const TraceFontAtlasMetrics::FCell& Advance =
				(Cell != nullptr) ? *Cell : TraceTextFile::SpaceCell();

			// Whitespace and unmapped characters advance and draw nothing. Emitting a quad for a
			// space would be a transparent draw call per space, on every frame, for nothing.
			if (Cell != nullptr && Char != TEXT(' '))
			{
				FGlyphQuad& Quad = OutQuads.AddDefaulted_GetRef();
				Quad.Pos = FVector2f(PenX, PenY);
				Quad.Size = FVector2f(Cell->USize * Scale, Cell->VSize * Scale);
				Quad.UVMin = FVector2f(Cell->U / AtlasW, Cell->V / AtlasH);
				Quad.UVSize = FVector2f(Cell->USize / AtlasW, Cell->VSize / AtlasH);
			}

			PenX += Advance.USize * Scale + Style.Tracking;
		}
	}

	return true;
}
