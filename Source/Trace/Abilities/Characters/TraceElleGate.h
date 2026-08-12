// Trace — Elle's SNAP gate, spec v18 §2.
//
// ===================================================================================================
// WHAT §2 ASKS FOR, CLAUSE BY CLAUSE, AND WHERE EACH ONE LIVES
// ===================================================================================================
//
//   "places a portal gate where she stands"
//        One of these actors, spawned at the pawn's feet. Server-spawned, replicated, no collision of
//        any kind — a gate is a place, not an obstacle, and giving it a collider would let it block a
//        bullet or become a movement base for the price of nothing gained.
//
//   "She may reactivate within 4 s to place a second gate, and then players teleport between them."
//        The 4 s window and the pairing live in UTraceAbilitySetElle, because they are facts about
//        ELLE'S CAST rather than about a gate. What lives here is the consequence: once Partner is
//        set, this gate moves whoever steps into it to the partner's mouth.
//
//   "If no second gate inside 4 s the first expires. With both placed, both expire after 8 s."
//        ONE absolute match-clock deadline per gate (ExpireMatchTime), rewritten to the pair deadline
//        at the moment the pair completes. The server destroys the actor when it passes; the visuals
//        leave with the actor rather than being faded out by a second timer that could disagree.
//
// ===================================================================================================
// *** THE FOUNDING INVARIANT, AND HOW THIS ACTOR IS FORBIDDEN FROM BREAKING IT ***
// ===================================================================================================
//
// Spec v18 §2, verbatim: "A Core carrier must not be teleported by an enemy gate."
//
// There is NO carrier test in this file and there must never be one. The classification is the whole
// mechanism, and it is two lines:
//
//   the entrant is Elle, or on Elle's team   ->  ETraceAbilityEffect::Beneficial
//   the entrant is an ENEMY of Elle          ->  ETraceAbilityEffect::Control
//
// and then UTraceAbilityComponent::CanAffect answers. Control on a Core carrier is exactly what
// CanAffectTargetDetailed refuses (ETraceAbilityBlockReason::CarrierControlImmune), so "an enemy gate
// may not move the carrier" is enforced by the same single function that enforces it for Chut's bash
// and Oyster's pull — not by a copy of the rule living here that could drift away from it.
//
// Beneficial passes for a carrier, deliberately: §14 §6 already established that a carrier may ride
// Rocco's Ripple, so a carrier walking into their OWN team's gate is the same question with the same
// answer. Whether that is *wanted* is the one thing a designer gets a knob for
// (UTraceSettings::bElleSnapCarrierMayUseGate) — and that knob is applied on top of the choke point,
// never instead of it.
//
// ===================================================================================================
// THE §2 [ASSUMPTION] THE USER IS MOST LIKELY TO REVERSE — FLAGGED HERE AS WELL AS IN THE SETTINGS
// ===================================================================================================
//
// "allowing players to teleport" is read as ANY PLAYER, BOTH TEAMS. It says players, not teammates,
// and Rocco's Ripple set the precedent that a placed thing is usable by everyone. §2 itself calls
// this "the most reversible-by-them decision in this doc". It is one tick box —
// UTraceSettings::bElleSnapUsableByBothTeams — and flipping it to false needs no code change: the
// enemy branch below simply stops being taken.
//
// ===================================================================================================
// WHY THE TELEPORT IS SERVER-ONLY, WHEN THE RIPPLE'S RIDE IS PREDICTED
// ===================================================================================================
//
// The Ripple pushes velocity, which is continuous: client and server integrating the same inputs stay
// together, so predicting it locally is a straight win. A gate is a DISCONTINUITY. A locally
// predicted teleport that the server refuses (the entrant turned out to be a carrier, or the gate had
// already expired on the authority) is a player standing somewhere they never went, corrected a round
// trip later — the single worst thing this codebase has learned to avoid (spec v10 §10 was exactly
// that failure, with the Core rather than a pawn).
//
// So: the server decides and the server moves, and AActor::TeleportTo's client adjustment carries it.
// The cost is one round trip of latency on the step-through, and it is a deliberate, stated trade
// rather than an oversight.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"     // FVector_NetQuantize
#include "GameFramework/Actor.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "Abilities/TraceAbilityTypes.h"  // ETraceAbilityEffect — the choke point's vocabulary
#include "TraceTypes.h"                  // ETraceTeam

#include "TraceElleGate.generated.h"

class APlayerState;
class ATraceCharacter;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;

/**
 * SPEC v18 §2 — THE EVIDENCE SURFACE FOR ELLE'S GATES.
 *
 * The carrier rule cannot be judged by looking at where a pawn ended up. "The carrier did not move"
 * is true on a build with no carrier rule at all if the fixture simply never reached the teleport —
 * which is exactly the uninformative green this project has been bitten by twice (see the note at the
 * top of TraceMaceOysterVerify.cpp).
 *
 * So every gate DECISION is counted at the point it is taken, split by whether the subject was a Core
 * carrier. A red arm that removes the rule must make CarrierTally().TeleportsCommitted climb; the
 * shipped rule must hold it at zero while OtherTally().TeleportsCommitted climbs in the same frames.
 * Counting the decision rather than the displacement is what lets a teleport to a destination two uu
 * away still count as the rule having been broken.
 */
