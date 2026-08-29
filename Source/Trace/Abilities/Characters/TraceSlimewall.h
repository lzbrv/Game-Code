// Trace — SLIMEBALL's SLIMEWALL, and the slow it leaves on anybody who walks through it (spec v18 §2).
//
// ===================================================================================================
// SPEC v18 §2, SLIMEBALL'S ACTIVATED ABILITY, VERBATIM
// ===================================================================================================
//
//   "Slimewall (25 s cooldown): throws up a wall in his aim direction, one player height tall and
//    wide, and about the length of a standard in-game box — (for now, make this changeable), so all
//    three dimensions are knobs. The wall can be shot through but obstructs vision, and moving
//    through it slows enemies by 35%. Lasts 4 s."
//
// ===================================================================================================
// *** "CAN BE SHOT THROUGH" IS THE MOST IMPORTANT SENTENCE IN THIS FILE ***
// ===================================================================================================
//
// The slab has NO COLLISION AT ALL — not "no blocking on the weapon channel", none. That is a
// stronger guarantee than the doc asks for and it is deliberate, because this project's hitscan is
// two traces and only one of them is obvious:
//
//   1. UTraceLagCompensationComponent::ResolveHitscan line-traces the LIVE world on ECC_Visibility to
//      bound the ray, ignoring every character. A slab that blocked ECC_Visibility would truncate
//      MaxDistance short of the enemy and the analytic body test would then never be reached — the
//      shot would silently miss, on the server, with no log and no hit marker. It would look exactly
//      like lag.
//   2. The same resolver runs on the CLIENT for the cosmetic tracer, so the tracer would stop in the
//      slime while the server said "hit" (or, worse, both would agree on a miss the player cannot
//      explain).
//
// A wall that eats bullets is not a weaker version of this ability; it is the opposite of it. So the
// slab is `ECollisionEnabled::NoCollision` with the "NoCollision" profile, it generates no overlaps,
// it affects no navigation, and HasAnyCollisionEnabled() exists so a harness can assert that rather
// than trust this comment. Trace.Slimeball.WallBlocksBullets is the RED ARM: it puts BlockAll back
// on the slab, which reproduces exactly the bug above, so the green run means something.
//
// "OBSTRUCTS VISION" IS THEREFORE PURE RENDERING, and that works because rendering and collision are
// independent in UE: an opaque mesh occludes the camera whether or not anything can trace against it.
// The cost, stated honestly rather than buried: an AI line-of-sight probe is also ECC_Visibility
// (ATraceBotController::HasLineOfSight), so BOTS CAN SEE THROUGH THE WALL AND HUMANS CANNOT. That is
// a real asymmetry and it is named in the report — it cannot be fixed from this file without giving
// the slab a blocking channel, which is the one thing §2 forbids. The fix is a dedicated trace
// channel for sight, and that is a project-settings change plus a line in AI/.
//
// ===================================================================================================
// THE 35% SLOW IS A CONTROL EFFECT, AND IT IS ASKED AGAIN EVERY TICK
// ===================================================================================================
//
// Spec §4: "NO abilities damage carriers", generalised to Control by the [ASSUMPTION] behind
// UTraceSettings::bCarrierImmuneToAbilityControl. A slow is Control, so it goes through
// UTraceAbilityComponent::CanAffect(Source, Target, Control) and NOWHERE ELSE — there is no carrier
// test written in this file, deliberately, because a second copy of that rule is a second thing that
// can rot.
//
// And it is re-asked on every tick rather than once at application, for the reason Oyster's poison
// spells out: the interesting case is not "slow a carrier" (refused at the wall), it is
//
//     an ordinary player is slowed, and half a second later they PICK UP THE CORE.
//
// UTraceSlimewallSlowComponent::TickComponent re-decides bSlowActive from the choke point every tick,
// so becoming the carrier un-slows them within a frame and dropping the Core resumes the slow for
// whatever is left of the linger. Trace.Slimeball.Verify exercises exactly that transition.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Subsystems/WorldSubsystem.h"   // UTraceSlimeStickSubsystem, at the foot of this header
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"                 // ETraceTeam
#include "Gameplay/TraceFxShapes.h"     // ETraceFxBlend — stored per piece, so it must be complete

#include "TraceSlimewall.generated.h"

