// Trace — the player pawn.
//
// Deliberately simple in shape and strict about collision: the capsule is the ONLY collider on
// this actor (ECC_Pawn). The cylinder/sphere/cone meshes are visual-only and set to NoCollision,
// so nothing can hit "the mesh" instead of "the character" — hitscan resolution and the trail
// trip test both reason purely about the capsule, and lag compensation records only the capsule
// pose. Adding a colliding component here would quietly break all three.
//
// Aiming and facing are separate on purpose: bUseControllerRotationYaw stays off and the movement
// component orients the capsule to the movement direction (third-person feel), while the shot
// direction comes from the control rotation via GetAimDirection().

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"        // FTimerHandle, EEndPlayReason
#include "Engine/NetSerialization.h"   // FVector_NetQuantizeNormal (ServerPass payload)
#include "GameFramework/Character.h"
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"                // ETraceTeam

#include "TraceCharacter.generated.h"

class AController;
class UCameraComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USpringArmComponent;
class UStaticMeshComponent;
class UTraceCharacterMovementComponent;
class UTraceHealthComponent;
class UTraceLagCompensationComponent;
class UTraceTrailComponent;
class UTraceWeaponComponent;

UCLASS()
class TRACE_API ATraceCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	/**
	 * The FObjectInitializer overload is mandatory: it is the only way to swap ACharacter's
	 * default movement component for UTraceCharacterMovementComponent, and GENERATED_BODY() does
	 * not declare it for us. Without it the engine silently uses the stock CMC and the dash
	 * prediction disappears.
	 */
	ATraceCharacter(const FObjectInitializer& OI);

	// --- Components ----------------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, Category = "Trace|Camera")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Camera")
	TObjectPtr<UCameraComponent> Camera;

	/** Cylinder. Visual only, NoCollision. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Visual")
	TObjectPtr<UStaticMeshComponent> BodyMesh;

	/** Sphere. Visual only, NoCollision. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Visual")
	TObjectPtr<UStaticMeshComponent> HeadMesh;

	/** Cone pointing along +X, so you can read someone's facing at a glance. Visual only. */
	UPROPERTY(VisibleAnywhere, Category = "Trace|Visual")
	TObjectPtr<UStaticMeshComponent> NoseMesh;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Components")
	TObjectPtr<UTraceHealthComponent> Health;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Components")
	TObjectPtr<UTraceWeaponComponent> Weapon;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Components")
	TObjectPtr<UTraceTrailComponent> Trail;

	UPROPERTY(VisibleAnywhere, Category = "Trace|Components")
	TObjectPtr<UTraceLagCompensationComponent> LagComp;

	// --- Replicated state ----------------------------------------------------------------------

	/** Set by ATraceCore through SetCarrying(). Drives invulnerability, the trail and the tint. */
	UPROPERTY(ReplicatedUsing = OnRep_IsCarrier)
	bool bIsCarrier = false;

	// --- AActor / ACharacter ---------------------------------------------------------------------

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void FellOutOfWorld(const class UDamageType& DmgType) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// --- Queries ---------------------------------------------------------------------------------

	/** Team from the PlayerState, or None while it has not replicated yet. */
	ETraceTeam GetTeam() const;
	bool IsCarrier() const;
	bool IsAlive() const;

	/** True during the dash window. The trail trip test keys off this — see contract §3. */
	bool IsDashing() const;

	UTraceCharacterMovementComponent* GetTraceMovement() const;

	// --- Server-authoritative state changes -------------------------------------------------------

	/** Server only. Flips carrier state and pulls the trail/weapon/PlayerState along with it. */
	void SetCarrying(bool bNewCarrying);

	/** Server only. Called from the health component's OnDeath; notifies the GameMode. */
	void HandleDeath(AController* Killer, FName Cause);

	// --- Presentation -----------------------------------------------------------------------------

	/**
	 * (Re)builds the team-coloured MIDs. Idempotent and safe to call from BeginPlay,
	 * OnRep_PlayerState, OnRep_Team (ATracePlayerState calls it) and OnRep_IsCarrier — team data
	 * arrives in an order nobody can rely on, so every path that could learn it calls this.
	 * No-op on a dedicated server.
	 */
	void ApplyTeamColors();

	/** Where bullets leave from. Chest height, slightly ahead of the capsule, yaw-aligned to aim. */
	FVector GetMuzzleLocation() const;

	/** Unit aim direction, corrected so the shot converges on what the crosshair covers. */
	FVector GetAimDirection() const;

	UFUNCTION()
	void OnRep_IsCarrier();

	// --- Input entry points (called by ATracePlayerController) --------------------------------------

	/** @param Value X = strafe (+right), Y = forward (+forward), already in -1..1. */
	void DoMove(const FVector2D& Value);

	/** @param Value X = yaw delta, Y = pitch delta, already sign-corrected by the mapping context. */
	void DoLook(const FVector2D& Value);

	void DoFirePressed();
	void DoFireReleased();
	void DoPass();
	void DoDash();

protected:
	/**
	 * UTraceHealthComponent drives SetDeadPresentation() straight out of OnRep_Health, which is the
	 * only place that knows about a health change on every machine at once.
	 */
	friend class UTraceHealthComponent;

	/** Bound to UTraceHealthComponent::OnDeath on the server; funnels into HandleDeath(). */
	UFUNCTION()
	void OnHealthDeath(AActor* Victim, AController* Killer, FName Cause);

	/**
	 * Freezes (or unfreezes) the pawn for death/respawn: movement disabled, capsule collision off,
	 * corpse dimmed.
	 *
	 * Driven from UTraceHealthComponent::OnRep_Health rather than from a multicast RPC, so the
	 * server, every connected client and any client that joins *after* the death all reach the same
	 * state from the replicated health value alone. A multicast would leave a late joiner with a
	 * corpse that still blocks movement, which the server would then have to correct away.
	 * Idempotent.
	 */
	void SetDeadPresentation(bool bDead);

	/**
	 * Passes the Core along the client's aim. Reliable because it is a state change, not an
	 * effect; the direction is quantised per contract §9.5 and re-validated server-side.
	 */
	UFUNCTION(Server, Reliable)
	void ServerPass(FVector_NetQuantizeNormal Direction);

	/** Server-side half of DoPass(), shared by the RPC and the listen-host path. */
	void PerformPass(const FVector& Direction);

	/** Creates the MID on first use, then just pushes the colour. */
	void ApplyColorToMesh(UStaticMeshComponent* Mesh, TObjectPtr<UMaterialInstanceDynamic>& InOutMID, const FLinearColor& InColor);

	/** Retry hook for the team colour: PlayerState/Team can replicate after the pawn exists. */
	void PollTeamColors();

private:
	/** Engine basic-shape material, resolved in the constructor so the cooker keeps it. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> BasicShapeMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BodyMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> HeadMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> NoseMID;

	/** Latches so one life produces exactly one death, however many sources fire at once. */
	bool bDeathHandled = false;

	/** Current state of SetDeadPresentation(), so repeated calls are free and cannot double-apply. */
	bool bDeadPresentation = false;

	FTimerHandle TeamColorTimerHandle;
	int32 TeamColorAttempts = 0;

	/** Server-side throttle for ServerPass, which is reliable and otherwise unbounded. */
	static constexpr float MinPassRequestInterval = 0.1f;
	float LastPassRequestTime = -1000.f;
};