namespace TraceElle
{
	struct FGateTally
	{
		/** The gate asked, was allowed, and moved somebody. */
		int32 TeleportsCommitted = 0;
		/** UTraceAbilityComponent::CanAffect said no. On a carrier + Control this IS the invariant. */
		int32 RefusedByChokePoint = 0;
		/** bElleSnapUsableByBothTeams is off and the entrant was an enemy of Elle. */
		int32 RefusedByTeamKnob = 0;
		/** bElleSnapCarrierMayUseGate is off and the entrant was a friendly carrier. */
		int32 RefusedByCarrierKnob = 0;
		/** Inside the per-player re-entry lockout — the thing that stops a pair being a trap. */
		int32 RefusedByLockout = 0;

		void Reset() { *this = FGateTally(); }
	};

	/** Decisions taken about a player who was holding the Core at that instant. */
	TRACE_API FGateTally& CarrierTally();

	/** Decisions taken about everybody else. The fixture proving itself. */
	TRACE_API FGateTally& OtherTally();

	TRACE_API void ResetTallies();
}

/**
 * One placed SNAP gate.
 *
 * Elle's ability set owns at most two at a time and destroys both at half time; each actor destroys
 * itself at its own deadline. A gate with no Partner is inert scenery — that is what "if no second
 * gate inside 4 s the first expires" describes, and it is why a lone gate teleports nobody rather
 * than being a special case somewhere.
 */
UCLASS()
class TRACE_API ATraceElleGate : public AActor
{
	GENERATED_BODY()

public:
	ATraceElleGate();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * AUTHORITY ONLY. Call immediately after SpawnActor, before anybody can tick.
	 *
	 * @param InSource           Elle's PlayerState — the instigator the choke point is asked about, and
	 *                           deliberately the PlayerState rather than the pawn so a gate outlives
	 *                           her death exactly as Rocco's Ripple outlives his.
	 * @param InSourceTeam       Elle's team at placement, cached so the ally/enemy classification is
	 *                           stable for the gate's whole life even if her PlayerState goes away.
	 * @param InMouth            world location a player must reach to be taken.
	 * @param InRadiusUU         how close counts as stepping in.
	 * @param InExpireMatchTime  absolute match-clock time this gate vanishes.
	 * @param bInSecondOfPair    cosmetic only: the second gate wears the other colour so a player can
	 *                           tell the two mouths apart at a glance.
	 */
	void InitialiseGate(APlayerState* InSource, ETraceTeam InSourceTeam, const FVector& InMouth,
	                    float InRadiusUU, float InExpireMatchTime, bool bInSecondOfPair);

	/**
	 * AUTHORITY ONLY. Ties two gates together, in both directions, and rewrites BOTH deadlines to
	 * @p InPairExpireMatchTime — §2's "with both placed, both expire after 8 s", where the 8 s is
	 * measured from the moment the PAIR completes rather than from either placement.
	 *
	 * Also seeds the re-entry lockout for everybody standing in either mouth right now. Without that,
	 * Elle — who by construction is stood in the second gate at the instant she places it — is thrown
	 * across the map by her own ability on the frame she casts it.
	 */
	void PairWith(ATraceElleGate* Other, float InPairExpireMatchTime);

	// --- queries, for the ability set, the HUD and the verification harness ------------------------

	FVector GetMouthLocation() const { return FVector(MouthLocation); }
	float   GetRadiusUU() const { return RadiusUU; }
	float   GetExpireMatchTime() const { return ExpireMatchTime; }
	ATraceElleGate* GetPartner() const { return Partner; }
	/** IsValid rather than != nullptr, so a partner that has been destroyed does not read as paired. */
	bool    IsPaired() const { return IsValid(Partner); }
	APlayerState* GetSourcePlayerState() const { return SourcePlayerState; }
	ETraceTeam GetSourceTeam() const { return SourceTeam; }

	/** True while @p Candidate is inside this gate's mouth. Pure query; used by the harness. */
	bool IsInsideMouth(const ATraceCharacter* Candidate) const;

	/**
	 * How many ring beads this machine has actually built, and 0 on a machine that draws nothing.
	 *
	 * "The gate exists" and "the gate is visible" are DIFFERENT FACTS and Demo 17 is what taught us to
	 * separate them: the user's report was "isn't opening a portal", which is a statement about what
	 * they SAW, and a harness that only asserts the actor exists cannot tell that apart from a mouth
	 * nobody can find. Trace.Elle.SnapPressTest prints it for every gate it places.
	 */
	int32 GetDrawnBeadCount() const;

