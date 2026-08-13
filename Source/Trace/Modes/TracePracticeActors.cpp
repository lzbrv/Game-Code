// Trace — the practice range's world furniture (spec v19 §2). See TracePracticeActors.h.

#include "Modes/TracePracticeActors.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Core/TraceCharacter.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceWeaponComponent.h"      // SetAutonomousAttacksAllowed — demo 19 item 2
#include "Modes/TracePracticeRange.h"
#include "Trace.h"                              // LogTraceGame
#include "TraceTypes.h"                         // ETraceTeam

// NAMED, not anonymous. Scripts/check-jumbo-build-collisions.py gates the build on this: two files
// that each define an anonymous-namespace symbol of the same name are one namespace with two
// definitions once UBT concatenates them, which is MSVC C2084 on Windows and silence on macOS.
namespace TracePracticeActors
{
	/** Radius of the walk-on trigger. Wide enough to catch a sprint, tight enough to be a "spot". */
	constexpr float PadTriggerRadius = 170.f;

	/** Seconds one pad ignores further entries for. See ATracePracticePad::LastAcceptedWorldTime. */
	constexpr float PadRetriggerLockout = 0.6f;

	/** The engine cylinder is a 100 uu diameter, 100 uu tall unit; this makes a 260 x 14 uu disc. */
	constexpr float PadDiscScaleXY = 2.6f;
	constexpr float PadDiscScaleZ = 0.14f;

	/** Height the floating label sits at, above the disc. */
	constexpr float PadLabelHeight = 210.f;

	FLinearColor ColourForRole(ETracePracticePadRole InRole, bool bLit)
	{
		switch (InRole)
		{
		case ETracePracticePadRole::CoreRack:
			return bLit ? FLinearColor(0.95f, 0.78f, 0.20f) : FLinearColor(0.42f, 0.35f, 0.12f);
		case ETracePracticePadRole::InfiniteAbilities:
			return bLit ? FLinearColor(0.35f, 0.90f, 0.55f) : FLinearColor(0.16f, 0.34f, 0.22f);
		case ETracePracticePadRole::CharacterSwap:
		default:
			return bLit ? FLinearColor(0.40f, 0.70f, 0.98f) : FLinearColor(0.17f, 0.28f, 0.40f);
		}
	}
}

// =================================================================================================
// ATracePracticePad
// =================================================================================================

ATracePracticePad::ATracePracticePad()
{
	PrimaryActorTick.bCanEverTick = false;

	// Replicated so a listen server's client half draws the pad too. The RULES are server-only —
	// OnPadOverlapBegin returns immediately without authority — so a client seeing the disc can never
	// be a client triggering it.
	bReplicates = true;
	SetReplicateMovement(false);

	Trigger = CreateDefaultSubobject<USphereComponent>(TEXT("Trigger"));
	SetRootComponent(Trigger);
	Trigger->InitSphereRadius(TracePracticeActors::PadTriggerRadius);
	Trigger->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
	Trigger->SetGenerateOverlapEvents(true);
	Trigger->SetCanEverAffectNavigation(false);

	Disc = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Disc"));
	Disc->SetupAttachment(Trigger);
	Disc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Disc->SetCanEverAffectNavigation(false);
	Disc->SetRelativeScale3D(FVector(TracePracticeActors::PadDiscScaleXY,
	                                 TracePracticeActors::PadDiscScaleXY,
	                                 TracePracticeActors::PadDiscScaleZ));

	// The same two engine primitives the arena is built out of, and the same fallback discipline: if
	// /Engine/BasicShapes is not cooked into this build the pad is invisible, and an invisible pad is
	// a degraded range rather than a crash.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		Disc->SetStaticMesh(CylinderFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> PadMaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (PadMaterialFinder.Succeeded())
	{
		Disc->SetMaterial(0, PadMaterialFinder.Object);
	}

	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Trigger);
	Label->SetRelativeLocation(FVector(0.f, 0.f, TracePracticeActors::PadLabelHeight));
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetVerticalAlignment(EVRTA_TextCenter);
	Label->SetWorldSize(46.f);
	Label->SetCanEverAffectNavigation(false);
}

void ATracePracticePad::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ATracePracticePad, PadRole);
}

void ATracePracticePad::BeginPlay()
{
	Super::BeginPlay();

	if (Trigger != nullptr)
	{
		Trigger->OnComponentBeginOverlap.AddDynamic(this, &ATracePracticePad::OnPadOverlapBegin);
	}
}

void ATracePracticePad::ConfigurePad(ETracePracticePadRole InRole, const FString& InLabel)
{
	PadRole = InRole;
	SetPadLabel(InLabel);
	SetPadLit(false);
}

void ATracePracticePad::SetPadLabel(const FString& InLabel)
{
	if (Label != nullptr)
	{
		Label->SetText(FText::FromString(InLabel));
	}
}

void ATracePracticePad::SetPadLit(bool bLit)
{
	if (Disc == nullptr)
	{
		return;
	}

	const FLinearColor PadColour = TracePracticeActors::ColourForRole(PadRole, bLit);

	if (UMaterialInstanceDynamic* PadMaterial = Disc->CreateAndSetMaterialInstanceDynamic(0))
	{
		// Both names, exactly as ATraceArenaBuilder does: the engine's BasicShapeMaterial exposes
		// "Color", the project's own parents expose "BaseColor", and setting a parameter a material
		// does not have is a no-op rather than an error.
		PadMaterial->SetVectorParameterValue(TEXT("Color"), PadColour);
		PadMaterial->SetVectorParameterValue(TEXT("BaseColor"), PadColour);
		PadMaterial->SetVectorParameterValue(TEXT("Emissive"), PadColour * (bLit ? 3.0f : 0.6f));
	}

	if (Label != nullptr)
	{
		Label->SetTextRenderColor(PadColour.ToFColor(/*bSRGB=*/true));
	}
}

