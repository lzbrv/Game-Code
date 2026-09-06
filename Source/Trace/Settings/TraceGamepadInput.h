// Trace — D31-PAD: the gamepad's own Enhanced Input mapping context.
//
// D32-PADMENU ALSO LIVES HERE, at the bottom of this file: `namespace TracePadMenu` is the one
// place the pad's MENU vocabulary (A confirms, B goes back, the D-pad and the left stick move a
// highlight) is written down, so the title screen, the character select, the team select and the
// options overlay cannot drift into four different answers. Read that block before adding a fifth
// pad-driven screen.
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
	 *
	 * D32-PADMENU gave it a second reader, on the same terms: the four pad-driven screens print their
	 * button captions only once this is true, so a keyboard-only player is never told about buttons
	 * they do not have. STILL NEVER A GATE ON INPUT — the first button a player presses has to work,
	 * and it cannot if a screen is waiting to have seen one.
	 *
	 * THE PROBE LIST IN Tick() IS WHAT MAKES THIS TRUE OR FALSE, and D32 had to widen it: it stopped
	 * after D-pad UP and DOWN, so a player who pressed only D-pad LEFT — the obvious thing to do on a
	 * screen with two choices side by side — moved the highlight and still reported no controller.
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

// =================================================================================================
// D32-PADMENU — the ONE place the pad's menu vocabulary is written down
// =================================================================================================
//
// THE OWNER'S ASK, VERBATIM: "Make a connected controller work on the main menu and character select
// menus."
//
// -------------------------------------------------------------------------------------------------
// WHY THIS IS A SHARED NAMESPACE AND NOT FOUR COPIES OF THE SAME `if`
// -------------------------------------------------------------------------------------------------
// D31-PAD made the OPTIONS OVERLAY pad-navigable by reading the D-pad, the left stick's digital keys
// and three face buttons inline in FTraceOptionsMenu::PollNavigation. That was right for one screen.
// D32 adds three more — the title screen, the character select and the team select — and four
// independent copies of "which button means yes" is precisely how a player ends up with A confirming
// on one screen and B confirming on the next.
//
// So the semantics live here, once, and every screen this tranche owns calls these functions:
//
//     A  (Gamepad_FaceButton_Bottom)  CONFIRM      the convention on every pad ever shipped
//     B  (Gamepad_FaceButton_Right)   BACK         ditto, and it is what a thumb reaches for
//     X  (Gamepad_FaceButton_Left)    the screen's SECOND verb, where it has one, and nothing
//                                     otherwise. Today that is only the team screen's CHANGE
//                                     CHARACTER (the C key's twin).
//     D-pad / left stick              MOVE the highlight
//     MENU/START                      already an Escape, synthesised by TickMenuButton above
//
// *** FTraceOptionsMenu IS NOT ROUTED THROUGH HERE, AND THAT IS A SCHEDULING FACT, NOT A DESIGN. ***
// UI/TraceOptionsMenu.cpp is another tranche's file this pass. Its inline keys are the SAME keys —
// A select, B back, D-pad+stick move — which is why the four screens agree today; the check
// `Trace.Pad.MenuVerify` asserts the agreement rather than trusting this paragraph, so the day
// somebody edits one of the two, a run says so. When that file next comes free it should call these.
//
// -------------------------------------------------------------------------------------------------
// WHY IT IS POLLED AND NOT BOUND, ON A SCREEN WHOSE KEYBOARD *IS* BOUND
// -------------------------------------------------------------------------------------------------
// The title screen's keyboard arrives through ATraceMenuPlayerController::SetupInputComponent, six
// BindKey calls. The obvious change is six more. Two reasons it is not what happened:
//
//   1. Source/Trace/UI/TraceMenuPlayerController.* is NOT in this tranche's ownership line. (Neither
//      was it in D31-PAD's, which is why that pass's report lists "the title screen is not
//      pad-navigable" as a known gap and proposes exactly those six lines.)
//   2. A bound key cannot express a HELD direction. Every other menu in this project walks its
//      selection on a repeat clock — press once to step, hold to scroll — and BindKey gives one
//      IE_Pressed edge and nothing else. The keyboard gets away with it because the OS repeats keys;
//      a D-pad has no OS repeat at all, so a bound pad D-pad would move the highlight exactly once
//      per physical press. Polling is what the other three screens already do.
//
// Nothing here removes, replaces or re-prioritises anything the keyboard or the mouse uses: every
// function below is a read.

class APlayerController;

namespace TracePadMenu
{
	/** A. CONFIRM / activate, on every screen. */
	TRACE_API const FKey& ConfirmKey();

	/** B. BACK / cancel / close, on every screen. */
	TRACE_API const FKey& BackKey();

	/** X. The screen's second verb, where it has one. Never "confirm" and never "back". */
	TRACE_API const FKey& AltKey();

	/**
	 * Whether a pad may drive a menu at all — i.e. UTraceUserSettings::bPadEnabled.
	 *
	 * THE SAME GATE TickMenuButton USES for MENU/START, and it is deliberate that a menu obeys it:
	 * `CONTROLLER INPUT → OFF` is the escape hatch a player reaches for when a pad with a worn stick
	 * is jittering, and an OFF that silenced the game but still walked the pause menu would read as
	 * the setting being broken.
	 */
	TRACE_API bool IsEnabled();

