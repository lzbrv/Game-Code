// Trace — player controller.
//
// Owns the *entire* Enhanced Input setup. All gameplay input is funnelled through the
// ATraceCharacter::Do* entry points; the controller itself never mutates gameplay state.
//
// TWO WAYS THE INPUT DATA CAN COME INTO EXISTENCE, AND BOTH ARE LIVE (spec v17 §6).
//
//   1. ASSETS — /Game/Trace/Input/IA_*.uasset and IMC_Trace.uasset, written by
//      Scripts/generate-input-assets.py FROM the same action table the C++ path uses. Preferred
//      when they are present, load cleanly and pass validation.
//   2. C++ — NewObject, exactly as this class has always done it. Used when the assets are
//      missing, fail to load, disagree with the C++ table, or when the toggle is off.
//
// The fallback is not a formality: the game plays identically either way, and which path ran is
// stated at Display in every log. `Trace.Input.VerifyAssets` prints the whole comparison.
//
// The project USED to cite "build contract §2: this project cannot author .uassets" as the reason
// everything here was built in code. That contract is retired — the arena bake authored 572 actors
// and 66 materials from a headless commandlet, and these input assets come from the same mechanism.
//
// WHY THE LOADED CONTEXT IS DUPLICATED RATHER THAN USED DIRECTLY, which is the one real hazard of
// the asset path: MapKey()/UnmapAll() MUTATE the context they are called on, and ApplyControlSettings
// rebuilds the whole mapping list on every settings change. Done to a loaded .uasset that mutation
// outlives PIE, dirties the asset, and would be shared by every local player. So the asset is a
// TEMPLATE: DuplicateObject gives this controller its own copy, which dies with it. The IA_* actions
// are NOT duplicated — they are never mutated, and Enhanced Input keys its per-action runtime state
// off the local player, not off the action object, so sharing them is correct.
//
// The one thing that will silently break this file: dropping the UPROPERTY on any IA_* member. A
// NewObject'd UInputAction has no other owner rooting it, so it would be collected at the next GC
// and input would stop working mid-match with no error anywhere.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"  // FDelegateHandle
#include "Engine/EngineTypes.h"          // EEndPlayReason
#include "GameFramework/PlayerController.h"
#include "Gameplay/TraceHitZones.h"      // ETraceHitZone (ClientNotifyHit's payload)
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TracePlayerController.generated.h"

class APawn;
class ATraceCharacter;
class ATracePlayerState;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

/**
 * Everything the HUD needs to draw the dash indicator, resolved in one call.
 *
 * A struct rather than four out-parameters because the HUD needs all of it at once and the pieces
 * are only meaningful together — a charge count without its recharge fraction cannot be drawn.
 *
 * Plain C++, declared outside the UCLASS: it is never reflected, never replicated and never touched
 * by Blueprint, so putting it through UHT would buy nothing.
 */
struct FTraceDashHudState
{
	/** Charges ready to spend right now. */
	int32 Charges = 0;

	/** Total charges the pawn has. 2 for the Core carrier once spec §5 lands, 1 otherwise. */
	int32 MaxCharges = 1;

	/** 0..1 progress of the charge currently regenerating. 1 when nothing is recharging. */
	float RechargeFraction = 1.f;

	/** Seconds until the next charge lands. 0 when nothing is recharging. */
	float Remaining = 0.f;
};

