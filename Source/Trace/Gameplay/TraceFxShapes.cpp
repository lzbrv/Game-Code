#include "Gameplay/TraceFxShapes.h"

#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "Math/UnrealMathUtility.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"

#include "Trace.h"

// -------------------------------------------------------------------------------------------------
// File-local helpers.
//
// NAMED FOR THE FILE, NOT ANONYMOUS. UBT compiles this module as a unity/jumbo build, so an
// anonymous namespace here shares one scope with every other .cpp in the same blob and a plain
// `namespace { bool bLogged; }` is a Windows-only link break waiting to happen. Scripts/build.sh
// gates on exactly that (check-jumbo-build-collisions.py).
// -------------------------------------------------------------------------------------------------
namespace TraceFxShapesFile
{
	/** LogPrimitiveGeometryOnce's latch. */
	bool bGeometryLogged = false;

	/** GetIcosphere's one attempt, and its answer. */
	bool bIcosphereChecked = false;
	UStaticMesh* IcosphereMesh = nullptr;

	/**
	 * One latch per ETraceFxBlend so a degradation is reported ONCE per run.
	 *
	 * SPEC v32 §1, verbatim: the effect "does not disappear and it does not warn every frame". This
	 * is called up to eighty times a second in a full match; a per-shot warning on a missing engine
	 * material would be a bigger outage than the missing material.
	 */
	bool bDegradationLogged[5] = { false, false, false, false, false };

	/**
	 * The mesh-local Z of a primitive's BOTTOM face, in mesh units (before scaling).
	 *
	 * /Engine/BasicShapes/Cylinder is centred on its origin (-50..+50) — the shipped tracer has
	 * relied on that since v4 — but the CONE's pivot convention is a property of an asset somebody
	 * else ships, and placing a cone's BASE at a given point is exactly the case where guessing
	 * wrong puts it half a cone out. So measure it rather than assume it. The unit conversion is
	 * still the one named constant; this only answers "where is this mesh's origin relative to its
	 * own base", which is a pivot question, not a unit question.
	 */
	double MeshLocalMinZ(const UStaticMesh* Shape)
	{
		if (Shape == nullptr)
		{
			return -0.5 * UTraceFxShapes::BasicShapeExtentUU;
		}
		const FBoxSphereBounds Bounds = Shape->GetBounds();
		return Bounds.Origin.Z - Bounds.BoxExtent.Z;
	}
}

UTraceFxShapes::UTraceFxShapes()
{
	// CONSTRUCTOR-TIME FObjectFinders, not runtime LoadObject, and this is the whole reason this
	// library is a UObject at all: a finder leaves a cook reference, a bare LoadObject does not and
	// resolves to null in a packaged build. ATraceCore's comments record that trap being fallen
	// into once already.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneFinder(TEXT("/Engine/BasicShapes/Plane.Plane"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));

	// M_TraceNeon IS COMMITTED, and this comment used to say the opposite.
	//
	// MAP_PLAN §9: every load site now names /Game/Trace/Materials/Parents, which is in the repository
	// (spec v17 §3 promoted it there and the material generator re-authors it in place). The old
	// /Game/Generated/Materials tree — gitignored, per-developer, produced by an earlier
	// Scripts/generate_content.py — has been DELETED from the working tree, so a second finder aimed at
	// it would only ever print "CDO Constructor: Failed to find" at Error on every launch of every
	// machine. The engine fallback below is still the honest degradation for a clone that somehow has
	// neither, and ResolveBlend() still handles it.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Trace/Materials/Parents/M_TraceNeon.M_TraceNeon"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> AdditiveFinder(TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	if (CylinderFinder.Succeeded()) { CylinderMesh = CylinderFinder.Object; }
	if (ConeFinder.Succeeded())     { ConeMesh     = ConeFinder.Object; }
	if (PlaneFinder.Succeeded())    { PlaneMesh    = PlaneFinder.Object; }
	if (SphereFinder.Succeeded())   { SphereMesh   = SphereFinder.Object; }

	if (NeonFinder.Succeeded())     { NeonMaterial     = NeonFinder.Object; }
	if (AdditiveFinder.Succeeded()) { AdditiveMaterial = AdditiveFinder.Object; }
	if (BasicFinder.Succeeded())    { FallbackMaterial = BasicFinder.Object; }
}

// =================================================================================================
// Primitives
// =================================================================================================

UStaticMesh* UTraceFxShapes::GetCylinder()
{
	LogPrimitiveGeometryOnce();
	return GetDefault<UTraceFxShapes>()->CylinderMesh;
}

