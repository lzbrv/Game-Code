// Trace — the player's own settings: mouse feel and key bindings.
//
// WHY THIS IS SEPARATE FROM UTraceSettings
// UTraceSettings is a UDeveloperSettings marked `defaultconfig`: it is the DESIGNER's table, it
// ships in Config/DefaultGame.ini, and it is checked into the repo. A player's mouse sensitivity is
// the opposite of that in every respect — it is per-machine, it is written at runtime, and it must
// never end up in a diff. So it lives here, in its own config hierarchy, and the two never mix.
//
// WHERE IT PERSISTS
// UCLASS(config = TraceUserSettings) with no `defaultconfig` and no `globaluserconfig` resolves to
//     <Project>/Saved/Config/<Platform>/TraceUserSettings.ini
// which is exactly right: writable, per-user, outside source control, and it survives a restart.
// Save() also flushes GConfig for that file, so the settings survive a hard kill of the process as
// well as a clean exit — which matters, because the way this build is usually closed is pkill.
//
// WHY THE CDO AND NOT AN INSTANCE
// Same reasoning as UTraceSettings::Get(). The config system populates a CDO automatically at class
// load and keeps it current; an instance would need rooting against GC and manual LoadConfig, and
// would then have to be found again from three unrelated call sites (two HUDs and a controller).

#pragma once

#include "CoreMinimal.h"
#include "Delegates/DelegateCombinations.h"
#include "InputCoreTypes.h"             // FKey / EKeys
#include "Misc/EnumClassFlags.h"        // ENUM_CLASS_FLAGS, for ETraceInputStates
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "TraceUserSettings.generated.h"

/**
 * Every rebindable action, in the order the options screen lists them.
 *
 * Look is deliberately absent: it is the mouse, it has no discrete key, and everything a player
 * could want to change about it is a sensitivity or an inversion rather than a binding.
 *
 * Fire and Pass are BOTH listed even though the spec has mouse1 double as "pass"/"throw" while
 * carrying the Core. That overload is a gameplay rule inside the weapon/character slice, not an
 * input rule: the player still deserves an explicit throw bind, and the two are free to be the same
 * key. Since spec v25 §7 the Pass row ships UNBOUND — see its own comment; mouse1 is the throw.
 */
UENUM()
enum class ETraceInputAction : uint8
{
	MoveForward = 0,
	MoveBack,
	MoveLeft,
	MoveRight,
	Jump,
	Crouch,
	Dash,
	/**
	 * Spec v3 §3. Took the slot Boost vacated, which is why Count is unchanged.
	 *
	 * *** SPEC v25 §7: THIS IS THE RIGHT MOUSE BUTTON NOW, and its ConfigId is "ParryPull". ***
	 * The ENUMERATOR keeps its name — internal identifiers are explicitly allowed to — but the id
	 * that reaches the .ini moved, for the reason the action table spells out at length: a default
	 * key change is invisible to every player who has ever saved a setting unless the id moves with
	 * it, because Save() writes a line for every action and RefreshFromConfig honours them all.
	 *
	 * The id has now done this twice: "Boost" -> "Parry" (spec v3, when the boost was deleted and
	 * nobody was to inherit a parry on the old boost key) and "Parry" -> "ParryPull" here. Both
	 * times the migration is the same one line of existing behaviour — RefreshFromConfig drops a
	 * line naming an id the table does not have — and both times there is no upgrade path to write.
	 *
	 * "ParryPull" and not "Parry2" because the button genuinely gained a second verb: spec v25 §2's
	 * Core pull is dispatched from the same press. See ATracePlayerController::OnParryStarted for
	 * why the two can never collide (parry needs you carrying, the pull needs you not carrying).
	 *
	 * *** SPEC v26 §1 SPLITS THE TWO VERBS APART AGAIN, AND THIS ROW KEEPS THE BUTTON. *** Verbatim:
	 * "Make parry and pull core two separate binds in the settings menu". The pull now has its own
	 * action (ETraceInputAction::PullCore, appended at the bottom of this list) with its own default
	 * and its own row on the keybind page; parry keeps right mouse, which is what spec v25 §7 asked
	 * for in as many words and which v26 does not touch.
	 *
	 * THE ConfigId STAYS "ParryPull" AND THAT IS DELIBERATE. Every id migration in this table costs a
	 * returning player their hand-rebound key once (RefreshFromConfig drops a line naming an id the
	 * table no longer has). v25 §7 already spent that cost to move parry onto this button; renaming
	 * back to "Parry" now would spend it a second time, for a cosmetic gain, and would silently
	 * un-rebind everybody who has rebound their parry since. The DISPLAY NAME is what a player reads
	 * and it does move — "PARRY / PULL CORE" becomes "PARRY" — because that string is not persisted
	 * anywhere and can be corrected for free.
	 */
	Parry,
	Fire,
	/**
	 * The Core throw / pass.
	 *
	 * *** SPEC v25 §7: NO DEFAULT KEY. *** It was right mouse; the note removes that bind, parry
	 * takes the button, and nothing replaces it here — mouse 1 already throws (goals) or passes
	 * (endzones) while carrying, so the verb keeps the button the HUD has always taught and this row
	 * is the optional second route. Its ConfigId moved to "ThrowCore" for the same migration reason
	 * Parry's did: without the move, a returning player's `Pass=RightMouseButton` line would put the
	 * throw straight back onto the button the note just cleared.
	 */
	Pass,
	Scoreboard,

