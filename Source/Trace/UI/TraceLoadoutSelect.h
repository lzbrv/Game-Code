// Trace — the loadout screen. The page that replaced character select.
//
// Spec: "after team select, replace the character page with a loadout screen... one passive ability,
// one active ability, and one movement ability", and "allow players to change ability loadouts
// during halftime".
//
// ---------------------------------------------------------------------------------------------
// IT IS A CARD GRID ON THE HANDMADE KIT
// ---------------------------------------------------------------------------------------------
// Three tabs across the top say which question you are on (MOVEMENT, PASSIVE, ACTIVATED) and what
// you have already answered; the grid below answers it. Every surface is the artist's kit
// (UI/Widgets/Menu/TraceMenuKit.h): navy plates, the orange hover glow baked into the hover plate,
// the dark disabled plate, white words, the blade pointer, on opaque black. The team screen before
// it is drawn from the same kit, so the two pages read as one machine.
//
// ---------------------------------------------------------------------------------------------
// WHAT A CARD SAYS, AND WHAT IT DELIBERATELY DOES NOT
// ---------------------------------------------------------------------------------------------
// A card is the ability's name where it has one, and its description. Every ability has a name now;
// one without (Demo 35 left three blank until the owner named them) shows the description alone.
//
// NO CHARACTER NAME APPEARS ANYWHERE ON THIS SCREEN. The abilities are freestanding; the kit id
// survives INTERNALLY as the ability's source and is not a thing a player sees.
//
// ---------------------------------------------------------------------------------------------
// THE INPUT MODEL — one verb per key, the same on keys, pad and pointer
// ---------------------------------------------------------------------------------------------
//     move the highlight   arrows / WASD / D-pad / stick      pointer
//     change tab           Q / E, LB / RB                     click a tab
//     EQUIP                ENTER, pad A                       click a card
//                          ...and ENTER / A then moves to the next tab, so three presses build a
//                          loadout. On the last tab it stays put and the LOCK IN plate lights up.
//     LOCK IN (SAVE)       F, pad X                           click LOCK IN (SAVE)
//     back                 BACKSPACE, pad B: the previous tab (match); leave without saving
//                          (library, where ESC does the same and a BACK plate is on screen)
//     saved loadouts       1-5 loads, SHIFT+1-5 stores        click / shift-click a slot (match)
//                          pad Y loads the next saved loadout (in slot order, skipping ones that
//                          cannot load); LT + Y saves to the slot last loaded or saved here, or to
//                          the first empty one
//
// EVERY KEY THE PAGE NAMES, IT NAMES FOR BOTH DEVICES. The chips beside the tabs and LOCK IN show the
// keyboard key, and the pad button next to it once a pad has been seen (and CONTROLLER INPUT is on);
// the legend is a keyboard line with a pad line under it. They used to flip to pad-only the moment
// any pad input was seen, which left a keyboard player reading LB / RB / X.
//
// LOCK IN SAYS SENDING, NOT LOCKED IN, until the server answers. The server closes the window when it
// accepts (the page then fades out on LOCKED IN) and sends ClientLockInRefused when it does not, which
// the page turns into a terse reason with the page still up and working.
//
// The first build of this page had ENTER lock in whatever was STAGED while the arrows only moved a
// highlight, so a keyboard or pad player could not change a single ability: they arrowed to SUSPEND,
// pressed ENTER and locked in JET BOOTS. Equipping and locking in are now two different keys.
//
// ONE THING WEARS THE HOVER LOOK AT A TIME (stylespec §5: hover and keyboard selection are one
// state). Whichever the player used last leads: a pointer that MOVED lights what it is on (a tab,
// LOCK IN, a saved slot, or the card it walked onto); a key or pad press hands the look back to the
// highlighted card, or to LOCK IN once ENTER has answered the last question. The tab you are on is
// not a hover: it keeps the default plate and says so with the olive word. See ResolveLit.
//
// EVERY KEY IS AN EDGE AGAINST THE KEY'S OWN LAST STATE, SAMPLED ON EVERY FRAME THE PAGE IS OPEN —
// including the frames its input is gated off (the pause menu in front, the frame it opened). Before
// that, the page only remembered a key while it was allowed to read it, so an ENTER still held from
// the team screen, from RESUME on the pause menu, or a SPACE held at the half-time whistle read as a
// fresh press on the first allowed frame and locked in the default loadout.
//
// ---------------------------------------------------------------------------------------------
// WHAT THIS CLASS IS NOT ALLOWED TO DECIDE
// ---------------------------------------------------------------------------------------------
// It does not decide whether a loadout is legal or whether it may be changed: both are server
// verdicts, consulted locally so the common case needs no round trip and then sent anyway.
// ServerSetLoadout is the only authority. It does not decide whether it is OPEN either —
// ATracePlayerState::bCharacterSelectOpen is replicated and is the only condition.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "UObject/WeakObjectPtr.h"

