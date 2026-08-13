// Trace — see TraceAtlasTextSwap.h.

#include "UI/Text/TraceAtlasTextSwap.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"

#include "UI/Text/TraceAtlasTextWidget.h"

// Named after the file, not anonymous. UBT compiles this module as a unity build, so two files that
// each open `namespace { }` become ONE namespace holding both sets of definitions — and "CopySlot"
// is exactly the kind of name a second UI file would also want.
// Scripts/check-jumbo-build-collisions.py gates the build on this.
namespace TraceAtlasTextSwapFile
{
	/**
	 * Copies every layout property from one panel slot to another of the SAME class, by reflection.
	 *
	 * Reflection rather than a switch on slot type, and the difference is not style. A switch would
	 * have to name UCanvasPanelSlot, UHorizontalBoxSlot, UVerticalBoxSlot, UOverlaySlot, USizeBoxSlot
	 * and UBorderSlot, and would silently drop the layout of the seventh kind the day somebody puts a
	 * label in one — a label that would then draw in the top-left corner of its panel with no error
	 * anywhere. Iterating the properties cannot miss a slot type, and cannot miss a NEW property on a
	 * slot type either (UCanvasPanelSlot's layout lives in one FAnchorData struct; a hand-written
	 * copy that predates a field silently stops copying it).
	 *
	 * Content and Parent are the two properties that are NOT layout — they are the graph edges the
	 * panel just rebuilt — and copying either would point the new slot back at the old widget.
	 */
	static void CopySlotLayout(UPanelSlot* Dest, const UPanelSlot* Source)
	{
		if (Dest == nullptr || Source == nullptr || Dest->GetClass() != Source->GetClass())
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Source->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			const FName Name = Property->GetFName();
			if (Name == GET_MEMBER_NAME_CHECKED(UPanelSlot, Content)
				|| Name == GET_MEMBER_NAME_CHECKED(UPanelSlot, Parent))
			{
				continue;
			}

			Property->CopyCompleteValue(
				Property->ContainerPtrToValuePtr<void>(Dest),
				Property->ContainerPtrToValuePtr<const void>(Source));
		}

		// UCanvasPanelSlot in particular does nothing until this is called — the properties above are
		// the UObject's copy of the layout, and this is what pushes them into the live Slate slot.
		Dest->SynchronizeProperties();
	}

	/**
	 * The text block's authored justification, as the horizontal alignment the atlas widget wants.
	 *
	 * READ BY REFLECTION because UTextLayoutWidget::Justification has a setter and no getter — it is
	 * writable from Blueprint and from the generator, and not readable from C++ by name. The property
	 * is the same one Scripts/generate-menu-widgets.py sets ("justification"), so this returns what
	 * the ASSET says rather than a guess repeated here.
	 *
	 * Centre when it cannot be read, because every slot on this screen is auto-sized to its text and
	 * an auto-sized slot makes the answer moot; it matters only if somebody later gives a label a
	 * fixed-width slot.
	 */
	static EHorizontalAlignment AlignFromJustification(const UTextBlock* Source)
	{
		ETextJustify::Type Justify = ETextJustify::Center;

		if (Source != nullptr)
		{
			if (const FByteProperty* Property = CastField<FByteProperty>(
				Source->GetClass()->FindPropertyByName(TEXT("Justification"))))
			{
				Justify = static_cast<ETextJustify::Type>(
					Property->GetPropertyValue_InContainer(Source));
			}
		}

		switch (Justify)
		{
		case ETextJustify::Right:  return HAlign_Right;
		case ETextJustify::Center: return HAlign_Center;
		default:                   return HAlign_Left;
		}
	}
}

