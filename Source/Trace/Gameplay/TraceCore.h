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
// LEGACY SHIMS. TryPickup and DropAt are kept, with their old signatures, because they still have
// live callers - the debug console (Trace.DebugTakeCore) and the uncredited-loss path
// (ATraceGameMode::Logout, and the death path before the killer is known) respectively. They are
// re-expressed on top of the status model - see each one's comment. New code should call
// GrantTo / RequestPassInput / KickoffTo instead.
//
// Throw, ResetToCenter and IsPickupLockedOutFor were also kept "so foreign call sites compile", and
// were then found to have ZERO call sites between them. All three are deleted; so is the replicated
// ECoreState, which nothing read. A shim that shims nothing is just a second, wronger description of
// the mechanic - Throw's own doc comment pointed at three functions that no longer exist.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"   // FVector_NetQuantizeNormal
#include "GameFramework/Actor.h"
#include "TraceTypes.h"
#include "TraceCore.generated.h"

class AController;
class ATraceCharacter;
class ATracePlayerState;
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
 * Which test in IsLegalPassTarget() refused a candidate receiver.
 *
 * The distinction that MATTERS is IsTransientPassRejection(): the last three are geometry sampled
 * once per frame against a field full of cover, so a running receiver blinks through them
 * constantly and an in-flight pass must be allowed to ride those blinks out (spec §4.1, the pass
 * measured dying 24 ms before completion). The first three are facts about the receiver - dead, an
 * enemy, gone - and must cancel a pass immediately, with no grace at all.
 *
 * Values are stable because Trace.PassStats indexes its refusal histogram by them.
 */
enum class ETracePassRejectReason : uint8
{
	None = 0,
	InvalidOrSelf = 1,
	Dead = 2,
	NotAnAlly = 3,
	OutOfRange = 4,
	NoLineOfSight = 5,
	Behind = 6,
	NotUnderCrosshair = 7
};

