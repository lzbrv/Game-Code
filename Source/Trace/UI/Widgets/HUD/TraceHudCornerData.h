// Trace — the bottom-right HUD corner, as DATA.
//
// Spec v17 §4, step 4b. The corner (ammo pinned to the corner, statuses stacking above it — spec
// v16 §2) is drawn two ways now: the shipped Canvas passes in ATraceHUD, and a UMG widget tree in
// Content/Trace/UI/HUD. This header is what stops those two from ever disagreeing about the game.
//
// *** ONE STATE, TWO PRESENTERS. *** ATraceHUD::BuildCornerState() asks the weapon component, the
// health component and the four ability sets exactly the questions the Canvas pass has always asked,
// in the same order, and writes the answers here. Neither presenter is allowed to ask a gameplay
// object anything. That is the whole reason this struct exists: a UMG port that re-derived "is this
// a bee clip" would be a second definition of the rule, free to drift from the one the gun enforces —
// which is precisely the class of bug the Canvas pass's own comments spend two hundred lines warning
// about.
//
// Nothing here is a UObject and nothing here is UPROPERTY: it is per-frame scratch, built and thrown
// away inside one DrawHUD, and making it reflected would only invite somebody to serialise it.

#pragma once

#include "CoreMinimal.h"

/**
 * One status chip's content, in the order the corner stacks them (index 0 sits NEAREST the ammo).
 *
 * @see ATraceHUD::DrawAmmoAndStatuses for why that order is fixed and why a status may never move
 *      the ammo count.
 */
struct FTraceHudCornerChip
{
	/** "VULNERABLE  x3  +35%" — already formatted, because the wording is a decision, not a style. */
	FString Label;

	/** The right-hand readout: "1.3s", or a distance for the two effects with no clock. */
	FString Readout;

	/** 0..1 of the effect REMAINING. DRAINS toward empty — the opposite of every cooldown meter. */
	float Fraction = 0.f;

	/** The status's own hue, from TraceHUDStatusStyle. Carries the tab, the readout and the drain. */
	FLinearColor Tint = FLinearColor::White;
};

/**
 * Everything the bottom-right corner shows this frame.
 *
 * Empty (bAmmoBlock false, no chips) is a perfectly ordinary state — a live player with the knife
 * out and nothing on them. "The corner draws nothing" is signalled by BuildCornerState returning
 * false, not by this struct.
 */
struct FTraceHudCornerState
{
	// ---- The ammo block ----------------------------------------------------------------------
	//
	// bAmmoBlock is UTraceWeaponComponent::ShouldShowAmmo() and nothing else — never a locally
	// re-derived "the carrier has no gun".

	bool bAmmoBlock = false;

	/** Rounds in the clip, and the size of the clip THAT IS ACTUALLY LOADED (a bee clip holds 5). */
	int32 InClip = 0;
	int32 ClipCapacity = 0;

	/** X's Sting clip. Changes the colour, the strip's SHAPE and the words — see the .cpp. */
	bool bBeeClip = false;

	bool bReloading = false;

	/** Under TraceHUDStatusStyle::LowAmmoFraction of an ordinary clip. Never true for a bee clip. */
	bool bLowAmmo = false;

	/** 0..1 of the reload ELAPSED (this one fills, because the gun is coming back). */
	float ReloadFraction = 0.f;

	/** Seconds left on the reload, for the label. */
	float ReloadRemaining = 0.f;

	/** "26", or "--" mid-reload. */
	FString CountText;

	/** "/30". Subordinate to the count on purpose: "7" must read in peripheral vision, "/30" need not. */
	FString CapacityText;

	/** "AMMO", or "BEE ROUNDS". */
	FString AmmoLabel;

	/** "[R]  RELOAD" from the player's OWN binding, or "RELOADING  1.2". */
	FString RightLabel;

	/** Bee amber or the neutral. Tints the rounds, the plate's border and the unlit ticks. */
	FLinearColor RoundsColor = FLinearColor::White;

	/** The count's own colour: reload gold, low-ammo red, or RoundsColor. */
	FLinearColor CountColor = FLinearColor::White;

	// The three remaining colours that CHANGE AT RUNTIME. They are carried here rather than authored
	// into the widget asset for one reason: the palette they come from (TraceHUDStatusStyle, in
	// TraceHUD.cpp) must have exactly one definition. Colours that never change — the panel fill, the
	// trough, the dim ink of "/30" — live in the .uasset, where a designer can retune them, and the
	// generator is the one place they are written twice.

	/** "AMMO" in dim ink, or "BEE ROUNDS" in bee amber. */
	FLinearColor AmmoLabelColor = FLinearColor::White;

	/** The reload key hint in dim ink, or the live "RELOADING  1.2" in reload gold. */
	FLinearColor RightLabelColor = FLinearColor::White;

