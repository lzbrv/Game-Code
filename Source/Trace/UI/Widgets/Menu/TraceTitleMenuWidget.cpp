// Trace — see TraceTitleMenuWidget.h.

#include "UI/Widgets/Menu/TraceTitleMenuWidget.h"

#include "Blueprint/SlateBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

#include "Trace.h"   // LogTraceGame
#include "UI/TraceHardwareCursor.h"
#include "UI/Text/TraceAtlasTextWidget.h"   // WP8.2 — the version label
#include "UI/Text/TraceText.h"
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"
#include "UI/Widgets/Menu/TraceMenuGridWidget.h"
#include "UI/Widgets/Menu/TraceMenuPalette.h"

// Named after the file, not anonymous: UBT concatenates .cpp files for the unity build and two
// anonymous namespaces become one. See Scripts/check-jumbo-build-collisions.py.
namespace TraceTitleMenuWidgetLocal
{
	// ---- The title block ---------------------------------------------------------------------------
	//
	// The composition constants (mark width/top, swoosh ratios, opacities) lived here until the
	// release pass. WP9 made the CANVAS title draw the same sprites at the same places, so they
	// moved to TraceTitleLayout in this class's header, where both renderers read ONE copy — the
	// full measurement note travels with them.

	// ---- The footer, once it is no longer sitting on the blurb ------------------------------------

	/** Reference px between the blurb's bottom and the line under it. */
	static constexpr float FooterGapBelowBlurb = 13.f;

	/** Reference px between the two footer lines. Matches the asset's authored 982.4 - 958.4. */
	static constexpr float FooterLineGap = 24.f;

	/** Extra air under the port-busy line, which is a notice and should not read as a key hint. */
	static constexpr float FooterGapAfterWarning = 7.f;

	/** The last line keeps at least this much air under it, whatever the console did. */
	static constexpr float FooterBottomMargin = 16.f;

	// ---- The lifted wordmark ---------------------------------------------------------------------

	/**
	 * Peak over-black luminance, 0..1, at or above which the sprite is left completely alone.
	 *
	 * THE SELF-DISABLING TEST. Today's crop peaks at 0.28 — that is the defect. A word lifted the way
	 * build_word() lifts PLAY and SETTINGS peaks at 1.0. Anything at 0.62 or brighter already reads
	 * on black and does not want a second opinion from this file.
	 */
	static constexpr float WordmarkLiftThreshold = 0.62f;

	/** The glyph body. Near-white, very slightly cool, matching the tagline's ink. */
	static const FColor WordmarkInk(242, 246, 255, 255);

	/**
	 * The glow the artist drew the mark with: TraceMenuArtStyle::Amber, at a strength that shows.
	 *
	 * SPEC v24 §0, applied here, and this one was not merely a stale literal — it was a WRONG one
	 * wearing a correct comment. The line read `FColor(255, 140, 40)` and claimed to be the artist's
	 * amber lifted, but sRGB(116,58,0) does not normalise to 255:140:40; it normalises to 255:128:0.
	 * The mark's halo had drifted a visible step toward yellow and no diff could ever have shown it,
	 * because the number and the sentence beside it were independent.
	 *
	 * It is now the derivation itself, shared with the menu row's selection rail — one expression of
	 * "the artist's amber, lifted", in TraceMenuArtStyle, so both follow if the sheet's amber is ever
	 * re-sampled. THE VISIBLE CHANGE IS SMALL AND IT IS REAL: the TRACE wordmark's outer glow moves
	 * from sRGB(255,140,40) to sRGB(255,128,0), i.e. back onto the artist's own hue.
	 *
	 * Function-local static for the static-initialisation-order reason spelled out at
	 * TraceMenuRowWidgetLocal::SelectionMarkColor: losing that race would be silent.
	 */
	static const FColor& WordmarkGlow()
	{
		static const FColor Color = TraceMenuArtStyle::AmberLifted().ToFColor(/*bSRGB=*/true);
		return Color;
	}

	/** Rec. 709 luminance of an sRGB byte triple, 0..1. Perceptual weighting, not an average. */
	static float ByteLuminance(const FColor& InColor)
	{
		return (0.2126f * InColor.R + 0.7152f * InColor.G + 0.0722f * InColor.B) / 255.f;
	}

