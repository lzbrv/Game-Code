// Trace — the in-match HUD.
//
// Mostly AHUD::DrawHUD Canvas drawing, with text from the engine's built-in fonts via
// GEngine->Get*Font(). The old header here said "no UMG, no widget blueprints, no .uasset of any
// kind (contract §2)"; BOTH of those contracts are retired (spec v17 §0) and the line is corrected
// rather than deleted, because a stale justification is how this project has repeatedly fooled
// itself. Trace.Build.cs now links UMG/Slate/SlateCore, and the arena bake authored 572 actors.
//
// *** WHAT IS UMG TODAY, AND WHAT IS NOT. *** Spec v17 §4 step 4b converted exactly ONE element:
// the bottom-right ammo + status corner, which now has two presenters behind the Trace.UI.UseUMG
// toggle — Content/Trace/UI/HUD/WBP_TraceHudCorner, with the Canvas pass below it as a live
// fallback that runs whenever the asset is absent, fails to validate, or the toggle is off.
// EVERYTHING ELSE ON THIS HUD IS STILL CANVAS and is not half-converted: the crosshair, the charge
// ring, the bottom-LEFT health/dash/ability-cooldown stack, the kill feed, the scoreboard,
// the death panel, the banners, the pause menu and the character select screen. See
// DrawAmmoAndStatuses() and BuildCornerState().
//
// This is the only UI the team gets for a while, so it is written to be edited:
//   * every element lives in its own small pass, called in back-to-front order from DrawHUD();
//   * all layout is authored against a 1080p-tall viewport and multiplied by UIScale;
//   * every colour comes from TraceTeamColor() or the TraceHUDStyle palette in the .cpp, so the
//     HUD always matches the world and there are no one-off literals to hunt down.
//
// Nothing here reaches into gameplay state directly beyond read-only getters. If a pass needs a
// new piece of information, add an accessor to ATracePlayerController (same ownership slice)
// rather than widening a gameplay class.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"          // ETraceTeam, TraceTeamColor
#include "UI/TraceCharacterSelect.h" // FTraceCharacterSelect — spec v14 §3
#include "UI/TraceLoadoutSelect.h"   // FTraceLoadoutSelect — the page that replaced it
#include "UI/TraceKillFeed.h"     // ETraceKillIcon, ATraceKillFeedRelay — spec v8 §6
#include "UI/TraceOptionsMenu.h"  // FTraceOptionsMenu
#include "UI/Widgets/HUD/TraceHudCornerData.h" // FTraceHudCornerState — spec v17 §4 (step 4b)
#include "UI/Widgets/Menu/TraceKitMotion.h"   // FTraceKitFade — the overlays' open/close fades (P10)

#include "TraceHUD.generated.h"

class ATraceCharacter;
class ATraceGameState;
class ATracePlayerController;
class ATracePlayerState;
class ATracePracticePad;
class UFont;
class UTraceHudCornerWidget;

UCLASS()
class TRACE_API ATraceHUD : public AHUD
{
	GENERATED_BODY()

public:
	//~ Begin AHUD interface
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	//~ End AHUD interface

	/** Which presenter drew the bottom-right corner. Reported by Trace.HUD.Corner.Verify. */
	enum class ECornerPath : uint8
	{
		/** Nothing decided yet — no frame has been drawn. */
		Undecided,
		/** The shipped Canvas passes. The fallback, and still the default. */
		Canvas,
		/** WBP_TraceHudCorner + WBP_TraceHudStatusChip, behind Trace.UI.UseUMG. */
		Umg,
	};

	/** Which presenter the LAST DRAWN FRAME used, and why. Never what was intended. */
	ECornerPath GetCornerPath() const { return CornerPath; }

	/** Human-readable reason the UMG corner was not adopted, or empty while it is in use. */
	const FString& GetCornerFallbackReason() const { return CornerFallbackReason; }

	/**
	 * THE REFUSAL TOAST (release FX/AUDIO plan §7.1, closes F3). One chip directly above the
	 * bottom-left ability row, for @p Seconds, fading out over the last 0.3 s.
	 *
	 * *** ONE SLOT, AND A NEW TOAST REPLACES THE OLD. *** A player mashing a cooling key produces a
	 * refusal per press; a stack would fill the corner with the same sentence written six times, and
	 * a queue would show them a message about a press they made two seconds ago. The newest refusal
	 * is the only one that describes the world they are in now.
	 *
	 * Called from the OWNING PLAYER'S machine only — UTraceAbilityComponent::TryActivate's three
	 * refusal arms and the V-cooldown arm (see TraceAbilityToast there, which does the filtering).
	 * This function does not re-check that: it draws what it is told, on the HUD it was called on,
	 * and a HUD only exists on a machine with a screen.
	 *
	 * IT ALSO OWNS THE SOUND. UIDeny plays here, rate-limited to one per 0.5 s, so a held key cannot
	 * machine-gun it and so there is exactly one rate limiter rather than one per producer.
	 */
	void ShowAbilityToast(const FText& Text, const FLinearColor& Tint, float Seconds = 1.6f);

	/** The words of the latest toast (empty when none was ever raised). Read by Trace.Flow.Verify. */
	const FText& GetLastAbilityToastText() const { return ToastText; }

	/**
	 * The player's OWN binding for the input action with config id @p ConfigId, upper case, or
	 * @p Fallback when the input slice has no such action or nothing is bound.
	 *
	 * Static and public because the refusal toast is worded in Abilities/ and has to print the same
	 * key the ability row prints. Three call sites had the identical lookup loop copied out before
	 * this existed (the ability row's "[E]", the ammo block's "[R] RELOAD", and the toast); a
	 * hardcoded key is a HUD that lies to the first player who rebinds it, and three copies of the
	 * lookup is three places to forget.
	 */
	static FString ActionKeyLabel(const TCHAR* ConfigId, const TCHAR* Fallback);

#if !UE_BUILD_SHIPPING
	/**
	 * Spec v16 §2 — prints WHAT THE LAST DRAWN FRAME ACTUALLY CONTAINED, for Trace.HUD.V16.Report
	 * and for the shot sequence that pairs a line of this with every screenshot it takes.
	 *
	 * It reports the DRAW RECORD, never the gameplay state that fed it. A report built from the
	 * weapon component would say "30 rounds" for a HUD that drew nothing at all, which is the exact
	 * class of self-certifying harness this project has already been burned by. Every field it
	 * prints is written at the point the pixels are emitted — including the number of magazine ticks
	 * and ring chords actually issued, so an element that computed a healthy value and then drew
	 * nothing shows up as a zero rather than as a pass.
	 */
	void LogV16DrawRecord(const TCHAR* Tag) const;

	/** The same record, for a harness that has to ASSERT on it rather than print it. */
	struct FV16DrawRecord
	{
		bool  bAmmoBlock = false;
		FString AmmoText;
		bool  bBeeClip = false;
		bool  bReloadBar = false;
		int32 MagazineTicks = 0;
		int32 ChipCount = 0;
		FString ChipText;
		bool  bChargeRing = false;
		float ChargeRingAlpha = -1.f;
		int32 ChargeRingChords = 0;
		bool  bChargeBar = false;
	};
	FV16DrawRecord GetV16DrawRecord() const;

	/**
	 * The FX/AUDIO plan §7 half of the draw record, for Trace.HUD.FxHudShots.
	 *
	 * Separate from FV16DrawRecord rather than bolted onto it: that struct is spec v16 §2's contract
	 * with a harness that predates this pass, and widening it would silently change what every
	 * existing assertion in TraceHUDV16Shots is reading.
	 */
	struct FFxHudDrawRecord
	{
		bool  bHitMarker = false;
		bool  bShieldBlockedMarker = false;
		FString ToastText;
		FString SecondaryRowText;
		FString Vignettes;
		FString ChipText;
		int32 ChipCount = 0;
	};
	FFxHudDrawRecord GetFxHudDrawRecord() const;

	/** Prints the record above with @p Tag, so a screenshot and a line of log are the same event. */
	void LogFxHudDrawRecord(const TCHAR* Tag) const;

	/**
	 * The handmade-kit HUD pass's half of the draw record, for Trace.HUD.Kit.Verify. Same rule as the
	 * two above: every field is written where the pixels are emitted, never from the state that fed
	 * the pass, and the whole record is cleared at the top of every DrawHUD.
	 */
	struct FHudKitRecord
	{
		/** A pause / select / loadout screen owned the view on this frame. */
		bool bOverlayUp = false;

