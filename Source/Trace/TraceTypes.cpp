// Trace — the one free helper TraceTypes.h declares but must not define inline.
//
// ===================================================================================================
// SPEC v26 §5 — "CHANGE IN GAME COLORS OF THE BLUE AND AMBER TEAM TO MATCH THE EXACT COLOR HEXES OF
// THE BLUE AND AMBER FROM THE MAIN MENU UI ASSETS"
// ===================================================================================================
//
// WHY THIS FILE EXISTS AT ALL. TraceTeamColor used to be four literals in TraceTypes.h. §5 says the
// two team colours must come from the menu art, and Demo 21's standing rule says a value derived from
// a base is stored RELATIVE to that base so it moves when the base moves — so the literals have to be
// replaced by an expression over TraceMenuArtStyle. TraceMenuArtStyle.h includes
// Fonts/SlateFontInfo.h, and TraceTypes.h is included by nearly every file in the module and is
// documented as having to stay dependency-light. Declaring there and defining here is the only way to
// have both. Nothing else moved: the function's name, signature and meaning are unchanged, and it is
// still the single point every mesh, tracer, trail and HUD element asks.
//
// ---------------------------------------------------------------------------------------------------
// WHAT THE SHEET ACTUALLY CONTAINS, MEASURED RATHER THAN ASSUMED
// ---------------------------------------------------------------------------------------------------
// Sampled from the artist's own sprites in Content/Trace/UI/Art/Source (opaque pixels only):
//
//     T_MenuBtn_Default.png    mean RGB(29, 41, 81)     brightest 0.1%: RGB(34, 51, 99)
//     T_MenuBtn_Hover.png      ring RGB(100, 48, 0)     (the palette rounds it to RGB(116, 58, 0))
//     T_TraceWordmark.png      white ink, amber halo, quoted by §5 as #FFBC78 .. #FFF2D6
//
// *** THERE IS NO BRIGHT BLUE ANYWHERE IN THE MENU ART. *** That is the finding that decides this
// section. The blue of the main menu is one colour and one colour only: the plate fill, a very dark
// navy, whose single brightest opaque pixel in the whole sprite is RGB(35, 51, 101). The amber is
// likewise a dark ring, RGB(116, 58, 0) — the brightness a player sees in the menu comes from the GLOW
// around it (the wordmark halo §5 quotes at #FFBC78..#FFF2D6), not from the value itself.
//
// ---------------------------------------------------------------------------------------------------
// THE TWO CANDIDATES, AND WHICH ONE SHIPS
// ---------------------------------------------------------------------------------------------------
// A. LITERAL      Blue = PlateFill        #1D2951    Amber = Amber         #743A00
// B. LIFTED       Blue = PlateFill lifted #5B81FF    Amber = AmberLifted   #FF8000     <-- SHIPPED
//
// (§5 quotes the hover ring as #643000, which is the RAW SAMPLE off the sheet; TraceMenuArtStyle::Amber
// is the palette's own rounded FColor(116, 58, 0) = #743A00. This file derives from the constant, so
// the constant's value is the one printed here and by Trace.Team.Report. The two are the same colour
// to within a rounding step and neither is usable as a team colour, so the choice below is unaffected.)
//
// Candidate A is the letter of the brief and is unusable, and this is arithmetic rather than taste.
// Relative luminance: #1D2951 = 0.025, #743A00 = 0.067, against #5B81FF = 0.25 and #FF8000 = 0.37.
// The arena's floor and its unlit structure sit in the 0.02-0.07 band — the same band as candidate A,
// not near it. A team mesh, a tracer and a trail in candidate A would be within a rounding error of
// the ground they are drawn on, and two teams that both read as "dark" is not a team colour; it is a
// bug report waiting to be filed, and it would be filed against the four systems listed above rather
// than against this line. The screenshots in the report show exactly this.
//
// Candidate B is what the menu LOOKS like. It is each sheet colour with its own hue and channel ratios
// kept exactly, normalised so its brightest channel reaches full — the transformation
// TraceMenuArtStyle::AmberLifted() already performs on the amber for exactly this reason ("the sheet's
// amber is a GLOW; anything that draws a FLAT shape in it ships as a dim brown smear at the sampled
// value"). A flat-shaded pawn on a dark field is the same problem as a 9 px selection rail, so it gets
// the same answer, and the amber half of the pair is literally the same call the wordmark's halo makes.
// Applying it to the navy gives #5B81FF, which is the plate's own hue at the brightness the menu
// composites it to.
//
// The owner can see candidate A without a rebuild: `Trace.Team.LiteralMenuColours 1` in the console,
// or -TraceExec="Trace.Team.LiteralMenuColours 1" on the command line. -DPCVars= was tried and does
// NOT reach it — measured, not assumed: the run logged "LIFTED" with the switch set that way, which is
// why the console form is the one documented here. Flipping it recolours the HUD, the tracers, the
// trails and every character mesh from the next frame; what it CANNOT recolour is Arena_Baked's cooked
// material instances, so the arena's own neon stays as baked and a candidate-A screenshot shows a
// normal-looking arena with an unreadable HUD on top of it. Both arms were photographed in a live
// match; see the report.
//
// ---------------------------------------------------------------------------------------------------
// WHAT IS DELIBERATELY *NOT* HERE
// ---------------------------------------------------------------------------------------------------
// The lift is duplicated logic — AmberLifted() does the same normalisation inside
// TraceMenuArtStyle.cpp. It is written out again below rather than hoisted into a shared
// TraceMenuArtStyle::Lifted(FLinearColor) because TraceMenuArtStyle is not this pass's to edit and two
// agents were working in that directory at the same time. THE AMBER HALF STILL CALLS AmberLifted()
// DIRECTLY, so the artist's amber has exactly one transformation in the codebase; only the navy uses
// the copy. If a later pass generalises AmberLifted, delete LiftToFullBrightness below and call it.

