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
//     CHARGES A THROW in mode B (spec v13 §6 — the press winds the throw up, the RELEASE launches
//     it instantly), so every existing caller — a human's mouse, DoPassPressed, the bots'
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

// =================================================================================================
// SPEC v25 §2 — THE TURNOVER IS A PLACE THE CORE CAN BE, NOT A TRANSFER
//
// Verbatim: "Instead of automatically going to the other team, a turnover is registered and the core
// stays on the ground where it landed." Everything mode B already had about a turnover — WHEN one
// fires, which surfaces count, the settle — is untouched (spec v25: "Turnover criteria remain the
// same"). What changed is only what happens next, and it is a three-row state table:
//
//   Loose, no turnover  | nobody pulls | anybody picks up            | normal beam
//   Turnover, 0-5 s     | OPPOSING team only, RMB + hover + line of  | opposing team's colour,
//                       | sight, 0.3 s continuous                    | LARGER
//                       | opposing team only picks up                |
//   After 5 s           | nobody pulls | either team, by touch       | normal beam
//
// THE THIRD ROW IS THE FIRST ROW. Once the lockout expires nothing distinguishes the two — nobody
// pulls, anybody may take it, the beam is normal — so the turnover is simply CLEARED at 5 s rather
// than kept alive as a third state that would have to be shown to behave identically. One latch,
// bTurnoverRegisteredThisFlight, stops the landing rule from noticing the still-resting Core a frame
// later and registering a second turnover, which would restart the clock forever.
//
// SERVER-AUTHORITATIVE, and the race is the reason. Two opponents can be filling at once and "first
// to complete wins"; a client that ran its own 0.3 s clock would sometimes disagree with the server
// about who that was, and the losing client would have watched a full ring hand the Core to somebody
// else. So the hold is measured ONLY on the server (FTraceCorePullHold::StartServerTime is written by
// the server and replicated down) and the ring the HUD draws is that number — see GetPullProgressFor.
// =================================================================================================

/**
 * SPEC v25 §2. One player's in-progress pull, as the SERVER sees it.
 *
 * Replicated as an array rather than as a single "who is pulling" pointer because the spec makes the
 * contested case explicit ("Two opponents pulling at once"), and a single pointer cannot show two
 * players their own ring. Five entries is the practical ceiling (one team), 8 bytes each.
 */
USTRUCT()
struct FTraceCorePullHold
{
	GENERATED_BODY()

	/** The player holding the button. Never null in a replicated entry. */
	UPROPERTY()
	TObjectPtr<ATraceCharacter> Puller = nullptr;

	/**
	 * Shared-clock time the CURRENT continuous hold began.
	 *
	 * Re-stamped on every fresh hold, because the fill CANCELS rather than pauses: losing hover,
	 * losing line of sight or releasing the button removes the entry outright, and re-satisfying the
	 * conditions starts a new one from zero.
	 */
	UPROPERTY()
	float StartServerTime = 0.f;
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

	// --- SPEC v25 §2. The turnover window. Inert (None / empty) outside it and in mode A. ----------

	/**
	 * THE TEAM THAT DROPPED IT, i.e. the team that is LOCKED OUT. None = no turnover is running.
	 *
	 * Stored as the losing team rather than as the winning one because that is what the sentence
	 * says ("Players from the team who dropped the core are locked out") and because it is the fact
	 * every rule here is phrased against: who may not pick up, and — by TraceOpposingTeam — who may
	 * pull and what colour the beam becomes. Storing the opponent instead would make the lockout a
	 * derived fact and the pull the stored one, which is the wrong way round for reading the code
	 * against the note.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Turnover)
	ETraceTeam TurnoverLockoutTeam = ETraceTeam::None;

	/** Shared-clock time the turnover was registered. Only meaningful while TurnoverLockoutTeam != None. */
	UPROPERTY(Replicated)
	float TurnoverStartServerTime = 0.f;

	/**
	 * Every pull currently being held, as the SERVER measures it. The HUD's ring reads this.
	 *
	 * Replicated to everybody rather than to the puller alone: this actor is bAlwaysRelevant and has
	 * no COND_OwnerOnly anywhere (see the file header), the payload is a handful of bytes, and a
	 * spectator or a teammate being able to see that somebody is pulling is information the note's
	 * own "small circle around the core" is already showing on screen.
	 */
	UPROPERTY(Replicated)
	TArray<FTraceCorePullHold> PullHolds;

	/**
	 * The player a COMPLETED pull is delivering the Core to, or null.
	 *
	 * Replicated because it is the difference between a Core falling and a Core being pulled, and a
	 * client that could not tell them apart would draw the wrong thing at the one moment the mechanic
	 * is happening. Cleared the instant they take it.
	 */
	UPROPERTY(Replicated)
	TObjectPtr<ATraceCharacter> PullWinner = nullptr;

	// --- SPEC v28 §7. The full-charge deadline. Inert (-1) in mode A and whenever nobody is charging.
	/**
	 * SERVER TRUTH. Shared-clock instant this holder's charge reached FULL, or < 0 when it has not.
	 *
	 * The ONE writer is ServerTickThrowAutoRelease() (plus ClearThrowCharge, which zeroes it with the
	 * rest of the charge). It is set to `press time + CoreThrowChargeSeconds`, i.e. the exact moment
	 * the rule fired, NOT the time of the tick that noticed — a tick-quantised start would make the
	 * red ring's 0.6 s a 0.6 s plus up to a frame, and the deadline and the drawing would then be
	 * measuring two different windows.
	 *
	 * Replicated to everybody rather than COND_OwnerOnly, which is this actor's standing policy (see
	 * PullHolds): it is bAlwaysRelevant, this is four bytes that change at most twice per throw, and
	 * an owner-only condition on an actor whose owner changes with possession is a way to lose the
	 * value on exactly the frame a possession change races a charge.
	 */
	UPROPERTY(Replicated)
	float ThrowFullChargeServerTime = -1.f;

	// --- SPEC v28 §7's CLIENT-SIDE EVIDENCE. Never replicated; each machine's own record. ----------
	//
	// "The ring shows the SERVER'S OWN progress" is a claim about the wire, and it cannot be checked
	// on the server: the host writes the stamp and reads it back out of its own memory, so a listen
	// host would report a green ring for a property that was never registered for replication at all.
	// These two are what a SECOND PROCESS reads back through Trace.ModeB.ThrowAutoRelease.Report, and
	// on a client a non-zero count is the wire itself answering.
	//
	// Updated in Tick on EVERY machine, ahead of the authority split, for the same reason TickFlightLog
	// is: the machine the question is about is the one that is not the server.

	/** The last full-charge instant this machine has SEEN, however it arrived. < 0 = never. */
	float LastSeenFullChargeStamp = -1.f;

	/** How many DISTINCT full-charge instants this machine has seen. On a client, packets received. */
	int32 FullChargeStampsSeen = 0;

	/** The largest auto-release alpha this machine has ever computed from those stamps. */
	float PeakSeenAutoReleaseAlpha = -1.f;

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

	/** MODE B. Authoritative position of the loose Core. Only meaningful while IsLoose(). */
	FVector GetLooseLocation() const { return LooseLocation; }

	// =============================================================================================
	// SPEC v25 §2 — THE TURNOVER STATE MACHINE. Everything below is GOALS MODE ONLY.
	//
	// Every query here answers correctly on every machine: the four facts it is built from
	// (TurnoverLockoutTeam, TurnoverStartServerTime, PullHolds, PullWinner) are all replicated, and
	// the clock is the shared one (AGameStateBase::GetServerWorldTimeSeconds), so a client reading
	// these is reading the server's answer late rather than a guess of its own.
	// =============================================================================================

