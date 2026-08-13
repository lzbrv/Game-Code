// Trace — the bottom-right HUD corner as a UMG widget. Spec v17 §4 (step 4b).
//
// The UMG twin of ATraceHUD::DrawAmmoAndStatuses: ammo pinned to the corner, statuses stacking
// upward above it. It is fed one FTraceHudCornerState per frame and asks the game nothing.
//
// ---------------------------------------------------------------------------------------------
// THIS IS A REORGANISATION, NOT A REDESIGN (spec v17 §0)
// ---------------------------------------------------------------------------------------------
// Every decision the Canvas corner encodes is preserved here on purpose, and the reasons are on
// ATraceHUD::DrawAmmoAndStatuses:
//
//   * AMMO NEVER MOVES. It is read mid-fight without looking, so it owns the corner; the status
//     stack grows upward above it and can only ever shuffle other statuses.
//   * STATUSES DRAIN, COOLDOWNS FILL. The bottom-left cooldown stack is NOT part of this step and
//     stays on Canvas — see the report. Keeping the two directions opposite is half of spec v16 §2's
//     "separate from cooldowns"; the corner is the other half.
//   * A BEE CLIP CHANGES THREE INDEPENDENT THINGS — the colour, the SHAPE of the magazine strip
//     (five fat pips instead of thirty thin ticks) and the words. Any one of the three survives a
//     compressed screenshot, a colour-blind player or a glance.
//
// The one thing that is deliberately NOT ported is the Canvas workaround for AHUD::DrawLine
// discarding alpha. That trap belongs to the charge ring (whose track is a dimmed RGB standing in
// for an alpha that never survived), which this step does not touch. Everything the CORNER draws
// goes through AHUD::DrawRect, which sets SE_BLEND_Translucent — so its alphas are real alphas and
// they port literally. Checked, not assumed.
//
// ---------------------------------------------------------------------------------------------
// LAYOUT: WHY C++ SETS ONLY TWO NUMBERS
// ---------------------------------------------------------------------------------------------
// The .uasset is authored in DESIGN pixels — the units the Canvas pass uses at UIScale 1.0 — so a
// designer opening WBP_TraceHudCorner sees the 260-wide block, the 24-tall chip and the 40-tall
// number row that the shipped HUD has always used. C++ contributes exactly two things per frame:
//
//   1. the corner stack's offset from the viewport's bottom-right (the HUD's own 40 px margin), and
//   2. a uniform render scale, UIScale / the viewport's DPI scale.
//
// (2) is what keeps the two paths the same size. The Canvas HUD scales itself by
// ViewH / 1080 (clamped), while UMG has already scaled every widget by the project's DPI curve —
// dividing one by the other leaves exactly the Canvas geometry. At 720p and 1080p, the two agree to
// within a percent and the transform is a no-op, so text stays crisp; it only does real work at the
// resolutions where the HUD's linear rule and the engine's DPI curve disagree.
//
// Anything else you want to move, move it in the asset.

#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"

#include "UI/Widgets/HUD/TraceHudCornerData.h"

#include "UI/Text/TraceAtlasTextSwap.h"

#include "TraceHudCornerWidget.generated.h"

class UBorder;
class UHorizontalBox;
class UImage;
class UProgressBar;
class UTextBlock;
class UTraceHudStatusChipWidget;
class UVerticalBox;
class UWidget;

UCLASS()
class TRACE_API UTraceHudCornerWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** /Game path of the corner widget blueprint. The ONE place it is written down in C++. */
	static const TCHAR* CornerBlueprintPath();

	/** /Game path of the chip widget blueprint, which the corner instantiates one per status. */
	static const TCHAR* ChipBlueprintPath();

	/**
	 * Resolves the chip class and checks every BindWidget property. Call once, immediately after
	 * CreateWidget and BEFORE the corner is used for anything.
	 *
	 * *** ALL OR NOTHING, AND IT NEVER REPAIRS. *** A corner that adopted half an asset would draw a
	 * hole where the ammo count used to be, which is strictly worse than the Canvas corner that has
	 * worked since v16. Any failure returns false with a human-readable reason and the HUD falls
	 * straight back — the same rule spec v17 §0.1 puts on every migrated system.
	 */
	bool InitialiseCorner(FString& OutReason);

	/**
	 * Fills the corner in from @p InState and returns WHAT IT ACTUALLY EMITTED.
	 *
	 * @param InDesignScale   UIScale / viewport DPI scale — see the layout note above.
	 */
	FTraceHudCornerPresented PresentCorner(const FTraceHudCornerState& InState, float InDesignScale);

	/** Collapses the whole corner. Cheap enough to call every frame; a collapsed widget never paints. */
	void HideCorner();

	/** Every BindWidget name this class needs, for the verifier and the generator's manifest. */
	static const TArray<FName>& RequiredWidgetNames();