/**
 * Trace's player controller.
 *
 * Input model. Every key below is the DEFAULT — all of them are rebindable through the options
 * screen, and the authority at runtime is UTraceUserSettings, not this comment. (This list used to
 * cite "contract §7: Canvas only"; that contract is retired and the citation was never relevant to
 * the key table anyway.) The ACTIONS themselves are assets under /Game/Trace/Input when they exist,
 * with the C++ constructors as the live fallback — see the file header:
 *   Move        Axis2D   WASD          -> X = strafe (+right), Y = forward (+forward)
 *   Look        Axis2D   Mouse X / Y   -> X = yaw delta, Y = pitch delta, scaled and signed by
 *                                         the player's sensitivity and invert-Y settings
 *   Jump        bool     Space
 *   Crouch      bool     Left Ctrl     (slide on the ground, fast-fall in the air)
 *   Fire        bool     LMB           (doubles as "put me back in" while dead)
 *   Pass        bool     RMB
 *   Dash        bool     Left Shift
 *   Parry       bool     RMB           (carrier only — 0.175s of trace invulnerability, spec v3 §3 / v8 §3 / v10 §4;
 *                                       moved from Q to right mouse by spec v25 §7)
 *   PullCore    bool     F             (spec v26 §1 — the turnover Core-pull, its OWN bind. Held.
 *                                       It rode the parry's button in v25 §2; §1 splits them, and the
 *                                       old precedence rule survives only as a tiebreak for a player
 *                                       who deliberately puts both actions on one key)
 *   Scoreboard  bool     Tab           (held)
 *   EquipKnife  bool     1             (spec v13 §2 — DIRECT SELECT, idempotent; the SWING rides IA_Fire)
 *   EquipGun    bool     2             (spec v13 §2 — DIRECT SELECT, idempotent)
 *   Reload      bool     R             (spec v16 §1 — the clip also reloads itself when it empties)
 *
 * SPEC v15 §5: TWO WEAPON KEYS, AND NO TOGGLE. Verbatim: "Switch weapon keybind so that it's only
 * switch to knife/switch to gun." The `SwapWeapon` action — gun <-> knife on F, spec v10 §1 — is
 * deleted outright: the enumerator, IA_SwapWeapon, its mapping, its handler and its row in the
 * rebind list. This supersedes spec v13 §2's [ASSUMPTION] that it was worth keeping alongside the
 * two direct selects.
 *
 * The toggle VERB survives one level down as TraceMelee::RequestSwapWeapon, because the weapon
 * component still needs "give me the other one" for the dev console (Trace.Knife.Swap) and for the
 * v13 §2 harness's red arm. What no longer exists is a KEY that means it, which is what was asked.
 */
