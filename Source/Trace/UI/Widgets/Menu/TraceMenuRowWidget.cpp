// Trace — see TraceMenuRowWidget.h.

#include "UI/Widgets/Menu/TraceMenuRowWidget.h"

#include "Blueprint/WidgetLayoutLibrary.h"      // GetViewportScale - the DPI factor, for the 2 px stroke
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "HAL/IConsoleManager.h"

#include "Trace.h"                              // LogTraceGame
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"
#include "UI/Widgets/Menu/TraceMenuPalette.h"   // TraceMenuStyle::RowHeight / RowPadX / PanelMaxWidth

// Named after the file, not anonymous — Scripts/check-jumbo-build-collisions.py, and the unity build
// it exists to protect.
namespace TraceMenuRowWidgetLocal
{
	// =============================================================================================
	// SPEC v24 §3 — THE HOVERED WORD IS THE ARTIST'S GREEN, AND IT IS THE SAME STATE AS THE OUTLINE
	// =============================================================================================
	//
	//     "The button's hover state doesn't fully work — the orange outline shows up while hovering,
	//      but not the green text from my assets"
	//
	// Both halves of that sentence were true, and they had different causes.
	//
	// WHY THE GREEN WAS NOT ON THE SCREEN. TraceMenuArtStyle::WordHover did not contain the artist's
	// hover colour. It contained sRGB(115,82,50) — the amber halo that pools around the lettering,
	// mis-sampled off the one plate on the sheet whose baked word is a dim state LEGEND rather than a
	// button label. Re-measured on the three plates that carry a real button label in the hover
	// state, the artist's word is a flat sRGB(85,107,47) on all three, to the byte; the full
	// measurement is in TraceMenuArtStyle.h. So spec v20 §0.5 — which found the value here illegible
	// at 1.9:1 and replaced it with white — was reacting to a bad number, and the row has drawn a
	// white word ever since. That departure is now retired: §0.5's reasoning was sound and its input
	// was wrong.
	//
	// WHY IT COULD DRIFT FROM THE OUTLINE AT ALL. The plate brush and the word colour used to be two
	// separate expressions over the same three booleans, a dozen lines apart. Nothing forced them to
	// agree; they simply happened to, until one of them was edited. §3 asks for the label to be
	// "driven from the same hover state that already drives the outline, so the two cannot disagree",
	// so there is now ONE function, VisualsFor(), with ONE switch, that returns the plate and the
	// word together. Disagreeing would take deleting a case.
	//
	// WHAT IS *NOT* THE ARTIST'S GREEN, AND WHY. The word is the label. Everything else on this row
	// is Trace's own furniture that the sheet has no opinion about — the address readout under PLAY
	// and JOIN, the DIFFICULTY / SCORING MODE value, the two arrows — and those keep the neutral
	// treatment they have today. That is a deliberate line, not an oversight: "HOST 192.168.1.185:7777"
	// is a string of digits a host reads out loud to somebody on the other end of a call, and putting
	// it at the green's 2.34:1 against the plate would cost real legibility for no instruction. §3
	// asks for the label's colour and it gets exactly that.
	//
	// THE ROW'S OWN NEUTRALS, for the furniture above. Unchanged from spec v20 §0.5:
	//   * on a selected row, full white;
	//   * on an unselected row, 85% white — plainly legible at 9.6:1 and quiet enough that the
	//     selected row is the one the eye lands on;
	//   * on a disabled row, TraceMenuArtStyle::WordDisabled.

	/** Furniture on a selected / pressed row. Full white. */
	static const FLinearColor FurnitureSelected = FLinearColor::White;

	/** Furniture on an unselected row. */
	static const FLinearColor FurnitureUnselected = FLinearColor(0.85f, 0.85f, 0.85f, 1.f);

	/**
	 * THE RED ARM for spec v24 §3. Non-zero puts the pre-v24 white word back on the hover state,
	 * leaving the plate, the ring and the rail exactly as they are — so ONE binary produces a "the
	 * outline lights but the word does not change" frame and a fixed one, and the pixel sample that
	 * says the label is green can be shown to be capable of saying it is white.
	 *
	 * File-scope and distinctively named rather than an anonymous-namespace static: UBT compiles this
	 * module as a unity build. Same reasoning as the namespace this sits in.
	 */
	static int32 GRowHoverWordRedArm = 0;

	static FAutoConsoleVariableRef CVarRowHoverWordRedArm(
		TEXT("Trace.UI.RowHoverWordRedArm"),
		GRowHoverWordRedArm,
		TEXT("Dev only. Spec v24 s3 RED ARM. 1 restores the pre-v24 white hover label, so the ")
		TEXT("artist's green can be measured against the defect it replaced from one build."),
		ECVF_Cheat);

	/**
	 * The plate's breathing tint on the selected row.
	 *
	 * It used to be 0.88 + 0.12*sin, i.e. a selected plate that spent most of its cycle DARKER than
	 * every unselected plate beside it — the same mistake as the word colour, one layer down, and it
	 * put up to 12% of noise into any before/after comparison of this row. Now it breathes upwards
	 * from parity, so the selected row is never the dim one at any phase.
	 */
	static constexpr float SelectedPlateBase = 1.f;
	static constexpr float SelectedPlateSwing = 0.10f;

	/** Alpha for the address readout on a row. Brighter on the selected row, as its label now is. */
	static constexpr float StatusAlphaSelected = 0.88f;
	static constexpr float StatusAlphaNormal = 0.70f;
	static constexpr float StatusAlphaDisabled = 0.45f;