	/** Reload gold: the gun is unavailable, which is the same news the empty clip was. */
	FLinearColor ReloadBarColor = FLinearColor::White;

	// ---- The status stack --------------------------------------------------------------------

	/** Bottom-up: index 0 is drawn nearest the ammo block. */
	TArray<FTraceHudCornerChip> Chips;
};

/**
 * What a presenter ACTUALLY EMITTED, handed back so the spec v16 §2 draw record stays honest.
 *
 * The record's whole point (see the harness header in TraceHUD.cpp) is that it reports pixels, not
 * intentions: a corner that computed a healthy 26 and then emitted no ticks has to show up as a
 * zero. So the UMG presenter fills this in AS IT SETS WIDGETS, and ATraceHUD copies it into the
 * record — rather than the HUD writing the record from the state it handed over, which would make
 * the record self-certifying in exactly the way this project has already been burned by three times.
 */
struct FTraceHudCornerPresented
{
	bool bAmmoBlock = false;
	bool bBeeClip = false;
	bool bReloadBar = false;

	/** Lit magazine ticks actually made visible. An empty strip cannot pass as a full one. */
	int32 LitTicks = 0;

	/** "26/30" — the two text blocks that were actually filled, concatenated. */
	FString AmmoText;

	/** One entry per chip widget actually shown, in the order shown (bottom-up). */
	TArray<FString> Chips;
};

/**
 * The corner's layout, in DESIGN pixels — i.e. the units the Canvas pass uses at UIScale 1.0, which
 * are also the units the .uasset is authored in.
 *
 * Only the numbers C++ still has to know live here. Everything else — the 260-wide block, the 24-tall
 * chip, the fonts — lives in the widget assets, because that is the half a designer needs to edit and
 * the entire point of spec v17 §4. If you change one of those in the editor, nothing in C++ needs to
 * know.
 */
namespace TraceHudCornerLayout
{
	/**
	 * Distance from the viewport's bottom-right corner to the corner stack's OUTER edge, in design
	 * pixels.
	 *
	 * 34, and the six between this and the Canvas pass's 40 is not a fudge. The Canvas HUD lays the
	 * corner out to a 40 px margin and then draws the ammo PLATE 6 px beyond it on every side
	 * (DrawAmmoBlock calls DrawPanel with a PlatePad of 6), so the plate's outer corner has always
	 * sat at 34 while the chips sat at 40. The widget stack anchors the outermost thing — the plate —
	 * and the chips carry the difference as a positive inset (ChipInsetDesignPx below).
	 *
	 * It is done that way round because the obvious alternative does not survive measurement:
	 * anchoring at 40 and giving the plate a NEGATIVE slot padding of -6 put the plate's bottom edge
	 * about four screen pixels off at 720p. Positive insets on the chips reproduce every edge exactly
	 * and are also the version a designer can reason about in the editor.
	 */
	static constexpr float StackMarginDesignPx = 34.f;

	/**
	 * The bottom margin to use when there is NO ammo plate — knife out, or carrying the Core.
	 *
	 * Not 34, because the Canvas corner does not close up when the plate goes away: DrawAmmoBlock
	 * returns BottomY unchanged and the stack still starts 12 px above it, which leaves the lowest
	 * chip's bottom edge 52 px off the floor rather than 40. Subtracting the chip's own 6 px of
	 * bottom padding gives the stack's edge: 46. Without this the corner would visibly drop by 12 px
	 * the moment a player picked the Core up, which is a change in what the player sees and therefore
	 * a bug in this pass.
	 */
	static constexpr float NoPlateBottomMarginDesignPx = 46.f;

	/**
	 * How far each status chip is inset from the stack's left and right edges, in design pixels.
	 *
	 * The other half of the plate's 6 px overhang, above. In C++ rather than in the asset because the
	 * chips are instantiated at runtime — one per live status — so their slots do not exist until then.
	 */
	static constexpr float ChipInsetDesignPx = 6.f;

	/**
	 * Vertical gap under each status chip, in design pixels: the gap between two chips, and — because
	 * every chip carries it — the gap between the lowest chip and the ammo plate's top edge.
	 *
	 * DrawStatusChip's own 6 px.
	 */
	static constexpr float ChipGapDesignPx = 6.f;

	/**
	 * Gap between two magazine ticks, in design pixels. DrawAmmoBlock's 1.5, and the ticks are
	 * likewise runtime-created (there are thirty of them, or five).
	 */
	static constexpr float TickGapDesignPx = 1.5f;

	/**
	 * Hard ceiling on magazine tick widgets, so a nonsense clip size cannot spawn ten thousand of
	 * them. Nothing in the shipped game comes close (30 rounds, or 5 bee rounds); if this ever
	 * clamps, the log says so and the draw record will disagree with the Canvas path — which is the
	 * correct, loud outcome rather than a silent one.
	 */
	static constexpr int32 MaxMagazineTicks = 240;
}
