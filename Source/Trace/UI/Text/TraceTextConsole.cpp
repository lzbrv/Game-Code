// Trace — `Trace.Text.Report` and `Trace.Text.Preview` (spec v22 §A1).
//
// =================================================================================================
// WHY A PREVIEW COMMAND, AND WHY IT DRAWS THROUGH BOTH RENDERERS
// =================================================================================================
// The failure this whole feature exists to avoid is a SILENT one: Slate substituting its last-resort
// face without logging anything. A report that only printed "the atlas loaded" would be exactly as
// trustworthy as the bug it is guarding against, because loading the texture is not the same claim
// as drawing with it.
//
// So Preview puts a specimen on screen through the REAL code paths — the same STraceAtlasText leaf
// the UMG title screen uses, and the same TraceCanvasText blitter the Canvas screens use — and
// alongside them a control line typeset in Lato through ordinary Slate. One screenshot then answers
// "which face is on screen" by comparison rather than by assertion: if the specimen and the control
// look the same, the atlas is NOT drawing, whatever any log line says.
//
// The Canvas half goes through UDebugDrawService rather than a HUD hook because every AHUD subclass
// in this project belongs to another spec item and another agent this pass. The service hands out
// the viewport's debug UCanvas, which is the same surface AHUD::PostRender draws into and is
// captured by FScreenshotRequest with bShowUI=true — i.e. by -TraceAutoShot.

#include "CoreMinimal.h"

#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#include "Trace.h"                                  // LogTraceGame
#include "UI/Text/TraceAtlasTextWidget.h"
#include "UI/Text/TraceCanvasText.h"
#include "UI/Text/TraceText.h"
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"

// Named after the file for the unity/jumbo build; see Scripts/check-jumbo-build-collisions.py.
namespace TraceTextConsoleFile
{
	/** The specimen. Caps, digits and the menu's own words — the strings this feature exists for. */
	static const TCHAR* SpecimenA = TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
	static const TCHAR* SpecimenB = TEXT("0123456789 .,:;!?-()");
	static const TCHAR* MenuWords = TEXT("PLAY SETTINGS JOIN PRACTICE QUIT");

	/**
	 * THE FACE STACK (spec v23 §A3, third row added by v25 §4).
	 *
	 * A character name, because that is the one string in the game that is meant to be bold, and it
	 * is full of flat vertical stems (M, I, T, R) — which is what makes the faces separable in a
	 * SCREENSHOT rather than only in a log line. The rows are drawn at the same size, from the same
	 * X, one line apart, so a vertical slice through them measures stems that differ only by face.
	 * "The flag was set" is not evidence; three sets of letterforms in one image is.
	 *
	 * The 'R' carries the v25 row on its own: Sofachrome's has a straight splayed leg and a squared
	 * bowl that runs flush to the advance; Erbaum's bowl is a flat-sided rectangle and its leg is a
	 * short vertical. Anyone comparing the two rows can name the face without reading the caption —
	 * which is the point, because the caption is exactly the assertion the photograph must replace.
	 */
	static const TCHAR* WeightSpecimen = TEXT("MORTIMER");

	/** Size for the face rows. Big enough that a stem is many pixels wide, so the ratio is robust. */
	static constexpr float WeightSpecimenSize = 44.f;

	/** Row colours, indexed by weight. Distinct so a row can be pointed at in a screenshot. */
	static FLinearColor WeightInk(ETraceTextWeight Weight)
	{
		switch (Weight)
		{
		case ETraceTextWeight::Bold: return FLinearColor(1.00f, 0.78f, 0.36f, 1.f);
		case ETraceTextWeight::Hud:  return FLinearColor(0.55f, 1.00f, 0.62f, 1.f);
		default:                     return FLinearColor(0.62f, 0.86f, 1.00f, 1.f);
		}
	}

	/** What each face is FOR, so the specimen says where on screen the reviewer should go looking. */
	static const TCHAR* WeightUse(ETraceTextWeight Weight)
	{
		switch (Weight)
		{
		case ETraceTextWeight::Bold: return TEXT("BOLD  (SELECT NAMES) ");
		case ETraceTextWeight::Hud:  return TEXT("HUD   (MATCH + ABIL) ");
		default:                     return TEXT("LIGHT (DEFAULT)      ");
		}
	}