	// =============================================================================================
	// SPEC v22 §A4 — THE SELECTION MARK IS NO LONGER THE ARTIST'S CRESCENT
	// =============================================================================================
	//
	// It was T_MenuBack drawn at 26 px, and at that size it read as a stray ')' floating in the gap
	// to the left of the row — measured off the 1920x1080 capture of the shipped default
	// (Saved/Screenshots/TraceAutoShot_Menu_20260813_085154_01.png), where the selected PLAY row's
	// mark is 20 x 26 of thin white stroke sitting 14 px outside a 707 x 60 plate.
	//
	// SCALING IT UP DOES NOT FIX IT, and that is why this is a different mark rather than a bigger
	// one. T_MenuBack is a BACK glyph: a crescent that tapers to nothing at both ends and whose belly
	// curves AWAY from the row it is supposed to indicate. Blown up it reads as a bigger parenthesis.
	// Nothing about it points, at any size, so no size is the fix.
	//
	// THE ARTIST'S MARK IS NOT THROWN AWAY. T_MenuBack is still the back glyph, and it is still drawn
	// as one by TraceOptionsMenu — this file is the only place that was borrowing it for a job it was
	// not drawn for. The sprite on disk is untouched, which also matters because the options menu
	// reads the same texture.
	//
	// WHAT REPLACES IT: a solid rounded RAIL on the row's leading edge, drawn by Slate itself as a
	// RoundedBox brush with NO texture at all. That choice is the whole point:
	//   * a solid has no small-size failure mode. The crescent failed because it is a tapered STROKE
	//     and a stroke loses its taper — i.e. its whole identity — as it shrinks. A filled bar at
	//     9 px wide is the same bar at 90;
	//   * it is resolution-independent for free. The mark is vector, so 1280x720 draws the same shape
	//     the 1920x1080 does rather than a resampled 20 px sprite;
	//   * it is unambiguous about WHICH row it marks in a way a floating glyph is not: it is vertically
	//     centred on the row and two thirds of its height, so it reads as that row's own edge rather
	//     than as a character parked in the gap.
	//
	// IT IS NOT FLUSH AGAINST THE PLATE, and that is measured rather than conceded. WBP_MenuRow's slot
	// (authored by Scripts/generate-menu-widgets.py, which this agent does not own) is auto-sized and
	// right-aligned 14 px outside the row, so the brush's ImageSize below IS the mark's size on screen
	// and the 14 px is the asset's. Profiling the selected plate's amber bloom across that gap on the
	// 1280x720 capture of the shipped default gives peak luminance 77 at the plate's lit edge, 30 at
	// 8 px out, 8 at 14 px out and exactly 0 from 16 px out. Amber-on-amber inside that bloom is the
	// one place a solid amber mark would lose its contrast, and the authored slot puts it on black
	// instead — so the mark stays where the asset puts it, and this comment is the reason why.
	//
	// It is deliberately SOLID AMBER rather than white. The sheet's own selection signal is the amber
	// ring on T_MenuBtn_Hover, so putting the mark in the same hue makes the ring and the rail read as
	// one selection system instead of two unrelated highlights sitting next to each other.

	/**
	 * The rail's colour: TraceMenuArtStyle::Amber's HUE at full brightness.
	 *
	 * The artist's amber is RGB(116,58,0) — the ring colour sampled off their sheet, where it is a
	 * GLOW and gets its brightness from the bloom around it rather than from the value itself. A flat
	 * 9 px bar has no bloom, so at the sampled value it would ship as a dim brown smear. Normalising
	 * to full brightness keeps the ratio exactly, so this is the artist's hue and only the level is
	 * not. It is the same move Scripts/slice-ui-assets.py's build_wordmark() makes with the artist's
	 * rim amber, and for the same reason.
	 *
	 * SPEC v24 §0, applied here: this used to be the literal FColor(255,127,0) with the derivation
	 * written out beside it in prose, i.e. an absolute standing in for a function of Amber, which
	 * would not have followed if Amber ever moved. It is now the function —
	 * TraceMenuArtStyle::AmberLifted() — and the title screen's wordmark glow, which carried a second
	 * and quietly WRONG copy of the same idea, now shares it.
	 *
	 * A FUNCTION-LOCAL static rather than a namespace-scope one, and that is not style. TraceMenuArtStyle
	 * ::Amber is a `static const FLinearColor` in a header, so every translation unit has its own
	 * dynamically-initialised copy, and AmberLifted() reads the one in TraceMenuArtStyle.cpp. A
	 * namespace-scope initialiser here would therefore be racing that TU's static init — and losing
	 * that race is SILENT: Amber would still be all-zero, AmberLifted's zero-peak guard would return
	 * it unchanged, and the selection rail would ship black on black. Deferring to first use puts the
	 * call after all static initialisation, where the answer cannot be wrong.
	 */
	static const FLinearColor& SelectionMarkColor()
	{
		static const FLinearColor Color = TraceMenuArtStyle::AmberLifted();
		return Color;
	}

	/**
	 * Rail size in the menu's 1080-tall reference space, so a 1080p window draws it 1:1 and every
	 * other resolution scales it through UMG's DPI curve.
	 *
	 * 9 x 40 against a 60 px row: two thirds of the row's height, which pairs it with the row without
	 * competing with the plate, and thick enough that it can never be mistaken for a text caret. The
	 * crescent it replaces was 20 x 26 of mostly empty box — this is roughly 3.5x the actual lit area
	 * in a shape that stays itself at any scale.
	 *
	 * SPEC v24 §0, applied here. Those two sentences describe the rail as a PROPORTION of the row —
	 * "two thirds of the row's height" — and then it was written down as the absolute 40, which stops
	 * being two thirds the moment TraceMenuStyle::RowHeight moves and quietly becomes a rail that is
	 * the wrong size for its row. Both dimensions are now expressed against RowHeight, so the mark
	 * follows the row instead of having to be re-tuned after it. At today's RowHeight of 60 these
	 * evaluate to exactly the 9 and 40 they replace, so nothing on screen moves in this pass.
	 */
	static constexpr float MarkHeightOfRow = 2.f / 3.f;
	static constexpr float MarkWidthOfRow = 0.15f;

