// Trace — the scrolling grid floor, ported to the UMG title screen and restyled (spec v23 §A1).
//
// -------------------------------------------------------------------------------------------------
// THIS FILE WAS AN ORPHAN AND IS NOT ONE ANY MORE. READ THIS BEFORE EDITING IT.
// -------------------------------------------------------------------------------------------------
// A previous pass left this header on disk declaring UCLASS UTraceMenuGrid with six member functions
// and NO .cpp. UnrealHeaderTool emits the reflection code and the vtable for a UCLASS whether or not
// anybody includes the header, so the module failed to LINK — and a failed link DELETES
// libUnrealEditor-Trace.dylib, which stops every agent on the machine, not just the one who owns the
// file:
//
//     Undefined symbols for architecture arm64:
//       "UTraceMenuGrid::RebuildWidget()", referenced from: vtable for UTraceMenuGrid
//       ... and four more, all from generated code, none from any call site.
//
// It was renamed to TraceMenuGridWidget.h.orphan-no-cpp to get the build back, which is why the
// design notes below survived. TraceMenuGridWidget.cpp now exists and implements every one of these
// declarations. *** If you delete or rename that .cpp, delete or rename this header in the same
// commit. *** A bare UCLASS header does not fail to compile; it fails to link, for everybody.
//
// -------------------------------------------------------------------------------------------------
// WHERE THIS CAME FROM
// -------------------------------------------------------------------------------------------------
// The owner asked for "something like the scrolling background grid that used to be in the main menu
// ui, just matching the current font/color/grungy style". It was never lost.
// ATraceMenuHUD::DrawGridFloor() still draws it on the CANVAS title screen — rails converging on a
// vanishing point, rungs scrolling toward the viewer at FMath::Pow(T, 2.6f) spacing, a bright
// horizon, scanlines. Spec v19 §5 made the artist's UMG screen the default and gave it a flat black
// backdrop, so the grid simply stopped being on the screen anybody sees. This is the port.
//
// The trick that makes it read as a floor rather than a ladder is the exponent, and it is copied
// verbatim from the Canvas comment: "evenly spaced rungs read as a ladder, squared spacing reads as
// a floor running away from you."
//
// -------------------------------------------------------------------------------------------------
// WHY THIS IS NOT UTraceMenuCanvasArt
// -------------------------------------------------------------------------------------------------
// UTraceMenuCanvasArt (ETraceMenuArtKind::Backdrop, same directory) is ALSO a Slate port of the same
// function, written for spec v17 §4, and it is unused — Scripts/generate-menu-widgets.py never puts
// it in WBP_TitleMenu. It is deliberately pixel-faithful: its own header says "spec v17 §0: a
// reorganisation must not change how anything looks", and its palette is the original pure cyan
// FLinearColor(0.16, 0.88, 1.00) with a hard-edged, evenly-lit floor.
//
// Spec v23 §A1 asks for the opposite of faithful: the artist's palette rather than the old pure
// cyan, and grungy/worn rather than clean neon. Retuning UTraceMenuCanvasArt would have destroyed
// the one thing it is for. So the old one stays exactly as it is, as the reference rendering, and
// the restyle lives here.
//
// -------------------------------------------------------------------------------------------------
// THE RESTYLE, AND WHERE EVERY COLOUR COMES FROM
// -------------------------------------------------------------------------------------------------
// Neither hue is invented and neither is typed in as a literal. Both are COMPUTED, at runtime, from
// a constant that already decides how something else on this screen looks — see SteelLine() and
// HorizonAmber() in the .cpp:
//
//   STEEL   the artist's button plate, TraceMenuArtStyle::PlateFill = RGB(29,41,81), with its
//           brightness put back (divided through by its own largest channel, which is exactly "the
//           same hue at full value"). Comes out at sRGB(108,142,255). The floor and the buttons are
//           therefore literally the same colour, which is what stops the background reading as a
//           second design. The old grid's pure cyan, sRGB(111,241,255), is a different hue from
//           anything the artist drew.
//   AMBER   TraceMenuArtStyle::Amber = RGB(116,58,0), the same way: sRGB(255,135,0). Measured
//           against the wordmark's own glow (TraceTitleMenuWidget's WordmarkGlow, RGB(255,140,40))
//           that is 5 bytes of green and 40 of blue apart — the same colour to the eye. It appears
//           in exactly ONE place, the horizon, so the brightest line in the background is the same
//           colour as the brightest thing in the title.
//
// "Grungy" is four specific departures from the clean neon original, all of them cheap:
//   1. every rung is drawn as nine independently jittered cells with occasional near-dropouts, so no
//      line in the frame is an unbroken machine-perfect stroke. The jitter is keyed to a rung's
//      TRAVELLING identity, not to its slot in the loop, so the broken cells move toward the viewer
//      with the rung they belong to instead of sitting still in screen space;
//   2. every rail carries a per-rail alpha jitter, so the fan is uneven;
//   3. dust marks scroll along the floor on the same perspective curve as the rungs;
//   4. the scanlines have hashed, slowly-reseeded per-line alpha instead of one constant, which is
//      what makes them read as a worn tube rather than a CSS overlay.
//
// -------------------------------------------------------------------------------------------------
// READABILITY IS A CONSTRAINT, NOT AN AFTERTHOUGHT
// -------------------------------------------------------------------------------------------------
// Spec v23 §A1: "it must not cost the rows their readability — measure text contrast with and
// without it."
//
// Three structural decisions do that work, and all three are visible in the numbers rather than
// argued. Note that WBP_TitleMenu's ConsolePanel is a CLEAR border — the rows wear their own plates
// and nothing else on this screen has anything opaque behind it — so "the grid is behind a panel"
// is not available as an answer here. It is behind the PLATES, and nothing else:
//
//   THE LIGHT IS AT THE HORIZON. The Canvas original brightens its rungs toward the BOTTOM of the
//   frame (alpha = 0.32 * T), which on the UMG screen would put the grid's strongest pixels directly
//   under the blurb and the two footer lines — the three strings on this screen that sit on bare
//   black. So the floor fades toward the viewer instead. See FloorFade().
//
//   THE HAZE STOPS BELOW THE TAGLINE. The horizon glow's height is chosen so its top edge lands
//   under the tagline's baseline (tagline at reference y=359, horizon at 0.66*1080=712.8, haze
//   0.30*1080=324 tall, so its top edge is y=388.8), and its ramp is quadratic, so the band is
//   already at ~0 alpha where the address chip is.
//
//   EVERY LEVEL IS A THIRD OF THE CANVAS ORIGINAL'S. Rails 0.085 against 0.34, rungs 0.115 against
//   0.32. The Canvas screen's grid is a foreground element; this one is wallpaper.
//
// -------------------------------------------------------------------------------------------------
// SWITCHES
// -------------------------------------------------------------------------------------------------
//   Trace.UI.MenuGrid 0|1              draw it at all. Default 1. Live: takes effect the next frame.
//   Trace.UI.MenuGridIntensity <float> master multiplier on every alpha. Default 1.
//   -TraceNoMenuGrid                   command line, beats the cvar, for the same reason
//                                      -TraceNoMenuUMG does: a cvar set with -ExecCmds arrives after
//                                      the first title screen has already been built. THIS IS THE
//                                      ARM THE CONTRAST MEASUREMENT IS TAKEN AGAINST.

