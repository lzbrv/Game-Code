// Trace — Mace's roped spike. The anchor and the visual; the rules live in UTraceAbilitySetMace.

#include "Abilities/Characters/TraceMaceSpike.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/Characters/TraceAbilitySetMace.h"
#include "Core/TraceCharacter.h"
#include "Trace.h"

ATraceMaceSpike::ATraceMaceSpike()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;
	// The flight is short and the anchor is replicated, so every machine can put the spike in the
	// right place from AnchorLocation alone once it lands. Replicating movement as well would spend
	// bandwidth on 0.4 s of interpolation nobody looks at.
	SetReplicateMovement(false);
	bAlwaysRelevant = true;

	USceneComponent* SpikeRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SpikeRoot);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(SpikeRoot);
	// NO COLLISION. See the header: a collider here would block bullets and break jars.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCollisionProfileName(TEXT("NoCollision"));
	Mesh->SetCastShadow(false);
	Mesh->SetRelativeScale3D(FVector(0.28f, 0.28f, 0.28f));

	Rope = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Rope"));
	Rope->SetupAttachment(SpikeRoot);
	Rope->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Rope->SetCollisionProfileName(TEXT("NoCollision"));
	Rope->SetCastShadow(false);
	Rope->SetVisibility(false);

	// ConstructorHelpers, not a runtime LoadObject, for the reason ATraceCore's constructor gives:
	// a runtime load of an engine basic shape resolves to null once cooked unless something has
	// already referenced it.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (ConeFinder.Succeeded())
	{
		Mesh->SetStaticMesh(ConeFinder.Object);
	}
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		Rope->SetStaticMesh(CylinderFinder.Object);
	}
}

void ATraceMaceSpike::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceMaceSpike, AnchorLocation);
	DOREPLIFETIME(ATraceMaceSpike, bEmbedded);
}

void ATraceMaceSpike::BeginPlay()
{
	Super::BeginPlay();

	if (const UWorld* WorldPtr = GetWorld())
	{
		SpawnWorldTime = WorldPtr->GetTimeSeconds();
	}
}

void ATraceMaceSpike::InitialiseFlight(UTraceAbilitySetMace* InOwnerSet, const FVector& InAnchor,
                                       float InTravelSpeed, float InBackstopLifetimeSeconds)
{
	OwnerSet = InOwnerSet;
	AnchorLocation = InAnchor;
	TravelSpeed = InTravelSpeed;
	BackstopLifetimeSeconds = FMath::Max(1.f, InBackstopLifetimeSeconds);

	if (TravelSpeed <= 0.f)
	{
		SetActorLocation(AnchorLocation);
		bEmbedded = true;
	}
}

void ATraceMaceSpike::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UpdateRope();

	if (!HasAuthority())
	{
		// A client's copy learns bEmbedded and AnchorLocation by replication. Snap to the anchor the
		// moment it is embedded so the visual never sits at a stale interpolated point.
		if (bEmbedded && !GetActorLocation().Equals(AnchorLocation, 1.f))
		{
			SetActorLocation(AnchorLocation);
		}
		return;
	}

	const UWorld* WorldPtr = GetWorld();
	if (WorldPtr != nullptr && (WorldPtr->GetTimeSeconds() - SpawnWorldTime) > BackstopLifetimeSeconds)
	{
		// The backstop, not the ability's timer. Mace clears her own spike on every legitimate route;
		// this only catches a spike whose owner vanished mid-flight.
		UE_LOG(LogTraceGame, Verbose, TEXT("[Mace] Spike hit its backstop lifetime and destroyed itself."));
		Destroy();
		return;
	}

	if (bEmbedded)
	{
		return;
	}

	const FVector Here = GetActorLocation();
	const FVector ToAnchor = AnchorLocation - Here;
	const float Remaining = ToAnchor.Size();
	const float Step = TravelSpeed * DeltaSeconds;

	if (Remaining <= Step || Remaining <= KINDA_SMALL_NUMBER)
	{
		SetActorLocation(AnchorLocation);
		bEmbedded = true;

		if (UTraceAbilitySetMace* Mace = OwnerSet.Get())
		{
			Mace->NotifySpikeEmbedded(this);
		}
		return;
	}

	SetActorLocation(Here + (ToAnchor / Remaining) * Step);
}

void ATraceMaceSpike::UpdateRope()
{
	// Cosmetic on every machine, and deliberately driven off whoever the ability component says the
	// pawn is rather than off a replicated pointer of our own: the rope is allowed to be a frame late.
	const UTraceAbilitySetMace* Mace = OwnerSet.Get();
	const ATraceCharacter* MacePawn = (Mace != nullptr) ? Mace->GetCharacter() : nullptr;
	if (Rope == nullptr)
	{
		return;
	}

	if (MacePawn == nullptr)
	{
		Rope->SetVisibility(false);
		return;
	}

	const FVector From = MacePawn->GetActorLocation();
	const FVector To = GetActorLocation();
	const FVector Delta = To - From;
	const float Length = Delta.Size();
	if (Length < 10.f)
	{
		Rope->SetVisibility(false);
		return;
	}

	// The engine cylinder is 100 uu tall about its centre and Z-aligned, so half the delta as the
	// offset and Length/100 as the Z scale puts it exactly between the two points.
	Rope->SetVisibility(true);
	Rope->SetWorldLocation(From + Delta * 0.5f);
	Rope->SetWorldRotation(Delta.Rotation() + FRotator(90.f, 0.f, 0.f));
	Rope->SetWorldScale3D(FVector(0.04f, 0.04f, Length / 100.f));
}
