// Trace — the player pawn. See TraceCharacter.h for the shape of the thing and why.

#include "Core/TraceCharacter.h"

#include "Net/UnrealNetwork.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/SpringArmComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

#include "Core/TraceGameMode.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "Gameplay/TraceWeaponComponent.h"
#include "Movement/TraceCharacterMovementComponent.h"
#include "Net/TraceLagCompensationComponent.h"
#include "Trace.h"
#include "TraceSettings.h"

namespace TraceCharacterLayout
{
	// The capsule is the collider and therefore the single source of truth for the character's
	// size — hitscan, the trail trip test and lag compensation all reason about it. Every visual
	// dimension below is derived from these two numbers so the meshes can never drift away from
	// what the game actually tests against.
	constexpr float CapsuleRadius = 34.f;
	constexpr float CapsuleHalfHeight = 88.f;

	/** Engine basic shapes are 100 uu cubes/cylinders/spheres, so scale = desired size / 100. */
	constexpr float BasicShapeSize = 100.f;

	constexpr float BodyHeight = 136.f;
	constexpr float HeadDiameter = 62.f;

	/** Head centre, chosen so the top of the sphere lands level with the top of the capsule. */
	constexpr float HeadCentreZ = CapsuleHalfHeight - (HeadDiameter * 0.5f);

	constexpr float NoseSize = 38.f;
	constexpr float NoseForward = 24.f;

	constexpr float SpringArmLength = 450.f;
	constexpr float SpringArmPivotZ = 60.f;
	constexpr float CameraFOV = 95.f;

	/** Muzzle: chest height, pushed out along the aim yaw so tracers do not start inside the body. */
	constexpr float MuzzleHeight = 40.f;
	constexpr float MuzzleForward = 45.f;

	/**
	 * Distance at which the muzzle ray is made to meet the camera ray. Anything past this converges
	 * to within a fraction of a degree; anything much closer than this is at point-blank range where
	 * the parallax is invisible anyway.
	 */
	constexpr float AimConvergenceDistance = 6000.f;

