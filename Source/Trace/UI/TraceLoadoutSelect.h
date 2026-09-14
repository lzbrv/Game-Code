// Trace — the loadout screen. The page that replaced character select.
//
// Spec: "after team select, replace the character page with a loadout screen... one passive ability,
// one active ability, and one movement ability", and "allow players to change ability loadouts
// during halftime".
//
// ---------------------------------------------------------------------------------------------
// IT IS A CARD GRID, BECAUSE THE SCREENS EITHER SIDE OF IT ARE
// ---------------------------------------------------------------------------------------------
// The first version of this page was three columns of text lists. It worked and it was wrong: the
// team screen before it and the character screen it replaced are both plates you point at and click,
// authored in 1080p design pixels against one palette, and a page of text lists between them reads
// as a different program. This one uses the same layout constants, the same neon-on-navy palette and
// the same pointer rules, so the three pages are one machine.
//
// A TAB PER SLOT, THEN TEN CARDS. The character screen asked ONE question with ten answers. This
// asks THREE, and they are independent — that is the entire rework. Three tabs across the top say
// which question you are on and what you have already answered; the grid below answers it.
//
// ---------------------------------------------------------------------------------------------
// WHAT A CARD SAYS, AND WHAT IT DELIBERATELY DOES NOT
// ---------------------------------------------------------------------------------------------
// ONLY THE ACTIVATED ABILITY HAS A NAME — RIPPLE, CHUD, SPIKE, PICKLER, STING, MODDED, SNAP,
// SLIMEWALL, QUAKE, ZIP. Movement and passive abilities never had names; the character screen drew
// them as prose under a heading and so does this. An earlier pass invented thirty names and renamed
// three real ones on top; that is reverted, and a card for a movement ability is its description.
//
// NO CHARACTER NAME APPEARS ANYWHERE ON THIS SCREEN. The abilities are freestanding — there is no
// "home" character to credit, and printing one would tell the player something that is no longer
// true about what they are building. The kit id survives INTERNALLY as the ability's source, which
// is what let the rework happen without an enum migration; it is not a thing a player sees.
//
// ---------------------------------------------------------------------------------------------
// WHAT THIS CLASS IS NOT ALLOWED TO DECIDE
// ---------------------------------------------------------------------------------------------
// The same rule the character screen lives under. It does not decide whether a loadout is legal and
// it does not decide whether it may be changed: both are server verdicts, consulted locally so the
// common case needs no round trip and then sent anyway. ServerSetLoadout is the only authority.
//
// It does not decide whether it is OPEN either — ATracePlayerState::bCharacterSelectOpen is
// replicated and is the only condition, which is what makes "the half time break shows this screen"
// true by construction rather than by a client clock that could disagree with the whistle.
//
// There is no "taken by a teammate" greying and there must not be: per-team uniqueness is about the
// FACE, so two players may absolutely run the same movement ability.
//
// ---------------------------------------------------------------------------------------------
// POINTER, KEYS AND PAD — all three, and the pointer rules are the character screen's
// ---------------------------------------------------------------------------------------------
// Every hit target keeps its rect from the last draw and the pointer is tested against those, which
// is how the character screen does it. Two of its hard-won rules are carried over rather than
// rediscovered: the FIRST pointer sample is not a move (or the highlight jumps to wherever the
// pointer was parked when the screen opened), and HOVER MAY ONLY TAKE THE HIGHLIGHT IF THE POINTER
// ACTUALLY MOVED (or a pointer resting over the grid pins the highlight and neither the keys nor the
// pad can shift it). Both are documented at length in TraceCharacterSelect.cpp; both cost a
// screenshot and a log to find the first time.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "UObject/WeakObjectPtr.h"

#include "Abilities/TraceAbilityTypes.h"
#include "Core/TraceCharacterRoster.h"

class AHUD;
class APlayerController;
class ATracePlayerState;

namespace TraceLoadoutSelect
{
	/**
	 * Is the loadout screen the one that shows after team select?
	 *
	 * `Trace.UI.LoadoutScreen`, default ON — this IS the rework. The arm exists so the character page
	 * can be put back in one console command without a build. Delete it, and the early-out in
	 * TraceCharacterSelect.cpp, once this screen has been played and kept.
	 */
	TRACE_API bool IsArmed();
}

