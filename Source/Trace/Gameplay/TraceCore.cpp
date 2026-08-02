// Copyright (c) Trace. All Rights Reserved.

#include "Gameplay/TraceCore.h"

#include "Net/UnrealNetwork.h"

#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceTrailComponent.h"
#include "World/TraceArenaBuilder.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/StaticMesh.h"
#include "Math/NumericLimits.h"                 // TNumericLimits<double> (ServerScanForPickup)
#include "Engine/World.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace TraceCoreConstants
{
	/** Physical radius of the Core. The Sphere basic shape is 100uu across, hence the 0.8 scale. */
	static constexpr float CollisionRadius = 40.f;
	static constexpr float MeshScale = 0.8f;

	/** Where the Core floats relative to the carrier's capsule (forward / right / up). */
	static const FVector CarryOffset(70.f, 0.f, 55.f);

	/** Hard ceiling on how long the Core may stay in ECoreState::InFlight before we call it loose.
	 *  OnProjectileStop is the normal path; this only catches pathological cases (falling out of
	 *  the world, a bounce loop) so the reset-to-centre timer can never be starved. */
	static constexpr float MaxFlightSeconds = 12.f;

	/** The Core will not reset to centre if it is already essentially at home. */
	static constexpr double HomeToleranceSq = 75.0 * 75.0;

	static const FLinearColor LooseColor(0.90f, 0.92f, 0.96f);
	static const FLinearColor FlightColor(1.00f, 0.90f, 0.45f);
}

ATraceCore::ATraceCore()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;
	SetReplicateMovement(true);
	SetCanBeDamaged(false);

	Sphere = CreateDefaultSubobject<USphereComponent>(TEXT("Sphere"));
	SetRootComponent(Sphere);
	Sphere->InitSphereRadius(TraceCoreConstants::CollisionRadius);
	Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Sphere->SetCollisionObjectType(ECC_WorldDynamic);
	Sphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	// Block only static/dynamic world geometry so the projectile component has something to bounce
	// off. Deliberately ignores Pawn (players run through it, which is what triggers a pickup) and
	// Visibility (a loose Core must never eat a bullet meant for a player).
	Sphere->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Sphere->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	Sphere->SetGenerateOverlapEvents(false);
	Sphere->SetCanEverAffectNavigation(false);
	Sphere->CanCharacterStepUpOn = ECB_No;

	PickupSphere = CreateDefaultSubobject<USphereComponent>(TEXT("PickupSphere"));
	PickupSphere->SetupAttachment(Sphere);
	PickupSphere->InitSphereRadius(110.f); // Overwritten from UTraceSettings::PickupRadius in BeginPlay.
	PickupSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PickupSphere->SetCollisionObjectType(ECC_WorldDynamic);
	PickupSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	PickupSphere->SetGenerateOverlapEvents(true);
	PickupSphere->SetCanEverAffectNavigation(false);
	PickupSphere->CanCharacterStepUpOn = ECB_No;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Sphere);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCollisionProfileName(TEXT("NoCollision"));
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->SetRelativeScale3D(FVector(TraceCoreConstants::MeshScale));

	// Constructor-time FObjectFinders are what make these engine assets cook into a packaged
	// build - a bare runtime LoadObject would resolve to null once cooked (see spec section 2).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMeshFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMeshFinder.Succeeded())
	{
		Mesh->SetStaticMesh(SphereMeshFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BaseMaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BaseMaterialFinder.Succeeded())
	{
		BaseMaterial = BaseMaterialFinder.Object;
	}

	Projectile = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Projectile"));
	Projectile->InitialSpeed = 0.f;                            // We set Velocity explicitly on throw.
	Projectile->MaxSpeed = 6000.f;                             // Must stay > 0; 0 would clamp to zero.
	Projectile->bShouldBounce = true;
	Projectile->Bounciness = 0.35f;
	Projectile->Friction = 0.40f;
	Projectile->BounceVelocityStopSimulatingThreshold = 40.f;
	Projectile->ProjectileGravityScale = 1.f;
	Projectile->bRotationFollowsVelocity = false;
	Projectile->SetComponentTickEnabled(false);                // Enabled by ApplyCoreState().
}

void ATraceCore::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceCore, Carrier);
	DOREPLIFETIME(ATraceCore, State);
}