UCLASS()
class TRACE_API ATracePlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ATracePlayerController();

	//~ Begin AActor / APlayerController interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupInputComponent() override;
	/** Server-side possession. */
	virtual void OnPossess(APawn* InPawn) override;
	/** Client-side possession (also runs on a listen-server host). */
	virtual void AcknowledgePossession(APawn* P) override;
	//~ End AActor / APlayerController interface

	/**
	 * Shooter-side hit confirmation, sent by the server once a hitscan resolves.
	 * Reliable: a dropped hit marker reads as a missed shot, which is worse than a late one.
	 */
	UFUNCTION(Client, Reliable)
	void ClientNotifyHit(bool bKilled, ETraceHitZone Zone);

	/** Which zone the last confirmed hit landed on. Drives the hitmarker's colour and shape. */
	ETraceHitZone GetLastHitMarkerZone() const { return LastHitMarkerZone; }

	/** Victim-side death notification. Drives the killer line on the death panel. */
	UFUNCTION(Client, Reliable)
	void ClientNotifyKilledBy(const FString& KillerName, FName Cause);

	/**
	 * KILLER-side notification for spec v6 §3: your parry killed the enemy who dashed your trace.
	 *
	 * The dasher already learns why they died through ClientNotifyKilledBy above — the kill carries
	 * cause "Parried" and the carrier as the killer. This is the other half, and it needs its own
	 * message because the carrier's screen shows them nothing at all otherwise: the enemy simply
	 * stops existing somewhere behind them, which is the one outcome a 0.1 s reaction check must
	 * never produce silently.
	 *
	 * Reliable for the same reason ClientNotifyHit is: a dropped confirmation of a hit reads as a
	 * miss, and a parry the carrier cannot confirm reads as a parry that did not register.
	 *
	 * A listen-server host needs none of this — TraceParry keeps the authoritative record and
	 * ATraceHUD reads it directly — but a remote client has no access to that record at all.
	 */
	UFUNCTION(Client, Reliable)
	void ClientNotifyParryKill(const FString& VictimName);

	/** Client-local time of our last parry kill, or a large negative sentinel. See the RPC above. */
	float GetLastParryKillTime() const { return LastParryKillTime; }

	/** Who our last parry killed; empty until the server tells us. */
	const FString& GetLastParryKillVictim() const { return LastParryKillVictim; }

	/** Asks the server to put us back in the game. See the implementation for the exact rules. */
	UFUNCTION(Server, Reliable)
	void ServerRequestRespawn();

	/** The possessed pawn as an ATraceCharacter, or null (unpossessed, or a non-Trace pawn). */
	ATraceCharacter* GetTraceCharacter() const;

	ATracePlayerState* GetTracePlayerState() const;

	/**
	 * Client-local world time (UWorld::GetTimeSeconds) of the last confirmed hit, or a large
	 * negative sentinel if we have never hit anything. ATraceHUD fades a marker out from this.
	 */
	float GetLastHitMarkerTime() const { return LastHitMarkerTime; }

	// -----------------------------------------------------------------------------------------
	// HUD support.
	//
	// Not part of the cross-agent contract (§7): ATraceHUD ships in the same ownership slice as
	// this controller, and these exist purely so the HUD never has to poll raw keys or guess.
	// -----------------------------------------------------------------------------------------

	/** True if the last confirmed hit was a kill — the HUD draws that marker differently. */
	bool WasLastHitMarkerAKill() const { return bLastHitMarkerWasKill; }

	/** True while the scoreboard key is held. */
	bool IsScoreboardOpen() const { return bScoreboardOpen; }

	/** Name of whoever killed us last; empty until the server tells us. */
	const FString& GetLastKillerName() const { return LastKillerName; }

	/** Cause of our last death: "Bullet", "Trail" or "Fell". NAME_None until set. */
	FName GetLastDeathCause() const { return LastDeathCause; }

	// -----------------------------------------------------------------------------------------
	// Settings
	// -----------------------------------------------------------------------------------------

	/**
	 * Rebuilds every key mapping and both look modifiers from UTraceUserSettings.
	 *
	 * Called on construction of the input data, and again on every settings change — the options
	 * screen is reachable from an in-game pause menu, so a player has to be able to fix their
	 * sensitivity and feel it on the next mouse movement, not on the next respawn.
	 *
	 * Cheap: it clears and repopulates a dozen mappings on one transient context and asks the
	 * subsystem to rebuild. Safe to call before the local player exists.
	 */
	void ApplyControlSettings();

	/**
	 * Silences every gameplay input handler and hands the mouse to the UI.
	 *
	 * Driven by ATraceHUD when the pause/settings overlay opens. Look input in particular MUST be
	 * suppressed: the overlay releases the mouse from the viewport, and the warp back to centre when
	 * capture is retaken is the same one-frame view teleport ApplyGameInputMode already defends
	 * against at spawn.
	 */
	void SetGameInputSuppressed(bool bSuppressed);

	bool IsGameInputSuppressed() const { return bGameInputSuppressed; }

	// -----------------------------------------------------------------------------------------
	// HUD data sources for mechanics owned by other slices.
	//
	// These exist so ATraceHUD never has to poll the movement component itself. They used to route
	// through a SFINAE compat header while the movement/character slices were still landing; those
	// slices have landed, so these now call the real accessors directly. A missing accessor is a
	// compile error again, which is the point — the shim's silent `false` was how a dead mechanic
	// could look exactly like a working one.
	// -----------------------------------------------------------------------------------------

	/** False when there is no living pawn to describe. */
	bool GetDashHudState(FTraceDashHudState& OutState) const;

	/** 0..1 through the 0.5s pass hold; negative when no pass is in progress. */
	float GetPassProgress() const;

	// -----------------------------------------------------------------------------------------
	// Input verification instrumentation.
	//
	// The bots call ATraceCharacter::Do* directly and never touch Enhanced Input, so before this
	// existed NOTHING in the project exercised the human input path — a broken binding would have
	// produced a completely silent, completely green run. These counters are the ground truth that
	// the synthetic-input harness in Source/Trace/Debug asserts against: they are incremented
	// inside the Enhanced Input delegates themselves, so a non-zero count is proof that the event
	// travelled OS/Slate -> viewport -> UEnhancedPlayerInput -> trigger -> our bound delegate.
	//
	// Plain ints, not UPROPERTYs: they are never replicated, never serialised and never read by
	// gameplay. Cost is a handful of bytes and one increment per event.
	// -----------------------------------------------------------------------------------------

	int32 DebugMoveEventCount = 0;
	int32 DebugLookEventCount = 0;
	int32 DebugFireStartedCount = 0;
	int32 DebugFireCompletedCount = 0;
	int32 DebugJumpCount = 0;
	int32 DebugPassCount = 0;
	int32 DebugDashCount = 0;
	int32 DebugScoreboardCount = 0;
	int32 DebugCrouchCount = 0;
	int32 DebugParryCount = 0;
	/**
	 * SPEC v26 §1. Presses that reached ATracePlayerController::DispatchCorePull and were dispatched,
	 * i.e. AFTER the per-frame de-duplication and the suppression gate.
	 *
	 * Counted where it is dispatched rather than in OnPullCoreStarted so that the shared-key tiebreak
	 * path is counted too — the question this number answers is "did the pull verb leave this
	 * machine", and it must not depend on which of the two handlers delivered it.
	 */
	int32 DebugPullPressCount = 0;
	/** Bumped by ClientNotifyHit — a server-confirmed hitscan resolution credited to us. */
	int32 DebugHitConfirmCount = 0;

	/**
	 * Spec v13 §2. Bumped on the FIRST LINE of the weapon handler, before any gate, so it counts
	 * "the key reached a bound delegate" and nothing else.
	 *
	 * IT EXISTS BECAUSE THE §2 HARNESS WAS BRIEFLY VACUOUS WITHOUT IT, and that is worth writing
	 * down. Trace.V13.Hotkeys proves a redundant press does not restart the pullout by pressing the
	 * key twice and watching the remaining time keep falling. Its first version held each synthetic
	 * key for 0.05 s and pressed again 0.05 s later, so the second press landed while the key was
	 * still logically DOWN — Enhanced Input emits Started only on a down EDGE, the handler never
	 * ran, and "the pullout was not restarted" was true because NOTHING HAPPENED. It passed. It also
	 * passed in the red arm, on the unguarded path, which is what gave it away.
	 *
	 * The fix is a longer gap, but the guarantee is this counter: the probe now asserts the count
	 * went up across every press, so a swallowed key fails the run loudly instead of passing it
	 * silently. A test that cannot fail is not evidence.
	 *
	 * ONE counter, not two: spec v15 §5 deleted the SwapWeapon handler that used to own the second,
	 * and BOTH arms of the v13 §2 harness now press the same direct-select key. That is what makes
	 * the red arm a genuine A/B — same key, same handler, same counter, one gate different.
	 */
	int32 DebugEquipPressCount = 0;

	// An accessor rather than reading the field directly, so the harness cannot write it. This
	// whole block is already inside the class's `public:` section (see line ~89) — no access
	// specifier is introduced here, because flipping one would silently re-scope every counter
	// above and the synthetic-input harness reads several of them.
	/** Read by the v13 §2 harness to prove a synthetic key actually reached a bound delegate. */
	int32 GetDebugEquipPressCount() const { return DebugEquipPressCount; }

	/**
	 * SPEC v16 §1. How many times OnReloadStarted has been ENTERED, counted on its first line before
	 * any gate — so it answers "the R key reached a bound delegate", which is a different question
	 * from "a reload started" and is the one Trace.Ammo.BindTest has to be able to ask.
	 *
	 * The distinction is the whole reason it exists, and DebugEquipPressCount's comment above records
	 * what it cost to learn: a probe that only watched the OUTCOME passed on a build where the key
	 * never reached the handler at all, because "nothing happened" and "the guard worked" look
	 * identical from outside.
	 */
	int32 DebugReloadPressCount = 0;

	int32 GetDebugReloadPressCount() const { return DebugReloadPressCount; }

	FVector2D DebugLastMoveValue = FVector2D::ZeroVector;
	FVector2D DebugLastLookValue = FVector2D::ZeroVector;

	/**
	 * Dumps every part of the Enhanced Input setup that can silently be wrong — the input
	 * component's real class, how many action bindings it carries, whether the subsystem actually
	 * accepted our mapping context, each action's ValueType, and the possession state.
	 *
	 * Lives here rather than in the harness because most of what it reports is protected state of
	 * this class. Logs at Display so it survives the default verbosity of an automated run.
	 */
	void LogInputDiagnostics(const TCHAR* Context) const;

	/**
	 * SPEC v17 §6. True when the IA_ and IMC_Trace assets were loaded, validated and adopted; false
	 * when this controller built its input in C++ (assets missing, invalid, or the toggle off).
	 *
	 * Read by Trace.Input.VerifyAssets. Not a setting — the decision is made once, in BuildInputData.
	 */
	bool IsUsingInputAssets() const { return bUsingInputAssets; }

	/**
	 * SPEC v17 §6. Prints this controller's live mapping list — action name, key and modifier
	 * classes, in order — so the asset path and the C++ path can be compared line for line.
	 *
	 * A member rather than a free function in the harness because InputMapping and every IA_* are
	 * protected, and because "what is actually bound right now" is the only question that
	 * distinguishes a migration that worked from one that merely compiled.
	 */
	void LogLiveInputMappings(const TCHAR* Context) const;

	/**
	 * SPEC v17 §6. The UInputAction each live key mapping points at, in mapping order.
	 *
	 * Exists for POINTER IDENTITY, which is the only thing that can tell the two paths apart: the
	 * C++ fallback names its NewObject'd action "IA_Move" too, so a name comparison would call the
	 * fallback a success. Trace.Input.VerifyAssets checks these against the loaded asset objects.
	 *
	 * An out-parameter of raw pointers rather than the mapping array itself, so this header does not
	 * have to pull in Enhanced Input's structs for a diagnostic.
	 */
	void GetLiveMappedActions(TArray<const UInputAction*>& OutActions) const;

	// -----------------------------------------------------------------------------------------
	// Networking diagnostics (spec v5 §0).
	//
	// Neither of these changes behaviour. They exist because the Demo 5 multiplayer report — "I'm
	// unsure if we were actually on a working network-client setup" — could not be answered from the
	// evidence that existed, and the cheapest permanent fix for that is for the answer to already be
	// in every log.
	// -----------------------------------------------------------------------------------------

	/**
	 * One Display line: net mode, whether this controller is local, whether it has authority, and
	 * the player it belongs to.
	 *
	 * Emitted for EVERY controller in BeginPlay, including the server-side proxies of remote
	 * players. On a listen server hosting one other human that is three lines, and the fact that
	 * there are three is itself the proof the connection worked.
	 */
	void LogNetworkRole(const TCHAR* Context) const;

	/**
	 * `TraceNetInfo` in the console: role, endpoint, every local adapter address, and the full
	 * PlayerState roster with bots marked.
	 *
	 * Exec rather than a cvar because the question it answers ("who is actually in this match?") is
	 * asked once, by hand, when something looks wrong — and because a cvar cannot print a list.
	 */
	UFUNCTION(Exec)
	void TraceNetInfo();

