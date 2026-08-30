// Trace — THE FIRST-PERSON VIEWMODEL: the guns, the gloved hands, the knife rig that hangs off
// them, and every clip and emissive band that animates the lot. See TraceCharacter.h for the pawn
// and the framing/depth arithmetic this file implements.
//
// *** THE SHOT PATH NEVER READS THE RIG. *** That invariant is why this file could be split off at
// all, and it is the first thing to check before adding anything to it. GetPawnViewLocation() is
// the single origin for the camera, the muzzle and the ray (TraceCharacter.h's header essay);
// nothing in hitscan, lag compensation, the melee arc or the trail asks this file anything. The one
// function here that LOOKS like an exception, GetViewModelMuzzleViewPoint(), is a COSMETIC query —
// it exists so a tracer can be drawn from the barrel a player can see, and it is deliberately not
// where the bullet comes from. Keep it that way: a shot that started reading the rig would make the
// crosshair lie by exactly the amount the gun is bobbing.
//
// WHAT MOVED, AND WHAT DID NOT (RESTRUCTURE tranche D4). Everything below came out of
// TraceCharacter.cpp verbatim — banners, measurements and essays included. The CAMERA did not:
// UpdateViewBlend() and GetThirdPersonPivotZ() stayed with the pawn, because they decide where the
// player is looking from and this file only decides what is drawn in front of that. This file is
// called from three places and no others: EnsureViewModelBuilt()/UpdateViewModel() on the pawn's
// tick, SetViewModelVisible() from the view blend, and NotifyWeaponFired() from the weapon
// component's cosmetic half.
//
// IT IS STILL ATraceCharacter, and that is deliberate rather than lazy. The plan sketched a
// UTraceViewModelComponent; a component would have to re-register the two tick prerequisites below
// against its own tick, and getting that wrong is invisible until a gun rides a one-frame-stale
// wrist — the exact bug TraceCharacter.h's viewmodel note warns about. A translation-unit split
// buys the same readability with no tick-registration surface at all, so the prerequisites moved
// here UNCHANGED and still name the pawn and the weapon component.
//
// LOCAL PLAYER ONLY, NEVER REPLICATED. Every component built here is owner-only and first-person
// (EFirstPersonPrimitiveType), so a remote client has none of it; nothing here is a replicated
// property, an RPC, or read by GetLifetimeReplicatedProps.

#include "Core/TraceCharacter.h"
#include "Core/TraceCharacterInternal.h"      // the measured layout tables and the asset/clip tables

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"            // the pack's twenty hand clips (spec v31 §6)
#include "Animation/AnimSingleNodeInstance.h"  // ... played without an anim blueprint; see the header
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"                // FMinimalViewInfo (GetViewModelMuzzleViewPoint)
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"      // EFirstPersonPrimitiveType (the viewmodel)
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"               // Trace.Hands.GloveFloor, a shipped cosmetic tunable
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"     // the ability the hands' idle pose reacts to
#include "Core/TraceCharacterRoster.h"          // the per-character cuff accent colour
#include "Movement/TraceCharacterMovementComponent.h"   // sliding: the hands lean with the slide
#include "Gameplay/TraceCore.h"                 // IsCarrier(): a carrier's hands hold the Core
#include "Gameplay/TraceKnifeView.h"            // spec v31 §5/§6: IsInspecting(), the F-key flourish
#include "Gameplay/TraceMelee.h"                // spec v28 §10: TraceMelee::IsDualWieldEnabled()
#include "Gameplay/TraceRailgunFireCurve.h"     // the measured fire curve the rig plays
#include "Gameplay/TraceWeaponComponent.h"      // the equipped weapon, the ammo, the fire latch
#include "Modes/TracePracticeRange.h"           // [DEMO 29 §2] the ONE gate the arms fixture is behind
#include "Settings/TraceUserSettings.h"
#include "Trace.h"
#include "TraceSettings.h"

// =================================================================================================
// First-person viewmodel — see TraceCharacter.h's "THE FIRST-PERSON VIEWMODEL" block for the
// framing and depth arithmetic every number below was placed against.
// =================================================================================================

UStaticMeshComponent* ATraceCharacter::AddViewModelPart(UStaticMesh* InMesh, const TCHAR* DebugName,
	const FVector& Location, const FRotator& Rotation, const FVector& Size, UMaterialInstanceDynamic* MID)
{
	if (InMesh == nullptr || ViewModelRoot == nullptr)
	{
		return nullptr;
	}

	UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(
		this, MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), FName(DebugName)));
	if (Part == nullptr)
	{
		return nullptr;
	}

	Part->SetMobility(EComponentMobility::Movable);
	Part->SetupAttachment(ViewModelRoot);
	Part->SetStaticMesh(InMesh);
	Part->SetRelativeLocationAndRotation(Location, Rotation);
	Part->SetRelativeScale3D(Size / TraceCharacterLayout::ViewModelShapeUnit);

	// Contract section 7 again, and it matters more here than anywhere: the capsule is the ONLY
	// collider on this actor. A viewmodel is 40 uu from the eye — a colliding one would be a
	// permanent obstacle welded to the player's face.
	Part->SetCollisionProfileName(TEXT("NoCollision"));
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetGenerateOverlapEvents(false);
	Part->SetCanEverAffectNavigation(false);
	Part->bReceivesDecals = false;

	// NOBODY ELSE MAY EVER SEE THIS. bOnlyOwnerSee restricts it to the machine whose view target
	// owns it, and no shadow of any kind is cast, so there is no path by which a floating gun
	// appears in anyone else's frame — not even as a silhouette on the floor.
	Part->SetOnlyOwnerSee(true);
	Part->SetCastShadow(false);
	Part->bCastHiddenShadow = false;

	// The whole point of the rig. Set before RegisterComponent so the scene proxy is created with it
	// rather than having to be rebuilt; see the depth arithmetic in TraceCharacter.h's viewmodel block.
	Part->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;

	if (MID != nullptr)
	{
		Part->SetMaterial(0, MID);
	}

	Part->RegisterComponent();
	ViewModelParts.Add(Part);
	return Part;
}

void ATraceCharacter::EnsureViewModelBuilt()
{
	if (bViewModelBuilt || ViewModelRoot == nullptr || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	bViewModelBuilt = true;

	if (CubeMesh == nullptr || CylinderMesh == nullptr)
	{
		// /Engine/BasicShapes ships with every install, so this is close to impossible — but an
		// invisible gun is a far better failure than a crash, and it is the same contract every
		// other optional asset in this file honours.
		UE_LOG(LogTraceGame, Warning,
			TEXT("First-person viewmodel skipped: /Engine/BasicShapes did not resolve."));
		return;
	}

	// --- Materials -------------------------------------------------------------------------------
	//
	// The gun is made of the same two materials as the arena, which is what makes it look like it
	// belongs in the world rather than like a prop dropped into it.
	//
	// The body albedo (0.06) is four times the arena's structure albedo, and deliberately: the three
	// directional lights in this arena total under 6 lux and none of them is aimed at the inside of a
	// player's face, so a gun at the arena's own 0.015 would be a black hole in the middle of the
	// frame. The emissive term does the rest — it is the only lighting term that does not depend on
	// an angle of incidence, which is exactly the argument the arena's own cover blocks make.
	{
		UMaterialInterface* BodyParent = (SurfaceMaterial != nullptr) ? SurfaceMaterial.Get() : BasicShapeMaterial.Get();
		if (BodyParent != nullptr)
		{
			ViewModelBodyMID = UMaterialInstanceDynamic::Create(BodyParent, this);
			if (ViewModelBodyMID != nullptr)
			{
				// MEASURED. The first pass ran albedo 0.055 at metallic 0.35, and against a black
				// arena that produced two flat mid-blue slabs where the hands should be: bright
				// enough to be the largest object in the frame, dark enough to have no detail in it,
				// and glossy enough to pick the floor's cyan up over the whole surface. Dropping the
				// albedo and most of the metallic hands the shape reading back to the light channels,
				// which is the same argument the arena's own cover blocks make - the silhouette is
				// drawn in neon, and the body is what the neon is drawn ON.
				const FLinearColor BodyColor(0.028f, 0.032f, 0.042f);
				ViewModelBodyMID->SetVectorParameterValue(TEXT("BaseColor"), BodyColor);
				ViewModelBodyMID->SetVectorParameterValue(TEXT("Color"), BodyColor);
				ViewModelBodyMID->SetScalarParameterValue(TEXT("Roughness"), 0.52f);
				ViewModelBodyMID->SetScalarParameterValue(TEXT("Metallic"), 0.12f);
				ViewModelBodyMID->SetVectorParameterValue(
					TEXT("Emissive"), TraceCharacterLayout::ViewModelBodyEmissiveColor);
				ViewModelBodyMID->SetScalarParameterValue(
					TEXT("EmissiveStrength"), TraceCharacterLayout::ViewModelBodyEmissiveStrength);
			}
		}

		UMaterialInterface* NeonParent = (NeonMaterial != nullptr) ? NeonMaterial.Get() : BasicShapeMaterial.Get();
		if (NeonParent != nullptr)
		{
			ViewModelNeonMID = UMaterialInstanceDynamic::Create(NeonParent, this);
		}
	}

	// --- Geometry --------------------------------------------------------------------------------
	//
	// Rig space: +X out of the lens, +Y right, +Z up, origin at the top-rear of the grip. Every
	// number here was placed against the framing arithmetic in TraceCharacter.h's viewmodel block, so
	// the gun sits in the lower right with its highest point a quarter of a frame below the crosshair.
	struct FViewModelPart
	{
		const TCHAR* Name;
		bool bCylinder;
		FVector Location;
		FRotator Rotation;
		FVector Size;
		bool bNeon;
		/** Part of the WEAPON (skipped when the railgun was built) rather than part of the HANDS. */
		bool bWeapon;
	};

	// [SPEC v31 §6] *** THIS IS WHAT REPLACES THE PROCEDURAL CUBE HANDS. ***
	//
	// Built FIRST, because the weapons are placed against its wrist bone. A false here is not an
	// error and is the normal state of a fresh clone: the four hand cubes, the two knuckle bars, the
	// two forearms and the two cuffs below are then built exactly as they always were. Both halves of
	// the fallback promise in the file header — a clone with no `git lfs pull`, and
	// -TraceNoCharacterArt asked for on purpose — go through this one return value.
	const bool bPackHands = BuildPackHandsViewModel();

	// =============================================================================================
	// *** DEMO 29 ITEM 2 — THE OWNER'S ARMS RIG. THIS IS THE ONLY SEAM IT CUTS INTO THIS FILE. ***
	// =============================================================================================
	//
	// "Implement this only in the practice range, for testing purposes." One call, immediately after
	// the pack rig it sits on top of and before anything is placed against that rig, and every line
	// of it is behind TracePracticeRange::IsActive() — so in a match this is one game-mode cast that
	// answers no and returns. See BuildOwnerArmsViewModel and this file's header block.
	//
	// AFTER BuildPackHandsViewModel AND NOT INSIDE IT, deliberately: the fixture is DERIVED from what
	// that function measured (HandsWristRestRig, re-based onto Idle_Pistol t=0 at the bottom of it),
	// so it cannot be computed until that function has finished, and putting it inside would bury a
	// range-only branch in the middle of the shipped rig's build.
#if !UE_BUILD_SHIPPING
	BuildOwnerArmsViewModel(bPackHands);
#endif

	// The railgun replaces the twelve gun parts if its art resolved. The hands and arms below are
	// built either way — they are what holds whichever weapon won.
	const bool bRailgun = BuildRailgunViewModel();

	// SPEC v30 §2 — and the SMG is built BESIDE the pistol, not instead of it. Both rigs exist from
	// this moment on and UpdateWeaponSelection() decides which one is drawn; see the declaration for
	// why a swap must not be allowed to construct geometry. A false here is not an error: the `3`
	// slot then shows whichever pistol rig this pawn got, and says so once.
	BuildSmgViewModel();

	const FViewModelPart Parts[] =
	{
		// The gun. A slide over a frame over a raked grip: three masses, which is what makes a
		// blocky shape read as a handgun rather than as a brick.
		{ TEXT("VMSlide"),      false, FVector(9.0f, 0.f, 2.4f),    FRotator::ZeroRotator,        FVector(21.0f, 4.6f, 5.2f),  false, true  },
		{ TEXT("VMFrame"),      false, FVector(7.0f, 0.f, -1.6f),   FRotator::ZeroRotator,        FVector(16.5f, 4.2f, 4.4f),  false, true  },
		{ TEXT("VMGrip"),       false, FVector(-1.6f, 0.f, -8.2f),  FRotator(14.f, 0.f, 0.f),     FVector(5.6f, 4.0f, 13.5f),  false, true  },
		{ TEXT("VMGuard"),      false, FVector(3.2f, 0.f, -4.6f),   FRotator::ZeroRotator,        FVector(5.6f, 3.0f, 1.4f),   false, true  },

		// Light channels. The muzzle ring is a cylinder turned to point down the barrel (pitch 90
		// swings the shape's own +Z axis onto +X), and it is the piece that makes the gun read as a
		// weapon at a glance: a lit circle where the shot comes out.
		{ TEXT("VMMuzzle"),     true,  TraceCharacterLayout::CubeGunMuzzle, FRotator(90.f, 0.f, 0.f), FVector(5.8f, 5.8f, 2.2f), true,  true  },
		{ TEXT("VMSlideNeon"),  false, FVector(8.4f, 0.f, 5.3f),    FRotator::ZeroRotator,        FVector(16.5f, 1.8f, 1.5f),  true,  true  },
		{ TEXT("VMSight"),      false, FVector(17.6f, 0.f, 5.6f),   FRotator::ZeroRotator,        FVector(1.4f, 1.4f, 2.0f),   true,  true  },
		{ TEXT("VMSideNeonL"),  false, FVector(7.6f, -2.4f, 0.4f),  FRotator::ZeroRotator,        FVector(12.5f, 0.9f, 1.6f),  true,  true  },
		{ TEXT("VMSideNeonR"),  false, FVector(7.6f, 2.4f, 0.4f),   FRotator::ZeroRotator,        FVector(12.5f, 0.9f, 1.6f),  true,  true  },
		{ TEXT("VMGripNeon"),   false, FVector(-3.9f, 0.f, -8.0f),  FRotator(14.f, 0.f, 0.f),     FVector(1.2f, 3.0f, 9.5f),   true,  true  },
		{ TEXT("VMRearSight"),  false, FVector(1.4f, 0.f, 5.6f),    FRotator::ZeroRotator,        FVector(1.6f, 3.6f, 2.0f),   true,  true  },
		{ TEXT("VMRailNeon"),   false, FVector(6.5f, 0.f, -3.9f),   FRotator::ZeroRotator,        FVector(13.0f, 1.6f, 1.0f),  true,  true  },

		// Hands. Blocks, not fingers: at this scale and this framing a gloved fist is a shape, and
		// trying to model knuckles on a 6 uu cube only produces noise. What DOES read is a lit bar
		// across each one — a knuckle line, in the same language as everything else in this world.
		{ TEXT("VMHandR"),      false, FVector(-0.8f, 0.f, -4.6f),  FRotator(14.f, 0.f, 0.f),     FVector(5.6f, 5.4f, 7.0f),   false, false },
		{ TEXT("VMKnuckleR"),   false, FVector(1.4f, 0.f, -3.2f),   FRotator(14.f, 0.f, 0.f),     FVector(1.2f, 5.0f, 4.6f),   true,  false },
		{ TEXT("VMHandL"),      false, FVector(2.8f, -3.4f, -4.0f), FRotator::ZeroRotator,        FVector(5.0f, 4.8f, 5.6f),   false, false },
		{ TEXT("VMKnuckleL"),   false, FVector(4.9f, -3.4f, -3.0f), FRotator::ZeroRotator,        FVector(1.1f, 4.4f, 3.8f),   true,  false }
	};

	// The right hand does not move: RailgunOrigin was DERIVED from it, so the railgun's grip lands
	// in it by construction. The left hand does — it comes off the cube gun's frame and forward onto
	// the railgun's foregrip, which is 7.4 uu further out (it was 9.6 before the weapon size law
	// brought the railgun down to the pack's 0.34 m; the foregrip is a mesh landmark, so it moved in
	// with the gun and RailgunLeftHand followed it without being retyped).
	constexpr int32 LeftHandIndex = 14;
	constexpr int32 LeftKnuckleIndex = 15;
	checkf(FCString::Strcmp(Parts[LeftHandIndex].Name, TEXT("VMHandL")) == 0
		&& FCString::Strcmp(Parts[LeftKnuckleIndex].Name, TEXT("VMKnuckleL")) == 0,
		TEXT("The viewmodel part table was reordered; the left-hand indices below no longer point "
			 "at the left hand, so the railgun would be held by nothing."));

	// [SPEC v31 §6] TraceCharacterLayout::HandsGripRig is not an independent number — it IS VMHandR's
	// position, which is the point RailgunOrigin and SmgOrigin were both derived from. The pack hands
	// put their fist there so the guns do not move. Asserted rather than commented, because the
	// failure mode of the two drifting apart is a fist closed on empty air next to a floating gun.
	constexpr int32 RightHandIndex = 12;
	checkf(FCString::Strcmp(Parts[RightHandIndex].Name, TEXT("VMHandR")) == 0
		&& Parts[RightHandIndex].Location.Equals(TraceCharacterLayout::HandsGripRig, 0.01f),
		TEXT("TraceCharacterLayout::HandsGripRig no longer matches VMHandR in the parts table; the "
			 "pack hands would close on a grip that is not where the weapons put theirs."));

	// [DUALWIELD] SPEC v28 §10 — the off hand comes off the weapon entirely and takes the knife.
	//
	// ONE `if`, ABOVE THE EXISTING TERNARIES RATHER THAN INSIDE THEM, so the railgun/cube choice below
	// is exactly the code that shipped in v27 and a revert has nothing to unpick. The anchor is
	// remembered on the actor (ViewModelOffHandLocation) because UTraceWeaponComponent needs to hang
	// KnifeViewRoot at the same point and must not carry a second copy of these numbers — that is the
	// duplicate-constant failure this codebase logs by name.
	//
	// READ AT BUILD TIME, WHICH IS ONCE PER PAWN. Flipping Trace.Knife.DualWield mid-session changes
	// every rule immediately but re-poses the hand on the next respawn; the .ini and the launch flag,
	// which are how the switch is actually meant to be thrown, are both set before any pawn exists.
	// Stated so nobody spends time on a "the hand did not move" that is not a bug.
	const bool bDualWieldPose = TraceMelee::IsDualWieldEnabled();

	const FVector LeftHand = bDualWieldPose
		? TraceCharacterLayout::DualWieldLeftHand
		: (bRailgun ? TraceCharacterLayout::RailgunLeftHand : Parts[LeftHandIndex].Location);
	const FVector LeftKnuckle = bDualWieldPose
		? TraceCharacterLayout::DualWieldLeftKnuckle
		: (bRailgun ? TraceCharacterLayout::RailgunLeftKnuckle : Parts[LeftKnuckleIndex].Location);

	// [SPEC v31 §6] With the pack rig up, the off-hand anchor is the REAL left wrist — already written
	// by BuildPackHandsViewModel out of the imported skeleton's reference pose — and must not be
	// overwritten by the cube table's guess at where a hand used to be. UTraceWeaponComponent hangs
	// the knife on this point, so a stale value would float the blade a hand's width from the fist.
	if (!bPackHands)
	{
		ViewModelOffHandLocation = LeftHand;
	}
	bViewModelOffHandFree = bDualWieldPose;

	/** The cube gun's lit muzzle ring, when that rig is the one built. See the muzzle marker below. */
	UStaticMeshComponent* CubeGunMuzzlePart = nullptr;

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Parts); ++Index)
	{
		const FViewModelPart& Part = Parts[Index];
		if (bRailgun && Part.bWeapon)
		{
			continue;
		}

		// [SPEC v31 §6] The four hand and knuckle cubes are what SK_TraceHands replaces. The weapon
		// parts are untouched by this: a machine with the pack hands but no railgun art still gets
		// the procedural cube gun, held by real fingers.
		if (bPackHands && !Part.bWeapon)
		{
			continue;
		}

		FVector Location = Part.Location;
		if (Index == LeftHandIndex)
		{
			Location = LeftHand;
		}
		else if (Index == LeftKnuckleIndex)
		{
			Location = LeftKnuckle;
		}

		UStaticMeshComponent* Built = AddViewModelPart(Part.bCylinder ? CylinderMesh : CubeMesh, Part.Name,
			Location, Part.Rotation, Part.Size,
			Part.bNeon ? ViewModelNeonMID : ViewModelBodyMID);

		// [SPEC v30 §2] The cube gun's own twelve pieces ARE the pistol rig when the railgun art is
		// missing, so the selector has to be able to hide them for the knife and SMG states just as it
		// hides the railgun's. Only reached when !bRailgun — the `continue` above skipped them
		// otherwise — so this can never double-add.
		if (Part.bWeapon && Built != nullptr)
		{
			PistolWeaponParts.Add(Built);
		}

		// [SPEC v31 §6] The cube gun's lit muzzle ring is placed at exactly CubeGunMuzzle, which is
		// where the fallback rig's muzzle MARKER goes too. Remembering it lets the marker be parented
		// to the ring instead of to the rig root, so it rides the hand for free — the same argument
		// that already parents the railgun's marker to the railgun's body.
		if (Built != nullptr && FCString::Strcmp(Part.Name, TEXT("VMMuzzle")) == 0)
		{
			CubeGunMuzzlePart = Built;
		}
	}

	// --- Forearms --------------------------------------------------------------------------------
	//
	// Two cylinders running from the hands down and back out of frame, each with a neon cuff. They
	// are short (17-18 uu) for the depth reason in the file header: the far end must stay in front of
	// the near plane once FirstPersonScale has halved its depth. That is not a compromise — a real
	// viewmodel's arms leave the bottom of the frame within a few centimetres of the hands too.
	struct FForearmSpec
	{
		const TCHAR* Name;
		const TCHAR* CuffName;
		FVector Hand;
		FVector Direction;   // normalised in place; points away from the gun, down and outward
		float Length;
		float Diameter;
	};

	FForearmSpec Forearms[] =
	{
		{ TEXT("VMForearmR"), TEXT("VMCuffR"), FVector(-0.8f, 0.f, -4.6f),  FVector(-0.42f, 0.36f, -0.86f),  17.f, 7.0f },
		{ TEXT("VMForearmL"), TEXT("VMCuffL"), LeftHand,                    FVector(-0.40f, -0.38f, -0.86f), 16.f, 6.7f }
	};

	// *** THE PACK RIG GETS THESE TOO NOW, AND THAT IS THE CHANGE. ***
	//
	// It used to `break` here, on the reasoning that "the pack's mesh carries its own forearms and
	// cuffs — 23.5 cm of them". It does carry them, and they are not drawn any more: they run at the
	// LENS rather than out of frame, which is the salmon wedge HandsHiddenBones documents. With them
	// hidden the pack rig is two gloves and no arms, so the tubes the procedural rig already had — and
	// which were already photographed leaving the bottom of the frame — are re-anchored onto the real
	// wrists and drawn under the gloves instead. The numbers and the projection are on
	// HandsArmLengthRightUU.
	//
	// THE ANCHORS ARE THE CAPTURED WRIST RESTS, NOT NEW CONSTANTS, and that matters twice over: they
	// are already measured, and they are the exact transforms UpdateWeaponsFollowHands rebases
	// against — so the same delta that carries the guns carries the arms, and an arm can never drift
	// off the hand it belongs to.
	const bool bPackArms = bPackHands && bHandsRigActive;
	if (bPackArms)
	{
		// *** THE RIGHT ARM ANCHORS ON THE FIST AND NOT ON wrist_right, AND THAT IS THE FIX FOR THE
		//     MISSING PALM. *** The fist is 6 uu DEEPER than the wrist, so a tube leaving the wrist
		//     downward stands in FRONT of the hand and wins the depth test against it — the full
		//     measurement, the photograph and the two rules that follow from it are on
		//     HandsArmLengthRightUU. Anchored on the fist the near cap is buried inside the closed
		//     glove and nothing of the sleeve is ever nearer the lens than the hand.
		//
		//     IT IS STILL CARRIED BY wrist_right's DELTA, which is what makes this free: HandsGripRig
		//     is rigid with that bone (it is the fist centroid measured against it), so the rest
		//     transform captured here rides the same HandsWristDelta the guns do and the arm cannot
		//     come off the hand.
		Forearms[0].Hand = TraceCharacterLayout::HandsGripRig;
		Forearms[0].Direction = TraceCharacterLayout::HandsArmDirectionRight;
		Forearms[0].Length = TraceCharacterLayout::HandsArmLengthRightUU;
		Forearms[0].Diameter = TraceCharacterLayout::HandsArmDiameterUU;

		// The left glove holds nothing, so it has no fist centroid to anchor on and keeps its wrist.
		// It takes the re-aimed direction for the OTHER reason on HandsArmLengthRightUU: an arm that
		// leans toward the lens is what brought on-screen geometry to 8.03 uu drawn — inside the
		// 10 uu near plane — for a fifth of every Walljump_* clip.
		Forearms[1].Hand = HandsOffWristRestRig.GetLocation();
		Forearms[1].Direction = TraceCharacterLayout::HandsArmDirectionLeft;
		Forearms[1].Length = TraceCharacterLayout::HandsArmLengthLeftUU;
		Forearms[1].Diameter = TraceCharacterLayout::HandsArmDiameterUU;
	}

	// The glove's own shell material, so the arm reads as the same object as the hand. Slot 0 of
	// SK_TraceHands is `shell`, which is what both hidden forearms were bound to — this is literally
	// the material the pack's own arm had. Null on the fallback rig, where ViewModelBodyMID is right.
	//
	// *** IT IS A SIBLING INSTANCE NOW, NOT THE GLOVE'S OWN. *** Same parent, same base colour, same
	// roughness — a DIFFERENT emissive floor, because the glove has been lifted clear of the weapon
	// it holds and these two tubes must not come up with it (HandsGloveEmissiveStrength has the
	// argument, HandsArmMID the storage). Slot 0 remains the fallback for the case where `shell` is
	// not on the export, which is exactly what shipped before.
	UMaterialInterface* PackArmMaterial = (bPackArms && HandsPart != nullptr)
		? ToRawPtr(HandsArmMID) : nullptr;
	if (bPackArms && PackArmMaterial == nullptr && HandsPart != nullptr)
	{
		PackArmMaterial = HandsPart->GetMaterial(0);
	}

	// AND THE BAND ON IT IS THE GLOVE'S OWN CIRCUIT, IN THE CHARACTER'S ACCENT, NOT THE ARENA'S NEON.
	//
	// *** IT IS A SIBLING INSTANCE NOW, NOT THE GLOVE'S OWN — the same move HandsArmMID made one
	// paragraph up, and for the same shape of reason. *** BuildHandsEmissive (which ran a moment ago,
	// walking the slot names rather than trusting an index) makes HandsCuffMID off MI_Pack_circuit_cyan
	// and writes the roster accent into it, so the band is the glove's circuit in a different HUE
	// while UpdateHandsEmissive drives its EmissiveIntensity from the SAME breath-and-flare value as
	// the knuckle rings. It therefore still breathes at rest and still flares on an action with the
	// rest of the glove, with no second thing to keep in step — CHARACTER_SHEETS §1, which makes this
	// ring the one and only per-character object in first person.
	//
	// TWO FALLBACKS, IN ORDER, AND NEITHER CHANGES A FRAME THAT USED TO WORK: the glove's own circuit
	// MID if the accent instance could not be created, and the arena neon if the glove's emissive did
	// not resolve at all — which is the same degradation BuildHandsEmissive already logs.
	UMaterialInterface* PackCuffMaterial = bPackArms ? ToRawPtr(HandsCuffMID) : nullptr;
	if (bPackArms && PackCuffMaterial == nullptr && HandsCyanMIDs.Num() > 0)
	{
		PackCuffMaterial = ToRawPtr(HandsCyanMIDs[0]);
	}

	HandsForearmParts.Reset();
	HandsForearmRest.Reset();
	HandsForearmRightNum = 0;

	for (const FForearmSpec& Arm : Forearms)
	{
		const FVector Dir = Arm.Direction.GetSafeNormal();
		if (Dir.IsNearlyZero())
		{
			continue;
		}

		// MakeFromZ because the basic cylinder's axis is its own +Z; this turns that axis onto the
		// arm direction without having to hand-derive a rotator.
		const FRotator ArmRotation = FRotationMatrix::MakeFromZ(Dir).Rotator();

		// The procedural rig starts its tube 2 uu clear of the cube hand because a cube hand is
		// 5 uu across and the tube would otherwise poke out of the knuckles. The pack arms start at
		// their anchor with no stand-off at all, because both anchors are INSIDE the glove — the
		// right one is the closed fist's own centroid and the left one is the wrist joint — so the
		// glove hides the near cap and no seam can open at the cuff.
		const float Standoff = bPackArms ? 0.f : 2.f;
		const FVector ArmCentre = Arm.Hand + Dir * (Standoff + Arm.Length * 0.5f);

		UStaticMeshComponent* const ArmPart = AddViewModelPart(CylinderMesh, Arm.Name, ArmCentre,
			ArmRotation, FVector(Arm.Diameter, Arm.Diameter, Arm.Length), ViewModelBodyMID);

		if (ArmPart != nullptr && PackArmMaterial != nullptr)
		{
			ArmPart->SetMaterial(0, PackArmMaterial);
		}

		// THE LIT BAND, AND BOTH OF ITS NUMBERS ARE SOLVED AGAINST THE FRAME RATHER THAN AGAINST THE
		// GLOVE — see HandsArmCuffAlongUU for the run of vertical frame fractions that fixes them.
		// The short version: the pack band used to sit 9 uu down, which is 5% of the half-frame BELOW
		// the bottom edge, so the arm shipped with no band on it and read as more gun body.
		const float CuffAlong = bPackArms
			? TraceCharacterLayout::HandsArmCuffAlongUU
			: TraceCharacterLayout::CubeArmCuffAlongUU;
		const float CuffProud = bPackArms
			? TraceCharacterLayout::HandsArmCuffProudUU
			: TraceCharacterLayout::CubeArmCuffProudUU;
		UStaticMeshComponent* const CuffPart = (Arm.CuffName != nullptr)
			? AddViewModelPart(CylinderMesh, Arm.CuffName, Arm.Hand + Dir * CuffAlong, ArmRotation,
				FVector(Arm.Diameter + CuffProud, Arm.Diameter + CuffProud, 1.8f), ViewModelNeonMID)
			: nullptr;

		if (CuffPart != nullptr && PackCuffMaterial != nullptr)
		{
			CuffPart->SetMaterial(0, PackCuffMaterial);
		}

		// [pack] Remembered so UpdateWeaponsFollowHands can carry each arm on ITS OWN wrist's delta.
		// The two hands are not rigid with each other (see HandsOffWristDelta), so one delta for both
		// would leave the left arm behind on every reload. RIGHT IS BUILT FIRST and the count is
		// recorded rather than an index being assumed, so a part that failed to build shifts nothing.
		if (bPackArms)
		{
			for (UStaticMeshComponent* const Part : { ArmPart, CuffPart })
			{
				if (Part != nullptr)
				{
					HandsForearmParts.Add(Part);
					HandsForearmRest.Add(Part->GetRelativeTransform());
				}
			}
			if (&Arm == &Forearms[0])
			{
				HandsForearmRightNum = HandsForearmParts.Num();
			}
		}
	}

	// --- [SPEC v31 §6] The rest transforms the hand-follow is expressed against -------------------
	//
	// READ BACK OFF THE COMPONENTS rather than re-derived from RailgunOrigin / SmgOrigin / the parts
	// table. Three reasons, and the third is the one that matters: the twelve cube-gun parts have no
	// named constants at all; retuning a scale or an origin then moves this automatically; and a
	// second hand-typed copy of a placement is the duplicate-constant failure this codebase logs by
	// name. Captured here, once, while every part is still at its shipped rest pose — before
	// UpdateWeaponsFollowHands has had a chance to move anything.
	PistolWeaponRest.Reset(PistolWeaponParts.Num());
	for (const TObjectPtr<UStaticMeshComponent>& Part : PistolWeaponParts)
	{
		PistolWeaponRest.Add(Part != nullptr ? Part->GetRelativeTransform() : FTransform::Identity);
	}
	SmgWeaponRest.Reset(SmgWeaponParts.Num());
	for (const TObjectPtr<UStaticMeshComponent>& Part : SmgWeaponParts)
	{
		SmgWeaponRest.Add(Part != nullptr ? Part->GetRelativeTransform() : FTransform::Identity);
	}

	// --- The muzzle marker (spec v26 §4) ---------------------------------------------------------
	//
	// "The bullet tracer animation needs to come from the gun barrel, not above or behind it."
	//
	// The reason it did not is that ATraceTracer had no way to ASK where the barrel was: it carried
	// three hand-tuned camera-space constants (a standoff plus a right/down screen offset) that had
	// been eyeballed against the small cube gun and were never revisited when the 185 cm railgun
	// replaced it. This is the fix at its root — the barrel now says where it is.
	//
	// Parented to the GUN, at the gun's own muzzle landmark, in the gun's own units:
	//   * railgun    -> a child of RailgunBodyPart at RailgunMuzzleLocal, the (107.4, 0, 4.5) cm point
	//                   recorded in railgun_manifest.json. The parent's RailgunScale is applied by the
	//                   scene graph, so this stays the mesh's real muzzle vertex whatever the rig
	//                   scale becomes.
	//   * cube gun   -> a child of ViewModelRoot at CubeGunMuzzle, the same constant the VMMuzzle ring
	//                   above is placed with.
	//
	// It is deliberately NOT a constant in ATraceTracer, and deliberately NOT the rig root: every
	// motion the gun has — recoil, sway, bob, the slide dip — is a transform on one of its ancestors,
	// and a marker under them inherits all of it with no code that has to remember to.
	// [SPEC v31 §6] THIRD CASE, and it exists because the guns now move: with the pack hands up and no
	// railgun art, a marker parented to the RIG ROOT would sit still while the cube gun rode the hand
	// away from it — a beam leaving from where the barrel used to be, which is the exact defect v26 §4
	// closed. Parented to the lit muzzle RING instead, which is placed at CubeGunMuzzle and is itself
	// carried by the hand-follow pass, so the marker inherits every motion the gun has with no code.
	if (ViewModelMuzzle == nullptr)
	{
		USceneComponent* MuzzleParent = (RailgunBodyPart != nullptr)
			? static_cast<USceneComponent*>(RailgunBodyPart)
			: (CubeGunMuzzlePart != nullptr
				? static_cast<USceneComponent*>(CubeGunMuzzlePart)
				: static_cast<USceneComponent*>(ViewModelRoot));
		const FVector MuzzleLocal = (RailgunBodyPart != nullptr)
			? TraceCharacterLayout::RailgunMuzzleLocal
			: (CubeGunMuzzlePart != nullptr ? FVector::ZeroVector : TraceCharacterLayout::CubeGunMuzzle);

		ViewModelMuzzle = NewObject<USceneComponent>(this, TEXT("ViewModelMuzzle"));
		if (ViewModelMuzzle != nullptr)
		{
			ViewModelMuzzle->SetMobility(EComponentMobility::Movable);
			ViewModelMuzzle->SetupAttachment(MuzzleParent);
			ViewModelMuzzle->SetRelativeLocation(MuzzleLocal);
			ViewModelMuzzle->RegisterComponent();
		}
	}

	// [SPEC v30 §5] The SMG's own marker, on the SMG's own body, at the SMG's own aperture. Same
	// arrangement, second gun — GetActiveMuzzleMarker() picks between them by what is DRAWN, so the
	// beam leaves whichever barrel the player is actually looking at with no change in ATraceTracer.
	if (ViewModelSmgMuzzle == nullptr && SmgBodyPart != nullptr)
	{
		ViewModelSmgMuzzle = NewObject<USceneComponent>(this, TEXT("ViewModelSmgMuzzle"));
		if (ViewModelSmgMuzzle != nullptr)
		{
			ViewModelSmgMuzzle->SetMobility(EComponentMobility::Movable);
			ViewModelSmgMuzzle->SetupAttachment(SmgBodyPart);
			ViewModelSmgMuzzle->SetRelativeLocation(TraceCharacterLayout::SmgMuzzleLocal);
			ViewModelSmgMuzzle->RegisterComponent();
		}
	}

	// Hidden until UpdateViewBlend says first person; ApplyTeamColors paints the light channels.
	for (UStaticMeshComponent* Part : ViewModelParts)
	{
		if (Part != nullptr)
		{
			Part->SetVisibility(false);
		}
	}
	bViewModelVisible = false;

	// [SPEC v30 §2] Two guns are now on the rig and only one of them may be drawn. Settle that here,
	// before the first frame, so a pawn that spawns holding the SMG never shows a pistol — not even
	// for the one tick it would take Tick() to get around to it.
	UpdateWeaponSelection();