void ATraceCore::BeginPlay()
{
	Super::BeginPlay();

	const UTraceSettings& Settings = UTraceSettings::Get();

	if (PickupSphere)
	{
		PickupSphere->SetSphereRadius(FMath::Max(TraceCoreConstants::CollisionRadius, Settings.PickupRadius), true);
		PickupSphere->OnComponentBeginOverlap.AddDynamic(this, &ATraceCore::OnPickupSphereBeginOverlap);
	}

	if (Projectile)
	{
		Projectile->OnProjectileStop.AddDynamic(this, &ATraceCore::OnProjectileStopped);
	}

	SpawnHomeLocation = GetActorLocation();

	if (const UWorld* World = GetWorld())
	{
		LooseSinceTime = World->GetTimeSeconds();
	}

	// Material work is pointless (and unreliable - shaders are not cooked for server targets) on a
	// dedicated server, so it is skipped there entirely.
	if (Mesh && BaseMaterial && GetNetMode() != NM_DedicatedServer)
	{
		MeshMID = Mesh->CreateDynamicMaterialInstance(0, BaseMaterial);
	}

	ApplyCoreState();
	UpdateVisuals();
}

void ATraceCore::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Carrier and State are two independent replicated properties and can land in either order, and
	// State intentionally has no OnRep (the contract declares it as a plain Replicated property).
	// Reconciling here keeps clients correct no matter which bunch arrives first.
	if (!bStateEverApplied || AppliedState != State || AppliedCarrier.Get() != Carrier)
	{
		ApplyCoreState();
		UpdateVisuals();
	}

	if (!HasAuthority())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	const UTraceSettings& Settings = UTraceSettings::Get();

	switch (State)
	{
	case ECoreState::Carried:
		// Safety net: if the carrier is destroyed or dies without anyone telling us (seamless
		// travel, a disconnect mid-carry), the Core must not vanish with them.
		if (!IsValid(Carrier) || !Carrier->IsAlive())
		{
			const FVector DropLocation = IsValid(Carrier) ? Carrier->GetActorLocation() : GetActorLocation();
			DropAt(DropLocation, FVector::ZeroVector);
		}
		break;

	case ECoreState::InFlight:
		ServerScanForPickup();
		if (Now - FlightStartTime > TraceCoreConstants::MaxFlightSeconds)
		{
			SetState(ECoreState::Loose);
		}
		break;

	case ECoreState::Loose:
		ServerScanForPickup();
		if (Settings.CoreResetTime > 0.f
			&& (Now - LooseSinceTime) >= Settings.CoreResetTime
			&& FVector::DistSquared(GetActorLocation(), GetHomeLocation()) > TraceCoreConstants::HomeToleranceSq)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("Core idle for %.1fs while loose - resetting to centre."), Settings.CoreResetTime);
			ResetToCenter();
		}
		break;

	default:
		break;
	}
}

class ATraceCharacter* ATraceCore::GetCarrier() const
{
	return Carrier;
}

FVector ATraceCore::GetHomeLocation() const
{
	// Resolved lazily rather than cached at BeginPlay: the GameMode may spawn the arena builder and
	// the Core in either order, and a level-placed builder may not have begun play yet either.
	if (const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(GetWorld()))
	{
		return Arena->GetCoreSpawnLocation();
	}
	return SpawnHomeLocation;
}

bool ATraceCore::IsPickupLockedOutFor(const class ATraceCharacter* Character) const
{
	if (Character == nullptr || LastThrower.Get() != Character)
	{
		return false;
	}

	const UWorld* World = GetWorld();
	return World != nullptr && World->GetTimeSeconds() < PickupLockoutEndTime;
}

void ATraceCore::TryPickup(class ATraceCharacter* Character)
{
	if (!HasAuthority() || !IsValid(Character))
	{
		return;
	}

	// Anyone may take a loose or in-flight Core - teammate or enemy. Interception is a feature.
	if (State == ECoreState::Carried || Character == Carrier)
	{
		return;
	}

	if (!Character->IsAlive() || IsPickupLockedOutFor(Character))
	{
		return;
	}

	Carrier = Character;

	// Attach first, THEN flip state: ApplyCoreState() turns movement replication off, and we want
	// the detach/attach change to be the last transform-relevant thing the server publishes.
	Projectile->StopMovementImmediately();
	AttachToActor(Character, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	SetActorRelativeLocation(TraceCoreConstants::CarryOffset);
	SetActorRelativeRotation(FRotator::ZeroRotator);

	// The three pieces of carrier state the rest of the game keys off. All server-authoritative,
	// all replicated independently by their owners.
	Character->SetCarrying(true);
	if (ATracePlayerState* CarrierState = Character->GetPlayerState<ATracePlayerState>())
	{
		CarrierState->bIsCarrier = true;
	}
	if (Character->Trail)
	{
		Character->Trail->SetEmitting(true);
	}

	SetState(ECoreState::Carried);

	// OnRep_* never fires on the authority, so drive the local view by hand.
	OnRep_Carrier();

	UE_LOG(LogTraceGame, Verbose, TEXT("Core picked up by %s"), *GetNameSafe(Character));
}

void ATraceCore::Throw(const FVector& Direction, float Speed)
{
	if (!HasAuthority())
	{
		return;
	}

	ATraceCharacter* Thrower = Carrier;
	if (!IsValid(Thrower))
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();

	FVector Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = Thrower->GetActorForwardVector();
	}

	// PassUpwardBias is a fraction of a unit vector added straight up before renormalising, so even
	// a perfectly flat aim lobs slightly and clears the floor instead of scraping along it.
	Dir = (Dir + FVector::UpVector * Settings.PassUpwardBias).GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = FVector::UpVector;
	}

	// Literal epsilon rather than KINDA_SMALL_NUMBER: the un-prefixed math macros were re-spelled
	// during the 5.x line and every other file in this module deliberately avoids them so the module
	// compiles unchanged on 5.4 - 5.8.
	const float FinalSpeed = (Speed > 1.e-4f) ? Speed : Settings.PassSpeed;
	const FVector LaunchLocation = GetActorLocation();

	// Lockout applies to the thrower only - everyone else may intercept the pass immediately.
	// GetWorld() can be null during teardown (Logout, level transition), which is exactly when
	// Throw/DropAt still get called from the destroyed-carrier safety net.
	LastThrower = Thrower;
	PickupLockoutEndTime = 0.f;
	if (const UWorld* ThrowWorld = GetWorld())
	{
		PickupLockoutEndTime = ThrowWorld->GetTimeSeconds() + FMath::Max(0.f, Settings.PickupLockoutAfterThrow);
	}

	ReleaseCarrier();
	SetState(ECoreState::InFlight);
	LaunchFrom(LaunchLocation, Dir * FinalSpeed);

	UE_LOG(LogTraceGame, Verbose, TEXT("Core thrown by %s at %.0f uu/s"), *GetNameSafe(Thrower), FinalSpeed);
}

