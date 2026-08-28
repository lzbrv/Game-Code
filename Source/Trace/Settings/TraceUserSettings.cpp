// Trace — player control settings implementation. See TraceUserSettings.h.

#include "Settings/TraceUserSettings.h"

#include "Engine/Engine.h"              // GEngine->GetCurrentPlayWorld, for the WP2 console hook
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"  // ServerChangeName - the engine's own rename path
#include "GameFramework/PlayerState.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Trace.h"                      // LogTraceGame
#include "UObject/UObjectGlobals.h"     // GetMutableDefault

// =================================================================================================
// The action table
//
// DefaultKey is a function pointer rather than an FKey value because EKeys' statics are initialised
// during module startup, and a namespace-scope FKey table would be built from them at static-init
// time with no ordering guarantee. A call is free and cannot be wrong.
// =================================================================================================

namespace
{
	/**
	 * "This row ships with only one key." Named rather than a null pointer so the table reads as a
	 * table — every row states its second bind, and sixteen of them state that there isn't one.
	 */
	FKey Default_None()        { return FKey(); }

	FKey Default_MoveForward() { return EKeys::W; }
	FKey Default_MoveBack()    { return EKeys::S; }
	FKey Default_MoveLeft()    { return EKeys::A; }
	FKey Default_MoveRight()   { return EKeys::D; }
	FKey Default_Jump()        { return EKeys::SpaceBar; }
	FKey Default_Crouch()      { return EKeys::LeftControl; }
	FKey Default_Dash()        { return EKeys::LeftShift; }
	/**
	 * *** SPEC v25 §7 — PARRY IS THE RIGHT MOUSE BUTTON. *** Verbatim: "Remove the default bind of
	 * right clicking to throw the core while carrying it. Make the default key bind for parrying
	 * right click mouse."
	 *
	 * This reverses the v3 §3 reasoning that put parry on Q, and the reversal is sound because the
	 * premise has gone: v3 refused right mouse because "RIGHT MOUSE IS ALREADY PASS, and overloading
	 * it onto the carrier's own pass button would make the two mechanics unusable together". The
	 * same note that moves parry here takes the throw off this button (see Default_Pass below), so
	 * the collision v3 was avoiding no longer exists.
	 *
	 * WHAT ELSE THIS BUTTON NOW DOES, AND WHY IT IS NOT A SECOND COLLISION. Spec v25 §2 makes right
	 * mouse the Core-PULL input during a turnover. Parry and pull are the same physical button and
	 * are dispatched from the same handler (ATracePlayerController::OnParryStarted), because they
	 * can never both be legal at the same instant:
	 *
	 *     PARRY requires you to BE CARRYING the Core   (ETraceParryRefusal::NotCarrying)
	 *     PULL  requires you to NOT be carrying it, and to be hovering a turned-over Core with line
	 *           of sight, on the team that did NOT drop it, inside the 5 s window
	 *
	 * Those two predicates are mutually exclusive on `is this pawn the carrier`, so the press is
	 * delivered to both verbs and at most one authoritative gate can accept it. Neither eats the
	 * other; there is no priority to get wrong. See OnParryStarted for the full argument.
	 *
	 * =============================================================================================
	 * *** SPEC v28 §3d — PARRY IS Q **AND** THE THUMB MOUSE BUTTON, AND IT IS OFF RIGHT MOUSE. ***
	 * =============================================================================================
	 *
	 * Verbatim: "Parry defaults to BOTH Q and the thumb mouse button."
	 *
	 * TWO DEFAULTS IS WHAT §3c's SECOND SLOT IS FOR, and this is the only row in the table that uses
	 * it. Q is where parry lived before v25 §7 moved it to the mouse, so returning muscle memory is
	 * being handed back rather than invented; ThumbMouseButton is EKeys' name for mouse 4, the
	 * button an FPS player's thumb is already resting on.
	 *
	 * IT ALSO VACATES THE RIGHT MOUSE BUTTON, WHICH IS THE POINT OF THE TIMING. Spec v28 §10 puts
	 * MELEE on right click by default. Parry has held that button since v25 §7, and two verbs on one
	 * button with no exclusion between them (you can parry and you can melee while carrying nothing)
	 * is a genuine conflict, not a shareable one — see ETraceInputStates. So §3d and §10 are the same
	 * change seen from two ends: this line is what makes right mouse free for the melee to take.
	 *
	 * *** THE ConfigId MOVES WITH IT: "ParryPull" -> "ParryKeys". *** This table's own history says a
	 * default key change is INVISIBLE to anybody who has ever saved a setting, because Save() writes a
	 * line for every action and RefreshFromConfig honours them all — the owner's machine has
	 * `ParryPull=RightMouseButton` in it right now. Shipping §3d under the old id would leave every
	 * existing player parrying on right mouse, i.e. sharing a button with the new melee, which is the
	 * exact collision this note exists to avoid. The id has done this twice before ("Boost" -> "Parry"
	 * in v3, "Parry" -> "ParryPull" in v25 §7) and the migration is the same one line of existing
	 * behaviour both times: a line naming an id the table does not have is DROPPED and the row falls
	 * back to its shipped default. Cost: one hand-rebound parry key, once, loudly, in the log.
	 *
	 * "ParryKeys" and not back to "Parry", which was free again, because a pre-v25 file still contains
	 * `Parry=Q` — reusing the string would resurrect a five-versions-old line, put parry on Q ALONE and
	 * silently drop the thumb button this note asks for.
	 */
	FKey Default_Parry()       { return EKeys::Q; }
	/** SPEC v28 §3d — the second half of "BOTH Q and the thumb mouse button". Mouse 4. */
	FKey Default_ParryAlt()    { return EKeys::ThumbMouseButton; }
	FKey Default_Fire()        { return EKeys::LeftMouseButton; }
	/**
	 * *** SPEC v25 §7 — THE THROW HAS NO DEFAULT KEY OF ITS OWN. *** It used to be right mouse; the
	 * note removes that bind and does not name a replacement, so this returns an INVALID FKey and
	 * the row ships UNBOUND.
	 *
	 * THE THROW IS NOT LOST, WHICH IS THE WHOLE REASON "NONE" IS THE RIGHT ANSWER RATHER THAN A
	 * CONSOLATION KEY. Mouse 1 already throws/passes the Core while carrying, in BOTH modes and
	 * from before this note:
	 *
	 *     goals mode     ATraceCharacter::DoFirePressed -> DoPassPressed -> the charged throw
	 *                    (the HUD's own carry caption is "LMB  -  THROW")
	 *     endzones mode  the same call, driving the 0.5 s hover-hold pass
	 *
	 * So the carrier's throw button is unchanged and is the one the HUD has always taught. This row
	 * is a SECOND, optional route to the same verb, and inventing a default for it — Q, freed by
	 * parry moving off it, was the obvious candidate — would have added a control nobody asked for,
	 * put a live key on a row the player has never needed, and made the freed key unavailable for
	 * the next thing. The row stays on the options screen so anyone who wants a dedicated throw key
	 * can bind one; it simply starts empty.
	 *
	 * AN INVALID DEFAULT IS A SUPPORTED STATE, not a special case. RefreshFromConfig seeds the table
	 * with it, ApplyControlSettings' MapButton skips invalid keys so no mapping is created,
	 * IsAtDefaults compares FKey() to FKey() and agrees, RedeliverHeldPressEdges' IsHeld already
	 * tests Key.IsValid() first, and DescribeKey prints "UNBOUND". Every one of those paths existed
	 * for ClearKey; this default only reaches them one step earlier.
	 */
	FKey Default_Pass()        { return FKey(); }
	FKey Default_Scoreboard()  { return EKeys::Tab; }
	/**
	 * Spec v13 §2, verbatim: "Change default keybinds for switching weapons to be: 1 (switch to
	 * knife) and 2 (switch to gun)."
	 *
	 * EKeys::One / EKeys::Two are the NUMBER ROW, not the numpad (that is EKeys::NumPadOne). The
	 * request is the genre convention — weapon slots on the top row — and the number row is what a
	 * player reaches for without thinking and what a laptop without a numpad still has.
	 *
	 * Both pass IsBindableKey: they are real buttons, not axes, not Escape and not AnyKey. Nothing
	 * else in the table claims a digit, so neither default steals a key from another action on a
	 * first run (SetKey's stealing rule would have logged it if it did).
	 *
	 * SPEC v15 §5: these two are now the ONLY weapon binds. The SwapWeapon toggle that used to sit
	 * above them — ConfigId "SwapWeapon", default F — is deleted, so F is unclaimed again and a
	 * player's saved `SwapWeapon=F` line is dropped on load like any other line naming an action
	 * that no longer exists. See the ETraceInputAction::EquipKnife comment for the full argument.
	 */
	/**
	 * *** SPEC v31 §1 — "1 is pistol, 2 is smg, 3 is knife". THE STOW LAYOUT IS GONE. ***
	 *
	 * v29 §5 shipped 1 = STOW GUNS, 2 = PISTOL, 3 = SMG. v31 §1 reverts the dual-wield knife, which
	 * takes the stow state with it — the knife is a WEAPON SLOT again, so there are three weapons and
	 * three keys and the owner has named which is which. Every one of the three keys changes meaning
	 * and every one of the three DEFAULTS moves:
	 *
	 *     key 1   the PISTOL   (was the stow)
	 *     key 2   the SMG      (was the pistol)
	 *     key 3   the KNIFE    (was the SMG) — and the knife slot is what pays the v12 §3 speed boost
	 *
	 * ALL THREE ARE THE NUMBER ROW, not the numpad (EKeys::NumPadOne and friends), for the reason the
	 * v13 §2 comment above gives: the number row is what a player reaches for without thinking and
	 * what a laptop still has. Nothing else in this table claims a digit, so no default steals a key
	 * from another action on a first run — SetKey's stealing rule would log it if one did.
	 *
	 * *** WHY THE ORDER IS PISTOL, SMG, KNIFE AND NOT KNIFE, PISTOL, SMG. *** It is the owner's own
	 * sentence and it is not arbitrary: the two guns are the weapons a player swaps between under
	 * pressure and they are now adjacent under the index and middle fingers, with the knife — a
	 * deliberate commitment that gives up shooting entirely — one key further out. v13 §2's original
	 * "1 knife, 2 gun" put the commitment first because there were only two weapons; with three, the
	 * guns take the near keys.
	 *
	 * *** THE CONFIG IDS ALL MOVE, AND THAT IS THE MIGRATION. *** See the ETraceInputAction::EquipKnife
	 * comment for the full argument; the short version is that Save() writes a line for EVERY action,
	 * so a returning player's TraceUserSettings.ini already says
	 *
	 *     KeyBindings=StowGuns=One          (or, for someone who last played v28, EquipKnife=One)
	 *     KeyBindings=EquipPistol=Two
	 *     KeyBindings=EquipSmg=Three
	 *
	 * and RefreshFromConfig honours all three over anything this table says. Ship the new layout
	 * under the old ids and a returning player presses 1 and gets nothing at all (there is no stow
	 * state any more), presses 2 and gets the pistol they were told is on 1, and presses 3 and gets
	 * the SMG they were told is on 2 — three dead or mislabelled binds, which is precisely what
	 * "migrate existing configs, or returning players keep dead binds" is about. Under the NEW ids
	 * all three saved lines name actions the table does not have, are DISCARDED by the parse loop
	 * exactly as `SwapWeapon=F` and `Boost=E` are, and all three rows seed their v31 defaults.
	 *
	 * "PistolSlot" / "SmgSlot" / "KnifeSlot" ARE FRESH STRINGS THAT HAVE NEVER BEEN IN ANYBODY'S
	 * FILE. Deliberately not a reuse of "EquipKnife" or "EquipGun": those two ARE in the files of
	 * everyone who last played v28 or earlier, and reusing one would honour a stale line instead of
	 * dropping it — putting the knife back on 1 for exactly the players this migration exists for.
	 * Trace.Settings.VerifyBinds prints every dropped line by name, so this is checkable.
	 */
	FKey Default_EquipPistol() { return EKeys::One; }
	FKey Default_EquipSmg()    { return EKeys::Two; }
	FKey Default_EquipKnife()  { return EKeys::Three; }
	/**
	 * SPEC v14 §5. E and V are NAMED BY THE DOC, so there is no key to choose here — only a collision
	 * to check. Taken already: WASD, Space, LeftCtrl, LeftShift, Q, mouse1, mouse2, Tab, 1, 2.
	 * Neither E nor V is claimed by any row above, so neither default steals a key on a first run
	 * (SetKey's stealing rule would log it if it did).
	 *
	 * Historical note worth keeping: E was the pre-v3 BOOST key. That action is gone and its ConfigId
	 * ("Boost") is not this one, so an old TraceUserSettings.ini's `Boost=E` line is dropped by
	 * RefreshFromConfig exactly as it is for Parry — nobody inherits an ability bound by accident.
	 */
	FKey Default_Ability()          { return EKeys::E; }
	FKey Default_AbilitySecondary() { return EKeys::V; }
	/**
	 * SPEC v16 §1, verbatim: "R to reload."
	 *
	 * R is NAMED BY THE DOC, so there is no key to choose — only a collision to check. Claimed
	 * already: WASD, Space, LeftCtrl, LeftShift, Q, mouse1, mouse2, Tab, 1, 2, E, V. R is free, so
	 * this default steals nothing on a first run (SetKey's stealing rule would log it if it did), and
	 * it is the genre convention besides.
	 */
	FKey Default_Reload()           { return EKeys::R; }
	/**
	 * SPEC v26 §1, verbatim: "Make parry and pull core two separate binds in the settings menu."
	 *
	 * NO KEY IS NAMED BY THE NOTE, so this one is a choice and it is argued rather than asserted.
	 * Claimed already: WASD, Space, LeftControl, LeftShift, mouse1, mouse2, Tab, 1, 2, E, V, R.
	 *
	 * F WINS ON TWO COUNTS. It is unclaimed — spec v15 §5 vacated it when it deleted the SwapWeapon
	 * toggle — and "F is the key you press to grab the thing you are looking at" is the single
	 * strongest convention this genre has, which matters for a verb whose whole interaction is
	 * "hover the turned-over Core and hold".
	 *
	 * Q WAS THE OTHER CANDIDATE and was passed over on purpose. Spec v25 §7 freed it when parry moved
	 * to right mouse, so it is available and it is close to WASD. But the pull is a HOLD taken while
	 * still steering, and Q asks the ring finger to leave A; F is under an index finger that is
	 * already resting on D. Q also carries a decade of "that is the ability key" muscle memory from
	 * other games in this genre, and this action is not an ability.
	 *
	 * RIGHT MOUSE IS DELIBERATELY NOT THE DEFAULT, even though that is where v25 §2 put the pull.
	 * The note asks for TWO binds; shipping both on one key would satisfy the letter (two rows on the
	 * page) and none of the intent. Parry keeps the button v25 §7 explicitly named for it.
	 *
	 * SetKey's stealing rule would log it if this default took a key from another action on a first
	 * run. It does not.
	 */
	/**
	 * =============================================================================================
	 * *** SPEC v31 §5 TOOK F. THE PULL IS ON G. THE ARGUMENT ABOVE IS STILL RIGHT AND STILL LOST. ***
	 * =============================================================================================
	 *
	 * "Implement the new knife model [...] bind the F key to inspect." The owner has now named F for
	 * two different verbs across two specs, and only one of them can have it. INSPECT WINS BECAUSE IT
	 * WAS NAMED; the pull's F was this file's own choice, argued at length directly above, and an
	 * argued choice loses to an instruction.
	 *
	 * THE TWO CANNOT SHARE THE KEY, and that is a mechanical fact rather than a preference. Both rows
	 * are `NotCarrying`, ActionsMayShareAKey is one bitwise AND, and intersecting masks mean the two
	 * verbs can both be legal at the same instant — knife in hand, loose Core in front of you. One
	 * press would fill a pull ring AND start a 3.2 s flourish. See ETraceInputAction::Inspect for the
	 * full write-up, including the one-line revert.
	 *
	 * G, AND NOT Q. Q's objection from the paragraph above has not changed (it asks the ring finger
	 * to leave A, and it carries a decade of "that is the ability key"). G is the key immediately to
	 * the right of the one the pull is losing, under the same index finger that is already on D, and
	 * it is unclaimed: WASD, Space, LeftCtrl, LeftShift, mouse1, mouse2, Q + thumb mouse, Tab, 1, 2,
	 * 3, E, V, R, F.
	 *
	 * *** THE PULL IS NOT LOSING ITS ONLY ROUTE, WHICH IS WHAT MAKES THIS AFFORDABLE. *** Spec v28
	 * §10's precedence rule already dispatches the pull from the MELEE button — TraceMelee::
	 * HandleMeleeInput returns EMeleeInputResult::CorePull when the circle is on screen, and
	 * ATracePlayerController::OnMeleeStarted logs it as such. A player who never notices the G row
	 * pulls with right mouse exactly as they do today.
	 *
	 * THE ConfigId MOVES WITH THE KEY — "PullCore" -> "PullCoreKey" — for the reason this table has
	 * now spent four times: Save() writes a line for EVERY action and RefreshFromConfig honours a
	 * saved line over this function, so without the move a returning player keeps `PullCore=F` and
	 * ends up with the pull and the flourish on one key, which is the exact collision being avoided.
	 */
	FKey Default_PullCore()         { return EKeys::G; }

