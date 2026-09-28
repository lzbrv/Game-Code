// Trace — the title screen's palette and layout constants, in ONE place.
//
// These lived in an unnamed block at the top of UI/TraceMenuHUD.cpp until spec v17 §4. They moved
// here because there are now TWO renderers for the same screen — the original AHUD::DrawHUD Canvas
// path and the UMG widget path behind `Trace.UI.UseUMG` — and two copies of a palette is two
// palettes that drift. Everything the Canvas path draws with, the widget classes default to, and
// `Trace.UI.VerifyMenu` compares the two.
//
// THE COLOURS BELOW ARE THE PRE-KIT "TRON" PALETTE AND ARE RETIRED FOR EVERY KIT SCREEN. The title,
// the JOIN prompt, the travel card and the pointer are all on the handmade kit now — navy plates,
// an orange hover glow and white words on black (UI/Widgets/Menu/TraceMenuArtStyle.h for the
// artist's colours, TraceMenuKit.h for how they are used). Cyan is not a kit colour. Cyan, CyanDeep,
// Ink, InkDim, Void and PanelFill stay only for the screens that have not been converted yet; do not
// reach for them on a new one. The layout constants below are still live and shared by both title
// renderers.
//
// Every pixel constant is authored against a 1080-tall viewport and multiplied by UIScale. The UMG
// path gets the same number a different way: UMG's DPI scale is (shortest side / 1080) under the
// engine's default curve, so a widget authored in these same reference pixels lands in the same
// place. `Trace.UI.VerifyMenu` measures that rather than trusting it.

#pragma once

#include "CoreMinimal.h"

#include "UI/TraceMatchOptions.h"   // ETraceBotDifficulty

namespace TraceMenuStyle
{
	static constexpr float ReferenceHeight = 1080.f;

	static const FLinearColor Void      (0.006f, 0.011f, 0.022f, 1.00f);
	static const FLinearColor Cyan      (0.16f,  0.88f,  1.00f,  1.00f);
	static const FLinearColor CyanDeep  (0.04f,  0.34f,  0.48f,  1.00f);
	static const FLinearColor Amber     (1.00f,  0.46f,  0.08f,  1.00f);
	static const FLinearColor Ink       (0.90f,  0.97f,  1.00f,  1.00f);
	static const FLinearColor InkDim    (0.42f,  0.58f,  0.66f,  1.00f);

	/** The console panel and the footer strip both sit on this. */
	static const FLinearColor PanelFill (0.004f, 0.014f, 0.026f, 0.94f);

	/**
	 * Row geometry, in reference pixels.
	 *
	 * Spacing came down from 78 to 71 when JOIN was added: six rows at the old pitch put the bottom
	 * of the console panel under the footer's dark strip at 1080p, which is the kind of collision
	 * that only shows up on somebody else's monitor.
	 */
	static constexpr float RowHeight  = 60.f;
	static constexpr float RowSpacing = 71.f;
	static constexpr float RowPadX    = 30.f;

	/** Console-panel geometry, also in reference pixels / fractions of the viewport. */
	static constexpr float PanelPadX      = 34.f;
	static constexpr float PanelPadTop    = 22.f;
	static constexpr float PanelPadBottom = 58.f;
	static constexpr float PanelTopFraction  = 0.395f;
	static constexpr float PanelWidthFraction = 0.52f;
	static constexpr float PanelMaxWidth      = 720.f;

	/**
	 * WHERE A TITLE ROW'S WORD SITS — one rule for both title renderers (UI QA F2).
	 *
	 * The kit centres a label on its plate, caps at TraceMenuKit::LabelCapFraction of the plate
	 * (TraceMenuKit::LabelSize), and so now do the four title rows that carry nothing else: PLAY,
	 * PRACTICE, SETTINGS, QUIT. The title used to set every row's word left-aligned 30 px in at caps
	 * 0.28 of the plate, so the same button read ~30% smaller on the title than in the pause menu.
	 *
	 * A row with a READOUT on its right — JOIN's address, DIFFICULTY's value in its chip — keeps its
	 * word on the leading edge, RowPadX in: at the kit's size a centred DIFFICULTY would end at about
	 * x 498 of 720 and the value chip starts at about 470. Whether those two stay left is the owner's
	 * call; this is the one line that decides it.
	 *
	 * The UMG row (UTraceMenuRow::PlaceLabel) and the Canvas row (ATraceMenuHUD::DrawRow) both ask
	 * this from the same FTraceMenuRowView, so the two renderers cannot put one word in two places.
	 */
	static constexpr bool RowLabelIsCentred(bool bHasStatus, bool bHasValue)
	{
		return !bHasStatus && !bHasValue;
	}