#if !UE_BUILD_SHIPPING
	// [DEMO 29 §2] And the same courtesy for the hand rig, for the same "not even for one tick"
	// reason — but it has to be HERE rather than at the bottom of BuildOwnerArmsViewModel, and that
	// is a fixed bug and not a preference. The fixture also puts away the pack's two procedural
	// SLEEVE tubes (the owner's rig has real forearms), and those are built by the parts loop ABOVE,
	// which runs after the hand rigs. Settled from inside the build they were still an empty array
	// and stayed on screen — photographed, once.
	UpdateOwnerArmsRig();

	// AND THE HOLD POSE WITH IT, on the same "not even for one tick" argument. BuildPackHandsViewModel
	// has already run UpdateHandsAnimation(0.f), so HandsLoadout is settled and this picks the right
	// hold; HandsWristDelta is still identity until the first UpdateWeaponsFollowHands, which is what
	// the weapons are placed at right now too, so the two agree on this frame as on every other.
	UpdateOwnerArmsPose();
#endif

	ApplyTeamColors();

	UE_LOG(LogTraceGame, Log,
		TEXT("%s built a first-person viewmodel (%d parts, hands=%s, pistol muzzle on %s, SMG rig %s)."),
		*GetName(), ViewModelParts.Num(),
		bPackHands ? TEXT("SK_TraceHands + 20 clips") : TEXT("PROCEDURAL CUBES (fallback)"),
		(RailgunBodyPart != nullptr) ? TEXT("the railgun body") : TEXT("the fallback rig"),
		(SmgBodyPart != nullptr) ? TEXT("built") : TEXT("ABSENT - the SMG slot falls back to the pistol"));
}

bool ATraceCharacter::BuildRailgunViewModel()
{
	if (RailgunBodyMesh == nullptr || RailgunRailLeftMesh == nullptr || RailgunRailRightMesh == nullptr)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("Railgun art did not resolve (body=%s railL=%s railR=%s); building the fallback gun. ")
			TEXT("Run Scripts/import-railgun.sh, or `git lfs pull` if this is a fresh clone."),
			RailgunBodyMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			RailgunRailLeftMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			RailgunRailRightMesh != nullptr ? TEXT("ok") : TEXT("MISSING"));
		return false;
	}

	// The same escape hatch -TraceNoCharacterArt gives the Mannequin: a way to SEE the fallback
	// without deleting anything, so "does the fallback still work" is a launch flag and not a
	// destructive experiment.
	if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoRailgun")))
	{
		UE_LOG(LogTraceGame, Log, TEXT("-TraceNoRailgun: building the procedural gun on purpose."));
		return false;
	}

	const float S = TraceCharacterLayout::RailgunScale;
	const FVector Size(TraceCharacterLayout::ViewModelShapeUnit * S);   // AddViewModelPart divides by the unit

	// The body carries its own materials from the asset, so no MID is passed: AddViewModelPart only
	// overrides slot 0, and this mesh has five slots that are already correct.
	RailgunBodyPart = AddViewModelPart(RailgunBodyMesh, TEXT("VMRailgunBody"),
		TraceCharacterLayout::RailgunOrigin, FRotator::ZeroRotator, Size, nullptr);

	// Each wall's mesh is baked around its hinge, so its rest position is the hinge offset scaled
	// into rig space and its rest rotation is zero. Those offsets came out of the source model and
	// are mirrored, hence one constant and a sign.
	const FVector HingeOffset(-5.0f, 7.8f, 4.5f);
	RailgunRailLeftPart = AddViewModelPart(RailgunRailLeftMesh, TEXT("VMRailgunRailL"),
		TraceCharacterLayout::RailgunOrigin + FVector(HingeOffset.X, -HingeOffset.Y, HingeOffset.Z) * S,
		FRotator::ZeroRotator, Size, nullptr);
	RailgunRailRightPart = AddViewModelPart(RailgunRailRightMesh, TEXT("VMRailgunRailR"),
		TraceCharacterLayout::RailgunOrigin + HingeOffset * S,
		FRotator::ZeroRotator, Size, nullptr);

	if (RailgunBodyPart == nullptr || RailgunRailLeftPart == nullptr || RailgunRailRightPart == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("Railgun parts failed to attach; falling back."));
		RailgunBodyPart = nullptr;
		RailgunRailLeftPart = nullptr;
		RailgunRailRightPart = nullptr;
		return false;
	}

	// Pull the two glowing slots out as dynamic instances. Found BY SLOT NAME, not by index: the
	// import assigns five slots and their order is an artefact of the OBJ writer, not a contract.
	const int32 CyanSlot = RailgunBodyPart->GetMaterialIndex(TraceCharacterAssets::RailgunCyanSlot);
	const int32 AmberSlot = RailgunBodyPart->GetMaterialIndex(TraceCharacterAssets::RailgunAmberSlot);
	if (CyanSlot != INDEX_NONE)
	{
		RailgunCyanMID = RailgunBodyPart->CreateDynamicMaterialInstance(CyanSlot);
	}
	if (AmberSlot != INDEX_NONE)
	{
		RailgunAmberMID = RailgunBodyPart->CreateDynamicMaterialInstance(AmberSlot);
	}
	if (RailgunCyanMID == nullptr || RailgunAmberMID == nullptr)
	{
		// Not fatal: the gun renders, it just will not flare. Loud, because a silently dead effect
		// is the exact failure this project keeps having to hunt down after the fact.
		UE_LOG(LogTraceGame, Warning,
			TEXT("Railgun built, but its glow will not animate: slot '%s'=%d, '%s'=%d."),
			*TraceCharacterAssets::RailgunCyanSlot.ToString(), CyanSlot,
			*TraceCharacterAssets::RailgunAmberSlot.ToString(), AmberSlot);
	}

	// [SPEC v30 §2] The three railgun parts are the PISTOL rig, and the selector has to be able to
	// take them off screen for the knife and SMG states.
	PistolWeaponParts.Add(RailgunBodyPart);
	PistolWeaponParts.Add(RailgunRailLeftPart);
	PistolWeaponParts.Add(RailgunRailRightPart);

	UE_LOG(LogTraceGame, Log, TEXT("%s built the railgun viewmodel (muzzle at rig x=%.1f)."),
		*GetName(),
		TraceCharacterLayout::RailgunOrigin.X + TraceCharacterLayout::RailgunMuzzleLocal.X * S);
	return true;
}

// =================================================================================================
// THE SMG RIG  —  spec v30 §2, §3, §4
// =================================================================================================
//
// "Demo 24 added the SMG with no viewmodel of its own — a verifier flagged that nothing on screen
// tells the player which gun they are holding."
//
// THE SHAPE OF THE FIX. Three weapon states exist and the answer has to be visible at a glance
// rather than in an ammo counter:
//
//     no gun  ->  both weapon rigs off screen, hands and knife only
//     pistol  ->  the pistol rig  (the railgun, or the procedural cube gun if the art is missing)
//     SMG     ->  THE SMG RIG     (or, if THAT art is missing, the pistol rig and a line in the log)
//
// *** THE KEY NUMBERS ARE DELIBERATELY NOT WRITTEN HERE ANY MORE (spec v32 §7d). *** This paragraph
// used to say "1 stows, 2 pistol, 3 SMG", which was v29 §5's arrangement; Demo 26 reverted the binds
// to 1 = PISTOL, 2 = SMG, 3 = KNIFE and this comment quietly became a lie that a reader would trust.
// Which key does what is UTraceUserSettings' business and a player can rebind it anyway, so the rule
// belongs there and the selector below is stated in WEAPONS, which cannot go stale.
//
// WHY THE MOTION IS CODE AND NOT AN ANIMATION. The GLB is the mesh-only export: `animations: []`.
// The kit's README promises `Fire` and `Reload` clips and warns, in the same paragraph, that the
// stage's bottom-right toolbar exports without them — which is the export we have. Waiting for the
// clips is not an option and is not necessary: §3 states the whole motion in numbers, and the
// pistol's Fire was reproduced from numbers the same way in spec v20. What is reproduced here is
// driven off the WEAPON'S REAL STATE — the fire cycle by NotifyWeaponFired, the reload by the
// component's own replicated deadline — so the picture cannot disagree with the gun.

bool ATraceCharacter::BuildSmgViewModel()
{
	if (SmgBodyMesh == nullptr || SmgWallLeftMesh == nullptr
		|| SmgWallRightMesh == nullptr || SmgMagMesh == nullptr)
	{
		// *** THE FALLBACK MUST SURVIVE. *** A fresh clone that has not run `git lfs pull` has the
		// .uasset files as LFS pointer stubs, so this is the NORMAL first-run state and not an
		// error — it must leave a playable game and a log line that says the one command that fixes
		// it. Warning rather than Error for the same reason the railgun's is: this is a missing
		// optional asset, and the pawn behind it is fully functional.
		UE_LOG(LogTraceGame, Warning,
			TEXT("SMG art did not resolve (body=%s wallL=%s wallR=%s mag=%s); the SMG weapon slot will ")
			TEXT("show the pistol rig instead. Run `Scripts/import-railgun.sh --rig smg`, or ")
			TEXT("`git lfs pull` if this is a fresh clone."),
			SmgBodyMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			SmgWallLeftMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			SmgWallRightMesh != nullptr ? TEXT("ok") : TEXT("MISSING"),
			SmgMagMesh != nullptr ? TEXT("ok") : TEXT("MISSING"));
		return false;
	}

	// The same escape hatch the railgun has, and -TraceNoRailgun suppresses BOTH: "show me the
	// fallback" is one intention, and having to remember two switches to express it is how a
	// half-forced state gets photographed and reported as a bug.
	if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoSmg"))
		|| FParse::Param(FCommandLine::Get(), TEXT("TraceNoRailgun")))
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("-TraceNoSmg/-TraceNoRailgun: the SMG slot will show the pistol rig on purpose."));
		return false;
	}

	const float S = TraceCharacterLayout::SmgScale;
	const FVector Size(TraceCharacterLayout::ViewModelShapeUnit * S);   // AddViewModelPart divides by the unit

	// ALL FOUR PARTS SIT AT THE SAME ORIGIN, and that is a property of the export rather than a
	// simplification. The railgun's walls had to be baked around inferred hinges and placed at a
	// mirrored offset; this GLB carries authored pivot nodes (wall_pivot_left/right, mag_pivot) and
	// every one of them is at (0,0,0) relative to the weapon root, so each group's rest transform is
	// SmgOrigin exactly and its motion is a pure delta on top. railgun_smg_manifest.json records
	// `attach_to_body_cm: [0,0,0]` for all three, which is where that claim is checked.
	//
	// No MID is passed on any of them: these meshes carry their own imported material instances on
	// four, two, two and three slots respectively, and AddViewModelPart's override only reaches
	// slot 0.
	SmgBodyPart = AddViewModelPart(SmgBodyMesh, TEXT("VMSmgBody"),
		TraceCharacterLayout::SmgOrigin, FRotator::ZeroRotator, Size, nullptr);
	SmgWallLeftPart = AddViewModelPart(SmgWallLeftMesh, TEXT("VMSmgWallL"),
		TraceCharacterLayout::SmgOrigin, FRotator::ZeroRotator, Size, nullptr);
	SmgWallRightPart = AddViewModelPart(SmgWallRightMesh, TEXT("VMSmgWallR"),
		TraceCharacterLayout::SmgOrigin, FRotator::ZeroRotator, Size, nullptr);
	SmgMagPart = AddViewModelPart(SmgMagMesh, TEXT("VMSmgMag"),
		TraceCharacterLayout::SmgOrigin, FRotator::ZeroRotator, Size, nullptr);

	if (SmgBodyPart == nullptr || SmgWallLeftPart == nullptr
		|| SmgWallRightPart == nullptr || SmgMagPart == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("SMG parts failed to attach; the SMG slot falls back to the pistol."));
		SmgBodyPart = nullptr;
		SmgWallLeftPart = nullptr;
		SmgWallRightPart = nullptr;
		SmgMagPart = nullptr;
		return false;
	}

	SmgWeaponParts.Add(SmgBodyPart);
	SmgWeaponParts.Add(SmgWallLeftPart);
	SmgWeaponParts.Add(SmgWallRightPart);
	SmgWeaponParts.Add(SmgMagPart);

	// --- The two glowing slots, and the one trap in this whole section ---------------------------
	//
	// *** THE SMG'S GLOW IS SPLIT ACROSS MESHES; THE PISTOL'S IS NOT. *** On the railgun both glowing
	// materials are on the single body mesh, so "two MIDs off the body part" was the whole pattern.
	// Here:
	//
	//     circuit_cyan  ->  Body (slot 2), WallLeft (slot 1), WallRight (slot 1).  NOT on the Mag.
	//     core_amber    ->  Mag (slot 1).                                          NOT on the Body.
	//
	// Copying the pistol's pattern verbatim therefore asks the BODY for core_amber, gets INDEX_NONE,
	// and produces an ammo readout that silently never lights — the failure mode this project keeps
	// having to find after the fact. Found BY SLOT NAME on each component that actually has it, for
	// the same reason the pistol's are: slot ORDER is an artefact of the OBJ writer, not a contract.
	const auto AddGlowMID = [this](UStaticMeshComponent* Part, const FName& Slot) -> UMaterialInstanceDynamic*
	{
		if (Part == nullptr)
		{
			return nullptr;
		}
		const int32 Index = Part->GetMaterialIndex(Slot);
		return (Index != INDEX_NONE) ? Part->CreateDynamicMaterialInstance(Index) : nullptr;
	};

	SmgCyanMIDs.Reset();
	for (UStaticMeshComponent* Part : { SmgBodyPart.Get(), SmgWallLeftPart.Get(), SmgWallRightPart.Get() })
	{
		if (UMaterialInstanceDynamic* MID = AddGlowMID(Part, TraceCharacterAssets::RailgunCyanSlot))
		{
			SmgCyanMIDs.Add(MID);
		}
	}
	SmgAmberMID = AddGlowMID(SmgMagPart, TraceCharacterAssets::RailgunAmberSlot);

	if (SmgCyanMIDs.Num() != 3 || SmgAmberMID == nullptr)
	{
		// Not fatal — the gun renders, it just will not flare or report ammo. Loud, and it names the
		// count rather than just "failed", because 2-of-3 cyan (a wall that stays dark through every
		// shot) is a real and much less obvious failure than 0-of-3.
		UE_LOG(LogTraceGame, Warning,
			TEXT("SMG built, but its glow is incomplete: circuit_cyan MIDs %d/3 (body+both walls), ")
			TEXT("core_amber on the magazine %s."),
			SmgCyanMIDs.Num(), SmgAmberMID != nullptr ? TEXT("ok") : TEXT("MISSING"));
	}

	// THE REST POSE HAS TO BE WRITTEN NOW, not on the first shot. circuit_cyan idles at 1.8x and the
	// imported material instance's own EmissiveIntensity default is 1.0, so a gun that is drawn but
	// never fired would sit visibly dull — and UpdateSmgAnimation only runs while the rig is on
	// screen, so there is no later moment that is guaranteed to happen first.
	for (UMaterialInstanceDynamic* MID : SmgCyanMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), TraceCharacterLayout::SmgCyanRest);
		}
	}
	if (SmgAmberMID != nullptr)
	{
		SmgAmberMID->SetScalarParameterValue(TEXT("EmissiveIntensity"), TraceCharacterLayout::SmgAmberFull);
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("%s built the SMG viewmodel (4 parts at scale %.2f, muzzle at rig x=%.1f, cyan MIDs %d, amber %s)."),
		*GetName(), S,
		TraceCharacterLayout::SmgOrigin.X + TraceCharacterLayout::SmgMuzzleLocal.X * S,
		SmgCyanMIDs.Num(), SmgAmberMID != nullptr ? TEXT("ok") : TEXT("MISSING"));
	return true;
}

// =================================================================================================
// THE PACK'S GLOVED HANDS  —  spec v31 §6
// =================================================================================================
//
// "Implement the new hand model and animations with an idle hold for the guns, core, and knife;
//  reload, stab, shoot, and move on a jump and wall jump"
//
// The design rationale, the axis convention, the scale measurements and the reason the guns follow
// the wrist by transform rather than by attachment are all in TraceCharacter.h. What is here is the
// build and the state machine.

namespace
{
	/**
	 * A bone's REFERENCE-POSE transform in COMPONENT space.
	 *
	 * Written out rather than using USkinnedMeshComponent::GetRefPoseTransform, which returns the
	 * bone's transform in its PARENT'S space — for `wrist_right`, a 2 cm offset from `hand_right`,
	 * which is not the number anything here wants. And deliberately NOT read off the live component
	 * (GetBoneTransform), because at build time the component has not ticked its pose yet and would
	 * answer with whatever the last evaluation left behind.
	 */
	FTransform RefPoseComponentSpace(const FReferenceSkeleton& RefSkeleton, int32 BoneIndex)
	{
		FTransform Result = FTransform::Identity;
		const TArray<FTransform>& BonePose = RefSkeleton.GetRefBonePose();

		for (int32 Index = BoneIndex; Index != INDEX_NONE; Index = RefSkeleton.GetParentIndex(Index))
		{
			if (!BonePose.IsValidIndex(Index))
			{
				return FTransform::Identity;
			}
			Result = Result * BonePose[Index];
		}
		return Result;
	}
}