	/**
	 * *** SPEC v31 §5 — THE KNIFE FLOURISH. "bind the F key to inspect." ***
	 *
	 * THE KEY IS NAMED BY THE OWNER, so there is no choice to argue — only a collision to resolve,
	 * and it is resolved directly above by moving the Core pull to G. This is the first row in this
	 * table whose default was taken FROM another action on purpose rather than found free; SetKey's
	 * stealing rule is not involved (that governs a player's rebinds, not the shipped table), so the
	 * two defaults are simply written to disagree with each other's history and the ConfigId
	 * migration on the loser's row is what makes it land.
	 *
	 * IT IS COSMETIC AND IT STAYS COSMETIC. TraceKnifeView::RequestInspect writes one presentation
	 * timestamp inside a world subsystem, sends no RPC, takes no cooldown, and is outranked by both
	 * the stab and the draw in the clip chooser — so a real action interrupts the flourish on the
	 * frame it is requested. Nothing in the project reads "is inspecting" as a gameplay condition.
	 */
	FKey Default_Inspect()          { return EKeys::F; }

	/**
	 * *** SPEC v28 §10 — "Melee should be bound to right click by default." ***
	 *
	 * THE RIGHT MOUSE BUTTON, AND IT IS FREE BY CONSTRUCTION RATHER THAN BY LUCK. Spec v25 §7 put the
	 * parry here; spec v28 §3d moved the parry to Q + the thumb mouse button precisely so this row
	 * could have the button, and paid the "ParryPull" -> "ParryKeys" id migration to make sure a
	 * RETURNING player's saved file could not put their old parry back on it. Trace.Input.VerifyRightMouse
	 * asserts the whole arrangement — melee holds the button, nothing else does — so a regression here
	 * fails a console command instead of a playtest.
	 *
	 * SetKey's stealing rule would log it if this default took a key from another action on a first run.
	 * It does not: the table above leaves the button unclaimed.
	 */
	FKey Default_Melee()            { return EKeys::RightMouseButton; }
}

const TArray<FTraceInputActionInfo>& TraceInputActions::All()
{
	// Function-local static: built on first use, after EKeys is up, and never rebuilt.
	static const TArray<FTraceInputActionInfo> Table =
	{
		// SPEC v28 §3b — THE FOURTH COLUMN IS THE EXCLUSION GROUP, and it is the whole conflict rule.
		// ETraceInputStates::Match means "live whether or not this pawn is carrying the Core", which is
		// true of everything a player does with their feet, their weapon slots and their abilities. The
		// four rows that are NOT Match are the four the note is about; each one argues for itself below.
		{ ETraceInputAction::MoveForward, TEXT("MoveForward"), TEXT("MOVE FORWARD"), &Default_MoveForward, &Default_None, ETraceInputStates::Match },
		{ ETraceInputAction::MoveBack,    TEXT("MoveBack"),    TEXT("MOVE BACK"),    &Default_MoveBack,    &Default_None, ETraceInputStates::Match },
		{ ETraceInputAction::MoveLeft,    TEXT("MoveLeft"),    TEXT("STRAFE LEFT"),  &Default_MoveLeft,    &Default_None, ETraceInputStates::Match },
		{ ETraceInputAction::MoveRight,   TEXT("MoveRight"),   TEXT("STRAFE RIGHT"), &Default_MoveRight,   &Default_None, ETraceInputStates::Match },
		{ ETraceInputAction::Jump,        TEXT("Jump"),        TEXT("JUMP"),         &Default_Jump,        &Default_None, ETraceInputStates::Match },
		{ ETraceInputAction::Crouch,      TEXT("Crouch"),      TEXT("CROUCH / SLIDE"), &Default_Crouch,    &Default_None, ETraceInputStates::Match },
		{ ETraceInputAction::Dash,        TEXT("Dash"),        TEXT("DASH"),         &Default_Dash,        &Default_None, ETraceInputStates::Match },
		// *** SPEC v25 §7. BOTH ConfigIds ON THESE TWO ROWS ARE DELIBERATELY NEW STRINGS. ***
		//
		// Read this before "tidying" them back. Changing a DEFAULT key does nothing at all for anyone
		// who has ever saved a setting, because Save() -> FlattenToConfig() writes a line for EVERY
		// action, so a returning player's TraceUserSettings.ini already contains
		//
		//     KeyBindings=Parry=Q
		//     KeyBindings=Pass=RightMouseButton
		//
		// and RefreshFromConfig honours both, over the top of whatever this table says. The owner's
		// own machine has exactly that file. Ship the new defaults under the old ids and right mouse
		// still throws the Core, parry is still on Q, and the note reads as un-done.
		//
		// So the ids move with the meaning, which is this codebase's established migration and not a
		// new idea: ETraceInputAction::Parry's own comment records "Boost" -> "Parry" doing precisely
		// this in spec v3, and spec v15's "SwapWeapon" removal relies on the same drop rule. A line
		// naming a ConfigId the table no longer has is DISCARDED by RefreshFromConfig (see the parse
		// loop) and the action falls back to its shipped default. Trace.Settings.VerifyBinds prints
		// every dropped line by name, so this is checkable rather than assertable.
		//
		// WHAT A RETURNING PLAYER LOSES: a hand-rebound parry or throw key, once. That is the price
		// of the note landing at all, it is paid a single time, and it is loud in the log.
		//
		// *** SPEC v28 §3d SPENDS THAT COST A THIRD TIME: "ParryPull" -> "ParryKeys". *** §3d moves the
		// parry onto Q AND the thumb mouse button, which is a DEFAULT CHANGE and therefore invisible to
		// every returning player unless the id moves with it — and this time leaving them behind would
		// not be cosmetic, because §10 is putting MELEE on the right mouse button they would still be
		// parrying with. The full argument is at Default_Parry(). The DisplayName also gains its second
		// chip on the options page; that string is persisted nowhere and is free to change.
		//
		// PARRY IS `Carrying` AND NOTHING ELSE: TraceParry::RequestParry refuses with
		// ETraceParryRefusal::NotCarrying, so a parry key is dead weight for a player who is not holding
		// the Core — which is exactly what makes it shareable with the pull and with fire.
		{ ETraceInputAction::Parry,       TEXT("ParryKeys"),   TEXT("PARRY"),        &Default_Parry,       &Default_ParryAlt, ETraceInputStates::Carrying },
		// FIRE IS `NotCarrying`, WHICH IS THE OWNER'S OWN SENTENCE: "firing only while NOT carrying".
		// ATraceCharacter::DoFirePressed returns early into DoPassPressed the moment bIsCarrier is true,
		// so the gun genuinely cannot be fired while holding the Core. That overload is why the note can
		// ask for the throw on this very button and be right: on a shared key the carrier's press reaches
		// the throw twice (once through fire's overload, once through the throw's own handler) and
		// ATraceCore::RequestPassInput is a latch, so the second arrival is absorbed.
		{ ETraceInputAction::Fire,        TEXT("Fire"),        TEXT("FIRE"),         &Default_Fire,        &Default_None, ETraceInputStates::NotCarrying },
		// THE THROW IS `Carrying`: ATraceCore::RequestPassInput only arms for the pawn that is holding
		// the Core. Fire's NotCarrying and this row's Carrying are disjoint, so ActionsMayShareAKey says
		// yes and spec v28 §3b's example — throw core on the same button as fire — is legal.
		{ ETraceInputAction::Pass,        TEXT("ThrowCore"),   TEXT("THROW / PASS CORE"), &Default_Pass, &Default_None, ETraceInputStates::Carrying },
		{ ETraceInputAction::Scoreboard,  TEXT("Scoreboard"),  TEXT("SCOREBOARD"),   &Default_Scoreboard,  &Default_None, ETraceInputStates::Match },
		// SPEC v13 §2. These two rows exist for the options screen as much as for the game: the
		// rebind list IS this table, walked in order, so an action that is not here is an action the
		// player cannot see or rebind however well it is wired up in the controller. "Both new binds
		// must appear in the settings rebind list" is satisfied by these lines and by nothing else.
		//
		// SPEC v15 §5 DELETED THE `SwapWeapon` ROW that used to sit directly above these two. That is
		// also what removes "SWAP WEAPON" from the options screen's rebind list — the list is this
		// table walked in order and nothing else, so there is no second place to go and delete it.
		//
		// *** SPEC v31 §1 — THREE WEAPONS, THREE KEYS: "1 is pistol, 2 is smg, 3 is knife". ***
		//
		//     1  PISTOL   -> ETraceEquippedWeapon::Gun     normal speed
		//     2  SMG      -> ETraceEquippedWeapon::Smg     normal speed
		//     3  KNIFE    -> ETraceEquippedWeapon::Knife   the v12 §3 speed boost, and a 35% shorter
		//                                                  pullout (v31 §1, KnifeSwapMultiplier)
		//
		// THIS REPLACES v29 §5's 1 = STOW GUNS / 2 = PISTOL / 3 = SMG. The stow state went away with
		// the dual-wield revert: with bDualWieldKnife off the knife is a WEAPON you swap to rather
		// than a permanent off-hand blade, so "put the guns away" and "take the knife out" stopped
		// being two different sentences. ETraceEquippedWeapon::Knife is the same enumerator it always
		// was and TraceMelee::ShouldUseKnifeMovementProfile is unchanged — it asks "is a FIREARM out",
		// which is true of the knife slot for exactly the reason it was true of the stow state.
		//
		// *** ALL THREE ConfigIds ARE NEW STRINGS, AND THAT IS THE MIGRATION. *** This is the fourth
		// time this file has paid that price ("Boost" -> "Parry" in v3, "Parry"/"Pass" ->
		// "ParryKeys"/"ThrowCore" in v28 §3d, "EquipKnife"/"EquipGun" -> "StowGuns"/"EquipPistol" in
		// v29 §5) and the argument is unchanged: Save() -> FlattenToConfig() writes a line for EVERY
		// action, so a returning player's TraceUserSettings.ini already contains
		//
		//     KeyBindings=StowGuns=One          <- or EquipKnife=One, for a v28-or-older file
		//     KeyBindings=EquipPistol=Two
		//     KeyBindings=EquipSmg=Three
		//
		// and RefreshFromConfig honours all three over whatever this table says. THIS TIME ALL THREE
		// HAD TO MOVE, not two: every key changes meaning, so leaving any id in place would leave that
		// row on its v29 key. Ship the new layout under the old ids and a returning player gets three
		// wrong binds — 1 asks for a stow state that no longer exists, 2 gives the pistol the page
		// says is on 1, 3 gives the SMG the page says is on 2. Under the new ids all three saved lines
		// name actions the table does not have, are DISCARDED by the parse loop exactly as
		// `SwapWeapon=F` is, and all three rows seed their v31 defaults.
		//
		// WHAT A RETURNING PLAYER LOSES: a hand-rebound weapon-select key, once, loudly
		// (Trace.Settings.VerifyBinds prints every dropped line by name).
		//
		// THE ENUMERATOR SPELLINGS STAY. ETraceInputAction is the INDEX into
		// UTraceUserSettings::Bindings and into this table, matched 1:1 by the static_assert below, so
		// renaming an enumerator is free but MOVING one is not — and EquipKnife/EquipGun are named by
		// hand in other slices' files. The MEANING moves; the C++ spelling does not. The ConfigId and
		// the DisplayName are the two strings that actually reach a player and both are correct here.
		// The options page takes its label straight from this table (v29 §5 deleted the
		// `TraceOptionsBindingRowLabel` override that used to sit over it), so these three DisplayName
		// strings are the whole of what the keybind page shows.
		{ ETraceInputAction::EquipKnife,  TEXT("KnifeSlot"),  TEXT("KNIFE"),  &Default_EquipKnife,  &Default_None, ETraceInputStates::Match },
		{ ETraceInputAction::EquipGun,    TEXT("PistolSlot"), TEXT("PISTOL"), &Default_EquipPistol, &Default_None, ETraceInputStates::Match },
		// SPEC v14 §5. Same reasoning as the two rows above: the rebind list IS this table, so an
		// ability the player cannot see here is an ability they cannot rebind however well it is
		// wired in the controller. The ConfigIds are the two strings ATraceHUD and
		// UTraceAbilityInputRelay already search for by name — do not rename them.
		{ ETraceInputAction::Ability,          TEXT("Ability"),          TEXT("ABILITY"),           &Default_Ability,          &Default_None, ETraceInputStates::Match },
		{ ETraceInputAction::AbilitySecondary, TEXT("AbilitySecondary"), TEXT("ABILITY (SECONDARY)"), &Default_AbilitySecondary, &Default_None, ETraceInputStates::Match },
		// SPEC v16 §1, "R to reload". Same reasoning as every row above: the options screen's rebind
		// list IS this table walked in order, so an action missing from here is an action the player
		// cannot see or rebind however well it is wired up in the controller.
		{ ETraceInputAction::Reload,           TEXT("Reload"),           TEXT("RELOAD"),            &Default_Reload,           &Default_None, ETraceInputStates::Match },
		// SPEC v26 §1 — "Make parry and pull core two separate binds in the settings menu."
		//
		// THIS LINE IS WHAT PUTS THE PULL ON THE KEYBIND PAGE, and nothing else does. The options
		// screen's rebind list IS this table walked in order (FTraceOptionsMenu::RebuildRows), so an
		// action that is not here is an action the player cannot see or rebind however well it is
		// wired up in the controller. That is the same sentence the EquipKnife, Ability and Reload
		// rows above already carry, and it is the whole reason each of them exists.
		//
		// LAST IN THE LIST, hence last on the page, because ETraceInputAction is append-only (see the
		// enumerator's comment). PULL CORE sitting under RELOAD rather than next to PARRY is the price
		// of never renumbering the runtime table, and it is the right trade: the ordering is cosmetic,
		// a renumber is a live bug.
		//
		// `NotCarrying`, and spec v25 §2 is where that comes from: the pull is refused to the team that
		// dropped the Core and to the carrier by definition — you cannot pull what you are holding. That
		// is what lets it share a key with the PARRY (Carrying) and refuses to let it share one with FIRE
		// (also NotCarrying), which is the conflict check earning its keep in both directions.
		//
		// *** SPEC v31 §5 MOVED THIS ROW OFF F AND ONTO G, AND MOVED ITS ConfigId WITH IT. ***
		// "bind the F key to inspect" named the key for the knife flourish, and the two rows cannot
		// share it (both NotCarrying, so ActionsMayShareAKey says no — one press would fill a pull
		// ring and start a 3.2 s flourish). "PullCore" -> "PullCoreKey" is the fourth id migration in
		// this table and it works the way the other three did: a returning player's saved
		// `PullCore=F` line names an id this table no longer has, is DISCARDED by RefreshFromConfig,
		// and they land on the shipped G. The full argument, including the one-line revert, is on
		// Default_PullCore and on ETraceInputAction::Inspect.
		{ ETraceInputAction::PullCore,         TEXT("PullCoreKey"),      TEXT("PULL CORE"),         &Default_PullCore,         &Default_None, ETraceInputStates::NotCarrying },

		// *** SPEC v28 §10 — THE MELEE BIND, AND THE ONE THING NEITHER §3 NOR §10 COULD SHIP ALONE. ***
		//
		// §10 built the verb and could not bind it (this file is §3's); §3 vacated right mouse and could
		// not add the row (the verb is §10's). Both said so in their hand-off notes and both were right.
		// The integrator owns the seam, so the row lands here. Everything else was already in place: the
		// button is unclaimed, TraceMelee::HandleMeleeInput is the whole verb including §10's Core-pull
		// precedence, and ATracePlayerController now maps IA_Melee through the same KeyFor/MapButton path
		// as every other button, so it is rebindable on the settings page like anything else.
		//
		// `NotCarrying`, MEASURED AND NOT ASSUMED — see the enumerator's comment for the two gates
		// (UTraceWeaponComponent::CanSwing's IsCarrier() refusal and ATraceCore::CanPullNow). The
		// consequence that matters: melee may NOT share a key with FIRE or PULL CORE, and PULL CORE is the
		// one that would actually have hurt — the pull already rides this button under §10's precedence,
		// so a second PullCore bind on it would dispatch the same verb twice from one press.
		{ ETraceInputAction::Melee,            TEXT("Melee"),            TEXT("MELEE"),             &Default_Melee,            &Default_None, ETraceInputStates::NotCarrying },
		// *** SPEC v31 §1 — "2 is smg." THE THIRD WEAPON ROW, ON THE 2 KEY SINCE v31. ***
		//
		// LAST IN THE LIST, hence last on the page, for the reason the PULL CORE row above gives:
		// ETraceInputAction is append-only, and SMG sitting under MELEE rather than next to PISTOL is
		// the cosmetic price of never renumbering the runtime table.
		//
		// ITS ConfigId MOVED TOO ("EquipSmg" -> "SmgSlot"), which v29's version of this comment could
		// say was unnecessary and v31's cannot. v29 shipped this row on the 3 key under the id
		// "EquipSmg", so every player who has opened the options screen since now HAS an
		// `EquipSmg=Three` line and RefreshFromConfig would honour it — leaving the SMG on 3 while the
		// keybind page, the HUD and this table all said 2. The fresh id drops that line.
		{ ETraceInputAction::EquipSmg,         TEXT("SmgSlot"),          TEXT("SMG"),               &Default_EquipSmg,         &Default_None, ETraceInputStates::Match },

		// *** SPEC v31 §5 — THE KNIFE FLOURISH, ON F, AS A ROW ON THE KEYBIND PAGE. ***
		//
		// "Inspect (3.20 s) is a flourish — bind it to F, as a new rebindable action in the settings
		// page like every other action." THIS LINE IS THE "like every other action" HALF, and nothing
		// else is: FTraceOptionsMenu::RebuildRows walks this table in order, so a verb that is not
		// here is a verb the player can neither see nor rebind, however well it is wired up.
		//
		// LAST IN THE LIST, hence last on the page, for the reason every appended row above gives.
		//
		// ITS ConfigId HAS NEVER APPEARED IN ANYBODY'S FILE, so — unlike the PULL CORE row directly
		// above, which had to pay a migration to get OFF F — this one needs none. A new row's id is
		// safe exactly once, and this is that once.
		//
		// `NotCarrying`, and it is enforced at the verb rather than merely declared here:
		// TraceKnifeView::RequestInspect refuses a carrier outright, because the pack's loadout table
		// makes the Core a two-hand cradle. So this key is genuinely dead while carrying and may
		// legally share with a Carrying-only action (the parry, the throw) if a player wants that. It
		// may NOT share with FIRE, MELEE or PULL CORE, which is the answer we want.
		{ ETraceInputAction::Inspect,          TEXT("Inspect"),          TEXT("INSPECT KNIFE"),     &Default_Inspect,          &Default_None, ETraceInputStates::NotCarrying },
	};

	// 19 -> 20 for spec v31 §5's INSPECT row. This assert did exactly the job it was written for: the
	// enumerator was appended and the table was not, and the build stopped rather than shipping a
	// table one shorter than the index space that walks it.
	static_assert(static_cast<int32>(ETraceInputAction::Count) == 20,
		"ETraceInputAction and TraceInputActions::All() have drifted apart. Add the new action to the "
		"table above, give it a ConfigId that will never change, and bind it in ATracePlayerController.");

	return Table;
}