	/**
	 * The widest a row's word may draw before it is shrunk to fit, in reference px: from the leading
	 * pad to where the value chip starts, about 60% across (720 * 0.6 - 30 = 402). Shared because the
	 * Canvas row sizes its word by the kit now too and needs the same bound as the UMG row.
	 */
	static constexpr float RowLabelMaxWidth = PanelMaxWidth * 0.60f - RowPadX;

	/**
	 * Where the grid floor meets the dark. Shared by the backdrop glow and the grid itself so the
	 * glow can never end somewhere the horizon line is not.
	 */
	static constexpr float HorizonFraction = 0.66f;

	/**
	 * Seconds after the title screen appears during which Enter/Space will not activate a row.
	 *
	 * Shortened from 0.75s now that the mouse path — which was the actual bug — is fixed at its
	 * cause by press/release semantics plus foreground-click suppression. See
	 * ATraceMenuHUD::AcceptUnlockTime.
	 */
	static constexpr float ActivationGraceSeconds = 0.35f;

	static FLinearColor WithAlpha(const FLinearColor& C, float A)
	{
		return FLinearColor(C.R, C.G, C.B, A);
	}

	// DifficultyColor() and DifficultyBlurb() lived here. The colour was retired by the release art
	// bible; the blurb moved to TraceDifficulty::ToBlurb (UI/TraceMatchOptions.h) so its words go
	// through TRACE_TEXT and Ranen can edit them.

	/**
	 * THE TITLE'S AMBER SELECTION RAIL — owner-requested, TITLE ONLY (not part of the kit; the kit's
	 * VisualsFor deliberately leaves it out). One definition for both title renderers: the UMG row
	 * (TraceMenuRowWidget) and the Canvas row (ATraceMenuHUD::DrawRow) used to draw two different
	 * bars — 9x40 rounded 14 px out, and 6x60 square 24 px out on a different pulse.
	 *
	 * Proportions of the row height, so the rail follows the row; the gap is reference px between
	 * the rail and the plate (the UMG row's slot is authored 14 px out by the widget generator).
	 */
	static constexpr float SelectionRailWidthOfRow  = 0.15f;
	static constexpr float SelectionRailHeightOfRow = 2.f / 3.f;
	static constexpr float SelectionRailGap         = 14.f;

	/**
	 * The console panel's rect, in reference pixels, for a viewport of @p InViewH reference-height
	 * units. BOTH renderers derive their row rects from this, which is what makes
	 * `Trace.UI.VerifyMenu`'s comparison meaningful rather than circular: the Canvas path multiplies
	 * by UIScale, the UMG path lets the DPI scale do it, and the verifier measures the difference.
	 */
	struct FConsoleLayout
	{
		float PanelX = 0.f;
		float PanelY = 0.f;
		float PanelW = 0.f;
		float PanelH = 0.f;
		float RowX = 0.f;
		float RowW = 0.f;
		float FirstRowY = 0.f;
	};

	/**
	 * @param InViewW  viewport width  in the caller's units (pixels for Canvas, reference for UMG)
	 * @param InViewH  viewport height in the same units
	 * @param InScale  1.0 when the caller's units are already reference pixels
	 */
	static FConsoleLayout ComputeConsoleLayout(float InViewW, float InViewH, float InScale, int32 InRowCount)
	{
		FConsoleLayout Out;
		const float CX = InViewW * 0.5f;
		Out.RowW = FMath::Min(InViewW * PanelWidthFraction, PanelMaxWidth * InScale);
		const float PadX = PanelPadX * InScale;
		Out.PanelW = Out.RowW + PadX * 2.f;
		Out.PanelX = CX - Out.PanelW * 0.5f;
		Out.PanelY = InViewH * PanelTopFraction;
		Out.PanelH = (PanelPadTop * InScale)
			+ (InRowCount - 1) * (RowSpacing * InScale)
			+ (RowHeight * InScale)
			+ (PanelPadBottom * InScale);
		Out.RowX = CX - Out.RowW * 0.5f;
		Out.FirstRowY = Out.PanelY + (PanelPadTop * InScale);
		return Out;
	}
}