	/**
	 * Spec v13 §2 — 1 = knife, 2 = gun. Verbatim: "Change default keybinds for switching weapons to
	 * be: 1 (switch to knife) and 2 (switch to gun)."
	 *
	 * THIS IS AN INPUT-MODEL CHANGE, NOT A REBIND. A toggle answers "give me the other weapon";
	 * these answer "give me THIS weapon", and no amount of rebinding turns one into the other —
	 * putting a toggle on 1 and on 2 would have made both keys do the same thing.
	 *
	 * DIRECT SELECT MEANS IDEMPOTENT: pressing 1 with the knife already out does nothing at all, and
	 * in particular does not restart the 0.2 s pullout. The guard lives in
	 * ATracePlayerController::HandleDirectEquip -> TraceMelee::RequestEquipIfDifferent — see the
	 * comment there for why it is still correct mid-pullout.
	 *
	 * SPEC v15 §5 DELETED THE `SwapWeapon` TOGGLE THAT USED TO SIT IMMEDIATELY ABOVE THIS LINE,
	 * verbatim: "Switch weapon keybind so that it's only switch to knife/switch to gun." It had a
	 * ConfigId of "SwapWeapon" and a default of F, and it is gone — the action, its IA_, its
	 * binding, its handler and its row in the rebind list. This supersedes spec v13 §2's
	 * [ASSUMPTION] that the toggle was worth keeping as a third rebindable action.
	 *
	 * WHAT THAT DELETION COSTS, AND WHY IT IS NOTHING. Removing an enumerator RENUMBERS every action
	 * below it, and this table's positions are the indices into UTraceUserSettings::Bindings — so a
	 * previously-saved TraceUserSettings.ini that referred to actions by POSITION would now be
	 * reading the wrong rows. It does not: RefreshFromConfig matches each `ConfigId=Key` line
	 * against FTraceInputActionInfo::ConfigId by STRING (see the parse loop) and a line naming an
	 * action that no longer exists is dropped, exactly as pre-v3 `Boost=E` lines already are. A
	 * player's `SwapWeapon=F` line is now one of those, and every other bind in their file still
	 * lands on the action it names. Trace.Settings.VerifyBinds prints the whole resolution, stale
	 * lines included, so this paragraph can be checked rather than believed.
	 *
	 * STILL APPEND-ONLY BELOW THIS POINT. The .ini survives a renumber; the in-memory table is what
	 * would tear if two builds disagreed about it mid-session, and there is no reason to find out.
	 *
	 * =============================================================================================
	 * *** SPEC v31 §1 — THE STOW STATE IS GONE. 1 IS PISTOL, 2 IS SMG, 3 IS KNIFE. ***
	 * =============================================================================================
	 *
	 * The owner's words: "Revert the knife changes (no dual wielding). [...] 1 is pistol, 2 is smg,
	 * 3 is knife." Reverting dual-wield makes the knife a WEAPON SLOT again rather than a permanent
	 * off-hand blade, so v29 §5's "stow the guns" state has nothing left to mean — putting the guns
	 * away and taking the knife out became one action again. Three weapons, three keys:
	 *
	 *     key 1   EquipGun    ->  the PISTOL                  -> normal speed
	 *     key 2   EquipSmg    ->  the SMG                     -> normal speed
	 *     key 3   EquipKnife  ->  the KNIFE, guns away        -> the v12 §3 speed boost, and a
	 *                                                            35% shorter pullout (v31 §1)
	 *
	 * WHICH IS v29 §5's LAYOUT ROTATED, AND THAT IS WHY ALL THREE ConfigIds HAD TO MOVE — see the
	 * table in TraceInputActions::All() for the full argument. Every key changed meaning, so every
	 * saved line had to be dropped: "StowGuns" -> "KnifeSlot", "EquipPistol" -> "PistolSlot",
	 * "EquipSmg" -> "SmgSlot". This is the fourth migration of this kind in this file ("Boost" ->
	 * "Parry", "Parry"/"Pass" -> "ParryKeys"/"ThrowCore", then v29 §5's pair) and it works the same
	 * way each time: Save() writes a line for EVERY action, RefreshFromConfig honours a saved line
	 * over this table, and a line naming an id the table no longer has is DISCARDED by the parse loop
	 * exactly as `SwapWeapon=F` and `Boost=E` are. Without the moves the new layout would land for
	 * nobody who has ever opened the options screen — a returning player would press 1 and get a stow
	 * state that no longer exists. Trace.Settings.VerifyBinds prints every dropped line by name.
	 *
	 * THE THREE C++ SPELLINGS ABOVE ARE HISTORICAL and are kept for one reason only: this enum is the
	 * INDEX into UTraceUserSettings::Bindings and into TraceInputActions::All(), which are matched
	 * 1:1 by a static_assert, so renaming an enumerator is free but MOVING or REMOVING one is not.
	 * Renaming them would also break Source/Trace/UI/TraceOptionsMenu.cpp, which names them by hand.
	 * The MEANING has now moved twice and the spelling has never moved; read the ConfigId and the
	 * DisplayName in TraceInputActions::All(), which are the two strings that reach a player. Note in
	 * particular that EquipKnife means the knife AGAIN, as it did before v29 §5 — the one enumerator
	 * in this file whose name has come back into agreement with its job.
	 */
	EquipKnife,
	EquipGun,

	/**
	 * SPEC v14 §5 — "Activated abilities bind to E by default, rebindable" and "Mace's suspend needs
	 * its own bind (V, per the doc)."
	 *
	 * APPENDED, never inserted, for the reason EquipKnife's comment gives above: ETraceInputAction is
	 * the index into UTraceUserSettings::Bindings, so anything inserted higher renumbers every action
	 * in the runtime table.
	 *
	 * THE CONFIG IDS ARE LOAD-BEARING AND ARE NOT FREE TO RENAME. "Ability" and "AbilitySecondary"
	 * are the exact strings ATraceHUD's ability row and UTraceAbilityInputRelay already look up (both
	 * were written against a table that did not yet contain these rows, and both print / poll the
	 * documented default until it does). Changing either string silently re-orphans them.
	 *
	 * TWO ACTIONS, NOT ONE. Ability is a PRESS (the activated ability, and Mace's spike reactivation);
	 * AbilitySecondary is a HOLD (Mace suspends only while it is down). An Enhanced Input Boolean
	 * action carries no payload that could distinguish them, and the two have different trigger
	 * events bound in ATracePlayerController.
	 */
	Ability,
	AbilitySecondary,

	/**
	 * SPEC v16 §1 — "R to reload".
	 *
	 * A PROPER REBINDABLE ACTION AND NOT A HARDCODED EKeys::R, which is the whole reason this row
	 * exists rather than a `case EKeys::R:` somewhere in the controller. Everything else a player
	 * touches in this game is on this table and therefore on the options screen; a reload key that
	 * was not would be the one control they could not change, and the one nobody could find.
	 *
	 * APPENDED, never inserted — ETraceInputAction is the index into UTraceUserSettings::Bindings, so
	 * anything placed higher renumbers every action below it. See EquipKnife's comment for what that
	 * costs and why the .ini survives it anyway (it is matched by ConfigId STRING, never by position).
	 *
	 * The RELOAD VERB itself is UTraceWeaponComponent::RequestReload, which is also what the
	 * AUTOMATIC reload uses, so the bind and the empty-clip case cannot drift apart. R is not the only
	 * way to reload and deliberately so: spec v16 §1 asks for both.
	 */
	Reload,

