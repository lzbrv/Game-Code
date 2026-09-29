// Trace — the handmade UI kit, drawn on a Canvas. ONE copy, for every AHUD screen.
//
// =================================================================================================
// WHAT THIS IS
// =================================================================================================
// The artist's kit is a handful of sprites (TraceMenuArtStyle.h names them and measures them) plus
// a few rules about how they are put on screen: which plate a state wears, what colour its word is,
// how far the glow overhangs the rect, how big the corner is. Before this file, three Canvas screens
// each carried their own copy of those rules:
//
//     TraceCharacterSelectArt::DrawPlate   9-slice, NO render-resource guard
//     TraceOptionsMenuArt::DrawPlate       3-slice, guarded
//     TraceMenuHUDSprites::DrawPlate       3-slice, guarded
//
// plus three sprite caches and the per-state switch hidden inside the UMG title row
// (TraceMenuRowWidgetLocal::VisualsFor). They now all call this file. Do not write a fourth copy:
// if a screen needs something the kit does not have, add it HERE.
//
// =================================================================================================
// THE ONE RULE THAT IS NOT OPTIONAL: A LOADED TEXTURE IS NOT A DRAWABLE ONE
// =================================================================================================
// AHUD::DrawTexture hands Texture->GetResource() straight to an FCanvasTileItem. A texture that is
// loaded but whose FTextureResource has no RHI texture yet becomes a batched element the render
// thread dies on (SIGSEGV in FBatchedElements::Draw, measured — see TraceOptionsMenu.cpp's history).
// LoadObject returns before the RHI texture exists, so the first frame or two after a cold load is
// exactly that state. Every draw in this file therefore goes through IsDrawable(), and Sprite()
// returns null until it is true. Callers never need their own guard, and must not bypass this one
// by calling HUD->DrawTexture on a kit texture themselves.
//
// =================================================================================================
// HOW TO USE IT (stylespec §3-§6)
// =================================================================================================
//   A button, the whole recipe:
//       const ETraceKitState State = TraceMenuKit::StateFor(bEnabled, bSelected, bPressed);
//       TraceMenuKit::DrawButton(HUD, State, X, Y, W, H, TRACE_TEXT("KEY", "LABEL"), Now);
//
//   Just the plate (your own text, or a tall card whose corner must stay button-sized):
//       TraceMenuKit::DrawStatePlate(HUD, State, X, Y, W, H, Now, /*CornerHeight=*/60.f * UIScale);
//
//   The texture-level primitive, for a screen that keeps its own fallback:
//       if (!TraceMenuKit::DrawPlate(HUD, TraceMenuKit::Sprite(ETraceKitSprite::BtnHover),
//               TraceMenuArtStyle::ButtonFrame, X, Y, W, H, H, Tint)) { ...your fallback... }
//
// Rects are ALWAYS the plate — the glow is drawn outside them. Sizes are screen pixels; multiply
// the 1080p reference numbers below by the screen's UIScale.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"

#include "UI/Text/TraceText.h"
#include "UI/Widgets/Menu/TraceKitMotion.h"   // FTraceKitFade — the kit's one open/close fade (P10)
#include "UI/Widgets/Menu/TraceMenuArtStyle.h"

class AHUD;
class APlayerController;
class UTexture;
class UTexture2D;

/** Every sprite the kit draws. The paths are TraceMenuArtStyle's; this is only the cache's index. */
enum class ETraceKitSprite : uint8
{
	BtnDefault,
	BtnHover,
	BtnDisabled,
	ValueBox,
	SliderTrack,
	SliderHandle,
	Chevron,
	Wordmark,
	Swoosh,
	Count
};

/**
 * The four states a kit control can be in (stylespec §5). Hover and keyboard selection are ONE
 * state. Rank, highest first: Disabled > Pressed > Hover > Default — see TraceMenuKit::StateFor.
 */
enum class ETraceKitState : uint8
{
	Default,
	Hover,
	Pressed,
	Disabled,
};

/** Everything one state decides, decided together, so the plate and the word cannot disagree. */
struct FTraceKitVisuals
{
	/** Which of the artist's three plates. The hover ring is baked into BtnHover. */
	ETraceKitSprite Plate = ETraceKitSprite::BtnDefault;

