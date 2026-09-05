// Trace — the character select screen (spec v14 §3).
//
// Verbatim: "Create a menu for selecting a character when a player loads into a game of goals" and
// "Do not allow players to select a character who has already been chosen by a player on their team".
//
// SHAPED EXACTLY LIKE FTraceOptionsMenu, and for the same three reasons that class spells out: it is
// plain C++ rather than a UObject (it holds nothing that outlives a frame and never replicates), it
// is drawn entirely through AHUD::DrawRect / DrawText / DrawLine, and it POLLS input rather than
// binding it. The polling argument is the strongest one here: the screen is up during the warm-up,
// when the local pawn may not exist yet and the gameplay input component may not be wired, and a
// bound delegate on a pawn that has not spawned is a key press that goes nowhere.
//
// STILL ON CANVAS AS OF SPEC v17 §4, AND THAT IS A REPORTED DECISION, NOT AN OVERSIGHT.
// The middle sentence above used to end "(contract §2 — no UMG, no .uasset)". THAT CONTRACT IS
// RETIRED: the project commits .uassets now and the TITLE SCREEN is a real UMG widget behind
// `Trace.UI.UseUMG` (see UI/Widgets/Menu/TraceTitleMenuWidget.h). This screen was not converted in
// that pass because §4 orders the work title menu -> options -> character select and says plainly
// that a half-converted screen is worse than an honest "not yet". What a conversion here has to
// carry, and what makes it a pass of its own: per-team uniqueness is a SERVER verdict this screen
// only reports, the bot-fill ordering of spec v15 §2, the pending-request timeout, and
// `Trace.Characters.ClickTest`'s one-press-per-card measurement. None of that is layout.
// Nothing here changed in that pass; it draws and behaves exactly as it did.
//
// ---------------------------------------------------------------------------------------------
// WHAT THIS CLASS IS NOT ALLOWED TO DECIDE
// ---------------------------------------------------------------------------------------------
// It does not decide whether a character is available. It cannot: two clients cannot see each other,
// and the whole point of per-team uniqueness is a rule about two players at once. It greys out what
// it BELIEVES is taken, purely so the common case needs no round trip — and then sends the request
// anyway and prints whatever the server says. ATraceGameMode::RequestCharacter is the only authority,
// and the "TAKEN - PICK AGAIN" line this screen shows is a server verdict being reported, never a
// local decision being made.
//
// A BOT TEAM-MATE COUNTS. Spec v15 §2 gives bots characters, so the greying above consults them
// exactly as it consults a human — the card says "TAKEN BY BOT BLUE 3" and means it. Under §2's
// ordering the bots on your team hold nothing while your screen is open, so on a fresh match every
// card is free; the case this is for is joining a match already in progress, where the bots filled
// long ago and their picks are the only reason a card would be grey.
//
// It does not decide whether the screen is open either. ATracePlayerState::bCharacterSelectOpen is
// replicated from the server and is the only condition. That is what makes "mode A shows no select
// screen at all" true by construction rather than by a client-side mode test that could be wrong for
// the first few frames after a travel.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtr.h"

#include "Core/TraceCharacterRoster.h"
#include "UI/TraceTeamSelect.h"   // D31-TEAMS — the team screen this one hosts; see below

class AHUD;
class APlayerController;
class ATracePlayerState;
class UFont;
class ATracePlayerController;

/**
 * The select overlay.
 *
 * Lifecycle is driven ENTIRELY by Tick(): the host calls it once per frame from DrawHUD and hands it
 * the local player state, and this class opens, draws, closes and fires its callbacks off the
 * replicated flag. There is no Open() for the host to remember to call, because the moment the host
 * has to remember something is the moment a player joins mid-warm-up and never gets a screen.
 */
class TRACE_API FTraceCharacterSelect
{
public:
	/**
	 * Called when the overlay starts being drawn and when it stops.
	 *
	 * The host uses these to silence gameplay input and hand the mouse back — the same contract
	 * FTraceOptionsMenu::OnClosed has, and for the same reason: doing it from each exit path
	 * separately is how a menu ends up leaving the game permanently unable to move.
	 */
	TFunction<void()> OnOpened;
	TFunction<void()> OnClosed;

	/**
	 * True while EITHER screen in this flow is up — the character select, or the team select this
	 * class hosts (D31-TEAMS).
	 *
	 * DELIBERATELY ONE ANSWER FOR BOTH, and that is the whole reason the team screen is hosted here
	 * rather than registered separately with ATraceHUD. This predicate is what hides the UMG ammo
	 * corner (ATraceHUD::DrawHUD's bOverlayOwnsScreen) and what stops the pause menu handing movement
	 * back to a player who is still behind a modal. A second overlay with its own IsOpen() would have
	 * needed both of those call sites to learn about it, and either one missed is a player left
	 * unable to move or an ammo counter drawn over a full-screen menu.
	 */
	bool IsOpen() const { return bOpen || TeamSelect.IsOpen(); }