FTraceAtlasLabel TraceAtlasTextSwap::Install(UUserWidget* Owner, UTextBlock* Source,
	float InMaxWidth, float SizeScale)
{
	FTraceAtlasLabel Label;

	if (Owner == nullptr || Source == nullptr || Owner->WidgetTree == nullptr)
	{
		return Label;
	}

	UPanelWidget* Panel = Source->GetParent();
	if (Panel == nullptr)
	{
		// Already swapped (we detach the source), or a text block the asset never parented. Either
		// way there is nothing safe to do and the caller's own SetText line still runs.
		return Label;
	}

	const int32 Index = Panel->GetChildIndex(Source);
	if (Index == INDEX_NONE)
	{
		return Label;
	}

	// ---- Build the replacement from what the ASSET says, not from constants here -------------------
	const FSlateFontInfo SourceFont = Source->GetFont();
	const float BaseSize = FMath::Max(1.f, SourceFont.Size * FMath::Max(0.01f, SizeScale));

	UTraceAtlasText* Atlas = Owner->WidgetTree->ConstructWidget<UTraceAtlasText>(
		UTraceAtlasText::StaticClass(),
		FName(*(Source->GetName() + TEXT("_Sofachrome"))));
	if (Atlas == nullptr)
	{
		return Label;
	}

	Atlas->SetSize(BaseSize);
	Atlas->SetColor(Source->GetColorAndOpacity().GetSpecifiedColor());
	Atlas->SetText(Source->GetText().ToString());
	Atlas->HorizontalAlignment = TraceAtlasTextSwapFile::AlignFromJustification(Source);
	Atlas->VerticalAlignment = VAlign_Center;

	// A label never takes the mouse. The rows own hit-testing on this screen and a widget that ate a
	// click over a row's word would break selection in a way that looks like the menu ignoring you.
	Atlas->SetVisibility(ESlateVisibility::HitTestInvisible);

	// ---- Take the source's place, keeping every sibling where it was -------------------------------
	//
	// Rebuilt rather than appended. Appending is enough for a canvas panel (Z-order sorts it) and
	// WRONG for a horizontal box, where the child order IS the layout: the value row would come out
	// "NORMAL < >" instead of "< NORMAL >". Clearing and re-adding in the recorded order is the one
	// operation that is correct for both, and both UPanelWidget::AddChild and RemoveChildAt update
	// the live Slate panel, so this works before OR after the Slate tree has been built.
	TArray<UWidget*> Children;
	TArray<UPanelSlot*> OldSlots;
	const int32 Count = Panel->GetChildrenCount();
	Children.Reserve(Count);
	OldSlots.Reserve(Count);
	for (int32 I = 0; I < Count; ++I)
	{
		UWidget* Child = Panel->GetChildAt(I);
		Children.Add(Child);
		OldSlots.Add(Child != nullptr ? Child->Slot : nullptr);
	}

	// RemoveChildAt nulls the old slot's Parent and Content but leaves its LAYOUT properties intact,
	// which is exactly what CopySlotLayout reads back out below.
	Panel->ClearChildren();

	for (int32 I = 0; I < Children.Num(); ++I)
	{
		UWidget* ToAdd = (I == Index) ? static_cast<UWidget*>(Atlas) : Children[I];
		if (ToAdd == nullptr)
		{
			continue;
		}

		UPanelSlot* NewSlot = Panel->AddChild(ToAdd);
		TraceAtlasTextSwapFile::CopySlotLayout(NewSlot, OldSlots[I]);
	}

	// The source is now parentless and stays that way. It is still referenced by the owning widget's
	// BindWidget property, so it is not garbage, and SetText/SetColorAndOpacity/SetVisibility on it
	// all still work — which is the entire reason the menu code above did not have to change.
	Label.Source = Source;
	Label.Atlas = Atlas;
	Label.BaseSize = BaseSize;
	Label.MaxWidth = InMaxWidth;
	return Label;
}

void TraceAtlasTextSwap::Mirror(const FTraceAtlasLabel& Label)
{
	if (!Label.IsValid())
	{
		return;
	}

	UTextBlock* Source = Label.Source;
	UTraceAtlasText* Atlas = Label.Atlas;

	const FString Text = Source->GetText().ToString();
	if (Atlas->Text != Text)
	{
		Atlas->SetText(Text);
	}

	const FLinearColor Color = Source->GetColorAndOpacity().GetSpecifiedColor();
	if (!Atlas->Color.Equals(Color))
	{
		Atlas->SetColor(Color);
	}

	// The model's visibility is the menu's INTENT — "this row has no status", "the port warning is
	// not showing". It is read rather than mirrored blindly so a hidden model stays hidden and a
	// shown one comes back as HitTestInvisible rather than as something that can swallow a click.
	const ESlateVisibility Wanted = (Source->GetVisibility() == ESlateVisibility::Collapsed)
		? ESlateVisibility::Collapsed
		: (Source->GetVisibility() == ESlateVisibility::Hidden)
			? ESlateVisibility::Hidden
			: ESlateVisibility::HitTestInvisible;
	if (Atlas->GetVisibility() != Wanted)
	{
		Atlas->SetVisibility(Wanted);
	}

	// ---- Shrink to fit, measured ------------------------------------------------------------------
	//
	// Always from BaseSize, never from the size we set last frame: scaling a scaled size converges on
	// zero over a few hundred frames, and a menu whose longest label slowly evaporates is a much
	// harder bug to see than one that is simply too wide.
	float Size = Label.BaseSize;
	if (Label.MaxWidth > 1.f)
	{
		if (Atlas->Size != Size)
		{
			Atlas->SetSize(Size);
		}
		const float Measured = Atlas->MeasureWidth();
		if (Measured > Label.MaxWidth && Measured > 1.f)
		{
			Size = Label.BaseSize * (Label.MaxWidth / Measured);
		}
	}
	if (!FMath::IsNearlyEqual(Atlas->Size, Size, 0.01f))
	{
		Atlas->SetSize(Size);
	}
}

void TraceAtlasTextSwap::MirrorAll(const TArray<FTraceAtlasLabel>& Labels)
{
	for (const FTraceAtlasLabel& Label : Labels)
	{
		Mirror(Label);
	}
}

UWidget* TraceAtlasTextSwap::Live(const TArray<FTraceAtlasLabel>& Labels, UTextBlock* Source)
{
	if (Source == nullptr)
	{
		return nullptr;
	}

	for (const FTraceAtlasLabel& Label : Labels)
	{
		if (Label.Source == Source && Label.Atlas != nullptr)
		{
			return Label.Atlas;
		}
	}

	return Source;
}