	/** Flat RGB multiplier for the plate when the state does not breathe (PressedTint for Pressed). */
	float PlateTint = 1.f;

	/** True for Hover: the plate breathes, see TraceMenuKit::HoverPulse. PlateTintAt() applies it. */
	bool bPulses = false;

	/** The label's colour. */
	FLinearColor Label = FLinearColor::White;

	/** The row's own furniture (readouts, values, arrows): white selected, 0.85 white otherwise. */
	FLinearColor Furniture = FLinearColor::White;
};

/** One [KEY] VERB pair of a key legend. Either string empty drops the pair, chip and all. */
struct FTraceKitLegendItem
{
	FString Key;
	FString Label;
};

/** One textured quad of a plate: screen rect, then UV origin and UV size. */
struct FTraceKitQuad
{
	float X = 0.f;
	float Y = 0.f;
	float W = 0.f;
	float H = 0.f;
	float U = 0.f;
	float V = 0.f;
	float UW = 0.f;
	float VH = 0.f;
};

namespace TraceMenuKit
{
	// =============================================================================================
	// COLOURS. The artist's sampled palette is TraceMenuArtStyle (PlateFill, WordDefault,
	// WordHoverLifted, WordDisabled, Amber/AmberLifted, ValueGlow/ValueGlowLifted, DisabledFill,
	// DisabledRing). These are the kit's USAGE-level values on top of it.
	// NOT the pre-kit Tron palette: never use TraceMenuStyle::Cyan & co. on a kit screen.
	// =============================================================================================

	/** Kit screens sit on opaque pure black (stylespec §1). */
	static const FLinearColor Background = FLinearColor(0.f, 0.f, 0.f, 1.f);

	/** A modal's scrim: black at this alpha (stylespec §1 allows 0.78-0.86). */
	static constexpr float ScrimAlpha = 0.82f;

	/** Furniture on a selected / pressed control. */
	static const FLinearColor FurnitureSelected = FLinearColor::White;

	/** Furniture on an unselected control — legible, and quiet enough that the selected one wins. */
	static const FLinearColor FurnitureUnselected = FLinearColor(0.85f, 0.85f, 0.85f, 1.f);

	/**
	 * Secondary words on black — a tagline, a caption, a note, a version string. A neutral grey with a
	 * hair of blue, the value the UMG title has always set its tagline and captions in
	 * (Scripts/generate-menu-widgets.py INK_DIM), so both title renderers agree. Not the pre-kit
	 * TraceMenuStyle::InkDim, which is a cyan.
	 */
	static const FLinearColor CaptionInk = FLinearColor(0.52f, 0.55f, 0.62f, 1.f);

	// =============================================================================================
	// PROPORTIONS, measured off the sheet (Scripts/slice-ui-assets.py's crop boxes).
	// =============================================================================================

	/** The main button: 4723 x 1230 sheet px, 3.84:1 (about 283 x 74 at 1080p on the sheet). */
	static constexpr float ButtonAspect = 4723.f / 1230.f;

	/** The KEY chip: the same plate cut 2060 wide, 1.67:1 (about 100 x 60 on a 60-tall row). */
	static constexpr float KeyChipAspect = 2060.f / 1230.f;

	/** A label's CAP height as a fraction of its plate's height: the sheet's 450/1230. */
	static constexpr float LabelCapFraction = 0.37f;

	/**
	 * Horizontal room kept between a label and its plate's ends, as a fraction of plate height (the
	 * sheet's DEFAULT / DISABLED words leave about 0.2 H). Sofachrome sets about 25% wider than the
	 * sheet's baked lettering, so a long word at the full cap height can overrun a sheet-proportioned
	 * plate: DrawLabel then shrinks it to fit rather than letting it touch the ends.
	 */
	static constexpr float LabelPadFraction = 0.25f;

	/**
	 * The slider on the sheet, relative to its TRACK SPRITE's height (the trough, halo included —
	 * T_MenuSliderTrack's 23 rows): the blade handle is 690/231 of it and centred on it, the value
	 * box's plate is 538/231 of it and centred on the same line.
	 */
	static constexpr float SliderHandleToTrack = 690.f / 231.f;
	static constexpr float ValuePlateToTrack = 538.f / 231.f;