UStaticMesh* UTraceFxShapes::GetCone()
{
	LogPrimitiveGeometryOnce();
	return GetDefault<UTraceFxShapes>()->ConeMesh;
}

UStaticMesh* UTraceFxShapes::GetPlane()
{
	LogPrimitiveGeometryOnce();
	return GetDefault<UTraceFxShapes>()->PlaneMesh;
}

UStaticMesh* UTraceFxShapes::GetSphere()
{
	LogPrimitiveGeometryOnce();
	return GetDefault<UTraceFxShapes>()->SphereMesh;
}

UStaticMesh* UTraceFxShapes::GetIcosphere()
{
	if (!TraceFxShapesFile::bIcosphereChecked)
	{
		TraceFxShapesFile::bIcosphereChecked = true;

		// ONE quiet attempt, at the two paths an icosphere would live at if the engine or this
		// project ever shipped one. LOAD_NoWarn | LOAD_Quiet because the expected answer is "no":
		// /Engine/BasicShapes holds Cone, Cube, Cylinder, Plane and Sphere and nothing else, so a
		// noisy miss here would be a warning printed on every launch about an asset nobody has.
		//
		// The second candidate is now certain to miss: MAP_PLAN §9 deleted /Game/Generated. It is kept
		// as the one line that documents where the mesh WOULD go if this project ever authors one, and
		// it costs a quiet lookup once per process. A MESH is not a material path — the §9 grep gate is
		// about /Game/Generated/Materials, which no source file references any more.
		static const TCHAR* Candidates[] =
		{
			TEXT("/Engine/BasicShapes/Icosphere.Icosphere"),
			TEXT("/Game/Generated/Meshes/SM_TraceIcosphere.SM_TraceIcosphere")
		};

		for (const TCHAR* Path : Candidates)
		{
			UStaticMesh* Found = LoadObject<UStaticMesh>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Found != nullptr)
			{
				// Rooted because this pointer outlives any caller and nothing else references it.
				Found->AddToRoot();
				TraceFxShapesFile::IcosphereMesh = Found;
				break;
			}
		}

		UE_LOG(LogTraceGame, Display, TEXT("FXSHAPES: icosphere=%s (FX doc asks for an icosahedron halo; %s)"),
			(TraceFxShapesFile::IcosphereMesh != nullptr) ? *GetNameSafe(TraceFxShapesFile::IcosphereMesh) : TEXT("NOT FOUND"),
			(TraceFxShapesFile::IcosphereMesh != nullptr)
				? TEXT("using it")
				: TEXT("degraded to /Engine/BasicShapes/Sphere, which SPEC v32 §1 names as the documented fallback"));
	}

	return (TraceFxShapesFile::IcosphereMesh != nullptr) ? TraceFxShapesFile::IcosphereMesh : GetSphere();
}

bool UTraceFxShapes::IsIcosphereDegraded()
{
	GetIcosphere();
	return TraceFxShapesFile::IcosphereMesh == nullptr;
}

void UTraceFxShapes::LogPrimitiveGeometryOnce()
{
	if (TraceFxShapesFile::bGeometryLogged)
	{
		return;
	}
	TraceFxShapesFile::bGeometryLogged = true;

	const UTraceFxShapes* Lib = GetDefault<UTraceFxShapes>();

	auto Describe = [](const TCHAR* Label, const UStaticMesh* Shape)
	{
		if (Shape == nullptr)
		{
			UE_LOG(LogTraceGame, Display, TEXT("FXSHAPES: %s = MISSING"), Label);
			return;
		}
		const FBoxSphereBounds Bounds = Shape->GetBounds();
		// EXTENT x2 is the across-measurement the 100 uu contract is about, and the origin says
		// where the pivot sits inside it. Both printed so a verifier can check the assumption that
		// RadiusUUToShapeScale is built on, rather than take this file's word for it.
		UE_LOG(LogTraceGame, Display,
			TEXT("FXSHAPES: %s = %s  across(X,Y)=%.1f,%.1f uu  tall(Z)=%.1f uu  pivot=%s  baseZ=%.1f"),
			Label, *GetNameSafe(Shape),
			2.0 * Bounds.BoxExtent.X, 2.0 * Bounds.BoxExtent.Y, 2.0 * Bounds.BoxExtent.Z,
			*Bounds.Origin.ToCompactString(), Bounds.Origin.Z - Bounds.BoxExtent.Z);
	};

	Describe(TEXT("Cylinder"), Lib->CylinderMesh);
	Describe(TEXT("Cone"),     Lib->ConeMesh);
	Describe(TEXT("Plane"),    Lib->PlaneMesh);
	Describe(TEXT("Sphere"),   Lib->SphereMesh);

	UE_LOG(LogTraceGame, Display,
		TEXT("FXSHAPES: units — 1 m = %.0f uu; radius->scale = %.4f (so r 3.0 uu = scale %.4f, FX doc r 0.030 m = %.4f); "
		     "materials neon=%s additive=%s fallback=%s"),
		MetresToUU, RadiusUUToShapeScale,
		ShapeScaleForRadiusUU(3.0f), ShapeScaleForRadiusMetres(0.030f),
		*GetNameSafe(Lib->NeonMaterial), *GetNameSafe(Lib->AdditiveMaterial), *GetNameSafe(Lib->FallbackMaterial));
}