	/**
	 * A BGRA8 copy of @p InTexture's top mip, or false if this build cannot reach the pixels.
	 *
	 * Two sources, in the order of how much they can be trusted:
	 *   1. the imported SOURCE art, which the editor keeps and which has never been compressed,
	 *      mip-dropped or thrown away after upload. This is the path that runs in-editor and in
	 *      -game off the editor binary, which is every way this screen is looked at today;
	 *   2. the cooked platform data, for a packaged build — present only while the CPU copy has not
	 *      been discarded, which is why the caller must treat failure as normal and fall back.
	 */
	static bool ReadTexturePixels(UTexture2D* InTexture, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		if (InTexture == nullptr)
		{
			return false;
		}

#if WITH_EDITORONLY_DATA
		if (InTexture->Source.IsValid() && InTexture->Source.GetFormat() == TSF_BGRA8)
		{
			TArray64<uint8> Raw;
			if (InTexture->Source.GetMipData(Raw, 0))
			{
				OutWidth = static_cast<int32>(InTexture->Source.GetSizeX());
				OutHeight = static_cast<int32>(InTexture->Source.GetSizeY());
				const int64 Expected = static_cast<int64>(OutWidth) * OutHeight * 4;
				if (OutWidth > 0 && OutHeight > 0 && Raw.Num() == Expected)
				{
					OutPixels.SetNumUninitialized(OutWidth * OutHeight);
					FMemory::Memcpy(OutPixels.GetData(), Raw.GetData(), Expected);
					return true;
				}
			}
		}
#endif

		FTexturePlatformData* PlatformData = InTexture->GetPlatformData();
		if (PlatformData == nullptr || PlatformData->Mips.Num() == 0 || PlatformData->PixelFormat != PF_B8G8R8A8)
		{
			return false;
		}

		FTexture2DMipMap& Mip = PlatformData->Mips[0];
		const int64 Expected = static_cast<int64>(Mip.SizeX) * Mip.SizeY * 4;
		if (Expected <= 0 || Mip.BulkData.GetBulkDataSize() != Expected)
		{
			return false;
		}

		bool bRead = false;
		if (const void* Data = Mip.BulkData.LockReadOnly())
		{
			OutWidth = Mip.SizeX;
			OutHeight = Mip.SizeY;
			OutPixels.SetNumUninitialized(OutWidth * OutHeight);
			FMemory::Memcpy(OutPixels.GetData(), Data, Expected);
			bRead = true;
		}
		Mip.BulkData.Unlock();
		return bRead;
	}

	/**
	 * One horizontal edge of @p InWidget in @p InRoot's local (reference-pixel) space.
	 *
	 * MEASURED, not recomputed: it is the same rectangle Slate drew with, which is the whole point —
	 * the collisions this file exists to fix were all somebody recomputing a position that Slate had
	 * already decided differently.
	 *
	 * @param InFraction 0 for the widget's top edge, 1 for its bottom.
	 * @return a negative number before the widget has ever been laid out, which is the first frame.
	 */
	static float LocalEdgeY(const FGeometry& InRoot, const UWidget* InWidget, float InFraction)
	{
		if (InWidget == nullptr)
		{
			return -1.f;
		}

		const FGeometry& Geometry = InWidget->GetCachedGeometry();
		const FVector2D Size = Geometry.GetLocalSize();
		if (Size.Y <= 1.0)
		{
			return -1.f;
		}

		const FVector2D Absolute = Geometry.LocalToAbsolute(FVector2D(0.0, Size.Y * InFraction));
		return static_cast<float>(InRoot.AbsoluteToLocal(Absolute).Y);
	}

	/**
	 * Puts @p InSlot at @p InPosition, and at @p InSize when that is positive — an auto-sizing slot
	 * (the two footer lines) passes a negative width to say "leave the size to Slate".
	 *
	 * Every write is guarded by a comparison because a SetPosition/SetSize invalidates layout: called
	 * unguarded from ApplyView this would re-run the canvas panel's arrange pass every single frame.
	 *
	 * @return true if anything actually moved.
	 */
	static bool SetSlotRect(UCanvasPanelSlot* InSlot, const FVector2D& InPosition, const FVector2D& InSize)
	{
		if (InSlot == nullptr)
		{
			return false;
		}

		bool bChanged = false;
		if (!InSlot->GetPosition().Equals(InPosition, 0.25))
		{
			InSlot->SetPosition(InPosition);
			bChanged = true;
		}
		if (InSize.X > 0.0 && !InSlot->GetSize().Equals(InSize, 0.25))
		{
			InSlot->SetSize(InSize);
			bChanged = true;
		}
		return bChanged;
	}
}

void UTraceTitleMenuWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// Resolved once, IN ETraceMenuRow ORDER — this list is the enum's mirror and the two are indexed
	// against each other, so a row inserted in the middle of the enum must be inserted here too. The
	// HUD checks the COUNT against its own enum and refuses to adopt the widget if they disagree, so
	// a row in the asset with no C++ behind it cannot become a row that draws and does nothing.
	//
	// That guard did its job when spec v19 §2 added PRACTICE: the regenerated asset carried seven
	// rows while this list still had six, and the menu fell back to the Canvas renderer with
	// "WBP_TitleMenu offers 6 row widget(s), C++ has 7" rather than drawing a broken screen.
	OrderedRows.Reset();
	OrderedRows.Add(RowPlay);
	OrderedRows.Add(RowJoin);
	OrderedRows.Add(RowPractice);
	OrderedRows.Add(RowDifficulty);
	OrderedRows.Add(RowSettings);
	OrderedRows.Add(RowQuit);

	// Once, here rather than in ApplyView: it reads a texture and builds another one, and the answer
	// cannot change while the game is running. See the header for why it is done in code at all.
	LiftWordmarkFromSprite();

	// Spec v23 §A1. Here rather than in ApplyView because NativeOnInitialized runs BEFORE the Slate
	// tree is built (UUserWidget::Initialize duplicates the tree and binds the widgets, then calls
	// this; RebuildWidget happens later, on AddToViewport). Adding the child now means the canvas is
	// arranged once, with the grid already in it, instead of being torn down and re-arranged on the
	// first frame the title screen is visible.
	InstallGridBackground();

	// WP8.2, same timing argument as the grid.
	InstallVersionLabel();
}