FString LexTraceInputStates(ETraceInputStates States)
{
	// Spelled out rather than printed as a number: every message this appears in is an explanation of
	// WHY two actions may or may not share a key, and "3" explains nothing to the person reading it.
	if (States == ETraceInputStates::None)
	{
		return TEXT("NONE");
	}

	TArray<FString> Parts;
	if (EnumHasAnyFlags(States, ETraceInputStates::Carrying))    { Parts.Add(TEXT("CARRYING")); }
	if (EnumHasAnyFlags(States, ETraceInputStates::NotCarrying)) { Parts.Add(TEXT("NOT CARRYING")); }
	if (EnumHasAnyFlags(States, ETraceInputStates::Menu))        { Parts.Add(TEXT("MENU")); }
	return FString::Join(Parts, TEXT("+"));
}

const FTraceInputActionInfo& TraceInputActions::Info(ETraceInputAction Action)
{
	const TArray<FTraceInputActionInfo>& Table = All();
	const int32 Index = FMath::Clamp(static_cast<int32>(Action), 0, Table.Num() - 1);
	return Table[Index];
}

// =================================================================================================
// Lifecycle
// =================================================================================================

UTraceUserSettings::UTraceUserSettings()
{
	// SPEC v28 §3c — MaxKeysPerAction slots per action, flat. See the Bindings declaration.
	Bindings.SetNum(static_cast<int32>(ETraceInputAction::Count) * MaxKeysPerAction);
}

UTraceUserSettings& UTraceUserSettings::Get()
{
	UTraceUserSettings* Settings = GetMutableDefault<UTraceUserSettings>();

	// The config system fills the CDO's `config` properties at class load, but KeyBindings is a flat
	// string list that still has to be parsed into FKeys. Doing it lazily here means every entry
	// point — controller, title screen, pause menu — gets a ready object without any of them having
	// to know about an init order.
	if (!Settings->bLoaded)
	{
		Settings->RefreshFromConfig();
	}

	return *Settings;
}

FTraceUserSettingsChanged& UTraceUserSettings::OnChanged()
{
	static FTraceUserSettingsChanged Delegate;
	return Delegate;
}

// =================================================================================================
// Mouse
// =================================================================================================

float UTraceUserSettings::GetLookScaleX() const
{
	// Clamped rather than trusted: this value can arrive from a hand-edited .ini, and a zero would
	// silently disable looking while a huge one would spin the player on the first mouse event.
	return FMath::Clamp(MouseSensitivity, MinSensitivity, MaxSensitivity);
}

float UTraceUserSettings::GetLookScaleY() const
{
	const float Scale = GetLookScaleX()
		* FMath::Clamp(MouseSensitivityYScale, MinSensitivityYScale, MaxSensitivityYScale);

	// The sign IS the inversion. Folding it into the Scalar modifier rather than adding or removing
	// a Negate modifier keeps the mapping's modifier list a fixed shape, so a live rebuild only ever
	// changes numbers.
	return bInvertMouseY ? -Scale : Scale;
}

// =================================================================================================
// SPEC v29 §3 — Crosshair
//
// NAMED namespace, not anonymous: this module is a unity/jumbo build and two files that each open an
// anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
// =================================================================================================

namespace TraceCrosshairPalette
{
	struct FEntry
	{
		const TCHAR* Name;
		FLinearColor Color;
	};

	/**
	 * EIGHT HUES, and the list is an argument rather than a colour wheel sampled at random.
	 *
	 * WHITE first because it is the shipped default and because it is the only entry that is legible
	 * against every surface in this arena without help. CYAN and ORANGE are the two team colours the
	 * HUD already uses (TraceOptionsStyle::Cyan / ::Amber) — a player who wants their crosshair to
	 * match the chrome should not have to eyeball it. LIME and MAGENTA are here for the case the
	 * arena is worst at: both sit far from the floor's black and far from the neon's cyan/amber, so
	 * they stay findable over a lit strip. RED is the convention and is kept even though it is the
	 * worst choice against this arena's amber, because a player who wants red wants red.
	 *
	 * *** RELEASE ART BIBLE §2.4 — THE DEFAULT IS THE INTERFACE'S OWN INK, NOT PAPER WHITE. ***
	 *
	 * The guard rail the bible states is a COLLISION rule: "the crosshair default must not be magenta
	 * while goal-slot markers are magenta ... leave the crosshair user-configurable with default Ink
	 * white". MAGENTA stays in the list (a player who wants it can still pick it — the same argument
	 * RED gets above); what changed is that entry 0 is now the exact `Ink` this game's interface is
	 * drawn in — sRGB-linear (0.90, 0.97, 1.00), the value carried by TraceMenuStyle::Ink and
	 * TraceOptionsStyle::Ink — rather than a flat (1,1,1) that belongs to no system.
	 *
	 * THE LITERALS AND NOT THE TOKEN, on purpose. Settings/ does not include UI/: this class is read
	 * by the HUD, the options page and the player controller, and making the player's own settings
	 * depend on a menu palette header would invert the layering for three floats. The numbers are
	 * repeated with their source named, which is the same trade the two UI copies of Ink already make
	 * with each other (see the palette note in UI/TraceOptionsMenu.cpp).
	 *
	 * MEASURED COST: 10% less red and 3% less green than the pre-bible (1,1,1) at the same alpha —
	 * below the threshold at which the reticle reads as tinted, and it is the same off-white every
	 * other piece of text on screen is set in. See the header's "shipped defaults" block, which
	 * records this as the one deliberate departure from pixel-identity.
	 *
	 * NO TEAM-COLOUR ENTRY. The crosshair already lifts toward the team colour when a pass target is
	 * under it (ATraceHUD::DrawCrosshair), and a base colour that also moved with the team would make
	 * that state change unreadable — the one thing on this reticle that MEANS something by its colour.
	 *
	 * Appending is free; INSERTING renumbers everybody's saved CrosshairColorIndex, so new entries go
	 * on the end. Same discipline as the action table above, for the same reason.
	 */
	static const FEntry* Table(int32& OutCount)
	{
		static const FEntry Entries[] =
		{
			// Release art bible §2.4 — the interface's Ink, not paper white. See the block above.
			{ TEXT("WHITE"),   FLinearColor(0.90f, 0.97f, 1.00f) },
			{ TEXT("CYAN"),    FLinearColor(0.16f, 0.88f, 1.00f) },
			{ TEXT("LIME"),    FLinearColor(0.55f, 1.00f, 0.15f) },
			{ TEXT("GREEN"),   FLinearColor(0.10f, 1.00f, 0.35f) },
			{ TEXT("AMBER"),   FLinearColor(1.00f, 0.72f, 0.10f) },
			{ TEXT("ORANGE"),  FLinearColor(1.00f, 0.46f, 0.08f) },
			{ TEXT("RED"),     FLinearColor(1.00f, 0.16f, 0.16f) },
			{ TEXT("MAGENTA"), FLinearColor(1.00f, 0.20f, 0.90f) },
		};

		OutCount = UE_ARRAY_COUNT(Entries);
		return Entries;
	}

	/**
	 * ONE definition of how many stops the COLOUR row has, asked of the same array Entry() clamps
	 * against. A row whose range outran its palette would print the last colour twice and read as a
	 * stuck control; one that fell short would make an entry unreachable.
	 */
	static int32 Count()
	{
		int32 N = 0;
		Table(N);
		return N;
	}

	static const FEntry& Entry(int32 Index)
	{
		int32 N = 0;
		const FEntry* Entries = Table(N);
		return Entries[FMath::Clamp(Index, 0, N - 1)];
	}

	/**
	 * *** THE OUTLINE'S ALPHA, AS A FRACTION OF THE CROSSHAIR'S OWN. ***
	 *
	 * 0.80 / 0.94 = 0.851, which is exactly the pair ATraceHUD::DrawAimReticle shipped before the
	 * crosshair was a setting — so a player on the defaults gets the identical surround they had.
	 *
	 * A FRACTION AND NOT THE 0.80 LITERAL, and this is the standing rule rather than a preference: the
	 * outline exists to separate the crosshair from the background, so its strength is a statement
	 * ABOUT the crosshair's strength. Pinned at 0.80, a player who slid opacity down to 25% would get
	 * a ghost of a cross wrapped in a fully present black box — the surround louder than the mark it
	 * surrounds, at the exact setting a player chooses because they want the crosshair to intrude
	 * less. Relative, it fades with the thing it outlines.
	 */
	static constexpr float OutlineAlphaFraction = 0.80f / 0.94f;
}

int32 UTraceUserSettings::NumCrosshairColors()
{
	return TraceCrosshairPalette::Count();
}

FLinearColor UTraceUserSettings::CrosshairPaletteColor(int32 Index)
{
	return TraceCrosshairPalette::Entry(Index).Color;
}

FString UTraceUserSettings::DescribeCrosshairColor(int32 Index)
{
	return FString(TraceCrosshairPalette::Entry(Index).Name);
}

float UTraceUserSettings::GetCrosshairSize() const
{
	return FMath::Clamp(CrosshairSize, MinCrosshairSize, MaxCrosshairSize);
}

float UTraceUserSettings::GetCrosshairThickness() const
{
	return FMath::Clamp(CrosshairThickness, MinCrosshairThickness, MaxCrosshairThickness);
}

float UTraceUserSettings::GetCrosshairGap() const
{
	return FMath::Clamp(CrosshairGap, MinCrosshairGap, MaxCrosshairGap);
}

float UTraceUserSettings::GetCrosshairOpacity() const
{
	return FMath::Clamp(CrosshairOpacity, MinCrosshairOpacity, MaxCrosshairOpacity);
}

FLinearColor UTraceUserSettings::GetCrosshairColor() const
{
	FLinearColor Color = CrosshairPaletteColor(CrosshairColorIndex);
	Color.A = GetCrosshairOpacity();
	return Color;
}

FLinearColor UTraceUserSettings::GetCrosshairOutlineColor() const
{
	if (!bCrosshairOutline)
	{
		// Fully transparent rather than "do not call me". The caller then has ONE code path that draws
		// a surround whose alpha happens to be zero, instead of two paths whose geometry could drift.
		return FLinearColor(0.f, 0.f, 0.f, 0.f);
	}

	return FLinearColor(0.f, 0.f, 0.f,
		GetCrosshairOpacity() * TraceCrosshairPalette::OutlineAlphaFraction);
}

float UTraceUserSettings::GetCrosshairArmReach() const
{
	return GetCrosshairGap() + GetCrosshairSize();
}

int32 UTraceUserSettings::BuildCrosshairBars(float CX, float CY, float PixelScale,
	FTraceCrosshairBar OutBars[TraceCrosshairMaxBars]) const
{
	// ---- Pixel snapping ------------------------------------------------------------------------
	//
	// Moved here verbatim from ATraceHUD::DrawAimReticle, whose note explains why every one of these
	// is an integer: at 1280x720 UIScale is 0.667, so an unsnapped arm is 1.33 px wide sitting on a
	// half-pixel boundary and Canvas anti-aliases it into a grey smudge. Integer rects at integer
	// coordinates land on exact pixels and receive no anti-aliasing at all.
	const float Scale = FMath::Max(0.01f, PixelScale);

	// *** THE FLOORS ARE 1 PX, NOT THE OLD 2 / 6 / 3, AND THAT IS NOT A REGRESSION. ***
	//
	// Those three numbers were written when the sizes were literals, to stop UIScale rounding from
	// reducing a fixed 11/2.5/5 reticle to a single invisible pixel on a small window. MEASURED at
	// every resolution the engine can produce — UIScale is clamped to 0.6..2.0 in ATraceHUD::DrawHUD
	// — none of them ever bit at the defaults: at UIScale 0.6 the three expressions give 2 / 7 / 3,
	// and the floors are 2 / 6 / 3. So keeping them would change nothing for a default player and
	// would silently overrule a player who deliberately asked for the minimum. A GAP row that reads
	// 0 and still draws three pixels of gap is the same class of defect as the toggle that could
	// only be turned on.
	//
	// What the floor still has to do is stop ROUNDING from destroying a shape the player asked for,
	// so it is 1 px on anything the player set above zero, and 0 only where they set 0.
	const float T   = FMath::Max(1.f, FMath::RoundToFloat(GetCrosshairThickness() * Scale));
	const float Arm = FMath::Max(1.f, FMath::RoundToFloat(GetCrosshairSize() * Scale));

	const float GapSetting = GetCrosshairGap();
	const float Gap = (GapSetting <= 0.f)
		? 0.f
		: FMath::Max(1.f, FMath::RoundToFloat(GapSetting * Scale));

	// Half a bar, floored, so the bar's own pixels straddle the centre symmetrically for odd T and
	// sit flush against it for even T. Both are exact; neither is a half-pixel.
	const float Half = FMath::FloorToFloat(T * 0.5f);

	const float X = FMath::RoundToFloat(CX);
	const float Y = FMath::RoundToFloat(CY);

	int32 Num = 0;
	OutBars[Num++] = { X - Gap - Arm, Y - Half,       Arm, T   };   // left
	OutBars[Num++] = { X + Gap,       Y - Half,       Arm, T   };   // right
	OutBars[Num++] = { X - Half,      Y - Gap - Arm,  T,   Arm };   // up
	OutBars[Num++] = { X - Half,      Y + Gap,        T,   Arm };   // down

	if (bCrosshairCenterDot)
	{
		// The exact aim point. Same bar width as the arms by construction — it is the same bar seen
		// end-on, and giving it its own size is how a centre dot ends up looking bolted on.
		OutBars[Num++] = { X - Half, Y - Half, T, T };
	}

	return Num;
}