		/**
		 * P10 — the fades as this frame drew them: the match layer (1 - the most opaque overlay), the
		 * live chrome (that, times 1 - the results screen), the pages' shared black, the pause menu, the
		 * results screen, and each page's own content fade.
		 */
		float MatchLayerOpacity = 1.f;
		float ChromeOpacity = 1.f;
		float PageBackdropAlpha = 0.f;
		float PauseAlpha = 0.f;
		float ResultAlpha = 0.f;
		float TeamSelectAlpha = 0.f;
		float LoadoutAlpha = 0.f;
		float ScoreboardAlpha = 0.f;
		float DeathAlpha = 0.f;
		float HalfTimeAlpha = 0.f;

		/**
		 * The UI clock (TraceMenuKit::RealSeconds) this frame's fades were advanced to, and three of the
		 * fades' raw LINEAR progress: Trace.UI.Fade.Verify checks that progress moved by exactly the real
		 * time that passed, frame by frame — which a world-time or per-frame fade cannot do.
		 */
		double DrawRealSeconds = 0.0;
		float PauseLinear = 0.f;
		float ScoreboardLinear = 0.f;
		float PageBackdropLinear = 0.f;

		/** The open flags behind those fades, as this frame's draw saw them. */
		bool bTeamSelectOpen = false;
		bool bLoadoutOpen = false;
		bool bPauseOpen = false;

		/**
		 * The two pages' titles as last set, in 1080p px: cap height, the line their caps are centred
		 * on, and letter spacing (team select, then the loadout page). The page turn must not change them.
		 */
		float TeamTitleCapPx = 0.f;
		float TeamTitleCapMidPx = 0.f;
		float TeamTitleTrackPx = 0.f;
		float LoadoutTitleCapPx = 0.f;
		float LoadoutTitleCapMidPx = 0.f;
		float LoadoutTitleTrackPx = 0.f;

		/** The top score bar, the crosshair, and how many kill-feed rows drew. */
		bool bTopPanel = false;
		bool bCrosshair = false;
		int32 KillFeedRows = 0;

		/** The live scoreboard's card alpha as last drawn (1: the arena does not show through it). */
		float ScoreboardPlateAlpha = 0.f;

		/** The bottom-left stack's plate, and the alpha it was drawn at (1: the world does not show through). */
		bool bStackPlate = false;
		float StackPlateAlpha = 0.f;

		/** How many meters this frame drew seated on the kit's rail (DrawMeter). */
		int32 MeterRails = 0;

		/** The ammo plate's two label words as drawn ("PISTOL" / "" at a full clip). */
		FString AmmoLabel;
		FString AmmoRightLabel;

		/** The left label's cap height as drawn, in 1080p px (screen px / UIScale), either corner path. */
		float AmmoLabelCapPx = 0.f;

		/** The capacity after the count ("/30") and its cap height as drawn, 1080p px, either corner path. */
		FString AmmoCapacity;
		float AmmoCapacityCapPx = 0.f;

		/** The ammo (or knife) plate's fill alpha as drawn: the Canvas plate's, or the UMG PlateFill's brush. */
		float AmmoPlateAlpha = 0.f;

		/** The bottom-right corner was the UMG widget this frame (false: the Canvas corner). */
		bool bUmgCorner = false;

		/** The KNIFE plate that stands in the ammo corner while the knife is out. */
		bool bKnifeBlock = false;

		/** The death panel, its killer line as drawn, and the kill-feed glyph it drew ("" for none). */
		bool bDeathPanel = false;
		FString DeathKillerLine;
		FString DeathIcon;

		/** The death panel's heading as drawn: its words, face (ETraceTextWeight as an int) and colour. */
		FString DeathHead;
		int32 DeathHeadWeight = -1;
		FLinearColor DeathHeadColor = FLinearColor::Transparent;

		/** The results screen's two headline lines as drawn (reason, then result), with face and colour. */
		FString ResultHead;
		int32 ResultHeadWeight = -1;
		FLinearColor ResultHeadColor = FLinearColor::Transparent;
		FString ResultLine;
		int32 ResultLineWeight = -1;
		FLinearColor ResultLineColor = FLinearColor::Transparent;

		/**
		 * The results screen's final score as drawn ("2 - 1"): on the kit's value box or not, its face,
		 * its cap height in 1080p px, and the blue and orange numbers' colours.
		 */
		FString ResultScore;
		bool bResultScoreBox = false;
		int32 ResultScoreWeight = -1;
		float ResultScoreCapPx = 0.f;
		FLinearColor ResultScoreBlueColor = FLinearColor::Transparent;
		FLinearColor ResultScoreOrangeColor = FLinearColor::Transparent;

		/** HALF TIME, the goal flash, and GO with the subtitle it carried. */
		bool bHalfTimeCard = false;
		bool bScoreFlash = false;
		bool bGoBanner = false;
		FString GoSubtitle;

		/** The HOSTING / CONNECTED chip, and the alpha it drew at. */
		bool bNetPanel = false;
		float NetPanelAlpha = 0.f;

		/** The network-failure banner, and every line of text it drew (the engine's text must not be one). */
		bool bNetFailurePanel = false;
		TArray<FString> NetFailureLines;

		/** Each kill-feed row's words as drawn, newest first ("KILLER > VICTIM", or "NAME LEFT"). */
		TArray<FString> KillFeedTexts;

		/** One practice-range pad label, as drawn over its pad (ATraceHUD::DrawPracticePadLabels). */
		struct FPadLabel
		{
			/** ETracePracticePadRole, as an integer so this header needs no practice include. */
			int32 PadRoleIndex = -1;
			FString Text;
			/** The plate's screen rect. The label's reading direction is left to right by construction. */
			FBox2D Rect = FBox2D(ForceInit);
			float Alpha = 0.f;
			/** The plate's own alpha as drawn: the label's distance fade alone, so the plate is opaque. */
			float PlateAlpha = 0.f;
			/** The amber-ringed (hover) plate: the pad's switch is ON. */
			bool bLit = false;
		};
		TArray<FPadLabel> PadLabels;
		FVector2D ViewSize = FVector2D::ZeroVector;
	};
	const FHudKitRecord& GetHudKitRecord() const { return HudKitRecord; }

	/** Trace.UI.Fade.Verify: the pause menu opened exactly as Escape opens it, and closed as RESUME does. */
	void DebugOpenPauseMenu() { OpenPauseMenu(); }
	void DebugClosePauseMenu() { PauseMenu.Close(); }
#endif

protected:
	// ---- Draw passes, in back-to-front order --------------------------------------------------

	/**
	 * Resolves where the reticle goes THIS frame, and who a pass would go to, before anything is
	 * drawn. Both are consumed by DrawCrosshair() and DrawPassProgress(), which must agree.
	 *
	 * Called once from DrawHUD() rather than from inside a pass, because two passes read it and
	 * the pass-target probe (a handful of line traces) may only ever run once a frame.
	 */
	void UpdateReticleAnchor();

	/**
	 * The crosshair. ALWAYS AT SCREEN CENTRE, in both camera modes — read the long note in the .cpp
	 * before changing that, it is a twice-reported bug ("there's still no crosshair in third person")
	 * and the previous fix failed because the reticle was drawn on the pass ray, ~30px below centre.
	 */
	void DrawCrosshair();

	/**
	 * The centre crosshair itself: four arms plus a centre dot, pixel-snapped, at @p CX,@p CY.
	 *
	 * @param Scale     1.0 in first person, ThirdPersonCrosshairScale in third.
	 * @param InkColor  fill colour; its alpha is multiplied by @p Visibility.
	 */
	void DrawAimReticle(float CX, float CY, float Visibility, float Scale, const FLinearColor& InkColor);

	/**
	 * The third-person PASS state, LAYERED ON the centre crosshair rather than replacing it: corner
	 * brackets concentric with it that close and take the team colour over a legal receiver, plus a
	 * small subordinate diamond at the projected pass ray (which is genuinely not screen centre in
	 * third person — see the note in the .cpp).
	 */
	void DrawPassReticle(float Visibility);

	/**
	 * The persistent "character art was never imported" warning, and the reason it exists at all —
	 * see the note in the .cpp. Draws nothing when the Mannequin is present.
	 */
	void DrawArtWarning();