	/** Defined below, next to the Slate half's version of the same comparison. */
	static void DrawWeightPair(UCanvas* Canvas, float X, float Y, float S, const TCHAR* Which);

	static TSharedPtr<SWidget> Overlay;
	static FDelegateHandle CanvasHandle;

	static bool IsShowing()
	{
		return Overlay.IsValid();
	}

	// ---------------------------------------------------------------------------------------------
	// The Canvas half
	// ---------------------------------------------------------------------------------------------

	static void DrawCanvasHalf(UCanvas* Canvas, APlayerController* /*PC*/)
	{
		if (Canvas == nullptr || Canvas->SizeX <= 0)
		{
			return;
		}

		// Everything below is authored against a 1080-tall frame and scaled, so the specimen is the
		// same specimen at 1280x720 and at 1920x1080 and a screenshot of one can be compared to the
		// other.
		const float S = FMath::Max(0.4f, Canvas->SizeY / 1080.f);
		const float X = 60.f * S;
		float Y = Canvas->SizeY * 0.52f;

		const FLinearColor Label(0.55f, 0.62f, 0.72f, 1.f);
		const FLinearColor Ink = FLinearColor::White;

		TraceText::FStyle Caption(15.f * S, Label);
		TraceCanvasText::Draw(Canvas, TEXT("CANVAS RENDERER  -  AHUD::DrawTexture, ONE TILE PER GLYPH"),
			X, Y, Caption);
		Y += TraceText::LineHeight(Caption.Size) * 1.2f;

		TraceText::FStyle Body(34.f * S, Ink);
		TraceCanvasText::Draw(Canvas, SpecimenA, X, Y, Body);
		Y += TraceText::LineHeight(Body.Size);
		TraceCanvasText::Draw(Canvas, SpecimenB, X, Y, Body);
		Y += TraceText::LineHeight(Body.Size);
		TraceCanvasText::Draw(Canvas, MenuWords, X, Y, Body);
		Y += TraceText::LineHeight(Body.Size) * 1.3f;

		// The measurement, on screen, in the same pass that drew the string. A width printed by a
		// harness that never drew anything is the kind of evidence this project has been burned by.
		const float Measured = TraceText::MeasureWidth(SpecimenA, Body);
		TraceCanvasText::Draw(Canvas,
			FString::Printf(TEXT("FACE %s   MEASURED %.1f PX   CAP %.1f   LINE %.1f"),
				*TraceText::FaceName().ToUpper(), Measured,
				TraceText::CapHeight(Body.Size), TraceText::LineHeight(Body.Size)),
			X, Y, Caption);
		Y += TraceText::LineHeight(Caption.Size) * 1.6f;

		DrawWeightPair(Canvas, X, Y, S, TEXT("CANVAS"));
	}