// =================================================================================================
// Materials
// =================================================================================================

const TCHAR* UTraceFxShapes::BlendName(ETraceFxBlend Blend)
{
	switch (Blend)
	{
	case ETraceFxBlend::Emissive:    return TEXT("Emissive");
	case ETraceFxBlend::Additive:    return TEXT("Additive");
	case ETraceFxBlend::Translucent: return TEXT("Translucent");
	case ETraceFxBlend::Fallback:    return TEXT("Fallback");
	default:                         return TEXT("None");
	}
}

UMaterialInterface* UTraceFxShapes::GetMaterial(ETraceFxBlend Blend)
{
	const UTraceFxShapes* Lib = GetDefault<UTraceFxShapes>();
	switch (Blend)
	{
	case ETraceFxBlend::Emissive:    return Lib->NeonMaterial;
	case ETraceFxBlend::Additive:
	case ETraceFxBlend::Translucent: return Lib->AdditiveMaterial;
	case ETraceFxBlend::Fallback:    return Lib->FallbackMaterial;
	default:                         return nullptr;
	}
}

ETraceFxBlend UTraceFxShapes::ResolveBlend(ETraceFxBlend Preferred)
{
	// The degradation ladder, in one place. Translucent has no parent of its own in this project
	// (see the enum comment), so it steps to Additive first and follows the same ladder from there.
	switch (Preferred)
	{
	case ETraceFxBlend::Translucent:
		return (GetMaterial(ETraceFxBlend::Additive) != nullptr)
			? ETraceFxBlend::Additive
			: ResolveBlend(ETraceFxBlend::Emissive);

	case ETraceFxBlend::Additive:
		return (GetMaterial(ETraceFxBlend::Additive) != nullptr)
			? ETraceFxBlend::Additive
			: ResolveBlend(ETraceFxBlend::Emissive);

	case ETraceFxBlend::Emissive:
		if (GetMaterial(ETraceFxBlend::Emissive) != nullptr)
		{
			return ETraceFxBlend::Emissive;
		}
		return (GetMaterial(ETraceFxBlend::Fallback) != nullptr) ? ETraceFxBlend::Fallback : ETraceFxBlend::None;

	case ETraceFxBlend::Fallback:
		return (GetMaterial(ETraceFxBlend::Fallback) != nullptr) ? ETraceFxBlend::Fallback : ETraceFxBlend::None;

	default:
		return ETraceFxBlend::None;
	}
}

UMaterialInstanceDynamic* UTraceFxShapes::MakeGlowMID(UMeshComponent* Component, int32 ElementIndex,
	ETraceFxBlend Preferred, ETraceFxBlend& OutAchieved)
{
	OutAchieved = ETraceFxBlend::None;
	if (Component == nullptr)
	{
		return nullptr;
	}

	const ETraceFxBlend Achieved = ResolveBlend(Preferred);

	// One line per degraded blend per run. Display rather than Warning: a missing generated material
	// is a documented state of a fresh clone, not an error, and this project's rule is that a
	// degradation announces itself once and then shuts up.
	const int32 Slot = FMath::Clamp(static_cast<int32>(Preferred), 0, 4);
	if (Achieved != Preferred && !TraceFxShapesFile::bDegradationLogged[Slot])
	{
		TraceFxShapesFile::bDegradationLogged[Slot] = true;
		UE_LOG(LogTraceGame, Display, TEXT("FXSHAPES: blend %s -> %s (%s)"),
			BlendName(Preferred), BlendName(Achieved),
			(Preferred == ETraceFxBlend::Translucent)
				? TEXT("this project has no translucent parent material; additive carries the opacity")
				: TEXT("the preferred parent material did not resolve"));
	}

	UMaterialInterface* Parent = GetMaterial(Achieved);
	if (Parent == nullptr)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* MID = Component->CreateDynamicMaterialInstance(ElementIndex, Parent);
	if (MID != nullptr)
	{
		OutAchieved = Achieved;
	}
	return MID;
}