	/** T_MenuSliderHandle's width per unit height (64 x 87, the same blade as the pointer). */
	static constexpr float SliderHandleAspect = 64.f / 87.f;

	/** Where the solid rail sits inside the track sprite: rows 6..17 of 23. */
	static constexpr float TrackRailTopV = 6.f / 23.f;
	static constexpr float TrackRailV = 11.f / 23.f;

	// =============================================================================================
	// MOTION (P10) — one set of timings for every kit screen, all on REAL time
	// =============================================================================================

	/** An overlay's open fade: the page, the pause menu, the scoreboard, the death panel. */
	static constexpr float FadeInSeconds = 0.15f;

	/** ...and its close. A touch quicker: a closing menu should get out of the way. */
	static constexpr float FadeOutSeconds = 0.12f;

	/**
	 * A plate's hover ring coming on, and going off. Quick on, so the pointer never feels late; slower
	 * off, so a highlight walked down a list leaves a short fading trail instead of blinking.
	 */
	static constexpr float HoverInSeconds = 0.08f;
	static constexpr float HoverOutSeconds = 0.16f;

	/** The hover breath: tint = Base + Swing * sin(t * Speed), i.e. 0.90..1.10 around parity. */
	static constexpr float HoverPulseBase = 1.f;
	static constexpr float HoverPulseSwing = 0.10f;
	static constexpr float HoverPulseSpeed = 4.5f;

	// =============================================================================================
	// REAL TIME AND OPACITY (P10)
	// =============================================================================================

	/**
	 * The UI's clock: FApp::GetCurrentTime — the application's frame time. It keeps running while the
	 * world is paused, is not dilated, and is the same value for the whole frame. Use it (or
	 * UWorld::GetRealTimeSeconds) for anything that animates; never UWorld::GetTimeSeconds.
	 * DOUBLE on purpose: the platform clock carries a large offset, and narrowed to float it rounds.
	 */
	TRACE_API double RealSeconds();

	/**
	 * The opacity every kit draw in this file, and every TraceCanvasText draw, is multiplied by right
	 * now. 1 outside any FScopedOpacity.
	 */
	TRACE_API float Opacity();

	/**
	 * Multiplies Opacity() by @p Alpha for its lifetime, and restores it on exit; scopes nest by
	 * multiplying. This is how a whole screen fades without every draw call learning an alpha: wrap the
	 * screen's Draw in one of these. Covers the kit (plates, labels, chips, legends, value boxes,
	 * sliders, background, scrim) and TraceCanvasText. NOT the pointer (TraceHardwareCursor draws it
	 * at full strength): it is the player's, and it appears the frame the screen does. A screen's OWN
	 * raw HUD->DrawRect calls must pass their colour through Faded().
	 */
	struct TRACE_API FScopedOpacity
	{
		explicit FScopedOpacity(float Alpha);
		~FScopedOpacity();
		FScopedOpacity(const FScopedOpacity&) = delete;
		FScopedOpacity& operator=(const FScopedOpacity&) = delete;
	private:
		float Saved = 1.f;
	};

	/** @p Color with its alpha multiplied by Opacity() — for a screen's own raw DrawRect / DrawLine. */
	TRACE_API FLinearColor Faded(const FLinearColor& Color);

	// =============================================================================================
	// THE GUARD AND THE SPRITES
	// =============================================================================================

	/** True only when @p Texture can be handed to AHUD::DrawTexture this frame. Null is false. */
	TRACE_API bool IsDrawable(const UTexture* Texture);

	/** The object path of a kit sprite (TraceMenuArtStyle's), or null for Count. */
	TRACE_API const TCHAR* SpritePath(ETraceKitSprite Which);

	/**
	 * The kit sprite, or NULL — which every caller treats as "draw your fallback this frame".
	 *
	 * Loaded on first use, then ROOTED (nothing else on the arena map references these, and a GC
	 * mid-match would blank a screen). A failed load is remembered, so a broken install does not
	 * hunt for the package every frame. Returns null while the texture is loaded but not yet
	 * IsDrawable — the one-or-two-frame window that used to crash.
	 */
	TRACE_API UTexture2D* Sprite(ETraceKitSprite Which);

