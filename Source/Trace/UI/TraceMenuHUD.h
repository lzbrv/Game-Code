// Trace — title screen.
//
// Same rules as the in-game HUD: pure AHUD::DrawHUD Canvas drawing, no UMG, no widget blueprints,
// no .uasset of any kind. Nothing here touches gameplay — the menu map is empty and this class
// draws every pixel on it, which is why it can commit to a look instead of negotiating with a
// scene.
//
// The look is stroke-drawn, not typeset. The engine's built-in fonts are bitmaps and go to mush
// somewhere around 3x, so the wordmark is a tiny vector font (TraceStrokeFont in the .cpp) made of
// line segments and drawn at whatever size the viewport wants. Everything smaller — menu rows,
// hints, the difficulty blurb — uses the bitmap fonts at scales they actually hold up at.
//
// This class owns menu *state* (which row is selected, which difficulty is chosen) as well as its
// presentation. ATraceMenuPlayerController owns nothing but the key bindings and forwards them
// here; that split keeps the layout maths and the hit testing in one file instead of two copies
// that drift.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "Math/Box2D.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceSettings.h"           // ETraceScoringMode (the A/B toggle's own enum)
#include "UI/TraceMatchOptions.h"   // ETraceBotDifficulty, TraceScoring
#include "UI/TraceNetworking.h"      // FTraceTextEntry, TraceNet
#include "UI/TraceOptionsMenu.h"     // FTraceOptionsMenu

#include "TraceMenuHUD.generated.h"

class UFont;

/** Rows of the title menu, top to bottom. */
enum class ETraceMenuRow : uint8
{
	/**
	 * Starts a match AND a listen server, always (spec v5 §0).
	 *
	 * There is deliberately no separate "host" row. A listen server with zero clients is
	 * indistinguishable from standalone for the player sitting at it — same frame rate, same bots,
	 * same everything — so making every PLAY joinable costs the single-player experience nothing and
	 * removes the entire class of failure where two people each started a private match and then
	 * wondered why they could not see each other. That is exactly what happened in the Demo 5
	 * playtest.
	 */
	Play       = 0,
	/**
	 * Connect to somebody else's PLAY. Opens the address prompt (see ATraceMenuHUD::OpenJoinPrompt).
	 *
	 * Second, directly under PLAY, because those two rows are the whole question a player is here to
	 * answer — "am I hosting or joining?" — and burying JOIN under the tuning rows would keep the
	 * feature as invisible as its absence was.
	 */
	Join       = 1,
	Difficulty = 2,
	/**
	 * SCORING MODE — the A/B toggle (spec v4 §7). Directly under DIFFICULTY, and above SETTINGS,
	 * because it is the second thing a playtester picks and the entire point of this build: the
	 * notes ask for the two rulesets to be compared back to back, which means switching between them
	 * has to be one keypress on the way into a match rather than a trip through a settings panel.
	 */
	Mode       = 3,
	/**
	 * Sensitivity, invert-Y and the key bindings. Sits above QUIT rather than below the blurb so
	 * that the things a player might change before their first match are adjacent, and the one
	 * destructive row stays last.
	 */
	Settings   = 4,
	Quit       = 5,

	Count      = 6
};

UCLASS()
class TRACE_API ATraceMenuHUD : public AHUD
{
	GENERATED_BODY()

public:
	//~ Begin AHUD interface
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	//~ End AHUD interface

	// ---- Input entry points, called by ATraceMenuPlayerController --------------------------------

	/** Up / down. Clamped rather than wrapped: three rows wrap badly and clamping reads as solid. */
	void MoveSelection(int32 Delta);

	/** Left / right. Only the DIFFICULTY and SCORING MODE rows respond. */
	void AdjustSelection(int32 Delta);

	/** Enter / Space / left click on the selected row. */
	void ActivateSelection();

	/** Escape. Quits from the title screen — there is nothing above it to back out to. */
	void CancelPressed();

	/**
	 * Left mouse button DOWN. Selects whatever is under the cursor and arms it — it does not
	 * activate. See PressedRow and bCursorHasMoved.
	 */
	void MousePressed();

	/** Left mouse button UP. Activates only if the release lands on the row the press armed. */
	void MouseReleased();

	ETraceBotDifficulty GetDifficulty() const { return Difficulty; }

	/** The ruleset PLAY will launch. See ETraceScoringMode. */
	ETraceScoringMode GetScoringMode() const { return ScoringMode; }

