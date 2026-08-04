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
class UPrimitiveComponent;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Declared in TraceSettings.h. Forward declared (legal: the underlying type is fixed) so that this
 * header stays free of the 110 kB settings header — every other file in the module includes this
 * one, and most of them only ever need IsModeB().
 */
enum class ETraceScoringMode : uint8;

// =================================================================================================
// MODE B — THE THROWABLE, INTERCEPTABLE CORE (spec v4 §7)
//
// Everything above this line describes MODE A, which is unchanged and still the default. Mode B is
// a second ruleset living behind UTraceSettings::ScoringMode, and it is built as an ADDITION to the
// model above rather than a replacement:
//
//   * possession, the trace, the turnover grace, the kill steal, the parry and the fallback are the
//     SAME code. GrantTo() is still the single funnel; ReleaseHolder() is still the single exit.
//   * what mode B adds is one extra place the Core can be: LOOSE. bLoose is true from the moment a
//     throw leaves the holder's hands until somebody takes it (or the reset timer fires).
//   * LMB is mode-dependent. RequestPassInput(true) starts the 0.5 s hover-pass in mode A and
//     THROWS in mode B, so every existing caller — a human's mouse, DoPassPressed, the bots'
//     ApplyPassInput — reaches the right verb without knowing the mode exists.
//
// WHAT THIS IS DELIBERATELY *NOT*. Spec v2 deleted a physical Core built from a
// UProjectileMovementComponent, a pickup sphere and a replicated "loose" enum, and that model had a
// string of re-entrancy bugs — including one where re-enabling pickup collision during a reset ran
// a synchronous overlap, attached the Core to whoever happened to be standing there and then stored
// its home position as a RELATIVE offset. None of that is resurrected here:
//
//   * NO UProjectileMovementComponent. The loose Core is integrated in ServerTickLooseCore() as a
//     handful of lines of ballistics plus ONE sphere sweep per tick. There is no component whose
//     own tick can fire between our state changes.
//   * NO PICKUP COLLIDER. This actor still has no collision of any kind. "First contact takes it"
//     is a server-side proximity POLL over the character roster (ServerTryLoosePickup), so there is
//     no collision to enable, therefore no synchronous overlap to fire, therefore no way for a
//     state flip to hand the Core to a bystander.
//   * EVERY "flip state then move the Core" sequence is wrapped in FCoreStateLock, which makes it
//     indivisible: any re-entrant throw / pickup / reset while one is in progress is refused
//     outright rather than interleaved.
//
// Scoring in mode B is a discrete GOAL at each end, never the full-width endzone. The geometry is
// never reconstructed here: it is read from the goal volumes ATraceArenaBuilder already builds
// (ATraceEndzone::IsGoalVolume / IsZoneActive / GetZoneBounds), with RegisterGoalVolume as a hook
// for any other system that owns a goal. Asking the volume that scores is what keeps the goal a
// player can see and the goal that awards points the same box, across the half-time side switch and
// any change to field size.
//
// WHICH MODE IS BEING PLAYED is likewise not stored here. ATraceGameState publishes it and this
// class asks (IsModeB()) at the point of use.
// =================================================================================================

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
	Debug = 4,

	// --- Mode B only -----------------------------------------------------------------------------
	//
	// Two reasons rather than one "pickup", because the SPEC MAKES THE DISTINCTION LOAD-BEARING:
	// "Turnovers in game state b should retain the grace period from game state a. However,
	// teammates picking up the core should not have a grace period." Interception crosses teams and
	// buys the 0.5 s trace grace; recovery does not and buys nothing. Splitting them here means the
	// grace decision is made by the same `AreAllies(PreviousTeam, NewTeam)` line mode A uses, and
	// the reason is only what the log and the kill feed print.

	/** A loose Core taken by a player on the OTHER team from the one that last held it. */
	Interception = 5,
	/** A loose Core taken by the team that last held it (thrower's own team, or the thrower). */
	Recovery = 6
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

	// --- Mode B state. All of it is inert, and stays at these defaults, in mode A. ----------------

	/**
	 * MODE B. True while the Core is in flight or resting on the ground with no holder.
	 *
	 * This is the state spec v2 deleted, brought back deliberately and confined to mode B: in mode A
	 * it is false for the entire match and every mode-B branch below is dead code. It is NOT the old
	 * ECoreState enum — there is exactly one extra bit of state ("is it out in the world"), because
	 * "in flight" versus "at rest" is a fact about LooseVelocity and not something anybody needs
	 * replicated separately.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Loose)
	bool bLoose = false;

	/** MODE B. Authoritative position of the loose Core. Only meaningful while bLoose. */
	UPROPERTY(Replicated)
	FVector_NetQuantize LooseLocation = FVector::ZeroVector;

	/**
	 * MODE B. Authoritative velocity of the loose Core, uu/s.
	 *
	 * Replicated so clients can dead-reckon between net updates rather than watching the Core
	 * teleport along its own arc at the net update rate. Movement replication stays OFF (see the
	 * constructor): ReplicatedMovement would fight the attachment the held Core still uses.
	 */
	UPROPERTY(Replicated)
	FVector_NetQuantize LooseVelocity = FVector::ZeroVector;

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

	// =============================================================================================
	// MODE B — scoring mode, the throw, the loose Core, the goals
	//
	// Every function here is a no-op (or returns the mode-A answer) while ScoringMode is mode A, so
	// callers never have to branch on the mode themselves.
	// =============================================================================================

	/**
	 * True when this match is playing mode B: goals, and a Core that can be thrown and intercepted.
	 *
	 * ASKED OF ATraceGameState, EVERY TIME, and never cached here. The mode is published once by
	 * ATraceGameMode::PublishScoringMode() from the travel URL and replicated on the GameState,
	 * which makes it the one legal answer; a copy latched on this actor would be a second source of
	 * truth for a fact that decides what this actor IS, and the two would eventually disagree on
	 * exactly the frame it mattered.
	 */
	bool IsModeB() const;

	/** Mode test for anybody holding only a UObject. False when there is no match yet. */
	static bool IsModeB(const UWorld* World);

	/**
	 * Server. "The scoring mode has just been published; reconcile now."
	 *
	 * NOT A SETTER, despite the name it kept so that ATraceGameMode::PublishScoringMode still
	 * compiles. It does not store the mode — ATraceGameState does, and this class asks. What it does
	 * is run the mode-change reconciliation on THIS frame rather than on the next tick, which
	 * matters because the frame a mode is published is also the frame the arena arms the other pair
	 * of volumes and the GameMode kicks off.
	 *
	 * @p PublishedMode is compared against what the GameState is actually reporting, and a
	 * disagreement is logged rather than obeyed: if those two ever differ, the ordering bug is the
	 * thing worth knowing about, and silently preferring either one would hide it.
	 */
	void SetScoringMode(ETraceScoringMode PublishedMode);

	/** MODE B. True while the Core is loose in the world with no holder. Always false in mode A. */
	bool IsLoose() const { return bLoose; }

	/** MODE B. Velocity of the loose Core, uu/s. Zero when held or at rest. */
	FVector GetLooseVelocity() const { return LooseVelocity; }

	/**
	 * MODE B. Where a chaser should run: the loose Core's position, lead by @p LeadSeconds of its
	 * own velocity so a bot does not chase where the Core WAS. False when the Core is not loose.
	 */
	bool GetLooseCoreInterceptPoint(float LeadSeconds, FVector& OutPoint) const;

	/**
	 * Server, MODE B. The holder throws the Core along their own aim.
	 *
	 * Direction comes from the SERVER's copy of @p Thrower's aim (ATraceCharacter::GetAimDirection),
	 * never from anything a client sent — the same policy that deleted the old PerformPass, whose
	 * whole payload was a client-supplied direction.
	 *
	 * @return true when a throw actually left. False for a wrong mode, a non-holder, a dead holder,
	 *         a Core that is already loose, or a throw still on cooldown.
	 */
	bool ThrowFromHolder(ATraceCharacter* Thrower);

	// --- Goals -----------------------------------------------------------------------------------

	/**
	 * Registers a goal volume. Call from the goal actor's BeginPlay (server or not; non-authority
	 * entries are simply never consulted for scoring).
	 *
	 * THIS IS THE HOOK FOR THE REAL GOAL ACTOR. While nothing is registered, the Core derives its
	 * goals from the live ATraceEndzone boxes instead — narrowed to UTraceSettings::
	 * GoalWidthFieldFraction and capped at GoalHeightUU — so mode B is playable and testable before
	 * the goal geometry lands, and stops deriving the moment one real volume registers.
	 *
	 * @param GoalOwner       the goal actor; also the key for UnregisterGoalVolume.
	 * @param DefendingTeam   the team that DEFENDS this goal. Their OPPONENT scores in it, exactly
	 *                        as with ATraceEndzone::OwningTeam. Getting this backwards produces a
	 *                        game that looks like it works and is precisely inverted.
	 * @param Volume          the primitive whose world bounds are the goal mouth.
	 */
	static void RegisterGoalVolume(AActor* GoalOwner, ETraceTeam DefendingTeam, UPrimitiveComponent* Volume);

	/** Removes a registration. Call from the goal actor's EndPlay. */
	static void UnregisterGoalVolume(AActor* GoalOwner);

	/**
	 * The centre of the goal @p AttackingTeam scores in, for bot steering and HUD markers.
	 *
	 * In mode B this is the narrow goal mouth; in mode A there are no goals and this returns false,
	 * which is the caller's cue to keep using the endzone. Answered from the same boxes that score,
	 * so a bot cannot run at a goal the rule does not recognise.
	 */
	static bool GetAttackGoalCentre(const UWorld* World, ETraceTeam AttackingTeam, FVector& OutCentre);

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

	UFUNCTION()
	void OnRep_Loose();

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

	// --- Mode B internals -------------------------------------------------------------------------

	/**
	 * The indivisibility guard the file header promises.
	 *
	 * Every mode-B sequence that flips state AND moves the Core (throw, pickup, reset, mode change)
	 * takes one of these for its whole duration, and every one of them refuses to start while
	 * another holds it. That is what makes "flip then move" atomic with respect to anything those
	 * steps can call back into — GrantTo drives the GameMode's score check, which resets the field,
	 * which kicks off, which releases this Core, and without the lock that re-entry would run
	 * against half-updated loose state. Not a mutex: this is single-threaded game code, and the
	 * hazard is re-entrancy, not concurrency.
	 */
	struct FCoreStateLock
	{
		explicit FCoreStateLock(ATraceCore* InCore) : Core(InCore) { Core->bCoreStateLocked = true; }
		~FCoreStateLock() { Core->bCoreStateLocked = false; }
		FCoreStateLock(const FCoreStateLock&) = delete;
		FCoreStateLock& operator=(const FCoreStateLock&) = delete;

	private:
		ATraceCore* Core;
	};

	/** Server, mode B. Integrates the loose Core, tests the goals, and polls for a taker. */
	void ServerTickLooseCore(float DeltaSeconds);

	/** Server, mode B. Nearest living player within the pickup radius takes it. True if one did. */
	bool ServerTryLoosePickup();

	/** Server, mode B. The guarded loose -> held transition. Sets the grace override, then grants. */
	void TakeLooseCore(ATraceCharacter* Taker);

	/** Clears every loose field. Does NOT grant: callers decide where the Core goes next. */
	void ClearLooseState();

	/** Server, mode B. The Core has been loose too long, or has left the world. Put it back in play. */
	void ResetLooseCore(const TCHAR* Reason);

	/**
	 * Server, mode B. Did the segment @p From -> @p To cross a goal @p ScoringTeam scores in?
	 *
	 * Swept rather than sampled: a Core thrown at 2600 uu/s covers 43 uu per frame at 60 Hz and far
	 * more on a hitching server, so a point test at each frame boundary can miss a 700 uu goal mouth
	 * outright. Awards the point (ATraceGameMode::NotifyScored) and returns true.
	 */
	bool CheckGoalScore(const FVector& From, const FVector& To, ETraceTeam ScoringTeam, const TCHAR* How);

	/** Rebuilds GoalBoxes from the registrations, or from the endzone volumes when there are none. */
	void RefreshGoalVolumes(bool bForce);

	/**
	 * Server, mode B, DIAGNOSTICS ONLY (Trace.ModeB.Verify).
	 *
	 * Puts the Core into the world at @p From with @p LaunchVelocity, as if thrown by @p FromTeam,
	 * through the same guarded release-then-place sequence a real throw uses. It exists because two
	 * of mode B's four rules — a Core thrown INTO a goal, and the reset timer — are reachable in a
	 * live match only by waiting for a bot to happen to produce them, and "we never saw it fail" is
	 * not evidence that a rule works.
	 */
	bool DebugLaunchLoose(const FVector& From, const FVector& LaunchVelocity, ETraceTeam FromTeam);

	/** Server. Runs the Trace.ModeB.Verify scenario, one step at a time. See the .cpp. */
	void TickModeBVerification();

	/**
	 * Called when the observed scoring mode changes. Normalises anything the new mode cannot
	 * represent — in practice, a Core that is loose when mode A is selected, which mode A has no
	 * pickup for and would leave lying there forever.
	 *
	 * It does NOT touch the scoring volumes. ATraceArenaBuilder builds both pairs and arms the
	 * right one (ATraceEndzone::SetZoneActive), so a second arming pass here would be one system
	 * fighting another over the same flag.
	 */
	void OnScoringModeChanged();


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

	// --- Mode B server state (nothing here is replicated; see the replicated block above) ----------

	/** True for the duration of a guarded throw / pickup / reset / mode change. See FCoreStateLock. */
	bool bCoreStateLocked = false;

	/**
	 * The team that last HELD the Core before it went loose.
	 *
	 * This is the whole grace rule in mode B. GrantTo() decides the turnover grace from
	 * `AreAllies(PreviousTeam, NewTeam)`, and when a loose Core is taken there is no previous
	 * holder to ask — Carrier is already null. Without this, every mode-B pickup would look like a
	 * team change and a teammate recovering a throw would be given a 0.5 s trace grace the spec
	 * explicitly denies them.
	 */
	ETraceTeam LooseFromTeam = ETraceTeam::None;

	/**
	 * Set immediately before the one GrantTo() call that follows a loose pickup, and consumed (and
	 * cleared) inside it.
	 *
	 * A single-use override rather than an extra parameter on GrantTo, because GrantTo is the shared
	 * funnel both modes drive and mode A must not grow an argument it can only ever pass None to.
	 */
	ETraceTeam GraceOverrideTeam = ETraceTeam::None;

	/** Who threw it. Held only for the brief self-pickup lockout and for the log line. */
	TWeakObjectPtr<ATraceCharacter> LooseThrower;

	/** Shared-clock time the Core went loose: drives the self-pickup lockout and the reset timer. */
	float LooseStartServerTime = 0.f;

	/** Shared-clock time the holder may next throw. Mirrors the pass cooldown's role in mode A. */
	float ThrowCooldownEndServerTime = 0.f;

	/** True once the loose Core has come to rest, so the log says "thrown" or "loose" correctly. */
	bool bLooseAtRest = false;

	/** Previous frame's holder position, so the mode-B carry-in test is swept rather than sampled. */
	FVector LastCarrierGoalTestLocation = FVector::ZeroVector;

	/** One goal mouth, in world space, with the team that DEFENDS it. */
	struct FTraceGoalBox
	{
		FBox Box = FBox(ForceInit);
		ETraceTeam DefendingTeam = ETraceTeam::None;
	};

	/** Refreshed on a slow cadence so the half-time side switch is picked up without polling cost. */
	TArray<FTraceGoalBox> GoalBoxes;
	float GoalBoxRefreshTime = 0.f;

	/** The mode OnScoringModeChanged() has already acted on, so a steady mode costs one compare. */
	bool bAppliedModeB = false;
	bool bModeEverApplied = false;

	// --- Trace.ModeB.Verify scenario state (diagnostics only) --------------------------------------

	int32 VerifyStep = -1;
	float VerifyStepDeadline = 0.f;
	ETraceTeam VerifyExpectTeam = ETraceTeam::None;
	bool VerifyExpectGrace = false;
	TWeakObjectPtr<ATraceCharacter> VerifyThrower;

	/**
	 * The take that ended the step, captured BY TakeLooseCore at the moment it happened.
	 *
	 * Not polled. The first version of this scenario watched for "somebody is now carrying" and
	 * reported the first carrier it saw, which on a live server is as likely to be a KICKOFF as a
	 * pickup — it caught the 1st-half kickoff landing on the same frame as the throw and reported a
	 * failure of a rule that had not run. A step now only ever judges the take it actually caused.
	 */
	bool bVerifyAwaitingTake = false;
	bool bVerifyTakeSeen = false;
	ETraceTeam VerifyTookTeam = ETraceTeam::None;

	/**
	 * The team the Core was thrown BY, captured with the take.
	 *
	 * Captured rather than read back afterwards because GrantTo overwrites LooseFromTeam with the new
	 * holder's team as part of taking possession — so by the time the step is judged, "who threw it"
	 * has already become "who has it", and every interception looked like a same-team recovery.
	 */
	ETraceTeam VerifyFromTeam = ETraceTeam::None;
	bool bVerifyTookGrace = false;
	FString VerifyTakerName;
	int32 VerifyPassCount = 0;
	int32 VerifyFailCount = 0;
	int32 VerifyGoalsAtStart = 0;

	/**
	 * What the LAST GrantTo actually decided about the turnover grace, recorded by GrantTo itself.
	 *
	 * Read by the verification scenario. Deliberately the rule's own output rather than anything
	 * observed afterwards: the alternative was to infer the grace from the trail's point count a
	 * fraction of a second later, which measures whether the receiver happened to be moving.
	 */
	bool bLastGrantTeamChanged = false;
	float LastGrantGraceSeconds = 0.f;

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

	/** Mode B: bLoose replicates independently of Carrier, so it joins the same reconciliation. */
	bool bAppliedLoose = false;

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