	/**
	 * SPEC v26 §1 — "Make parry and pull core two separate binds in the settings menu."
	 *
	 * THE TURNOVER CORE-PULL, AS ITS OWN ACTION. Spec v25 §2 put it on right mouse alongside the
	 * parry and resolved the overlap by an argument rather than a bind: the two gates are exact
	 * opposites on "am I carrying the Core", so one press could be delivered to both verbs and at
	 * most one could accept. That reasoning is still true and is still in
	 * ATracePlayerController::OnParryStarted — but it is now only a TIEBREAK, used when a player has
	 * deliberately put both actions on one key. By default they are two keys and neither knows about
	 * the other.
	 *
	 * DEFAULT F, and the collision check is the only decision there was. Claimed already: WASD,
	 * Space, LeftCtrl, LeftShift, mouse1, mouse2, Tab, 1, 2, E, V, R. F is free — spec v15 §5 vacated
	 * it when it deleted the SwapWeapon toggle — and "F to grab the thing you are looking at" is the
	 * strongest convention this genre has. Q was the other candidate (parry vacated it in v25 §7) and
	 * was passed over because the pull is a HOLD performed while hovering the Core, and F sits under
	 * the index finger that is already on D.
	 *
	 * APPENDED, never inserted, for the reason EquipKnife's comment gives at length: ETraceInputAction
	 * is the index into UTraceUserSettings::Bindings, so anything placed higher renumbers every action
	 * below it. The saved .ini survives a renumber anyway (it is matched by ConfigId STRING), but the
	 * in-memory table is what would tear if two builds disagreed about it mid-session.
	 *
	 * NOTHING OUTSIDE THIS FILE LOOKS THE ConfigId UP BY STRING, unlike "Ability", "AbilitySecondary"
	 * and "Reload" — so "PullCore" is free to be renamed later if a better name appears. It is
	 * documented here so the next reader does not have to prove it again.
	 *
	 * A RETURNING PLAYER GETS THIS ROW FOR FREE AND LOSES NOTHING. There has never been a "PullCore"
	 * line in anybody's TraceUserSettings.ini, so RefreshFromConfig finds no override and seeds the
	 * shipped default. Their existing ParryPull line still lands on the parry, exactly as before.
	 */
	PullCore,

	/**
	 * *** SPEC v28 §10 — THE MELEE BIND. "Melee should be bound to right click by default." ***
	 *
	 * THIS ROW IS THE INTEGRATION SEAM BETWEEN §3 AND §10 AND IT DID NOT EXIST IN EITHER SLICE.
	 * §10 shipped the whole verb (TraceMelee::HandleMeleeInput) and could not bind it, because this
	 * table is §3's file. §3 VACATED the right mouse button for it (see Default_Parry's note — the
	 * "ParryPull" -> "ParryKeys" migration is what takes the button off the parry) and did not add
	 * the row, because the verb was §10's file. Both slices said so plainly in their hand-off notes.
	 * Without this enumerator the button is bound to nothing at all and melee is reachable only from
	 * the console — which is exactly the state the integrator found, and this is the fix.
	 *
	 * APPENDED, never inserted, for the reason EquipKnife's comment gives at length: ETraceInputAction
	 * is the index into UTraceUserSettings::Bindings, so anything placed higher renumbers every action
	 * below it. A returning player has never had a "Melee" line, so RefreshFromConfig finds no
	 * override and seeds the shipped default; nothing they have bound moves.
	 *
	 * ITS EXCLUSION GROUP IS NotCarrying, AND THAT IS MEASURED RATHER THAN ASSUMED. The bind carries
	 * two verbs and BOTH refuse while this pawn is the carrier:
	 *   - the swing, at UTraceWeaponComponent::CanSwing's `if (Character->IsCarrier())` gate, which
	 *     refuses with ETraceMeleeRefusal::Carrying — the attacking half of the carrier's bargain;
	 *   - the Core pull, at ATraceCore::CanPullNow, which requires an empty-handed puller.
	 * So a melee key is dead while carrying, exactly as fire is, and it may legally share a key with
	 * a Carrying-only action (the parry, the throw). It may NOT share with FIRE or PULL CORE, which
	 * is the answer we want: the pull already rides this button under §10's precedence rule, so
	 * putting PullCore on it as well would be two dispatches of one verb.
	 */
	Melee,

	/**
	 * *** SPEC v29 §5 — THE THIRD WEAPON KEY. "Pressing 2 pulls out pistol, 3 pulls out smg." ***
	 *
	 * Until this row existed there were exactly TWO weapon binds and the SMG was reachable only by
	 * the console or by a swap toggle nothing binds. The keybind page's rebind list IS
	 * TraceInputActions::All() walked in order, so an action that is not in this enum is an action
	 * the player can neither press nor rebind however well it is wired up in the controller — the
	 * same sentence the EquipKnife, Ability, Reload, PullCore and Melee rows all carry.
	 *
	 * APPENDED, never inserted, for the reason EquipKnife's comment gives at length: this enum is
	 * the index into UTraceUserSettings::Bindings, so anything placed higher renumbers every action
	 * below it.
	 *
	 * *** SPEC v31 §1 MOVED THIS ROW TO THE 2 KEY AND HAD TO MIGRATE ITS ConfigId TO DO IT. *** The
	 * paragraph that used to sit here said "a returning player has never had an EquipSmg line, so
	 * nothing they have bound moves" — true when v29 §5 introduced the row, and false the moment it
	 * shipped. Everyone who has opened the options screen since now has `EquipSmg=Three` saved, and
	 * RefreshFromConfig would have honoured it and pinned the SMG to 3 while every label said 2. The
	 * id is now "SmgSlot", which has never been in anybody's file, so the stale line is dropped and
	 * the 2 key is seeded. That is the general lesson: a NEW row's id is safe exactly once.
	 *
	 * `Match`, not `NotCarrying`, and that is the same answer the other two weapon rows give. The
	 * weapon SELECTOR is legal while carrying — UTraceWeaponComponent::RequestEquip refuses a
	 * carrier's swap with ETraceMeleeRefusal::Carrying, but that is a refusal at the verb, not a
	 * state in which the key means something else. Marking it NotCarrying would let it share a key
	 * with the throw, and "3 throws the Core sometimes" is not a bind anybody wants.
	 */
	EquipSmg,