private:
	/**
	 * Emits ONE Display line the first time each action ever fires, then never again.
	 *
	 * This is not covered by Trace.LogInput, and deliberately so. The next time somebody reports
	 * "I couldn't shoot", the answer to "did the key even reach the game?" has to be in the log
	 * they already have — not behind a cvar nobody thought to set before playing. Seven lines per
	 * session is nothing; the absence of the "Fire" line is a diagnosis.
	 */
	void LogFirstEventOfAction(uint8 Bit, const TCHAR* ActionName);

	/** One bit per action, so the line above is emitted exactly once each. */
	uint8 FirstEventLoggedMask = 0;

	static constexpr uint8 FirstEvent_Move       = 1 << 0;
	static constexpr uint8 FirstEvent_Look       = 1 << 1;
	static constexpr uint8 FirstEvent_Jump       = 1 << 2;
	static constexpr uint8 FirstEvent_Fire       = 1 << 3;
	static constexpr uint8 FirstEvent_Pass       = 1 << 4;
	static constexpr uint8 FirstEvent_Dash       = 1 << 5;
	static constexpr uint8 FirstEvent_Scoreboard = 1 << 6;
	static constexpr uint8 FirstEvent_Crouch     = 1 << 7;
	// Parry deliberately has no bit: the mask is a uint8 and is full. Parry is the newest action, and
	// unlike the others it has a loud diagnosis of its own — Trace.DebugParry reports the refusal
	// reason for every attempt — so it is the one action whose "did my key do anything?" question is
	// already answerable without widening the mask.

