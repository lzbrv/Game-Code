// Trace — Rocco's Ripple. See the header for the clause-by-clause reading of spec v14 §6.

#include "Abilities/Characters/TraceRippleActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                  // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Core/TraceCharacter.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceRippleTuning
{
	/** Beads per ring. Twenty reads as a ring at 90 uu radius without being a mesh budget. */
	constexpr int32 BeadsPerRing = 20;

	/** Bead cross-section, uu. /Engine/BasicShapes primitives are 100 uu across. */
	constexpr float BeadThicknessUU = 14.f;

	/** Rings, including the start ring. Capped so a long path does not become a tunnel. */
	constexpr int32 MaxRings = 12;

	/** Emissive strength for M_TraceNeon. Above 1 so it clears the bloom threshold, like the arena. */
	constexpr float RingGlow = 3.5f;
}

// =================================================================================================
// Construction
// =================================================================================================

ATraceRippleActor::ATraceRippleActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	bReplicates = true;
	SetReplicateMovement(false);      // it never moves; replicating a static transform is pure cost
	bAlwaysRelevant = true;           // a 4 s path across the arena must not be culled off a client
	SetNetUpdateFrequency(10.f);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	auto MakeRingMesh = [this](const TCHAR* Name) -> UInstancedStaticMeshComponent*
	{
		UInstancedStaticMeshComponent* Mesh = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
		Mesh->SetupAttachment(Root);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCollisionProfileName(TEXT("NoCollision"));
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->SetCastShadow(false);
		Mesh->bReceivesDecals = false;
		return Mesh;
	};

	StartRingMesh = MakeRingMesh(TEXT("StartRingMesh"));
	TrailRingMesh = MakeRingMesh(TEXT("TrailRingMesh"));

	// /Engine/BasicShapes ships with every install; M_TraceNeon is generated content and may not.
	// Both lookups are static so the cost is paid once per process, exactly as ATraceCore does it.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		BeadMesh = CylinderFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	if (NeonFinder.Succeeded())
	{
		NeonMaterial = NeonFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (BasicFinder.Succeeded())
	{
		FallbackMaterial = BasicFinder.Object;
	}
}

void ATraceRippleActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceRippleActor, RippleStart);
	DOREPLIFETIME(ATraceRippleActor, RippleDirection);
	DOREPLIFETIME(ATraceRippleActor, PathLength);
	DOREPLIFETIME(ATraceRippleActor, RideSpeed);
	DOREPLIFETIME(ATraceRippleActor, EntryRadius);
	DOREPLIFETIME(ATraceRippleActor, ExpireMatchTime);
	DOREPLIFETIME(ATraceRippleActor, SourcePlayerState);
}

void ATraceRippleActor::InitialiseRipple(APlayerState* InSource, const FVector& InStart, const FVector& InDirection,
                                         float InPathLength, float InRideSpeed, float InEntryRadius,
                                         float InExpireMatchTime)
{
	SourcePlayerState = InSource;
	RippleStart       = InStart;
	RippleDirection   = InDirection.GetSafeNormal();
	PathLength        = FMath::Max(0.f, InPathLength);
	RideSpeed         = FMath::Max(0.f, InRideSpeed);
	EntryRadius       = FMath::Max(0.f, InEntryRadius);
	ExpireMatchTime   = InExpireMatchTime;

	SetActorLocation(InStart);
	SetActorRotation(FRotator::ZeroRotator);
}

void ATraceRippleActor::BeginPlay()
{
	Super::BeginPlay();

	// On the server everything is already set by InitialiseRipple. On a client the replicated
	// properties may not have landed yet, so the rings are built from Tick instead — see
	// BuildRingsIfNeeded, which is idempotent and cheap until the path arrives.
	BuildRingsIfNeeded();
}

float ATraceRippleActor::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

// =================================================================================================
// Tick
// =================================================================================================

void ATraceRippleActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	BuildRingsIfNeeded();
	UpdateRides(DeltaSeconds);
}

bool ATraceRippleActor::ShouldSimulate(const ATraceCharacter* Candidate) const
{
	if (Candidate == nullptr)
	{
		return false;
	}

	// The server owns every pawn's truth. A client owns exactly one pawn's prediction. A simulated
	// proxy is nobody's to push — its motion arrives by replication like everything else.
	return HasAuthority() || Candidate->IsLocallyControlled();
}

bool ATraceRippleActor::IsRiding(const ATraceCharacter* Candidate) const
{
	for (const FRippleRider& Rider : Riders)
	{
		if (Rider.Pawn.Get() == Candidate)
		{
			return true;
		}
	}
	return false;
}