	/**
	 * *** SPEC v31 §5 — THE KNIFE FLOURISH. "bind the F key to inspect", as a REBINDABLE ACTION. ***
	 *
	 * The spec is explicit that this is "a new rebindable action in the settings page like every
	 * other action", and that sentence is the whole reason this enumerator exists rather than a
	 * `case EKeys::F:` in the controller. The keybind page's rebind list IS TraceInputActions::All()
	 * walked in order, so an action that is not in this enum is an action the player can neither
	 * press nor rebind however well it is wired up — the same sentence the EquipKnife, Ability,
	 * Reload, PullCore, Melee and EquipSmg rows all carry.
	 *
	 * APPENDED, never inserted, for the reason EquipKnife's comment gives at length: this enum is the
	 * index into UTraceUserSettings::Bindings, so anything placed higher renumbers every action below
	 * it. A returning player has never had an "Inspect" line, so RefreshFromConfig finds no override
	 * and seeds the shipped default; nothing they have bound moves.
	 *
	 * =============================================================================================
	 * *** F WAS ALREADY TAKEN. THE PULL CORE MOVED TO G, AND THAT IS A REAL COST. SAY IT PLAINLY. ***
	 * =============================================================================================
	 *
	 * ETraceInputAction::PullCore has shipped on F since spec v26 §1, on the argument — still a good
	 * one — that "F to grab the thing you are looking at" is the strongest convention this genre has.
	 * The owner has now asked for F twice, for two different verbs, and only one of them can have it.
	 *
	 * BOTH ARE `NotCarrying`, SO THEY CANNOT SIMPLY SHARE THE KEY. UTraceUserSettings::
	 * ActionsMayShareAKey is one bitwise AND over the exclusion groups, and two actions may share a
	 * key only when their masks are DISJOINT — the reasoning spec v28 §3b set up so that FIRE and
	 * THROW could share mouse 1. Inspect needs a knife in hand; the pull needs empty hands and a
	 * loose Core in front of you; both of those are states you are in while NOT carrying, and they
	 * overlap. One press would fill a pull ring AND start a 3.2 s flourish, which is exactly the
	 * "two dispatches of one press" the Melee row's comment refuses.
	 *
	 * SO F GOES TO THE VERB THE OWNER JUST NAMED, and the pull takes G — the nearest free key under
	 * the same finger (claimed: WASD, Space, LeftCtrl, LeftShift, mouse1, mouse2, Q + thumb mouse,
	 * Tab, 1, 2, 3, E, V, R, and now F). WHAT SOFTENS THE COST: the pull is NOT losing its only
	 * route. Spec v28 §10's precedence rule already dispatches it from the MELEE button
	 * (TraceMelee::HandleMeleeInput -> "CORE PULL (the circle is on screen)"), so a player who never
	 * finds G can still pull with right mouse exactly as they do today. G is the second route, not
	 * the only one.
	 *
	 * THE MIGRATION IS THE USUAL ONE: "PullCore" -> "PullCoreKey", so a returning player's saved
	 * `PullCore=F` line names an id this table no longer has and is DISCARDED by RefreshFromConfig's
	 * parse loop, leaving them on the shipped G. Without the id move the default change would be
	 * invisible to everyone who has ever opened the options screen and they would keep a PULL and an
	 * INSPECT on the same key — the precise collision this is avoiding.
	 *
	 * THE ONE-LINE REVERT, if the owner would rather keep the pull on F: put Default_PullCore back to
	 * EKeys::F, put Default_Inspect on EKeys::G, and change the two ConfigIds back. Nothing else in
	 * the project reads either string.
	 *
	 * `NotCarrying`, and it is measured rather than assumed: TraceKnifeView::RequestInspect refuses a
	 * carrier outright, because the pack's own loadout table makes the Core a TWO-HAND CRADLE and a
	 * knife flourish with both hands on the objective is not a pose that exists. So the flourish key
	 * is genuinely dead while carrying, exactly as fire is, and it may legally share a key with a
	 * Carrying-only action (the parry, the throw) if a player wants that.
	 */
	Inspect,

	Count UMETA(Hidden)
};

/**
 * *** SPEC v28 §3b — WHEN TWO ACTIONS ARE ALLOWED TO SHARE ONE KEY. ***
 *
 * Verbatim: "it will not let me bind throw core to the same button as fire but I should be able to do
 * so [...] Throwing the Core only happens while carrying; firing only happens while NOT carrying —
 * they are mutually exclusive, so the conflict check must reason about STATE, not just about the key.
 * Define the exclusion groups explicitly."
 *
 * THIS ENUM IS THAT DEFINITION. Every action in the table above carries a mask of the states it can
 * actually DO something in, and UTraceUserSettings::ActionsMayShareAKey is one bitwise AND: two
 * actions conflict if and only if their masks INTERSECT. Disjoint masks mean the two verbs can never
 * both be legal at the same instant, so one key can carry both and the authoritative gates decide
 * which one accepts — which is not a new idea here, it is exactly the argument
 * ATracePlayerController::OnParryStarted has made about parry and the Core-pull since spec v25 §2.
 *
 * WHY "MENU" IS A BIT AND HOLDS NOTHING TODAY. The spec names two axes — "carrying vs not carrying,
 * menu vs match" — and the second one has a real answer: NO action in this table is live while the
 * settings/pause overlay is up. FTraceOptionsMenu polls its own keys (Escape, Enter, the arrows,
 * Backspace), none of which are rebindable, and ATracePlayerController::SetGameInputSuppressed makes
 * every gameplay handler early-return for as long as the overlay is open. So a match action and the
 * menu can never contend, the bit is unoccupied, and that is the finding rather than an omission. It
 * exists so that a menu action added later (a chat key, say) states its context in the same place as
 * everything else and gets the correct answer without anybody rewriting the check.
 */
enum class ETraceInputStates : uint8
{
	None        = 0,
	/** Live while this pawn is the Core carrier: the throw, the parry. */
	Carrying    = 1 << 0,
	/** Live while this pawn is NOT the carrier: shooting, the turnover pull. */
	NotCarrying = 1 << 1,
	/** Live while a menu/overlay owns input. See the note above: nothing holds this today. */
	Menu        = 1 << 2,