void UTraceFxShapes::SetGlow(UMaterialInstanceDynamic* MID, ETraceFxBlend Blend,
	const FLinearColor& Color, float Intensity, float Opacity)
{
	if (MID == nullptr)
	{
		return;
	}

	const float SafeIntensity = FMath::Max(0.f, Intensity);
	const float SafeOpacity = FMath::Clamp(Opacity, 0.f, 1.f);

	switch (Blend)
	{
	case ETraceFxBlend::Additive:
	case ETraceFxBlend::Translucent:
		// EmissiveMeshMaterial has no Glow scalar, so brightness rides in the colour — and is
		// therefore clamped at 1.0 and cannot be pushed past white. That is fine for a soft sleeve
		// and it is why this blend is never used for a hot core. Opacity and intensity multiply
		// together because for ADDITIVE geometry they are the same physical quantity: how much of
		// this colour is added to what is behind it.
		MID->SetVectorParameterValue(TEXT("Color"), Color * (SafeIntensity * SafeOpacity));
		return;

	case ETraceFxBlend::Emissive:
	case ETraceFxBlend::Fallback:
	default:
		// M_TraceNeon: EmissiveColor = Color * Glow, Color a plain 0..1 hue. BRIGHTNESS MUST RIDE ON
		// THE SCALAR — a material instance clamps vector parameters to [0,1], so folding 6.6 into
		// the colour silently yields flat white at intensity 1 and the piece renders as a dull matte
		// tube with no bloom at all. That failure looks exactly like "the effect is not rendering".
		//
		// BasicShapeMaterial (the Fallback) takes the same "Color" and has no "Glow"; writing a
		// parameter a material does not have is a no-op, so one branch serves both and the fallback
		// simply keeps its hue and stops glowing.
		MID->SetVectorParameterValue(TEXT("Color"), Color);
		MID->SetScalarParameterValue(TEXT("Glow"), SafeIntensity * SafeOpacity);
		return;
	}
}

// =================================================================================================
// Component setup
// =================================================================================================

void UTraceFxShapes::ConfigureFxComponent(UPrimitiveComponent* Component)
{
	if (Component == nullptr)
	{
		return;
	}

	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCollisionProfileName(TEXT("NoCollision"));
	Component->SetGenerateOverlapEvents(false);
	Component->SetCastShadow(false);
	Component->bReceivesDecals = false;
	Component->bUseAsOccluder = false;
}

// =================================================================================================
// Geometry
// =================================================================================================

void UTraceFxShapes::StretchAlongLocalZ(USceneComponent* Piece, double StartZ, double EndZ, float RadiusUU)
{
	if (Piece == nullptr)
	{
		return;
	}

	const double Length = EndZ - StartZ;
	const double Centre = 0.5 * (StartZ + EndZ);
	const float Thickness = ShapeScaleForRadiusUU(FMath::Max(0.f, RadiusUU));

	// The cylinder is centred on its own origin, so its centre goes at the midpoint and its Z scale
	// is the length. FMath::Abs so a caller that hands the ends in the other order still gets a
	// cylinder rather than an inside-out one.
	Piece->SetRelativeLocation(FVector(0.0, 0.0, Centre));
	Piece->SetRelativeScale3D(FVector(Thickness, Thickness, ShapeScaleForLengthUU(static_cast<float>(FMath::Abs(Length)))));
}

void UTraceFxShapes::StretchBetween(USceneComponent* Piece, const FVector& From, const FVector& To, float RadiusUU)
{
	if (Piece == nullptr)
	{
		return;
	}

	const FVector Delta = To - From;
	const double Length = Delta.Size();
	if (Length <= UE_DOUBLE_SMALL_NUMBER || Delta.ContainsNaN())
	{
		// A zero-length segment has no rotation to build. Collapse it rather than leaving whatever
		// transform was there before, which would be a stale beam pointing at last frame's target.
		Piece->SetRelativeScale3D(FVector::ZeroVector);
		return;
	}

	const FVector Dir = Delta / Length;
	const float Thickness = ShapeScaleForRadiusUU(FMath::Max(0.f, RadiusUU));

	Piece->SetWorldLocationAndRotation(From + Delta * 0.5, FRotationMatrix::MakeFromZ(Dir).ToQuat());
	Piece->SetWorldScale3D(FVector(Thickness, Thickness, ShapeScaleForLengthUU(static_cast<float>(Length))));
}