void ATraceRippleActor::UpdateRides(float DeltaSeconds)
{
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return;
	}

	const float Now = MatchTimeNow();

	// "Lasts 4 s, then all effects and visuals vanish." One deadline, one destroy, and the visuals
	// leave with the actor rather than being faded out by a second timer that could disagree.
	//
	// TESTED BEFORE THE PATH-LENGTH GUARD BELOW, DELIBERATELY: a ripple that somehow reached the
	// world with a zero-length path would otherwise take the early return every tick and never be
	// destroyed. The expiry is the one thing that must not depend on the ripple being well formed.
	if (HasAuthority() && ExpireMatchTime > 0.f && Now >= ExpireMatchTime)
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Ripple] expired at match time %.2f after %d rider(s)."),
			Now, LifetimeRiders.Num());
		Destroy();
		return;
	}

	if (PathLength <= 0.f)
	{
		return;   // on a client, until the replicated path arrives
	}

	const FVector Direction = FVector(RippleDirection);

	// ---- advance the pawns already riding --------------------------------------------------------
	for (int32 Index = Riders.Num() - 1; Index >= 0; --Index)
	{
		ATraceCharacter* RiderPawn = Riders[Index].Pawn.Get();
		UTraceCharacterMovementComponent* Move = (RiderPawn != nullptr) ? RiderPawn->GetTraceMovement() : nullptr;

		const bool bStillValid = (RiderPawn != nullptr) && RiderPawn->IsAlive() && (Move != nullptr)
			&& ShouldSimulate(RiderPawn);

		Riders[Index].DistanceTravelled += RideSpeed * DeltaSeconds;

		const bool bFinished = !bStillValid
			|| (Riders[Index].DistanceTravelled >= PathLength)
			|| (Now >= ExpireMatchTime);

		if (bFinished)
		{
			// The ride hands back a fast player rather than a stopped one — the same choice the dash
			// makes on exit (ApplyDashExitSpeed). Nothing is zeroed here.
			Riders.RemoveAt(Index);
			continue;
		}

		// Ground movement would discard the vertical component of an upward path on the very first
		// frame, so a rider is put into the falling mode the ride actually needs. Mode first, then
		// velocity: a mode change can rewrite Velocity, and the write must be the last word.
		if (Move->IsMovingOnGround())
		{
			Move->SetMovementMode(MOVE_Falling);
		}
		Move->Velocity = Direction * RideSpeed;
	}

	// ---- pick up anybody standing in the entrance ------------------------------------------------
	const FVector Start = FVector(RippleStart);
	const float EntryRadiusSq = EntryRadius * EntryRadius;

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive() || !ShouldSimulate(Candidate))
		{
			continue;
		}

		// ONE RIDE PER PLAYER PER RIPPLE. [ASSUMPTION] — §6 does not say. Without it a path whose
		// end lands back inside its own entry radius (a short vertical ripple on a ledge) would loop
		// a player forever, and "lasts 4 s" would become "is stuck for 4 s".
		bool bAlreadyRode = false;
		for (const TWeakObjectPtr<ATraceCharacter>& Past : LifetimeRiders)
		{
			if (Past.Get() == Candidate)
			{
				bAlreadyRode = true;
				break;
			}
		}
		if (bAlreadyRode)
		{
			continue;
		}

		if (FVector::DistSquared(Candidate->GetActorLocation(), Start) > EntryRadiusSq)
		{
			continue;
		}

		// *** THE CHOKE POINT. Beneficial — which is the effect class spec §4 defines precisely so
		// that §6's "any character, either team... the Core carrier can use it" is expressible
		// WITHOUT an exception to the carrier rule. There is no team test here on purpose. ***
		if (!UTraceAbilityComponent::CanAffect(SourcePlayerState, Candidate, ETraceAbilityEffect::Beneficial))
		{
			continue;
		}

		UTraceCharacterMovementComponent* Move = Candidate->GetTraceMovement();
		if (Move == nullptr)
		{
			continue;
		}

		FRippleRider NewRider;
		NewRider.Pawn = Candidate;
		NewRider.DistanceTravelled = 0.f;
		Riders.Add(NewRider);
		LifetimeRiders.Add(Candidate);

		if (Move->IsMovingOnGround())
		{
			Move->SetMovementMode(MOVE_Falling);
		}
		Move->Velocity = Direction * RideSpeed;

		UE_LOG(LogTraceGame, Verbose,
			TEXT("[Ripple] %s entered (authority=%d, carrier=%d) — propelled at %.0f uu/s along (%s)."),
			*GetNameSafe(Candidate), HasAuthority() ? 1 : 0,
			UTraceAbilityComponent::IsCarrier(Candidate) ? 1 : 0, RideSpeed, *Direction.ToCompactString());
	}
}

const TCHAR* ATraceRippleActor::DescribeEntryRefusal(const ATraceCharacter* Candidate, const APlayerState* Source)
{
	if (const UTraceAbilityComponent* Comp = UTraceAbilityComponent::Get(Source))
	{
		return TraceAbilityBlockReasonToString(
			Comp->CanAffectTargetDetailed(Candidate, ETraceAbilityEffect::Beneficial));
	}
	return TEXT("<no ability component on the source>");
}

// =================================================================================================
// The rings — §6's "short series of rings along the path, with the starting ring in a different
// colour so it is obvious where to take it"
// =================================================================================================

