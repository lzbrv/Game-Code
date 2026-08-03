// Copyright (c) Trace. All Rights Reserved.
//
// THE CORE IS A STATUS, NOT AN OBJECT (mechanics spec v2 §2).
//
// This class used to be a physical actor: a sphere with a UProjectileMovementComponent, a pickup
// volume, a bounce model and a "loose on the ground" state that anybody could run over. All of
// that is gone. What remains is a single replicated actor whose only real content is a pointer to
// the character currently HOLDING the Core, plus a cosmetic beacon attached to them so both teams
// can see who has it from across a 24000uu field.
//
// The Core is never thrown, never dropped and never loose for longer than a kickoff. It moves in
// exactly three ways, all server-authoritative:
//
//   1. PASS      - the holder hovers their crosshair over a teammate and holds mouse1 for 0.5s.
//   2. KILL      - whoever kills the holder receives it. That includes the trail-dash kill
//                  (UTraceTrailComponent routes the death through the killer's controller) and
//                  the "interception" case, which under a physics-free Core means killing the
//                  holder during their pass window, while their shield is down.
//   3. FALLBACK  - a death with no attributable enemy killer (fall damage, suicide, disconnect)
//                  hands it to the nearest living enemy, and if there is none the Core waits,
//                  holderless, and is granted to that team's next spawn. It must never vanish.
//
// THE RISK BEAT (§4). The moment a pass is INPUT, two things flip on the same frame:
//   - the holder's TRACE becomes invulnerable (an enemy dash can no longer break it), and
//   - the holder LOSES THEIR SHIELD and can be shot.
// Both derive from one replicated bool, bPassActive, so they cannot drift apart. Cancelling
// (looking away, releasing the button, the target dying) reverts both instantly.
//
// NETWORKING
//  - Every mutator early-outs unless HasAuthority(). The GameMode spawns the Core, so on this
//    class HasAuthority() really does mean "the server".
//  - The Core SetOwner()s itself to its current holder. That is what lets the holding client send
//    ServerSetPassInput() to this actor at all (RPC routing needs an owning connection) and it
//    costs nothing else, since nothing here is COND_OwnerOnly.
//  - Movement replication is off permanently: the Core is either attached to a holder (attachment
//    replicates on its own) or parked at its home location, which every machine computes.
//
// LEGACY SHIMS. TryPickup / Throw / DropAt / ResetToCenter are kept, with their old signatures,
// because the GameMode, the endzone, the bots and the debug console still call them. They are
// re-expressed on top of the status model - see each one's comment. New code should call
// GrantTo / RequestPassInput / KickoffTo instead.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"   // FVector_NetQuantizeNormal
#include "GameFramework/Actor.h"
#include "TraceTypes.h"
#include "TraceCore.generated.h"

class AController;
class ATraceCharacter;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

/** Why the Core changed hands. Logging, kill-feed and (for the grace rule) behaviour. */
UENUM()
enum class ETraceCoreGrantReason : uint8
{
	/** Kickoff at match start, after a score, or after a half-time switch. */
	Kickoff = 0,
	/** A completed 0.5s hover pass to a teammate. Reads as continuous possession: no trace grace. */
	Pass = 1,
	/** The holder was killed and the killer is taking it. Includes trail-dash kills. */
	Kill = 2,
	/** The holder died or left with nobody to credit; nearest living enemy receives it. */
	Fallback = 3,
	/** Console / debug / harness grant. */
	Debug = 4
};

/**
 * The single contested objective, modelled as replicated status.
 *
 * Exactly one exists per match and the GameState holds the pointer (ATraceGameState::Core).
 */
UCLASS()
class TRACE_API ATraceCore : public AActor
{
	GENERATED_BODY()

public:
	ATraceCore();

	// =============================================================================================
	// Replicated status
	// =============================================================================================

	/**
	 * The character currently holding the Core, or null during a kickoff window.
	 *
	 * Kept under the old name because the GameMode, the endzone, the HUD and the bots all read it
	 * through GetCarrier(). "Holder" is the term used everywhere in the new code.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Carrier)
	TObjectPtr<ATraceCharacter> Carrier = nullptr;

	/** Only Loose (== holderless) and Carried occur now. InFlight is dead: nothing flies. */
	UPROPERTY(Replicated)
	ECoreState State = ECoreState::Loose;

