// Trace — see TraceMenuRowWidget.h.

#include "UI/Widgets/Menu/TraceMenuRowWidget.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"

void UTraceMenuRow::ApplyView(const FTraceMenuRowView& InView, float InNow)
{
	// Every BindWidget below is REQUIRED, so a null here means the Blueprint failed to compile and
	// the game is running an older generated class. Null-checked anyway: a menu that draws a partial
	// row is recoverable, a menu that crashes on the title screen is not.
	const float Pulse = 0.72f + 0.28f * FMath::Sin(InNow * PulseSpeed);

	if (RowPlate != nullptr)
	{
		const float PlateAlpha = InView.bSelected ? SelectedPlateOpacity : UnselectedPlateOpacity;
		RowPlate->SetColorAndOpacity(FLinearColor(0.f, 0.02f, 0.04f, PlateAlpha));
	}

	const float EdgeAlpha = InView.bSelected ? SelectedEdgeOpacity : UnselectedEdgeOpacity;
	const FLinearColor EdgeColor(AccentColor.R, AccentColor.G, AccentColor.B, EdgeAlpha);
	if (TopEdge != nullptr)
	{
		TopEdge->SetColorAndOpacity(EdgeColor);
	}
	if (BottomEdge != nullptr)
	{
		BottomEdge->SetColorAndOpacity(EdgeColor);
	}

	if (SelectionWash != nullptr)
	{
		SelectionWash->SetColorAndOpacity(FLinearColor(AccentColor.R, AccentColor.G, AccentColor.B,
			InView.bSelected ? 0.10f * Pulse : 0.f));
	}
	if (SelectionBar != nullptr)
	{
		SelectionBar->SetColorAndOpacity(FLinearColor(AccentColor.R, AccentColor.G, AccentColor.B,
			InView.bSelected ? Pulse : 0.f));
	}
	if (ChevronText != nullptr)
	{
		ChevronText->SetVisibility(InView.bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
		ChevronText->SetColorAndOpacity(FSlateColor(AccentColor));
	}

	if (LabelText != nullptr)
	{
		LabelText->SetText(FText::FromString(InView.Label));
		LabelText->SetColorAndOpacity(FSlateColor(InView.bSelected ? SelectedLabelColor : UnselectedLabelColor));
	}

	if (StatusText != nullptr)
	{
		const bool bShowStatus = !InView.Status.IsEmpty();
		StatusText->SetVisibility(bShowStatus ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowStatus)
		{
			StatusText->SetText(FText::FromString(InView.Status));
			const FLinearColor StatusColor = InView.bSelected
				? FLinearColor(AccentColor.R, AccentColor.G, AccentColor.B, 0.95f)
				: FLinearColor(UnselectedLabelColor.R, UnselectedLabelColor.G, UnselectedLabelColor.B, 0.85f);
			StatusText->SetColorAndOpacity(FSlateColor(StatusColor));
		}
	}

	const bool bShowValue = !InView.Value.IsEmpty();
	if (ValueText != nullptr)
	{
		ValueText->SetVisibility(bShowValue ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowValue)
		{
			ValueText->SetText(FText::FromString(InView.Value));
			ValueText->SetColorAndOpacity(FSlateColor(InView.ValueColor));
		}
	}

	const auto ApplyArrow = [this, &InView](UTextBlock* Arrow, bool bLive)
	{
		if (Arrow == nullptr)
		{
			return;
		}
		Arrow->SetVisibility(InView.bShowArrows ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (InView.bShowArrows)
		{
			Arrow->SetColorAndOpacity(FSlateColor(FLinearColor(AccentColor.R, AccentColor.G, AccentColor.B,
				bLive ? 0.95f : DisabledArrowAlpha)));
		}
	};
	ApplyArrow(LeftArrowText, InView.bCanLeft);
	ApplyArrow(RightArrowText, InView.bCanRight);
}