	/** True while a registered turnover's lockout window is still running. False in mode A. */
	bool IsTurnoverActive() const;

	/** The team that DROPPED the Core and is locked out of it. None when no turnover is running. */
	ETraceTeam GetTurnoverLockoutTeam() const { return TurnoverLockoutTeam; }

	/**
	 * The team the turnover belongs TO: the only side that may pull or pick up during the window, and
	 * the colour the beam becomes. None when no turnover is running.
	 *
	 * DERIVED, never stored, so it cannot drift from the lockout it is the opposite of.
	 */
	ETraceTeam GetTurnoverPullingTeam() const { return TraceOpposingTeam(TurnoverLockoutTeam); }

	/** Seconds left on the lockout window. 0 when no turnover is running. */
	float GetTurnoverSecondsRemaining() const;

	/** 0..1 of the way through the lockout window. -1 when no turnover is running. */
	float GetTurnoverAlpha() const;

	/**
	 * *** THE HUD'S RING. SPEC v25 §3 asks for "real server-side progress, not a local guess". ***
	 *
	 * 0..1 of @p Player's current pull hold, or -1 when they are not pulling. Read straight off the
	 * replicated PullHolds entry the SERVER stamped, so the ring a player watches fill is the same
	 * number that decides the race — there is deliberately no local prediction to disagree with it.
	 *
	 * Safe on any actor, on any machine, including null and a non-character.
	 */
	float GetPullProgressFor(const AActor* Player) const;

	/** Convenience for the HUD: GetPullProgressFor() of this machine's own locally-controlled pawn. */
	float GetLocalPullProgress() const;

	/** The player a completed pull is delivering the Core to, or null. */
	ATraceCharacter* GetPullWinner() const { return PullWinner; }

	/** True while the Core is flying to the winner of a pull. */
	bool IsPullTravelling() const { return PullWinner != nullptr; }

	/**
	 * Would @p Puller be allowed to start (or continue) a pull on this frame?
	 *
	 * The WHOLE rule, in one place, asked by the server's state machine, by the HUD's "should I draw
	 * a ring" test and by the red-arm harness — so a ring can never appear for a pull the server
	 * would refuse. @p OutReason names the test that failed, for the log and the harness.
	 */
	bool CanPullNow(const ATraceCharacter* Puller, const TCHAR** OutReason = nullptr) const;

	/**
	 * THE PULL BUTTON. Call on the machine that owns the input; safe from any machine.
	 *
	 * *** §7's right mouse routes here. *** Precedence is the caller's to apply and this function
	 * does not assume it: it refuses anybody who is carrying the Core, so a carrier's right mouse is
	 * free to mean parry, and it refuses anybody CanPullNow() refuses, so a press that is not over a
	 * turned-over Core costs one refused latch and nothing else.
	 *
	 * On the server the latch is applied directly. On a client it is forwarded through that client's
	 * own ATraceCorePullRelay — this actor is owned by its HOLDER, so a non-holding client cannot RPC
	 * it at all, which is the whole reason the relay exists.
	 */
	void RequestPullInput(bool bPressed, ATraceCharacter* Requester);

	/** SPEC v25 §2. Seconds of continuous hold that complete a pull. UTraceSettings::CorePullHoldSeconds. */
	static float GetPullHoldSeconds();

	/** SPEC v25 §2. Length of the lockout window. UTraceSettings::CoreTurnoverLockoutSeconds. */
	static float GetTurnoverLockoutSeconds();

	/**
	 * SPEC v25 §2/§3. How much LARGER the beam is during the turnover window, as a MULTIPLIER of the
	 * normal beam width.
	 *
	 * RELATIVE, per Demo 21's standing rule: the note asks for a beam that is "larger", which is a
	 * statement about the normal beam, so a retune of the normal beam has to move this with it. An
	 * absolute width stored here would silently stop being "larger" the day the base changed.
	 */
	static float GetTurnoverBeamScale();

#if !UE_BUILD_SHIPPING
	/**
	 * DEV ONLY — SPEC v25 INTEGRATION SCAFFOLDING. Stage a turnover in front of the LOCAL PLAYER.
	 *
	 * WHY THIS EXISTS. Every §2 harness in this file drives BOTS, deliberately: the hover rule under
	 * test is "where is this player looking", and moving a human's control rotation from a test would
	 * be faking the very input being measured. That is right for the red arm and useless for the
	 * integrator, whose job is to PHOTOGRAPH the window — the beam in the other team's colour, the
	 * ring filling, the locked-out team walking over the Core and getting nothing. Over five runs the
	 * §3 agent's local pawn satisfied CanPullNow on 6 frames out of ~1600 for exactly this reason.
	 *
	 * So this moves the CORE ONLY, and never the aim: it puts the Core on the FLOOR @p DistanceUU in
	 * front of the player, resting, where a thrown Core would have come to a stop. Whether the player
	 * is then hovering it is left entirely to them and to the shipping CanPullNow — which is why the
	 * demo below gets its own red arm for free, and has repeatedly recorded "not hovering the Core"
	 * from this exact staging. Everything after that is the shipping path: DebugLaunchLoose + the
	 * shipping RegisterTurnover(), then the real ServerTickTurnover, the real CanPullNow, the real
	 * 0.3 s clock.
	 *
	 * RESTING, NOT HANGING, and that is load-bearing rather than cosmetic: a Core staged in mid-air
	 * "resumes flight" (measured: "nothing at all under it (orb 134 uu clear) - resuming flight"),
	 * falls, re-lands, fires a SECOND genuine turnover that restarts the window, and lands somewhere
	 * the player is no longer looking — which cancelled a pull at 0.777 of its hold.
	 *
	 * @param bLockLocalTeam  true  = the LOCAL player's team dropped it. They are locked out; the ring
	 *                                must NOT appear for them and touch must be refused for 5 s.
	 *                        false = the OPPOSING team dropped it. The local player may pull it, which
	 *                                is the arm `Trace.HUD.PullRing.Hold` can then complete.
	 */
	static bool DebugStageTurnoverAtLocalCrosshair(UWorld* World, float DistanceUU, bool bLockLocalTeam,
		FString& OutReport);

	/**
	 * DEV ONLY. The catcher ServerApplyCatchZone last chose, or null.
	 *
	 * Exists for ONE caller: Trace.Integration.Verify's magnet check (spec v14 §6 — Mace's +30%
	 * magnet radius). That passive is a change to WHO IS IN RANGE, and "who is in range" is not
	 * observable from outside the catch loop by any other means. The alternative was to
	 * re-implement the loop inside the harness, which is exactly how this project has previously
	 * verified something other than its shipping code.
	 */
	ATraceCharacter* GetCatchZoneTargetForTest() const { return CatchZoneTarget.Get(); }
#endif

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
	 * @param HeldSeconds  SPEC v13 §6. How long the throw button was held, measured on the SERVER's
	 *                     own clock (see RequestPassInput). Negative means "full charge" and is what
	 *                     the diagnostics use, so a momentum measurement stays comparable with the
	 *                     pre-v13 numbers.
	 * @return true when a throw actually left. False for a wrong mode, a non-holder, a dead holder,
	 *         a Core that is already loose, or a throw still on cooldown.
	 */
	bool ThrowFromHolder(ATraceCharacter* Thrower, float HeldSeconds = -1.f);