	// --- Pass window (§4). These three are ONE fact; they replicate together. --------------------

	/**
	 * True from the instant a pass is INPUT until it completes or cancels.
	 *
	 * While true: the holder's trace cannot be broken and the holder's shield is down. Nothing
	 * else in the codebase may store either of those facts separately - they are read back out of
	 * here (IsShieldSuppressedFor / UTraceTrailComponent::IsTraceInvulnerable).
	 */
	UPROPERTY(ReplicatedUsing = OnRep_PassState)
	bool bPassActive = false;

	/** Who the active pass is aimed at. Null unless bPassActive. */
	UPROPERTY(ReplicatedUsing = OnRep_PassState)
	TObjectPtr<ATraceCharacter> PassTarget = nullptr;

	/** Shared-clock time the active pass began, so clients can draw the 0.5s ring themselves. */
	UPROPERTY(Replicated)
	float PassStartServerTime = 0.f;

	/** Shared-clock time the holder may next START a pass. Replicated so the HUD can grey out. */
	UPROPERTY(Replicated)
	float PassCooldownEndServerTime = 0.f;

	// =============================================================================================
	// Contract surface - new API
	// =============================================================================================

	ATraceCharacter* GetCarrier() const;
	ATraceCharacter* GetHolder() const { return GetCarrier(); }
	bool IsHeld() const;

	/** The team currently holding the Core, or None while holderless. */
	ETraceTeam GetHolderTeam() const;

	/** Server. Transfers the Core. The single funnel: every other path ends up here. */
	void GrantTo(ATraceCharacter* NewHolder, ETraceCoreGrantReason Reason);

	/**
	 * Server. Drops possession and queues a kickoff to @p ReceivingTeam after a short delay.
	 *
	 * The delay exists because every caller (a score, match start, half time) teleports ten pawns
	 * immediately afterwards; granting first would lay a trace across the teleport.
	 *
	 * ETraceTeam::None means OUT OF PLAY: park the Core at the centre, holderless, and grant it to
	 * nobody. That is what a half-time interval and a finished match need, and it is deliberately
	 * the same entry point as a kickoff so ATraceGameMode has exactly one function to call.
	 * The next KickoffTo() with a real team puts it back in play.
	 */
	void KickoffTo(ETraceTeam ReceivingTeam);

	/** True while the Core is deliberately out of play (see KickoffTo). */
	bool IsOutOfPlay() const { return bOutOfPlay; }

	// --- Pass input ------------------------------------------------------------------------------

	/**
	 * The holder's mouse1 state. Call on the machine that owns the input; it predicts locally and
	 * forwards to the server. Safe to call from anywhere, including on a non-holder (no-op).
	 */
	void RequestPassInput(bool bPressed);

	/** Server RPC half of RequestPassInput. Routed via this actor's Owner (= the holder). */
	UFUNCTION(Server, Reliable)
	void ServerSetPassInput(bool bPressed);

	/** 0..1 progress of the pass hold, from replicated state, or from local prediction if newer. */
	float GetPassProgress() const;

	/** The teammate the pass is (or is being predicted to be) aimed at. Null when there is none. */
	ATraceCharacter* GetEffectivePassTarget() const;

	/** True while the local machine believes a pass is being held. Server truth wins when present. */
	bool IsPassActive() const;

	/** Seconds until the holder may start another pass. 0 when ready. */
	float GetPassCooldownRemaining() const;

	/**
	 * Whoever the given character would pass to right now, evaluated from THEIR aim. Server-side
	 * truth when called on the server; a local prediction when called on the owning client.
	 * Public so the HUD can highlight the receiver before the button is pressed.
	 */
	ATraceCharacter* FindPassTargetFor(const ATraceCharacter* Holder) const;

	// --- Cross-system queries --------------------------------------------------------------------

	/** The Core for this world, or null. Convenience for callers that only have a UObject. */
	static ATraceCore* Get(const UWorld* World);