public:

protected:
	// ---- Enhanced Input data ------------------------------------------------------------------
	//
	// Populated EITHER from /Game/Trace/Input (preferred) OR by NewObject (the live fallback) —
	// see the file header for the whole argument. Which one ran is in the log and in
	// IsUsingInputAssets().
	//
	// These MUST stay UPROPERTYs. On the C++ path nothing else roots the NewObject'd actions; on
	// the asset path the pointers are what keep the loaded assets referenced. Transient because
	// they are resolved from scratch on every load and must never be serialised.

	/**
	 * Always a PER-CONTROLLER object, never the asset itself: on the asset path this is a
	 * DuplicateObject of the loaded IMC_Trace. ApplyControlSettings calls UnmapAll/MapKey on it a
	 * dozen times per settings change, and doing that to a loaded .uasset would dirty the asset.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> InputMapping;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Move;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Look;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Jump;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Fire;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Pass;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Dash;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Scoreboard;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Crouch;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Parry;

	/**
	 * Spec v13 §2. DIRECT SELECT, default 1 and 2. These two are the WHOLE weapon input model as of
	 * spec v15 §5 — there is no IA_SwapWeapon any more, and there is deliberately no IA_Swing
	 * either: the swing rides IA_Fire, because UTraceWeaponComponent's fire path branches on
	 * IsKnifeEquipped(), so mouse1 shoots with the gun and swings with the knife.
	 *
	 * Two actions rather than one action with a value: an Enhanced Input Boolean action carries no
	 * payload that could say WHICH weapon, and giving them one action plus a modifier would put the
	 * weapon identity inside the mapping, where the rebind screen could not see it and where a
	 * player rebinding "equip" to one key would silently get half a mechanic.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_EquipKnife;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_EquipGun;

	/**
	 * SPEC v14 §5 — the two ability binds, default E and V.
	 *
	 * IA_Ability is bound on Started only: the activated ability is a press. IA_AbilitySecondary is
	 * bound on Started AND Completed AND Canceled, because Mace's suspend is a HOLD and a dropped
	 * release edge would leave her floating — the same asymmetry, and for the same reason, as
	 * IA_Pass's hover pass.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Ability;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_AbilitySecondary;

	/**
	 * SPEC v16 §1 — "R to reload". Default R, rebindable like everything else.
	 *
	 * Bound on Started ONLY, with the same asymmetry argument as IA_EquipKnife / IA_EquipGun: there is
	 * no held state to release, and a Completed binding would send a second reload request on key-up
	 * that UTraceWeaponComponent::RequestReload would then swallow because a reload is already
	 * running — a binding that only works because something downstream ignores it.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Reload;

	/**
	 * SPEC v26 §1 — "Make parry and pull core two separate binds in the settings menu." Default F.
	 *
	 * Its own UInputAction and not a second mapping on IA_Parry, which is the whole point of the item:
	 * two actions is what makes two rows on the keybind page, two rebindable keys, and two independent
	 * handlers. One action with two keys would be one bind wearing a second key.
	 *
	 * Bound on Started AND Completed AND Canceled, because the pull is a HOLD — "releasing right mouse
	 * cancels" was spec v25 §2's rule and it is unchanged by moving the verb to its own key. A dropped
	 * release edge here leaves a pull ring filling on the server behind a pause menu, which is the
	 * exact failure OnPassCompleted's comment documents.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_PullCore;

	/**
	 * *** SPEC v28 §10 — THE MELEE BIND. Default RIGHT MOUSE, rebindable like everything else. ***
	 *
	 * The whole verb is TraceMelee::HandleMeleeInput and this action does nothing but deliver edges to
	 * it. In particular the PRECEDENCE — "if and only if a player is looking at a pullable core, and
	 * being shown the circle icon to pull, this keybind should override the melee keybind" — is not
	 * decided here: HandleMeleeInput asks ATraceCore::CanPullNow, which is the same predicate ATraceHUD
	 * asks to decide whether to DRAW the circle. One fact, not two that can drift.
	 *
	 * Bound on Started AND Completed AND Canceled, and BOTH edges are load-bearing. The swing is a
	 * press-edge action and does not care, but the press may have gone to the Core PULL, which is a
	 * HOLD — a dropped release leaves a pull ring filling on the server behind a pause menu or a lost
	 * window focus. HandleMeleeInput forwards every release to the Core unconditionally so the caller
	 * never has to remember which verb the press went to; see its comment.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Melee;

	/**
	 * Resolves InputMapping and every IA_* exactly once, then lays down the key mappings.
	 *
	 * The two halves are separate on purpose. The ACTIONS must be resolved exactly once and never
	 * replaced, because SetupInputComponent binds delegates to those specific objects and a fresh
	 * UInputAction would leave every binding pointing at an action nothing maps to. The KEY MAPPINGS
	 * are torn down and rebuilt on every settings change. Cheap and safe to call repeatedly.
	 *
	 * SPEC v17 §6: tries the assets first, falls back to the C++ constructors. Both are below.
	 */
	void BuildInputData();

	/**
	 * SPEC v17 §6. Loads /Game/Trace/Input, validates it against the C++ table and adopts it.
	 *
	 * @return true only if EVERY action loaded, every ValueType and AccumulationBehavior matched
	 *         what the C++ path would have produced, and the mapping context loaded. Partial
	 *         success is treated as failure and nothing is assigned — half an input scheme is worse
	 *         than the fallback, and the fallback is known-good.
	 *
	 * Never mutates a loaded asset. Validation is a comparison, not a repair: an asset that
	 * disagrees with the C++ table is a REGENERATE that did not happen, and silently fixing it up
	 * in memory would hide exactly the drift Trace.Input.VerifyAssets exists to find.
	 */
	bool TryAdoptInputAssets();

	/** The original path, unchanged: NewObject for the context and all fifteen actions. */
	void ConstructInputDataInCode();

	/** Registers InputMapping with the local player's Enhanced Input subsystem. Idempotent. */
	void AddInputMappings();

	/** Unregisters it again. Called from EndPlay so travel does not leak stale contexts. */
	void RemoveInputMappings();

	/**
	 * Puts the viewport into game-only input with the initial mouse-down NOT consumed, so the click
	 * that recaptures a window also registers as a shot. Re-applied on every possession; see the
	 * implementation for why the default FInputModeGameOnly is wrong for a shooter.
	 */
	void ApplyGameInputMode();

	// ---- Input handlers -----------------------------------------------------------------------
	void OnMoveInput(const FInputActionValue& Value);
	void OnLookInput(const FInputActionValue& Value);
	void OnJumpStarted();
	void OnJumpCompleted();
	void OnFireStarted();
	void OnFireCompleted();
	void OnPassStarted();
	/** Release edge of the held hover pass (spec §4). Must fire even when input is suppressed. */
	void OnPassCompleted();
	void OnDashStarted();
	/** Press edge of the parry (spec v3 §3). Carrier-only; the refusal lives in TraceParry. */
	void OnParryStarted();
	/**
	 * Release edge. Parry is a tap, not a hold, so this does nothing — but it is bound anyway, and
	 * Canceled with it, so every button in this class has the same Started/Completed/Canceled shape.
	 * That symmetry is what fixed the pass-input latch; an action bound on Started alone is how a
	 * held-key bug gets reintroduced the day somebody makes the window hold-to-extend.
	 */
	void OnParryCompleted();

	/**
	 * SPEC v26 §1. Press and release edges of the CORE PULL, which is now its own bind (default F).
	 *
	 * Makes no decision of its own, exactly like OnParryStarted: every refusal (not a turnover, wrong
	 * team, not hovering, no line of sight, already carrying, window expired) lives behind
	 * ATraceCore's own authoritative gate. An input handler that second-guessed any of those would be
	 * a client deciding the outcome of a server-authoritative verb.
	 *
	 * The RELEASE is deliberately not gated on bGameInputSuppressed and uses GetPawn rather than
	 * GetLivingCharacter — see OnParryCompleted for the argument. Opening the pause menu mid-pull
	 * suppresses input; dying mid-pull makes the pawn non-living. Those are the two cases where the
	 * cancel must still be delivered.
	 */
	void OnPullCoreStarted();
	void OnPullCoreCompleted();

	/**
	 * SPEC v28 §10 — press and release edges of the MELEE bind (right mouse by default).
	 *
	 * Makes NO decision of its own, exactly like OnParryStarted and OnPullCoreStarted: the swing's
	 * refusals (dead, carrying, mid-pullout, dashing, on cooldown) live behind
	 * UTraceWeaponComponent::CanSwing and the pull's live behind ATraceCore, and the choice between
	 * the two is TraceMelee::ShouldCorePullOverrideMelee. A second opinion here is how a predicted
	 * state and an authoritative one come to disagree.
	 *
	 * The RELEASE is deliberately NOT gated on bGameInputSuppressed and uses GetPawn rather than
	 * GetLivingCharacter, for the reason OnParryCompleted and OnPullCoreCompleted both document:
	 * opening the pause menu mid-pull suppresses input and dying mid-pull makes the pawn non-living,
	 * and those are exactly the two cases where the cancel MUST still be delivered.
	 */
	void OnMeleeStarted();
	void OnMeleeCompleted();

	/**
	 * SPEC v26 §1. The one place the pull is dispatched, and the reason it is a function.
	 *
	 * Two handlers can reach the pull on one frame — its own bind, and (only when the player has put
	 * both actions on the SAME key) the parry's tiebreak path. This collapses that into exactly one
	 * dispatch per frame per edge, so a deliberately shared key sends one RPC rather than two.
	 *
	 * @param bPressed  true for the press edge, false for the release.
	 */
	void DispatchCorePull(bool bPressed);

	/** True when the player has deliberately bound PARRY and PULL CORE to the same key. */
	bool DoParryAndPullShareAKey() const;

	/** Frame number of the last DispatchCorePull, per edge, so the two callers cannot double-fire. */
	uint64 LastPullPressFrame = 0;
	uint64 LastPullReleaseFrame = 0;

	/**
	 * Spec v13 §2. Press edge of the two DIRECT-SELECT weapon binds. Press-only, and that asymmetry
	 * is deliberate: there is no held state here, so a release binding would fire a second request
	 * on key-up.
	 *
	 * @param bWantKnife  true for the 1 key, false for the 2 key.
	 *
	 * ONE implementation behind two handlers rather than two copies: the only difference between
	 * them is the destination weapon, and the idempotence guard is the part that must not be allowed
	 * to drift between the knife path and the gun path.
	 */
	void OnEquipKnifeStarted();
	void OnEquipGunStarted();
	void HandleDirectEquip(bool bWantKnife, const TCHAR* ActionLabel);

	/**
	 * SPEC v16 §1. Press edge of the reload bind (R).
	 *
	 * Makes NO decision of its own — every refusal (full clip, already reloading, dead, carrying, the
	 * knife out, an ability-loaded clip) lives behind UTraceWeaponComponent::RequestReload, which is
	 * also what the AUTOMATIC reload goes through. A second opinion here about whether a reload is
	 * legal is exactly how a predicted state and an authoritative one come to disagree, and it is the
	 * same reasoning OnParryStarted's comment gives.
	 */
	void OnReloadStarted();

	void OnCrouchStarted();
	void OnCrouchCompleted();
	void OnScoreboardStarted();
	void OnScoreboardCompleted();

	/**
	 * SPEC v14 §5. Press edge of the activated ability (E).
	 *
	 * Routed through UTraceAbilityInputRelay::RouteActivatePressed rather than straight into
	 * UTraceAbilityComponent::HandleActivatePressed, because ONE character's press is not an
	 * activation: Mace's second press of Spike is the reactivation that pulls her, and
	 * TryActivate() correctly refuses it (the throw's own 20 s cooldown is running). That routing
	 * rule is written down exactly once, in the relay, and both the key path and the relay's own
	 * interim poll call it.
	 */
	void OnAbilityStarted();

	/** SPEC v14 §5. Press / release of the secondary ability (V) — Mace's suspend is a hold. */
	void OnAbilitySecondaryStarted();
	void OnAbilitySecondaryCompleted();

	/** GetTraceCharacter(), but null when the pawn is missing or dead. */
	ATraceCharacter* GetLivingCharacter() const;

