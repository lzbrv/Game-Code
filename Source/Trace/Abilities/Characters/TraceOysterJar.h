// Trace — Oyster's jar (spec v14 §6). Both kinds of jar are this one actor.
//
// ===================================================================================================
// THE DOC'S OWN CLARIFICATION IS THE DESIGN OF THIS CLASS
// ===================================================================================================
//
// Passive:   "leaves a poison jar at the start of every dash, including while carrying the Core. An
//             enemy touching a jar breaks it, poisoning nearby enemies... Jars last 4 s on the
//             ground. Max 3; a fourth despawns the oldest."
//
// Activated: "Pickler: lobs a jar that, ON LANDING, deals 30 damage in an area and pulls enemies
//             within a small radius toward it."
//
//             "The jar does not explode upon landing, IT IS THE SAME AS HIS OTHER JARS, which stay
//              behind for a short period until broken."
//
// So Pickler is not a grenade with a jar-shaped model. It is a NORMAL JAR that happens to fire an
// impact effect the moment it lands, and then goes on being a normal jar — same 4 s, same break on
// touch, same poison burst, and it counts against the same max of 3. One actor with a bIsPickler
// flag is therefore not a shortcut, it is the doc's model: the landing effect is a one-shot event on
// the way to becoming an ordinary jar, and it fires exactly once.
//
// ===================================================================================================
// *** SPEC v26 §6b OVERTURNS THE CLARIFICATION ABOVE, FOR THE PICKLER JAR AND ONLY THE PICKLER JAR ***
// ===================================================================================================
//
//   "The E jar's should explode once the pull animation finishes, rather than waiting for a jump to
//    trigger them."
//
// The older sentence — "the jar does not explode upon landing, it is the same as his other jars" — is
// left standing above rather than deleted, because it is still the reason THIS CLASS is one actor with
// a flag instead of two classes, and because a reader who finds the v14 doc needs to know it was
// considered and superseded. What changed is only how a Pickler jar ENDS: it now lands, fires its
// impact, waits out the pull (GetPicklerDetonateDelaySeconds — derived from the pull, not typed), and
// then breaks itself, which fires the same Burst() an enemy's touch would have. It is still a normal
// jar for that window: touchable, breakable, jumpable, counted against the max of 3.
//
// HIS DASH JARS ARE COMPLETELY UNTOUCHED. They still lie there for their 4 s waiting for an enemy or a
// jump. §6b says "the E jar", singular, and the passive jars are the whole of his passive.
//
// ===================================================================================================
// EVERY EFFECT IN THIS FILE GOES THROUGH THE CHOKE POINT
// ===================================================================================================
//
// Spec §4 names "Oyster's poison ticks" and "Pickler's 30 area damage" as two of the fifteen new ways
// to break the founding invariant, and Pickler's pull and poison's slow are two more that §4's
// [ASSUMPTION] covers. There are exactly four places in this file where a player is touched, and all
// four ask UTraceAbilityComponent first:
//
//   FireLandingEffect()  Pickler's 30      -> ApplyAbilityDamage (Damage)   -> never a carrier, no knob
//   FireLandingEffect()  Pickler's pull    -> CanAffectTarget (Control)     -> never a carrier, knobbed
//   Burst()              poison attachment -> CanAffectTarget (both)        -> never a carrier
//   (and the poison's own per-tick re-check, in UTraceOysterPoisonComponent)
//
// STILL FOUR, after spec v16 §3. Burst() now also spawns ATraceOysterPoisonCloud, and that is NOT a
// fifth vector: the cloud is a replicated, collisionless, purely cosmetic marker of the volume the
// burst covered. It reads no player, touches no player, and no rule anywhere reads it — so it is
// deliberately NOT behind the choke point, because there is nobody to ask about. It is sized from the
// same Radius the loop above it uses, which is the only contract it has.
//
// COLLISION: the jar blocks WORLD STATIC ONLY, and ignores everything else. It has to block geometry
// or the lob could not land; it must ignore pawns or it would become a movement base, block a
// bullet, and let a player push a live jar around. "Touching" a jar is a radius test in Tick, not a
// physics overlap, so a jar in the floor is still touchable and a jar cannot be body-blocked.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"    // ETraceTeam

#include "TraceOysterJar.generated.h"

class ATraceCharacter;
class USphereComponent;
class UStaticMeshComponent;
class UTraceAbilityComponent;

namespace TraceOysterJar
{
	/**
	 * SPEC v19 §4.4 TEST ARM. True while Trace.Oyster.LegacyThrow is 1, which restores every part of
	 * the pre-v19 Pickler throw — the raw-muzzle release, the one-Euler-step-per-frame flight, and the
	 * jar cap being allowed to delete a jar that is still in the air. It exists so
	 * Trace.Oyster.PicklerThrowTest can be shown FAILING in the same process it later passes in.
	 * NEVER SHIP 1.
	 */
	TRACE_API bool IsLegacyThrow();

