// Trace — the team select screen (D31-TEAMS (a)).
//
// Verbatim: "Add an option to switch teams while in a hosted match. Players can hit H to pull up a
// team select menu. Players should load into team select before character select."
//
// D32-PADMENU: A CONTROLLER DRIVES THIS SCREEN TOO. D-pad or left stick moves the highlight, A
// confirms, B closes (the pad's H), X changes loadout (the pad's C) while the loadout may change.
// The buttons come from TracePadMenu in Settings/TraceGamepadInput.h so that this screen, the
// character screen, the title screen and the options overlay cannot disagree about what A means;
// the REPEAT CLOCK is the menus' shared one (TracePadMenu::RepeatDelay / RepeatInterval), shared
// with the arrow keys, so a thumb and a finger scroll at the same speed here and on the next page.
// A pad opens this screen mid-match through the pause menu's TEAM row, not a button — see PollOpenHotkey.
//
// SHAPED EXACTLY LIKE FTraceCharacterSelect, and read that file's header first — every argument it
// makes applies here unchanged. It is plain C++ rather than a UObject (it holds nothing that
// outlives a frame and never replicates), it draws through the handmade kit (TraceMenuKit), and
// it POLLS input rather than binding it. The polling argument is if anything stronger here: this
// screen is up during the warm-up before any pawn exists, AND it has to answer a key (H) that is
// pressed while the screen is CLOSED and the gameplay input component owns the keyboard.
//
// ---------------------------------------------------------------------------------------------
// WHO OWNS WHAT
// ---------------------------------------------------------------------------------------------
// THIS CLASS DECIDES NOTHING. It does not decide whether the screen is open — that is
// ATracePlayerController::bTeamSelectOpen, replicated from the server, and it is the only condition.
// It does not decide whether a switch is legal either: it asks ATraceGameMode::IsTeamSwitchAllowed,
// which is a static, pure function of the replicated roster precisely so that the SCREEN and the
// SERVER cannot disagree about the rule. What it computes from that is a BELIEF — PlayerArray on a
// client is one round trip old — so it greys the row and prints the reason, and then sends the
// request anyway and prints whatever the server actually said. ATraceGameMode::RequestTeamChange is
// the only authority.
//
// That is the same split the character-select screen documents at length, and it is the same split
// for the same reason: two clients cannot see each other, and "is this team too full" is a question
// about everybody at once.
//
// ---------------------------------------------------------------------------------------------
// WHY IT IS HOSTED BY FTraceCharacterSelect RATHER THAN BY THE HUD
// ---------------------------------------------------------------------------------------------
// ATraceHUD::DrawHUD already ticks the character select every frame, outside every gate, and uses
// ONE flag — CharacterSelect.IsOpen() — to hide the UMG ammo corner and to decide whether closing
// the pause menu should hand movement back. The two screens are one flow (team, then character) and
// share one input-suppression contract, so a second overlay registered separately would have needed
// all three of those places to learn about it, and any one of them missed is a player left unable
// to move or an ammo counter drawn over a modal.
//
// So FTraceCharacterSelect owns an instance of this class, ticks it first, and reports itself open
// while either screen is up. Nothing in ATraceHUD changed. See TraceCharacterSelect.cpp's Tick.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "UObject/WeakObjectPtr.h"

#include "TraceTypes.h"   // ETraceTeam
#include "UI/Widgets/Menu/TraceKitMotion.h"   // FTraceKitFade — the page's open/close fade (P10)
#include "UI/Widgets/Menu/TraceKitPageClock.h"   // FTraceKitPageClockDraw — what the page's clock drew

class AHUD;
class ATracePlayerController;
class ATracePlayerState;
struct FKey;

/**
 * The team-select overlay.
 *
 * Lifecycle is driven ENTIRELY by Tick(), exactly like the character select: the host calls it once
 * per frame and this class opens, draws, polls and closes off the replicated flag. There is no
 * Open() for a host to remember to call, because the moment a host has to remember something is the
 * moment a player joins mid-warm-up and never gets a screen.
 */
class TRACE_API FTraceTeamSelect
{
public:
	/** True while the overlay is being drawn. The host folds this into its own IsOpen(). */
	bool IsOpen() const { return bOpen; }

	/**
	 * P10: the page's opacity this frame, 0..1 — fading in after it opens, fading out (drawn as it last
	 * stood, with no input and no pointer) for FadeOutSeconds after it closes. IsOpen() is unchanged by
	 * the fade: input, the pointer and every rule still follow the replicated flag alone.
	 */
	float GetFadeAlpha() const { return Fade.Alpha(); }