	/**
	 * The cached texture WITHOUT loading it and without the drawable check. Diagnostics only
	 * (readiness logs, harnesses): never draw what this returns.
	 */
	TRACE_API UTexture2D* PeekSprite(ETraceKitSprite Which);

	/**
	 * Loads (and roots) every kit sprite NOW, so their render resources exist long before any screen
	 * asks for them. Call it from a HUD's BeginPlay.
	 *
	 * Without it a screen's first frame loads the sprites synchronously inside a draw pass: the load
	 * stalls that frame, and because a freshly loaded texture has no RHI texture yet, every plate on
	 * it draws its flat fallback for a frame before the art appears (measured on the settings overlay:
	 * all 8 sprites not drawable on its first frame, and a 93 ms frame). Idempotent and cheap after
	 * the first call. Returns how many sprites are loaded.
	 *
	 * P11: it primes the POINTER too (TraceHardwareCursor::Prime, rooted like these), which was the one
	 * piece of kit art still loaded by its first draw — a FlushAsyncLoading on the frame team select
	 * opened. The return value still counts the ETraceKitSprite sheet only.
	 */
	TRACE_API int32 Prime();

	// =============================================================================================
	// STATES (stylespec §5) — the switch lifted out of the UMG title row, shared by both renderers
	// =============================================================================================

	/** Disabled outranks everything; a press is a hover with a finger down; else Default. */
	TRACE_API ETraceKitState StateFor(bool bEnabled, bool bSelected, bool bPressed = false);

	/**
	 *   Default   BtnDefault, tint 1.0,         label WordDefault,       furniture 0.85 white
	 *   Hover     BtnHover,   breathes,         label WordHoverLifted(), furniture white
	 *   Pressed   BtnHover,   tint PressedTint, label WordHoverLifted(), furniture white
	 *   Disabled  BtnDisabled, tint 1.0,        label WordDisabled,      furniture WordDisabled
	 *
	 * NOT in here, on purpose: the title screen's white outline on default plates and its amber
	 * selection rail. Those are owner-requested extras for the TITLE only (TraceMenuRowWidget adds
	 * them on top of this). Do not copy them to another screen.
	 */
	TRACE_API FTraceKitVisuals VisualsFor(ETraceKitState State);

	/** The hover breath at @p NowSeconds. @p Speed is radians per second. */
	TRACE_API float HoverPulse(float NowSeconds, float Speed = HoverPulseSpeed);

	/** The plate multiplier @p Visuals wants at @p NowSeconds: HoverPulse if it breathes, else PlateTint. */
	TRACE_API float PlateTintAt(const FTraceKitVisuals& Visuals, float NowSeconds);

	// ---- HOVER TRANSITIONS (P10) -----------------------------------------------------------------
	//
	// DrawStatePlate (and so DrawButton and DrawKeyChip) no longer SWAPS the default plate for the
	// hover plate in one frame: it keeps a small blend per plate, keyed by the plate's rect, and lays
	// the hover plate over the default one at that blend (HoverInSeconds on, HoverOutSeconds off, real
	// time). No screen keeps any state for it. A rect that was not drawn on the previous frame starts
	// at its target — a page that has just opened does not animate its highlight in, and a plate that
	// moves simply does what it always did. Disabled and Pressed are immediate (a press must feel
	// instant; a disabled plate is a different sprite).

	/**
	 * The hover blend (0 = default, 1 = hover) of the plate at this rect, advanced to this frame with
	 * @p bHovered as its target. Idempotent within a frame, so a screen can ask for it to colour its
	 * own label and the plate draw will read the same value.
	 */
	TRACE_API float HoverBlend(float X, float Y, float W, float H, bool bHovered);

	/**
	 * Every hover blend asked inside this scope (DrawStatePlate, DrawButton, DrawKeyChip, VisualsAt,
	 * HoverBlend) is keyed by @p Salt as well as by the plate's rect. The previous salt comes back on
	 * exit. Outside any scope the salt is 0, and the keys are the plain rect keys every screen had.
	 *
	 * NEEDED WHEN TWO SURFACES DRAW PLATES ON THE SAME RECTS IN ONE FRAME. The pause menu's LOADOUTS
	 * editor and the match loadout page are two FTraceLoadoutSelects with one layout, drawn page first,
	 * editor second. Keyed by rect alone they shared one blend per card, and the second ask in a frame
	 * reads what the first left, so the editor's hover ring sat on the card lit on the page beneath it.
	 * Pass something that tells the two apart (the instance's address works).
	 */
	struct TRACE_API FScopedHoverSalt
	{
		explicit FScopedHoverSalt(uint32 Salt);
		~FScopedHoverSalt();
		FScopedHoverSalt(const FScopedHoverSalt&) = delete;
		FScopedHoverSalt& operator=(const FScopedHoverSalt&) = delete;
	private:
		uint32 Saved = 0;
	};

