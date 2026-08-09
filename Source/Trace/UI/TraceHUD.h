// Trace — Canvas HUD.
//
// Pure AHUD::DrawHUD Canvas drawing: no UMG, no widget blueprints, no .uasset of any kind
// (contract §2). Text uses the engine's built-in fonts via GEngine->Get*Font().
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
#include "UI/TraceKillFeed.h"     // ETraceKillIcon, ATraceKillFeedRelay — spec v8 §6
#include "UI/TraceOptionsMenu.h"  // FTraceOptionsMenu

#include "TraceHUD.generated.h"

class ATraceCharacter;
class ATraceGameState;
class ATracePlayerController;
class ATracePlayerState;
class UFont;

UCLASS()
class TRACE_API ATraceHUD : public AHUD
{
	GENERATED_BODY()

public:
	//~ Begin AHUD interface
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	//~ End AHUD interface

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
	 */
	void DrawPassProgress();

	void DrawHitMarker();

	/**
	 * Health, the EQUIPPED WEAPON, dash CHARGES and the parry cooldown: the bottom-left ability
	 * stack. (Boost is gone.)
	 *
	 * Row order is load-bearing, not cosmetic. The stack grows UPWARDS from the health bar, so a row
	 * drawn earlier sits lower and closer to health. Health is drawn last and never moves; the
	 * weapon row is drawn first because it is the only other row that is ALWAYS present, which makes
	 * it the only other row that can be found by muscle memory. Everything above it is conditional
	 * and is allowed to shuffle.
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

	void DrawScoresAndClock();
	void DrawCoreBanner();

	/**
	 * Top-right: what this process is on the network, and the address other people dial to reach it.
	 *
	 * "HOSTING — 100.101.102.103:7777" is a headline feature of spec v5 §0, not chrome. The reported
	 * bug was two machines each running a private match and nobody able to tell whether their setup
	 * worked; a host who can read their own address off their own screen, mid-match, while somebody
	 * asks them for it over voice, is the affordance that makes the rest of this usable.
	 *
	 * It reads the world's ACTUAL net mode, never what the menu intended — so a listen server whose
	 * bind failed says OFFLINE rather than repeating a promise the process did not keep.
	 */
	void DrawNetworkStatus();

	/** The last connection or travel failure, if recent. Same store the title screen draws from. */
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
	 * panel that already owns the top-right corner (that panel publishes KillFeedTopY).
	 */
	void DrawKillFeed();

	/**
	 * One kill-feed glyph, drawn from a 13x13 bitmap at @p Cell pixels per cell, top-left at @p X,@p Y.
	 *
	 * NO IMPORTED ART AND NO UMG (contract §2 / spec v8 §6): every icon is an ASCII bitmap in the
	 * .cpp, emitted as run-length integer DrawRects with a one-pixel dark surround. Integer cells
	 * for the same reason the crosshair uses them — a fractional-width rect on a half-pixel boundary
	 * is anti-aliased into a smudge, and these are 26 px tall shapes that have to read at a glance.
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

	/** Draws one team's column of the scoreboard. Returns the height consumed, in pixels. */
	float DrawScoreboardTeam(ETraceTeam Team, float X, float Y, float Width);

	// ---- Small drawing helpers ----------------------------------------------------------------
	void DrawTextLeft(const FString& Text, const FLinearColor& Color, float X, float Y, UFont* Font, float Scale);
	void DrawTextCentered(const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale);
	void DrawTextRight(const FString& Text, const FLinearColor& Color, float RightX, float Y, UFont* Font, float Scale);

	float MeasureWidth(const FString& Text, UFont* Font, float Scale);
	float MeasureHeight(const FString& Text, UFont* Font, float Scale);

	/** Top-left Y that vertically centres @p Text inside the box [BoxY, BoxY + BoxH]. */
	float VCenterTextY(const FString& Text, UFont* Font, float Scale, float BoxY, float BoxH);

	/** Row of charge pips: @p Charges lit, the next one part-filled by @p PartialFraction. */
	void DrawChargePips(float X, float Y, float W, float H, int32 Charges, int32 MaxCharges,
		float PartialFraction, const FLinearColor& FillColor);

	/** Filled rect plus a thin border, used as the background of every panel. */
	void DrawPanel(float X, float Y, float W, float H, const FLinearColor& Fill, const FLinearColor& Border);

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

	/**
	 * MODE B this frame (spec v4 §7): goals instead of endzones, and a Core that is thrown and
	 * intercepted. Read from ATraceGameState once at the top of DrawHUD so every pass below branches
	 * on the same answer.
	 *
	 * Not decoration. Mode B changes what the left mouse button does while carrying — it throws
	 * instead of starting the hover-pass — so a pass that hardcodes mode A's wording is a pass that
	 * teaches the wrong control for half of the A/B playtest.
	 *
	 * A bool rather than the enum so this header does not have to take the 110 kB TraceSettings.h
	 * for one comparison; passes that need to PRINT the mode ask TraceGS->GetScoringMode().
	 */
	bool bGoalMode = false;

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

	/** Binds the select screen's input-suppression callbacks. Idempotent; called from BeginPlay. */
	void WireCharacterSelect();

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
	 * Y the kill feed may start at, published by DrawNetworkStatus() each frame.
	 *
	 * The network panel and the feed both want the top-right corner, and the panel's height depends
	 * on its text (an address, a human count) — so the feed cannot hardcode a clearance. Set every
	 * frame before the feed draws; defaults to the top margin when the panel drew nothing.
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

	/** Client-local time the match went InProgress, so "GO" can be shown for a moment. */
	float MatchStartTime = -1000.f;
	ETraceMatchState LastSeenMatchState = ETraceMatchState::WaitingForPlayers;
	bool bMatchStateCacheValid = false;
};