/** True for the geometric tests a momentary blink of which must not cancel an in-flight pass. */
inline bool IsTransientPassRejection(ETracePassRejectReason Reason)
{
	return Reason == ETracePassRejectReason::OutOfRange
		|| Reason == ETracePassRejectReason::NoLineOfSight
		|| Reason == ETracePassRejectReason::Behind
		|| Reason == ETracePassRejectReason::NotUnderCrosshair;
}

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

	// ATraceCore::State IS DELETED, and ECoreState with it. It was a REPLICATED property written in
	// two places and read by nobody in the entire module — a replicated property and a DOREPLIFETIME
	// entry spent on a fact no code consulted. Its enum was worse than useless: InFlight
	// ("travelling under projectile movement") was unreachable because nothing flies, and Loose was
	// documented "on the ground, pickable by anyone", which flatly contradicts the model — there is
	// no pickup. Carrier == nullptr is the only "holderless" test anything needs.

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
	 * forwards to the server.
	 *
	 * @p Requester is the pawn whose button this is, and it is NOT optional bookkeeping.
	 * bPassInputHeld is a single latch shared by the whole match, so "safe to call on a non-holder"
	 * — which this used to claim — was false in both directions: a non-holder's press could arm the
	 * holder's pass, and a non-holder's mouse1 RELEASE could cancel it. The rules are now:
	 *
	 *   PRESS   accepted only from the living current holder.
	 *   RELEASE accepted from the current holder OR from whoever latched it. The second half is
	 *           load-bearing: a completed pass moves the Core to the receiver BEFORE the player's
	 *           finger leaves the button, so the passer is no longer the holder when their release
	 *           arrives, and dropping it there is what leaves the latch set into the next possession.
	 */
	void RequestPassInput(bool bPressed, ATraceCharacter* Requester);

	/**
	 * Server RPC half of RequestPassInput. Routed via this actor's Owner (= the holder).
	 *
	 * Requester travels with the call rather than being inferred from the Owner: ownership moves the
	 * instant the Core does, so a release sent a frame before a transfer would otherwise be applied
	 * to the pawn that just RECEIVED the Core.
	 */
	UFUNCTION(Server, Reliable)
	void ServerSetPassInput(bool bPressed, ATraceCharacter* Requester);

	/** 0..1 progress of the pass hold, from replicated state, or from local prediction if newer. */
	float GetPassProgress() const;

	/** The teammate the pass is (or is being predicted to be) aimed at. Null when there is none. */
	ATraceCharacter* GetEffectivePassTarget() const;

	/** True while the local machine believes a pass is being held. Server truth wins when present. */
	bool IsPassActive() const;

	/** Seconds until the holder may start another pass. 0 when ready. */
	float GetPassCooldownRemaining() const;

	/**
	 * Diagnostics: the raw latched mouse1 state the server is holding for the current holder.
	 *
	 * Meaningful on the authority only (a client never writes it). Exposed because a latched pass
	 * input with nobody's finger on the button is invisible in every other reading of the game —
	 * see Trace.DebugViewProbe, which prints it next to the carrier flag it can silently corrupt.
	 */
	bool IsPassInputHeld() const { return bPassInputHeld; }

	/** Diagnostics: true once the local machine has predicted a pass that the server has not confirmed. */
	bool IsPassLocallyPredicted() const { return bLocalPassPredicted; }

	/**
	 * Whoever the given character would pass to right now, evaluated from THEIR aim. Server-side
	 * truth when called on the server; a local prediction when called on the owning client.
	 * Public so the HUD can highlight the receiver before the button is pressed.
	 */
	ATraceCharacter* FindPassTargetFor(const ATraceCharacter* Holder) const;

	// PUBLIC, not private, for one reason: the console diagnostic Trace.DebugPassTargets asks the
	// REAL rule below why each teammate was refused. A debug command that re-implemented the rule
	// would be a second copy of it, and a second copy is how a pass-acquisition bug gets diagnosed
	// against logic the game does not actually run. Both are const queries; neither moves the Core.

	/**
	 * True if @p Candidate is a legal receiver for @p Holder right now: alive, on their team, in
	 * range, in line of sight, and (when @p bRequireAim) under their crosshair.
	 *
	 * bRequireAim is false for AI holders during an active pass only. A bot's crosshair is driven
	 * BY this class (DriveBotAimAtPassTarget), and the bot controller's own aim slew runs in an
	 * unspecified order relative to this actor's tick, so re-testing the hover against a rotation we
	 * just wrote is a coin flip that would make bot passes cancel at random. Range, line of sight
	 * and "still alive and on my team" are enforced for bots exactly as for humans.
	 *
	 * @param OutRejectReason optional; on a false return, set to a literal naming the test that
	 *        failed. Diagnostics only (Trace.DebugPassTargets), and the reason this is an out-param
	 *        rather than a second copy of the rules in the debug command: a copy would drift, and a
	 *        pass-acquisition bug diagnosed against a drifted copy is worse than no diagnosis.
	 */
	bool IsLegalPassTarget(const ATraceCharacter* Holder, const ATraceCharacter* Candidate, bool bRequireAim = true,
		const TCHAR** OutRejectReason = nullptr, ETracePassRejectReason* OutRejectCode = nullptr) const;

	/** Every character the match knows about. GameMode list, with an actor-iterator fallback. */
	void GatherCharacters(TArray<ATraceCharacter*>& OutCharacters) const;

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

	/**
	 * True if @p Character is the CURRENT holder, answered from the Core's own Carrier pointer.
	 * Never from the pawn mirror.
	 *
	 * Use this — not ATraceCharacter::IsCarrier() — anywhere a GAMEPLAY RULE turns on who is
	 * holding the Core. The pawn's bIsCarrier and the PlayerState's are presentation mirrors: they
	 * drive the camera mode, the tint and the scoreboard, and they are replicated separately from
	 * the Core, so there are frames in which they disagree with it. Damage and hit resolution used
	 * to mix the two in a single expression (`Target->IsCarrier() && !IsShieldSuppressedFor(Target)`
	 * — a mirror AND the truth), which is correct today only by luck of ordering.
	 *
	 * Safe on any actor, on any machine, including null.
	 */
	static bool IsCoreHolder(const AActor* Character);

	/** §4: while a pass is in flight the holder's trace cannot be broken. */
	static bool IsTraceInvulnerableFor(const AActor* Character);

	// =============================================================================================
	// Legacy shims - see the file header. Kept so foreign call sites still compile.
	// =============================================================================================

	/** Legacy. Now "grant the Core to this character if it is holderless". */
	void TryPickup(ATraceCharacter* Character);

	/**
	 * Legacy. The holder lost the Core with nobody credited (Logout, and the GameMode's
	 * death path before the killer is known). Queues the §2 fallback: nearest living enemy, else
	 * hold for that team's next spawn. Location and impulse are ignored.
	 */
	void DropAt(const FVector& Location, const FVector& Impulse);

	// Throw / ResetToCenter / IsPickupLockedOutFor ARE DELETED — all three had zero callers. See the
	// tombstones in the .cpp. TryPickup and DropAt below are kept because they DO have live callers
	// (Trace.DebugTakeCore, and the Logout / uncredited-death path respectively).

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

	/**
	 * Server, diagnostics only, throttled to 10 Hz and gated on Trace.PassStats.
	 *
	 * Answers "how often does the pass option fail to show up, and which test refused each teammate"
	 * with a number instead of an impression. Runs the REAL rule (IsLegalPassTarget) so the answer
	 * cannot drift from the game.
	 */
	void SamplePassAvailabilityStats();

	/**
	 * Server. Forgets the held mouse1 latch WITHOUT a release having been heard.
	 *
	 * Legitimate in exactly one situation: possession changed, so the button that armed the latch
	 * belongs to a pawn that is no longer the holder. Deliberately NOT called from CancelPass - see
	 * the note there.
	 */
	void ClearPassInput();

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


	// --- Server state ----------------------------------------------------------------------------

	/** Mouse1 as last reported by the holder. The server never trusts anything else from them. */
	bool bPassInputHeld = false;

	/**
	 * The pawn that latched bPassInputHeld, i.e. whose finger is on the button.
	 *
	 * Tracked on every machine (each keeps its own copy; nothing about it is replicated) so that the
	 * release can be matched to the press even after the Core has changed hands in between — see
	 * RequestPassInput. Cleared with the latch itself in CancelPass.
	 */
	TWeakObjectPtr<ATraceCharacter> PassInputInstigator;

	/**
	 * Shared-clock time the ACTIVE pass's receiver last passed IsLegalPassTarget(), or <= 0 when
	 * they are legal right now.
	 *
	 * §4.1: line of sight and "under the crosshair" are instantaneous tests sampled once per frame,
	 * and the new arena's cover density means a running receiver blinks out of legality repeatedly
	 * during any 0.5 s hold. Cancelling on the first blink is what killed a pass 24 ms before it
	 * completed. The pass now cancels only when the receiver has been continuously illegal for
	 * Trace.Pass.GraceSeconds, and only for the geometric tests - death or a team change still
	 * cancels on the frame it happens (see IsTransientPassRejection).
	 */
	float PassGraceStartServerTime = 0.f;

	/** The holder whose OnDeath we are currently bound to. */
	TWeakObjectPtr<ATraceCharacter> BoundDeathHolder;

	/**
	 * The holder's PlayerState, cached in GrantTo().
	 *
	 * Exists for one reason: ATracePlayerState OUTLIVES the pawn, so the bIsCarrier mirror has to
	 * stay clearable after the pawn is gone. ReleaseHolder() has a real path where the pawn is
	 * already invalid (Tick step 2, a GC'd pawn, level teardown), and without a handle taken while
	 * the pawn was alive there is no way to reach the PlayerState — which left a phantom carrier on
	 * the scoreboard permanently. Cleared in ReleaseHolder BEFORE its `!IsValid(Previous)` return.
	 *
	 * Weak, not raw: a disconnect destroys the PlayerState too, and this must not keep it alive.
	 */
	TWeakObjectPtr<ATracePlayerState> HolderPlayerState;

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

	// --- Sticky acquisition (per machine; nothing here is replicated) -------------------------------
	//
	// FindPassTargetFor() is a const query called from three places with different holders - the
	// gameplay path (the carrier), ATraceHUD's 20 Hz highlight poll (the local player) and the bot
	// controller. So the cache is keyed by holder and mutable: it must not change the meaning of a
	// const query, only stop the answer flickering on and off between frames. See
	// TraceCoreTuning::PassAcquireStickyDefault for why the flicker is the reported bug.

	mutable TWeakObjectPtr<ATraceCharacter> StickyAcquireHolder;
	mutable TWeakObjectPtr<ATraceCharacter> StickyAcquireTarget;
	mutable float StickyAcquireServerTime = 0.f;

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