	// --- SPEC v13 §6 — THE CHARGE-UP THROW ---------------------------------------------------------
	//
	// Verbatim: "When a player RELEASES the throw button, the core should instantly be released." /
	// "The longer the player holds down, the more momentum the core has." / "Start by making a one
	// second charge up time to reach the current core throw momentum" / "Charge time to throw momentum
	// should be a linear correlation" / "So if the player just clicks the throw button it will throw
	// with very low momentum".
	//
	// FOUR NUMBERS, ALL TUNABLE, and they are the four the note names:
	//
	//   TIME    GetThrowChargeSeconds()      — how long a full wind-up takes. 1.0 s, as asked.
	//   FLOOR   GetThrowChargeFloor()        — the fraction an instant click gets. 0.15
	//                                          ([ASSUMPTION]: "very low" but NOT zero, because a
	//                                          zero-momentum throw drops at the player's feet and
	//                                          reads as a bug, not as a weak throw).
	//   MAX     GetThrowChargeMaxFraction()  — the ceiling on the NORMALISED HOLD when the clamp is
	//                                          off, so an overcharge variant is reachable. 1.0 by
	//                                          default, which makes a full hold exactly "the current
	//                                          core throw momentum" and nothing stronger.
	//   CLAMP   DoesThrowChargeClamp()       — whether holding PAST the charge time keeps adding.
	//                                          Default on ([ASSUMPTION]: it clamps).
	//
	// ALL FOUR ARE UTraceSettings PROPERTIES, bound BY NAME, and the names are the settings page's own
	// (CoreThrowChargeSeconds, CoreThrowChargeFloorFraction, CoreThrowChargeMaxFraction,
	// bCoreThrowChargeClampsAtFull) with Config/DefaultGame.ini shipping values for all four. The ini
	// wins. The arithmetic below is that file's, copied verbatim rather than reinterpreted.
	//
	// WHAT THE CHARGE SCALES, EXACTLY: the IMPULSE, and only the impulse. Spec v8 §4's inherited
	// velocity — the thrower's own motion, vertical included — is added ON TOP at full strength and is
	// NOT scaled by the charge. So a jumping instant-click still leaves with the whole jump in it (it
	// simply has almost no impulse of its own), and a jumping full charge leaves with the jump plus a
	// full-power impulse. See ComputeThrowLaunchVelocity, which is the one expression both the Core
	// and the bot solver use.

	/** MODE B, spec v13 §6. Seconds of hold that reach full throw momentum. UTraceSettings::CoreThrowChargeSeconds. */
	static float GetThrowChargeSeconds();

	/** MODE B, spec v13 §6. Impulse fraction an instant click throws with. UTraceSettings::CoreThrowChargeFloor. */
	static float GetThrowChargeFloor();

	/**
	 * MODE B, spec v13 §6. Ceiling on the NORMALISED HOLD when the clamp is off — NOT a power ceiling.
	 *
	 * Power is always Floor + (1 - Floor) * t, so at 1.5 a 1.5 s hold throws at 1.43x, not 1.5x. That
	 * is UTraceSettings::CoreThrowChargeMaxFraction's own definition, and it is followed rather than
	 * re-derived: two definitions of "maximum charge" that differ by 5% is a disagreement nobody finds
	 * until a designer wonders why the slider lies.
	 */
	static float GetThrowChargeMaxFraction();

	/** MODE B, spec v13 §6. False lets a hold past the charge time keep adding momentum. */
	static bool DoesThrowChargeClamp();

	/**
	 * MODE B, spec v13 §6. THE LINEAR MAP, published so nothing re-derives it.
	 *
	 * Power = Floor + (1 - Floor) * tCap, where t = HeldSeconds / ChargeSeconds and tCap is t capped at
	 * 1 (clamping) or at GetThrowChargeMaxFraction() (not clamping). A negative @p HeldSeconds means
	 * "full charge" and returns exactly 1.0 — the throw the game had before v13.
	 *
	 * The HUD charge meter, the bots' "charge for distance" solver and the throw itself all call this,
	 * for the same reason SteerTowardCatchPoint is shared: a second copy of the curve is a second
	 * answer, and the one the player sees would be the copy.
	 *
	 * SPEC v19 §3 — @p Thrower EXTENDS THE CAP ON t, AND NOTHING ELSE ABOUT THE CURVE.
	 *
	 * Mortimer may usefully hold for TraceAbilityTraits::GetThrowChargeHoldScale() times as long as
	 * anybody else, so his tCap is that much larger and the SAME line Power = Floor + (1 - Floor) * t
	 * is simply extrapolated further along. He is not given a separate multiplier, a separate floor or
	 * a separate curve — there is still exactly one curve in this game, which is the whole point of
	 * this function existing.
	 *
	 * DEMO 21 ITEM 7 — AND PAST THE ORIGINAL 100% POINT THAT EXTRAPOLATION IS WORTH 0.6x.
	 *
	 *     EffectiveT = min(t, tCap) + PastFull * clamp(t - tCap, 0, tCap * (HoldScale - 1))
	 *     Power      = Floor + (1 - Floor) * EffectiveT
	 *
	 * where tCap is still the ORIGINAL 100% point (1, or GetThrowChargeMaxFraction() with the clamp
	 * off) and PastFull is TraceAbilityTraits::GetThrowChargePastFullScale(). ONE LINE WITH A BEND IN
	 * IT, not two curves: the gradient past the bend is a FRACTION of the shared gradient, so a
	 * retune of the floor or the charge time still moves both halves together (spec v24 §0).
	 *
	 * For every character but Mortimer HoldScale is 1, so the second term's own clamp is [0, 0] and
	 * the whole of Demo 21 item 7 is arithmetically absent — not "defaulted away", absent.
	 *
	 * Defaults to nullptr = "no character opinion", which reproduces the shipped clamp exactly, so
	 * every existing caller (the meter's floor probe, the diagnostics that measure the curve itself)
	 * keeps measuring what it always did.
	 */
	static float GetThrowChargeScaleForHold(float HeldSeconds, const AActor* Thrower = nullptr);

	/** The inverse of the above: how long to hold for @p ChargeScale. Used by the bots. */
	static float GetThrowHoldSecondsForScale(float ChargeScale);

	/**
	 * EVERY MACHINE, spec v13 §6. Is the LOCAL player winding a throw up right now?
	 *
	 * PREDICTED, not replicated, and that is the requirement rather than an optimisation: a charge
	 * meter that waits for the server is a meter that lags the finger by the round trip, and a player
	 * cannot aim a power they are shown late. The press starts a purely local clock on the owning
	 * machine at the instant the button goes down; the THROW itself is still resolved entirely on the
	 * server from the server's own copy of the hold (see RequestPassInput), so the prediction can
	 * never launch anything or lie about where the Core went — the worst it can do is show a meter
	 * for a throw the server refuses on cooldown.
	 *
	 * A bot's charge deliberately does NOT set this: it is the local PLAYER's meter.
	 */
	bool IsThrowCharging() const;

	/** 0..1 wind-up progress for the HUD meter, or -1 when the local player is not charging. */
	float GetThrowChargeAlpha() const;

	/** Floor..Max — the momentum fraction a release right now would throw with. -1 when not charging. */
	float GetThrowChargeScaleNow() const;