void UTraceUserSettings::ResetCrosshairToDefaults()
{
	// DELIBERATELY NOT PART OF ResetToDefaults(), which is the CONTROLS page's reset. The video page
	// makes the same separation for the same reason (see EAction::ResetVideoDefaults): a player who
	// pressed a reset on the page about their crosshair did not ask to lose their key bindings, and
	// one row that quietly did both would be the most destructive control in this menu.
	CrosshairSize        = DefaultCrosshairSize;
	CrosshairThickness   = DefaultCrosshairThickness;
	CrosshairGap         = DefaultCrosshairGap;
	CrosshairColorIndex  = DefaultCrosshairColor;
	CrosshairOpacity     = DefaultCrosshairOpacity;
	bCrosshairCenterDot  = bDefaultCrosshairCenterDot;
	bCrosshairOutline    = bDefaultCrosshairOutline;

	Save();
}

// =================================================================================================
// UI PLAN WP2 — the call sign
// =================================================================================================

FString UTraceUserSettings::SanitizeCallSign(const FString& Raw)
{
	// Trim first, so "  ROXIE  " is a sixteen-character budget spent on five letters rather than on
	// nine spaces. ToUpper second, so the filter below only ever sees the case that will be stored.
	const FString Trimmed = Raw.TrimStartAndEnd().ToUpper();

	FString Clean;
	Clean.Reserve(FMath::Min(Trimmed.Len(), MaxCallSignLength));

	for (const TCHAR Char : Trimmed)
	{
		if (Clean.Len() >= MaxCallSignLength)
		{
			break;
		}

		// The WP2.3 alphabet, written here rather than asked of FTraceTextEntry: this class is the
		// STORAGE and must not depend on the UI to be correct — the .ini can be hand-edited with no
		// text field involved at all. The two lists are identical and each says so.
		const bool bLegal = FChar::IsAlnum(Char)
			|| Char == TEXT(' ') || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('.');
		if (bLegal)
		{
			Clean.AppendChar(Char);
		}
	}

	// Trimmed AGAIN, because the filter can strip the middle out of "A ~ B" and leave "A  B", and
	// because a name whose only surviving characters were spaces is not a name.
	Clean.TrimStartAndEndInline();
	return Clean;
}

FString UTraceUserSettings::GetCallSignOrDefault() const
{
	const FString Clean = SanitizeCallSign(CallSign);
	return Clean.IsEmpty() ? FString(DefaultCallSign) : Clean;
}

// =================================================================================================
// UI PLAN WP3 — player volume
// =================================================================================================

float UTraceUserSettings::GetAudioMasterVolume() const
{
	return FMath::Clamp(AudioMasterVolume, MinAudioVolume, MaxAudioVolume);
}

float UTraceUserSettings::GetAudioSfxVolume() const
{
	return FMath::Clamp(AudioSfxVolume, MinAudioVolume, MaxAudioVolume);
}

float UTraceUserSettings::GetAudioMusicVolume() const
{
	return FMath::Clamp(AudioMusicVolume, MinAudioVolume, MaxAudioVolume);
}

float UTraceUserSettings::GetUserGainForFamily(bool bIsMusic) const
{
	return GetAudioMasterVolume() * (bIsMusic ? GetAudioMusicVolume() : GetAudioSfxVolume());
}

void UTraceUserSettings::ResetAudioToDefaults()
{
	// The three faders and NOTHING else — not the crosshair, not the bindings, not the mouse. Third
	// instance of the rule the video and crosshair resets already state: a reset row belongs to the
	// page it is drawn on.
	AudioMasterVolume = DefaultAudioMasterVolume;
	AudioSfxVolume    = DefaultAudioSfxVolume;
	AudioMusicVolume  = DefaultAudioMusicVolume;

	Save();
}

bool UTraceUserSettings::IsAudioAtDefaults() const
{
	return FMath::IsNearlyEqual(AudioMasterVolume, DefaultAudioMasterVolume)
		&& FMath::IsNearlyEqual(AudioSfxVolume, DefaultAudioSfxVolume)
		&& FMath::IsNearlyEqual(AudioMusicVolume, DefaultAudioMusicVolume);
}

bool UTraceUserSettings::IsCrosshairAtDefaults() const
{
	return FMath::IsNearlyEqual(CrosshairSize, DefaultCrosshairSize)
		&& FMath::IsNearlyEqual(CrosshairThickness, DefaultCrosshairThickness)
		&& FMath::IsNearlyEqual(CrosshairGap, DefaultCrosshairGap)
		&& FMath::IsNearlyEqual(CrosshairOpacity, DefaultCrosshairOpacity)
		&& CrosshairColorIndex == DefaultCrosshairColor
		&& bCrosshairCenterDot == bDefaultCrosshairCenterDot
		&& bCrosshairOutline == bDefaultCrosshairOutline;
}

#if !UE_BUILD_SHIPPING
namespace TraceCrosshairConsole
{
	/**
	 * `Trace.Crosshair.Status` — prints what the crosshair actually IS, not what the .ini says.
	 *
	 * It reads through the clamped accessors and prints the derived arm reach, which is the number
	 * four other things in ATraceHUD are laid out against. That makes it the check for two claims a
	 * screenshot cannot make on its own: that the shipped defaults still produce the pre-v29 geometry
	 * (11 arm / 2.5 bar / 5 gap / 16 reach / 94% / dot on / outline on), and that a value which
	 * arrived from a hand-edited .ini was clamped rather than obeyed.
	 */
	void Status()
	{
		const UTraceUserSettings& S = UTraceUserSettings::Get();

		UE_LOG(LogTraceGame, Display,
			TEXT("[Crosshair] size=%.2f thickness=%.2f gap=%.2f reach=%.2f (1080p-reference px) ")
			TEXT("colour=%s(%d) opacity=%.2f dot=%s outline=%s(alpha %.3f) atDefaults=%s"),
			S.GetCrosshairSize(), S.GetCrosshairThickness(), S.GetCrosshairGap(), S.GetCrosshairArmReach(),
			*UTraceUserSettings::DescribeCrosshairColor(S.CrosshairColorIndex), S.CrosshairColorIndex,
			S.GetCrosshairOpacity(),
			S.bCrosshairCenterDot ? TEXT("ON") : TEXT("OFF"),
			S.bCrosshairOutline ? TEXT("ON") : TEXT("OFF"),
			S.GetCrosshairOutlineColor().A,
			S.IsCrosshairAtDefaults() ? TEXT("yes") : TEXT("no"));

		// RAW as well as clamped, because "the setting did nothing" and "the setting was out of range
		// and got clamped" look identical from a screenshot and need different fixes.
		UE_LOG(LogTraceGame, Display,
			TEXT("[Crosshair]   raw ini values: size=%.2f thickness=%.2f gap=%.2f opacity=%.2f -> %s"),
			S.CrosshairSize, S.CrosshairThickness, S.CrosshairGap, S.CrosshairOpacity,
			*S.GetClass()->GetConfigName());

		// RELEASE ART BIBLE §2.4 — the actual RGB, so "the default is not magenta" is a fact in the
		// log rather than an inference from a screenshot of a small white cross. At the shipped
		// default this prints entry 0 = the interface's Ink (0.900, 0.970, 1.000).
		const FLinearColor Ink = UTraceUserSettings::CrosshairPaletteColor(S.CrosshairColorIndex);
		UE_LOG(LogTraceGame, Display,
			TEXT("[Crosshair]   ink rgb = (%.3f, %.3f, %.3f)  default index = %d (%s)"),
			Ink.R, Ink.G, Ink.B, UTraceUserSettings::DefaultCrosshairColor,
			*UTraceUserSettings::DescribeCrosshairColor(UTraceUserSettings::DefaultCrosshairColor));
	}

	FAutoConsoleCommand CmdCrosshairStatus(
		TEXT("Trace.Crosshair.Status"),
		TEXT("Spec v29 s3. Prints the live crosshair geometry through the clamped accessors, plus the raw ")
		TEXT("values that came out of TraceUserSettings.ini and the derived arm reach the pass brackets ")
		TEXT("and both throw rings are laid out against."),
		FConsoleCommandDelegate::CreateStatic(&Status));
}

// =================================================================================================
// UI PLAN WP2 / WP3 — the headless hooks for the call sign and the three faders
//
// A headless run has no keyboard, so there is no way to TYPE a call sign and no way to DRAG a
// fader — and a setting nobody can drive from a script is a setting nobody can be shown to have
// checked. These are the same class of hole Trace.Menu.Settings and Trace.Menu.Crosshair fill for
// the pages themselves, and they are deliberately thin: each one writes through the SAME public
// entry points the settings rows use, so a run that passes here is evidence about the storage and
// the application path rather than about the harness.
//
// NAMED namespace, not anonymous: this module is a unity/jumbo build and two files that each open
// an anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
// =================================================================================================

namespace TraceUserSettingsConsole
{
	/** The local player's controller in whatever world is actually playing, or null on the title screen. */
	APlayerController* LocalController()
	{
		UWorld* const World = (GEngine != nullptr) ? GEngine->GetCurrentPlayWorld() : nullptr;
		return (World != nullptr) ? World->GetFirstPlayerController() : nullptr;
	}

	/** Whatever this machine currently calls the local player, for the before/after halves of a log line. */
	FString LiveName()
	{
		const APlayerController* const PC = LocalController();
		const APlayerState* const State = (PC != nullptr) ? PC->PlayerState : nullptr;
		return (State != nullptr) ? State->GetPlayerName() : FString(TEXT("<no player state>"));
	}

	void CallSignStatus()
	{
		const UTraceUserSettings& S = UTraceUserSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[CallSign] stored='%s'  effective='%s'  live player state='%s'  -> %s"),
			*S.CallSign, *S.GetCallSignOrDefault(), *LiveName(), *S.GetClass()->GetConfigName());
	}

	/**
	 * `Trace.CallSign.Set <name>` — write it, save it, and push it at the live match if there is one.
	 *
	 * The push goes through APlayerController::ServerChangeName, which is the engine's own rename
	 * path (-> AGameModeBase::ChangeName -> APlayerState::SetPlayerName, replicated). NOT a direct
	 * SetPlayerName on the local state: that would be a client lying to itself, and the thing this
	 * command exists to prove is precisely that the name reaches every machine's PlayerState.
	 */
	void CallSignSet(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[CallSign] Trace.CallSign.Set <name>"));
			return;
		}

		// Rejoined with spaces: a call sign may legally contain them (WP2.3) and the console splits
		// on whitespace, so "Trace.CallSign.Set MAIN CHARACTER" must not arrive as one word.
		const FString Requested = FString::Join(Args, TEXT(" "));

		UTraceUserSettings& S = UTraceUserSettings::Get();
		const FString Before = LiveName();

		S.CallSign = UTraceUserSettings::SanitizeCallSign(Requested);
		S.Save();

		const FString Effective = S.GetCallSignOrDefault();

		if (APlayerController* const PC = LocalController())
		{
			PC->ServerChangeName(Effective);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[CallSign] requested='%s' -> stored='%s' -> effective='%s'. Player state was '%s'; ")
			TEXT("ServerChangeName %s."),
			*Requested, *S.CallSign, *Effective, *Before,
			(LocalController() != nullptr) ? TEXT("sent") : TEXT("SKIPPED (no local controller — title screen)"));
	}

	/**
	 * `Trace.Audio.UserGain` — the three faders and the gain they produce, named.
	 *
	 * Trace.Audio.Loudness already REPORTS the user gain (it reads VolumeFor, which these multiply
	 * into), but it reports it folded into one number per event. This prints the three terms, so a
	 * before/after pair of runs says WHICH fader moved rather than only that something did.
	 */
	void AudioUserGain()
	{
		const UTraceUserSettings& S = UTraceUserSettings::Get();
		UE_LOG(LogTraceGame, Display,
			TEXT("[UserGain] master=%.3f  sfx=%.3f  music=%.3f  -> effects x%.4f, music x%.4f  ")
			TEXT("atDefaults=%s  raw ini: %.3f/%.3f/%.3f -> %s"),
			S.GetAudioMasterVolume(), S.GetAudioSfxVolume(), S.GetAudioMusicVolume(),
			S.GetUserGainForFamily(/*bIsMusic=*/false), S.GetUserGainForFamily(/*bIsMusic=*/true),
			S.IsAudioAtDefaults() ? TEXT("yes") : TEXT("no"),
			S.AudioMasterVolume, S.AudioSfxVolume, S.AudioMusicVolume,
			*S.GetClass()->GetConfigName());
	}

	/**
	 * `Trace.Audio.SetUserGain <master> [sfx] [music]` — drive the three faders from a script.
	 *
	 * Writes through the same fields the sliders write and calls the same Save(), so "settings
	 * survive relaunch" is provable without a human touching a mouse. Omitted arguments are left
	 * alone rather than defaulted, so a run can move ONE fader and prove the other two did not move.
	 */
	void AudioSetUserGain(const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[UserGain] Trace.Audio.SetUserGain <master> [sfx] [music]   (0..1 each)"));
			return;
		}

		UTraceUserSettings& S = UTraceUserSettings::Get();
		S.AudioMasterVolume = FCString::Atof(*Args[0]);
		if (Args.Num() > 1) { S.AudioSfxVolume   = FCString::Atof(*Args[1]); }
		if (Args.Num() > 2) { S.AudioMusicVolume = FCString::Atof(*Args[2]); }
		S.Save();

		AudioUserGain();
	}

	FAutoConsoleCommand CmdCallSignStatus(
		TEXT("Trace.CallSign.Status"),
		TEXT("UI plan WP2. Prints the stored call sign, the sanitised name the game will use, and what ")
		TEXT("the local APlayerState is actually called right now."),
		FConsoleCommandDelegate::CreateStatic(&CallSignStatus));

	FAutoConsoleCommand CmdCallSignSet(
		TEXT("Trace.CallSign.Set"),
		TEXT("UI plan WP2. Trace.CallSign.Set <name>. Sanitises, stores, saves and pushes it at the live ")
		TEXT("match through ServerChangeName - the same path the settings row's submit takes. The only ")
		TEXT("way a headless run can set a name, since the row needs a keyboard."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&CallSignSet));

	FAutoConsoleCommand CmdAudioUserGain(
		TEXT("Trace.Audio.UserGain"),
		TEXT("UI plan WP3. Prints the player's three volume faders and the gain each family gets from ")
		TEXT("them - the three terms Trace.Audio.Loudness reports folded together."),
		FConsoleCommandDelegate::CreateStatic(&AudioUserGain));

	FAutoConsoleCommand CmdAudioSetUserGain(
		TEXT("Trace.Audio.SetUserGain"),
		TEXT("UI plan WP3. Trace.Audio.SetUserGain <master> [sfx] [music]. Writes the faders through the ")
		TEXT("same fields and the same Save() the sliders use, so a headless run can prove Loudness ")
		TEXT("scales and that the values survive a relaunch."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&AudioSetUserGain));
}
#endif

// =================================================================================================
// Bindings
// =================================================================================================

bool UTraceUserSettings::IsBindableKey(const FKey& Key)
{
	if (!Key.IsValid())
	{
		return false;
	}

	// SPEC v10 §8 — "Allow Mouse button 1 and mouse button 2 as keybinds in the settings menu."
	//
	// MOUSE BUTTONS ARE BINDABLE AND ALWAYS HAVE BEEN, and this comment exists so the next person to
	// read the note does not "fix" it here a second time. The shipped defaults are LeftMouseButton
	// for FIRE and — since spec v25 §7 — RightMouseButton for PARRY (see the table at the top of
	// this file; it was PASS until v25 moved the throw off the button), so a rule that rejected
	// mouse buttons would have refused to load the game's own default bindings on the first run —
	// which is not what happens. Trace.VerifyBindableKeys prints the verdict for every mouse button
	// in the build so this can be checked rather than argued about.
	//
	// AXES ARE THE ONLY MOUSE INPUT REFUSED, and that is a different thing entirely. MouseX / MouseY
	// / Mouse2D / MouseWheelAxis are what the Look mapping consumes; binding Dash to "MouseX" would
	// fire it continuously for as long as the player looked around. MouseScrollUp and MouseScrollDown
	// are BUTTONS in UE's model, not axes, so a scroll click stays bindable — which is what a player
	// who wants dash on the wheel expects.
	//
	// Gestures and touch are not reachable on this platform. Everything else — keyboard, mouse
	// buttons, wheel clicks, gamepad face buttons — is fair game.
	if (Key.IsAxis1D() || Key.IsAxis2D() || Key.IsAxis3D() || Key.IsGesture() || Key.IsTouch())
	{
		return false;
	}

	// Escape is the universal "get me out of here" in this menu and must never become a game bind,
	// or the player can lock themselves out of the settings screen with the settings screen.
	if (Key == EKeys::Escape)
	{
		return false;
	}

	// EKeys::AnyKey is a real, valid, non-axis FKey that fires for EVERY key press. Measured: the
	// rebind capture walks this list and matches it before it ever reaches the key the player
	// actually pressed, so the first rebind produced "MOVE FORWARD -> ANY KEY" and every subsequent
	// key press moved the player forward. It is a wildcard, not a button.
	if (Key == EKeys::AnyKey)
	{
		return false;
	}

	return true;
}

