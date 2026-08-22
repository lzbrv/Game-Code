#include "Gameplay/TraceTracer.h"

#include "Camera/CameraComponent.h"           // spec v30 §5 — the first-person morph, applied to a
#include "Camera/CameraTypes.h"               //   point the pawn's own marker does not cover
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"                      // TActorIterator — Trace.Fx.Beam follows the newest shot
#include "HAL/IConsoleManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "Math/UnrealMathUtility.h"

#include "Core/TraceCharacter.h"               // GetViewModelMuzzleViewPoint (spec v26 §4)
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"                        // TraceTeamColor — spec v26 §5, the ONE team palette

/**
 * *** THE ONE SIZE KNOB FOR THE WHOLE BEAM. *** See ATraceTracer::GetBeamProfileScale in the header
 * for the argument; this is where the default was chosen.
 *
 * 0.55 WAS PICKED BY LOOKING, and here is the whole of what was looked at. At 1.0 — the frame the
 * user called "a bit too large", Saved/Screenshots/v31integ_45_smg_firing.png as it stood before
 * this pass — the shot is a white plank filling roughly a third of the frame's width, flat enough
 * to read as one of the arena's structural rails. Re-photographed at 0.55 by the same
 * Trace.Integ.WalkGuns run, the same shot is a slender bolt that converges on the crosshair with
 * the taper visible along it. A 0.40 run was taken as the other bracket: measurably smaller (the
 * muzzle flash's disc went 666 -> 411 -> 283 px of frame across 1.0 / 0.55 / 0.40) but past what
 * was asked for — the note was "a BIT too large", and 0.55 is a 45% cut, not a disappearance.
 *
 * READ THAT BRACKET AS A MEASUREMENT OF THE CORE BEAM AND NOT OF THE FLASH. The three disc widths
 * above were taken while the muzzle cone was still drawn on the OPAQUE emissive material, so what
 * they measured was the silhouette of a solid disc, which is a function of its size alone. The cone
 * is additive now (see the blend argument at the muzzle-flash beat in InitTracer) and its apparent
 * size is a function of its brightness curve as well as its geometry — it is at its widest exactly
 * when it has faded to nothing. The 0.55 chosen here is still the right number for the beam, but a
 * future bracket of the FLASH has to be re-measured; the old px figures do not carry over.
 *
 * The knob is live, so the next pass can re-photograph a different value without a rebuild, which
 * is most of the point of it being a CVar: -dpcvars=Trace.Fx.BeamScale=0.40 on the editor command
 * line is how the bracket above was taken, no rebuild involved.
 *
 * File-scope `static`, like TraceCore.cpp's block of the same: internal linkage, so the jumbo build
 * cannot collide it with anything, and no anonymous namespace for the collision checker to flag.
 */
static TAutoConsoleVariable<float> GCVarTracerBeamScale(
	TEXT("Trace.Fx.BeamScale"),
	0.55f,
	TEXT("One multiplier on the ENTIRE authored beam profile — core muzzle and tip radius, halo ")
	TEXT("radius, and the muzzle-flash cone's radius and height — applied after the legibility floor ")
	TEXT("and the shipped Tracer settings. Every FX-doc proportion (tip/muzzle, halo/muzzle, the ")
	TEXT("cone's 16:30) is preserved because they all move together; only the absolute size changes. ")
	TEXT("1.0 is the doc's authored size, which photographs as a plank at viewmodel range. Clamped ")
	TEXT("0.05..4.0. Trace.Fx.Beam's three ABSOLUTE rows grade doc x this; its two RATIO rows are ")
	TEXT("scale-invariant and grade the doc's raw proportions whatever this is set to."),
	ECVF_Default);

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

	/** The SMG's body mesh, by the one name the table above already uses. */
	bool TracerIsSmgBodyMesh(FName MeshName)
	{
		return MeshName == FName(TEXT("SM_RailgunSmg_Body"));
	}

	/**
	 * SPEC v32 §2 — THE ONE MUZZLE CONE THAT IS ON THE LOCAL PLAYER'S OWN VIEWMODEL.
	 *
	 * Weak, because the tracer it points at self-deletes on its own life span and nothing here is
	 * allowed to keep it alive a frame longer. Null whenever no first-person shot is currently
	 * flashing, which is the common case.
	 *
	 * See ATraceTracer::MuzzleFlashSeconds for the whole argument. In one line: the SMG fires every
	 * 0.100 s, the cone lasts 0.280 s, and three unlit emissive cones summed on top of each other a
	 * hand's breadth from the camera is a flashbang. So the newest first-person shot RESTARTS this
	 * one rather than adding to it. Remote shooters are untouched — they each keep their own.
	 */
	TWeakObjectPtr<ATraceTracer> GFirstPersonFlashOwner;
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

	// SPEC v32 §1 — the meshes and the three materials now come from UTraceFxShapes, whose CDO holds
	// them behind the same constructor-time FObjectFinders that used to sit here. That is what keeps
	// the cooker following them; a bare runtime LoadObject leaves no cook reference and resolves to
	// null in a packaged build. Nothing about the resolution changed, only where it lives, so that
	// §3's core halo and §4's stab streak degrade through the same ladder as this beam instead of
	// each re-implementing the fallback rule with slightly different edges.
	auto MakePiece = [this](const TCHAR* Name, UStaticMesh* Shape) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* Component = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Component->SetupAttachment(EffectRoot);
		UTraceFxShapes::ConfigureFxComponent(Component);
		if (Shape != nullptr)
		{
			Component->SetStaticMesh(Shape);
		}
		return Component;
	};

	UStaticMesh* Cylinder = UTraceFxShapes::GetCylinder();

	// THE TAPER IS THREE COMPONENTS. Named by index rather than Near/Mid/Far so that changing
	// BeamTaperSegments is a one-line change here and not a rename; the loop is bounded by the
	// header's constant, and the static_assert is what stops that constant and this fixed-size
	// UPROPERTY array (which UHT needs as a literal) from drifting apart.
	static_assert(UE_ARRAY_COUNT(BeamSegments) == BeamTaperSegments,
		"BeamSegments[] and BeamTaperSegments must agree; UHT needs the array bound as a literal.");
	for (int32 Index = 0; Index < BeamTaperSegments; ++Index)
	{
		BeamSegments[Index] = MakePiece(*FString::Printf(TEXT("BeamSegment%d"), Index), Cylinder);
	}

	BeamHalo    = MakePiece(TEXT("BeamHalo"), Cylinder);
	MuzzleFlash = MakePiece(TEXT("MuzzleFlash"), UTraceFxShapes::GetCone());

	// No ImpactFlash: spec v4 §4 deleted the sphere at the far end of the beam. And no sphere at the
	// near end either since v32 — the muzzle flash is the FX doc's cone now.
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

float ATraceTracer::GetBeamProfileScale()
{
	// Clamped here and only here, so every consumer — the effect and the harness that grades it —
	// sees the same number and neither can be handed a zero or a negative from the console.
	return FMath::Clamp(GCVarTracerBeamScale.GetValueOnAnyThread(), 0.05f, 4.0f);
}

ATraceTracer* ATraceTracer::GetNewestTracer(const UWorld* World, bool bFirstPersonOnly)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	ATraceTracer* Newest = nullptr;
	for (TActorIterator<ATraceTracer> It(const_cast<UWorld*>(World)); It; ++It)
	{
		ATraceTracer* Candidate = *It;
		if (Candidate == nullptr || !IsValid(Candidate))
		{
			continue;
		}
		// "Mine", read off the beam itself: bFirstPersonShot is set only when this machine's own
		// viewmodel muzzle was resolved and the beam was moved onto it, which no remote shooter's
		// tracer can ever satisfy. See the parameter's note in the header for why the probe needs it.
		if (bFirstPersonOnly && !Candidate->bFirstPersonShot)
		{
			continue;
		}
		// Newest by SPAWN TIME, not by iteration order: the actor iterator's order is a property of
		// the world's actor array and says nothing about which shot is the most recent one.
		if (Newest == nullptr || Candidate->SpawnTimeSeconds > Newest->SpawnTimeSeconds)
		{
			Newest = Candidate;
		}
	}
	return Newest;
}

