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