	/**
	 * Every face, same string, same size, same left edge, one line apart.
	 *
	 * Drawn through the ORDINARY draw call with nothing but Style.Weight changed between the rows —
	 * so if these rows have the same letterforms on screen, the weight argument is not reaching the
	 * renderer, whatever the log says. That is the same comparison-not-assertion trick the Lato
	 * control line uses above, applied to face instead of to typeface.
	 *
	 * The caption carries each row's width, cap height and the FONT FILE it came from. Spec v25 §4
	 * says to verify by identifying the FACE in a screenshot rather than by asserting a flag; a
	 * caption that only said "Hud" would be an assertion, so it prints "Erbaum-Bold.otf" — and prints
	 * it from the EFFECTIVE face, so a sheet that failed to load names Sofachrome and gives the game
	 * away instead of covering for itself.
	 *
	 * The numbers are the other half of the claim. WIDTHS must all DIFFER: three different font files,
	 * bold about half again as wide as light and Erbaum narrower than either, which is exactly why
	 * every measurement has to be told its weight. CAP must MATCH between Light and Bold (65 px at
	 * em 96 — two real cuts of one family) and must NOT match for Hud (70 px — a different family).
	 * Numbers that would all move if the face table and the sheets ever came apart.
	 */
	static void DrawWeightPair(UCanvas* Canvas, float X, float Y, float S, const TCHAR* Which)
	{
		const FLinearColor Label(0.55f, 0.62f, 0.72f, 1.f);

		TraceText::FStyle Caption(15.f * S, Label);
		TraceCanvasText::Draw(Canvas,
			FString::Printf(TEXT("%s  -  EVERY FACE, ONE LAYOUT PASS, ONLY Style.Weight DIFFERS"), Which),
			X, Y, Caption);
		Y += TraceText::LineHeight(Caption.Size) * 1.15f;

		const float Size = WeightSpecimenSize * S;
		const float Pitch = TraceText::LineHeight(Size) * 1.02f;

		// The label column is measured in the LIGHT weight and reserved for every row, so the
		// specimens all start at exactly the same X — the comparison depends on that.
		TraceText::FStyle TagStyle(13.f * S, Label);
		float TagW = 0.f;
		for (int32 Pass = 0; Pass < static_cast<int32>(ETraceTextWeight::Count); ++Pass)
		{
			TagW = FMath::Max(TagW,
				TraceText::MeasureWidth(WeightUse(static_cast<ETraceTextWeight>(Pass)), TagStyle));
		}
		TagW += 8.f * S;

		for (int32 Pass = 0; Pass < static_cast<int32>(ETraceTextWeight::Count); ++Pass)
		{
			const ETraceTextWeight Weight = static_cast<ETraceTextWeight>(Pass);

			TraceText::FStyle Row(Size, WeightInk(Weight));
			Row.Weight = Weight;   // <<< the only difference between the rows

			const float RowY = Y + Pitch * Pass;
			// The tag sits on the SPECIMEN'S baseline, so the ascent it is shifted by has to be the
			// specimen's face and not the tag's — the faces no longer share one.
			TraceCanvasText::Draw(Canvas, WeightUse(Weight), X,
				RowY + (TraceText::Ascent(Size, Weight) - TraceText::Ascent(TagStyle.Size)), TagStyle);
			TraceCanvasText::Draw(Canvas, WeightSpecimen, X + TagW, RowY, Row);

			TraceCanvasText::Draw(Canvas,
				FString::Printf(TEXT("%s = %s  W %.1f  CAP %.1f  %s"),
					TraceText::WeightName(Weight),
					TraceText::FaceSourceFile(Weight),
					TraceText::MeasureWidth(WeightSpecimen, Row),
					TraceText::CapHeight(Size, Weight),
					TraceText::IsWeightActive(Weight)
						? TEXT("OWN SHEET")
						: TEXT("SUBSTITUTED - this weight did NOT load")),
				X + TagW + TraceText::MeasureWidth(WeightSpecimen, Row) + (18.f * S),
				RowY + (TraceText::Ascent(Size, Weight) - TraceText::Ascent(Caption.Size)), Caption);
		}
	}

	// ---------------------------------------------------------------------------------------------
	// The Slate half
	// ---------------------------------------------------------------------------------------------

	static TSharedRef<SWidget> MakeAtlasLine(const FString& InText, float InSize, const FLinearColor& InColor,
		ETraceTextWeight InWeight = ETraceTextWeight::Light)
	{
		FTraceAtlasTextParams Params;
		Params.Text = InText;
		Params.Style.Size = InSize;
		Params.Style.Color = InColor;
		Params.Style.Weight = InWeight;
		Params.SlotHAlign = TraceText::EHAlign::Left;
		Params.SlotVAlign = TraceText::EVAlign::Top;

		TSharedRef<STraceAtlasText> Line = SNew(STraceAtlasText);
		Line->SetParams(Params);
		return Line;
	}

	/** One face row again, through the UMG/Slate leaf this time. Same claim, other renderer. */
	static TSharedRef<SWidget> MakeWeightRow(ETraceTextWeight Weight)
	{
		const FLinearColor Label(0.55f, 0.62f, 0.72f, 1.f);
		const FLinearColor Ink = WeightInk(Weight);
		TraceText::FStyle Row(WeightSpecimenSize, Ink);
		Row.Weight = Weight;

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				MakeAtlasLine(WeightUse(Weight), 13.f, Label)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(10.f, 0.f, 0.f, 0.f)
			[
				MakeAtlasLine(WeightSpecimen, WeightSpecimenSize, Ink, Weight)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(18.f, 0.f, 0.f, 0.f)
			[
				MakeAtlasLine(FString::Printf(TEXT("%s = %s  W %.1f  CAP %.1f  %s"),
					TraceText::WeightName(Weight),
					TraceText::FaceSourceFile(Weight),
					TraceText::MeasureWidth(WeightSpecimen, Row),
					TraceText::CapHeight(WeightSpecimenSize, Weight),
					TraceText::IsWeightActive(Weight)
						? TEXT("OWN SHEET")
						: TEXT("SUBSTITUTED - this weight did NOT load")),
					13.f, Label)
			];
	}

	static TSharedRef<SWidget> BuildOverlay()
	{
		const FLinearColor Label(0.55f, 0.62f, 0.72f, 1.f);
		const FLinearColor Ink = FLinearColor::White;
		const FLinearColor Control(1.f, 0.72f, 0.30f, 1.f);

		TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);

		Box->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
		[
			MakeAtlasLine(FString::Printf(TEXT("TRACE TEXT PREVIEW  -  LIVE FACE: %s"),
				*TraceText::FaceName().ToUpper()), 22.f, Ink)
		];

		Box->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 2.f)
		[
			MakeAtlasLine(TEXT("UMG / SLATE  -  FSlateDrawElement::MakeBox, ONE BOX PER GLYPH"), 15.f, Label)
		];