#include "InputCoreTypes.h"   // FKey

#include "Abilities/TraceAbilityTypes.h"
#include "TraceTypes.h"   // ETraceMatchState
#include "UI/Text/TraceTextWeight.h"
#include "UI/Widgets/Menu/TraceKitMotion.h"   // FTraceKitFade — the page's open/close fade (P10)
#include "UI/Widgets/Menu/TraceKitPageClock.h"   // FTraceKitPageClockDraw — what the page's clock drew

class AHUD;
class APlayerController;
class ATracePlayerState;
class UObject;

namespace TraceLoadoutSelect
{
	/**
	 * Is the loadout screen the one that shows after team select?
	 *
	 * `Trace.UI.LoadoutScreen`, default ON — this IS the rework. The arm exists so the character page
	 * can be put back in one console command without a build.
	 */
	TRACE_API bool IsArmed();

	/**
	 * May a player hand their loadout back from the team screen right now (C, pad X)?
	 *
	 * THE LOCK'S OWN RULE, asked by the server before it reopens a select window and by the team
	 * screen before it offers the key, so the legend can never advertise a key the server refuses.
	 * "Loadout is locked until halftime": yes before the match starts and during the half-time break,
	 * no during live play. With the loadout page switched off (the legacy character page), the old
	 * mid-match character switch is allowed as it always was.
	 */
	TRACE_API bool IsReopenAllowed(ETraceMatchState MatchState, bool bHalfTimeBreak, bool bLoadoutArmed);

	/** The same rule, read off @p WorldContext's replicated Trace game state. True with no game state. */
	TRACE_API bool IsReopenAllowed(const UObject* WorldContext);

	/**
	 * Is @p Key one the loadout page acts on (Q / E tabs, F lock, ENTER equip, 1-5, the arrows, the
	 * pointer's click, the pad's)? Part of FTraceCharacterSelect::ReadsKey — the flow's answer when
	 * the window closes and gameplay input comes back: Q is PARRY, and a Q pressed to change tab must
	 * not come back as one.
	 *
	 * DERIVED FROM THE PAGE'S OWN KEY TABLE (ReadKeysWith), not a second copy of it, so a verb added
	 * there is covered here with no edit. SPACE is not in that table, so it is not in this one.
	 */
	TRACE_API bool ReadsKey(const FKey& Key);
}

/**
 * One frame of the page's keys and buttons, as LEVELS ("held right now"), keyboard and pad folded.
 * Public so the harness can drive the real input path with scripted frames.
 */
struct FTraceLoadoutKeys
{
	/** -1 / 0 / +1: arrows, WASD, D-pad, left stick. */
	int32 NavX = 0;
	int32 NavY = 0;

	bool bEquip = false;     // ENTER, pad A
	bool bLock = false;      // F, pad X
	bool bBack = false;      // BACKSPACE, pad B (and ESC in library mode)
	bool bTabLeft = false;   // Q, pad LB
	bool bTabRight = false;  // E, pad RB
	bool bShift = false;     // SHIFT, pad LT: turns a load into a save
	bool bNumber[5] = { false, false, false, false, false };

	/** Pad Y: load the next saved loadout (with LT held: save). The pad has no number keys. */
	bool bSavedNext = false;