	/**
	 * The 0.5s pass hold (spec §4), drawn as a ring closing around the reticle.
	 *
	 * This is the single most consequential half-second in the game — the moment the pass is input
	 * the carrier's trace becomes invulnerable AND their own shield drops — so the player needs to
	 * see the timer, not guess at it. Sits at screen centre because that is also where the receiver
	 * they must stay on is.
	 *
	 * *** THIS IS "THE OLD CIRCLE AROUND THE CROSSHAIR ANIMATION FOR GAME MODE A". *** Spec v16 §2
	 * asks for it to be reused for the throw charge; DrawThrowChargeRing() below is that reuse, and
	 * both go through the same DrawCrosshairRing() so there is exactly one ring in this file.
	 */
	void DrawPassProgress();

	/**
	 * SPEC v16 §2 — the throw charge, MOVED OFF THE BOTTOM-LEFT BAR AND ONTO THE CROSSHAIR.
	 *
	 * Verbatim: "For the throw charge, use the old circle around the crosshair animation for game
	 * mode a to demonstrate how charged /100% the throw is, rather than a bar on the hud."
	 *
	 * The old animation was not invented for this and was not recovered from git either: it is
	 * DrawPassProgress(), which has drawn mode A's hover-pass hold as a closing ring around the
	 * crosshair since the "Mechanics v2" commit and is still called every frame. This pass feeds the
	 * SAME ring geometry from ATraceCore's predicted charge instead, so the thing the player
	 * remembers is literally the thing they get.
	 *
	 * The two can never collide: mode A starts a hover pass and never a throw, mode B throws and
	 * never passes. The pass still wins if a build ever manages both at once — see the early-out.
	 */
	void DrawThrowChargeRing();

	/**
	 * True when DrawThrowChargeRing() will draw this frame. ONE definition.
	 *
	 * DrawCrosshair() used to ask it too, because it wrote an "LMB  -  THROW" caption on the same
	 * pixel row as the ring's "52%  -  POWER 66%" and the ring's had to win. That caption has since
	 * been removed (the co-developer's text pass), so only the ring itself asks now.
	 */
	bool IsThrowChargeRingUp() const;

	/**
	 * THE ONE RING. A @p FillAlpha-fraction arc closing clockwise from twelve o'clock around the
	 * centre crosshair, over a dim full-circle track, with @p Caption centred underneath it.
	 *
	 * Split out of DrawPassProgress() by spec v16 §2 so the pass hold and the throw charge are the
	 * same animation rather than two rings that drift apart. Radius is derived from the live
	 * ThirdPersonCrosshairScale setting, not from a literal — a ring pinned to a constant gets
	 * sliced by the crosshair arms the moment a designer raises that.
	 */
	void DrawCrosshairRing(float FillAlpha, const FLinearColor& FillColor,
		const FString& Caption, const FLinearColor& CaptionColor);

	void DrawHitMarker();

	/**
	 * OWNER-ONLY SCREEN-EDGE TINTS (FX/AUDIO plan §2.5 and §2.6; bible §6.4).
	 *
	 * Two states are invisible to the player they are happening to unless the screen says so: Elle's
	 * cloak (which changes nothing about her own view) and Oyster's poison (whose cloud is behind
	 * you the moment you run out of it). Both get a band around the frame edge, in the effect's own
	 * hue, at the alpha the bible fixes — 0.10 for the cloak, 0.18 for the poison.
	 *
	 * A BAND, NOT A FULL-SCREEN WASH, and the alphas are ceilings rather than suggestions: this is
	 * the one HUD element drawn over the whole play area, and a tint heavy enough to be pretty is a
	 * tint heavy enough to hide an enemy against a wall.
	 */
	void DrawOwnerVignettes();

	/**
	 * One vignette band: @p Hue fading from @p PeakAlpha at the frame edge to nothing inboard.
	 *
	 * Drawn as concentric single-colour frames because this HUD has no material and no gradient
	 * primitive — Canvas gives it DrawRect and nothing else. The step count is chosen so the ramp
	 * reads as smooth at 1080p and costs four rects per step; a real gradient would be a material,
	 * and a material would be an asset, and FX §0 keeps this pass asset-free.
	 */
	void DrawScreenEdgeVignette(const FLinearColor& Hue, float PeakAlpha);

	/**
	 * Health, dash CHARGES, the ability rows and the parry cooldown: the bottom-left stack, on the
	 * handmade kit's plate. (Boost is gone; the WEAPON row is gone too — the gun is named on the ammo
	 * plate, and the knife has its own plate there.)
	 *
	 * Row order is load-bearing, not cosmetic. The stack grows UPWARDS from the health bar, so a row
	 * drawn earlier sits lower and closer to health. Health never moves; DASH is drawn first because it
	 * is the one other row every player has every frame; the ability rows next; and the rows that come
	 * and go (PARRY while carrying, SLIDE while a slide-jump is live) sit ON TOP, so their coming and
	 * going moves nothing but the plate's top edge.
	 */
	void DrawHealthAndDash();

	/**
	 * Spec v14 §5 — the ACTIVATED ability's cooldown row, drawn as part of the bottom-left stack.
	 *
	 * *** IT DRAWS WHILE THE PLAYER IS DEAD, AND THAT IS THE WHOLE POINT OF THE PASS. *** The spec is
	 * explicit: "Character ability cooldowns should continue to countdown while a player is dead. They
	 * should not automatically reset due to death or a goal (a player can spawn with an ability timer
	 * still counting down)." A player who dies at 18 seconds remaining, respawns, presses E and gets
	 * nothing has been shown a bug unless the countdown was on screen the whole time. So this row is
	 * fed from ATracePlayerState — which survives the pawn — and never from the pawn or its component.
	 *
	 * Returns the Y the next row up should use, so the stack keeps growing upwards from health.
	 *
	 * @param RowY  top of the row to draw at.
	 */
	float DrawAbilityRow(float RowY, float Margin, float BarW, float RowH, float LabelW, const FLinearColor& TeamTint);

	/**
	 * FX/AUDIO plan §7.2 (closes F2) — the V row: a HALF-HEIGHT row UNDER the E row, drawn only for
	 * a character whose ability set overrides GetSecondaryCooldownDisplay().
	 *
	 * *** UNDER, WHICH MEANS IT IS DRAWN FIRST AND THE E ROW MOVES UP. *** The bottom-left stack
	 * grows upward from the health bar, so "under the E row" is the slot the E row was going to use.
	 * The row BELOW (dash) was already drawn and never moves — which is the stack's standing rule:
	 * only the top of the stack is allowed to shuffle.
	 *
	 * @param RowY  top of the half-height row. @return the Y the E row should now draw at.
	 */
	float DrawSecondaryCooldownRow(float RowY, float Margin, float BarW, float RowH, float LabelW,
		const FLinearColor& Accent);

	/**
	 * True when DrawSecondaryCooldownRow() will draw this frame.
	 *
	 * @param OutRowH     the half-height row's own height.
	 * @param OutAdvance  how far up the stack cursor moves past it — MEASURED off the font, not
	 *                    derived from the row box. The first capture of this row photographed the V
	 *                    caption printed through the E caption: both are FontSmall, and a text line
	 *                    is TALLER than a 6 px meter, so spacing the rows by their boxes overlapped
	 *                    their words. Every other consumer of a width on this HUD measures rather
	 *                    than assumes (see the label-gutter note in DrawHealthAndDash); this is the
	 *                    same rule applied to a height.
	 *
	 * Not const because measuring text is not: it goes through the atlas.
	 */
	bool IsSecondaryRowUp(float& OutRowH, float& OutAdvance, float RowH);

	/**
	 * The §7.1 refusal toast's draw pass. @p TopY is the top of the ability block, i.e. the Y
	 * DrawAbilityRow() returned, so the chip sits directly above the row the refusal was about.
	 */
	void DrawAbilityToast(float TopY, float Margin, float RowH);

	/**
	 * The health bar itself, split out of DrawHealthAndDash() because regeneration (spec v13 §1)
	 * gave it three states to draw rather than one width.
	 *
	 * THE POINT OF THE PASS, stated so nobody trims it as decoration: "a player learns to break line
	 * of sight rather than pushing". A bar that silently refills teaches nothing — the player has to
	 * see the clock running down while they are exposed, and see the climb start the moment it does.
	 * So this draws the countdown as well as the climb, and it draws them on the one element in the
	 * whole HUD that a player already looks at without being asked.
	 *
	 * @param X,Y,W,H  The bar's rect, in pixels.
	 */
	void DrawHealthBar(const class UTraceHealthComponent* HealthComp, float X, float Y, float W, float H);