class APlayerState;
class ATraceCharacter;
class UTraceAbilityComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * THE ALARM, per effect vector — the same shape, and the same argument, as TraceOyster::FEffectTally.
 *
 * The framework's TraceAbility::GetCarrierAbilityDamageHitCount() counts ability DAMAGE that reached a
 * carrier, and Slimeball's wall deals none, so that counter would report "clean" for a build in which
 * every carrier who crossed a slime wall was crippled. Counted here instead, per vector, for carriers
 * and non-carriers alike:
 *
 *   Carrier  must be ALL ZERO for the life of a correct process. Every increment also logs an Error.
 *   Other    is the fixture proving itself — a run in which the wall slowed nobody proves nothing
 *            about carriers, it proves the harness never fired.
 *
 * Counted at the point the effect is DECIDED, not where it is observed, so a slow applied to a
 * stationary bot who never moves still counts. What is under test is whether the rule let it happen.
 */
namespace TraceSlimewall
{
	struct FSlowTally
	{
		/** A slow that actually attached to (or refreshed on) a target. */
		int32 Applications = 0;
		/** A tick on which the -35% was decided to be in force for a target. Server only. */
		int32 SlowFrames = 0;

		int32 Total() const { return Applications + SlowFrames; }
		void Reset() { *this = FSlowTally(); }
	};

	/** Slows that landed on a player holding the Core. MUST STAY ALL ZERO. */
	TRACE_API FSlowTally& CarrierTally();

	/** Slows that landed on anybody else. The fixture's own proof of life. */
	TRACE_API FSlowTally& OtherTally();

	TRACE_API void ResetTallies();

	/** Files @p Landed under the right tally and logs an Error if the victim was the carrier. */
	TRACE_API void RecordEffect(const ATraceCharacter* Target, const TCHAR* VectorName, int32 FSlowTally::* Field);
}

/**
 * One target's slime slow. Server-authoritative; replicated so the owning client clamps its own speed
 * on exactly the frames the server does and the correction path stays quiet.
 *
 * Created on the victim's pawn by ATraceSlimewall and by nothing else. Removes itself when the linger
 * runs out, and dies with the pawn on a respawn — which is correct: slime is on a body, not on a
 * player.
 */
UCLASS(ClassGroup = (Trace))
class TRACE_API UTraceSlimewallSlowComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTraceSlimewallSlowComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

	/**
	 * SERVER ONLY. Slows @p Target on behalf of @p InSource, or refreshes an existing slow.
	 *
	 * Does NOT check the choke point itself — the caller has already, because whether to touch the
	 * target AT ALL is the wall's decision. Every tick after this re-checks, which is where the
	 * "picked the Core up mid-slow" case is caught.
	 *
	 * @return the live component, or null when nothing could be attached.
	 */
	static UTraceSlimewallSlowComponent* ApplyTo(ATraceCharacter* Target, APlayerState* InSource, float LingerSeconds);

	/** The live slime slow on @p Target, or null. */
	static UTraceSlimewallSlowComponent* Find(const ATraceCharacter* Target);

	/** Absolute match time the slow ends. Replicated — never a duration; the framework's rule. */
	float GetEndMatchTime() const { return EndMatchTime; }

	/** True when the -35% is IN FORCE this tick — i.e. the choke point last said yes. Replicated. */
	bool IsSlowActive() const { return bSlowActive; }

	/**
	 * SPEC v18 §2 — THE ONE DEFINITION OF "HOW MUCH SLOWER", read by two callers.
	 *
	 * TraceAbilityDebuff::GetMoveSpeedMultiplier multiplies by it (FX_AUDIO_PLAN §7.3's F7 line), and
	 * UTraceCharacterMovementComponent::GetMaxSpeed() multiplies by that aggregator, so GetMaxSpeed()
	 * IS the slimed ceiling and acceleration targets it. ApplySlowClamp() then enforces that same
	 * ceiling on the frames the acceleration model would otherwise overshoot it — WITHOUT re-applying
	 * the fraction, which is what would compound 0.65 into 0.42. The -35% is applied exactly once.
	 *
	 * 1.0 whenever the slow is not in force, which includes every tick the §4 choke point refused,
	 * because bSlowActive IS that answer.
	 *
	 * READ FROM THE KNOB, not from a replicated copy, exactly as UTraceOysterPoisonComponent does:
	 * UTraceSettings is identical on both ends and live-editable on both ends, so a replicated
	 * snapshot would only add a way for the two to disagree.
	 */
	float GetSpeedMultiplier() const;

	/** SERVER ONLY. One authority pass, callable from a harness so a measurement needs no tick. */
	void ServerRefreshNow();

	/**
	 * FX_AUDIO_PLAN §2.10 "victim slow tell". True when this MACHINE is drawing the feet ring.
	 *
	 * False on a dedicated server, false while the victim is cloaked, false if the §1.4 loop budget
	 * refused the piece (a victim who is also poisoned is already carrying three drips) — all three
	 * are correct answers, which is why the harness reads this rather than assuming one.
	 */
	bool IsSlowTellDrawn() const;