	/**
	 * True while the settings overlay owns the screen.
	 *
	 * ATraceMenuPlayerController's key bindings still fire while it is up — they are bound to the
	 * controller, not to the menu — so every input entry point above checks this and returns. The
	 * overlay polls its own input instead; see FTraceOptionsMenu for why.
	 */
	bool IsOptionsOpen() const { return OptionsMenu.IsOpen(); }

	/**
	 * True while the JOIN address prompt owns the screen.
	 *
	 * Same contract as IsOptionsOpen(), and for a sharper reason: the title screen binds A, S, D and
	 * W as navigation keys, and those are letters somebody typing "aya.tail1234.ts.net" needs. Every
	 * entry point above returns early while this is true, and FTraceTextEntry polls the keyboard
	 * itself.
	 */
	bool IsJoinPromptOpen() const { return JoinEntry.IsActive(); }

protected:
	// ---- Draw passes, back to front --------------------------------------------------------------
	void DrawBackdrop();
	void DrawGridFloor();

	/** Inset frame with corner ticks. Gives the screen an edge so it reads as an instrument. */
	void DrawBezel();

	void DrawWordmark();

	/**
	 * "YOUR ADDRESS — 100.101.102.103:7777", directly under the tagline.
	 *
	 * Treated as a headline element rather than a footnote, because it removes the single most
	 * common multiplayer failure this project has actually seen: not knowing what to type. The host
	 * reads it off their own title screen and says it out loud; nobody has to run `tailscale ip -4`
	 * in a terminal, and nobody has to find the networking doc first.
	 */
	void DrawAddressChip();

	/** Red strip across the top when a connection or travel attempt failed. See TraceNet. */
	void DrawFailureBanner();

	void DrawMenuRows();
	void DrawFooter();
	void DrawCursor();
	void DrawTravelOverlay();

	/** The modal address field raised by the JOIN row. Draws nothing while the field is inactive. */
	void DrawJoinPrompt();

	/** One menu row. Returns the row's screen rect so the caller can store it for hit testing. */
	FBox2D DrawRow(ETraceMenuRow Row, float CenterX, float Y, float Width, bool bSelected);

	// ---- Actions ---------------------------------------------------------------------------------

	/**
	 * Travels to the arena as a LISTEN SERVER — the fix for spec v5 §0.
	 *
	 * The URL gains a bare `listen` option alongside the difficulty and mode. That one word is the
	 * entire difference between the shipped build and a joinable one: without it
	 * UEngine::LoadMap never calls UWorld::Listen, no net driver is created, nothing binds UDP 7777,
	 * and the match is unreachable no matter how correct the other machine's setup is.
	 */
	void StartMatch();

	/** Raises the address prompt, pre-filled with the last address that was joined. */
	void OpenJoinPrompt();

	/**
	 * Validates whatever is in the field, remembers it, and calls ClientTravel.
	 *
	 * TRAVEL_Absolute, because a join is not relative to the map we are on — the menu map's package
	 * name must not be prepended to an IP address.
	 */
	void ConfirmJoin();

	/** Raises the settings overlay on its settings page, with BACK closing it outright. */
	void OpenOptions();

	void QuitGame();

	// ---- Small drawing helpers -------------------------------------------------------------------
	void DrawTextCentered(const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale);
	float MeasureWidth(const FString& Text, UFont* Font, float Scale);
	float MeasureHeight(const FString& Text, UFont* Font, float Scale);

	/** Line with a wide, dim under-stroke — the cheapest convincing neon on a Canvas. */
	void DrawGlowLine(float X0, float Y0, float X1, float Y1, const FLinearColor& Color, float Thickness);

	/** Stroke-font text, centred on @p CenterX. @p Height is the cap height in pixels. */
	void DrawStrokeTextCentered(const FString& Text, const FLinearColor& Color, float CenterX, float Y, float Height, float Thickness);

	/** Width the same call to DrawStrokeTextCentered would occupy. */
	static float MeasureStrokeText(const FString& Text, float Height);

private:
	ETraceMenuRow Selected = ETraceMenuRow::Play;
	ETraceBotDifficulty Difficulty = TraceDifficulty::Default;

	/**
	 * The scoring mode PLAY will launch (spec v4 §7).
	 *
	 * Seeded from UTraceSettings in BeginPlay rather than hardcoded to mode A, so returning to the
	 * title screen after a mode-B match comes back on mode B. Somebody A/B testing plays several
	 * matches of one mode in a row; making them re-pick it every single time is how a toggle stops
	 * being used.
	 */
	ETraceScoringMode ScoringMode = ETraceScoringMode::EndzoneStatusCore;

	/** Screen rects of the rows as of the last draw, used for mouse hover and click hit testing. */
	FBox2D RowRects[static_cast<int32>(ETraceMenuRow::Count)];
	bool bRowRectsValid = false;