	/**
	 * ESC's engine press EVENT this frame (library mode only). MENU/START's synthetic Escape is a
	 * press and a release inside one frame, which no level read ever sees.
	 */
	bool bEscapePressed = false;

	/** Every key held — what the page assumes on the frame it opens. */
	static FTraceLoadoutKeys AllHeld()
	{
		FTraceLoadoutKeys Keys;
		Keys.bEquip = Keys.bLock = Keys.bBack = Keys.bTabLeft = Keys.bTabRight = Keys.bSavedNext = true;
		for (bool& bDigit : Keys.bNumber)
		{
			bDigit = true;
		}
		return Keys;
	}
};

/**
 * What wears the kit's hover look this frame — at most ONE thing on the page. Resolved in one place
 * (FTraceLoadoutSelect::ResolveLit), drawn from, and read back by the harness.
 */
struct FTraceLoadoutLit
{
	int32 Card = INDEX_NONE;
	int32 Tab = INDEX_NONE;
	int32 Saved = INDEX_NONE;
	bool bConfirm = false;   // LOCK IN (SAVE in the library)
	bool bBack = false;      // the library's BACK

	/** The pointer's button is down on the lit thing, and the press began on this page: PRESSED. */
	bool bPressed = false;

	int32 Count() const
	{
		return (Card != INDEX_NONE ? 1 : 0) + (Tab != INDEX_NONE ? 1 : 0) + (Saved != INDEX_NONE ? 1 : 0)
			+ (bConfirm ? 1 : 0) + (bBack ? 1 : 0);
	}
};

struct TRACE_API FTraceLoadoutSelect
{
	bool IsOpen() const { return bOpen; }

	/**
	 * P10: the in-match page's opacity this frame, 0..1 — fading in after it opens, and fading out for
	 * FadeOutSeconds after it closes (drawn as it last stood: no input, no pointer). IsOpen() does not
	 * include the fade. Library mode does not fade: its host (the options overlay) does.
	 */
	float GetFadeAlpha() const { return Fade.Alpha(); }

	/**
	 * Should the in-match page be up for @p LocalState this frame? The server's select window AND
	 * the Trace.UI.LoadoutScreen arm.
	 */
	static bool WantsOpen(const ATracePlayerState* LocalState);

	/**
	 * @param bInputAllowed false while something in front owns the keys AND THE POINTER (the pause
	 *                      menu, or team select). The page keeps drawing, draws no pointer of its own,
	 *                      and keeps sampling keys so nothing held through the gate fires after it.
	 */
	void Tick(AHUD* HUD, APlayerController* PC, ATracePlayerState* LocalState,
		float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed);

	// ---- library mode: the same grid, editing a SAVED slot instead of what you are playing ------
	//
	// THE ONE DIFFERENCE IS WHERE LOCK IN GOES: in match mode it asks the server, in library mode it
	// writes the slot to disk and closes (the plate reads SAVE). This mode has no player state to
	// send to, so it can never be a way around the loadout lock.
	void OpenLibrary(int32 SlotIndex);
	void CloseLibrary();
	bool IsLibraryOpen() const { return LibrarySlot != INDEX_NONE; }
	int32 GetLibrarySlot() const { return LibrarySlot; }

	/** Draws and drives the editor. Call INSTEAD of Tick(). Returns false once it has closed. */
	bool TickLibrary(AHUD* HUD, APlayerController* PC,
		float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed);

	// ---- test seams -----------------------------------------------------------------------------
	void DebugPick(ETraceLoadoutSlot Slot, ETraceAbilityId Id);
	void DebugConfirm(ATracePlayerState* LocalState);
	void DebugRecall(int32 Index);
	void DebugStore(int32 Index);
	FTraceLoadout GetStaged() const { return Staged; }
	int32 GetTab() const { return Tab; }
	int32 GetHighlighted(ETraceLoadoutSlot Slot) const;

	/** What the page does on the frame it opens (seed from the server, every key assumed held). */
	void DebugBeginInput(ATracePlayerState* LocalState);