	static constexpr float MarkWidth = TraceMenuStyle::RowHeight * MarkWidthOfRow;
	static constexpr float MarkHeight = TraceMenuStyle::RowHeight * MarkHeightOfRow;

	/** Fully rounded ends: a radius of half the width turns the bar's caps into semicircles. */
	static constexpr float MarkRadius = MarkWidth * 0.5f;

	// =============================================================================================
	// ONE STATE, ONE SWITCH — spec v24 §3's "so the two cannot disagree"
	// =============================================================================================

	/**
	 * The four states a row can be in, in the order a player would rank them.
	 *
	 * Disabled outranks everything: a row that cannot be pressed must not look pressable even while
	 * the cursor is on it. Pressed outranks hovered, because a press IS the hover plus a finger down.
	 * Hover and keyboard selection are the same thing on this menu — see ATraceMenuHUD::DrawHUD.
	 */
	enum class ERowState : uint8
	{
		Normal,
		Hovered,
		Pressed,
		Disabled,
	};

	static ERowState StateFor(const FTraceMenuRowView& InView)
	{
		if (!InView.bEnabled)          { return ERowState::Disabled; }
		if (InView.bPressed)           { return ERowState::Pressed; }
		if (InView.bSelected)          { return ERowState::Hovered; }
		return ERowState::Normal;
	}

	/**
	 * Everything one state decides, decided together.
	 *
	 * This struct is the whole of §3's guarantee. The orange outline is a property of @c Plate (it is
	 * baked into T_MenuBtn_Hover) and the green word is @c Label; they are produced by ONE switch
	 * over ONE value, so the only way to light the ring without turning the word green — the exact
	 * defect reported — is to delete a line from a single case.
	 */
	struct FRowVisuals
	{
		/** Which of the three plates the artist drew. The hover plate is the one with the ring. */
		enum class EPlate : uint8 { Default, Hover, Disabled } Plate = EPlate::Default;

		/** The label's colour, from the artist's palette. */
		FLinearColor Label = TraceMenuArtStyle::WordDefault;

		/** Neutral for the row's own furniture: the address readout, the value, the arrows. */
		FLinearColor Furniture = FurnitureUnselected;

		/** true while this state should breathe. Pressed does not; it is held. */
		bool bPulses = false;

		/** Flat multiplier applied to the plate and the rail when the state does not breathe. */
		float FlatTint = 1.f;

		/**
		 * SPEC v26 §7 — the white outline, on the DEFAULT state and nowhere else.
		 *
		 * A FIELD OF THIS STRUCT, not a separate `if (!bSelected)` twenty lines away, and that is the
		 * same argument §3 made for the label: "Hover keeps its amber ring; do not stack both" is a
		 * statement about two things never being true at once, and the cheapest way to guarantee it is
		 * to make them one value. The ring lives in EPlate::Hover and the stroke lives here, and the
		 * switch below sets each case's pair together — so a frame carrying both would take editing a
		 * case to say so out loud.
		 */
		bool bOutline = false;
	};

	static FRowVisuals VisualsFor(ERowState InState)
	{
		FRowVisuals Out;
		switch (InState)
		{
		case ERowState::Disabled:
			Out.Plate = FRowVisuals::EPlate::Disabled;
			Out.Label = TraceMenuArtStyle::WordDisabled;
			Out.Furniture = TraceMenuArtStyle::WordDisabled;
			break;

		case ERowState::Pressed:
			// The sheet has three plates and a press is not one of them, so pressed is the HOVER
			// plate — ring and all — knocked down. Marked as the stand-in it is; a fourth sprite
			// would replace this case and nothing else.
			Out.Plate = FRowVisuals::EPlate::Hover;
			Out.Label = TraceMenuArtStyle::WordHover;
			Out.Furniture = FurnitureSelected;
			Out.FlatTint = TraceMenuArtStyle::PressedTint;
			break;

		case ERowState::Hovered:
			// THE TWO HALVES OF §3, ON ADJACENT LINES. The ring comes from the plate; the green comes
			// from the artist's palette; neither can be true without the other.
			Out.Plate = FRowVisuals::EPlate::Hover;
			Out.Label = TraceMenuArtStyle::WordHover;
			Out.Furniture = FurnitureSelected;
			Out.bPulses = true;
			break;

		case ERowState::Normal:
		default:
			Out.Plate = FRowVisuals::EPlate::Default;
			Out.Label = TraceMenuArtStyle::WordDefault;
			Out.Furniture = FurnitureUnselected;
			// SPEC v26 §7. THE ONLY CASE THAT SETS IT. "The default button state (non-hover)" is this
			// case by name: Hovered and Pressed both wear the sheet's amber ring and must not stack a
			// second outline on it, and Disabled is not the default state — it is the artist's own
			// near-black plate with a grey ring, whose entire job is to say "you cannot press this",
			// and a bright white stroke around a dead row would be the loudest thing on the screen.
			Out.bOutline = true;
			break;
		}

		// The red arm sits AFTER the switch, deliberately: it must be able to break the pairing the
		// switch guarantees, because a red arm that cannot reproduce the defect proves nothing.
		if (GRowHoverWordRedArm != 0 && (InState == ERowState::Hovered || InState == ERowState::Pressed))
		{
			Out.Label = FurnitureSelected;
		}

		return Out;
	}

