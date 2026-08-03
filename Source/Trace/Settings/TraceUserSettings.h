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
	Boost,
	Fire,
	Pass,
	Scoreboard,

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