	/** Default -> Hover visuals mixed at @p Blend (label, furniture). For Disabled/Pressed, VisualsFor(State). */
	TRACE_API FTraceKitVisuals VisualsForBlend(ETraceKitState State, float Blend);

	/**
	 * VisualsFor, eased: the label and furniture colours of the plate at this rect as they are THIS
	 * frame, mid-transition included. Use it where a screen colours its own words on a state plate.
	 */
	TRACE_API FTraceKitVisuals VisualsAt(ETraceKitState State, float X, float Y, float W, float H);

	// =============================================================================================
	// PLATES
	// =============================================================================================

	/**
	 * The quads a plate is drawn as. Pure geometry — no texture, no HUD — so a harness can check it.
	 *
	 * @param CornerHeight  the plate height the CORNER is sized for, in screen px. Pass H for a
	 *                      button (the corner is the sheet's own); pass a button's height (e.g.
	 *                      60*UIScale) for a tall card, or the artist's corner turns into a lozenge.
	 * @return how many of @p Out were filled (0 when the rect or frame cannot describe a plate).
	 *
	 * The sprite is drawn over (X, Y, W, H) grown by Frame.Glow * CornerHeight / Frame.PlateH on
	 * every side, so the PLATE lands on the rect and the baked glow overhangs it. With
	 * CornerHeight == H this is exactly what the old 3-slice drew (Trace.UI.Kit.Verify proves it).
	 */
	TRACE_API int32 PlateQuads(const TraceMenuArtStyle::FSpriteFrame& Frame,
		float X, float Y, float W, float H, float CornerHeight, FTraceKitQuad (&Out)[9]);

	/**
	 * The artist's plate, 9-sliced onto the rect. The texture-level primitive.
	 *
	 * @return false, having drawn NOTHING, when @p Texture is not drawable this frame (null, no
	 *         resource, no RHI texture) or the rect is degenerate. The caller draws its fallback.
	 */
	TRACE_API bool DrawPlate(AHUD* HUD, UTexture2D* Texture, const TraceMenuArtStyle::FSpriteFrame& Frame,
		float X, float Y, float W, float H, float CornerHeight, const FLinearColor& Tint);

	/**
	 * The kit's stand-in for a plate whose texture is not drawable: a flat rect in the plate's own
	 * fill (DisabledFill + a DisabledRing edge for Disabled, an AmberLifted edge for Hover/Pressed),
	 * scaled by @p Tint. Seen for a frame or two after a cold load, or in a build with no art.
	 */
	TRACE_API void DrawFallbackPlate(AHUD* HUD, ETraceKitState State, float X, float Y, float W, float H,
		float Tint = 1.f);

	/**
	 * The plate for @p State: the state's sprite at the state's tint (breathing if it breathes),
	 * with DrawFallbackPlate when the sprite is not drawable. @p CornerHeight <= 0 means H.
	 */
	TRACE_API void DrawStatePlate(AHUD* HUD, ETraceKitState State, float X, float Y, float W, float H,
		float NowSeconds, float CornerHeight = 0.f);

	/**
	 * A PANEL: the state's plate as a surface rather than a control (the match HUD's score bar,
	 * chips, kill-feed rows, cards). Two differences from DrawStatePlate, both deliberate:
	 *
	 *   * it never breathes. A hover plate here means "this one is about you", not "the pointer is
	 *     on it", and a panel that pulsed for as long as it was up would be a strobe;
	 *   * @p Alpha multiplies the sprite AND its fallback, so a panel can fade out with the words on
	 *     it. A fading row whose plate stayed solid is a box with nothing in it.
	 *
	 * @p CornerHeight <= 0 means H. Draws nothing at Alpha <= 0. Returns true when the sprite drew,
	 * false when the flat fallback did (or nothing did).
	 */
	TRACE_API bool DrawPanelPlate(AHUD* HUD, ETraceKitState State, float X, float Y, float W, float H,
		float CornerHeight = 0.f, float Alpha = 1.f);