	/**
	 * SPEC v26 §6 TEST ARM. True while Trace.Oyster.LegacyE is 1, which restores BOTH of the
	 * pre-v26 behaviours §6 replaces: the Pickler jar gets no fuse and lies there waiting to be
	 * broken, and poisoning an enemy refunds nothing. It exists so Trace.Oyster.ETest can be shown
	 * FAILING in the same process, in the same match, on the same fixtures it later passes on — a
	 * green that never saw the old behaviour is not evidence. NEVER SHIP 1.
	 *
	 * Read by ATraceOysterJar::Land (the fuse) and by TraceOysterPoisonFile::RefundPicklerForPoisoning
	 * (the refund). Those are the only two places §6 changed anything, which is why one switch covers
	 * both halves of it.
	 */
	TRACE_API bool IsLegacyE();
}

UCLASS()
class TRACE_API ATraceOysterJar : public AActor
{
	GENERATED_BODY()

public:
	ATraceOysterJar();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * SERVER ONLY. @p InVelocity zero means the jar is placed on the ground already (the dash jar);
	 * anything else lobs it (Pickler) and the landing effect fires when it stops.
	 */
	void Initialise(UTraceAbilityComponent* InSourceComp, ETraceTeam InOwnerTeam, bool bInIsPickler,
	                const FVector& InVelocity);

	/** True for the jar Pickler threw — the only kind with a landing effect. */
	bool IsPickler() const { return bIsPickler; }

	/** True once it is lying on the ground and can be broken. */
	bool IsGrounded() const { return bGrounded; }

	/** Absolute match time this jar despawns untouched. 0 while still in the air. */
	float GetExpiryMatchTime() const { return ExpiryMatchTime; }

	/** The sphere the flight is swept with, and the radius a release point has to be clear by. */
	static float GetJarCollisionRadiusUU() { return 18.f; }

	/**
	 * SPEC v26 §6b — HOW LONG "UNTIL THE PULL ANIMATION FINISHES" IS, IN SECONDS.
	 *
	 * DERIVED, never typed. The pull is the thing being waited for, so the pull is the base: an enemy
	 * caught at the very edge of OysterPicklerPullRadiusUU and launched at OysterPicklerPullSpeed
	 * covers that gap in radius/speed seconds — 380/1300 = 0.29 s at the shipped values — and that is
	 * the moment the yank is over. OysterPicklerDetonateDelayScale then scales it (1.0 = detonate as
	 * the last of them arrives). Retune the pull and this moves with it, which a hard-coded 0.29 would
	 * not; that is Demo 21's standing rule applied to a duration rather than to an offset.
	 *
	 * CLAMPED at both ends, and both clamps are relative too:
	 *   floor  one tick of the jar's own tick group, so the jar is always SEEN to land before it goes;
	 *   ceiling OysterJarLifetimeSeconds, so no scale can produce a Pickler jar that quietly expires
	 *           instead of exploding — the failure this whole section exists to remove.
	 */
	static float GetPicklerDetonateDelaySeconds();

	/**
	 * SPEC v19 §4.4 — WHERE A LOBBED JAR MAY BE LET GO OF.
	 *
	 * @p DesiredRelease is the muzzle: 22 uu along the aim ray from the eye, and the jar's own sphere is
	 * 18 uu on top of that. A WALL can never swallow it — the 34 uu capsule radius keeps every vertical
	 * face at least 12 uu beyond the muzzle's furthest reach — but a CEILING can, and easily: the eye
	 * sits 64 uu above the capsule centre and the capsule is 88 uu tall, so a roof may be 24 uu above
	 * the eye while he stands comfortably under it. Aim up and the muzzle goes 22 of those 24. A swept
	 * move that starts penetrating is refused at Time 0, so the jar was BORN inside that roof and came
	 * to rest buried in it, invisible.
	 *
	 * HONEST SIZE OF THIS: it moves where the jar ends up, not how far it flies — the lob is stopped by
	 * the ceiling either way. The frame-rate and jar-cap fixes are the ones that carry "more consistent
	 * to throw"; Trace.Oyster.PicklerThrowTest measures all three and says which is which.
	 *
	 * Sweeps the jar's own sphere from @p InsideThrower — the eye, which is inside a capsule that is by
	 * definition standing in clear space — out to @p DesiredRelease, and returns the last point on that
	 * segment the jar fits in. Never returns a point the jar is embedded at, and never returns a point
	 * behind the thrower's eye.
	 */
	static FVector ResolveReleaseLocation(const UWorld* WorldPtr, const FVector& InsideThrower,
	                                      const FVector& DesiredRelease, const AActor* IgnoreActor);

