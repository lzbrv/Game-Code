// Trace — the player pawn. See TraceCharacter.h for the shape of the thing and why.

#include "Core/TraceCharacter.h"

#include "Net/UnrealNetwork.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"            // the pack's twenty hand clips are LOADED here, in
                                              // the constructor, and animated in TraceCharacterViewModel.cpp
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "HAL/IConsoleManager.h"               // TAutoConsoleVariable: the out-of-bounds knobs below.
                                              // FTSTicker, GEngine, TActorIterator, the free camera
                                              // and DrawDebug went with the console commands to
                                              // TraceCharacterDebugCommands.cpp, and so did the five
                                              // includes that existed only for them.
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"    // ApplyRotationMode: human vs bot
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"


#include "Core/TraceCharacterInternal.h"      // the measured layout/asset constant tables, and the
                                              // extern block for this file's console variables
#include "Core/TraceCharacterRoster.h"        // NoneId: the "nobody yet" character id the FP cuff
                                             // accent resets to. The body mesh path and its yaw are
                                             // read in TraceCharacterBody.cpp now.
#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Abilities/TraceAbilityComponent.h"    // the ability the pawn's death and select paths drive
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceParry.h"                // the parry entry point and its queries (spec §3)
#include "Gameplay/TraceTrailComponent.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Net/TraceLagCompensationComponent.h"
#include "Settings/TraceGameUserSettings.h"    // ApplySavedFieldOfView(): the VIDEO page's FOV row
#include "Trace.h"
#include "TraceSettings.h"
#include "World/TraceArenaBuilder.h"          // SetBase(): the arena is not a moving platform

namespace
{
	/**
	 * Where this character is looking, as a rotation.
	 *
	 * The control rotation is the authority: it exists on the owning client (locally set) and on the
	 * server (replicated inside every ServerMove), which are the only two machines that resolve a
	 * shot. A simulated proxy on a bystander's client has no controller at all, so it falls back to
	 * the replicated actor rotation — nothing gameplay-critical ever asks a simulated proxy where it
	 * is aiming.
	 */
	FRotator ResolveAimRotation(const ATraceCharacter& TraceChar)
	{
		if (const AController* OwningController = TraceChar.GetController())
		{
			return OwningController->GetControlRotation();
		}

		FRotator Rotation = TraceChar.GetActorRotation();
		Rotation.Pitch = 0.f;
		Rotation.Roll = 0.f;
		return Rotation;
	}

	/** Clamps a tint back into a sane range and forces opaque alpha (colours get scaled about). */
	/**
	 * "Is a human sitting behind this pawn's eyes on THIS machine."
	 *
	 * Not APawn::IsLocallyControlled(), which is true for every bot on the server as well — an
	 * AIController is by definition a local controller. Hiding a bot's body from "its owner" is
	 * harmless right up until something makes that bot a view target, and paying for a camera blend
	 * on nine pawns that have no camera is pure waste.
	 */
	bool IsLocalPlayerPawn(const ATraceCharacter& TraceChar)
	{
		const APlayerController* PC = Cast<APlayerController>(TraceChar.GetController());
		return PC != nullptr && PC->IsLocalController();
	}

	/**
	 * Push the player's saved FIELD OF VIEW onto this pawn's camera.
	 *
	 * WHY THIS LIVES HERE AND NOT ONLY IN THE SETTINGS CLASS. UTraceGameUserSettings also re-applies
	 * the FOV from a 1 Hz ticker, which is what made the row work before this pawn knew about it —
	 * but a ticker can be up to a second late, and the second after a respawn is exactly when a
	 * player is re-orienting. Called from BeginPlay and again on the frame the pawn becomes locally
	 * controlled (on a client the controller arrives by replication AFTER the pawn, so BeginPlay
	 * alone is not enough), the value is correct on the first rendered frame of every life.
	 *
	 * Only ever writes a locally controlled human's camera: no other pawn's UCameraComponent is
	 * rendered through, and a bot's FOV is not the local player's business.
	 *
	 * Does nothing when the settings object is unavailable (dedicated server, early startup) — the
	 * constructor's shipped 95 stands, which is the correct fallback. It deliberately leaves
	 * FirstPersonFieldOfView alone: that is the view model's own projection, and widening the world
	 * must not stretch the gun.
	 */
	void ApplySavedFieldOfView(const ATraceCharacter& TraceChar, UCameraComponent* InCamera)
	{
		if (InCamera == nullptr || !IsLocalPlayerPawn(TraceChar))
		{
			return;
		}

		if (const UTraceGameUserSettings* const Video = UTraceGameUserSettings::Get())
		{
			InCamera->SetFieldOfView(Video->GetFieldOfView());
		}
	}
	/** Shared setup for the fallback shapes: drawable, and incapable of colliding. */
	void ConfigureVisualMesh(UStaticMeshComponent* InMesh)
	{
		if (InMesh == nullptr)
		{
			return;
		}

		// Contract §7: the capsule is the ONLY collider. A colliding mesh here would let a bullet
		// stop on "the shoulder" while the lag-compensated capsule test says it missed.
		InMesh->SetCollisionProfileName(TEXT("NoCollision"));
		InMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		InMesh->SetGenerateOverlapEvents(false);
		InMesh->SetCanEverAffectNavigation(false);
		InMesh->bReceivesDecals = false;

		// Same reason as the skeletal mesh: hidden from its owner in first person, still casting.
		InMesh->bCastHiddenShadow = true;
	}
}

// =================================================================================================
// Construction
// =================================================================================================