	// =============================================================================================
	// TEXT ON THE KIT
	// =============================================================================================

	/**
	 * The text size whose caps are LabelCapFraction of @p PlateH in @p Weight. Caps differ by face
	 * (Erbaum's are taller than Sofachrome's at one size), so the weight is part of the answer.
	 */
	TRACE_API float LabelSize(float PlateH, ETraceTextWeight Weight = ETraceTextWeight::Light);

	/**
	 * @p Text centred on (CenterX, CenterY) by its CAPS, sized for a @p PlateH plate, shrunk to fit
	 * @p MaxWidth if it would not (MaxWidth <= 0: no limit). Light weight by default (owner's choice,
	 * v23); the settings submenus pass ETraceTextWeight::Hud for their body (Erbaum Bold, spec v26 §2).
	 * Draws nothing for an empty string. Returns the width drawn. @p bTabularDigits for a VALUE that
	 * changes (a slider's number, a countdown): see TraceText::FStyle::bTabularDigits.
	 */
	TRACE_API float DrawLabel(AHUD* HUD, const FString& Text, float CenterX, float CenterY, float PlateH,
		const FLinearColor& Color, float MaxWidth = 0.f, ETraceTextWeight Weight = ETraceTextWeight::Light,
		bool bTabularDigits = false);

	// =============================================================================================
	// CONTROLS
	// =============================================================================================

	/**
	 * A whole kit button: DrawStatePlate + the label centred in the state's colour.
	 *
	 * AN EMPTY LABEL DRAWS NOTHING and returns false (the text contract: Ranen removes a line with
	 * "KEY =", and chrome around a removed string must go with it). Use DrawStatePlate for a plate
	 * that is meant to be empty.
	 */
	TRACE_API bool DrawButton(AHUD* HUD, ETraceKitState State, float X, float Y, float W, float H,
		const FString& Label, float NowSeconds, float CornerHeight = 0.f);

	/** The value box's plate (T_MenuValueBox, ValueFrame, gold glow) with its own fallback. */
	TRACE_API void DrawValueBoxPlate(AHUD* HUD, float X, float Y, float W, float H,
		const FLinearColor& Tint = FLinearColor::White);

	/**
	 * A value box holding @p Text, centred. The sheet sets the number in olive; use white
	 * (WordDefault) or WordHoverLifted(), never cyan. Draws nothing for an empty string. Its digits are
	 * TABULAR: a value box holds a value, and a value that changes must not slide about in its box.
	 */
	TRACE_API bool DrawValueBox(AHUD* HUD, float X, float Y, float W, float H, const FString& Text,
		const FLinearColor& TextColor = FLinearColor::White);

	// ---- THE FULL-SCREEN PAGES' HEADER LINE: team select and the loadout page (1080p design px) ----

	/** Every screen title's cap height: SETTINGS, PAUSED, JOIN A GAME, FULL TIME, and both pages. */
	static constexpr float PageTitleCapPx = 30.f;

	/** Where the two pages centre their title's caps, from the top of the view. */
	static constexpr float PageTitleCapMidPx = 56.f;

	/** The two pages' side margin. The countdown's box is right-aligned to it. */
	static constexpr float PageMarginPx = 54.f;

	/** The countdown's value box height, and the type size of its TIME label. */
	static constexpr float PageClockBoxPx = 44.f;
	static constexpr float PageClockLabelPx = 18.f;

	/**
	 * THE PAGES' COUNTDOWN: TIME, then the whole seconds left in the kit's value box, right-aligned to
	 * the page margin with its caps on the title's line. The number pulses amber for the last five
	 * seconds. The loadout page draws it in its header, and the pause menu draws it in the same place
	 * over either page, so pausing never hides the clock that will close the page underneath. Honours
	 * the current opacity. Returns the left edge of what it drew; nothing is drawn for @p SecondsLeft < 0.
	 */
	TRACE_API float DrawPageClock(AHUD* HUD, float ViewW, float UIScale, float SecondsLeft, float NowSeconds);

