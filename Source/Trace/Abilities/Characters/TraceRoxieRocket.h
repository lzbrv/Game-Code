// Trace — ROXIE's rocket (spec v18 §2).
//
// Verbatim: "fires a rocket that launches her backwards, fast and far. The rocket deals 100 damage on
// impact, ANYWHERE ON THE BODY — no headshot/body distinction. It WOBBLES in flight, deliberately
// inaccurate and hard to aim. 35 s cooldown."
//
// DEMO 29 ITEM 7, verbatim, AMENDS THE 100 AND THE SIZE: "Make Roxie's rocket hit radius match the
// model, but it should only do 100 damage for direct impacts. Otherwise, the damage should fall off."
//
//   THE SIZE   there is now ONE radius. GetHitRadiusUU() is the touch radius, the drawn body radius,
//              the world-sweep radius and the blast radius, and GetVisualRadiusUU() returns it
//              unchanged. The x1.6 drawn-size multiplier Patch 28 added is unwired, not deleted.
//   THE 100    is now the DIRECT-IMPACT number: the flight sweep found the victim's capsule within the
//              touch radius of the path, i.e. the projectile hit them. That is the only path that
//              deals it, and it is still flat and still has no hit zones.
//   THE REST   is GetSplashDamageAtGapUU(): every pawn the DETONATION catches takes a smoothstep
//              falloff from half the direct damage at the impact point down to zero at the edge of the
//              blast — which is the same radius, so the RocketBurst you see is the blast that hurt.
//
// ===================================================================================================
// THIS ACTOR IS THE FLIGHT AND THE VISUAL. THE DAMAGE RULE IS ROXIE'S.
// ===================================================================================================
//
// Same division of labour ATraceMaceSpike states and for the same reason: this actor calls
// UTraceAbilitySetRoxie::ApplyRocketDamageTo() and does not touch health, a health component or a
// carrier test of its own. That is not tidiness — spec v18 §2 calls the flat 100 "the most dangerous
// number added to this game", and the founding invariant (NO ABILITY MAY DAMAGE A CORE CARRIER) is
// enforced in exactly one function, UTraceAbilityComponent::CanAffectTarget. One damage call site in
// Roxie's file is one thing to audit; a second one here would be the second.
//
// DEMO 29's FALLOFF DID NOT ADD A SECOND ONE. The splash loop (ApplySplashDamage) decides HOW MUCH
// with arithmetic and then asks that same single function WHETHER, once per victim — so the carrier
// rule, the friendly-fire rule and the self rule apply to a blast exactly as they applied to a direct
// hit, and there is still one place in this feature where health can move.
//
// *** THAT IS ALSO THE WHOLE ANSWER TO ROCKET-JUMP SELF-DAMAGE, AND IT IS STRUCTURAL. ***
// ApplySplashDamage does NOT skip the shooter — she is offered to the choke point like anybody else,
// and CanAffectTargetDetailed returns ETraceAbilityBlockReason::Self for a pawn damaging itself with
// its own ability, unconditionally and with no knob. So Roxie's own rocket deals her 0 at every range
// including point blank, her rocket jump costs exactly what it cost in Demo 28 (nothing), and it is
// not possible for a retune of the falloff to make it lethal. A knob for self-damage would be a knob
// that moves nothing, which this project calls a dead knob and refuses to ship.
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

#include "Gameplay/TraceFxShapes.h"       // ETraceFxBlend — stored per piece, so it must be complete

#include "TraceRoxieRocket.generated.h"