ATraceCharacter::ATraceCharacter(const FObjectInitializer& OI)
	// This is the whole reason the FObjectInitializer constructor exists: ACharacter creates its
	// movement component as a named default subobject, and overriding the class here is the only
	// way to get UTraceCharacterMovementComponent (and with it the predicted dash) in its place.
	: Super(OI.SetDefaultSubobjectClass<UTraceCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	// The actor tick exists for exactly one thing: the first/third person camera blend, which needs
	// a per-frame delta and nothing else. AActor defaults it off and ACharacter does not override
	// ::Tick at all in 5.8, so this is a new tick function rather than a re-enabled one; everything
	// else here still ticks itself (the movement component, lag compensation, the trail).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;
	SetReplicateMovement(true);

	// MEASURED (spec v5 section 0 acceptance run): a joining client saw only 5 of the 10 characters.
	// APawn's default NetCullDistanceSquared is 225000000 = 15000 uu, and this field is 33600 x 9600
	// - a corner-to-corner distance of ~34900 uu - so on a full-length field half the roster is
	// never relevant and simply is not in the client's world. Trace's whole premise is reading an
	// enemy's trail from across the arena, so there is no distance at which a player stops mattering.
	//
	// 40000 uu covers the diagonal with headroom. Squared it is 1.6e9, which is exact in a float
	// (it is 1.6e9 < 2^31 and the value has few significant digits), so no precision game is being
	// played here. Ten pawns on one field is a trivial relevancy set; this is not a bandwidth risk.
	//
	// Written through the SETTER, not the field. Direct access to NetCullDistanceSquared is
	// UE_DEPRECATED(5.5) and clang here reports it; Unreal builds this module warnings-as-errors on
	// MSVC, so the field form is a Windows build break waiting to happen (and the deprecation note
	// says it stops compiling outright next release).
	SetNetCullDistanceSquared(40000.f * 40000.f);

	// Set here so the class default is right for a pawn that is spawned and never possessed, and
	// re-derived from that default every time ACharacter::UnCrouch calls RecalculateBaseEyeHeight().
	// The first-person camera is placed at exactly this height — see EyeHeight.
	BaseEyeHeight = TraceCharacterLayout::EyeHeight;

	// A starting value only. Crouching now exists (it is how sliding works — spec section 5), and the
	// real crouched eye height depends on the crouched capsule height, which belongs to
	// UTraceCharacterMovementComponent. OnStartCrouch() computes it from the resized capsule; see the
	// note on the override.
	CrouchedEyeHeight = TraceCharacterLayout::EyeHeight;

	// Facing is mode-dependent and is configured by ApplyRotationMode() once a controller exists —
	// first person turns the body with the aim, carrying turns it with the movement. These class
	// defaults are the carrying/bot case (capsule follows its movement, camera and shot direction
	// follow the control rotation), which is also the only correct answer for an unpossessed pawn.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->InitCapsuleSize(TraceCharacterLayout::CapsuleRadius, TraceCharacterLayout::CapsuleHalfHeight);
		Capsule->SetCollisionProfileName(TEXT("Pawn"));

		// The spring arm below probes on ECC_Camera, and the engine's Pawn profile overrides only
		// Visibility to Ignore - Camera stays Block. In a 5v5 that means any teammate standing behind
		// you yanks your camera into your own head. Pawn-vs-Pawn *movement* blocking is on the
		// WorldDynamic/Pawn channels and is unaffected by this.
		Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

		// Both sides of an overlap pair must generate events. The endzone trigger and the Core's
		// pickup sphere are the other half; without this the game has no way to score or to pick
		// the Core up.
		Capsule->SetGenerateOverlapEvents(true);
	}

	// --- The character you actually see ----------------------------------------------------------
	//
	// ACharacter already owns a USkeletalMeshComponent; this places it and makes it harmless. The
	// asset and the anim blueprint are attached later, in SetupCharacterVisuals(), because they are
	// imported per developer and may not exist (see the header).
	//
	// The relative transform is set HERE and nowhere else: ACharacter::PostInitializeComponents()
	// snapshots it into BaseTranslationOffset/BaseRotationOffset, which root motion and crouch then
	// work against. Moving the mesh after that point desynchronises those.
	if (USkeletalMeshComponent* SkeletalMesh = GetMesh())
	{
		SkeletalMesh->SetRelativeLocation(FVector(0.f, 0.f, TraceCharacterLayout::MeshOffsetZ));
		SkeletalMesh->SetRelativeRotation(FRotator(0.f, TraceCharacterLayout::MeshYaw, 0.f));

		// Contract §7 again: the capsule is the only collider. The mannequin ships with a physics
		// asset (PA_Mannequin) and would otherwise start blocking bullets per-bone, which is exactly
		// the "hit the shoulder, miss the capsule" desync the rest of this file is built to avoid.
		SkeletalMesh->SetCollisionProfileName(TEXT("NoCollision"));
		SkeletalMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SkeletalMesh->SetGenerateOverlapEvents(false);
		SkeletalMesh->SetCanEverAffectNavigation(false);
		SkeletalMesh->bReceivesDecals = false;

		// In first person this mesh is hidden from its own owner (SetOwnerNoSee). Without this the
		// player would also lose their own shadow, and on a black floor with hard neon key lights the
		// shadow is most of what tells you where your feet are. Costs one extra shadow-only draw for
		// the one pawn that is hidden, and nothing at all for the other nine.
		SkeletalMesh->bCastHiddenShadow = true;

		// Ten characters in a 5v5, most of them off-screen most of the time. Skipping the pose for
		// anything not rendered is the single biggest animation saving available and costs nothing
		// visible — the pose is rebuilt the moment the character comes back on screen.
		SkeletalMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;

		// Hidden until SetupCharacterVisuals() confirms there is a mesh to show, so a missing import
		// can never leave a T-posing null-mesh component drawing nothing over the fallback shapes.
		SkeletalMesh->SetVisibility(false);
	}

	// Soft references, resolved once per pawn in PostInitializeComponents(). Assigning them here
	// (rather than in a config or a Blueprint) keeps them on the CDO, which is what makes the cooker
	// follow them into a packaged build.
	CharacterMeshAsset = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(TraceCharacterAssets::MannequinMesh));
	CharacterAnimClass = TSoftClassPtr<UAnimInstance>(FSoftClassPath(TraceCharacterAssets::UnarmedAnimClass));

	// --- Camera rig ------------------------------------------------------------------------------

	// ONE arm, ONE camera, for both view modes — UpdateViewBlend() lerps the arm between them. Two
	// camera actors and a SetViewTarget would put a player camera manager blend on top of ours and
	// would cut, not travel; the travel is the whole point.
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);

	// First person is the default state of the game, so the rig is built collapsed onto the eye and
	// only opens out when the Core is picked up.
	SpringArm->TargetArmLength = TraceCharacterLayout::FirstPersonArmLength;
	SpringArm->TargetOffset = FVector(0.f, 0.f, TraceCharacterLayout::EyeHeight);
	// SocketOffset stays zero in both modes — see ThirdPersonPivotZ for why the height lives in
	// TargetOffset instead.
	SpringArm->SocketOffset = FVector::ZeroVector;

	SpringArm->bUsePawnControlRotation = true;
	SpringArm->bDoCollisionTest = true;
	// Camera lag would smear the crosshair away from the aim direction the weapon actually uses.
	SpringArm->bEnableCameraLag = false;
	SpringArm->bEnableCameraRotationLag = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;   // the arm already applied it

	// The SHIPPED default only. The player's own value (VIDEO page -> FIELD OF VIEW, persisted in
	// GameUserSettings.ini) is pushed on top of this by ApplySavedFieldOfView() from BeginPlay and
	// again the frame this pawn becomes locally controlled — see the note there. It is deliberately
	// NOT read here: this runs during CDO construction, before the engine has created the
	// UTraceGameUserSettings, so a lookup here would either be null or force the settings object
	// into existence early. TraceCharacterLayout::CameraFOV and
	// UTraceGameUserSettings::DefaultFieldOfView are the same 95, so a player who never touches the
	// row never sees a change.
	Camera->SetFieldOfView(TraceCharacterLayout::CameraFOV);

	// First-person rendering parameters. These affect ONLY primitives tagged
	// EFirstPersonPrimitiveType::FirstPerson — i.e. the viewmodel and nothing else in the world — so
	// they are harmless while the camera is in third person, where the viewmodel is hidden anyway.
	//
	// FirstPersonFieldOfView 70 against a scene FOV of 95: the renderer cancels the scene projection
	// and re-applies this one, so the gun is drawn through a normal lens instead of the wide one the
	// arena needs, which is what stops a viewmodel at arm's length looking stretched and enormous.
	// FirstPersonScale 0.5 then squashes the rig's DEPTH range toward the camera by half while
	// preserving the solid angle it covers, so it looks identical and can no longer reach into the
	// scene. See the depth arithmetic in the file header.
	Camera->bEnableFirstPersonFieldOfView = true;
	Camera->bEnableFirstPersonScale = true;
	Camera->FirstPersonFieldOfView = TraceCharacterLayout::FirstPersonViewModelFOV;
	Camera->FirstPersonScale = TraceCharacterLayout::FirstPersonViewModelScale;

	// The viewmodel rig hangs off the CAMERA, not off the capsule or the mesh: it has to inherit the
	// aim exactly, and the camera is the one thing in the hierarchy that already has it. Nothing in
	// the shot path reads this component, so animating it cannot move a bullet.
	ViewModelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ViewModelRoot"));
	ViewModelRoot->SetupAttachment(Camera);
	ViewModelRoot->SetRelativeLocationAndRotation(
		TraceCharacterLayout::ViewModelRestLocation, TraceCharacterLayout::ViewModelRestRotation);

	// --- Fallback shapes -------------------------------------------------------------------------
	//
	// A clone that has not run Scripts/import-mannequin.sh has no character art at all. Rather than
	// render nothing — an invisible player is far worse than an ugly one — the pawn keeps the old
	// primitive body/head, built from /Engine/BasicShapes which ship with every install, hidden
	// unless the mannequin fails to load.

	FallbackBodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FallbackBodyMesh"));
	FallbackBodyMesh->SetupAttachment(RootComponent);
	ConfigureVisualMesh(FallbackBodyMesh);
	FallbackBodyMesh->SetVisibility(false);

	FallbackHeadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FallbackHeadMesh"));
	FallbackHeadMesh->SetupAttachment(RootComponent);
	ConfigureVisualMesh(FallbackHeadMesh);
	FallbackHeadMesh->SetVisibility(false);

	// Constructor-time FObjectFinders (not runtime LoadObject) on purpose: the reference lands on
	// the CDO, which is what makes the cooker pull these engine assets into a packaged build. A
	// bare runtime load of the same path would resolve in the editor and return null in a package.
	// These are /Engine/ assets, so unlike the mannequin they are always present and a hard
	// reference is safe.
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
		static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

		if (CubeFinder.Succeeded())
		{
			CubeMesh = CubeFinder.Object;
		}
		if (CylinderFinder.Succeeded())
		{
			CylinderMesh = CylinderFinder.Object;
			FallbackBodyMesh->SetStaticMesh(CylinderFinder.Object);
		}
		if (SphereFinder.Succeeded())
		{
			FallbackHeadMesh->SetStaticMesh(SphereFinder.Object);
		}
		if (MaterialFinder.Succeeded())
		{
			BasicShapeMaterial = MaterialFinder.Object;
		}
	}

	// The Tron materials, resolved the same way ATraceArenaBuilder resolves them: constructor-time
	// finders so the reference lands on the CDO and the cooker follows it, COMMITTED pair first and
	// the gitignored legacy pair only as a fallback, and a tolerated miss on both that
	// MakeViewModelMaterials() turns into a BasicShapeMaterial fallback.
	//
	// Separate static finders per path rather than a loop: ConstructorHelpers::FObjectFinder must be
	// static and is only legal during construction, so each candidate needs its own.
	{
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> SurfaceFinder(TraceCharacterAssets::SurfaceMaterialPath);
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> NeonFinder(TraceCharacterAssets::NeonMaterialPath);
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> LegacySurfaceFinder(TraceCharacterAssets::LegacySurfaceMaterialPath);
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> LegacyNeonFinder(TraceCharacterAssets::LegacyNeonMaterialPath);

		if (SurfaceFinder.Succeeded())
		{
			SurfaceMaterial = SurfaceFinder.Object;
		}
		else if (LegacySurfaceFinder.Succeeded())
		{
			SurfaceMaterial = LegacySurfaceFinder.Object;
		}

		if (NeonFinder.Succeeded())
		{
			NeonMaterial = NeonFinder.Object;
		}
		else if (LegacyNeonFinder.Succeeded())
		{
			NeonMaterial = LegacyNeonFinder.Object;
		}
	}

	// The railgun's three meshes. Constructor-time finders for the same reason as everything above:
	// the reference lands on the CDO, so the cooker packages the art without anyone maintaining a
	// list of "additional assets to cook". A miss is not an error here — EnsureViewModelBuilt()
	// checks all three and builds the cube gun instead if any is absent.
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> RailgunBodyFinder(TraceCharacterAssets::RailgunBodyMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> RailgunRailLeftFinder(TraceCharacterAssets::RailgunRailLeftMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> RailgunRailRightFinder(TraceCharacterAssets::RailgunRailRightMeshPath);

		if (RailgunBodyFinder.Succeeded())
		{
			RailgunBodyMesh = RailgunBodyFinder.Object;
		}
		if (RailgunRailLeftFinder.Succeeded())
		{
			RailgunRailLeftMesh = RailgunRailLeftFinder.Object;
		}
		if (RailgunRailRightFinder.Succeeded())
		{
			RailgunRailRightMesh = RailgunRailRightFinder.Object;
		}
	}

	// The SMG's four meshes, on the identical contract: a CDO reference so the cooker packages them,
	// and a miss on any one is a fallback rather than an error. See BuildSmgViewModel().
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SmgBodyFinder(TraceCharacterAssets::SmgBodyMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SmgWallLeftFinder(TraceCharacterAssets::SmgWallLeftMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SmgWallRightFinder(TraceCharacterAssets::SmgWallRightMeshPath);
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SmgMagFinder(TraceCharacterAssets::SmgMagMeshPath);

		if (SmgBodyFinder.Succeeded())
		{
			SmgBodyMesh = SmgBodyFinder.Object;
		}
		if (SmgWallLeftFinder.Succeeded())
		{
			SmgWallLeftMesh = SmgWallLeftFinder.Object;
		}
		if (SmgWallRightFinder.Succeeded())
		{
			SmgWallRightMesh = SmgWallRightFinder.Object;
		}
		if (SmgMagFinder.Succeeded())
		{
			SmgMagMesh = SmgMagFinder.Object;
		}
	}

	// [SPEC v31 §6] The pack's gloved hands: one skeletal mesh and twenty sequences, on the identical
	// contract as the two guns above — CDO references so the cooker packages them, and a miss on any
	// one of them is a fallback rather than an error (see BuildPackHandsViewModel).
	//
	// A STATIC ARRAY OF FINDERS rather than twenty named ones. ConstructorHelpers::FObjectFinder must
	// be static and may only be constructed inside a constructor; a function-local static ARRAY
	// satisfies both — it is built exactly once, on the first construction, which is the same
	// guarantee each individual static above relies on — and it keeps the twenty paths in ONE ordered
	// table next to the enum that indexes them instead of twenty places to get out of step.
	{
		static ConstructorHelpers::FObjectFinder<USkeletalMesh> HandsFinder(TraceCharacterAssets::HandsMeshPath);
		if (HandsFinder.Succeeded())
		{
			HandsMesh = HandsFinder.Object;
		}

		static ConstructorHelpers::FObjectFinder<UAnimSequence> HandsAnimFinders[] =
		{
			TraceCharacterAssets::HandsAnimPaths[0],  TraceCharacterAssets::HandsAnimPaths[1],
			TraceCharacterAssets::HandsAnimPaths[2],  TraceCharacterAssets::HandsAnimPaths[3],
			TraceCharacterAssets::HandsAnimPaths[4],  TraceCharacterAssets::HandsAnimPaths[5],
			TraceCharacterAssets::HandsAnimPaths[6],  TraceCharacterAssets::HandsAnimPaths[7],
			TraceCharacterAssets::HandsAnimPaths[8],  TraceCharacterAssets::HandsAnimPaths[9],
			TraceCharacterAssets::HandsAnimPaths[10], TraceCharacterAssets::HandsAnimPaths[11],
			TraceCharacterAssets::HandsAnimPaths[12], TraceCharacterAssets::HandsAnimPaths[13],
			TraceCharacterAssets::HandsAnimPaths[14], TraceCharacterAssets::HandsAnimPaths[15],
			TraceCharacterAssets::HandsAnimPaths[16], TraceCharacterAssets::HandsAnimPaths[17],
			TraceCharacterAssets::HandsAnimPaths[18], TraceCharacterAssets::HandsAnimPaths[19]
		};
		static_assert(UE_ARRAY_COUNT(HandsAnimFinders) == TraceCharacterAssets::HandsClip_Count,
			"The constructor's hand clip finders and the clip index enum have drifted apart.");

		HandsAnims.SetNum(TraceCharacterAssets::HandsClip_Count);
		for (int32 Index = 0; Index < TraceCharacterAssets::HandsClip_Count; ++Index)
		{
			HandsAnims[Index] = HandsAnimFinders[Index].Succeeded() ? HandsAnimFinders[Index].Object : nullptr;
		}
	}

#if !UE_BUILD_SHIPPING
	// [DEMO 29 §2] The owner's own arms rig and its four solved hold poses, on the SAME optional
	// contract as everything above: CDO references so a Development cook packages them, and a miss is
	// a fixture that does not appear (or one loadout that falls back to the bind pose) rather than an
	// error. NONE of the pack's twenty clips can serve here — they are on SK_TraceHands_Skeleton and
	// do not interchange — so this rig has its own four, authored by Scripts/pose_hands.py against
	// the very weapon geometry the shipped viewmodel puts in front of it.
	//
	// INSIDE THE GUARD, WHICH IS THE POINT. A Shipping binary does not reference these assets at all,
	// so the cooker does not carry a test fixture into a shipped build. See
	// TracePracticeRange::ShouldUseOwnerArmsViewModel().
	{
		static ConstructorHelpers::FObjectFinder<USkeletalMesh> ArmsFinder(TraceCharacterAssets::ArmsMeshPath);
		if (ArmsFinder.Succeeded())
		{
			ArmsMesh = ArmsFinder.Object;
		}

		// A STATIC ARRAY OF FINDERS, for the same two reasons the twenty above are one: a finder must
		// be static and constructed inside a constructor, and one ordered table beside the enum that
		// indexes it cannot get out of step the way four named variables can.
		static ConstructorHelpers::FObjectFinder<UAnimSequence> ArmsPoseFinders[] =
		{
			TraceCharacterAssets::ArmsPosePaths[0], TraceCharacterAssets::ArmsPosePaths[1],
			TraceCharacterAssets::ArmsPosePaths[2], TraceCharacterAssets::ArmsPosePaths[3]
		};
		static_assert(UE_ARRAY_COUNT(ArmsPoseFinders) == TraceCharacterAssets::ArmsPose_Count,
			"The constructor's arms pose finders and the arms pose index enum have drifted apart.");

		ArmsPoses.SetNum(TraceCharacterAssets::ArmsPose_Count);
		for (int32 Index = 0; Index < TraceCharacterAssets::ArmsPose_Count; ++Index)
		{
			ArmsPoses[Index] = ArmsPoseFinders[Index].Succeeded() ? ArmsPoseFinders[Index].Object : nullptr;
		}
	}
#endif

	// Body: a cylinder standing on the bottom of the capsule, as wide as the capsule is.
	{
		const float BodyScaleXY = (TraceCharacterLayout::CapsuleRadius * 2.f) / TraceCharacterLayout::BasicShapeSize;
		const float BodyScaleZ = TraceCharacterLayout::BodyHeight / TraceCharacterLayout::BasicShapeSize;
		FallbackBodyMesh->SetRelativeScale3D(FVector(BodyScaleXY, BodyScaleXY, BodyScaleZ));
		FallbackBodyMesh->SetRelativeLocation(FVector(
			0.f, 0.f, (TraceCharacterLayout::BodyHeight * 0.5f) - TraceCharacterLayout::CapsuleHalfHeight));
	}

	// Head: a sphere capping the body, overlapping it slightly so the silhouette reads as one piece.
	{
		const float HeadScale = TraceCharacterLayout::HeadDiameter / TraceCharacterLayout::BasicShapeSize;
		FallbackHeadMesh->SetRelativeScale3D(FVector(HeadScale));
		FallbackHeadMesh->SetRelativeLocation(FVector(0.f, 0.f, TraceCharacterLayout::HeadCentreZ));
	}

	// --- Slide skid streak -------------------------------------------------------------------------
	//
	// Flat on the deck under the feet, unlit, team-coloured, and hidden until the pawn is actually
	// sliding. See UpdateCrouchPresentation for why this exists at all.
	SlideSkidMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SlideSkidMesh"));
	SlideSkidMesh->SetupAttachment(RootComponent);
	// After the FObjectFinder block above, which is where CubeMesh is resolved. A null mesh here
	// would leave the streak invisible AND leave CreateDynamicMaterialInstance with no slot to wrap,
	// so ApplyTeamColors would silently never build SlideSkidMID either.
	if (CubeMesh != nullptr)
	{
		SlideSkidMesh->SetStaticMesh(CubeMesh);
	}
	ConfigureVisualMesh(SlideSkidMesh);
	SlideSkidMesh->SetVisibility(false);
	// The one visual on this pawn that must NOT be hidden from its owner in first person and must
	// NOT cast a shadow: it is a light on the floor, not part of the body.
	SlideSkidMesh->bCastHiddenShadow = false;
	SlideSkidMesh->SetCastShadow(false);
	SlideSkidMesh->SetRelativeLocation(FVector(0.f, 0.f, -TraceCharacterLayout::CapsuleHalfHeight + 3.f));

	// --- Movement capabilities ---------------------------------------------------------------------
	//
	// Crouch has to be ALLOWED on the nav agent or UCharacterMovementComponent::Crouch() silently
	// refuses, and crouch is how spec section 5's slide is implemented. Set here rather than in the
	// movement component because it is a property of the pawn, and setting it twice is free.
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->GetNavAgentPropertiesRef().bCanCrouch = true;
	}

	// --- Gameplay components ---------------------------------------------------------------------

	Health = CreateDefaultSubobject<UTraceHealthComponent>(TEXT("Health"));
	Weapon = CreateDefaultSubobject<UTraceWeaponComponent>(TEXT("Weapon"));
	LagComp = CreateDefaultSubobject<UTraceLagCompensationComponent>(TEXT("LagComp"));

	// The trail is a scene component: its points are laid in world space from the owner's position,
	// so it needs a transform in the hierarchy.
	Trail = CreateDefaultSubobject<UTraceTrailComponent>(TEXT("Trail"));
	Trail->SetupAttachment(RootComponent);
}

