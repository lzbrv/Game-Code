// Trace — player controller.
//
// Owns the *entire* Enhanced Input setup. The project ships no .uasset (contract §2), so the
// mapping context, every UInputAction and every modifier are constructed here with NewObject and
// kept alive by UPROPERTY members. All gameplay input is funnelled through the
// ATraceCharacter::Do* entry points; the controller itself never mutates gameplay state.
//
// Why transient input objects are safe (and in fact better than assets): MapKey() mutates the
// mapping context it is called on. Done to a loaded .uasset that mutation persists past PIE and
// dirties the asset. A NewObject'd context is per-controller and dies with it.
//
// The one thing that will silently break this file: dropping the UPROPERTY on any IA_* member.
// NewObject'd UInputActions have no other owner rooting them, so they would be collected at the
// next GC and input would stop working mid-match with no error anywhere.

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
 * Input model (contract §7). Every key below is the DEFAULT — all of them are rebindable through
 * the options screen, and the authority at runtime is UTraceUserSettings, not this comment:
 *   Move        Axis2D   WASD          -> X = strafe (+right), Y = forward (+forward)
 *   Look        Axis2D   Mouse X / Y   -> X = yaw delta, Y = pitch delta, scaled and signed by
 *                                         the player's sensitivity and invert-Y settings
 *   Jump        bool     Space
 *   Crouch      bool     Left Ctrl     (slide on the ground, fast-fall in the air)
 *   Fire        bool     LMB           (doubles as "put me back in" while dead)
 *   Pass        bool     RMB
 *   Dash        bool     Left Shift
 *   Parry       bool     Q             (carrier only — 0.1s of trace invulnerability, spec v3 §3)
 *   Scoreboard  bool     Tab           (held)
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
	/** Bumped by ClientNotifyHit — a server-confirmed hitscan resolution credited to us. */
	int32 DebugHitConfirmCount = 0;

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
	// ---- Enhanced Input data, all built in C++ ------------------------------------------------
	//
	// These MUST stay UPROPERTYs — see the file header. Transient because they are rebuilt from
	// code on every load and must never be serialised.

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
	 * Builds InputMapping and every IA_* exactly once, then lays down the key mappings.
	 *
	 * The two halves are separate on purpose. The ACTIONS must be created exactly once and never
	 * replaced, because SetupInputComponent binds delegates to those specific objects and a fresh
	 * UInputAction would leave every binding pointing at an action nothing maps to. The KEY MAPPINGS
	 * are torn down and rebuilt on every settings change. Cheap and safe to call repeatedly.
	 */
	void BuildInputData();

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
	void OnCrouchStarted();
	void OnCrouchCompleted();
	void OnScoreboardStarted();
	void OnScoreboardCompleted();

	/** GetTraceCharacter(), but null when the pawn is missing or dead. */
	ATraceCharacter* GetLivingCharacter() const;

private:
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

	/**
	 * Latches once AddInputMappings() has REPORTED a mapping-context failure, so a persistent
	 * misconfiguration logs once instead of once per respawn. The check itself is not latched — a
	 * failure that only appears later (seamless travel, a second local player) still gets reported.
	 */
	bool bInputFailureReported = false;

	FString LastKillerName;
	FName LastDeathCause = NAME_None;

	/** True while the pause/settings overlay owns the screen. See SetGameInputSuppressed. */
	bool bGameInputSuppressed = false;

	/** Registration with UTraceUserSettings::OnChanged, released in EndPlay. */
	FDelegateHandle SettingsChangedHandle;

	/** Server-side: world time of the last respawn request we acted on. */
	float LastRespawnRequestTime = NeverHitSentinel;
};
