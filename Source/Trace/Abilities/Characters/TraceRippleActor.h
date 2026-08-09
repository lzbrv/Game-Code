// Trace — Rocco's Ripple, spec v14 §6.
//
// ===================================================================================================
// WHAT §6 ACTUALLY ASKS FOR, CLAUSE BY CLAUSE, AND WHERE EACH ONE LIVES
// ===================================================================================================
//
//   "dash in any direction on a SEPARATE cooldown from the standard dash"
//        The direction is composed by the SAME pure function the standard dash uses
//        (UTraceCharacterMovementComponent::ComputeDashDirection), so "any direction" means exactly
//        what it means for a dash — W/S carry the aim pitch, so straight up and diagonals work.
//        The cooldown is the ability framework's activated cooldown (RoccoRippleCooldownSeconds),
//        which is a different number in a different place from the movement component's dash charge
//        pool. Firing the Ripple does not spend a dash charge and never touches DashTimeRemaining.
//
//   "leaving a ripple behind... any character, EITHER TEAM, may enter the ripple's start and be
//    propelled along the dash's path (including up or diagonally). THE CORE CARRIER CAN USE IT."
//        This actor is that ripple. Entry is asked of the choke point as
//        ETraceAbilityEffect::Beneficial, which is the one effect class spec §4 lets through for a
//        carrier — and it lets teammates and enemies through too. There is no team test here and
//        there must not be one: a second team rule would be a second answer to a question §4 says
//        has exactly one.
//
//   "Players holding guns — Rocco included — CAN SHOOT while riding it, unlike a normal dash."
//        This is why the ride is NOT implemented as a dash. UTraceCharacterMovementComponent::
//        AreWeaponActionsBlocked() is a thin alias of IsDashing(), so anything that went through
//        StartDash() would be silenced for its whole window. The ride writes Velocity directly and
//        never raises the dash flag, so the weapon gate stays open by construction rather than by a
//        special case somebody has to remember. TraceCharacterVerify asserts exactly that.
//
//   "Lasts 4 s, then all effects and visuals vanish."
//        One absolute match-clock deadline (ExpireMatchTime) governs the rides, the rings and the
//        actor's own lifetime. The server destroys the actor at the deadline; the visuals go with it.
//
//   "a short series of rings along the path, with the STARTING RING IN A DIFFERENT COLOUR"
//        Two instanced-static-mesh components, one per colour, so the start ring is a different
//        material instance rather than a per-instance tint that the basic-shape fallback could not
//        honour. RoccoRippleStartRingColor / RoccoRippleTrailRingColor are the knobs.
//
// ===================================================================================================
// THE PREDICTION MODEL, STATED PLAINLY BECAUSE IT IS NOT THE USUAL ONE
// ===================================================================================================
//
// The ride is a per-frame Velocity write, and it runs on TWO kinds of machine:
//
//     the SERVER            for every pawn — authoritative.
//     each CLIENT           for its OWN locally-controlled pawn only — prediction.
//
// Both machines drive the same code from the same replicated inputs (start, direction, length, ride
// speed, deadline), so the local rider's screen and the server agree without a correction on every
// frame. A simulated proxy is left entirely alone: its movement arrives by replication, exactly as
// it does for every other kind of motion in this game.
//
// This deliberately does NOT go through the movement component's saved-move pipeline. Adding a ride
// flag to FSavedMove_Trace is the "right" long-term answer and it is a movement-slice change; it is
// named as a cross-file need in the report rather than smuggled in here.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"     // FVector_NetQuantize
#include "UObject/ObjectPtr.h"

#include "TraceRippleActor.generated.h"

class APlayerState;
class ATraceCharacter;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;

/**
 * One live ripple: a straight path through the world that anybody may ride once, plus its rings.
 *
 * Server-spawned and replicated. Rocco's ability set owns exactly one at a time and destroys it at
 * half time; the actor destroys itself at its deadline.
 */
