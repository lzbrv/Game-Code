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
#include "Engine/EngineTypes.h"          // EEndPlayReason
#include "GameFramework/PlayerController.h"
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
 * Trace's player controller.
 *
 * Input model (contract §7):
 *   Move        Axis2D   WASD          -> X = strafe (+right), Y = forward (+forward)
 *   Look        Axis2D   Mouse X / Y   -> X = yaw delta, Y = pitch delta (already un-inverted)
 *   Jump        bool     Space
 *   Fire        bool     LMB           (doubles as "put me back in" while dead)
 *   Pass        bool     RMB
 *   Dash        bool     Left Shift
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
	void ClientNotifyHit(bool bKilled);

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

	/** Builds InputMapping and every IA_* exactly once. Cheap and safe to call repeatedly. */
	void BuildInputData();

	/** Registers InputMapping with the local player's Enhanced Input subsystem. Idempotent. */
	void AddInputMappings();

	/** Unregisters it again. Called from EndPlay so travel does not leak stale contexts. */
	void RemoveInputMappings();

	// ---- Input handlers -----------------------------------------------------------------------
	void OnMoveInput(const FInputActionValue& Value);
	void OnLookInput(const FInputActionValue& Value);
	void OnJumpStarted();
	void OnJumpCompleted();
	void OnFireStarted();
	void OnFireCompleted();
	void OnPassStarted();
	void OnDashStarted();
	void OnScoreboardStarted();
	void OnScoreboardCompleted();

	/** GetTraceCharacter(), but null when the pawn is missing or dead. */
	ATraceCharacter* GetLivingCharacter() const;

private:
	/** Priority of our mapping context. Nothing else adds a context, so 0 is fine. */
	static constexpr int32 InputMappingPriority = 0;

	/** Minimum seconds between honoured ServerRequestRespawn calls (anti-spam). */
	static constexpr float RespawnRequestCooldown = 0.5f;

	/** Far enough in the past that the HUD's "age" test fails on the first frame. */
	static constexpr float NeverHitSentinel = -1000.f;

	float LastHitMarkerTime = NeverHitSentinel;
	bool bLastHitMarkerWasKill = false;

	bool bScoreboardOpen = false;

	/**
	 * Latches after the first post-add verification of the mapping context, so the diagnostic in
	 * AddInputMappings() reports a misconfiguration once rather than on every respawn.
	 */
	bool bInputContextChecked = false;

	FString LastKillerName;
	FName LastDeathCause = NAME_None;

	/** Server-side: world time of the last respawn request we acted on. */
	float LastRespawnRequestTime = NeverHitSentinel;
};