void UTraceTitleMenuWidget::InstallGridBackground()
{
	if (GridFloor != nullptr)
	{
		return;
	}

	if (Backdrop == nullptr || WidgetTree == nullptr)
	{
		return;
	}

	// The grid's parent is BACKDROP'S parent, resolved from the tree rather than assumed to be
	// "RootCanvas": the one relationship that has to hold is "directly above the black fill", and
	// asking the black fill where it lives is the only way to state that without naming a widget
	// this class has no BindWidget contract with.
	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(Backdrop->GetParent());
	UCanvasPanelSlot* BackdropSlot = Cast<UCanvasPanelSlot>(Backdrop->Slot);
	if (RootCanvas == nullptr || BackdropSlot == nullptr)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[MenuGrid] Backdrop is not on a canvas panel, so there is no z-order this widget can ")
			TEXT("prove it is below. The grid is NOT installed; the title screen draws on flat black, ")
			TEXT("exactly as it did before spec v23."));
		return;
	}

	UTraceMenuGrid* Grid = WidgetTree->ConstructWidget<UTraceMenuGrid>(UTraceMenuGrid::StaticClass(), TEXT("GridFloor"));
	if (Grid == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[MenuGrid] Could not construct the grid widget; drawing flat black."));
		return;
	}

	UCanvasPanelSlot* GridSlot = RootCanvas->AddChildToCanvas(Grid);
	if (GridSlot == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[MenuGrid] Could not slot the grid widget; drawing flat black."));
		return;
	}

	// Full bleed. Anchors rather than a size, so this is right at every window size and every DPI
	// scale without the widget knowing either — the same rule the rest of this class works under.
	GridSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
	GridSlot->SetOffsets(FMargin(0.f, 0.f, 0.f, 0.f));
	GridSlot->SetAlignment(FVector2D::ZeroVector);

	// *** THE FOUR LINES THAT KEEP THE GRID BEHIND EVERY WORD ON THIS SCREEN. ***
	//
	// The obvious version of this — give the grid the Backdrop's own z-order and let it sort in
	// afterwards — was written first and IS NOT SAFE, for a reason that is only visible in the engine
	// source. Two ways equal z-orders lose their order:
	//
	//   1. SConstraintCanvas::Construct's sort comparator breaks a z-order TIE on the slots' MEMORY
	//      ADDRESSES (Widgets/Layout/SConstraintCanvas.cpp: "AZOrder == BZOrder ?
	//      reinterpret_cast<UPTRINT>(&A) < reinterpret_cast<UPTRINT>(&B) : ..."), not on insertion
	//      order;
	//   2. with the project setting bExplicitCanvasChildZOrder ON, children sharing a z-order are
	//      deliberately arranged into ONE Slate layer so the renderer can batch them, and their draw
	//      order inside it stops being a layout property at all. It is off in this project today —
	//      Config/ does not set it — which is exactly the kind of fact that changes without anyone
	//      thinking about this file.
	//
	// A tie decided by an allocator is a background that draws over the menu on some launches and not
	// others. So there are NO TIES: the grid takes one z-order below the lowest sibling it must stay
	// behind, and the Backdrop is pushed one below THAT. Derived from the siblings rather than typed
	// in, so re-running Scripts/generate-menu-widgets.py with different numbers cannot break it.
	int32 LowestSibling = MAX_int32;
	for (int32 Index = 0; Index < RootCanvas->GetChildrenCount(); ++Index)
	{
		// Non-const on purpose: Cast<> through a const UWidget's TObjectPtr member is one of the
		// resolutions clang takes and MSVC has been fussier about, and this file cannot be compiled
		// here for Windows (spec v23 Track C). Nothing below writes through it.
		UWidget* Sibling = RootCanvas->GetChildAt(Index);
		if (Sibling == nullptr || Sibling == Grid || Sibling == Backdrop)
		{
			continue;
		}
		if (UCanvasPanelSlot* SiblingSlot = Cast<UCanvasPanelSlot>(Sibling->Slot))
		{
			LowestSibling = FMath::Min(LowestSibling, SiblingSlot->GetZOrder());
		}
	}
	if (LowestSibling == MAX_int32)
	{
		// Nothing but the Backdrop on this canvas. Cannot happen with the generated asset; if it ever
		// does, the grid still has to end up above the black fill and below whatever arrives later.
		LowestSibling = BackdropSlot->GetZOrder() + 2;
	}

	const int32 GridZ = LowestSibling - 1;
	GridSlot->SetZOrder(GridZ);
	BackdropSlot->SetZOrder(GridZ - 1);
	GridFloor = Grid;

	UE_LOG(LogTraceGame, Display,
		TEXT("[MenuGrid] Installed: Backdrop z=%d < grid z=%d < lowest other sibling z=%d, %d sibling(s) ")
		TEXT("on the root canvas. %s"),
		BackdropSlot->GetZOrder(), GridZ, LowestSibling, RootCanvas->GetChildrenCount(),
		*UTraceMenuGrid::Describe());
}

