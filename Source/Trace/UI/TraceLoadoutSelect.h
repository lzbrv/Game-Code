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
// A card is the ability's name where it has one, and its description. Three abilities are unnamed
// on purpose (Demo 35) and their card is the description alone.
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
//
// The first build of this page had ENTER lock in whatever was STAGED while the arrows only moved a
// highlight, so a keyboard or pad player could not change a single ability: they arrowed to SUSPEND,
// pressed ENTER and locked in JET BOOTS. Equipping and locking in are now two different keys.
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
	bool bShift = false;
	bool bNumber[5] = { false, false, false, false, false };

	/**
	 * ESC's engine press EVENT this frame (library mode only). MENU/START's synthetic Escape is a
	 * press and a release inside one frame, which no level read ever sees.
	 */
	bool bEscapePressed = false;

	/** Every key held — what the page assumes on the frame it opens. */
	static FTraceLoadoutKeys AllHeld()
	{
		FTraceLoadoutKeys Keys;
		Keys.bEquip = Keys.bLock = Keys.bBack = Keys.bTabLeft = Keys.bTabRight = true;
		for (bool& bDigit : Keys.bNumber)
		{
			bDigit = true;
		}
		return Keys;
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

	void MoveTab(int32 Delta);
	void MoveCard(int32 DeltaX, int32 DeltaY);
	void EquipHighlighted();
	void LockIn(ATracePlayerState* LocalState);
	void Confirm(ATracePlayerState* LocalState);
	void Recall(int32 Index);
	void Store(int32 Index);
	void SetMessage(const FString& Text, bool bWarning);

	void Draw(AHUD* HUD, APlayerController* PC, const ATracePlayerState* LocalState, const FString& Title);
	void DrawHeader(AHUD* HUD, const ATracePlayerState* LocalState, const FString& Title);
	void DrawTabs(AHUD* HUD, APlayerController* PC, float X, float Y, float W);
	void DrawGrid(AHUD* HUD, float X, float Y, float W, float H);
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

	/** One name size per tab, the smallest any of its names needs, so a grid's names all match. */
	float NameFitSize[static_cast<int32>(ETraceLoadoutSlot::Count)] = { -1.f, -1.f, -1.f };
	float NameFitWidth[static_cast<int32>(ETraceLoadoutSlot::Count)] = { -1.f, -1.f, -1.f };

	/** ...and one description size per tab, the largest at which every card's words fit. */
	float BodyFitSize[static_cast<int32>(ETraceLoadoutSlot::Count)] = { -1.f, -1.f, -1.f };
	float BodyFitRoom[static_cast<int32>(ETraceLoadoutSlot::Count)] = { -1.f, -1.f, -1.f };
};