	/**
	 * SPEC v16 §2 — THE BOTTOM-RIGHT CORNER: ammo in the corner itself, statuses stacking above it.
	 *
	 * Verbatim: "Show ammo in the hud on the bottom right" and "Statuses should show on the hud in
	 * the bottom right, separate from cooldowns (eg speed boost, poisoned, vulnerable, etc)".
	 *
	 * *** THE LAYOUT IS THE DECISION, SO IT IS WRITTEN DOWN HERE. *** Two features were told to
	 * share one corner, and the arrangement is the same one the bottom-left stack already uses and
	 * for the same reason:
	 *
	 *   * AMMO IS PINNED TO THE CORNER AND NEVER MOVES. It is read constantly, mid-fight, without
	 *     looking — exactly like health on the left — so it gets the fixed home. Every status is
	 *     conditional and would otherwise push it around.
	 *   * STATUSES STACK UPWARDS ABOVE IT, in a fixed priority order, so a status appearing or
	 *     expiring never moves the ammo count and only ever shuffles other statuses.
	 *
	 * "SEPARATE FROM COOLDOWNS" IS ANSWERED TWICE, because the corner alone is not enough — a player
	 * glancing down still has to be able to tell a status from a cooldown at speed:
	 *
	 *   1. cooldowns live in the bottom-LEFT stack and are untouched by this pass;
	 *   2. cooldowns FILL toward ready; statuses DRAIN toward gone. Opposite directions, so the two
	 *      never read as the same widget even out of the corner of an eye.
	 *
	 * Nothing here is drawn for a dead player: a status on a corpse is not information, and
	 * UTraceWeaponComponent::ShouldShowAmmo() already answers the ammo half.
	 */
	void DrawAmmoAndStatuses();

	// ---- Spec v17 §4 (step 4b): ONE STATE, TWO PRESENTERS --------------------------------------
	//
	// The corner is the only element on this HUD with a UMG path. The split below is what makes that
	// safe: everything that ASKS THE GAME A QUESTION happens once, in BuildCornerState(), and both
	// presenters are handed the answers. Neither may look at a weapon component, a health component
	// or an ability set. A UMG port that re-derived "is this a bee clip" would be a second definition
	// of a rule the gun already enforces, free to drift from it — which is the exact failure this
	// file's own comments spend two hundred lines warning about.

	/**
	 * Everything the corner shows this frame, asked of gameplay ONCE.
	 *
	 * @return false when the corner draws nothing at all: the red arm (Trace.HUD.V16 0), or a dead
	 *         player — a status on a corpse is not information, and poison dies with the body.
	 */
	bool BuildCornerState(FTraceHudCornerState& OutState) const;

	/** The shipped Canvas presenter. Unchanged pixels; it simply reads the state instead of the game. */
	void PresentCornerCanvas(const FTraceHudCornerState& InState);

	/**
	 * The UMG presenter. @return false when it could not run — no asset, a binding that did not
	 * resolve, no local player — in which case the caller falls back to Canvas THIS FRAME.
	 *
	 * @param bInLive false hides the corner without tearing the widget down (dead, red arm, an
	 *                overlay covering the screen, or the whistle having gone).
	 */
	bool PresentCornerUmg(bool bInLive, const FTraceHudCornerState& InState);

	/**
	 * Loads, builds, validates and adds the UMG corner (and its chip pool), once per HUD. @return true
	 * when CornerWidget exists afterwards. A failed adoption latches bCornerAdoptFailed and puts the
	 * corner on Canvas, exactly as it always did. P11: BeginPlay calls it (then collapses the corner),
	 * so the class load and widget build happen at map load rather than in the first frame's draw.
	 */
	bool AdoptCornerWidget();

	/** True when Trace.UI.UseUMG says the corner may use the widget. Never registers the cvar. */
	bool IsUmgCornerEnabled() const;

	/** Collapses the UMG corner if one exists. A no-op otherwise; safe every frame. */
	void HideCornerWidget();

	/**
	 * Records which presenter owns the corner and says so in the log — ONCE, on change.
	 *
	 * Spec v17 §0.1 does not merely ask for a fallback, it asks the fallback to ANNOUNCE ITSELF: a
	 * migration that quietly degrades is one nobody can tell has degraded. Per-frame logging would
	 * bury it, so this only speaks when the answer changes.
	 */
	void SetCornerPath(ECornerPath InPath, const FString& InReason);

	/**
	 * The ammo block itself, right-aligned with its bottom edge at @p BottomY. Returns the Y its top
	 * edge landed on, which is where the status stack starts growing upward from.
	 *
	 * GATED ENTIRELY ON UTraceWeaponComponent::ShouldShowAmmo() — never on a locally re-derived
	 * "the carrier has no gun", which would be a second definition of the rule able to disagree with
	 * the one the gun itself enforces. Returns @p BottomY unchanged when it draws nothing.
	 *
	 * SPEC v16 §1's Sting clause is the other half of this pass: X's Sting REPLACES the clip with 5
	 * bee rounds, so a player who does not know that sees 25 rounds vanish and reads it as the gun
	 * eating their ammo. A bee clip therefore changes three independent things at once — the colour,
	 * the shape of the magazine strip, and the words — so the difference survives a small window, a
	 * colour-blind player, and a compressed screenshot.
	 */
	float DrawAmmoBlock(const FTraceHudCornerState& InState, float RightX, float BottomY, float BlockW);

	/**
	 * The KNIFE plate, in the ammo plate's place while the knife is out: the word, one meter toward
	 * ready (the pullout, then the swing cooldown) and the cooldown's seconds. Returns its top edge.
	 */
	float DrawKnifeBlock(const FTraceHudCornerState& InState, float RightX, float BottomY, float BlockW);

	/**
	 * One status chip, right-aligned, with its BOTTOM edge at @p BottomY. Returns the Y above it.
	 *
	 * @param Fraction  0..1 of the effect REMAINING — the draining indicator spec v16 §2 demands
	 *                  ("a bare icon that never changes is not a status display"). Drains to empty.
	 * @param Readout   the duration text, right-aligned inside the chip. Never empty in practice;
	 *                  the two statuses with no clock (Mace's pull) pass a distance instead, which
	 *                  is that effect's real progress.
	 */
	float DrawStatusChip(float RightX, float BottomY, float ChipW, const FString& Label,
		const FString& Readout, float Fraction, const FLinearColor& Tint);

	void DrawScoresAndClock();
	void DrawCoreBanner();

	/**
	 * Top-right: what this process is on the network, and the address other people dial to reach it.
	 *
	 * "HOSTING — 100.101.102.103:7777" is a headline feature of spec v5 §0, not chrome. The reported
	 * bug was two machines each running a private match and nobody able to tell whether their setup
	 * worked; a host who can read their own address off their own screen, while somebody asks them
	 * for it over voice, is the affordance that makes the rest of this usable.
	 *
	 * NO LONGER PINNED TO EVERY FRAME (visual audit §4.1: "raw IPs" in all match footage). The
	 * panel now draws only while Tab holds the scoreboard, for a few seconds after the connection
	 * answer changes, or when the state is a warning (the failed-bind shout, which must never be
	 * hidden). The connection LOGIC and the human-count change log run every frame regardless —
	 * the log is load-bearing for two-machine triage and does not care whether the panel is up.
	 *
	 * It reads the world's ACTUAL net mode, never what the menu intended — so a listen server whose
	 * bind failed says OFFLINE rather than repeating a promise the process did not keep.
	 */
	void DrawNetworkStatus();

	/**
	 * P11: builds the network chip's words (role, headline, detail, accent) into the NetPanel* members
	 * and runs the change detection and change log that used to run every frame. Called by
	 * DrawNetworkStatus only when FNetPanelInputs moves, or once a second.
	 */
	void ComposeNetworkStatus();

	/**
	 * The practice range's pad labels (CORE RACK, INFINITE ABILITIES ON / OFF, LOADOUT): each a kit
	 * plate with the pad's words, projected over the pad's label anchor and drawn in screen space, so
	 * it faces the player from every side — the world-space text it replaces read mirrored from the
	 * range's own spawn line. Sized by distance like a sign in the world, faded out far away, hidden
	 * behind walls, and never under an overlay. A lit pad (INFINITE ABILITIES ON) wears the amber-ringed
	 * plate. Draws nothing outside the range (no pads exist) or for a label emptied in the text file.
	 */
	void DrawPracticePadLabels();

	/**
	 * The last connection or travel failure, if recent (12 s): its one-line headline on the kit's
	 * amber-ringed plate, centred. Same store the title screen draws from. Never under an overlay.
	 */
	void DrawNetworkFailureBanner();