float UTraceFxShapes::TaperSegmentRadiusUU(float StartRadiusUU, float EndRadiusUU, int32 Index, int32 Count)
{
	if (Count <= 0)
	{
		return StartRadiusUU;
	}
	// The segment's MID-POINT along the taper, so the stack straddles the ideal cone instead of
	// sitting wholly inside it (which would read as a beam thinner than the doc asks for) or wholly
	// outside it (fatter).
	const float Alpha = (2.f * static_cast<float>(Index) + 1.f) / (2.f * static_cast<float>(Count));
	return TaperRadiusAt(StartRadiusUU, EndRadiusUU, Alpha);
}

int32 UTraceFxShapes::TaperAlongLocalZ(TArrayView<UStaticMeshComponent* const> Segments,
	double StartZ, double EndZ, float StartRadiusUU, float EndRadiusUU, float* OutSegmentRadiiUU)
{
	const int32 Count = Segments.Num();
	if (Count <= 0)
	{
		return 0;
	}

	const double Span = EndZ - StartZ;
	int32 Placed = 0;

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float SegmentRadius = TaperSegmentRadiusUU(StartRadiusUU, EndRadiusUU, Index, Count);
		if (OutSegmentRadiiUU != nullptr)
		{
			OutSegmentRadiiUU[Index] = SegmentRadius;
		}

		UStaticMeshComponent* Segment = Segments[Index];
		if (Segment == nullptr)
		{
			continue;
		}

		const double A0 = static_cast<double>(Index) / static_cast<double>(Count);
		const double A1 = static_cast<double>(Index + 1) / static_cast<double>(Count);
		StretchAlongLocalZ(Segment, StartZ + Span * A0, StartZ + Span * A1, SegmentRadius);
		++Placed;
	}

	return Placed;
}

int32 UTraceFxShapes::TaperBetween(TArrayView<UStaticMeshComponent* const> Segments,
	const FVector& From, const FVector& To, float StartRadiusUU, float EndRadiusUU, float* OutSegmentRadiiUU)
{
	const int32 Count = Segments.Num();
	if (Count <= 0)
	{
		return 0;
	}

	int32 Placed = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float SegmentRadius = TaperSegmentRadiusUU(StartRadiusUU, EndRadiusUU, Index, Count);
		if (OutSegmentRadiiUU != nullptr)
		{
			OutSegmentRadiiUU[Index] = SegmentRadius;
		}

		UStaticMeshComponent* Segment = Segments[Index];
		if (Segment == nullptr)
		{
			continue;
		}

		const double A0 = static_cast<double>(Index) / static_cast<double>(Count);
		const double A1 = static_cast<double>(Index + 1) / static_cast<double>(Count);
		StretchBetween(Segment, FMath::Lerp(From, To, A0), FMath::Lerp(From, To, A1), SegmentRadius);
		++Placed;
	}

	return Placed;
}

void UTraceFxShapes::PlaceConeAlongLocalZ(USceneComponent* Piece, double BaseZ, float RadiusUU, float HeightUU)
{
	if (Piece == nullptr)
	{
		return;
	}

	const float SafeHeight = FMath::Max(0.f, HeightUU);
	const float Thickness = ShapeScaleForRadiusUU(FMath::Max(0.f, RadiusUU));
	const float ZScale = ShapeScaleForLengthUU(SafeHeight);

	// Where the mesh's own base sits relative to its pivot, MEASURED (see MeshLocalMinZ). Once
	// scaled, the base lands at MinZ * ZScale, so the offset that puts it exactly on BaseZ is the
	// difference. This is what makes "a cone at the muzzle pointing down the beam" land on the
	// muzzle rather than half a cone behind or in front of it, whichever pivot convention the
	// engine's Cone.uasset happens to use.
	const double BaseOffset = TraceFxShapesFile::MeshLocalMinZ(GetCone()) * static_cast<double>(ZScale);

	Piece->SetRelativeLocation(FVector(0.0, 0.0, BaseZ - BaseOffset));
	Piece->SetRelativeScale3D(FVector(Thickness, Thickness, ZScale));
}

void UTraceFxShapes::SizePlane(USceneComponent* Piece, float WidthUU, float HeightUU)
{
	if (Piece == nullptr)
	{
		return;
	}

	// A plane is 100 x 100 uu in its own XY, so both axes take the LENGTH conversion, not the radius
	// one — a plane is specified by its full width, a cylinder by its radius. Mixing the two is the
	// 2x error this library's one constant exists to make impossible to write by accident.
	Piece->SetRelativeScale3D(FVector(
		ShapeScaleForLengthUU(FMath::Max(0.f, WidthUU)),
		ShapeScaleForLengthUU(FMath::Max(0.f, HeightUU)),
		1.0));
}