// =================================================================================================
// Lifecycle
// =================================================================================================

void ATraceCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// After Super, deliberately: ACharacter::PostInitializeComponents() is what snapshots the mesh's
	// relative transform and initialises the anim instance machinery. Dressing the mesh before that
	// would have the base class re-derive its offsets from a component we were halfway through
	// changing.
	SetupCharacterVisuals();
}

void ATraceCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (Health != nullptr)
	{
		// AddUnique so a re-entrant BeginPlay (seamless travel, level streaming) cannot double-bind
		// and turn one death into two.
		Health->OnDeath.AddUniqueDynamic(this, &ATraceCharacter::OnHealthDeath);
	}

	UWorld* World = GetWorld();

	if (HasAuthority() && World != nullptr)
	{
		// The GameMode's roster is what the lag-compensated hitscan resolver and the trail trip test
		// iterate. A character that fails to register is invisible to both: unshootable and unable
		// to trip a trail.
		if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
		{
			GameMode->RegisterCharacter(this);
		}
	}

	ApplyTeamColors();

	// First of four calls; see the declaration. This one is the only one that can be early enough on
	// a listen server, and is far too early on a client — which is the whole reason there are four.
	UpdateCharacterBodyMesh();

	// Snapped, never blended: a pawn spawns without the Core, so the first frame must already be
	// first person. Blending into it would fly every camera in the match forward from 450 uu back.
	ApplyRotationMode();
	UpdateViewBlend(0.f, /*bSnap=*/true);

	// The player's FOV, on the first frame of the life rather than up to a second into it.
	ApplySavedFieldOfView(*this, Camera);

	// Team colours are cosmetic, so the poll costs a dedicated server nothing (it never runs there).
	if (World != nullptr && GetNetMode() != NM_DedicatedServer && GetTeam() == ETraceTeam::None)
	{
		World->GetTimerManager().SetTimer(
			TeamColorTimerHandle, this, &ATraceCharacter::PollTeamColors,
			TraceCharacterLayout::TeamColorPollInterval, /*bLoop=*/true);
	}
}

void ATraceCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(TeamColorTimerHandle);

		if (HasAuthority())
		{
			// Unregister before Super: the roster holds weak pointers, but leaving a dead entry in
			// it makes every iteration pay for it until the next compaction.
			if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
			{
				GameMode->UnregisterCharacter(this);
			}
		}
	}

	if (Health != nullptr)
	{
		Health->OnDeath.RemoveDynamic(this, &ATraceCharacter::OnHealthDeath);
	}

	Super::EndPlay(EndPlayReason);
}

void ATraceCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// SPEC v19 §4.1, first, and server-only. It is here rather than in the game mode because the
	// question is about ONE pawn's transform and this is the function that already runs once per pawn
	// per frame; a game-mode sweep would be a second iteration over the same actors to ask the same
	// thing. Two branches on a client, where it returns immediately.
	ServerCheckArenaBounds();

	// The rest of this function is presentation. See UpdateViewBlend().
	//
	// The rotation model is re-asserted here rather than only on possession because on a client the
	// controller arrives by replication, after the pawn: there is no PossessedBy() on that machine.
	// Both calls early-out on "nothing changed", so the steady-state cost is two branches.
	const bool bLocalPlayer = IsLocalPlayerPawn(*this);
	const bool bJustBecameLocal = bLocalPlayer && !bWasLocallyControlled;
	bWasLocallyControlled = bLocalPlayer;

	// The client path: BeginPlay ran before this pawn had a controller, so that call declined.
	if (bJustBecameLocal)
	{
		ApplySavedFieldOfView(*this, Camera);
	}

	ApplyRotationMode();

	// *** THE ONE THAT CANNOT GO STALE. *** Every pawn, every machine, every frame — and one integer
	// compare in the steady state, because the applied character id is remembered. This is what makes
	// a late-arriving PlayerState, a mid-match character switch and a late joiner all the same case.
	UpdateCharacterBodyMesh(/*bIsPoll=*/true);

	// BEFORE UpdateViewBlend, and the order is load-bearing: this is what moves BaseEyeHeight for a
	// slide, and UpdateViewBlend pins the spring arm to BaseEyeHeight. Running it after would leave
	// the camera one frame behind the point the shot is built from for the whole descent, i.e. a
	// non-zero eyeErr exactly while the player is sliding. Runs for every pawn on every machine.
	UpdateCrouchPresentation(DeltaSeconds);

	UpdateViewBlend(DeltaSeconds, /*bSnap=*/bJustBecameLocal);

	// Local player only — UpdateViewBlend has already hidden the rig on everyone else, and this
	// early-outs on a hidden one.
	if (bLocalPlayer)
	{
		UpdateViewModel(DeltaSeconds);
	}
}

void ATraceCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Server side, this is the first moment GetPlayerState() can answer — and therefore the first
	// moment the team is knowable.
	ApplyTeamColors();

	// ...and the first moment the pawn knows whether it is a bot or a human, which is what decides
	// the rotation model. Snapped for the same reason as BeginPlay: a fresh pawn has no Core.
	ApplyRotationMode();
	UpdateViewBlend(0.f, /*bSnap=*/true);

	// Server side, this is where a RESPAWN gets its body back: the new pawn is possessed by the same
	// controller, carrying the same PlayerState, which already knows the character.
	UpdateCharacterBodyMesh();
}

void ATraceCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	// Client side, the PlayerState pointer can arrive before or after the pawn, and its Team can
	// arrive after that again. Every path that could learn the team ends up here.
	ApplyTeamColors();

	// And the character with it — though usually NOT YET, because the id is forwarded from an ability
	// component that replicates separately from the PlayerState it hangs on. This call is the cheap
	// early-out that catches the lucky ordering; Tick is what catches the rest.
	UpdateCharacterBodyMesh();
}

// =================================================================================================
// SPEC v19 §4.1 — OUT OF BOUNDS: DIE AND RESPAWN
//
// Verbatim: "If a player ever goes out of bounds of the arena, they should die and respawn."
//
// WHAT WAS ALREADY THERE, AND WHY IT IS NOT ENOUGH. FellOutOfWorld() below already turns the engine's
// KillZ into a real death, so falling THROUGH the floor was covered. Nothing covered leaving the
// arena sideways — through a seam, over a wall, out past a goal alcove — and the two characters
// landing this pass make that dramatically more reachable: Lily gets five seconds of flight and
// Mortimer gets a mantle. The spec's own warning is the design constraint here.
//
// *** THIS IS DELIBERATELY A HORIZONTAL TEST, PLUS A FLOOR. THERE IS NO CEILING BY DEFAULT. ***
// "Out of bounds" has to mean GENUINELY OUTSIDE THE ARENA and not "higher than usual", because the
// arena has no roof and a flying Lily above the wall line is still over the pitch, still inside the
// playing area seen from above, and still coming down. A ceiling that killed her would be a new bug
// wearing this feature's clothes. Trace.Bounds.CeilingMargin exists for a designer who disagrees and
// is 0 (off) as shipped.
//
// The margins are generous on purpose. This rule's failure modes are wildly asymmetric: a boundary
// that is slightly too tight kills players who are standing somewhere legal, which is unplayable; a
// boundary that is slightly too loose lets somebody stand in a void for another half second before
// dying, which nobody will ever notice.
// =================================================================================================

// THE FIVE KNOBS LIVE IN A NAMESPACE, AND THAT IS NOT DECORATION. Trace.Bounds.Verify — the
// harness that proves this rule — was moved out to TraceCharacterDebugCommands.cpp, and it prints
// all five values before it runs so that a reader can tell a real PASS from one bought by somebody
// having widened a margin. Reading them from another translation unit needs external linkage, and
// a bare `CVarBoundsEnabled` at global scope in a monolithic Shipping link is a name collision
// waiting to happen. Declared in TraceCharacterInternal.h; defined here, ONCE, beside the essay
// above, because a console variable constructed twice appears twice in the console.
namespace TraceCharacterBounds
{
	TAutoConsoleVariable<int32> CVarBoundsEnabled(
		TEXT("Trace.Bounds.Enabled"), 1,
		TEXT("SPEC v19 §4.1. 1 (shipped): a player genuinely outside the arena dies and respawns, credited "
		     "to nobody. 0: removes the rule, which is the A/B arm Trace.Bounds.Verify must go RED on."),
		ECVF_Cheat);

