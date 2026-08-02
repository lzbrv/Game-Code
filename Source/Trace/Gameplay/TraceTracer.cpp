#include "Gameplay/TraceTracer.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "UObject/ConstructorHelpers.h"

#include "Trace.h"

ATraceTracer::ATraceTracer()
{
	PrimaryActorTick.bCanEverTick = false;

	// Cosmetic-only: each machine spawns its own, so this must never go on the wire.
	bReplicates = false;
	SetCanBeDamaged(false);
	InitialLifeSpan = TracerLifeSeconds;

	TracerMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TracerMesh"));
	SetRootComponent(TracerMesh);
	TracerMesh->SetMobility(EComponentMobility::Movable);
	TracerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TracerMesh->SetCollisionProfileName(TEXT("NoCollision"));
	TracerMesh->SetGenerateOverlapEvents(false);
	TracerMesh->SetCastShadow(false);
	TracerMesh->bReceivesDecals = false;

	// Constructor-time FObjectFinder (not a runtime load) so the cooker keeps these engine assets.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		TracerMesh->SetStaticMesh(CylinderFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (MaterialFinder.Succeeded())
	{
		TracerMaterial = MaterialFinder.Object;
	}
}

ATraceTracer* ATraceTracer::Spawn(UWorld* World, const FVector& From, const FVector& To, const FLinearColor& Color)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	// A headless server has nobody to show this to.
	if (World->GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}

	if (From.ContainsNaN() || To.ContainsNaN())
	{
		return nullptr;
	}

	if ((To - From).SizeSquared() < static_cast<double>(MinTracerLengthUU) * static_cast<double>(MinTracerLengthUU))
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	ATraceTracer* Tracer = World->SpawnActor<ATraceTracer>(ATraceTracer::StaticClass(), FTransform::Identity, SpawnParams);
	if (Tracer != nullptr)
	{
		Tracer->InitTracer(From, To, Color);
	}
	return Tracer;
}

void ATraceTracer::InitTracer(const FVector& From, const FVector& To, const FLinearColor& Color)
{
	const FVector Delta = To - From;
	const double Length = Delta.Size();
	if (Length < static_cast<double>(MinTracerLengthUU))
	{
		return;
	}

	// The basic cylinder runs along local +Z, is BasicShapeExtentUU tall and centred on its own
	// origin: sit the actor at the midpoint of the shot, aim local +Z down the shot, and scale so
	// the mesh spans exactly [From, To].
	const FVector Dir = Delta / Length;
	SetActorLocationAndRotation(From + Delta * 0.5, FRotationMatrix::MakeFromZ(Dir).ToQuat());
	const double Thin = static_cast<double>(TracerThicknessUU) / static_cast<double>(BasicShapeExtentUU);
	SetActorScale3D(FVector(Thin, Thin, Length / static_cast<double>(BasicShapeExtentUU)));

	if (TracerMesh == nullptr)
	{
		return;
	}

	if (TracerMesh->GetStaticMesh() == nullptr)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("ATraceTracer: /Engine/BasicShapes/Cylinder did not resolve; tracer will be invisible."));
		return;
	}

	if (TracerMaterial != nullptr)
	{
		if (UMaterialInstanceDynamic* MID = TracerMesh->CreateDynamicMaterialInstance(0, TracerMaterial))
		{
			// "Color" is BasicShapeMaterial's real vector parameter; "BaseColor" is a harmless
			// no-op if absent (SetVectorParameterValue on an unknown name is silently ignored).
			MID->SetVectorParameterValue(TEXT("Color"), Color);
			MID->SetVectorParameterValue(TEXT("BaseColor"), Color);
		}
	}
}
