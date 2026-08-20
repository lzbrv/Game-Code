#include "Gameplay/TraceTracer.h"

#include "Camera/CameraComponent.h"           // spec v30 §5 — the first-person morph, applied to a
#include "Camera/CameraTypes.h"               //   point the pawn's own marker does not cover
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "Math/UnrealMathUtility.h"
#include "UObject/ConstructorHelpers.h"

#include "Core/TraceCharacter.h"               // GetViewModelMuzzleViewPoint (spec v26 §4)
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"                        // TraceTeamColor — spec v26 §5, the ONE team palette

namespace
{
	/**
	 * Where the local viewer's eye is, or false if there is nobody looking.
	 *
	 * Used only to decide whether this beam is being drawn practically inside the shooter's own
	 * camera, which is what the standoff exists to handle. The camera manager (not the pawn) is
	 * asked because the spring arm's collision probe and the first/third person view blend both move
	 * the real eye, and it is the REAL eye the flash would be blinding.
	 */
	bool GetLocalView(const UWorld* World, FVector& OutLocation, FRotator& OutRotation)
	{
		if (World == nullptr)
		{
			return false;
		}

		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			if (PC == nullptr || !PC->IsLocalController() || PC->PlayerCameraManager == nullptr)
			{
				continue;
			}
			OutLocation = PC->PlayerCameraManager->GetCameraLocation();
			OutRotation = PC->PlayerCameraManager->GetCameraRotation();
			return true;
		}
		return false;
	}

	/**
	 * SPEC v26 §4. The pawn the local viewer is looking OUT OF, if it is one of ours.
	 *
	 * GetViewTarget() rather than GetPawn(): the beam is being aligned to what is on this screen, and
	 * the view target is by definition the thing the screen is drawn from. They are the same object in
	 * every normal frame; they differ during a spectator or death-cam takeover, which is exactly when
	 * "my own viewmodel" is the wrong thing to align to.
	 *
	 * Named for this file. The jumbo/unity build concatenates translation units, so an anonymous
	 * namespace here shares a scope with every other .cpp in the same blob and a plain
	 * GetLocalCharacter() would be a collision waiting to happen (Scripts/check-jumbo-build-collisions.py
	 * gates on precisely that).
	 */
	const ATraceCharacter* GetTracerLocalViewCharacter(const UWorld* World)
	{
		if (World == nullptr)
		{
			return nullptr;
		}

		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			if (PC == nullptr || !PC->IsLocalController())
			{
				continue;
			}
			return Cast<ATraceCharacter>(PC->GetViewTarget());
		}
		return nullptr;
	}

	// ---------------------------------------------------------------------------------------------
	// SPEC v30 §5 — THE GUN ON SCREEN, AND ITS OWN MUZZLE
	//
	// Named for this file, not because these are pretty names, but because the anonymous namespace
	// they live in is shared with every other .cpp in the same jumbo blob (see the note on
	// GetTracerLocalViewCharacter above).
	// ---------------------------------------------------------------------------------------------

	/**
	 * One viewmodel gun: the mesh that identifies it, and where its barrel ends in its own space.
	 *
	 * THESE TWO NUMBERS ARE MEASUREMENTS, NOT TUNING. The pistol's is the same (107.4, 0, 4.5) cm
	 * vertex recorded in railgun_manifest.json that TraceCharacterLayout::RailgunMuzzleLocal already
	 * places its marker at, so the pistol's answer here is identical to the pawn's by construction.
	 * The SMG's is the aperture centre measured out of railgun_smg.glb — muzzle_aperture's own local
	 * bounding-box centre carried through the node chain — which confirms spec v30 §1's -0.588 m and
	 * refutes the kit README's -0.59 m. It is deliberately NOT the forward-most vertex: that is
	 * X = 63.0 at Y = -4.3, the outer rim of the aperture ring, which is not on the beam axis.
	 *
	 * WHY A TABLE HERE RATHER THAN A CONSTANT ON THE PAWN. Only ATraceCharacter can parent a marker
	 * to a gun, and that file belongs to the viewmodel slice; this file's job is to be right about
	 * the gun that is on screen whichever way that slice resolves. When the marker follows the active
	 * weapon, every entry below merely agrees with it and changes no answer — which is a thing
	 * Trace.Smg.Probe MEASURES (MarkerToGunUU) rather than a thing this comment asserts. If the pawn
	 * ever grows a per-gun muzzle accessor, delete this table and call it.
	 */
	struct FTracerGunMuzzle
	{
		const TCHAR* StaticMeshName;
		FVector MeshLocalCm;
	};

	const FTracerGunMuzzle GTracerGunMuzzles[] =
	{
		{ TEXT("SM_Railgun_Body"),    FVector(107.4, 0.0, 4.5) },
		{ TEXT("SM_RailgunSmg_Body"), FVector(58.8,  0.0, 4.5) }
	};

	/**
	 * Applies the first-person re-projection to a world point on the pawn's viewmodel.
	 *
	 * THE SAME MORPH ATraceCharacter::GetViewModelMuzzleViewPoint() APPLIES, and for the same reason:
	 * the rig is tagged EFirstPersonPrimitiveType::FirstPerson, so the renderer does not draw it at
	 * its own world transform. A beam started at the un-morphed point lands beside the barrel instead
	 * of out of it. bIgnoreFirstPersonScale is true for the argument set out in full on that function
	 * — it keeps the near end at the gun's true depth so a 20 uu sheath does not fill the frame.
	 *
	 * Returns @p RawWorld unchanged whenever there is nothing to morph WITH (no camera, first-person
	 * rendering off), which is exactly what the pawn does in the same situation.
	 */
	FVector TracerMorphToFirstPerson(const ATraceCharacter* Shooter, const FVector& RawWorld)
	{
		if (Shooter == nullptr)
		{
			return RawWorld;
		}

		// AActor::FindComponentByClass is const and hands back a mutable component, which is what
		// GetCameraView needs. The pawn has exactly one camera; see the constructor comment there for
		// why calling GetCameraView from a query is safe on it (bUsePawnControlRotation is false).
		UCameraComponent* ShooterCamera = Shooter->FindComponentByClass<UCameraComponent>();
		if (ShooterCamera == nullptr)
		{
			return RawWorld;
		}

		FMinimalViewInfo POV;
		ShooterCamera->GetCameraView(0.f, POV);
		if (!POV.bUseFirstPersonParameters)
		{
			return RawWorld;
		}

		const FVector Morphed = POV.TransformWorldToFirstPerson(RawWorld, /*bIgnoreFirstPersonScale=*/true);
		return Morphed.ContainsNaN() ? RawWorld : Morphed;
	}

	/**
	 * Is this part of the rig actually being DRAWN?
	 *
	 * USceneComponent::IsVisible() covers BOTH of the ways this project hides a weapon — the
	 * SetVisibility() that SetViewModelVisible() uses to put the whole rig away in third person, and
	 * the SetHiddenInGame() that the gun swap uses to put one of the two guns away — because it reads
	 * both flags. Every gun part is hidden individually by both mechanisms, so a per-component answer
	 * is the complete answer.
	 *
	 * *** AND IT MUST NOT WALK UP THE ATTACHMENT CHAIN, WHICH IS THE OBVIOUS "IMPROVEMENT" HERE. ***
	 * IsVisible() is deliberately not parent-aware, so a reader who wants to be thorough reaches for a
	 * loop over GetAttachParent() — and every viewmodel part would immediately test as NOT DRAWN. The
	 * chain from a gun part runs ViewModelRoot -> Camera -> SpringArm -> the capsule, and
	 * UShapeComponent's constructor sets bHiddenInGame = true unconditionally, so the pawn's own root
	 * reports invisible on every frame of every match. The walk would silently switch this whole
	 * resolver off: no gun would ever be found, the beam would fall back to the pawn's marker, and
	 * nothing would log a word about it. Do not add the loop.
	 */
	bool TracerIsComponentDrawn(const USceneComponent* Component)
	{
		return Component != nullptr && Component->IsVisible();
	}

	/**
	 * The muzzle of the weapon the pawn is DRAWING, in world space and un-morphed.
	 *
	 * "Drawing" is decided by the components themselves — a visible static mesh component whose asset
	 * is one of the guns in the table — and not by the weapon selector, because the selector says
	 * what the pawn is HOLDING and those two differ in exactly the case that matters: spec v30 §2
	 * requires the SMG to fall back to the pistol rig when its art is missing, and a beam that
	 * believed the selector would then leave a gun that is not on screen.
	 */
	bool TracerFindDrawnGunMuzzle(const ATraceCharacter* Shooter, FName& OutMeshName,
		FVector& OutMeshLocalCm, FVector& OutRawWorld)
	{
		if (Shooter == nullptr)
		{
			return false;
		}

		TArray<UStaticMeshComponent*, TInlineAllocator<24>> Parts;
		Shooter->GetComponents<UStaticMeshComponent>(Parts);

		int32 BestEntry = INDEX_NONE;
		const UStaticMeshComponent* BestPart = nullptr;

		for (const UStaticMeshComponent* Part : Parts)
		{
			if (Part == nullptr || !Part->IsRegistered() || !TracerIsComponentDrawn(Part))
			{
				continue;
			}
			const UStaticMesh* Mesh = Part->GetStaticMesh();
			if (Mesh == nullptr)
			{
				continue;
			}

			for (int32 Index = 0; Index < UE_ARRAY_COUNT(GTracerGunMuzzles); ++Index)
			{
				if (Mesh->GetFName() != FName(GTracerGunMuzzles[Index].StaticMeshName))
				{
					continue;
				}

				// A tie can only happen if a rig draws two gun bodies at once, which no code path
				// does today. Resolving it by TABLE ORDER rather than by whichever component happened
				// to register first makes the answer deterministic and reviewable: the last entry
				// wins, so adding a gun to the table is what decides it, not the scene graph.
				if (BestEntry == INDEX_NONE || Index > BestEntry)
				{
					BestEntry = Index;
					BestPart = Part;
				}
				break;
			}
		}

		if (BestEntry == INDEX_NONE || BestPart == nullptr)
		{
			return false;
		}

		OutMeshName = FName(GTracerGunMuzzles[BestEntry].StaticMeshName);
		OutMeshLocalCm = GTracerGunMuzzles[BestEntry].MeshLocalCm;

		// The FULL component transform, live: the scale that shrinks a 1.28 m weapon into a viewmodel,
		// every parent's recoil kick, sway, bob and dip. Nothing here caches, so nothing here can go
		// stale when the rig moves.
		OutRawWorld = BestPart->GetComponentTransform().TransformPosition(OutMeshLocalCm);
		return !OutRawWorld.ContainsNaN();
	}
} // namespace