#include "TraceTypes.h"

#include "HAL/IConsoleManager.h"

#include "Trace.h"                                  // LogTraceGame
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"

// =================================================================================================
// SPEC v26 §5 — THE OWNER'S OVERRULE SWITCH
//
// 0 (shipped): candidate B, the menu's colours at the brightness the menu reads at.
// 1:           candidate A, the sampled bytes used literally. Almost black on a dark arena; this is
//              the arm the second screenshot in the report was taken with, and the reason B ships.
//
// Set it at runtime (`Trace.Team.LiteralMenuColours 1`) and the HUD, the tracers and the trails
// follow on the next frame. The arena's baked material instances do NOT — Arena_Baked carries them as
// cooked assets — so a full candidate-A arena needs a re-bake, not a cvar. Say so rather than let a
// screenshot imply the floor disagreed with the choice.
// =================================================================================================
static TAutoConsoleVariable<int32> CVarTeamLiteralMenuColours(
	TEXT("Trace.Team.LiteralMenuColours"), 0,
	TEXT("Spec v26 §5. 0 (shipped): the menu palette lifted to the brightness the menu reads at — "
	     "Blue #5B81FF, Amber #FF8000. 1: TraceMenuArtStyle's sampled bytes used literally — Blue "
	     "#1D2951, Amber #743A00, which is what the sheet says and is nearly black on a dark arena. "
	     "Trace.Team.Report prints whichever is live."),
	ECVF_Cheat);

// Named after the file rather than anonymous: two anonymous namespaces merged into one unity
// translation unit is MSVC C2084 on Windows only, and Scripts/check-jumbo-build-collisions.py gates
// on the name being here.
namespace TraceTypesFile
{
	/**
	 * @p SheetColour with its hue and channel ratios untouched and its brightest channel taken to
	 * full. See the header block: this is TraceMenuArtStyle::AmberLifted()'s transformation, applied
	 * to the navy that AmberLifted has no reason to know about.
	 *
	 * Normalised in sRGB BYTES rather than in linear, for AmberLifted's own stated reason: the ratio
	 * the eye and the sheet agree on is the byte one, and doing it in linear produces a visibly
	 * different, more saturated colour.
	 */
	static FLinearColor LiftToFullBrightness(const FLinearColor& SheetColour)
	{
		const FColor Bytes = SheetColour.ToFColor(/*bSRGB=*/true);
		const uint8 Peak = FMath::Max3(Bytes.R, Bytes.G, Bytes.B);
		if (Peak == 0)
		{
			return SheetColour;   // black has no hue to preserve and no brightest channel to raise
		}

		const float Scale = 255.f / static_cast<float>(Peak);
		const auto Lift = [Scale](uint8 InChannel)
		{
			return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(InChannel * Scale), 0, 255));
		};
		return FLinearColor::FromSRGBColor(FColor(Lift(Bytes.R), Lift(Bytes.G), Lift(Bytes.B), 255));
	}

	/** Opaque, whatever the palette constant carried. A team colour is never translucent. */
	static FLinearColor Opaque(const FLinearColor& In)
	{
		return FLinearColor(In.R, In.G, In.B, 1.0f);
	}
}

