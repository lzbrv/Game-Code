// Trace — D31-PAD: the gamepad's own Enhanced Input mapping context.
//
// THE OWNER'S ASK, VERBATIM: "Add a subpage within settings for controller keybinds. Create a
// default mapping, so that if a player connects a controller with Bluetooth instead/in addition to a
// mouse or trackpad they can use a controller."
//
// -------------------------------------------------------------------------------------------------
// WHY THIS IS A SECOND CONTEXT AND NOT A FEW MORE LINES IN ApplyControlSettings
// -------------------------------------------------------------------------------------------------
// ATracePlayerController::ApplyControlSettings builds ONE UInputMappingContext out of
// UTraceUserSettings::GetKeys(), tearing it down and rebuilding it on every settings change. Three
// things the pad needs cannot be expressed there:
//
//   1. THE STICKS. IA_Look is mapped to EKeys::MouseX / MouseY with a Scalar carrying the mouse's
//      sensitivity. A stick needs a different KEY, a dead zone, a response curve and — the one that
//      is not negotiable — ScaleByDeltaTime, because a stick is a RATE and a mouse is a DELTA. The
//      same is true of IA_Move, which is built there from four 1D keys and swizzles.
//   2. TWO TABLES. GetKeys() is the KEYBOARD table. Folding pad buttons into its return value would
//      put a pad bind in the mouse context, where a keyboard RESET would then wipe it and where the
//      HUD's "HOLD [G]" caption would start printing a controller button on a machine with no pad.
//   3. OWNERSHIP. Source/Trace/Core is another tranche's file this pass; Source/Trace/Settings is
//      this one's. That is a scheduling fact rather than an argument, but the design it forced is
//      the better one anyway, and the paragraph above is why.
//
// TWO CONTEXTS IS A SUPPORTED SHAPE, not a trick: UEnhancedInputLocalPlayerSubsystem merges the
// mappings of every applied context, and priority only decides who CONSUMES a key when both name it.
// These two never name the same key — IsBindablePadKey refuses anything that is not a pad button and
// the options menu's keyboard capture refuses anything that is — so the merge is a union.
//
// -------------------------------------------------------------------------------------------------
// HOT-PLUG: WHY THERE IS NO HOT-PLUG CODE
// -------------------------------------------------------------------------------------------------
// The owner's case is a pad paired over Bluetooth MID-SESSION, possibly alongside a mouse. The
// answer is that NOTHING IN THIS FILE IS CONDITIONAL ON A PAD BEING PRESENT. The context is applied
// as soon as a local player exists and stays applied for the life of the game instance, whether or
// not any pad has ever been seen. So:
//
//   * connecting a pad mid-match needs no detection, no rebuild and no event — the mappings were
//     already there, and the first stick sample macOS delivers is consumed by them;
//   * connecting a pad cannot disable the keyboard, because nothing here ever removes, replaces or
//     re-prioritises the keyboard/mouse context — it is a different object this file never touches;
//   * a pad DISCONNECTING mid-match is equally uneventful: its keys stop arriving, the mappings sit
//     idle, and the keyboard is exactly where it was.
//
// Detection exists only for the one thing that genuinely needs it: telling the player, on the
// controller settings page, whether this machine has seen a pad at all — so that "my controller does
// nothing" and "I have not pressed anything yet" are distinguishable. That is a readout, never a
// gate. See HasSeenGamepadInput().
//
// -------------------------------------------------------------------------------------------------
// THE ONE THING IT SYNTHESISES, AND WHY IT IS SAFE
// -------------------------------------------------------------------------------------------------
// MENU/START (EKeys::Gamepad_Special_Right) injects an Escape press through the local player
// controller, because the pause menu is opened by a raw `WasInputKeyJustPressed(EKeys::Escape)` poll
// in ATraceHUD, which no mapping context can reach. Escape is the ONLY key in this build for which
// synthesising is provably harmless: UTraceUserSettings::IsBindableKey refuses it explicitly, so no
// gameplay action can ever be sitting on it, and the only things that read it are the three menus.
// Nothing else is synthesised — every other pad button goes through a real mapping.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/ObjectPtr.h"

#include "TraceGamepadInput.generated.h"

class APlayerController;
class UInputAction;
class UInputMappingContext;

/**
 * Owns IMC_TracePad for the life of the game instance.
 *
 * A GAME INSTANCE SUBSYSTEM and not a world one, because it must survive a travel: the pad's context
 * is applied to the LOCAL PLAYER, which outlives the world, and a per-world owner would have to
 * re-apply it on every map load and would leave a window on each one in which the pad did nothing.
 */
