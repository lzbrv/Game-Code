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

	/** The player who threw it, for the choke point and the kill feed. May be null later. */
	UTraceAbilityComponent* GetSourceComponent() const { return SourceComponent.Get(); }

	/**
	 * SERVER ONLY. Breaks the jar right now: fires the poison burst and destroys the actor.
	 * The one place a break happens; the touch test, the jar-jump and the harness all call it.
	 */
	void ServerBreakNow(const TCHAR* Why);

	/** SERVER ONLY. Forces an airborne Pickler jar to land here and now. For the harness. */
	void ServerForceLandNow();

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
