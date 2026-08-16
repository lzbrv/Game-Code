// Trace — the bottom-right HUD corner as a UMG widget. Spec v17 §4 (step 4b).

#include "UI/Widgets/HUD/TraceHudCornerWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Trace.h"                                   // LogTraceGame
#include "UI/Widgets/HUD/TraceHudStatusChipWidget.h"

/**
 * Named after the file, per the build contract's jumbo rule (spec v17 §0): an anonymous namespace
 * here would be concatenated with every other UI .cpp in the unity blob and collide on Windows.
 */
namespace TraceHudCornerWidgetFile
{
	/** Same colour, new alpha — TraceHUDStyle::WithAlpha, which lives in another translation unit. */
	static FLinearColor WithAlpha(const FLinearColor& InColor, float InAlpha)
	{
		return FLinearColor(InColor.R, InColor.G, InColor.B, InAlpha);
	}

	/** How much of the rounds colour the plate's border carries. DrawAmmoBlock's own two numbers. */
	static constexpr float PlateEdgeAlphaNormal = 0.30f;
	static constexpr float PlateEdgeAlphaBee = 0.55f;

	/** An unlit magazine tick: the rounds colour at 14%, so the strip's SHAPE survives an empty clip. */
	static constexpr float UnlitTickAlpha = 0.14f;
}

const TCHAR* UTraceHudCornerWidget::CornerBlueprintPath()
{
	// The _C suffix is the GENERATED CLASS, not the asset. LoadClass on the asset path alone returns
	// null, which would look exactly like a missing asset and send the HUD to Canvas for the wrong
	// reason — a failure mode worth naming, because "it silently fell back" is the one outcome
	// spec v17 §0.1 is written to prevent.
	return TEXT("/Game/Trace/UI/HUD/WBP_TraceHudCorner.WBP_TraceHudCorner_C");
}

const TCHAR* UTraceHudCornerWidget::ChipBlueprintPath()
{
	return TEXT("/Game/Trace/UI/HUD/WBP_TraceHudStatusChip.WBP_TraceHudStatusChip_C");
}

const TArray<FName>& UTraceHudCornerWidget::RequiredWidgetNames()
{
	// Written out rather than reflected off the class, for the same reason the chip's list is: the
	// generator authors a tree from this list, and a list derived from the same reflection the
	// binding uses could not catch a generator that simply forgot one.
	static const TArray<FName> Names = {
		TEXT("CornerStack"),
		TEXT("StatusStack"),
		TEXT("PlateOutline"),
		TEXT("PlateFill"),
		TEXT("AmmoLabelText"),
		TEXT("ReloadLabelText"),
		TEXT("MagazineTrough"),
		TEXT("MagazineStrip"),
		TEXT("ReloadBar"),
		TEXT("CountText"),
		TEXT("CapacityText"),
	};
	return Names;
}

void UTraceHudCornerWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// COLLAPSED UNTIL THE FIRST PRESENT, so a corner added to the viewport a frame before the HUD has
	// any state to put in it cannot flash an empty plate. HitTestInvisible for the rest of its life:
	// this thing sits over the pause menu's hit area and must never eat a click meant for RESUME.
	SetVisibility(ESlateVisibility::Collapsed);
}