	/**
	 * §4: the holder's shield is suppressed for the whole pass window.
	 *
	 * UTraceHealthComponent::IsInvulnerable() must consult this, or the risk beat does not exist.
	 * Returns false for everybody who is not the current holder, so it is safe to call on any
	 * actor from any machine.
	 */
	static bool IsShieldSuppressedFor(const AActor* Character);

	/** §4: while a pass is in flight the holder's trace cannot be broken. */
	static bool IsTraceInvulnerableFor(const AActor* Character);

	// =============================================================================================
	// Legacy shims - see the file header. Kept so foreign call sites still compile.
	// =============================================================================================

	/** Legacy. Now "grant the Core to this character if it is holderless". */
	void TryPickup(ATraceCharacter* Character);

	/**
	 * Legacy pass entry point (ATraceCharacter::PerformPass, and therefore the bots' DoPass()).
	 *
	 * There is no throwing any more, so this is re-expressed as "the holder inputs a pass". The
	 * direction and speed arguments are ignored: the server evaluates the holder's OWN aim, which
	 * is the only thing it is allowed to trust.
	 */
	void Throw(const FVector& Direction, float Speed);

	/**
	 * Legacy. The holder lost the Core with nobody credited (Logout, and the GameMode's
	 * death path before the killer is known). Queues the §2 fallback: nearest living enemy, else
	 * hold for that team's next spawn. Location and impulse are ignored.
	 */
	void DropAt(const FVector& Location, const FVector& Impulse);

	/** Legacy. Kickoff. The team that was scored on receives it (see KickoffTo). */
	void ResetToCenter();

	/** Legacy. Nothing is ever locked out of anything now; always false. */
	bool IsPickupLockedOutFor(const ATraceCharacter* Character) const;

	/** Arena centre, resolved from the arena builder and falling back to the spawn point. */
	FVector GetHomeLocation() const;

	// =============================================================================================
	// AActor
	// =============================================================================================

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * Owner replicates independently of Carrier and can land a frame later. bOwnerNoSee on the orb
	 * and the beacon is resolved against the owner chain cached in the render proxy, so the proxy
	 * has to be rebuilt when it changes or one client sees the marker hidden from the wrong player.
	 */
	virtual void OnRep_Owner() override;

	UFUNCTION()
	void OnRep_Carrier();

	UFUNCTION()
	void OnRep_PassState();

	// =============================================================================================
	// Cosmetic components. No collision anywhere on this actor - it is a status.
	// =============================================================================================

	UPROPERTY(VisibleAnywhere, Category = "Trace|Core")
	TObjectPtr<USceneComponent> Root;

	/** The orb itself, floating above the holder's head. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Core")
	TObjectPtr<UStaticMeshComponent> Mesh;

	/** A tall thin emissive shaft above the orb: this is what makes the holder findable at range. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Core")
	TObjectPtr<UStaticMeshComponent> Beacon;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Bound to the current holder's health component. This is how a kill transfers the Core. */
	UFUNCTION()
	void OnHolderDeath(AActor* Victim, AController* Killer, FName Cause);

private:
	// --- Server ---------------------------------------------------------------------------------

	/** Server. Ticks the pass state machine: acquire, validate, complete, cancel. */
	void ServerTickPass(float DeltaSeconds);

	/** Server. Starts the hold on @p Target. */
	void BeginPass(ATraceCharacter* Target);

	/** Server. Ends the hold, restoring the shield and the trace's vulnerability instantly. */
	void CancelPass(const TCHAR* Reason);

	/** Server. Detaches, clears carrying state on the outgoing holder and unbinds their death. */
	void ReleaseHolder();

	/** Server. Applies the §2 fallback for @p LostTeam: nearest living enemy, else wait for one. */
	void ResolveFallback(ETraceTeam LostTeam);

	/** Server. Grants to the closest living member of PendingGrantTeam, if there is one. */
	bool TryResolvePendingGrant();

	/** Server. Keeps a bot's crosshair on its receiver so bots can satisfy the hover rule. */
	void DriveBotAimAtPassTarget();

	/** Server. Re-arms the holder's trail if anything external switched it off. */
	void EnforceHolderTrailState();

	/** Server + clients. Applies attachment, collisionless-ness and the beacon transform. */
	void ApplyAttachment();