	/** The rail, as a brush. Textureless: Slate fills the rounded box from TintColor. */
	static FSlateBrush MakeSelectionMarkBrush()
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.OutlineSettings = FSlateBrushOutlineSettings(MarkRadius);
		Brush.TintColor = FSlateColor(SelectionMarkColor());
		// The canvas slot that holds this image is AUTO-SIZED, so the brush's ImageSize IS the mark's
		// size on screen. Nothing else has to be told the rail got bigger.
		Brush.SetImageSize(FVector2f(MarkWidth, MarkHeight));
		Brush.SetResourceObject(nullptr);
		return Brush;
	}

	// =============================================================================================
	// SPEC v26 §7 — THE DEFAULT STATE'S 2 PX WHITE OUTLINE
	// =============================================================================================
	//
	//     "Add a 2pixel white outline to the default button state (non-hover)"
	//
	// It is drawn as a textureless Slate RoundedBox laid exactly over the row rect, for the same
	// reason §A4's selection rail is one: a stroke is the shape with the worst small-size failure
	// mode there is, and a vector one has none. A 2 px sprite border would be resampled by the DPI
	// scale and turn into a grey smear at 720p and a soft 4 px band at 4K.
	//
	// THE ROW RECT IS WHERE THE ARTIST'S PLATE IS FITTED, which is why the outline needs no offsets of
	// its own. The plate sprite carries 128 sheet pixels of glow outside the plate and WBP_MenuRow's
	// slot pushes the image out by exactly that overhang (Scripts/generate-menu-widgets.py:
	// frame_glow_inset), so the plate lands on the row rectangle the mouse hit-tests. The stroke is
	// anchored to that same rectangle rather than to a copy of its numbers, so it follows if the row
	// is ever resized instead of being two values that happen to agree today.
	//
	// MEASURED, because "lands on" is not quite "coincides with": the sprite's own plate edge is soft,
	// and a column through a row's top edge shows the white stroke, then 1 px (720p) / 2 px (1080p) /
	// 4 px (1440p) of the sprite's near-black rim, then the navy fill. So the stroke sits on the
	// plate's outermost rim with a hairline of the artist's own dark edge inside it. That reads as a
	// keyline and is left alone deliberately — closing it would mean hand-tuning an inset against one
	// sprite's antialiasing, which is exactly the kind of number that stops being true after a
	// re-slice.

	/** White. §7 says white, and the label's default state is the same white (WordDefault). */
	static const FLinearColor& OutlineColor()
	{
		// Function-local, for the static-initialisation-order reason spelled out at
		// SelectionMarkColor(): TraceMenuArtStyle::WordDefault is a header-scope `static const
		// FLinearColor` and reading it from a namespace-scope initialiser here would be a race whose
		// failure mode is silent (an all-zero colour draws a black outline on a dark plate).
		static const FLinearColor Color = TraceMenuArtStyle::WordDefault;
		return Color;
	}

	/**
	 * The stroke's corner radius, in the row's LOCAL units.
	 *
	 * Derived, not chosen. TraceMenuArtStyle::ButtonFrame records the artist's plate as 4723 x 1230
	 * sheet px inside a crop with 128 px of glow on every side, with the corner curve fully open 428
	 * px in from the SPRITE's edge — i.e. 300 px in from the PLATE's edge, which for a circular corner
	 * is the radius. As a fraction of the plate's height that is 300/1230, and the row is
	 * TraceMenuStyle::RowHeight tall, so the radius is that fraction of the row. At today's 60 px row
	 * it evaluates to 14.6, and if either the artist re-cuts the sprite or the row changes height the
	 * outline's corner follows instead of having to be re-tuned to match a plate it no longer fits.
	 *
	 * IT SCALES WITH THE ROW AND THE WIDTH DOES NOT, and that asymmetry is deliberate: a corner is
	 * part of the button's SHAPE, so it must scale with the button, while the stroke is part of its
	 * FINISH and the owner asked for that in pixels.
	 */
	static float OutlineCornerRadius()
	{
		const TraceMenuArtStyle::FSpriteFrame& Frame = TraceMenuArtStyle::ButtonFrame;
		const float RadiusInPlatePixels = FMath::Max(0.f, Frame.Cap - Frame.Glow);

		// FMath::Max on the divisor rather than an early-out: ButtonFrame is a compile-time constant,
		// so clang folds any `if (PlateH <= 0)` guard away and then rejects the unreachable return
		// under -Wunreachable-code-return (this build treats it as an error). This form is safe
		// against a future edit that zeroes the plate height AND survives constant folding.
		return TraceMenuStyle::RowHeight
			* (RadiusInPlatePixels / FMath::Max(KINDA_SMALL_NUMBER, Frame.PlateH));
	}

	/**
	 * THE RED ARM for spec v26 §7. Non-zero takes DefaultOutlineWidthPx as LOCAL units instead of
	 * device pixels — i.e. it puts back the naive implementation, the one that writes 2 into a Slate
	 * brush and calls it two pixels.
	 *
	 * It exists because §7's requirement is not "draw an outline", it is "draw an outline that is
	 * still 2 px at 720p and at 4K", and a capture of the FIXED build alone cannot show that the
	 * measurement is capable of saying anything else. With this at 1 the same binary draws the defect
	 * — 1 px at 720p, 3 px at 1440p, 4 px on a 4K display — so the pixel counts either side of the
	 * fix come off one build, one harness and one measuring script.
	 *
	 * File-scope and distinctively named rather than an anonymous-namespace static, for the unity
	 * build. Same reasoning as GRowHoverWordRedArm above.
	 */
	static int32 GRowOutlineLocalUnitsRedArm = 0;

	static FAutoConsoleVariableRef CVarRowOutlineLocalUnitsRedArm(
		TEXT("Trace.UI.RowOutlineLocalUnitsRedArm"),
		GRowOutlineLocalUnitsRedArm,
		TEXT("Dev only. Spec v26 s7 RED ARM. 1 makes the default row outline's width LOCAL units ")
		TEXT("instead of device pixels, which is the resolution-dependent stroke the fix replaces. ")
		TEXT("MEASURED off this arm: 1 px at 1280x720 and 3 px at 2560x1440 against the fix's 2 px at ")
		TEXT("both; 4 px at 4K follows from the same DPI curve. Photograph both arms from one run."),
		ECVF_Cheat);

	/**
	 * The last LOCAL WIDTH a row reported, so the line below is printed once per distinct stroke
	 * rather than once per row. Six rows resolve the same width on the same frame; six identical
	 * lines would be noise, and noise in a log is how the one line that mattered gets skipped.
	 *
	 * Keyed on the width and NOT on the layout scale, and that is a correction rather than a
	 * preference: keying on the scale suppressed the red arm's own line, because flipping the arm
	 * changes the stroke without changing the resolution — so the one build that could photograph both
	 * arms could only log one of them.
	 */
	static float GLastLoggedOutlineLocalWidth = -1.f;

	/**
	 * Says, in one line, what §7's stroke actually came out as — the requested device width, the
	 * layout scale it was divided by, and the local width that produces it.
	 *
	 * It exists because "2 px" is a claim about PIXELS and everything in this file is in local units:
	 * without this, a log can only report the number somebody typed. With it, a capture at any
	 * resolution can be checked against the line the same run printed.
	 */
	static void LogOutlineWidthOnce(float RequestedWidth, float Scale, float LocalWidth)
	{
		if (FMath::IsNearlyEqual(GLastLoggedOutlineLocalWidth, LocalWidth, 0.001f))
		{
			return;
		}
		GLastLoggedOutlineLocalWidth = LocalWidth;

		UE_LOG(LogTraceGame, Display,
			TEXT("[MenuRow] Spec v26 s7 outline: asked for %.2f, layout scale %.4f -> %.3f local units ")
			TEXT("= %.2f device px%s. Corner radius %.2f local units, from the artist's plate ")
			TEXT("(ButtonFrame) and RowHeight %.0f."),
			RequestedWidth, Scale, LocalWidth, LocalWidth * Scale,
			(GRowOutlineLocalUnitsRedArm != 0) ? TEXT("  [RED ARM: width is LOCAL units]") : TEXT(""),
			OutlineCornerRadius(), TraceMenuStyle::RowHeight);
	}

	/**
	 * The outline brush for a stroke @p LocalWidth local units wide.
	 *
	 * The FILL is transparent — this is an outline, not a plate, and the artist's plate is already
	 * underneath it. Slate draws a rounded box's outline INSIDE the rectangle, so the stroke sits on
	 * the plate rather than in the gap between rows, and it never widens the row's footprint.
	 *
	 * Note that a rounded box's outline colour is taken from the brush and is NOT multiplied by the
	 * widget's ColorAndOpacity (SlateCore's DrawElementTypes.cpp, ET_RoundedBox), so this white is
	 * the white that reaches the screen and cannot be dimmed by a tint set somewhere else.
	 */
	static FSlateBrush MakeDefaultOutlineBrush(float LocalWidth)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.OutlineSettings = FSlateBrushOutlineSettings(
			OutlineCornerRadius(), FSlateColor(OutlineColor()), LocalWidth);
		Brush.TintColor = FSlateColor(FLinearColor(0.f, 0.f, 0.f, 0.f));
		Brush.SetResourceObject(nullptr);
		return Brush;
	}
}