	/**
	 * Spec v8 §6 — the kill feed. A right-hand stack of recent kills, newest at the top:
	 *
	 *     <killer>  [icon]  <victim>
	 *
	 * Names in their own team's colour, the glyph between them naming the CAUSE, rows ageing out
	 * after ATraceKillFeedRelay::EntryLifetime and capped at MaxDrawnEntries.
	 *
	 * READS A REPLICATED STORE, NOT LOCAL EVENTS, and that distinction is the whole point of the
	 * v8 pass. Every row this draws comes out of ATraceKillFeedRelay's replicated Entries array —
	 * property state, deliberately not a multicast RPC, because a dropped RPC is a row that is gone
	 * forever for one player while everyone else sees it (measured: 5 announced, 3 received). The
	 * host reads that array through the same code path a joining client does, so there is no
	 * arrangement of this HUD in which the host has a feed and a client does not. See TraceKillFeed.h.
	 *
	 * Drawn AFTER DrawNetworkStatus() so it can hang off the bottom of the "HOSTING — <address>"
	 * panel that shares the top-right corner (that panel publishes KillFeedTopY on the frames it
	 * draws; since WP8.4 gated it, most match frames leave the feed at the top-panel line).
	 */
	void DrawKillFeed();

	/**
	 * One kill-feed glyph, stroke-drawn into the box whose top-left is @p X,@p Y and whose side is
	 * @p Cell * 13 px (the legacy bitmap-grid box — the layout math in DrawKillFeed() is unchanged).
	 *
	 * NO IMPORTED ART AND NO UMG (contract §2 / spec v8 §6): every icon is a table of line strokes
	 * (plus DrawRect dots) authored in a 22x22 design box in the .cpp, scaled into the pixel box and
	 * drawn at the HUD's own stroke weight — 2 px at the 26 px 1080p icon, floored at 1.5 px — over
	 * a darker, thicker surround pass so the glyph survives crossing lit neon (bible §7.3: upgrade
	 * the glyph language to strokes, don't embrace the bitmaps).
	 */
	void DrawKillIcon(ETraceKillIcon Icon, float X, float Y, float Cell, const FLinearColor& Color);

	/**
	 * The centre-screen phase callout: the warm-up countdown, and "GO" as the match starts.
	 *
	 * This exists because of a specific complaint: the match "keeps stopping and restarting
	 * without anything seemingly changing". Nothing was restarting — the warm-up, the capture
	 * reset and the final whistle simply all looked identical, because none of them said anything.
	 * Every phase transition now announces itself.
	 */
	void DrawPhaseBanner();

	/** Two seconds of "BLUE SCORES" after a capture, so the field reset has a visible cause. */
	void DrawScoreFlash();

	/**
	 * Spec v6 §3, the carrier's half of the parry punish: "PARRIED — <name> DASHED YOUR TRACE".
	 *
	 * The dasher already gets an unmistakable answer (their own death panel names the carrier and
	 * the cause "Parried"). Without this the carrier gets NOTHING: they press Q, the trace flashes
	 * red for a tenth of a second, and an enemy they may never have seen dies somewhere behind them.
	 * A 0.1 s reaction check that cannot be confirmed is a mechanic players stop believing in.
	 *
	 * Two sources, in order, and both are needed: TraceParry's authoritative server-side record
	 * (which is what a listen-server HOST reads, and the host is the only human in a bot playtest),
	 * then ATracePlayerController::GetLastParryKillTime as the remote-client fallback.
	 */
	void DrawParryKillBanner();

	void DrawDeathPanel();
	void DrawMatchResult();
	void DrawScoreboard();

	/**
	 * The results screen's CONTINUE (ENTER / pad A). Host: ends the post-match window for everybody,
	 * as its timer would. Client: takes this player home. Once per HUD.
	 */
	void ContinueFromResults();

	/**
	 * True while the local player is holding the scoreboard open (Tab).
	 *
	 * THE one answer to that question on this HUD: DrawScoreboard() draws on it, and
	 * DrawNetworkStatus() uses it as a reveal gate — extracting it keeps the two passes from
	 * ever disagreeing about whether the board is up.
	 */
	bool IsScoreboardHeld() const;

	/** Draws one team's column of the scoreboard. Returns the height consumed, in pixels. */
	float DrawScoreboardTeam(ETraceTeam Team, float X, float Y, float Width);

	// ---- Small drawing helpers ----------------------------------------------------------------
	//
	// @p bTabular: tabular figures (TraceText::FStyle::bTabularDigits) — for a number that ticks (the
	// clock, a countdown, a score, the ammo count), so it keeps its width instead of sliding. A caller
	// that MEASURES such a number must pass the same flag.
	void DrawTextLeft(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale,
		bool bTabular = false);
	void DrawTextCentered(const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale,
		bool bTabular = false);
	void DrawTextRight(const FString& Text, const FLinearColor& Color, float RightX, float Y, UFont* Font, float Scale,
		bool bTabular = false);

	float MeasureWidth(const FString& Text, UFont* Font, float Scale, bool bTabular = false);
	float MeasureHeight(const FString& Text, UFont* Font, float Scale);

	/** Top-left Y that vertically centres @p Text inside the box [BoxY, BoxY + BoxH]. */
	float VCenterTextY(const FString& Text, UFont* Font, float Scale, float BoxY, float BoxH);

	/** Row of charge pips: @p Charges lit, the next one part-filled by @p PartialFraction. */
	void DrawChargePips(float X, float Y, float W, float H, int32 Charges, int32 MaxCharges,
		float PartialFraction, const FLinearColor& FillColor);

	/**
	 * Filled rect plus a thin border. NO LONGER the background of the match HUD's panels — those are
	 * the handmade kit's plate now (DrawKitPanel). Kept for the two WARNING panels that are meant to
	 * look like nothing else on screen: the missing-art warning and the network failure banner.
	 */
	void DrawPanel(float X, float Y, float W, float H, const FLinearColor& Fill, const FLinearColor& Border);

	/**
	 * THE HANDMADE KIT'S PLATE under a HUD panel: T_MenuBtn_Default 9-sliced through
	 * TraceMenuKit::DrawPanelPlate, its corner a button's (capped at 60 reference px) so a tall card
	 * keeps the artist's corner. @p bAboutYou draws the kit's amber-ringed plate instead (the menus'
	 * "this one" state) for a row that is about the local player. Never breathes; @p Alpha fades the
	 * plate with its contents.
	 */
	void DrawKitPanel(float X, float Y, float W, float H, float Alpha = 1.f, bool bAboutYou = false);

	/**
	 * A stroke whose ALPHA SURVIVES. AHUD::DrawLine goes through FBatchedElements::AddLine, which sets
	 * the colour's alpha to 1 before it is ever blended — so every "fade" drawn with it stayed at full
	 * strength and then vanished in one frame (the hit marker, the kill-feed glyphs). This draws the
	 * stroke as two translucent triangles instead. Use it for anything that fades.
	 */
	void DrawLineAlpha(float X0, float Y0, float X1, float Y1, const FLinearColor& Color, float Thickness);

	/**
	 * One right-hand caption of the bottom-left stack, drawn at (@p X, centred on the row), and
	 * RECORDED: the plate under the stack is sized to the widest caption the rows drew, so a short
	 * caption no longer sits in a wide empty panel. Digits are measured as zeros, so a countdown does
	 * not make the plate breathe.
	 */
	void DrawStackCaption(const FString& Text, const FLinearColor& Color, float X, float RowY, float RowH);

	/**
	 * True while a screen that owns the whole view is up — the pause menu, team select, character
	 * select, the loadout page. The match chrome (clock, banners, kill feed, crosshair, the two corners)
	 * does not draw under them: under their scrims it was ghost text cutting through the page title.
	 *
	 * P10: that is now reached through the FADE, not this flag: the chrome draws at MatchLayerOpacity
	 * (1 - the overlay's opacity) and stops drawing when that reaches 0, so it crossfades with the
	 * overlay rather than vanishing the frame the overlay opens. This flag stays the "is one open"
	 * answer for input and the draw record.
	 */
	bool IsFullScreenOverlayUp() const;

	// ---- P10: the overlays' fades, and what they leave of the match -----------------------------

	/**
	 * Advances every overlay fade this HUD owns (the pages' shared black, the results screen) and the
	 * pause menu's, and derives MatchLayerOpacity / ChromeOpacity from them. Top of DrawHUD, once.
	 */
	void UpdateOverlayFades();

	/**
	 * AHUD::DrawRect at the current TraceMenuKit::Opacity(): EVERY flat rect this HUD draws goes
	 * through here, so the match layer crossfades with an overlay instead of vanishing under it.
	 */
	void DrawHudRect(const FLinearColor& Color, float X, float Y, float W, float H);