UCLASS()
class TRACE_API UTraceGamepadInputSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Rebuilds the context from UTraceUserSettings and re-applies it.
	 *
	 * Called on every UTraceUserSettings::OnChanged broadcast — i.e. on every rebind and every slider
	 * move — for the same reason ATracePlayerController::ApplyControlSettings is: a bind the player
	 * can see on the page but the mapping context has never heard of is a bind that does nothing.
	 */
	void ApplyPadSettings();

	/**
	 * Priority of the pad context, ABOVE the keyboard/mouse context's 0.
	 *
	 * The two never map the same key, so on the shipped configuration this number cannot matter. It
	 * is 1 rather than 0 to settle the one case it can: a HAND-EDITED TraceUserSettings.ini that puts
	 * a pad button in the keyboard table. Then one physical button names two actions, exactly one of
	 * them can win, and the pad page's answer is the one the player edited most recently in a UI.
	 */
	static constexpr int32 PadMappingPriority = 1;

	/**
	 * True once ANY gamepad key or axis has been seen on this machine since launch.
	 *
	 * A READOUT AND NEVER A GATE — see the hot-plug note in this file's header. It exists so the
	 * controller settings page can say "NO CONTROLLER SEEN YET" instead of leaving a player who has
	 * paired nothing staring at a page of binds wondering why they do nothing.
	 */
	bool HasSeenGamepadInput() const { return bSeenGamepadInput; }

	/** The live context, for the verification commands. Null before the first local player exists. */
	const UInputMappingContext* GetPadContext() const { return PadContext; }

	/**
	 * The resolved UInputAction for an asset name ("IA_Look"), or null.
	 *
	 * Exists for Trace.Pad.Drive, which proves a pad key reaches an ACTION by reading that action's
	 * live value back out of UEnhancedPlayerInput — a proof that does not depend on the pawn being
	 * alive, on the camera, or on anything downstream of Enhanced Input.
	 */
	UInputAction* FindResolvedAction(FName ActionName) const;

	/** The one accessor. Null on a dedicated server, or before the game instance is up. */
	static UTraceGamepadInputSubsystem* Get(const UObject* WorldContext);

private:
	/**
	 * Per-frame, and it early-outs in four lines on all but a handful of them.
	 *
	 * FTSTicker rather than a tickable subsystem because the two jobs need different rates and
	 * neither wants a UWorld: re-applying the context is an idempotent check that only does work when
	 * a local player has just appeared, and the MENU/START edge has to be sampled every frame or a
	 * button tap between two samples is lost.
	 */
	bool Tick(float DeltaSeconds);

	/** The first local player controller in any game world, or null. */
	APlayerController* LocalController() const;

	/**
	 * Fills the IA_ pointers, preferring the objects the player's input is ACTUALLY using.
	 *
	 * TWO SOURCES, IN THIS ORDER, and the order is the whole point. ATracePlayerController has two
	 * ways to obtain its UInputAction objects (see that file's header): the /Game/Trace/Input assets,
	 * or NewObject in C++ when the assets are missing or rejected. A context that maps the WRONG
	 * object compiles, applies, and does nothing at all.
	 *
	 *   1. The live per-player mapping list (UEnhancedPlayerInput::GetEnhancedActionMappings), keyed
	 *      by the action's object NAME. Whichever path built them, that list holds the very objects
	 *      the controller bound its handlers to, so a name found here is exact.
	 *   2. LoadObject from /Game/Trace/Input, for an action that has no mapping at all — THROW / PASS
	 *      CORE ships unbound on the keyboard, so it can never appear in (1).
	 *
	 * @return true when at least IA_Move and IA_Look were resolved; false means no pad mappings can
	 *         be built and the reason has been logged.
	 */
	bool ResolveInputActions();

	/** Sampled every frame: the rising edge is what turns MENU/START into an Escape. */
	void TickMenuButton(APlayerController* PC);

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> PadContext;

	/** Resolved once per context rebuild, by name. Transient and UPROPERTY'd so GC cannot take them. */
	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UInputAction>> Actions;

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle SettingsChangedHandle;

	/** The local player the context is currently applied to. Null means "not applied yet". */
	TWeakObjectPtr<class ULocalPlayer> AppliedTo;

	/** See TickMenuButton. Tracked here rather than read as an edge, so a coarse tick cannot miss it. */
	bool bMenuButtonWasDown = false;

	bool bSeenGamepadInput = false;

	/** Latches the "no input assets, and the C++ fallback did not name what I needed" warning. */
	bool bResolveFailureReported = false;
};