void ATraceCore::DropAt(const FVector& Location, const FVector& Impulse)
{
	if (!HasAuthority())
	{
		return;
	}

	ATraceCharacter* Dropper = Carrier;

	ReleaseCarrier();

	// Same lockout as a throw, and for the same reason: the player who just lost the Core must not
	// re-collect it in the same frame. It never applies to anyone else.
	if (Dropper != nullptr)
	{
		LastThrower = Dropper;
		PickupLockoutEndTime = 0.f;
		if (const UWorld* DropWorld = GetWorld())
		{
			PickupLockoutEndTime = DropWorld->GetTimeSeconds() + FMath::Max(0.f, UTraceSettings::Get().PickupLockoutAfterThrow);
		}
	}

	// Enter InFlight even with a zero impulse so gravity settles it onto the floor; OnProjectileStop
	// flips it to Loose (and starts the reset countdown) the moment it comes to rest.
	SetState(ECoreState::InFlight);
	LaunchFrom(Location, Impulse);

	UE_LOG(LogTraceGame, Verbose, TEXT("Core dropped at %s"), *Location.ToCompactString());
}

void ATraceCore::ResetToCenter()
{
	if (!HasAuthority())
	{
		return;
	}

	ReleaseCarrier();

	LastThrower = nullptr;
	PickupLockoutEndTime = 0.f;

	SetState(ECoreState::Loose);

	if (Projectile)
	{
		// StopSimulating() nulls UpdatedComponent when the Core comes to rest, so rebind before use.
		if (Projectile->UpdatedComponent == nullptr)
		{
			Projectile->SetUpdatedComponent(Sphere);
		}
		Projectile->StopMovementImmediately();
	}

	SetActorLocation(GetHomeLocation(), false, nullptr, ETeleportType::TeleportPhysics);
	SetActorRotation(FRotator::ZeroRotator);

	if (const UWorld* World = GetWorld())
	{
		LooseSinceTime = World->GetTimeSeconds();
	}

	OnRep_Carrier();
}

void ATraceCore::OnRep_Carrier()
{
	ApplyCoreState();
	UpdateVisuals();
}

void ATraceCore::OnPickupSphereBeginOverlap(UPrimitiveComponent* /*OverlappedComponent*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/, const FHitResult& /*SweepResult*/)
{
	if (!HasAuthority())
	{
		return;
	}

	// Immediate path so a fast pass is caught on the frame it arrives; ServerScanForPickup() is the
	// polling backstop for players who were already standing on the Core (no new overlap fires).
	TryPickup(Cast<ATraceCharacter>(OtherActor));
}

void ATraceCore::OnProjectileStopped(const FHitResult& /*ImpactResult*/)
{
	if (!HasAuthority() || State != ECoreState::InFlight)
	{
		return;
	}

	SetState(ECoreState::Loose);
}

void ATraceCore::SetState(ECoreState NewState)
{
	State = NewState;

	if (NewState == ECoreState::Loose)
	{
		if (const UWorld* World = GetWorld())
		{
			LooseSinceTime = World->GetTimeSeconds();
		}
	}

	ApplyCoreState();
	UpdateVisuals();
}