	// --- SPEC v28 §7 — A FULL CHARGE CANNOT BE HELD FOREVER -----------------------------------------
	//
	// Verbatim: "A player can hold a core at full throw charge indefinitely. Make a second little red
	// ring which fills on the inside of the green one on a .6seconds timer. This timer should start
	// right when throw charge reaches full. When the second timer completes, the core is released at
	// full charge automatically, if a player doesn't throw it before then."
	//
	// *** THE SERVER DECIDES, AND THE RING DRAWS THE SERVER'S OWN CLOCK. *** This is the one thing
	// about §7 that is a networking requirement rather than a feature. Everything about the GREEN ring
	// is deliberately PREDICTED (see IsThrowCharging above): it shows the player their own finger, it
	// can be a round trip ahead of the server, and the worst that costs is a meter that read 3% high
	// at the release. A red ring predicted the same way would be a different thing entirely — it does
	// not report a hold, it reports a DEADLINE, and a locally-run deadline finishes at a different
	// instant on every machine. A player would watch their ring complete and keep holding for another
	// 80 ms, or watch the Core leave with the ring at 92%. So:
	//
	//   * ThrowFullChargeServerTime is written ONLY on the authority, at the exact instant the
	//     server's own hold crossed CoreThrowChargeSeconds (start + charge time, NOT "the tick we
	//     noticed"), and it is replicated.
	//   * GetThrowAutoReleaseAlpha() below is that instant measured against the SHARED clock
	//     (AGameStateBase::GetServerWorldTimeSeconds), so the number the HUD draws is arithmetic on
	//     the server's own decision rather than a second opinion about it.
	//   * The auto-release itself is ServerTickThrowAutoRelease(), fired off the identical deadline.
	//
	// The ring can therefore only ever be LATE by the client's downstream latency — never early, and
	// never finished while the Core is still in the hand.
	//
	// A MANUAL THROW IS COMPLETELY UNCHANGED. Nothing here touches RequestPassInput's release path:
	// releasing before the deadline runs the same line it always has, with the same server-measured
	// hold. §7 only adds an upper bound to how long the press may sit there.

	/**
	 * SPEC v28 §7. Seconds from FULL CHARGE to the automatic release. 0.6 s, as asked.
	 *
	 * Bound BY NAME to UTraceSettings::CoreThrowFullChargeAutoReleaseSeconds if that property exists,
	 * exactly like the other four charge knobs, so a shipped ini value wins the moment one is added;
	 * until then Trace.ModeB.ThrowAutoReleaseSeconds is the knob and 0.6 is its default. 0 disables
	 * the auto-release (and the red ring with it), which is the RED ARM for this section.
	 */
	static float GetThrowAutoReleaseSeconds();

	/**
	 * SPEC v28 §7. 0..1 fill of the RED ring, or -1 when there is no full charge being held.
	 *
	 * SERVER TRUTH, on every machine: it is (shared clock now - the server's own full-charge instant)
	 * over the window. -1 means "the server has not seen this charge reach full", which is also what a
	 * client sees for the fraction of a round trip between the two — the ring simply has not appeared
	 * yet, rather than appearing with a number nobody authored.
	 */
	float GetThrowAutoReleaseAlpha() const;

	// SPEC v28 §7's wire record, read by Trace.ModeB.ThrowAutoRelease.Report. See the members.
	int32 GetFullChargeStampsSeen() const     { return FullChargeStampsSeen; }
	float GetLastSeenFullChargeStamp() const  { return LastSeenFullChargeStamp; }
	float GetPeakSeenAutoReleaseAlpha() const { return PeakSeenAutoReleaseAlpha; }

	/** The raw replicated instant, for that same report. Prefer GetThrowAutoReleaseAlpha() to draw with. */
	float GetThrowFullChargeServerTimeForTest() const { return ThrowFullChargeServerTime; }

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

		// --- SPEC v13 §8 counters. Verification only, never a rule. ---------------------------------

		/**
		 * Upward-facing contacts REFUSED as landings because the Core was not settling on them —
		 * a graze across the flat top of a structure or the corner cove's treads, mid-flight.
		 *
		 * THIS IS THE BUG'S OWN COUNTER. Under the pre-v13 rule every one of these was a turnover,
		 * which is precisely "the core is thrown and it turns over before it touches the ground".
		 */
		int32 GlancingContactsRejected = 0;

		/**
		 * Turnovers that fired on a FAST, SHALLOW arrival — the Core was still travelling above
		 * Trace.ModeB.MidAirTurnoverSpeed and met the surface at less than
		 * Trace.ModeB.MidAirTurnoverDegrees. That is a Core flying PAST something, not landing on it.
		 *
		 * THE NUMBER THE §8 REPRO READS, and it is deliberately a statement about the GEOMETRY OF THE
		 * EVENT rather than about the rule's verdict: both arms of the A/B compute it identically, so
		 * `Trace.ModeB.LandingRule 0` makes it climb and the shipped rule holds it at 0. A counter
		 * defined as "a turnover the rule refused" could not do that — it would read zero in both arms
		 * by construction and would prove nothing.
		 *
		 * SPEED ALONE IS NOT ENOUGH and neither is height. A Core dropped straight onto the floor
		 * arrives at 1500 uu/s and is a perfectly good landing; a Core resting on a crate is 400 uu up
		 * and is spec v7 §4 working. It is the combination of "fast" and "barely descending" that
		 * describes what the user reported.
		 */
		int32 MidAirTurnovers = 0;

		/** Every other turnover: the Core arrived on the surface or stopped on it. The intended outcome. */
		int32 LandedTurnovers = 0;

		// --- SPEC v19 §1.5 counters. Verification only, never a rule. --------------------------------
		//
		// Verbatim, with the user's own emphasis: "ENSURE the ball does not turnover until it actually
		// visibly touches the ground or the top of an obstacle." That word is a re-report — v13 §8
		// already fixed the GRAZE (a Core flying PAST a surface), and the sentence came back anyway. So
		// the number this pass drives to zero is deliberately NOT another statement about the arrival:
		// it is a statement about WHERE THE VISIBLE ORB WAS when possession changed.
		//
		// Measured by ATraceCore::MeasureVisibleSupportGap(): a sphere of the ORB'S RENDERED RADIUS,
		// swept straight down from the Core's centre, answering "how far is the ball a player can see
		// from the thing that is holding it up". Zero-ish means it is sitting on something. A large
		// number means possession changed with clear air under the ball, which is the report.

		/** Turnovers whose orb was within Trace.ModeB.TurnoverContactSlack of the surface below it. */
		int32 GroundedTurnovers = 0;

		/**
		 * THE SPEC v19 §1.5 BUG'S OWN COUNTER: turnovers that fired with a VISIBLE GAP under the orb.
		 *
		 * MUST BE ZERO. Computed from the geometry of the event and not from the rule's verdict, so
		 * both arms of the A/B compute it identically: Trace.ModeB.GroundedTurnover 0 removes the rule
		 * and makes this climb, the shipped rule holds it at 0.
		 */
		int32 UngroundedTurnovers = 0;

		/** Largest gap seen under any turnover, uu. The headline number of the §1.5 report. */
		float WorstTurnoverGapUU = 0.f;

		/**
		 * Landings REFUSED by the §1.5 rule: an upward-facing contact the Core was not actually
		 * resting on top of — the lip or corner of a block, where the depenetration normal points up
		 * but there is nothing underneath. Before this pass every one of these was a turnover in
		 * mid-air.
		 */
		int32 UngroundedLandingsRefused = 0;

		// --- SPEC v19 §1.5, THE OTHER HALF: "VISIBLY" IS A STATEMENT ABOUT FRAMES. -------------------
		//
		// [DIAGNOSED] FROM THE SHIPPED BUILD'S OWN INSTRUMENT, before a line was changed. A
		// Trace.ModeB.TurnoverRepro run with Trace.ModeB.TurnoverLog on prints, twelve times out of
		// twelve, a CONTACT line and a SURFACE TURNOVER line CARRYING THE SAME FRAME NUMBER. The Core
		// is moved onto the surface and handed to the enemy inside one tick, and rendering happens
		// after the tick — so THE BALL IS NEVER DRAWN TOUCHING ANYTHING. The last frame a player sees
		// it loose it is still in the air; the next frame it is in an enemy's hands.
		//
		// That is the sentence the user re-sent with ENSURE in capitals, and it is why the v13 §8 fix
		// (which is still holding — 0 mid-air turnovers on the same run) did not close the report:
		// every arrival test in the world is a question about the frame BEFORE the ball touches, and
		// this one is about the frames AFTER.
		//
		// So the landing is now LATCHED and possession is held for Trace.ModeB.TurnoverSettleSeconds,
		// during which the ball is drawn resting or bouncing where it fell. These two counters are the
		// proof, and they measure the EVENT — how many frames of contact a player could actually have
		// seen — rather than the rule's verdict, so both arms of the A/B compute them identically.