void ATraceTracer::RetireMuzzleFlash()
{
	if (MuzzleFlash != nullptr)
	{
		MuzzleFlash->SetVisibility(false);
	}
	bMuzzleVisible = false;
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
				bFirstPersonShot = true;
			}

			// --- SPEC v32 §2: WHICH OF THE TWO PROFILES IS THIS SHOT? --------------------------
			//
			// The pistol is r 3.0 -> 1.3 uu with a 16 uu cone; the SMG is 2.4 -> 1.3 with 13. Asked
			// of the GUN THAT IS DRAWN, which the resolver above has already found for its own
			// purposes — the same fact, read once. GetShownGun() is the second source only because
			// the resolver legitimately reports no gun on the procedural cube rig, where the pawn's
			// weapon selector is the only thing left that knows.
			//
			// AND IT IS ONLY ASKED FOR THE LOCAL SHOOTER'S OWN BEAM. ATraceTracer::Spawn is handed
			// two points and a colour; it is not told who fired, and this file does not own the
			// call site that would have to tell it (UTraceWeaponComponent::PlayLocalTracer). So a
			// REMOTE player's beam draws the pistol profile regardless of what they are holding.
			// That is a deliberate, bounded inaccuracy: the difference is 0.6 uu of radius at the
			// muzzle of a gun that is by definition metres away and usually tens of metres, i.e.
			// well under a pixel, while the case it IS right for — the gun filling the bottom of
			// your own screen — is the case the FX doc was written for. Flagged in the pass report
			// as the one change that would need a file outside this slice.
			if (Muzzle.bHasGun)
			{
				ProfileGunMesh = Muzzle.GunMesh;
				bSmgProfile = TracerIsSmgBodyMesh(Muzzle.GunMesh);
			}
			else if (Shooter != nullptr)
			{
				bSmgProfile = (Shooter->GetShownGun() == ATraceCharacter::EShownGun::Smg);
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

	// --- WIDTH: THE FX DOC'S AUTHORED PROFILE, SCALED BY THE SHIPPED KNOBS ----------------------
	//
	// SPEC v32 §2 gives absolute radii — 3.0 -> 1.3 uu (pistol), 2.4 -> 1.3 (SMG), halo 5.2, cone 16
	// / 13 x 30 — and UTraceSettings' Tracer block is a shipped panel that must keep working. See
	// HaloRatioReferenceDefault in the header for how the two are reconciled: the knobs become
	// SCALES, normalised so that at their shipped defaults the doc's numbers appear unmodified.
	//
	// Read fresh per shot, so dragging the sliders in Project Settings still retunes the beam with
	// PIE running - that is the point of these living in settings at all.
	const UTraceSettings& Settings = UTraceSettings::Get();

	const float CoreRadiusMin = FMath::Clamp(Settings.TracerRadiusMinUU, MinSafeRadiusUU, MaxSafeRadiusUU);
	const float CoreRadiusMax = FMath::Clamp(Settings.TracerRadiusMaxUU, CoreRadiusMin, MaxSafeRadiusUU);

	const float ProfileMuzzleRadius = bSmgProfile ? SmgCoreMuzzleRadiusUU : PistolCoreMuzzleRadiusUU;

	// *** THE PROPORTIONAL MODEL SURVIVES, AS A FLOOR AT LONG RANGE, AND HERE IS WHY. ***
	//
	// The pre-v32 beam had no authored radius at all: it was radius-proportional-to-length, and the
	// argument for that in TraceSettings.h is still true and still good — "any radius that is sane
	// across a corridor is a sub-pixel thread across the field". The FX doc's 3.0 uu is authored for
	// a preview turntable a metre or two from the camera, exactly like its 2.6 m length, and at
	// 8000 uu it is a quarter of a pixel.
	//
	// So: the doc's radius WINS everywhere it is legible, and the old proportional radius takes over
	// only where it is larger, which at the shipped 0.00155/uu means shots beyond ~1935 uu. Under
	// that — every viewmodel shot, every corridor fight, everything a verifier will photograph — the
	// muzzle measures exactly 3.0 uu. Over it, the WHOLE profile is scaled by one factor so the
	// taper ratio, the halo and the cone keep their authored proportions instead of the beam growing
	// fat inside a fixed halo.
	const float LegibilityRadius = FMath::Clamp(
		static_cast<float>(Length) * FMath::Max(0.f, Settings.TracerRadiusPerLength),
		CoreRadiusMin, CoreRadiusMax);

	CoreMuzzleRadiusUU = FMath::Clamp(FMath::Max(ProfileMuzzleRadius, LegibilityRadius),
		CoreRadiusMin, CoreRadiusMax);

	const float ProfileScale = CoreMuzzleRadiusUU / FMath::Max(ProfileMuzzleRadius, KINDA_SMALL_NUMBER);
	CoreTipRadiusUU = CoreTipRadiusProfileUU * ProfileScale;

	// The halo knob, normalised. FMath::Max(1.f, ...) is the tooltip's own floor.
	const float HaloRatio = FMath::Max(1.f, Settings.TracerSheathRadiusRatio);
	HaloRadiusUU = FMath::Clamp(
		HaloRadiusProfileUU * ProfileScale * (HaloRatio / HaloRatioReferenceDefault),
		MinSafeRadiusUU, MaxSafeRadiusUU);
	HaloOpacityValue = HaloOpacityProfile;

	// The cone knob, normalised, applied to BOTH cone dimensions so it stays the doc's shape.
	const float FlashKnobScale =
		FMath::Clamp(Settings.TracerMuzzleRadiusUU, MinSafeRadiusUU, MaxSafeRadiusUU) / FlashRadiusReferenceDefaultUU;
	FlashConeRadiusUU = (bSmgProfile ? SmgFlashConeRadiusUU : PistolFlashConeRadiusUU) * FlashKnobScale;
	FlashConeHeightUU = FlashConeHeightProfileUU * FlashKnobScale;

	// --- AND THEN THE ONE SIZE KNOB, LAST, ON EVERYTHING AT ONCE --------------------------------
	//
	// Trace.Fx.BeamScale. See ATraceTracer::GetBeamProfileScale in the header for what it is for (at
	// 1.0 the doc's authored beam photographs as a white plank the width of the arena's rails) and
	// why it is a CVar. What matters HERE is that it is applied at the END and to EVERY dimension:
	//
	//   * At the end, so it scales the radius that actually won — the doc's, or the legibility floor
	//     where the floor is larger. A long shot is the fattest beam in the game and the one the
	//     plank complaint is loudest about, so a knob that the floor could out-vote would miss it.
	//   * To every dimension, so nothing above it can be told apart afterwards: multiply five
	//     numbers by one factor and every ratio between them is unchanged. The FX doc's shape is
	//     therefore intact at any setting, which is precisely what lets Trace.Fx.Beam keep grading
	//     the doc's raw proportions on its two ratio rows while its absolute rows grade doc x knob.
	//
	// It deliberately overrides the MinSafeRadiusUU/CoreRadiusMin floors above rather than being
	// clamped by them: those exist to stop a bad ini producing a zero or a wall, and this knob has
	// its own 0.05..4.0 clamp for the same job. A knob that could not take the beam below the
	// settings' minimum radius would be unable to do the one thing it was added for.
	const float BeamProfileScale = GetBeamProfileScale();
	CoreMuzzleRadiusUU *= BeamProfileScale;
	CoreTipRadiusUU *= BeamProfileScale;
	HaloRadiusUU *= BeamProfileScale;
	FlashConeRadiusUU *= BeamProfileScale;
	FlashConeHeightUU *= BeamProfileScale;

	BeamLengthUU = static_cast<float>(Length);

	// Local +Z runs down the shot, so every piece can be placed by its distance along the beam.
	SetActorLocationAndRotation(BeamStart, FRotationMatrix::MakeFromZ(BeamDir).ToQuat());

	ShotColor = Color;
	HotColor = FMath::Lerp(Color, FLinearColor::White, HotColorWhiteMix);

	// --- beam core: ONE MID SHARED BY THE THREE TAPER SEGMENTS -----------------------------------
	//
	// The MID is made on the first segment and assigned to the other two, so the three cylinders are
	// literally the same material instance. They cannot drift apart, and the per-frame fade is one
	// parameter write rather than three.
	if (BeamSegments[0] != nullptr)
	{
		CoreMID = UTraceFxShapes::MakeGlowMID(BeamSegments[0], 0, ETraceFxBlend::Emissive, CoreBlend);
		if (BeamSegments[0]->GetStaticMesh() == nullptr)
		{
			UE_LOG(LogTraceGame, Verbose, TEXT("ATraceTracer: /Engine/BasicShapes/Cylinder did not resolve; beam will be invisible."));
		}
	}
	for (int32 Index = 1; Index < BeamTaperSegments; ++Index)
	{
		if (BeamSegments[Index] != nullptr && CoreMID != nullptr)
		{
			BeamSegments[Index]->SetMaterial(0, CoreMID);
		}
	}

	// --- the halo sleeve --------------------------------------------------------------------------
	//
	// NON-OCCLUDING OR NOTHING. An opaque sleeve would swallow the core it is supposed to surround,
	// which looks worse than having no sleeve at all — so the request is Translucent, the library
	// degrades it to Additive (this project has no translucent parent; see ETraceFxBlend), and if
	// even that is unavailable the beat is dropped and the core plus bloom carries the effect.
	// UTraceFxShapes::ResolveBlend would happily hand back the OPAQUE emissive material as its next
	// rung, which is right for a core and wrong for a sleeve, so that one case is rejected here.
	if (BeamHalo != nullptr)
	{
		HaloMID = UTraceFxShapes::MakeGlowMID(BeamHalo, 0, ETraceFxBlend::Translucent, HaloBlend);
		bHaloVisible = (HaloMID != nullptr)
			&& (HaloBlend == ETraceFxBlend::Additive || HaloBlend == ETraceFxBlend::Translucent)
			&& (HaloRadiusUU > CoreMuzzleRadiusUU);
		if (!bHaloVisible)
		{
			BeamHalo->SetVisibility(false);
		}
	}

	// --- muzzle flash -----------------------------------------------------------------------------
	// Not the sphere spec v4 §4 removed - that one was at the far end. This is the one on the
	// viewmodel, and it is what makes a first-person shot visible at all, so it is switchable rather
	// than deleted. bTracerMuzzleFlash is a shipped setting and still suppresses it outright.
	//
	// *** NON-OCCLUDING OR NOTHING, FOR THE SAME REASON AS THE HALO AND MUCH MORE SO. ***
	//
	// This asked for Emissive — M_TraceNeon, which is unlit but OPAQUE — until a verifier measured
	// what that does. The cone grows to r 28 uu about 85 uu from the eye, which is a disc 27% of the
	// frame wide, and an opaque disc there is a hole punched in the world: photographed at
	// Saved/Screenshots/v34knifeFINAL_42_pistol_firing.png the arena rail read (169,229,254) at
	// x=1100 and was cut dead at the disc edge, x=1050 reading a flat (40,55,124). Worse, the piece
	// it hid best was the beam it belongs to — the core is a 1.6 uu thread that starts INSIDE this
	// cone and comes out the far end of it, so an opaque cone deletes the first metres of every
	// first-person shot. And because the fade rides on brightness while the geometry keeps growing,
	// a faded-out OPAQUE cone is not gone, it is a dark matte disc: the (40,55,124) above is this
	// cone at age ~0.2 s with its emissive already down at 8% of full.
	//
	// So the request is Translucent, the library degrades it to Additive (this project has no
	// translucent parent; see ETraceFxBlend), and the OPAQUE rungs below that — Emissive and
	// Fallback — are REJECTED here rather than accepted, exactly as the halo beat immediately above
	// rejects them. Additive geometry writes no depth: the arena stays visible through the flash, the
	// core stays visible inside it, and a flash faded to zero adds zero and is genuinely gone.
	// If the engine's additive material is ever missing the flash is dropped and the core plus the
	// halo plus the bloom carry the shot, which is worse than a flash and far better than a hole.
	bMuzzleVisible = Settings.bTracerMuzzleFlash && (Length >= MinLengthForMuzzleFlashUU);
	if (MuzzleFlash != nullptr)
	{
		if (bMuzzleVisible)
		{
			MuzzleMID = UTraceFxShapes::MakeGlowMID(MuzzleFlash, 0, ETraceFxBlend::Translucent, FlashBlend);
			bMuzzleVisible = (MuzzleMID != nullptr)
				&& (FlashBlend == ETraceFxBlend::Additive || FlashBlend == ETraceFxBlend::Translucent);
		}
		if (!bMuzzleVisible)
		{
			MuzzleFlash->SetVisibility(false);
		}
	}

	// ONE FLASH ON THE VIEWMODEL, RESTARTED (see MuzzleFlashSeconds in the header). The previous
	// owner is put out before this one lights, so a held SMG trigger cannot stack three cones a
	// hand's breadth from the camera. Same world only: two worlds in one process (PIE) must not put
	// out each other's flashes.
	if (bMuzzleVisible && bFirstPersonShot)
	{
		ATraceTracer* PreviousFlash = GFirstPersonFlashOwner.Get();
		if (PreviousFlash != nullptr && PreviousFlash != this && PreviousFlash->GetWorld() == GetWorld())
		{
			PreviousFlash->RetireMuzzleFlash();
		}
		GFirstPersonFlashOwner = this;
	}

	// NO IMPACT POP. Spec v4 §4: "Remove the sphere from the end of the bullet tracer hitscan
	// animation, so it's just a bullet trace." The beam still terminates exactly on the impact
	// point, which is the information the sphere was covering up. bImpacted is now unused; see the
	// note on Spawn() in the header for why the parameter survives.
	(void)bImpacted;

	// THE CLOCK. Absolute, sampled once, never advanced — see SpawnTimeSeconds in the header.
	const UWorld* ShotWorld = GetWorld();
	SpawnTimeSeconds = (ShotWorld != nullptr) ? ShotWorld->GetTimeSeconds() : 0.0;

	// Frame one must already look right: this is hitscan, so there is no state before "fully drawn".
	UpdateEffect(0.f);

	// VERBOSE ON PURPOSE - this fires up to ~80 times a second in a full match. Enable it with
	// "log LogTraceGame Verbose" when the beam needs inspecting; do not conclude from its silence
	// that the tracer is not spawning. (Trace.Fx.Beam logs at Display and is the better probe.)
	UE_LOG(LogTraceGame, Verbose,
		TEXT("TRACER: profile=%s%s start=%s len=%.1fuu scale=%.3f core=%.3f->%.3fuu halo=%.3fuu op=%.3f "
		     "cone=r%.2f/h%.2fuu blends core=%s halo=%s flash=%s"),
		bSmgProfile ? TEXT("SMG") : TEXT("PISTOL"), bFirstPersonShot ? TEXT(" (first person)") : TEXT(""),
		*BeamStart.ToCompactString(), Length, BeamProfileScale, CoreMuzzleRadiusUU, CoreTipRadiusUU,
		HaloRadiusUU, HaloOpacityValue, FlashConeRadiusUU, FlashConeHeightUU,
		UTraceFxShapes::BlendName(CoreBlend), UTraceFxShapes::BlendName(HaloBlend),
		UTraceFxShapes::BlendName(FlashBlend));
}