ATraceTracer::ATraceTracer()
{
	// Ticks for one sixth of a second to drive the fade, then deletes itself.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// Cosmetic-only: each machine spawns its own, so this must never go on the wire.
	bReplicates = false;
	SetCanBeDamaged(false);
	InitialLifeSpan = TracerLifeSeconds;

	EffectRoot = CreateDefaultSubobject<USceneComponent>(TEXT("EffectRoot"));
	SetRootComponent(EffectRoot);
	EffectRoot->SetMobility(EComponentMobility::Movable);

	// Constructor-time FObjectFinders (not runtime loads) so the cooker keeps these assets.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));

	// M_TraceNeon is generated by Scripts/generate_content.py into a gitignored content path, so a
	// fresh clone that has not run the generator legitimately does not have it. FObjectFinder logs
	// the miss and we fall back to BasicShapeMaterial: the beam loses its glow but the game plays.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TEXT("/Game/Generated/Materials/M_TraceNeon.M_TraceNeon"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> AdditiveFinder(TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

	if (NeonFinder.Succeeded())
	{
		NeonMaterial = NeonFinder.Object;
	}
	if (AdditiveFinder.Succeeded())
	{
		AdditiveMaterial = AdditiveFinder.Object;
	}
	if (BasicFinder.Succeeded())
	{
		FallbackMaterial = BasicFinder.Object;
	}

	// One lambda's worth of shared setup: every piece of this effect is visual-only, collisionless,
	// shadowless and decal-free. Getting any of those wrong on a thing that spawns 80 times a second
	// is a performance bug, and a colliding tracer would break hitscan outright.
	auto MakePiece = [this](const TCHAR* Name, UStaticMesh* Mesh) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* Component = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Component->SetupAttachment(EffectRoot);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCollisionProfileName(TEXT("NoCollision"));
		Component->SetGenerateOverlapEvents(false);
		Component->SetCastShadow(false);
		Component->bReceivesDecals = false;
		Component->bUseAsOccluder = false;
		if (Mesh != nullptr)
		{
			Component->SetStaticMesh(Mesh);
		}
		return Component;
	};

	UStaticMesh* Cylinder = CylinderFinder.Succeeded() ? CylinderFinder.Object : nullptr;
	UStaticMesh* Sphere = SphereFinder.Succeeded() ? SphereFinder.Object : nullptr;

	BeamCore    = MakePiece(TEXT("BeamCore"), Cylinder);
	BeamSheath  = MakePiece(TEXT("BeamSheath"), Cylinder);
	MuzzleFlash = MakePiece(TEXT("MuzzleFlash"), Sphere);

	// No ImpactFlash: spec v4 §4 deleted the sphere at the far end of the beam. The Sphere mesh is
	// still loaded above because the muzzle flash uses it.
}