	/**
	 * The levels ReadKeys would produce if exactly @p DownKeys were held — the key table itself, so a
	 * harness can prove LB / RB change tab and SPACE (jump) does nothing, without touching the engine.
	 */
	static FTraceLoadoutKeys DebugKeysFor(const TArray<FKey>& DownKeys, bool bLibrary);

	/** One frame of @p Down through the SAME sample-then-act path Tick and TickLibrary run. */
	void DebugInput(const FTraceLoadoutKeys& Down, ATracePlayerState* LocalState, bool bInputAllowed,
		float InNow);

	/** Parks the pointer as if a mouse sample had landed there. */
	void DebugSetCursor(const FVector2D& Pos);

	/** Would the page draw its pointer this frame? False while something in front owns it. */
	bool DebugWouldDrawPointer() const { return bHasCursor && bPointerOwned; }

	/** Test seam: the screen rect of card @p Index as of the last draw, for a scripted click. */
	FBox2D DebugCardRect(int32 Index) const;

	/** Test seams: park hit rects where a draw would leave them, so the pointer path runs without a canvas. */
	void DebugSetTabRect(int32 Index, const FBox2D& Rect);
	void DebugSetCardRect(int32 Index, const FBox2D& Rect);
	void DebugSetConfirmRect(const FBox2D& Rect) { ConfirmRect = Rect; }

	/** Test seam: park saved slot @p Index's hit rect where a draw would leave it. */
	void DebugSetSavedRect(int32 Index, const FBox2D& Rect);

	/**
	 * One pointer sample at @p Pos, button up or down (SHIFT held or not), through the SAME path the
	 * live page polls.
	 */
	void DebugPointer(const FVector2D& Pos, bool bButtonDown, ATracePlayerState* LocalState, bool bShiftHeld = false);

	/** What would wear the hover look if the page drew now. */
	FTraceLoadoutLit DebugLit() const { return ResolveLit(); }

	/** The status line as it stands (SENDING, LOCKED IN, a refusal, LOADED 2...). */
	const FString& DebugMessage() const { return LastMessage; }

	/** The in-match title the last Tick chose, and whether it drew the half-time score and note. */
	const FString& DebugTitle() const { return PageTitle; }
	bool DebugHalfTimeHeader() const { return bHalfTimeHeader; }

	/** The page's clock (TIME, top right) as the last draw drew it; none without a deadline. A harness reads it. */
	const FTraceKitPageClockDraw& GetDrawnClock() const { return DrawnClock; }

#if !UE_BUILD_SHIPPING
	/**
	 * The title as the last draw set it, in 1080p design px: its cap height, where its caps are
	 * centred from the top, and its letter spacing. Trace.UI.Fade.Verify holds team select's against it.
	 */
	float GetDebugTitleCapPx() const { return DebugTitleCapPx; }
	float GetDebugTitleCapMidPx() const { return DebugTitleCapMidPx; }
	float GetDebugTitleTrackPx() const { return 0.f; }   // DrawCapText sets no tracking
#endif

	/** The hover salt this page draws under (TraceMenuKit::FScopedHoverSalt): one per instance. */
	uint32 HoverSalt() const;

	/**
	 * The type sizes DrawCard would use for EVERY card of EVERY tab at this view: one name size and
	 * one description size per card, tab by tab, in grid order. Unnamed cards report a name size of 0.
	 */
	void DebugTypeSizes(float InViewW, float InViewH, float InUIScale, TArray<float>& OutNameSizes,
		TArray<float>& OutBodySizes);

	/** The most cards one tab can hold: the grid is Columns x Rows. */
	static constexpr int32 MaxCards = 10;

private:
	/** The open edge, shared by the match page and the harness. */
	void OnOpened(ATracePlayerState* LocalState);

	/** Reads this frame's levels off the controller. Library mode adds ESC as a back key. */
	FTraceLoadoutKeys ReadKeys(const APlayerController* PC) const;