	/** Set once Play has been taken, so a second Enter during the level load cannot travel twice. */
	bool bTravelling = false;

	/** What the travel overlay should say: "ENTERING THE ARENA" or "CONNECTING TO <addr>". */
	FString TravelCaption;

	// ---- JOIN ------------------------------------------------------------------------------------

	/**
	 * The address field. Active state IS the "prompt is open" state — see IsJoinPromptOpen().
	 *
	 * Not a UPROPERTY and does not need to be: FTraceTextEntry is plain C++ holding an FString and
	 * an int, exactly like FTraceOptionsMenu above it.
	 */
	FTraceTextEntry JoinEntry;

	/**
	 * The address shown on the JOIN row and pre-filled into the field.
	 *
	 * Loaded from disk in BeginPlay (TraceNet::LoadLastJoinAddress) and written back on every
	 * successful join. The spec is explicit about why: nobody should retype a Tailscale address
	 * every test, and a prototype that makes them do it is a prototype that stops being tested.
	 */
	FString LastJoinAddress;

	/** Set when the typed address is unusable. Drawn under the field. */
	FString JoinError;

	/** The field contents JoinError was raised about; any edit away from it clears the message. */
	FString JoinErrorText;

	/**
	 * World time at which the title screen started accepting Enter/Space.
	 *
	 * HISTORY, because this used to be the whole fix and is now a backstop.
	 * The menu was observed activating PLAY on its own within a couple of seconds of the map
	 * loading, intermittently, with no AutoPlay timer armed — the symptom the player described as
	 * the game "restarting without anything changing". A 0.75s grace period was added as mitigation
	 * without identifying the source.
	 *
	 * The source is now identified, reproduced (5 self-activations in 8 launches, then 2 in 10 after
	 * a partial fix) and fixed at the cause. It was NOT a window-focus click, which is what the first
	 * two attempts assumed: it is the viewport leaving EMouseCaptureMode::
	 * CapturePermanently_IncludingInitialMouseDown when the menu installs its own input mode, which
	 * replays the "initial mouse down" that mode is named for. See ATraceMenuPlayerController::
	 * BeginPlay, which now puts the menu viewport in NoCapture so there is no capture to acquire and
	 * nothing to replay.
	 *
	 * Two further defences remain, both independently correct: activation requires a press AND a
	 * release on the same row (see PressedRow), and a click is ignored until the cursor has actually
	 * moved (see bCursorHasMoved).
	 *
	 * The deadline stays as a shortened backstop, and it is the ONLY defence left on the keyboard
	 * path: a stray Enter can still arrive from the terminal that launched the game, and a key press
	 * has no focus edge to test against the way a click does. Navigation (arrows, hover) is
	 * deliberately never gated — moving the selection is harmless and gating it would feel like lag.
	 */
	float AcceptUnlockTime = 0.f;

	/** True once the keyboard grace period above has elapsed. */
	bool AcceptsActivation() const;

	/**
	 * The row the current mouse-down armed, or INDEX_NONE when no press is outstanding.
	 *
	 * Activation happens on release, and only if the release is still inside this row. That is
	 * ordinary button behaviour: it makes a stray down harmless by itself, and it lets a player who
	 * has second thoughts slide off the row before letting go.
	 */
	int32 PressedRow = INDEX_NONE;

	/**
	 * Whether the game window was foreground as of the previous frame, sampled in DrawHUD.
	 *
	 * A mouse-down that arrives when this is false IS the click that brought the window forward, so
	 * it is dropped rather than armed. This is the specific, testable form of the bug the grace
	 * period was papering over.
	 */
	bool bWindowFocusedLastFrame = false;

	/** Refreshes bWindowFocusedLastFrame from the game viewport. Called once per DrawHUD. */
	void UpdateWindowFocus();

	/** Row under the given viewport-space point, or INDEX_NONE. Requires bRowRectsValid. */
	int32 RowAtPoint(const FVector2D& Point) const;

	/** Cursor position in viewport space, if the controller has one. */
	bool GetCursorPoint(FVector2D& OutPoint) const;

	/**
	 * Last cursor position, and whether we have ever seen one.
	 *
	 * Hover only re-selects a row when the cursor has MOVED. A cursor parked over QUIT would
	 * otherwise re-assert itself every frame and make the arrow keys look broken.
	 */
	FVector2D LastCursorPos = FVector2D::ZeroVector;
	bool bHasCursor = false;

	/** Where the cursor was the first time we ever saw it, for the movement test below. */
	FVector2D FirstCursorPos = FVector2D::ZeroVector;