bool ATraceCharacter::BuildPackHandsViewModel()
{
	if (ViewModelRoot == nullptr)
	{
		return false;
	}

	// *** THE FALLBACK MUST SURVIVE, and this is the gate that keeps it reachable. ***
	//
	// -TraceNoCharacterArt is the switch the file header has always promised would reach the
	// procedural rig, so it has to reach THIS one too — it would be a strange kind of "art disabled"
	// that still drew imported hands. -TraceNoPackHands is the narrower form, for looking at the cube
	// rig on a machine where the Mannequin IS wanted.
	if (FParse::Param(FCommandLine::Get(), TEXT("TraceNoCharacterArt"))
		|| FParse::Param(FCommandLine::Get(), TEXT("TraceNoPackHands")))
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("-TraceNoCharacterArt/-TraceNoPackHands: building the procedural cube hands on purpose."));
		return false;
	}

	if (HandsMesh == nullptr)
	{
		// A fresh clone that has not run `git lfs pull` has the .uasset as an LFS pointer stub, so
		// this is the NORMAL first-run state, not an error. Warning, not Error, for the same reason
		// the SMG's miss is a warning: the pawn behind it is fully functional.
		UE_LOG(LogTraceGame, Warning,
			TEXT("Pack hands did not resolve (%s); building the procedural cube hands. Run ")
			TEXT("./Scripts/import-pack.sh, or `git lfs pull` if this is a fresh clone."),
			TraceCharacterAssets::HandsMeshPath);
		return false;
	}

	// THE FOUR IDLES ARE THE MINIMUM. An action clip that failed to import degrades to its loadout's
	// idle (ResolveHandsClip's rule), which is a hand that holds still rather than a hand that
	// vanishes — but with no idle there is nothing to fall back TO, and a skeletal mesh with no
	// animation playing shows the reference pose, which for this rig is the knife hold.
	for (int32 Index = TraceCharacterAssets::HandsClip_IdleKnife;
		Index <= TraceCharacterAssets::HandsClip_IdleCore; ++Index)
	{
		if (!HandsAnims.IsValidIndex(Index) || HandsAnims[Index] == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("Pack hands: idle clip '%s' did not resolve; building the procedural cube hands."),
				TraceCharacterAssets::HandsClipNames[Index]);
			return false;
		}
	}

	const FReferenceSkeleton& RefSkeleton = HandsMesh->GetRefSkeleton();
	const int32 WristIndex = RefSkeleton.FindBoneIndex(TraceCharacterAssets::HandsWeaponBone);
	if (WristIndex == INDEX_NONE)
	{
		// LOUD. Everything about where the guns sit is expressed against this bone, so a rename in a
		// re-export would otherwise show up as weapons quietly parked at the rig origin — which looks
		// like a placement bug in this file rather than like an art change.
		UE_LOG(LogTraceGame, Error,
			TEXT("Pack hands: SK_TraceHands has no bone '%s' (the hands README names it as the weapon ")
			TEXT("mount). Falling back to the procedural cube hands. Bone count on the imported ")
			TEXT("skeleton: %d."),
			*TraceCharacterAssets::HandsWeaponBone.ToString(), RefSkeleton.GetNum());
		return false;
	}

	HandsPart = NewObject<USkeletalMeshComponent>(this,
		MakeUniqueObjectName(this, USkeletalMeshComponent::StaticClass(), TEXT("ViewModelHands")));
	if (HandsPart == nullptr)
	{
		return false;
	}

	HandsPart->SetMobility(EComponentMobility::Movable);
	HandsPart->SetupAttachment(ViewModelRoot);
	HandsPart->SetSkeletalMeshAsset(HandsMesh);

	// PLACEMENT, AND IT IS ONE LINE OF ARITHMETIC. The fist goes on the grip the two guns already put
	// their grip landmarks on, so the hand closes around a weapon that does not move; the yaw turns
	// the pack's -Y forward onto the rig's +X. Both terms are in TraceCharacterLayout with their
	// measurements.
	const FRotator HandsRotation(0.f, TraceCharacterLayout::HandsYaw, 0.f);
	const FVector HandsLocation = TraceCharacterLayout::HandsGripRig
		- HandsRotation.RotateVector(TraceCharacterLayout::HandsFistLocal * TraceCharacterLayout::HandsScale);

	HandsPart->SetRelativeLocationAndRotation(HandsLocation, HandsRotation);
	HandsPart->SetRelativeScale3D(FVector(TraceCharacterLayout::HandsScale));

	// Contract §7, the same rule every other visual on this actor keeps: the capsule is the ONLY
	// collider. These are 107 rigid bones 50 cm from the eye; a colliding one would be an obstacle
	// welded to the player's face and would let a bullet stop on "the glove".
	HandsPart->SetCollisionProfileName(TEXT("NoCollision"));
	HandsPart->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HandsPart->SetGenerateOverlapEvents(false);
	HandsPart->SetCanEverAffectNavigation(false);
	HandsPart->bReceivesDecals = false;

	// NOBODY ELSE MAY EVER SEE THIS, and no shadow of any kind — the same two flags every viewmodel
	// part carries, for the same reason: there must be no path by which a pair of floating hands
	// appears in another player's frame, not even as a silhouette on the floor.
	HandsPart->SetOnlyOwnerSee(true);
	HandsPart->SetCastShadow(false);
	HandsPart->bCastHiddenShadow = false;

	// The whole point of the rig, and set BEFORE RegisterComponent so the scene proxy is created with
	// it rather than having to be rebuilt. See the depth arithmetic on HandsScale.
	HandsPart->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;

	// ALWAYS, not OnlyTickPoseWhenRendered. This file SETS the pose every frame from real state and
	// then reads the wrist bone back out to place the guns; a pose that skipped a tick would park the
	// weapon on a stale bone. It is one skeletal mesh on the one pawn a human is inside.
	HandsPart->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	HandsPart->bEnableUpdateRateOptimizations = false;

	// NO ANIM BLUEPRINT. Four idles and one action at a time is what single-node mode is for, and an
	// AnimBP would be a second asset to keep in step with the twenty clips this file already names.
	HandsPart->SetAnimationMode(EAnimationMode::AnimationSingleNode);

	HandsPart->SetVisibility(false);   // UpdateViewBlend decides; matches every other rig part
	HandsPart->RegisterComponent();

	// *** STOP DRAWING THE PACK'S OWN FOREARMS. *** The argument, the measurements and the reason the
	// cuffs stay is on HandsHiddenBones. AFTER RegisterComponent because that is what runs InitAnim
	// and sizes BoneVisibilityStates — called before it, HideBoneByName is silently a no-op, which is
	// the failure mode this codebase keeps finding weeks late.
	//
	// A hidden bone is drawn with its parent's matrix scaled by zero, so the box collapses to a point
	// at hand_<side>'s origin rather than to the world origin: no stray triangle anywhere.
	//
	// RESOLVED INDICES ARE LOGGED, not assumed. HideBoneByName's contract on a name it cannot find is
	// to do nothing at all, so a re-export that renames a node would otherwise show up as the wedge
	// silently coming back with no line anywhere saying why.
	{
		FString Hidden;
		for (const FName& BoneName : TraceCharacterAssets::HandsHiddenBones)
		{
			const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
			if (BoneIndex == INDEX_NONE)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("Pack hands: bone '%s' is not on SK_TraceHands, so it cannot be hidden. If the ")
					TEXT("export renamed it, the viewmodel will draw a forearm straight at the lens ")
					TEXT("again — see HandsHiddenBones."), *BoneName.ToString());
				continue;
			}
			HandsPart->HideBoneByName(BoneName, EPhysBodyOp::PBO_None);
			Hidden += FString::Printf(TEXT(" %s(#%d)"), *BoneName.ToString(), BoneIndex);
		}
		UE_LOG(LogTraceGame, Log, TEXT("Pack hands: hidden bones:%s"),
			Hidden.IsEmpty() ? TEXT(" <none>") : *Hidden);
	}

	// *** THE ACTOR MUST TICK AFTER THE POSE. ***
	//
	// UpdateWeaponsFollowHands reads wrist_right off this component and puts the guns there. Component
	// and actor ticks are in the same tick group with no ordering guarantee, so without this the gun
	// would be composed against whichever evaluation happened to have run — and half the time that is
	// last frame's, which at 10 uu of wrist travel through a jump is the gun visibly swimming inside
	// the fist. With the prerequisite, hand and gun are always the same frame's pose.
	AddTickPrerequisiteComponent(HandsPart);

	// *** AND SO MUST THE WEAPON COMPONENT, FOR THE SAME REASON AND ON ITS OWN ACCOUNT. ***
	//
	// [v32 §8] The actor's prerequisite above orders the ACTOR against the pose; it says nothing about
	// this actor's own components, which are separate tick functions in the same group. UpdateKnifeVisuals
	// now poses the blade through GetViewModelWeaponDelta(), i.e. off exactly the socket the line above
	// exists to sequence, so without this the blade would be composed against last frame's evaluation
	// about half the time and swim in the fist on alternate frames — the identical defect, one tick
	// function over. Costs nothing when the pack rig is absent, because this whole function is not
	// reached then.
	if (Weapon != nullptr)
	{
		Weapon->AddTickPrerequisiteComponent(HandsPart);
	}

	// --- The two facts everything else is expressed against ---------------------------------------
	//
	// Both are RIG-space transforms in the mesh's REFERENCE pose. HandsWristRestRig is the base every
	// weapon offset is stored relative to (the standing rule); the left wrist is what
	// GetViewModelOffHand() reports from here on, so UTraceWeaponComponent hangs the knife on a hand
	// that exists instead of on the cube that used to be there.
	const FTransform HandsRelative(HandsRotation, HandsLocation,
		FVector(TraceCharacterLayout::HandsScale));
	HandsWristRestRig = RefPoseComponentSpace(RefSkeleton, WristIndex) * HandsRelative;
	HandsWristDelta = FTransform::Identity;
	HandsOffWristDelta = FTransform::Identity;
	bHandsRigActive = true;

	const int32 OffHandIndex = RefSkeleton.FindBoneIndex(TraceCharacterAssets::HandsOffHandBone);
	if (OffHandIndex != INDEX_NONE)
	{
		HandsOffWristRestRig = RefPoseComponentSpace(RefSkeleton, OffHandIndex) * HandsRelative;

		// The KNIFE's hand, and it deliberately keeps the REFERENCE pose even after the two rests
		// below are re-based. The reference pose IS Idle_Knife's first frame (measured), so for the
		// one rig that hangs off this point the reference pose is already the right pose.
		ViewModelOffHandLocation = HandsOffWristRestRig.GetLocation();
	}

	// --- Do the imported clips still say what this file believes they say? ------------------------
	//
	// EVERY TIMING DECISION ABOVE IS AN ARGUMENT ABOUT A LENGTH — the 1.75x inspect rate, the throw
	// that must not be truncated to 0.55 s, the draw that is allowed to overrun the pullout. All of
	// them are silently wrong the day someone re-exports a clip at a different length, and the symptom
	// would be a hand that looks subtly out of step with a blade: exactly the kind of defect this
	// project keeps finding weeks late. One frame at 60 Hz is the tolerance, because the 120 Hz SMG
	// bake and float rounding both live well inside it.
	{
		FString Drift;
		for (int32 Index = 0; Index < TraceCharacterAssets::HandsClip_Count; ++Index)
		{
			const UAnimSequence* Clip = HandsAnims.IsValidIndex(Index) ? HandsAnims[Index].Get() : nullptr;
			if (Clip == nullptr)
			{
				continue;
			}
			const float Expected = TraceCharacterAssets::HandsClipAuthoredSeconds[Index];
			const float Actual = Clip->GetPlayLength();
			if (FMath::Abs(Actual - Expected) > (1.f / 60.f))
			{
				Drift += FString::Printf(TEXT("  %s %.4fs (expected %.4fs)"),
					TraceCharacterAssets::HandsClipNames[Index], Actual, Expected);
			}
		}
		if (!Drift.IsEmpty())
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("Pack hands: imported clip lengths have drifted from what TraceCharacter.cpp is ")
				TEXT("timed against, so a pairing with the weapon clips may now be wrong:%s"), *Drift);
		}
	}

	// Seed the pose NOW rather than on the first tick. A skeletal mesh with nothing playing shows its
	// reference pose, and this rig's reference pose is the KNIFE hold (measured: the GLB's default
	// node transforms are Idle_Knife's first frame), so a pistol player's first rendered frame would
	// otherwise be a hand shaped for a blade.
	HandsClipIndex = INDEX_NONE;
	HandsClipTime = 0.f;
	HandsAction = EHandsAction::None;
	HandsLoadout = EHandsLoadout::Pistol;
	UpdateHandsAnimation(0.f);

	// *** THE BASE POSE THE WEAPONS ARE MEASURED AGAINST IS Idle_Pistol AT t=0, NOT THE REFERENCE
	//     POSE — AND UNTIL NOW IT WAS THE REFERENCE POSE. ***
	//
	// HandsWristRestRig is the pose in which HandsWristDelta must be IDENTITY, because the weapons'
	// rest transforms were placed for exactly one pose: HandsGripRig is the fist centroid measured in
	// Idle_Pistol at t=0, and RailgunOrigin / SmgOrigin are both derived from it. Captured off the
	// REFERENCE skeleton instead, the base was Idle_Knife's first frame (the paragraph above says so),
	// so the delta was never identity where it was supposed to be and every weapon part was drawn
	// through a constant offset it was never meant to have.
	//
	// MEASURED, off gloved_hands.glb through this file's own transform chain:
	//   wrist_right rig, reference pose (what was stored): (-6.06, -0.43, -0.81)
	//   wrist_right rig, Idle_Pistol t=0 (what is correct): (-6.88, -0.17, -1.52)
	//   the resulting always-on offset between hand and weapon: 6.51 deg / 0.96 uu for the pistol,
	//   7.34 deg / 3.18 uu for the SMG, 6.93 deg / 1.63 uu for the Core, 0.00 for the knife (whose
	//   idle IS the reference pose, which is why this hid for so long). On screen the railgun's grip
	//   moves 17 px and its drawn axis untilts 3.5 deg; ViewModelMuzzle is a child of the gun body, so
	//   the tracer's beam origin was inheriting all of it too.
	//
	// READ THE WAY UpdateWeaponsFollowHands READS IT, off the live pose, rather than re-derived: one
	// expression, one chance to be wrong. TickAnimation then RefreshBoneTransforms because the seed
	// above only moves the play head — the component-space transforms are not evaluated until
	// something asks for them, and a socket read before that would return the reference pose again
	// and silently change nothing.
	if (WristIndex != INDEX_NONE)
	{
		const FTransform RefBaseRight = HandsWristRestRig;
		const FTransform RefBaseLeft = HandsOffWristRestRig;

		HandsPart->TickAnimation(0.f, /*bNeedsValidRootMotion=*/false);
		HandsPart->RefreshBoneTransforms();

		const FTransform Relative = HandsPart->GetRelativeTransform();
		const FTransform SeededRight =
			HandsPart->GetSocketTransform(TraceCharacterAssets::HandsWeaponBone, RTS_Component) * Relative;
		const FTransform SeededLeft = (OffHandIndex != INDEX_NONE)
			? HandsPart->GetSocketTransform(TraceCharacterAssets::HandsOffHandBone, RTS_Component) * Relative
			: RefBaseLeft;

		// AN UNEVALUATED POSE READS AS THE IDENTITY, AND THE IDENTITY WOULD PARK EVERY GUN AT THE RIG
		// ORIGIN. The two poses are the same hand a fraction of a second apart, so they cannot be more
		// than a few uu apart; anything further means the read did not do what this comment claims and
		// the reference-pose value — today's shipped behaviour — is the safe thing to keep.
		constexpr double MaxRebaseTravelUU = 15.0;
		const double MovedRight = FVector::Dist(RefBaseRight.GetLocation(), SeededRight.GetLocation());
		const double MovedLeft = FVector::Dist(RefBaseLeft.GetLocation(), SeededLeft.GetLocation());
		if (MovedRight <= MaxRebaseTravelUU && MovedLeft <= MaxRebaseTravelUU)
		{
			HandsWristRestRig = SeededRight;
			HandsOffWristRestRig = SeededLeft;
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("Pack hands: the Idle_Pistol seed pose read %.2f uu (right) / %.2f uu (left) from ")
				TEXT("the reference pose, which is further than one idle frame can be. Keeping the ")
				TEXT("reference pose as the weapon base; the guns will carry the constant offset the ")
				TEXT("comment above describes."), MovedRight, MovedLeft);
		}

		UE_LOG(LogTraceGame, Log,
			TEXT("Pack hands: weapon base pose re-based onto Idle_Pistol t=0 — 'wrist_right' rig ")
			TEXT("(%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f), 'wrist_left' rig (%.2f, %.2f, %.2f) -> ")
			TEXT("(%.2f, %.2f, %.2f)."),
			RefBaseRight.GetLocation().X, RefBaseRight.GetLocation().Y, RefBaseRight.GetLocation().Z,
			HandsWristRestRig.GetLocation().X, HandsWristRestRig.GetLocation().Y, HandsWristRestRig.GetLocation().Z,
			RefBaseLeft.GetLocation().X, RefBaseLeft.GetLocation().Y, RefBaseLeft.GetLocation().Z,
			HandsOffWristRestRig.GetLocation().X, HandsOffWristRestRig.GetLocation().Y, HandsOffWristRestRig.GetLocation().Z);
	}

	// =============================================================================================
	// *** THE GRIP, EXPRESSED IN wrist_right's OWN SPACE — WHICH IS THE ONLY SPACE A THING ATTACHED
	//     TO THAT BONE CAN BE PLACED IN.  (spec v32 §8) ***
	// =============================================================================================
	//
	// Everything in this file places props by TRANSFORM in RIG space, so "the fist is at HandsGripRig"
	// is all it has ever needed. UTraceKnifeViewSubsystem does the opposite and does it on the pack's
	// own instruction — SK_TraceKnife is ATTACHED to wrist_right, "every weapon is a right-handed
	// one-hand hold" — and an attached component's offset is in the BONE's frame, not the rig's.
	//
	// IT WAS GUESSED, AND THE GUESS IS THE PHOTOGRAPHED DEFECT. TraceKnifeView.cpp carried
	// `WristOffset(7.0, 0, 0)` under the comment "the hand is 19 cm from wrist to fingertip, so ~7 uu
	// down the fingers is the middle of the grip". The MAGNITUDE is very nearly right — the real
	// figure is 6.81 uu — but +X is not the direction the fingers run in on this skeleton, and the
	// pack's own README says so in passing: the finger CURL axis is "local X (negative = toward
	// palm)", i.e. bone-local X is the curl axis, not the length of the hand. Pushing the blade 7 uu
	// along it puts the pivot out through the SIDE of the wrist, which is exactly what
	// Saved/Screenshots/v31integ_47_key3_knife_idle.png shows: a balisong beside the forearm with lit
	// floor between it and the fingers.
	//
	// SO IT IS DERIVED HERE INSTEAD, FROM TWO NUMBERS THAT ARE ALREADY MEASURED, AND FROM NO NEW ONE.
	// HandsWristRestRig is the wrist in rig space in the base pose; HandsGripRig is the closed fist's
	// centroid in rig space in the SAME base pose (that is the whole meaning of HandsFistLocal, and
	// EnsureViewModelBuilt's checkf keeps it equal to the parts table). Asking the first where the
	// second is, in its own coordinates, is the answer by construction:
	//
	//     grip in wrist space = HandsWristRestRig^-1 . HandsGripRig
	//
	// InverseTransformPosition divides by the wrist's scale on the way through, so the result is in
	// the unscaled bone-local units an attached component's relative location is expressed in — it
	// stays correct if HandsScale is ever retuned, which a hand-typed vector would not.
	//
	// CAPTURED ONCE, AT THE BASE POSE, exactly like HandsWristRestRig above and for the same reason:
	// the fist's offset from its own wrist is a property of the HAND, and the pack authors one fist
	// and moves the wrist (measured: 0.4 uu of spread across the pistol, SMG and knife idles — see
	// HandsFistLocal). Re-deriving it per frame would be re-measuring a constant against a moving
	// pose and would make the blade jitter inside the fist.
	HandsGripWristLocal = HandsWristRestRig.InverseTransformPosition(TraceCharacterLayout::HandsGripRig);
	UE_LOG(LogTraceGame, Log,
		TEXT("Pack hands: grip in 'wrist_right' local space = (%.2f, %.2f, %.2f) uu, |%.2f| uu. ")
		TEXT("Anything ATTACHED to that bone (the pack blade) belongs here; anything placed by ")
		TEXT("transform belongs at rig (%.2f, %.2f, %.2f)."),
		HandsGripWristLocal.X, HandsGripWristLocal.Y, HandsGripWristLocal.Z, HandsGripWristLocal.Size(),
		TraceCharacterLayout::HandsGripRig.X, TraceCharacterLayout::HandsGripRig.Y,
		TraceCharacterLayout::HandsGripRig.Z);

	// [SPEC v32 §5] And the glow, which includes writing the idle rest pose — see BuildHandsEmissive
	// for why that cannot wait for a first action, and for the WorldGridMaterial check it does on the
	// way past.
	BuildHandsEmissive();

	UE_LOG(LogTraceGame, Log,
		TEXT("%s built the pack hands (%d bones, scale %.2f, rig loc (%.2f, %.2f, %.2f) yaw %.0f, ")
		TEXT("'%s' rest at rig (%.2f, %.2f, %.2f), %d/%d clips resolved)."),
		*GetName(), RefSkeleton.GetNum(), TraceCharacterLayout::HandsScale,
		HandsLocation.X, HandsLocation.Y, HandsLocation.Z, TraceCharacterLayout::HandsYaw,
		*TraceCharacterAssets::HandsWeaponBone.ToString(),
		HandsWristRestRig.GetLocation().X, HandsWristRestRig.GetLocation().Y, HandsWristRestRig.GetLocation().Z,
		[this]() { int32 N = 0; for (const TObjectPtr<UAnimSequence>& A : HandsAnims) { if (A != nullptr) { ++N; } } return N; }(),
		TraceCharacterAssets::HandsClip_Count);
	return true;
}

#if !UE_BUILD_SHIPPING

// =================================================================================================
// DEMO 29 ITEM 2 — the owner's own first-person arms rig, PRACTICE RANGE ONLY
//
// Verbatim: "I need to test this first person arms rig i created. See if you can animate it to hold
// the knife and guns correctly. The ring fingers bend opposite the rest, so you'll have to invert
// that. Implement this only in the practice range, for testing purposes."
//
// THIS FILE PUTS THE RIG ON SCREEN AND PLAYS THE HOLD ON IT. The rig is drawn at the right size, in
// the right place, in one game mode, WITH the pose for whatever weapon is in the hand. The poses
// themselves are Scripts/pose_hands.py's deliverable — one solved AnimSequence per loadout on
// SK_TraceArms_Skeleton, listed in TraceCharacterAssets::ArmsPosePaths — and the ring-finger
// inversion is fixed in that rig's BIND pose by Scripts/import_hands.py, so there is no per-finger
// special case anywhere in this file.
//
// AND THE POSE BRINGS ITS OWN PLACEMENT WITH IT, WHICH IS THE ONE TRAP IN THIS SECTION. Each pose
// was solved by IK from shoulders pinned at a specific rig-space transform; drawn under any OTHER
// component transform the solved hands land wherever that transform puts them, not on the gun. So
// UpdateOwnerArmsPose writes BOTH — the sequence and the transform it was solved against — and
// BuildOwnerArmsViewModel's derived placement below is what the BIND pose (a T-pose) is drawn at,
// which is now only the degrade path for a missing pose asset.
//
// WHY THE PACK RIG IS STILL BUILT AND STILL TICKED, and this is the decision the whole tranche turns
// on: every weapon in this viewmodel is composed off `wrist_right`, a bone SK_TraceArms does not
// carry, and the two AddTickPrerequisiteComponent(HandsPart) calls above order the actor and the
// weapon component against THAT pose. Swapping the mesh under HandsPart would have moved the guns,
// the muzzle markers, the tracer origin and the balisong's hold basis onto an unposed T-pose and
// broken both prerequisites' meaning in the same edit. So the pack rig keeps running and stops being
// DRAWN, the guns do not move by a micrometre, and the fixture is a second cosmetic component with
// no prerequisite of its own — because nothing reads a bone off it.
//
// (That is also what the fixture needs: the weapons are the TARGET the new rig has to be posed onto.
// A target that moved with the rig would make the posing unfalsifiable.)
// =================================================================================================