	/** Everything a player can do in a match regardless of who is holding the Core. */
	Match       = Carrying | NotCarrying,
};
ENUM_CLASS_FLAGS(ETraceInputStates);

/** "CARRYING", "NOT CARRYING", "CARRYING+NOT CARRYING", "NONE". For logs and the options page. */
TRACE_API FString LexTraceInputStates(ETraceInputStates States);

/** Static description of one action: its stable config id and the label the options screen shows. */
struct FTraceInputActionInfo
{
	ETraceInputAction Action;
	/** Stable, never localised, never renamed — this is what ends up in the .ini. */
	const TCHAR* ConfigId;
	const TCHAR* DisplayName;
	/** The shipped PRIMARY key (slot 0). May return an invalid FKey: "ships unbound" is supported. */
	FKey (*DefaultKey)();
	/**
	 * SPEC v28 §3c — the shipped SECOND key (slot 1), or an invalid FKey for "only one by default".
	 *
	 * A separate function pointer rather than an array for the same reason DefaultKey is a function at
	 * all: EKeys' statics are built during module startup, so a namespace-scope FKey cannot be trusted
	 * to exist yet. Only one row uses it today — §3d's parry, which ships on Q AND the thumb mouse
	 * button — and every other row names Default_None, which is honest and greppable.
	 */
	FKey (*DefaultKeyAlt)();
	/** SPEC v28 §3b — the exclusion group. See ETraceInputStates. */
	ETraceInputStates States;
};

namespace TraceInputActions
{
	/** The table, indexed by ETraceInputAction. Always ETraceInputAction::Count entries. */
	TRACE_API const TArray<FTraceInputActionInfo>& All();

	TRACE_API const FTraceInputActionInfo& Info(ETraceInputAction Action);
}

/**
 * SPEC v29 §3 — one axis-aligned bar of the crosshair, in SCREEN pixels: (X, Y, W, H).
 *
 * A plain struct at file scope and NOT a USTRUCT: it is never replicated, never serialised and never
 * seen by Blueprint, and dragging UHT into it would buy nothing. It exists so that the two things
 * that draw this crosshair — ATraceHUD::DrawAimReticle in a match, and the live preview on the
 * settings page — share ONE definition of the shape instead of two that agree until somebody edits
 * one of them. A preview that can disagree with the thing it previews is worse than no preview.
 */
struct FTraceCrosshairBar
{
	float X = 0.f;
	float Y = 0.f;
	float W = 0.f;
	float H = 0.f;
};

/** Bars a crosshair can have: four arms plus the centre dot. */
static constexpr int32 TraceCrosshairMaxBars = 5;

/**
 * Fires whenever any control setting changes.
 *
 * Declared at file scope rather than inside the UCLASS: UHT parses the class body, and a delegate
 * macro in there is one more construct it has to be right about for no gain.
 */
DECLARE_MULTICAST_DELEGATE(FTraceUserSettingsChanged);

/**
 * Player-owned control settings.
 *
 * Everything here is read live — nothing caches a copy — so a change made in the pause menu is felt
 * on the very next mouse event without a rebuild, a travel or a respawn.
 */
UCLASS(config = TraceUserSettings)
class TRACE_API UTraceUserSettings : public UObject
{
	GENERATED_BODY()

public:
	UTraceUserSettings();

	/** The one accessor. Mutable because the options screen writes through it. */
	static UTraceUserSettings& Get();

	/**
	 * The change broadcast.
	 *
	 * ATracePlayerController listens and rebuilds its Enhanced Input mapping context. Nothing else
	 * needs to: sensitivity and inversion are both expressed as modifiers ON that context, so one
	 * rebuild is the entire application path.
	 */
	static FTraceUserSettingsChanged& OnChanged();

	// ---------------------------------------------------------------------------------------------
	// Mouse
	// ---------------------------------------------------------------------------------------------

	/** Shipped defaults, in one place, so the property, ResetToDefaults and IsAtDefaults cannot drift. */
	static constexpr float DefaultSensitivity = 1.50f;
	static constexpr float DefaultSensitivityYScale = 1.00f;

	/**
	 * Degrees of view rotation per unit of raw mouse delta. This IS the Scalar modifier value on the
	 * Look mappings; there is no other scaling anywhere in the chain — Config/DefaultInput.ini sets
	 * bEnableLegacyInputScales=False, so APlayerController::AddYawInput/AddPitchInput multiply by
	 * exactly 1.0 and this number is the whole story.
	 *
	 * LOWERED FROM THE SHIPPED 2.5 TO 1.5, a 40% cut, because the player said the build was too
	 * sensitive.
	 *
	 * 40% and not more, on purpose. The report was "a BIT too sensitive", and the vertical axis was
	 * ALSO inverted for them at the time (see bInvertMouseY) — fighting an inverted axis makes any
	 * sensitivity feel twitchy, so some unknown share of that complaint belongs to the inversion and
	 * is fixed by fixing it. Cutting all the way to 1.0 risked trading "too fast" for "unplayably
	 * slow", which is a worse first impression and a harder one to diagnose. The slider spans
	 * 0.10 to 4.00, so the player can put it anywhere; this only has to be a good starting point.
	 */
	UPROPERTY(config)
	float MouseSensitivity = DefaultSensitivity;

	/**
	 * Extra multiplier applied to the VERTICAL axis only, on top of MouseSensitivity.
	 *
	 * Separating the axes is close to free here — the two mappings already carry their own modifier
	 * lists — and a lot of players want pitch slower than yaw because the pitch range is 180 degrees
	 * while the yaw range is unbounded.
	 */
	UPROPERTY(config)
	float MouseSensitivityYScale = DefaultSensitivityYScale;

	/**
	 * Invert the vertical axis. FALSE is standard FPS convention: push the mouse forward, look up.
	 *
	 * THE BUG THIS FIXES. Raw EKeys::MouseY is positive when the mouse moves UP
	 * (FSceneViewport::OnMouseMove does `MouseDelta.Y -= CursorDelta.Y`, negating the screen-space
	 * delta). With bEnableLegacyInputScales=False, APlayerController::AddPitchInput adds
	 * `Val * 1.0` straight onto RotationInput.Pitch, and positive pitch looks UP. So mouse-up should
	 * arrive positive and be added as-is.
	 *
	 * The shipped mapping context put a Negate modifier on MouseY, which is correct ONLY under the
	 * legacy input scales, where InputPitchScale_DEPRECATED is -2.5 and supplies its own inversion.
	 * With legacy scales off, the Negate was the only sign flip left in the chain, so pushing the
	 * mouse forward looked DOWN. That is exactly what the player reported, and it is why the default
	 * had to change rather than just gaining a toggle.
	 */
	UPROPERTY(config)
	bool bInvertMouseY = false;

