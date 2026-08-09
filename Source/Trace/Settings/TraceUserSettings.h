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
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "TraceUserSettings.generated.h"

/**
 * Every rebindable action, in the order the options screen lists them.
 *
 * Look is deliberately absent: it is the mouse, it has no discrete key, and everything a player
 * could want to change about it is a sensitivity or an inversion rather than a binding.
 *
 * Fire and Pass are BOTH listed even though the spec has mouse1 double as "pass" while carrying the
 * Core. That overload is a gameplay rule inside the weapon/character slice, not an input rule: the
 * player still deserves an explicit pass bind, and the two are free to be the same key.
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
	 * The ConfigId is new ("Parry", never "Boost"), so a TraceUserSettings.ini written by an older
	 * build carries a `Boost=E` line that RefreshFromConfig simply does not recognise and drops. That
	 * is the whole migration: no player inherits a parry bound to the old boost key by accident, and
	 * nobody has to write an upgrade path.
	 */
	Parry,
	Fire,
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

	Count UMETA(Hidden)
};

/** Static description of one action: its stable config id and the label the options screen shows. */
struct FTraceInputActionInfo
{
	ETraceInputAction Action;
	/** Stable, never localised, never renamed — this is what ends up in the .ini. */
	const TCHAR* ConfigId;
	const TCHAR* DisplayName;
	FKey (*DefaultKey)();
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
	 * Serialised bindings, one "ConfigId=KeyName" string per entry.
	 *
	 * A TArray<FString> rather than a TMap<FName, FKey> on purpose. Config TMaps of USTRUCT values
	 * are the sort of thing that works until it silently does not, and FKey round-tripping through
	 * ExportText adds a second failure mode on top. A flat list of "Jump=SpaceBar" is legible in the
	 * .ini, hand-editable, and cannot half-load.
	 *
	 * Not the runtime source of truth — Bindings below is. This is only what hits the disk.
	 */
	UPROPERTY(config)
	TArray<FString> KeyBindings;

	/** Current key for @p Action. Returns an invalid FKey if the action is deliberately unbound. */
	FKey GetKey(ETraceInputAction Action) const;

	/**
	 * Binds @p Key to @p Action, unbinding whatever else held that key.
	 *
	 * Stealing rather than refusing is the behaviour every shooter has: a player who binds Dash to
	 * Shift while Shift is already Crouch means "Dash is Shift now", and being told "that key is
	 * taken" with no way to proceed is the single most annoying thing an options screen can do. The
	 * stolen action is left visibly UNBOUND so nothing is lost silently.
	 */
	void SetKey(ETraceInputAction Action, const FKey& Key);

	/**
	 * Explicitly unbinds @p Action.
	 *
	 * Separate from SetKey(Action, FKey()) on purpose. SetKey REFUSES an invalid key, because an
	 * invalid key is also what an unparseable .ini line produces, and a load path must never be able
	 * to silently wipe a binding. Unbinding is a deliberate act and gets its own entry point.
	 */
	void ClearKey(ETraceInputAction Action);

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
	/** Runtime table, indexed by ETraceInputAction. Never serialised; KeyBindings is. */
	TArray<FKey> Bindings;

	/** Mirrors Bindings back into KeyBindings before a save. */
	void FlattenToConfig();

	bool bLoaded = false;
};