	/** Recolours the orb and the beacon for the holding team. No-op on a dedicated server. */
	void UpdateVisuals();

	/** Pushes bPassActive onto the holder's trail component so the trace hardens/softens. */
	void ApplyTraceInvulnerability();

	/** Shared clock, matching UTraceTrailComponent::GetServerTimeSeconds(). */
	float GetServerTimeSeconds() const;

	/**
	 * True if @p Candidate is a legal receiver for @p Holder right now: alive, on their team, in
	 * range, in line of sight, and (when @p bRequireAim) under their crosshair.
	 *
	 * bRequireAim is false for AI holders during an active pass only. A bot's crosshair is driven
	 * BY this class (DriveBotAimAtPassTarget), and the bot controller's own aim slew runs in an
	 * unspecified order relative to this actor's tick, so re-testing the hover against a rotation we
	 * just wrote is a coin flip that would make bot passes cancel at random. Range, line of sight
	 * and "still alive and on my team" are enforced for bots exactly as for humans.
	 */
	bool IsLegalPassTarget(const ATraceCharacter* Holder, const ATraceCharacter* Candidate, bool bRequireAim = true) const;

	/** Every character the match knows about. GameMode list, with an actor-iterator fallback. */
	void GatherCharacters(TArray<ATraceCharacter*>& OutCharacters) const;

	// --- Server state ----------------------------------------------------------------------------

	/** Mouse1 as last reported by the holder. The server never trusts anything else from them. */
	bool bPassInputHeld = false;

	/** The holder whose OnDeath we are currently bound to. */
	TWeakObjectPtr<ATraceCharacter> BoundDeathHolder;

	/**
	 * The holder we most recently let go of.
	 *
	 * Load-bearing, and the reason is an ordering hazard worth spelling out. The health component's
	 * OnDeath is a multicast: ATraceCharacter is bound first, and its handler runs the whole death
	 * chain (HandleDeath -> ATraceGameMode::NotifyCharacterDied -> ATraceCore::DropAt) BEFORE our own
	 * OnHolderDeath listener on the same broadcast is reached. By then Carrier is already null, so a
	 * naive "is the victim my holder?" test would reject the very death that is supposed to transfer
	 * the Core - and the killer would silently lose their reward to the nearest-enemy fallback.
	 */
	TWeakObjectPtr<ATraceCharacter> RecentlyReleasedHolder;

	/** Team owed the Core while it is holderless. None = nobody is waiting. */
	ETraceTeam PendingGrantTeam = ETraceTeam::None;

	/**
	 * Set by KickoffTo(ETraceTeam::None): the Core is parked and deliberately unowned.
	 *
	 * This exists to distinguish "nobody has it yet" (which must self-heal, because there is no
	 * pickup left and a holderless Core is a dead match) from "nobody is SUPPOSED to have it"
	 * (half-time interval, post-match), which must not. The self-heal is not deleted, only
	 * lengthened: see the guarded recovery in Tick().
	 */
	bool bOutOfPlay = false;

	/** Shared-clock time the pending grant may be resolved (kickoffs wait out the teleport). */
	float PendingGrantTime = 0.f;

	/** Set by DropAt(); resolved on the next tick unless OnHolderDeath beats it to the punch. */
	bool bFallbackQueued = false;
	ETraceTeam FallbackTeam = ETraceTeam::None;

	FVector SpawnHomeLocation = FVector::ZeroVector;

	// --- Local prediction (owning client, and the listen host) -------------------------------------

	bool bLocalPassPredicted = false;
	float LocalPassPredictStartTime = 0.f;
	TWeakObjectPtr<ATraceCharacter> LocalPassPredictTarget;

	// --- Applied-state reconciliation --------------------------------------------------------------

	/** Carrier/State/bPassActive are separate properties and can land in any order. */
	TWeakObjectPtr<ATraceCharacter> AppliedHolder;
	bool bAppliedEver = false;
	bool bAppliedPassActive = false;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MeshMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BeaconMID = nullptr;

	bool bMaterialIsNeon = false;
	FLinearColor AppliedColor = FLinearColor::White;
	bool bColorApplied = false;
};