bool UTraceHudCornerWidget::InitialiseCorner(FString& OutReason)
{
	OutReason.Reset();

	// ---- Every BindWidget property, by name -----------------------------------------------------
	//
	// The Widget Blueprint compiler already refuses to compile an asset that is missing one of these.
	// This is the SECOND check, and it exists because the first one only runs where a compiler does:
	// a cooked asset built by a stale generate, or one whose widget was renamed and re-saved by a
	// designer, arrives here with a null and no compiler in sight.
	if (CornerStack == nullptr)      { OutReason = TEXT("CornerStack");      return false; }
	if (StatusStack == nullptr)      { OutReason = TEXT("StatusStack");      return false; }
	if (PlateOutline == nullptr)     { OutReason = TEXT("PlateOutline");     return false; }
	if (PlateFill == nullptr)        { OutReason = TEXT("PlateFill");        return false; }
	if (AmmoLabelText == nullptr)    { OutReason = TEXT("AmmoLabelText");    return false; }
	if (ReloadLabelText == nullptr)  { OutReason = TEXT("ReloadLabelText");  return false; }
	if (MagazineTrough == nullptr)   { OutReason = TEXT("MagazineTrough");   return false; }
	if (MagazineStrip == nullptr)    { OutReason = TEXT("MagazineStrip");    return false; }
	if (ReloadBar == nullptr)        { OutReason = TEXT("ReloadBar");        return false; }
	if (CountText == nullptr)        { OutReason = TEXT("CountText");        return false; }
	if (CapacityText == nullptr)     { OutReason = TEXT("CapacityText");     return false; }

	// The corner has to be a child of a canvas panel: its bottom-right anchoring is the whole layout.
	if (Cast<UCanvasPanelSlot>(CornerStack->Slot) == nullptr)
	{
		OutReason = TEXT("CornerStack is not in a Canvas Panel (its bottom-right anchor is the layout)");
		return false;
	}

	// ---- The chip class -------------------------------------------------------------------------
	if (ChipWidgetClass == nullptr)
	{
		ChipWidgetClass = LoadClass<UTraceHudStatusChipWidget>(nullptr, ChipBlueprintPath());
	}
	if (ChipWidgetClass == nullptr)
	{
		OutReason = FString::Printf(TEXT("%s did not load"), ChipBlueprintPath());
		return false;
	}

	// ONE CHIP IS BUILT AND VALIDATED NOW, not on the first frame a player is poisoned. A chip asset
	// with a missing binding would otherwise pass adoption, sit dormant through a whole warm-up, and
	// then fail in the middle of a fight — after the HUD had already committed to the UMG path and
	// stopped drawing the Canvas one.
	UTraceHudStatusChipWidget* ProbeChip = CreateWidget<UTraceHudStatusChipWidget>(this, ChipWidgetClass);
	if (ProbeChip == nullptr)
	{
		OutReason = FString::Printf(TEXT("CreateWidget failed for %s"), ChipBlueprintPath());
		return false;
	}

	FString ChipUnbound;
	if (!ProbeChip->ValidateBindings(ChipUnbound))
	{
		OutReason = FString::Printf(TEXT("WBP_TraceHudStatusChip is missing '%s'"), *ChipUnbound);
		return false;
	}

	// Keep the probe: it becomes chip 0, so the validation costs one widget rather than one wasted one.
	if (UVerticalBoxSlot* ProbeSlot = StatusStack->AddChildToVerticalBox(ProbeChip))
	{
		ProbeSlot->SetPadding(FMargin(0.f, 0.f, 0.f, TraceHudCornerLayout::ChipGapDesignPx));
		ProbeSlot->SetHorizontalAlignment(HAlign_Fill);
	}
	ProbeChip->SetVisibility(ESlateVisibility::Collapsed);
	ChipPool.Add(ProbeChip);

	return true;
}

void UTraceHudCornerWidget::HideCorner()
{
	SetVisibility(ESlateVisibility::Collapsed);
}