	/**
	 * The line the page's clock (TIME and its box, top right) is centred on, in 1080p px from the top:
	 * the title's, or under the title on a window too narrow for both. The pause menu draws the clock
	 * on it over this page, so pausing does not move the clock.
	 */
	float GetClockCapMidPx() const { return ClockCapMidPx; }

	/** The page's clock as the last Draw drew it (none when the page has no deadline). A harness reads it. */
	const FTraceKitPageClockDraw& GetDrawnClock() const { return DrawnClock; }

	/**
	 * Poll input and draw. Call exactly once per frame, before the character select's own draw.
	 *
	 * @param PC            the LOCAL controller. Null closes the overlay — the open flag lives on it.
	 * @param LocalState    the local player's state, for their current team. May be null early.
	 * @param bInputAllowed false while something in front of this owns the keyboard AND the pointer
	 *                      (the pause menu). The screen still DRAWS, for the reason the character
	 *                      select still draws: a screen that vanished behind the pause menu would read
	 *                      as the choice having been cancelled, and the close-out clock is still
	 *                      running under it. It does not draw its POINTER then: the surface in front
	 *                      has the live one, and a frozen blade under the scrim is a second pointer.
	 */
	void Tick(AHUD* HUD, ATracePlayerController* PC, ATracePlayerState* LocalState,
		float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed);

	/**
	 * H, polled by the host on every frame BOTH screens are closed.
	 *
	 * Static, and here rather than in the host, so the one file that knows what H means owns both
	 * edges of it: this opens the screen and PollInput's own H closes it again. Returns true when a
	 * request was sent — the host ignores it today, and it is returned anyway because the caller that
	 * eventually wants to swallow the key press should not have to re-derive whether one happened.
	 *
	 * *** KEYBOARD ONLY, AND D32-PADMENU LEFT IT THAT WAY DELIBERATELY. *** This poll runs during
	 * GAMEPLAY, so any pad button it read would have to be one no gameplay verb is using — and the
	 * shipped layout uses all sixteen a standard controller has, with MENU/START spent on pause. The
	 * .cpp has the full argument and the two fixes that would remove the gap. Nothing a pad player
	 * NEEDS is behind this key: the join flow opens the screen by itself, and once it is up a pad
	 * drives all of it.
	 */
	static bool PollOpenHotkey(ATracePlayerController* PC);

	/** The key that opens this screen. Referenced by the footer hint so the two cannot drift. */
	static const TCHAR* OpenKeyName() { return TEXT("H"); }

	/**
	 * Is @p Key one this screen acts on? Part of FTraceCharacterSelect::ReadsKey — the flow's answer
	 * when it hands gameplay input back. PollInput's keys; a new verb there belongs here too.
	 */
	static bool ReadsKey(const FKey& Key);

#if !UE_BUILD_SHIPPING
	/**
	 * Trace.Teams.Pick <blue|orange> — drives the highlight and the confirm exactly as a key would.
	 *
	 * Goes through the SAME request path a key press does rather than calling the controller RPC
	 * directly: what a headless verification of this screen must prove is that THIS FILE's request
	 * path works, and a test that bypassed it would pass with the whole screen disconnected.
	 */
	void DebugPick(ETraceTeam Team, ATracePlayerController* PC, ATracePlayerState* LocalState);

	/**
	 * The title as the last Draw set it, in 1080p design px: its cap height, where its caps are
	 * centred from the top, and its letter spacing. Trace.UI.Fade.Verify holds it against the loadout
	 * page's across the page turn.
	 */
	float GetDebugTitleCapPx() const { return DebugTitleCapPx; }
	float GetDebugTitleCapMidPx() const { return DebugTitleCapMidPx; }
	float GetDebugTitleTrackPx() const { return DebugTitleTrackPx; }
#endif

private:
	// ---- Rows ------------------------------------------------------------------------------------
	//
	// Two team plates and two actions. A linear index rather than an enum because left/right walks it
	// and the number keys index it, and because the two actions are drawn as a footer row rather than
	// as plates — the index is the input model, not the layout.

	static constexpr int32 RowBlue = 0;
	static constexpr int32 RowOrange = 1;
	static constexpr int32 RowCount = 2;

	static ETraceTeam TeamForRow(int32 Row) { return (Row == RowOrange) ? ETraceTeam::Orange : ETraceTeam::Blue; }

