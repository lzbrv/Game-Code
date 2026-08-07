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

	// --- SPEC v10 §10: the teleport channel. ------------------------------------------------------
	//
	// THE PROBLEM THIS SOLVES, and it is NOT the one the spec guessed at.
	//
	// The spec's lead was "the client interpolates replicated movement, so it slides the Core across
	// the map". That cannot be what happens here: the constructor calls SetReplicateMovement(false),
	// so ReplicatedMovement is never sent for this actor and there is no interpolator to blame. The
	// measured defect is the opposite one — the client never learns about the reset AT ALL.
	//
	// Every server reset (KickoffTo, the park-at-home in Tick, a throw's launch point) moved the actor
	// with SetActorLocation and replicated NOTHING about it, because the only transform channel this
	// actor has is its ATTACHMENT. On a goal the client's Core therefore detaches with
	// KeepWorldTransform and is STRANDED in the endzone for the whole kickoff delay, then snaps the
	// full length of the field when the next holder finally attaches it. That cross-map jump, off a
	// bright always-relevant beacon, is "the core randomly flies across a players screen ... after a
	// goal".
	//
	// So the fix is a channel, not a smoothing flag. TeleportLocation carries WHERE and TeleportSerial
	// carries THAT IT HAPPENED; the serial is what makes a second teleport to the same place still
	// arrive, and what makes the event legible independently of Carrier / bLoose / AttachmentReplication,
	// none of whose OnRep order relative to this one is guaranteed. Both are written by exactly one
	// function, ServerTeleport(), so "cover every path" is a grep with one answer.
	//
	// NOT used for the per-frame integration of a loose Core: that is continuous motion, already
	// covered by LooseLocation + the client's dead reckoning, and bumping a serial 60 times a second
	// would put 16 bytes on the wire per frame for a discontinuity that is not happening.

	/** Where the last SERVER-SIDE DISCONTINUITY put the Core. Written only by ServerTeleport(). */
	UPROPERTY(Replicated)
	FVector_NetQuantize TeleportLocation = FVector::ZeroVector;

	/**
	 * Bumped by every server-side teleport. Wraps at 256, which is fine: clients react to the CHANGE,
	 * and 256 resets between two net updates to one client is not a reachable state.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_TeleportSerial)
	uint8 TeleportSerial = 0;

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

	/**
	 * Server, diagnostics only (Trace.Core.GoalRepro). Arms the spec v10 §10 reproduction: teleport
	 * the current holder deep into the attacking end, then — after a staging delay long enough for a
	 * 40 ms client to have seen them there — fire the SAME KickoffTo() a scored goal fires.
	 */
	bool RequestGoalRepro();

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
	 * THIS IS THE HOOK FOR THE REAL GOAL ACTOR. While nothing is registered, the Core reads the live
	 * ATraceEndzone goal volumes instead (RefreshGoalVolumes step 2) — the same boxes the arena
	 * builder sized and drew, so mode B is playable and testable before any external goal actor
	 * lands, and it stops reading them the moment one real volume registers.
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
	 * How a goal was put in.
	 *
	 * Counted apart because "bots never carry it in" and "bots never throw it in" are DIFFERENT
	 * failures with different fixes, and both were live at once last pass — a single goal total
	 * showed neither, and every goal that did land came from the scripted verifier or a deflection.
	 */
	enum class EGoalMethod : uint8
	{
		Thrown = 0,
		Carried = 1,
		/** Possession changed hands while the new holder was already standing inside the mouth. */
		Granted = 2,
		Count = 3
	};

	/** Running tally per EGoalMethod. Printed by the [ModeB] tally line and by Trace.ModeB.Tally. */
	static int32 GoalsByMethod[static_cast<int32>(EGoalMethod::Count)];

	/**
	 * SPEC v7 §4 counters. Verification only — never a game rule.
	 *
	 * The rule does NOT distinguish a floor from the roof of a crate (that is the whole point of it),
	 * so the only way to show that the generalised path is actually being exercised, rather than the
	 * old floor-only one, is to count the two outcomes separately here and report both. Printed by
	 * Trace.ModeB.SurfaceStats and by the Trace.ModeB.VerifySurfaces scenario.
	 */
	struct FSurfaceRuleStats
	{
		/** Turnovers where the supporting surface was at (or near) the arena floor. */
		int32 GroundTurnovers = 0;

		/** Turnovers where it was well above the floor: the top of a structure. */
		int32 TopTurnovers = 0;

		/** Blocking contacts whose normal said WALL, and which therefore bounced instead. */
		int32 WallBounces = 0;

		/** Wall bounces too slow to matter that were nonetheless refused a resting place on the wall. */
		int32 WallRestRefusals = 0;

		/** Landings the at-rest probe found that the contact test alone would have missed. */
		int32 RestProbeRescues = 0;
	};

	static FSurfaceRuleStats SurfaceStats;

	/**
	 * The centre of the goal @p AttackingTeam scores in, for bot steering and HUD markers.
	 *
	 * In mode B this is the narrow goal mouth; in mode A there are no goals and this returns false,
	 * which is the caller's cue to keep using the endzone. Answered from the same boxes that score,
	 * so a bot cannot run at a goal the rule does not recognise.
	 */
	static bool GetAttackGoalCentre(const UWorld* World, ETraceTeam AttackingTeam, FVector& OutCentre);

	/**
	 * The whole goal box @p AttackingTeam scores in, not just its centre.
	 *
	 * Exists because the mouth is now NARROW (spec v5 §4) and a caller that only knows the centre
	 * cannot tell whether it is lined up with it. ATraceBotController needs exactly that: its attack
	 * lane spreads bots across ±BotAttackLaneFieldFraction of the field HALF-WIDTH, which is ±1440 uu
	 * on this pitch — wider than the 2000 uu goal — so a carrier steering at "the goal, on my lane"
	 * was steering at a point beside the mouth. Same boxes CheckGoalScore awards points off.
	 */
	static bool GetAttackGoalBox(const UWorld* World, ETraceTeam AttackingTeam, FBox& OutBox);

	/**
	 * SPEC v6 §4.3. The CIRCLE @p AttackingTeam scores in: centre, radius and plane normal.
	 *
	 * The goal is now a hoop set into a back wall, and the box a bot used to aim at is only its
	 * bounding slab - so aiming at the box centre is aiming at the middle of a square whose corners
	 * are wall. This is the shape that actually scores, published for the same reason the flight
	 * model is: ATraceBotController would otherwise have to rebuild it from the arena's constants and
	 * would silently disagree with the rule the moment either changed.
	 *
	 * @return false in mode A, and false in mode B if the armed goal is not a ring (an externally
	 *         registered box goal, for instance) - in which case the caller should fall back to
	 *         GetAttackGoalBox.
	 */
	static bool GetAttackGoalRing(const UWorld* World, ETraceTeam AttackingTeam, FVector& OutCentre,
		float& OutRadius, FVector& OutAxis);

	// --- MODE B: the Core's own flight model, published --------------------------------------------
	//
	// PUBLISHED, rather than left for each caller to reconstruct, because it already went wrong once:
	// ATraceBotController::ComputeThrowAimPoint read the Trace.ModeB.* CONSOLE VARIABLES directly and
	// so silently ignored the UTraceSettings properties that override them, and would have ignored
	// the v5 weight model entirely — every bot aiming as if the Core were still the old light one,
	// i.e. throwing long over a goal it is trying to hit. One model, one owner, asked at the point of
	// use so a live retune reaches the bots on the same frame it reaches the Core.

	/** MODE B. Launch speed of a thrown Core, uu/s, after the weight model. */
	static float GetThrowSpeed();

	/** MODE B. Upward component added to a throw, as a fraction of the launch speed, after weight. */
	static float GetThrowUpBias();

	/** MODE B. SIGNED gravity acting on a loose Core in @p World, uu/s^2 (negative), after weight. */
	static float GetThrowGravityZ(const UWorld* World);

	/** MODE B. Forward offset of the launch point from the thrower's eye. */
	static float GetThrowMuzzleForward();

	// --- SPEC v8 §4: THE THROW CARRIES THE THROWER'S MOMENTUM --------------------------------------
	//
	// Verbatim: "When jumping and throwing the core, the core doesn't seem to keep momentum. Make sure
	// that the core has the momentum from the throw and also carries momentum from the player."
	//
	// The throw used to be a pure impulse in the aim direction: a Core thrown by a player sprinting at
	// 900 uu/s left at exactly the same velocity as one thrown standing still, and a Core thrown at the
	// top of a jump left with the jump's +Z thrown away. THAT SECOND CASE IS THE ONE THE USER NOTICED,
	// and it is the more visible of the two because a jump's vertical velocity is the one component a
	// player is watching at the moment they release.
	//
	// The launch is now impulse + up bias + THROWER VELOCITY * inheritance, vertical included. The
	// three accessors below are published for the same reason the flight model above them is: the bot
	// throw solver reconstructs the launch to aim, and a solver that does not know about the inherited
	// term aims at where the Core would have gone before this change.

	/**
	 * MODE B. Fraction of the thrower's own velocity added to the launch. 1.0 = full inheritance.
	 *
	 * Spec v8 §4's [ASSUMPTION] is full inheritance, and this exists so that "too strong" is a number
	 * a designer can change rather than a fudge factor compiled into the launch. Bound by name to
	 * UTraceSettings::CoreThrowVelocityInheritance, with Trace.ModeB.ThrowVelocityInheritance behind it.
	 */
	static float GetThrowVelocityInheritance();

	/**
	 * MODE B. The inherited term on its own: @p Thrower's current velocity times the fraction above.
	 *
	 * Zero for a null or non-pawn actor, and zero in mode A, so it is safe to add unconditionally.
	 */
	static FVector GetInheritedThrowVelocity(const AActor* Thrower);

	/**
	 * MODE B. THE COMPLETE LAUNCH VELOCITY a throw by @p Thrower along @p AimDirection leaves with.
	 *
	 * ThrowFromHolder() calls exactly this, so anything that needs to predict the flight — the bot
	 * throw solver, the arc lane sweep, a HUD trajectory — gets the same answer the Core will actually
	 * use, including the inherited term. Reconstructing it from GetThrowSpeed()/GetThrowUpBias() alone
	 * is now WRONG by up to the thrower's full speed, which on a sprinting carrier is ~40% of the
	 * launch speed.
	 */
	static FVector ComputeThrowLaunchVelocity(const AActor* Thrower, const FVector& AimDirection);

	/**
	 * The last throw, broken into its parts. Diagnostics only (Trace.ModeB.ThrowMomentum).
	 *
	 * Recorded because "the core doesn't seem to keep momentum" is a claim about a launch velocity, and
	 * the only honest answer to it is the three numbers that make one up. A single "launch speed" in
	 * the log cannot distinguish a throw that inherited nothing from one that inherited a velocity
	 * pointing backwards.
	 */
	struct FThrowMomentumSample
	{
		bool bValid = false;
		/** Speed of the impulse alone: throw speed plus the up bias, before inheritance. */
		float ImpulseSpeed = 0.f;
		/** Magnitude of the inherited term. */
		float InheritedSpeed = 0.f;
		/** Magnitude of the sum, i.e. what actually left the hand. */
		float LaunchSpeed = 0.f;
		/** The thrower's velocity at the instant of release, split so a jump is visible as a jump. */
		float ThrowerSpeed2D = 0.f;
		float ThrowerVelocityZ = 0.f;
		bool bThrowerFalling = false;
		/** Z of the launch velocity, which is the number the user is watching on a jumping throw. */
		float LaunchVelocityZ = 0.f;
		float Inheritance = 0.f;
		FString ThrowerName;
	};

	static FThrowMomentumSample LastThrow;

	/**
	 * Server, diagnostics only (Trace.ModeB.MomentumTest). Three throws by the current holder — at a
	 * dead stop, at a full run, and airborne with a jump's vertical velocity — printed with their
	 * pre-v8 baselines beside them.
	 *
	 * Exists because the three numbers the note is about cannot be arranged on cue in a live match:
	 * a bot happens to throw while running, essentially never throws at the top of a jump, and never
	 * throws standing still at all. It WRITES THE THROWER'S VELOCITY and then calls the shipping
	 * ThrowFromHolder, so what is measured is the real launch and not a re-derivation of it.
	 *
	 * It mutates a pawn's velocity, so it must not be run alongside a harness that is also driving
	 * pawns (-TraceTripTest).
	 */
	void RunThrowMomentumTest();

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