	/**
	 * AHUD::DrawLine at the current opacity. AHUD::DrawLine throws alpha away (see DrawLineAlpha), so a
	 * faded frame is drawn as DrawLineAlpha and a full-strength frame exactly as before.
	 */
	void DrawHudLine(float X0, float Y0, float X1, float Y1, const FLinearColor& Color, float Thickness);

	/** Horizontal meter: drop shadow, dark trough, coloured fill from the left. Fraction is clamped. */
	void DrawMeter(float X, float Y, float W, float H, float Fraction, const FLinearColor& FillColor);

	/** mm:ss, clamped at zero. */
	static FString FormatClock(float Seconds);

	// ---- Per-frame scratch --------------------------------------------------------------------
	// Resolved once at the top of DrawHUD and consumed by the passes. Held as UPROPERTYs purely so
	// that nothing can ever leave a stale raw UObject pointer behind between frames.

	UPROPERTY(Transient) TObjectPtr<ATracePlayerController> TracePC;
	UPROPERTY(Transient) TObjectPtr<ATraceCharacter> LocalChar;
	UPROPERTY(Transient) TObjectPtr<ATracePlayerState> LocalPS;
	UPROPERTY(Transient) TObjectPtr<ATraceGameState> TraceGS;

	UPROPERTY(Transient) TObjectPtr<UFont> FontSmall;
	UPROPERTY(Transient) TObjectPtr<UFont> FontMedium;
	UPROPERTY(Transient) TObjectPtr<UFont> FontLarge;

	/** Viewport size in pixels. */
	float ViewW = 0.f;
	float ViewH = 0.f;

	/** Layout scale: every constant below is authored against a 1080p-tall viewport. */
	float UIScale = 1.f;

	/** Client-local world time for this draw. */
	float Now = 0.f;

	ETraceTeam LocalTeam = ETraceTeam::None;
	bool bLocalAlive = false;
	bool bLocalCarrying = false;
	bool bLocalDead = false;

	// bGoalMode IS GONE. Spec v4 §7's A/B toggle put it here — read from ATraceGameState once at the
	// top of DrawHUD so every pass below branched on the same answer — because the mode changed what
	// the left mouse button does while carrying, and a prompt that hardcoded the wrong ruleset's
	// wording taught the wrong control for half of a playtest. The endzone ruleset has been removed:
	// LMB throws, the banner says so, and there is nothing left to branch on.

	// ---- Reticle anchor, resolved once per frame by UpdateReticleAnchor() ----------------------

	/**
	 * 0 = fully first person, 1 = fully third person, eased — the camera's own blend, not the
	 * carrier bool, so everything keyed off it moves with the camera rather than a beat ahead of it.
	 */
	float ViewBlend = 0.f;

	/** Screen-space centre of the reticle. Exactly the viewport centre in first person. */
	float ReticleX = 0.f;
	float ReticleY = 0.f;

	/**
	 * How far down the pass ray the third-person reticle is anchored, in world units, eased so that
	 * acquiring or losing a receiver slides the reticle instead of teleporting it.
	 */
	float PassAnchorDistance = -1.f;

	/** Whoever a pass would go to right now. Weak: it is a pawn, and pawns die mid-frame. */
	TWeakObjectPtr<ATraceCharacter> HoveredPassTarget;

	/** Last time FindPassTargetFor() was run, so the probe is throttled rather than per-frame. */
	float LastPassTargetPollTime = -1000.f;

	/** Last receiver announced to the log, so the line is printed on change and never per frame. */
	TWeakObjectPtr<ATraceCharacter> LastLoggedPassTarget;

	/** 0 = no receiver, 1 = locked on. Eased, so the bracket close reads as a movement. */
	float PassLockAlpha = 0.f;

private:
	/**
	 * Death is observed locally rather than driven by ClientNotifyKilledBy: the pawn can die (or
	 * simply be destroyed) without the server ever sending us a killer — falling out of the arena,
	 * a trail kill on our own carrier, a mid-round travel. The RPC only supplies the killer's
	 * *name*; the countdown has to work regardless.
	 */
	float LocalDeathTime = -1000.f;

	/** Keeps the death panel off screen during the pre-match window, when we legitimately have no pawn. */
	bool bHasSpawnedOnce = false;

	bool bWasDeadLastDraw = false;

	// ---- Locally observed transitions ----------------------------------------------------------
	//
	// Both of these are edge detectors over replicated values rather than RPCs. A capture is
	// already fully described by the score changing, and the match phase by TraceMatchState; adding
	// an RPC for either would give a late joiner or a reconnecting client a *different* HUD from
	// everyone else's for no gain.

	// ---- Pause / settings overlay ----------------------------------------------------------------

	/**
	 * The pause menu, which is the same FTraceOptionsMenu the title screen puts up — opened one page
	 * higher, on its root, so the player gets RESUME / SETTINGS / RETURN TO TITLE / QUIT.
	 *
	 * The HUD owns it rather than the controller because it is a drawn thing, and because every other
	 * screen in this project is drawn by an AHUD; putting it on the controller would mean the
	 * controller owning Canvas layout, which is the one thing this codebase has consistently refused
	 * to do.
	 */
	FTraceOptionsMenu PauseMenu;

	/** Opens the pause menu, wires its callbacks and silences gameplay input. */
	void OpenPauseMenu();

	/**
	 * The character select screen (spec v14 §3).
	 *
	 * Owned by the HUD for the same reason the pause menu is: it is a drawn thing, and every screen in
	 * this project is drawn by an AHUD. It needs no Open() call — it follows the replicated
	 * ATracePlayerState::bCharacterSelectOpen, so a player who joins mid-warm-up gets it without the
	 * HUD having to notice them arriving. See TraceCharacterSelect.h.
	 */
	FTraceCharacterSelect CharacterSelect;

	/**
	 * THE PAGE THAT REPLACED THE CHARACTER GRID. Same open condition, same host, same frame slot —
	 * CharacterSelect still owns the TEAM screen and its overlay callbacks and simply stops drawing
	 * its own ten cards while `Trace.UI.LoadoutScreen` is on. See TraceLoadoutSelect.h.
	 */
	FTraceLoadoutSelect LoadoutSelect;

	/** Binds the select screen's input-suppression callbacks. Idempotent; called from BeginPlay. */
	void WireCharacterSelect();

	// ---- P10: overlay fades (all on real time; see UI/Widgets/Menu/TraceKitMotion.h) ----------------

	/**
	 * THE PAGES' SHARED BLACK. Team select, the loadout page and character select each fade their own
	 * content in and out, and each sits on opaque black; drawn only by the pages, the black would
	 * crossfade too, and the arena would show through the middle of the team -> loadout page turn (two
	 * half-faded blacks cover 75%). So the HUD keeps ONE black under all of them, faded on "any page up
	 * (or in its hand-off hold)" — it stays solid through a page turn and fades only at the ends.
	 */
	FTraceKitFade PageBackdropFade;

	/** The Tab scoreboard, the death panel, the HALF TIME card, and the full-time results. */
	FTraceKitFade ScoreboardFade;
	FTraceKitFade DeathFade;
	FTraceKitFade HalfTimeFade;
	FTraceKitFade ResultFade;

	/** The HALF TIME card's second line as last drawn, for its fade-out after the break ends. */
	FString HalfTimeSubline;

	/**
	 * What the overlays leave of the match THIS frame: 1 - the most opaque of (the pages' black, the
	 * pause menu). The kill feed, the net chips and all the chrome draw at it; 0 means nothing of the
	 * match draws (the old IsFullScreenOverlayUp gate, reached at the END of a fade instead of the start).
	 */
	float MatchLayerOpacity = 1.f;

	/** MatchLayerOpacity, further faded out as the results screen fades in — the live-play chrome. */
	float ChromeOpacity = 1.f;

	/** The UMG corner's render opacity as last set, so it is only touched when it changes. */
	float CornerWidgetOpacity = 1.f;

	// ---- The UMG corner (spec v17 §4, step 4b) --------------------------------------------------
	//
	// *** THE CANVAS PATH IS A LIVE FALLBACK, NOT A DELETED ONE (spec v17 §0.1). *** Asset missing,
	// asset invalid, toggle off — the corner is the one the game has drawn since v16 and the log says
	// which arm won, once, by name. The widget is created only when the toggle asks for it, so a build
	// with the toggle off never touches UMG. P11: when it IS on, BeginPlay adopts it (AdoptCornerWidget)
	// rather than the first frame's draw, so the class load and widget build land at map load; the
	// first frame then finds it built. A build with no UI assets pays one failed LoadClass either way.