FString UTraceUserSettings::DescribeKey(const FKey& Key)
{
	if (!Key.IsValid())
	{
		return TEXT("UNBOUND");
	}

	// GetDisplayName gives "Space Bar" / "Left Shift" / "Left Mouse Button", which is what a player
	// recognises; the internal FName would give "SpaceBar" and "LeftMouseButton".
	return Key.GetDisplayName().ToString().ToUpper();
}

int32 UTraceUserSettings::SlotIndex(ETraceInputAction Action, int32 Slot)
{
	const int32 ActionIndex = static_cast<int32>(Action);
	if (ActionIndex < 0 || ActionIndex >= static_cast<int32>(ETraceInputAction::Count)
		|| Slot < 0 || Slot >= MaxKeysPerAction)
	{
		return INDEX_NONE;
	}
	return ActionIndex * MaxKeysPerAction + Slot;
}

bool UTraceUserSettings::ActionsMayShareAKey(ETraceInputAction A, ETraceInputAction B)
{
	// SPEC v28 §3b. The whole conflict rule, in one bitwise AND. See ETraceInputStates for why this is
	// a STATE question and not a key question, and for the owner's own example (throw core vs fire).
	//
	// An action never "shares" with itself: the caller is asking "may B keep this key while A takes
	// it", and A == B is the slot logic's business (SetKey clears a duplicate in the other slot).
	if (A == B)
	{
		return false;
	}

	const ETraceInputStates StatesA = TraceInputActions::Info(A).States;
	const ETraceInputStates StatesB = TraceInputActions::Info(B).States;

	// EnumHasAnyFlags on the intersection: no overlap means no instant at which both are legal.
	return !EnumHasAnyFlags(StatesA, StatesB);
}

FKey UTraceUserSettings::GetKey(ETraceInputAction Action) const
{
	return GetKey(Action, 0);
}

FKey UTraceUserSettings::GetKey(ETraceInputAction Action, int32 Slot) const
{
	const int32 Index = SlotIndex(Action, Slot);
	return Bindings.IsValidIndex(Index) ? Bindings[Index] : FKey();
}

void UTraceUserSettings::GetKeys(ETraceInputAction Action, TArray<FKey>& OutKeys) const
{
	OutKeys.Reset();
	for (int32 Slot = 0; Slot < MaxKeysPerAction; ++Slot)
	{
		const FKey Key = GetKey(Action, Slot);
		if (Key.IsValid())
		{
			OutKeys.Add(Key);
		}
	}
}

bool UTraceUserSettings::ActionUsesKey(ETraceInputAction Action, const FKey& Key) const
{
	// An invalid key matches nothing, on purpose: two UNBOUND slots would otherwise compare equal and
	// every "is this action's button down" caller would answer yes for a player who unbound it.
	if (!Key.IsValid())
	{
		return false;
	}

	for (int32 Slot = 0; Slot < MaxKeysPerAction; ++Slot)
	{
		if (GetKey(Action, Slot) == Key)
		{
			return true;
		}
	}
	return false;
}

FString UTraceUserSettings::DescribeBinding(ETraceInputAction Action) const
{
	TArray<FKey> Keys;
	GetKeys(Action, Keys);
	if (Keys.Num() == 0)
	{
		return TEXT("UNBOUND");
	}

	FString Out;
	for (const FKey& Key : Keys)
	{
		if (!Out.IsEmpty())
		{
			Out += TEXT("  /  ");
		}
		Out += DescribeKey(Key);
	}
	return Out;
}

/**
 * SPEC v28 §3b — the A/B arm for the state-aware conflict check.
 *
 * 1 restores the behaviour exactly as it shipped before v28: EVERY other action loses the key,
 * whether or not the two could ever be legal at the same instant. That is the RED arm — it is the
 * reproduction of "it will not let me bind throw core to the same button as fire" — and it exists
 * because this project's standing rule is that a harness which cannot go red is not evidence.
 *
 * *** ECVF_Cheat SINCE W9-SHIPGUARD, AND THE PARAGRAPH THAT USED TO BE HERE HAD THE COST WRONG. ***
 * It read "Not ECVF_Cheat: it changes no gameplay rule, only which of the player's own binds survive
 * an edit, and a playtester who preferred the old behaviour should be able to have it back". The
 * first half is still true and is why this arm is harmless; the second half assumed the flag takes
 * the switch away from a playtester, and it does not. ECVF_Cheat is INERT wherever
 * DISABLE_CHEAT_CVARS is 0 — that is every configuration except Shipping and Test — so the console,
 * -ExecCmds and ConsoleVariables.ini [Startup] all still reach it on any build a playtester runs.
 * The ONE thing the flag closes is the last injection path into a SHIPPED build:
 * FConfigCacheIni::LoadConsoleVariablesFromINI applies Engine.ini's [ConsoleVariables] section with
 * bAllowCheating = false in every configuration, and in a packaged game that ini is player-writable.
 * A switch whose own help text calls one of its values "the RED arm" does not belong there, and the
 * consistency matters more than the individual verdict: W8-BATTERY found twenty such arms shipping
 * unflagged next to siblings that were flagged, and a rule with twenty exceptions is not a rule.
 */
static int32 GTraceKeysLegacySteal = 0;
static FAutoConsoleVariableRef CVarTraceKeysLegacySteal(
	TEXT("Trace.Keys.LegacySteal"),
	GTraceKeysLegacySteal,
	TEXT("Spec v28 sec 3b. 0 (default): a key is taken from another action ONLY when the two can both "
	     "be legal at the same instant, so throw-core and fire may share a button. 1 is the RED arm - "
	     "the pre-v28 rule where any other action holding the key loses it unconditionally."),
	ECVF_Cheat);

void UTraceUserSettings::SetKey(ETraceInputAction Action, const FKey& Key)
{
	SetKey(Action, 0, Key);
}

void UTraceUserSettings::SetKey(ETraceInputAction Action, int32 Slot, const FKey& Key)
{
	const int32 Index = SlotIndex(Action, Slot);
	if (!Bindings.IsValidIndex(Index) || !IsBindableKey(Key))
	{
		return;
	}

	// ---- The same action's OTHER slot ----------------------------------------------------------
	//
	// One action holding one key twice is not a second bind, it is a wasted row: the options page would
	// draw the same chip in both columns and Enhanced Input would map the key to the action twice. So a
	// duplicate inside this action is always cleared, whatever the exclusion groups say.
	for (int32 Other = 0; Other < MaxKeysPerAction; ++Other)
	{
		const int32 OtherIndex = SlotIndex(Action, Other);
		if (Other != Slot && Bindings.IsValidIndex(OtherIndex) && Bindings[OtherIndex] == Key)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[Settings] %s already had '%s' in slot %d; moving it to slot %d rather than holding it twice."),
				TraceInputActions::Info(Action).DisplayName, *DescribeKey(Key), Other + 1, Slot + 1);
			Bindings[OtherIndex] = FKey();
		}
	}

	// ---- SPEC v28 §3b — take the key ONLY from an action that genuinely conflicts ----------------
	//
	// This loop used to be unconditional, and unconditional stealing IS the reported defect: a player
	// who put THROW CORE on mouse 1 watched FIRE go blank, which reads exactly as "it will not let me
	// bind throw core to the same button as fire". Now the exclusion groups decide, and two verbs that
	// can never both be legal keep the button between them.
	for (const FTraceInputActionInfo& Info : TraceInputActions::All())
	{
		if (Info.Action == Action)
		{
			continue;
		}

		for (int32 OtherSlot = 0; OtherSlot < MaxKeysPerAction; ++OtherSlot)
		{
			const int32 OtherIndex = SlotIndex(Info.Action, OtherSlot);
			if (!Bindings.IsValidIndex(OtherIndex) || Bindings[OtherIndex] != Key)
			{
				continue;
			}

			if (GTraceKeysLegacySteal == 0 && ActionsMayShareAKey(Action, Info.Action))
			{
				// Logged at Display and not silently, because "two actions are on one key" is a thing a
				// player should be able to confirm from the log when they meant to do it — and the thing
				// somebody debugging a double-fire will look for first.
				UE_LOG(LogTraceGame, Display,
					TEXT("[Settings] '%s' is now SHARED by %s and %s. Allowed: their states are exclusive ")
					TEXT("(%s vs %s), so at most one of them can ever accept the press."),
					*DescribeKey(Key), TraceInputActions::Info(Action).DisplayName, Info.DisplayName,
					*LexTraceInputStates(TraceInputActions::Info(Action).States),
					*LexTraceInputStates(Info.States));
				continue;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[Settings] '%s' taken from %s and given to %s — they can both be legal at once (%s vs %s)."),
				*DescribeKey(Key), Info.DisplayName, TraceInputActions::Info(Action).DisplayName,
				*LexTraceInputStates(Info.States),
				*LexTraceInputStates(TraceInputActions::Info(Action).States));
			Bindings[OtherIndex] = FKey();
		}
	}

	Bindings[Index] = Key;
	Save();
}

void UTraceUserSettings::ClearKey(ETraceInputAction Action)
{
	bool bChanged = false;
	for (int32 Slot = 0; Slot < MaxKeysPerAction; ++Slot)
	{
		const int32 Index = SlotIndex(Action, Slot);
		if (Bindings.IsValidIndex(Index) && Bindings[Index].IsValid())
		{
			Bindings[Index] = FKey();
			bChanged = true;
		}
	}

	if (!bChanged)
	{
		return;
	}

	UE_LOG(LogTraceGame, Display, TEXT("[Settings] %s unbound (every slot)."),
		TraceInputActions::Info(Action).DisplayName);
	Save();
}

void UTraceUserSettings::ClearKey(ETraceInputAction Action, int32 Slot)
{
	const int32 Index = SlotIndex(Action, Slot);
	if (!Bindings.IsValidIndex(Index) || !Bindings[Index].IsValid())
	{
		return;
	}

	UE_LOG(LogTraceGame, Display, TEXT("[Settings] %s slot %d unbound (was '%s')."),
		TraceInputActions::Info(Action).DisplayName, Slot + 1, *DescribeKey(Bindings[Index]));
	Bindings[Index] = FKey();
	Save();
}

void UTraceUserSettings::RefreshFromConfig()
{
	const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();

	Bindings.Reset();
	Bindings.SetNum(Table.Num() * MaxKeysPerAction);

	// Start from the shipped defaults, then let the .ini override entry by entry. A truncated or
	// partially corrupt file therefore degrades to "some defaults" rather than to "no controls".
	for (int32 Index = 0; Index < Table.Num(); ++Index)
	{
		const ETraceInputAction Action = static_cast<ETraceInputAction>(Index);
		Bindings[SlotIndex(Action, 0)] = Table[Index].DefaultKey();
		// SPEC v28 §3c/§3d — the shipped SECOND bind. Only the parry row has one today.
		Bindings[SlotIndex(Action, 1)] = (Table[Index].DefaultKeyAlt != nullptr)
			? Table[Index].DefaultKeyAlt() : FKey();
	}

	for (const FString& Entry : KeyBindings)
	{
		FString ConfigId;
		FString KeyList;
		if (!Entry.Split(TEXT("="), &ConfigId, &KeyList))
		{
			continue;
		}

		ConfigId.TrimStartAndEndInline();
		KeyList.TrimStartAndEndInline();

		const int32 Index = Table.IndexOfByPredicate(
			[&ConfigId](const FTraceInputActionInfo& Info) { return ConfigId.Equals(Info.ConfigId, ESearchCase::IgnoreCase); });

		if (Index == INDEX_NONE)
		{
			// An action that no longer exists. Dropping it silently is correct: the alternative is
			// refusing to load a file that a previous build wrote perfectly legitimately.
			continue;
		}

		const ETraceInputAction Action = static_cast<ETraceInputAction>(Index);

		// SPEC v28 §3c — the value is a comma-separated slot list now. A line from ANY older build has
		// no comma and therefore produces exactly one element, which lands in slot 0 — which is what
		// that build meant — and slot 1 is then cleared below, because the FILE is authoritative for
		// every slot of an action it names. That last clause is what stops a returning player picking up
		// a brand-new second default on a key they had deliberately changed.
		TArray<FString> KeyNames;
		KeyList.ParseIntoArray(KeyNames, TEXT(","), /*InCullEmpty=*/false);

		if (KeyNames.Num() > MaxKeysPerAction)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Settings] '%s' names %d keys; this build binds at most %d per action. The extras are ignored."),
				*ConfigId, KeyNames.Num(), MaxKeysPerAction);
			KeyNames.SetNum(MaxKeysPerAction);
		}

		for (int32 Slot = 0; Slot < MaxKeysPerAction; ++Slot)
		{
			const int32 Flat = SlotIndex(Action, Slot);

			if (!KeyNames.IsValidIndex(Slot))
			{
				// The file names this action but not this slot: an older one-key line, or a two-key line
				// this build shortened. Either way the player has no key there.
				Bindings[Flat] = FKey();
				continue;
			}

			FString KeyName = KeyNames[Slot];
			KeyName.TrimStartAndEndInline();

			// The explicit unbound marker, which is what a stolen key leaves behind.
			if (KeyName.IsEmpty() || KeyName.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				Bindings[Flat] = FKey();
				continue;
			}

			const FKey Key(*KeyName);
			if (IsBindableKey(Key))
			{
				Bindings[Flat] = Key;
			}
			else
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[Settings] Ignoring '%s' for action '%s' slot %d: not a bindable key on this platform. ")
					TEXT("Keeping the default."),
					*KeyName, *ConfigId, Slot + 1);
			}
		}
	}

	bLoaded = true;
}

void UTraceUserSettings::FlattenToConfig()
{
	const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();

	KeyBindings.Reset(Table.Num());
	for (int32 Index = 0; Index < Table.Num(); ++Index)
	{
		const ETraceInputAction Action = static_cast<ETraceInputAction>(Index);

		// FKey::GetFName() is the stable serialisation name ("SpaceBar"), NOT the display name.
		// Writing the display name would produce a file that cannot be read back.
		//
		// SPEC v28 §3c: every slot is written, "None" included, so the file is a complete statement of
		// the player's intent. Writing only the occupied slots would make "I deliberately cleared my
		// second parry key" indistinguishable from "this build did not know about slot 2", and the next
		// launch would hand the thumb button back.
		FString Value;
		for (int32 Slot = 0; Slot < MaxKeysPerAction; ++Slot)
		{
			const FKey Key = GetKey(Action, Slot);
			if (Slot > 0)
			{
				Value += TEXT(",");
			}
			Value += Key.IsValid() ? Key.GetFName().ToString() : FString(TEXT("None"));
		}

		KeyBindings.Add(FString::Printf(TEXT("%s=%s"), Table[Index].ConfigId, *Value));
	}
}

void UTraceUserSettings::ResetToDefaults()
{
	MouseSensitivity = DefaultSensitivity;
	MouseSensitivityYScale = DefaultSensitivityYScale;
	bInvertMouseY = false;

	KeyBindings.Reset();
	RefreshFromConfig();      // repopulates Bindings straight from the defaults table
	Save();
}

bool UTraceUserSettings::IsAtDefaults() const
{
	if (!FMath::IsNearlyEqual(MouseSensitivity, DefaultSensitivity)
		|| !FMath::IsNearlyEqual(MouseSensitivityYScale, DefaultSensitivityYScale)
		|| bInvertMouseY)
	{
		return false;
	}

	const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();
	for (int32 Index = 0; Index < Table.Num(); ++Index)
	{
		const ETraceInputAction Action = static_cast<ETraceInputAction>(Index);

		// BOTH slots, or the RESET row would dim itself for a player who had only changed their second
		// parry key — the one row on this page whose whole job is to say "you have changed something".
		if (GetKey(Action, 0) != Table[Index].DefaultKey())
		{
			return false;
		}

		const FKey DefaultAlt = (Table[Index].DefaultKeyAlt != nullptr) ? Table[Index].DefaultKeyAlt() : FKey();
		if (GetKey(Action, 1) != DefaultAlt)
		{
			return false;
		}
	}

	return true;
}