struct TRACE_API FTraceLoadoutSelect
{
	bool IsOpen() const { return bOpen; }

	void Tick(AHUD* HUD, APlayerController* PC, ATracePlayerState* LocalState,
		float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed);

	// ---- library mode: the same grid, editing a SAVED slot instead of what you are playing ------
	//
	// The main menu needs a page to build the five saved loadouts on and it needs exactly this
	// screen. THE ONE DIFFERENCE IS WHERE CONFIRM GOES: in match mode it asks the server, in library
	// mode it writes the slot to disk and closes. A mode rather than a subclass because everything
	// except the destination of one keypress is the same code — and it means the library can never
	// be a way around the loadout lock, by construction: this mode has no player state to send to.
	void OpenLibrary(int32 SlotIndex);
	void CloseLibrary();
	bool IsLibraryOpen() const { return LibrarySlot != INDEX_NONE; }
	int32 GetLibrarySlot() const { return LibrarySlot; }

	/** Draws and drives the editor. Call INSTEAD of Tick(). Returns false once it has closed. */
	bool TickLibrary(AHUD* HUD, APlayerController* PC,
		float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed);

	// ---- test seams -----------------------------------------------------------------------------
	void DebugPick(ETraceLoadoutSlot Slot, ETraceCharacterId Id);
	void DebugConfirm(ATracePlayerState* LocalState);
	void DebugRecall(int32 Index);
	void DebugStore(int32 Index);
	FTraceLoadout GetStaged() const { return Staged; }

	/** Test seam: the screen rect of card @p Index as of the last draw, for a scripted click. */
	FBox2D DebugCardRect(int32 Index) const;

private:
	void PollKeys(APlayerController* PC, ATracePlayerState* LocalState);
	void PollPointer(APlayerController* PC, ATracePlayerState* LocalState);

	void MoveTab(int32 Delta);
	void MoveCard(int32 Delta);
	void EquipHighlighted();
	void Confirm(ATracePlayerState* LocalState);
	void Recall(int32 Index);
	void Store(int32 Index);

	void Draw(AHUD* HUD, const TCHAR* Title, const TCHAR* FooterHint);
	void DrawTabs(AHUD* HUD, float X, float Y, float W);
	void DrawCard(AHUD* HUD, int32 Index, float X, float Y, float W, float H);
	void DrawSavedRow(AHUD* HUD, float X, float Y, float W);
	void DrawPointer(AHUD* HUD);

	void SyncStagedFromServer(ATracePlayerState* LocalState);

	/** The slot the tabs are on, and which card is highlighted within each slot's grid. */
	int32 Tab = 0;
	int32 Highlighted[static_cast<int32>(ETraceLoadoutSlot::Count)] = { 0, 0, 0 };

	FTraceLoadout Staged;
	bool bOpen = false;
	bool bStagedSeeded = false;

	int32 LibrarySlot = INDEX_NONE;

	FString LastMessage;
	float LastMessageTime = -1000.f;

	float ViewW = 0.f;
	float ViewH = 0.f;
	float UIScale = 1.f;
	float Now = 0.f;

	// ---- pointer ---------------------------------------------------------------------------------
	FVector2D CursorPos = FVector2D::ZeroVector;
	bool bHasCursor = false;
	bool bMouseWasDown = false;

	/** Rects from the last draw. Invalid until drawn, which is why every click path tests bIsValid. */
	FBox2D CardRects[TraceCharacterRoster::Count];
	FBox2D TabRects[static_cast<int32>(ETraceLoadoutSlot::Count)];
	FBox2D SavedRects[5];
	FBox2D ConfirmRect = FBox2D(ForceInit);

	int32 HoveredCard = INDEX_NONE;
	int32 HoveredTab = INDEX_NONE;
	int32 HoveredSaved = INDEX_NONE;
	bool bHoveredConfirm = false;

	// ---- keys ------------------------------------------------------------------------------------
	float NextNavTime = 0.f;
	static constexpr float NavRepeatDelay = 0.35f;
	static constexpr float NavRepeatInterval = 0.12f;
	bool bConfirmWasDown = false;
	bool bCancelWasDown = false;
	bool bNumberWasDown[5] = { false, false, false, false, false };

	/** Swallows the frame that opened the page, so the press that opened it cannot also confirm. */
	uint64 IgnoreInputBeforeFrame = 0;
};
