// Trace — see TraceMenuRowWidget.h.

#include "UI/Widgets/Menu/TraceMenuRowWidget.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"

#include "UI/Widgets/Menu/TraceMenuArtStyle.h"

// Named after the file, not anonymous — Scripts/check-jumbo-build-collisions.py, and the unity build
// it exists to protect.
namespace TraceMenuRowWidgetLocal
{
	// =============================================================================================
	// SPEC v20 §0.5 — THE SELECTED ROW MUST BE THE EASIEST THING ON THE SCREEN TO READ
	// =============================================================================================
	//
	// It was the hardest. Hover and selection are the same thing on this menu, so the artist's HOVER
	// word colour — TraceMenuArtStyle::WordHover, RGB(115,82,50), a muted gold they drew for a word
	// sitting on a lit plate — was carrying the SELECTION signal. Measured off the 1920x1080 capture:
	// the selected PLAY word rendered at RGB(114,83,53) on a plate at RGB(34,45,82), a contrast ratio
	// of 1.9:1, while every unselected row rendered pure white at 13.4:1. The row the player is about
	// to press was seven times harder to read than the six rows around it.
	//
	// This is a DELIBERATE DEPARTURE FROM THE SHEET, in the same spirit as WordDisabled's — which is
	// documented in TraceMenuArtStyle.h as exactly that. It is kept here, local to the row, rather
	// than changed there, because TraceMenuArtStyle is the ARTIST'S palette, sampled off their art to
	// the byte; the moment a value in it stops being what the sheet says, that file stops being
	// trustworthy. The artist's amber has not been thrown away either — it is still on the selected
	// row, in the ring of T_MenuBtn_Hover, which is where the sheet puts it.
	//
	// The three colours below are one system and have to be read together:
	//   * a selected row is pure white, the brightest text on the screen;
	//   * an unselected row is 85% white — plainly legible at 9.6:1, and quiet enough that the
	//     selected row is the one the eye lands on;
	//   * a disabled row is unchanged, still TraceMenuArtStyle::WordDisabled.

	/** Selected / pressed. Full white, and nothing else on this screen is brighter. */
	static const FLinearColor WordSelected = FLinearColor::White;

	/** Unselected. Dimmed just enough to lose the brightness contest with the selected row. */
	static const FLinearColor WordUnselected = FLinearColor(0.85f, 0.85f, 0.85f, 1.f);

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
	 * to full brightness keeps the ratio exactly (116:58:0 -> 255:127:0), so this is the artist's hue
	 * and only the level is this file's. It is the same move Scripts/slice-ui-assets.py's
	 * build_wordmark() makes with the artist's rim amber, and for the same reason.
	 *
	 * Not put in TraceMenuArtStyle: that file is the artist's palette sampled to the byte, and the
	 * moment a value in it stops being what the sheet says it stops being trustworthy. Same argument
	 * as WordSelected above.
	 */
	static const FLinearColor SelectionMarkColor = FLinearColor::FromSRGBColor(FColor(255, 127, 0));

	/**
	 * Rail size in the menu's 1080-tall reference space, so a 1080p window draws it 1:1 and every
	 * other resolution scales it through UMG's DPI curve.
	 *
	 * 9 x 40 against a 60 px row: two thirds of the row's height, which pairs it with the row without
	 * competing with the plate, and thick enough that it can never be mistaken for a text caret. The
	 * crescent it replaces was 20 x 26 of mostly empty box — this is roughly 3.5x the actual lit area
	 * in a shape that stays itself at any scale.
	 */
	static constexpr float MarkWidth = 9.f;
	static constexpr float MarkHeight = 40.f;

	/** Fully rounded ends: a radius of half the width turns the bar's caps into semicircles. */
	static constexpr float MarkRadius = MarkWidth * 0.5f;

	/** The rail, as a brush. Textureless: Slate fills the rounded box from TintColor. */
	static FSlateBrush MakeSelectionMarkBrush()
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.OutlineSettings = FSlateBrushOutlineSettings(MarkRadius);
		Brush.TintColor = FSlateColor(SelectionMarkColor);
		// The canvas slot that holds this image is AUTO-SIZED, so the brush's ImageSize IS the mark's
		// size on screen. Nothing else has to be told the rail got bigger.
		Brush.SetImageSize(FVector2f(MarkWidth, MarkHeight));
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
	// one the slot was measured for is the string that reaches the value chip first. The numbers are
	// the generator's own: WBP_MenuRow is authored 720 local units wide with ROW_PAD_X = 30, and the
	// value chip on a row that has one starts around 60% of the way across.
	//
	// The readout, the value and the arrows are left unbounded on purpose: they are right-aligned and
	// short, and a shrink-to-fit on a right-aligned string that never overflows is a size that
	// wobbles for no reason.
	constexpr float LabelMaxWidth = 400.f;