void UTraceTitleMenuWidget::InstallVersionLabel()
{
	if (VersionLabel != nullptr)
	{
		return;
	}

	if (Backdrop == nullptr || WidgetTree == nullptr)
	{
		return;
	}

	// Same parent-resolution move as InstallGridBackground: ask the black fill where it lives.
	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(Backdrop->GetParent());
	if (RootCanvas == nullptr)
	{
		return;
	}

	UTraceAtlasText* Label = WidgetTree->ConstructWidget<UTraceAtlasText>(
		UTraceAtlasText::StaticClass(), TEXT("VersionText"));
	if (Label == nullptr)
	{
		return;
	}

	// FS_FOOTER x 0.9 and ink-dim at 0.55: a build identifier for screenshots and bug reports, and
	// deliberately the quietest type on the screen. The Canvas path draws the same string at the
	// same corner (ATraceMenuHUD::DrawVersionString).
	Label->SetSize(11.7f);
	Label->SetColor(TraceMenuStyle::WithAlpha(TraceMenuStyle::InkDim, 0.55f));
	Label->SetVisibility(ESlateVisibility::HitTestInvisible);

	UCanvasPanelSlot* LabelSlot = RootCanvas->AddChildToCanvas(Label);
	if (LabelSlot == nullptr)
	{
		return;
	}

	// Bottom-right, in the same reference pixels as the Canvas draw (right edge -24, and -14 puts
	// the ink where a FontSmall x 0.9 line drawn at ViewH - 28 ends up). Anchored so every window
	// size and DPI scale agrees without this widget knowing either.
	LabelSlot->SetAnchors(FAnchors(1.f, 1.f, 1.f, 1.f));
	LabelSlot->SetAlignment(FVector2D(1.0, 1.0));
	LabelSlot->SetAutoSize(true);
	LabelSlot->SetPosition(FVector2D(-24.0, -14.0));

	// Above every static layer (footer z 6), below the cursor and the travel/failure overlays, and
	// NOT a tie with anything — a z-order tie is broken by allocator addresses (the measured hazard
	// InstallGridBackground documents). Derived from the travel overlay's own slot so a regenerated
	// asset moves this with it.
	int32 LabelZ = 7;
	if (TravelOverlay != nullptr)
	{
		if (const UCanvasPanelSlot* TravelSlot = Cast<UCanvasPanelSlot>(TravelOverlay->Slot))
		{
			LabelZ = TravelSlot->GetZOrder() - 2;
		}
	}
	LabelSlot->SetZOrder(LabelZ);

	VersionLabel = Label;
}

void UTraceTitleMenuWidget::InstallAtlasLabels()
{
	if (bAtlasLabelsInstalled)
	{
		return;
	}
	bAtlasLabelsInstalled = true;

	// The three strings that run the full width of the screen get a fit bound; nothing else does.
	// These are LOCAL units, and the root canvas of WBP_TitleMenu is 1920 of them wide at any window
	// size (UMG's DPI curve keeps the reference resolution fixed), so 1760 is the screen with a 80-px
	// margin down each side. The footer hint is the longest string on the title screen and it is the
	// one Sofachrome's extra width would have pushed off the edge.
	constexpr float FullWidthFit = 1760.f;

	AtlasLabels.Reset();
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, TaglineText, FullWidthFit));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, AddressCaptionText));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, AddressValueText));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, PortWarningText, FullWidthFit));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, BlurbText, FullWidthFit));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, FooterKeysText, FullWidthFit));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, FooterHintText, FullWidthFit));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, TravelCaptionText, FullWidthFit));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, TravelHintText, FullWidthFit));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, FailureHeadlineText, FullWidthFit));
	AtlasLabels.Add(TraceAtlasTextSwap::Install(this, FailureDetailText, FullWidthFit));

	AtlasLabels.RemoveAll([](const FTraceAtlasLabel& Label) { return !Label.IsValid(); });

	UE_LOG(LogTemp, Log, TEXT("[MenuText] Title screen: %d label(s) moved onto the glyph atlas. %s"),
		AtlasLabels.Num(), *TraceText::DescribeFace());
}

