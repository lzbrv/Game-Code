// Trace — one status chip, as a UMG widget. Spec v17 §4 (step 4b).

#include "UI/Widgets/HUD/TraceHudStatusChipWidget.h"

#include "Components/Border.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"

/**
 * Named after the file, per the build contract's jumbo rule: an anonymous namespace here would be
 * concatenated with every other UI .cpp in the unity blob and collide on Windows.
 */
namespace TraceHudStatusChipWidgetFile
{
	/** Same colour, new alpha. The Canvas pass's TraceHUDStyle::WithAlpha, which lives in another TU. */
	static FLinearColor WithAlpha(const FLinearColor& InColor, float InAlpha)
	{
		return FLinearColor(InColor.R, InColor.G, InColor.B, InAlpha);
	}

	/** How much of the tint the outline carries. Matches DrawStatusChip's border exactly. */
	static constexpr float OutlineAlpha = 0.45f;

	/** The readout is the tint, very slightly knocked back. Matches DrawStatusChip exactly. */
	static constexpr float ReadoutAlpha = 0.95f;
}

const TArray<FName>& UTraceHudStatusChipWidget::RequiredWidgetNames()
{
	// Written out rather than reflected off the class: the generator needs this list to author a tree
	// that will bind, and a list derived from the same reflection the binding uses could not catch a
	// generator that simply forgot one. Two independent statements of the same fact is the point.
	static const TArray<FName> Names = {
		TEXT("ChipOutline"),
		TEXT("ChipFill"),
		TEXT("ColorTab"),
		TEXT("LabelText"),
		TEXT("ReadoutText"),
		TEXT("DrainBar"),
	};
	return Names;
}

bool UTraceHudStatusChipWidget::ValidateBindings(FString& OutMissing) const
{
	if (ChipOutline == nullptr) { OutMissing = TEXT("ChipOutline"); return false; }
	if (ChipFill == nullptr)    { OutMissing = TEXT("ChipFill");    return false; }
	if (ColorTab == nullptr)    { OutMissing = TEXT("ColorTab");    return false; }
	if (LabelText == nullptr)   { OutMissing = TEXT("LabelText");   return false; }
	if (ReadoutText == nullptr) { OutMissing = TEXT("ReadoutText"); return false; }
	if (DrainBar == nullptr)    { OutMissing = TEXT("DrainBar");    return false; }
	OutMissing.Reset();
	return true;
}

void UTraceHudStatusChipWidget::InstallAtlasLabels()
{
	if (bAtlasLabelsInstalled)
	{
		return;
	}
	bAtlasLabelsInstalled = true;

	AtlasLabels.Reset();
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, LabelText));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, ReadoutText));
	AtlasLabels.RemoveAll([](const FTraceAtlasLabel& Label) { return !Label.IsValid(); });
}

FString UTraceHudStatusChipWidget::ApplyChip(const FTraceHudCornerChip& InChip)
{
	FString Unbound;
	if (!ValidateBindings(Unbound))
	{
		// Never reached in practice — the corner refuses to adopt an asset whose bindings do not
		// resolve — but a chip that silently drew nothing would be invisible in the draw record too.
		return FString::Printf(TEXT("<chip widget unbound: %s>"), *Unbound);
	}

	SetVisibility(ESlateVisibility::HitTestInvisible);

	// The tint, on the three things that carry it. Everything else about the chip — the fill colour,
	// the trough behind the drain, the fonts, the paddings — is styling and stays in the asset.
	ChipOutline->SetBrushColor(TraceHudStatusChipWidgetFile::WithAlpha(
		InChip.Tint, TraceHudStatusChipWidgetFile::OutlineAlpha));
	ColorTab->SetColorAndOpacity(InChip.Tint);
	ReadoutText->SetColorAndOpacity(FSlateColor(TraceHudStatusChipWidgetFile::WithAlpha(
		InChip.Tint, TraceHudStatusChipWidgetFile::ReadoutAlpha)));
	DrainBar->SetFillColorAndOpacity(InChip.Tint);

	InstallAtlasLabels();

	LabelText->SetText(FText::FromString(InChip.Label));
	ReadoutText->SetText(FText::FromString(InChip.Readout));

	TraceAtlasTextSwap::MirrorAll(AtlasLabels);

	const float DrainFraction = FMath::Clamp(InChip.Fraction, 0.f, 1.f);
	DrainBar->SetPercent(DrainFraction);

	// The record line is the Canvas pass's, character for character, so the two paths' records can be
	// diffed rather than eyeballed. See ATraceHUD::DrawStatusChip.
	return FString::Printf(TEXT("%s | %s | drain=%.2f"), *InChip.Label, *InChip.Readout, DrainFraction);
}