void ATracePracticePad::OnPadOverlapBegin(UPrimitiveComponent* /*OverlappedComponent*/, AActor* OtherActor,
                                          UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/,
                                          bool /*bFromSweep*/, const FHitResult& /*SweepResult*/)
{
	// SERVER ONLY. A pad is a rule, and rules live on the server; the client half of a listen server
	// reaches the same code through the same call, so nothing is lost by refusing here.
	if (!HasAuthority())
	{
		return;
	}

	ATraceCharacter* const Toucher = Cast<ATraceCharacter>(OtherActor);
	if (Toucher == nullptr || !Toucher->IsAlive())
	{
		return;
	}

	// Dummies stand still, but a knockback, a Mace pull or a respawn could still put one on a pad,
	// and a target dummy must never be able to pocket the Core.
	if (Toucher->GetController() == nullptr || Toucher->GetController()->IsA<ATracePracticeDummyController>())
	{
		return;
	}

	const UWorld* const WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return;
	}

	// ONE STEP, ONE ACTIVATION. A pawn's capsule and its hit-zone primitives can all begin
	// overlapping in the same frame; without this, stepping onto the Core rack would put the Core
	// down and immediately pick it back up, which reads as "the pad does nothing".
	const float NowWorld = WorldPtr->GetTimeSeconds();
	if (NowWorld - LastAcceptedWorldTime < TracePracticeActors::PadRetriggerLockout)
	{
		return;
	}
	LastAcceptedWorldTime = NowWorld;

	if (UTracePracticeRangeSubsystem* Range = UTracePracticeRangeSubsystem::Get(WorldPtr))
	{
		Range->HandlePadTouched(this, Toucher);
	}
}

// =================================================================================================
// ATracePracticePost
// =================================================================================================

ATracePracticePost::ATracePracticePost()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(false);

	Disc = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Disc"));
	SetRootComponent(Disc);
	Disc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Disc->SetCanEverAffectNavigation(false);
	Disc->SetRelativeScale3D(FVector(1.4f, 1.4f, 0.06f));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> PostCylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (PostCylinderFinder.Succeeded())
	{
		Disc->SetStaticMesh(PostCylinderFinder.Object);
	}

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> PostMaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (PostMaterialFinder.Succeeded())
	{
		Disc->SetMaterial(0, PostMaterialFinder.Object);
	}
}

// =================================================================================================
// ATracePracticeDummyController
// =================================================================================================

ATracePracticeDummyController::ATracePracticeDummyController()
{
	PrimaryActorTick.bCanEverTick = false;

	// The PlayerState is the whole reason this is a controller at all: it is what carries the team,
	// what UTraceAbilityWorldSubsystem hangs the ability component off, and what ATraceGameMode's
	// death path needs in order to schedule a respawn.
	bWantsPlayerState = true;

	// No behaviour tree, no blackboard, no perception. Possess and stop.
	bStartAILogicOnPossess = false;
	bStopAILogicOnUnposses = true;
}

void ATracePracticeDummyController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	// BELT AND BRACES ON "STATIONARY". Nothing in this class issues movement, but a dummy that
	// somehow received input — a future shared possession path, a debug command — must still stand
	// still, and APawn::Internal_AddMovementInput honours this flag on the controller.
	SetIgnoreMoveInput(true);

	if (ATracePlayerState* DummyState = GetPlayerState<ATracePlayerState>())
	{
		// ETraceTeam::None, explicitly. It is already the default, and it is load-bearing rather
		// than incidental — see the block at the top of TracePracticeActors.h for the four rules
		// that fall out of it.
		DummyState->SetTeam(ETraceTeam::None);
		DummyState->SetPlayerName(FString::Printf(TEXT("Dummy %s"), *GetName().Right(3)));
	}

	if (ATraceCharacter* DummyPawn = Cast<ATraceCharacter>(InPawn))
	{
		// *** DEMO 19 ITEM 2: "Don't let the bots attack." ***
		//
		// THIS CLASS DOING NOTHING WAS NEVER ENOUGH, and the header's confidence that it was is the
		// bug. UTraceWeaponComponent::TickBotKnife runs on the SERVER for every pawn whose controller
		// is not an APlayerController and asks the controller nothing at all: it found the nearest
		// non-teammate, drew the knife inside 500 uu and swung inside 153 uu. A dummy on
		// ETraceTeam::None has no teammates, so every one of the other four targets and the player
		// were enemies to it — the row armed itself the moment it was built, and it swung at anyone
		// who closed to knifing distance, which is the only distance you can knife one from.
		//
		// HERE, and not in BuildRange, because this runs on EVERY possession: a target that dies goes
		// through the shipped respawn pipeline and comes back with a BRAND NEW pawn and a brand new
		// weapon component, whose bit is back at its default. Disarming at spawn time would have
		// re-armed the row on the first death — the kind of fix that measures green once.
		if (UTraceWeaponComponent* DummyWeapon = DummyPawn->FindComponentByClass<UTraceWeaponComponent>())
		{
			DummyWeapon->SetAutonomousAttacksAllowed(false);
		}

		UE_LOG(LogTraceGame, Verbose, TEXT("[Practice] dummy '%s' possessed %s at %s (weapon disarmed: "
			"a target does not fight back)."),
			*GetName(), *DummyPawn->GetName(), *DummyPawn->GetActorLocation().ToCompactString());
	}
}