FLinearColor TraceTeamColor(ETraceTeam Team)
{
	// Resolved once, on first CALL — function-local statics rather than file-scope ones, so there is
	// no static-initialisation-order dependency on TraceMenuArtStyle's own file-scope constants, and
	// no sRGB<->linear conversion in the HUD's per-element draw loops (this is called hundreds of
	// times a frame by TraceHUD alone).
	static const FLinearColor LiftedBlue   = TraceTypesFile::LiftToFullBrightness(TraceMenuArtStyle::PlateFill);
	static const FLinearColor LiftedOrange = TraceTypesFile::Opaque(TraceMenuArtStyle::AmberLifted());
	static const FLinearColor LiteralBlue  = TraceTypesFile::Opaque(TraceMenuArtStyle::PlateFill);
	static const FLinearColor LiteralAmber = TraceTypesFile::Opaque(TraceMenuArtStyle::Amber);

	const bool bLiteral = CVarTeamLiteralMenuColours.GetValueOnAnyThread() != 0;

	switch (Team)
	{
	case ETraceTeam::Blue:   return bLiteral ? LiteralBlue  : LiftedBlue;
	case ETraceTeam::Orange: return bLiteral ? LiteralAmber : LiftedOrange;
	default:                 return FLinearColor(0.50f, 0.50f, 0.50f, 1.0f);
	}
}

FString TraceDescribeTeamColors()
{
	const FColor Blue = TraceTeamColor(ETraceTeam::Blue).ToFColor(/*bSRGB=*/true);
	const FColor Orange = TraceTeamColor(ETraceTeam::Orange).ToFColor(/*bSRGB=*/true);

	// THE SOURCE CONSTANTS ARE READ, NOT RETYPED, and this line has already caught itself out once:
	// it used to print "::Amber #643000", copied from the spec's measurement of the hover ring, while
	// TraceMenuArtStyle::Amber is FColor(116,58,0) = #743A00. A report that quotes a number the code
	// does not hold is worse than no report, because it is the thing a reader would check against.
	const FColor PlateFillBytes = TraceMenuArtStyle::PlateFill.ToFColor(/*bSRGB=*/true);
	const FColor AmberBytes = TraceMenuArtStyle::Amber.ToFColor(/*bSRGB=*/true);

	const bool bLiteral = CVarTeamLiteralMenuColours.GetValueOnAnyThread() != 0;

	return FString::Printf(
		TEXT("spec v26 §5 team palette: %s — Blue #%02X%02X%02X, Amber #%02X%02X%02X. Derived from ")
		TEXT("TraceMenuArtStyle::PlateFill #%02X%02X%02X and ::Amber #%02X%02X%02X; ")
		TEXT("`Trace.Team.LiteralMenuColours` shows the other candidate."),
		bLiteral ? TEXT("LITERAL (candidate A)") : TEXT("LIFTED (candidate B, shipped)"),
		Blue.R, Blue.G, Blue.B, Orange.R, Orange.G, Orange.B,
		PlateFillBytes.R, PlateFillBytes.G, PlateFillBytes.B,
		AmberBytes.R, AmberBytes.G, AmberBytes.B);
}

namespace TraceTypesFile
{
	static void ReportTeamColors()
	{
		UE_LOG(LogTraceGame, Display, TEXT("[TEAMCOLOUR] %s"), *TraceDescribeTeamColors());
	}

	static FAutoConsoleCommand CmdTeamReport(
		TEXT("Trace.Team.Report"),
		TEXT("Spec v26 §5. Prints the live team palette and the menu constants it is derived from."),
		FConsoleCommandDelegate::CreateStatic(&TraceTypesFile::ReportTeamColors));
}