ATraceTracer* ATraceTracer::Spawn(UWorld* World, const FVector& From, const FVector& To, const FLinearColor& Color, bool bImpacted)
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
		Tracer->InitTracer(From, To, Color, bImpacted);
	}
	return Tracer;
}

bool ATraceTracer::GetGunMuzzleLandmark(FName StaticMeshName, FVector& OutMeshLocalCm)
{
	for (const FTracerGunMuzzle& Entry : GTracerGunMuzzles)
	{
		if (StaticMeshName == FName(Entry.StaticMeshName))
		{
			OutMeshLocalCm = Entry.MeshLocalCm;
			return true;
		}
	}
	return false;
}

bool ATraceTracer::ResolveViewModelBeamStart(const ATraceCharacter* Shooter, FTraceTracerBeamStart& OutInfo)
{
	OutInfo = FTraceTracerBeamStart();
	if (Shooter == nullptr)
	{
		return false;
	}

	// STEP 1: the pawn's own answer, which is also the VISIBILITY GATE for everything below it.
	// GetViewModelMuzzleViewPoint() says no when there is no rig, no camera, or the rig is not being
	// drawn (third person, dead, dedicated server) — and in every one of those cases there is no gun
	// on screen for a beam to leave, whatever the components say. Asking it first means this function
	// cannot start relocating beams in situations the shipped v26 path correctly refused to.
	OutInfo.bHasMarker = Shooter->GetViewModelMuzzleViewPoint(OutInfo.MarkerDrawn);
	if (!OutInfo.bHasMarker)
	{
		return false;
	}
	OutInfo.Start = OutInfo.MarkerDrawn;
	OutInfo.bValid = true;

	// The un-morphed marker, fetched here rather than inside step 3 so that it is populated on EVERY
	// path. A report that leaves it at the origin when no gun mesh was found would make the
	// raw-versus-drawn comparison in Trace.Smg.Probe read as an enormous morph on the fallback rig —
	// a diagnostic that lies in exactly the case somebody is diagnosing.
	FVector RawMarker = FVector::ZeroVector;
	const bool bHasRawMarker = Shooter->DebugGetViewModelMuzzleRaw(RawMarker);
	if (bHasRawMarker)
	{
		OutInfo.MarkerRaw = RawMarker;
	}

	// STEP 2: the gun that is actually drawn, and where its own barrel ends.
	OutInfo.bHasGun = TracerFindDrawnGunMuzzle(Shooter, OutInfo.GunMesh, OutInfo.GunMuzzleLocalCm, OutInfo.GunMuzzleRaw);
	if (!OutInfo.bHasGun)
	{
		// The procedural cube gun, or art this file has never heard of. The marker is the only thing
		// that knows where that is, and it is right about it — this is the fallback rig's path and it
		// is unchanged from v26.
		return true;
	}

	// STEP 3: do the two agree? DebugGetViewModelMuzzleRaw is the un-morphed marker, which is the only
	// thing comparable with a component's world position — comparing a morphed point against a raw one
	// would report a disagreement on every frame and hand the answer to step 4 permanently.
	if (bHasRawMarker)
	{
		OutInfo.MarkerToGunUU = FVector::Dist(RawMarker, OutInfo.GunMuzzleRaw);
		if (OutInfo.MarkerToGunUU <= MuzzleAgreementUU)
		{
			// The marker IS on the gun being drawn. Use the pawn's own answer, so that the shipped
			// path stays the shipped path and this function is provably a no-op for the pistol.
			return true;
		}
	}

	// STEP 4: the marker is somewhere else — on the other gun, or on the rig root. The gun on screen
	// wins, because it is the one the player watches the beam fail to leave.
	OutInfo.Start = TracerMorphToFirstPerson(Shooter, OutInfo.GunMuzzleRaw);
	OutInfo.bFromGunMesh = true;
	return true;
}