protected:
	//~ Begin UUserWidget interface
	virtual void NativeConstruct() override;
	//~ End UUserWidget interface

	// ---- The corner ---------------------------------------------------------------------------

	/**
	 * The whole corner, anchored to the viewport's bottom-right. Its canvas slot offset and its
	 * render scale are the two numbers C++ sets; everything inside it is the asset's business.
	 */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UVerticalBox> CornerStack;

	/** The status chips live here, newest priority nearest the ammo. Filled bottom-up. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UVerticalBox> StatusStack;

	// ---- The ammo plate -----------------------------------------------------------------------

	/**
	 * The plate's outline, tinted by the ROUNDS colour, so a bee clip changes the plate as well as
	 * its contents and the whole corner announces itself.
	 */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UBorder> PlateOutline;

	/**
	 * The dark fill behind the ammo block, and it is not decoration: this arena is emissive neon with
	 * a bloom pass, and the first v16 capture put a white "26" over a blown-out light where it was
	 * almost unreadable. The one number a player checks without looking cannot depend on what happens
	 * to be behind it.
	 */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UBorder> PlateFill;

	/** "AMMO", or "BEE ROUNDS" — the third independent bee-round signal after colour and shape. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> AmmoLabelText;

	/** "[R]  RELOAD" from the player's OWN binding, or the live "RELOADING  1.2". */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> ReloadLabelText;

	/** The dark trough the magazine ticks sit on, and which shows through the gaps between them. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UImage> MagazineTrough;

	/** One tick per round. The ticks themselves are pooled at runtime — see the .cpp. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UHorizontalBox> MagazineStrip;

	/** Mid-reload the strip becomes a single filling bar: a THIRD shape, so "the gun is coming back". */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UProgressBar> ReloadBar;

	/** The big number. Twice the body scale, because it is read in peripheral vision. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> CountText;

	/** "/30", baseline-aligned to the count and deliberately subordinate to it. */
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UTextBlock> CapacityText;

private:
	/**
	 * The chip class, loaded from ChipBlueprintPath() once in InitialiseCorner.
	 *
	 * A soft path resolved at runtime rather than a UPROPERTY(EditDefaultsOnly) class reference,
	 * because a hard reference from C++ to a Blueprint asset is exactly the dependency that would
	 * stop the C++-only fallback from being a real fallback: the module would then refuse to load
	 * without the asset it is supposed to survive the absence of.
	 */
	UPROPERTY(Transient)
	TSubclassOf<UTraceHudStatusChipWidget> ChipWidgetClass;

	/** Pooled chip widgets. Created on demand, reused, collapsed when a status ends. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTraceHudStatusChipWidget>> ChipPool;

	/** Pooled magazine ticks, likewise: a clip is reloaded far more often than its size changes. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UImage>> TickPool;

	/** Applies the corner stack's offset and render scale. Idempotent; called every present. */
	void ApplyCornerTransform(float InDesignScale, bool bInAmmoVisible);

	/** Fills the ammo plate in and records what it emitted. Collapses the plate when there is no gun. */
	void PresentAmmo(const FTraceHudCornerState& InState, FTraceHudCornerPresented& OutPresented);

	/** Fills the status stack in, bottom-up, and records every chip it actually showed. */
	void PresentChips(const FTraceHudCornerState& InState, FTraceHudCornerPresented& OutPresented);

	/** Grows the tick pool to @p InCount visible ticks, collapsing any beyond it. */
	void EnsureTicks(int32 InCount);

	// ---- SPEC v22 §A1: THE CORNER TYPES IN THE ARTIST'S FACE ---------------------------------------
	//
	// The Canvas half of this HUD types from the glyph atlas (ATraceHUD's text helpers). This is the
	// UMG half of the SAME corner — `Trace.UI.HUD.UseUMG` picks which one draws — so leaving it alone
	// would have made the game's typeface depend on a cvar. The four text blocks stay as the model and
	// keep receiving exactly the SetText they always did; see UI/Text/TraceAtlasTextSwap.h.

	/** Installed once, on the first PresentAmmo. */
	bool bAtlasLabelsInstalled = false;

	UPROPERTY(Transient)
	TArray<FTraceAtlasLabel> AtlasLabels;

	void InstallAtlasLabels();
};