	/** Clamps used by both the slider and any value arriving from a hand-edited .ini. */
	static constexpr float MinSensitivity = 0.10f;
	static constexpr float MaxSensitivity = 4.00f;
	static constexpr float MinSensitivityYScale = 0.25f;
	static constexpr float MaxSensitivityYScale = 2.00f;

	/** The scalar the Look mapping should use for yaw. Always finite and inside the clamp. */
	float GetLookScaleX() const;

	/** The scalar for pitch, sign included: negative means "inverted". */
	float GetLookScaleY() const;

	// ---------------------------------------------------------------------------------------------
	// Bindings
	// ---------------------------------------------------------------------------------------------

	/**
	 * SPEC v28 §3c — HOW MANY KEYS ONE ACTION MAY CARRY. "Up to TWO keybinds per action, both
	 * editable in the settings page."
	 *
	 * TWO AND NOT N. The number is in the note, and it is also the number the options row can draw
	 * legibly: the keybind row has one label and a value column, and two chips fit that column at
	 * 720p where three do not. Every loop below is written over this constant rather than over a
	 * literal 2, so raising it is one edit here plus one wider value column — but nothing in the
	 * save format, the loader or the conflict check would have to change.
	 */
	static constexpr int32 MaxKeysPerAction = 2;

	/**
	 * Serialised bindings, one "ConfigId=KeyName" string per entry.
	 *
	 * A TArray<FString> rather than a TMap<FName, FKey> on purpose. Config TMaps of USTRUCT values
	 * are the sort of thing that works until it silently does not, and FKey round-tripping through
	 * ExportText adds a second failure mode on top. A flat list of "Jump=SpaceBar" is legible in the
	 * .ini, hand-editable, and cannot half-load.
	 *
	 * *** SPEC v28 §3c: THE VALUE IS NOW A COMMA-SEPARATED LIST OF UP TO MaxKeysPerAction KEYS. ***
	 *
	 *     Parry=Q,ThumbMouseButton
	 *     Jump=SpaceBar
	 *     Fire=LeftMouseButton,None
	 *
	 * THIS FORMAT IS BACKWARD COMPATIBLE BY CONSTRUCTION, which is why the two-key feature costs no
	 * ConfigId migration of its own. A line written by any previous build has no comma, so it parses
	 * as "slot 0 = this key, slot 1 = empty" — exactly what that build meant. Nothing has to detect a
	 * version, and a hand-edited file with three keys on a line is truncated with a warning rather
	 * than rejected.
	 *
	 * Not the runtime source of truth — Bindings below is. This is only what hits the disk.
	 */
	UPROPERTY(config)
	TArray<FString> KeyBindings;

	/**
	 * The PRIMARY key for @p Action (slot 0). Returns an invalid FKey if that slot is unbound.
	 *
	 * Deliberately still the whole answer for every caller that only wants something to PRINT — the
	 * HUD's ability chip, the pull ring's "HOLD [F]" caption, the movement tutorial. A row with two
	 * binds shows its first one there, which is what those captions have always meant.
	 */
	FKey GetKey(ETraceInputAction Action) const;

	/** The key in @p Slot (0 .. MaxKeysPerAction-1). Invalid FKey for an empty slot or a bad index. */
	FKey GetKey(ETraceInputAction Action, int32 Slot) const;

	/** Every VALID key bound to @p Action, in slot order. Empty when the action is fully unbound. */
	void GetKeys(ETraceInputAction Action, TArray<FKey>& OutKeys) const;

	/** True when @p Key is in ANY of @p Action's slots. The "is this action's button down" question. */
	bool ActionUsesKey(ETraceInputAction Action, const FKey& Key) const;

	/**
	 * SPEC v28 §3b — may these two actions hold the same key?
	 *
	 * TRUE when their ETraceInputStates masks are disjoint, i.e. there is no single instant at which
	 * both verbs could be legal. Throw-core (Carrying) and fire (NotCarrying) are the note's own
	 * example and answer TRUE; fire and the Core-pull are both NotCarrying and answer FALSE.
	 *
	 * An action is never in conflict with itself (the caller wants "may B keep this key while A takes
	 * it", and A == B is the slot logic's business, not the conflict check's).
	 */
	static bool ActionsMayShareAKey(ETraceInputAction A, ETraceInputAction B);

	/**
	 * Binds @p Key to @p Action's primary slot, taking it only from actions that GENUINELY conflict.
	 *
	 * Stealing rather than refusing is the behaviour every shooter has: a player who binds Dash to
	 * Shift while Shift is already Crouch means "Dash is Shift now", and being told "that key is
	 * taken" with no way to proceed is the single most annoying thing an options screen can do. The
	 * stolen action is left visibly UNBOUND so nothing is lost silently.
	 *
	 * *** SPEC v28 §3b LIMITS WHO GETS STOLEN FROM, AND THAT IS THE ITEM. *** The owner's report is
	 * "it will not let me bind throw core to the same button as fire" — and from the player's side
	 * that is precisely what unconditional stealing looks like: they put the throw on mouse 1 and
	 * FIRE went blank, so the bind "did not work". Now a key is only taken from an action whose
	 * exclusion group overlaps this one's; two actions that can never be legal at the same instant
	 * simply both keep it. See ActionsMayShareAKey.
	 */
	void SetKey(ETraceInputAction Action, const FKey& Key);

	/** As above, into an explicit slot. Slot 1 is spec v28 §3c's second bind. */
	void SetKey(ETraceInputAction Action, int32 Slot, const FKey& Key);

	/**
	 * Explicitly unbinds @p Action — every slot.
	 *
	 * Separate from SetKey(Action, FKey()) on purpose. SetKey REFUSES an invalid key, because an
	 * invalid key is also what an unparseable .ini line produces, and a load path must never be able
	 * to silently wipe a binding. Unbinding is a deliberate act and gets its own entry point.
	 */
	void ClearKey(ETraceInputAction Action);