void UTraceUserSettings::Save()
{
	FlattenToConfig();

	// SaveConfig writes into GConfig's in-memory copy of the file. Without the explicit Flush the
	// values only reach disk at a clean shutdown — and this build is normally ended with pkill, so
	// "clean shutdown" is the case that does not happen.
	SaveConfig();
	if (GConfig != nullptr)
	{
		GConfig->Flush(false, GetClass()->GetConfigName());
	}

	// One listener today (ATracePlayerController) but broadcast unconditionally: the title screen
	// has no controller at all, and a future one must not have to be remembered here.
	OnChanged().Broadcast();

	UE_LOG(LogTraceGame, Display,
		TEXT("[Settings] Saved. sensitivity=%.2f yScale=%.2f invertY=%d -> %s"),
		MouseSensitivity, MouseSensitivityYScale, bInvertMouseY ? 1 : 0,
		*GetClass()->GetConfigName());
}

#if !UE_BUILD_SHIPPING

// =================================================================================================
// Shared verification plumbing
//
// NAMED namespace, not anonymous: this module is compiled as a unity/jumbo build and two files that
// each open an anonymous namespace become one namespace with two definitions.
// Scripts/check-jumbo-build-collisions.py gates the build on exactly that.
// =================================================================================================

namespace TraceUserSettingsVerify
{
	/**
	 * SPEC v28 §3c — splits an .ini VALUE ("Q,ThumbMouseButton", "SpaceBar", "None") into slot names.
	 *
	 * One place, used by the loader's twin below and by both verification commands, so a check can
	 * never disagree with RefreshFromConfig about what a line MEANS. Empties are kept, because
	 * "LeftMouseButton," is a real two-slot statement whose second slot is empty.
	 */
	void SplitSlots(const FString& Value, TArray<FString>& OutNames)
	{
		OutNames.Reset();
		Value.ParseIntoArray(OutNames, TEXT(","), /*InCullEmpty=*/false);
		for (FString& Name : OutNames)
		{
			Name.TrimStartAndEndInline();
		}
	}

	/** Every slot of every action, flat, exactly as UTraceUserSettings stores them. */
	void Snapshot(const UTraceUserSettings& Settings, TArray<FKey>& OutKeys)
	{
		OutKeys.Reset();
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			for (int32 Slot = 0; Slot < UTraceUserSettings::MaxKeysPerAction; ++Slot)
			{
				OutKeys.Add(Settings.GetKey(Info.Action, Slot));
			}
		}
	}

	/**
	 * Puts a Snapshot back, EVERY SLOT, and clears the whole table first.
	 *
	 * The clear is not tidiness. Restoring slot by slot on top of whatever the diagnostic left behind
	 * means SetKey can be asked to give action A a key that action B is still holding from the test,
	 * and if those two genuinely conflict it will take it back off B — undoing a restore that had
	 * already happened. Clearing first makes the restore order irrelevant.
	 *
	 * ClearKey rather than SetKey for the empty slots, because SetKey REFUSES an invalid key by design
	 * (an unparseable .ini line must never be able to wipe a binding), so a legitimately UNBOUND slot
	 * — the shipped state of the throw row, and of slot 2 on sixteen of the seventeen actions — could
	 * not be restored through SetKey at all.
	 */
	void Restore(UTraceUserSettings& Settings, const TArray<FKey>& Keys)
	{
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			Settings.ClearKey(Info.Action);
		}

		int32 Flat = 0;
		for (const FTraceInputActionInfo& Info : TraceInputActions::All())
		{
			for (int32 Slot = 0; Slot < UTraceUserSettings::MaxKeysPerAction; ++Slot, ++Flat)
			{
				if (Keys.IsValidIndex(Flat) && Keys[Flat].IsValid())
				{
					Settings.SetKey(Info.Action, Slot, Keys[Flat]);
				}
			}
		}
	}
}

// =================================================================================================
// Trace.VerifyBindableKeys — SPEC v10 §8, the evidence half.
//
// The note is "allow mouse button 1 and mouse button 2 as keybinds", i.e. a report that the rebind
// UI refuses them. There are exactly two places a key can be refused between a player pressing it
// and it becoming a binding:
//
//   1. VALIDATION — UTraceUserSettings::IsBindableKey, which is what builds the options menu's
//      capture list and what SetKey and RefreshFromConfig both gate on. If a key fails here it can
//      never be captured, never be loaded from the ini and never be set programmatically.
//   2. DELIVERY — whether the press reaches APlayerController::WasInputKeyJustPressed at all while
//      the options overlay is up, which is an input-routing question, not a settings one.
//
// This command answers (1) exhaustively and by name, so (2) is what is left over. It walks every key
// the engine knows, prints the verdict for the mouse set specifically, and then does a full
// round trip on LeftMouseButton and RightMouseButton — bind it, read it back, flatten it to the
// string the ini stores, re-parse it — because "IsBindableKey says yes" and "the binding survives a
// save and a load" are two different claims and the second is the one a player experiences.
//
// The bindings it touches are restored before it returns, so it is safe to run mid-session.
// =================================================================================================

namespace
{
	void VerifyBindableKeys()
	{
		UTraceUserSettings& Settings = UTraceUserSettings::Get();

		UE_LOG(LogTraceGame, Display,
			TEXT("[BindableKeys] ===== spec v10 s8: which keys the rebind UI is allowed to capture ====="));

		// --- The mouse set, named one by one -----------------------------------------------------
		//
		// Listed explicitly rather than filtered out of GetAllKeys by IsMouseButton(), because the
		// question being answered is "is THIS key, the one in the note, bindable" and a filter that
		// silently returned an empty list would read as a pass.
		const TArray<FKey> MouseKeys =
		{
			EKeys::LeftMouseButton, EKeys::RightMouseButton, EKeys::MiddleMouseButton,
			EKeys::ThumbMouseButton, EKeys::ThumbMouseButton2,
			EKeys::MouseScrollUp, EKeys::MouseScrollDown,
			EKeys::MouseX, EKeys::MouseY, EKeys::MouseWheelAxis
		};

		int32 MouseButtonsBindable = 0;
		for (const FKey& Key : MouseKeys)
		{
			const bool bBindable = UTraceUserSettings::IsBindableKey(Key);
			const bool bAxis = Key.IsAxis1D() || Key.IsAxis2D() || Key.IsAxis3D();

			if (bBindable && !bAxis)
			{
				++MouseButtonsBindable;
			}

			UE_LOG(LogTraceGame, Display,
				TEXT("[BindableKeys]   %-20s bindable=%d  axis=%d  (%s)"),
				*Key.GetFName().ToString(), bBindable ? 1 : 0, bAxis ? 1 : 0,
				*UTraceUserSettings::DescribeKey(Key));
		}

		// --- The exclusions that must SURVIVE ------------------------------------------------------
		const bool bEscapeExcluded = !UTraceUserSettings::IsBindableKey(EKeys::Escape);
		const bool bAnyKeyExcluded = !UTraceUserSettings::IsBindableKey(EKeys::AnyKey);

		UE_LOG(LogTraceGame, Display,
			TEXT("[BindableKeys]   exclusions kept: Escape=%d AnyKey=%d (both must be 1)"),
			bEscapeExcluded ? 1 : 0, bAnyKeyExcluded ? 1 : 0);

		// --- The whole capture list, counted ------------------------------------------------------
		TArray<FKey> AllKeys;
		EKeys::GetAllKeys(AllKeys);

		int32 Capturable = 0;
		int32 CapturableMouse = 0;
		for (const FKey& Key : AllKeys)
		{
			if (!UTraceUserSettings::IsBindableKey(Key))
			{
				continue;
			}
			++Capturable;
			CapturableMouse += Key.IsMouseButton() ? 1 : 0;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[BindableKeys]   the options menu's capture list is %d of %d engine keys, %d of them mouse buttons."),
			Capturable, AllKeys.Num(), CapturableMouse);

		// --- The round trip -----------------------------------------------------------------------
		//
		// Dash is the victim on purpose: it is not one of the two actions that already OWN a mouse
		// button, so a pass here cannot be an accident of the defaults.
		//
		// SPEC v25 §7 MOVED WHICH ACTION THE RIGHT BUTTON BELONGS TO — it was PASS, it is now PARRY —
		// so the snapshot below is taken over the WHOLE table rather than over a hand-written list of
		// the two victims. A list of names is exactly what went stale here, and a restore that misses
		// an action leaves the player's controls damaged by a diagnostic (Trace.V10.RebindFire learned
		// the same lesson the same way; see its comment).
		//
		// SPEC v28 §3c: the snapshot covers EVERY SLOT, not just the primary. An action now has two, and a
		// restore that put back only the first would have quietly deleted the player's second parry key
		// every time somebody ran a diagnostic — which is the same class of damage the paragraph above is
		// about, one feature later.
		TArray<FKey> Before;
		TraceUserSettingsVerify::Snapshot(Settings, Before);

		int32 RoundTripsOk = 0;
		for (const FKey& Key : { EKeys::LeftMouseButton, EKeys::RightMouseButton })
		{
			Settings.SetKey(ETraceInputAction::Dash, Key);
			const FKey ReadBack = Settings.GetKey(ETraceInputAction::Dash);

			// Through the string form the ini actually stores, and back. This is where a key that
			// validates but does not SERIALISE would be caught.
			const FString Serialised = ReadBack.IsValid() ? ReadBack.GetFName().ToString() : TEXT("None");
			const FKey Reparsed(*Serialised);

			const bool bOk = (ReadBack == Key) && (Reparsed == Key) && UTraceUserSettings::IsBindableKey(Reparsed);
			RoundTripsOk += bOk ? 1 : 0;

			UE_LOG(LogTraceGame, Display,
				TEXT("[BindableKeys]   DASH <- %-18s set=%d ini='%s' reparsed=%s -> %s"),
				*Key.GetFName().ToString(), (ReadBack == Key) ? 1 : 0, *Serialised,
				*Reparsed.GetFName().ToString(), bOk ? TEXT("ROUND TRIP OK") : TEXT("FAILED"));
		}

		// Put everything back — every action, every slot, including whichever action SetKey took the two
		// buttons from. Restore() clears the whole table before it writes, so the order cannot matter.
		TraceUserSettingsVerify::Restore(Settings, Before);

		// Two calls rather than a ternary verbosity: UE_LOG's verbosity argument is a token the macro
		// pastes into a compile-time category check, not a value, so it cannot be an expression.
		const bool bPass = (MouseButtonsBindable >= 5) && bEscapeExcluded && bAnyKeyExcluded && (RoundTripsOk == 2);

#define TRACE_BINDABLE_VERDICT_ARGS \
	bPass ? TEXT("VALIDATION ACCEPTS MOUSE BUTTONS") : TEXT("VALIDATION IS REJECTING SOMETHING"), \
	MouseButtonsBindable, RoundTripsOk, (bEscapeExcluded && bAnyKeyExcluded) ? 1 : 0

#define TRACE_BINDABLE_VERDICT_TEXT \
	TEXT("[BindableKeys] VERDICT: %s. Mouse buttons bindable=%d/7, round trips=%d/2, Escape and ") \
	TEXT("AnyKey still excluded=%d. If this passes and the MENU still refuses a mouse click, the ") \
	TEXT("refusal is in DELIVERY (the press never reaching WasInputKeyJustPressed while the overlay ") \
	TEXT("is up), not in validation.")

		if (bPass)
		{
			UE_LOG(LogTraceGame, Display, TRACE_BINDABLE_VERDICT_TEXT, TRACE_BINDABLE_VERDICT_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_BINDABLE_VERDICT_TEXT, TRACE_BINDABLE_VERDICT_ARGS);
		}

#undef TRACE_BINDABLE_VERDICT_ARGS
#undef TRACE_BINDABLE_VERDICT_TEXT
	}

	// =============================================================================================
	// Trace.Settings.VerifyBinds — SPEC v15 §5, the evidence half.
	//
	// §5 deletes ETraceInputAction::SwapWeapon, which RENUMBERS every enumerator below it. The claim
	// is that a player's existing TraceUserSettings.ini survives that unharmed, because the file is
	// keyed by ConfigId STRING and never by position. That claim is cheap to write and has been
	// wrong in this codebase before, so this command checks it against the file that is actually on
	// disk rather than against the in-memory table:
	//
	//   1. It reads the RAW `KeyBindings` lines back out of GConfig — the same strings the .ini
	//      holds — instead of trusting UTraceUserSettings::KeyBindings, which any Save() in the run
	//      would already have rewritten from the current table.
	//   2. For every line naming an action that still exists, it asserts the resolved binding IS the
	//      key that line names. That is what "every other bind still lands on the right action"
	//      means, and a renumber would break it wholesale.
	//   3. For every line naming an action that no longer exists — `SwapWeapon=F`, and pre-v3
	//      `Boost=E` — it asserts the line was DROPPED and names it. Dropped, not defaulted-over:
	//      the check below proves no surviving action inherited that key.
	//   4. For every action the file does NOT mention, it asserts the shipped default is in place.
	//
	// Together those four exhaust the file: every line is either honoured or explicitly discarded,
	// and every action is either from the file or from the defaults. There is no third outcome for a
	// renumber to hide in.
	// =============================================================================================
	void VerifyBinds()
	{
		UTraceUserSettings& Settings = UTraceUserSettings::Get();
		const TArray<FTraceInputActionInfo>& Table = TraceInputActions::All();

		UE_LOG(LogTraceGame, Display,
			TEXT("[VerifyBinds] ===== spec v15 s5: does a pre-v15 TraceUserSettings.ini still load correctly? ====="));

		// Straight out of the config cache, so this reports the FILE and not our own idea of it.
		TArray<FString> RawLines;
		const FString Section = UTraceUserSettings::StaticClass()->GetPathName();
		const FString Filename = UTraceUserSettings::StaticClass()->GetConfigName();
		if (GConfig != nullptr)
		{
			GConfig->GetArray(*Section, TEXT("KeyBindings"), RawLines, Filename);
		}

		UE_LOG(LogTraceGame, Display, TEXT("[VerifyBinds]   file '%s' section '%s' holds %d KeyBindings line(s)."),
			*Filename, *Section, RawLines.Num());

		int32 Failures = 0;
		int32 Honoured = 0;
		int32 Dropped = 0;
		TSet<FString> NamedIds;

		for (const FString& Line : RawLines)
		{
			FString ConfigId;
			FString KeyList;
			if (!Line.Split(TEXT("="), &ConfigId, &KeyList))
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[VerifyBinds]   '%s' is not 'Id=Key'; skipped."), *Line);
				continue;
			}
			ConfigId.TrimStartAndEndInline();
			KeyList.TrimStartAndEndInline();
			NamedIds.Add(ConfigId.ToLower());

			const int32 Index = Table.IndexOfByPredicate(
				[&ConfigId](const FTraceInputActionInfo& Info) { return ConfigId.Equals(Info.ConfigId, ESearchCase::IgnoreCase); });

			if (Index == INDEX_NONE)
			{
				++Dropped;
				UE_LOG(LogTraceGame, Display,
					TEXT("[VerifyBinds]   DROPPED  '%s' — no action by that ConfigId any more. This is the case ")
					TEXT("spec v15 s5 is about; a 'SwapWeapon=F' line from a pre-v15 build lands here, and since ")
					TEXT("spec v28 s3d so does 'ParryPull=RightMouseButton'."),
					*Line);
				continue;
			}

			// SPEC v28 §3c — EVERY SLOT THE LINE NAMES, and every slot it does not. A one-key line from an
			// older build says slot 1 = that key AND slot 2 = nothing, so both halves are asserted; checking
			// only the first would pass a loader that quietly left a stale second bind in place.
			TArray<FString> SlotNames;
			TraceUserSettingsVerify::SplitSlots(KeyList, SlotNames);

			const ETraceInputAction LineAction = static_cast<ETraceInputAction>(Index);
			bool bLineOk = true;
			for (int32 Slot = 0; Slot < UTraceUserSettings::MaxKeysPerAction; ++Slot)
			{
				const FString SlotName = SlotNames.IsValidIndex(Slot) ? SlotNames[Slot] : FString();
				const bool bWantsUnbound = SlotName.IsEmpty() || SlotName.Equals(TEXT("None"), ESearchCase::IgnoreCase);
				const FKey Wanted(*SlotName);
				const FKey Actual = Settings.GetKey(LineAction, Slot);

				if (bWantsUnbound ? !Actual.IsValid() : (Actual == Wanted))
				{
					continue;
				}

				bLineOk = false;
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[VerifyBinds]   WRONG    %-18s slot %d asked for '%s' but %s resolved to '%s' — the file has ")
					TEXT("been read BY POSITION somewhere, or a slot was dropped."),
					*ConfigId, Slot + 1, SlotName.IsEmpty() ? TEXT("None") : *SlotName,
					Table[Index].DisplayName, *Actual.GetFName().ToString());
			}