void ATraceCore::ApplyCoreState()
{
	AppliedState = State;
	AppliedCarrier = Carrier;
	bStateEverApplied = true;

	const bool bCarried = (State == ECoreState::Carried);

	if (Projectile)
	{
		// Simulate on the AUTHORITY only. UProjectileMovementComponent::TickComponent has no net-role
		// check of its own, so leaving it enabled on a client makes that client integrate gravity and
		// bounces locally (from a Velocity that is never replicated) while bReplicateMovement also
		// snaps the actor to the server transform - the two fight and the Core visibly jitters for the
		// whole flight. Worse, a client-side bounce-stop calls StopSimulating(), which nulls that
		// client's UpdatedComponent permanently (only the server-only LaunchFrom rebinds it).
		// While carried, a ticking component would also fight the attachment by moving the root in
		// world space, so it stays off on every machine in that case.
		const bool bShouldSimulate = !bCarried && HasAuthority();
		if (bShouldSimulate)
		{
			Projectile->SetComponentTickEnabled(true);
		}
		else
		{
			Projectile->StopMovementImmediately();
			Projectile->SetComponentTickEnabled(false);
		}
	}

	if (Sphere)
	{
		Sphere->SetCollisionEnabled(bCarried ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryOnly);
	}
	if (PickupSphere)
	{
		PickupSphere->SetCollisionEnabled(bCarried ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryOnly);
	}

	if (HasAuthority())
	{
		// Movement replication is only worth paying for when the server is the thing moving the
		// Core. Carried => attached, and attachment replicates on its own.
		SetReplicateMovement(!bCarried);
	}
}

void ATraceCore::UpdateVisuals()
{
	if (MeshMID == nullptr)
	{
		return;
	}

	FLinearColor Color = TraceCoreConstants::LooseColor;
	if (State == ECoreState::Carried && IsValid(Carrier))
	{
		Color = TraceTeamColor(Carrier->GetTeam());
	}
	else if (State == ECoreState::InFlight)
	{
		Color = TraceCoreConstants::FlightColor;
	}

	MeshMID->SetVectorParameterValue(TEXT("Color"), Color);     // BasicShapeMaterial's real parameter.
	MeshMID->SetVectorParameterValue(TEXT("BaseColor"), Color); // Harmless no-op if absent.
}

void ATraceCore::ReleaseCarrier()
{
	ATraceCharacter* Previous = Carrier;
	Carrier = nullptr;

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

	if (Previous == nullptr)
	{
		return;
	}

	// Undo exactly the three things a pickup set. Note we deliberately only stop *emitting* here:
	// wiping the existing trail on a voluntary pass would delete the counterplay the enemy team
	// already earned. Death-time clearing is ATraceCharacter::HandleDeath's job.
	Previous->SetCarrying(false);
	if (ATracePlayerState* PreviousState = Previous->GetPlayerState<ATracePlayerState>())
	{
		PreviousState->bIsCarrier = false;
	}
	if (Previous->Trail)
	{
		Previous->Trail->SetEmitting(false);
	}
}

void ATraceCore::LaunchFrom(const FVector& Location, const FVector& NewVelocity)
{
	SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);

	if (Projectile)
	{
		// UProjectileMovementComponent::StopSimulating() clears UpdatedComponent when the Core comes
		// to rest, and HasStoppedSimulation() also keys off IsActive(). Restore both before launch.
		if (Projectile->UpdatedComponent == nullptr)
		{
			Projectile->SetUpdatedComponent(Sphere);
		}
		if (!Projectile->IsActive())
		{
			Projectile->Activate(true);
		}
		Projectile->SetComponentTickEnabled(true);
		Projectile->Velocity = NewVelocity;
		Projectile->UpdateComponentVelocity();
	}

	if (const UWorld* World = GetWorld())
	{
		FlightStartTime = World->GetTimeSeconds();
	}
}

void ATraceCore::ServerScanForPickup()
{
	if (PickupSphere == nullptr || State == ECoreState::Carried)
	{
		return;
	}

	TArray<AActor*> Overlapping;
	PickupSphere->GetOverlappingActors(Overlapping, ATraceCharacter::StaticClass());
	if (Overlapping.Num() == 0)
	{
		return;
	}

	const FVector CoreLocation = GetActorLocation();
	ATraceCharacter* Best = nullptr;
	double BestDistSq = TNumericLimits<double>::Max();

	for (AActor* Candidate : Overlapping)
	{
		ATraceCharacter* Character = Cast<ATraceCharacter>(Candidate);
		if (!IsValid(Character) || !Character->IsAlive() || IsPickupLockedOutFor(Character))
		{
			continue;
		}

		const double DistSq = FVector::DistSquared(CoreLocation, Character->GetActorLocation());
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Character;
		}
	}

	if (Best != nullptr)
	{
		TryPickup(Best);
	}
}
