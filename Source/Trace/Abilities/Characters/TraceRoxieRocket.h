// Trace — ROXIE's rocket (spec v18 §2).
//
// Verbatim: "fires a rocket that launches her backwards, fast and far. The rocket deals 100 damage on
// impact, ANYWHERE ON THE BODY — no headshot/body distinction. It WOBBLES in flight, deliberately
// inaccurate and hard to aim. 35 s cooldown."
//
// ===================================================================================================
// THIS ACTOR IS THE FLIGHT AND THE VISUAL. THE DAMAGE RULE IS ROXIE'S.
// ===================================================================================================
//
// Same division of labour ATraceMaceSpike states and for the same reason: on a body hit this actor
// calls UTraceAbilitySetRoxie::ApplyRocketDamageTo() and does not touch health, a health component or
// a carrier test of its own. That is not tidiness — spec v18 §2 calls the flat 100 "the most dangerous
// number added to this game", and the founding invariant (NO ABILITY MAY DAMAGE A CORE CARRIER) is
// enforced in exactly one function, UTraceAbilityComponent::CanAffectTarget. One damage call site in
// Roxie's file is one thing to audit; a second one here would be the second.
//
// The SELF-LAUNCH is not here either. "Launches her backwards" writes ROXIE's velocity, so it lives
// with Roxie exactly as Mace's pull lives with Mace.
//
// ===================================================================================================
// THE PATH IS ARITHMETIC, NOT SIMULATION, AND THAT IS WHAT MAKES THE WOBBLE HONEST
// ===================================================================================================
//
// TraceRoxieRocket::GetPositionAtTime() is a pure function of (launch origin, launch direction, launch
// instant, and three knobs). Every machine calls it with the same arguments off the MATCH clock —
// AGameStateBase::GetServerWorldTimeSeconds, the one clock a server and a lagged client agree on — so:
//
//   * the rocket a player SEES is the rocket the server's hit test used. This is X's bee-orbit
//     argument (Abilities/Characters/TraceAbilitySetX.h) applied to a projectile, and it is the whole
//     reason the wobble can be a gameplay feature rather than a lie: a drawn path that differed from
//     the lethal path would be exactly the "the lethal volume is not the drawn volume" defect this
//     project has already spent a pass fixing on the ribbon;
//   * nothing per-frame is replicated. Four values go out once, at spawn;
//   * the wobble is testable WITHOUT A WORLD. Trace.Roxie.RocketFlightTest evaluates this function
//     directly, which is what lets "deliberately inaccurate" be a measured deviation in uu rather than
//     an adjective. Amplitude 0 is its RED ARM: a straight, easily-aimed 100-damage projectile.
//
// THE SEED IS WHY IT IS HARD TO AIM. Without one, every rocket would wobble through the identical
// sine and a player would simply learn to lead it — the ability would be exactly as accurate as a
// straight rocket after a week. The server rolls one per shot and replicates it, so the arc is
// deterministic on every machine and unpredictable to the shooter.
//
// COLLISION: NONE, on any component. The rocket is decoration around a moving point, and its hits are
// resolved by an explicit server-side sweep (see TickFlightAuthority). A collider here would block
// bullets, break an Oyster jar, catch the Core and become a movement base — four bugs for no gain, and
// it is the same call ATraceMaceSpike and ATraceBeeSwarm both made.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"      // FVector_NetQuantize / FVector_NetQuantizeNormal
#include "GameFramework/Actor.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceRoxieRocket.generated.h"

class ATraceCharacter;
class UStaticMeshComponent;
class UTraceAbilitySetRoxie;

/**
 * The rocket, as pure arithmetic and as resolved knob reads.
 *
 * Named after its file (TraceRoxieRocket.cpp) rather than left anonymous: an unnamed namespace in a
 * unity build collides with every other unnamed namespace compiled into the same translation unit,
 * which on this project is a Windows-only failure macOS cannot see.
 *
 * Every accessor below reads UTraceSettings and CLAMPS. Nothing may read those properties directly —
 * the .ini layers over the header defaults on this project, so a caller that dereferences the property
 * gets whichever of the two happened to win, unclamped. Same contract as TraceXBees and TraceAmmo.
 */
namespace TraceRoxieRocket
{
	/** §2's flat 100. "Anywhere on the body" — no hit-zone lookup anywhere in this feature. */
	TRACE_API float GetDamage();

	/** uu/s along the aim line. Deliberately slow enough to be seen and dodged; see the knob. */
	TRACE_API float GetSpeedUU();