private:
	/** REPLICATED. Absolute match-clock time. */
	UPROPERTY(Replicated)
	float EndMatchTime = 0.f;

	/** REPLICATED. Re-decided by the server every tick from the choke point. */
	UPROPERTY(Replicated)
	bool bSlowActive = false;

	/** Who slimed them. Weak: Slimeball may die, leave, or change character mid-slow. */
	TWeakObjectPtr<APlayerState> SourcePlayerState;

	ATraceCharacter* GetVictim() const;
	float MatchTimeNow() const;

	/** Every machine that simulates the victim. The clamp itself. */
	void ApplySlowClamp();

	// ---------------------------------------------------------------------------------------------
	// FX_AUDIO_PLAN §2.10 — THE VICTIM SLOW TELL, AND WHY IT LIVES ON THIS COMPONENT
	// ---------------------------------------------------------------------------------------------
	//
	// §2.10: "while UTraceSlimewallSlowComponent active: feet shimmer ring r 40 uu additive Slowed
	// (0.35,0.55,1.00) I 0.3 | the slow component replicates per-victim — attach in its activation on
	// each machine".
	//
	// This component is already a replicated sub-object whose TickComponent runs on every machine (it
	// has to; ApplySlowClamp needs it), and bSlowActive is already replicated because the owning
	// client clamps its own speed off it. So the tell needs no router edge, no RPC and no new
	// replicated field: bSlowActive IS the fact, on every machine, and one additive ring hangs off it.
	//
	// THE HUE IS `Slowed` AND NOT SLIME GREEN, which is ART_BIBLE §6.2's hue priority doing its job:
	// the wall is Slimeball's object and wears his accent; the debuff on the victim is a STATUS and
	// wears the semantic wheel's colour, the same one every other slow in the game will use.

	/** Every machine that draws. Creates the ring on the first eligible tick and shimmers it. */
	void UpdateSlowTellFx();

	/** Idempotent. Called when the slow lapses, on death, on cloak, and from both destruction paths. */
	void DetachSlowTellFx();

	/** The feet ring. Null until the first drawing tick; never made on a dedicated server. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> SlowTellRing = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SlowTellMID = nullptr;

	/** Local seconds, driving the shimmer. Real time and local: see the drip's twin in Oyster's file. */
	float SlowTellElapsed = 0.f;
};

/**
 * THE SLAB. A 4 s box of slime, placed in Slimeball's aim direction.
 *
 * Replicated, server-spawned, and it owns exactly two things: its own look, and the per-tick decision
 * of who is standing in it. It owns none of Slimeball's rules — the cooldown, the placement sweep and
 * the "may I even do this" gates all live in UTraceAbilitySetSlimeball, because they are about him.
 *
 * THE THREE DIMENSIONS ARE THREE SEPARATE KNOBS because §2 says "(for now, make this changeable)".
 * HEIGHT is vertical, WIDTH is the thickness (what an enemy walks THROUGH), LENGTH is the long run.
 *
 * *** PATCH 28 ITEM 2 TURNED THE WALL THROUGH 90 DEGREES: "Slimeball's slimewall should be placed
 * *** forward instead of laterally in front of him." ***
 *
 * Until Patch 28 the LENGTH ran ACROSS his aim — a 1100 uu barrier facing him, 176 uu thick, which
 * is the wall you hide BEHIND. It now runs ALONG his aim: a 176 x 176 uu cross-section (one player
 * tall and one player wide, exactly as §2 words it) extending 1100 uu away from him. It is the wall
 * you run BESIDE, and it splits a lane rather than capping it.
 *
 * *** THE SLAB'S OWN LOCAL AXES DID NOT MOVE, AND THAT IS THE WHOLE IMPLEMENTATION. *** WallAim is,
 * and always was, the slab's local +X — its NORMAL, the thin axis. What changed is what the ability
 * hands in: it used to pass his planar aim, and now it passes the LATERAL vector, so the long local
 * +Y span points down his aim instead of across it. Everything that reads the slab in its own basis
 * — IsInsideWall's OBB test, the rise, the lip, the three seams, ATraceBotController's segment-vs-
 * slab test — is expressed in that basis and needed no edit. (W4-KITS-C was asked to keep §2.10's FX
 * orientation-agnostic so that this would be a placement change; it held, on the condition that the
 * long axis stays local +Y. SWAPPING THE EXTENTS INSTEAD WOULD HAVE BROKEN IT: the three seams are
 * distributed along local Y and sized across local X, so a swap would have drawn three 1100 uu
 * stripes running the length of the wall instead of four equal panels along it.)
 */