void ATraceRippleActor::BuildRingsIfNeeded()
{
	if (bRingsBuilt || PathLength <= 0.f || BeadMesh == nullptr)
	{
		return;
	}

	// Shaders are not cooked for server targets, so a dedicated server builds nothing at all.
	if (GetNetMode() == NM_DedicatedServer)
	{
		bRingsBuilt = true;
		return;
	}

	bRingsBuilt = true;

	const UTraceSettings& Settings = UTraceSettings::Get();
	UMaterialInterface* Parent = (NeonMaterial != nullptr) ? NeonMaterial.Get() : FallbackMaterial.Get();

	auto SetupMesh = [this, Parent](UInstancedStaticMeshComponent* Mesh,
	                                TObjectPtr<UMaterialInstanceDynamic>& OutMID,
	                                const FLinearColor& Color)
	{
		if (Mesh == nullptr)
		{
			return;
		}
		Mesh->SetStaticMesh(BeadMesh);
		if (Parent != nullptr)
		{
			OutMID = UMaterialInstanceDynamic::Create(Parent, this);
			if (OutMID != nullptr)
			{
				// "Color" is what both M_TraceNeon and BasicShapeMaterial call it; "Glow" is neon
				// only and is a no-op on the fallback, which is why the fallback also gets a matte
				// roughness — it will not bloom, and that is the cost of not having generated the
				// content pack.
				OutMID->SetVectorParameterValue(TEXT("Color"), Color);
				OutMID->SetVectorParameterValue(TEXT("BaseColor"), Color);
				OutMID->SetScalarParameterValue(TEXT("Glow"), TraceRippleTuning::RingGlow);
				OutMID->SetScalarParameterValue(TEXT("Roughness"), 0.9f);
				Mesh->SetMaterial(0, OutMID);
			}
		}
	};

	SetupMesh(StartRingMesh, StartRingMID, Settings.RoccoRippleStartRingColor);
	SetupMesh(TrailRingMesh, TrailRingMID, Settings.RoccoRippleTrailRingColor);

	const FVector Start = FVector(RippleStart);
	const FVector Direction = FVector(RippleDirection).GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		return;
	}

	const float Spacing = FMath::Max(20.f, Settings.RoccoRippleRingSpacingUU);
	const int32 TrailRings = FMath::Clamp(FMath::FloorToInt(PathLength / Spacing), 1, TraceRippleTuning::MaxRings - 1);

	// THE START RING IS FIRST AND IS ITS OWN COMPONENT. §6 asks for it in a different colour "so it
	// is obvious where to take it" — it marks the ENTRANCE, which is the only place entry is
	// possible, so making it a separate draw rather than a tinted instance is the point.
	AddRing(StartRingMesh, Start, Direction);

	for (int32 Index = 1; Index <= TrailRings; ++Index)
	{
		const float Distance = FMath::Min(PathLength, Spacing * static_cast<float>(Index));
		AddRing(TrailRingMesh, Start + Direction * Distance, Direction);
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Ripple] rings built: 1 START ring (colour %s) + %d trail rings (colour %s), spacing %.0f uu, "
		     "radius %.0f uu, path %.0f uu."),
		*Settings.RoccoRippleStartRingColor.ToString(), TrailRings,
		*Settings.RoccoRippleTrailRingColor.ToString(), Spacing, Settings.RoccoRippleRingRadiusUU, PathLength);
}

void ATraceRippleActor::AddRing(UInstancedStaticMeshComponent* Mesh, const FVector& Center, const FVector& Direction) const
{
	if (Mesh == nullptr)
	{
		return;
	}

	const float RingRadius = FMath::Max(10.f, UTraceSettings::Get().RoccoRippleRingRadiusUU);

	// A basis for the plane the ring lies in. FindBestAxisVectors is the engine's own numerically
	// safe pick, which matters because the path may be exactly vertical.
	FVector AxisU = FVector::ZeroVector;
	FVector AxisV = FVector::ZeroVector;
	Direction.FindBestAxisVectors(AxisU, AxisV);

	// Each bead is a cylinder lying along the ring's tangent, so a ring of them reads as a torus
	// rather than a necklace. 1.2 overlaps them slightly and hides the seams.
	const float ArcLength = (2.f * PI * RingRadius / static_cast<float>(TraceRippleTuning::BeadsPerRing)) * 1.2f;
	const FVector BeadScale(TraceRippleTuning::BeadThicknessUU / 100.f,
	                        TraceRippleTuning::BeadThicknessUU / 100.f,
	                        ArcLength / 100.f);

	const FVector CenterRelative = Center - GetActorLocation();

	for (int32 Bead = 0; Bead < TraceRippleTuning::BeadsPerRing; ++Bead)
	{
		const float Angle = (2.f * PI * static_cast<float>(Bead)) / static_cast<float>(TraceRippleTuning::BeadsPerRing);
		const float SinA = FMath::Sin(Angle);
		const float CosA = FMath::Cos(Angle);

		const FVector Offset  = (AxisU * CosA + AxisV * SinA) * RingRadius;
		const FVector Tangent = (AxisU * -SinA + AxisV * CosA);

		const FTransform BeadTransform(FRotationMatrix::MakeFromZ(Tangent).Rotator(),
		                               CenterRelative + Offset, BeadScale);
		Mesh->AddInstance(BeadTransform);
	}
}