	// --- measurement, for Trace.Oyster.PicklerThrowTest ------------------------------------------------

	/** Where the lob was let go of, and at what velocity. Both zero for a dash jar. */
	FVector GetLaunchLocation() const { return LaunchLocation; }
	FVector GetLaunchVelocity() const { return LaunchVelocity; }

	/** True once Pickler's impact has fired. Always false on a dash jar. */
	bool HasFiredLandingEffect() const { return bLandingEffectFired; }

	/** Absolute match time it came to rest. 0 while it is still in the air. */
	float GetLandedMatchTime() const { return LandedMatchTime; }

	/**
	 * SPEC v26 §6b. Absolute match time this jar detonates on its own. 0 on a dash jar and on a
	 * Pickler jar that has not landed yet — a dash jar has no fuse at all and still waits to be broken.
	 */
	float GetDetonateMatchTime() const { return DetonateMatchTime; }

	/** How many swept steps the flight took. The frame-rate independence measurement reads it. */
	int32 GetFlightSweepCount() const { return FlightSweepCount; }

	/** True when the release point had to be pulled back out of geometry. */
	bool WasReleaseClamped() const { return bReleaseClamped; }

	/** The player who threw it, for the choke point and the kill feed. May be null later. */
	UTraceAbilityComponent* GetSourceComponent() const { return SourceComponent.Get(); }

	/**
	 * SERVER ONLY. Breaks the jar right now: fires the poison burst and destroys the actor.
	 * The one place a break happens; the touch test, the jar-jump and the harness all call it.
	 */
	void ServerBreakNow(const TCHAR* Why);

	/** SERVER ONLY. Forces an airborne Pickler jar to land here and now. For the harness. */
	void ServerForceLandNow();

	/** SERVER ONLY. Records that this jar was released from a clamped point. Called by the thrower. */
	void NoteReleaseWasClamped() { bReleaseClamped = true; }

protected:
	virtual void BeginPlay() override;

	/** REPLICATED. Cosmetic only on clients; every rule runs on the server. */
	UPROPERTY(Replicated)
	bool bIsPickler = false;

	UPROPERTY(Replicated)
	bool bGrounded = false;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Oyster")
	TObjectPtr<UStaticMeshComponent> Mesh = nullptr;

	/** Blocks WORLD STATIC ONLY — see the header. It is what makes the lob land, and nothing else. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Oyster")
	TObjectPtr<USphereComponent> Collision = nullptr;

private:
	TWeakObjectPtr<UTraceAbilityComponent> SourceComponent;

	ETraceTeam OwnerTeam = ETraceTeam::None;

	/** Flight velocity while airborne. Integrated by hand against world gravity — see TickFlight. */
	FVector FlightVelocity = FVector::ZeroVector;

	/** Absolute match time. 0 until it lands, because "jars last 4 s ON THE GROUND". */
	float ExpiryMatchTime = 0.f;

	/** A landing effect fires exactly once, however the jar came to rest. */
	bool bLandingEffectFired = false;

	/**
	 * SPEC v26 §6b. Absolute match time the Pickler jar blows itself up, 0 when it has no fuse.
	 *
	 * Stored as LandedMatchTime + GetPicklerDetonateDelaySeconds() at the moment of landing, i.e. as a
	 * deadline on the same match clock every other timer in this actor uses (ExpiryMatchTime,
	 * LandedMatchTime). A countdown here would drift against a clock the server is already smoothing.
	 */
	float DetonateMatchTime = 0.f;

	/** MEASUREMENT ONLY. The state a throw was launched with, and what the flight cost. */
	FVector LaunchLocation = FVector::ZeroVector;
	FVector LaunchVelocity = FVector::ZeroVector;
	float   LandedMatchTime = 0.f;
	int32   FlightSweepCount = 0;
	bool    bReleaseClamped = false;

	float MatchTimeNow() const;

	void TickFlight(float DeltaSeconds);
	void TickGrounded();

	/** Server. Lands the jar at its current position and fires Pickler's impact if this is one. */
	void Land();

	/** Server. Pickler's "30 damage in an area and pulls enemies within a small radius toward it". */
	void FireLandingEffect();

	/** Server. The poison burst a break produces, and (spec v16 §3) the cloud that announces it. */
	void Burst();

	/** Server. The touch test: is a living enemy inside the break radius. */
	ATraceCharacter* FindToucher() const;

	/** True when @p Candidate is an enemy of the jar's owner — the only players who break a jar. */
	bool IsEnemyOfOwner(const ATraceCharacter* Candidate) const;
};