void UTraceTitleMenuWidget::ApplyView(const FTraceTitleMenuView& InView)
{
	// Spec v22 §A1. First frame only; see the header. Before the SetTextOn calls below, so the very
	// first frame this screen draws is already in the artist's face rather than one frame of Lato.
	InstallAtlasLabels();

	const auto SetTextOn = [](UTextBlock* Block, const FString& InText)
	{
		if (Block != nullptr)
		{
			Block->SetText(FText::FromString(InText));
		}
	};

	// ---- Art -------------------------------------------------------------------------------------
	//
	// The backdrop is static and authored in the asset. The wordmark and the swoosh are NOT any more:
	// spec v20 §0.1-0.2 are both authored rects, and this is the file that can enforce them and be
	// photographed doing it. See ComposeTitleBlock().
	ComposeTitleBlock();

	// Spec v23 §A1. THE SCROLL LIVES HERE AND NOWHERE ELSE: the grid owns no clock, exactly as
	// UTraceMenuCanvasArt owns none, so the only thing that can make it move is this line running
	// every frame off ATraceMenuHUD's own World time — the same clock DrawGridFloor() scrolls the
	// Canvas grid on. If a future refactor stops calling ApplyView per frame, the grid freezes, and a
	// frozen grid is a FAILED grid. Two frames a second apart must not be identical.
	if (GridFloor != nullptr)
	{
		// Re-asked every frame so `Trace.UI.MenuGrid 0` typed into the console takes effect without a
		// restart. Guarded because SetVisibility invalidates layout on the whole canvas.
		const ESlateVisibility Wanted = UTraceMenuGrid::WantsGrid()
			? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
		if (GridFloor->GetVisibility() != Wanted)
		{
			GridFloor->SetVisibility(Wanted);
		}
		if (Wanted != ESlateVisibility::Collapsed)
		{
			GridFloor->SetAnimationTime(InView.Now);
		}
	}

	if (MenuCursor != nullptr)
	{
		MenuCursor->SetVisibility(InView.bCursorVisible
			? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);

		// ---- SPEC v24 §2 — AND THE OS ONE STOPS BEING DRAWN ON TOP OF IT --------------------------
		//
		//     "The new cursor ui is working, but my cursor shows on top of it"
		//
		// The line above is the ONLY thing on this screen that knows whether the artist's arrow is on
		// the glass this frame, so it is the right place to say so. Renewed rather than switched: the
		// lease expires two frames after this stops being called, which is what makes a travel, a
		// keyboard-only modal, a collapsed widget or a destroyed HUD hand the pointer back without
		// any of them having to remember to. UI/TraceHardwareCursor.h has the argument in full.
		//
		// This does not break the "reads no game state, calls no gameplay code, makes no decision"
		// rule at the top of this class's header, and the distinction is worth stating because it is
		// load-bearing. bCursorVisible is a fact about the VIEW it was handed — the same bool that
		// decides the image's visibility one line up — not a question asked of the game. The widget
		// still decides nothing; it reports what it drew.
		//
		// EnsureRunning() is unconditional and the renewal is not. On the frames this screen is NOT
		// drawing a pointer — a travel, or a modal in front of it — the arbiter still has to be
		// ticking, because it is the ticker that expires the lease and the ticker that covers the
		// SETTINGS modal, whose own file belongs to another slice this pass.
		TraceHardwareCursor::EnsureRunning();
		if (InView.bCursorVisible)
		{
			TraceHardwareCursor::RenewSuppression(GetOwningPlayer(), TEXT("title screen"));
		}

		// ---- THE POINTER IS THE SAME OBJECT ON BOTH RENDERERS (UI QA finding 6) -------------------
		//
		// The QA pass photographed this screen's two arms side by side and found two different
		// pointers: this widget's blade and, on the Canvas arm, a nine-pixel cyan cross. That arm now
		// draws the blade too, through TraceHardwareCursor::DrawPointer.
		//
		// A widget cannot call an AHUD draw, so this is the same three facts reaching the same sprite
		// by the only route a widget has: the tint, the height and the tip anchor, all read from the
		// place that owns them rather than from this asset. Applying them here rather than trusting
		// WBP_TitleMenu is what makes "identical on both renderers" true by construction — an asset
		// edit cannot silently move one arm away from the other, and a re-slice of the sprite moves
		// both at once.
		//
		// SetColorAndOpacity and not the brush's own tint: the brush is shared with whatever else the
		// asset uses it for, and this is a per-widget multiply.
		MenuCursor->SetColorAndOpacity(TraceHardwareCursor::PointerTint());

		// Re-ANCHORED, not offset. The view hands over a 0..1 fraction of the viewport, so setting the
		// anchor to it puts the sprite in the right place at any DPI scale and any window size without
		// this widget having to know either. Offsets would have needed both.
		if (InView.bCursorVisible)
		{
			if (UCanvasPanelSlot* CursorSlot = Cast<UCanvasPanelSlot>(MenuCursor->Slot))
			{
				const FVector2D Fraction(
					FMath::Clamp(InView.CursorFraction.X, 0.0, 1.0),
					FMath::Clamp(InView.CursorFraction.Y, 0.0, 1.0));
				CursorSlot->SetAnchors(FAnchors(
					static_cast<float>(Fraction.X), static_cast<float>(Fraction.Y),
					static_cast<float>(Fraction.X), static_cast<float>(Fraction.Y)));

				// UIScale 1 deliberately: UMG has already applied the DPI scale to this widget, and
				// under the engine's default curve that scale IS the Canvas path's UIScale (shortest
				// side / 1080). So a size stated in reference pixels here lands at the same number of
				// screen pixels the Canvas arm computes. Trace.UI.VerifyMenu measures that agreement
				// for the rest of the screen; this is the pointer joining it.
				const float CursorH = TraceHardwareCursor::PointerHeight(1.f);
				CursorSlot->SetAutoSize(false);
				CursorSlot->SetSize(FVector2D(CursorH * TraceMenuArtStyle::CursorAspect, CursorH));

				// The ALIGNMENT is the tip, so the anchor point above is the pixel the arrow's point
				// sits on — the same tip-anchoring the Canvas surfaces do by subtracting it.
				CursorSlot->SetAlignment(FVector2D(
					TraceMenuArtStyle::CursorTipU, TraceMenuArtStyle::CursorTipV));
			}
		}
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
		// WHERE it goes is decided with the rest of the bottom stack, in PlaceFooterBelowBlurb().
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

	// WP8.2 — guarded because the string is a process constant and SetText invalidates layout;
	// this runs exactly once, on the first frame.
	if (VersionLabel != nullptr && VersionLabel->Text != InView.Version)
	{
		VersionLabel->SetText(InView.Version);
		VersionLabel->SetVisibility(InView.Version.IsEmpty()
			? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}

	// AFTER the three strings are set, because it measures where the blurb ended up. Spec v20 §0.4.
	PlaceFooterBelowBlurb();

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

	// LAST. Every SetTextOn above wrote to a model; this is the line that puts the frame on screen in
	// the artist's typeface. See UI/Text/TraceAtlasTextSwap.h.
	TraceAtlasTextSwap::MirrorAll(AtlasLabels);
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

void UTraceTitleMenuWidget::LiftWordmarkFromSprite()
{
	using namespace TraceTitleMenuWidgetLocal;

	if (bWordmarkLiftAttempted)
	{
		return;
	}
	bWordmarkLiftAttempted = true;

	UTexture2D* Source = (Wordmark != nullptr)
		? Cast<UTexture2D>(Wordmark->GetBrush().GetResourceObject()) : nullptr;
	if (Source == nullptr)
	{
		// No sprite at all is somebody else's failure — CountResolvedArt already reports it, and
		// `Trace.UI.VerifyMenuArt` already fails on it. Nothing to lift, nothing to say twice.
		return;
	}

	TArray<FColor> Pixels;
	int32 Width = 0;
	int32 Height = 0;
	if (!ReadTexturePixels(Source, Pixels, Width, Height))
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[MenuArt] The TRACE wordmark could not be read back (%s), so it is drawn as the artist's ")
			TEXT("crop: a navy glyph in an amber glow on black, which reads as two offset words. ")
			TEXT("Re-cut the sprite in Scripts/slice-ui-assets.py to fix it at the source."),
			*Source->GetName());
		return;
	}

	// THE SELF-DISABLING TEST, and the measurement the fix is justified by. Composited over the black
	// backdrop a pixel contributes colour * alpha, so this is literally how bright the brightest part
	// of the mark can get on this screen.
	float PeakLuminance = 0.f;
	for (const FColor& Pixel : Pixels)
	{
		PeakLuminance = FMath::Max(PeakLuminance, ByteLuminance(Pixel) * (Pixel.A / 255.f));
	}

	if (PeakLuminance >= WordmarkLiftThreshold)
	{
		UE_LOG(LogTraceGame, Display,
			TEXT("[MenuArt] The TRACE wordmark already peaks at luminance %.2f over black, so it is drawn ")
			TEXT("exactly as it was cut. Nothing was recoloured."), PeakLuminance);
		return;
	}

	// The lift itself. The artist's two tones are unambiguous — the glyph body is navy (B > R) and the
	// glow is amber (R >= B) — so the split needs no threshold to tune and no letterform is moved,
	// re-cut or re-shaped. ALPHA IS COPIED UNTOUCHED, which is what keeps the artist's stroke weights,
	// their antialiasing and the soft fall-off of their glow exactly as drawn.
	int32 BodyPixels = 0;
	int32 GlowPixels = 0;
	for (FColor& Pixel : Pixels)
	{
		if (Pixel.A == 0)
		{
			continue;
		}
		const bool bGlyphBody = Pixel.B > Pixel.R;
		const FColor Replacement = bGlyphBody ? WordmarkInk : WordmarkGlow();
		Pixel.R = Replacement.R;
		Pixel.G = Replacement.G;
		Pixel.B = Replacement.B;
		(bGlyphBody ? BodyPixels : GlowPixels) += 1;
	}

	UTexture2D* Lifted = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8, TEXT("T_TraceWordmark_Lifted"));
	if (Lifted == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("[MenuArt] Could not create the lifted TRACE wordmark; drawing the crop."));
		return;
	}

	Lifted->SRGB = Source->SRGB;
	Lifted->Filter = Source->Filter;
	Lifted->AddressX = TA_Clamp;
	Lifted->AddressY = TA_Clamp;
	Lifted->NeverStream = true;

	FTexture2DMipMap& Mip = Lifted->GetPlatformData()->Mips[0];
	if (void* Destination = Mip.BulkData.Lock(LOCK_READ_WRITE))
	{
		FMemory::Memcpy(Destination, Pixels.GetData(), static_cast<int64>(Width) * Height * 4);
	}
	Mip.BulkData.Unlock();
	Lifted->UpdateResource();

	LiftedWordmark = Lifted;

	// BOTH places the mark is drawn. The travel overlay uses the same sprite at 420 px and 55%
	// opacity, and it is on screen for exactly as long as a join takes — i.e. the one screen a
	// title-screen screenshot can never catch. Leaving it on the old crop would have shipped the
	// defect somewhere nobody looks.
	Wordmark->SetBrushResourceObject(LiftedWordmark);
	if (TravelWordmark != nullptr && TravelWordmark->GetBrush().GetResourceObject() == Source)
	{
		TravelWordmark->SetBrushResourceObject(LiftedWordmark);
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[MenuArt] TRACE wordmark lifted: the crop peaked at luminance %.2f over black (navy glyph, ")
		TEXT("amber glow), which reads as two words. %d glyph pixels are now ink and %d glow pixels are ")
		TEXT("the artist's amber; the alpha, and so every letterform, is untouched."),
		PeakLuminance, BodyPixels, GlowPixels);
}