	/** Edges against last frame (ALWAYS, gated or not), then the verbs (only when allowed). */
	void StepInput(const FTraceLoadoutKeys& Down, ATracePlayerState* LocalState, bool bInputAllowed);

	void PollPointer(APlayerController* PC, ATracePlayerState* LocalState, bool bAct);

	/** PollPointer past the controller: one sample (position if @p bSampled, the button, SHIFT). */
	void StepPointer(bool bSampled, const FVector2D& SamplePos, bool bButtonDown, bool bShiftHeld,
		ATracePlayerState* LocalState, bool bAct);

	/** The ONE thing that wears the hover look, from the pointer, the keys' highlight and LOCK IN's hint. */
	FTraceLoadoutLit ResolveLit() const;

	void MoveTab(int32 Delta);
	void MoveCard(int32 DeltaX, int32 DeltaY);
	void EquipHighlighted();
	void LockIn(ATracePlayerState* LocalState);
	void Confirm(ATracePlayerState* LocalState);
	void Recall(int32 Index);
	void Store(int32 Index);

	/** Pad Y: the next saved loadout that can load, after the one last loaded or saved here. */
	void RecallNext();

	/** Pad LT + Y: the slot last loaded or saved here, else the first empty one. */
	void StoreCurrent();

	/** A refusal of the LOCK IN this page sent, if ClientLockInRefused has brought one since. */
	void ReadLockInReply(const ATracePlayerState* LocalState);

	void SetMessage(const FString& Text, bool bWarning);

	/** The grid's tile size at the current view. */
	void GridTile(float& OutTileW, float& OutTileH) const;

	/** ONE name size and ONE description size for every card on every tab, solved once per layout. */
	void SolveTypeSizes(float TileW, float TileH);

	void Draw(AHUD* HUD, APlayerController* PC, const ATracePlayerState* LocalState, const FString& Title);
	void DrawHeader(AHUD* HUD, const ATracePlayerState* LocalState, const FString& Title);
	void DrawTabs(AHUD* HUD, APlayerController* PC, float X, float Y, float W);
	void DrawGrid(AHUD* HUD, float X, float Y);   // the grid's size is the layout's (GridTile)
	void DrawCard(AHUD* HUD, int32 Index, float X, float Y, float W, float H, float NameSize,
		float TabBodySize, float BodyRoom);
	void DrawActionRow(AHUD* HUD, APlayerController* PC, float X, float Y, float W);
	void DrawSavedRow(AHUD* HUD, float X, float Y, float MaxW);
	void DrawFooter(AHUD* HUD, APlayerController* PC, float Y);
	void DrawPointer(AHUD* HUD, APlayerController* PC);

	void SyncStagedFromServer(ATracePlayerState* LocalState);

	/** The slot the tabs are on, and which card is highlighted within each slot's grid. */
	int32 Tab = 0;
	int32 Highlighted[static_cast<int32>(ETraceLoadoutSlot::Count)] = { 0, 0, 0 };

	FTraceLoadout Staged;
	bool bOpen = false;

	/** The player changed something since the page opened. Only then does the deadline send it. */
	bool bStagedDirty = false;

	/** The deadline send has gone; it goes once per opening. */
	bool bAutoSent = false;

	/** ENTER / A just equipped on the last tab: LOCK IN glows to say what is left to do. */
	bool bLockHint = false;

	/**
	 * A LOCK IN has gone to the server and not been answered. RefusalsSeen is the component's refusal
	 * count at the send, and SentLoadout what was sent, so a close of the window can tell "accepted"
	 * from "the deadline shut it while the request was in flight".
	 */
	bool bLockSent = false;
	int32 RefusalsSeen = 0;
	FTraceLoadout SentLoadout;

	/** The saved slot this opening last loaded or stored: where pad Y goes on from and LT + Y saves to. */
	int32 LastSavedSlot = INDEX_NONE;

	/** The in-match title (BUILD YOUR LOADOUT, or HALF TIME in the break), chosen by Tick. */
	FString PageTitle;