void ATraceCharacter::BuildOwnerArmsViewModel(bool bPackHandsBuilt)
{
	// *** THE GATE, AND IT IS ASKED ONCE. *** IsActive() rather than ShouldUseOwnerArmsViewModel():
	// the component is CONSTRUCTED on the range question alone so that Trace.Practice.ArmsRig can be
	// flipped BOTH WAYS inside one session — the owner asked to A/B this against the shipped hands,
	// and a knob that can only be turned off is not an A/B. Which of the two is DRAWN is the knob's
	// business, per frame, in UpdateOwnerArmsRig.
	if (ViewModelRoot == nullptr || !TracePracticeRange::IsActive(GetWorld()))
	{
		return;
	}

	// REFUSED, NOT WORKED AROUND. Everything below is derived from the pack rig's live measurements —
	// the scale yardstick is SK_TraceHands' own hand, the anchor is where its wrist ended up in rig
	// space after the Idle_Pistol re-base. On the procedural cube fallback none of that exists, and a
	// fixture that quietly placed itself somewhere else would be a worse answer than one that says
	// why it is not on screen.
	if (!bPackHandsBuilt || HandsPart == nullptr || HandsMesh == nullptr)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Practice] The owner's arms rig needs the pack hands to measure and anchor against, ")
			TEXT("and this pawn is on the procedural cube fallback. Showing the cubes; run ")
			TEXT("./Scripts/import-pack.sh (or drop -TraceNoPackHands) to see the fixture."));
		return;
	}

	if (ArmsMesh == nullptr)
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Practice] The owner's arms rig did not resolve (%s); the practice range is showing ")
			TEXT("the shipped pack hands. Run ./Scripts/import-hands.sh."),
			TraceCharacterAssets::ArmsMeshPath);
		return;
	}

	const FReferenceSkeleton& ArmsRef = ArmsMesh->GetRefSkeleton();
	const int32 ArmsHandIndex = ArmsRef.FindBoneIndex(TraceCharacterAssets::ArmsHandBone);
	const int32 ArmsElbowIndex = ArmsRef.FindBoneIndex(TraceCharacterAssets::ArmsForearmBone);
	if (ArmsHandIndex == INDEX_NONE || ArmsElbowIndex == INDEX_NONE)
	{
		// LOUD, and it names both bones: the placement is DERIVED from them, so without them there is
		// no honest position to put the rig in. A re-export that renamed them would otherwise show up
		// as a pair of arms at the rig origin, which reads as a placement bug in this file rather
		// than as an art change.
		UE_LOG(LogTraceGame, Error,
			TEXT("[Practice] The owner's arms rig has no '%s' (%d) / '%s' (%d) — the two bones its ")
			TEXT("placement is derived from. Not building it; the range keeps the pack hands. Bone ")
			TEXT("count on the imported skeleton: %d."),
			*TraceCharacterAssets::ArmsHandBone.ToString(), ArmsHandIndex,
			*TraceCharacterAssets::ArmsForearmBone.ToString(), ArmsElbowIndex, ArmsRef.GetNum());
		return;
	}

	const FVector ArmsHandLocal = RefPoseComponentSpace(ArmsRef, ArmsHandIndex).GetLocation();
	const FVector ArmsElbowLocal = RefPoseComponentSpace(ArmsRef, ArmsElbowIndex).GetLocation();

	// --- 1. SCALE: measured off both skeletons, not typed --------------------------------------
	//
	// `hand_right -> index_right_2` is the one length that means the same thing on two differently
	// proportioned hands, and both rigs carry both bones (the pack's copy is load-bearing in
	// TraceKnifeView.cpp's hold basis). Today this comes out at 9.14 / 17.83 = 0.513, which is the
	// measured 1.95x the import report records — but it is DERIVED every run, so a re-export at a
	// different size retunes the fixture instead of quietly doubling it.
	//
	// The fallback is the same number frozen (ArmsFallbackScale), used only if a bone has been
	// renamed, and it is warned about: a fixture at roughly the right size with a line in the log
	// beats a fixture at twice scale filling the frame.
	const FReferenceSkeleton& PackRef = HandsMesh->GetRefSkeleton();
	const int32 PackHandIndex = PackRef.FindBoneIndex(TraceCharacterAssets::ArmsHandBone);
	const int32 PackTipIndex = PackRef.FindBoneIndex(TraceCharacterAssets::ArmsFingerTipBone);
	const int32 ArmsTipIndex = ArmsRef.FindBoneIndex(TraceCharacterAssets::ArmsFingerTipBone);

	double PackSpan = -1.0;
	double ArmsSpan = -1.0;
	if (PackHandIndex != INDEX_NONE && PackTipIndex != INDEX_NONE && ArmsTipIndex != INDEX_NONE)
	{
		PackSpan = FVector::Dist(RefPoseComponentSpace(PackRef, PackHandIndex).GetLocation(),
			RefPoseComponentSpace(PackRef, PackTipIndex).GetLocation());
		ArmsSpan = FVector::Dist(ArmsHandLocal, RefPoseComponentSpace(ArmsRef, ArmsTipIndex).GetLocation());
	}

	float ArmsScale = TraceCharacterAssets::ArmsFallbackScale;
	bool bScaleDerived = false;
	if (PackSpan > 1.0 && ArmsSpan > 1.0)
	{
		// The pack's span is in MESH units; what has to match is what is DRAWN, so it carries the
		// pack rig's own component scale through. HandsScale is 1.0 today and this is therefore a
		// no-op today — which is exactly why it is written down rather than left implicit.
		ArmsScale = static_cast<float>((PackSpan * TraceCharacterLayout::HandsScale) / ArmsSpan);
		bScaleDerived = true;
	}
	else
	{
		UE_LOG(LogTraceGame, Warning,
			TEXT("[Practice] The owner's arms rig could not be measured against the pack hands ")
			TEXT("('%s' -> '%s' missing on one of them); falling back to the frozen scale %.4f. The ")
			TEXT("fixture will be roughly, not exactly, hand sized."),
			*TraceCharacterAssets::ArmsHandBone.ToString(),
			*TraceCharacterAssets::ArmsFingerTipBone.ToString(), ArmsScale);
	}

	// *** SECTIONS 2 AND 3 BUILD THE *BIND POSE'S* PLACEMENT, AND THAT IS NOW THE DEGRADE PATH. ***
	// A solved hold pose carries the placement it was solved against (TraceCharacterAssets::
	// ArmsPoseTranslationRig), and UpdateOwnerArmsPose writes that one whenever a pose resolves. What
	// follows is what the T-posed asset is drawn at when a pose asset is MISSING: it is still
	// derived rather than typed, so a re-export lands somewhere honest instead of at the rig origin,
	// and it is stored in ArmsBindPlacement at the bottom of section 4.

	// --- 2. ROTATION: aim the arm the way a viewmodel arm has to run ----------------------------
	//
	// The rig is T-POSED, so its right arm runs straight out along its own -X. A viewmodel arm has to
	// leave the fist DOWNWARD AND BACKWARD, off the bottom of the frame within a few centimetres —
	// that is the whole argument on HandsArmLengthRightUU, and its conclusion is already a named
	// constant: HandsArmDirectionRight, the direction the procedural sleeve leaves the fist in. So
	// the rig's own hand -> elbow axis is rotated onto that, with FindBetweenNormals' minimal arc.
	//
	// MINIMAL ARC, WHICH LEAVES THE TWIST ABOUT THE ARM UNDECIDED, and on a bind pose that is all it
	// can honestly be: which way the palm faces is a POSE decision, and a pose that has one states it
	// in its own placement. This rotation is a placement for a T-pose and not a claim about the wrist.
	const FVector ArmsAxisLocal = (ArmsElbowLocal - ArmsHandLocal).GetSafeNormal();
	const FVector WantAxisRig = TraceCharacterLayout::HandsArmDirectionRight.GetSafeNormal();
	const FQuat ArmsRotation = (ArmsAxisLocal.IsNearlyZero() || WantAxisRig.IsNearlyZero())
		? FQuat::Identity
		: FQuat::FindBetweenNormals(ArmsAxisLocal, WantAxisRig);

	// --- 3. POSITION: put the owner's hand where the pack's hand already is ----------------------
	//
	// HandsWristRestRig is `wrist_right` in RIG space in the base pose — the point every weapon
	// offset in this file is stored relative to, and (after the Idle_Pistol re-base at the bottom of
	// BuildPackHandsViewModel) the point the fist is actually closed at. Landing `hand_right` on it
	// is the whole placement: no new constant, and the fixture follows automatically if the pack rig
	// is ever re-tuned.
	const FVector ArmsAnchorRig = HandsWristRestRig.GetLocation();
	const FVector ArmsLocation = ArmsAnchorRig - ArmsRotation.RotateVector(ArmsHandLocal * ArmsScale);

	// --- 4. The component ------------------------------------------------------------------------

	ArmsPart = NewObject<USkeletalMeshComponent>(this,
		MakeUniqueObjectName(this, USkeletalMeshComponent::StaticClass(), TEXT("ViewModelOwnerArms")));
	if (ArmsPart == nullptr)
	{
		return;
	}

	ArmsPart->SetMobility(EComponentMobility::Movable);
	ArmsPart->SetupAttachment(ViewModelRoot);
	ArmsPart->SetSkeletalMeshAsset(ArmsMesh);
	ArmsPart->SetRelativeLocationAndRotation(ArmsLocation, ArmsRotation);
	ArmsPart->SetRelativeScale3D(FVector(ArmsScale));

	// KEPT, because UpdateOwnerArmsPose has to be able to come BACK to it: a loadout whose pose asset
	// did not resolve is drawn in the bind pose, and the bind pose only means anything under the
	// placement derived for it. Everything else is drawn at the pose's own placement.
	ArmsBindPlacement = FTransform(ArmsRotation, ArmsLocation, FVector(ArmsScale));

	// Contract §7, the same rule every other viewmodel part keeps: the capsule is the ONLY collider.
	ArmsPart->SetCollisionProfileName(TEXT("NoCollision"));
	ArmsPart->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ArmsPart->SetGenerateOverlapEvents(false);
	ArmsPart->SetCanEverAffectNavigation(false);
	ArmsPart->bReceivesDecals = false;

	// NOBODY ELSE MAY EVER SEE THIS, and no shadow of any kind — a test fixture must not be able to
	// put a pair of floating arms in another player's frame even by accident. (It cannot reach
	// another machine anyway: the range is single-player. Belt and braces, because these two flags
	// are the ones every other part in this file carries and a fixture is not a reason to be the
	// exception.)
	ArmsPart->SetOnlyOwnerSee(true);
	ArmsPart->SetCastShadow(false);
	ArmsPart->bCastHiddenShadow = false;

	// Set BEFORE RegisterComponent so the scene proxy is created with it. Same depth compression the
	// rest of the rig is drawn under; see TraceCharacter.h's viewmodel block.
	ArmsPart->FirstPersonPrimitiveType = EFirstPersonPrimitiveType::FirstPerson;

	// SINGLE-NODE MODE, WHICH IS THE HANDLE UpdateOwnerArmsPose HANGS THE HOLD POSE ON. No anim
	// blueprint: there is one static pose per loadout and a graph to choose between four sequences by
	// a value this class already owns would be four assets and a compile step to say what one switch
	// statement says. Set BEFORE RegisterComponent so InitAnim creates the single-node instance —
	// SetAnimation() is a silent no-op without one, which is exactly how this rig spent its first
	// three days on screen T-posed.
	//
	// NOTHING IS ASSIGNED HERE. The pose depends on the weapon in the hand, and that is settled per
	// frame; EnsureViewModelBuilt calls UpdateOwnerArmsPose at the bottom of its own body so the
	// first DRAWN frame is already holding something.
	ArmsPart->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	ArmsPart->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	ArmsPart->bEnableUpdateRateOptimizations = false;

	// bVisible follows the VIEW BLEND, exactly like every other part (SetViewModelVisible drives it).
	// Read off the pawn rather than written false so the fixture cannot be born hidden on a pawn whose
	// rig is already up — SetViewModelVisible early-outs when the flag has not changed, and a fixture
	// that missed that edge would simply never appear.
	ArmsPart->SetVisibility(bViewModelVisible);

	// *** AND bHiddenInGame IS THE A/B FLAG, WRITTEN ONLY BY UpdateOwnerArmsRig. *** Born hidden and
	// bOwnerArmsShown born false, so the two agree from frame zero and the first update writes both
	// rigs whichever way the knob is set. Nothing else in this codebase writes bHiddenInGame on the
	// hands: UpdateWeaponSelection touches only the two gun lists, and
	// UTraceWeaponComponent::IsViewModelHandPart exists precisely to keep the knife rule off them.
	ArmsPart->SetHiddenInGame(true);
	bOwnerArmsShown = false;

	ArmsPart->RegisterComponent();

	// *** NOT ONE BONE IS HIDDEN, AND THAT IS A RULE RATHER THAN AN OMISSION. *** The pack rig hides
	// `forearm_right` / `forearm_left` because they are childless decoration leaves that fly at the
	// lens. On THIS rig those same two names are the real forearms and the hands' PARENTS — same
	// anatomy, opposite parentage — so HideBoneByName here would collapse each hand to a point. See
	// TraceCharacterAssets::ArmsForearmBone.

	// --- 5. Lighting, borrowed rather than invented ----------------------------------------------
	//
	// The rig wears `shell` (MI_Pack_shell, bound at import), whose base colour is 0.041 with
	// EmissiveColor (0,0,0) — and this arena puts under 6 lux on the inside of a player's face, so
	// with no floor the arms would render as a silhouette and the fixture would prove nothing. The
	// floor is the GLOVE's, to the number: the MIDs are appended to HandsUnlitMIDs, so
	// ApplyHandsGloveFloor and the shipped Trace.Hands.GloveFloor knob drive the arms and the gloves
	// with one write and there is no second brightness to keep in step.
	{
		int32 LitSlots = 0;
		const TArray<FName> SlotNames = ArmsPart->GetMaterialSlotNames();
		for (int32 Index = 0; Index < SlotNames.Num(); ++Index)
		{
			if (SlotNames[Index] != TraceCharacterAssets::PackShellSlot
				&& SlotNames[Index] != TraceCharacterAssets::PackCarbonSlot)
			{
				continue;
			}
			UMaterialInstanceDynamic* const Floor = ArmsPart->CreateDynamicMaterialInstance(Index);
			if (Floor == nullptr)
			{
				continue;
			}

			// Written here as well as registered above, because ApplyHandsGloveFloor only runs again
			// when the knob CHANGES — a slot that was only registered would stay black until somebody
			// typed at the console. HandsGloveFloorApplied is the product BuildHandsEmissive just
			// wrote (knob included); the constant is the floor for the impossible case where the pack
			// rig's own emissive never got built.
			const float Strength = (HandsGloveFloorApplied >= 0.f)
				? HandsGloveFloorApplied
				: TraceCharacterLayout::HandsGloveEmissiveStrength;
			const FLinearColor& Tint = TraceCharacterLayout::ViewModelBodyEmissiveColor;
			Floor->SetVectorParameterValue(TEXT("EmissiveColor"),
				FLinearColor(Tint.R * Strength, Tint.G * Strength, Tint.B * Strength, 1.f));
			Floor->SetScalarParameterValue(TEXT("EmissiveIntensity"), 1.f);

			HandsUnlitMIDs.Add(Floor);
			++LitSlots;
		}

		if (LitSlots == 0)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Practice] The owner's arms rig has no '%s'/'%s' slot to light (%d slots on the ")
				TEXT("mesh), so it will render as a silhouette against this arena. Re-run ")
				TEXT("./Scripts/import-hands.sh, which binds slot `shell` to MI_Pack_shell."),
				*TraceCharacterAssets::PackShellSlot.ToString(),
				*TraceCharacterAssets::PackCarbonSlot.ToString(), SlotNames.Num());
		}
	}

	// EVERY DERIVED NUMBER, ON ONE LINE, because the pose stage's first question is "where did you
	// put it and how big is it" and the second is "did you measure that or type it".
	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] Built the owner's arms rig SK_TraceArms (%d bones) as the practice range's ")
		TEXT("first-person hands. BIND placement (the fallback): scale %.4f (%s: pack hand %.2f uu / ")
		TEXT("arms hand %.2f uu), rig loc (%.2f, %.2f, %.2f), '%s' anchored on the pack's '%s' rest ")
		TEXT("at rig (%.2f, %.2f, %.2f), arm axis (%.3f, %.3f, %.3f) -> (%.3f, %.3f, %.3f). The rig ")
		TEXT("is normally drawn in a SOLVED HOLD POSE at that pose's own placement instead — see the ")
		TEXT("[Practice] arms pose line below. The pack rig is still built, still ticked and still ")
		TEXT("holds every weapon, it is only not drawn. Trace.Practice.ArmsRig 0 puts the pack hands ")
		TEXT("back."),
		ArmsRef.GetNum(), ArmsScale, bScaleDerived ? TEXT("measured") : TEXT("FROZEN FALLBACK"),
		PackSpan, ArmsSpan,
		ArmsLocation.X, ArmsLocation.Y, ArmsLocation.Z,
		*TraceCharacterAssets::ArmsHandBone.ToString(),
		*TraceCharacterAssets::HandsWeaponBone.ToString(),
		ArmsAnchorRig.X, ArmsAnchorRig.Y, ArmsAnchorRig.Z,
		ArmsAxisLocal.X, ArmsAxisLocal.Y, ArmsAxisLocal.Z,
		WantAxisRig.X, WantAxisRig.Y, WantAxisRig.Z);

	// *** WHICH RIG IS DRAWN IS NOT SETTLED HERE, AND THAT IS DELIBERATE. *** EnsureViewModelBuilt
	// calls UpdateOwnerArmsRig at the BOTTOM of its own body, once the parts loop has built the
	// pack's two sleeve tubes — which the fixture also has to put away, and which do not exist yet at
	// this point in the sequence. Settled from here they stayed on screen; that was photographed.
}

void ATraceCharacter::UpdateOwnerArmsRig()
{
	// *** THIS NULL CHECK IS THE ISOLATION, AND IT IS THE WHOLE PER-FRAME COST IN A MATCH. ***
	// ArmsPart is only ever constructed under TracePracticeRange::IsActive(), so in every world whose
	// game mode is not ATracePracticeGameMode this returns here and no cvar is read at all.
	if (ArmsPart == nullptr)
	{
		return;
	}

	const bool bWantArms = TracePracticeRange::ShouldUseOwnerArmsViewModel(GetWorld());
	if (bWantArms == bOwnerArmsShown)
	{
		return;
	}
	bOwnerArmsShown = bWantArms;

	// bHiddenInGame, NOT bVisible. bVisible belongs to SetViewModelVisible and follows the view
	// blend; a component draws only when both agree. Split this way the fixture cannot make the
	// viewmodel appear in third person, and the view blend cannot bring the hidden rig back.
	ArmsPart->SetHiddenInGame(!bWantArms);
	if (HandsPart != nullptr)
	{
		HandsPart->SetHiddenInGame(bWantArms);
	}

	// AND THE PACK'S SLEEVES WITH IT. The two tubes and their lit cuff bands exist for exactly one
	// reason — the pack rig's own forearms are hidden, and two gloves with no arms float
	// (HandsArmDirectionRight). The owner's rig HAS forearms, so leaving them on would draw a
	// procedural sleeve through a real arm. They are the pack presentation, and they go with it.
	for (const TObjectPtr<UStaticMeshComponent>& Part : HandsForearmParts)
	{
		if (Part != nullptr)
		{
			Part->SetHiddenInGame(bWantArms);
		}
	}

	// Display, not Verbose: this is a fixture swapping the thing the player is looking at, in a mode
	// whose whole purpose is looking at it, and "which rig is on screen" is the first question any
	// frame taken here raises. One line per flip, and flips only happen when a human types.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] First-person rig -> %s (Trace.Practice.ArmsRig %d). The pack rig is still "
		     "built and still ticked either way; only what is DRAWN changes."),
		bWantArms ? TEXT("THE OWNER'S ARMS (SK_TraceArms, in its solved hold pose)")
		          : TEXT("the shipped pack hands (SK_TraceHands)"),
		bWantArms ? 1 : 0);
}

// =================================================================================================
// THE HOLD POSE — the half of demo 29 item 2 that makes the fixture worth photographing
// =================================================================================================
//
// WHAT WAS WRONG, AND IT WAS INVISIBLE IN EVERY LOG LINE. The component was put in single-node mode
// and nothing was ever assigned to it, so the rig rendered its BIND POSE: a flat, splayed, open palm
// laid across three different weapons, for pistol, SMG and knife alike. Two stages had each verified
// their own half — the poses were photographed on offline preview meshes whose REFERENCE pose was
// the pose, and the wiring was photographed with the poses admittedly absent — and neither had ever
// asked the only question that matters, which is whether the poses play on the wired rig. They did
// not. The fix is this function; the guard against it happening again is that the pose is now read
// back off the LIVE component by Trace.Hands.Probe rather than asserted from the code that wrote it.
//
// AND THE SECOND HALF OF THE FIX IS THE PLACEMENT, which is the part that is easy to get wrong
// twice. These poses are not additive fingers-only clips: each is a full 51-bone solve with a
// two-bone IK that ran from shoulders pinned by ONE specific component transform. Play such a pose
// under BuildOwnerArmsViewModel's bind placement — which was derived to land the T-POSE's hand on
// the pack wrist — and the solved hand goes wherever that lands it. The pose and its transform are
// one deliverable, and this function writes them together.

void ATraceCharacter::UpdateOwnerArmsPose()
{
	// The same null check, for the same reason and at the same price as UpdateOwnerArmsRig's: in
	// every world that is not the practice range this feature costs one pointer compare per frame.
	if (ArmsPart == nullptr)
	{
		return;
	}

	// *** ONE FACT, TWO RIGS. *** The pose is chosen by HandsLoadout — the value UpdateHandsAnimation
	// settled from the replicated weapon selector EARLIER IN THIS SAME TICK for the pack rig's own
	// clip — and not by a second read of the selector. So the hidden rig that is carrying the gun and
	// the visible rig that is gripping it cannot disagree about which weapon that is, and
	// Trace.Hands.Hold (which forces the loadout) moves both.
	//
	// INDEXED, NOT SWITCHED, and these four asserts are what makes that safe: the pose table is
	// written in EHandsLoadout's own order, and this is the one translation unit where the enum and
	// the table are both visible to say so at compile time.
	static_assert(static_cast<int32>(EHandsLoadout::Knife) == TraceCharacterAssets::ArmsPose_Knife,
		"ArmsPosePaths is indexed by EHandsLoadout and the two have drifted apart (knife).");
	static_assert(static_cast<int32>(EHandsLoadout::Pistol) == TraceCharacterAssets::ArmsPose_Pistol,
		"ArmsPosePaths is indexed by EHandsLoadout and the two have drifted apart (pistol).");
	static_assert(static_cast<int32>(EHandsLoadout::Smg) == TraceCharacterAssets::ArmsPose_Smg,
		"ArmsPosePaths is indexed by EHandsLoadout and the two have drifted apart (smg).");
	static_assert(static_cast<int32>(EHandsLoadout::Core) == TraceCharacterAssets::ArmsPose_Fist,
		"The Core loadout must map to the fist pose; see TraceCharacterAssets::ArmsPosePaths.");

	const int32 DesiredPose = static_cast<int32>(HandsLoadout);
	UAnimSequence* const Pose = ArmsPoses.IsValidIndex(DesiredPose) ? ArmsPoses[DesiredPose].Get() : nullptr;

	if (DesiredPose != ArmsPoseApplied)
	{
		ArmsPoseApplied = DesiredPose;

		// SetAnimation, NOT PlayAnimation, and that is the whole handling of "these are poses, not
		// clips". USkeletalMeshComponent::SetAnimation assigns the asset with looping OFF and playing
		// OFF; the single-node instance then evaluates its one frame every tick and never advances.
		// PlayAnimation would start a 0.033 s sequence free-running at 30 loops a second — harmless
		// on two identical keys, and exactly the kind of thing that stops being harmless the first
		// time somebody solves a two-frame pose. Stop() and the explicit seek say the intent out loud
		// rather than resting on that default.
		ArmsPart->SetAnimation(Pose);
		ArmsPart->Stop();
		ArmsPart->SetPosition(0.f, /*bFireNotifies=*/false);

		if (Pose != nullptr)
		{
			UE_LOG(LogTraceGame, Display,
				TEXT("[Practice] Arms pose -> %s (%.3f s, held at frame 0) at the placement it was ")
				TEXT("solved against: scale %.4f, yaw %.1f, rig (%.2f, %.2f, %.2f). It rides the same ")
				TEXT("HandsWristDelta the weapons do, so the grip cannot drift off the gun."),
				TraceCharacterAssets::ArmsPoseNames[DesiredPose], Pose->GetPlayLength(),
				TraceCharacterAssets::ArmsPoseScale, TraceCharacterAssets::ArmsPoseYawDegrees,
				TraceCharacterAssets::ArmsPoseTranslationRig.X,
				TraceCharacterAssets::ArmsPoseTranslationRig.Y,
				TraceCharacterAssets::ArmsPoseTranslationRig.Z);
		}
		else
		{
			// LOUD AND SPECIFIC, because the symptom — a flat open hand laid across a gun — is the
			// exact symptom this whole function exists to have fixed, and a reader who meets it again
			// must be told which asset is missing rather than left to re-diagnose it.
			UE_LOG(LogTraceGame, Warning,
				TEXT("[Practice] The owner's arms have no hold pose for this loadout (%s did not ")
				TEXT("resolve), so they are drawn FLAT AND OPEN in their bind pose at the derived ")
				TEXT("bind placement. Run ./Scripts/pose-hands.sh."),
				TraceCharacterAssets::ArmsPoseNames[DesiredPose]);
		}
	}

	// --- THE PLACEMENT, EVERY FRAME ---------------------------------------------------------------
	//
	// TWO REASONS IT IS NOT WRITTEN ONCE BESIDE THE SetAnimation ABOVE.
	//
	//   THE POSE OWNS THE TRANSFORM. Which of the two placements is correct is a property of what is
	//   ON the rig, so it is settled from the same `Pose` pointer the sequence came from — there is
	//   no way to leave a solved pose sitting at the bind placement, or the reverse.
	//
	//   AND THE GUN MOVES. Every weapon part in this viewmodel is drawn at `rest x HandsWristDelta`
	//   (UpdateWeaponsFollowHands) — the pack rig's live wrist, which keeps walking with Idle_Pistol
	//   even while that rig is hidden. Measured in the range: 0.22 uu on the pistol, 2.10 uu on the
	//   SMG, 0.97 uu / 6.5 deg on the knife. The poses were solved against the weapons at REST, so
	//   the arms are carried by the SAME delta and the two stay exactly as solved. Left static, the
	//   SMG's hand would slide 2.1 uu off a grip that a finger sits 0.9 uu thick on.
	//
	// The delta rotates about the WRIST rather than the rig origin (see ComputeHandsWristDelta), so
	// this swings the shoulders a few uu and leaves the fist welded where it was posed — which is the
	// right way round for a picture of a hand on a gun.
	static const FTransform PosePlacement(
		FRotator(0.f, TraceCharacterAssets::ArmsPoseYawDegrees, 0.f),
		TraceCharacterAssets::ArmsPoseTranslationRig,
		FVector(TraceCharacterAssets::ArmsPoseScale));

	ArmsPart->SetRelativeTransform((Pose != nullptr ? PosePlacement : ArmsBindPlacement) * HandsWristDelta);
}

USkeletalMeshComponent* ATraceCharacter::GetViewModelArmsMesh() const
{
	return ArmsPart;
}

#endif // !UE_BUILD_SHIPPING

int32 ATraceCharacter::ResolveHandsClip(EHandsLoadout Loadout, EHandsAction Action) const
{
	using namespace TraceCharacterAssets;

	// THE IDLE IS THE FLOOR. Every loadout has one and it is what an impossible pair — shoot with the
	// knife, reload the Core — resolves to, rather than a wrong clip or an empty hand. The pack
	// deliberately baked only the pairs that exist in play, so "no clip" is the normal answer to most
	// of this table and is not an error.
	int32 Idle = HandsClip_IdlePistol;
	switch (Loadout)
	{
	case EHandsLoadout::Knife: Idle = HandsClip_IdleKnife; break;
	case EHandsLoadout::Smg:   Idle = HandsClip_IdleSmg;   break;
	case EHandsLoadout::Core:  Idle = HandsClip_IdleCore;  break;
	default:                   Idle = HandsClip_IdlePistol; break;
	}

	int32 Clip = INDEX_NONE;
	switch (Action)
	{
	case EHandsAction::None:
		Clip = Idle;
		break;

	case EHandsAction::Draw:
		Clip = (Loadout == EHandsLoadout::Knife) ? HandsClip_DrawKnife : INDEX_NONE;
		break;

	case EHandsAction::Stab:
		Clip = (Loadout == EHandsLoadout::Knife) ? HandsClip_StabKnife : INDEX_NONE;
		break;

	case EHandsAction::Inspect:
		Clip = (Loadout == EHandsLoadout::Knife) ? HandsClip_InspectKnife : INDEX_NONE;
		break;

	case EHandsAction::Shoot:
		if (Loadout == EHandsLoadout::Pistol) { Clip = HandsClip_ShootPistol; }
		else if (Loadout == EHandsLoadout::Smg) { Clip = HandsClip_ShootSmg; }
		break;

	case EHandsAction::Reload:
		if (Loadout == EHandsLoadout::Pistol) { Clip = HandsClip_ReloadPistol; }
		else if (Loadout == EHandsLoadout::Smg) { Clip = HandsClip_ReloadSmg; }
		break;

	case EHandsAction::Throw:
		Clip = (Loadout == EHandsLoadout::Core) ? HandsClip_ThrowCore : INDEX_NONE;
		break;

	case EHandsAction::Jump:
		switch (Loadout)
		{
		case EHandsLoadout::Knife: Clip = HandsClip_JumpKnife; break;
		case EHandsLoadout::Smg:   Clip = HandsClip_JumpSmg;   break;
		case EHandsLoadout::Core:  Clip = HandsClip_JumpCore;  break;
		default:                   Clip = HandsClip_JumpPistol; break;
		}
		break;

	case EHandsAction::Walljump:
		switch (Loadout)
		{
		case EHandsLoadout::Knife: Clip = HandsClip_WalljumpKnife; break;
		case EHandsLoadout::Smg:   Clip = HandsClip_WalljumpSmg;   break;
		case EHandsLoadout::Core:  Clip = HandsClip_WalljumpCore;  break;
		default:                   Clip = HandsClip_WalljumpPistol; break;
		}
		break;

	default:
		break;
	}

	// A clip that exists in the table but failed to import degrades to the idle too, so one missing
	// .uasset costs one action rather than the whole rig.
	if (Clip == INDEX_NONE || !HandsAnims.IsValidIndex(Clip) || HandsAnims[Clip] == nullptr)
	{
		Clip = Idle;
	}
	return HandsAnims.IsValidIndex(Clip) && HandsAnims[Clip] != nullptr ? Clip : INDEX_NONE;
}