bool ATraceTracer::DescribeLocalBeamStart(const UWorld* World, FTraceTracerBeamStart& OutInfo)
{
	return ResolveViewModelBeamStart(GetTracerLocalViewCharacter(World), OutInfo);
}

UMaterialInstanceDynamic* ATraceTracer::MakeMID(UStaticMeshComponent* Mesh, UMaterialInterface* Material)
{
	if (Mesh == nullptr || Material == nullptr)
	{
		return nullptr;
	}
	return Mesh->CreateDynamicMaterialInstance(0, Material);
}

void ATraceTracer::SetMIDColor(UMaterialInstanceDynamic* MID, const FLinearColor& Color, float Intensity, bool bGlowScalar)
{
	if (MID == nullptr)
	{
		return;
	}

	if (bGlowScalar)
	{
		// M_TraceNeon: EmissiveColor = Color * Glow, with Color a plain 0..1 hue and Glow the
		// multiplier that pushes it past the bloom threshold. This is exactly how the arena builder
		// and the trail drive the same material, and matching them matters: a vector parameter is
		// the wrong place to carry a value of 30, and the shipped, proven idiom is the scalar.
		MID->SetVectorParameterValue(TEXT("Color"), Color);
		MID->SetScalarParameterValue(TEXT("Glow"), FMath::Max(0.f, Intensity));

		// BasicShapeMaterial fallback: lit, "Color" only, no Glow. Nothing extra to do - the line
		// above already gave it the right hue, it just will not glow.
		return;
	}

	// EmissiveMeshMaterial (the additive sheath) has no Glow scalar, so intensity has to ride in
	// the colour itself.
	MID->SetVectorParameterValue(TEXT("Color"), Color * FMath::Max(0.f, Intensity));
}