	/** Unbinds ONE slot. What Backspace on the options page does to the chip the player has selected. */
	void ClearKey(ETraceInputAction Action, int32 Slot);

	/** "Q  /  THUMB MOUSE BUTTON", or "UNBOUND". The whole binding, for a log line or a caption. */
	FString DescribeBinding(ETraceInputAction Action) const;

	/** Restores the shipped default for every action AND for the mouse. */
	void ResetToDefaults();

	/** True when nothing has been changed from the shipped defaults. Drives the reset row's dimming. */
	bool IsAtDefaults() const;

	/**
	 * Writes to Saved/Config/<Platform>/TraceUserSettings.ini and flushes.
	 *
	 * Called after every single change rather than on close. The whole point of this feature is that
	 * the player stops having to redo it every launch, and this build is normally terminated by a
	 * kill rather than a clean shutdown.
	 */
	void Save();

	/**
	 * Populates the runtime binding table from KeyBindings, filling any gap with the default.
	 *
	 * Safe to call repeatedly. Called lazily by Get() on first use, and by ResetToDefaults.
	 */
	void RefreshFromConfig();

	/** True if @p Key is something a player could sensibly bind: a real button, not an axis. */
	static bool IsBindableKey(const FKey& Key);

	/** "SPACE BAR" / "LEFT SHIFT" / "UNBOUND". Upper case, for the options screen. */
	static FString DescribeKey(const FKey& Key);

	// ---------------------------------------------------------------------------------------------
	// SPEC v29 §3 — THE CROSSHAIR
	//
	// "Add a page in settings to customize the crosshair."
	//
	// WHY THESE LIVE HERE AND NOT IN UTraceSettings. Same argument the file header makes about mouse
	// sensitivity: a crosshair is per-machine taste, written at runtime by the player, and must never
	// appear in a diff. UTraceSettings is the designer's checked-in table and is exactly the wrong
	// home. The one crosshair number that IS a designer's decision — ThirdPersonCrosshairScale, how
	// much bigger the cross gets in the carrying view — stays over there, because it is a statement
	// about the third-person camera rather than about a player's eyesight, and the two MULTIPLY (see
	// ATraceHUD::DrawAimReticle).
	//
	// *** THE SHIPPED DEFAULTS ARE THE EXACT NUMBERS DrawAimReticle ALREADY DREW. *** 11 px of arm,
	// 2.5 px of bar, a 5 px gap, 94% opaque white, dot on, outline on. A player who never opens this
	// page must get a pixel-identical crosshair to the one they had before it existed, and that is
	// checkable: Trace.Crosshair.Status prints the live geometry, and at defaults it prints what the
	// literals used to produce.
	//
	// EVERY NUMBER IS IN 1080p-REFERENCE PIXELS, exactly like the literals it replaced — the HUD
	// multiplies by UIScale (ViewH / 1080) and by the third-person scale at draw time. Storing screen
	// pixels instead would give a player who changes resolution a different crosshair.

	/** Shipped defaults, in one place, so the property, the reset and the at-defaults test cannot drift. */
	static constexpr float DefaultCrosshairSize      = 11.00f;
	static constexpr float DefaultCrosshairThickness = 2.50f;
	static constexpr float DefaultCrosshairGap       = 5.00f;
	static constexpr float DefaultCrosshairOpacity   = 0.94f;
	static constexpr int32 DefaultCrosshairColor     = 0;      // WHITE — see CrosshairPaletteColor
	static constexpr bool  bDefaultCrosshairCenterDot = true;
	static constexpr bool  bDefaultCrosshairOutline   = true;

	/**
	 * Clamps, used by the settings rows AND by the accessors below.
	 *
	 * The accessors clamp too, and that is not belt-and-braces: this file is a hand-editable .ini, and
	 * a CrosshairThickness of 4000 there would otherwise paint the whole viewport white with no way to
	 * get back except by finding the file. The floor matters as much as the ceiling — a size of 0
	 * would be a crosshair that does not exist, which reads as the game being broken rather than as a
	 * setting being at its minimum. That is what the CENTRE DOT toggle is for.
	 */
	static constexpr float MinCrosshairSize      = 2.00f;
	static constexpr float MaxCrosshairSize      = 24.00f;
	static constexpr float MinCrosshairThickness = 1.00f;
	static constexpr float MaxCrosshairThickness = 8.00f;
	static constexpr float MinCrosshairGap       = 0.00f;
	static constexpr float MaxCrosshairGap       = 20.00f;
	static constexpr float MinCrosshairOpacity   = 0.20f;
	static constexpr float MaxCrosshairOpacity   = 1.00f;

	/** Length of ONE arm, in 1080p-reference pixels. The four arms are drawn from the gap outwards. */
	UPROPERTY(config)
	float CrosshairSize = DefaultCrosshairSize;

	/** Bar width, in 1080p-reference pixels. Also the side of the centre dot — they are one bar. */
	UPROPERTY(config)
	float CrosshairThickness = DefaultCrosshairThickness;

	/** Empty space between the exact aim point and the inner end of each arm, reference pixels. */
	UPROPERTY(config)
	float CrosshairGap = DefaultCrosshairGap;

	/**
	 * Index into the named palette below.
	 *
	 * AN INDEX AND NOT AN FLinearColor, deliberately, and it is the one place this page is less than a
	 * full colour picker. Three reasons, in order: the options overlay is drawn through
	 * AHUD::DrawRect from inside DrawHUD and has no colour-picker widget to offer (there is no Slate
	 * here — see the header of UI/TraceOptionsMenu.h); every row on that page is driven by arrow keys
	 * and a drag across a track, which is a one-dimensional control and therefore wants a
	 * one-dimensional value; and an index round-trips through the .ini exactly, where a struct
	 * property is one more thing that can half-load.
	 *
	 * The cost is real and is stated rather than hidden: a player cannot dial in an arbitrary RGB.
	 * If that is wanted later, the honest upgrade is three sliders (R, G, B) writing an FLinearColor,
	 * not a text field.
	 */
	UPROPERTY(config)
	int32 CrosshairColorIndex = DefaultCrosshairColor;

	/**
	 * 0..1, multiplied into the ink's alpha.
	 *
	 * SEPARATE FROM THE COLOUR, not folded into the palette entry's A. The palette is eight fixed
	 * hues and the opacity is a continuous slider; one field carrying both would mean picking a new
	 * colour silently threw away the opacity the player had set.
	 *
	 * *** THE OUTLINE'S ALPHA IS DERIVED FROM THIS, NOT PINNED. *** See GetCrosshairOutlineColor.
	 */
	UPROPERTY(config)
	float CrosshairOpacity = DefaultCrosshairOpacity;