		/**
		 * *** THE §1.5 BUG'S OWN COUNTER. MUST BE ZERO. *** Turnovers that fired on the very frame the
		 * landing was established, i.e. with zero rendered frames of the ball in contact.
		 *
		 * Trace.ModeB.GroundedTurnover 0 restores the pre-v19 behaviour exactly and makes this equal
		 * the total. On the shipped rule it is 0.
		 */
		int32 UnseenTurnovers = 0;

		/** Turnovers the player could actually watch land. The intended outcome; must be > 0. */
		int32 SeenTurnovers = 0;

		/** Fewest rendered frames of contact behind any turnover. -1 until a turnover has fired. */
		int32 FewestTurnoverContactFrames = -1;

		/** Shortest time any turnover was held for, seconds. -1 until a turnover has fired. */
		float ShortestTurnoverDwellSeconds = -1.f;
	};

	static FSurfaceRuleStats SurfaceStats;

	/**
	 * SPEC v13 §5 counters — the contested magnet. Verification only, never a rule.
	 *
	 * "If the core is within the 'magnet' zone of two or more players from opposite teams, it should
	 * go to the player closest to the core." A rule about which of several candidates wins is
	 * unobservable from a log that only ever names the winner, so the contested frames are counted
	 * apart from the uncontested ones and every switch of target is counted with them.
	 */
	struct FCatchContestStats
	{
		/** Frames the magnet ran with EXACTLY ONE eligible player in range. */
		int32 UncontestedFrames = 0;

		/** Frames with two or more — the case the spec is about. */
		int32 ContestedFrames = 0;

		/** Of those, frames where the contenders were not all on the same team. */
		int32 CrossTeamContestedFrames = 0;

		/** Distinct contests entered (a contested frame whose previous frame was not contested). */
		int32 Contests = 0;

		/** Times the pull moved from one player to another while the Core stayed loose. */
		int32 TargetSwitches = 0;

		/** Switches HYSTERESIS refused: a challenger was nearer, but not by enough to be worth a flicker. */
		int32 HysteresisHolds = 0;

		/** Largest contested set seen. */
		int32 MaxContenders = 0;
	};

	static FCatchContestStats CatchStats;

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
	 *
	 * SPEC v13 §6. @p ChargeScale multiplies THE IMPULSE ONLY:
	 *
	 *     launch = (aim * speed + up * speed * upbias) * ChargeScale  +  thrower velocity * inheritance
	 *
	 * The inherited term is deliberately outside the bracket. Charge is how hard you threw it; the
	 * inheritance is the motion the Core already had in your hands, and a player who jumps and taps
	 * has not undone their own jump. Default 1 = a full-power throw, i.e. exactly the pre-v13 launch.
	 */
	static FVector ComputeThrowLaunchVelocity(const AActor* Thrower, const FVector& AimDirection,
		float ChargeScale = 1.f);

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
		/** SPEC v13 §6. The hold that produced it, and the impulse fraction that hold bought. */
		float HeldSeconds = 0.f;
		float ChargeScale = 1.f;
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
	 * SPEC v25 §2. The turnover started or ended: recolour and resize the beam on this machine now.
	 *
	 * The beam is the one part of the turnover a player reads from across the field, so it must not
	 * wait for the next Tick reconciliation — a 5 s window that spends its first frame the wrong
	 * colour is 20% of the read gone at 60 Hz on the very frame the player is looking for it.
	 */
	UFUNCTION()
	void OnRep_Turnover();

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
	 *
	 * SPEC v12 §4 cut CatchRadius 500 -> 450 ("reduce the 'magnet' radius for catching in game mode b
	 * by 10%"). The steering itself is TraceModeBTuning::SteerTowardCatchPoint, which this calls and
	 * which Trace.ModeB.CatchTest also calls — the measurement and the game run the same function on
	 * purpose, so a catch rate reported for the magnet is a catch rate for THE magnet.
	 *
	 * --- SPEC v13 §5: THE CONTESTED ZONE RESOLVES TO THE NEAREST PLAYER -----------------------------
	 *
	 * Verbatim: "If the core is within the 'magnet' zone of two or more players from opposite teams,
	 * it should go to the player closest to the core."
	 *
	 * [ASSUMPTION] "from opposite teams" describes the interesting case rather than restricting the
	 * rule, so NEAREST-WINS IS APPLIED TO ANY CONTESTED SET, two teammates included. A magnet that
	 * resolved cross-team contests by distance and same-team contests by roster order would be two
	 * rules where the note states one, and the second would be invisible until it misbehaved.
	 *
	 * THE CORE CURVES TO EXACTLY ONE PLAYER. It always did — one Best, one call to the steering — so
	 * the change is not "stop pulling toward several", it is that the winner is now DECIDED rather
	 * than emergent, deterministic frame to frame, and STABLE:
	 *
	 *   NEAREST WINS, measured to the capsule SURFACE, which is the same distance the falloff and the
	 *   pickup radius use. Ties (identical distance to the millimetre) break on a stable per-player
	 *   key, never on roster order, so two players standing on the same spot cannot make the answer
	 *   depend on the order GatherCharacters happened to return them in.
	 *
	 *   HYSTERESIS. A challenger has to be nearer than the incumbent BY A MARGIN
	 *   (Trace.ModeB.CatchContestHysteresis, 60 uu) before the pull moves. Without it two defenders
	 *   converging at a similar range swap the target every few frames, and the steering — which is an
	 *   exponential approach toward whichever point it is given — averages out to a Core that curves
	 *   toward neither and is caught by nobody. The margin is one-off, not per-frame: once the pull
	 *   moves, the new holder is the incumbent and the old one needs the same margin to take it back.
	 *   It is dropped the instant the incumbent stops being eligible (dead, out of range, behind the
	 *   Core), because holding a target that no longer qualifies is not stability, it is a stale pull.
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

	/**
	 * SPEC v19 §1.5. How far the ORB A PLAYER CAN SEE is from whatever is holding it up, in uu.
	 *
	 * *** THIS IS THE MEASUREMENT THE SPEC ASKS FOR, AND IT IS NOT THE ONE THE RULE ALREADY HAD. ***
	 * v7 §4 asks what the contact NORMAL was; v13 §8 asks how the Core ARRIVED. Both are questions
	 * about an event, and both can be satisfied by a Core that is nowhere near the top of anything —
	 * the case that survives them is a contact with the LIP OR CORNER of a block, where the sphere
	 * sweep's depenetration normal points up (so it reads as a floor), the descent is steep (so it
	 * reads as a landing), and the ball is beside the block with clear air underneath. Possession
	 * changes; the player watches the Core carry on falling. That is "it turned over before it
	 * touched", and no amount of extra angle tuning can see it, because the geometry it is wrong
	 * about is BELOW the Core rather than in front of it.
	 *
	 * So this asks the player's question directly: sweep a sphere of the RENDERED orb radius (the
	 * mesh is the 100 uu engine sphere at TraceCoreTuning::OrbScale, so 20 uu — deliberately NOT the
	 * 22 uu collision radius, because the number that matters is the one the eye can check) straight
	 * down from the Core's centre and return the distance it travelled before touching something.
	 *
	 * @param OutSupportPoint  where the support was found. Untouched when nothing was found.
	 * @return 0 when the orb is already touching, the gap in uu when it is not, and
	 *         Trace.ModeB.TurnoverContactProbe (the full probe length) when there is nothing under it
	 *         at all — which is the strongest possible "this is mid-air" and sorts correctly as the
	 *         worst case in the tallies.
	 */
	double MeasureVisibleSupportGap(FVector& OutSupportPoint) const;

	// --- SPEC v25 §2 internals. Server only unless marked. ----------------------------------------

	/**
	 * SERVER. Registers a turnover: the Core STAYS where it landed and the window opens.
	 *
	 * The one writer of TurnoverLockoutTeam / TurnoverStartServerTime, called from exactly one place
	 * (ServerSurfaceTurnover), so "when does a turnover start" has one answer and one log line.
	 */
	void RegisterTurnover(ETraceTeam DroppingTeam, const FVector& Where, const TCHAR* Why);

	// --- SPEC v28 §2 — THE TURNOVER SOUND FIRES ON THE WRONG EVENT --------------------------------
	//
	// Verbatim: "The core turnover sound should play globally when the core is dropped by a team or a
	// carrier is killed (a turnover), NOT when a team picks up a core which was locked out."
	//
	// WHAT IT USED TO DO, measured rather than assumed: there was exactly one CoreTurnover call site
	// in the whole module and it sat inside RegisterTurnover(), which is reached from exactly one
	// shipping place — ServerSurfaceTurnover, i.e. the instant a THROWN Core has finished bouncing,
	// come to rest and stayed still for Trace.ModeB.TurnoverSettleSeconds. That is not the drop; it is
	// seconds later, it is the frame the five-second lockout OPENS and the other side may take the
	// Core, and a carrier who is shot dead produced no turnover sound at all because a kill never
	// reaches that function.
	//
	// WHAT IT DOES NOW: the sound is announced at the two moments the owner names, both of which are
	// the moment a team STOPS HOLDING THE CORE —
	//
	//   * the carrier throws it and the Core goes loose  (ThrowFromHolder — "dropped by a team")
	//   * the carrier is killed, or leaves               (OnHolderDeath / DropAt — "a carrier is killed")
	//
	// and NOWHERE else. RegisterTurnover is silent, so the landing is silent, the lockout expiring is
	// silent (it never made a sound), and every pickup path — ServerTryLoosePickup -> TakeLooseCore,
	// ServerCompletePull, GrantTo — is silent, which is the half of the sentence with "NOT" in it.
	//
	// ONE ANNOUNCEMENT PER LOSS, and the de-dup below is not defensive padding: a carrier's death
	// legitimately reaches this twice. ATraceGameMode::NotifyCharacterDied calls DropAt() from inside
	// the health component's OnDeath broadcast, and ATraceCore::OnHolderDeath is a listener on that
	// SAME broadcast — two handlers, one death. Keyed on the pawn that lost it as well as the time, so
	// two genuinely different losses in the same quarter second (a kill, then the killer immediately
	// throwing) still make two sounds.

	/**
	 * SERVER. Plays CoreTurnover, game-side, at @p Where — once per possession loss.
	 *
	 * @param Loser  the pawn that just lost the Core. Used only as the de-dup key.
	 * @param Why    for the log line.
	 */
	void AnnounceTurnoverSound(const FVector& Where, const ATraceCharacter* Loser, const TCHAR* Why);

	/** De-dup key for AnnounceTurnoverSound: who lost it, and on the shared clock, when. */
	TWeakObjectPtr<const ATraceCharacter> LastTurnoverSoundLoser;
	float LastTurnoverSoundServerTime = -1000.f;

	/** SERVER. Ends the window (expiry, the pull completing, the Core leaving the world). */
	void ClearTurnover(const TCHAR* Why);

	/**
	 * SERVER. One frame of the pull state machine: validate every hold, cancel the ones that no
	 * longer qualify, complete the OLDEST one that has reached the hold time, expire the window.
	 *
	 * Runs from ServerTickLooseCore, ahead of the pickup poll, because a completed pull and a
	 * first-contact pickup are two answers to "who gets it" and the pull is the one the player earned.
	 */
	void ServerTickTurnover(float DeltaSeconds);

	/** SERVER. Applies one press/release to the latch. The only writer of PullInputHeld. */
	void ServerApplyPullInput(ATraceCharacter* Requester, bool bPressed);

	/** SERVER. Drops @p Puller's hold and their latch entry, if any. CANCELS; never pauses. */
	void CancelPullFor(const ATraceCharacter* Puller, const TCHAR* Why, bool bAlsoClearLatch);

	/**
	 * SERVER. A pull reached the hold time: the Core starts travelling to @p Winner and every other
	 * fill is cancelled ("first to complete wins; the loser's fill cancels").
	 */
	void ServerCompletePull(ATraceCharacter* Winner);

	/**
	 * SERVER. One frame of the delivery flight. Returns true when it consumed the tick (the Core is
	 * still travelling, or has just been taken and the caller must touch no member state).
	 *
	 * THE SPEED IS ATraceCore::GetThrowSpeed(), NOT A NUMBER OF ITS OWN — spec v25: "at full core
	 * thrown velocity ... the same speed constant a thrown Core uses, not a new number". So a retune
	 * of the throw, or of the Core's weight (which divides the throw speed by sqrt(mass)), moves the
	 * pull with it. See the standing rule in the module notes.
	 */
	bool ServerTickPullTravel(float DeltaSeconds);

	/** Server, mode B. The guarded loose -> held transition. Sets the grace override, then grants. */
	void TakeLooseCore(ATraceCharacter* Taker);

	/** Clears every loose field. Does NOT grant: callers decide where the Core goes next. */
	void ClearLooseState();

	/**
	 * SPEC v19 §1.5. Forgets any pending landing, so the next flight is awarded on its own clock.
	 *
	 * Called by ClearLooseState() (every path that ENDS a flight) and by both paths that START one —
	 * the real throw and DebugLaunchLoose — because those two set the loose fields by hand rather than
	 * through ClearLooseState, and a stale latch there would award a fresh throw the instant it left
	 * the hand.
	 */
	void ClearPendingTurnover();

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
	 * Server, diagnostics only. SPEC v25 §2's red arm: park the Core loose at @p Where and register a
	 * turnover against @p DroppingTeam, exactly as a landed throw would.
	 *
	 * It goes through DebugLaunchLoose and then the SHIPPING RegisterTurnover(), so the window it
	 * opens is the window the game plays; the only thing it fakes is the throw, which is the part a
	 * live match cannot be asked to arrange on cue. Every check the harness makes afterwards goes
	 * through CanPullNow / RequestPullInput / ServerTickTurnover — the real ones.
	 *
	 * @return false when the Core could not be put loose (mode A, a locked state, no team).
	 */
	bool DebugRegisterTurnover(ETraceTeam DroppingTeam, const FVector& Where);

	/**
	 * Server. Runs the Trace.ModeB.TurnoverVerify scenario, one step per tick. See the .cpp.
	 *
	 * SPEC v25 §2 asks for four rules to be RED-ARMED: a same-team player must fail to pull, a player
	 * with no line of sight must fail, a release at 0.29 s must fail, and the lockout must expire.
	 * Each of those is a NEGATIVE claim, and a negative claim about a rule nobody has watched fire is
	 * worth nothing — so every red step here is paired with the green one beside it, on the same
	 * pawns, in the same window, through the same functions.
	 */
	void TickTurnoverVerify();

	/** Server, harness only. Points a BOT's controller at the loose Core so it satisfies the hover test. */
	bool DriveAimAtLooseCore(ATraceCharacter* Puller);

	// --- Trace.ModeB.TurnoverVerify scenario state (diagnostics only) ------------------------------

	int32 TurnoverVerifyStep = -1;
	float TurnoverVerifyDeadline = 0.f;
	float TurnoverVerifyMark = 0.f;
	int32 TurnoverVerifyPassCount = 0;
	int32 TurnoverVerifyFailCount = 0;
	int32 TurnoverVerifySkipCount = 0;
	int32 TurnoverVerifyRetriesLeft = 0;
	bool bTurnoverVerifyArmed = false;

	/**
	 * The red arm has already run once in this session.
	 *
	 * A LATCH THAT IS NEVER RESET, and it is not belt and braces: -ExecCmds and -TraceExec set the
	 * arming CVar at ECVF_SetByConsole, and a later Set(0, ECVF_SetByCode) from inside the game is
	 * SILENTLY IGNORED because console priority beats code priority. The first version of this
	 * harness re-armed on the tick after it reported and ran three times in forty seconds - the same
	 * defect the momentum test's own comment in this file warns about. Anything that disarms itself
	 * has to do it with state it owns.
	 */
	bool bTurnoverVerifyDone = false;

	/** Step 4 only: the delivery was actually SEEN, so a puller who merely walked over it cannot pass. */
	bool bTurnoverVerifySawWinner = false;
	TWeakObjectPtr<ATraceCharacter> TurnoverVerifyPuller;
	TWeakObjectPtr<ATraceCharacter> TurnoverVerifyLocked;

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

	// --- SPEC v13 §6, the charge. Server truth and the owning machine's prediction, side by side. ---

	/**
	 * SERVER. Shared-clock time the current holder pressed the throw button, or < 0 when nobody is
	 * charging.
	 *
	 * THE HOLD IS MEASURED ON THE SERVER, from the press RPC to the release RPC, and the client is
	 * never asked how long it held. That is the same policy the aim direction has had since the old
	 * PerformPass was deleted: a client-supplied hold is a client-supplied launch velocity, and it
	 * would be the single most valuable thing in the game to lie about. The cost is that a player's
	 * charge is shortened by their own latency at the release, which is the honest trade — the meter
	 * they aim with is local and instant (see LocalThrowChargeStartTime), the momentum is not.
	 */
	float ThrowChargeStartServerTime = -1.f;

	/** SERVER. Whose press it is, so a release from anybody else cannot fire it. */
	TWeakObjectPtr<ATraceCharacter> ThrowChargeHolder;

	/**
	 * EVERY MACHINE, the LOCAL PLAYER's own meter. Never replicated, and never set for a bot.
	 *
	 * On a listen server this exists alongside the server fields above and is set from the same press;
	 * they are not redundant, they answer different questions ("what will the server launch" against
	 * "what is my finger doing right now"), and only the second one can be instant.
	 */
	bool bLocalThrowCharging = false;
	float LocalThrowChargeStartTime = 0.f;

	/** Ends any charge in progress WITHOUT throwing. Death, a possession change, a mode switch. */
	void ClearThrowCharge(const TCHAR* Reason);

	/**
	 * SPEC v28 §7. SERVER. Publishes the full-charge instant and fires the automatic release.
	 *
	 * Called once per tick from the HELD branch of Tick(), which is the only state a charge can exist
	 * in. Two jobs, in this order and deliberately in one function:
	 *   1. maintain ThrowFullChargeServerTime — set it the tick the server's own hold crosses the
	 *      charge time, clear it if a re-press restarted the clock below full;
	 *   2. when the shared clock passes that instant + GetThrowAutoReleaseSeconds(), throw.
	 *
	 * Keeping them together is what makes "the ring shows the server's own progress" true by
	 * construction: the number the HUD draws and the deadline the throw fires on are the SAME
	 * expression, not two copies of it that a retune could separate.
	 *
	 * @return true when it threw. The caller must touch no member state afterwards — the Core is
	 *         loose and this tick is over.
	 */
	bool ServerTickThrowAutoRelease();

	// --- SPEC v25 §2 server state. Nothing here is replicated; the four facts above are. ----------

	/**
	 * Whose pull button is DOWN, as last reported by that player's own machine.
	 *
	 * Separate from PullHolds on purpose, and the separation is the "cancels, does not pause" rule:
	 * the latch survives a lost hover, the HOLD does not. A player who keeps the button down through
	 * a blink of cover therefore starts a FRESH 0.3 s the moment the crosshair comes back, rather
	 * than resuming a hold that would let them pull through a wall by looking away.
	 */
	TArray<TWeakObjectPtr<ATraceCharacter>> PullInputHeld;

	/**
	 * A turnover has already been registered for THIS loose period.
	 *
	 * Load-bearing rather than tidy. The Core now stays lying on the surface it landed on, so the
	 * landing rule sees the same at-rest, supported Core on every following frame — without this
	 * latch it would register a second turnover a frame later, and a third after that, restarting the
	 * 5 s clock for as long as the Core sat there. Cleared by ClearLooseState(), i.e. by every path
	 * that ends a flight.
	 */
	bool bTurnoverRegisteredThisFlight = false;

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
	 * SPEC v6 §4.1. The catcher the magnet is currently pulling toward.
	 *
	 * Held so that "the Core curved toward X" is announced ONCE per catch rather than every frame of
	 * the curve. An invisible mechanic with no log line is one nobody can tell is broken, and this
	 * project has twice declared a working mechanic dead for exactly that reason.
	 *
	 * SPEC v13 §5 GAVE IT A SECOND JOB AND IT IS NO LONGER "for the log line only": this is the
	 * INCUMBENT the contest's hysteresis is measured against. That makes it real state rather than a
	 * reporting convenience, so it is cleared on every path that ends a flight (ClearLooseState, a new
	 * throw, the magnet finding nobody in range) — a stale incumbent would hand a 50 uu head start to
	 * a player from the previous possession.
	 */
	TWeakObjectPtr<ATraceCharacter> CatchZoneTarget;

	/** SPEC v13 §5. True while the last magnet frame had two or more eligible players in range. */
	bool bCatchZoneContested = false;

	/**
	 * SPEC v13 §8. Shared-clock time the CURRENT flight last had a blocked sweep, and the speed it
	 * was travelling at when it did. Diagnostics for Trace.ModeB.TurnoverLog only.
	 */
	float LastContactServerTime = -1.f;

	// --- SPEC v19 §1.5: THE LANDING IS LATCHED, THEN SHOWN, THEN AWARDED. --------------------------
	//
	// "ENSURE the ball does not turnover until it actually visibly touches the ground or the top of an
	// obstacle." Before this pass the landing and the change of possession were the SAME TICK, so no
	// rendered frame ever contained the ball in contact — see FSurfaceRuleStats::UnseenTurnovers for
	// the measurement that says so. These fields are what turns one instant into two: the landing is
	// recorded here, the ball keeps being drawn where it fell, and ServerSurfaceTurnover runs once the
	// player has actually had frames to see it.
	//
	// Every one of them is server-only and is cleared by ClearLooseState(), so a new flight can never
	// inherit the previous one's pending landing.

	/** Shared-clock time this flight's landing was established, or -1 when none is pending. */
	float PendingTurnoverLandedServerTime = -1.f;

	/** GFrameCounter at that instant, so "how many frames was it drawn touching" is exact. */
	uint64 PendingTurnoverLandedFrame = 0;

	/** The surface the pending landing was established on. Refreshed while contact continues. */
	FVector PendingTurnoverSurfacePoint = FVector::ZeroVector;
	FVector PendingTurnoverSurfaceNormal = FVector::UpVector;

	/**
	 * The v13 §8 arrival numbers, captured at the LANDING rather than at the award.
	 *
	 * Load-bearing rather than tidy: MidAirTurnovers asks how the Core was travelling WHEN IT MET THE
	 * SURFACE, and once the award is a few frames later the live velocity is a different question's
	 * answer. Reading the fresh one would silently retire the §8 counter.
	 */
	double PendingTurnoverArrivalSpeed = 0.0;
	double PendingTurnoverArrivalSin = 1.0;

	/** Whether the at-rest probe established the pending landing (feeds RestProbeRescues). */
	bool bPendingTurnoverByRestProbe = false;

	// --- SPEC v13 §8: the mid-air turnover reproduction. Diagnostics only. --------------------------
	//
	// Armed with Trace.ModeB.TurnoverRepro <shots>, from -ExecCmds as well as the console for the
	// reason every other harness here is: a console command can only be typed into a window, and the
	// testing policy forbids one.

	/** Server. Fires the repro shots one at a time and prints the tally. See the .cpp. */
	void TickTurnoverRepro();

	int32 TurnoverReproShotsLeft = 0;
	float TurnoverReproNextShotTime = 0.f;
	bool bTurnoverReproArmed = false;
	bool bTurnoverReproReported = false;

	/** Shots actually launched, split by shape: a GRAZE across a flat top, and a DROP onto the floor. */
	int32 TurnoverReproGrazeShots = 0;
	int32 TurnoverReproDropShots = 0;
	int32 TurnoverReproSkipped = 0;

	/** SurfaceStats snapshot taken when the repro armed, so the tally is the repro's own and not the match's. */
	int32 TurnoverReproMidAirAtStart = 0;
	int32 TurnoverReproLandedAtStart = 0;
	int32 TurnoverReproRejectedAtStart = 0;

	/** SPEC v19 §1.5's half of the same snapshot. */
	int32 TurnoverReproUnseenAtStart = 0;
	int32 TurnoverReproSeenAtStart = 0;
	int32 TurnoverReproUngroundedAtStart = 0;
	int32 TurnoverReproGroundedAtStart = 0;

	/**
	 * The graze geometry, resolved ONCE when the repro arms and then reused for every shot.
	 *
	 * Re-probing per shot would let the harness quietly measure a different crate each time, which is
	 * how a repro stops being a repro. Zero extent means no usable cover was found, which is reported
	 * rather than worked around.
	 */
	FVector TurnoverReproTopPoint = FVector::ZeroVector;
	FVector TurnoverReproGrazeDirection = FVector::ForwardVector;
	double TurnoverReproTopExtent = 0.0;

	/**
	 * Server, diagnostics only. Measures how far the flat top under @p FromPoint runs in each of eight
	 * compass directions and returns the longest, so the graze shot can be aimed ALONG a face rather
	 * than at whichever way the probe grid happened to sample.
	 *
	 * @return the run length in uu (0 when the surface is too small to graze).
	 */
	double MeasureTopFaceExtent(const FVector& FromPoint, FVector& OutDirection) const;

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

	/**
	 * SPEC v25 §2. The same pattern one event earlier: the TURNOVER REGISTRATION, captured by
	 * RegisterTurnover at the moment it happens.
	 *
	 * Steps 6, 7 and 8 used to assert "and then the enemy had it". Under v25 the rule stops at "the
	 * Core stays there and the thrower's team is locked out", and what happens next belongs to
	 * whoever earns it — so those steps judge this instead. Not polled, for the reason
	 * bVerifyAwaitingTake is not: an enemy can pull the Core and close the window inside the same
	 * few frames, and a poll would report a rule that fired as one that did not.
	 */
	bool bVerifyAwaitingTurnover = false;
	bool bVerifyTurnoverSeen = false;
	ETraceTeam VerifyTurnoverLockedTeam = ETraceTeam::None;
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
	 * Step 9 only: re-fires the wall shot left before the step is allowed to report a failure.
	 *
	 * The wall shot crosses open pitch with ten bots playing on it, and its own log shows what a
	 * failing run actually is: a bot standing in the corridor intercepts the Core a tenth of a second
	 * after launch — the catch magnet even curves it into them — so the step reports THE WALL RULE
	 * broken on the strength of where somebody was standing. Retrying makes it measure its own
	 * subject. It still fails when the retries run out, and says that is what happened.
	 */
	int32 VerifyWallShotRetriesLeft = 0;

	/** Step 9 only: has the wall shot been fired at all yet? Gates the one-time retry allowance. */
	bool bVerifyWallShotFiredOnce = false;

	/**
	 * Step 8 only: the parked Core has settled and has been declared thrown.
	 *
	 * The step cannot arm on the frame it launches — a Core still in the air would be caught by the
	 * ordinary contact test and the step would silently become a second copy of step 7.
	 */
	bool bVerifyRestArmed = false;

	/**
	 * Step 8 only: re-parks left if somebody takes the Core before it has settled.
	 *
	 * SPEC v25 §2 IS WHY THIS IS NEEDED, and the reason is worth stating because it is a change in
	 * the world rather than in the step. Step 7 now leaves the Core lying on the crate for the whole
	 * lockout window, so the bots converge on it and are still standing there when step 8 parks its
	 * own Core on the same surface — which is then taken on the first frame, before it can ever come
	 * to rest, and the at-rest probe under test never runs. Re-parking is the harness getting its own
	 * subject back; it still fails when the retries run out, and says that is what happened.
	 *
	 * Never reset: the scenario runs once per session, and a counter that topped itself back up would
	 * retry forever against a situation that is not going to change.
	 */
	int32 VerifyRestParkRetriesLeft = 3;

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

	/**
	 * SPEC v25 §2: and so does the turnover window, which additionally ENDS ON A CLOCK rather than on
	 * a replicated write — the last packet a client gets says "open", and five seconds later it is
	 * simply not open any more. So the reconciliation has to compare against IsTurnoverActive(), not
	 * against an OnRep, or the beam would stay big and the wrong colour until the next possession.
	 */
	bool bAppliedTurnoverActive = false;

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


/**
 * SPEC v25 §2 — THE PULL BUTTON'S RETURN PATH, AND WHY IT NEEDS AN ACTOR OF ITS OWN.
 *
 * A client may only send a Server RPC on an actor its own connection OWNS: the engine resolves
 * AActor::GetNetConnection() up the owner chain and drops anything else. ATraceCore SetOwner()s
 * itself to its HOLDER — that is what makes the holder's pass RPC legal — and a pull is by definition
 * sent by somebody who is NOT holding it, and by up to five of them at once. So there is no way to
 * put ServerSetPullInput on the Core: one actor has one owner.
 *
 * This is the smallest thing that fixes that. One per player controller, owned by it, relevant to
 * nobody else, carrying one bool. ATraceGameMode spawns it in PostLogin and destroys it in Logout;
 * bots never need one, because a bot's controller is already on the server and its pull applies
 * directly.
 *
 * IT DECIDES NOTHING. The whole payload is "my button is down / up". Eligibility, the 0.3 s clock,
 * the race and the completion are all ATraceCore's, on the server, exactly as spec v25 requires.
 */
UCLASS(NotPlaceable, Transient)
class TRACE_API ATraceCorePullRelay : public AActor
{
	GENERATED_BODY()

public:
	ATraceCorePullRelay();

	/** The owning client's pull button. Server applies it to the Core's latch. */
	UFUNCTION(Server, Reliable)
	void ServerSetPullInput(bool bPressed);

	/**
	 * The relay owned by @p Controller ON THIS MACHINE, or null.
	 *
	 * An iteration rather than a cached pointer on the controller, because the controller is not this
	 * agent's file to add a member to and because there is exactly one press and one release per
	 * pull — a linear scan over a handful of actors twice per pull is not a cost worth a coupling.
	 */
	static ATraceCorePullRelay* Find(const AController* Controller);
};