void UTraceTitleMenuWidget::ComposeTitleBlock()
{
	using namespace TraceTitleLayout;          // the shared composition numbers (see the header)
	using namespace TraceTitleMenuWidgetLocal; // SetSlotRect and friends

	UCanvasPanelSlot* MarkSlot = (Wordmark != nullptr) ? Cast<UCanvasPanelSlot>(Wordmark->Slot) : nullptr;
	UCanvasPanelSlot* SwooshSlot = (SwooshImage != nullptr) ? Cast<UCanvasPanelSlot>(SwooshImage->Slot) : nullptr;
	if (MarkSlot == nullptr || SwooshSlot == nullptr)
	{
		return;
	}

	// Reference pixels: UMG's DPI scale is (shortest side / 1080), so this is 1080 tall at every
	// window size the game runs at and these numbers mean the same thing on all of them.
	const FVector2D RootSize = GetCachedGeometry().GetLocalSize();
	if (RootSize.X <= 1.0)
	{
		return;
	}

	// Aspect from the TEXTURE, not from a constant, so a re-slice that changes the crop cannot leave
	// this file stretching it. The fallbacks are the sheet's own crop ratios.
	const auto SpriteAspect = [](const UImage* InImage, float InFallback) -> float
	{
		const UTexture2D* Texture = (InImage != nullptr)
			? Cast<UTexture2D>(InImage->GetBrush().GetResourceObject()) : nullptr;
		if (Texture != nullptr && Texture->GetSizeX() > 0)
		{
			return static_cast<float>(Texture->GetSizeY()) / static_cast<float>(Texture->GetSizeX());
		}
		return InFallback;
	};

	const float MarkW = FMath::Min(MarkWidth, static_cast<float>(RootSize.X) * MarkMaxWidthFraction);
	const float MarkH = MarkW * SpriteAspect(Wordmark, 2392.f / 14480.f);

	float SwooshW = MarkW * SwooshWidthOfMark;
	const float SwooshAspect = SpriteAspect(SwooshImage, 3856.f / 18600.f);
	const float SwooshTop = MarkTopY + MarkH + MarkW * SwooshGapOfMark;

	// The clamp, and the only part of this that is not the artist's own proportion: whatever the
	// sheet says, the flourish stops above the tagline. Taken from where the tagline actually IS so
	// that moving it in the asset moves this too; the shared authored position is the fallback.
	float TaglineTop = TraceTitleLayout::TaglineY;
	if (const UCanvasPanelSlot* TaglineSlot = (TaglineText != nullptr)
		? Cast<UCanvasPanelSlot>(LiveText(TaglineText)->Slot) : nullptr)
	{
		TaglineTop = static_cast<float>(TaglineSlot->GetPosition().Y);
	}

	const float MaxSwooshH = FMath::Max(1.f, TaglineTop - SwooshClearOfTagline - SwooshTop);
	if (SwooshW * SwooshAspect > MaxSwooshH)
	{
		SwooshW = MaxSwooshH / FMath::Max(SwooshAspect, KINDA_SMALL_NUMBER);
	}
	const float SwooshH = SwooshW * SwooshAspect;

	// Both slots are anchored top-centre and aligned (0.5, 0), so X is an offset from the centre line
	// and the artist's leftward bias is a negative number rather than a re-anchor.
	SetSlotRect(MarkSlot, FVector2D(0.0, MarkTopY), FVector2D(MarkW, MarkH));
	SetSlotRect(SwooshSlot, FVector2D(-MarkW * SwooshLeftOfMark, SwooshTop), FVector2D(SwooshW, SwooshH));

	if (!FMath::IsNearlyEqual(SwooshImage->GetRenderOpacity(), SwooshOpacity, 0.01f))
	{
		SwooshImage->SetRenderOpacity(SwooshOpacity);
	}
}