	/** The corner widget, once adopted. Null while on the Canvas path. */
	UPROPERTY(Transient)
	TObjectPtr<UTraceHudCornerWidget> CornerWidget;

	/**
	 * Set once adoption has been attempted and failed, so a missing asset costs one LoadClass for the
	 * lifetime of the HUD instead of one per frame. Cleared only by a new HUD (a travel, a respawn of
	 * the whole world), which is also the only moment the asset could plausibly have appeared.
	 */
	bool bCornerAdoptFailed = false;

	/** Which presenter drew the corner on the last frame, and why the other one did not. */
	ECornerPath CornerPath = ECornerPath::Undecided;
	FString CornerFallbackReason;

	/** Guards the one-per-HUD adoption line, so the log names the arm once rather than every frame. */
	bool bLoggedCornerPath = false;

	/** Guards the one-per-HUD line naming the viewport, the HUD scale and the DPI scale. */
	bool bLoggedCornerScale = false;

	/**
	 * True when the UMG corner was addressed this frame, so DrawHUD can collapse it when it was not.
	 *
	 * Needed because the corner pass is INSIDE DrawHUD's `!bPostMatch` branch and behind a live
	 * player check, while a UMG widget added to the player screen keeps painting until something
	 * tells it not to. Without this, the last frame of a match would leave a frozen ammo count
	 * hanging over the full-time screen — the exact "a half-converted screen that draws nothing, or
	 * draws twice" failure spec v17 §4 calls out by name.
	 */
	bool bCornerAddressedThisDraw = false;

#if !UE_BUILD_SHIPPING
	/**
	 * -TraceAutoPause=<seconds> raises the pause menu mid-match, so a headless run can capture it.
	 *
	 * Same reasoning as -TraceAutoSettings on the title screen: a pause menu has no gameplay side
	 * effect to assert on, so the only proof it works is a frame with the panel in it.
	 */
	float AutoPauseAtSeconds = -1.f;
	bool bAutoPauseFired = false;
	float FirstDrawTime = -1.f;

	/**
	 * Draw counter, and the draw on which to capture the paused frame.
	 *
	 * DRAWS, not seconds, and this is the whole point. In standalone the pause menu calls
	 * SetPause(true), which stops the world clock — so every world timer stops with it, including
	 * TraceAutoShot's. Measured: with -TraceAutoPause=5 -TraceAutoShot=7, the pause landed first and
	 * the screenshot timer never fired again, so the one screen this switch exists to photograph was
	 * the one screen it could not photograph. DrawHUD keeps being called while paused, so counting
	 * draws is the only clock that still runs.
	 */
	int32 DrawCount = 0;
	int32 AutoPauseShotAtDraw = -1;
#endif

	/**
	 * One Display line on the first draw naming which of the not-yet-landed mechanics this build
	 * actually has. Without it, a missing parry meter is indistinguishable from a broken one — and
	 * this project has twice lost time to a mechanic that was merely quiet rather than dead.
	 */
	void LogAffordanceAvailabilityOnce();
	bool bLoggedAffordances = false;

	int32 LastSeenBlueScore = 0;
	int32 LastSeenOrangeScore = 0;
	bool bScoreCacheValid = false;

	/**
	 * Humans in the match as of the last line DrawNetworkStatus() logged, so the roster is written
	 * once per change instead of once per frame. -1 forces the first draw to report.
	 */
	int32 LastLoggedHumanCount = -1;

	/**
	 * The network panel's last answer (role|headline|detail, human count included), and when it
	 * changed.
	 *
	 * The HOSTING panel is no longer pinned to the screen for the whole match (a raw IP burned
	 * into every frame was the visual audit's §4.1 complaint): it draws while the scoreboard is
	 * held, for RevealSeconds after this answer changes (match start, a player joining, a role
	 * flip), and always for a warning state. Empty string means "no answer seen yet", which
	 * counts as a change so the first frames of a match still introduce the address.
	 */
	FString LastConnectionAnswer;
	float LastRoleChangeTime = -1000.f;

	/**
	 * P11 — what the network chip's words depend on, read every frame (no allocation) and compared with
	 * the last composed set. Pointers are compared as identities only, never dereferenced.
	 */
	struct FNetPanelInputs
	{
		int32 NetMode = -1;
		const void* Driver = nullptr;
		const void* ServerConnection = nullptr;
		int32 Connections = -1;
		int32 Humans = -1;
		uint32 TextGeneration = 0;

		bool operator==(const FNetPanelInputs& Other) const
		{
			return NetMode == Other.NetMode && Driver == Other.Driver
				&& ServerConnection == Other.ServerConnection && Connections == Other.Connections
				&& Humans == Other.Humans && TextGeneration == Other.TextGeneration;
		}
	};
	FNetPanelInputs NetPanelInputs;
	double NetPanelComposedAt = -1000.0;

	/** The composed chip: false for an ordinary offline match (no chrome at all). */
	bool bNetPanelHasAnswer = false;
	uint8 NetPanelRole = 0;   // TraceNet::ERole
	FString NetPanelHeadline;
	FString NetPanelDetail;
	FLinearColor NetPanelAccent = FLinearColor::White;

	// ---- Practice range pad labels --------------------------------------------------------------

	/** The range's pads, re-found every PracticePadPollSeconds (empty in every real match). */
	TArray<TWeakObjectPtr<ATracePracticePad>> PracticePads;
	float LastPracticePadPollTime = -1000.f;

	// ---- Kill feed (spec v8 §6) ----------------------------------------------------------------

	/**
	 * The world's kill-feed relay. Weak and re-found on demand: on a client the actor arrives by
	 * replication some frames after the HUD exists, and it can be destroyed by a travel.
	 */
	TWeakObjectPtr<ATraceKillFeedRelay> KillFeedRelay;

	/**
	 * Last time the relay was looked for, so the search is throttled rather than per-frame.
	 *
	 * Finding it is a TActorIterator over the whole arena. That is trivial once — but a client
	 * connected to a build without the relay would otherwise pay it on every single frame forever,
	 * and this HUD's one rule about probes (see PassTargetPollInterval) is that they get an interval.
	 */
	float LastKillFeedRelayPollTime = -1000.f;

	/**
	 * Y the kill feed may start at. Reset to the top-panel line by DrawHUD every frame, and pushed
	 * down by DrawNetworkStatus() on the frames its panel actually draws (the panel is gated —
	 * see DrawNetworkStatus). The panel's height depends on its text (an address, a human count),
	 * so the feed cannot hardcode a clearance; on panel-less frames the feed rides at the default.
	 */
	float KillFeedTopY = 0.f;

	// ---- Health bar easing (spec v13 §1) -------------------------------------------------------
	//
	// Regeneration arrives on a client as a replicated float updated at the actor's net rate, not
	// per frame — so the raw value STEPS, and a health bar that jumps in visible increments reads as
	// a networking fault rather than as healing. These smooth the DRAWN width without ever changing
	// the number printed on the bar, which stays the authoritative one.

	/**
	 * The health fraction currently drawn, eased toward the real one.
	 *
	 * *** IT EASES UP AND SNAPS DOWN, AND THE ASYMMETRY IS THE WHOLE DESIGN. *** Healing is slow and
	 * gains nothing from being shown instantly, so it is smoothed. Damage is the single most urgent
	 * thing this HUD reports and must never lag by even a frame — a bar that glides down after a
	 * body shot would be a bar that tells the player they are safer than they are. Negative means
	 * "no value yet"; the next draw snaps.
	 */
	float DrawnHealthFraction = -1.f;

	/** Whose health DrawnHealthFraction is easing. A different pawn (a respawn) snaps instead of glides. */
	TWeakObjectPtr<ATraceCharacter> DrawnHealthPawn;

	/** Time of the previous health draw, so the ease is per-second rather than per-frame. */
	float LastHealthDrawTime = -1.f;

	/** Client-local time the last capture was observed, and who scored it. */
	float ScoreFlashTime = -1000.f;
	ETraceTeam ScoreFlashTeam = ETraceTeam::None;

	/**
	 * Client-local time play last KICKED OFF, so "GO" can be shown for a moment: the match going
	 * InProgress, and — this was missing — the half-time break ending. TraceMatchState stays
	 * InProgress through the break, so the second half used to start with no cue at all.
	 */
	float MatchStartTime = -1000.f;
	ETraceMatchState LastSeenMatchState = ETraceMatchState::WaitingForPlayers;
	bool bMatchStateCacheValid = false;