private:
	/**
	 * SPEC v18 §1c. Re-fires the press edge of every HOLD-shaped gameplay action whose key is still
	 * physically down at the moment gameplay input is handed back.
	 *
	 * Called from exactly one place, the restore branch of SetGameInputSuppressed, and it is the
	 * mirror of the DoFireReleased/StopJumping/DoMove(0) that the suppress branch does. Enhanced Input
	 * fires Started on a TRANSITION, so a key that was already held when a menu opened produces no
	 * further Started after it closes — the player has to release and press again. Held axes recover
	 * on their own (IA_Move is bound on Triggered); buttons are what get lost, which is why §1c reads
	 * as "movement is fine but the action did not happen".
	 *
	 * NOT A BUFFER. It asks the input device what is down RIGHT NOW, so a press made and released
	 * under the overlay is correctly gone. And deliberately only the hold-shaped actions — see the
	 * comment at the call site for why a resting finger must not spend a dash or a 35 s ability.
	 */
	void RedeliverHeldPressEdges();

	/** Priority of our mapping context. Nothing else adds a context, so 0 is fine. */
	static constexpr int32 InputMappingPriority = 0;

	/** Minimum seconds between honoured ServerRequestRespawn calls (anti-spam). */
	static constexpr float RespawnRequestCooldown = 0.5f;

	/** How long look input is dropped after the viewport takes the mouse. See ApplyGameInputMode. */
	static constexpr float LookSuppressAfterCapture = 0.5f;

	/** Look rate budget. A look event above this rate is discarded as a capture artefact. */
	static constexpr float MaxLookDegreesPerSecond = 2500.f;

	/** Floor for the rate budget, so a very high frame rate cannot make the threshold absurdly tight. */
	static constexpr float MinLookSpikeDegrees = 20.f;

	/** World time before which look input is discarded. */
	float IgnoreLookUntilTime = 0.f;

	/** Far enough in the past that the HUD's "age" test fails on the first frame. */
	static constexpr float NeverHitSentinel = -1000.f;

	float LastHitMarkerTime = NeverHitSentinel;
	bool bLastHitMarkerWasKill = false;
	ETraceHitZone LastHitMarkerZone = ETraceHitZone::None;

	bool bScoreboardOpen = false;

	/** SPEC v17 §6. Set once by BuildInputData. See IsUsingInputAssets(). */
	bool bUsingInputAssets = false;

	/**
	 * Latches once AddInputMappings() has REPORTED a mapping-context failure, so a persistent
	 * misconfiguration logs once instead of once per respawn. The check itself is not latched — a
	 * failure that only appears later (seamless travel, a second local player) still gets reported.
	 */
	bool bInputFailureReported = false;

	FString LastKillerName;
	FName LastDeathCause = NAME_None;

	/** Spec v6 §3, remote-carrier feedback. Written only by ClientNotifyParryKill. */
	float LastParryKillTime = NeverHitSentinel;
	FString LastParryKillVictim;

	/** True while the pause/settings overlay owns the screen. See SetGameInputSuppressed. */
	bool bGameInputSuppressed = false;

	/** Registration with UTraceUserSettings::OnChanged, released in EndPlay. */
	FDelegateHandle SettingsChangedHandle;

	/** Server-side: world time of the last respawn request we acted on. */
	float LastRespawnRequestTime = NeverHitSentinel;
};