void UTraceHudCornerWidget::ApplyCornerTransform(float InDesignScale, bool bInAmmoVisible)
{
	const float SafeScale = FMath::Clamp(InDesignScale, 0.05f, 8.f);

	if (UCanvasPanelSlot* CornerSlot = Cast<UCanvasPanelSlot>(CornerStack->Slot))
	{
		// Anchored to the viewport's bottom-right with an alignment of (1,1), so these offsets are
		// the margin, measured from the corner of the screen to the corner of the stack. They are
		// scaled because they are LAYOUT numbers and the render transform below does not move layout.
		//
		// The BOTTOM margin depends on whether the plate is up, and that is not a special case bolted
		// on: it is what the Canvas pass does. See NoPlateBottomMarginDesignPx.
		const float MarginX = TraceHudCornerLayout::StackMarginDesignPx * SafeScale;
		const float MarginY = (bInAmmoVisible
			? TraceHudCornerLayout::StackMarginDesignPx
			: TraceHudCornerLayout::NoPlateBottomMarginDesignPx) * SafeScale;

		CornerSlot->SetPosition(FVector2D(-MarginX, -MarginY));
	}

	// Pivot at the stack's own bottom-right, so growing the scale grows the corner UP AND LEFT from a
	// fixed point rather than sliding it off the screen.
	CornerStack->SetRenderTransformPivot(FVector2D(1.f, 1.f));
	CornerStack->SetRenderScale(FVector2D(SafeScale, SafeScale));
}

FTraceHudCornerPresented UTraceHudCornerWidget::PresentCorner(const FTraceHudCornerState& InState,
	float InDesignScale)
{
	FTraceHudCornerPresented Presented;

	SetVisibility(ESlateVisibility::HitTestInvisible);
	ApplyCornerTransform(InDesignScale, InState.bAmmoBlock);

	PresentChips(InState, Presented);
	PresentAmmo(InState, Presented);

	return Presented;
}

void UTraceHudCornerWidget::InstallAtlasLabels()
{
	if (bAtlasLabelsInstalled)
	{
		return;
	}
	bAtlasLabelsInstalled = true;

	// ETraceTextWeight::Hud — Erbaum Bold, spec v25 §4. This corner IS the in-match HUD; it is only
	// UMG rather than Canvas because spec v17 §4 moved it, and a player reading "24 / 30" here and a
	// clock two hundred pixels away in a different typeface would be the exact "two typefaces one
	// line apart" complaint spec v23 §A4 already answered once. ATraceHUD::DrawTextLeft and friends
	// take the same face from TraceHUDType::HudWeight().
	AtlasLabels.Reset();
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, CountText, 0.f, 1.f, ETraceTextWeight::Hud));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, CapacityText, 0.f, 1.f, ETraceTextWeight::Hud));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, AmmoLabelText, 0.f, 1.f, ETraceTextWeight::Hud));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, ReloadLabelText, 0.f, 1.f, ETraceTextWeight::Hud));
	AtlasLabels.RemoveAll([](const FTraceAtlasLabel& Label) { return !Label.IsValid(); });
}