int32 UTraceMenuRow::CountResolvedArt(int32& OutTotal, TArray<FString>& OutMissing, const FString& InLabel) const
{
	int32 Resolved = 0;

	const auto Check = [&OutTotal, &Resolved, &OutMissing, &InLabel](bool bHasTexture, const TCHAR* InWhat)
	{
		++OutTotal;
		if (bHasTexture)
		{
			++Resolved;
			return;
		}
		OutMissing.Add(FString::Printf(TEXT("%s — %s"), *InLabel, InWhat));
	};

	// The three states the artist drew. All three are checked even though only one is on screen at a
	// time: the hover and disabled plates are exactly the ones a still screenshot never catches.
	Check(PlateDefaultBrush.GetResourceObject() != nullptr, TEXT("default plate"));
	Check(PlateHoverBrush.GetResourceObject() != nullptr, TEXT("hover plate"));
	Check(PlateDisabledBrush.GetResourceObject() != nullptr, TEXT("disabled plate"));

	// THE WORD SPRITE IS NO LONGER ART THIS ROW DRAWS — spec v22 §A1 retired it, and every row now
	// types its word from the Sofachrome atlas instead (see ApplyView). It is deliberately NOT
	// counted: the sprite still resolves, so counting it would report a green slot for a texture that
	// is never on the screen, which is worse than not counting it at all. Two of the seven rows had
	// one; the other five never did, and the screen no longer knows the difference.
	//
	// What replaced it is not a sprite either, so there is nothing here to check in its place —
	// whether the typeface actually resolved is `Trace.Text.Report`'s job, not this counter's.

	// The selection mark is the one slot on this row that is NOT a sprite any more (spec v22 §A4), so
	// asking it for a resource object would report a permanent, unfixable miss the moment the rail is
	// applied. What is actually worth checking is that the slot will PAINT something: either the
	// artist's brush the asset still ships, or the rail that replaces it. A brush left at NoDrawType
	// draws nothing and still fails, which is the failure this check exists for.
	const bool bSelectionMarkDraws = ChevronSprite != nullptr
		&& (ChevronSprite->GetBrush().DrawAs == ESlateBrushDrawType::RoundedBox
			|| ChevronSprite->GetBrush().GetResourceObject() != nullptr);
	Check(bSelectionMarkDraws, TEXT("selection mark"));
	Check(ValueChip != nullptr && ValueChip->GetBrush().GetResourceObject() != nullptr,
		TEXT("value chip"));

	return Resolved;
}