	void PollInput(ATracePlayerController* PC, ATracePlayerState* LocalState);
	void Confirm(ATracePlayerController* PC, ATracePlayerState* LocalState);
	void Draw(AHUD* HUD, ATracePlayerController* PC, ATracePlayerState* LocalState);
	void DrawTeamPlate(AHUD* HUD, ATracePlayerController* PC, ATracePlayerState* LocalState,
		int32 Row, float X, float Y, float W, float H);
	void DrawFooter(AHUD* HUD, ATracePlayerController* PC);
	void DrawCursor(AHUD* HUD, ATracePlayerController* PC);

	/** C / pad X is offered and read only while the loadout may be changed (warm-up, half time). */
	static bool CanChangeLoadout(const ATracePlayerController* PC);

	/** The verdict line, or empty. Reads the controller's client-local last-result trio. */
	FString VerdictLine(const ATracePlayerController* PC) const;

	bool bOpen = false;

	/** P10 — the page's open/close fade, on real time. See GetFadeAlpha. */
	FTraceKitFade Fade;

	/**
	 * The clock the page ANIMATES on (hover breath, the urgent countdown pulse): the world's REAL time,
	 * which keeps running under the pause menu. Now (the host's world time) stays the clock of this
	 * page's logic, because the verdict timestamps it is compared with are world time.
	 */
	float AnimNow = 0.f;

	/**
	 * 0..RowCount-1. STARTS ON THE PLAYER'S OWN TEAM, so the reflex "continue" press — ENTER or A —
	 * keeps the team and moves on (the server answers AlreadyOnTeam and closes the screen). It used to
	 * start on the other team, which made that same press a team switch.
	 */
	int32 Highlighted = RowBlue;

	/** See GetClockCapMidPx. Set by every Draw; the title's line (TraceMenuKit::PageTitleCapMidPx) before the first. */
	float ClockCapMidPx = 56.f;

	/** See GetDrawnClock. */
	FTraceKitPageClockDraw DrawnClock;

	/** A refusal this screen made itself (it does not send what the balance rule already refuses). */
	FString LocalVerdict;
	float LocalVerdictTime = -1000.f;

	/** Screen rects of the two plates as of the last draw. Hit testing and hover use them. */
	FBox2D RowRects[RowCount];

	/** The plate the pointer is over, or INDEX_NONE. Recomputed every frame from the drawn rects. */
	int32 HoveredRow = INDEX_NONE;

	/** Frame before which no input is read; the key that opened this must not also press a row. */
	uint64 IgnoreInputBeforeFrame = 0;

	/** Local time of the last request, so a held key does not send ten a second. */
	float LastRequestTime = -1000.f;
	static constexpr float RequestCooldown = 0.75f;

	// ---- Cached per-frame view metrics, set at the top of Tick ---------------------------------

	float ViewW = 0.f;
	float ViewH = 0.f;
	float UIScale = 1.f;
	float Now = 0.f;

	// ---- Mouse ---------------------------------------------------------------------------------

	FVector2D CursorPos = FVector2D::ZeroVector;
	bool bHasCursor = false;
	bool bMouseWasDown = false;

	/** This screen is front-most and draws the pointer. False under the pause menu. */
	bool bPointerOwned = true;

	// ---- Held-key repeat for the left/right walk: the menus' shared clock (TracePadMenu) ---------

	int32 LastNavDir = 0;
	float NextNavTime = 0.f;

#if !UE_BUILD_SHIPPING
	/** See GetDebugTitleCapPx. */
	float DebugTitleCapPx = 0.f;
	float DebugTitleCapMidPx = 0.f;
	float DebugTitleTrackPx = 0.f;
#endif
};

#if !UE_BUILD_SHIPPING
/**
 * Set by Trace.Teams.Pick, consumed by the next Tick of whichever screen is open. 1 = Blue,
 * 2 = Orange, 0 = idle.
 *
 * A file-scope int rather than a pointer to the instance, for the reason
 * GTraceCharacterSelectDebugPick's comment gives: this class is a plain member of an object owned by
 * an AHUD that a travel destroys, and a raw pointer to it left in a console command is a dangling
 * pointer waiting for a map change. An int is safe to leave behind.
 */
extern TRACE_API int32 GTraceTeamSelectDebugPick;

// PICK IS THE ONLY LATCH, and that is the whole design of the console surface. Trace.Teams.Select,
// .Close and .Character each call one function on the controller — the same call the H and C keys
// make — so they run IMMEDIATELY and there is nothing on this object for a latch to protect.
// Trace.Teams.Report only reads, and runs immediately for a sharper reason: a -TraceExec round fires
// its whole list in one callback, so a latched report would always print one tick late and
// "report, pick, report" would produce two identical before-pictures. See the commands at the bottom
// of TraceTeamSelect.cpp.
#endif
