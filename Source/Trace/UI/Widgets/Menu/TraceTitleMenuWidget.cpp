// Trace — see TraceTitleMenuWidget.h.

#include "UI/Widgets/Menu/TraceTitleMenuWidget.h"

#include "Blueprint/SlateBlueprintLibrary.h"
#include "Components/Border.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"

#include "UI/Widgets/Menu/TraceMenuCanvasArt.h"
#include "UI/Widgets/Menu/TraceMenuPalette.h"
#include "UI/Widgets/Menu/TraceStrokeTextWidget.h"

void UTraceTitleMenuWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// Resolved once, in ETraceMenuRow order. The HUD checks the count against its own enum and
	// refuses to adopt the widget if they disagree — a seventh row in the asset with no C++ behind
	// it would otherwise be a row that draws and does nothing.
	OrderedRows.Reset();
	OrderedRows.Add(RowPlay);
	OrderedRows.Add(RowJoin);
	OrderedRows.Add(RowDifficulty);
	OrderedRows.Add(RowMode);
	OrderedRows.Add(RowSettings);
	OrderedRows.Add(RowQuit);
}

void UTraceTitleMenuWidget::ApplyView(const FTraceTitleMenuView& InView)
{
	const auto SetTextOn = [](UTextBlock* Block, const FString& InText)
	{
		if (Block != nullptr)
		{
			Block->SetText(FText::FromString(InText));
		}
	};

	// ---- Art -------------------------------------------------------------------------------------
	if (Backdrop != nullptr)
	{
		Backdrop->SetAnimationTime(InView.Now);
	}
	if (MenuCursor != nullptr)
	{
		MenuCursor->SetCursor(InView.bCursorVisible, InView.CursorFraction);
	}

	// ---- Headline --------------------------------------------------------------------------------
	SetTextOn(TaglineText, InView.Tagline);
	SetTextOn(AddressCaptionText, InView.AddressCaption);
	SetTextOn(AddressValueText, InView.AddressValue);

	if (PortWarningText != nullptr)
	{
		const bool bWarn = !InView.PortWarning.IsEmpty();
		PortWarningText->SetVisibility(bWarn ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bWarn)
		{
			PortWarningText->SetText(FText::FromString(InView.PortWarning));
		}
	}

	// ---- Rows ------------------------------------------------------------------------------------
	//
	// The console's WIDTH is the one piece of layout this class sets, and it is not a design decision
	// — it is the shipped clamp, reproduced. The Canvas path uses
	// min(viewport width * 0.52, 720 reference px), which is a MINIMUM and therefore cannot be
	// expressed by a canvas-panel anchor: an anchor gives you the fraction OR the fixed size, never
	// the smaller of the two. Everything else about this tree is in the .uasset where a designer can
	// reach it. Without this, the two renderers would place the rows differently on any window
	// narrower than about 1.28:1 — and `Trace.UI.VerifyMenu` would correctly call that a failure.
	SyncConsoleWidth();

	const int32 RowsToApply = FMath::Min(InView.RowCount, OrderedRows.Num());
	for (int32 Index = 0; Index < RowsToApply; ++Index)
	{
		if (UTraceMenuRow* Row = OrderedRows[Index])
		{
			Row->ApplyView(InView.Rows[Index], InView.Now);
		}
	}

	SetTextOn(BlurbText, InView.Blurb);
	SetTextOn(FooterKeysText, InView.FooterKeys);
	SetTextOn(FooterHintText, InView.FooterHint);

	// ---- Travel ----------------------------------------------------------------------------------
	if (TravelOverlay != nullptr)
	{
		TravelOverlay->SetVisibility(InView.bTravelVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (InView.bTravelVisible)
	{
		SetTextOn(TravelCaptionText, InView.TravelCaption);
		if (TravelHintText != nullptr)
		{
			const bool bHint = !InView.TravelHint.IsEmpty();
			TravelHintText->SetVisibility(bHint ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (bHint)
			{
				TravelHintText->SetText(FText::FromString(InView.TravelHint));
			}
		}
	}

	// ---- Failure ---------------------------------------------------------------------------------
	if (FailureBanner != nullptr)
	{
		FailureBanner->SetVisibility(InView.bFailureVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (InView.bFailureVisible)
		{
			// One render opacity on the whole banner rather than a fade baked into four colours: the
			// banner's fill, its two rails and both lines of type all fade together, which is what the
			// Canvas path spends five multiplications achieving.
			FailureBanner->SetRenderOpacity(InView.FailureFade);
			SetTextOn(FailureHeadlineText, InView.FailureHeadline);
			SetTextOn(FailureDetailText, InView.FailureDetail);
		}
	}
}

void UTraceTitleMenuWidget::SyncConsoleWidth()
{
	if (ConsolePanel == nullptr)
	{
		return;
	}

	UCanvasPanelSlot* PanelSlot = Cast<UCanvasPanelSlot>(ConsolePanel->Slot);
	if (PanelSlot == nullptr)
	{
		return;
	}

	// The widget's own geometry, in the local (DPI-scaled, 1080-tall) units the asset is authored
	// in. Zero on the first frame, before Slate has laid anything out.
	const FVector2D RootSize = GetCachedGeometry().GetLocalSize();
	if (RootSize.X <= 1.0)
	{
		return;
	}

	const float RowWidth = FMath::Min(static_cast<float>(RootSize.X) * TraceMenuStyle::PanelWidthFraction,
		TraceMenuStyle::PanelMaxWidth);
	const float PanelWidth = RowWidth + TraceMenuStyle::PanelPadX * 2.f;

	FVector2D PanelSize = PanelSlot->GetSize();
	if (!FMath::IsNearlyEqual(static_cast<float>(PanelSize.X), PanelWidth, 0.25f))
	{
		PanelSize.X = PanelWidth;
		PanelSlot->SetSize(PanelSize);
	}
}

bool UTraceTitleMenuWidget::GetRowViewportRect(int32 InRowIndex, FBox2D& OutRect) const
{
	if (!OrderedRows.IsValidIndex(InRowIndex))
	{
		return false;
	}

	const UTraceMenuRow* Row = OrderedRows[InRowIndex];
	if (Row == nullptr)
	{
		return false;
	}

	const FGeometry& RowGeometry = Row->GetCachedGeometry();
	const FVector2D LocalSize = RowGeometry.GetLocalSize();
	if (LocalSize.X <= 1.0 || LocalSize.Y <= 1.0)
	{
		// Never laid out yet. The very first frame, and only that frame.
		return false;
	}

	FVector2D TopLeftPixel = FVector2D::ZeroVector;
	FVector2D TopLeftViewport = FVector2D::ZeroVector;
	FVector2D BottomRightPixel = FVector2D::ZeroVector;
	FVector2D BottomRightViewport = FVector2D::ZeroVector;

	// PixelPosition, not ViewportPosition: the HUD compares these against
	// APlayerController::GetMousePosition, which is in viewport RESOLUTION units. ViewportPosition is
	// in DPI-scaled widget units and would be wrong by exactly the DPI scale — which is 1.0 at 1080p,
	// so getting this backwards would look correct on the developer's monitor and be wrong on
	// everybody else's. `Trace.UI.VerifyMenu` measures it instead of trusting it.
	USlateBlueprintLibrary::LocalToViewport(this, RowGeometry, FVector2D::ZeroVector, TopLeftPixel, TopLeftViewport);
	USlateBlueprintLibrary::LocalToViewport(this, RowGeometry, LocalSize, BottomRightPixel, BottomRightViewport);

	OutRect = FBox2D(TopLeftPixel, BottomRightPixel);
	return OutRect.bIsValid != 0;
}