void ATraceTracer::UpdateEffect(float AgeSeconds)
{
	// --- HOLD, THEN FADE (FX doc: "Spawn at discharge, hold 0.10 s, fade over the decay") --------
	//
	// The hold is not decoration. A hitscan beam is drawn and gone inside three frames at 60 Hz, and
	// a fade that starts on frame one means the brightest frame the player ever sees is one they
	// were not looking at yet. It also makes the effect MEASURABLE: throughout the hold the geometry
	// is at exactly the doc's radii, so "the taper radii in uu" is a well-defined question with the
	// answer 3.000 / 1.300 rather than "3.0 times whatever the fade was on the frame you sampled".
	// The age this frame's geometry is being laid at, recorded for the probe. See
	// FTraceTracerShotDebug::GeometryAgeSeconds: the muzzle cone's authored SIZE is a function of
	// age, so a probe grading the live cone has to evaluate the doc's curve at the age the
	// transform was actually written at rather than at the age its own sample happened to read.
	GeometryAgeSeconds = AgeSeconds;

	const float DecayAlpha = FMath::Clamp(
		(AgeSeconds - BeamHoldSeconds) / FMath::Max(BeamDecaySeconds, KINDA_SMALL_NUMBER), 0.f, 1.f);

	// Ease-out on intensity: full brightness on arrival, then a fast drop with a short tail. That
	// asymmetry is what reads as "weighty" rather than "a light switch".
	const float Fade = FMath::Pow(1.f - DecayAlpha, 2.2f);

	// --- core: holds the doc's taper, then thins as it dies --------------------------------------
	// The geometry is re-laid every frame rather than only scaled, because a taper is three
	// positions as well as three radii and the two must not be able to disagree. Gated on the
	// COMPONENTS, not on the MID: if no material resolved at all we still want the geometry at the
	// right thickness rather than three 100 uu wide default cylinders.
	const float Thinning = 0.35f + 0.65f * Fade;
	UStaticMeshComponent* Segments[BeamTaperSegments] = { BeamSegments[0], BeamSegments[1], BeamSegments[2] };
	UTraceFxShapes::TaperAlongLocalZ(
		MakeArrayView(Segments, BeamTaperSegments), 0.0, static_cast<double>(BeamLengthUU),
		CoreMuzzleRadiusUU * Thinning, CoreTipRadiusUU * Thinning);
	UTraceFxShapes::SetGlow(CoreMID, CoreBlend, HotColor, CoreIntensity * Fade);

	// --- halo: constant radius, fading opacity ---------------------------------------------------
	//
	// DELIBERATELY NOT COLLAPSING INWARD the way the pre-v32 sheath did. The FX doc's sleeve has one
	// radius and one opacity, and "hold, then fade over the decay" is a statement about the second.
	// Animating the radius as well would mean the number a verifier is told to measure — r 5.2 uu —
	// is only true on the frames nobody sampled.
	if (bHaloVisible && BeamHalo != nullptr)
	{
		UTraceFxShapes::StretchAlongLocalZ(BeamHalo, 0.0, static_cast<double>(BeamLengthUU), HaloRadiusUU);
		UTraceFxShapes::SetGlow(HaloMID, HaloBlend, ShotColor, HaloIntensity, HaloOpacityValue * Fade);
	}

	// --- muzzle: a cone down the beam, growing and dying inside 0.28 s ---------------------------
	//
	// FX doc: "visible for the first 0.28 s of decay, scaling 0.55 -> 3.2x". The doc gives the size
	// curve and not the brightness curve, and the brightness curve is the safety-critical half: 3.2x
	// of a 16 uu radius is a 51 uu cone roughly 85 uu from the shooter's own eye, and unlit additive
	// does not attenuate with distance, so at constant brightness the last frame of every shot would
	// be a white screen. It is therefore read as an EXPANDING SHOCKWAVE — brightest at its smallest,
	// gone by the time it is biggest — which is both how a real muzzle flash looks and the only
	// reading under which the doc's own numbers are usable at this range.
	//
	// AND THE FADE ONLY ACTUALLY REMOVES THE CONE NOW THAT THE BLEND IS ADDITIVE (see the rejection
	// of the opaque rungs in InitTracer). Fading an OPAQUE cone's emissive to zero leaves a
	// full-size dark disc sitting in front of the arena, which is what a verifier photographed; on
	// an additive blend the same curve at the same numbers adds nothing and the cone is simply not
	// there. The shockwave reading is unchanged — it is only now true on screen as well as in the
	// arithmetic.
	if (bMuzzleVisible && MuzzleFlash != nullptr)
	{
		const float FlashAlpha = FMath::Clamp(AgeSeconds / MuzzleFlashSeconds, 0.f, 1.f);
		const float FlashScale = FMath::Lerp(MuzzleFlashStartScale, MuzzleFlashEndScale, FlashAlpha);
		const float FlashFade = 1.f - FlashAlpha;

		// The cone's BASE sits on the muzzle and its apex points down the beam, which is the whole
		// difference between this and the sphere it replaces: a flash that is a shape with a
		// direction reads as "something left the barrel that way".
		UTraceFxShapes::PlaceConeAlongLocalZ(MuzzleFlash, 0.0,
			FlashConeRadiusUU * FlashScale, FlashConeHeightUU * FlashScale);

		// The fade rides in the OPACITY argument, not folded into the intensity, because on an
		// additive blend that is literally what it is: how much of this colour is added to what is
		// behind it. Same idiom as the halo one beat up, and the same reason — SetGlow's additive
		// branch multiplies the two together anyway, so this is the honest place to put it.
		UTraceFxShapes::SetGlow(MuzzleMID, FlashBlend, HotColor, MuzzleIntensity, FlashFade * FlashFade);

		if (FlashAlpha >= 1.f)
		{
			RetireMuzzleFlash();
		}
	}

	// The impact beat used to be here: an expanding sphere at the hit point. Deleted, spec v4 §4.
}