void ATraceTracer::InitTracer(const FVector& From, const FVector& To, const FLinearColor& Color, bool bImpacted)
{
	FVector Delta = To - From;
	double Length = Delta.Size();
	if (Length < static_cast<double>(MinTracerLengthUU))
	{
		return;
	}
	const FVector Dir = Delta / Length;

	// --- first person start: OUT OF THE BARREL (spec v26 §4, see the class comment) --------------
	//
	// Two conditions, in this order, and both are needed:
	//
	//   1. IS THIS MY OWN SHOT? Every machine spawns a tracer for every shot in the match. Only the
	//      one whose origin is essentially at my eye is mine, and only mine may be moved onto my
	//      viewmodel — relocating somebody else's beam onto my barrel would draw their shot leaving
	//      my gun.
	//   2. WHERE IS MY BARREL? Asked of the pawn, which owns the rig and the camera and is the only
	//      thing that can answer. It says no when there is no viewmodel drawn (third person while
	//      carrying, dead, a fresh clone with no art), and then the beam simply starts at the true
	//      origin, exactly as it does on every remote machine. SPEC v30 §5: "my barrel" is now
	//      whichever gun is on screen, which is what ResolveViewModelBeamStart() settles.
	//
	// The point-blank impact-pop dimming that used to live here is gone with the impact sphere it
	// protected against. Nothing else in this effect is large, unlit and drawn at the far end of the
	// ray, so there is no longer a whiteout to defend against there.
	FVector BeamStart = From;
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	if (GetLocalView(GetWorld(), ViewLocation, ViewRotation)
		&& FVector::Dist(ViewLocation, From) < FirstPersonProximityUU)
	{
		const ATraceCharacter* Shooter = GetTracerLocalViewCharacter(GetWorld());
		FTraceTracerBeamStart Muzzle;
		if (ResolveViewModelBeamStart(Shooter, Muzzle))
		{
			// Measured ALONG THE SHOT, not as a straight distance: a muzzle that is off to one side of
			// the ray still has plenty of beam left in front of it, and a straight-line test would
			// reject those. What must not happen is starting at or past the impact, which draws the
			// beam backwards through the player's own face.
			const double Remaining = FVector::DotProduct(To - Muzzle.Start, Dir);
			if (Remaining >= MinBeamBeyondMuzzleUU)
			{
				BeamStart = Muzzle.Start;
			}

			// VERBOSE, and it fires per shot: this is the one line that says WHICH GUN the beam was
			// placed on and whether the pawn's marker had to be overridden to do it. Trace.Smg.Probe
			// prints the same fields on demand, from the same function, without firing.
			UE_LOG(LogTraceGame, Verbose,
				TEXT("TRACER MUZZLE: gun=%s source=%s markerToGun=%.3fuu start=%s beamBeyond=%.1fuu"),
				Muzzle.bHasGun ? *Muzzle.GunMesh.ToString() : TEXT("none (fallback rig)"),
				Muzzle.bFromGunMesh ? TEXT("GUN MESH (marker overridden)") : TEXT("pawn marker"),
				Muzzle.MarkerToGunUU, *Muzzle.Start.ToCompactString(),
				FVector::DotProduct(To - Muzzle.Start, Dir));
		}
	}

	// Re-derive from the (possibly moved) start so the beam still lands exactly on the impact point.
	// This is the whole trick: the near end moves to the gun, the far end does not move at all, and
	// the beam therefore converges on the crosshair.
	Delta = To - BeamStart;
	Length = Delta.Size();
	if (Length < static_cast<double>(MinTracerLengthUU))
	{
		return;
	}
	const FVector BeamDir = Delta / Length;

	// --- width (spec v4 §4) ---------------------------------------------------------------------
	//
	// Read fresh per shot, so dragging the Tracer sliders in Project Settings retunes the beam with
	// PIE running - that is the point of moving these out of this file's constants.
	//
	// Radius in, diameter out: /Engine/BasicShapes primitives are 100 uu ACROSS, so a diameter is
	// what divides cleanly into a mesh scale. The doubling happens exactly here so nothing further
	// down has to remember which unit it is holding.
	const UTraceSettings& Settings = UTraceSettings::Get();

	const float CoreRadiusMin = FMath::Clamp(Settings.TracerRadiusMinUU, MinSafeRadiusUU, MaxSafeRadiusUU);
	const float CoreRadiusMax = FMath::Clamp(Settings.TracerRadiusMaxUU, CoreRadiusMin, MaxSafeRadiusUU);
	const float CoreRadius = FMath::Clamp(
		static_cast<float>(Length) * FMath::Max(0.f, Settings.TracerRadiusPerLength),
		CoreRadiusMin, CoreRadiusMax);

	CoreDiameter = 2.f * CoreRadius;
	SheathDiameter = CoreDiameter * FMath::Max(1.f, Settings.TracerSheathRadiusRatio);
	MuzzleDiameter = 2.f * FMath::Clamp(Settings.TracerMuzzleRadiusUU, MinSafeRadiusUU, MaxSafeRadiusUU);

	// Local +Z runs down the shot, so every piece can be placed by its distance along the beam.
	SetActorLocationAndRotation(BeamStart, FRotationMatrix::MakeFromZ(BeamDir).ToQuat());

	ShotColor = Color;
	HotColor = FMath::Lerp(Color, FLinearColor::White, HotColorWhiteMix);

	const double HalfLength = Length * 0.5;
	const double LengthScale = Length / static_cast<double>(BasicShapeExtentUU);

	// --- beam core ------------------------------------------------------------------------------
	if (BeamCore != nullptr)
	{
		BeamCore->SetRelativeLocation(FVector(0.0, 0.0, HalfLength));
		CoreMID = MakeMID(BeamCore, (NeonMaterial != nullptr) ? NeonMaterial.Get() : FallbackMaterial.Get());
		if (BeamCore->GetStaticMesh() == nullptr)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("ATraceTracer: /Engine/BasicShapes/Cylinder did not resolve; beam will be invisible."));
		}
	}

	// --- beam sheath ----------------------------------------------------------------------------
	// Additive or nothing. An OPAQUE sheath would swallow the core it is supposed to surround, which
	// looks worse than having no sheath at all, so if the additive material is missing this beat is
	// simply dropped and the core plus bloom carries the effect.
	if (BeamSheath != nullptr)
	{
		if (AdditiveMaterial != nullptr)
		{
			BeamSheath->SetRelativeLocation(FVector(0.0, 0.0, HalfLength));
			SheathMID = MakeMID(BeamSheath, AdditiveMaterial.Get());
		}
		else
		{
			BeamSheath->SetVisibility(false);
		}
	}

	// --- muzzle flash ---------------------------------------------------------------------------
	// Not the sphere spec v4 §4 removed - that one was at the far end. This is the one on the
	// viewmodel, and it is what makes a first-person shot visible at all, so it is switchable rather
	// than deleted.
	bMuzzleVisible = Settings.bTracerMuzzleFlash && (Length >= MinLengthForMuzzleFlashUU);
	if (MuzzleFlash != nullptr)
	{
		if (bMuzzleVisible)
		{
			MuzzleFlash->SetRelativeLocation(FVector::ZeroVector);
			MuzzleMID = MakeMID(MuzzleFlash, (NeonMaterial != nullptr) ? NeonMaterial.Get() : FallbackMaterial.Get());
		}
		else
		{
			MuzzleFlash->SetVisibility(false);
		}
	}

	// NO IMPACT POP. Spec v4 §4: "Remove the sphere from the end of the bullet tracer hitscan
	// animation, so it's just a bullet trace." The beam still terminates exactly on the impact
	// point, which is the information the sphere was covering up. bImpacted is now unused; see the
	// note on Spawn() in the header for why the parameter survives.
	(void)bImpacted;

	// Cache the length-driven scale on the components that need it, then let ApplyFade own the rest.
	if (BeamCore != nullptr)
	{
		BeamCore->SetRelativeScale3D(FVector(1.0, 1.0, LengthScale));
	}
	if (BeamSheath != nullptr)
	{
		BeamSheath->SetRelativeScale3D(FVector(1.0, 1.0, LengthScale));
	}

	// Frame one must already look right: this is hitscan, so there is no state before "fully drawn".
	ApplyFade(0.f);

	// VERBOSE ON PURPOSE - this fires up to ~80 times a second in a full match. Enable it with
	// "log LogTraceGame Verbose" when the beam needs inspecting; do not conclude from its silence
	// that the tracer is not spawning. (Trace.TestBeam logs at Display and is the better probe.)
	UE_LOG(LogTraceGame, Verbose,
		TEXT("TRACER: start=%s len=%.0f coreD=%.2f sheathD=%.2f muzzleD=%.2f coreMat=%s coreParent=%s bounds=%s r=%.0f vis=%d sheathMat=%s"),
		*BeamStart.ToCompactString(), Length, CoreDiameter, SheathDiameter, MuzzleDiameter,
		(BeamCore != nullptr) ? *GetNameSafe(BeamCore->GetMaterial(0)) : TEXT("-"),
		(CoreMID != nullptr) ? *GetNameSafe(CoreMID->Parent) : TEXT("-"),
		(BeamCore != nullptr) ? *BeamCore->Bounds.Origin.ToCompactString() : TEXT("-"),
		(BeamCore != nullptr) ? BeamCore->Bounds.SphereRadius : 0.f,
		(BeamCore != nullptr && BeamCore->IsVisible()) ? 1 : 0,
		(BeamSheath != nullptr) ? *GetNameSafe(BeamSheath->GetMaterial(0)) : TEXT("-"));
}