#pragma once

#include "CoreMinimal.h"
#include "Components/Widget.h"

#include "TraceMenuGridWidget.generated.h"

class STraceMenuGrid;

/**
 * Every number the grid needs, in one plain struct so the Slate leaf is handed a copy rather than
 * reaching back into a UObject during paint. Same pattern as FTraceMenuArtParams.
 *
 * Fractions of the widget's own size wherever a fraction will do, so none of this depends on the
 * viewport resolution or the DPI scale.
 */
struct FTraceMenuGridParams
{
	/** Seconds. Pushed in from ATraceMenuHUD's clock via the view; this widget owns no clock. */
	float TimeSeconds = 0.f;

	/** Master multiplier on every alpha below. `Trace.UI.MenuGridIntensity`. */
	float Intensity = 1.f;

	// ---- Geometry, shared with the Canvas renderer -------------------------------------------------

	float HorizonFraction = 0.66f;
	int32 RailCount = 16;
	float RailSpreadFraction = 0.34f;
	int32 RungCount = 22;
	float RungScrollSpeed = 0.16f;
	/** The exponent that makes it a floor and not a ladder. See the header comment. */
	float RungExponent = 2.6f;

	// ---- Levels ------------------------------------------------------------------------------------

	float RailAlpha = 0.085f;
	float RungAlpha = 0.115f;
	float HazeStrength = 0.13f;
	float HazeHeightFraction = 0.30f;
	float HorizonHaloAlpha = 0.13f;
	float HorizonMidAlpha = 0.28f;
	float HorizonCoreAlpha = 0.50f;