	AtlasLabels.Reset();
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, LabelText, LabelMaxWidth));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, StatusText));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, ValueText));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, LeftArrowText));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, RightArrowText));

	AtlasLabels.RemoveAll([](const FTraceAtlasLabel& Label) { return !Label.IsValid(); });
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

	// ---- The plate: which of the artist's three states is true right now ---------------------------
	//
	// Order matters, and it is the order a player would name them. Disabled outranks everything —
	// a row that cannot be pressed must not look pressable even while the cursor is on it. Pressed
	// outranks hover, because the press IS the hover plus a finger down.
	const bool bDisabled = !InView.bEnabled;
	const bool bPressed  = InView.bEnabled && InView.bPressed;
	const bool bHovered  = InView.bEnabled && !bPressed && InView.bSelected;

	if (PlateSprite != nullptr)
	{
		const FSlateBrush& Chosen = bDisabled ? PlateDisabledBrush
			: (bPressed || bHovered) ? PlateHoverBrush
			: PlateDefaultBrush;
		PlateSprite->SetBrush(Chosen);

		// The sheet has three plates and a press is not one of them, so pressed is the hover plate
		// dimmed. Marked as the stand-in it is; a fourth sprite would replace this line.
		//
		// The hover plate also breathes, at the same rate the Canvas row's selection bar always did,
		// so a selected row is alive on either renderer.
		float Tint = 1.f;
		if (bPressed)
		{
			Tint = TraceMenuArtStyle::PressedTint;
		}
		else if (bHovered)
		{
			// Breathes UPWARDS from parity. See TraceMenuRowWidgetLocal::SelectedPlateBase.
			Tint = TraceMenuRowWidgetLocal::SelectedPlateBase
				+ TraceMenuRowWidgetLocal::SelectedPlateSwing * FMath::Sin(InNow * PulseSpeed);
		}
		PlateSprite->SetColorAndOpacity(FLinearColor(Tint, Tint, Tint, 1.f));
	}

	// The word's colour is the whole difference between the artist's three states, which is why one
	// white sprite can serve all three and why the typed labels use the same three colours.
	//
	// Spec v20 §0.5: the selected state is the artist's PLATE with a white word rather than their
	// hover word colour, which measured 1.9:1 against the plate it sits on. The whole argument, and
	// what was kept of the sheet, is at the top of this file.
	const bool bSelectedNow = bPressed || bHovered;
	const FLinearColor WordColor = bDisabled ? TraceMenuArtStyle::WordDisabled
		: bSelectedNow ? TraceMenuRowWidgetLocal::WordSelected
		: TraceMenuRowWidgetLocal::WordUnselected;

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
			// object rather than a bar parked beside a pulsing button.
			const float MarkTint = bPressed ? TraceMenuArtStyle::PressedTint
				: TraceMenuRowWidgetLocal::SelectedPlateBase
					+ TraceMenuRowWidgetLocal::SelectedPlateSwing * FMath::Sin(InNow * PulseSpeed);
			ChevronSprite->SetColorAndOpacity(FLinearColor(MarkTint, MarkTint, MarkTint, 1.f));
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
			// Dimmer than the label on purpose: it is a readout, not the thing you press. But it
			// followed WordHover down with the label — "HOST 192.168.1.185:7777" on the selected PLAY
			// row was dim brown on blue — so the selected row's readout is lifted with its word.
			const float StatusAlpha = bDisabled ? TraceMenuRowWidgetLocal::StatusAlphaDisabled
				: bSelectedNow ? TraceMenuRowWidgetLocal::StatusAlphaSelected
				: TraceMenuRowWidgetLocal::StatusAlphaNormal;
			StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(
				WordColor.R, WordColor.G, WordColor.B, StatusAlpha)));
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

	const auto ApplyArrow = [this, &InView, &WordColor, bDisabled](UTextBlock* InArrow, bool bLive)
	{
		if (InArrow == nullptr)
		{
			return;
		}
		InArrow->SetVisibility(InView.bShowArrows ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (InView.bShowArrows)
		{
			InArrow->SetColorAndOpacity(FSlateColor(FLinearColor(WordColor.R, WordColor.G, WordColor.B,
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