#if !UE_BUILD_SHIPPING
	/**
	 * DEV ONLY. Force the pass window open on the current holder, without hovering a teammate.
	 *
	 * Exists for ONE caller: Trace.Knife.CarrierImmunityTest. The knife's carrier immunity has two
	 * independent guards and only one of them is interesting. A carrier whose shield is UP is
	 * already skipped by UTraceLagCompensationComponent::ResolveHitscan, which every knife sample
	 * goes through — so a back-stab against an ordinary carrier proves nothing about
	 * TraceMelee::ResolveSwing's own rule, and would pass on a build where that rule was deleted.
	 *
	 * The rule only does work during a PASS, when ATraceCore::IsShieldSuppressedFor drops the shield
	 * so bullets can land. That half-second is the entire risk: without the melee-specific rule the
	 * knife would be immune-by-accident for the rest of the match and lethal for exactly the window
	 * that decides games. Staging that window is what makes the test able to fail.
	 *
	 * Returns the teammate the pass was aimed at, or null if there was no holder or no teammate.
	 */
	ATraceCharacter* DebugForcePassWindow();
#endif

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

	/**
	 * SPEC v10 §10. The server teleported the Core; put it there NOW, this frame, with no
	 * dead reckoning and no waiting for the next possession event to place it.
	 */
	UFUNCTION()
	void OnRep_TeleportSerial();

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

	/**
	 * Server, mode B, SPEC v6 §4.1 — THE CATCH ZONE.
	 *
	 * Verbatim: "create a small invisible radius around players that acts as a 'catch zone,' so that
	 * when the core enters that area, it curves towards the player like a magnet."
	 *
	 * Bends LooseVelocity toward the nearest eligible catcher inside CatchRadius. SPEED IS PRESERVED
	 * and only the DIRECTION is steered, which is what makes it read as a catch rather than as a
	 * tractor beam: a Core that arrives faster still arrives faster, it just arrives at the player.
	 * The pull falls off with distance so the edge of the zone is a nudge and the middle of it is a
	 * catch, and it is scaled by DeltaSeconds so the curve is frame-rate independent.
	 *
	 * EVERY player, friend or enemy (spec: interception is a feature), except the thrower for
	 * CatchThrowerLockout seconds — without that a throw would curve straight back into the hands it
	 * left, since it leaves from inside its own thrower's catch zone.
	 */
	void ServerApplyCatchZone(float DeltaSeconds);

	/**
	 * Server, mode B, SPEC v6 §4.2 AS GENERALISED BY SPEC v7 §4 — SURFACE CONTACT IS A TURNOVER.
	 *
	 * v6, verbatim: "When a team has possession of the core, throws it, and it hits the ground, it
	 * should automatically turnover to the closest player on the enemy team."
	 * v7, verbatim: "Sometimes the core gets stuck up top of an object in gamemode b. This should also
	 * count as a turnover. Walls should not, the core should bounce off those."
	 *
	 * ONE function for both, deliberately. "The top of a crate" and "the floor" are the same event to
	 * this rule and the caller has already decided that by asking @p SurfaceNormal — a second entry
	 * point would be a second copy of a rule the spec states once.
	 *
	 * Grants the Core to the nearest living member of the team OPPOSING @p LooseFromTeam. That is a
	 * cross-team change of possession, so it goes through TakeLooseCore like an interception and
	 * picks up the 0.5 s trace grace by the same AreAllies() line — spec v5 §0's rule, unmodified.
	 *
	 * Falls back to ResetLooseCore when that team has nobody alive, because the Core may never be
	 * lost and a kickoff to the same side is the closest thing to the intended outcome.
	 *
	 * @param SurfacePoint  where the Core is being held up, for the log and the floor/top tally.
	 * @param SurfaceNormal the normal that decided this was a landing. Reported, never re-tested here.
	 * @return true when the Core left the loose state (granted or reset). The caller must touch no
	 *         member state afterwards: a grant can score, which resets the field.
	 */
	bool ServerSurfaceTurnover(const FVector& SurfacePoint, const FVector& SurfaceNormal);

	/**
	 * Server, mode B, SPEC v7 §4. What is holding a Core at rest up, if anything?
	 *
	 * THE FIX FOR "the core gets stuck up top of an object". The contact test in the flight
	 * integration only runs on a frame whose sweep was blocked, and a Core that reaches rest on an
	 * edge hit (normal nowhere near vertical) switches that integration off for good — after which
	 * nothing ever asks again. This asks, with the same sphere and the same channel the flight uses,
	 * so the two cannot disagree about what the Core is touching.
	 *
	 * @return false when there is nothing underneath at all, which is a Core "at rest" in mid-air:
	 *         the caller puts it back in flight rather than leaving it hanging.
	 */
	bool ServerProbeRestingSurface(FVector& OutPoint, FVector& OutNormal) const;

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
	bool CheckGoalScore(const FVector& From, const FVector& To, ETraceTeam ScoringTeam, EGoalMethod Method,
		const TCHAR* How);

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
	bool DebugLaunchLoose(const FVector& From, const FVector& LaunchVelocity, ETraceTeam FromTeam,
		bool bAsThrow = true);

	/** Server. Runs the Trace.ModeB.Verify scenario, one step at a time. See the .cpp. */
	void TickModeBVerification();

	/**
	 * EVERY MACHINE, diagnostics only (Trace.ModeB.FlightLog). Logs this machine's own view of the
	 * loose Core at 10 Hz. Deliberately not authority-gated — see spec v8 §0.
	 */
	void TickFlightLog();

	/**
	 * Server. Is a JOINING CLIENT's pawn live in this match right now?
	 *
	 * The §0 gate for the momentum measurement. Asks for a possessed pawn rather than merely a
	 * connection, because a connection exists well before the client has a character to throw with —
	 * and a measurement taken in that window is a host-side measurement wearing a client's name.
	 */
	bool HasRemoteClientPawn() const;

	/** Throttle for TickFlightLog. Local to each machine; nothing about it is replicated. */
	float NextFlightLogTime = 0.f;
	bool bFlightLogWasLoose = false;

	// =============================================================================================
	// SPEC v10 §10 — the teleport funnel, and the audit that proved what was actually wrong
	// =============================================================================================

	/**
	 * SERVER. The ONE way this actor is moved discontinuously. Places it, records the destination,
	 * bumps the serial and forces the update, so every client snaps to the same point on the same
	 * event instead of inferring the reset from a possession change that may not come for a second.
	 *
	 * Continuous motion does NOT come through here: an attached Core rides its holder, and a loose
	 * Core is integrated frame by frame into LooseLocation. This is for discontinuities only —
	 * kickoff, score, half time, match start, the park-at-home and a throw's launch point.
	 */
	void ServerTeleport(const FVector& Where, const TCHAR* Why);

	/**
	 * EVERY MACHINE. Puts a HOLDERLESS Core where that machine's own rules say it belongs: the loose
	 * position in mode B, the arena centre otherwise.
	 *
	 * This is the second half of the contract the constructor states ("holderless => every machine
	 * computes the home location identically") and it was previously implemented on the server only —
	 * the park-at-home in Tick sits after the `!HasAuthority()` early return. A client therefore never
	 * parked anything, which is the stranding half of the §10 bug. A no-op when somebody is holding
	 * it: the attachment owns the transform then.
	 */
	void PlaceHolderlessCore();

	/**
	 * EVERY MACHINE, diagnostics only (Trace.Core.TeleportAudit).
	 *
	 * Watches this machine's OWN copy of the Core across a possession reset and reports one line per
	 * reset: how far it travelled, in how many frames, and how far from home it sat while it did.
	 * That is what separates the three candidate explanations from each other — a multi-frame SLIDE
	 * (interpolation, the spec's lead), a one-frame JUMP from the wrong place (stranding, what the
	 * measurement actually found) and a correct SNAP.
	 */
	void TickTeleportAudit();

	/** Server, diagnostics only (Trace.Core.GoalRepro). Stages and then fires the post-goal reset. */
	void TickGoalRepro();

	// --- Audit state. Per machine, never replicated. -----------------------------------------------
	FVector AuditLastLocation = FVector::ZeroVector;
	bool bAuditHasLast = false;
	bool bAuditWasHeld = false;
	bool bAuditWindowOpen = false;
	float AuditWindowEndTime = 0.f;
	double AuditPathLength = 0.0;
	double AuditMaxStep = 0.0;
	double AuditWorstHomeError = 0.0;
	float AuditAwayFromHomeSeconds = 0.f;
	int32 AuditMovingFrames = 0;
	int32 AuditFrames = 0;

	// --- Goal-repro state. Server only, never replicated. ------------------------------------------
	bool bGoalReproArmed = false;
	float GoalReproFireTime = 0.f;
	ETraceTeam GoalReproTeam = ETraceTeam::None;
	int32 GoalReproRunsDone = 0;
	float GoalReproNextAutoTime = 0.f;

	/**
	 * The momentum measurement has already run once.
	 *
	 * A LATCH, not a re-read of the CVar, and the difference was a measured bug: the arming CVar is
	 * set from -ExecCmds, i.e. at ECVF_SetByConsole, and a later Set(0, ECVF_SetByCode) from inside
	 * the game is SILENTLY IGNORED because console-priority beats code-priority. The disarm never
	 * took, and the test re-fired every tick for the rest of the match — 48 runs instead of 1, with
	 * the Core being thrown out of every possession. Anything that disarms itself has to do it with
	 * state it owns.
	 */
	bool bMomentumTestRun = false;

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

	/**
	 * SPEC v6 §4.2. True while the loose Core is one a team actually THREW.
	 *
	 * The turnover rule is a statement about a throw — "when a team has possession of the core,
	 * throws it, and it hits the ground" — and not about every way a Core can end up in the air. The
	 * only other way it can is Trace.ModeB.Verify's DebugLaunchLoose, whose reset-timer step parks a
	 * Core on the floor on purpose; without this flag that step would hand the Core to the nearest
	 * enemy on its first frame and would be testing the new rule instead of the timer.
	 */
	bool bLooseFromThrow = false;

	/**
	 * SPEC v6 §4.1. The catcher the magnet is currently pulling toward, for the log line only.
	 *
	 * Held so that "the Core curved toward X" is announced ONCE per catch rather than every frame of
	 * the curve. An invisible mechanic with no log line is one nobody can tell is broken, and this
	 * project has twice declared a working mechanic dead for exactly that reason.
	 */
	TWeakObjectPtr<ATraceCharacter> CatchZoneTarget;

	/** Previous frame's holder position, so the mode-B carry-in test is swept rather than sampled. */
	FVector LastCarrierGoalTestLocation = FVector::ZeroVector;

	/**
	 * One goal mouth, in world space, with the team that DEFENDS it.
	 *
	 * SPEC v6 §4.3 added the ring. The BOX is still here and is still the broad phase — it is the
	 * ring's bounding slab, it is what a swept segment is cheaply rejected against, and it is what
	 * GetAttackGoalBox publishes — but when bRing is set, a segment that crosses the box only scores
	 * if it crosses the DISC. Keeping both means the cheap test stays cheap and the corners of the
	 * slab, which are wall, stop awarding points.
	 */
	struct FTraceGoalBox
	{
		FBox Box = FBox(ForceInit);
		ETraceTeam DefendingTeam = ETraceTeam::None;

		/** Spec v6: this goal is a circle in a wall, not a box on the floor. */
		bool bRing = false;

		/** Centre of the ring mouth. Meaningless unless bRing. */
		FVector RingCentre = FVector::ZeroVector;

		/** Radius of the ring mouth, uu. */
		float RingRadius = 0.f;

		/** Unit normal of the ring plane, i.e. the direction a scoring throw travels through it. */
		FVector RingAxis = FVector::ForwardVector;
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

	/**
	 * GoalsByMethod, sampled when a bot-behaviour step starts (steps 4 and 5).
	 *
	 * Those two steps do not test the Core at all - they test that ATraceBotController actually
	 * REACHES the two scoring paths, which is the gap this pass was asked to close ("bots NEVER throw
	 * at the goal"; "scoring by CARRYING into a goal never fired"). Both are unreachable in a live
	 * match on a 33600 uu pitch inside any window a test run gets, because a carrier is measured
	 *16000-27000 uu from the mouth for the whole of it. Putting a carrier where the decision is
	 * actually taken is the only way to prove the branch fires rather than merely that it compiles.
	 */
	int32 VerifyGoalTallyAtStart = 0;

	/**
	 * SPEC v7 §4 scenario state (steps 7 and 8).
	 *
	 * The scenario can be armed two ways: Trace.ModeB.Verify runs the whole thing from step 0, and
	 * Trace.ModeB.VerifySurfaces starts it at step 7, because steps 4 and 5 wait on bots to score and
	 * a surface-rule run should not have to sit through that.
	 */
	bool bVerifySurfacesOnly = false;
	int32 VerifyWallBouncesAtStart = 0;
	int32 VerifyTurnoversAtStart = 0;
	int32 VerifyRescuesAtStart = 0;

	/**
	 * Step 8 only: the parked Core has settled and has been declared thrown.
	 *
	 * The step cannot arm on the frame it launches — a Core still in the air would be caught by the
	 * ordinary contact test and the step would silently become a second copy of step 7.
	 */
	bool bVerifyRestArmed = false;

	/**
	 * Server, diagnostics. Finds a raised, horizontal-ish surface in the arena — the TOP of a piece of
	 * cover — and, on its side, a vertical face to bounce off.
	 *
	 * Sampled from the world with the same sphere the Core flies with, rather than read off
	 * ATraceArenaBuilder's construction constants: the rule under test is "what does the geometry
	 * say", so a test that asked the builder where it MEANT to put a crate could pass against an arena
	 * whose crates were somewhere else.
	 *
	 * @return false when the arena has no raised cover to land on, which is a SKIP and not a failure.
	 */
	bool FindVerificationSurfaces(FVector& OutTopPoint, FVector& OutWallPoint, FVector& OutWallNormal) const;

	/** Set on step 5 when a throw is seen to leave, so a shot that misses is still distinguishable. */
	bool bVerifyThrowSeen = false;
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