	/** The half-time break as of the last draw, for the falling edge above. */
	bool bWasHalfTimeBreak = false;

	/** The last kickoff was the end of a half-time break: GO says SIDES SWITCHED under it. */
	bool bKickoffAfterBreak = false;

	// ---- The select screens and the HOSTING chip ----------------------------------------------

	/** Team select / character select / loadout up on the last draw (NOT the pause menu). */
	bool bSelectOverlayWasUp = false;

	/** When those last came up. The HOSTING chip's reveal window is re-armed if they ate it. */
	float SelectOverlayOpenedAt = -1000.f;

	/** The HOSTING chip's drawn alpha: it fades out instead of vanishing and yanking the kill feed up. */
	float NetPanelAlpha = 0.f;

	/** The kill feed's drawn top, eased toward KillFeedTopY so it glides when the chip above it goes. */
	float DrawnKillFeedTopY = -1.f;

	// ---- The bottom-left stack's plate ---------------------------------------------------------

	/** Rightmost caption edge drawn THIS frame (DrawStackCaption), and the one the last frame drew. */
	float StackCaptionRightThisFrame = 0.f;
	float StackCaptionRightLastFrame = 0.f;

	/** The plate's drawn right edge: grows at once (words never hang off it), shrinks eased. */
	float DrawnStackPlateRight = -1.f;

	// ---- The E row's ready flash -----------------------------------------------------------------

	/**
	 * The E row flashes once on the RISING EDGE of ready, like the V row, and then holds steady. It
	 * used to pulse for as long as E was ready — most of the match — right above a V row whose own
	 * comment calls exactly that a strobe.
	 */
	float ActivatedReadyFlashTime = -1000.f;
	bool bActivatedWasCooling = false;

	// ---- The Core banner's pulse -----------------------------------------------------------------

	/** The banner's text on the last draw, and when it last changed: it pulses for 2 s after a change. */
	FString LastCoreBannerText;
	float CoreBannerChangeTime = -1000.f;

	// ---- FX/AUDIO plan §7.1 — the refusal toast's one slot -------------------------------------

	/** What the toast says. Empty means no toast has ever been raised on this HUD. */
	FText ToastText;

	/** The chip's hairline and text tint: dim ink for "not yet", danger red for "no". */
	FLinearColor ToastTint = FLinearColor::White;

	/** Client-local time ShowAbilityToast() was last called, and for how long it asked to stay up. */
	float ToastStartTime = -1000.f;
	float ToastSeconds = 0.f;

	/**
	 * Client-local time UIDeny last played for a toast. THE ONE RATE LIMITER for the whole feature.
	 *
	 * A held ability key produces a refusal every frame it is down. The chip replacing itself is
	 * free (it is the same pixels), but the sound is not — unlimited it becomes a buzz, which is how
	 * a piece of feedback turns into a thing players mute the game to escape.
	 */
	float LastToastSoundTime = -1000.f;

	// ---- FX/AUDIO plan §7.2 — the V row's ready flash ------------------------------------------

	/**
	 * Client-local time the V cooldown was last observed crossing from cooling to ready, so the row
	 * can flash once. A RISING EDGE, not a state: the row is greyed for thirty-five seconds and the
	 * one moment worth an animation is the instant it stops being.
	 */
	float SecondaryReadyFlashTime = -1000.f;

	/** Whether the V cooldown was cooling on the previous drawn frame. Feeds the edge above. */
	bool bSecondaryWasCooling = false;

	// ---- FX/AUDIO plan §7.4 — the shield-blocked marker's sound --------------------------------

	/**
	 * The hit-marker timestamp the ShieldBlock sound was last played for.
	 *
	 * The marker is drawn from a TIME plus a flag, so "a new blocked hit arrived" is that timestamp
	 * changing — there is no event to bind to. Stored rather than recomputed because DrawHitMarker
	 * runs every frame for the whole 0.25 s the marker is up, and a sound per frame is a buzz.
	 */
	float LastShieldBlockSoundMarkerTime = -1000.f;

	// ---- FX/AUDIO plan §5.7 — the match-end stinger --------------------------------------------

	/** True once the full-time stinger has played on this HUD. One per match, never per frame. */
	bool bMatchEndStingerPlayed = false;

	/**
	 * WHEN THE MENU BED COMES BACK UP UNDER THE RESULTS SCREEN — world REAL time, so a pause cannot
	 * strand it. Negative = the match has not ended on this HUD yet.
	 *
	 * The results screen is 14 s long (TraceMatchFlow::PostMatchDuration) and the stinger is under
	 * three of them, so bringing the bed back only at the title screen left 11-14 s in which the only
	 * audio was whatever the surviving bots happened to walk on. See DrawMatchResult.
	 */
	float MatchEndBedResumeRealTime = -1.f;

	/** True once the results-screen bed has been asked for. Pairs with the timestamp above. */
	bool bMatchEndBedResumed = false;

	/** CONTINUE has been pressed on the results screen; the travel is under way. */
	bool bResultsContinued = false;

	/**
	 * THE RESULTS SCREEN'S CONTINUE WAITS (TraceHUDResultsInput in TraceHUD.cpp). Real time
	 * (TraceMenuKit::RealSeconds) of the first frame the results screen stood fully faded in; negative
	 * until then. CONTINUE is accepted only ContinueGraceSeconds after it.
	 */
	double ResultsShownRealSeconds = -1.0;

	/**
	 * The results screen's own memory of its two CONTINUE keys (pad A, ENTER), sampled on EVERY frame
	 * of the screen from the whistle on — grace, pause menu and all — and SEEDED DOWN. A key already
	 * held when the match ended (pad A is JUMP) has to be let go and pressed again before it counts,
	 * and a press inside the grace is spent there rather than carried past it.
	 */
	bool bResultsPadConfirmWasDown = true;
	bool bResultsEnterWasDown = true;

	/** The CONTINUE legend's fade: it comes up when the grace ends, so the key is named only once it works. */
	FTraceKitFade ResultsContinueFade;

#if !UE_BUILD_SHIPPING
	// ---- Spec v16 §2 draw record ----------------------------------------------------------------
	//
	// *** WHAT WAS ACTUALLY DRAWN, NOT WHAT THE GAME STATE SAYS. *** Every field here is written by
	// the draw pass itself, at the point the pixels are emitted, and Trace.HUD.V16.Report prints
	// them back. That distinction is the whole point: a harness that asked the weapon component how
	// much ammo it had would pass with a HUD that draws nothing at all, which is exactly the failure
	// this project has been bitten by (a geometry index reporting itself healthy while returning 0).
	//
	// Cleared at the top of every DrawHUD so a stale record can never be read as a live one.

	bool  bDrewAmmoBlock = false;
	FString DrawnAmmoText;
	bool  bDrewBeeClip = false;
	bool  bDrewReloadBar = false;
	int32 DrawnMagazineTicks = 0;      // lit ticks actually emitted, so an empty strip cannot pass

	/** One entry per status chip drawn, in the order drawn (bottom-up). */
	TArray<FString> DrawnStatusChips;

	bool  bDrewChargeRing = false;
	float DrawnChargeRingAlpha = -1.f;
	int32 DrawnChargeRingSegments = 0;  // filled chords emitted; 0 with a positive alpha is a lie
	bool  bDrewChargeBar = false;       // the SUPERSEDED bottom-left row. Must be false when armed.

	// ---- FX/AUDIO plan §7 draw record -----------------------------------------------------------
	//
	// Same rule as the block above and for the same reason: every field is written where the pixels
	// are emitted, never from the state that fed the pass. Trace.HUD.FxHudShots asserts on these.

	/** §7.4 — a hit marker of EITHER kind was emitted this frame. */
	bool bDrewHitMarker = false;

	/** §7.4 — the BLOCKED marker (the "+") was what DrawHitMarker actually emitted this frame. */
	bool bDrewShieldBlockedMarker = false;

	/** §7.1 — the toast chip's text as drawn, empty on a frame with no toast up. */
	FString DrawnToastText;

	/** §7.2 — the V row's caption as drawn ("ROCKET  12.4"), empty when the row did not draw. */
	FString DrawnSecondaryRowText;

	/** §2.5/§2.6 — the owner vignettes actually emitted, e.g. "CLOAK a=0.10". */
	TArray<FString> DrawnVignettes;

	/** The handmade-kit pass's record. See FHudKitRecord. */
	FHudKitRecord HudKitRecord;
#endif
};