	// ---- The readability structure ------------------------------------------------------------------

	/** Fraction of the height at which the floor starts fading toward the viewer. */
	float NearFadeStart = 0.74f;
	/** Fraction of the height by which the fade has fully bitten. */
	float NearFadeEnd = 0.95f;
	/** What is left of the grid under the blurb and the footer. */
	float NearFadeFloor = 0.14f;

	float VignetteWidthFraction = 0.14f;
	float VignetteStrength = 0.55f;

	// ---- Grunge -------------------------------------------------------------------------------------

	int32 ScanlineStep = 4;
	float ScanlineAlpha = 0.20f;
	/** How much of a scanline's alpha is hashed rather than constant, 0..1. */
	float ScanlineNoise = 0.55f;
	/** Reseeds per second for the scanline noise. Low: film grain, not television static. */
	float ScanlineNoiseHz = 9.f;

	int32 DustCount = 26;
	float DustAlpha = 0.075f;

	/** Cells a rung is broken into, and how often one of them all but drops out. */
	int32 RungCells = 9;
	float RungDropoutChance = 0.12f;
};

/**
 * The scrolling grid floor. Sizes itself to whatever slot it is given, derives every pixel from
 * that, and takes no input.
 *
 * NOT a BindWidget: it is not in WBP_TitleMenu and is not meant to be. UTraceTitleMenuWidget adds it
 * to the root canvas at runtime, directly above the black Backdrop and below every other layer — see
 * UTraceTitleMenuWidget::InstallGridBackground() for why that is a code decision rather than a
 * generator one.
 */
UCLASS(meta = (DisplayName = "Trace Menu Grid"))
class TRACE_API UTraceMenuGrid : public UWidget
{
	GENERATED_BODY()

public:
	UTraceMenuGrid();

	/**
	 * Whether the grid should draw at all: `-TraceNoMenuGrid` first, then `Trace.UI.MenuGrid`.
	 *
	 * Command line beats cvar, which is the same ordering (and the same reason) as
	 * ATraceMenuHUD's WantsUMG(): a cvar set with -ExecCmds arrives after the title screen has
	 * already been built, so a launch that wants the grid off has to be able to say so up front.
	 */
	static bool WantsGrid();

	/** `Trace.UI.MenuGridIntensity`. 1 is the shipped level; 0 is indistinguishable from off. */
	static float GridIntensity();

	/** Drives the scroll, the dust and the scanline noise. Pushed in once per frame from the view. */
	void SetAnimationTime(float InSeconds);

	/** One line naming the palette and the levels, for the log and for a verifier. */
	static FString Describe();

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
	FTraceMenuGridParams BuildParams() const;

	/** Mirrors FTraceMenuGridParams::TimeSeconds so the value survives a Slate rebuild. */
	float TimeSeconds = 0.f;

	TSharedPtr<STraceMenuGrid> GridWidget;
};