UCLASS()
class TRACE_API ATraceSlimewall : public AActor
{
	GENERATED_BODY()

public:
	ATraceSlimewall();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * SERVER ONLY. The one way a slime wall is ever made.
	 *
	 * @param WorldPtr        world to spawn into.
	 * @param InSource        Slimeball's PlayerState — the instigator the choke point is asked about.
	 * @param InSourceTeam    his team, so "does not slow his own side" needs no pawn lookup later.
	 * @param InCenter        the centre of the slab, already resolved onto the floor by the caller.
	 * @param InPlanarAim     unit, horizontal. The slab's local +X, i.e. its NORMAL — the direction of
	 *                        its THICKNESS. Since Patch 28 item 2 the cast ability passes the LATERAL
	 *                        vector here, not the player's aim; ResolveForwardRun below computes it.
	 * @param InHalfExtents   (THICKNESS/2 along InPlanarAim, LONG SPAN/2 across it, HEIGHT/2), in uu.
	 * @param InExpireMatchTime  absolute match-clock time it vanishes.
	 * @return the wall, or null (no world, no authority, a degenerate size).
	 */
	static ATraceSlimewall* ServerSpawn(UWorld* WorldPtr, APlayerState* InSource, ETraceTeam InSourceTeam,
	                                    const FVector& InCenter, const FVector& InPlanarAim,
	                                    const FVector& InHalfExtents, float InExpireMatchTime);

	/**
	 * *** PATCH 28 ITEM 2. WHERE A CAST WALL GOES, AND WHICH WAY IT POINTS. ***
	 *
	 * "Slimeball's slimewall should be placed forward instead of laterally in front of him."
	 *
	 * Lays a slab of @p ThicknessUU x @p HeightUU cross-section running @p ForwardSpanUU AWAY from
	 * @p Placer along @p PlanarAim, starting @p StandoffUU in front of him, and drops it onto the
	 * floor under its middle. It lives on the WALL rather than in Slimeball's ability set because it
	 * is the geometry of this actor, and because both of its outputs (the centre and the slab normal)
	 * have to agree with the slab basis this class documents — a caller reconstructing them is a
	 * second opinion about which way a slime wall faces.
	 *
	 * TWO WAYS IT CAN COME BACK FALSE, both a FREE FIZZLE at the call site (§2's "Mace's spike
	 * hitting nothing" precedent — the framework charges the 25 s only on a true return):
	 *   * no world, no placer, or no horizontal aim;
	 *   * the clear run ahead is shorter than the wall's own thickness, i.e. there is not room for a
	 *     piece of wall that is longer than it is wide. Anything shorter is a block, not a wall.
	 *
	 * IT SHORTENS RATHER THAN RETREATING, and that is the deliberate half. The lateral wall pulled
	 * ITSELF BACK toward him when something blocked the way, because the obstacle was in front of a
	 * 176 uu thickness. A forward wall's long axis points INTO the obstacle, so retreating would move
	 * the whole 1100 uu run back over his own head to buy 176 uu of clearance. Instead the far end
	 * stops AT the obstacle — which is what the old code's own comment asked for in words ("throwing
	 * a wall at a pillar two metres away puts it against the pillar rather than half inside it").
	 * The near end therefore never moves, so "it goes up SlimewallRangeUU in front of me" stays true.
	 *
	 * @param StandoffUU  distance from the placer to the NEAR END of the wall. Raised to
	 *                    (capsule radius + 10) if it is smaller, because a wall he is standing inside
	 *                    is a wall he cannot see out of.
	 * @param OutSlabNormal  the LATERAL unit vector to hand ServerSpawn as InPlanarAim.
	 * @param OutHalfExtents (ThicknessUU/2, ACHIEVED span/2, HeightUU/2) — the span is the achieved
	 *                    one, which is <= ForwardSpanUU when something was in the way.
	 */
	static bool ResolveForwardRun(const UWorld* WorldPtr, const ATraceCharacter* Placer,
	                              const FVector& PlanarAim, float ThicknessUU, float ForwardSpanUU,
	                              float HeightUU, float StandoffUU,
	                              FVector& OutCenter, FVector& OutSlabNormal, FVector& OutHalfExtents,
	                              FString& OutWhy);

	/**
	 * SERVER ONLY. One pass of "who is standing in me, and may I slow them".
	 *
	 * Called from Tick, and callable directly so a harness can measure the decision in the SAME
	 * statement sequence it stages — no tick in between, so a bot walking past cannot land inside a
	 * measurement. Same reasoning as the X fixture's MeasureDamage.
	 */
	void ServerUpdateSlows();