	/**
	 * The slider's trough (T_MenuSliderTrack), @p H being the SPRITE's height (halo included; the
	 * rail itself is TrackRailV of it). Samples a clean band of the sprite for the stretched middle,
	 * so no baked blade can repeat along it. Fallback: a PlateFill rail.
	 */
	TRACE_API void DrawSliderTrack(AHUD* HUD, float X, float Y, float W, float H,
		const FLinearColor& Tint = FLinearColor::White);

	/**
	 * The slider's RAIL (T_MenuSliderTrack) as a meter's groove, for the match HUD's bars. (X, Y, W, H) is
	 * the rail ITSELF — the navy between its two gold lips, lips included — not the sprite: the gold halo
	 * overhangs it on every side by at most @p MaxHalo px, squashed rather than cut so it still fades out,
	 * which lets meters stacked a few pixels apart keep the kit's glow without their halos piling up. A
	 * fill belongs inside RailLipInset(H) of the two long edges. Fallback: a PlateFill rect with gold edges.
	 */
	TRACE_API void DrawRail(AHUD* HUD, float X, float Y, float W, float H, float MaxHalo,
		const FLinearColor& Tint = FLinearColor::White);

	/** How far inside a DrawRail rect @p H tall its gold lips reach, in whole pixels, at least 1. */
	TRACE_API float RailLipInset(float H);

	/** Where the blade handle sits for a track of sprite height @p TrackH centred on (CenterX, CenterY). */
	TRACE_API FBox2D SliderHandleRect(float CenterX, float CenterY, float TrackH);

	/**
	 * The kit's thumb: the white blade (T_MenuSliderHandle), SliderHandleToTrack x the track's
	 * sprite height, centred on the value's point on the rail. Fallback: a white vertical bar.
	 *
	 * READ UI QA FINDING 6b BEFORE USING IT: this blade is the same picture as the pointer, and the
	 * options page stopped drawing it for that reason. It is here because the style spec names it.
	 */
	TRACE_API void DrawSliderHandle(AHUD* HUD, float CenterX, float CenterY, float TrackH,
		const FLinearColor& Tint = FLinearColor::White);

	/** The width DrawKeyChip would take: KeyChipAspect * H, or wider for a long key name. */
	TRACE_API float KeyChipWidth(const FString& Key, float H);

	/**
	 * A KEY chip — the button plate at 1.67:1 with the key's name centred — at (X, Y), @p H tall.
	 * Returns the width drawn; an empty key name draws nothing and returns 0.
	 */
	TRACE_API float DrawKeyChip(AHUD* HUD, ETraceKitState State, float X, float Y, float H,
		const FString& Key, float NowSeconds);

	/**
	 * @p Text with its CAPS centred on @p CapCenterY — the way the kit sits every word on a plate —
	 * and X read per @p Style's HAlign. Returns the width drawn; nothing for an empty string.
	 */
	TRACE_API float DrawTextCapCentered(AHUD* HUD, const FString& Text, float X, float CapCenterY,
		const TraceText::FStyle& Style);

	/** The text style whose CAPS are @p CapH tall in @p Weight. */
	TRACE_API TraceText::FStyle CapStyle(float CapH, const FLinearColor& Color,
		ETraceTextWeight Weight = ETraceTextWeight::Light);

	/** The width @p Text takes at caps @p CapH in @p Weight. 0 for an empty string. */
	TRACE_API float CapTextWidth(const FString& Text, float CapH, ETraceTextWeight Weight = ETraceTextWeight::Light);

	/**
	 * @p Text with caps @p CapH tall, centred on @p CapCenterY, X per @p HAlign, shrunk to fit
	 * @p MaxW (0: no limit). Draws nothing for an empty string. Returns the width drawn. The caption /
	 * note / field-text primitive for a kit screen that is not a button.
	 */
	TRACE_API float DrawCapText(AHUD* HUD, const FString& Text, float X, float CapCenterY, float CapH,
		const FLinearColor& Color, ETraceTextWeight Weight = ETraceTextWeight::Light,
		TraceText::EHAlign HAlign = TraceText::EHAlign::Center, float MaxW = 0.f);