	/**
	 * The square at the exact aim point.
	 *
	 * Worth a toggle rather than being implied by the gap: a player who wants a wide gap for target
	 * visibility usually still wants the dot (it is the pixel the bullet goes to), and a player who
	 * finds the dot noisy over a busy floor wants it gone without giving up the arms.
	 */
	UPROPERTY(config)
	bool bCrosshairCenterDot = bDefaultCrosshairCenterDot;

	/**
	 * The one-pixel dark surround.
	 *
	 * ON by default and it should stay on for almost everybody — the arena is a black floor under
	 * saturated cyan and amber neon, and an unoutlined white cross vanishes the moment it crosses a
	 * lit edge. It is a toggle because it is the one thing on this page a player might turn OFF for a
	 * reason (a hard-edged single-colour cross is easier to see against a flat background, and some
	 * players genuinely track it better), and because a setting nobody can turn off is not a setting.
	 */
	UPROPERTY(config)
	bool bCrosshairOutline = bDefaultCrosshairOutline;

	/** How many entries the colour palette has. The COLOUR row's range is 0 .. this - 1. */
	static int32 NumCrosshairColors();

	/** Palette entry @p Index, opaque. Out-of-range clamps rather than returning black. */
	static FLinearColor CrosshairPaletteColor(int32 Index);

	/** "WHITE", "CYAN", "RED"... Upper case, for the options row. */
	static FString DescribeCrosshairColor(int32 Index);

	// ---- The accessors the HUD reads. Nothing outside this class touches the raw fields. --------
	//
	// Every one of them clamps. A hand-edited .ini is a supported way to configure this game (the
	// whole KeyBindings format above is designed for it) and it is therefore also a supported way to
	// arrive here with nonsense.

	float GetCrosshairSize() const;
	float GetCrosshairThickness() const;
	float GetCrosshairGap() const;
	float GetCrosshairOpacity() const;

	/** The palette colour with the opacity already folded into A. The fill the HUD draws with. */
	FLinearColor GetCrosshairColor() const;

	/**
	 * The surround's colour, or a fully transparent one when the outline is off.
	 *
	 * *** ITS ALPHA IS RELATIVE TO THE CROSSHAIR'S OWN. *** The shipped pair was ink alpha 0.94 and
	 * shadow alpha 0.80; the shadow is therefore 0.851 of the ink, and that RATIO is what is stored.
	 * Pinning the shadow at a literal 0.80 would mean a player who dropped the crosshair to 25%
	 * opacity got a near-invisible cross inside a fully-present black outline — the outline louder
	 * than the thing it exists to outline. Standing rule: a value that MODIFIES a base is stored
	 * relative to that base.
	 */
	FLinearColor GetCrosshairOutlineColor() const;

	/**
	 * *** THE OUTER TIP OF THE ARMS, IN 1080p-REFERENCE PIXELS: gap + arm. ***
	 *
	 * ONE DEFINITION, because four separate things in ATraceHUD are laid out AROUND the crosshair and
	 * every one of them used to carry its own copy of the literal `(5 + 11)`:
	 *   - the third-person pass brackets, which must frame the cross rather than cut through it;
	 *   - the green throw-charge / pass-hold ring;
	 *   - the red auto-release ring inside it;
	 *   - and the crosshair itself.
	 * Those literals were correct for exactly as long as the crosshair was not a setting. The moment
	 * a player can widen the gap or lengthen the arms, a ring pinned to 16 reference pixels is a ring
	 * drawn straight through the arms — which is the same defect spec v28 §7 already fixed once for
	 * the ThirdPersonCrosshairScale knob (see TraceHUDThrowRings). Standing rule again: everything
	 * that is positioned relative to the crosshair asks the crosshair how big it is.
	 */
	float GetCrosshairArmReach() const;

	/**
	 * The crosshair's bars, in SCREEN pixels, centred on (@p CX, @p CY).
	 *
	 * THE ONE DEFINITION OF THE SHAPE. Two things draw this crosshair — the HUD in a match and the
	 * live preview on the settings page — and a preview built from its own copy of the arithmetic is
	 * a preview that will one day lie about the setting it is previewing.
	 *
	 * PIXEL SNAPPING LIVES HERE, for the reason ATraceHUD::DrawAimReticle spells out at length: at
	 * 720p a fractional rect on a half-pixel boundary is anti-aliased into a grey smudge, which is
	 * what "the crosshair looks out of focus" actually was. Integer thickness, integer lengths,
	 * integer origin, axis-aligned — and therefore no anti-aliasing at all.
	 *
	 * @param PixelScale  UIScale times any per-view scale (the third-person crosshair scale). This is
	 *                    what turns the stored 1080p-reference numbers into this frame's pixels.
	 * @param OutBars     receives up to TraceCrosshairMaxBars entries.
	 * @return how many were written: 4, or 5 when the centre dot is on.
	 */
	int32 BuildCrosshairBars(float CX, float CY, float PixelScale,
		FTraceCrosshairBar OutBars[TraceCrosshairMaxBars]) const;

	/** Puts ONLY the crosshair back to its shipped defaults, and saves. Deliberately not ResetToDefaults. */
	void ResetCrosshairToDefaults();

	/** True when every crosshair field is on its shipped default. Drives the crosshair page's reset row. */
	bool IsCrosshairAtDefaults() const;

private:
	/**
	 * Runtime table. Never serialised; KeyBindings is.
	 *
	 * FLAT, indexed by (action * MaxKeysPerAction + slot), rather than an array of a two-key struct.
	 * A struct would have to be a USTRUCT to be worth the name and would then drag UHT into a table
	 * that is deliberately not reflected; a flat array keeps SlotIndex() as the single place that
	 * knows the layout, and every loop below goes through it.
	 */
	TArray<FKey> Bindings;

	/** The one place that knows the Bindings layout. INDEX_NONE for an out-of-range action or slot. */
	static int32 SlotIndex(ETraceInputAction Action, int32 Slot);

	/** Mirrors Bindings back into KeyBindings before a save. */
	void FlattenToConfig();

	bool bLoaded = false;
};