void ATraceCharacter::UpdateHandsAnimation(float DeltaSeconds)
{
	if (HandsPart == nullptr)
	{
		return;
	}

	const UWorld* World = GetWorld();

	// --- Trace.Hands.Hold, checked FIRST ---------------------------------------------------------
	//
	// For the same reason the railgun's and the SMG's holds are checked first: a pinned pose has no
	// shot, no reload and no jump behind it. And it is not a luxury here — Shoot_{Pistol,Smg} is
	// 0.1667 s, which is ten frames at 60, so there is no way to photograph the trigger-pull frame
	// without it. It also forces the loadout, which is the only way to see Idle_Core at all: carrying
	// the Core is third person and the rig is hidden for the whole of it.
	if (HandsDebugAlpha >= 0.f && World != nullptr && World->GetTimeSeconds() < HandsDebugUntil)
	{
		if (HandsAnims.IsValidIndex(HandsDebugClipIndex) && HandsAnims[HandsDebugClipIndex] != nullptr)
		{
			const UAnimSequence* Clip = HandsAnims[HandsDebugClipIndex];
			const float Held = FMath::Clamp(HandsDebugAlpha, 0.f, 1.f) * Clip->GetPlayLength();

			if (HandsClipIndex != HandsDebugClipIndex)
			{
				HandsClipIndex = HandsDebugClipIndex;
				HandsPart->SetAnimation(HandsAnims[HandsClipIndex]);
				HandsPart->Stop();

				// *** AND THE OFF HAND, HERE TOO, BECAUSE THIS IS THE SECOND CLIP SWITCH. ***
				// A harness that photographs a state real play never has is worse than no harness:
				// the FIRST run of this fix pinned Walljump_Pistol and printed offHand=HIDDEN while
				// the framing block on the same line said the left forearm was at v = -0.76, i.e. in
				// frame — the exact combination the per-clip table exists to make impossible. The
				// live path below carries the identical call; the two must not drift.
				ApplyHandsOffHandVisibility(HandsClipIndex);
			}
			HandsClipTime = Held;
			HandsLoadout = HandsDebugLoadout;
			HandsPart->SetPosition(Held, /*bFireNotifies=*/false);

			// PUBLISHED HERE TOO, because this is a DRAW: a pinned pose is still a pose on screen and
			// the gloves must be lit for the frame that is showing. A held IDLE publishes 0 — the idle
			// is the breath, not a flare — which is the same answer the loop below gives it.
			HandsClipPulseNorm =
				(HandsDebugClipIndex != ResolveHandsClip(HandsDebugLoadout, EHandsAction::None))
					? TraceCharacterLayout::HandsActionFlare(Held / FMath::Max(Clip->GetPlayLength(), KINDA_SMALL_NUMBER))
					: 0.f;
		}
		return;
	}
	if (HandsDebugAlpha >= 0.f)
	{
		HandsDebugAlpha = -1.f;
		HandsDebugClipIndex = INDEX_NONE;
	}

	// --- 1. THE LOADOUT, from real state ----------------------------------------------------------
	//
	// Carrying the Core outranks the weapon selector, because a carrier's hands are ON the Core
	// whatever is holstered. Everything else is the REPLICATED selector — the same value the damage
	// table, the fire rate and the ammo counter read — so the hand shape and the weapon being
	// simulated cannot disagree.
	EHandsLoadout DesiredLoadout = EHandsLoadout::Pistol;
	if (bIsCarrier)
	{
		DesiredLoadout = EHandsLoadout::Core;
	}
	else if (Weapon != nullptr)
	{
		switch (Weapon->GetEquippedWeapon())
		{
		case ETraceEquippedWeapon::Knife: DesiredLoadout = EHandsLoadout::Knife; break;
		case ETraceEquippedWeapon::Smg:   DesiredLoadout = EHandsLoadout::Smg;   break;
		default:                          DesiredLoadout = EHandsLoadout::Pistol; break;
		}
	}

	// --- 2. EDGE-DETECT THE ACTIONS, all of them off state that already exists --------------------
	//
	// Priority, highest first. A NEW event replaces a running clip when it ranks at least as high, so
	// a shot always cuts a jump (the recoil is the more urgent read) and an inspect flourish is
	// interrupted by anything real — which is exactly what §5 requires of it.
	//
	//   5  shoot, stab      the frames a player is actually reading
	//   4  reload, draw, throw
	//   3  jump, wall jump
	//   1  inspect
	auto Rank = [](EHandsAction Action) -> int32
	{
		switch (Action)
		{
		case EHandsAction::Shoot:
		case EHandsAction::Stab:     return 5;
		case EHandsAction::Reload:
		case EHandsAction::Draw:
		case EHandsAction::Throw:    return 4;
		case EHandsAction::Jump:
		case EHandsAction::Walljump: return 3;
		case EHandsAction::Inspect:  return 1;
		default:                     return 0;
		}
	};

	EHandsAction Event = EHandsAction::None;

	const bool bReloading = (Weapon != nullptr) && Weapon->IsReloading();
	const bool bDeploying = (Weapon != nullptr) && Weapon->IsDeploying();
	const bool bInspecting = TraceKnifeView::IsInspecting(this);

	// *** SPEC v32 §7a — THE STAB EDGE, AND IT USED TO BE READ OFF THE WRONG NUMBER. ***
	//
	// WHAT WAS HERE:  (Weapon->GetShootLockoutRemaining() > 0.f), edge-detected against a boolean.
	// WHY IT COULD NEVER FIRE:  a knife swing does not set the shoot lockout. It sets the SWING
	// COOLDOWN. The flag was therefore false on every frame of every swing, EHandsAction::Stab was
	// never raised, ResolveHandsClip was never asked for A_Hands_Stab_Knife, and the blade thrust
	// out of a fist that stayed in its idle for the whole 0.300 s.
	//
	// WHAT IS HERE NOW IS UTraceKnifeViewSubsystem::ChooseClip'S OWN RULE, character for character,
	// off the SAME accessor — so the hand clip and the blade clip start on the same frame off one
	// fact rather than two detectors that agree until one of them is retuned. The cooldown only ever
	// counts DOWN, so a RISE is unambiguously a swing that has just begun; and unlike "is a swing in
	// flight", a rise survives two swings back to back inside one clip length instead of rendering
	// them as one long stab.
	//
	// A THIRD DETECTOR WAS THE OTHER OPTION AND IS THE WRONG SHAPE — §7a says so and it is right.
	// The genuinely better shape, which this pass could not take, is TraceKnifeView publishing the
	// edge ONCE the way it already publishes IsInspecting(), and both readers consuming it; that
	// needs a file this pass does not own and is written into the report instead.
	const float SwingCooldown = TraceMelee::GetSwingCooldownRemaining(this);
	const bool bSwingSeeded = (HandsSwingCooldownLast >= 0.f);
	const bool bStabEdge = bSwingSeeded && (SwingCooldown > HandsSwingCooldownLast + KINDA_SMALL_NUMBER);

	// A WALL JUMP AND A JUMP BOTH ARRIVE THROUGH OnJumped. The counter is what tells them apart, and
	// it is the movement component's own predicted state rather than a second copy of the rule.
	int32 WallJumps = HandsLastWallJumpCount;
	if (const UTraceCharacterMovementComponent* Movement = GetTraceMovement())
	{
		WallJumps = Movement->GetWallJumpsSinceGround();
	}
	const bool bWallJumped = (HandsLastWallJumpCount >= 0) && (WallJumps > HandsLastWallJumpCount);

	if (bHandsShotPending && (DesiredLoadout == EHandsLoadout::Pistol || DesiredLoadout == EHandsLoadout::Smg))
	{
		Event = EHandsAction::Shoot;
	}
	else if (bStabEdge)
	{
		Event = EHandsAction::Stab;
	}
	else if (bReloading && !bHandsWasReloading)
	{
		Event = EHandsAction::Reload;
	}
	else if (bDeploying && !bHandsWasDeploying && DesiredLoadout == EHandsLoadout::Knife)
	{
		Event = EHandsAction::Draw;
	}
	else if (bHandsWasCarrier && !bIsCarrier)
	{
		// THE THROW, caught on the falling edge of the carry. By this frame the loadout has already
		// stopped being Core, so the clip is played against a FORCED Core loadout below — the hands
		// have to finish the throw they started. The camera is still blending back out of third
		// person for the first 0.35 s of it, so what the player sees is the follow-through, which is
		// the right half to see.
		Event = EHandsAction::Throw;
	}
	else if (bWallJumped)
	{
		Event = EHandsAction::Walljump;
	}
	else if (bHandsJumpPending)
	{
		Event = EHandsAction::Jump;
	}
	else if (bInspecting && !bHandsWasInspecting)
	{
		// *** INSPECT IS DRIVEN FROM REAL STATE AFTER ALL, and this is the better answer. ***
		//
		// §5 owns the F bind and the knife's own 3.20 s flourish, and it publishes
		// TraceKnifeView::IsInspecting() as a presentation-only query. Reading it means the hand and
		// the blade start on the SAME FRAME off ONE fact, instead of two files each being told
		// separately and hoping they agree — which is the two-objects-agreeing-about-one-fact failure
		// this codebase logs by name. PlayHandsAction() remains for anything that has no such state
		// to publish. Last in the chain because Inspect is the lowest-ranked action there is: a
		// flourish must never win a race against a shot or a swap.
		Event = EHandsAction::Inspect;
	}

	bHandsShotPending = false;
	bHandsJumpPending = false;
	HandsLastWallJumpCount = WallJumps;
	bHandsWasReloading = bReloading;
	HandsSwingCooldownLast = SwingCooldown;
	bHandsWasDeploying = bDeploying;
	bHandsWasCarrier = bIsCarrier;
	bHandsWasInspecting = bInspecting;

	// --- 3. Settle which clip is playing ----------------------------------------------------------

	// *** THE THROW IS THE ONE ACTION THAT OUTLIVES ITS LOADOUT, and it is why the two lines below are
	// not simply "loadout = desired". *** bIsCarrier is already FALSE by the frame the throw is
	// detected — losing the Core is what the throw IS — so by the ordinary rule the hands would snap
	// to a pistol grip on frame one of a 1.050 s wind-up-and-follow-through. bHandsLoadoutLatched
	// holds the cradle open for exactly the length of that clip, and ANY other event releases it, so
	// a player who throws and immediately shoots gets the recoil on the very next frame rather than
	// waiting out a flourish.
	if (Event != EHandsAction::None && Event != EHandsAction::Throw)
	{
		bHandsLoadoutLatched = false;
	}

	// A loadout change cancels whatever action was running: the clip belongs to the old hand shape and
	// finishing it would be a pistol recoil played by a fist closed on a knife. Settled BEFORE the
	// event is taken, so a swap and its own Draw on the same frame do not cancel each other.
	if (!bHandsLoadoutLatched && DesiredLoadout != HandsLoadout)
	{
		HandsLoadout = DesiredLoadout;
		HandsAction = EHandsAction::None;
		HandsClipTime = 0.f;
	}

	if (Event != EHandsAction::None && Rank(Event) >= Rank(HandsAction))
	{
		HandsAction = Event;
		HandsClipTime = 0.f;
		if (Event == EHandsAction::Throw)
		{
			HandsLoadout = EHandsLoadout::Core;
			bHandsLoadoutLatched = true;
		}
	}

	const int32 DesiredClip = ResolveHandsClip(HandsLoadout, HandsAction);
	if (DesiredClip == INDEX_NONE)
	{
		// Nothing is drawn on this path, so nothing may be left published: a stale flare would weld
		// the gloves at whatever brightness the last drawn frame had. Same argument UpdateRailgunFire
		// makes where it zeroes PistolPulseNorm on its own early-out.
		HandsClipPulseNorm = 0.f;
		return;
	}

	const UAnimSequence* Clip = HandsAnims[DesiredClip];
	const float ClipLength = FMath::Max(Clip->GetPlayLength(), KINDA_SMALL_NUMBER);
	const bool bLooping = (HandsAction == EHandsAction::None);

	if (DesiredClip != HandsClipIndex)
	{
		HandsClipIndex = DesiredClip;
		HandsPart->SetAnimation(HandsAnims[HandsClipIndex]);

		// *** AND WHETHER THE LEFT GLOVE IS IN THIS CLIP AT ALL. *** Settled HERE, on the one frame
		// the whole hand is being re-posed anyway, and nowhere else: a visibility decision taken
		// against a per-frame framing test would pop the moment the idle's breath carried the palm
		// across the threshold. The per-clip table, the sweep behind it and the two families of clip
		// that are still exempt are on TraceCharacterAssets::HandsClipShowsOffHand.
		ApplyHandsOffHandVisibility(HandsClipIndex);

		// Stop(), so nothing advances but this function. The single node instance's own clock adds
		// DeltaTime and THEN evaluates, which is precisely the per-frame-reader failure the spec
		// warns about — on a 0.1667 s shoot clip it means frame 0, the trigger pull, is never drawn.
		HandsPart->Stop();
		if (Event == EHandsAction::None && HandsAction != EHandsAction::None)
		{
			// A clip that changed without an event is a loadout swap under a running action; restart
			// rather than resume at a time that belongs to a different clip's length.
			HandsClipTime = 0.f;
		}
	}

	// --- 4. WHERE IN THE CLIP, and this is the half the spec's warning is about --------------------
	//
	// TWO KINDS OF CLIP, and neither of them is a free-running timer.
	//
	//   READ OFF THE WEAPON. The reload's position is a pure function of UTraceWeaponComponent's own
	//   replicated deadline, every frame. That is what makes the picture unable to lie: a reload that
	//   is cancelled, that arrives late over the network, or that an ability shortened still puts the
	//   left hand exactly where the gun's remaining time says it should be. It is the same
	//   construction UpdateSmgAnimation uses for the magazine, and it also resolves the 0.800 s
	//   authored / 1.300 s gameplay conflict the same way — by stretching, not by holding.
	//
	//   ADVANCED BY THIS FUNCTION, sampled BEFORE it advances. Everything else. HandsClipTime is
	//   written to the component first and incremented afterwards, so the frame that follows a shot
	//   draws t=0.
	float SampleTime = HandsClipTime;
	float PlayRate = 1.f;

	if (HandsAction == EHandsAction::Reload && Weapon != nullptr && Weapon->IsReloading())
	{
		const float Total = FMath::Max(Weapon->GetReloadSeconds(), KINDA_SMALL_NUMBER);
		const float Phase = FMath::Clamp(1.f - (Weapon->GetReloadRemaining() / Total), 0.f, 1.f);
		SampleTime = Phase * ClipLength;
		HandsClipTime = SampleTime;
	}
	else
	{
		switch (HandsAction)
		{
		case EHandsAction::Inspect:
			// *** THE ONE REAL DISCREPANCY IN THE PACK. *** A_Hands_Inspect_Knife is 5.600 s and the
			// knife's own Inspect is 3.200 s, though the README's pairing table calls them
			// frame-for-frame. The knife carries the authoritative four catch beats inside 3.20 s, so
			// the knife is the clock and the hand plays at exactly 1.75x to land on it.
			PlayRate = TraceCharacterLayout::HandsInspectAuthoredSeconds
				/ FMath::Max(TraceCharacterLayout::KnifeInspectAuthoredSeconds, KINDA_SMALL_NUMBER);
			break;

		default:
			// EVERYTHING ELSE AT ITS AUTHORED RATE, and Stab is the one where that is a decision
			// rather than a default. The pack's Stab_Knife is 0.300 s and the gameplay swing lockout
			// (TraceMelee::GetSwingAnimSeconds, 0.32 s shipped) is 20 ms longer, so stretching would
			// have made the hand agree with the LOCKOUT and disagree with the BLADE — and §5 plays
			// A_Knife_Stab at its authored 0.300 s. The spec's instruction is explicit: the hand and
			// weapon clips are authored frame-for-frame, do not re-time them. The blade wins; the
			// 20 ms of lockout left after the thrust lands is not a thing anyone can see.
			//
			// Draw is here for the same family of reasons — see HandsDrawAuthoredSeconds for why the
			// wrist flip is allowed to overrun the pullout instead of being compressed 4x.
			PlayRate = 1.f;
			break;
		}
	}

	SampleTime = bLooping ? FMath::Fmod(SampleTime, ClipLength) : FMath::Clamp(SampleTime, 0.f, ClipLength);
	HandsPart->SetPosition(SampleTime, /*bFireNotifies=*/false);

	// *** THE GLOVES' FLARE FOR THE POSE THAT WAS JUST DRAWN, SETTLED ON THIS LINE. ***
	//
	// Between SetPosition and the advance below, which is the only window in the frame where "the
	// pose on screen" and "the playhead" are the same number. UpdateHandsEmissive runs later in this
	// same Tick and reads the value from here; when it instead re-derived the triangle from
	// HandsClipTime it was reading a playhead that had already moved on, and the glove peaked one
	// frame BEFORE the blade whose streak is driven off ITS sampled playhead. Measured, at a fixed
	// 60 Hz: drawn t=0.0000 s of a stab lit at 0.159, drawn t=0.0333 s lit at 0.476 — every reading
	// the value belonging to t + 1/60.
	//
	// A LOOPING IDLE PUBLISHES 0 rather than a point on the triangle: the idle's own brightness is
	// the stateless breath in UpdateHandsEmissive, and lighting it off a 2.4 s loop's phase would put
	// a slow sawtooth flare under the breath that the FX doc does not ask for.
	HandsClipPulseNorm = bLooping ? 0.f : TraceCharacterLayout::HandsActionFlare(SampleTime / ClipLength);

	// ADVANCED AFTER THE SAMPLE. The reload branch above already wrote its own absolute position, so
	// this only moves the clips that are genuinely time-driven.
	if (DeltaSeconds > 0.f && !(HandsAction == EHandsAction::Reload && Weapon != nullptr && Weapon->IsReloading()))
	{
		HandsClipTime = SampleTime + DeltaSeconds * PlayRate;
	}

	// A one-shot that has run out drops back to the idle. Non-looping clips hold their last frame,
	// and the README's own rule makes that safe: "actions start and end on their hold pose".
	if (!bLooping && HandsClipTime >= ClipLength)
	{
		HandsAction = EHandsAction::None;
		HandsClipTime = 0.f;
		bHandsLoadoutLatched = false;
	}
}

// =================================================================================================
// SPEC v32 §5 — THE GLOVES' EMISSIVE
// =================================================================================================
//
// unreal-fx_README's last section, and it was the last one with no implementation at all: there was
// no HandsCyanMID, no HandsAmberMID, nothing. The rig was on screen wearing whatever brightness the
// imported material instance happened to default to.
//
//     "Idle 0.95-1.15x, rising to 2.7x cyan / 2.1x amber at the peak of any action. Drive it from
//      the same curve as the weapon so hands and weapon pulse together."
//
// THREE SENTENCES, THREE DECISIONS, and each of them is one this codebase has already paid for:
//
//   1. FIND THE SLOTS BY NAME AND COUNT THEM. Not by index, and not by assuming the layout of
//      another mesh. The SMG's `core_amber` is on the magazine ONLY and its `circuit_cyan` is on
//      three components; copying the pistol's "two MIDs off the body" there produced an INDEX_NONE
//      and an ammo cell that silently never lit. See BuildHandsEmissive.
//   2. WRITE THE REST POSE AT BUILD TIME. Same argument BuildSmgViewModel makes for its 1.8x: the
//      imported instance defaults to 1.0 and there is no later moment guaranteed to happen first.
//   3. THE IDLE IS A FUNCTION OF THE CLOCK, NOT AN ACCUMULATOR — ATraceCore::UpdateCoreArtEmissive's
//      reasoning, unchanged.
//
// AND THE FOURTH, WHICH IS THE ONE THE DOC ACTUALLY EMPHASISES: the action spike is the WEAPON'S OWN
// normalised value remapped, never a parallel timer of the same length. See GetHandsActionPulse.

/**
 * *** ONE KNOB FOR HOW MUCH LIGHT THE GLOVE CARRIES OF ITS OWN. ***
 *
 * Multiplies TraceCharacterLayout::HandsGloveEmissiveStrength, which is where the whole argument for
 * the number lives. It is a CVar and not a rebuild because the value it is trading off — "is the fist
 * a lighter mass than the gun inside it, without becoming a lamp" — is a judgement made by LOOKING at
 * a frame, and this module's shipped idiom for exactly that is a live knob (Trace.Core.FxGeometry,
 * Trace.Fx.BeamScale, the heart-light pair).
 *
 * IT REACHES THE GLOVES ONLY. The two forearm tubes are on their own instance (HandsArmMID) at
 * ViewModelBodyEmissiveStrength and are deliberately NOT on this knob; the guns are not on it either,
 * and that separation is the effect being tuned rather than an oversight — see
 * HandsGloveEmissiveStrength.
 *
 * 0 is a legal setting and is the shipped-before-v33 look: the gloves fall back to the same floor the
 * weapons have, which is the state the "no palm anywhere" frames were photographed in. That makes it
 * a working A/B rather than only a brightness dial.
 */
static TAutoConsoleVariable<float> CVarTraceHandsGloveFloor(
	TEXT("Trace.Hands.GloveFloor"), 1.0f,
	TEXT("Spec v33. Multiplies the emissive floor written on the pack gloves' unlit slots (shell, ")
	TEXT("carbon). 1.0 (shipped) is TraceCharacterLayout::HandsGloveEmissiveStrength; 0 drops the ")
	TEXT("gloves back to the weapons' own near-black, which is the A/B the fix was made against. ")
	TEXT("The forearm tubes and the guns are NOT on this knob."),
	ECVF_Default);

/** The clamp, named once so the write and the log cannot disagree about what was asked for. */
static constexpr float TraceHandsGloveFloorMin = 0.f;
static constexpr float TraceHandsGloveFloorMax = 6.f;

