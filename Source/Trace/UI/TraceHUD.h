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

	/** Health, dash CHARGES and the parry cooldown: the bottom-left ability stack. (Boost is gone.) */
	void DrawHealthAndDash();

	void DrawScoresAndClock();
	void DrawCoreBanner();

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

	/** Client-local time the last capture was observed, and who scored it. */
	float ScoreFlashTime = -1000.f;
	ETraceTeam ScoreFlashTeam = ETraceTeam::None;

	/** Client-local time the match went InProgress, so "GO" can be shown for a moment. */
	float MatchStartTime = -1000.f;
	ETraceMatchState LastSeenMatchState = ETraceMatchState::WaitingForPlayers;
	bool bMatchStateCacheValid = false;
};