void ATraceTracer::ApplyFade(float Alpha)
{
	Alpha = FMath::Clamp(Alpha, 0.f, 1.f);

	// Ease-out on intensity: full brightness on arrival, then a fast drop with a short tail. That
	// asymmetry is what reads as "weighty" rather than "a light switch".
	const float Fade = FMath::Pow(1.f - Alpha, 2.2f);

	const double DiameterScale = 1.0 / static_cast<double>(BasicShapeExtentUU);

	// --- core: stays sharp, thins as it dies ----------------------------------------------------
	// Gated on the COMPONENT, not on the MID: if no material resolved at all we still want the
	// geometry at the right thickness rather than a 100 uu wide default cylinder.
	if (BeamCore != nullptr)
	{
		const double Thickness = CoreDiameter * (0.35 + 0.65 * Fade) * DiameterScale;
		const FVector Scale = BeamCore->GetRelativeScale3D();
		BeamCore->SetRelativeScale3D(FVector(Thickness, Thickness, Scale.Z));
		SetMIDColor(CoreMID, HotColor, CoreIntensity * Fade, /*bGlowScalar=*/true);
	}

	// --- sheath: collapses inward faster than the core, so the beam visibly tightens ------------
	if (BeamSheath != nullptr && SheathMID != nullptr)
	{
		const float SheathFade = Fade * Fade;
		const double Thickness = SheathDiameter * (0.25 + 0.75 * SheathFade) * DiameterScale;
		const FVector Scale = BeamSheath->GetRelativeScale3D();
		BeamSheath->SetRelativeScale3D(FVector(Thickness, Thickness, Scale.Z));
		SetMIDColor(SheathMID, ShotColor, SheathIntensity * SheathFade, /*bGlowScalar=*/false);
	}

	// --- muzzle: brightest thing on screen for a moment, gone before the beam is -----------------
	if (bMuzzleVisible && MuzzleFlash != nullptr)
	{
		const float MuzzleAlpha = FMath::Clamp(Alpha / MuzzleFlashLifeFraction, 0.f, 1.f);
		const float MuzzleFade = 1.f - MuzzleAlpha;
		const double Size = MuzzleDiameter * (0.4 + 0.6 * MuzzleFade) * DiameterScale;
		MuzzleFlash->SetRelativeScale3D(FVector(Size));
		SetMIDColor(MuzzleMID, HotColor, MuzzleIntensity * MuzzleFade * MuzzleFade, /*bGlowScalar=*/true);

		if (MuzzleAlpha >= 1.f)
		{
			MuzzleFlash->SetVisibility(false);
			bMuzzleVisible = false;
		}
	}

	// The impact beat used to be here: an expanding sphere at the hit point. Deleted, spec v4 §4.
}