	/**
	 * True once any pad input has been seen on this machine — the gate for a screen's PAD HINTS only.
	 *
	 * NEVER A GATE ON INPUT (see UTraceGamepadInputSubsystem::HasSeenGamepadInput, which this
	 * forwards to): the first button a player presses has to work, and it cannot work if the screen
	 * is waiting to have seen one. Hints are the opposite case — a keyboard-only player should not be
	 * told about buttons they do not have, and the moment they touch a pad the caption appears.
	 *
	 * Returns false when there is no game instance yet, which is the honest answer.
	 */
	TRACE_API bool HasSeenPad(const UObject* WorldContext);

	/**
	 * -1 / 0 / +1 from the D-pad and the left stick together. X is left/right, Y is up/down (down
	 * positive, matching every MoveSelection in this project).
	 *
	 * THE STICK IS READ AS ITS FOUR DIGITAL KEYS (Gamepad_LeftStick_Left and friends) rather than as
	 * an axis, exactly as FTraceOptionsMenu::PollNavigation does. The engine synthesises those from
	 * the stick with its own threshold, so a caller's repeat clock — written for a key that is either
	 * down or not — works unchanged, and a stick held at half deflection does not scroll a list at
	 * half speed.
	 *
	 * IsInputKeyDown OR WasInputKeyJustPressed, not either alone. Down alone loses a press that began
	 * and ended between two polls, which is exactly what synthetic injection produces and what a very
	 * quick tap produces on a laggy frame; just-pressed alone cannot express a HELD direction, which
	 * is what the repeat clock needs.
	 */
	TRACE_API int32 NavX(const APlayerController* PC);
	TRACE_API int32 NavY(const APlayerController* PC);

	/**
	 * A / B / X, read as WasInputKeyJustPressed — the same edge the keyboard next to them is read as.
	 * All three return false when IsEnabled() is false.
	 *
	 * *** THESE CAN REPORT ONE PHYSICAL PRESS ON TWO CONSECUTIVE FRAMES. *** Measured, not feared:
	 * the title-screen run of 2026-09-06 logged `Pad A (confirm) on PLAY` on frame 93 and again on
	 * frame 94, the frame the ClientTravel began — UPlayerInput only clears EventCounts when
	 * ProcessInputStack runs, and a frame that is busy loading a map may not run one. Everything that
	 * already had a latch of its own absorbed it (the character screen's PendingRequest, the team
	 * screen's RequestCooldown, the title screen's bTravelling), which is why it was invisible until
	 * a log was read line by line.
	 *
	 * A caller for whom "at most once per physical press" is load-bearing — the title screen, where a
	 * second A would submit the JOIN prompt the first one opened — must use RisingEdge below instead.
	 */
	TRACE_API bool ConfirmPressed(const APlayerController* PC);
	TRACE_API bool BackPressed(const APlayerController* PC);
	TRACE_API bool AltPressed(const APlayerController* PC);

	/**
	 * A rising edge computed from the CALLER'S OWN previous sample, which cannot fire twice for one
	 * press however the engine's frames fall.
	 *
	 * The same argument, and the same shape, as UTraceGamepadInputSubsystem::TickMenuButton's handling
	 * of MENU/START: a remembered down-state is a promise about the physical button, where
	 * WasInputKeyJustPressed is a promise about a per-frame event list that something else owns.
	 *
	 * @param bWasDown in/out, one bool per key per caller. Stale is harmless: a poll that is skipped
	 *                 for a frame simply leaves the button "already down", which suppresses an edge
	 *                 rather than inventing one.
	 * @return true on the frame the button goes from up to down. Always false while IsEnabled() is
	 *         false — and it still records the state, so switching CONTROLLER INPUT back on under a
	 *         held button does not fire one.
	 */
	TRACE_API bool RisingEdge(const APlayerController* PC, const FKey& Key, bool& bWasDown);

	/**
	 * Held-key repeat, shared so four screens cannot drift into four scroll speeds.
	 *
	 * Matches FTraceOptionsMenu's numbers (0.38 s before the first repeat, then one step every
	 * 0.14 s). The character select's own keyboard repeat is 0.35/0.12 and is NOT changed by this
	 * tranche — the pad and the keyboard run separate clocks on that screen, because merging them
	 * would have meant editing behaviour the owner did not ask about.
	 */
	inline constexpr float RepeatDelay = 0.38f;
	inline constexpr float RepeatInterval = 0.14f;

	/**
	 * One step of the shared repeat clock.
	 *
	 * @param Dir        this frame's direction, -1 / 0 / +1
	 * @param LastDir    in/out: the previous frame's direction
	 * @param NextTime   in/out: local time the next repeat is due
	 * @param Now        local time
	 * @return true when the caller should move the highlight one step this frame.
	 */
	TRACE_API bool StepRepeat(int32 Dir, int32& LastDir, float& NextTime, float Now);
}