	/** Team-colour polling: PlayerState and Team arrive in an order nothing can rely on. */
	constexpr float TeamColorPollInterval = 0.25f;
	constexpr int32 MaxTeamColorAttempts = 24;   // ~6 s, then give up and stay grey
}

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
	FRotator ResolveAimRotation(const ATraceCharacter& Character)
	{
		if (const AController* OwningController = Character.GetController())
		{
			return OwningController->GetControlRotation();
		}

		FRotator Rotation = Character.GetActorRotation();
		Rotation.Pitch = 0.f;
		Rotation.Roll = 0.f;
		return Rotation;
	}

	/** Clamps a tint back into a sane range and forces opaque alpha (colours get scaled about). */
	FLinearColor SanitizeTint(const FLinearColor& InColor)
	{
		return FLinearColor(
			FMath::Clamp(InColor.R, 0.f, 1.f),
			FMath::Clamp(InColor.G, 0.f, 1.f),
			FMath::Clamp(InColor.B, 0.f, 1.f),
			1.f);
	}

	/** Shared setup for the three decorative meshes: visible, and incapable of colliding. */
	void ConfigureVisualMesh(UStaticMeshComponent* Mesh)
	{
		if (Mesh == nullptr)
		{
			return;
		}

		// Contract §7: the capsule is the ONLY collider. A colliding mesh here would let a bullet
		// stop on "the shoulder" while the lag-compensated capsule test says it missed.
		Mesh->SetCollisionProfileName(TEXT("NoCollision"));
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->bReceivesDecals = false;
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
	// ACharacter's own tick settings are left exactly as the engine configured them. Nothing here
	// needs an actor tick of its own — the movement component ticks itself, the lag-compensation
	// component records its own frames and the trail ticks server-side — but ACharacter::Tick is
	// part of the root-motion/simulated-proxy path and is not ours to switch off.

	bReplicates = true;
	SetReplicateMovement(true);

	// Aim and facing are deliberately independent. The capsule turns toward its movement
	// (bOrientRotationToMovement, set on the movement component) while the camera and the shot
	// direction follow the control rotation — so a player can strafe around a target while
	// shooting at it, and other players can read their momentum from their body.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->InitCapsuleSize(TraceCharacterLayout::CapsuleRadius, TraceCharacterLayout::CapsuleHalfHeight);
		Capsule->SetCollisionProfileName(TEXT("Pawn"));

		// Both sides of an overlap pair must generate events. The endzone trigger and the Core's
		// pickup sphere are the other half; without this the game has no way to score or to pick
		// the Core up.
		Capsule->SetGenerateOverlapEvents(true);
	}

	// No skeletal mesh is used (contract §2 forbids authored assets) — the static shapes below are
	// the character. Make sure the inherited component cannot collide or draw anything.
	if (USkeletalMeshComponent* SkeletalMesh = GetMesh())
	{
		SkeletalMesh->SetCollisionProfileName(TEXT("NoCollision"));
		SkeletalMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SkeletalMesh->SetGenerateOverlapEvents(false);
		SkeletalMesh->SetVisibility(false);
	}

	// --- Camera rig ------------------------------------------------------------------------------

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = TraceCharacterLayout::SpringArmLength;
	// Centred, not over-the-shoulder: a lateral offset makes the muzzle ray and the crosshair
	// disagree at close range, and this prototype has one crosshair dead centre.
	SpringArm->SocketOffset = FVector(0.f, 0.f, TraceCharacterLayout::SpringArmPivotZ);
	SpringArm->bUsePawnControlRotation = true;
	SpringArm->bDoCollisionTest = true;
	// Camera lag would smear the crosshair away from the aim direction the weapon actually uses.
	SpringArm->bEnableCameraLag = false;
	SpringArm->bEnableCameraRotationLag = false;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;   // the arm already applied it
	Camera->SetFieldOfView(TraceCharacterLayout::CameraFOV);

	// --- Visual meshes ---------------------------------------------------------------------------

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(RootComponent);
	ConfigureVisualMesh(BodyMesh);

	HeadMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HeadMesh"));
	HeadMesh->SetupAttachment(RootComponent);
	ConfigureVisualMesh(HeadMesh);

	NoseMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("NoseMesh"));
	NoseMesh->SetupAttachment(RootComponent);
	ConfigureVisualMesh(NoseMesh);

	// Constructor-time FObjectFinders (not runtime LoadObject) on purpose: the reference lands on
	// the CDO, which is what makes the cooker pull these engine assets into a packaged build. A
	// bare runtime load of the same path would resolve in the editor and return null in a package.
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeFinder(TEXT("/Engine/BasicShapes/Cone.Cone"));
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));

		if (CylinderFinder.Succeeded())
		{
			BodyMesh->SetStaticMesh(CylinderFinder.Object);
		}
		if (SphereFinder.Succeeded())
		{
			HeadMesh->SetStaticMesh(SphereFinder.Object);
		}
		if (ConeFinder.Succeeded())
		{
			NoseMesh->SetStaticMesh(ConeFinder.Object);
		}
		if (MaterialFinder.Succeeded())
		{
			BasicShapeMaterial = MaterialFinder.Object;
		}
	}

	// Body: a cylinder standing on the bottom of the capsule, as wide as the capsule is.
	{
		const float BodyScaleXY = (TraceCharacterLayout::CapsuleRadius * 2.f) / TraceCharacterLayout::BasicShapeSize;
		const float BodyScaleZ = TraceCharacterLayout::BodyHeight / TraceCharacterLayout::BasicShapeSize;
		BodyMesh->SetRelativeScale3D(FVector(BodyScaleXY, BodyScaleXY, BodyScaleZ));
		BodyMesh->SetRelativeLocation(FVector(
			0.f, 0.f, (TraceCharacterLayout::BodyHeight * 0.5f) - TraceCharacterLayout::CapsuleHalfHeight));
	}

	// Head: a sphere capping the body, overlapping it slightly so the silhouette reads as one piece.
	{
		const float HeadScale = TraceCharacterLayout::HeadDiameter / TraceCharacterLayout::BasicShapeSize;
		HeadMesh->SetRelativeScale3D(FVector(HeadScale));
		HeadMesh->SetRelativeLocation(FVector(0.f, 0.f, TraceCharacterLayout::HeadCentreZ));
	}

	// Nose: a cone laid on its side so you can read someone's facing across the arena.
	// NOTE: the exact pivot of /Engine/BasicShapes/Cone (base centre vs bounds centre) is not
	// something we can verify without the editor. A pitch of -90 maps the cone's local +Z onto the
	// actor's +X either way, so it always points forward; only its offset along X may want a nudge.
	{
		const float NoseScale = TraceCharacterLayout::NoseSize / TraceCharacterLayout::BasicShapeSize;
		NoseMesh->SetRelativeScale3D(FVector(NoseScale));
		NoseMesh->SetRelativeRotation(FRotator(-90.f, 0.f, 0.f));
		NoseMesh->SetRelativeLocation(FVector(
			TraceCharacterLayout::NoseForward, 0.f, TraceCharacterLayout::HeadCentreZ));
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

void ATraceCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Server side, this is the first moment GetPlayerState() can answer — and therefore the first
	// moment the team is knowable.
	ApplyTeamColors();
}

void ATraceCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	// Client side, the PlayerState pointer can arrive before or after the pawn, and its Team can
	// arrive after that again. Every path that could learn the team ends up here.
	ApplyTeamColors();
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

UTraceCharacterMovementComponent* ATraceCharacter::GetTraceMovement() const
{
	// Not cached: Cast<> on a known-typed subobject is a pointer compare against the class chain,
	// and caching a component pointer in a UPROPERTY set from the constructor is a subtler thing
	// than it looks when the CDO is involved.
	return Cast<UTraceCharacterMovementComponent>(GetCharacterMovement());
}

FVector ATraceCharacter::GetMuzzleLocation() const
{
	// Yaw only: pitching the camera up should not swing the muzzle up out of the character's chest,
	// it should just change where the shot goes.
	const FRotator AimRotation = ResolveAimRotation(*this);
	const FVector Forward = FRotator(0.f, AimRotation.Yaw, 0.f).Vector();

	return GetActorLocation()
		+ FVector(0.f, 0.f, TraceCharacterLayout::MuzzleHeight)
		+ Forward * TraceCharacterLayout::MuzzleForward;
}

FVector ATraceCharacter::GetAimDirection() const
{
	const FRotator AimRotation = ResolveAimRotation(*this);
	const FVector ViewForward = AimRotation.Vector();

	// The muzzle sits below and in front of the eye, so firing straight along the view forward would
	// put every shot slightly under the crosshair. Aim at a point far along the *view* ray instead
	// and shoot from the muzzle toward it — the two rays converge, and by any range that matters the
	// error is a fraction of a degree.
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

	// ATraceCore drives the PlayerState mirror and the trail as well. Doing it here too keeps the
	// pawn internally consistent for any other caller, and both writes are idempotent — the trail
	// component early-outs when the emitting state is unchanged.
	if (ATracePlayerState* State = GetPlayerState<ATracePlayerState>())
	{
		State->bIsCarrier = bNewCarrying;
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

	ApplyTeamColors();
}

// =================================================================================================
// Presentation
// =================================================================================================

void ATraceCharacter::ApplyTeamColors()
{
	// A dedicated server cooks no material shaders and renders nothing, so there is neither a need
	// nor a guarantee that the basic-shape material resolves there.
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	const FLinearColor TeamColor = TraceTeamColor(GetTeam());

	// Carrier reads as "lit up". Pull hard toward white so the Core holder is unmistakable at arena
	// distance, but keep enough hue to tell which team is holding it.
	// Blended per component on purpose. FMath::Lerp<T> expands to `A + Alpha * (B - A)`, i.e. it
	// needs `float * FLinearColor`; whether that free operator exists has moved around across the
	// 5.x line, and UI/TraceHUD.cpp hand-rolls its own colour lerp for exactly this reason. Doing it
	// component-wise costs nothing and cannot be wrong on any engine version.
	FLinearColor BodyColor = TeamColor;
	if (bIsCarrier)
	{
		constexpr float CarrierWhiteBlend = 0.72f;
		BodyColor = FLinearColor(
			FMath::Lerp(TeamColor.R, 1.f, CarrierWhiteBlend),
			FMath::Lerp(TeamColor.G, 1.f, CarrierWhiteBlend),
			FMath::Lerp(TeamColor.B, 1.f, CarrierWhiteBlend),
			1.f);
	}
	FLinearColor HeadColor = bIsCarrier ? FLinearColor::White : (TeamColor * 1.4f);
	FLinearColor NoseColor = bIsCarrier ? FLinearColor::White : FLinearColor(0.85f, 0.85f, 0.85f, 1.f);

	if (bDeadPresentation)
	{
		// Dim rather than hide: seeing where someone died is useful information, and it makes the
		// respawn delay legible without any UI.
		BodyColor *= 0.2f;
		HeadColor *= 0.2f;
		NoseColor *= 0.2f;
	}

	ApplyColorToMesh(BodyMesh, BodyMID, SanitizeTint(BodyColor));
	ApplyColorToMesh(HeadMesh, HeadMID, SanitizeTint(HeadColor));
	ApplyColorToMesh(NoseMesh, NoseMID, SanitizeTint(NoseColor));
}

void ATraceCharacter::ApplyColorToMesh(UStaticMeshComponent* Mesh, TObjectPtr<UMaterialInstanceDynamic>& InOutMID, const FLinearColor& InColor)
{
	if (Mesh == nullptr || BasicShapeMaterial == nullptr)
	{
		return;
	}

	// Created once and then reused. This function runs on every team change, every carrier change,
	// every death and on a poll timer — allocating a fresh material instance each time would leave a
	// trail of them for the GC.
	if (InOutMID == nullptr)
	{
		InOutMID = Mesh->CreateDynamicMaterialInstance(0, BasicShapeMaterial);
	}

	if (InOutMID == nullptr)
	{
		return;
	}

	// BasicShapeMaterial's parameter is "Color"; "BaseColor" is set as well because setting a
	// parameter that does not exist is a silent no-op, and this way the code survives being pointed
	// at a differently-named material later.
	InOutMID->SetVectorParameterValue(TEXT("Color"), InColor);
	InOutMID->SetVectorParameterValue(TEXT("BaseColor"), InColor);
}

void ATraceCharacter::PollTeamColors()
{
	++TeamColorAttempts;
	ApplyTeamColors();

	// Stop as soon as the team is known, or give up. The OnRep hooks are the primary path; this
	// exists because the pawn, its PlayerState and that PlayerState's Team replicate independently
	// and no single callback is guaranteed to fire last.
	if (GetTeam() != ETraceTeam::None || TeamColorAttempts >= TraceCharacterLayout::MaxTeamColorAttempts)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(TeamColorTimerHandle);
		}
	}
}