	TAutoConsoleVariable<float> CVarBoundsMarginXY(
		TEXT("Trace.Bounds.MarginXY"), 1200.f,
		TEXT("SPEC v19 §4.1. How far OUTSIDE the arena footprint, in uu, a player may be before it counts "
		     "as out of bounds. Generous by design: goal alcoves and the standoff shells all sit at or just "
		     "past the wall line, and killing somebody standing in a legal alcove is far worse than letting "
		     "somebody hang in a void for another half second."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarBoundsMarginBelow(
		TEXT("Trace.Bounds.MarginBelow"), 800.f,
		TEXT("SPEC v19 §4.1. How far BELOW the arena floor, in uu, before a player is out of bounds. This "
		     "usually fires before the engine's KillZ does, which is the point: it produces a death that "
		     "reads as 'you left the arena' rather than one that reads as the world ending."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarBoundsCeilingMargin(
		TEXT("Trace.Bounds.CeilingMargin"), 0.f,
		TEXT("SPEC v19 §4.1. 0 (shipped, and the default on purpose): THERE IS NO CEILING. Lily flies and "
		     "Mortimer mantles, so 'above the wall line' is a legal place to be and must not be a death. "
		     "Set it to a positive number of uu above the wall tops to add one anyway."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarBoundsGraceSeconds(
		TEXT("Trace.Bounds.GraceSeconds"), 0.35f,
		TEXT("SPEC v19 §4.1. How long a player must be continuously outside before the rule kills them. "
		     "Not a hair trigger: a depenetration push, an unratified teleport and the frame between a "
		     "spawn transform and its first movement update all look like one frame out of bounds."),
		ECVF_Default);
}

// The rule below reads them unqualified, exactly as it did when they were file-static.
using namespace TraceCharacterBounds;

/** Counts real out-of-bounds deaths. Read by Trace.Bounds.Verify so the harness is evidence. */
static int32 GTraceOutOfBoundsDeaths = 0;

/** The FName the death panel and the kill credit see. "the arena" is what the panel prints for it. */
static const FName GTraceOutOfBoundsCause(TEXT("OutOfBounds"));

bool ATraceCharacter::IsLocationOutOfArenaBounds(const UWorld* World, const FVector& Location, FString& OutReason)
{
	OutReason.Reset();

	if (World == nullptr || CVarBoundsEnabled.GetValueOnAnyThread() == 0)
	{
		return false;
	}

	const ATraceArenaBuilder* Arena = ATraceArenaBuilder::Get(World);
	if (Arena == nullptr)
	{
		// No arena means no bounds to be outside of — a menu map, the practice range if it is carved
		// somewhere else, a test fixture. Refusing to guess is the only safe answer: a rule that kills
		// on a map it does not understand is a rule that deletes somebody else's feature.
		return false;
	}

	const FBox Field = Arena->GetFieldBounds();
	if (Field.IsValid == 0)
	{
		return false;
	}

	const double MarginXY = FMath::Max(0.f, CVarBoundsMarginXY.GetValueOnAnyThread());
	const double MarginBelow = FMath::Max(0.f, CVarBoundsMarginBelow.GetValueOnAnyThread());
	const double CeilingMargin = CVarBoundsCeilingMargin.GetValueOnAnyThread();

	if (Location.X < Field.Min.X - MarginXY || Location.X > Field.Max.X + MarginXY)
	{
		OutReason = FString::Printf(TEXT("%.0f uu past the end wall"),
			(Location.X < Field.Min.X) ? (Field.Min.X - Location.X) : (Location.X - Field.Max.X));
		return true;
	}

	if (Location.Y < Field.Min.Y - MarginXY || Location.Y > Field.Max.Y + MarginXY)
	{
		OutReason = FString::Printf(TEXT("%.0f uu past the sideline wall"),
			(Location.Y < Field.Min.Y) ? (Field.Min.Y - Location.Y) : (Location.Y - Field.Max.Y));
		return true;
	}

	if (Location.Z < Field.Min.Z - MarginBelow)
	{
		OutReason = FString::Printf(TEXT("%.0f uu below the floor"), Field.Min.Z - Location.Z);
		return true;
	}

	// See the block above: OFF by default, and that is a decision rather than an oversight.
	if (CeilingMargin > 0.f && Location.Z > Field.Max.Z + CeilingMargin)
	{
		OutReason = FString::Printf(TEXT("%.0f uu above the wall tops"), Location.Z - Field.Max.Z);
		return true;
	}

	return false;
}

void ATraceCharacter::ServerCheckArenaBounds()
{
	UWorld* const MyWorld = GetWorld();
	if (!HasAuthority() || MyWorld == nullptr || !IsAlive() || Health == nullptr)
	{
		OutOfBoundsSinceServerTime = -1.f;
		return;
	}

	FString Reason;
	if (!IsLocationOutOfArenaBounds(MyWorld, GetActorLocation(), Reason))
	{
		OutOfBoundsSinceServerTime = -1.f;
		return;
	}

	const float NowSeconds = static_cast<float>(MyWorld->GetTimeSeconds());
	if (OutOfBoundsSinceServerTime < 0.f)
	{
		OutOfBoundsSinceServerTime = NowSeconds;
		return;   // The grace starts here. See the field's comment for what it is protecting against.
	}

	if ((NowSeconds - OutOfBoundsSinceServerTime) < FMath::Max(0.f, CVarBoundsGraceSeconds.GetValueOnAnyThread()))
	{
		return;
	}

	OutOfBoundsSinceServerTime = -1.f;
	++GTraceOutOfBoundsDeaths;

	UE_LOG(LogTraceGame, Display,
		TEXT("[Bounds] spec v19 §4.1: %s went OUT OF BOUNDS at %s (%s) - killing them. Credited to "
		     "nobody; their ability cooldowns keep running through it."),
		*GetName(), *GetActorLocation().ToCompactString(), *Reason);

	// Kill(), not ApplyDamage(), for the same reason FellOutOfWorld() uses it: a Core carrier is
	// invulnerable to damage, and a carrier who walks out of the world would otherwise take the Core
	// with them permanently. This is a world death, so it goes through the door the trail uses.
	//
	// *** THE KILLER IS DELIBERATELY nullptr. *** That is what makes it uncreditable:
	// ATraceGameMode::NotifyCharacterDied reads a null killer as bSelfKill, which skips the Kills
	// column entirely and makes the victim's death panel say "by the arena". Passing GetController()
	// would reach the same place today, but only because a self-kill happens to be excluded too —
	// null says the thing we actually mean.
	Health->Kill(nullptr, GTraceOutOfBoundsCause);
}

void ATraceCharacter::FellOutOfWorld(const UDamageType& DmgType)
{
	if (HasAuthority() && IsAlive() && Health != nullptr)
	{
		// Kill(), not ApplyDamage(): the carrier is invulnerable to damage, and a carrier who falls
		// out of the world would otherwise take the Core with them, permanently.
		Health->Kill(GetController(), FName(TEXT("Fell")));

		// Deliberately not calling Super here. AActor::FellOutOfWorld destroys the actor outright,
		// and the GameMode still needs a live pawn to read the death location from and to credit the
		// death against. The corpse is frozen by SetDeadPresentation(); when the engine tests the
		// kill Z again on a later frame this character is no longer alive, so the call falls through
		// to Super and the body is cleaned up then.
		return;
	}

	Super::FellOutOfWorld(DmgType);
}

void ATraceCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None: every client needs this, not just the owner — it drives the carrier tint, the HUD
	// banner and the trail's "who is the carrier" question.
	DOREPLIFETIME(ATraceCharacter, bIsCarrier);

	// COND_SkipOwner: the owning client predicts its own slide from the saved move and already has a
	// better answer than any packet could carry. Everyone ELSE has no way to know at all — see the
	// property's comment; without this a sliding player's hit zones desync from what a shooter sees.
	DOREPLIFETIME_CONDITION(ATraceCharacter, bReplicatedSliding, COND_SkipOwner);
}

// =================================================================================================
// Queries
// =================================================================================================

ETraceTeam ATraceCharacter::GetTeam() const
{
	// Team lives on the PlayerState because it outlives the pawn (contract §7). Null until the
	// PlayerState has replicated, which is why ApplyTeamColors() is re-entrant and polled.
	if (const ATracePlayerState* State = GetPlayerState<ATracePlayerState>())
	{
		return State->Team;
	}
	return ETraceTeam::None;
}

bool ATraceCharacter::IsCarrier() const
{
	return bIsCarrier;
}

bool ATraceCharacter::IsAlive() const
{
	return Health != nullptr && Health->IsAlive();
}

bool ATraceCharacter::IsDashing() const
{
	const UTraceCharacterMovementComponent* Movement = GetTraceMovement();
	return Movement != nullptr && Movement->IsDashing();
}

bool ATraceCharacter::AreWeaponActionsBlocked() const
{
	// Spec v10 §6. See the header for why this is not simply IsDashing() at the call site.
	const UTraceCharacterMovementComponent* Movement = GetTraceMovement();
	return Movement != nullptr && Movement->AreWeaponActionsBlocked();
}

UTraceCharacterMovementComponent* ATraceCharacter::GetTraceMovement() const
{
	// Not cached: Cast<> on a known-typed subobject is a pointer compare against the class chain,
	// and caching a component pointer in a UPROPERTY set from the constructor is a subtler thing
	// than it looks when the CDO is involved.
	return Cast<UTraceCharacterMovementComponent>(GetCharacterMovement());
}

#if !UE_BUILD_SHIPPING
/**
 * Dev only. Forces the third-person camera regardless of the Core, so the procedural slide pose can
 * actually be LOOKED AT.
 *
 * It exists because the pose is the one part of spec v4 §1 that cannot be verified from a log line,
 * and the pawn a headless run drives is in first person — where its own body is deliberately hidden.
 * Bots are the only other sliding bodies and they are specks at arena scale. Cheat-flagged, compiled
 * out of shipping, and it changes nothing but which camera the local player looks through.
 */
int32 GTraceForceThirdPerson = 0;
static FAutoConsoleVariableRef CVarTraceForceThirdPerson(
	TEXT("Trace.ForceThirdPerson"),
	GTraceForceThirdPerson,
	TEXT("Dev only. Non-zero forces the third-person camera on the local player, so the slide pose and "
	     "the carry blend can be inspected without holding the Core."),
	ECVF_Cheat);
#endif

bool ATraceCharacter::WantsFirstPersonView() const
{
#if !UE_BUILD_SHIPPING
	if (GTraceForceThirdPerson != 0)
	{
		return false;
	}
#endif

	// The entire rule. Carrying the Core is the one state that needs the space behind you visible,
	// because the trail you are laying there is the only thing that can kill you.
	return !bIsCarrier;
}

float ATraceCharacter::GetViewBlendAlpha() const
{
	return ViewBlendAlpha;
}

int32 ATraceCharacter::GetViewModelPartCount() const
{
	return ViewModelParts.Num();
}

bool ATraceCharacter::IsViewModelVisible() const
{
	return bViewModelVisible;
}

FVector ATraceCharacter::GetMuzzleLocation() const
{
	// ON THE AIM RAY. The eye, stepped forward along the full aim rotation — pitch included, unlike
	// the old chest muzzle which used yaw only and therefore sat off the ray whenever the player
	// looked anywhere but the horizon.
	//
	// GetPawnViewLocation() (actor + BaseEyeHeight) rather than the camera component's world
	// location, deliberately and for the same reason as in GetAimDirection(): it is pure arithmetic
	// on the actor transform, so the server computes the same origin as the client. The camera's
	// real position depends on the spring arm's collision probe and on where the view blend happens
	// to be this frame, neither of which the server knows or should know.
	//
	// Every shot in the game is fired from first person (a carrier cannot fire), so this is the
	// first-person muzzle and there is no second case to keep in step.
	const FVector ViewForward = ResolveAimRotation(*this).Vector();

	return GetPawnViewLocation() + ViewForward * TraceCharacterLayout::MuzzleForward;
}

FVector ATraceCharacter::GetAimDirection() const
{
	const FRotator AimRotation = ResolveAimRotation(*this);
	const FVector ViewForward = AimRotation.Vector();

	// Aim at a point far along the *view* ray and shoot from the muzzle toward it, so the shot
	// converges on whatever the crosshair covers wherever the muzzle happens to be.
	//
	// Since the muzzle was moved onto the ray itself this is arithmetically the identity — the
	// focus point, the muzzle and the eye are collinear, so Converged == ViewForward to float
	// precision. That is the point: the crosshair is exact in first person, and it stays exact if
	// anyone later moves the muzzle to a weapon socket off the axis.
	//
	// GetPawnViewLocation() is used rather than the camera component's world location on purpose: it
	// is pure arithmetic on the actor transform, so the server computes the same answer as the
	// client. The camera's real position depends on the spring arm's collision probe, which can and
	// does differ between machines.
	const FVector FocusPoint = GetPawnViewLocation() + ViewForward * TraceCharacterLayout::AimConvergenceDistance;
	const FVector Converged = (FocusPoint - GetMuzzleLocation()).GetSafeNormal();

	return Converged.IsNearlyZero() ? ViewForward : Converged;
}

// =================================================================================================
// Server-authoritative state
// =================================================================================================

void ATraceCharacter::SetCarrying(bool bNewCarrying)
{
	if (!HasAuthority() || bIsCarrier == bNewCarrying)
	{
		return;
	}

	bIsCarrier = bNewCarrying;

	// THIS IS THE SOLE WRITER of ATracePlayerState::bIsCarrier for a pawn that still exists.
	//
	// It used to be one of FOUR: ATraceCore::GrantTo, ATraceCore::ReleaseHolder and
	// ATraceGameMode::NotifyCharacterDied all wrote the mirror too, and every one of them also called
	// SetCarrying — so three of the four writes were pure duplication, and duplicated state with
	// scattered writers is what the whole-project audit named as the most likely source of the next
	// bug. Those three are gone. If you find yourself adding a fourth, the answer is to call
	// SetCarrying() instead.
	//
	// The ONE case this cannot cover is a holder whose pawn has already been destroyed: the
	// PlayerState outlives the pawn, so there is no pawn left to call. ATraceCore caches the holder's
	// PlayerState at grant time and clears it in ReleaseHolder() for exactly that case — see the
	// comment there. Do not re-add a write here to "fix" it; a destroyed pawn cannot reach this line.
	if (ATracePlayerState* State = GetPlayerState<ATracePlayerState>())
	{
		State->bIsCarrier = bNewCarrying;
		// The scoreboard reads this on every client; without a nudge it waits for the PlayerState's
		// ordinary replication interval, which is long enough to be seen as a wrong scoreboard.
		State->ForceNetUpdate();
	}

	if (Trail != nullptr)
	{
		Trail->SetEmitting(bNewCarrying);
	}

	if (bNewCarrying && Weapon != nullptr)
	{
		// The carrier cannot shoot (contract §3). Dropping the trigger here means a player who was
		// mid-burst when they caught the Core does not resume firing the instant they pass it on.
		Weapon->StopFire();
	}

	// Replication callbacks never fire on the authority; run it by hand so a listen server tints
	// itself exactly like a remote client does.
	OnRep_IsCarrier();

	// Carrier state gates invulnerability and the HUD banner — worth a packet immediately.
	ForceNetUpdate();

	// Display, not Verbose, and one line per possession change is cheap at ten players. bIsCarrier is
	// the ONLY input to the view mode (WantsFirstPersonView), so "the camera is stuck in third
	// person" is always a question about this exact line: either it never ran, or it ran on the
	// server and the owning client never heard about it. Without it, both look identical.
	UE_LOG(LogTraceGame, Display, TEXT("[Carry] %s bIsCarrier -> %d (authority, netmode=%d)"),
		*GetName(), bNewCarrying ? 1 : 0, static_cast<int32>(GetNetMode()));
}

void ATraceCharacter::HandleDeath(AController* Killer, FName Cause)
{
	if (!HasAuthority() || bDeathHandled)
	{
		return;
	}
	bDeathHandled = true;

	if (Weapon != nullptr)
	{
		Weapon->StopFire();
	}

	// The trail is the carrier's threat and it must not outlive them by even a frame (contract §3:
	// "on carrier death the trail is cleared instantly"). Non-carriers have an empty trail, so this
	// is unconditional and cheap. The GameMode repeats it; both calls are idempotent.
	if (Trail != nullptr)
	{
		Trail->SetEmitting(false);
		Trail->ClearTrail();
	}

	// Normally already applied — Health::Kill/ApplyDamage call OnRep_Health on the server, which
	// routes here. Repeating it covers a direct HandleDeath() call and costs nothing.
	SetDeadPresentation(true);

	UE_LOG(LogTraceGame, Verbose, TEXT("%s died (%s)"), *GetName(), *Cause.ToString());

	// Last, and while still possessed: the GameMode drops the Core at this location, credits the
	// kill from the still-attached PlayerState and schedules the respawn.
	if (UWorld* World = GetWorld())
	{
		if (ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
		{
			GameMode->NotifyCharacterDied(this, Killer, Cause);
		}
	}
}

void ATraceCharacter::OnHealthDeath(AActor* Victim, AController* Killer, FName Cause)
{
	// Victim is always this actor; the delegate carries it for listeners that bind to many pawns.
	HandleDeath(Killer, Cause);
}

void ATraceCharacter::SetDeadPresentation(bool bDead)
{
	if (bDeadPresentation == bDead)
	{
		return;
	}
	bDeadPresentation = bDead;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		if (bDead)
		{
			Movement->StopMovementImmediately();
			Movement->DisableMovement();
		}
		else
		{
			Movement->SetMovementMode(MOVE_Walking);
		}
	}

	if (!bDead)
	{
		// Coming back to life re-arms the death latch. The GameMode normally destroys a corpse and
		// spawns a fresh pawn, but nothing in the contract forbids reviving one in place — and a
		// revived pawn that could never die again would be a very quiet bug.
		bDeathHandled = false;
	}

	// Corpses stop blocking: bullets, dashes and the endzone trigger all pass straight through one.
	// This runs on every machine (see the header) so no client is left colliding with something the
	// server does not, which would otherwise show up as rubber-banding around a body.
	// The visual meshes are NoCollision in their own right and stay that way through this toggle.
	SetActorEnableCollision(!bDead);

	// And the body itself goes, on the same frame, on every machine. There is no dying animation to
	// wait for and no ragdoll to settle: the pawn is already frozen and already non-colliding, so a
	// lingering mesh is a prop that reads as a live player from any distance at which you cannot see
	// that it is dimmed. Deliberately AFTER the collision toggle, so nothing can ever be invisible
	// and still solid.
	SetCorpseHidden(bDead);

	ApplyTeamColors();
}

void ATraceCharacter::SetCorpseHidden(bool bInHidden)
{
	// Note the parameter name. A local or parameter called bHidden would shadow AActor's own member
	// and fail the Windows build on C4458 — this file has already paid that toll once.

	// One flag, every primitive this actor owns: the mannequin, both fallback shapes, the skid
	// streak, the pooled trail meshes and every viewmodel part. See the header for why this is done
	// at the actor level rather than component by component.
	SetActorHiddenInGame(bInHidden);

	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		// bCastHiddenShadow is set in the constructor so that a first-person player, whose own mesh is
		// hidden from their own camera, still casts a shadow they can find their feet by. On a corpse
		// that same flag is a bug with a very obvious symptom: the body disappears and a body-shaped
		// shadow stays standing on the floor. Cleared while dead, restored on revive.
		MeshComp->SetCastHiddenShadow(!bInHidden);

		// A hidden mesh under OnlyTickPoseWhenRendered stops evaluating anyway; saying so explicitly
		// keeps a dead pawn from being the one skeleton still animating because it happened to be the
		// local player's (SetOwnBodyHiddenFromOwner puts that one on AlwaysTickPoseAndRefreshBones).
		if (bInHidden)
		{
			MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
		}
	}

	if (bInHidden)
	{
		// The viewmodel hangs off the CAMERA, and the camera is still the local player's view target
		// while the respawn timer runs — so its own visibility bool has to be brought in line, or
		// UpdateViewBlend would go on believing the gun is shown and skip the restore.
		SetViewModelVisible(false);
	}

	// Display, not Verbose. This project has twice declared a mechanic dead because the only line
	// proving it ran was suppressed at the default verbosity of an automated run — and "did the body
	// actually go away on every machine" is precisely the kind of claim a screenshot cannot settle
	// for the nine pawns that were off camera. One line per death and one per respawn.
	UE_LOG(LogTraceGame, Display, TEXT("[Corpse] %s %s (hiddenInGame=%d, alive=%d, authority=%d)"),
		*GetName(),
		bInHidden ? TEXT("hidden on death") : TEXT("shown on respawn"),
		IsHidden() ? 1 : 0,
		IsAlive() ? 1 : 0,
		HasAuthority() ? 1 : 0);

	// NOT touched here: the Core. It is a separate actor that merely attaches to its holder, it is
	// still in play, and hiding it with the body would blank the one object the whole match is about.
	// ATraceCore's own holder-sanity pass releases it from a dead carrier (see its Tick), which is
	// what makes the orb leave the corpse.
	//
	// NOT touched either: lag compensation. UTraceLagCompensationComponent records the CAPSULE pose,
	// which is a transform and is unaffected by visibility; it also skips targets that are not alive.
	// Hit registration for everyone still breathing is untouched by any of this.
}


void ATraceCharacter::OnRep_IsCarrier()
{
	ApplyTeamColors();

	// The remote half of the [Carry] line in SetCarrying(). On a client this is the only proof the
	// new value actually arrived — and a replicated bool re-set to the value it already holds fires
	// no OnRep at all, so a missing line here next to a present one there IS the bug report.
	if (!HasAuthority())
	{
		UE_LOG(LogTraceGame, Display, TEXT("[Carry] %s bIsCarrier -> %d (replicated to client)"),
			*GetName(), bIsCarrier ? 1 : 0);
	}

	// The camera is NOT moved from here. UpdateViewBlend() reads the carrier state every frame and
	// walks toward it, which means the transition is identical whether the state arrived by
	// replication, by SetCarrying() on a listen host, or by the Core being passed away — and there
	// is no path that can leave the camera stranded in the wrong mode because a callback did not
	// fire on some machine.
}

// =================================================================================================
// View mode — first person, third person while carrying. See the file header.
// =================================================================================================

float ATraceCharacter::GetThirdPersonPivotZ() const
{
	// Resolved through the trail component rather than off UTraceSettings::TrailHeight directly, so
	// that the height the camera clears is the height the trail is ACTUALLY built at — the console
	// override Trace.Trail.Height moves both together, and the two can never disagree.
	//
	// UTraceTrailComponent builds its wall centred on the carrier's actor location and rides a cap
	// strip of clamp(Height * 0.12, 14, 42) on top of it, so the highest lit surface sits half the
	// height plus half the cap above the actor centre — and TargetOffset.Z is measured from that
	// same actor centre.
	const float TrailHeight = FMath::Max(0.f, UTraceTrailComponent::GetTraceTrailHeight());
	const float TrailTopAboveCentre = TrailHeight * 0.5f + FMath::Clamp(TrailHeight * 0.12f, 14.f, 42.f) * 0.5f;

	// Max, not a plain sum, and the floor is CarryPivotZ rather than the generic ThirdPersonPivotZ.
	// Spec v7 §3 cut the trail to a third of its height, and without this floor that visibility change
	// would have dragged the approved carry framing down 68uu with it. See CarryPivotZ for the whole
	// argument and for the one-line way to reverse this decision.
	return FMath::Max(
		TraceCharacterLayout::CarryPivotZ,
		TrailTopAboveCentre + TraceCharacterLayout::TrailCameraClearance);
}

void ATraceCharacter::UpdateViewBlend(float DeltaSeconds, bool bSnap)
{
	// Pure presentation: a dedicated server has no camera, renders nothing, and must not spend a
	// frame of anyone's time on this.
	if (SpringArm == nullptr || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// This pawn's camera is only ever looked through by the machine that controls it. On every other
	// machine the arm is inert scenery — but the owner-visibility flag is NOT inert, so if this pawn
	// ever was ours (a listen host respawning, a controller changing hands) the body has to be put
	// back before we stop maintaining it.
	if (!IsLocalPlayerPawn(*this))
	{
		SetOwnBodyHiddenFromOwner(false);
		SetViewModelVisible(false);
		return;
	}

	// The first frame this pawn turns out to be the one a human is inside is the first frame it is
	// worth building a gun for. See EnsureViewModelBuilt() for why this is lazy.
	EnsureViewModelBuilt();

	const float Target = WantsFirstPersonView() ? 0.f : 1.f;

	if (bSnap || TraceCharacterLayout::ViewBlendSeconds <= 0.f)
	{
		ViewBlendAlpha = Target;
	}
	else
	{
		// Constant rate, so the pull-back always takes exactly ViewBlendSeconds however far through a
		// previous blend it was interrupted — a player who grabs the Core, passes it and catches it
		// again inside 0.7 s gets a continuous camera, not a stutter.
		ViewBlendAlpha = FMath::FInterpConstantTo(
			ViewBlendAlpha, Target, DeltaSeconds, 1.f / TraceCharacterLayout::ViewBlendSeconds);
	}

	// Smoothstep. The easing is the difference between "the camera was yanked" and "the camera
	// pulled back": it leaves and arrives at zero velocity, so neither end of the move reads as a cut.
	const float Eased = ViewBlendAlpha * ViewBlendAlpha * (3.f - 2.f * ViewBlendAlpha);

	// TargetArmLength 0 puts the camera exactly on the arm origin, and the arm origin is
	// TargetOffset above the capsule centre — which at Eased 0 is precisely GetPawnViewLocation(),
	// the point the aim ray is built from. That equality is the first-person aim guarantee; do not
	// introduce a lateral SocketOffset here without re-reading GetMuzzleLocation().
	SpringArm->TargetArmLength = FMath::Lerp(
		TraceCharacterLayout::FirstPersonArmLength, TraceCharacterLayout::ThirdPersonArmLength, Eased);

	// BaseEyeHeight, not the EyeHeight constant, and this is load-bearing now that sliding exists.
	// GetPawnViewLocation() is actor + BaseEyeHeight and is what GetAimDirection() and
	// GetMuzzleLocation() build the shot from; ACharacter drops BaseEyeHeight to CrouchedEyeHeight
	// the moment a slide starts. Pinning the arm to the standing constant instead would leave the
	// camera 60 uu above the point the bullet leaves from for the whole slide — the crosshair would
	// lie exactly when the player is moving fastest. Reading the live value keeps eyeErr at 0.00 uu
	// in first person standing AND crouched, which is what Trace.DebugViewProbe measures.
	SpringArm->TargetOffset = FVector(0.f, 0.f, FMath::Lerp(
		BaseEyeHeight, GetThirdPersonPivotZ(), Eased));

	// Our own body follows the camera out of the way. Other players' pawns are untouched by this —
	// SetOwnerNoSee hides a mesh from ONE viewer, the one whose view target owns it.
	const bool bFirstPersonNow = (Eased < TraceCharacterLayout::OwnBodyHideAlpha);
	SetOwnBodyHiddenFromOwner(bFirstPersonNow);

	// The gun follows the same switch as the body, so the two can never disagree: the moment the
	// camera pulls back to show the carrier, the viewmodel goes away with it. A corpse holds no gun
	// either — the third condition is what stops a dead player staring at a floating pistol while
	// the respawn timer runs.
	SetViewModelVisible(bFirstPersonNow && !bDeadPresentation && IsAlive());
}

// =================================================================================================
// Crouch / slide
// =================================================================================================

void ATraceCharacter::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	// BEFORE Super — see the note on the declaration. ACharacter::OnStartCrouch is what calls
	// RecalculateBaseEyeHeight(), and that is what copies CrouchedEyeHeight into BaseEyeHeight.
	if (const UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		const float NewHalfHeight = Capsule->GetScaledCapsuleHalfHeight();

		// Clamped INSIDE the capsule (minus a margin) rather than left free: BaseEyeHeight is where
		// the first-person camera and the aim ray both live, and an eye above the top of a crouched
		// capsule would let a slide see over cover the body is genuinely behind.
		const float MaxEye = FMath::Max(4.f, NewHalfHeight - TraceCharacterLayout::CrouchedEyeCapsuleMargin);
		CrouchedEyeHeight = FMath::Clamp(TraceCharacterLayout::CrouchedEyeAboveFeet - NewHalfHeight, 4.f, MaxEye);
	}

	Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
}

void ATraceCharacter::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
}

// -----------------------------------------------------------------------------------------------
// Movement base — the arena is not a moving platform. Full reasoning on the declarations.
// -----------------------------------------------------------------------------------------------

bool ATraceCharacter::ShouldIgnoreAsMovementBase(const UPrimitiveComponent* BaseComponent)
{
	if (BaseComponent == nullptr)
	{
		return false;
	}

	// Cheap out first: anything that is not Movable is already a static base as far as the engine is
	// concerned (MovementBaseUtility::IsDynamicBase is exactly a Movable test), so it never produces
	// a base-relative correction and there is nothing to suppress.
	if (BaseComponent->Mobility != EComponentMobility::Movable)
	{
		return false;
	}

	// The arena and only the arena. Characters and the Core are real dynamic bases.
	return BaseComponent->GetOwner() != nullptr
		&& BaseComponent->GetOwner()->IsA<ATraceArenaBuilder>();
}

void ATraceCharacter::SetBase(FMovementBaseInterfaceData* MovementBaseInterfaceData, const FName BoneName, bool bNotifyActor)
{
	const UPrimitiveComponent* AsPrimitive = (MovementBaseInterfaceData != nullptr)
		? Cast<UPrimitiveComponent>(MovementBaseInterfaceData->GetMovementBaseObject())
		: nullptr;

	if (ShouldIgnoreAsMovementBase(AsPrimitive))
	{
		// Not "skip the call" — the pawn may currently be based on something and has to be told it
		// no longer is, or it keeps a stale base forever.
		FMovementBaseInterfaceData* NoBase = nullptr;
		Super::SetBase(NoBase, NAME_None, bNotifyActor);
		return;
	}

	Super::SetBase(MovementBaseInterfaceData, BoneName, bNotifyActor);
}

void ATraceCharacter::UpdateCrouchPresentation(float DeltaSeconds)
{
	// WHAT "CROUCHED" MEANS IN THIS GAME, and why this does not read bIsCrouched.
	//
	// UTraceCharacterMovementComponent overrides CanCrouchInCurrentState() to ALWAYS RETURN FALSE,
	// on purpose and with a good reason: the capsule is this project's single source of truth for
	// hitscan, for the pose history the server rewinds and for the trail trip test, and a slide that
	// silently halved a pawn's hit height would change all three on the server only, in the middle
	// of the one mechanic the game is about. The crouch key therefore never resizes anything and
	// ACharacter::bIsCrouched is never set — it is consumed as an INPUT and turned into a slide.
	//
	// So the state to present is IsSliding(), not bIsCrouched. Both are checked anyway, so this
	// keeps working if the capsule ever does start shrinking.
	const UTraceCharacterMovementComponent* TraceMovement = GetTraceMovement();
	const bool bLocallySliding = (TraceMovement != nullptr && TraceMovement->IsSliding());

	// The authority is the one machine that knows the truth for every pawn — its own, its bots' and
	// every remote client's, all of which it simulates from the same saved moves. Publishing it here
	// (rather than from the movement component) keeps the write next to the single reader.
	if (HasAuthority())
	{
		bReplicatedSliding = bLocallySliding;
	}

	// bReplicatedSliding is what carries the slide to a THIRD machine: a simulated proxy runs no
	// saved moves, so without it a bystander's copy of a sliding player stands bolt upright and, far
	// worse, lays its hit zones out at standing height (see the property's comment). ORed, never
	// substituted, so the owner's own prediction always wins on the frame it starts.
	const bool bSliding = bLocallySliding || bReplicatedSliding || bIsCrouched;

	// --- The eye ---------------------------------------------------------------------------------
	//
	// The capsule stays put; the EYE does not have to, and it is the half of the crouch the player
	// actually experiences. Driving BaseEyeHeight (rather than dropping the camera on its own) is
	// what keeps the aim honest: GetPawnViewLocation() is actor + BaseEyeHeight, UpdateViewBlend
	// pins the spring arm to the same value, so the camera, the muzzle and the bullet all descend
	// together and Trace.DebugViewProbe still reads eyeErr 0.00 mid-slide. Dropping only the camera
	// would have put the crosshair 34 uu above the shot for the whole slide.
	//
	// This half runs on EVERY machine including a dedicated server, because the server evaluates
	// GetPawnViewLocation() when it resolves a shot; the visual half below does not.
	const float StandingEye = bIsCrouched ? CrouchedEyeHeight : TraceCharacterLayout::EyeHeight;
	const float TargetEye = bSliding
		? FMath::Min(StandingEye, TraceCharacterLayout::SlideEyeHeight)
		: StandingEye;
	BaseEyeHeight = FMath::FInterpTo(BaseEyeHeight, TargetEye, DeltaSeconds,
		TraceCharacterLayout::SlideEyeInterpSpeed);

	if (GetNetMode() == NM_DedicatedServer)
	{
		return;   // nothing is drawn there
	}

	// --- THE SLIDE POSE (spec v4 §1) — PROCEDURAL, BECAUSE THERE IS NO STOCK SLIDE ANIMATION ------
	//
	// The Demo 4 notes ask: "If unreal has a default slide animation for the mannequins, please add
	// that in." IT DOES NOT. The Mannequin set Scripts/import-mannequin.sh brings in is Death, Jump,
	// Pistol, Rifle and Unarmed locomotion — BS_Idle_Walk_Run and MM_Idle — and there is no slide and
	// no crouch anywhere in Templates/TemplateResources or Engine/Content. Nothing below is a shipped
	// Epic animation; it is an approximation assembled from the skeleton the project already has, and
	// a real slide would still have to be authored or bought.
	//
	// Four things together sell it, and the fourth matters most:
	//   RECLINE   the body tips BACK over its own feet (SlidePoseLeanDegrees), feet leading, which is
	//             the Apex/Titanfall silhouette.
	//   DROP      the whole mesh sinks toward the deck (SlidePoseDropUU). The capsule deliberately
	//             does NOT shrink — it is the single source of truth for hitscan, for the pose history
	//             the server rewinds and for the trail trip test — so this is the only thing that
	//             makes a sliding player look low from the outside.
	//   ROLL      a few degrees of lead shoulder (SlidePoseRollDegrees) so the pose is a body
	//             committed to a direction rather than a mannequin tipped back on a hinge.
	//   ANIM RATE the locomotion blend space is slowed almost to a stop (SlidePoseAnimRateScale).
	//             Without this the legs sprint at full cadence underneath the reclined torso, which is
	//             the one thing that unambiguously reads as a bug rather than as a move; near zero
	//             they freeze mid-stride, which is close enough to "extended into a slide" to sell.
	//
	// All of it is driven off the same eased CrouchLeanAlpha, which is derived from the slide state —
	// client-predicted and server-simulated from the same saved moves — so a bystander's copy reaches
	// this pose on its own, with no RPC and no presentation flag, and it cannot disagree with the
	// movement that caused it. And all of it is visual: nothing here feeds the simulation or moves a
	// hit zone.
	const UTraceSettings& PoseSettings = UTraceSettings::Get();

	const float LeanTarget = bSliding ? 1.f : 0.f;
	const float PreviousLean = CrouchLeanAlpha;
	CrouchLeanAlpha = FMath::FInterpTo(CrouchLeanAlpha, LeanTarget, DeltaSeconds,
		FMath::Max(1.f, PoseSettings.SlidePoseBlendSpeed));

	// Composed as Pose * Yaw, not as a rotator sum: the mesh already carries BodyMeshYaw (-90 for the
	// Mannequin) to point its authored forward down the actor's +X, and adding a pitch to a rotator that is already yawed 90
	// degrees rolls the character sideways instead of tipping it back. Quaternion order in Unreal is
	// "apply the right one first", so this yaws the mesh into place and THEN pitches and rolls it
	// about the ACTOR's own Y and X axes, which are the axes a body leans and lists about.
	//
	// The pivot is the mesh origin, which sits at the bottom of the capsule (MeshOffsetZ), i.e. at the
	// feet — so the body tips over its feet rather than swinging about its waist, and the head travels
	// while the feet stay planted on the skid streak.
	if (FMath::Abs(CrouchLeanAlpha - PreviousLean) > 0.001f)
	{
		if (USkeletalMeshComponent* MeshComp = GetMesh())
		{
			const float LeanDegrees = FMath::Clamp(PoseSettings.SlidePoseLeanDegrees, 0.f, 70.f);
			const float RollDegrees = FMath::Clamp(PoseSettings.SlidePoseRollDegrees, -40.f, 40.f);
			const float DropUU      = FMath::Clamp(PoseSettings.SlidePoseDropUU, 0.f, 80.f);

			// BodyMeshYaw, not the Mannequin's constant: this pawn's body depends on which character
			// is playing it, and Rocco's rig needs 0 where the Mannequin needs -90. Using the constant
			// here would leave a Rocco player standing straight until his first slide and sideways
			// forever after it.
			const FQuat YawQuat(FRotator(0.f, BodyMeshYaw, 0.f));
			const FQuat PoseQuat(FRotator(
				CrouchLeanAlpha * LeanDegrees,   // pitch: positive tips the body BACK, feet leading
				0.f,
				CrouchLeanAlpha * RollDegrees)); // roll: the lead shoulder
			MeshComp->SetRelativeRotation(PoseQuat * YawQuat);

			// Straight down from the standing offset. Kept modest and clamped because the feet sit at
			// the mesh origin: drop far enough and they go through the deck. The recline is already
			// bringing the head down by roughly (1 - cos(lean)) of the body height, so this only has to
			// finish the job.
			MeshComp->SetRelativeLocation(FVector(
				0.f, 0.f, TraceCharacterLayout::MeshOffsetZ - CrouchLeanAlpha * DropUU));

			// THE ONE THAT STOPS IT LOOKING LIKE A BUG. Lerped rather than switched so the legs wind
			// down into the slide and spin back up out of it, instead of snapping.
			MeshComp->GlobalAnimRateScale = FMath::Lerp(
				1.f, FMath::Clamp(PoseSettings.SlidePoseAnimRateScale, 0.f, 1.f), CrouchLeanAlpha);
		}
	}

	// --- The skid streak -------------------------------------------------------------------------
	//
	// Only while crouched, on the ground AND actually moving: a stationary crouch is a crouch, and a
	// streak under a player who is standing still would be nonsense. Scaled by speed so the streak
	// grows out of nothing as a slide starts and dies as it runs out, which is the whole read.
	if (SlideSkidMesh == nullptr)
	{
		return;
	}

	const UCharacterMovementComponent* Movement = GetCharacterMovement();
	const bool bGrounded = (Movement != nullptr) && Movement->IsMovingOnGround();
	const float PlanarSpeed = GetVelocity().Size2D();
	const float SkidTarget = (bSliding && bGrounded && IsAlive())
		? FMath::Clamp(PlanarSpeed / TraceCharacterLayout::SkidFullSpeed, 0.f, 1.f)
		: 0.f;

	SkidGlowAlpha = FMath::FInterpTo(SkidGlowAlpha, SkidTarget, DeltaSeconds, 8.f);

	const bool bShowSkid = SkidGlowAlpha > 0.05f;
	if (SlideSkidMesh->IsVisible() != bShowSkid)
	{
		SlideSkidMesh->SetVisibility(bShowSkid);
	}
	if (!bShowSkid)
	{
		return;
	}

	// Sits under the crouched capsule, not the standing one: the capsule really did shrink, so the
	// streak has to come up with it or it would be drawn inside the floor.
	const float FeetZ = (GetCapsuleComponent() != nullptr)
		? -GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3.f
		: -TraceCharacterLayout::CapsuleHalfHeight + 3.f;

	// Trailing BEHIND the pawn, which is where a skid mark belongs. Local +X is the actor's forward
	// and the actor faces its movement while sliding.
	SlideSkidMesh->SetRelativeLocation(
		FVector(-TraceCharacterLayout::SkidLength * 0.5f * SkidGlowAlpha, 0.f, FeetZ));
	SlideSkidMesh->SetRelativeScale3D(FVector(
		TraceCharacterLayout::SkidLength * SkidGlowAlpha / TraceCharacterLayout::ViewModelShapeUnit,
		TraceCharacterLayout::SkidWidth / TraceCharacterLayout::ViewModelShapeUnit,
		TraceCharacterLayout::SkidThickness / TraceCharacterLayout::ViewModelShapeUnit));
}

void ATraceCharacter::ApplyRotationMode()
{
	// Only a human in first person turns their body with their aim.
	//
	// A bot is never looked out of, and ABP_Unarmed drives a SPEED-only blend space: a pawn whose
	// body faces its aim while it strafes plays a forward run sideways. Nobody sees that on their own
	// character (it is hidden in first person, and a carrier faces its movement), but everyone sees
	// it on all nine other characters — so bots stay on orient-to-movement in both modes. This is a
	// deliberate asymmetry, not an oversight.
	const bool bHumanControlled = (Cast<APlayerController>(GetController()) != nullptr);
	const bool bFirstPersonRotation = bHumanControlled && WantsFirstPersonView();

	if (bRotationModeApplied && bRotationModeIsFirstPerson == bFirstPersonRotation)
	{
		return;
	}
	bRotationModeApplied = true;
	bRotationModeIsFirstPerson = bFirstPersonRotation;

	// These two are exclusive by construction: leaving both on makes the movement component and the
	// controller fight over the same yaw every frame.
	bUseControllerRotationYaw = bFirstPersonRotation;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->bOrientRotationToMovement = !bFirstPersonRotation;
	}
}

void ATraceCharacter::SetOwnBodyHiddenFromOwner(bool bInHidden)
{
	// Guarded because SetOwnerNoSee marks the render state dirty; the blend calls this every frame.
	if (bOwnBodyHiddenFromOwner == bInHidden)
	{
		return;
	}
	bOwnBodyHiddenFromOwner = bInHidden;

	// All three visual components, because which of them is showing depends on whether the mannequin
	// import has been run — a fresh clone in first person must not be staring at the inside of a
	// fallback cylinder.
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetOwnerNoSee(bInHidden);

		// Every other pawn keeps OnlyTickPoseWhenRendered, which is the big animation saving in a
		// 5v5. This one pawn cannot: in first person it is deliberately not drawn for the only
		// viewer there is, so "when rendered" would mean "never" — its shadow (bCastHiddenShadow,
		// set in the constructor) would freeze mid-stride, and the pose would still be stale on the
		// frame the Core is picked up and the body swings back into view. One always-ticked
		// skeleton out of ten is a price worth paying for both of those.
		MeshComp->VisibilityBasedAnimTickOption = bInHidden
			? EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
			: EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	}
	if (FallbackBodyMesh != nullptr)
	{
		FallbackBodyMesh->SetOwnerNoSee(bInHidden);
	}
	if (FallbackHeadMesh != nullptr)
	{
		FallbackHeadMesh->SetOwnerNoSee(bInHidden);
	}
}

// =================================================================================================
// Input entry points
// =================================================================================================

void ATraceCharacter::DoMove(const FVector2D& Value)
{
	if (Controller == nullptr || Value.IsNearlyZero())
	{
		return;
	}

	// Movement is relative to where the camera looks, never to where the capsule faces: the capsule
	// is busy turning to follow the movement (bOrientRotationToMovement), so feeding its own
	// rotation back in as the input basis would make it spiral.
	const FRotationMatrix YawBasis(FRotator(0.f, GetControlRotation().Yaw, 0.f));

	AddMovementInput(YawBasis.GetUnitAxis(EAxis::X), Value.Y);   // W/S
	AddMovementInput(YawBasis.GetUnitAxis(EAxis::Y), Value.X);   // A/D
}

void ATraceCharacter::DoLook(const FVector2D& Value)
{
	// Both signs are already correct on arrival: the Look mapping's Y scalar carries BOTH the
	// vertical sensitivity and the invert-Y sign (Settings/TraceUserSettings). Nothing else in the
	// chain flips a sign — with bEnableLegacyInputScales=False, AddControllerPitchInput's multiplier
	// is +1 and raw MouseY is already positive when the mouse moves up. Do not add a negate here.
	AddControllerYawInput(Value.X);
	AddControllerPitchInput(Value.Y);
}

void ATraceCharacter::DoFirePressed()
{
	// SPEC §4. Carrying the Core overloads mouse1: it passes instead of shooting, and the gun stays
	// silent. Returning here (rather than also calling StartFire) is what guarantees the "cannot
	// shoot while carrying" rule holds even if the weapon's own gate is ever relaxed.
	if (bIsCarrier)
	{
		DoPassPressed();
		return;
	}

	// The weapon owns every remaining rule about whether the shot is legal (dead, fire rate). The
	// pawn just forwards the trigger.
	if (Weapon != nullptr)
	{
		Weapon->StartFire();
	}
}

void ATraceCharacter::DoFireReleased()
{
	// UNCONDITIONAL, both halves. The comment here used to say "the release ALWAYS propagates" and
	// then gate the pass half on bIsCarrier, which is the one state guaranteed to be wrong by the
	// time it is read: a completed pass clears bIsCarrier on this pawn BEFORE the player's finger
	// leaves the button, so the gate swallowed exactly the release it existed to deliver. See
	// DoPassReleased(), whose own comment has always said this.
	//
	// Safe to send unconditionally now that ATraceCore::RequestPassInput takes the requester and
	// decides for itself whose button it is - a non-holder's mouse1 release can no longer cancel
	// the holder's pass, which is what made the gate look necessary in the first place.
	DoPassReleased();

	if (Weapon != nullptr)
	{
		Weapon->StopFire();
	}
}

void ATraceCharacter::DoPassPressed()
{
	// No local bIsCarrier gate. ATraceCore owns "may this pawn arm the pass", it checks the Core's
	// own idea of who is holding rather than a replicated mirror of it, and it re-checks possession,
	// range, line of sight and the aim cone on the server every tick of the hold. A second copy of
	// the rule here could only ever disagree with the first.
	if (ATraceCore* TheCore = ATraceCore::Get(GetWorld()))
	{
		TheCore->RequestPassInput(true, this);
	}
}

void ATraceCharacter::DoPassReleased()
{
	// NOT gated on bIsCarrier: a completed pass clears bIsCarrier on this pawn before the player's
	// finger leaves the button, and the Core still needs to hear the release so bPassInputHeld does
	// not stay latched into the next possession. Passing `this` is what makes that safe — the Core
	// matches the release against whoever armed the latch, so this pawn can still deliver its own
	// release after losing the Core, while a non-holder's release cannot touch anybody else's pass.
	if (ATraceCore* TheCore = ATraceCore::Get(GetWorld()))
	{
		TheCore->RequestPassInput(false, this);
	}
}

float ATraceCharacter::GetPassProgress() const
{
	// Negative means "no pass in progress" — the HUD's contract, see the header. Only the holder
	// ever reports progress, so a spectator or a teammate draws nothing.
	if (!bIsCarrier)
	{
		return -1.f;
	}

	const ATraceCore* TheCore = ATraceCore::Get(GetWorld());
	if (TheCore == nullptr || TheCore->GetCarrier() != this || !TheCore->IsPassActive())
	{
		return -1.f;
	}

	return FMath::Clamp(TheCore->GetPassProgress(), 0.f, 1.f);
}

float ATraceCharacter::GetHitZonePostureScale() const
{
	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	if (Capsule == nullptr)
	{
		return 1.f;
	}

	// Both heights are measured from the FEET, which is where the zone model lays its bands out
	// from. Standing is the constant rather than the live capsule's own default so that a pawn whose
	// capsule is ever resized still reports 1.0 when it is upright.
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const float StandingAboveFeet = HalfHeight + TraceCharacterLayout::EyeHeight;
	if (StandingAboveFeet <= KINDA_SMALL_NUMBER)
	{
		return 1.f;
	}

	const float CurrentAboveFeet = HalfHeight + BaseEyeHeight;

	// Floored well above zero: a degenerate posture would collapse the head sphere onto the feet and
	// turn a leg shot into a one-shot kill. 0.5 is far below anything the slide can produce (0.776).
	return FMath::Clamp(CurrentAboveFeet / StandingAboveFeet, 0.5f, 1.f);
}

// DELETED THIS PASS: ServerPass / PerformPass / DoPass.
//
// All three were legacy and all three had ZERO callers. The pass has been a held hover evaluated
// entirely on the server from the holder's own aim (ATraceCore::ServerTickPass) for two passes now;
// the direction a client sends is precisely the thing the server must not trust, so PerformPass's
// whole payload was unusable. DoPass() was a one-line alias for DoPassPressed().
//
// ServerPass was kept "in case an old client build calls it". There are no shipped clients, and a
// declared reliable server RPC with no callers is a surface a modified client can burn a reliable
// slot on every frame — rate-limited, so not a hole, but cost for nothing. The rate limiter
// (LastPassRequestTime / MinPassRequestInterval) went with it.
//
// The live path is DoPassPressed / DoPassReleased -> ATraceCore::RequestPassInput, which has its own
// server RPC (ATraceCore::ServerSetPassInput) that re-enters the same validation. One door.

void ATraceCharacter::DoDash()
{
	if (!IsAlive())
	{
		return;
	}

	if (UTraceCharacterMovementComponent* Movement = GetTraceMovement())
	{
		// No RPC. StartDash() raises a flag that the next saved move packs into FLAG_Custom_0, so the
		// dash is simulated locally this frame and reproduced identically by the server from the
		// ordinary ServerMove — that is what makes it predicted rather than merely responsive.
		Movement->StartDash();
	}
}

// BOOST IS GONE (spec §1: "remove boost from the game entirely"). ATraceCharacter::DoBoost() used to
// live here and forwarded to UTraceCharacterMovementComponent::StartBoost(). It was deleted with the
// rest of the feature; see the report for the two callers outside this file that must go with it
// (ATracePlayerController::OnBoostStarted and the IA_Boost binding).

// =================================================================================================
// Parry (spec §3) — input routing only
// =================================================================================================
//
// THE MECHANIC IS NOT HERE AND MUST NOT MOVE HERE. Gameplay/TraceParry.h is the policy and entry
// point (duration, cooldown, refusal reasons, the red tint) and UTraceTrailComponent owns the
// window itself, because trace invulnerability and trace colour are already its two jobs and the
// pass window's invulnerability already lives beside it — the two compose there by OR instead of
// fighting over a flag on the pawn. This function is the pawn-side doorway and nothing more.
//
// TraceParry::RequestParry() is documented as "THE ONE ENTRY POINT. Wire the parry bind, the bots
// and any debug command to this", and it is safe from any machine: it refuses non-carriers, refuses
// a cooldown, predicts the red tint on the owning client and sends the server RPC itself. So there
// is deliberately NO ServerParry RPC on this class — a second path to the same window is how the
// prediction and the authoritative window end up disagreeing.

void ATraceCharacter::DoParryPressed()
{
	// Every rule (carrier-only, cooldown, death) belongs to TraceParry::RequestParry, which reports
	// them through ETraceParryRefusal. Duplicating any of them here would give the mechanic two
	// policies to keep in step; the ONE thing checked locally is that there is a pawn to parry with,
	// and RequestParry treats even a null pawn as a refusal rather than a crash.
	ETraceParryRefusal Refusal = ETraceParryRefusal::None;
	TraceParry::RequestParry(this, &Refusal);

	// Verbose and refusal-only: a non-carrier holding the key is the normal case, not an error, and
	// with ten pawns in a match a Display line per press would be per-frame noise. Trace.DebugParry
	// (Gameplay/TraceParry.cpp) is the loud diagnostic when one is actually wanted.
	UE_LOG(LogTraceGame, Verbose, TEXT("[%s] parry input: %s"), *GetName(), LexToString(Refusal));
}

void ATraceCharacter::DoParryReleased()
{
	// Intentionally empty — see the header. Parry is a tap; the window length is TraceParry's and
	// holding the key must not extend it.
}

bool ATraceCharacter::IsParryActive() const
{
	return TraceParry::IsParryActiveFor(this);
}

bool ATraceCharacter::GetParryHudState(float& OutRemaining, float& OutTotal, bool& bOutActive) const
{
	OutRemaining = FMath::Max(0.f, TraceParry::GetCooldownRemainingFor(this));
	OutTotal = FMath::Max(KINDA_SMALL_NUMBER, TraceParry::GetCooldownTotal());
	bOutActive = TraceParry::IsParryActiveFor(this);
	return true;
}