void ATraceTracer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// A STATELESS FUNCTION OF AN ABSOLUTE CLOCK, not an accumulator. DeltaSeconds is deliberately
	// unused: an `Age += Delta` here would double-advance across a hitch and drift over the 0.85 s
	// this actor lives, and every duration in the effect is short enough for that to show. It also
	// removes the sample-before-advancing hazard entirely — there is nothing to advance.
	(void)DeltaSeconds;

	const UWorld* TickWorld = GetWorld();
	const float AgeSeconds = (TickWorld != nullptr)
		? static_cast<float>(TickWorld->GetTimeSeconds() - SpawnTimeSeconds)
		: 0.f;

	UpdateEffect(AgeSeconds);
}

bool ATraceTracer::DescribeShot(FTraceTracerShotDebug& OutInfo) const
{
	OutInfo = FTraceTracerShotDebug();

	if (BeamLengthUU <= 0.f)
	{
		return false; // never drawn: a degenerate segment that returned early from InitTracer
	}

	OutInfo.bSmgProfile = bSmgProfile;
	OutInfo.bFirstPersonShot = bFirstPersonShot;
	OutInfo.GunMesh = ProfileGunMesh;

	OutInfo.ProfileMuzzleRadiusUU = CoreMuzzleRadiusUU;
	OutInfo.ProfileTipRadiusUU = CoreTipRadiusUU;
	OutInfo.ProfileHaloRadiusUU = HaloRadiusUU;
	OutInfo.ProfileHaloOpacity = HaloOpacityValue;
	OutInfo.ProfileConeRadiusUU = FlashConeRadiusUU;
	OutInfo.ProfileConeHeightUU = FlashConeHeightUU;

	OutInfo.CoreBlend = CoreBlend;
	OutInfo.HaloBlend = HaloBlend;
	OutInfo.FlashBlend = FlashBlend;

	const UWorld* DescribeWorld = GetWorld();
	OutInfo.AgeSeconds = (DescribeWorld != nullptr)
		? static_cast<float>(DescribeWorld->GetTimeSeconds() - SpawnTimeSeconds) : 0.f;

	// The age the transforms below were WRITTEN at, straight out of the effect. Reported separately
	// from AgeSeconds on purpose — see FTraceTracerShotDebug::GeometryAgeSeconds.
	OutInfo.GeometryAgeSeconds = GeometryAgeSeconds;

	// --- EVERYTHING BELOW IS READ BACK OFF THE LIVE COMPONENTS ------------------------------------
	//
	// Not recomputed from the constants above. See FTraceTracerShotDebug: a probe that re-derives
	// the number it expects can only ever prove that one constant is spelled the same way twice, and
	// the specific bug this guards against — the 100x metres/centimetres error — would be invisible
	// to it because both copies would be wrong together.
	for (int32 Index = 0; Index < BeamTaperSegments; ++Index)
	{
		const UStaticMeshComponent* Segment = BeamSegments[Index];
		if (Segment == nullptr)
		{
			continue;
		}
		OutInfo.MeasuredSegmentRadiiUU[Index] =
			UTraceFxShapes::RadiusUUFromShapeScale(static_cast<float>(Segment->GetRelativeScale3D().X));
		OutInfo.LengthUU += UTraceFxShapes::LengthUUFromShapeScale(static_cast<float>(Segment->GetRelativeScale3D().Z));
		++OutInfo.SegmentCount;
	}

	// The taper's END radii, extrapolated from the two OUTERMOST measured segments back to the ends
	// of the beam. The segments sit at their own mid-points, so the muzzle radius is half a step
	// beyond the first one; doing that arithmetic here, on measured values, keeps the reported
	// "3.000 uu at the muzzle" a measurement rather than a restatement.
	if (OutInfo.SegmentCount >= 2)
	{
		const float First = OutInfo.MeasuredSegmentRadiiUU[0];
		const float Last = OutInfo.MeasuredSegmentRadiiUU[OutInfo.SegmentCount - 1];
		const float Step = (Last - First) / static_cast<float>(OutInfo.SegmentCount - 1);
		OutInfo.MeasuredMuzzleRadiusUU = First - 0.5f * Step;
		OutInfo.MeasuredTipRadiusUU = Last + 0.5f * Step;
	}
	else if (OutInfo.SegmentCount == 1)
	{
		OutInfo.MeasuredMuzzleRadiusUU = OutInfo.MeasuredSegmentRadiiUU[0];
		OutInfo.MeasuredTipRadiusUU = OutInfo.MeasuredSegmentRadiiUU[0];
	}

	OutInfo.bHaloVisible = bHaloVisible && (BeamHalo != nullptr) && BeamHalo->IsVisible();
	if (BeamHalo != nullptr)
	{
		OutInfo.MeasuredHaloRadiusUU =
			UTraceFxShapes::RadiusUUFromShapeScale(static_cast<float>(BeamHalo->GetRelativeScale3D().X));

		// The one property of "double-sided" that can be asked of a material at runtime. Printed
		// rather than asserted: it is a property of an engine asset, and the FX doc's reason for
		// asking for it (you must be able to see through the sleeve from any angle) is already met
		// by an additive blend that writes no depth.
		const UMaterialInterface* HaloMaterial = BeamHalo->GetMaterial(0);
		OutInfo.bHaloTwoSided = (HaloMaterial != nullptr) && HaloMaterial->IsTwoSided();
	}

	// The instantaneous opacity, recomputed from the SAME expression UpdateEffect uses — which is
	// legitimate here because opacity is not a transform and cannot be read back off a MID without
	// the material exposing it. Flagged so nobody mistakes it for a measurement.
	const float DecayAlpha = FMath::Clamp(
		(OutInfo.AgeSeconds - BeamHoldSeconds) / FMath::Max(BeamDecaySeconds, KINDA_SMALL_NUMBER), 0.f, 1.f);
	OutInfo.HaloOpacityNow = OutInfo.bHaloVisible ? HaloOpacityValue * FMath::Pow(1.f - DecayAlpha, 2.2f) : 0.f;

	OutInfo.bFlashVisible = bMuzzleVisible && (MuzzleFlash != nullptr) && MuzzleFlash->IsVisible();
	if (MuzzleFlash != nullptr)
	{
		OutInfo.MeasuredConeRadiusUU =
			UTraceFxShapes::RadiusUUFromShapeScale(static_cast<float>(MuzzleFlash->GetRelativeScale3D().X));
		OutInfo.MeasuredConeHeightUU =
			UTraceFxShapes::LengthUUFromShapeScale(static_cast<float>(MuzzleFlash->GetRelativeScale3D().Z));

		// The flash SCALE, measured: the live cone radius divided by its authored radius. This is
		// the "flash scale over time" a verifier is told to watch, and it comes out of the transform
		// rather than out of the lerp that produced it.
		//
		// *** IT IS A REPORTING FIELD, NOT A DENOMINATOR. *** It is measurement / constant, so
		// dividing the measurement BACK by it recovers the constant exactly, whatever the transform
		// actually is — an algebraic identity that prints as a passing measurement. The verdict did
		// that for two of its six checks and they could not fail on geometry; PrintVerdict now grades
		// MeasuredConeRadiusUU against the authored curve evaluated at GeometryAgeSeconds instead.
		// Anything downstream that wants "how big is the cone" must use MeasuredConeRadiusUU.
		OutInfo.FlashScaleNow = (FlashConeRadiusUU > KINDA_SMALL_NUMBER)
			? (OutInfo.MeasuredConeRadiusUU / FlashConeRadiusUU) : 0.f;
	}

	return true;
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

// -------------------------------------------------------------------------------------------------
// Trace.Fx.Beam — SPEC v32 §2's evidence, measured off the beams that are actually on screen
//
// "PROVE IT AT RUNTIME. Compiling is not evidence." The four numbers the pass is judged on are the
// beam's resolved length, the taper radii in uu, the halo opacity and the flash scale over time, and
// three of the four are only meaningful as a TIME SERIES — a flash that scales 0.55 -> 3.2x over
// 0.28 s cannot be photographed by a single print. So this samples every frame for a few seconds,
// follows the newest tracer, and prints one row per frame out of ATraceTracer::DescribeShot(), which
// reads its numbers back off the live component transforms.
//
// *** "THE NEWEST TRACER" MEANS THE NEWEST ONE OF MINE. *** The arena is full of bots and every one
// of them spawns a tracer per shot, so an unfiltered "newest in the world" grades whoever fired last
// — and a remote beam is never told which gun fired it, so it draws the PISTOL profile whatever its
// shooter is holding. That is what made Trace.Fx.BeamSmg fail 4 of 6 while the SMG's own beam was
// measuring 2.400 uu perfectly on the rows either side of the graded one, and it is also why the
// pistol arm's green was weaker than it looked: it was passing on a bot's beam. Sample() now asks
// for the newest FIRST-PERSON tracer and counts the frames it skipped.
//
// *** AND IT CAN FAIL, WHICH IS THE POINT. *** SPEC v32 §8: "A harness whose red and green arms
// agree is not measuring its rule. Three harnesses in this project have printed a pass over their
// own failure." The optional second argument names the profile the caller EXPECTS — pistol or smg —
// and the verdict compares the measured geometry against THAT profile's authored radii. Fire the
// pistol and ask for `smg` and it must FAIL on the muzzle radius (3.0 measured against 2.4
// expected), because those are genuinely different beams. That is the red arm, it is one word on the
// command line, and it is run in the pass report.
// -------------------------------------------------------------------------------------------------
namespace TraceTracerFxProbe
{
	FTSTicker::FDelegateHandle GTicker;
	float GElapsed = 0.f;
	float GWatchUntil = 0.f;
	bool GExpectSmg = false;
	bool GHasExpectation = false;

	/**
	 * The slot key this run presses before it fires, or empty for "measure whatever is already
	 * being shot". See ArmScripted for why the probe drives its own input at all.
	 */
	FString GSlotKey;
	bool GPressedSlot = false;
	bool GPulledTrigger = false;

	/** When the trigger goes down, and for how long. See the timeline in ArmScripted. */
	constexpr float TriggerAtSeconds = 0.80f;
	constexpr float TriggerHoldSeconds = 2.00f;

	/** The sample kept for the verdict: the newest tracer caught inside its hold window. */
	bool GHaveBest = false;
	FTraceTracerShotDebug GBest;
	uint32 GBestId = 0;
	uint32 GLastSeenId = 0;
	int32 GSampleCount = 0;
	int32 GTracersSeen = 0;

	/**
	 * How many frames the newest beam in the world belonged to SOMEBODY ELSE and was skipped.
	 *
	 * Reported, not merely dropped. This is the count that was silently being graded before: a match
	 * is full of bots, each of them spawns a tracer per shot, and a remote beam is not told which gun
	 * fired it so it draws the PISTOL profile whatever the shooter holds. Trace.Fx.BeamSmg was
	 * failing 4 of 6 on a bot's pistol shot, and Trace.Fx.BeamPistol was PASSING on one — a green
	 * that says nothing about the gun in your own hands. Printing the number keeps that visible
	 * instead of turning it back into an assumption.
	 */
	int32 GRemoteSamplesSkipped = 0;

	/** Tolerance on a radius, in uu. Two orders of magnitude below the 100x error that matters. */
	constexpr float RadiusToleranceUU = 0.05f;
	constexpr float OpacityTolerance = 0.01f;

	/**
	 * Tolerance on a dimensionless radius RATIO. Comfortably inside the gap the ratios have to
	 * resolve: pistol tip/muzzle is 0.4333 and the SMG's is 0.5417, so 0.01 leaves an order of
	 * magnitude of margin and still refuses a taper that is off by a percent.
	 */
	constexpr float RatioTolerance = 0.01f;

	void PrintSample(const FTraceTracerShotDebug& Sample)
	{
		// BOTH CLOCKS, EVERY FRAME. `age` is when the probe looked; `laid` is the age the effect
		// wrote these transforms at. The cone's authored size is a function of the second one, and
		// the verdict grades the live cone against the doc's curve evaluated there — so the two
		// being visibly equal on every row is what says the grading is anchored to the right frame.
		// They are also the only way to notice, from a log alone, if a future change to when the
		// probe samples ever puts them out of step.
		UE_LOG(LogTraceGame, Display,
			TEXT("FXBEAM: age=%.3fs laid=%.3fs profile=%s%s gun=%s len=%.1fuu core=%.3f->%.3fuu seg=[%.3f,%.3f,%.3f] "
			     "halo=%.3fuu op=%.3f blend=%s twoSided=%d flash=%s scale=%.3f cone=r%.2f/h%.2fuu"),
			Sample.AgeSeconds, Sample.GeometryAgeSeconds,
			Sample.bSmgProfile ? TEXT("SMG") : TEXT("PISTOL"),
			Sample.bFirstPersonShot ? TEXT("(1P)") : TEXT("(remote)"),
			Sample.GunMesh.IsNone() ? TEXT("-") : *Sample.GunMesh.ToString(),
			Sample.LengthUU,
			Sample.MeasuredMuzzleRadiusUU, Sample.MeasuredTipRadiusUU,
			Sample.MeasuredSegmentRadiiUU[0], Sample.MeasuredSegmentRadiiUU[1], Sample.MeasuredSegmentRadiiUU[2],
			Sample.MeasuredHaloRadiusUU, Sample.HaloOpacityNow,
			UTraceFxShapes::BlendName(Sample.HaloBlend), Sample.bHaloTwoSided ? 1 : 0,
			Sample.bFlashVisible ? TEXT("on") : TEXT("off"), Sample.FlashScaleNow,
			Sample.MeasuredConeRadiusUU, Sample.MeasuredConeHeightUU);
	}

	/**
	 * The verdict. Five of the six rows are a live component transform against an authored number;
	 * the sixth (halo opacity) is a recomputation, because a MID will not hand a scalar back, and it
	 * says so in its own row. NOTHING here divides a measurement by a quantity derived from that same
	 * measurement — that is an identity, it prints as a pass over any transform at all, and the two
	 * cone rows did exactly that until a verifier caught them.
	 *
	 * Note which side of each comparison the EXPECTATION comes from: the muzzle and cone radii come
	 * from the profile the CALLER named, not from the profile the beam used, so naming the wrong one
	 * fails. The tip radius, halo radius and halo opacity are shared by both guns and are compared
	 * against the FX doc's own numbers.
	 */
	void PrintVerdict()
	{
		if (!GHaveBest)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("FXBEAM: FAIL — no FIRST-PERSON tracer was drawn during the window (%d of mine "
				     "seen, %d samples, %d frames where the newest beam in the world was somebody "
				     "else's and was skipped). Nothing was measured, so nothing passed."),
				GTracersSeen, GSampleCount, GRemoteSamplesSkipped);
			return;
		}

		const float ExpectedMuzzle = GExpectSmg ? 2.4f : 3.0f;
		const float ExpectedTip = 1.3f;
		const float ExpectedHalo = 5.2f;
		const float ExpectedOpacity = 0.55f;
		const float ExpectedCone = GExpectSmg ? 13.0f : 16.0f;
		const float ExpectedConeHeight = 30.0f;

		// =========================================================================================
		// THE LEGIBILITY FLOOR — WHY THE ABSOLUTE RADII ARE NOT THE WHOLE CHECK (v32 integration)
		//
		// This verdict was written asserting the FX doc's absolute radii and nothing else, and it
		// FAILED ITS OWN GREEN ARM on the first run that ever executed it: a pistol shot 5689 uu
		// down the arena measured 6.500 uu at the muzzle against the doc's 3.000, with the tip and
		// the halo out by the same 2.167x factor.
		//
		// That is not the effect being wrong. InitTracer's own comment says it in full: the doc's
		// 3.0 uu is authored for a preview turntable a metre from the camera, and at 8000 uu it is a
		// quarter of a pixel — so the pre-v32 proportional radius survives as a FLOOR, and where it
		// wins THE WHOLE PROFILE is multiplied by one factor so the taper, the halo and the cone keep
		// their authored proportions. At the shipped knobs the doc's absolute numbers are exact under
		// ~1935 uu (every corridor fight, every viewmodel shot) and the floor takes over above it.
		//
		// So the check has to grade the rule that actually ships, in two halves:
		//
		//   ABSOLUTE, against whichever of the two rules wins at this beam's MEASURED length. This
		//   is the half that catches the 100x metres/centimetres error — the thing that matters —
		//   and it needs the shipped knobs, which are INPUTS to the effect and not its output. Note
		//   honestly that when the floor binds this half stops telling a pistol from an SMG, because
		//   the floor is the same number for both.
		//
		//   PROPORTIONS, always, and they are what the doc actually authored: tip/muzzle is 1.3/3.0
		//   for the pistol and 1.3/2.4 for the SMG, halo/muzzle is 5.2/3.0 against 5.2/2.4. Both are
		//   read off two independent live components, both are immune to the floor (which scales
		//   numerator and denominator together), and both DIFFER BETWEEN THE PROFILES — which is what
		//   keeps Trace.Fx.BeamRedArm red at any range. The cone, which the floor never touches, is
		//   the third profile-distinguishing check.
		// =========================================================================================
		const UTraceSettings& VerdictSettings = UTraceSettings::Get();
		const float FloorMinUU = FMath::Max(0.f, VerdictSettings.TracerRadiusMinUU);
		const float FloorMaxUU = FMath::Max(FloorMinUU, VerdictSettings.TracerRadiusMaxUU);
		const float LegibilityRadiusUU = FMath::Clamp(
			GBest.LengthUU * FMath::Max(0.f, VerdictSettings.TracerRadiusPerLength), FloorMinUU, FloorMaxUU);
		const bool bFloorBinding = (LegibilityRadiusUU > ExpectedMuzzle);

		// =========================================================================================
		// AND THEN THE SIZE KNOB — ON THE ABSOLUTE ROWS ONLY, WHICH IS THE HONEST SPLIT
		//
		// Trace.Fx.BeamScale multiplies every dimension of the drawn profile (see the block at the
		// end of InitTracer's width section). It is an INPUT to the effect, in exactly the sense
		// TracerSheathRadiusRatio and TracerMuzzleRadiusUU already are, so the expected side has to
		// follow it or a tuning pass turns this harness red for no reason.
		//
		// *** BUT IT MUST NOT BE DIVIDED OUT OF THE MEASUREMENT. *** The failure mode this file has
		// already been caught in once is a check that cancels the live transform algebraically and
		// prints "measured" over an identity. So the knob appears on ONE side only, and only on the
		// three rows whose units are uu: core muzzle radius, cone radius, cone height. The two RATIO
		// rows never see it — they cannot, because a uniform scale cancels out of a ratio, which is
		// the property that makes them the rows that tell a pistol from an SMG at any size and keeps
		// Trace.Fx.BeamRedArm red whatever the knob is set to.
		// =========================================================================================
		const float BeamScale = ATraceTracer::GetBeamProfileScale();
		const float ExpectedMuzzleOnScreen = FMath::Max(ExpectedMuzzle, LegibilityRadiusUU) * BeamScale;

		// The halo knob, normalised exactly as InitTracer normalises it. 3.2 is the shipped
		// TracerSheathRadiusRatio default this profile was calibrated against (see
		// ATraceTracer::HaloRatioReferenceDefault, which is private to the class); restating it here
		// is deliberate — if the shipped default ever moves without the profile moving with it, this
		// line goes red, which is the correct answer rather than a silent re-fit.
		const float HaloKnobScale = FMath::Max(1.f, VerdictSettings.TracerSheathRadiusRatio) / 3.2f;

		const float MeasuredTaperRatio = (GBest.MeasuredMuzzleRadiusUU > KINDA_SMALL_NUMBER)
			? (GBest.MeasuredTipRadiusUU / GBest.MeasuredMuzzleRadiusUU) : 0.f;
		const float MeasuredHaloRatio = (GBest.MeasuredMuzzleRadiusUU > KINDA_SMALL_NUMBER)
			? (GBest.MeasuredHaloRadiusUU / GBest.MeasuredMuzzleRadiusUU) : 0.f;
		const float ExpectedTaperRatio = ExpectedTip / ExpectedMuzzle;
		const float ExpectedHaloRatio = (ExpectedHalo / ExpectedMuzzle) * HaloKnobScale;

		UE_LOG(LogTraceGame, Display,
			TEXT("FXBEAM: this beam is %.1f uu long, so the on-screen muzzle radius is set by %s — "
			     "doc %.3f uu vs legibility floor %.3f uu (len x %.5f/uu, clamped %.2f..%.2f), all "
			     "of it then x %.3f (Trace.Fx.BeamScale). The ABSOLUTE check below grades against "
			     "%.3f uu; the two RATIO checks grade the doc's authored proportions, are immune to "
			     "both the floor and the knob, and are what tells a pistol from an SMG at any range."),
			GBest.LengthUU, bFloorBinding ? TEXT("THE LEGIBILITY FLOOR") : TEXT("the FX doc"),
			ExpectedMuzzle, LegibilityRadiusUU, VerdictSettings.TracerRadiusPerLength,
			FloorMinUU, FloorMaxUU, BeamScale, ExpectedMuzzleOnScreen);

		// =========================================================================================
		// THE MUZZLE CONE — GRADED AGAINST THE AUTHORED CURVE, NOT AGAINST ITSELF
		//
		// This is the half of the verdict a verifier caught printing an identity. It used to read:
		//
		//     ConeAuthored = MeasuredConeRadiusUU / FlashScaleNow
		//
		// and FlashScaleNow is itself MeasuredConeRadiusUU / FlashConeRadiusUU (DescribeShot), so
		// ConeAuthored was identically FlashConeRadiusUU — the member variable — graded against
		// 16.0. Ditto the height. THE LIVE CONE'S TRANSFORM CANCELLED OUT OF BOTH ROWS, and they
		// still printed "measured 16.000". Injecting a 100x metres/centimetres error into
		// PlaceConeAlongLocalZ put an r 0.09 / h 0.16 uu cone on screen and the verdict printed
		// PASS 6/6 over it — measured and reproduced before this was rewritten.
		//
		// The honest check needs both halves of the doc's contract, "r 16 uu scaling 0.55 -> 3.2x
		// over 0.28 s", on the EXPECTED side, and nothing but the live transform on the measured
		// side. So: evaluate the doc's own lerp at the age the geometry was laid at, multiply by
		// the authored radius for the profile the CALLER named, and compare that in uu against what
		// the component's scale actually is. Now a cone 100x too small measures 0.088 against an
		// expected 8.800 and the row goes red, which is the entire point of the row.
		//
		// The age comes from GeometryAgeSeconds — the age UpdateEffect was handed, recorded by the
		// effect — and not from the probe's own sample clock. The cone moves 2.5 uu of radius per
		// 60 Hz frame, which is fifty times this tolerance, so a one-frame skew between "when it was
		// drawn" and "when I looked" would be indistinguishable from a real defect. The two are
		// printed side by side below so that a skew announces itself rather than grading anything.
		//
		// The knob is normalised exactly as it is for the halo, and for the same reason: the shipped
		// TracerMuzzleRadiusUU is a SCALE on the authored cone (see the FlashRadiusReferenceDefaultUU
		// note in the header, "cone radius = 16 uu * (TracerMuzzleRadiusUU / 2.5)"), so the expected
		// size has to follow the slider or a player who has touched it turns the harness red for no
		// reason. 2.5 is restated here deliberately, like the halo's 3.2: if the shipped default ever
		// moves without the profile moving with it, this line goes red, which is the correct answer.
		// =========================================================================================
		const float FlashKnobScale =
			FMath::Max(KINDA_SMALL_NUMBER, VerdictSettings.TracerMuzzleRadiusUU) / 2.5f;
		const float AuthoredFlashScale = FMath::Lerp(
			ATraceTracer::MuzzleFlashStartScale, ATraceTracer::MuzzleFlashEndScale,
			FMath::Clamp(GBest.GeometryAgeSeconds / ATraceTracer::MuzzleFlashSeconds, 0.f, 1.f));
		const float ExpectedConeRadiusNow = ExpectedCone * FlashKnobScale * AuthoredFlashScale * BeamScale;
		const float ExpectedConeHeightNow = ExpectedConeHeight * FlashKnobScale * AuthoredFlashScale * BeamScale;

		UE_LOG(LogTraceGame, Display,
			TEXT("FXBEAM: the cone was laid at age %.4fs of its %.2fs window (probe sampled at "
			     "%.4fs — a skew here and only here would falsify the two cone rows), so the FX "
			     "doc's %.2f->%.2fx curve is at %.3fx and the authored r %.1f / h %.1f uu, x %.3f "
			     "(Trace.Fx.BeamScale), should be on screen at r %.3f / h %.3f uu. The rows below "
			     "grade the LIVE component scale against those, not the constants divided out of "
			     "themselves."),
			GBest.GeometryAgeSeconds, ATraceTracer::MuzzleFlashSeconds, GBest.AgeSeconds,
			ATraceTracer::MuzzleFlashStartScale, ATraceTracer::MuzzleFlashEndScale, AuthoredFlashScale,
			ExpectedCone * FlashKnobScale, ExpectedConeHeight * FlashKnobScale, BeamScale,
			ExpectedConeRadiusNow, ExpectedConeHeightNow);

		// A suppressed or degraded flash measures zero and fails both cone rows, which is right but
		// looks like a geometry bug. Say which it is before the table rather than after it.
		if (!GBest.bFlashVisible)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("FXBEAM: no cone was on screen on the graded frame (bTracerMuzzleFlash off, or "
				     "no ADDITIVE material resolved and the flash was dropped rather than drawn as "
				     "an occluding opaque cone) — the two cone rows "
				     "below will measure 0.000 for that reason and not because the size is wrong."));
		}

		// WHERE EACH NUMBER CAME FROM, IN THE TABLE ITSELF. Five of these six are read back off a
		// live component transform; the halo's opacity cannot be, because a MID does not hand back
		// the scalar you wrote into it, so it is recomputed from the same fade expression UpdateEffect
		// uses (DescribeShot says so at the point it does it). Printing every row under the word
		// "measured" was how two recomputations passed for readbacks in the first place, so the row
		// now carries its own provenance and a reader can weigh it accordingly.
		struct FCheck
		{
			const TCHAR* Name;
			const TCHAR* Source;
			float Measured;
			float Expected;
			float Tolerance;
		};

		const TCHAR* const Readback = TEXT("measured");
		const TCHAR* const Derived = TEXT("recomputed");

		const FCheck Checks[] =
		{
			{ TEXT("core muzzle radius uu"), Readback, GBest.MeasuredMuzzleRadiusUU, ExpectedMuzzleOnScreen, RadiusToleranceUU },
			{ TEXT("taper ratio tip/muzzle"), Readback, MeasuredTaperRatio,          ExpectedTaperRatio,     RatioTolerance    },
			{ TEXT("halo ratio halo/muzzle"), Readback, MeasuredHaloRatio,           ExpectedHaloRatio,      RatioTolerance    },
			{ TEXT("halo opacity"),          Derived,  GBest.HaloOpacityNow,         ExpectedOpacity,        OpacityTolerance  },
			{ TEXT("flash cone radius uu"),  Readback, GBest.MeasuredConeRadiusUU,   ExpectedConeRadiusNow,  RadiusToleranceUU },
			{ TEXT("flash cone height uu"),  Readback, GBest.MeasuredConeHeightUU,   ExpectedConeHeightNow,  RadiusToleranceUU },
		};

		int32 Failures = 0;
		for (const FCheck& Check : Checks)
		{
			const bool bPass = FMath::IsNearlyEqual(Check.Measured, Check.Expected, Check.Tolerance);
			if (!bPass)
			{
				++Failures;
			}
			UE_LOG(LogTraceGame, Display, TEXT("FXBEAM:   %-24s %-10s %8.3f  expected %8.3f  %s"),
				Check.Name, Check.Source, Check.Measured, Check.Expected, bPass ? TEXT("ok") : TEXT("*** FAIL ***"));
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("FXBEAM: %s — expected %s profile, beam was %s from %s, sampled at age %.3fs "
			     "(%d samples over %d of MY tracer(s), %d frames of somebody else's skipped); "
			     "length %.1f uu, hold %.2fs decay %.2fs life %.2fs"),
			(Failures == 0) ? TEXT("PASS") : TEXT("FAIL"),
			GExpectSmg ? TEXT("SMG") : TEXT("PISTOL"),
			GBest.bSmgProfile ? TEXT("SMG") : TEXT("PISTOL"),
			GBest.GunMesh.IsNone() ? TEXT("no resolved gun mesh") : *GBest.GunMesh.ToString(),
			GBest.AgeSeconds, GSampleCount, GTracersSeen, GRemoteSamplesSkipped, GBest.LengthUU,
			ATraceTracer::BeamHoldSeconds, ATraceTracer::BeamDecaySeconds, ATraceTracer::TracerLifeSeconds);

		if (Failures != 0)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("FXBEAM: FAIL — %d of %d checks did not match."),
				Failures, static_cast<int32>(UE_ARRAY_COUNT(Checks)));
		}
	}

	void Sample()
	{
		if (GEngine == nullptr)
		{
			return;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* SampleWorld = Context.World();
			if (SampleWorld == nullptr || SampleWorld->GetNetMode() == NM_DedicatedServer)
			{
				continue;
			}

			// *** MY OWN BEAM, NOT THE NEWEST BEAM IN THE ARENA. ***
			//
			// The scripted arms press a weapon key and pull MY trigger, so the only beam that can
			// answer "did the gun I just drew produce the right profile?" is one placed on MY
			// viewmodel. This used to be an unfiltered GetNewestTracer(), and in a match full of
			// bots it graded whichever shot was most recent — which was frequently a bot's, drawn
			// with the remote fallback profile because a remote beam is never told which gun fired
			// it. That is the whole of the "SMG shots draw the pistol's beam" defect: the SMG's own
			// beam was measuring 2.400 uu correctly on the frames the probe looked at it, and the
			// verdict was graded on a bot's 3.000.
			ATraceTracer* Newest = ATraceTracer::GetNewestTracer(SampleWorld, /*bFirstPersonOnly=*/true);
			if (Newest == nullptr)
			{
				// Nothing of mine is on screen this frame. Note whether somebody else's was, so the
				// verdict can tell "I never fired" apart from "I was drowned out by the bots".
				if (ATraceTracer::GetNewestTracer(SampleWorld) != nullptr)
				{
					++GRemoteSamplesSkipped;
				}
				continue;
			}
			if (ATraceTracer::GetNewestTracer(SampleWorld) != Newest)
			{
				++GRemoteSamplesSkipped;
			}

			FTraceTracerShotDebug Shot;
			if (!Newest->DescribeShot(Shot))
			{
				continue;
			}

			const uint32 TracerId = Newest->GetUniqueID();
			++GSampleCount;
			if (TracerId != GLastSeenId)
			{
				GLastSeenId = TracerId;
				++GTracersSeen;
			}

			PrintSample(Shot);

			// KEEP THE NEWEST TRACER CAUGHT INSIDE ITS HOLD, and only inside the hold: that is the
			// window the FX doc's radii describe, and after it the beam is deliberately thinning so
			// 3.0 uu is no longer the right answer. Newest rather than smallest-age, because a run
			// that switches weapons mid-window would otherwise be graded on whichever frame happened
			// to be sampled earliest — which could be a shot from the PREVIOUS gun. If nothing lands
			// inside a hold, nothing is kept and the verdict says so rather than grading the wrong
			// frame, which is the failure mode §8 calls out by name.
			if (Shot.AgeSeconds <= ATraceTracer::BeamHoldSeconds
				&& (!GHaveBest || TracerId != GBestId || Shot.AgeSeconds < GBest.AgeSeconds))
			{
				GHaveBest = true;
				GBest = Shot;
				GBestId = TracerId;
			}
		}
	}

	/** The local player controller, for issuing the scripted input commands through. */
	APlayerController* FindLocalController()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* ExecWorld = Context.World();
			if (ExecWorld == nullptr || ExecWorld->GetNetMode() == NM_DedicatedServer)
			{
				continue;
			}
			for (FConstPlayerControllerIterator It = ExecWorld->GetPlayerControllerIterator(); It; ++It)
			{
				APlayerController* PC = It->Get();
				if (PC != nullptr && PC->IsLocalController())
				{
					return PC;
				}
			}
		}
		return nullptr;
	}

	void RunCommand(const FString& Command)
	{
		APlayerController* PC = FindLocalController();
		UE_LOG(LogTraceGame, Display, TEXT("FXBEAM: > %s"), *Command);
		if (PC != nullptr)
		{
			PC->ConsoleCommand(Command, /*bWriteToLog=*/false);
		}
		else if (GEngine != nullptr)
		{
			GEngine->Exec(nullptr, *Command);
		}
	}

	void Arm(float DurationSeconds, bool bExpectSmg, bool bHasExpectation, const FString& SlotKey)
	{
		GElapsed = 0.f;
		GWatchUntil = FMath::Clamp(DurationSeconds, 0.2f, 60.f);
		GExpectSmg = bExpectSmg;
		GHasExpectation = bHasExpectation;
		GSlotKey = SlotKey;
		GPressedSlot = SlotKey.IsEmpty();
		GPulledTrigger = SlotKey.IsEmpty();
		GHaveBest = false;
		GBest = FTraceTracerShotDebug();
		GBestId = 0;
		GLastSeenId = 0;
		GSampleCount = 0;
		GTracersSeen = 0;
		GRemoteSamplesSkipped = 0;

		// The units and the primitives, printed before the first sample: everything below is
		// meaningless if a BasicShape is not 100 uu across, and that is an assumption about somebody
		// else's assets.
		UTraceFxShapes::LogPrimitiveGeometryOnce();

		UE_LOG(LogTraceGame, Display,
			TEXT("FXBEAM: watching for %.1fs, expecting %s, and grading MY OWN first-person beam only "
			     "(anybody else's is skipped: a remote beam is not told which gun fired it). FX doc "
			     "profile: pistol r 3.0->1.3 uu, SMG r 2.4->1.3 uu, halo r 5.2 uu @ 0.55, cone "
			     "r 16/13 x h 30 uu scaling 0.55->3.2x over %.2fs, every dimension x %.3f "
			     "(Trace.Fx.BeamScale); hold %.2fs then fade over %.2fs."),
			GWatchUntil, bHasExpectation ? (bExpectSmg ? TEXT("the SMG profile") : TEXT("the PISTOL profile"))
			                             : TEXT("nothing (no verdict)"),
			ATraceTracer::MuzzleFlashSeconds, ATraceTracer::GetBeamProfileScale(),
			ATraceTracer::BeamHoldSeconds, ATraceTracer::BeamDecaySeconds);

		if (GTicker.IsValid())
		{
			return; // already running; the reset above just restarted the window
		}

		GTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[](float Delta) -> bool
			{
				GElapsed += Delta;

				// The scripted arms drive their own input. Both commands take ARGUMENTS, and a
				// -TraceExec= value containing a space does not survive the macOS launcher re-exec
				// (see the three argument-free aliases in TraceIntegrationWalk.cpp for the measured
				// account of that) — so they are issued from in here, where arguments are free,
				// rather than from the command line.
				if (!GPressedSlot && GElapsed >= 0.f)
				{
					GPressedSlot = true;
					RunCommand(FString::Printf(TEXT("Trace.SimInput %s 0.10"), *GSlotKey));
				}
				if (!GPulledTrigger && GElapsed >= TriggerAtSeconds)
				{
					GPulledTrigger = true;
					RunCommand(FString::Printf(TEXT("Trace.SimInput LeftMouseButton %.2f"), TriggerHoldSeconds));
				}

				// Only sample once something can be firing: before the trigger there is nothing to
				// measure and the rows would be noise around whatever the last shot left behind.
				if (GPulledTrigger)
				{
					Sample();
				}

				if (GElapsed < GWatchUntil)
				{
					return true;
				}

				if (GHasExpectation)
				{
					PrintVerdict();
				}
				else
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("FXBEAM: done — %d samples over %d of my tracer(s) (%d frames of "
						     "somebody else's skipped), no verdict asked for."),
						GSampleCount, GTracersSeen, GRemoteSamplesSkipped);
				}
				GTicker.Reset();
				return false;
			}), 0.f);
	}

	/**
	 * One scripted arm: pull out @p SlotKey's weapon, wait out the pullout, hold the trigger, and
	 * grade the beams that come out against @p bExpectSmg's authored profile.
	 *
	 * The timeline mirrors the one Trace.Integ.Walk already uses and for the same measured reasons:
	 * 0.80 s between the slot key and the trigger covers the selector's replication plus the 0.200 s
	 * pullout, so the rounds that come out are definitely from the gun that was asked for.
	 */
	void ArmScripted(bool bExpectSmg, const TCHAR* SlotKey)
	{
		Arm(TriggerAtSeconds + TriggerHoldSeconds + 0.60f, bExpectSmg, /*bHasExpectation=*/true, SlotKey);
	}
} // namespace TraceTracerFxProbe