		Box->AddSlot().AutoHeight()[ MakeAtlasLine(SpecimenA, 34.f, Ink) ];
		Box->AddSlot().AutoHeight()[ MakeAtlasLine(SpecimenB, 34.f, Ink) ];
		Box->AddSlot().AutoHeight()[ MakeAtlasLine(MenuWords, 34.f, Ink) ];

		// THE CONTROL. Ordinary Slate text in the fallback face, at a matched size, directly under
		// the specimen. If these two lines have the same letterforms then the atlas is not drawing —
		// which is the one thing a log line cannot tell you.
		Box->AddSlot().AutoHeight().Padding(0.f, 12.f, 0.f, 2.f)
		[
			MakeAtlasLine(TEXT("CONTROL  -  ORDINARY SLATE TEXT IN THE FALLBACK FACE"), 15.f, Label)
		];
		Box->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
				.Text(FText::FromString(SpecimenA))
				.Font(TraceMenuArtStyle::MenuFont(34.f))
				.ColorAndOpacity(FSlateColor(Control))
		];
		Box->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
				.Text(FText::FromString(MenuWords))
				.Font(TraceMenuArtStyle::MenuFont(34.f))
				.ColorAndOpacity(FSlateColor(Control))
		];

		// THE FACE STACK — see DrawWeightPair. Same rows, through the Slate leaf.
		Box->AddSlot().AutoHeight().Padding(0.f, 16.f, 0.f, 2.f)
		[
			MakeAtlasLine(TEXT("UMG / SLATE  -  EVERY FACE, ONLY Style.Weight DIFFERS"), 15.f, Label)
		];
		for (int32 Index = 0; Index < static_cast<int32>(ETraceTextWeight::Count); ++Index)
		{
			Box->AddSlot().AutoHeight()
			[
				MakeWeightRow(static_cast<ETraceTextWeight>(Index))
			];
		}

		return SNew(SOverlay)
			+ SOverlay::Slot()
				.HAlign(HAlign_Left)
				.VAlign(VAlign_Top)
				.Padding(60.f, 60.f, 0.f, 0.f)
			[
				Box
			];
	}

	static void Hide()
	{
		if (CanvasHandle.IsValid())
		{
			UDebugDrawService::Unregister(CanvasHandle);
			CanvasHandle.Reset();
		}

		if (Overlay.IsValid())
		{
			if (GEngine != nullptr && GEngine->GameViewport != nullptr)
			{
				GEngine->GameViewport->RemoveViewportWidgetContent(Overlay.ToSharedRef());
			}
			Overlay.Reset();
		}
	}

	static void Show()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Text] Trace.Text.Preview needs a game viewport; there is none yet."));
			return;
		}

		Overlay = BuildOverlay();
		GEngine->GameViewport->AddViewportWidgetContent(Overlay.ToSharedRef(), /*ZOrder=*/100);

		CanvasHandle = UDebugDrawService::Register(TEXT("Game"),
			FDebugDrawDelegate::CreateStatic(&DrawCanvasHalf));

		UE_LOG(LogTraceGame, Display,
			TEXT("[Text] Preview is up. Top block is the UMG/Slate path, lower block is the Canvas ")
			TEXT("path, and the amber lines are the Lato control. Live face: %s"),
			*TraceText::FaceName());
	}

	// ---------------------------------------------------------------------------------------------
	// The commands
	// ---------------------------------------------------------------------------------------------

	static void Report(const TArray<FString>& /*Args*/)
	{
		const FString Description = TraceText::DescribeFace();

		UE_LOG(LogTraceGame, Display, TEXT("=== Trace.Text.Report ==="));
		UE_LOG(LogTraceGame, Display, TEXT("[Text] PATH: %s"),
			TraceText::IsAtlasActive() ? TEXT("GLYPH ATLAS") : TEXT("FALLBACK FONT"));
		UE_LOG(LogTraceGame, Display, TEXT("[Text] %s"), *Description);

		// A width from each renderer's shared layout pass, so the report carries a number that would
		// change if the metrics and the sheet ever came apart.
		const TraceText::FStyle Probe(34.f);
		UE_LOG(LogTraceGame, Display,
			TEXT("[Text] At size 34: cap %.2f px, ascent %.2f px, line %.2f px, \"%s\" measures %.2f px."),
			TraceText::CapHeight(34.f), TraceText::Ascent(34.f), TraceText::LineHeight(34.f),
			SpecimenA, TraceText::MeasureWidth(SpecimenA, Probe));
		// Per weight, and reporting BOTH the number that must match and the number that must not.
		// Equal widths are what makes bold safe to swap in without reflowing; different cap heights
		// are the measurable proof the two sheets are genuinely different ink. If these ever came out
		// the other way round, the weight argument would not be reaching the layout pass.
		for (int32 Index = 0; Index < static_cast<int32>(ETraceTextWeight::Count); ++Index)
		{
			const ETraceTextWeight Weight = static_cast<ETraceTextWeight>(Index);
			TraceText::FStyle Row(34.f);
			Row.Weight = Weight;
			UE_LOG(LogTraceGame, Display,
				TEXT("[Text]   weight %-6s = %-30s %s  \"%s\" measures %.2f px, cap %.2f px, ascent %.2f px"),
				TraceText::WeightName(Weight),
				TraceText::FaceSourceFile(Weight),
				TraceText::IsWeightActive(Weight) ? TEXT("(own sheet)  ") : TEXT("(SUBSTITUTED)"),
				SpecimenA, TraceText::MeasureWidth(SpecimenA, Row),
				TraceText::CapHeight(34.f, Weight), TraceText::Ascent(34.f, Weight));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[Text] Preview it with `Trace.Text.Preview` — it now draws both weights of \"%s\" ")
			TEXT("through both renderers. Force the fallback with `Trace.Text.Atlas 0` or ")
			TEXT("-TraceNoFontAtlas."), WeightSpecimen);

		if (GEngine != nullptr)
		{
			GEngine->AddOnScreenDebugMessage(-1, 12.f,
				TraceText::IsAtlasActive() ? FColor::Green : FColor::Orange,
				FString::Printf(TEXT("Trace.Text: %s"),
					TraceText::IsAtlasActive()
						? TEXT("GLYPH ATLAS - Sofachrome is drawing")
						: TEXT("FALLBACK - Sofachrome is NOT drawing")));
			GEngine->AddOnScreenDebugMessage(-1, 12.f, FColor::Silver, Description);
		}
	}

	static void Preview(const TArray<FString>& Args)
	{
		// One argument, optional: `on` / `off`. Bare, it toggles, which is what a person typing at a
		// console wants and what a -TraceExec line can rely on being idempotent about.
		bool bWant = !IsShowing();
		if (Args.Num() > 0)
		{
			const FString& Arg = Args[0];
			bWant = !(Arg == TEXT("0") || Arg.Equals(TEXT("off"), ESearchCase::IgnoreCase));
		}

		if (bWant == IsShowing())
		{
			return;
		}

		if (bWant)
		{
			Show();
		}
		else
		{
			Hide();
			UE_LOG(LogTraceGame, Display, TEXT("[Text] Preview hidden."));
		}
	}

	static FAutoConsoleCommand ReportCmd(
		TEXT("Trace.Text.Report"),
		TEXT("Names the typeface the menus are ACTUALLY drawing with, and why, on screen and in the log."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Report));

	static FAutoConsoleCommand PreviewCmd(
		TEXT("Trace.Text.Preview"),
		TEXT("Toggles a specimen drawn through BOTH renderers plus a Lato control line. ")
		TEXT("Optional argument: on | off."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Preview));
}
