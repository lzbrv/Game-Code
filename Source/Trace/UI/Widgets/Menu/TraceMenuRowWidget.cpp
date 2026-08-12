// Trace — see TraceMenuRowWidget.h.

#include "UI/Widgets/Menu/TraceMenuRowWidget.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"

#include "UI/Widgets/Menu/TraceMenuArtStyle.h"

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

	// ONLY the rows whose word is on the sheet claim a word sprite — PLAY and SETTINGS. The other
	// four type their label in the stand-in font (see TraceMenuArtStyle.h) and have no sprite to
	// resolve, so counting one for them would report a permanent, unfixable miss.
	if (bLabelIsSprite)
	{
		Check(WordBrush.GetResourceObject() != nullptr, TEXT("word sprite"));
	}

	Check(ChevronSprite != nullptr && ChevronSprite->GetBrush().GetResourceObject() != nullptr,
		TEXT("selection chevron"));
	Check(ValueChip != nullptr && ValueChip->GetBrush().GetResourceObject() != nullptr,
		TEXT("value chip"));

	return Resolved;
}

void UTraceMenuRow::ApplyView(const FTraceMenuRowView& InView, float InNow)
{
	// Every BindWidget below is REQUIRED, so a null here means the Blueprint failed to compile and
	// the game is running an older generated class. Null-checked anyway: a menu that draws a partial
	// row is recoverable, a menu that crashes on the title screen is not.

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
			Tint = 0.88f + 0.12f * FMath::Sin(InNow * PulseSpeed);
		}
		PlateSprite->SetColorAndOpacity(FLinearColor(Tint, Tint, Tint, 1.f));
	}

	// The word's colour is the whole difference between the artist's three states, which is why one
	// white sprite can serve all three and why the typed labels use the same three colours.
	const FLinearColor WordColor = bDisabled ? TraceMenuArtStyle::WordDisabled
		: (bPressed || bHovered) ? TraceMenuArtStyle::WordHover
		: TraceMenuArtStyle::WordDefault;

	if (ChevronSprite != nullptr)
	{
		ChevronSprite->SetVisibility(InView.bSelected && InView.bEnabled
			? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
		ChevronSprite->SetColorAndOpacity(TraceMenuArtStyle::WordDefault);
	}

	// ---- The label: the artist's letterforms where the sheet has them, typed where it does not -----
	const bool bUseSprite = bLabelIsSprite && (WordSprite != nullptr)
		&& (WordBrush.GetResourceObject() != nullptr);

	if (WordSprite != nullptr)
	{
		WordSprite->SetVisibility(bUseSprite ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bUseSprite)
		{
			WordSprite->SetBrush(WordBrush);
			WordSprite->SetColorAndOpacity(WordColor);
		}
	}

	if (LabelText != nullptr)
	{
		LabelText->SetVisibility(bUseSprite ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
		if (!bUseSprite)
		{
			LabelText->SetText(FText::FromString(InView.Label));
			LabelText->SetColorAndOpacity(FSlateColor(WordColor));
		}
	}

	if (StatusText != nullptr)
	{
		const bool bShowStatus = !InView.Status.IsEmpty();
		StatusText->SetVisibility(bShowStatus ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowStatus)
		{
			// Dimmer than the label on purpose: it is a readout, not the thing you press.
			StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(
				WordColor.R, WordColor.G, WordColor.B, bDisabled ? 0.45f : 0.70f)));
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
}