void UTraceMenuRow::InstallAtlasLabels()
{
	if (bAtlasLabelsInstalled)
	{
		return;
	}
	bAtlasLabelsInstalled = true;

	// MaxWidth on the word only. It is the one string on this row whose length is not under this
	// project's control in the way the others are — "SCORING MODE" in a face a third wider than the
	// one the slot was measured for is the string that reaches the value chip first.
	//
	// The readout, the value and the arrows are left unbounded on purpose: they are right-aligned and
	// short, and a shrink-to-fit on a right-aligned string that never overflows is a size that
	// wobbles for no reason.
	//
	// SPEC v24 §0, applied here. This was the absolute 400, with a comment explaining that it came
	// from the row being 720 local units wide, its padding being 30, and the value chip starting
	// around 60% across — i.e. a number DERIVED from three constants that live in TraceMenuStyle and
	// then written down as a literal, which stops being true the moment any of the three moves. It is
	// now the arithmetic itself: 720 * 0.6 - 30 = 402, two pixels from the literal it replaces and
	// now attached to the things it was always a function of.
	constexpr float ValueChipStartFraction = 0.60f;
	constexpr float LabelMaxWidth =
		TraceMenuStyle::PanelMaxWidth * ValueChipStartFraction - TraceMenuStyle::RowPadX;

	AtlasLabels.Reset();
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, LabelText, LabelMaxWidth));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, StatusText));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, ValueText));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, LeftArrowText));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, RightArrowText));

	AtlasLabels.RemoveAll([](const FTraceAtlasLabel& Label) { return !Label.IsValid(); });
}

float UTraceMenuRow::LayoutScale() const
{
	// The row's OWN scale, once it has been painted at least once. GetLocalSize is the "has it been
	// painted" test: an unpainted widget's cached geometry is the identity, whose size is zero and
	// whose scale is a perfectly plausible-looking 1.0 — which at 720p would draw the first frame's
	// stroke 33% thin and, worse, would go on looking right in code review.
	const FGeometry& Cached = GetCachedGeometry();
	const float CachedScale = Cached.GetAccumulatedLayoutTransform().GetScale();
	if (Cached.GetLocalSize().X > 1.f && CachedScale > KINDA_SMALL_NUMBER)
	{
		return CachedScale;
	}

	// Before the first paint: the viewport's DPI factor, which is what the widget will be scaled by.
	const float ViewportScale = UWidgetLayoutLibrary::GetViewportScale(this);
	return (ViewportScale > KINDA_SMALL_NUMBER) ? ViewportScale : 1.f;
}

void UTraceMenuRow::InstallDefaultOutline()
{
	if (bDefaultOutlineInstalled)
	{
		return;
	}
	bDefaultOutlineInstalled = true;

	// Everything here is optional in the way the whole UMG migration is optional: if the tree is not
	// what this expects, DefaultOutline stays null, ApplyDefaultOutline becomes a no-op and the row
	// draws exactly what it drew before §7. A missing outline is a cosmetic regression; a crash on
	// the title screen is not recoverable.
	if (WidgetTree == nullptr || PlateSprite == nullptr)
	{
		return;
	}

	// The plate's own panel, so the stroke cannot end up in a different coordinate space from the
	// thing it is outlining. Found through the plate rather than by name for the same reason: the
	// name of the canvas lives in a generator this file does not own.
	UCanvasPanel* Canvas = Cast<UCanvasPanel>(PlateSprite->GetParent());
	if (Canvas == nullptr)
	{
		return;
	}

	UImage* Outline = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(),
		TEXT("DefaultOutline"));
	if (Outline == nullptr)
	{
		return;
	}

	// Never takes the mouse. ATraceMenuHUD hit-tests rows by their cached geometry and a widget that
	// swallowed a click over a row would break selection in a way that looks like the menu ignoring
	// the player — the same rule every label on this row follows.
	Outline->SetVisibility(ESlateVisibility::Collapsed);
	Outline->SetBrush(TraceMenuRowWidgetLocal::MakeDefaultOutlineBrush(0.f));

	UCanvasPanelSlot* OutlineSlot = Canvas->AddChildToCanvas(Outline);
	if (OutlineSlot == nullptr)
	{
		return;
	}

	// THE ROW RECT, exactly: anchored to all four corners with zero offsets. The plate sprite's own
	// slot is pushed OUTWARD by its glow overhang so that the artist's plate is fitted to this same
	// rectangle — see the block above MakeDefaultOutlineBrush, including what a column through the
	// edge actually measures.
	OutlineSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
	OutlineSlot->SetOffsets(FMargin(0.f, 0.f, 0.f, 0.f));
	OutlineSlot->SetAlignment(FVector2D(0.f, 0.f));
	OutlineSlot->SetAutoSize(false);

	// Above the plate (z 0), below the value chip (z 3) and the words (z 4). The stroke belongs to
	// the button, so it goes on the button; it must not be able to cross a label.
	OutlineSlot->SetZOrder(1);

	DefaultOutline = Outline;
}