	/** Is @p Candidate's capsule intersecting the slab right now? Cheap OBB test; no collision needed. */
	bool IsInsideWall(const ATraceCharacter* Candidate) const;

	FVector GetHalfExtentsUU() const { return FVector(HalfExtents); }
	FVector GetWallCenter() const { return FVector(WallCenter); }
	FVector GetWallAim() const { return FVector(WallAim); }
	float   GetExpireMatchTime() const { return ExpireMatchTime; }
	ETraceTeam GetSourceTeam() const { return SourceTeam; }
	APlayerState* GetSourcePlayerState() const { return SourcePlayerState; }

	/**
	 * *** THE INVARIANT, AS A QUESTION A HARNESS CAN ASK. ***
	 * True if ANY component on this actor has collision enabled. §2 says the wall can be shot
	 * through, so on a shipped build this MUST be false. It is true only under the
	 * Trace.Slimeball.WallBlocksBullets red arm, which exists so the shot-through test can go red.
	 */
	bool HasAnyCollisionEnabled() const;

	/** How many distinct players this wall has slowed since it went up. The fixture's proof of life. */
	int32 GetLifetimeSlowedCount() const { return LifetimeSlowed.Num(); }

	/** The parent material the slab ended up on, for the harness. "<none>" when it could not draw. */
	FString DescribeSlabMaterial() const;

	// =============================================================================================
	// FX_AUDIO_PLAN §2.10 — THE DRESSING, AND THE ONE PROMISE IT MAKES ABOUT PLACEMENT
	// =============================================================================================
	//
	// §2.10 asks for a top lip strip and three vertical seam strips so the slab "reads as authored"
	// rather than as an engine cube — the arena's own lip/trim grammar (ART_BIBLE §3.2: within one
	// object the top lip outranks the face trim), plus a 0.18 s rise on spawn and a 0.3 s glow fade on
	// expiry so neither end is a pop (§6.4).
	//
	// *** EVERY PIECE IS BUILT IN THE SLAB'S OWN LOCAL FRAME, FROM HalfExtents AND NOTHING ELSE. ***
	// There is no world direction, no "in front of Slimeball" and no aim vector anywhere in the
	// dressing: local +X is the thickness, +Y the span, +Z up, and the lip and seams are placed in
	// those. The owner has a queued request to move the wall FORWARD of Slimeball instead of laterally
	// — when that lands it changes ServerSpawn's CALLER and nothing here, because a wall that is
	// dressed relative to itself looks identical wherever it is put. The seams deliberately pass
	// THROUGH the slab and stand proud of both faces for the same reason: a wall approached from the
	// other side must not be the undressed one.

	/**
	 * 0 while the wall is still rising, 1 once it is up. Drives the drawn Z scale only.
	 *
	 * PRESENTATION, NOT GAMEPLAY, AND THAT IS DELIBERATE AND NARROW. IsInsideWall() keeps testing the
	 * FULL HalfExtents from the first frame, so for at most SpawnRiseSeconds (0.18) the drawn slab is
	 * a growing subset of the volume that slows. Two reasons it is the right call: the wall grows from
	 * its BASE, so the drawn part is always the part nearest the floor — where a body standing in the
	 * footprint already is — and the alternative (gating the slow on the drawn height) would make a
	 * cosmetic timeline load-bearing in the one file whose invariant is "no second copy of a rule".
	 * ART_BIBLE §6.4 sanctions the mirror case in as many words ("expiry dissolves ... 0.3 s fade,
	 * never a pop-out"), and this is that, run backwards, over 0.18 s.
	 *
	 * A wall with NO DEADLINE (ExpireMatchTime == 0) skips the rise entirely and is up from its first
	 * frame — see the function body for why that is a statement about what such a wall IS rather than
	 * a courtesy to a fixture, and for the red arm it was needed to keep honest.
	 */
	float GetRisenFraction() const;

	/** 1 normally; ramps to 0 over the last SpawnFadeSeconds before ExpireMatchTime. */
	float GetExpiryFadeFraction() const;

	/** How many DRESSING pieces this machine actually built and made visible (lip + seams). */
	int32 GetDressingPieceCount() const;

	/** "slab=Emissive lip=Emissive seams=Emissive" — the ACHIEVED blends, for the log. */
	FString DescribeDressing() const;

protected:
	virtual void BeginPlay() override;

	/** REPLICATED. The slab's centre. Also the actor location; replicated so a client's maths agrees. */
	UPROPERTY(Replicated)
	FVector_NetQuantize WallCenter = FVector::ZeroVector;