	/** Seconds before the rocket gives up. Speed x this is the effective range. */
	TRACE_API float GetLifetimeSeconds();

	/** The rocket's OWN touch radius, added to the victim's capsule radius. NOT a splash radius. */
	TRACE_API float GetHitRadiusUU();

	/**
	 * DEMO 17 item 3. Multiplier on the DRAWN body, whose base size is the hit radius above.
	 *
	 * 1.0 means "draw it exactly as big as the thing that kills you", which is the honest default and a
	 * seven-fold widening of what shipped before Demo 17 (a 13 uu dart around a 45 uu lethal radius).
	 */
	TRACE_API float GetVisualScale();

	/** Widest lateral excursion from the aim line, in uu. ZERO IS THE RED ARM — it flies straight. */
	TRACE_API float GetWobbleAmplitudeUU();

	/** Wobbles per second. With the amplitude, these two ARE "deliberately inaccurate". */
	TRACE_API float GetWobbleFrequencyHz();

	/** uu/s Roxie is thrown, opposite her aim. Applied to HER, by UTraceAbilitySetRoxie. */
	TRACE_API float GetSelfLaunchImpulse();

	/** Fraction of that impulse sent straight up, so "far" has air time to happen in. */
	TRACE_API float GetSelfLaunchUpBias();

	/** §2: 35 s. The V ability's own cooldown, separate from MODDED's 25 s on E. */
	TRACE_API float GetCooldownSeconds();

	/**
	 * The death cause a rocket kill carries, "RoxieRocket".
	 *
	 * ONE DEFINITION, because there are now two readers and they are in different slices: Roxie's own
	 * DealDamage() call passes it, and UI/TraceKillFeed.cpp matches on it to draw the right glyph.
	 * As two string literals a rename would silently drop the feed back to a plain BULLET round — the
	 * kill would still be reported, with the wrong verb, which is the class of bug nothing catches.
	 * Same shape and same reason as TraceMelee::GetKnifeKillCause().
	 */
	TRACE_API FName GetKillCause();

	/**
	 * The two axes the wobble swings in, orthonormal and perpendicular to @p Direction.
	 *
	 * Deterministic — a pure function of the direction — so the server's hit test and every client's
	 * visual build the identical frame. Falls back to a second cross product when the direction is
	 * vertical and the first one degenerates.
	 */
	TRACE_API void BuildWobbleBasis(const FVector& Direction, FVector& OutRightAxis, FVector& OutUpAxis);

	/**
	 * The lateral excursion alone, with the forward travel removed.
	 *
	 * EXACTLY ZERO AT t = 0, by construction (both terms are sines of a phase that starts at 0), so
	 * the rocket leaves the muzzle ON the crosshair and wanders off it — rather than spawning already
	 * displaced, which reads as the gun being misaligned rather than as the rocket being wild.
	 *
	 * Two incommensurate frequencies, not one: a single sine returns to the aim line every period, so
	 * a player would only have to learn the period to fire "on the beat" and hit dead straight. The
	 * second term at 0.61x makes the path a Lissajous wander that does not repeat inside any lifetime
	 * this knob can be set to.
	 *
	 * @param WobbleSeedTurns  0..1, the roll angle of the wobble plane. Rolled per shot on the server
	 *                         and replicated. This is what stops the arc from being learnable.
	 */
	TRACE_API FVector GetWobbleOffsetAtTime(const FVector& Direction, float SecondsSinceLaunch,
	                                        float WobbleAmplitudeUU, float WobbleFrequencyHz,
	                                        float WobbleSeedTurns);

	/**
	 * WHERE THE ROCKET IS, @p SecondsSinceLaunch after it left the muzzle. THE one path function.
	 *
	 * Three callers, deliberately: the server's per-frame hit sweep, every machine's visual, and
	 * Trace.Roxie.RocketFlightTest. Everything it needs is an argument so the three cannot drift.
	 */
	TRACE_API FVector GetPositionAtTime(const FVector& Origin, const FVector& Direction,
	                                    float SecondsSinceLaunch, float SpeedUU,
	                                    float WobbleAmplitudeUU, float WobbleFrequencyHz,
	                                    float WobbleSeedTurns);
}

/**
 * One rocket in flight. Spawned by the server, drawn by everybody, destroyed on the first thing it
 * touches.
 *
 * REPLICATES FOUR VALUES, ONCE, AT SPAWN — origin, direction, launch instant on the match clock, and
 * the wobble seed. There is no per-frame movement replication (SetReplicateMovement(false)) because
 * every machine derives the position from those four and the match clock; see the file header.
 */