void UTraceMenuRow::ApplyDefaultOutline(bool bWanted)
{
	if (DefaultOutline == nullptr)
	{
		return;
	}

	DefaultOutline->SetVisibility(bWanted
		? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	if (!bWanted)
	{
		return;
	}

	// ---- 2 DEVICE PIXELS, AT EVERY RESOLUTION (spec v26 §7) ---------------------------------------
	//
	// A Slate rounded box's outline Width is in LOCAL units and is scaled to the screen with the rest
	// of the widget, so the local width that produces a constant device width is the requested pixel
	// count DIVIDED BY the layout scale. The engine's default DPI curve makes that scale 0.666 at
	// 720p, 1.0 at 1080p, 1.333 at 1440p and 2.0 at 4K, which is exactly the ladder §7 says the
	// stroke must not ride: a hard-coded 2 here would be 1.3 px, 2 px, 2.7 px and 4 px.
	//
	// Stored as the device width and divided by the live scale, rather than stored per resolution: the
	// value that modifies the base follows the base.
	const float Scale = FMath::Max(KINDA_SMALL_NUMBER, LayoutScale());
	const float Requested = FMath::Max(0.f, DefaultOutlineWidthPx);

	// The red arm SKIPS the division, which is precisely the bug: it makes the brush's local width
	// equal to the requested number and lets the DPI scale multiply it on the way to the screen.
	const float LocalWidth = (TraceMenuRowWidgetLocal::GRowOutlineLocalUnitsRedArm != 0)
		? Requested
		: Requested / Scale;

	// Rebuilt only when it actually moves. SetBrush invalidates this widget's paint, and doing that
	// every frame for a value that changes only when the window is resized would put a needless
	// invalidation on all six rows of a screen that is otherwise almost entirely static.
	if (!FMath::IsNearlyEqual(DefaultOutlineLocalWidth, LocalWidth, 0.01f))
	{
		DefaultOutlineLocalWidth = LocalWidth;
		DefaultOutline->SetBrush(TraceMenuRowWidgetLocal::MakeDefaultOutlineBrush(LocalWidth));
		TraceMenuRowWidgetLocal::LogOutlineWidthOnce(Requested, Scale, LocalWidth);
	}
}

void UTraceMenuRow::ApplyView(const FTraceMenuRowView& InView, float InNow)
{
	// Every BindWidget below is REQUIRED, so a null here means the Blueprint failed to compile and
	// the game is running an older generated class. Null-checked anyway: a menu that draws a partial
	// row is recoverable, a menu that crashes on the title screen is not.

	// Spec v22 §A1. Done HERE, on the first frame the row is asked to draw, rather than in
	// NativeOnInitialized: a row inside WBP_TitleMenu's tree is initialised at a moment this class
	// does not control, and ApplyView is the one place it is certain the widget tree exists AND the
	// menu wants this row on screen. Latched, so it costs one branch a frame afterwards.
	InstallAtlasLabels();

	// Spec v26 §7, and here for the same reason: the widget tree is certain to exist by the time the
	// menu asks this row to draw, and nowhere earlier is.
	InstallDefaultOutline();

	// ---- ONE STATE, AND EVERYTHING THIS ROW LOOKS LIKE COMES OUT OF IT (spec v24 §3) --------------
	//
	// These two lines are the fix for "the orange outline shows up while hovering, but not the green
	// text". The ring lives in the plate and the green lives in the label, and both of them are now
	// fields of one struct returned by one switch over one value — so a frame where the outline is
	// lit and the word is not green is no longer expressible. See VisualsFor().
	const TraceMenuRowWidgetLocal::ERowState RowState = TraceMenuRowWidgetLocal::StateFor(InView);
	const TraceMenuRowWidgetLocal::FRowVisuals Visuals = TraceMenuRowWidgetLocal::VisualsFor(RowState);

	// Kept as a named boolean because several call sites below read better for it; it is now DERIVED
	// from the state rather than being a second, parallel decision.
	const bool bDisabled = RowState == TraceMenuRowWidgetLocal::ERowState::Disabled;

	// The plate's tint: the breathing that has always marked a hovered row, or the flat knock-down a
	// press and a disabled row use. One expression, so the rail below can share it and the two can
	// never fall out of phase.
	const float PlateTint = Visuals.bPulses
		? (TraceMenuRowWidgetLocal::SelectedPlateBase
			+ TraceMenuRowWidgetLocal::SelectedPlateSwing * FMath::Sin(InNow * PulseSpeed))
		: Visuals.FlatTint;

	if (PlateSprite != nullptr)
	{
		using EPlate = TraceMenuRowWidgetLocal::FRowVisuals::EPlate;
		const FSlateBrush& Chosen =
			  (Visuals.Plate == EPlate::Disabled) ? PlateDisabledBrush
			: (Visuals.Plate == EPlate::Hover)    ? PlateHoverBrush
			:                                       PlateDefaultBrush;
		PlateSprite->SetBrush(Chosen);
		PlateSprite->SetColorAndOpacity(FLinearColor(PlateTint, PlateTint, PlateTint, 1.f));
	}

	// SPEC v26 §7 — the white stroke, from THE SAME FRAME'S Visuals as the plate above. The hover
	// plate carries the artist's amber ring and Visuals.bOutline is false in every state that uses it,
	// so "do not stack both" is a property of the switch rather than of this call site.
	ApplyDefaultOutline(Visuals.bOutline);

	// The label's colour. THE SAME FRAME'S Visuals as the plate above — that identity is the whole of
	// §3, and it is why this is a field read rather than a second conditional.
	const FLinearColor WordColor = Visuals.Label;

	// The row's own furniture — the address readout, the value, the two arrows — keeps the neutral
	// treatment it has today rather than following the word into the artist's green. The reason is at
	// the top of this file, under "WHAT IS *NOT* THE ARTIST'S GREEN".
	const FLinearColor FurnitureColor = Visuals.Furniture;

	if (ChevronSprite != nullptr)
	{
		const bool bShowMark = InView.bSelected && InView.bEnabled;
		ChevronSprite->SetVisibility(bShowMark ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);

		if (bShowMark)
		{
			// Applied once rather than every frame: the brush never changes, and re-setting it would
			// invalidate this widget's paint every tick for nothing. The asset still ships the
			// artist's crescent in this slot, so this is also what swaps it out — see the argument
			// at the top of this file.
			if (!bSelectionMarkApplied)
			{
				ChevronSprite->SetBrush(TraceMenuRowWidgetLocal::MakeSelectionMarkBrush());
				bSelectionMarkApplied = true;
			}

			// The rail breathes with the plate it marks, off the same clock, so the two read as one
			// object rather than a bar parked beside a pulsing button. Literally the same value now,
			// rather than a second copy of the same expression — see PlateTint above.
			ChevronSprite->SetColorAndOpacity(FLinearColor(PlateTint, PlateTint, PlateTint, 1.f));
		}
	}

	// ---- The label: ONE TYPEFACE, and it is the artist's ------------------------------------------
	//
	// SPEC v22 §A1, the half of it the player sees: "retire the split ... set those labels as live
	// text like every other label so the whole screen is one typeface."
	//
	// Until this pass a row could spell its word one of two ways, and which one it got depended on
	// whether the artist had happened to bake that particular word into the sheet. PLAY and SETTINGS
	// were sprites in the artist's squared face; JOIN, PRACTICE, DIFFICULTY, SCORING MODE and QUIT
	// were Lato, one row apart from them, in the same column. There is a real Sofachrome renderer now
	// (UI/Text/), so the row types every word in the same letterforms and the sprite is retired.
	//
	// The sprite is RETIRED, NOT DELETED, which is what A1 asks for: WordBrush is still a UPROPERTY,
	// T_MenuWord_Play and friends are still in the repo and still in the asset, and bLabelIsSprite is
	// still authored per instance. Nothing about the art was thrown away — the row simply stopped
	// needing a picture of a word to be able to show one.
	if (WordSprite != nullptr)
	{
		WordSprite->SetVisibility(ESlateVisibility::Collapsed);
	}

	if (LabelText != nullptr)
	{
		LabelText->SetVisibility(ESlateVisibility::HitTestInvisible);
		LabelText->SetText(FText::FromString(InView.Label));
		LabelText->SetColorAndOpacity(FSlateColor(WordColor));
	}

	if (StatusText != nullptr)
	{
		const bool bShowStatus = !InView.Status.IsEmpty();
		StatusText->SetVisibility(bShowStatus ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowStatus)
		{
			// Dimmer than the label on purpose: it is a readout, not the thing you press. It draws in
			// the row's FURNITURE colour rather than the label's, which is what keeps
			// "HOST 192.168.1.185:7777" legible on the hovered row now that the label is the artist's
			// green — an address a host reads out loud is the last string on this screen that should
			// be sitting at 2.34:1.
			const float StatusAlpha =
				  (RowState == TraceMenuRowWidgetLocal::ERowState::Disabled) ? TraceMenuRowWidgetLocal::StatusAlphaDisabled
				: (RowState == TraceMenuRowWidgetLocal::ERowState::Normal)   ? TraceMenuRowWidgetLocal::StatusAlphaNormal
				:                                                             TraceMenuRowWidgetLocal::StatusAlphaSelected;
			StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(
				FurnitureColor.R, FurnitureColor.G, FurnitureColor.B, StatusAlpha)));
			StatusText->SetText(FText::FromString(InView.Status));
		}
	}

	const bool bShowValue = !InView.Value.IsEmpty();
	if (ValueChip != nullptr)
	{
		// The chip the artist drew beside their slider, reused here because it is the same job: a
		// framed box holding a value that changes. It appears only where there IS a value.
		ValueChip->SetVisibility(bShowValue ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowValue)
		{
			ValueChip->SetColorAndOpacity(FLinearColor(1.f, 1.f, 1.f, bDisabled ? 0.35f : 1.f));
		}
	}
	if (ValueText != nullptr)
	{
		ValueText->SetVisibility(bShowValue ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowValue)
		{
			ValueText->SetText(FText::FromString(InView.Value));
			ValueText->SetColorAndOpacity(FSlateColor(bDisabled
				? TraceMenuArtStyle::WordDisabled : InView.ValueColor));
		}
	}

	// Furniture, like the readout above: the arrows say which way the value can move and are not part
	// of the artist's three word states.
	const auto ApplyArrow = [this, &InView, &FurnitureColor, bDisabled](UTextBlock* InArrow, bool bLive)
	{
		if (InArrow == nullptr)
		{
			return;
		}
		InArrow->SetVisibility(InView.bShowArrows ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (InView.bShowArrows)
		{
			InArrow->SetColorAndOpacity(FSlateColor(FLinearColor(
				FurnitureColor.R, FurnitureColor.G, FurnitureColor.B,
				(bLive && !bDisabled) ? 0.95f : DisabledArrowAlpha)));
		}
	};
	ApplyArrow(LeftArrowText, InView.bCanLeft);
	ApplyArrow(RightArrowText, InView.bCanRight);

	// LAST, and it has to be last: every branch above writes to a UTextBlock that is now this row's
	// MODEL rather than what draws, so this is the line that moves a frame's worth of decisions onto
	// the screen. Putting it anywhere else would show the previous frame's word.
	TraceAtlasTextSwap::MirrorAll(AtlasLabels);
}