	/** See GetDrawnClock. */
	FTraceKitPageClockDraw DrawnClock;

#if !UE_BUILD_SHIPPING
	/** See GetDebugTitleCapPx. */
	float DebugTitleCapPx = 0.f;
	float DebugTitleCapMidPx = 0.f;
#endif

	/** The break's header: the score and SIDES SWITCHED beside the title. */
	bool bHalfTimeHeader = false;

	int32 LibrarySlot = INDEX_NONE;

	FString LastMessage;
	float LastMessageTime = -1000.f;
	bool bMessageIsWarning = false;

	float ViewW = 0.f;
	float ViewH = 0.f;
	float UIScale = 1.f;
	float Now = 0.f;

	/**
	 * The clock the page ANIMATES on (hover breath, the urgent timer pulse): real time, which keeps
	 * running under the pause menu. Now stays the clock of the page's logic (key repeat, messages).
	 */
	float AnimNow = 0.f;

	/** P10 — the in-match page's open/close fade, on real time. */
	FTraceKitFade Fade;

	// ---- pointer ---------------------------------------------------------------------------------
	FVector2D CursorPos = FVector2D::ZeroVector;
	bool bHasCursor = false;
	bool bMouseWasDown = false;

	/** A release only clicks if its press began on this page while it was taking input. */
	bool bMouseArmed = false;

	/** SHIFT on the last pointer sample: a click on a saved slot then SAVES, so any slot is a target. */
	bool bPointerShift = false;

	/** This page is front-most and draws the pointer. False under the pause menu. */
	bool bPointerOwned = true;

	/** Rects from the last draw. Reset to invalid on every open, before anything is hit-tested. */
	FBox2D CardRects[MaxCards];
	FBox2D TabRects[static_cast<int32>(ETraceLoadoutSlot::Count)];
	FBox2D SavedRects[5];
	FBox2D ConfirmRect = FBox2D(ForceInit);
	FBox2D BackRect = FBox2D(ForceInit);

	int32 HoveredCard = INDEX_NONE;
	int32 HoveredTab = INDEX_NONE;
	int32 HoveredSaved = INDEX_NONE;
	bool bHoveredConfirm = false;
	bool bHoveredBack = false;

	/**
	 * The pointer, not the keys, was used last: what it rests on wears the hover look. A pointer that
	 * moves (or presses) sets it; any key or pad verb clears it. Off on every open.
	 */
	bool bPointerLed = false;

	/** ResolveLit, taken once at the top of Draw so every plate on the frame reads the same answer. */
	FTraceLoadoutLit FrameLit;

	// ---- keys ------------------------------------------------------------------------------------
	/** Last frame's levels. Every verb is an edge against this. */
	FTraceLoadoutKeys Held;

	/** The shared menu repeat clock (TracePadMenu::StepRepeat), one encoded direction. */
	int32 NavLastDir = 0;
	float NavNextTime = 0.f;

	/** Swallows the frame that opened the page, so the press that opened it cannot also act. */
	uint64 IgnoreInputBeforeFrame = 0;

	// ---- text caches: the layout only changes on a resize or a tab switch -------------------------
	struct FWrapCache
	{
		ETraceAbilityId Id = ETraceAbilityId::None;
		float Width = -1.f;
		float TabSize = -1.f;
		float Size = -1.f;
		ETraceTextWeight Weight = ETraceTextWeight::Light;
		TArray<FString> Lines;
	};
	FWrapCache WrapCache[MaxCards];

	/**
	 * ONE NAME SIZE FOR THE WHOLE SCREEN, the smallest any name on any tab needs, and ONE DESCRIPTION
	 * SIZE, the largest at which every card on every tab fits. They were solved per tab, so switching
	 * tab visibly rescaled the cards (names 13 / 12 / 18 px caps, bodies 14 / 10 / 11). Re-solved only
	 * when the tile or the description face changes.
	 */
	float NameFitSize = -1.f;
	float BodyFitSize = -1.f;
	float BodyFitRoom = -1.f;
	float FitTileW = -1.f;
	float FitTileH = -1.f;
	ETraceTextWeight FitWeight = ETraceTextWeight::Light;
};