static FAutoConsoleCommand GTraceFxBeamCmd(
	TEXT("Trace.Fx.Beam"),
	TEXT("Trace.Fx.Beam [seconds] [pistol|smg] — measure the live beam's length, taper radii, halo opacity "
	     "and flash scale every frame while YOU shoot. Naming a profile turns it into a pass/fail check "
	     "against that profile's authored radii; name the wrong one and it must fail."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const float Seconds = (Args.Num() > 0) ? FCString::Atof(*Args[0]) : 2.0f;

		bool bExpectSmg = false;
		bool bHasExpectation = false;
		if (Args.Num() > 1)
		{
			bHasExpectation = true;
			bExpectSmg = Args[1].StartsWith(TEXT("s"), ESearchCase::IgnoreCase);
		}

		TraceTracerFxProbe::Arm(Seconds, bExpectSmg, bHasExpectation, FString());
	}));

// -------------------------------------------------------------------------------------------------
// THREE ARGUMENT-FREE ARMS, AND THE THIRD ONE IS THE RED ARM
//
// Argument-free for the reason TraceIntegrationWalk.cpp documents at length: a -TraceExec= value
// containing a space does not survive the macOS launcher's re-exec, so a headless run can only
// invoke commands that take none. Each of these drives its own weapon swap and its own trigger.
//
//   Trace.Fx.BeamPistol   pulls out slot 1, fires, grades against the PISTOL profile -> must PASS
//   Trace.Fx.BeamSmg      pulls out slot 2, fires, grades against the SMG profile    -> must PASS
//   Trace.Fx.BeamRedArm   pulls out slot 1, fires, grades against the SMG profile    -> must FAIL
//
// The third one is not a joke command. SPEC v32 §8: "A harness whose red and green arms agree is not
// measuring its rule." BeamRedArm fires the same weapon BeamPistol does and asks for a different
// answer, so if it ever prints PASS then the check is not reading the geometry — and since the two
// profiles differ only in the muzzle radius (3.0 vs 2.4 uu) and the cone radius (16 vs 13 uu), it is
// specifically the taper and the cone that it proves are being measured rather than assumed.
// -------------------------------------------------------------------------------------------------
static FAutoConsoleCommand GTraceFxBeamPistolCmd(
	TEXT("Trace.Fx.BeamPistol"),
	TEXT("Scripted: draw the pistol, fire, and grade the beam against the pistol's authored profile."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceTracerFxProbe::ArmScripted(/*bExpectSmg=*/false, TEXT("One")); }));

static FAutoConsoleCommand GTraceFxBeamSmgCmd(
	TEXT("Trace.Fx.BeamSmg"),
	TEXT("Scripted: draw the SMG, fire, and grade the beam against the SMG's authored profile."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceTracerFxProbe::ArmScripted(/*bExpectSmg=*/true, TEXT("Two")); }));

static FAutoConsoleCommand GTraceFxBeamRedArmCmd(
	TEXT("Trace.Fx.BeamRedArm"),
	TEXT("RED ARM: draw the PISTOL, fire, and grade it against the SMG profile. Must FAIL — if it "
	     "passes, Trace.Fx.BeamPistol is not measuring anything."),
	FConsoleCommandDelegate::CreateStatic([]() { TraceTracerFxProbe::ArmScripted(/*bExpectSmg=*/true, TEXT("One")); }));
#endif // !UE_BUILD_SHIPPING