void UTraceTitleMenuWidget::PlaceFooterBelowBlurb()
{
	using namespace TraceTitleMenuWidgetLocal;

	// LiveText, not the text block itself: spec v22 §A1 moved every one of these onto the glyph
	// atlas, and a swapped text block has no slot of its own any more. See TraceAtlasTextSwap::Live.
	UCanvasPanelSlot* KeysSlot = (FooterKeysText != nullptr)
		? Cast<UCanvasPanelSlot>(LiveText(FooterKeysText)->Slot) : nullptr;
	UCanvasPanelSlot* HintSlot = (FooterHintText != nullptr)
		? Cast<UCanvasPanelSlot>(LiveText(FooterHintText)->Slot) : nullptr;
	if (KeysSlot == nullptr && HintSlot == nullptr)
	{
		return;
	}

	const FGeometry& RootGeometry = GetCachedGeometry();
	const FVector2D RootSize = RootGeometry.GetLocalSize();
	if (RootSize.Y <= 1.0)
	{
		return;
	}

	// WHERE THE BLURB ACTUALLY IS, measured off Slate rather than recomputed from the row count.
	// The blurb is a grandchild of the root canvas (ConsolePanel -> vertical box -> here), so its
	// bottom is converted through absolute space; it is the same number the pixels are drawn at.
	float BlurbBottom = LocalEdgeY(RootGeometry, LiveText(BlurbText), 1.f);

	if (BlurbBottom <= 0.f)
	{
		// First frame, before Slate has laid the console out. Derived from the SAME shared layout the
		// Canvas renderer uses, so the two agree even on the frame nothing has been measured yet.
		const TraceMenuStyle::FConsoleLayout Layout = TraceMenuStyle::ComputeConsoleLayout(
			static_cast<float>(RootSize.X), static_cast<float>(RootSize.Y), 1.f,
			FMath::Max(OrderedRows.Num(), 1));
		BlurbBottom = Layout.FirstRowY
			+ (FMath::Max(OrderedRows.Num(), 1) - 1) * TraceMenuStyle::RowSpacing
			+ TraceMenuStyle::RowHeight
			+ 22.f    // the blurb's own top padding in the asset
			+ 19.f;   // one line of it at FS_BLURB
	}

	float KeysY = BlurbBottom + FooterGapBelowBlurb;

	// THE PORT-BUSY LINE, when it is on at all, is part of this stack — and that placement is a
	// concession, not a preference. It belongs under the address chip it contradicts, which is where
	// the Canvas renderer puts it (ATraceMenuHUD::DrawAddressChip, at ChipY + ChipH + 4). It does not
	// fit there on THIS renderer: the artist's chip wears a 9-sliced button plate whose glow reaches
	// y 437, the first row's plate glow starts at y 442, and the line's ink is 11 px tall. Measured
	// from a capture with UDP 7777 deliberately held, it overlapped the PLAY row at its authored y of
	// 436 and overlapped the chip at every y that cleared the row. So it flows with the blurb
	// instead, above the key hints, where there is room for it to be read.
	if (PortWarningText != nullptr && PortWarningText->GetVisibility() != ESlateVisibility::Collapsed)
	{
		if (UCanvasPanelSlot* WarningSlot = Cast<UCanvasPanelSlot>(LiveText(PortWarningText)->Slot))
		{
			SetSlotRect(WarningSlot, FVector2D(0.0, KeysY), FVector2D(-1.0, -1.0));
			KeysY += FMath::Max(1.f,
				static_cast<float>(LiveText(PortWarningText)->GetCachedGeometry().GetLocalSize().Y))
				+ FooterGapAfterWarning;
		}
	}

	const float LastLineHeight = (HintSlot != nullptr && LiveText(FooterHintText)->GetCachedGeometry().GetLocalSize().Y > 1.0)
		? static_cast<float>(LiveText(FooterHintText)->GetCachedGeometry().GetLocalSize().Y) : 17.f;

	// The console can only grow downwards, so the last thing to check is that the screen still has
	// room. If it ever does not, both lines slide up together rather than one of them walking off.
	const float LowestKeysY = static_cast<float>(RootSize.Y) - FooterBottomMargin - LastLineHeight - FooterLineGap;
	KeysY = FMath::Min(KeysY, LowestKeysY);

	SetSlotRect(KeysSlot, FVector2D(0.0, KeysY), FVector2D(-1.0, -1.0));
	SetSlotRect(HintSlot, FVector2D(0.0, KeysY + FooterLineGap), FVector2D(-1.0, -1.0));
}

