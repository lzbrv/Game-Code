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

	// The weight enum and the generated face table are two halves of one index. If somebody adds a
	// weight to Scripts/import_font_atlas.py's WEIGHTS table and not to ETraceTextWeight (or the
	// reverse), every lookup below would be off by one and the menu would draw in the wrong cut with
	// nothing logged. Break the build instead.
	static_assert(static_cast<int32>(ETraceTextWeight::Count) == Metrics::NumWeights,
		"ETraceTextWeight and TraceFontAtlasMetrics::Faces must list the same weights in the same "
		"order. Add it to Scripts/import_font_atlas.py's WEIGHTS table and to TraceTextWeight.h, "
		"then re-run Scripts/import-font-atlas.sh.");
	static_assert(static_cast<int32>(ETraceTextWeight::Light) == Metrics::DefaultWeight,
		"Light must be the default weight — the owner's instruction is that the game is light and "
		"bold is the exception, so a caller that says nothing has to get light.");

	static constexpr int32 NumWeights = Metrics::NumWeights;

	static constexpr int32 WeightIndex(ETraceTextWeight Weight)
	{
		const int32 Index = static_cast<int32>(Weight);
		return (Index >= 0 && Index < NumWeights) ? Index : Metrics::DefaultWeight;
	}

	/** One weight's runtime state: its sheet, and whether it survived the staleness guard. */
	struct FWeightState
	{
		bool bUsable = false;
		FString FailureReason;
	};

	static FWeightState WeightStates[NumWeights];

	/**
	 * Keeps the sheets alive — one per weight.
	 *
	 * A TStrongObjectPtr in a static would work until module shutdown ordering bit somebody; an
	 * FGCObject is the pattern the engine itself uses for exactly this and it releases cleanly.
	 */
	class FAtlasRef : public FGCObject
	{
	public:
		TObjectPtr<UTexture2D> Textures[NumWeights];

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			for (TObjectPtr<UTexture2D>& Texture : Textures)
			{
				Collector.AddReferencedObject(Texture);
			}
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
	/** The DEFAULT weight's texture loaded AND passed the guard. Ignores the two overrides. */
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
	/** Loads and guards ONE weight's sheet. Returns true when that weight can draw its own ink. */
	static bool ResolveWeight(int32 Index)
	{
		const Metrics::FFace& Face = Metrics::Face(Index);
		FWeightState& State = WeightStates[Index];

		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, Face.TextureAsset);
		if (Texture == nullptr)
		{
			State.FailureReason = FString::Printf(
				TEXT("%s (the %s weight) did not load. Run Scripts/import-font-atlas.sh to import it."),
				Face.TextureAsset, Face.Name);
			return false;
		}

		// Before ANY question is asked of the texture's dimensions. See the comment on this function.
		WaitForTextureBuild(Texture);

		const int32 Width = static_cast<int32>(Texture->GetSizeX());
		const int32 Height = static_cast<int32>(Texture->GetSizeY());
		if (Width != Face.AtlasWidth || Height != Face.AtlasHeight)
		{
			State.FailureReason = FString::Printf(
				TEXT("%s (the %s weight) is %dx%d but TraceFontAtlasMetrics.h describes a %dx%d sheet ")
				TEXT("— the metrics and the texture are out of step, so every glyph would sample the ")
				TEXT("wrong cell. Re-run Scripts/import-font-atlas.sh (it does both halves together)."),
				Face.TextureAsset, Face.Name, Width, Height, Face.AtlasWidth, Face.AtlasHeight);
			return false;
		}

		Refs().Textures[Index] = Texture;
		State.bUsable = true;
		return true;
	}

	static void Resolve()
	{
		if (bResolved)
		{
			return;
		}
		bResolved = true;

		// EVERY weight is resolved here, not lazily on first bold draw. The character-select screen
		// is the first thing that asks for bold and it asks during a draw pass; a synchronous texture
		// load and an editor-build shader/texture compile on that frame is a visible hitch, and the
		// failure log would land in the middle of gameplay rather than at menu load.
		for (int32 Index = 0; Index < NumWeights; ++Index)
		{
			ResolveWeight(Index);
		}

		bAtlasUsable = WeightStates[Metrics::DefaultWeight].bUsable;
		FailureReason = WeightStates[Metrics::DefaultWeight].FailureReason;

		if (!bAtlasUsable)
		{
			UE_LOG(LogTraceGame, Error, TEXT("[Text] %s Falling back to %s."),
				*FailureReason, TraceMenuArtStyle::MenuFontSourceFile);
			return;
		}

		const Metrics::FFace& Default = Metrics::Face(Metrics::DefaultWeight);
		UE_LOG(LogTraceGame, Display,
			TEXT("[Text] The glyph atlas is live. Default face %s = %s: %s (%dx%d, %d glyphs, ")
			TEXT("em %.0fpx, cap %.0fpx). Type is drawn one quad per glyph — no UFont and no ")
			TEXT("FSlateFontInfo is involved."),
			Default.Name, Default.Source, Default.TextureAsset, Default.AtlasWidth,
			Default.AtlasHeight, Metrics::NumGlyphs, Metrics::EmSize, Default.CapHeight);

		// One line per face, naming the FILE each one is rasterised from. This is the line that
		// answers "is the HUD really in Erbaum" in a log, and its screenshot counterpart is the
		// per-weight caption in Trace.Text.Preview.
		for (int32 Index = 0; Index < NumWeights; ++Index)
		{
			const Metrics::FFace& Face = Metrics::Face(Index);
			UE_LOG(LogTraceGame, Display,
				TEXT("[Text]   %-5s -> %-30s %s  (cap %.0f, ascent %.0f/descent %.0f)"),
				Face.Name, Face.Source,
				WeightStates[Index].bUsable ? TEXT("own sheet") : TEXT("SUBSTITUTED"),
				Face.CapHeight, Face.Ascent, Face.Descent);
		}

		// A weight that failed while the default one worked is NOT a fallback to Lato — it draws in
		// the default weight, which is still Sofachrome. That is the right degradation (the owner
		// wants light everywhere; bold is the exception), but it is invisible on screen, so it has to
		// be loud in the log or a missing bold sheet reads as "the emphasis was never wired up".
		for (int32 Index = 0; Index < NumWeights; ++Index)
		{
			if (Index != Metrics::DefaultWeight && !WeightStates[Index].bUsable)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Text] The %s weight is NOT available, so anything asking for it draws in %s ")
					TEXT("instead — same typeface, wrong emphasis, and nothing on screen will say so. %s"),
					Metrics::Face(Index).Name, Default.Name, *WeightStates[Index].FailureReason);
			}
		}
	}

	/**
	 * The weight that will actually be DRAWN when a caller asks for @p Weight.
	 *
	 * One place decides substitution, so the metrics, the layout and both renderers cannot disagree
	 * about which sheet a string is in — a widget measuring bold while the blitter drew light would
	 * mis-size every row it centred.
	 */
	static int32 EffectiveIndex(ETraceTextWeight Weight)
	{
		const int32 Index = WeightIndex(Weight);

		// Deliberately does NOT force Resolve(). CapHeight() and friends are pure metric queries that
		// used to answer from constexpr data, and making them load a texture would drag a synchronous
		// load (and, in an editor build, a texture compile) into whatever early frame first asked how
		// tall a capital is. Before resolution the caller gets the metrics of the weight it ASKED for,
		// which is the right answer while there is no evidence that weight is broken; after
		// resolution a weight that failed reports the metrics of the one that will really be drawn.
		if (!bResolved)
		{
			return Index;
		}
		return WeightStates[Index].bUsable ? Index : Metrics::DefaultWeight;
	}

	static const Metrics::FFace& EffectiveFace(ETraceTextWeight Weight)
	{
		return Metrics::Face(EffectiveIndex(Weight));
	}

	/** Atlas pixels -> screen pixels. */
	static float ScaleFor(float Size)
	{
		return FMath::Max(0.f, Size) / Metrics::EmSize;
	}

	static const Metrics::FCell* FindCell(const Metrics::FFace& Face, TCHAR Char)
	{
		const int32 Code = static_cast<int32>(Char);
		if (Code < Metrics::FirstCode || Code > Metrics::LastCode)
		{
			return nullptr;
		}
		return &Face.Cells[Code - Metrics::FirstCode];
	}

	/** The cell an unmapped character advances by, so foreign text still occupies sane space. */
	static const Metrics::FCell& SpaceCell(const Metrics::FFace& Face)
	{
		return Face.Cells[TEXT(' ') - Metrics::FirstCode];
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

	static float AtlasLineWidth(const FString& Line, float Size, float Tracking, ETraceTextWeight Weight)
	{
		const Metrics::FFace& Face = EffectiveFace(Weight);
		const float Scale = ScaleFor(Size);
		float Width = 0.f;
		int32 Drawn = 0;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			const Metrics::FCell* Cell = FindCell(Face, Line[Index]);
			Width += (Cell != nullptr ? Cell->USize : SpaceCell(Face).USize) * Scale;
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

namespace TraceTextFile
{
	/** "Sofachrome W05 ExtraLight.ttf" -> "Sofachrome". Family only; the cut is the weight's job. */
	static FString FamilyFromFile(const TCHAR* File)
	{
		FString Source(File);
		Source.RemoveFromEnd(TEXT(".otf"));
		Source.RemoveFromEnd(TEXT(".ttf"));
		// One per family, in the order a name is built: the licensed files are named
		// "Sofachrome W05 ExtraLight", "Sofachrome Rg", "Erbaum-Bold", "Lato-Regular".
		Source.RemoveFromEnd(TEXT(" W05 ExtraLight"));
		Source.RemoveFromEnd(TEXT(" Rg"));
		Source.RemoveFromEnd(TEXT("-Bold"));
		Source.RemoveFromEnd(TEXT("-Regular"));
		return Source;
	}
}

FString TraceText::FaceName()
{
	if (IsAtlasActive())
	{
		// The name of the typeface, not of the asset: this string is what a screenshot is captioned
		// with, and "T_FontAtlas" would not tell a reviewer which letterforms they are looking at.
		return TraceTextFile::FamilyFromFile(TraceFontAtlasMetrics::SourceFont);
	}

	return TraceTextFile::FamilyFromFile(TraceMenuArtStyle::MenuFontSourceFile);
}

FString TraceText::FaceName(ETraceTextWeight Weight)
{
	if (!IsAtlasActive())
	{
		return TraceTextFile::FamilyFromFile(TraceMenuArtStyle::MenuFontSourceFile);
	}

	// The EFFECTIVE face. Spec v25 §4 asks for the face to be identified in a photograph rather than
	// asserted, and a caption naming the face that was REQUESTED would be exactly the assertion it is
	// meant to replace: if T_FontAtlasHud failed the staleness guard the HUD draws in Sofachrome and
	// this has to say Sofachrome.
	return TraceTextFile::FamilyFromFile(TraceTextFile::EffectiveFace(Weight).Source);
}

const TCHAR* TraceText::FaceSourceFile(ETraceTextWeight Weight)
{
	if (!IsAtlasActive())
	{
		return TraceMenuArtStyle::MenuFontSourceFile;
	}
	return TraceTextFile::EffectiveFace(Weight).Source;
}

bool TraceText::IsWeightActive(ETraceTextWeight Weight)
{
	if (!IsAtlasActive())
	{
		return false;
	}
	return TraceTextFile::WeightStates[TraceTextFile::WeightIndex(Weight)].bUsable;
}

const TCHAR* TraceText::WeightName(ETraceTextWeight Weight)
{
	return TraceFontAtlasMetrics::Face(TraceTextFile::WeightIndex(Weight)).Name;
}

ETraceTextWeight TraceText::WeightFromName(const FString& Name, ETraceTextWeight Fallback,
	bool* bOutMatched)
{
	for (int32 Index = 0; Index < TraceTextFile::NumWeights; ++Index)
	{
		if (Name.Equals(TraceFontAtlasMetrics::Face(Index).Name, ESearchCase::IgnoreCase))
		{
			if (bOutMatched != nullptr)
			{
				*bOutMatched = true;
			}
			return static_cast<ETraceTextWeight>(Index);
		}
	}

	if (bOutMatched != nullptr)
	{
		*bOutMatched = false;
	}
	return Fallback;
}

UTexture2D* TraceText::AtlasTexture(ETraceTextWeight Weight)
{
	TraceTextFile::Resolve();
	// The EFFECTIVE weight, so the texture handed back is always the one the cell table and the
	// measurement above were taken from. Handing back a null for an unavailable weight would make
	// each renderer invent its own substitution, and they would not agree.
	return TraceTextFile::Refs().Textures[TraceTextFile::EffectiveIndex(Weight)];
}

FString TraceText::DescribeFace()
{
	TraceTextFile::Resolve();

	if (IsAtlasActive())
	{
		FString Weights;
		for (int32 Index = 0; Index < TraceTextFile::NumWeights; ++Index)
		{
			const TraceFontAtlasMetrics::FFace& Face = TraceFontAtlasMetrics::Face(Index);
			Weights += FString::Printf(TEXT("%s%s = %s (cap %.0f, ascent %.0f, thin %.0f)%s"),
				Weights.IsEmpty() ? TEXT("") : TEXT(", "),
				Face.Name, Face.Source, Face.CapHeight, Face.Ascent, Face.Erosion,
				TraceTextFile::WeightStates[Index].bUsable ? TEXT("") : TEXT(" — UNAVAILABLE"));
		}

		const TraceFontAtlasMetrics::FFace& Default =
			TraceFontAtlasMetrics::Face(TraceFontAtlasMetrics::DefaultWeight);

		return FString::Printf(
			TEXT("GLYPH ATLAS — %s is the default face, from %s (%dx%d, %d glyphs, em %.0f, cap %.0f). ")
			TEXT("Every letter is one textured quad drawn by Source/Trace/UI/Text; no UFont and no ")
			TEXT("FSlateFontInfo is involved, which is the whole point — an offline UFont cannot drive ")
			TEXT("UMG. Faces: %s. Each is rasterised from its OWN font file (thin 0 for all of them), ")
			TEXT("so they share NEITHER advances NOR baselines — measure and baseline-align a string in ")
			TEXT("the weight you will draw it in. They DO share the %.0f px line box, which is what one ")
			TEXT("layout pass needs and what the importer enforces. Force the fallback with ")
			TEXT("-TraceNoFontAtlas or `Trace.Text.Atlas 0`."),
			Default.Source, Default.TextureAsset, Default.AtlasWidth, Default.AtlasHeight,
			TraceFontAtlasMetrics::NumGlyphs, TraceFontAtlasMetrics::EmSize, Default.CapHeight,
			*Weights, TraceFontAtlasMetrics::LineHeight);
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

float TraceText::Ascent(float Size, ETraceTextWeight Weight)
{
	// PER FACE since v25. The line box is shared and enforced; the baseline inside it is not — Erbaum
	// splits 116 px as 93/23 against Sofachrome's 95/21. The EFFECTIVE face, so a weight that failed
	// the staleness guard reports the baseline of the face that will really be drawn.
	return TraceTextFile::EffectiveFace(Weight).Ascent * TraceTextFile::ScaleFor(Size);
}

float TraceText::Descent(float Size, ETraceTextWeight Weight)
{
	return TraceTextFile::EffectiveFace(Weight).Descent * TraceTextFile::ScaleFor(Size);
}

float TraceText::CapHeight(float Size, ETraceTextWeight Weight)
{
	// Per weight, and this one is NOT a rounding difference: eroding the drawn face to synthesise
	// the light cut takes rows off the top of every capital, so light caps come out ~10% shorter
	// than bold ones at the same Size. The EFFECTIVE face, so a caller measuring a weight that
	// failed to load gets the cap height of the weight that will actually be drawn.
	return TraceTextFile::EffectiveFace(Weight).CapHeight * TraceTextFile::ScaleFor(Size);
}

float TraceText::SizeForCapHeight(float InCapHeight, ETraceTextWeight Weight)
{
	const float FaceCapHeight = TraceTextFile::EffectiveFace(Weight).CapHeight;
	if (FaceCapHeight <= 0.f)
	{
		return 0.f;
	}
	return FMath::Max(0.f, InCapHeight) * (TraceFontAtlasMetrics::EmSize / FaceCapHeight);
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
			? TraceTextFile::AtlasLineWidth(Line, Style.Size, Style.Tracking, Style.Weight)
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
	// Both of these ask the STYLE'S OWN face where its baseline is. They used to read one shared
	// ascent, which was true while every face split the line box the same way and stopped being true
	// when Erbaum arrived splitting it 93/23 against Sofachrome's 95/21.
	case EVAlign::Baseline: Y = -Ascent(Size, Style.Weight); break;
	// A baked word sprite's top edge is the CAP LINE, not the line box, so live text that must sit
	// where one sat has to be lifted by the gap between them.
	case EVAlign::CapTop:   Y = -(Ascent(Size, Style.Weight) - CapHeight(Size, Style.Weight)); break;
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

	// THE ONLY THING THE WEIGHT CHANGES. One face is picked here, once, and the loop below is the
	// same loop it has always been — the pen advances by the same widths, the lines break in the
	// same places, and only which sheet the UVs address moves. That is what makes this one layout
	// path with two metric sources rather than two code paths.
	const TraceFontAtlasMetrics::FFace& Face = TraceTextFile::EffectiveFace(Style.Weight);
	const float AtlasW = static_cast<float>(Face.AtlasWidth);
	const float AtlasH = static_cast<float>(Face.AtlasHeight);

	OutQuads.Reserve(Text.Len());

	for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
	{
		const FString& Line = Lines[LineIndex];

		// Every line is aligned inside the BLOCK, so a centred two-line label centres both lines
		// rather than centring the longest and left-aligning the rest.
		float PenX = Origin.X;
		const float LineWidth =
			TraceTextFile::AtlasLineWidth(Line, Style.Size, Style.Tracking, Style.Weight);
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
			const TraceFontAtlasMetrics::FCell* Cell = TraceTextFile::FindCell(Face, Char);
			const TraceFontAtlasMetrics::FCell& Advance =
				(Cell != nullptr) ? *Cell : TraceTextFile::SpaceCell(Face);

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
