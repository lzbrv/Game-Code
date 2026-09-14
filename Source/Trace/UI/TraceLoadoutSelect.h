// Trace — the loadout screen. The page that replaced character select.
//
// Spec: "at the beginning of a game, after team select, replace the character page with a loadout
// screen... one passive ability, one active ability, and one movement ability", and "allow players
// to change ability loadouts during halftime".
//
// SHAPED EXACTLY LIKE FTraceCharacterSelect, deliberately, and the reasons are that class's three:
// plain C++ (nothing here outlives a frame and nothing replicates), drawn through AHUD::DrawRect /
// DrawText, and it POLLS input rather than binding it. The polling argument is the same and just as
// strong: this screen is up during warm-up and during the half time break, when the local pawn may
// not exist and the gameplay input component may not be wired, and a bound delegate on a pawn that
// has not spawned is a key press that goes nowhere.
//
// ---------------------------------------------------------------------------------------------
// WHAT THIS CLASS IS NOT ALLOWED TO DECIDE
// ---------------------------------------------------------------------------------------------
// The same rule the character screen lives under, for the same reason. It does not decide whether a
// loadout is legal and it does not decide whether it may be changed. Both are server verdicts:
// UTraceAbilityComponent::IsLoadoutLegal and ::IsLoadoutChangeOpen are consulted so the common case
// needs no round trip, and then the request is sent anyway and whatever comes back is what the
// player is shown. ServerSetLoadout is the only authority.
//
// It does not decide whether it is OPEN either. ATracePlayerState::bCharacterSelectOpen is
// replicated from the server and is the only condition — the same flag the character screen used,
// which is what makes "the half time break shows this screen" true by construction rather than by a
// client-side clock that could disagree with the whistle.
//
// ---------------------------------------------------------------------------------------------
// THREE COLUMNS, NOT TEN CARDS, AND THAT IS THE WHOLE DESIGN
// ---------------------------------------------------------------------------------------------
// The character screen asked one question with ten answers. This one asks THREE questions with ten
// answers each, and they are independent — that is the entire point of the rework. So the layout is
// three columns side by side, each a list of the ten abilities for that slot, each showing what is
// currently equipped. Left and right move between questions; up and down answer the one you are on.
//
// There is no "taken by a teammate" greying here and there must not be: per-team uniqueness is about
// the FACE now, not the abilities, so two players on a team may absolutely run the same movement
// ability. Adding a grey-out would be inventing a rule the server does not enforce.
//
// ---------------------------------------------------------------------------------------------
// D32-PADMENU — A CONTROLLER DRIVES THIS SCREEN
// ---------------------------------------------------------------------------------------------
// D-pad and left stick walk the grid, A confirms, exactly as on the character screen, and the button
// names come from TracePadMenu so the screens cannot disagree about what A means. The repeat clock
// is this screen's own and is shared with its arrow keys, so a thumb and a finger walk at one speed.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "UObject/WeakObjectPtr.h"

#include "Abilities/TraceAbilityTypes.h"
#include "Core/TraceCharacterRoster.h"

class AHUD;
class APlayerController;
class ATracePlayerState;
class UFont;

namespace TraceLoadoutSelect
{
	/**
	 * Is the loadout screen the one that shows after team select?
	 *
	 * `Trace.UI.LoadoutScreen`, default ON — this IS the rework. The arm exists so the character page
	 * can be put back in one console command if the loadout screen turns out to be wrong in
	 * playtest, without a build: the two screens are mutually exclusive and the character page is
	 * still whole behind it. Delete the arm, and this file's early-out in TraceCharacterSelect.cpp,
	 * once the loadout screen has been played and kept.
	 */
	TRACE_API bool IsArmed();
}

struct TRACE_API FTraceLoadoutSelect
{
	/** Open when the server says the select window is, exactly as the character screen was. */
	bool IsOpen() const { return bOpen; }

	void Tick(AHUD* HUD, APlayerController* PC, ATracePlayerState* LocalState,
		float InViewW, float InViewH, float InUIScale, float InNow, bool bInputAllowed);

	/** Test seam: set one slot's pick without a keypress. */
	void DebugPick(ETraceLoadoutSlot Slot, ETraceCharacterId Id);

	/** Test seam: send whatever is currently staged, as if ENTER had been pressed. */
	void DebugConfirm(ATracePlayerState* LocalState);

	/** What the screen currently has staged — not necessarily what the server has accepted. */
	FTraceLoadout GetStaged() const { return Staged; }

	// NO TEAM SCREEN HERE, DELIBERATELY. FTraceCharacterSelect still hosts it and still runs first;
	// this page replaces only what came AFTER team select. Two hosts would tick it twice.

private:
	void PollInput(APlayerController* PC, ATracePlayerState* LocalState);
	void MoveColumn(int32 Delta);
	void MoveRow(int32 Delta);
	void Confirm(ATracePlayerState* LocalState);

	void Draw(AHUD* HUD, ATracePlayerState* LocalState);
	void DrawColumn(AHUD* HUD, int32 ColumnIndex, float X, float Y, float W, float H);
	void DrawFooter(AHUD* HUD, float X, float Y, float W);

	/** Seeds Staged from whatever the player already has, so opening mid-match is not a blank page. */
	void SyncStagedFromServer(ATracePlayerState* LocalState);

	bool bOpen = false;

	/** Which of the three questions the cursor is on, and which answer it is hovering. */
	int32 Column = 0;
	int32 Row[static_cast<int32>(ETraceLoadoutSlot::Count)] = { 0, 0, 0 };

	/** The pick in progress. Sent on confirm, never before. */
	FTraceLoadout Staged;

	/** True once Staged has been seeded for this opening. Cleared when the screen closes. */
	bool bStagedSeeded = false;

	/** What the server last refused, so the screen can say so rather than silently reverting. */
	FString LastRefusal;
	float LastRefusalTime = -1000.f;

	float ViewW = 0.f;
	float ViewH = 0.f;
	float UIScale = 1.f;
	float Now = 0.f;

	float NextNavTime = 0.f;
	static constexpr float NavRepeatDelay = 0.35f;
	static constexpr float NavRepeatInterval = 0.12f;

	bool bConfirmWasDown = false;
};
