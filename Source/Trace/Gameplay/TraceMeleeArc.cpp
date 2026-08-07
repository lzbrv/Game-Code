#include "Gameplay/TraceMeleeArc.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/UnrealMathUtility.h"
#include "UObject/ConstructorHelpers.h"

#include "Trace.h"

ATraceMeleeArc::ATraceMeleeArc()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// Cosmetic-only: each machine spawns its own from the multicast, so this must never go on the
	// wire. Same contract as ATraceTracer.
	bReplicates = false;
	SetCanBeDamaged(false);

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("ArcRoot"));
	SetRootComponent(Root);
	Root->SetMobility(EComponentMobility::Movable);

	// Constructor-time finders (not runtime loads) so the cooker keeps these assets. M_TraceNeon is
	// generated into a gitignored content path, so a fresh clone legitimately may not have it — the
	// slash then renders on the lit fallback with no glow, which is dimmer but never missing.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	if (CylinderFinder.Succeeded())
	{
		CylinderMesh = CylinderFinder.Object;
	}
	if (NeonFinder.Succeeded())
	{
		NeonMaterial = NeonFinder.Object;
	}
	if (BasicFinder.Succeeded())
	{
		FallbackMaterial = BasicFinder.Object;
	}

	// The segments are created as DEFAULT subobjects rather than at runtime, so the whole fan exists
	// on the CDO and spawning one costs a template copy instead of nine NewObject/RegisterComponent
	// round trips. A knife swing is rate-limited to one per 0.5 s per player, but ten bots swinging
	// is still ninety components a second and this is the cheap way to pay for them.
	Segments.Reserve(NumSegments);
	for (int32 Index = 0; Index < NumSegments; ++Index)
	{
		const FName SegmentName(*FString::Printf(TEXT("ArcSegment%d"), Index));
		UStaticMeshComponent* Segment = CreateDefaultSubobject<UStaticMeshComponent>(SegmentName);
		Segment->SetupAttachment(Root);
		Segment->SetMobility(EComponentMobility::Movable);

		// Visual only, and every one of these matters on something that spawns mid-fight: a
		// colliding slash would be an obstacle welded into a fight, and a shadow-casting one would
		// throw a moving shadow across the arena for a fifth of a second.
		Segment->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Segment->SetCollisionProfileName(TEXT("NoCollision"));
		Segment->SetGenerateOverlapEvents(false);
		Segment->SetCanEverAffectNavigation(false);
		Segment->SetCastShadow(false);
		Segment->bReceivesDecals = false;
		Segment->bUseAsOccluder = false;

		if (CylinderMesh != nullptr)
		{
			Segment->SetStaticMesh(CylinderMesh);
		}

		Segments.Add(Segment);
	}
}

ATraceMeleeArc* ATraceMeleeArc::Spawn(UWorld* World, const FVector& Origin, const FVector& Forward,
	const FVector& SwingAxis, float ArcDegrees, float RadiusUU, const FLinearColor& Color,
	bool bConnected)
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

	if (Origin.ContainsNaN() || Forward.ContainsNaN() || SwingAxis.ContainsNaN())
	{
		return nullptr;
	}
	if (Forward.IsNearlyZero() || SwingAxis.IsNearlyZero() || RadiusUU <= 1.f)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;

	ATraceMeleeArc* Arc = World->SpawnActor<ATraceMeleeArc>(
		ATraceMeleeArc::StaticClass(), FTransform(Origin), SpawnParams);
	if (Arc != nullptr)
	{
		Arc->Build(Forward, SwingAxis, ArcDegrees, RadiusUU, Color, bConnected);
	}
	return Arc;
}