	// ---- A KEY LEGEND: [KEY] VERB   [KEY] VERB ... ----------------------------------------------
	//
	// A footer of KEY chips, each followed by the verb it does, in the light face at the chip's own
	// label size and the unselected furniture colour. Everything scales with the chip height, so a
	// width measured at one height scales linearly to any other: two legends on one page (keyboard and
	// pad) fit to ONE scale by measuring both with KeyLegendWidth and drawing both at the fitted height.

	/** The width DrawKeyLegend takes at chip height @p ChipH. Pairs with an empty key or verb are skipped. */
	TRACE_API float KeyLegendWidth(const TArray<FTraceKitLegendItem>& Items, float ChipH);

	/** The legend, centred on @p CenterX with the chips' tops at @p Y. Returns the width drawn. */
	TRACE_API float DrawKeyLegend(AHUD* HUD, const TArray<FTraceKitLegendItem>& Items, float CenterX, float Y,
		float ChipH, float NowSeconds);

	/**
	 * The chip height that makes the widest of @p Widths (each measured at @p ChipH) fit @p MaxW —
	 * never below 70 % of @p ChipH, where the type stops being readable and clipping is the better
	 * failure.
	 */
	TRACE_API float KeyLegendFit(float ChipH, float MaxW, std::initializer_list<float> Widths);

	/**
	 * A straight stroke from (X0, Y0) to (X1, Y1), @p Thickness px wide, that HONOURS ALPHA and the
	 * current opacity. AHUD::DrawLine does not: the engine sets a line's alpha to 1 before batching it,
	 * so a dimmed chevron drawn with it came out at full strength. Two translucent triangles instead.
	 */
	TRACE_API void DrawStroke(AHUD* HUD, float X0, float Y0, float X1, float Y1, const FLinearColor& Color,
		float Thickness);

#if !UE_BUILD_SHIPPING
	/** How many DrawStroke calls have reached a canvas, and the colour (fade included) of the last. */
	TRACE_API int64 DebugStrokesIssued();
	TRACE_API FLinearColor DebugLastStrokeColor();
#endif

	/** Opaque black over the whole view (stylespec §1). */
	TRACE_API void DrawBackground(AHUD* HUD, float ViewW, float ViewH);

	/** A modal scrim over the whole view: black at @p Alpha. */
	TRACE_API void DrawScrim(AHUD* HUD, float ViewW, float ViewH, float Alpha = ScrimAlpha);

	/**
	 * The travel card's "still working" sign (P12): the kit's crescent (ETraceKitSprite::Chevron,
	 * T_MenuBack) in the bottom-right corner, turning once per TraceTitleLayout::SpinnerPeriodSeconds on
	 * the PLATFORM clock (@p PlatformSeconds = FPlatformTime::Seconds()), so it is at the same angle as
	 * the Slate loading card that takes over from this one. Honours the current opacity. Draws nothing
	 * until the sprite is drawable; returns whether it drew.
	 */
	TRACE_API bool DrawTravelSpinner(AHUD* HUD, float ViewW, float ViewH, float UIScale, double PlatformSeconds);

	// =============================================================================================
	// THE POINTER — owned by UI/TraceHardwareCursor.h; these are the kit's names for it
	// =============================================================================================

	/** TraceHardwareCursor::DrawPointer: the blade, tip on @p Tip. False = not drawable, draw yours. */
	TRACE_API bool DrawCursor(AHUD* HUD, const FVector2D& Tip, float UIScale);

	/**
	 * The whole §9 recipe for a screen that shows the pointer this frame: EnsureRunning,
	 * RenewSuppression(PC, Owner) — hides the OS arrow for two frames — then DrawCursor.
	 * @p Owner must be a string literal (it is not copied).
	 */
	TRACE_API bool ShowCursor(AHUD* HUD, APlayerController* PC, const TCHAR* Owner, const FVector2D& Tip,
		float UIScale);

#if !UE_BUILD_SHIPPING
	/**
	 * `Trace.UI.Kit.Specimen 1`: every control above, in every state, on a black page over whatever
	 * the HUD drew. ATraceMenuHUD calls this last in DrawHUD. A no-op while the CVar is 0.
	 */
	TRACE_API void DrawSpecimenIfRequested(AHUD* HUD, float ViewW, float ViewH, float UIScale, float NowSeconds);
#endif
}