	/** REPLICATED. Unit, horizontal. The slab's local +X — the direction its WIDTH is measured along. */
	UPROPERTY(Replicated)
	FVector_NetQuantizeNormal WallAim = FVector::ForwardVector;

	/** REPLICATED. (WIDTH/2, LENGTH/2, HEIGHT/2) in uu, in the slab's own frame. */
	UPROPERTY(Replicated)
	FVector_NetQuantize HalfExtents = FVector::ZeroVector;

	/** REPLICATED. Absolute match time the wall vanishes. */
	UPROPERTY(Replicated)
	float ExpireMatchTime = 0.f;

	/**
	 * REPLICATED. Absolute match time the wall was cast — the other end of the same clock.
	 *
	 * FOUR BYTES, WRITTEN ONCE, AND IT IS WHAT MAKES THE RISE HONEST ON A CLIENT. The alternative was
	 * to derive it as ExpireMatchTime - SlimewallDurationSeconds, which is a SECOND source of truth
	 * for when the wall started: it would be silently wrong for every wall spawned with a different
	 * duration (the fixtures all pass expire = 0) and would drift the moment the knob is retuned
	 * mid-match. Both ends of the wall's life are now absolute match times, which is what every other
	 * timer in this project is and is what lets a client that received the actor late still animate it
	 * correctly. It never changes after the initial bunch, so it costs no update traffic at all.
	 */
	UPROPERTY(Replicated)
	float SpawnMatchTime = 0.f;

	/** REPLICATED. Slimeball. Held as the instigator the §4 choke point is asked about. */
	UPROPERTY(Replicated)
	TObjectPtr<APlayerState> SourcePlayerState = nullptr;

	/** REPLICATED. His team, so "does not slow his own side" survives him dying or leaving. */
	UPROPERTY(Replicated)
	ETraceTeam SourceTeam = ETraceTeam::None;

	UPROPERTY()
	TObjectPtr<USceneComponent> WallRoot = nullptr;

	/** The visible slab. NO COLLISION — see the file header; that is the whole ability. */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Slab = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> SlabMID = nullptr;

	/**
	 * FX_AUDIO_PLAN §2.10's top lip: a band capping the slab, standing proud of both faces and of the
	 * top so it reads as a lip and not as a decal. Glow above the slab's, below the seams' — the
	 * arena's own "the top lip outranks the face trim" ratio (ART_BIBLE §3.2).
	 */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Lip = nullptr;

	/**
	 * The three vertical seam strips, as ONE component with three instances.
	 *
	 * An ISM and not three components, which is this project's own precedent for a repeated element
	 * (ATraceElleGate's beads, ATraceMortimerQuakeWave's, ATraceRippleActor's rings, and every
	 * ATraceFxBurst scatter). Each instance spans the slab's FULL thickness and stands proud of both
	 * faces: a seam that existed only on the face Slimeball happened to be looking at would make the
	 * wall's two sides different objects, and the placement of this wall is about to change.
	 */
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> Seams = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> LipMID = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> SeamMID = nullptr;

	/** /Engine/BasicShapes/Cube. Held hard so the cooker follows it. */
	UPROPERTY()
	TObjectPtr<UStaticMesh> SlabMesh = nullptr;

	/** M_TraceNeon when the content pack exists, BasicShapeMaterial otherwise. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> NeonMaterial = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> FallbackMaterial = nullptr;

private:
	/** Idempotent, and cheap until the replicated extents arrive — some frames after BeginPlay. */
	void BuildSlabIfNeeded();

	/**
	 * FX_AUDIO_PLAN §2.10. Re-places the slab, the lip and the seams for the current rise fraction,
	 * and pushes the current expiry fade into all three MIDs.
	 *
	 * Called every tick on EVERY machine (before Tick's authority guard) because both animations are
	 * driven entirely off the two replicated match times — nothing about the rise or the fade goes on
	 * the wire, exactly as ATraceOysterPoisonCloud drives its own fade.
	 */
	void UpdateWallAnimation();

	/** Builds the lip and the seam instances. Idempotent; skipped whole on a dedicated server. */
	void BuildDressingIfNeeded(const FVector& Extents);

	/** The rise half of UpdateWallAnimation: slab, lip and seam transforms at @p Risen in [0,1]. */
	void ApplyRiseGeometry(const FVector& Extents, float Risen);

	float MatchTimeNow() const;

	/** The dressing pieces' achieved blends. None means the piece is hidden, never grey. */
	ETraceFxBlend LipBlend = ETraceFxBlend::None;
	ETraceFxBlend SeamBlend = ETraceFxBlend::None;