			if (bLineOk)
			{
				++Honoured;
				UE_LOG(LogTraceGame, Display, TEXT("[VerifyBinds]   ok       %-18s -> %-28s (%s)"),
					*ConfigId, *KeyList, Table[Index].DisplayName);
			}
		}

		// Everything the file did not mention must be sitting on its shipped default. This is the half
		// that catches a dropped line quietly leaking its key onto a neighbour.
		for (int32 Index = 0; Index < Table.Num(); ++Index)
		{
			if (NamedIds.Contains(FString(Table[Index].ConfigId).ToLower()))
			{
				continue;
			}

			const ETraceInputAction Unnamed = static_cast<ETraceInputAction>(Index);
			const FKey DefaultPrimary = Table[Index].DefaultKey();
			const FKey DefaultAlt = (Table[Index].DefaultKeyAlt != nullptr) ? Table[Index].DefaultKeyAlt() : FKey();

			// SPEC v28 §3c — BOTH shipped slots. §3d ships PARRY with two, so an action that is not in the
			// file has to come back with both of them or the second default is silently not landing.
			if (Settings.GetKey(Unnamed, 0) == DefaultPrimary && Settings.GetKey(Unnamed, 1) == DefaultAlt)
			{
				UE_LOG(LogTraceGame, Display, TEXT("[VerifyBinds]   default  %-18s -> %s"),
					Table[Index].ConfigId, *Settings.DescribeBinding(Unnamed));
			}
			else
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[VerifyBinds]   WRONG    %s is not named in the file, so it should be its shipped default ")
					TEXT("'%s' + '%s', but it is '%s'."),
					Table[Index].ConfigId, *DefaultPrimary.GetFName().ToString(), *DefaultAlt.GetFName().ToString(),
					*Settings.DescribeBinding(Unnamed));
			}
		}

		// ---- THE VACUITY GUARD, and it is the most important line in this command --------------------
		//
		// Everything above passes trivially on a file whose bindings are all defaults, or whose lines
		// happen to sit in table order: by-ConfigId and by-position agree there, so the run would prove
		// nothing about the renumber it exists to be worried about. So COUNT the disagreement. This is
		// what a positional loader — the bug — would have produced from these very lines, and if it is
		// identical to what we got, the fixture cannot tell the two loaders apart and this run is not
		// evidence.
		int32 PositionalWouldDiffer = 0;
		for (int32 Line = 0; Line < RawLines.Num() && Line < Table.Num(); ++Line)
		{
			FString ConfigId;
			FString KeyList;
			if (!RawLines[Line].Split(TEXT("="), &ConfigId, &KeyList))
			{
				continue;
			}

			// The PRIMARY slot only: this arm asks "would a by-position loader have put a different key on
			// this action", and slot 1 is the same question a second time.
			TArray<FString> SlotNames;
			TraceUserSettingsVerify::SplitSlots(KeyList, SlotNames);
			const FString KeyName = SlotNames.Num() > 0 ? SlotNames[0] : FString();

			const FKey AsPositional(*KeyName);
			if (Settings.GetKey(static_cast<ETraceInputAction>(Line)) != AsPositional)
			{
				++PositionalWouldDiffer;
			}
		}

		// A WARNING RATHER THAN A FAILURE SINCE SPEC v26 §1, and only because the fixture arm below now
		// supplies the discrimination this file could not. Before v26 there was no other source of it,
		// so "your file cannot tell the two loaders apart" was correctly fatal; now it is a statement
		// about the machine the command was run on, and failing a verification because the person
		// running it never rebound anything would be noise. If the fixture arm does not run or is
		// itself vacuous, this becomes a failure again — see the flag's use below.
		bool bFileCannotDiscriminate = false;
		if (PositionalWouldDiffer == 0)
		{
			bFileCannotDiscriminate = true;
			UE_LOG(LogTraceGame, Warning,
				TEXT("[VerifyBinds]   this FILE cannot discriminate: reading it BY POSITION would have ")
				TEXT("produced the same bindings as reading it by ConfigId, because its lines happen to sit ")
				TEXT("in table order. The v26 s1 fixture arm below supplies the discrimination instead."));
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[VerifyBinds]   discriminating: a by-POSITION read of this file would have put %d of %d ")
				TEXT("action(s) on the wrong key. It did not, which is the claim."),
				PositionalWouldDiffer, Table.Num());
		}

		// ---- SPEC v26 §1 — THE APPEND-ONLY PROOF, ON A FIXTURE THIS COMMAND BUILDS ITSELF ----------
		//
		// §1 adds ETraceInputAction::PullCore. Appending an enumerator is safe ONLY because the file is
		// keyed by ConfigId string and never by position, and the guard above can only test that claim
		// as well as whoever is running it happens to have rebound their keys. On the machine this was
		// written on it reported VACUOUS — every line sat in table order — so the strongest check in
		// the command proved nothing about the very change that most needed it.
		//
		// So this arm stops depending on the player's file. It writes a fixture whose lines are
		// deliberately OUT OF TABLE ORDER, onto distinctive keys, including one line for the brand-new
		// PullCore and one naming an action that no longer exists, then runs the REAL loader
		// (RefreshFromConfig, the same call Get() makes on first use) over it.
		//
		// IN MEMORY ONLY, AND RESTORED. The fixture is written into UTraceUserSettings::KeyBindings —
		// the member RefreshFromConfig actually parses — and NOT into GConfig. That is a correction,
		// not a shortcut: the first version of this arm called GConfig->SetArray and every fixture line
		// came back as the shipped default, because RefreshFromConfig re-parses the MEMBER array that
		// LoadConfig populated at class load and never re-reads the config cache. Writing the member is
		// both the thing that works and the thing that cannot touch disk: nothing here calls Save(),
		// and the original array is put back and re-loaded before the command returns. That matters
		// more than usual — this can be run mid-match from the console.
		int32 FixtureDiscriminates = 0;
		{
			struct FFixtureLine { const TCHAR* ConfigId; const TCHAR* KeyList; };

			// REVERSED relative to the table, and every key non-default, so a positional loader cannot
			// accidentally agree. "SwapWeapon" is the dead id spec v15 §5 removed and is here to prove
			// the drop path still drops rather than shifting everything after it by one.
			//
			// SPEC v28 §3c ADDS TWO MORE JOBS TO THIS FIXTURE, both on lines that already existed:
			//   * "ParryKeys" carries TWO keys, so the comma format is proved through the REAL loader
			//     rather than asserted — and its id is the v28 §3d one, so the row also proves the new id
			//     resolves at all.
			//   * "Reload" carries a one-key line with NO comma, which is what every previously-written
			//     TraceUserSettings.ini on every machine looks like. Its second slot must come back EMPTY:
			//     that is the backward-compatibility claim, and it is the one a two-slot loader is most
			//     likely to get wrong by leaving the shipped default in place.
			const FFixtureLine Fixture[] =
			{
				{ TEXT("PullCore"),         TEXT("Nine")                    },   // the new row — LAST in the table, FIRST here
				{ TEXT("SwapWeapon"),       TEXT("Seven")                   },   // dead id: must be DROPPED, not shifted
				{ TEXT("Reload"),           TEXT("Eight")                   },   // v28: one key, no comma — a pre-v28 line
				{ TEXT("AbilitySecondary"), TEXT("Six")                     },
				{ TEXT("ParryKeys"),        TEXT("Five,ThumbMouseButton2")  },   // v28 §3c: TWO keys on one line
				{ TEXT("MoveForward"),      TEXT("Four")                    },   // row 0 of the table — LAST here
			};

			TArray<FString> FixtureLines;
			for (const FFixtureLine& Line : Fixture)
			{
				FixtureLines.Add(FString::Printf(TEXT("%s=%s"), Line.ConfigId, Line.KeyList));
			}

			const TArray<FString> SavedKeyBindings = Settings.KeyBindings;
			Settings.KeyBindings = FixtureLines;
			Settings.RefreshFromConfig();

			int32 FixtureFailures = 0;
			int32 FixtureSlotsChecked = 0;

			for (int32 Line = 0; Line < UE_ARRAY_COUNT(Fixture); ++Line)
			{
				const FString Id(Fixture[Line].ConfigId);

				TArray<FString> SlotNames;
				TraceUserSettingsVerify::SplitSlots(Fixture[Line].KeyList, SlotNames);

				const int32 Index = Table.IndexOfByPredicate(
					[&Id](const FTraceInputActionInfo& Info) { return Id.Equals(Info.ConfigId, ESearchCase::IgnoreCase); });

				if (Index == INDEX_NONE)
				{
					// The dead id. Nobody may be holding its key, in ANY slot — that is what "dropped" means,
					// as opposed to "shifted onto the neighbour".
					const FKey Wanted(*SlotNames[0]);
					const bool bNobodyHasIt = Table.IndexOfByPredicate(
						[&Settings, &Wanted](const FTraceInputActionInfo& Info)
						{ return Settings.ActionUsesKey(Info.Action, Wanted); }) == INDEX_NONE;
					if (!bNobodyHasIt)
					{
						++FixtureFailures;
						UE_LOG(LogTraceGame, Error,
							TEXT("[VerifyBinds]   v26 s1 FIXTURE: the dead id '%s' was not dropped — something ")
							TEXT("inherited '%s'."), *Id, Fixture[Line].KeyList);
					}
					continue;
				}

				const ETraceInputAction FixtureAction = static_cast<ETraceInputAction>(Index);
				for (int32 Slot = 0; Slot < UTraceUserSettings::MaxKeysPerAction; ++Slot)
				{
					++FixtureSlotsChecked;

					const FString SlotName = SlotNames.IsValidIndex(Slot) ? SlotNames[Slot] : FString();
					const FKey Wanted(*SlotName);
					const FKey Actual = Settings.GetKey(FixtureAction, Slot);
					const bool bWantsUnbound = SlotName.IsEmpty() || SlotName.Equals(TEXT("None"), ESearchCase::IgnoreCase);

					if (bWantsUnbound ? !Actual.IsValid() : (Actual == Wanted))
					{
						continue;
					}

					++FixtureFailures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[VerifyBinds]   FIXTURE: '%s' (%s) slot %d asked for '%s' and resolved to '%s'. Either ")
						TEXT("appending an action RENUMBERED the table, or the v28 s3c slot list did not load."),
						*Id, Table[Index].DisplayName, Slot + 1,
						SlotName.IsEmpty() ? TEXT("None") : *SlotName, *Actual.GetFName().ToString());
				}

				// Would a positional loader have got this line wrong? It must, for at least one line,
				// or the fixture is as vacuous as the file was.
				if (Table.IsValidIndex(Line) && Index != Line)
				{
					++FixtureDiscriminates;
				}
			}

			if (FixtureDiscriminates == 0)
			{
				++FixtureFailures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[VerifyBinds]   v26 s1 FIXTURE is VACUOUS: its lines are in table order after all."));
			}

			Failures += FixtureFailures;

			if (FixtureFailures == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[VerifyBinds]   ok       APPEND-ONLY + v28 s3c SLOT PROOF: a %d-line out-of-order fixture "
					     "(PullCore first, MoveForward last, one dead id, one TWO-KEY line and one legacy "
					     "one-key line) loaded through the real RefreshFromConfig; all %d slots landed on the "
					     "action AND the slot they NAME. %d line(s) would have landed on the wrong action under "
					     "a by-position read, so this fixture can tell the two loaders apart. The player's file "
					     "on disk was not touched."),
					static_cast<int32>(UE_ARRAY_COUNT(Fixture)), FixtureSlotsChecked, FixtureDiscriminates);
			}

			// Put the player's own lines back and re-load them, so nothing after this command — and in
			// particular no later Save() by the options screen — can see the fixture.
			Settings.KeyBindings = SavedKeyBindings;
			Settings.RefreshFromConfig();
		}

		// The vacuity guard's verdict, decided once BOTH sources of discrimination have been tried.
		// Neither one on its own is enough: a file in table order proves nothing, and a fixture that
		// failed to run leaves the renumber claim untested.
		if (bFileCannotDiscriminate && FixtureDiscriminates == 0)
		{
			++Failures;
			UE_LOG(LogTraceGame, Error,
				TEXT("[VerifyBinds]   VACUOUS: neither the player's file nor the v26 s1 fixture could tell a ")
				TEXT("by-ConfigId loader from a by-position one, so this run says nothing about renumbering."));
		}

		// The removed action, by name. Stated separately because "SwapWeapon is gone" is the actual
		// requirement and an empty ini would otherwise let every check above pass vacuously.
		const bool bSwapGone = Table.IndexOfByPredicate(
			[](const FTraceInputActionInfo& Info) { return FCString::Stricmp(Info.ConfigId, TEXT("SwapWeapon")) == 0; }) == INDEX_NONE;
		if (!bSwapGone)
		{
			++Failures;
			UE_LOG(LogTraceGame, Error, TEXT("[VerifyBinds]   SwapWeapon is STILL in the action table — spec v15 s5 removes it."));
		}

#define TRACE_VERIFYBINDS_ARGS \
	(Failures == 0) ? TEXT("THE PRE-v15 FILE LOADS CORRECTLY") : TEXT("A BINDING LANDED ON THE WRONG ACTION"), \
	Honoured, Dropped, Table.Num(), bSwapGone ? 1 : 0, Failures

#define TRACE_VERIFYBINDS_TEXT \
	TEXT("[VerifyBinds] VERDICT: %s. %d line(s) honoured, %d stale line(s) dropped, %d action(s) in the ") \
	TEXT("table, SwapWeapon removed=%d, %d failure(s).")

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_VERIFYBINDS_TEXT, TRACE_VERIFYBINDS_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_VERIFYBINDS_TEXT, TRACE_VERIFYBINDS_ARGS);
		}

