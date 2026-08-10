// Trace — the title screen's palette and layout constants, in ONE place.
//
// These lived in an unnamed block at the top of UI/TraceMenuHUD.cpp until spec v17 §4. They moved
// here because there are now TWO renderers for the same screen — the original AHUD::DrawHUD Canvas
// path and the UMG widget path behind `Trace.UI.UseUMG` — and two copies of a palette is two
// palettes that drift. Everything the Canvas path draws with, the widget classes default to, and
// `Trace.UI.VerifyMenu` compares the two.
//
// The whole screen is two hues — a cyan that carries the interface and an amber that only ever
// marks danger or the Hard setting — over near-black. That restraint is the entire look: the moment
// a third colour appears, the grid stops reading as a grid and starts reading as noise.
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

	/** Colour that marks each difficulty, so the setting is legible without reading the word. */
	static FLinearColor DifficultyColor(ETraceBotDifficulty InDifficulty)
	{
		switch (InDifficulty)
		{
		case ETraceBotDifficulty::Easy: return FLinearColor(0.30f, 1.00f, 0.72f, 1.f);
		case ETraceBotDifficulty::Hard: return Amber;
		default:                        return Cyan;
		}
	}

	static FString DifficultyBlurb(ETraceBotDifficulty InDifficulty)
	{
		switch (InDifficulty)
		{
		case ETraceBotDifficulty::Easy:
			return TEXT("BOTS REACT SLOWLY AND SHOOT LOOSELY.  START HERE.");
		case ETraceBotDifficulty::Hard:
			return TEXT("BOTS REACT FAST, AIM TIGHT AND PUSH THE CORE.");
		default:
			return TEXT("BOTS PLAY THE SHIPPED TUNING.  A FAIR FIGHT.");
		}
	}

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