void ATraceTracer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	Age += DeltaSeconds;
	ApplyFade(Age / TracerLifeSeconds);
}

// -------------------------------------------------------------------------------------------
// Trace.TestBeam — look at the effect without having to be shooting
//
// Worth its thirty lines. A beam fired in first person runs almost straight away from the camera,
// so a screenshot taken mid-burst shows it nearly edge on and proves very little about whether the
// geometry, the material or the fade are right. This spawns beams ACROSS the view, at a known
// distance, on a timer, so an offscreen automated run can capture the effect square-on. It is also
// the fastest way to re-tune the look without launching a match.
// -------------------------------------------------------------------------------------------
#if !UE_BUILD_SHIPPING
namespace
{
	FTSTicker::FDelegateHandle GTestBeamTicker;
	float GTestBeamRemaining = 0.f;
	float GTestBeamAccumulator = 0.f;

	/** Spawns one broadside beam and one down-the-barrel beam in front of the local camera. */
	void SpawnTestBeams(UWorld* World, const FVector& ViewLocation, const FRotator& ViewRotation)
	{
		const FRotationMatrix Axes(ViewRotation);
		const FVector Forward = Axes.GetScaledAxis(EAxis::X);
		const FVector Right = Axes.GetScaledAxis(EAxis::Y);
		const FVector Up = Axes.GetScaledAxis(EAxis::Z);

		// Broadside: crosses the whole field of view 600 uu out, so nothing about it is foreshortened.
		const FVector Centre = ViewLocation + Forward * 600.0 + Up * -40.0;
		const FVector BroadsideA = Centre - Right * 700.0;
		const FVector BroadsideB = Centre + Right * 700.0;
		// SPEC v26 §5 — DERIVED, NOT RETYPED. These two were pasted copies of the pre-v26 team
		// literals, so the moment TraceTeamColor moved they became the only tracers in the build
		// still drawn in the old cyan/orange — a test beam that no longer looks like the thing it
		// exists to test. They now read the one palette function every other consumer reads, and
		// they follow it wherever it goes next.
		ATraceTracer::Spawn(World, BroadsideA, BroadsideB, TraceTeamColor(ETraceTeam::Blue), /*bImpacted=*/true);

		// Down the barrel: the real first-person case, including the viewmodel offset.
		ATraceTracer::Spawn(World, ViewLocation + Forward * 22.0, ViewLocation + Forward * 1400.0,
			TraceTeamColor(ETraceTeam::Orange), /*bImpacted=*/true);
	}