class ATraceCharacter;
class UAudioComponent;
class UMaterialInstanceDynamic;
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
	/**
	 * §2's 100, WHICH IS NOW THE DIRECT-IMPACT NUMBER RATHER THAN THE ONLY NUMBER.
	 *
	 * DEMO 29 ITEM 7, verbatim: "it should only do 100 damage for direct impacts. Otherwise, the
	 * damage should fall off." So this is the ceiling and the reward for actually hitting somebody;
	 * everything the detonation catches nearby is GetSplashDamageAtGapUU() of it. "Anywhere on the
	 * body" still holds — there is no hit-zone lookup anywhere in this feature, on either path.
	 */
	TRACE_API float GetDamage();

	/** uu/s along the aim line. Deliberately slow enough to be seen and dodged; see the knob. */
	TRACE_API float GetSpeedUU();

	/** Seconds before the rocket gives up. Speed x this is the effective range. */
	TRACE_API float GetLifetimeSeconds();

	/**
	 * *** THE ONE RADIUS. DEMO 29 ITEM 7: "make Roxie's rocket hit radius match the model". ***
	 *
	 * This single number is now, simultaneously and by construction rather than by anyone keeping four
	 * values in step:
	 *
	 *   1. the rocket's own TOUCH radius, added to the victim's capsule radius by the flight sweep —
	 *      i.e. how close the path has to come to a pawn to be a DIRECT IMPACT;
	 *   2. the DRAWN body's radius. GetVisualRadiusUU() returns this and nothing else, so the model a
	 *      player watches is exactly the projectile that hits them (ApplyVisualSize);
	 *   3. the radius of the world sweep that stops the rocket on geometry, so the drawn nose and the
	 *      thing that touches the wall are the same size;
	 *   4. the BLAST radius of the detonation's falloff damage (GetSplashDamageAtGapUU), which is also
	 *      exactly the radius ATraceFxBurst draws the RocketBurst at — it reads this same knob live
	 *      (ATraceFxBurst::DefaultRadiusUUFor). The explosion you see IS the explosion that hurts.
	 *
	 * BEFORE DEMO 29 there were two numbers: this one at 45 uu and a separate x1.6 drawn-size
	 * multiplier that put a 72 uu model around it. That is precisely the drift class this project has
	 * paid for twice (the ribbon's lethal-vs-drawn volume, and Chut's copied backstab number), and the
	 * owner found it by eye: "make the hit radius match the model". The model won — 72 uu — and the
	 * multiplier is gone from the reading path (see RoxieRocketVisualScale in TraceSettings.h, which is
	 * retained for config compatibility and has no reader).
	 */
	TRACE_API float GetHitRadiusUU();

	/**
	 * *** THE ONE NUMBER THE WHOLE ROCKET IS DRAWN FROM, AND SINCE DEMO 29 IT IS THE HIT RADIUS. ***
	 *
	 * Returns GetHitRadiusUU(), full stop. Not "the hit radius times something" — the same call, so
	 * the drawn body and the lethal body are ONE value and cannot be moved apart by editing one of
	 * them. That is Demo 29 item 7's actual ask ("make Roxie's rocket hit radius match the model")
	 * expressed as code rather than as two ini keys somebody has to remember to move together.
	 *
	 * IT USED TO BE GetHitRadiusUU() x RoxieRocketVisualScale. Demo 17 set the scale to 1.0 (draw the
	 * touch radius), Patch 28 raised it to 1.6 because the owner asked for a bigger model, and Demo 29
	 * is the owner reporting the consequence: a 72 uu model around a 45 uu hit radius, i.e. the picture
	 * was bigger than the projectile. The fix is not a smaller model — the owner liked the bigger one —
	 * it is one number at the model's size. RoxieRocketVisualScale still exists in TraceSettings.h for
	 * config compatibility and HAS NO READER; to put the multiplier back, restore the multiply here and
	 * the accessor beside GetHitRadiusUU(), and nothing else.
	 *
	 * This is still the unit every other piece of the rocket's presentation is expressed in — the launch
	 * flash, the three trail segments and the body's own length. Nothing in TraceRoxieRocket.cpp writes
	 * a bare size literal; every one is a named fraction of this. So "make it bigger" remains ONE edit
	 * (RoxieRocketHitRadiusUU) and now correctly makes the hit bigger too.
	 *
	 * The two CEILINGS that do not scale with it are the bible's, not the rocket's, and they are
	 * applied where they are read: the muzzle flash's 40 uu (§6.4) and the 8 uu emissive width floor
	 * (§3.4). A bigger rocket gets a bigger trail; it does not get a bigger muzzle ceiling.
	 */
	TRACE_API float GetVisualRadiusUU();

	/**
	 * DEMO 29 ITEM 7's FALLOFF, AS A PURE FUNCTION SO IT CAN BE TABULATED WITHOUT A WORLD.
	 *
	 * "It should only do 100 damage for direct impacts. Otherwise, the damage should fall off."
	 *
	 * A DIRECT IMPACT is the projectile ACTUALLY HITTING THE PAWN — the flight sweep found the pawn's
	 * capsule within GetHitRadiusUU() of the path and the rocket detonated ON them. That case never
	 * comes through this function; it is GetDamage(), flat, exactly as §2 wrote it.
	 *
	 * This function is the OTHERWISE: everybody else the detonation catches, scored on how far their
	 * CAPSULE SURFACE is from the point the rocket ended at.
	 *
	 *   gap 0                 GetDamage() x GetSplashMaxFraction()  (they were standing in it)
	 *   gap GetHitRadiusUU()  0                                     (the edge of the drawn burst)
	 *   beyond that           0, and they are not even iterated
	 *
	 * SMOOTHSTEP, NOT LINEAR, and the choice is deliberate. Its slope is zero at BOTH ends:
	 *   * at the outer edge that means the blast fades to nothing instead of stopping at a nonzero
	 *     value, so there is no boundary a player can be killed by 1 uu on either side of — which is
	 *     the complaint a linear falloff generates and the reason it gets replaced later;
	 *   * at the impact point it means the best near-miss pays a CONSISTENT amount rather than being
	 *     hyper-sensitive to a few uu. That matters here more than in most games, because this rocket
	 *     WOBBLES on purpose (§2, "deliberately inaccurate") — a linear ramp would punish the wobble a
	 *     second time, on damage, after already punishing it on aim.
	 * The steep middle is where the interesting gradient lives, which is where it belongs.
	 *
	 * @param SurfaceGapUU  uu from the detonation point to the nearest point on the victim's CAPSULE
	 *                      SURFACE (never to their actor origin, which is between their feet).
	 */
	TRACE_API float GetSplashDamageAtGapUU(float SurfaceGapUU);

	/**
	 * The fraction of GetDamage() a pawn takes standing exactly AT the detonation point.
	 *
	 * SHIPPED AT 0.5: "a direct hit kills you; the best possible near miss does exactly half." The
	 * ceiling is below 1.0 by design and by clamp — the whole of item 7 is that 100 is the reward for a
	 * direct impact, so no splash may ever equal one.
	 */
	TRACE_API float GetSplashMaxFraction();

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

	/**
	 * ONE LINE OF MEASUREMENTS TAKEN OFF THE LIVE COMPONENTS, for Trace.Roxie.RocketFxTest.
	 *
	 * *** READ BACK, NEVER RE-DERIVED. *** The body radius, the flash's current radius and the trail's
	 * three segment radii all come out of the components' world scales through UTraceFxShapes'
	 * inverse helpers — the same conversion that wrote them, run backwards. A verifier that recomputed
	 * them from TraceRoxieRocketFile would only be checking its own arithmetic.
	 */
	FString DebugDescribeFx() const;

	/** True while the launch flash exists — i.e. inside its 0.28 s, before it destroys itself. */
	bool IsLaunchFlashUp() const { return LaunchFlash != nullptr; }

	/**
	 * The DRAWN body's radius in uu, READ BACK off the live component's world scale — never re-derived
	 * from the knob.
	 *
	 * This is what makes "the hit radius matches the model" a measurement instead of a claim. Comparing
	 * TraceRoxieRocket::GetVisualRadiusUU() against GetHitRadiusUU() would compare a function with
	 * itself and pass on a build where ApplyVisualSize never ran; comparing THIS against GetHitRadiusUU()
	 * asks the mesh how big it actually is. Trace.Roxie.RocketShot prints both on the photographed frame.
	 */
	float GetDrawnBodyRadiusUU() const;

	/**
	 * SERVER, DEV FIXTURE ONLY. Ends this rocket AT @p Location, through the shipped
	 * DetonateAndDestroy — same burst, same splash, same choke point, same record.
	 *
	 * *** WHY THE HARNESS COMMANDS THE END POINT INSTEAD OF FLYING INTO ONE. ***
	 * Demo 29 §7's falloff is a function of the distance from the DETONATION POINT to a pawn, so a
	 * table of it needs that point known to the uu. A real flight cannot supply that: the rocket
	 * resolves its hits against the segment it swept THIS TICK, so where it stops is quantised by the
	 * frame rate (43 uu per tick at the shipped speed) and the gap that produced a given damage would
	 * be a different number on a different machine. Commanding the end point removes the only variable
	 * the test is not about, and changes nothing about the code under test — the whole damage path
	 * below this call is the shipped one, and the DIRECT-impact row of the same table still comes from
	 * a real rocket flown into a real pawn.
	 *
	 * @param Location where to end. Nothing checks it is reachable; that is the fixture's business.
	 */
	void DebugDetonateAt(const FVector& Location);

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

	/**
	 * FX_AUDIO_PLAN §2.3's LAUNCH FLASH: a muzzle-style cone at the spawn point, ember, growing
	 * 0.55x -> 3.2x over 0.28 s and then DESTROYED.
	 *
	 * *** IT IS DESTROYED RATHER THAN HIDDEN, AND THAT IS THE PRIMITIVE BUDGET TALKING. *** The
	 * bible allows four primitives per effect. A rocket in steady flight is Body plus three trail
	 * segments — exactly four — and the flash is a FIFTH for the first 0.28 s of a three-second
	 * flight. Hiding it would leave the rocket permanently at five; DestroyComponent() puts it back
	 * to four for 92% of every flight and costs one null check per tick.
	 *
	 * It does NOT move with the rocket: a muzzle flash belongs to the tube, so this stays at the
	 * launch origin (SetUsingAbsoluteLocation) while the rocket leaves it behind, which is what the
	 * tracer's own cone does and is the difference between a launch and a permanent nose glow.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> LaunchFlash = nullptr;

	/**
	 * §2.3's TRAIL: three stacked cylinders behind the rocket, tapering 9 uu -> 4 uu over 220 uu,
	 * additive ember at intensity 0.5 with a 30 Hz flicker between 0.4 and 0.6.
	 *
	 * THREE SEGMENTS AND NOT ONE because one mesh cannot taper — the same choice, and the same
	 * reasoning, as UTraceFxShapes::TaperAlongLocalZ documents for the tracer's bolt. The plan asks
	 * for 2 uu at the tail and this draws 4 uu: sub-8 uu emissive dissolves into dashes under TSR
	 * (bible §3.4), which is the floor ATraceFxBurst::MinEmissiveRadiusUU enforces for the same
	 * reason on every spark in the game.
	 *
	 * THE FLICKER IS NOT A LETHAL-TELEGRAPH PULSE. §3.3's prohibition is on a brightness oscillation
	 * that a player could mistake for STATE — an armed tell, a charge, a warning. This is a 30 Hz
	 * transient on a decorative exhaust whose lethal element (the body, whose size IS the hit radius)
	 * holds a constant brightness throughout. The plan names it and permits it in the same sentence.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> TrailSegments[3] = { nullptr, nullptr, nullptr };

private:
	/** SERVER ONLY. Sweeps the segment covered since the last tick and resolves the first thing hit. */
	void TickFlightAuthority(const FVector& FromPosition, const FVector& ToPosition);

	/** Places the mesh and points it along the instantaneous direction of travel. Every machine. */
	void UpdateVisual(const FVector& AtPosition);

	/**
	 * Builds the launch flash and the three trail segments. Called from BeginPlay on EVERY machine —
	 * the rocket is replicated, so its BeginPlay is the broadcast and no RPC is involved.
	 *
	 * Every piece goes through UTraceFxShapes::MakeGlowMID and stores the achieved blend, so a build
	 * where the materials did not resolve gets a HIDDEN piece rather than a default-grey 100 uu
	 * cylinder flying across the arena (bible §6.1's degradation ladder, and the "no grey primitive"
	 * rule ATraceFxBurst is measured by).
	 */
	void BuildFlightFx();

	/**
	 * Drives the flash's 0.28 s growth and the trail's placement and flicker. Every machine, per
	 * frame, from Tick. Zero spawns: both are the components BuildFlightFx already made.
	 *
	 * @param AtPosition       where the rocket is this frame.
	 * @param SecondsInFlight  on the MATCH clock, so every machine's flash is at the same size at the
	 *                         same instant — the same argument the path itself is built on.
	 */
	void UpdateFlightFx(const FVector& AtPosition, float SecondsInFlight);

	/**
	 * SERVER ONLY. Spawns §2.3's RocketBurst at @p Location and destroys this actor.
	 *
	 * *** ONE FUNCTION FOR THE THREE ENDINGS §2.3 NAMES, AND TWO THAT DELIBERATELY DO NOT USE IT. ***
	 *
	 * §2.3: the burst plays on "any end: body, wall, expiry". Those three all come through here, so
	 * three call sites cannot each forget it differently — which is what an expiry did before this
	 * existed: the rocket simply stopped being drawn.
	 *
	 * The two that call Destroy() directly instead are not endings, they are teardowns, and bursting
	 * on them would advertise a detonation that never happened:
	 *   * the OWNER-LOST fizzle. Roxie swapped character or left mid-flight; the rocket cannot deal
	 *     damage any more, so it must not look as though it did.
	 *   * the local-time BACKSTOP. It only fires in a world whose match clock never became usable
	 *     (no GameState, a fixture) — i.e. a rocket that never really flew.
	 *
	 * The burst is spawned BEFORE the Destroy: ATraceFxBurst is its own replicated actor and does not
	 * care that its parent is about to go, but spawning it after would be spawning it from a dead
	 * object.
	 *
	 * @param Normal        the surface normal for a wall hit, or the rocket's own travel direction
	 *                      reversed when there is no surface. RocketBurst lays its ring flat on this.
	 * @param DirectVictim  the pawn the rocket physically hit, or null for a wall or an expiry. They
	 *                      already took the full GetDamage() and are excluded from the splash below, so
	 *                      a direct impact is 100 rather than 100 plus a blast on top of itself.
	 * @param DirectDealt   what that pawn actually took, so the detonation record carries the number
	 *                      the health component saw rather than the number that was asked for — the two
	 *                      differ under Chut's damage reduction, X's vulnerable stacks and any future
	 *                      modifier, and a harness table that printed the ask would quietly lie.
	 */
	void DetonateAndDestroy(const FVector& Location, const FVector& Normal,
	                        ATraceCharacter* DirectVictim = nullptr, float DirectDealt = 0.f);

	/**
	 * DEMO 29 ITEM 7's "OTHERWISE, THE DAMAGE SHOULD FALL OFF". SERVER ONLY, once per detonation.
	 *
	 * Every living pawn whose CAPSULE SURFACE is within GetHitRadiusUU() of @p Location takes
	 * GetSplashDamageAtGapUU() of that gap, through UTraceAbilitySetRoxie::ApplyRocketDamageTo — the
	 * same single call the direct hit uses, so the carrier rule is enforced in the same one place.
	 *
	 * NOBODY IS SKIPPED EXCEPT @p DirectVictim, and that exclusion is arithmetic rather than policy:
	 * they took the direct 100 a line earlier and must not also take a blast centred on themselves.
	 * In particular THE SHOOTER IS NOT SKIPPED — she is offered to the choke point, which refuses her
	 * for ETraceAbilityBlockReason::Self. See the file header: that is what makes "the rocket jump did
	 * not become lethal" a structural fact rather than a special case somebody could delete.
	 *
	 * NO LINE-OF-SIGHT TRACE, deliberately. The blast is one body radius wide (72 uu at the shipped
	 * knob), so the volume it could leak through a wall is a few uu of a thin panel, while an occlusion
	 * ray from an impact point ON a floor to a pawn standing ON that floor grazes the floor and would
	 * cancel the commonest legitimate case — a rocket landing at somebody's feet. The false negative is
	 * worse than the false positive at this size. Revisit it if the radius ever grows past a metre.
	 */
	void ApplySplashDamage(const FVector& Location, ATraceCharacter* DirectVictim, float DirectDealt);

	/**
	 * DEMO 17 item 3, DEMO 29 item 7. Sizes the drawn body AT the rocket's own hit radius — no
	 * multiplier, no second knob. GetVisualRadiusUU() IS GetHitRadiusUU().
	 *
	 * Runs in BeginPlay rather than the constructor because the radius is a live settings knob and a
	 * CDO built before the .ini layers over the header would bake the wrong value in for the whole
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
	 * The dynamic materials for the flash and the three trail segments, and the blends they ACHIEVED.
	 *
	 * The blend is stored rather than assumed because UTraceFxShapes::MakeGlowMID can legitimately
	 * degrade (Additive -> Emissive -> Fallback -> None) and SetGlow must be given what was achieved,
	 * never what was asked for: the two materials take intensity by completely different routes and
	 * writing a parameter a material does not have is a silent no-op.
	 */
	/**
	 * The rocket BODY's material, and the blend it achieved.
	 *
	 * *** THIS USED TO BE A MID MADE FROM THE CONE'S OWN DEFAULT MATERIAL, AND IT WAS NOT EMBER. ***
	 * The Demo 17 code called Body->CreateDynamicMaterialInstance(0, Body->GetMaterial(0)), i.e. it
	 * wrapped whatever /Engine/BasicShapes/Cone ships with — BasicShapeMaterial, which is LIT and has
	 * a "Color" input and no emissive at all. So "EmissiveColor", "Glow", "EmissiveStrength" and
	 * "EmissivePower" were four silent no-ops and the rocket flew as a matte cone, which on this
	 * arena's black floor is a DARK cone. It is visible in the first capture of this tranche
	 * (FxHud_vrow_COOLING) as a near-black blob with a bright trail behind it.
	 *
	 * Resolved through UTraceFxShapes now, like every other piece: M_TraceNeon when it is there,
	 * degrading down the documented ladder, and the achieved blend stored so SetGlow writes the
	 * parameter the material actually has.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BodyMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> LaunchFlashMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> TrailMIDs[3] = { nullptr, nullptr, nullptr };

	ETraceFxBlend BodyBlend = ETraceFxBlend::None;
	ETraceFxBlend LaunchFlashBlend = ETraceFxBlend::None;
	ETraceFxBlend TrailBlend = ETraceFxBlend::None;

	/**
	 * §2.3's RoxieRocketLoop, started in BeginPlay on every machine and owned by this actor.
	 *
	 * *** IT IS NOT FADED OUT, AND THAT IS A DECISION RATHER THAN AN OMISSION. *** StartLoopOn hands
	 * back a component with bAutoDestroy off, which everywhere else in this plan means "the caller
	 * must FadeOut on the off-edge or it leaks". Here the off-edge IS the actor's destruction: the
	 * component is attached to this rocket's root and goes with it, on every machine, so there is
	 * nothing left to leak and nothing left alive to fade.
	 *
	 * The abrupt stop is also the right sound. A rocket does not trail away — it ends, and the frame
	 * it ends on is the frame ATraceFxBurst plays RoxieRocketBurst at the same point in space. A
	 * quarter-second fade under the detonation would be the engine still running after the explosion.
	 *
	 * The pointer is kept so that the loop is one named, findable thing rather than an anonymous
	 * component, and so a future change that DOES need a fade has somewhere to put it.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> FlightLoop = nullptr;

	/**
	 * Local world time the actor spawned, for the BACKSTOP only.
	 *
	 * The real expiry is the match-clock lifetime, which every machine agrees on. This is the second
	 * belt: a rocket whose shooter disconnected, or that spawned before the GameState replicated a
	 * usable clock, must not be able to outlive the match. ATraceMaceSpike carries the same backstop.
	 */
	float SpawnWorldTime = 0.f;
};