	/** World time the title screen came up. Both the grace period and the settling window use it. */
	float TitleShownTime = 0.f;

	/**
	 * Whether the cursor has moved since the title screen appeared. A click is ignored until it has.
	 *
	 * Defence in depth behind the capture-mode fix in ATraceMenuPlayerController::BeginPlay.
	 *
	 * What the logs actually showed: 5 title screens out of 8 took PLAY on their own, and each one
	 * was a complete, well-formed mouse-down/mouse-up pair ~58ms apart, at a stable cursor position,
	 * 1.6 to 7 seconds in, while the viewport already reported itself foreground, always within
	 * ~150px of screen centre. It is indistinguishable from a real click by every property except
	 * one — the cursor never moved.
	 *
	 * A player always moves the mouse onto a button before pressing it. A click delivered at whatever
	 * coordinates the pointer happened to be sitting at when the window came up, with no motion
	 * before it, is not a player. That is the discriminator, and it is the same one the hover code
	 * above already relies on for the same underlying reason.
	 *
	 * The keyboard is deliberately unaffected: someone who genuinely never touches the mouse can
	 * still drive the whole menu with the arrows and Enter.
	 */
	bool bCursorHasMoved = false;

	/**
	 * Bottom of the tagline as of the last DrawWordmark(), so DrawAddressChip() can sit under it
	 * without re-deriving the wordmark's cap height, rule offset and font metrics. Two copies of
	 * that arithmetic is two things to keep in step.
	 */
	float TaglineBottomY = 0.f;

	/** Viewport size and the layout scale every constant in the .cpp is authored against. */
	float ViewW = 0.f;
	float ViewH = 0.f;
	float UIScale = 1.f;
	float Now = 0.f;

	UPROPERTY(Transient) TObjectPtr<UFont> FontSmall;
	UPROPERTY(Transient) TObjectPtr<UFont> FontMedium;
	UPROPERTY(Transient) TObjectPtr<UFont> FontLarge;

	/**
	 * The settings overlay, shared verbatim with the in-game pause menu.
	 *
	 * Not a UPROPERTY, and does not need to be: FTraceOptionsMenu is a plain C++ class that holds no
	 * UObject reference across a frame — the AHUD and the APlayerController it draws and polls
	 * through are both passed in per call.
	 */
	FTraceOptionsMenu OptionsMenu;

#if !UE_BUILD_SHIPPING
	/**
	 * -TraceAutoPlay=<seconds> presses Play for you. Purely a verification aid: it is the only way
	 * to prove menu -> match -> results -> menu end to end from a single headless launch.
	 */
	void ArmAutoPlay();
	FTimerHandle AutoPlayTimer;

	/**
	 * -TraceAutoSettings=<seconds> opens the settings overlay and then DRIVES it with a scripted
	 * sequence of synthetic key presses: raise the sensitivity slider, toggle invert-Y, and rebind
	 * MOVE FORWARD.
	 *
	 * This exists because the settings screen is the one feature in this pass with no observable
	 * gameplay side effect — nothing about it shows up in a log line or a match statistic, and the
	 * whole thing could be dead on arrival while every automated check stayed green. The keys go in
	 * through APlayerController::InputKey, which is the same entry point a physical keyboard uses, so
	 * a run that ends with a TraceUserSettings.ini containing the changed values has proved the
	 * entire chain: poll -> row -> setting -> save -> disk.
	 *
	 * Press and release are separate steps on purpose: FTraceOptionsMenu reads the arrow keys with
	 * IsInputKeyDown (it has held-key repeat), so a press-and-release inside one frame would be
	 * invisible to it.
	 */
	void ArmAutoSettings();
	void AutoSettingsStep();
	FTimerHandle AutoSettingsTimer;
	FTimerHandle AutoSettingsStepTimer;
	int32 AutoSettingsIndex = 0;
	bool bAutoSettingsAwaitingRelease = false;

	/**
	 * -TraceAutoJoin=<seconds> -TraceJoinAddress=<ip:port> drives the JOIN row for you.
	 *
	 * The counterpart of -TraceAutoPlay, and it exists for the same reason: the acceptance test for
	 * this whole pass is TWO processes sharing one match, and a test that reached the second process
	 * by passing the address on the command line would prove the engine works while proving nothing
	 * at all about the menu — which is where the bug was. This drives the real row: it opens the real
	 * prompt, fills the real field and submits it through the real ConfirmJoin().
	 */
	void ArmAutoJoin();
	FTimerHandle AutoJoinTimer;

	/** Submits 2.5s after the prompt opens, so -TraceAutoShot has a window to photograph it. */
	FTimerHandle AutoJoinSubmitTimer;
#endif
};