void ATraceCharacter::BuildHandsEmissive()
{
	HandsCyanMIDs.Reset();
	HandsAmberMIDs.Reset();
	HandsUnlitMIDs.Reset();
	HandsArmMID = nullptr;
	HandsCuffMID = nullptr;
	HandsCuffAccentId = TraceCharacterRoster::NoneId;
	HandsGloveFloorApplied = -1.f;

	if (HandsPart == nullptr)
	{
		return;
	}

	// *** WALKED, NOT ASSUMED. *** GetMaterialIndex() answers with the FIRST slot of a given name and
	// would hide a second one; the SMG's three cyan slots are the standing proof that "how many
	// carry this name" is a property of the export. So every slot is visited and every match gets its
	// own MID, and the count is logged below so a re-export that renames or merges a slot shows up as
	// a number in the log rather than as a rig that quietly stops pulsing.
	const TArray<FName> SlotNames = HandsPart->GetMaterialSlotNames();

	// *** AND WHILE WE ARE HERE: IS THE SLOT WEARING PACK ART OR THE GREY CHECKERBOARD? ***
	// A v31 verifier found every pack mesh on /Engine/EngineMaterials/WorldGridMaterial because
	// Interchange imported the meshes and the MI_Pack_* instances and bound NEITHER. That was fixed
	// by Scripts/bind_pack_materials.py, but "was fixed once" is not a guarantee — a re-import
	// silently undoes it — so the check is permanent and lives here, where the MID is created.
	FString Grey;

	for (int32 Index = 0; Index < SlotNames.Num(); ++Index)
	{
		const bool bCyan = (SlotNames[Index] == TraceCharacterAssets::RailgunCyanSlot);
		const bool bAmber = (SlotNames[Index] == TraceCharacterAssets::RailgunAmberSlot);

		// *** THE UNLIT SLOTS GET A CONSTANT FLOOR, AND THAT IS WHAT MAKES THE FIST A FIST. ***
		//
		// This branch is the fix for the photographed complaint. `shell` and `carbon` ship with
		// EmissiveColor (0, 0, 0) (Scripts/import_pack.py, MATERIALS) and base colours of 0.041 and
		// 0.0086, and this arena puts under 6 lux on the inside of a player's face — so the palm,
		// the back of the hand and the fingers' bodies rendered as a silhouette, leaving only the
		// glossy `plating` chips and the cyan circuit runs on screen. That is not a hand holding a
		// gun; it is what the verifier photographed as "detached plates alongside it with no palm
		// anywhere", and it is the same defect on the two forearm tubes, which wear slot 0's
		// material by design so that the sleeve and the glove read as one object.
		//
		// THE NUMBER IS NOT A NEW ONE. It is ViewModelBodyMID's own emissive term — the constant the
		// procedural rig has always carried and the reason the procedural rig photographs correctly
		// — restated in this master's parameter names, with the strength folded into the colour
		// because that is the convention the pack import already writes (EmissiveIntensity stays a
		// clean 1.0 = at rest). One value, one place, two rigs.
		//
		// CREATED ON THE COMPONENT, NEVER ON THE ASSET. MI_Pack_shell is also worn by the Core, the
		// knife and both pack weapon meshes; a MID belongs to HandsPart alone, so nothing outside
		// this viewmodel can see the change.
		if (!bCyan && !bAmber)
		{
			const bool bUnlit = (SlotNames[Index] == TraceCharacterAssets::PackShellSlot)
				|| (SlotNames[Index] == TraceCharacterAssets::PackCarbonSlot);
			if (bUnlit)
			{
				if (UMaterialInstanceDynamic* Floor = HandsPart->CreateDynamicMaterialInstance(Index))
				{
					HandsUnlitMIDs.Add(Floor);

					// *** AND THE SLEEVE'S OWN COPY OF `shell`, MADE HERE BECAUSE HERE IS WHERE THE
					//     PARENT IS IN HAND. *** The two forearm tubes wear the same base material as
					//     the glove and a DIFFERENT emissive floor — the argument is on
					//     HandsGloveEmissiveStrength, the storage is on HandsArmMID. Created off the
					//     MID's parent rather than off the MID, so it is a sibling of the glove's
					//     instance and not a copy of whatever the glove happens to be set to.
					if (SlotNames[Index] == TraceCharacterAssets::PackShellSlot && HandsArmMID == nullptr)
					{
						HandsArmMID = UMaterialInstanceDynamic::Create(Floor->Parent.Get(), this);
						if (HandsArmMID != nullptr)
						{
							const FLinearColor& Tint = TraceCharacterLayout::ViewModelBodyEmissiveColor;
							constexpr float Sleeve = TraceCharacterLayout::ViewModelBodyEmissiveStrength;
							HandsArmMID->SetVectorParameterValue(TEXT("EmissiveColor"),
								FLinearColor(Tint.R * Sleeve, Tint.G * Sleeve, Tint.B * Sleeve, 1.f));
							HandsArmMID->SetScalarParameterValue(TEXT("EmissiveIntensity"), 1.f);
						}
					}
				}
			}
			continue;
		}

		const UMaterialInterface* Bound = HandsPart->GetMaterial(Index);
		const FString BoundName = (Bound != nullptr) ? Bound->GetName() : TEXT("NONE");
		if (Bound == nullptr || BoundName.Contains(TEXT("WorldGrid")))
		{
			Grey += FString::Printf(TEXT("  slot %d '%s' = %s"), Index, *SlotNames[Index].ToString(), *BoundName);
		}

		if (UMaterialInstanceDynamic* MID = HandsPart->CreateDynamicMaterialInstance(Index))
		{
			(bCyan ? HandsCyanMIDs : HandsAmberMIDs).Add(MID);
		}
	}

	if (!Grey.IsEmpty())
	{
		// LOUD, and it names the fix. A grey glove is not a subtle defect once you know to look for
		// it, but it is invisible in a log and indistinguishable in a screenshot from "the emissive
		// driver is broken" — which is a completely different investigation.
		UE_LOG(LogTraceGame, Error,
			TEXT("Pack hands: a glowing slot is still wearing the engine's grey developer material, ")
			TEXT("so no EmissiveIntensity written here can be seen. Run ")
			TEXT("`Scripts/bind_pack_materials.py` through the editor's Python:%s"), *Grey);
	}

	// THE FLOOR ITSELF, THROUGH THE ONE WRITER, so the build-time value and the live value can never
	// be two different pieces of arithmetic. See ApplyHandsGloveFloor and CVarTraceHandsGloveFloor.
	ApplyHandsGloveFloor(FMath::Clamp(CVarTraceHandsGloveFloor.GetValueOnGameThread(),
		TraceHandsGloveFloorMin, TraceHandsGloveFloorMax));

	// *** THE REST POSE, NOW. *** Not on the first action: a player who draws the gloves and stands
	// still has no first action, and the imported instance's own EmissiveIntensity default is 1.0
	// against an idle band centred on 1.05 / 1.00. The gap is small and the principle is not — it is
	// the same one BuildSmgViewModel states for its much larger 1.0-vs-1.8 gap, and a rig that is
	// drawn but has not acted must not sit at a brightness no state of the game ever asks for.
	for (const TObjectPtr<UMaterialInstanceDynamic>& MID : HandsCyanMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), TraceCharacterLayout::HandsCyanIdleMid);
		}
	}
	for (const TObjectPtr<UMaterialInstanceDynamic>& MID : HandsAmberMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), TraceCharacterLayout::HandsAmberIdleMid);
		}
	}

	// *** THE CUFF RINGS' OWN INSTANCE — CHARACTER_SHEETS §1's ONE PER-CHARACTER FP ELEMENT. ***
	//
	// A SIBLING OF THE GLOVE'S CIRCUIT, made here for the same reason and in the same way HandsArmMID
	// is made above: here is where the parent is in hand. Created off the MID's PARENT rather than
	// off the MID, so the cuffs are a second instance of MI_Pack_circuit_cyan and not a copy of
	// whatever the glove's circuit happens to be set to on this frame.
	//
	// AND THE PACK'S OWN COLOURS ARE READ OFF THE GLOVE BEFORE ANYTHING IS WRITTEN, so "no character"
	// restores exactly what the export authored instead of a literal in this file that a re-export
	// would silently falsify. This is the same "read it out of the asset rather than guess at it"
	// rule ApplyColorToSkeletalMesh states for M_Mannequin's parameter names.
	if (HandsCyanMIDs.Num() > 0 && HandsCyanMIDs[0] != nullptr)
	{
		HandsCuffCyanEmissive = HandsCyanMIDs[0]->K2_GetVectorParameterValue(TEXT("EmissiveColor"));
		HandsCuffCyanAlbedo = HandsCyanMIDs[0]->K2_GetVectorParameterValue(TEXT("BaseColor"));

		if (UMaterialInterface* const CuffParent = HandsCyanMIDs[0]->Parent.Get())
		{
			HandsCuffMID = UMaterialInstanceDynamic::Create(CuffParent, this);
		}
	}
	if (HandsCuffMID != nullptr)
	{
		// The rest pose, on the same argument the cyan MIDs' own rest pose is written on: a player who
		// draws the gloves and stands still has no first action, and the imported instance's default
		// is 1.0 against an idle band centred on 1.05.
		HandsCuffMID->SetScalarParameterValue(TEXT("EmissiveIntensity"), TraceCharacterLayout::HandsCyanIdleMid);

		// The pawn may already know who it is — the viewmodel is rebuilt on a body change, and a
		// select-screen pick lands before the rig does on a mid-match swap. Asking now costs one
		// roster lookup and means the first frame the cuffs are drawn is already the right colour.
		ApplyHandsCuffAccent(AppliedBodyCharacterId);
	}

	if (HandsCyanMIDs.Num() == 0 || HandsAmberMIDs.Num() == 0)
	{
		// Not fatal — the gloves render, they just will not breathe or flare. Loud, and it names the
		// COUNT rather than saying "failed", because "cyan found, amber missing" is a real and much
		// less obvious failure than finding neither: the knuckle rings would pulse and the palm node
		// would sit dead, which reads as an art bug rather than as a lookup that missed.
		UE_LOG(LogTraceGame, Warning,
			TEXT("Pack hands built, but their glow is incomplete: '%s' MIDs %d, '%s' MIDs %d, across ")
			TEXT("%d material slots (%s)."),
			*TraceCharacterAssets::RailgunCyanSlot.ToString(), HandsCyanMIDs.Num(),
			*TraceCharacterAssets::RailgunAmberSlot.ToString(), HandsAmberMIDs.Num(),
			SlotNames.Num(),
			*FString::JoinBy(SlotNames, TEXT(", "), [](const FName& N) { return N.ToString(); }));
		return;
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("%s built the pack hands' emissive (%d '%s' MIDs, %d '%s' MIDs of %d slots; rest pose ")
		TEXT("written at cyan %.2fx / amber %.2fx; %d unlit slot(s) given the %.4f/%.4f/%.4f ")
		TEXT("constant floor; cuff accent %s)."),
		*GetName(),
		HandsCyanMIDs.Num(), *TraceCharacterAssets::RailgunCyanSlot.ToString(),
		HandsAmberMIDs.Num(), *TraceCharacterAssets::RailgunAmberSlot.ToString(),
		SlotNames.Num(),
		TraceCharacterLayout::HandsCyanIdleMid, TraceCharacterLayout::HandsAmberIdleMid,
		HandsUnlitMIDs.Num(),
		TraceCharacterLayout::ViewModelBodyEmissiveColor.R * HandsGloveFloorApplied,
		TraceCharacterLayout::ViewModelBodyEmissiveColor.G * HandsGloveFloorApplied,
		TraceCharacterLayout::ViewModelBodyEmissiveColor.B * HandsGloveFloorApplied,
		// NAMED, not just counted: "the cuff MID exists" and "the cuff is wearing X's accent" are
		// different claims, and the second is the one CHARACTER_SHEETS §1 is actually about. A run
		// that greps this line can tell an unbuilt instance from a pawn that has not picked yet.
		(HandsCuffMID == nullptr)
			? TEXT("ABSENT - cuffs stay on the glove's own circuit")
			: *TraceCharacterRoster::NameFor(HandsCuffAccentId));
}

void ATraceCharacter::ApplyHandsGloveFloor(float Multiplier)
{
	// THE STRENGTH, NOT THE COLOUR, IS WHAT THE KNOB MOVES. The tint stays
	// ViewModelBodyEmissiveColor — it is the arena's own cool cast and the reason the glove reads as
	// lit by the same light the rest of the viewmodel is lit by rather than as painted a new colour.
	const float Strength = TraceCharacterLayout::HandsGloveEmissiveStrength * Multiplier;

	for (const TObjectPtr<UMaterialInstanceDynamic>& MID : HandsUnlitMIDs)
	{
		if (MID != nullptr)
		{
			// Component-wise rather than `Colour * Strength`, so the alpha channel is left at 1
			// instead of being scaled along with the RGB. Nothing reads it today; a folded alpha is
			// the kind of thing that reads as a bug the day something does.
			const FLinearColor& Tint = TraceCharacterLayout::ViewModelBodyEmissiveColor;
			MID->SetVectorParameterValue(TEXT("EmissiveColor"),
				FLinearColor(Tint.R * Strength, Tint.G * Strength, Tint.B * Strength, 1.f));

			// The pack folds strength into the colour and leaves intensity at a clean 1.0 — the
			// convention Scripts/import_pack.py writes and the one every other MI_Pack_* read here
			// keeps. Written rather than assumed, because the imported instance's default is what
			// this would otherwise inherit.
			MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), 1.f);
		}
	}

	// REMEMBERED AS THE PRODUCT, not as the multiplier, because that is the number the log prints and
	// the number a reader wants to compare against ViewModelBodyEmissiveStrength.
	HandsGloveFloorApplied = Strength;
}

void ATraceCharacter::ApplyHandsCuffAccent(uint8 CharacterId)
{
	if (HandsCuffMID == nullptr)
	{
		return;
	}

	// RECORDED BEFORE THE WORK, exactly as ApplyCharacterBodyMesh records its id before the load: the
	// caller's compare is what makes a per-frame poll free, and it has to be true of the "no row"
	// answer too or an unlisted id would re-derive this colour sixty times a second.
	HandsCuffAccentId = CharacterId;

	FLinearColor Emissive = HandsCuffCyanEmissive;
	FLinearColor Albedo = HandsCuffCyanAlbedo;

	if (const TraceCharacterRoster::FTraceCharacterEntry* Row = TraceCharacterRoster::Find(CharacterId))
	{
		// *** RE-SCALED TO THE PACK'S OWN LUMINANCE, WHICH IS THE WHOLE DIFFERENCE BETWEEN AN IDENTITY
		//     TELL AND A BRIGHTNESS TELL. ***
		//
		// The ten accents are chosen to be distinguishable in HUE (ART_BIBLE §2.3) and are nowhere
		// near equal in value: Lily's ice (0.75, 0.92, 1.00) carries better than twice the Rec.709
		// luminance of Mortimer's slate (0.38, 0.52, 0.85). Written raw, the ring would be a lamp on
		// one character and a smudge on another, and every frame-fraction measurement behind
		// TraceCharacterLayout::HandsArmCuffAlongUU — which is where the band sits, solved against how
		// it READS at the bottom edge — was made at one brightness. So the hue moves and the value
		// does not.
		//
		// Rec.709 rather than a plain average because these are LINEAR colours going to a lit
		// emissive, and the eye's green bias is the thing being held constant.
		auto Luminance = [](const FLinearColor& C)
		{
			return 0.2126f * C.R + 0.7152f * C.G + 0.0722f * C.B;
		};

		const float CyanLuma = Luminance(HandsCuffCyanEmissive);
		const float AccentLuma = Luminance(Row->Accent);
		const float Scale = (AccentLuma > KINDA_SMALL_NUMBER) ? (CyanLuma / AccentLuma) : 1.f;

		Emissive = FLinearColor(Row->Accent.R * Scale, Row->Accent.G * Scale, Row->Accent.B * Scale, 1.f);

		// The albedo is the accent AT ITS AUTHORED VALUE, not rescaled. It is what the ring is when
		// nothing is lighting it, it is two orders of magnitude below the emissive term in this
		// arena, and matching the pack's saturated cyan albedo (0.003, 1.0, 1.0) in hue is all it has
		// to do.
		Albedo = FLinearColor(Row->Accent.R, Row->Accent.G, Row->Accent.B, 1.f);
	}

	// The pack's convention, followed rather than reinvented: strength folded into the colour,
	// EmissiveIntensity left as the clean per-frame multiplier (Scripts/import_pack.py MATERIALS).
	// UpdateHandsEmissive writes that multiplier from the SAME curve as the glove's circuit, so the
	// cuff breathes and flares with the hand it is under and there is no second thing to keep in step.
	HandsCuffMID->SetVectorParameterValue(TEXT("EmissiveColor"), Emissive);
	HandsCuffMID->SetVectorParameterValue(TEXT("BaseColor"), Albedo);
}

void ATraceCharacter::ApplyHandsOffHandVisibility(int32 ClipIndex)
{
	// Nothing to hide on the procedural cube rig — it has no bones and its left hand is deliberately
	// ON the weapon (RailgunLeftHand), which is the composition this whole fix is trying to get back.
	if (HandsPart == nullptr || !bHandsRigActive)
	{
		return;
	}

	// AN UNKNOWN CLIP SHOWS THE HAND. INDEX_NONE is the "no clip resolved" state and a value past the
	// end of the table can only mean the table and the clip list have drifted (the static_assert
	// catches that at compile time, so this is the runtime belt); in both cases drawing a hand the
	// animation may be using beats deleting one it definitely is.
	const bool bTableSaysShow =
		(ClipIndex < 0)
		|| (ClipIndex >= static_cast<int32>(UE_ARRAY_COUNT(TraceCharacterAssets::HandsClipShowsOffHand)))
		|| TraceCharacterAssets::HandsClipShowsOffHand[ClipIndex];

	// *** AND THE OFF HAND IS NEVER HIDDEN WHILE SOMETHING IS HANGING ON IT. *** Under spec v28 §10's
	// dual-wield switch the blade is in the off hand on EVERY loadout and for as long as the pawn is
	// alive (UTraceWeaponComponent's bBladeVisible), resting on GetViewModelOffHand(). Hiding the
	// glove there would leave a knife floating with nothing holding it — a worse frame than the shard
	// this is removing, and one that only appears when somebody flips a switch that is off today.
	const bool bShow = bTableSaysShow || TraceMelee::IsDualWieldEnabled();

	// Already in the state we want. HideBoneByName rebuilds the visibility array and dirties the
	// render state, so it is worth the compare rather than being re-asserted on every clip change.
	if (bShow == !bHandsOffHandHidden)
	{
		return;
	}

	bHandsOffHandHidden = !bShow;

	if (bHandsOffHandHidden)
	{
		HandsPart->HideBoneByName(TraceCharacterAssets::HandsOffHandRootBone, EPhysBodyOp::PBO_None);
	}
	else
	{
		HandsPart->UnHideBoneByName(TraceCharacterAssets::HandsOffHandRootBone);
	}

	// *** forearm_left MUST STILL BE HIDDEN AFTER THIS, AND IT IS. *** The pack's own left forearm box
	// is a child of hand_left and is hidden for good (HandsHiddenBones — it is half of the salmon
	// wedge). Cycling its parent's visibility does not lose that: RebuildVisibilityArray only
	// overwrites states that are not BVS_ExplicitlyHidden, so the child comes back HiddenByParent and
	// then goes back to ExplicitlyHidden rather than to Visible. Stated here because the failure mode
	// if it were ever untrue is the wedge silently returning on the first reload of the match.
}

bool ATraceCharacter::DebugGetHandsOffHandHidden() const
{
	return bHandsOffHandHidden;
}

float ATraceCharacter::GetHandsActionPulse(const TCHAR*& OutSource) const
{
	// NOTHING IS COMPUTED HERE ANY MORE — all three of the numbers below are published by the driver
	// that owns each one, at the point that driver computes it. That is the whole shape of the fix
	// described in part 2, and it is why this function no longer needs the layout constants.

	// --- 1. THE WEAPON'S OWN NUMBER, when a weapon is in the hand ---------------------------------
	//
	// Read, never recomputed. Each gun's driver publishes the identical float its own
	// EmissiveIntensity write is built from, so "hands and weapon pulse together" is a property of
	// there being one variable rather than a property of two timers having been given equal lengths.
	//
	// ZERO ON THE FALLBACK CUBE GUN, and deliberately: a rig with no imported art has no discharge
	// curve to share, so the hand falls through to its own clip below rather than flaring off a
	// number nothing on screen is being driven by.
	float WeaponPulse = 0.f;
	const TCHAR* WeaponSource = nullptr;
	switch (HandsLoadout)
	{
	case EHandsLoadout::Pistol:
		WeaponPulse = PistolPulseNorm;
		WeaponSource = TEXT("railgun discharge curve");
		break;
	case EHandsLoadout::Smg:
		WeaponPulse = SmgPulseNorm;
		WeaponSource = TEXT("smg shot curve");
		break;
	default:
		break;
	}
	WeaponPulse = FMath::Clamp(WeaponPulse, 0.f, 1.f);

	// --- 2. THE HAND CLIP'S OWN PLAYHEAD, for every action that has no weapon curve ----------------
	//
	// *** THIS IS THE ONE DEVIATION IN §5 AND IT IS A DEVIATION THE DOC FORCES. *** "The peak of ANY
	// action" includes the stab, the reload, the throw and both jumps, and three of those happen with
	// no firing weapon in the hand at all. The blade DOES have a curve — TraceKnifeView's StabFlare —
	// but the only accessor that exposes it is documented, in that file, as existing for one harness
	// and nothing else, and reaching into it from here would make a presentation seam into a
	// dependency against its author's stated intent.
	//
	// So the fall-back reads the clip that IS the action — but it READS it, out of
	// HandsClipPulseNorm, and does not re-derive it. That distinction was a real, measured bug and
	// not a style note. This branch used to evaluate the triangle here, out of HandsClipTime, and
	// HandsClipTime IS ALREADY THE NEXT FRAME'S PLAYHEAD by the time anything downstream can read it:
	// UpdateHandsAnimation draws the pose at SampleTime and then advances. UpdateHandsEmissive runs
	// later in the same Tick, so the gloves were lit for a pose that had not been drawn yet — pulse
	// 0.159 against a drawn playhead of 0.0000 s on a 0.300 s stab, one whole frame of lead — while
	// the blade's streak, correctly driven off ITS sampled playhead, was still on the drawn frame.
	// The two peaked one frame apart, which is exactly the disagreement §5 exists to prevent.
	//
	// This is the house rule in spec v32 §8, and it is a per-frame reader of a SHORT quantity that
	// broke it: the whole stab is eighteen frames. The fix is the same one UpdateSmgAnimation already
	// applies to its fire phase — settle the value at the point of the draw and publish it.
	//
	// The shape is UTraceKnifeViewSubsystem::StabFlare's, triangle for triangle, off the SAME
	// StabPeakFraction (see HandsActionFlare). With the hand's Stab_Knife and the blade's
	// A_Knife_Stab both 0.300 s and both played at rate 1.0, the glove and the blade now genuinely do
	// peak on the same frame — which is the outcome §5 is asking for and the one a paired probe line
	// can check, because pulse must equal that triangle at the playhead printed beside it.
	const float ClipPulse = FMath::Clamp(HandsClipPulseNorm, 0.f, 1.f);

	// THE HOTTER OF THE TWO WINS, which resolves the one frame where both have something to say. On
	// a shot the weapon is at 1.0 while the recoil clip is still at 0 (its own triangle starts at
	// zero), so the discharge takes the frame cleanly — exactly the frame a player reads — and the
	// clip carries the tail after the flash has fallen away.
	if (WeaponPulse >= ClipPulse)
	{
		OutSource = (WeaponPulse > KINDA_SMALL_NUMBER && WeaponSource != nullptr)
			? WeaponSource : TEXT("idle");
		return WeaponPulse;
	}

	// The clip's NAME is not repeated here: Trace.Hands.Probe already prints it, on the line above
	// this value, off the component itself. Naming the driver is the job; naming it twice would be a
	// second copy of one fact and, in a Tick path, a string built sixty times a second to say it.
	OutSource = TEXT("the hand clip's own playhead");
	return ClipPulse;
}

void ATraceCharacter::UpdateHandsEmissive()
{
	if (HandsCyanMIDs.Num() == 0 && HandsAmberMIDs.Num() == 0)
	{
		// *** THE PROCEDURAL CUBE PATH, AND IT DEGRADES IN SILENCE. *** The fallback rig is
		// team-coloured engine primitives with no named material slots at all, so there is nothing
		// here to write and nothing has gone wrong. §5 is explicit that this must not warn, and it is
		// right to be: this runs sixty times a second on a machine whose only problem is that
		// `git lfs pull` has not been run, and a per-frame warning would bury the ONE build-time line
		// that actually tells that player what to do. Which rig is live is reported by
		// Trace.Hands.Probe, which is where somebody is actually looking when they ask.
		return;
	}

	// *** THE CONSTANT FLOOR, FOLLOWED RATHER THAN RE-WRITTEN. ***
	//
	// The gloves' body brightness is NOT part of the breath and must not be: a hand that breathed
	// would be a hand made of light rather than a hand lit well enough to see (HandsUnlitMIDs says
	// so). What is live here is only the KNOB — one float compare against what is already on the
	// material, and a write on the frames where somebody has actually moved it. So
	// -dpcvars=Trace.Hands.GloveFloor=0 is a working A/B against the pre-v33 look with no rebuild,
	// and a session that never touches it pays a comparison.
	const float FloorKnob = FMath::Clamp(CVarTraceHandsGloveFloor.GetValueOnGameThread(),
		TraceHandsGloveFloorMin, TraceHandsGloveFloorMax);
	if (!FMath::IsNearlyEqual(TraceCharacterLayout::HandsGloveEmissiveStrength * FloorKnob,
		HandsGloveFloorApplied, KINDA_SMALL_NUMBER))
	{
		ApplyHandsGloveFloor(FloorKnob);
	}

	const UWorld* World = GetWorld();
	const float Now = (World != nullptr) ? static_cast<float>(World->GetTimeSeconds()) : 0.f;

	// --- THE IDLE BREATH: A STATELESS FUNCTION OF AN ABSOLUTE CLOCK -------------------------------
	//
	// sin(t*w) on the world clock, exactly as ATraceCore::UpdateCoreArtEmissive argues for and for
	// the same three reasons: an accumulator DRIFTS (a thousand additions of a float delta is not the
	// elapsed time), DOUBLE-ADVANCES across a hitch or a paused-then-resumed frame, and DESYNCHRONISES
	// between machines, so two players watching the same pair of gloves would see them breathing out
	// of phase. This function remembers nothing, so none of those can happen to it.
	const float Breath = 0.5f + 0.5f * FMath::Sin(Now * TraceCharacterLayout::HandsIdleBreathRadPerSecond);

	const TCHAR* Source = nullptr;
	const float Pulse = FMath::Clamp(GetHandsActionPulse(Source), 0.f, 1.f);
	HandsPulseLast = Pulse;
	HandsPulseSourceLast = Source;

	// The action lifts OUT OF the idle band rather than replacing it, which is what "idle 0.95-1.15x,
	// RISING TO 2.7x" describes: at rest the breath is the whole story, at the peak the breath is
	// invisible under the flare, and in between the two blend with no seam and no discontinuity on
	// the frame an action starts or ends.
	const float Cyan = FMath::Lerp(
		FMath::Lerp(TraceCharacterLayout::HandsCyanIdleLow, TraceCharacterLayout::HandsCyanIdleHigh, Breath),
		TraceCharacterLayout::HandsCyanPeak, Pulse);
	const float Amber = FMath::Lerp(
		FMath::Lerp(TraceCharacterLayout::HandsAmberIdleLow, TraceCharacterLayout::HandsAmberIdleHigh, Breath),
		TraceCharacterLayout::HandsAmberPeak, Pulse);

	// Every matching slot written together — the knuckle rings, the palm channel and the wrist cuff
	// are one circuit in the art and lighting only the first of them would read as a broken glove.
	for (const TObjectPtr<UMaterialInstanceDynamic>& MID : HandsCyanMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), Cyan);
		}
	}

	// *** THE SLEEVE CUFF RINGS: SAME CURVE, DIFFERENT HUE (CHARACTER_SHEETS §1). ***
	//
	// Written from `Cyan` and not from a value of its own, which is the entire design of the accent
	// cuff: it is the glove's circuit in the character's colour, so it must breathe and flare on the
	// same frame the rest of the circuit does. A parallel curve of the same shape would be two
	// objects agreeing about one fact, which is the failure this file logs by name.
	//
	// AND THE HUE IS A POLL WITH A REMEMBERED ANSWER, for the reason UpdateCharacterBodyMesh gives
	// for the body it mirrors: the selection can arrive after this rig was built and can change at
	// the select screen, and there is no event guaranteed to happen after both. Steady state is one
	// integer compare per frame.
	if (HandsCuffMID != nullptr)
	{
		if (HandsCuffAccentId != AppliedBodyCharacterId)
		{
			ApplyHandsCuffAccent(AppliedBodyCharacterId);
		}
		HandsCuffMID->SetScalarParameterValue(TEXT("EmissiveIntensity"), Cyan);
	}
	for (const TObjectPtr<UMaterialInstanceDynamic>& MID : HandsAmberMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), Amber);
		}
	}
}

FTransform ATraceCharacter::ComputeHandsWristDelta(const FName& BoneName, const FTransform& RestRig) const
{
	if (!bHandsRigActive || HandsPart == nullptr)
	{
		return FTransform::Identity;
	}

	// The bone in RIG space: component space carried out through the mesh component's own relative
	// transform, which is where HandsScale / HandsYaw / HandsLocation live. Reading it any other way
	// would mean re-deriving those three, which is the duplicate-constant failure this file logs by
	// name in half a dozen places.
	const FTransform WristNow =
		HandsPart->GetSocketTransform(BoneName, RTS_Component) * HandsPart->GetRelativeTransform();

	// `W0^-1 * W1`, in UE's apply-A-then-B order: take the authored pose out of the base pose's frame
	// and put it back on the wrist as it stands now. A prop multiplied by this rotates about the
	// WRIST, not about the rig origin — which is what "held" means and what a naive rig-space offset
	// gets wrong.
	return RestRig.Inverse() * WristNow;
}

FTransform ATraceCharacter::GetViewModelWeaponDelta() const
{
	return ComputeHandsWristDelta(TraceCharacterAssets::HandsWeaponBone, HandsWristRestRig);
}

FTransform ATraceCharacter::GetViewModelOffHandDelta() const
{
	return ComputeHandsWristDelta(TraceCharacterAssets::HandsOffHandBone, HandsOffWristRestRig);
}

bool ATraceCharacter::GetViewModelGripWristLocal(FVector& OutWristLocal) const
{
	// FALSE, WITH THE OUTPUT UNTOUCHED, WHEN THERE IS NO PACK RIG. The caller's own fallback is then
	// the right answer and it must not be overwritten with a zero that would park a blade on a bone
	// that does not exist. Same contract as GetViewModelOffHand().
	if (!bHandsRigActive || HandsPart == nullptr)
	{
		return false;
	}

	OutWristLocal = HandsGripWristLocal;
	return true;
}

void ATraceCharacter::UpdateWeaponsFollowHands()
{
	if (!bHandsRigActive || HandsPart == nullptr)
	{
		return;
	}

	// wrist_right AS OF THIS FRAME'S POSE, in rig space, as a delta from the reference pose the
	// weapons' rest transforms were captured against. The tick prerequisite in
	// BuildPackHandsViewModel is what guarantees "this frame's" is true rather than nearly true.
	HandsWristDelta = ComputeHandsWristDelta(
		TraceCharacterAssets::HandsWeaponBone, HandsWristRestRig);

	// The off hand gets its OWN delta, and it needs one: the two wrists are not rigid with each other.
	// At Idle_Pistol the left wrist sits at rig (-14.42, -13.64, -8.64) and by the middle of
	// Reload_Pistol it has travelled to (0.85, 3.11, -17.07) while the right wrist has barely moved —
	// so a left forearm carried on the right wrist's delta would be left behind on every reload.
	// Cheap: one more socket read on a component whose pose has already been evaluated this frame.
	HandsOffWristDelta = ComputeHandsWristDelta(
		TraceCharacterAssets::HandsOffHandBone, HandsOffWristRestRig);

	// The forearm tubes and their lit bands: the first HandsForearmRightNum entries belong to the
	// right wrist and the rest to the left, in the order BuildViewModel added them. Same
	// rest-times-delta rule as the weapons, so each arm is welded to its own wrist by construction
	// rather than by a second placement anyone could forget to keep in step.
	for (int32 Index = 0; Index < HandsForearmParts.Num(); ++Index)
	{
		if (HandsForearmParts[Index] != nullptr && HandsForearmRest.IsValidIndex(Index))
		{
			const FTransform& Delta = (Index < HandsForearmRightNum) ? HandsWristDelta : HandsOffWristDelta;
			HandsForearmParts[Index]->SetRelativeTransform(HandsForearmRest[Index] * Delta);
		}
	}

	// EVERY weapon part, not only the ones with an animation. UpdateRailgunFire early-outs when
	// nothing is firing and the twelve procedural cube-gun parts never move at all, so a pass that
	// only touched the animated ones would leave the rest hanging in mid-air while the hand walked
	// away from them. Sixteen transforms on one pawn is nothing; a gun left behind by a jump is not.
	for (int32 Index = 0; Index < PistolWeaponParts.Num(); ++Index)
	{
		if (PistolWeaponParts[Index] != nullptr && PistolWeaponRest.IsValidIndex(Index))
		{
			const FTransform Pose = PistolWeaponRest[Index] * HandsWristDelta;
			PistolWeaponParts[Index]->SetRelativeTransform(Pose);
		}
	}
	for (int32 Index = 0; Index < SmgWeaponParts.Num(); ++Index)
	{
		if (SmgWeaponParts[Index] != nullptr && SmgWeaponRest.IsValidIndex(Index))
		{
			const FTransform Pose = SmgWeaponRest[Index] * HandsWristDelta;
			SmgWeaponParts[Index]->SetRelativeTransform(Pose);
		}
	}
}