	void ArmTestBeam(float DurationSeconds)
	{
		GTestBeamRemaining = FMath::Clamp(DurationSeconds, 0.5f, 120.f);
		GTestBeamAccumulator = 0.f;

		if (GTestBeamTicker.IsValid())
		{
			return; // already running; the line above just extended it
		}

		UE_LOG(LogTraceGame, Display, TEXT("TESTBEAM: spawning railgun beams for %.1fs."), GTestBeamRemaining);

		GTestBeamTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[](float Delta) -> bool
			{
				GTestBeamRemaining -= Delta;
				if (GTestBeamRemaining <= 0.f)
				{
					UE_LOG(LogTraceGame, Display, TEXT("TESTBEAM: done."));
					GTestBeamTicker.Reset();
					return false;
				}

				GTestBeamAccumulator += Delta;
				if (GTestBeamAccumulator < 0.2f)
				{
					return true;
				}
				GTestBeamAccumulator = 0.f;

				if (GEngine == nullptr)
				{
					return true;
				}
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					UWorld* World = Context.World();
					if (World == nullptr || World->GetNetMode() == NM_DedicatedServer)
					{
						continue;
					}
					FVector ViewLocation = FVector::ZeroVector;
					FRotator ViewRotation = FRotator::ZeroRotator;
					if (!GetLocalView(World, ViewLocation, ViewRotation))
					{
						UE_LOG(LogTraceGame, Display, TEXT("TESTBEAM: no local view in world %s"), *GetNameSafe(World));
						continue;
					}
					SpawnTestBeams(World, ViewLocation, ViewRotation);
					UE_LOG(LogTraceGame, Display, TEXT("TESTBEAM: spawned at view %s"), *ViewLocation.ToCompactString());
				}
				return true;
			}), 0.f);
	}
} // namespace

static FAutoConsoleCommand GTraceTestBeamCmd(
	TEXT("Trace.TestBeam"),
	TEXT("Trace.TestBeam [seconds] — spawn railgun beams in front of the local camera so the effect can be inspected."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		ArmTestBeam((Args.Num() > 0) ? FCString::Atof(*Args[0]) : 10.f);
	}));
#endif // !UE_BUILD_SHIPPING