	/**
	 * Poll input and draw. Call exactly once per frame from the owning AHUD::DrawHUD.
	 *
	 * @param HUD           the drawing surface
	 * @param PC            the controller whose key state is polled; may be null (draw-only frame)
	 * @param LocalState    the LOCAL player's state. Null closes the overlay — a player with no state
	 *                      has no team, so there is nothing this screen could correctly show.
	 * @param bInputAllowed false while something in front of this owns the keyboard (the pause menu).
	 *                      The screen still DRAWS: a select screen that vanished behind the pause menu
	 *                      would make the player think their pick was cancelled.
	 */
	void Tick(AHUD* HUD, APlayerController* PC, ATracePlayerState* LocalState,
		float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed);

#if !UE_BUILD_SHIPPING
	/**
	 * Trace.Characters.Select <n> — drives the highlight and the pick exactly as the keyboard would.
	 * @param CharacterId  a character ID (FirstId..LastId, i.e. 1..8 since spec v18 §2), NOT a card index.
	 *
	 * Public only so the console command can reach it, and it goes through the SAME request path a
	 * key press does rather than calling the player state directly: what a headless verification of
	 * this screen must prove is that THIS FILE's request path works, and a test that bypassed it
	 * would pass with every card on the screen disconnected.
	 */
	void DebugPick(int32 CharacterId);
#endif

private:
	// ---- Input ---------------------------------------------------------------------------------

	void PollInput(APlayerController* PC, ATracePlayerState* LocalState);

	/** Moves the highlight by @p Delta, clamped to the roster. Skips nothing — taken cards are shown. */
	void MoveHighlight(int32 Delta);

	/** Sends the highlighted card to the server. No-op when the card is one we believe is taken. */
	void ConfirmHighlighted(ATracePlayerState* LocalState);

	// ---- Belief about the roster ----------------------------------------------------------------

	/**
	 * The TEAM-MATE currently believed to hold @p CharacterId, or null.
	 *
	 * "Believed" is doing real work in that sentence: this walks the replicated PlayerArray, which on
	 * a client is a snapshot that is one round trip old. It is right often enough to grey a card and
	 * never right enough to refuse a request — see the file header.
	 */
	const ATracePlayerState* FindTeammateHolding(const ATracePlayerState* LocalState, uint8 CharacterId) const;

	// ---- Draw -----------------------------------------------------------------------------------

	void Draw(AHUD* HUD, ATracePlayerState* LocalState);
	void DrawCard(AHUD* HUD, ATracePlayerState* LocalState, int32 CardIndex, float X, float Y, float W, float H);
	void DrawFrame(AHUD* HUD, float X, float Y, float W, float H, const FLinearColor& Color);
	void DrawCursor(AHUD* HUD);

	/**
	 * Word-wraps @p Text into @p MaxWidth and draws it from @p Y down. Returns the Y below the block.
	 *
	 * The ability descriptions are whole sentences (spec §6's own wording) and the cards are a fifth
	 * of a 1280-pixel screen wide, so wrapping is not a nicety — without it the passive text runs off
	 * the card and over its neighbour. AHUD has no wrapping draw, hence this.
	 */
	float DrawWrapped(AHUD* HUD, const FString& Text, const FLinearColor& Color,
		float X, float Y, float MaxWidth, UFont* Font, float Scale, float LineGap);

	float MeasureWidth(AHUD* HUD, const FString& Text, UFont* Font, float Scale) const;
	float MeasureHeight(AHUD* HUD, const FString& Text, UFont* Font, float Scale) const;
	void DrawTextCentered(AHUD* HUD, const FString& Text, const FLinearColor& Color, float CenterX, float Y, UFont* Font, float Scale);

	// ---- State ----------------------------------------------------------------------------------

	bool bOpen = false;

	// ---- D31-TEAMS ------------------------------------------------------------------------------

	/**
	 * The team-select screen. Ticked from this class's Tick, drawn under this one, and reported
	 * through IsOpen() above. See TraceTeamSelect.h for why it lives here.
	 */
	FTraceTeamSelect TeamSelect;

	/**
	 * bOpen || TeamSelect.IsOpen() as of the last frame.
	 *
	 * OnOpened / OnClosed fire off THIS rather than off bOpen, because what the host does with them —
	 * silence gameplay input, hand the mouse back — is about "is a modal up", not about which one.
	 * Firing them per screen would restore movement for one frame in the gap between the team screen
	 * closing and the character screen opening, which is a frame in which a held W walks the player
	 * out of the arena behind a menu.
	 */
	bool bOverlayOpen = false;