UCLASS()
class TRACE_API ATraceRippleActor : public AActor
{
	GENERATED_BODY()

public:
	ATraceRippleActor();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * AUTHORITY ONLY. Call immediately after SpawnActor, before anybody can tick.
	 *
	 * @param InSource        Rocco's PlayerState — the instigator the choke point is asked about.
	 * @param InStart         world location of the START ring (the entrance).
	 * @param InDirection     unit direction of the dash's path. Fully 3D.
	 * @param InPathLength    how far the path runs, uu.
	 * @param InRideSpeed     uu/s a rider is propelled at.
	 * @param InEntryRadius   how close to the start a player must be to be picked up.
	 * @param InExpireMatchTime  absolute match-clock time the whole thing vanishes.
	 */
	void InitialiseRipple(APlayerState* InSource, const FVector& InStart, const FVector& InDirection,
	                      float InPathLength, float InRideSpeed, float InEntryRadius, float InExpireMatchTime);

	// --- queries, for the HUD and for the verification harness ------------------------------------

	FVector GetRippleStart() const { return FVector(RippleStart); }
	FVector GetRippleDirection() const { return FVector(RippleDirection); }
	float   GetPathLength() const { return PathLength; }
	float   GetRideSpeed() const { return RideSpeed; }
	float   GetEntryRadius() const { return EntryRadius; }
	float   GetExpireMatchTime() const { return ExpireMatchTime; }

	/** True while @p Candidate is being propelled by this ripple on THIS machine. */
	bool IsRiding(const ATraceCharacter* Candidate) const;

	/** How many pawns this machine is currently propelling. */
	int32 GetRiderCount() const { return Riders.Num(); }

	/** How many pawns have ridden this ripple on this machine, ever. One ride each, by design. */
	int32 GetLifetimeRiderCount() const { return LifetimeRiders.Num(); }

	/** The reason the last entry attempt for @p Candidate was refused, for the harness's logs. */
	static const TCHAR* DescribeEntryRefusal(const ATraceCharacter* Candidate, const APlayerState* Source);

protected:
	/** REPLICATED. The entrance. */
	UPROPERTY(Replicated)
	FVector_NetQuantize RippleStart = FVector::ZeroVector;

	/** REPLICATED. Unit direction of the path — 3D, so "up or diagonally" is not a special case. */
	UPROPERTY(Replicated)
	FVector_NetQuantize RippleDirection = FVector::ZeroVector;

	UPROPERTY(Replicated)
	float PathLength = 0.f;

	UPROPERTY(Replicated)
	float RideSpeed = 0.f;

	UPROPERTY(Replicated)
	float EntryRadius = 0.f;

	/** REPLICATED. Absolute match-clock deadline. See the cooldown contract on the ability component. */
	UPROPERTY(Replicated)
	float ExpireMatchTime = 0.f;

	/** REPLICATED. Rocco's PlayerState, so the choke point has an instigator even after he dies. */
	UPROPERTY(Replicated)
	TObjectPtr<APlayerState> SourcePlayerState = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Root = nullptr;

	/** The START ring. Its own component so it can carry its own colour (§6's "different colour"). */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> StartRingMesh = nullptr;

	/** Every ring after the first. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> TrailRingMesh = nullptr;

private:
	/** One rider's progress along the path, on this machine. */
	struct FRippleRider
	{
		TWeakObjectPtr<ATraceCharacter> Pawn;
		float DistanceTravelled = 0.f;
	};

	/** Who this machine is propelling right now. */
	TArray<FRippleRider> Riders;

	/** Who has already ridden. ONE RIDE PER PLAYER PER RIPPLE — see the [ASSUMPTION] in the .cpp. */
	TArray<TWeakObjectPtr<ATraceCharacter>> LifetimeRiders;

	/** Server + local-prediction half of Tick. */
	void UpdateRides(float DeltaSeconds);

	/** True when this machine is responsible for simulating @p Candidate's movement. */
	bool ShouldSimulate(const ATraceCharacter* Candidate) const;

	/** Cosmetic half. Builds the rings once the replicated path has arrived. */
	void BuildRingsIfNeeded();

	/** Adds one ring of instances to @p Mesh, centred on @p Center, in the plane normal to the path. */
	void AddRing(UInstancedStaticMeshComponent* Mesh, const FVector& Center, const FVector& Direction) const;

	/** The match clock. Identical to the ability component's. */
	float MatchTimeNow() const;

	bool bRingsBuilt = false;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> BeadMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> NeonMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> FallbackMaterial = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> StartRingMID = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> TrailRingMID = nullptr;
};