	/** True once the lip/seams have been sized and dressed on this machine. */
	bool bDressingBuilt = false;

	/** The last fade written into the MIDs, so a steady wall is not three material writes a frame. */
	float LastAppliedFade = -1.f;

	/** The last rise written into the geometry, so a wall that is UP is not re-transformed every tick. */
	float LastAppliedRisen = -1.f;

	/** The mesh is attached. True on every net mode, including a dedicated server — see BuildSlabIfNeeded. */
	bool bSlabBuilt = false;

	/** The material is on. Never true on a dedicated server, which cooks no shaders. */
	bool bSlabDressed = false;

	/** Distinct victims, for the harness. Not a rule — a wall may slow the same player repeatedly. */
	TArray<TWeakObjectPtr<ATraceCharacter>> LifetimeSlowed;
};

/**
 * FX_AUDIO_PLAN §2.10 "stick goo" — the slime under Slimeball while he is clinging to a wall.
 *
 * ===================================================================================================
 * §2.10, VERBATIM
 * ===================================================================================================
 *
 *   "Stick goo | while stuck (`MovementActive`): one additive slime sphere r 26 uu I 0.4 at the
 *    pawn-wall contact point + 2 drip spheres r 6 uu falling 30 uu, loop | router edge"
 *
 * ===================================================================================================
 * *** IT IS SELF-DRIVING, AND THAT IS A DEVIATION WITH A REASON THAT IS NOT ARCHITECTURAL TASTE ***
 * ===================================================================================================
 *
 * §2.10 names the §1.2 FX router as the hook: `UTraceAbilitySetSlimeball::OnClientStateEdge` would
 * turn this on and off on every machine off the replicated `MovementActive` edge, and that is the
 * better shape. This class does not live in that file because the release plan's ownership map does
 * not give `TraceAbilitySetSlimeball.*` to anybody in this wave, and writing another tranche's file
 * is the one thing the plan forbids outright.
 *
 * So the component reads THE SAME FACT FROM THE SAME PLACE, one tick later instead of on the edge:
 * `UTraceAbilityComponent::GetNetState().Flags & TraceAbilityFlags::MovementActive`, which is a
 * replicated field on a component that exists on every machine. There is no second source of truth
 * here and nothing new on the wire — the difference between this and the router is 1 frame of latency
 * and one `if` per tick.
 *
 * WHAT STILL NEEDS THE KIT, and it is ONE LINE: something has to create the component. Whoever ends
 * up owning `TraceAbilitySetSlimeball.*` should call `UTraceSlimeStickFxComponent::EnsureOn(Pawn)`
 * from `OnPawnSpawned()` / `SyncClientFx()`, and may then delete this class's polling in favour of a
 * direct `SetStuck()` call from `OnClientStateEdge` — the polling is written so that swap is a
 * deletion and not a rewrite. Until then `Trace.Slimeball.StickGoo` is what puts it on a pawn, and
 * the release report files it as a handoff rather than pretending it is wired.
 */
UCLASS(ClassGroup = (Trace))
class TRACE_API UTraceSlimeStickFxComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTraceSlimeStickFxComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

	/**
	 * Idempotent. Puts one of these on @p Pawn (LOCAL to this machine — it is cosmetic and replicates
	 * nothing) and returns it. Null for a null/dead pawn or on a dedicated server, which draws nothing.
	 */
	static UTraceSlimeStickFxComponent* EnsureOn(ATraceCharacter* Pawn);

	/** The live one on @p Pawn, or null. */
	static UTraceSlimeStickFxComponent* Find(const ATraceCharacter* Pawn);

	/**
	 * THE SEAM THE ROUTER WOULD CALL. Forces the goo on or off and, when turning it on, tells it where
	 * the wall is. Pass a zero @p ContactWorld to have it probe for the wall itself.
	 *
	 * While `bDrivenExternally` is set by this call the per-tick poll stops asking the ability state,
	 * so a kit that wires the router edge does not have to fight the fallback.
	 */
	void SetStuck(bool bStuck, const FVector& ContactWorld);

	/** How many goo pieces this machine is drawing: 3 when stuck and the budget allowed it, else 0. */
	int32 GetGooPieceCount() const;

	/**
	 * True while SOMEBODY HAS FORCED THE GOO ON through SetStuck() rather than the replicated flag —
	 * i.e. `Trace.Slimeball.StickGoo 1`, or a kit that has wired the §1.2 router edge.
	 *
	 * UTraceSlimeStickSubsystem reads it so that the STICK SOUND has exactly one producer whichever
	 * way the goo was turned on: when this is true the subsystem takes its edge from here instead of
	 * from `MovementActive`, so the harness proves the audio on the same statement that proves the FX
	 * and a wired router edge does not double it.
	 */
	bool IsForcedStuck() const { return bDrivenExternally && bStuckNow; }