void ATraceCharacter::OnRep_IsCarrier()
{
	ApplyTeamColors();
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
	// Both signs are already correct on arrival: the mapping context negates raw MouseY, which is
	// positive when the mouse moves up while AddControllerPitchInput treats positive as down.
	AddControllerYawInput(Value.X);
	AddControllerPitchInput(Value.Y);
}

void ATraceCharacter::DoFirePressed()
{
	// The weapon owns every rule about whether the shot is legal (carrying the Core, dead, fire
	// rate). The pawn just forwards the trigger.
	if (Weapon != nullptr)
	{
		Weapon->StartFire();
	}
}

void ATraceCharacter::DoFireReleased()
{
	if (Weapon != nullptr)
	{
		Weapon->StopFire();
	}
}

void ATraceCharacter::DoPass()
{
	// Local early-out only; PerformPass re-checks everything on the server, where bIsCarrier is
	// authoritative and the Core can confirm it is actually being held by this character.
	if (!bIsCarrier)
	{
		return;
	}

	// No upward bias applied here — ATraceCore::Throw adds UTraceSettings::PassUpwardBias itself, and
	// adding it twice would lob every pass into the ceiling.
	const FVector Direction = GetAimDirection();

	if (HasAuthority())
	{
		PerformPass(Direction);
	}
	else
	{
		ServerPass(Direction);
	}
}

void ATraceCharacter::ServerPass_Implementation(FVector_NetQuantizeNormal Direction)
{
	// Rate limit before anything else. This is a reliable server RPC with no cooldown of its own, so
	// a modified or looping client could otherwise burn a reliable slot every frame. PerformPass is
	// what actually gates the pass; this only bounds the cost of the rejected calls.
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();
	if ((Now - LastPassRequestTime) < MinPassRequestInterval)
	{
		return;
	}
	LastPassRequestTime = Now;

	// Network input: validate, never check() (contract §10). A quantised normal can arrive as zero
	// or non-finite from a malformed or malicious client.
	const FVector Requested(Direction);
	if (Requested.ContainsNaN() || Requested.IsNearlyZero())
	{
		return;
	}

	PerformPass(Requested);
}

void ATraceCharacter::PerformPass(const FVector& Direction)
{
	if (!HasAuthority() || !bIsCarrier)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	ATraceCore* TheCore = nullptr;
	if (const ATraceGameState* TraceGameState = World->GetGameState<ATraceGameState>())
	{
		TheCore = TraceGameState->Core;
	}

	// The Core is the authority on who is holding it. bIsCarrier is a mirror, and a stale mirror
	// must not be able to launch a Core somebody else is carrying.
	if (TheCore == nullptr || TheCore->GetCarrier() != this)
	{
		return;
	}

	FVector Dir = Direction.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		Dir = GetAimDirection();
	}

	TheCore->Throw(Dir, UTraceSettings::Get().PassSpeed);
}

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