void ATraceCharacter::SetViewModelWeaponPose(UStaticMeshComponent* Part,
	const FVector& RigLocation, const FRotator& RigRotation)
{
	if (Part == nullptr)
	{
		return;
	}

	// On the fallback rig HandsWristDelta is identity and this is the same write v30 made. With the
	// pack hands it is the same rig-space pose, carried to wherever the wrist is now.
	const FTransform Pose = FTransform(RigRotation, RigLocation) * HandsWristDelta;
	Part->SetRelativeLocationAndRotation(Pose.GetLocation(), Pose.GetRotation());
}

bool ATraceCharacter::UsesPackHands() const
{
	return HandsPart != nullptr;
}

USkeletalMeshComponent* ATraceCharacter::GetViewModelHandsMesh() const
{
	return HandsPart;
}

FName ATraceCharacter::GetWeaponAttachBoneName()
{
	return TraceCharacterAssets::HandsWeaponBone;
}

void ATraceCharacter::PlayHandsAction(EHandsAction Action)
{
	if (HandsPart == nullptr || Action == EHandsAction::None)
	{
		return;
	}

	// Refused rather than mis-played when the pack did not bake this pair — ResolveHandsClip would
	// silently answer with the idle, and an inspect that quietly did nothing is a better outcome than
	// an inspect that played a jump.
	if (ResolveHandsClip(HandsLoadout, Action) == ResolveHandsClip(HandsLoadout, EHandsAction::None))
	{
		return;
	}

	HandsAction = Action;
	HandsClipTime = 0.f;
}

void ATraceCharacter::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();

	// A LATCH, not a PlayAnimation. The clip's time has to be sampled before it is advanced and that
	// ordering lives in exactly one place; setting the pose from an event handler would put a second
	// writer on the same clock.
	bHandsJumpPending = true;
}

bool ATraceCharacter::DebugGetHandsState(FString& OutClipName, float& OutTimeSeconds,
	float& OutLengthSeconds, FString& OutLoadout) const
{
	OutClipName = TEXT("NONE");
	OutTimeSeconds = -1.f;
	OutLengthSeconds = -1.f;
	OutLoadout = TEXT("-");

	if (HandsPart == nullptr)
	{
		return false;
	}

	switch (HandsLoadout)
	{
	case EHandsLoadout::Knife:  OutLoadout = TEXT("knife");  break;
	case EHandsLoadout::Pistol: OutLoadout = TEXT("pistol"); break;
	case EHandsLoadout::Smg:    OutLoadout = TEXT("smg");    break;
	case EHandsLoadout::Core:   OutLoadout = TEXT("core");   break;
	default: break;
	}

	// READ BACK OFF THE COMPONENT, not off HandsClipTime — the same argument DebugGetRailgunEmissive
	// makes for reading EmissiveIntensity instead of trusting the write. If the single node instance
	// ever starts advancing on its own again, this is what shows it.
	if (const UAnimSingleNodeInstance* Instance = HandsPart->GetSingleNodeInstance())
	{
		OutTimeSeconds = Instance->GetCurrentTime();
		if (const UAnimSequenceBase* Playing = Cast<UAnimSequenceBase>(Instance->GetAnimationAsset()))
		{
			OutClipName = Playing->GetName();
			OutLengthSeconds = Playing->GetPlayLength();
		}
	}
	if (HandsAnims.IsValidIndex(HandsClipIndex))
	{
		OutClipName = TraceCharacterAssets::HandsClipNames[HandsClipIndex];
	}
	return true;
}

bool ATraceCharacter::DebugGetHandsEmissive(float& OutCyan, float& OutAmber,
	int32& OutCyanSlots, int32& OutAmberSlots) const
{
	OutCyan = -1.f;
	OutAmber = -1.f;
	OutCyanSlots = HandsCyanMIDs.Num();
	OutAmberSlots = HandsAmberMIDs.Num();

	// READ BACK OFF THE LIVE MATERIAL, never off the float we last wrote. Writing a parameter name
	// that is not on a material is a silent no-op — the exact failure DebugGetRailgunEmissive exists
	// to catch — and a harness that reported its own intention would pass over precisely that bug.
	bool bAny = false;
	if (HandsCyanMIDs.Num() > 0 && HandsCyanMIDs[0] != nullptr)
	{
		bAny |= HandsCyanMIDs[0]->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutCyan);
	}
	if (HandsAmberMIDs.Num() > 0 && HandsAmberMIDs[0] != nullptr)
	{
		bAny |= HandsAmberMIDs[0]->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutAmber);
	}
	return bAny;
}

const TCHAR* ATraceCharacter::DebugGetHandsPulse(float& OutPulse) const
{
	// THE VALUE THAT WAS USED, not a fresh call to GetHandsActionPulse. Re-deriving it here would
	// make the probe agree with itself by construction — the same argument Trace.Audio.Loudness
	// makes for asking UTraceAudioSubsystem::VolumeFor rather than recomputing master x scale.
	OutPulse = HandsPulseLast;
	return (HandsPulseSourceLast != nullptr) ? HandsPulseSourceLast : TEXT("-");
}

bool ATraceCharacter::DebugGetViewModelFraming(FString& OutLine) const
{
	if (Camera == nullptr || ViewModelRoot == nullptr)
	{
		return false;
	}

	// CAMERA SPACE STRAIGHT OFF THE SCENE GRAPH. A UCameraComponent's own frame is +X out of the
	// lens, +Y right, +Z up — the rig's own axes — so a part's transform relative to the camera IS
	// the camera-space point ViewModelFrameFraction wants, with every live offset (sway, bob, the
	// fire kick, the hand-follow) already folded in by the components themselves. Deriving it from
	// the layout constants instead would only ever describe the rest pose, which is the mistake
	// this function exists to stop being made a third time.
	const FTransform CameraWorld = Camera->GetComponentTransform();

	auto Describe = [&CameraWorld](const TCHAR* Label, const FVector& WorldPoint) -> FString
	{
		const FVector2D Frame = TraceCharacterLayout::ViewModelFrameFraction(
			CameraWorld.InverseTransformPosition(WorldPoint));

		// The verdict, not just the numbers. "v=-1.05" and "below the bottom edge" are the same fact,
		// and only one of them survives being skimmed at two in the morning by the next pass.
		const TCHAR* Verdict = (FMath::Abs(Frame.Y) <= 1.0 && FMath::Abs(Frame.X) <= 1.0)
			? TEXT("in") : TEXT("OFF-FRAME");
		return FString::Printf(TEXT("  %s u=%+.2f v=%+.2f %s"), Label, Frame.X, Frame.Y, Verdict);
	};

	if (HandsPart != nullptr && bHandsRigActive)
	{
		OutLine += Describe(TEXT("wristR"),
			HandsPart->GetSocketLocation(TraceCharacterAssets::HandsWeaponBone));
		OutLine += Describe(TEXT("wristL"),
			HandsPart->GetSocketLocation(TraceCharacterAssets::HandsOffHandBone));

		// *** AND THE TOP OF THE LEFT GLOVE, WHICH IS NOT THE SAME POINT AS ITS WRIST. ***
		//
		// The reload exemption in HandsClipShowsOffHand was argued off the wristL row above and was
		// wrong for precisely that reason. The wrist is the ARM's end of the glove; the palm hangs
		// off it TOWARD the lens, so a wrist sitting on the bottom edge still rasterises its
		// `plating` knuckle caps about a tenth of a half-frame INSIDE the picture — which is the
		// whole margin the decision turns on, and which the wristL row cannot see. That gap is the
		// detached shard in the bottom-left corner of every one-handed frame this project has
		// photographed, so the highest point of the hand is now MEASURED beside the wrist instead of
		// being inferred from it.
		//
		// Two details that a reader will otherwise have to re-derive:
		//   * forearm_left is skipped. It is a child of hand_left, it points back at the lens, and it
		//     is hidden for good (HandsHiddenBones) — including it would make this row report a bone
		//     that is never drawn, which is the same class of error as the census saying "drawn".
		//   * the row keeps reading while the glove is HIDDEN. HideBoneByName only rewrites the
		//     render-side visibility array; the component-space pose is still there to be asked. That
		//     is deliberate: a check that went quiet the moment the fix worked could not show the fix
		//     still working, and "hidden" and "off frame" are two different claims.
		const USkeletalMesh* HandsAsset = HandsPart->GetSkeletalMeshAsset();
		const int32 OffRootIndex = (HandsAsset != nullptr)
			? HandsAsset->GetRefSkeleton().FindBoneIndex(TraceCharacterAssets::HandsOffHandRootBone)
			: INDEX_NONE;

		if (OffRootIndex != INDEX_NONE)
		{
			const FReferenceSkeleton& HandsRefSkeleton = HandsAsset->GetRefSkeleton();

			int32 TopIndex = INDEX_NONE;
			float TopV = TNumericLimits<float>::Lowest();
			for (int32 BoneIndex = OffRootIndex; BoneIndex < HandsRefSkeleton.GetNum(); ++BoneIndex)
			{
				if (BoneIndex != OffRootIndex && !HandsRefSkeleton.BoneIsChildOf(BoneIndex, OffRootIndex))
				{
					continue;
				}

				const FName BoneName = HandsRefSkeleton.GetBoneName(BoneIndex);

				// bBoneHidden, not bHidden: AActor declares bHidden, and a local of that name is
				// error C4458 on MSVC while compiling silently on Apple clang. See
				// Scripts/check-engine-member-shadowing.py, which now knows this name.
				bool bBoneHidden = false;
				for (const FName& Skip : TraceCharacterAssets::HandsHiddenBones)
				{
					bBoneHidden |= (Skip == BoneName);
				}
				if (bBoneHidden)
				{
					continue;
				}

				const FVector2D Frame = TraceCharacterLayout::ViewModelFrameFraction(
					CameraWorld.InverseTransformPosition(HandsPart->GetBoneLocation(BoneName)));
				if (Frame.Y > TopV)
				{
					TopV = Frame.Y;
					TopIndex = BoneIndex;
				}
			}

			if (TopIndex != INDEX_NONE)
			{
				// The bone's NAME is in the label, because "the glove's top is in frame" and "the
				// index knuckle is in frame" are different sentences and only the second one tells
				// the next reader which end of the hand is doing it.
				const FName TopName = HandsRefSkeleton.GetBoneName(TopIndex);
				OutLine += Describe(*FString::Printf(TEXT("handL_top[%s]"), *TopName.ToString()),
					HandsPart->GetBoneLocation(TopName));
			}
		}
	}

	for (const TObjectPtr<UStaticMeshComponent>& Part : ViewModelParts)
	{
		if (Part == nullptr || !Part->IsVisible() || Part->bHiddenInGame)
		{
			continue;
		}

		// The arms and their bands only. The gun is measured by its own probes and by the depth rule,
		// and a line per cube-gun part would bury the four names this is actually about.
		const FString Name = Part->GetName();
		if (Name.StartsWith(TEXT("VMForearm")) || Name.StartsWith(TEXT("VMCuff")))
		{
			OutLine += Describe(*Name, Part->GetComponentLocation());
		}
	}

	return !OutLine.IsEmpty();
}

void ATraceCharacter::DebugHoldHandsClip(EHandsLoadout Loadout, EHandsAction Action,
	float Alpha, float HoldSeconds)
{
	const UWorld* World = GetWorld();
	HandsDebugLoadout = Loadout;
	HandsDebugClipIndex = (Alpha >= 0.f) ? ResolveHandsClip(Loadout, Action) : INDEX_NONE;
	HandsDebugAlpha = (HandsDebugClipIndex != INDEX_NONE) ? Alpha : -1.f;
	HandsDebugUntil = (World != nullptr && HandsDebugAlpha >= 0.f)
		? World->GetTimeSeconds() + FMath::Max(0.f, HoldSeconds) : -1.0;

	// HOW THIS REACHES THE CORE CRADLE AT ALL, since carrying the Core is third person and hides the
	// whole rig: it forces the LOADOUT without faking the carry. The pawn is not a carrier, so
	// UpdateViewBlend keeps it in first person and the rig stays on screen — which is the only way
	// Idle_Core and Throw_Core can be photographed. This nudge is for the case where a hold IS asked
	// for mid-carry; UpdateViewBlend re-asserts visibility every frame from the blend, so it is the
	// first frame only and cannot strand a hidden rig on screen.
	if (HandsDebugAlpha >= 0.f && !bIsCarrier)
	{
		SetViewModelVisible(true);
	}
}

void ATraceCharacter::UpdateRailgunFire(float DeltaSeconds)
{
	if (RailgunBodyPart == nullptr)
	{
		return;
	}

	// Trace.Railgun.Hold pins the pose so a screenshot can catch it. Checked before the early-out
	// below, because a held pose has no shot behind it.
	bool bHeld = false;
	if (RailgunDebugHoldAlpha >= 0.f)
	{
		const UWorld* World = GetWorld();
		if (World != nullptr && World->GetTimeSeconds() < RailgunDebugHoldUntil)
		{
			bHeld = true;
		}
		else
		{
			RailgunDebugHoldAlpha = -1.f;
			RailgunFireElapsed = -1.f;
		}
	}

	if (!bHeld && RailgunFireElapsed < 0.f)
	{
		// [SPEC v32 §5] NOTHING IS PLAYING, AND THAT HAS TO BE SAID OUT LOUD rather than left as
		// whatever the last shot wrote. The gloves read this float every frame; a published value
		// that went stale when the gun stopped firing would leave them welded at the discharge
		// brightness until the next trigger pull.
		PistolPulseNorm = 0.f;
		return;
	}

	if (!bHeld)
	{
		RailgunFireElapsed += DeltaSeconds;
	}

	// Map elapsed real time onto the authored clip, starting at the discharge frame. The charge
	// segment is deliberately skipped: this weapon has no windup to charge through.
	const float Span = TraceRailgunFireCurve::ClipSeconds - TraceRailgunFireCurve::DischargeSeconds;
	const float Alpha = bHeld
		? FMath::Clamp(RailgunDebugHoldAlpha, 0.f, 1.f)
		: ((RailgunFireDuration > KINDA_SMALL_NUMBER)
			? FMath::Clamp(RailgunFireElapsed / RailgunFireDuration, 0.f, 1.f) : 1.f);
	const float ClipTime = TraceRailgunFireCurve::DischargeSeconds + Alpha * Span;

	float Cyan = 1.f;
	float Amber = 1.f;
	TraceRailgunFireCurve::Sample(ClipTime, Cyan, Amber);

	if (RailgunCyanMID != nullptr)
	{
		RailgunCyanMID->SetScalarParameterValue(TEXT("EmissiveIntensity"), Cyan);
	}
	if (RailgunAmberMID != nullptr)
	{
		RailgunAmberMID->SetScalarParameterValue(TEXT("EmissiveIntensity"), Amber);
	}

	// The MECHANICS ride the same authored curve as the glow, normalised to 0..1, so the rails are
	// widest at the brightest frame and shut as the flash dies. Two effects, one curve, no chance of
	// them drifting apart the way two hand-tuned timelines would.
	const float Mechanical = FMath::Clamp(
		(Cyan - 1.f) / FMath::Max(TraceRailgunFireCurve::PeakCyan - 1.f, KINDA_SMALL_NUMBER), 0.f, 1.f);

	// [SPEC v32 §5] AND THE GLOVES RIDE IT TOO — "drive it from the same curve as the weapon so hands
	// and weapon pulse together", taken literally. Published here, at the point it is computed, so
	// GetHandsActionPulse reads THIS float rather than re-deriving one of its own from the same
	// clock: a re-derivation looks identical the day it is written and is the thing that drifts the
	// first time anyone retunes a fire interval or pins a phase with Trace.Railgun.Hold.
	PistolPulseNorm = Mechanical;

	const float S = TraceCharacterLayout::RailgunScale;
	const FVector Recoil(-TraceCharacterLayout::RailgunRecoilBackUU * S * Mechanical, 0.f, 0.f);
	const FRotator RecoilPitch(-TraceCharacterLayout::RailgunRecoilPitchDegrees * Mechanical, 0.f, 0.f);

	// [SPEC v31 §6] The three writes below are UNCHANGED as arithmetic — the same rig-space pose v30
	// computed — and go out through SetViewModelWeaponPose so the gun rides wrist_right when the pack
	// hands are up. With no pack hands the delta is identity and this is the same call it always was.
	SetViewModelWeaponPose(RailgunBodyPart,
		TraceCharacterLayout::RailgunOrigin + Recoil, RecoilPitch);

	const float Throw = TraceCharacterLayout::RailgunRailThrowUU * S * Mechanical;
	const float Cant = TraceCharacterLayout::RailgunRailCantDegrees * Mechanical;
	const FVector HingeOffset(-5.0f, 7.8f, 4.5f);

	SetViewModelWeaponPose(RailgunRailLeftPart,
		TraceCharacterLayout::RailgunOrigin
			+ FVector(HingeOffset.X, -HingeOffset.Y, HingeOffset.Z) * S
			+ Recoil + FVector(0.f, -Throw, 0.f),
		RecoilPitch + FRotator(0.f, -Cant, 0.f));

	SetViewModelWeaponPose(RailgunRailRightPart,
		TraceCharacterLayout::RailgunOrigin + HingeOffset * S
			+ Recoil + FVector(0.f, Throw, 0.f),
		RecoilPitch + FRotator(0.f, Cant, 0.f));

	// Finished: park the state so the next shot restarts cleanly, and so a rig that is never fired
	// again is not doing this arithmetic forever.
	if (Alpha >= 1.f && !bHeld)
	{
		RailgunFireElapsed = -1.f;
	}
}

// -------------------------------------------------------------------------------------------------
// SPEC v30 §2 — WHICH GUN IS ON SCREEN
// -------------------------------------------------------------------------------------------------
//
// *** THE TWO VISIBILITY LAYERS, AND WHY THIS USES THE SECOND ONE. ***
//
// A first-person weapon part is drawn only when BOTH `bVisible` and `!bHiddenInGame` say so, and the
// two flags have separate setters that never touch each other. That is exactly one flag more than
// this project had owners for, and the extra one is what makes a three-state selector safe:
//
//   bVisible        "is the rig on screen at all". TWO writers already: ATraceCharacter::
//                   SetViewModelVisible (the carry blend, the corpse, respawns) and
//                   UTraceWeaponComponent::SetGunViewModelHidden (the knife). The latter deliberately
//                   RE-ASSERTS ITSELF EVERY TICK — that re-assert is the spec v12 §7 fix — and it
//                   sets every non-hand part it can find under ViewModelRoot.
//   bHiddenInGame   "which of the two guns is selected". Written HERE AND NOWHERE ELSE.
//
// Had the selector been written in bVisible it would have been in a fight it could not win: the
// knife's per-tick re-assert shows every gun part whenever the knife is not out, so a pistol hidden
// for the SMG would come back sixty times a second, and which one you saw would depend on component
// tick order. Splitting the layers means the two rules COMPOSE instead of racing — the knife decides
// whether any gun is drawn, this decides which one, and neither has to know the other exists.
//
// It also keeps UTraceWeaponComponent::GetViewModelCensus honest for free: it counts with
// IsVisible(), which already folds in bHiddenInGame, so the SMG-hidden pistol is correctly NOT
// counted as a gun on screen and Trace.Knife.DualWeaponTest keeps measuring what it measures.

void ATraceCharacter::UpdateWeaponSelection()
{
	if (!bViewModelBuilt || ViewModelRoot == nullptr)
	{
		return;
	}

	// THE REPLICATED SELECTOR IS THE SOURCE OF TRUTH, not a local guess and not an input event. It is
	// the same value the damage table, the fire rate and the ammo counter read, so the gun on screen
	// and the gun being simulated cannot disagree — which is the entire complaint spec §2 opens with.
	//
	// *** IT ANSWERS "WHICH GUN", AND DELIBERATELY NOT "ANY GUN AT ALL". *** The `1` state — guns
	// stowed — is UTraceWeaponComponent's rule, enforced by SetGunViewModelHidden and re-asserted
	// every tick, and it works on this rig unchanged because both gun rigs are ordinary children of
	// ViewModelRoot. So the Knife case below moves NOTHING: it records that nothing is drawn and
	// leaves the holstered firearm's flags exactly where they were.
	//
	// THAT RESTRAINT IS NOT TIDINESS, IT IS A MEASURED FIX. The first version of this function also
	// hid both rigs on Knife — harmless-looking, and the same rule enforced twice. Running
	// Trace.Knife.DualWeaponTest with -TraceLegacyKnife then reported
	//     RESULT: *** NOT PROVEN *** — the RED arm did not reproduce the bug
	// because that harness's red arm restores the v12 §7 latch defect in SetGunViewModelHidden and
	// counts the gun parts left on screen beside the knife — and this function was quietly hiding
	// them for it. A second owner for one rule had taken an existing verifier's red arm away. With
	// the Knife case inert the red arm reproduces again, and the guns-stowed state is still exactly as
	// correct as it was before this pass, through the path that is actually tested.
	bool bStowed = false;
	if (Weapon != nullptr)
	{
		switch (Weapon->GetEquippedWeapon())
		{
		case ETraceEquippedWeapon::Knife:
			// SPEC v29 §5 gave this value back its old meaning: guns stowed. The holstered firearm
			// does not change when you put it away, so SelectedFirearm is left alone and comes back
			// unchanged on the next `2` or `3`.
			bStowed = true;
			break;

		case ETraceEquippedWeapon::Smg:
			// THE FALLBACK. No SMG art (a fresh clone without `git lfs pull`, or -TraceNoSmg) means
			// the SMG slot shows the pistol rig rather than an empty pair of hands. A player who can
			// see a gun and shoot it has a playable game; a player holding nothing has a bug report.
			SelectedFirearm = (SmgBodyPart != nullptr) ? EShownGun::Smg : EShownGun::Pistol;
			if (SmgBodyPart == nullptr && !bSmgFallbackLogged)
			{
				bSmgFallbackLogged = true;
				UE_LOG(LogTraceGame, Warning,
					TEXT("%s selected the SMG but has no SMG rig; showing the pistol rig instead. ")
					TEXT("The weapon's damage, fire rate, clip and reload are unaffected — this is a ")
					TEXT("MISSING-ART fallback, not a gameplay change. See the build log above for which ")
					TEXT("mesh was absent."), *GetName());
			}
			break;

		default:
			SelectedFirearm = EShownGun::Pistol;
			break;
		}
	}

	// What is actually on screen, which is what GetShownGun() promises to report: nothing while the
	// guns are stowed, otherwise the firearm this rig is holding.
	ShownGun = bStowed ? EShownGun::None : SelectedFirearm;

	const bool bShowPistol = (SelectedFirearm == EShownGun::Pistol);

	for (const TObjectPtr<UStaticMeshComponent>& Part : PistolWeaponParts)
	{
		if (Part != nullptr)
		{
			Part->SetHiddenInGame(!bShowPistol);
		}
	}
	for (const TObjectPtr<UStaticMeshComponent>& Part : SmgWeaponParts)
	{
		if (Part != nullptr)
		{
			Part->SetHiddenInGame(bShowPistol);
		}
	}
}

USceneComponent* ATraceCharacter::GetActiveMuzzleMarker() const
{
	// [SPEC v30 §5] The beam must leave whichever gun is actually on screen. Asked of what is DRAWN
	// rather than of the selector, so the missing-art fallback — SMG selected, pistol rig up — puts
	// the beam on the barrel the player can see rather than on one that is hidden.
	if (SelectedFirearm == EShownGun::Smg && ViewModelSmgMuzzle != nullptr)
	{
		return ViewModelSmgMuzzle;
	}
	return ViewModelMuzzle;
}

// -------------------------------------------------------------------------------------------------
// SPEC v30 §3 and §4 — the SMG's motion and glow
// -------------------------------------------------------------------------------------------------