private:
	/** Reads MovementActive off the pawn's replicated ability state. See the class comment. */
	void PollStuckState();

	/** Creates the blob and its two drips through the §1.4 budget helper. */
	void AttachGoo();

	/** Idempotent. */
	void DetachGoo();

	/** Eight horizontal probes at capsule height; the nearest wall wins. Cheap, and re-run at 5 Hz. */
	bool ProbeContactPoint(FVector& OutContactWorld) const;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> GooPieces;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> GooMIDs;

	/** Pawn-local, so the goo rides the pawn without a per-frame world transform. */
	FVector ContactLocal = FVector::ZeroVector;

	float Elapsed = 0.f;
	float NextProbeSeconds = 0.f;
	bool bStuckNow = false;
	bool bDrivenExternally = false;
};

// ===================================================================================================
// THE STICK MOMENT — one producer, for the SOUND and for the GOO
// ===================================================================================================
//
// *** WHY THIS EXISTS: `SlimeballStick` WAS THE ONE ROW OF THE 43-STEM BANK WITH NO PRODUCER. ***
//
// W5-AUDIOMIX observed 42 of the 43 FX_AUDIO_PLAN §5.1 events reaching the audio engine at their real
// trigger site. The 43rd was `SlimeballStick`: imported, resolving, mixed (-14.0 dB, +38.6 dB over
// the floor) and referenced by nothing at all in Source/. Its FX half — UTraceSlimeStickFxComponent's
// goo — was already live and was photographed attaching with silence beside it.
//
// The reason was never that the moment is hard to find. It is that the moment is DECIDED in
// `TraceAbilitySetSlimeball.cpp` (the `MovementActive` flag: "holding V with a wall in reach"), and
// that file has not been in any wave's ownership since Aug 12, so no tranche could add the call. The
// same ownership wall is why the goo component above polls instead of riding the router edge, and why
// it has never had a shipping creator either — see its class comment's "WHAT STILL NEEDS THE KIT".
//
// This subsystem is the answer to both, and it needs nobody's file: it reads the SAME replicated fact
// from the SAME place the goo already reads it, one level up.
//
//   THE SOUND is authority-only and multicast, because `SlimeballStick` is declared World-side in
//   Audio/TraceSoundEvents.h. TraceAudio::PlayAt refuses on a client, so the guard is belt and
//   braces, but the edge is only TRACKED on the authority — a client that tracked it too would keep
//   a second copy of the state for nothing.
//
//   THE GOO is the opposite: local, cosmetic, and never on a dedicated server. So the two halves are
//   driven by one poll but gated differently, and that asymmetry is the whole reason the sound could
//   not simply be dropped into the component (which does not exist on a dedicated server, where the
//   authority usually lives).
//
// ONE PRODUCER, WHICHEVER WAY THE STICK ARRIVED. When something has forced the goo on through
// SetStuck() — `Trace.Slimeball.StickGoo`, or a kit that later wires the §1.2 edge — the poll defers
// to it (UTraceSlimeStickFxComponent::IsForcedStuck), exactly as the component's own poll already
// defers to `bDrivenExternally`. There is no path on which one stick makes two sounds.
//
// WHEN THE KIT FILE IS FINALLY OWNED: calling `SetStuck()` from `OnClientStateEdge` is still the
// better shape and it costs this class nothing — the deferral above is what makes that swap a
// deletion rather than a rewrite.

UCLASS()
class TRACE_API UTraceSlimeStickSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// UWorldSubsystem
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return true; }

	/** The subsystem for @p WorldContextObject's world, or null. Null is legal everywhere. */
	static UTraceSlimeStickSubsystem* Get(const UObject* WorldContextObject);

	/** How many stick sounds this world has multicast. The harness's proof of life. */
	int32 GetStickSoundCount() const { return StickSoundCount; }

private:
	/**
	 * The pawns that were stuck at the last poll, by ability component.
	 *
	 * Keyed on the COMPONENT (which lives on the PlayerState and therefore survives a respawn) rather
	 * than on the pawn, so a death does not lose the state and re-fire the sound the instant the
	 * player is put back. Weak, so a disconnect drops out on its own.
	 */
	TSet<TWeakObjectPtr<UTraceAbilityComponent>> WereStuck;

	int32 StickSoundCount = 0;

	/** 60 Hz is pointless for a level change; 20 Hz costs a stick at most 50 ms of lateness. */
	float PollAccumulator = 0.f;
};
