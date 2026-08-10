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
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"        // ETraceTeam

#include "TraceSlimewall.generated.h"

class APlayerState;
class ATraceCharacter;
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
	 * SPEC v18 §2 — THE ONE DEFINITION OF "HOW MUCH SLOWER".
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
};

/**
 * THE SLAB. A 4 s box of slime, placed in Slimeball's aim direction.
 *
 * Replicated, server-spawned, and it owns exactly two things: its own look, and the per-tick decision
 * of who is standing in it. It owns none of Slimeball's rules — the cooldown, the placement sweep and
 * the "may I even do this" gates all live in UTraceAbilitySetSlimeball, because they are about him.
 *
 * THE THREE DIMENSIONS ARE THREE SEPARATE KNOBS because §2 says "(for now, make this changeable)".
 * The axis each one means is an [ASSUMPTION] recorded on the knobs themselves in TraceSettings.h:
 * HEIGHT is vertical, WIDTH is the thickness along his aim (what an enemy walks THROUGH), LENGTH is
 * the span across his aim (what you hide behind).
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
	 * @param InPlanarAim     unit, horizontal. The slab's local +X, i.e. the direction of its WIDTH.
	 * @param InHalfExtents   (WIDTH/2 along aim, LENGTH/2 across aim, HEIGHT/2 vertical), in uu.
	 * @param InExpireMatchTime  absolute match-clock time it vanishes.
	 * @return the wall, or null (no world, no authority, a degenerate size).
	 */
	static ATraceSlimewall* ServerSpawn(UWorld* WorldPtr, APlayerState* InSource, ETraceTeam InSourceTeam,
	                                    const FVector& InCenter, const FVector& InPlanarAim,
	                                    const FVector& InHalfExtents, float InExpireMatchTime);

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

	float MatchTimeNow() const;

	/** The mesh is attached. True on every net mode, including a dedicated server — see BuildSlabIfNeeded. */
	bool bSlabBuilt = false;

	/** The material is on. Never true on a dedicated server, which cooks no shaders. */
	bool bSlabDressed = false;

	/** Distinct victims, for the harness. Not a rule — a wall may slow the same player repeatedly. */
	TArray<TWeakObjectPtr<ATraceCharacter>> LifetimeSlowed;
};