UCLASS(NotPlaceable)
class TRACE_API ATraceRoxieRocket : public AActor
{
	GENERATED_BODY()

public:
	ATraceRoxieRocket();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * SERVER ONLY. Call immediately after SpawnActor, before the first replication.
	 *
	 * @param InOwnerSet        Roxie. Weakly held: if she swaps character or leaves mid-flight the
	 *                          rocket fizzles rather than dealing damage on behalf of nobody.
	 * @param InOrigin          the muzzle.
	 * @param InDirection       her aim, normalised.
	 * @param InLaunchMatchTime AGameStateBase::GetServerWorldTimeSeconds at the shot.
	 * @param InWobbleSeedTurns 0..1. Roll the seed at the CALL SITE, not here, so a fixture can pin it.
	 */
	void InitialiseFlight(UTraceAbilitySetRoxie* InOwnerSet, const FVector& InOrigin,
	                      const FVector& InDirection, float InLaunchMatchTime, float InWobbleSeedTurns);

	/** Where this rocket is right now, on any machine. The same answer the hit sweep used. */
	UFUNCTION(BlueprintPure, Category = "Trace|Roxie")
	FVector GetCurrentPosition() const;

	/** Seconds this rocket has been flying, on the match clock. Negative before the clock is up. */
	float GetSecondsInFlight() const;

	/** The launch parameters, for the harness and for anything that wants to re-derive the path. */
	FVector GetLaunchOrigin() const { return LaunchOrigin; }
	FVector GetLaunchDirection() const { return LaunchDirection; }
	float   GetWobbleSeedTurns() const { return WobbleSeedTurns; }

protected:
	virtual void BeginPlay() override;

	/** REPLICATED, written once. The muzzle the rocket left from. */
	UPROPERTY(Replicated)
	FVector_NetQuantize LaunchOrigin = FVector::ZeroVector;

	/** REPLICATED, written once. Roxie's aim at the shot, normalised. */
	UPROPERTY(Replicated)
	FVector_NetQuantizeNormal LaunchDirection = FVector::ForwardVector;

	/** REPLICATED, written once. The MATCH clock instant of the shot — never a local world time. */
	UPROPERTY(Replicated)
	float LaunchMatchTime = 0.f;

	/** REPLICATED, written once. 0..1, the roll of the wobble plane. See the file header. */
	UPROPERTY(Replicated)
	float WobbleSeedTurns = 0.f;

	/** The visible rocket. NO COLLISION — see the file header. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Roxie")
	TObjectPtr<UStaticMeshComponent> Body = nullptr;

private:
	/** SERVER ONLY. Sweeps the segment covered since the last tick and resolves the first thing hit. */
	void TickFlightAuthority(const FVector& FromPosition, const FVector& ToPosition);

	/** Places the mesh and points it along the instantaneous direction of travel. Every machine. */
	void UpdateVisual(const FVector& AtPosition);

	/**
	 * DEMO 17 item 3. Sizes the drawn body from the rocket's own HIT RADIUS, times RoxieRocketVisualScale.
	 *
	 * Runs in BeginPlay rather than the constructor because both numbers are live settings knobs and a
	 * CDO built before the .ini layers over the header would bake the wrong pair in for the whole
	 * process — so a retune during PIE reaches the next rocket.
	 */
	void ApplyVisualSize();

	/** Roxie. The rules — the damage and the choke point — are hers; see the file header. */
	TWeakObjectPtr<UTraceAbilitySetRoxie> OwnerSet;

	/**
	 * Roxie's PAWN at the shot, ignored by the world sweep and skipped by the body test.
	 *
	 * A separate weak pointer rather than "ask OwnerSet for its character", because she may die
	 * mid-flight and respawn: the rocket must keep ignoring the pawn it was fired FROM (it starts
	 * inside her capsule), not whatever pawn she happens to own three seconds later.
	 */
	TWeakObjectPtr<ATraceCharacter> ShooterPawn;

	/** Server-side: where the last hit sweep ended, so the next one is continuous with it. */
	FVector LastSweptPosition = FVector::ZeroVector;
	bool bHasSwept = false;

	/**
	 * Local world time the actor spawned, for the BACKSTOP only.
	 *
	 * The real expiry is the match-clock lifetime, which every machine agrees on. This is the second
	 * belt: a rocket whose shooter disconnected, or that spawned before the GameState replicated a
	 * usable clock, must not be able to outlive the match. ATraceMaceSpike carries the same backstop.
	 */
	float SpawnWorldTime = 0.f;
};