void ATraceMeleeArc::Build(const FVector& Forward, const FVector& SwingAxis, float ArcDegrees,
	float RadiusUU, const FLinearColor& Color, bool bConnected)
{
	SlashColor = Color;

	// A connecting swing flares harder and hangs a little longer. That is the ONLY feedback a
	// bystander (or the victim) gets that a hundred damage just happened, so it is deliberately a
	// visible step rather than a subtle one.
	PeakGlow = bConnected ? 9.f : 5.f;
	LifeSeconds = bConnected ? 0.26f : 0.20f;
	InitialLifeSpan = LifeSeconds + 0.05f;

	const FVector Axis = SwingAxis.GetSafeNormal();
	const FVector Centre = Forward.GetSafeNormal();
	const float Radius = RadiusUU * BladeRadiusFraction;
	const float HalfArc = FMath::Clamp(ArcDegrees, 5.f, 270.f) * 0.5f;

	// Cylinders are laid along the CHORD between consecutive arc points rather than at the arc
	// points themselves, so consecutive segments meet end to end and the fan reads as one continuous
	// cut instead of as a dotted line.
	SegmentMIDs.Reset();
	SegmentMIDs.SetNum(Segments.Num());

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		UStaticMeshComponent* Segment = Segments[Index];
		if (Segment == nullptr)
		{
			continue;
		}

		const float AlphaA = (static_cast<float>(Index) / static_cast<float>(NumSegments)) * 2.f - 1.f;
		const float AlphaB = (static_cast<float>(Index + 1) / static_cast<float>(NumSegments)) * 2.f - 1.f;

		const FVector PointA = Centre.RotateAngleAxis(AlphaA * HalfArc, Axis) * Radius;
		const FVector PointB = Centre.RotateAngleAxis(AlphaB * HalfArc, Axis) * Radius;

		const FVector Chord = PointB - PointA;
		const float Length = static_cast<float>(Chord.Size());
		if (Length < 0.5f)
		{
			Segment->SetVisibility(false);
			continue;
		}

		// MakeFromZ because the basic cylinder's own axis is +Z; this turns that axis onto the
		// chord without hand-deriving a rotator. /Engine/BasicShapes primitives are 100 uu, hence
		// the divisions.
		Segment->SetRelativeLocationAndRotation(
			PointA + Chord * 0.5f,
			FRotationMatrix::MakeFromZ(Chord / Length).Rotator());
		Segment->SetRelativeScale3D(FVector(
			SegmentThicknessUU / 100.f, SegmentThicknessUU / 100.f, Length / 100.f));

		UMaterialInterface* Source = (NeonMaterial != nullptr) ? NeonMaterial.Get() : FallbackMaterial.Get();
		if (Source != nullptr)
		{
			SegmentMIDs[Index] = Segment->CreateDynamicMaterialInstance(0, Source);
		}

		// Dark on frame one. The sweep below is what lights them, in order — see the header.
		Segment->SetVisibility(true);
		PushSegment(Index, 0.f);
	}

	bHaveNeonParameters = (NeonMaterial != nullptr);

	if (Segments.Num() > 0 && SegmentMIDs.Num() > 0 && SegmentMIDs[0] == nullptr)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("ATraceMeleeArc: no material resolved; the slash will render untinted."));
	}
}

void ATraceMeleeArc::PushSegment(int32 Index, float Intensity)
{
	if (!SegmentMIDs.IsValidIndex(Index) || SegmentMIDs[Index] == nullptr)
	{
		return;
	}

	UMaterialInstanceDynamic* MID = SegmentMIDs[Index];

	// The blade's edge is hotter than its tint, exactly like the tracer's near-white core: pushing
	// the colour toward white as the intensity rises is what makes it read as BRIGHT rather than as
	// merely saturated once the tonemapper has had it.
	const FLinearColor Hot = FMath::Lerp(SlashColor, FLinearColor::White, 0.45f);

	MID->SetVectorParameterValue(TEXT("Color"), Hot);
	MID->SetScalarParameterValue(TEXT("Glow"), FMath::Max(0.f, Intensity));

	if (!bHaveNeonParameters)
	{
		// BasicShapeMaterial fallback: lit, "Color" only, no Glow scalar. Fold the intensity into
		// the colour so the sweep is still visible, just without the bloom.
		MID->SetVectorParameterValue(TEXT("Color"), Hot * FMath::Clamp(Intensity / FMath::Max(1.f, PeakGlow), 0.f, 1.f));
	}
}

void ATraceMeleeArc::BeginPlay()
{
	Super::BeginPlay();
}

void ATraceMeleeArc::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	Age += DeltaSeconds;
	const float Life = FMath::Max(0.01f, LifeSeconds);
	const float Alpha = FMath::Clamp(Age / Life, 0.f, 1.f);

	if (Alpha >= 1.f)
	{
		Destroy();
		return;
	}

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		// Each segment ignites at its own point in the sweep, so the slash draws itself across the
		// arc — that is the beat that carries the DIRECTION of the swing, which is the only thing a
		// victim can actually learn from it.
		const float IgniteAt = SweepFraction * (static_cast<float>(Index) / static_cast<float>(FMath::Max(1, NumSegments - 1)));
		if (Alpha < IgniteAt)
		{
			PushSegment(Index, 0.f);
			continue;
		}

		// Decay measured from ITS OWN ignition, so the leading edge is brightest and the tail is
		// already dying. A uniform fade reads as a light switching off; a trailing one reads as
		// motion.
		const float SinceIgnition = (Alpha - IgniteAt) / FMath::Max(0.05f, 1.f - IgniteAt);
		const float Falloff = FMath::Square(1.f - FMath::Clamp(SinceIgnition, 0.f, 1.f));
		PushSegment(Index, PeakGlow * Falloff);
	}
}