int32 UTraceTitleMenuWidget::CountResolvedArt(int32& OutTotal, TArray<FString>& OutMissing, bool bIncludeBackdrop) const
{
	OutTotal = 0;
	int32 Resolved = 0;

	const auto Check = [&OutTotal, &Resolved, &OutMissing](const UImage* InImage, const TCHAR* InName)
	{
		++OutTotal;
		if (InImage != nullptr && InImage->GetBrush().GetResourceObject() != nullptr)
		{
			++Resolved;
			return;
		}
		OutMissing.Add(InName);
	};

	// Backdrop is deliberately NOT in this list: it is a flat black fill and has no texture by design.
	// Which is exactly what makes it the red arm — see the parameter's comment in the header.
	if (bIncludeBackdrop)
	{
		Check(Backdrop, TEXT("Backdrop (red arm — this one is SUPPOSED to have no texture)"));
	}
	Check(SwooshImage, TEXT("SwooshImage"));
	Check(Wordmark, TEXT("Wordmark"));
	Check(MenuCursor, TEXT("MenuCursor"));
	Check(TravelWordmark, TEXT("TravelWordmark"));

	// Each row is asked slot by slot rather than as a yes/no, because the slots that go missing
	// unnoticed are the ones that are not on screen right now — the hover plate, the disabled plate,
	// the chevron. See UTraceMenuRow::CountResolvedArt.
	for (int32 Index = 0; Index < OrderedRows.Num(); ++Index)
	{
		const UTraceMenuRow* Row = OrderedRows[Index];
		if (Row == nullptr)
		{
			// A null row is a BindWidget that did not resolve, i.e. the asset is older than this
			// class. One line, not five missing brushes, because there is one thing to fix.
			++OutTotal;
			OutMissing.Add(FString::Printf(TEXT("row %d is not in the asset at all"), Index));
			continue;
		}
		Resolved += Row->CountResolvedArt(OutTotal, OutMissing, FString::Printf(TEXT("row %d"), Index));
	}

	return Resolved;
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