	/**
	 * 0..Count-1, an index into TraceCharacterRoster::All(). Never an id; convert at the point of use.
	 *
	 * It is a LINEAR index even though spec v18 §2 draws the cards as a grid: the grid is a drawing
	 * decision (see TraceSelectGrid in the .cpp) and left/right still walk the roster in reading
	 * order, so "press right four times" and "press 5" agree about where they land.
	 */
	int32 Highlighted = 0;

	/** The card the pointer is over, or INDEX_NONE. Recomputed every frame from the drawn rects. */
	int32 HoveredCard = INDEX_NONE;

	/**
	 * Screen rects of every card as of the last draw. Hit testing and hover use them.
	 *
	 * Sized by the roster, so adding a character grows it — the ONE thing that must not happen here is
	 * a fixed 5 that a ninth card writes past. Cleared to FBox2D(ForceInit) every time the screen
	 * opens, because FBox2D leaves bIsValid uninitialised and PollInput runs before the first Draw.
	 */
	FBox2D CardRects[TraceCharacterRoster::Count];

	/** The request this screen is waiting on, or NoneId. Stops a held key sending ten a second. */
	uint8 PendingRequest = TraceCharacterRoster::NoneId;

	/** Local time the pending request was sent, so a lost packet does not deadlock the screen. */
	float PendingRequestTime = -1000.f;

	/** How long a request may stay pending before the screen lets the player try again. */
	static constexpr float PendingRequestTimeout = 2.f;

	/** Frame before which no input is read; the key that opened something must not also press a card. */
	uint64 IgnoreInputBeforeFrame = 0;

	/** One layout line per process, so a capture can be told apart from a stale binary. See Draw(). */
	bool bLoggedLayoutOnce = false;

	// ---- Cached per-frame view metrics, set at the top of Tick ---------------------------------

	float ViewW = 0.f;
	float ViewH = 0.f;
	float UIScale = 1.f;
	float Now = 0.f;

	UFont* FontSmall = nullptr;
	UFont* FontMedium = nullptr;
	UFont* FontLarge = nullptr;

	// ---- Mouse ---------------------------------------------------------------------------------

	FVector2D CursorPos = FVector2D::ZeroVector;
	bool bHasCursor = false;
	bool bMouseWasDown = false;

	// ---- Held-key repeat for the left/right walk ------------------------------------------------

	int32 LastNavDir = 0;
	float NextNavTime = 0.f;
	static constexpr float NavRepeatDelay = 0.35f;
	static constexpr float NavRepeatInterval = 0.12f;

#if !UE_BUILD_SHIPPING
	// ---- Trace.Characters.ClickTest — spec v15 §4's measurement for THIS screen -------------------
	//
	// §4 is "one press = one action" everywhere a menu takes a click, and this screen is one of the
	// four named surfaces. It cannot be measured the way the title screen is: the click harness on
	// ATraceMenuHUD lives on the menu map and this screen only exists inside a match. So it gets its
	// own, in the same shape — park a real cursor on a real card, deliver complete LMB down/up pairs
	// through the real input path, and count how many the card needed.
	//
	// Deliberately tiny and deliberately here rather than in the HUD: ATraceHUD belongs to another
	// slice, and the whole point is to exercise THIS class's PollInput.

	/** 0..Count-1 while a click test is running, INDEX_NONE otherwise. */
	int32 ClickTestCard = INDEX_NONE;

	/** 0 = park the cursor, 1 = press, 2 = release, 3 = judge. Same reason for the split as the menu's. */
	int32 ClickTestStage = 0;

	/** Frame before which the current stage does not advance; several frames per stage. */
	uint64 ClickTestNextFrame = 0;

	/** Complete down/up pairs delivered so far in this test. */
	int32 ClickTestClicks = 0;

	/** Runs one stage. Called from Tick while the screen is open and a test is armed. */
	void ClickTestStep(APlayerController* PC, ATracePlayerState* LocalState);

	/**
	 * Prints the verdict and disarms. Called from the judging stage AND from the screen-closed path,
	 * because a click that works closes the screen and Tick then stops driving the test at all.
	 */
	void ReportClickTest(ATracePlayerState* LocalState);
#endif
};

#if !UE_BUILD_SHIPPING
/**
 * Set by the Trace.Characters.Select console command, consumed by the next Tick of whichever screen
 * is open. A file-scope hook rather than a pointer to the instance for the reason FTraceOptionsMenu's
 * GActiveOptionsMenu comment gives from the other direction: this class is a plain member of an AHUD
 * that a travel destroys, and a raw pointer to it left in a console command is a dangling pointer
 * waiting for a map change. An int is safe to leave behind.
 */
extern TRACE_API int32 GTraceCharacterSelectDebugPick;

/**
 * Set by Trace.Characters.ClickTest, consumed by the next Tick of whichever screen is open. 1..5,
 * or 0 for idle. Same file-scope-int reasoning as GTraceCharacterSelectDebugPick above.
 */
extern TRACE_API int32 GTraceCharacterSelectClickTest;
#endif