#undef TRACE_VERIFYBINDS_ARGS
#undef TRACE_VERIFYBINDS_TEXT
	}

	// =============================================================================================
	// Trace.Keys.VerifyV28 — SPEC v28 §3b, §3c and §3d, the evidence half.
	//
	// Three separate claims, and none of them can be shown by reading the defaults table back to
	// itself, so all three are exercised through the SHIPPING write path (UTraceUserSettings::SetKey,
	// the same call the options page makes) and read back through the live table:
	//
	//   §3b  Two actions whose states are exclusive KEEP one key between them; two whose states
	//        overlap do not. The owner's own example is the first arm — throw-core onto the fire key,
	//        and FIRE must still be on it afterwards. The second arm is the guard against a "fix" that
	//        just stopped stealing altogether: the Core-pull onto the fire key must still take it.
	//   §3c  A second key set on slot 2 survives the .ini round trip (flatten -> parse), which is the
	//        step that would silently drop it.
	//   §3d  PARRY ships on Q AND the thumb mouse button, and NOTHING holds the right mouse button any
	//        more — the half §10's melee is waiting on.
	//
	// EVERY BINDING IS PUT BACK, all slots, through TraceUserSettingsVerify::Restore. This can be run
	// mid-match from the console and must not cost the player their controls.
	//
	// RED ARM: `Trace.Keys.LegacySteal 1` restores the pre-v28 unconditional steal, and the §3b arm
	// then FAILS. A run of this command in each arm is what makes it evidence rather than assertion.
	// =============================================================================================
	void VerifyV28Keys()
	{
		UTraceUserSettings& Settings = UTraceUserSettings::Get();

		UE_LOG(LogTraceGame, Display,
			TEXT("[KeysV28] ===== spec v28 s3: two binds per action, state-aware sharing, parry on Q + thumb ====="));

		int32 Failures = 0;

		// ---- §3d: the shipped parry, and the right mouse button ---------------------------------
		{
			const FTraceInputActionInfo& ParryInfo = TraceInputActions::Info(ETraceInputAction::Parry);
			const FKey ShippedPrimary = ParryInfo.DefaultKey();
			const FKey ShippedAlt = (ParryInfo.DefaultKeyAlt != nullptr) ? ParryInfo.DefaultKeyAlt() : FKey();

			const bool bDefaultsOk = (ShippedPrimary == EKeys::Q) && (ShippedAlt == EKeys::ThumbMouseButton);
			Failures += bDefaultsOk ? 0 : 1;

			if (bDefaultsOk)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[KeysV28]   ok       s3d SHIPPED PARRY = '%s' + '%s' (ConfigId '%s'). Live binding: %s"),
					*UTraceUserSettings::DescribeKey(ShippedPrimary), *UTraceUserSettings::DescribeKey(ShippedAlt),
					ParryInfo.ConfigId, *Settings.DescribeBinding(ETraceInputAction::Parry));
			}
			else
			{
				UE_LOG(LogTraceGame, Error,
					TEXT("[KeysV28]   WRONG    s3d asks for parry on BOTH Q and the thumb mouse button; the table ")
					TEXT("ships '%s' + '%s'."),
					*UTraceUserSettings::DescribeKey(ShippedPrimary), *UTraceUserSettings::DescribeKey(ShippedAlt));
			}

			// The LIVE binding is reported separately and NOT failed on: a player is allowed to rebind
			// their parry, and failing this command for them would make it useless on a real machine.
			if (!Settings.ActionUsesKey(ETraceInputAction::Parry, EKeys::Q)
				|| !Settings.ActionUsesKey(ETraceInputAction::Parry, EKeys::ThumbMouseButton))
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[KeysV28]   note     the LIVE parry is %s, not the shipped pair. That is a rebind (or a ")
					TEXT("TraceUserSettings.ini that already names 'ParryKeys'), not a failure."),
					*Settings.DescribeBinding(ETraceInputAction::Parry));
			}

			// =========================================================================================
			// RIGHT MOUSE MUST BE HELD BY MELEE, AND BY NOTHING ELSE.
			//
			// *** THIS ASSERTION WAS INVERTED BY THE INTEGRATION PASS, AND THE OLD ONE WAS RIGHT WHEN IT
			// WAS WRITTEN. *** §3 shipped before §10's melee had a row in this table at all: the only
			// thing this file could prove then was the half it owned - that §3d had VACATED the button -
			// so it asserted "nothing holds right mouse". The integrator then added
			// ETraceInputAction::Melee on exactly that button, which is what §10 asked for, and this
			// command began failing a build that was finally correct. Worth naming, because it is the
			// classic stale-harness failure: the assertion did not become wrong, its PREMISE did.
			//
			// IT IS ASSERTED ON THE SHIPPED DEFAULTS, NOT ON THE LIVE BINDINGS, which is the same choice
			// the §3d parry check twenty lines above makes and for the same reason: a player is allowed
			// to rebind their melee, and a command that fails on a machine whose TraceUserSettings.ini
			// has been touched is a command nobody can use on a real machine. (This very run found
			// 'EquipGun -> K' in the local .ini, so that is not a hypothetical.) The LIVE state is
			// reported underneath as a note.
			//
			// The rule has two halves and both matter:
			//   MELEE SHIPS ON IT    - otherwise §10's "melee should be bound to right click by default"
			//                          never reaches a player, which is exactly the state the integrator
			//                          found: §3 vacated the button and §10 could not claim it;
			//   NOTHING ELSE DOES    - the ORIGINAL check, unchanged in substance. A parry shipping on
			//                          right mouse is what a returning player gets if the 'ParryPull' ->
			//                          'ParryKeys' migration did not take, and it would now be a genuine
			//                          double-bind rather than merely a stale default.
			//
			// THE CORE PULL IS NOT A COUNTER-EXAMPLE. It rides this button too, but not as a BINDING:
			// TraceMelee::HandleMeleeInput consults ATraceCore::CanPullNow and dispatches the pull
			// instead of the swing while the pull circle is on screen. One key, one action, one press,
			// two verbs chosen by state - so PullCore correctly does not appear here, and if it ever
			// does, that is one press dispatching the same verb twice.
			// =========================================================================================
			{
				int32 RmbDefaultHolders = 0;
				bool  bMeleeShipsOnRmb  = false;

				for (const FTraceInputActionInfo& Info : TraceInputActions::All())
				{
					const FKey Primary = (Info.DefaultKey != nullptr) ? Info.DefaultKey() : FKey();
					const FKey Alt     = (Info.DefaultKeyAlt != nullptr) ? Info.DefaultKeyAlt() : FKey();

					if (Primary != EKeys::RightMouseButton && Alt != EKeys::RightMouseButton)
					{
						continue;
					}

					++RmbDefaultHolders;

					if (Info.Action == ETraceInputAction::Melee)
					{
						bMeleeShipsOnRmb = true;
						continue;
					}

					++Failures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[KeysV28]   WRONG    '%s' (%s) SHIPS on the right mouse button as well as MELEE. Spec ")
						TEXT("v28 s3d vacates that button for s10's melee and nothing else may share it. If this is ")
						TEXT("the parry, the 'ParryPull' -> 'ParryKeys' migration did not take."),
						Info.ConfigId, Info.DisplayName);
				}

				if (bMeleeShipsOnRmb)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[KeysV28]   ok       s10 MELEE SHIPS on the RIGHT MOUSE BUTTON and is the only one of ")
						TEXT("the %d rebindable actions that does. The Core pull rides the same press through ")
						TEXT("TraceMelee::HandleMeleeInput's precedence, not through a second binding."),
						TraceInputActions::All().Num());
				}
				else
				{
					++Failures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[KeysV28]   WRONG    MELEE does not ship on the right mouse button. Spec v28 s10: ")
						TEXT("\"Melee should be bound to right click by default.\" %s"),
						(RmbDefaultHolders == 0)
							? TEXT("Nothing at all ships on the button - the pre-integration state, where s3d vacated "
							       "it and no row claimed it.")
							: TEXT("Something else ships on it, which is worse: the button is claimed by the wrong verb."));
				}

				// The LIVE table, reported and NOT failed on - see the block comment above.
				int32 LiveRmbHolders = 0;
				FString LiveRmbNames;
				for (const FTraceInputActionInfo& Info : TraceInputActions::All())
				{
					if (Settings.ActionUsesKey(Info.Action, EKeys::RightMouseButton))
					{
						++LiveRmbHolders;
						LiveRmbNames += (LiveRmbNames.IsEmpty() ? TEXT("") : TEXT(", "));
						LiveRmbNames += Info.DisplayName;
					}
				}

				if (LiveRmbHolders == 1 && Settings.ActionUsesKey(ETraceInputAction::Melee, EKeys::RightMouseButton))
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[KeysV28]   ok       and on THIS machine's live bindings the right mouse button is "
						     "MELEE alone, so the shipped default is what is actually being played."));
				}
				else
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[KeysV28]   note     this machine's LIVE right mouse button is held by %d action(s) "
						     "[%s] and melee resolves to %s. That is a rebind in "
						     "Saved/Config/<Platform>/TraceUserSettings.ini, not a failure."),
						LiveRmbHolders,
						LiveRmbHolders == 0 ? TEXT("nothing") : *LiveRmbNames,
						*Settings.DescribeBinding(ETraceInputAction::Melee));
				}
			}
		}

		// ---- §3b: the exclusion groups, as a table ------------------------------------------------
		{
			struct FPairExpectation
			{
				ETraceInputAction A;
				ETraceInputAction B;
				bool bMayShare;
				const TCHAR* Why;
			};

			const FPairExpectation Pairs[] =
			{
				{ ETraceInputAction::Pass,  ETraceInputAction::Fire,     true,
				  TEXT("the owner's own example: throwing needs the Core, firing needs not to have it") },
				{ ETraceInputAction::Parry, ETraceInputAction::PullCore, true,
				  TEXT("v25 s2's argument, now expressed as data rather than a comment") },
				{ ETraceInputAction::Parry, ETraceInputAction::Fire,     true,
				  TEXT("parry is carrier-only, fire is non-carrier-only") },
				{ ETraceInputAction::Fire,  ETraceInputAction::PullCore, false,
				  TEXT("both live while NOT carrying - a genuine conflict") },
				{ ETraceInputAction::Parry, ETraceInputAction::Pass,     false,
				  TEXT("both live while carrying - a genuine conflict") },
				{ ETraceInputAction::Jump,  ETraceInputAction::Dash,     false,
				  TEXT("both live in every state - the ordinary case, and it must still refuse") },
			};

			for (const FPairExpectation& Pair : Pairs)
			{
				const bool bActual = UTraceUserSettings::ActionsMayShareAKey(Pair.A, Pair.B);
				if (bActual == Pair.bMayShare)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[KeysV28]   ok       s3b %-22s + %-22s may share=%d  (%s / %s) - %s"),
						TraceInputActions::Info(Pair.A).DisplayName, TraceInputActions::Info(Pair.B).DisplayName,
						bActual ? 1 : 0,
						*LexTraceInputStates(TraceInputActions::Info(Pair.A).States),
						*LexTraceInputStates(TraceInputActions::Info(Pair.B).States), Pair.Why);
				}
				else
				{
					++Failures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[KeysV28]   WRONG    s3b %s + %s: may share=%d, expected %d (%s)."),
						TraceInputActions::Info(Pair.A).DisplayName, TraceInputActions::Info(Pair.B).DisplayName,
						bActual ? 1 : 0, Pair.bMayShare ? 1 : 0, Pair.Why);
				}
			}
		}

		// ---- §3b end to end, and §3c's round trip, through the real write path ---------------------
		//
		// Snapshot FIRST. Everything below writes real bindings and saves them.
		TArray<FKey> Before;
		TraceUserSettingsVerify::Snapshot(Settings, Before);

		{
			const FKey FireKey = Settings.GetKey(ETraceInputAction::Fire);
			if (!FireKey.IsValid())
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[KeysV28]   WRONG    FIRE is unbound on this machine, so the s3b sharing arm cannot be ")
					TEXT("measured. Bind fire and run again."));
			}
			else
			{
				// --- ALLOWED: throw core onto the fire button ----------------------------------------
				Settings.SetKey(ETraceInputAction::Pass, 0, FireKey);

				const bool bThrowTook  = Settings.ActionUsesKey(ETraceInputAction::Pass, FireKey);
				const bool bFireKeptIt = Settings.ActionUsesKey(ETraceInputAction::Fire, FireKey);

				if (bThrowTook && bFireKeptIt)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[KeysV28]   ok       s3b THE ITEM: THROW / PASS CORE was bound to '%s' and FIRE STILL HAS IT. ")
						TEXT("One button, two verbs, and the carrier test decides which one runs."),
						*UTraceUserSettings::DescribeKey(FireKey));
				}
				else
				{
					++Failures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[KeysV28]   WRONG    s3b THE ITEM FAILED: after binding the throw to '%s', throwHasIt=%d ")
						TEXT("fireHasIt=%d. This is the reported bug (fire went blank) - or Trace.Keys.LegacySteal is 1, ")
						TEXT("which is the RED arm and is expected to print exactly this."),
						*UTraceUserSettings::DescribeKey(FireKey), bThrowTook ? 1 : 0, bFireKeptIt ? 1 : 0);
				}

				// --- REFUSED: the Core-pull onto the same button --------------------------------------
				//
				// The guard against "we fixed it by never stealing". Fire and the pull are both
				// NotCarrying, so this one MUST take the key away from fire.
				Settings.SetKey(ETraceInputAction::PullCore, 0, FireKey);
				const bool bPullTook = Settings.ActionUsesKey(ETraceInputAction::PullCore, FireKey);
				const bool bFireLost = !Settings.ActionUsesKey(ETraceInputAction::Fire, FireKey);

				if (bPullTook && bFireLost)
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[KeysV28]   ok       s3b THE OTHER HALF: PULL CORE took '%s' AWAY from FIRE, because both ")
						TEXT("are live while not carrying. Sharing is allowed, not universal."),
						*UTraceUserSettings::DescribeKey(FireKey));
				}
				else
				{
					++Failures;
					UE_LOG(LogTraceGame, Error,
						TEXT("[KeysV28]   WRONG    s3b a GENUINE conflict was allowed: pullHasIt=%d fireStillHasIt=%d on ")
						TEXT("'%s'. The check has stopped refusing anything."),
						bPullTook ? 1 : 0, bFireLost ? 0 : 1, *UTraceUserSettings::DescribeKey(FireKey));
				}
			}

			// --- §3c: a SECOND key, through the .ini round trip -----------------------------------
			//
			// DASH is the victim: ETraceInputStates::Match, so nothing about it is special-cased by the
			// conflict rule, and it is not one of the four actions the arms above are moving around.
			Settings.SetKey(ETraceInputAction::Dash, 0, EKeys::Nine);
			Settings.SetKey(ETraceInputAction::Dash, 1, EKeys::Eight);

			const bool bBothSet = (Settings.GetKey(ETraceInputAction::Dash, 0) == EKeys::Nine)
				&& (Settings.GetKey(ETraceInputAction::Dash, 1) == EKeys::Eight);

			// Through the exact strings the .ini holds and back, which is where a second slot that
			// validates but does not SERIALISE would be lost. Save() has already flattened; re-parsing
			// the member array is what the loader does on the next launch.
			Settings.RefreshFromConfig();
			const bool bSurvived = (Settings.GetKey(ETraceInputAction::Dash, 0) == EKeys::Nine)
				&& (Settings.GetKey(ETraceInputAction::Dash, 1) == EKeys::Eight);

			if (bBothSet && bSurvived)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[KeysV28]   ok       s3c DASH held TWO keys (%s) and both survived a flatten and re-parse of ")
					TEXT("the ini's own string form."),
					*Settings.DescribeBinding(ETraceInputAction::Dash));
			}
			else
			{
				++Failures;
				UE_LOG(LogTraceGame, Error,
					TEXT("[KeysV28]   WRONG    s3c set=%d survivedRoundTrip=%d; DASH is '%s'. The second slot does not ")
					TEXT("persist."),
					bBothSet ? 1 : 0, bSurvived ? 1 : 0, *Settings.DescribeBinding(ETraceInputAction::Dash));
			}
		}

		// Everything the arms above wrote, put back — all seventeen actions, both slots.
		TraceUserSettingsVerify::Restore(Settings, Before);

		int32 RestoreFailures = 0;
		{
			TArray<FKey> After;
			TraceUserSettingsVerify::Snapshot(Settings, After);
			for (int32 Index = 0; Index < Before.Num() && Index < After.Num(); ++Index)
			{
				RestoreFailures += (Before[Index] == After[Index]) ? 0 : 1;
			}
		}
		if (RestoreFailures > 0)
		{
			++Failures;
			UE_LOG(LogTraceGame, Error,
				TEXT("[KeysV28]   WRONG    %d slot(s) were not restored. This command has DAMAGED the player's ")
				TEXT("bindings - that is worse than the bug it measures."), RestoreFailures);
		}
		else
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[KeysV28]   ok       every binding restored (%d slots)."), Before.Num());
		}

#define TRACE_KEYSV28_ARGS \
	(Failures == 0) ? TEXT("SPEC v28 s3b/s3c/s3d HOLD") : TEXT("SOMETHING IN SPEC v28 s3 IS NOT TRUE"), \
	GTraceKeysLegacySteal, Failures

#define TRACE_KEYSV28_TEXT \
	TEXT("[KeysV28] VERDICT: %s. Trace.Keys.LegacySteal=%d (1 is the RED arm and MUST fail the s3b item), ") \
	TEXT("%d failure(s).")

		if (Failures == 0)
		{
			UE_LOG(LogTraceGame, Display, TRACE_KEYSV28_TEXT, TRACE_KEYSV28_ARGS);
		}
		else
		{
			UE_LOG(LogTraceGame, Error, TRACE_KEYSV28_TEXT, TRACE_KEYSV28_ARGS);
		}

#undef TRACE_KEYSV28_ARGS
#undef TRACE_KEYSV28_TEXT
	}

	FAutoConsoleCommand CmdVerifyV28Keys(
		TEXT("Trace.Keys.VerifyV28"),
		TEXT("Spec v28 s3. Proves two actions with exclusive states KEEP one key between them (throw core ")
		TEXT("on the fire button) while two that overlap do not, that a second bind survives the ini round ")
		TEXT("trip, and that parry ships on Q + the thumb mouse button with nothing left on right mouse. ")
		TEXT("Restores every binding it touches. Trace.Keys.LegacySteal 1 is the red arm."),
		FConsoleCommandDelegate::CreateStatic(&VerifyV28Keys));

	FAutoConsoleCommand CmdVerifyBinds(
		TEXT("Trace.Settings.VerifyBinds"),
		TEXT("Spec v15 s5. Reads the raw KeyBindings lines back out of TraceUserSettings.ini and proves ")
		TEXT("every line naming a live action resolved to that action's key, every line naming a dead one ")
		TEXT("(SwapWeapon, Boost) was dropped, and every unmentioned action is on its shipped default."),
		FConsoleCommandDelegate::CreateStatic(&VerifyBinds));

	FAutoConsoleCommand CmdVerifyBindableKeys(
		TEXT("Trace.VerifyBindableKeys"),
		TEXT("Spec v10 s8. Prints whether every mouse button passes UTraceUserSettings::IsBindableKey, ")
		TEXT("whether Escape and AnyKey are still excluded, and round-trips LeftMouseButton and ")
		TEXT("RightMouseButton through a real binding and the ini's string form. Restores the bindings it ")
		TEXT("touches."),
		FConsoleCommandDelegate::CreateStatic(&VerifyBindableKeys));
}

#endif // !UE_BUILD_SHIPPING