	/**
	 * The effect class an entry by @p Candidate is asked about, and the ONE decision that makes the
	 * founding invariant work. Public so a harness can print it rather than infer it.
	 *
	 * Beneficial for Elle and her team; Control for an enemy — because a gate an enemy placed moving
	 * you is a thing their ability did to you, which is the definition ETraceAbilityEffect gives.
	 */
	ETraceAbilityEffect ClassifyEntry(const ATraceCharacter* Candidate) const;

	/** The reason the last entry attempt for @p Candidate would be refused, as a literal, for logs. */
	const TCHAR* DescribeEntryRefusal(const ATraceCharacter* Candidate) const;

protected:
	/** REPLICATED. The mouth. Set once at placement and never moved. */
	UPROPERTY(Replicated)
	FVector_NetQuantize MouthLocation = FVector::ZeroVector;

	UPROPERTY(Replicated)
	float RadiusUU = 0.f;

	/** REPLICATED. Absolute match-clock deadline. See the cooldown contract on the ability component. */
	UPROPERTY(Replicated)
	float ExpireMatchTime = 0.f;

	/**
	 * REPLICATED. The other mouth, or null while this gate is still alone.
	 *
	 * Replicated because a client has to be able to draw a PAIRED gate differently from a lone one —
	 * a mouth that will move you and a mouth that will not must not look the same.
	 */
	UPROPERTY(Replicated)
	TObjectPtr<ATraceElleGate> Partner = nullptr;

	/** REPLICATED. Elle's PlayerState, so the choke point has an instigator even after she dies. */
	UPROPERTY(Replicated)
	TObjectPtr<APlayerState> SourcePlayerState = nullptr;

	/** REPLICATED. Elle's team at placement. See InitialiseGate. */
	UPROPERTY(Replicated)
	ETraceTeam SourceTeam = ETraceTeam::None;

	/** REPLICATED. Cosmetic: which of the two colours this mouth wears. */
	UPROPERTY(Replicated)
	bool bSecondOfPair = false;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Root = nullptr;

	/** The rings. One component, one material instance — the colour is per GATE, not per ring. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> RingMesh = nullptr;

private:
	/** SERVER ONLY. Expiry, then the step-through poll. */
	void ServerTickGate();

	/** SERVER ONLY. Moves @p Candidate to the partner's mouth. Assumes every rule has already passed. */
	void CommitTeleport(ATraceCharacter* Candidate);

	/** True when @p Candidate is inside the per-player re-entry lockout on THIS gate. */
	bool IsLockedOut(const ATraceCharacter* Candidate) const;

	/** Applies the lockout to @p Candidate on this gate AND on the partner. See PairWith. */
	void ApplyLockout(ATraceCharacter* Candidate);

	/** Drops lockout entries whose pawn has gone, so the map cannot grow across a long pair. */
	void PruneLockouts();

	/** Marks @p Candidate as already standing in this mouth, so only a fresh ENTRY can fire it. */
	void MarkAlreadyInside(ATraceCharacter* Candidate);

	/** Cosmetic half. Builds the rings once the replicated mouth has arrived. */
	void BuildRingsIfNeeded();

	/** One ring of beads, centred on @p Center, lying in the horizontal plane. */
	void AddRing(const FVector& Center, float Radius) const;

	/** The match clock. Identical to the ability component's. */
	float MatchTimeNow() const;

	/**
	 * Who may not be taken again yet, and until when (absolute match time).
	 *
	 * A BACKSTOP SINCE DEMO 17, NOT THE MECHANISM. The rule that keeps the pair from being a trap is
	 * now the step-in EDGE below (InsideLastLook): an arrival is not an entry, so the far mouth does
	 * not send you back at all. This still covers the fast in-out-in case, where a player really did
	 * leave and really did step in again inside a second.
	 */
	TMap<TWeakObjectPtr<ATraceCharacter>, float> LockoutUntilMatchTime;

	/**
	 * DEMO 17 item 1. Who was standing in this mouth when it was last looked at.
	 *
	 * *** A GATE TAKES YOU WHEN YOU STEP IN, NOT WHILE YOU STAND IN IT. *** Entry used to be a pure
	 * proximity poll with a 1 s lockout in front of it, which meant a player who simply stood still
	 * near one of the mouths was thrown to the other one EVERY SECOND for the pair's whole 8 s life —
	 * measured at seven teleports in seven seconds on a stationary Elle standing in the mouth she had
	 * just placed. That is not a portal doing nothing, it is a portal doing something awful, and it
	 * reads to a player exactly as "the ability doesn't work".
	 *
	 * So the rule is an EDGE: outside on the previous look, inside now. Arriving through the partner
	 * seeds the destination's set (you did not step into the far mouth, you were put there), and
	 * PairWith seeds both, because Elle is by construction stood in the second mouth as she places it.
	 * The lockout above survives as a backstop for a player who sprints out and straight back in.
	 */
	TSet<TWeakObjectPtr<ATraceCharacter>> InsideLastLook;

	bool bRingsBuilt = false;

	/** Colour the rings were last built with, so a pairing can recolour without a rebuild. */
	bool bBuiltAsPaired = false;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> BeadMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> NeonMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FallbackMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RingMID = nullptr;
};
