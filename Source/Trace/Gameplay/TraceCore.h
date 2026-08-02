// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TraceTypes.h"
#include "TraceCore.generated.h"

class ATraceCharacter;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPrimitiveComponent;
class UProjectileMovementComponent;
class USphereComponent;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * The single contested objective.
 *
 * There is exactly one Core in the match and BOTH teams fight over it - anyone may pick it up or
 * intercept a pass, teammate or enemy. Picking it up is the single event that turns a player into
 * "the carrier": it drives ATraceCharacter::SetCarrying(true) (invulnerability + no gun),
 * ATracePlayerState::bIsCarrier (HUD / scoreboard) and UTraceTrailComponent::SetEmitting(true)
 * (the trail that dashing enemies can trip). Dropping, throwing or resetting undoes all three.
 *
 * Networking:
 *  - Server-authoritative. Every mutator early-outs unless HasAuthority(). The Core is always
 *    spawned by the GameMode, so HasAuthority() really does mean "server" for this class.
 *  - Movement replication is toggled: ON while it is loose or in flight (the only times the server
 *    actually moves it), OFF while carried, because attachment replication already puts it on the
 *    carrier and ReplicatedMovement would be pure wasted bandwidth. Attachment replicates
 *    independently of bReplicateMovement (AActor::PreReplication gathers it whenever the root is
 *    attached), so detaching still reaches clients correctly.
 */
UCLASS()
class TRACE_API ATraceCore : public AActor
{
	GENERATED_BODY()

public:
	ATraceCore();

	//~ Contract surface (spec section 7)

	/** The character currently holding the Core, or null. Replicated so clients can tint/attach. */
	UPROPERTY(ReplicatedUsing = OnRep_Carrier)
	TObjectPtr<class ATraceCharacter> Carrier = nullptr;

	UPROPERTY(Replicated)
	ECoreState State = ECoreState::Loose;

	/** Server. Attempts to give the Core to Character. Fails silently if it is already carried, the
	 *  character is dead, or the character is still inside their own post-throw pickup lockout. */
	void TryPickup(class ATraceCharacter* Character);

	/** Server. Releases the current carrier and launches the Core. Speed <= 0 uses PassSpeed. */
	void Throw(const FVector& Direction, float Speed);

	/** Server. Releases the current carrier and puts the Core at Location with the given velocity. */
	void DropAt(const FVector& Location, const FVector& Impulse);

	/** Server. Releases any carrier and returns the Core to the arena pedestal. */
	void ResetToCenter();

	class ATraceCharacter* GetCarrier() const;

	UFUNCTION()
	void OnRep_Carrier();

	//~ End contract surface

	//~ AActor
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	//~ End AActor

	/** True while Character is the player who last threw/dropped the Core and the lockout is live. */
	bool IsPickupLockedOutFor(const class ATraceCharacter* Character) const;

	/** Arena centre this Core resets to. Resolved from the arena builder, falling back to spawn. */
	FVector GetHomeLocation() const;

	/** Root collision. Swept by the projectile movement component; blocks world, ignores pawns. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Core")
	TObjectPtr<USphereComponent> Sphere;

	/** Overlap-only volume sized to UTraceSettings::PickupRadius. Drives pickup/interception. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Core")
	TObjectPtr<USphereComponent> PickupSphere;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Core")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Core")
	TObjectPtr<UProjectileMovementComponent> Projectile;

protected:
	virtual void BeginPlay() override;

	UFUNCTION()
	void OnPickupSphereBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void OnProjectileStopped(const FHitResult& ImpactResult);

private:
	/** Server. Sets State and re-applies every state-dependent side effect. */
	void SetState(ECoreState NewState);

	/** Applies collision / projectile / movement-replication state. Runs on server AND clients. */
	void ApplyCoreState();

	/** Recolours the Core for its current state. No-op on a dedicated server. */
	void UpdateVisuals();

	/** Server. Clears the current carrier's carrying/trail state and detaches. */
	void ReleaseCarrier();

	/** Server. Teleports to Location and (re)starts projectile simulation with NewVelocity. */
	void LaunchFrom(const FVector& Location, const FVector& NewVelocity);

	/** Server. Nearest valid character inside PickupSphere gets the Core. */
	void ServerScanForPickup();

	UPROPERTY()
	TObjectPtr<UMaterialInterface> BaseMaterial = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> MeshMID = nullptr;

	/** Whoever last released the Core; blocked from re-catching until PickupLockoutEndTime. */
	TWeakObjectPtr<class ATraceCharacter> LastThrower;

	float PickupLockoutEndTime = 0.f;
	float LooseSinceTime = 0.f;
	float FlightStartTime = 0.f;
	FVector SpawnHomeLocation = FVector::ZeroVector;

	/** Last state we actually applied locally. State has no OnRep by contract, and Carrier/State
	 *  can arrive in either order, so clients reconcile from Tick as well as OnRep_Carrier. */
	ECoreState AppliedState = ECoreState::Loose;
	TWeakObjectPtr<class ATraceCharacter> AppliedCarrier;
	bool bStateEverApplied = false;
};