void UTraceHudCornerWidget::PresentAmmo(const FTraceHudCornerState& InState,
	FTraceHudCornerPresented& OutPresented)
{
	// NO GUN, NO PLATE. The gate is UTraceWeaponComponent::ShouldShowAmmo(), decided once in
	// ATraceHUD::BuildCornerState and carried here — never re-derived, because a second definition of
	// "the carrier has no gun" is free to disagree with the one the weapon enforces.
	if (!InState.bAmmoBlock)
	{
		PlateOutline->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	PlateOutline->SetVisibility(ESlateVisibility::HitTestInvisible);

	// The plate's border takes the rounds colour, so a bee clip changes the plate as well as its
	// contents and the whole corner announces itself.
	PlateOutline->SetBrushColor(TraceHudCornerWidgetFile::WithAlpha(InState.RoundsColor,
		InState.bBeeClip ? TraceHudCornerWidgetFile::PlateEdgeAlphaBee
		                 : TraceHudCornerWidgetFile::PlateEdgeAlphaNormal));

	InstallAtlasLabels();

	CountText->SetText(FText::FromString(InState.CountText));
	CountText->SetColorAndOpacity(FSlateColor(InState.CountColor));
	CapacityText->SetText(FText::FromString(InState.CapacityText));

	AmmoLabelText->SetText(FText::FromString(InState.AmmoLabel));
	AmmoLabelText->SetColorAndOpacity(FSlateColor(InState.AmmoLabelColor));

	ReloadLabelText->SetText(FText::FromString(InState.RightLabel));
	ReloadLabelText->SetColorAndOpacity(FSlateColor(InState.RightLabelColor));

	// After all four SetTexts, before the magazine strip: this is what moves them onto the screen.
	TraceAtlasTextSwap::MirrorAll(AtlasLabels);

	// ---- The magazine strip ---------------------------------------------------------------------
	//
	// THREE SHAPES, NOT TWO. Thirty thin ticks, five fat pips, or — mid-reload — one filling bar. The
	// tick COUNT is what makes a bee clip unmistakable before any colour or word is read, and the
	// reload bar means "the gun is coming back" never has to be inferred from a number that is
	// briefly meaningless.
	if (InState.bReloading)
	{
		MagazineStrip->SetVisibility(ESlateVisibility::Collapsed);
		ReloadBar->SetVisibility(ESlateVisibility::HitTestInvisible);
		ReloadBar->SetFillColorAndOpacity(InState.ReloadBarColor);
		ReloadBar->SetPercent(FMath::Clamp(InState.ReloadFraction, 0.f, 1.f));

		OutPresented.bReloadBar = true;
	}
	else
	{
		ReloadBar->SetVisibility(ESlateVisibility::Collapsed);
		MagazineStrip->SetVisibility(ESlateVisibility::HitTestInvisible);

		const int32 TickCount = FMath::Clamp(InState.ClipCapacity, 0,
			TraceHudCornerLayout::MaxMagazineTicks);
		if (TickCount < InState.ClipCapacity)
		{
			// Loud rather than silent: the draw record will now disagree with the Canvas path, and a
			// disagreement nobody was told about is how a HUD ends up lying about a clip.
			UE_LOG(LogTraceGame, Warning,
				TEXT("[HUDUMG] clip capacity %d exceeds the %d-tick ceiling; the strip is CLAMPED and the "
				     "draw record will not match the Canvas corner."),
				InState.ClipCapacity, TraceHudCornerLayout::MaxMagazineTicks);
		}

		EnsureTicks(TickCount);

		const FLinearColor UnlitColor = TraceHudCornerWidgetFile::WithAlpha(InState.RoundsColor,
			TraceHudCornerWidgetFile::UnlitTickAlpha);

		for (int32 TickIndex = 0; TickIndex < TickCount; ++TickIndex)
		{
			const bool bTickLit = (TickIndex < InState.InClip);
			TickPool[TickIndex]->SetColorAndOpacity(bTickLit ? InState.RoundsColor : UnlitColor);

			// Counted where the pixel is emitted, exactly like the Canvas pass counts it: a strip that
			// computed a healthy clip and then showed nothing has to appear in the record as a zero.
			OutPresented.LitTicks += bTickLit ? 1 : 0;
		}
	}

	OutPresented.bAmmoBlock = true;
	OutPresented.bBeeClip = InState.bBeeClip;
	OutPresented.AmmoText = InState.CountText + InState.CapacityText;
}

void UTraceHudCornerWidget::PresentChips(const FTraceHudCornerState& InState,
	FTraceHudCornerPresented& OutPresented)
{
	const int32 ChipCount = InState.Chips.Num();

	// Grow the pool to fit. Chips are pooled rather than rebuilt because the stack changes every time
	// a poison ticks over a tenth of a second, and re-parenting six widgets a frame would allocate a
	// fresh slot object per chip per frame for a corner that looks identical.
	while (ChipPool.Num() < ChipCount)
	{
		UTraceHudStatusChipWidget* NewChip = CreateWidget<UTraceHudStatusChipWidget>(this, ChipWidgetClass);
		if (NewChip == nullptr)
		{
			break;
		}

		if (UVerticalBoxSlot* ChipSlot = StatusStack->AddChildToVerticalBox(NewChip))
		{
			ChipSlot->SetPadding(FMargin(
				TraceHudCornerLayout::ChipInsetDesignPx, 0.f,
				TraceHudCornerLayout::ChipInsetDesignPx, TraceHudCornerLayout::ChipGapDesignPx));
			ChipSlot->SetHorizontalAlignment(HAlign_Fill);
		}
		ChipPool.Add(NewChip);
	}

	// *** THE STACK IS FILLED FROM THE BOTTOM. *** Chips[0] is the one nearest the ammo (the Canvas
	// pass draws it first and lowest), and in a vertical box sitting ABOVE the plate the bottom-most
	// visible child is the LAST one — so chip k goes into pool slot (visible count - 1 - k). Filling
	// top-down instead would silently invert a priority order that spec v16 §2 fixes on purpose: the
	// things being done TO the player sit nearest the corner.
	const int32 Usable = FMath::Min(ChipCount, ChipPool.Num());

	for (int32 PoolIndex = 0; PoolIndex < ChipPool.Num(); ++PoolIndex)
	{
		if (PoolIndex >= Usable)
		{
			ChipPool[PoolIndex]->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	for (int32 ChipIndex = 0; ChipIndex < Usable; ++ChipIndex)
	{
		UTraceHudStatusChipWidget* ChipWidget = ChipPool[Usable - 1 - ChipIndex];
		OutPresented.Chips.Add(ChipWidget->ApplyChip(InState.Chips[ChipIndex]));
	}

	StatusStack->SetVisibility(Usable > 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void UTraceHudCornerWidget::EnsureTicks(int32 InCount)
{
	while (TickPool.Num() < InCount)
	{
		UImage* NewTick = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		if (NewTick == nullptr)
		{
			break;
		}

		// *** ONE PIXEL OF DESIRED SIZE, AND IT IS NOT COSMETIC. *** A UImage's desired size comes
		// from its brush, which defaults to 32x32, and a horizontal box's desired WIDTH is the sum of
		// its children's desired widths even when every one of them is set to Fill — Fill only decides
		// how the leftover space is shared. Thirty default ticks therefore asked for 960 units and
		// dragged the whole corner out to two and a half times its size. Measured, from a photograph;
		// the draw record was perfectly happy throughout. The real width comes from the Fill rule and
		// the plate's own SizeBox.
		//
		// SetDesiredSizeOverride, not SetBrushSize: the latter is UE_DEPRECATED(5.0) and MSVC raises
		// C4996 for it ("your project will no longer compile" on the next engine release). clang did
		// not warn, so this reached a teammate's Windows build. The override is also the more honest
		// call — the problem being solved IS the desired size, and SetBrushSize only moved it as a
		// side effect of resizing the brush.
		NewTick->SetDesiredSizeOverride(FVector2D(1.f, 1.f));

		if (UHorizontalBoxSlot* TickSlot = MagazineStrip->AddChildToHorizontalBox(NewTick))
		{
			// Every tick takes an equal share of the strip, which is what makes five bee pips FAT and
			// thirty ordinary ticks thin without either number being written down anywhere.
			TickSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			TickSlot->SetHorizontalAlignment(HAlign_Fill);
			TickSlot->SetVerticalAlignment(VAlign_Fill);

			// The gap goes on the left of every tick but the first, so N ticks leave exactly N-1 gaps
			// and the strip still spans the full block width.
			const float LeftGap = (TickPool.Num() > 0) ? TraceHudCornerLayout::TickGapDesignPx : 0.f;
			TickSlot->SetPadding(FMargin(LeftGap, 0.f, 0.f, 0.f));
		}
		TickPool.Add(NewTick);
	}

	for (int32 TickIndex = 0; TickIndex < TickPool.Num(); ++TickIndex)
	{
		TickPool[TickIndex]->SetVisibility(TickIndex < InCount
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}
}