void ATraceCharacter::UpdateSmgAnimation(float DeltaSeconds)
{
	if (SmgBodyPart == nullptr)
	{
		return;
	}

	// Trace.Smg.Hold pins a pose so a screenshot can catch it, and it is checked FIRST for the same
	// reason the railgun's is: a held pose has no shot and no reload behind it. The whole fire cycle
	// is 0.100 s — three frames at 30 fps — so without this there is no way to photograph the shot
	// frame at all, and "the walls move" would be a claim rather than a picture.
	bool bHeld = false;
	if (SmgDebugHoldAlpha >= 0.f || SmgDebugHoldReloadAlpha >= 0.f)
	{
		const UWorld* World = GetWorld();
		if (World != nullptr && World->GetTimeSeconds() < SmgDebugHoldUntil)
		{
			bHeld = true;
		}
		else
		{
			SmgDebugHoldAlpha = -1.f;
			SmgDebugHoldReloadAlpha = -1.f;
			SmgFireElapsed = -1.f;
		}
	}

	// --- Where in the 0.100 s fire cycle are we? -------------------------------------------------
	//
	// DRIVEN BY REAL SHOTS. SmgFireElapsed is armed by NotifyWeaponFired, which UTraceWeaponComponent
	// calls at the moment a round is committed. Nothing here free-runs: with the trigger up the phase
	// sits at 1.0, every curve returns zero, and the gun is at rest by construction rather than by a
	// timer happening to be in the right place.
	// *** SAMPLE FIRST, THEN ADVANCE — AND THE ORDER IS THE WHOLE POINT. ***
	//
	// NotifyWeaponFired arms this at 0, which IS the shot frame: full +/-42 mm wall snap, full 4.8x
	// cyan, full recoil. Advancing before sampling meant phase 0 was never once evaluated — the
	// first sample after a shot landed a whole frame in, and on a 0.100 s cycle at 50 fps that is
	// 20% of the way down the decay. Measured before this change: peak cyan 4.058 instead of 4.8,
	// recoil at 55-75% of its authored amplitude, and the wall snap essentially never rendered.
	//
	// It is the same shape as the fire-rate bug in spec v29 §2f: a per-frame reader sampling a
	// quantity that changes faster than the frame does, and losing the part that falls between.
	float FirePhase = 1.f;   // 1.0 == settled
	if (bHeld && SmgDebugHoldAlpha >= 0.f)
	{
		FirePhase = FMath::Clamp(SmgDebugHoldAlpha, 0.f, 1.f);
	}
	else if (SmgFireElapsed >= 0.f)
	{
		FirePhase = (SmgFireDuration > KINDA_SMALL_NUMBER)
			? FMath::Clamp(SmgFireElapsed / SmgFireDuration, 0.f, 1.f) : 1.f;
	}

	// Advanced AFTER the sample above, so the frame that follows a shot draws the shot frame.
	if (!bHeld && SmgFireElapsed >= 0.f)
	{
		SmgFireElapsed += DeltaSeconds;
	}

	// --- Where in the reload are we? -------------------------------------------------------------
	//
	// READ OFF THE WEAPON'S OWN REPLICATED DEADLINE, every frame, rather than started by an event and
	// counted locally. That is what makes the picture unable to lie: a reload that is cancelled, that
	// arrives late over the network, or that runs at a length nobody told this file about still puts
	// the magazine exactly where the gun's remaining time says it should be.
	//
	// AND IT IS WHERE THE 0.8 s / 1.3 s CONFLICT IS RESOLVED. GetReloadSeconds() is the GAMEPLAY
	// number (1.3 s for the SMG, from Config/DefaultGame.ini), so dividing by it time-stretches the
	// authored 0.8 s motion onto it. See the constants block for why stretching beats holding.
	float ReloadPhase = -1.f;   // negative == not reloading, magazine seated
	if (bHeld && SmgDebugHoldReloadAlpha >= 0.f)
	{
		ReloadPhase = FMath::Clamp(SmgDebugHoldReloadAlpha, 0.f, 1.f);
	}
	else if (Weapon != nullptr && Weapon->IsReloading())
	{
		const float Total = FMath::Max(Weapon->GetReloadSeconds(), KINDA_SMALL_NUMBER);
		ReloadPhase = FMath::Clamp(1.f - (Weapon->GetReloadRemaining() / Total), 0.f, 1.f);
	}

	// --- §4: the emissive ------------------------------------------------------------------------
	//
	// circuit_cyan idles at 1.8x and spikes to 4.8x on the shot frame. Written to ALL THREE MIDs
	// together — body and both walls — because the channel light runs down the rails, and lighting
	// only the body would leave the two brightest strips on the weapon dead through every shot.
	//
	// [SPEC v32 §5] THE FALL IS EVALUATED ONCE AND PUBLISHED, and both the gun's own glow below and
	// the GLOVES (GetHandsActionPulse) read that one float. "Drive it from the same curve as the
	// weapon so hands and weapon pulse together" is only structurally true if it is literally the
	// same value; two calls to the same function with the same argument would be true today and
	// would stop being true the moment one of them was retuned. It is written on EVERY frame,
	// including the settled ones where the answer is 0, so it can never go stale.
	SmgPulseNorm = TraceCharacterLayout::SmgFlashFall(FirePhase);

	const float Cyan = TraceCharacterLayout::SmgCyanRest
		+ (TraceCharacterLayout::SmgCyanPeak - TraceCharacterLayout::SmgCyanRest) * SmgPulseNorm;

	for (const TObjectPtr<UMaterialInstanceDynamic>& MID : SmgCyanMIDs)
	{
		if (MID != nullptr)
		{
			MID->SetScalarParameterValue(TEXT("EmissiveIntensity"), Cyan);
		}
	}

	// core_amber is THE AMMO READOUT — 1.4x at a full 40, 0.35x at empty, interpolating. Driven by
	// the clip and not by the fire clip, which is the point: the cell visibly drains as the magazine
	// empties, so a player can read how much they have left off the gun in their hands instead of off
	// a number in the corner. It refills on the frame the reload lands, which is the same frame the
	// magazine seats — one event, two things on screen agreeing about it.
	//
	// The clip is asked for even when the weapon is not the SMG, because a swap can happen at any
	// time and a stale amber value would be a lit cell on a gun that is empty.
	if (SmgAmberMID != nullptr)
	{
		float Fill = 1.f;
		if (Weapon != nullptr)
		{
			const int32 ClipSize = FMath::Max(1, Weapon->GetClipSize());
			Fill = FMath::Clamp(static_cast<float>(Weapon->GetClipAmmo()) / static_cast<float>(ClipSize), 0.f, 1.f);
		}
		SmgAmberMID->SetScalarParameterValue(TEXT("EmissiveIntensity"),
			FMath::Lerp(TraceCharacterLayout::SmgAmberEmpty, TraceCharacterLayout::SmgAmberFull, Fill));
	}

	// --- §3: the motion ---------------------------------------------------------------------------
	//
	// UNITS, ONCE, HERE. Every constant below is in the MESH's centimetres (the kit quotes millimetres
	// and the import is x100), so every one of them is multiplied by SmgScale to reach rig space. A
	// value that modifies a base must be relative to that base: retuning the rig's size must not
	// silently change how far the walls travel across the gun.
	const float S = TraceCharacterLayout::SmgScale;

	// The whole weapon recoils — the root node `railgun_smg` in the kit's table, which is body, walls
	// and magazine together. Applied to all four components as one rigid transform about SmgOrigin,
	// which is exact because all four meshes are baked around that same origin.
	//
	// The pitch sign follows the kit (-0.045 rad) and the railgun's own convention. This is the
	// receiver rocking INSIDE the hands; the muzzle rise a player actually reads comes from
	// ViewModelKick, which pitches the entire rig — hands included — and is not replaced here.
	// The same normalised fall the glow above is built from — read off SmgPulseNorm rather than
	// recomputed, for the reason stated where it is published.
	const float Recoil = SmgPulseNorm;
	const FVector RecoilOffset(-TraceCharacterLayout::SmgRecoilBackUU * S * Recoil, 0.f, 0.f);
	const FRotator RecoilPitch(-TraceCharacterLayout::SmgRecoilPitchDegrees * Recoil, 0.f, 0.f);

	// The walls snap apart on the shot frame and elastic-settle. LEFT IS -Y: the manifest puts
	// SM_RailgunSmg_WallLeft entirely at y = -12.7 .. -4.7 cm, so the sign is read off the geometry
	// rather than assumed from a node name.
	const float Throw = TraceCharacterLayout::SmgWallThrowUU * S
		* TraceCharacterLayout::SmgElasticSettle(FirePhase);

	// The magazine. Straight down in the weapon's own frame; the recoil pitch above then carries it
	// with the rest of the gun, so a reload during a burst does not tear the cell off the well.
	const float MagDrop = (ReloadPhase >= 0.f) ? TraceCharacterLayout::SmgMagDrop(ReloadPhase) : 0.f;

	// [SPEC v31 §6] Same four poses as v30, routed through SetViewModelWeaponPose so they ride
	// wrist_right when the pack hands are up. Identity delta on the fallback rig.
	SetViewModelWeaponPose(SmgBodyPart,
		TraceCharacterLayout::SmgOrigin + RecoilOffset, RecoilPitch);
	SetViewModelWeaponPose(SmgWallLeftPart,
		TraceCharacterLayout::SmgOrigin + RecoilOffset + FVector(0.f, -Throw, 0.f), RecoilPitch);
	SetViewModelWeaponPose(SmgWallRightPart,
		TraceCharacterLayout::SmgOrigin + RecoilOffset + FVector(0.f, Throw, 0.f), RecoilPitch);
	SetViewModelWeaponPose(SmgMagPart,
		TraceCharacterLayout::SmgOrigin + RecoilOffset + FVector(0.f, 0.f, -MagDrop * S), RecoilPitch);

	// Finished: park the state so the next round restarts the cycle cleanly. At 600 RPM the next
	// shot lands on the frame after this, which is exactly what "set it to loop" means.
	if (FirePhase >= 1.f && !bHeld)
	{
		SmgFireElapsed = -1.f;
	}
}

void ATraceCharacter::DebugHoldRailgunPhase(float Alpha, float HoldSeconds)
{
	const UWorld* World = GetWorld();
	RailgunDebugHoldAlpha = Alpha;
	RailgunDebugHoldUntil = (World != nullptr && Alpha >= 0.f)
		? World->GetTimeSeconds() + FMath::Max(0.f, HoldSeconds) : -1.0;
}

void ATraceCharacter::DebugGetRailgunParts(UStaticMeshComponent*& OutBody,
	UStaticMeshComponent*& OutRailLeft, UStaticMeshComponent*& OutRailRight) const
{
	OutBody = RailgunBodyPart;
	OutRailLeft = RailgunRailLeftPart;
	OutRailRight = RailgunRailRightPart;
}

bool ATraceCharacter::DebugGetRailgunEmissive(float& OutCyan, float& OutAmber) const
{
	OutCyan = -1.f;
	OutAmber = -1.f;
	if (RailgunCyanMID == nullptr || RailgunAmberMID == nullptr)
	{
		return false;
	}
	const bool bCyan = RailgunCyanMID->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutCyan);
	const bool bAmber = RailgunAmberMID->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutAmber);
	return bCyan && bAmber;
}

bool ATraceCharacter::UsesRailgunViewModel() const
{
	return RailgunBodyPart != nullptr;
}

// --- The SMG's twins of the four accessors above  (spec v30 §2) ----------------------------------

bool ATraceCharacter::UsesSmgViewModel() const
{
	return SmgBodyPart != nullptr;
}

ATraceCharacter::EShownGun ATraceCharacter::GetShownGun() const
{
	return ShownGun;
}

void ATraceCharacter::DebugHoldSmgPhase(float Alpha, float ReloadAlpha, float HoldSeconds)
{
	const UWorld* World = GetWorld();
	SmgDebugHoldAlpha = Alpha;
	SmgDebugHoldReloadAlpha = ReloadAlpha;
	SmgDebugHoldUntil = (World != nullptr && (Alpha >= 0.f || ReloadAlpha >= 0.f))
		? World->GetTimeSeconds() + FMath::Max(0.f, HoldSeconds) : -1.0;
}

void ATraceCharacter::DebugGetSmgParts(UStaticMeshComponent*& OutBody, UStaticMeshComponent*& OutWallLeft,
	UStaticMeshComponent*& OutWallRight, UStaticMeshComponent*& OutMag) const
{
	OutBody = SmgBodyPart;
	OutWallLeft = SmgWallLeftPart;
	OutWallRight = SmgWallRightPart;
	OutMag = SmgMagPart;
}

bool ATraceCharacter::DebugGetSmgEmissive(float& OutCyan, float& OutAmber) const
{
	OutCyan = -1.f;
	OutAmber = -1.f;
	if (SmgCyanMIDs.Num() == 0 || SmgCyanMIDs[0] == nullptr || SmgAmberMID == nullptr)
	{
		return false;
	}
	const bool bCyan = SmgCyanMIDs[0]->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutCyan);
	const bool bAmber = SmgAmberMID->GetScalarParameterValue(TEXT("EmissiveIntensity"), OutAmber);
	return bCyan && bAmber;
}

bool ATraceCharacter::DebugGetViewModelMuzzleRaw(FVector& OutWorldLocation) const
{
	// [SPEC v30 §5] The gun that is DRAWN, not the pistol's marker unconditionally.
	const USceneComponent* Marker = GetActiveMuzzleMarker();
	if (Marker == nullptr)
	{
		return false;
	}
	OutWorldLocation = Marker->GetComponentLocation();
	return true;
}

bool ATraceCharacter::GetViewModelMuzzleViewPoint(FVector& OutWorldLocation) const
{
	// Nothing drawn, nothing to answer. bViewModelVisible rather than bViewModelBuilt: a carrier in
	// third person and a corpse both still HAVE a rig, they just are not looking at it, and a beam
	// started at a hidden gun would come out of thin air beside the camera.
	// [SPEC v30 §5] GetActiveMuzzleMarker(), not ViewModelMuzzle: the beam has to leave whichever of
	// the two guns is actually being drawn. Everything below is unchanged — the marker is a child of
	// its own gun's body either way, so the same argument about inheriting recoil, sway and bob holds
	// for both of them.
	const USceneComponent* Marker = GetActiveMuzzleMarker();
	if (Marker == nullptr || Camera == nullptr || !bViewModelVisible)
	{
		return false;
	}

	// The camera component, not the player camera manager. The marker hangs off this component, so its
	// position RELATIVE to this transform is exact by construction; asking the manager instead would
	// mix two transforms that agree today (arm length 0, no lag, no modifiers) but need not tomorrow.
	//
	// GetCameraView() is not const and is not purely a getter: it WRITES the component's world rotation
	// when bUsePawnControlRotation is set. It is safe from a const query here only because this camera
	// hangs off a spring arm that has already applied the control rotation, so bUsePawnControlRotation
	// is false (see the constructor). Turn that flag on and this call starts moving the camera from
	// inside a tracer spawn — read the constructor before changing it.
	FMinimalViewInfo POV;
	Camera->GetCameraView(0.f, POV);

	// Not an optimisation — a correctness gate. TransformWorldToFirstPerson() uses FirstPersonFOV and
	// FirstPersonScale unconditionally, and GetCameraView fills them with neutral values (scene FOV,
	// scale 1) when the feature is off; that is a no-op, but saying so explicitly means a future
	// "turn the first-person rendering off" cannot silently start shifting the beam.
	if (!POV.bUseFirstPersonParameters)
	{
		OutWorldLocation = Marker->GetComponentLocation();
		return true;
	}

	// bIgnoreFirstPersonScale=TRUE, and the engine's own comment on the parameter is the argument:
	// "useful for cases where a full size projectile is spawned in front of the first person weapon.
	// By ignoring the first person scale for the spawn location, the spawned full-size projectile will
	// be spawned a bit further away from the camera, but its on-screen size will look correct."
	//
	// The tracer is exactly that — a world-space beam, drawn at world depth, spawned in front of a
	// first-person weapon. The two flags put the start at the SAME PIXEL either way (the morph scales
	// depth and offset together, so the projected point does not move); what changes is how far from
	// the eye the near end of a 20 uu-wide sheath sits. Taking the full squash would park it ~34 uu
	// out, where that sheath subtends a third of the frame and the muzzle flash becomes the near-field
	// whiteout the class comment in TraceTracer.h was written about. Ignoring the scale leaves it at
	// the gun's true ~85 uu, which is also within a couple of uu of the standoff this replaces — so
	// the beam's THICKNESS at the muzzle is unchanged and only its POSITION moves onto the barrel.
	OutWorldLocation = POV.TransformWorldToFirstPerson(
		Marker->GetComponentLocation(), /*bIgnoreFirstPersonScale=*/true);
	return !OutWorldLocation.ContainsNaN();
}

UMaterialInstanceDynamic* ATraceCharacter::GetViewModelBodyMID() const
{
	return ViewModelBodyMID;
}

UMaterialInstanceDynamic* ATraceCharacter::GetViewModelNeonMID() const
{
	return ViewModelNeonMID;
}

bool ATraceCharacter::GetViewModelOffHand(FVector& OutLocation) const
{
	// [DUALWIELD] Both facts come out of EnsureViewModelBuilt, which is the only writer. Reporting
	// the location even when the hand is NOT free is deliberate: a caller that wants to know where
	// the support hand is (a future two-handed prop, a debug draw) gets a real answer, and the bool
	// is the only thing that says whether the hand is available to hold something.
	OutLocation = ViewModelOffHandLocation;
	return bViewModelBuilt && bViewModelOffHandFree;
}

void ATraceCharacter::SetViewModelVisible(bool bVisible)
{
	if (bViewModelVisible == bVisible)
	{
		return;
	}
	bViewModelVisible = bVisible;

	for (UStaticMeshComponent* Part : ViewModelParts)
	{
		if (Part != nullptr)
		{
			Part->SetVisibility(bVisible);
		}
	}

	// [SPEC v31 §6] The pack hands are not in ViewModelParts — that array is typed to static meshes,
	// and UTraceWeaponComponent's knife rule walks it by name looking for gun parts to hide. Kept out
	// of it deliberately: the hands must NEVER be hidden by the weapon selector, which is the same
	// rule IsViewModelHandPart already encodes for the cube hands. They follow the RIG's visibility
	// and nothing else.
	if (HandsPart != nullptr)
	{
		HandsPart->SetVisibility(bVisible);
	}

#if !UE_BUILD_SHIPPING
	// [DEMO 29 §2] And the practice range's fixture, on the identical terms and for the identical
	// reason: it is a hand rig, so it follows the RIG's visibility and nothing else. Null everywhere
	// but the range. Which of the two hand rigs is DRAWN is bHiddenInGame's job, not this one's —
	// see UpdateOwnerArmsRig for why the two flags are kept apart.
	if (ArmsPart != nullptr)
	{
		ArmsPart->SetVisibility(bVisible);
	}
#endif
}

void ATraceCharacter::UpdateViewModel(float DeltaSeconds)
{
	if (ViewModelRoot == nullptr)
	{
		return;
	}

	// [SPEC v30 §2] WHICH GUN, decided before anything else and OUTSIDE the bAnimate gate below.
	// A rig that is hidden — a Core carrier in third person, a corpse — can still have its selector
	// changed, and settling that here means the correct gun is already on the rig at the moment
	// SetViewModelVisible brings it back, rather than a frame of the wrong one. It is also cheap:
	// SetHiddenInGame is a no-op when the flag already matches.
	UpdateWeaponSelection();

#if !UE_BUILD_SHIPPING
	// [DEMO 29 §2] WHICH HAND RIG, on exactly the same terms as WHICH GUN above and immediately after
	// it: outside the bAnimate gate, because a hidden rig can still have its selector changed and the
	// right one must already be up when SetViewModelVisible brings the viewmodel back.
	//
	// In a match this is one null pointer compare — ArmsPart is only ever constructed inside the
	// practice range. See UpdateOwnerArmsRig.
	UpdateOwnerArmsRig();
#endif

	// A hidden rig still gets its state DECAYED rather than frozen, so a player who takes the Core
	// mid-burst and hands it back does not come out of third person with a stale recoil kick and a
	// bob phase from four seconds ago. It just does not get a transform written.
	const bool bAnimate = bViewModelVisible && DeltaSeconds > 0.f;

	// --- Sway ------------------------------------------------------------------------------------
	//
	// The rig lags a fast turn by a fraction of a degree and springs back. This is the single
	// cheapest thing that makes a viewmodel feel like an object being carried rather than a decal
	// stuck to the lens — and it is free of consequence, because nothing in the shot path reads the
	// rig transform. GetAimDirection() is still built from the control rotation alone.
	const FRotator ControlRotation = GetControlRotation();
	if (bViewModelHasLastRotation && DeltaSeconds > 0.f)
	{
		const FRotator Delta = (ControlRotation - ViewModelLastControlRotation).GetNormalized();

		ViewModelSwayYaw = FMath::Clamp(
			ViewModelSwayYaw - static_cast<float>(Delta.Yaw) * TraceCharacterLayout::SwayPerDegree,
			-TraceCharacterLayout::SwayMaxDegrees, TraceCharacterLayout::SwayMaxDegrees);
		ViewModelSwayPitch = FMath::Clamp(
			ViewModelSwayPitch - static_cast<float>(Delta.Pitch) * TraceCharacterLayout::SwayPerDegree,
			-TraceCharacterLayout::SwayMaxDegrees, TraceCharacterLayout::SwayMaxDegrees);
	}
	ViewModelLastControlRotation = ControlRotation;
	bViewModelHasLastRotation = true;

	ViewModelSwayYaw = FMath::FInterpTo(ViewModelSwayYaw, 0.f, DeltaSeconds, TraceCharacterLayout::SwayRecoverSpeed);
	ViewModelSwayPitch = FMath::FInterpTo(ViewModelSwayPitch, 0.f, DeltaSeconds, TraceCharacterLayout::SwayRecoverSpeed);

	// --- Walk bob --------------------------------------------------------------------------------

	const UCharacterMovementComponent* Movement = GetCharacterMovement();
	const bool bGrounded = (Movement != nullptr) && Movement->IsMovingOnGround();
	const float PlanarSpeed = GetVelocity().Size2D();
	const float MaxSpeed = (Movement != nullptr && Movement->MaxWalkSpeed > 1.f) ? Movement->MaxWalkSpeed : 720.f;
	const float BobTarget = bGrounded ? FMath::Clamp(PlanarSpeed / MaxSpeed, 0.f, 1.f) : 0.f;

	ViewModelBobStrength = FMath::FInterpTo(ViewModelBobStrength, BobTarget, DeltaSeconds, TraceCharacterLayout::BobInterpSpeed);
	ViewModelBobPhase = FMath::Fmod(
		ViewModelBobPhase + DeltaSeconds * TraceCharacterLayout::BobBaseRate * (0.6f + ViewModelBobStrength),
		2.f * PI);

	// --- Recoil and crouch dip -------------------------------------------------------------------

	ViewModelKick = FMath::FInterpTo(ViewModelKick, 0.f, DeltaSeconds, TraceCharacterLayout::KickRecoverSpeed);

	// The gun drops a little further than the eye does during a slide — the hands come down into the
	// lap. CrouchLeanAlpha is the already-eased slide state UpdateCrouchPresentation computed this
	// frame, so the two cannot disagree about when a slide started.
	ViewModelCrouchDip = FMath::FInterpTo(ViewModelCrouchDip, CrouchLeanAlpha, DeltaSeconds,
		TraceCharacterLayout::CrouchLeanInterpSpeed);

	// [SPEC v31 §6] ABOVE THE bAnimate GATE, on purpose, and for the reason the comment on that gate
	// already gives about the recoil kick: a hidden rig must keep its state moving or it comes back
	// wrong. Two of these are not hypothetical. Throw_Core is 1.050 s and STARTS while the rig is
	// hidden — the throw is what takes the camera out of third person — so a rig that only animated
	// while visible would snap into the middle of a follow-through it never began. And the wall-jump
	// edge detector reads a counter that keeps moving while a carrier runs; frozen, it would fire one
	// spurious wall jump on the frame the hands came back.
	UpdateHandsAnimation(DeltaSeconds);
	UpdateWeaponsFollowHands();

#if !UE_BUILD_SHIPPING
	// [DEMO 29 §2] HERE AND NOWHERE EARLIER, because it reads what both lines above just wrote:
	// HandsLoadout (which hold to play) from UpdateHandsAnimation, and HandsWristDelta (where the gun
	// has got to this frame) from UpdateWeaponsFollowHands. Called one line after the guns are placed
	// and against the same delta, so the fist and the grip are settled from one measurement.
	//
	// ABOVE THE bAnimate GATE, with UpdateHandsAnimation, and for the same reason: a rig that is
	// hidden must not come back holding the previous weapon. One pointer compare in a match.
	UpdateOwnerArmsPose();
#endif

	if (!bAnimate)
	{
		return;
	}

	// Vertical bob runs at twice the lateral rate — one dip per footfall, one sway per stride, which
	// is what a gait actually does.
	FVector Offset(
		-ViewModelKick * TraceCharacterLayout::KickBackUU,
		FMath::Sin(ViewModelBobPhase) * TraceCharacterLayout::BobLateralUU * ViewModelBobStrength,
		FMath::Sin(ViewModelBobPhase * 2.f) * TraceCharacterLayout::BobVerticalUU * ViewModelBobStrength
			- ViewModelCrouchDip * TraceCharacterLayout::CrouchDipUU);

	const FRotator Rotation = TraceCharacterLayout::ViewModelRestRotation
		+ FRotator(ViewModelSwayPitch + ViewModelKick * TraceCharacterLayout::KickPitchDegrees,
			ViewModelSwayYaw, 0.f);

	ViewModelRoot->SetRelativeLocationAndRotation(TraceCharacterLayout::ViewModelRestLocation + Offset, Rotation);

	// [SPEC v31 §6] The two calls above the gate have already picked the hands' frame and carried
	// EVERY weapon part to wherever wrist_right ended up. What follows writes the guns' own part
	// motion ON TOP, through SetViewModelWeaponPose, which folds the same wrist delta in — so the
	// order matters: run the other way round, the recoil would be composed and then immediately
	// overwritten by the rest pose, i.e. a gun that never recoils on a hand that does.

	// The railgun's own animation runs on top of the rig transform above: the rig carries the whole
	// weapon-and-hands assembly, this moves parts of the weapon relative to it.
	UpdateRailgunFire(DeltaSeconds);

	// [SPEC v30 §3/§4] And the SMG's, on the same terms. BOTH are ticked whichever gun is on screen,
	// deliberately: the hidden one costs four early-outs or four transforms that nothing renders, and
	// the alternative — animating only the selected rig — leaves the other one holding whatever pose
	// it had when it was put away, so a swap back mid-burst would show a gun frozen with its walls
	// open. Cheap insurance against a class of bug that only appears under a swap.
	UpdateSmgAnimation(DeltaSeconds);

	// [SPEC v32 §5] *** LAST, AND THE ORDER IS THE WHOLE POINT. *** The gloves' flare is the gun's own
	// normalised discharge value remapped, and the two lines above are what publish that value for
	// THIS frame. Run any earlier and the hands would be lit by the previous frame's shot — invisible
	// at 60 Hz on a 0.75 s decay, and wrong in exactly the way that makes a verifier holding a pinned
	// phase read two numbers that should be one.
	//
	// Below the bAnimate gate rather than above it, unlike UpdateHandsAnimation: the clip's POSITION
	// has to keep moving while the rig is hidden (a throw starts in third person and must not snap
	// into its own follow-through), but a hidden rig's BRIGHTNESS is not a state anything can read,
	// and the first visible frame writes it correctly before it is drawn.
	UpdateHandsEmissive();
}

void ATraceCharacter::NotifyWeaponFired()
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	if ((Now - LastFireKickTime) < TraceCharacterLayout::FireKickRefractorySeconds)
	{
		return;
	}
	LastFireKickTime = Now;

	ViewModelKick = 1.f;

	// [SPEC v31 §6] The hand's own recoil clip, armed by the SAME real shot that arms the two guns'.
	// A LATCH rather than a PlayAnimation, because Shoot_{Pistol,Smg} is 0.1667 s — ten frames at 60 —
	// and the sample-before-advance ordering that makes its frame 0 exist at all lives in exactly one
	// place. A second writer here would be the per-frame-reader bug in a different costume.
	bHandsShotPending = true;

	// Restart the railgun's discharge, time-warped so it always finishes before the next round can
	// leave: at 150 RPM that is 0.36 s for the clip's 0.85 s tail, and it shortens further for the
	// characters that fire faster. Rails caught half-open by the next shot would read as a stutter.
	if (RailgunBodyPart != nullptr)
	{
		const float FireInterval = FMath::Max(0.01f, UTraceSettings::Get().FireInterval)
			* UTraceAbilityComponent::GetFireIntervalScaleFor(this);
		RailgunFireDuration = FMath::Clamp(
			FireInterval * TraceCharacterLayout::RailgunFireIntervalFraction,
			TraceCharacterLayout::RailgunFireMinSeconds,
			TraceRailgunFireCurve::ClipSeconds - TraceRailgunFireCurve::DischargeSeconds);
		RailgunFireElapsed = 0.f;
	}

	// [SPEC v30 §3] The SMG's cycle, restarted by the same real shot. THIS IS THE ONLY THING THAT
	// STARTS IT — there is no free-running timer anywhere in UpdateSmgAnimation — so the walls
	// cannot snap on a frame no round left on.
	//
	// The cycle is the kit's 0.100 s clip, but its LENGTH is taken from the weapon in hand rather
	// than from that constant, because 0.100 s is only the cadence of a stock SMG: Roxie's Modded
	// runs the same gun at 990 RPM and Slimeball's stuck passive at 780. Using the authored length
	// there would leave the walls still ringing when the next round left. GetFireInterval() is the
	// component's own answer, ability scaling already folded in, so the animation inherits every
	// per-character fire-rate modifier for free — the same seam spec v28 §9 required of the SMG's
	// gameplay numbers.
	if (SmgBodyPart != nullptr)
	{
		float Cycle = TraceCharacterLayout::SmgFireClipSeconds;
		if (Weapon != nullptr)
		{
			Cycle = static_cast<float>(Weapon->GetFireInterval());
		}
		SmgFireDuration = FMath::Max(
			Cycle * TraceCharacterLayout::SmgFireIntervalFraction,
			TraceCharacterLayout::SmgFireMinSeconds);
		SmgFireElapsed = 0.f;
	}
}
