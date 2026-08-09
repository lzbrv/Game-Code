// Trace — Oyster's jar. See the header: Pickler is a normal jar with a one-shot landing effect.

#include "Abilities/Characters/TraceOysterJar.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"                 // TActorIterator
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

#include "Abilities/TraceAbilityComponent.h"
#include "Abilities/Characters/TraceOysterPoison.h"
#include "Core/TraceCharacter.h"
#include "Trace.h"
#include "TraceSettings.h"

ATraceOysterJar::ATraceOysterJar()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;
	SetReplicateMovement(true);      // the lob is short but it is the only thing clients see move
	bAlwaysRelevant = true;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetRootComponent(Collision);
	Collision->InitSphereRadius(18.f);
	// WORLD STATIC ONLY. Blocking geometry is what makes the lob land; ignoring everything else is
	// what stops the jar becoming a movement base, a bullet shield or a thing players can shove.
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetGenerateOverlapEvents(false);
	Collision->SetCanEverAffectNavigation(false);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCollisionProfileName(TEXT("NoCollision"));
	Mesh->SetCastShadow(false);
	Mesh->SetRelativeScale3D(FVector(0.35f, 0.35f, 0.35f));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderFinder.Succeeded())
	{
		Mesh->SetStaticMesh(CylinderFinder.Object);
	}
}

void ATraceOysterJar::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ATraceOysterJar, bIsPickler);
	DOREPLIFETIME(ATraceOysterJar, bGrounded);
}

void ATraceOysterJar::BeginPlay()
{
	Super::BeginPlay();
}

float ATraceOysterJar::MatchTimeNow() const
{
	const UWorld* WorldPtr = GetWorld();
	const AGameStateBase* ClockState = (WorldPtr != nullptr) ? WorldPtr->GetGameState() : nullptr;
	return (ClockState != nullptr) ? static_cast<float>(ClockState->GetServerWorldTimeSeconds()) : 0.f;
}

void ATraceOysterJar::Initialise(UTraceAbilityComponent* InSourceComp, ETraceTeam InOwnerTeam,
                                 bool bInIsPickler, const FVector& InVelocity)
{
	SourceComponent = InSourceComp;
	OwnerTeam = InOwnerTeam;
	bIsPickler = bInIsPickler;
	FlightVelocity = InVelocity;

	if (FlightVelocity.IsNearlyZero())
	{
		// The dash jar. It is dropped where he is, so it is on the ground from the first frame and
		// its 4 s starts now.
		Land();
	}
}

// =================================================================================================
// Tick
// =================================================================================================

void ATraceOysterJar::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority())
	{
		return;   // every rule is server-side; a client's jar is a mesh
	}

	if (!bGrounded)
	{
		TickFlight(DeltaSeconds);
		return;
	}

	TickGrounded();
}

void ATraceOysterJar::TickFlight(float DeltaSeconds)
{
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return;
	}

	// Plain ballistics against WORLD gravity, integrated here rather than by a projectile movement
	// component. The lob is under a second, it must not be affected by the pawn gravity scale the
	// movement component keeps re-pushing, and a swept SetActorLocation gives the landing point for
	// free — which is the one thing the jar actually needs from its flight.
	FlightVelocity.Z += WorldPtr->GetGravityZ() * DeltaSeconds;

	FHitResult Hit;
	SetActorLocation(GetActorLocation() + FlightVelocity * DeltaSeconds, /*bSweep*/ true, &Hit);

	if (Hit.bBlockingHit)
	{
		Land();
	}
}

void ATraceOysterJar::TickGrounded()
{
	// "An enemy TOUCHING a jar breaks it."
	if (ATraceCharacter* Toucher = FindToucher())
	{
		UE_LOG(LogTraceGame, Verbose, TEXT("[Oyster] Jar broken by %s."), *GetNameSafe(Toucher));
		ServerBreakNow(TEXT("touched by an enemy"));
		return;
	}

	// "Jars last 4 s on the ground." Expiring untouched is NOT a break: nothing bursts, it is simply
	// gone. §6 ties the poison to being broken, not to the jar existing.
	if (ExpiryMatchTime > 0.f && MatchTimeNow() >= ExpiryMatchTime)
	{
		Destroy();
	}
}

// =================================================================================================
// Landing
// =================================================================================================

void ATraceOysterJar::Land()
{
	if (bGrounded)
	{
		return;
	}

	bGrounded = true;
	FlightVelocity = FVector::ZeroVector;
	ExpiryMatchTime = MatchTimeNow() + FMath::Max(0.25f, UTraceSettings::Get().OysterJarLifetimeSeconds);

	if (bIsPickler && !bLandingEffectFired)
	{
		bLandingEffectFired = true;
		FireLandingEffect();
	}
}

void ATraceOysterJar::ServerForceLandNow()
{
	if (HasAuthority())
	{
		Land();
	}
}

void ATraceOysterJar::FireLandingEffect()
{
	UWorld* WorldPtr = GetWorld();
	UTraceAbilityComponent* SourceComp = SourceComponent.Get();
	if (WorldPtr == nullptr || SourceComp == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float DamageRadius = FMath::Max(1.f, Settings.OysterPicklerDamageRadiusUU);
	const float PullRadius   = FMath::Max(1.f, Settings.OysterPicklerPullRadiusUU);
	const float PullSpeed    = FMath::Max(0.f, Settings.OysterPicklerPullSpeed);
	const float ImpactDamage = FMath::Max(0.f, Settings.OysterPicklerDamage);
	const FVector Origin = GetActorLocation();

	int32 DamagedCount = 0;
	int32 PulledCount = 0;

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive())
		{
			continue;
		}

		const float Distance = FVector::Dist(Candidate->GetActorLocation(), Origin);

		// --- "deals 30 damage in an area" -----------------------------------------------------------
		if (Distance <= DamageRadius)
		{
			// THE CHOKE POINT. ApplyAbilityDamage asks CanAffectTargetDetailed(Damage) itself and
			// returns 0 for a carrier, for a team-mate and for the dead. There is no second path.
			const float Dealt = SourceComp->ApplyAbilityDamage(Candidate, ImpactDamage, TEXT("OysterPickler"));
			if (Dealt > 0.f)
			{
				++DamagedCount;
				TraceOyster::RecordEffect(Candidate, TEXT("Pickler impact damage"),
					&TraceOyster::FEffectTally::PicklerDamageHits);
			}
		}

		// --- "pulls enemies within a small radius toward it" ------------------------------------------
		if (Distance <= PullRadius && PullSpeed > 0.f)
		{
			// THE CHOKE POINT AGAIN, and with a DIFFERENT effect class. A pull is Control, so it is
			// governed by §4's [ASSUMPTION] (bCarrierImmuneToAbilityControl) rather than by the
			// unconditional damage rule — one flag reverses it, and it reverses here and nowhere else.
			if (!SourceComp->CanAffectTarget(Candidate, ETraceAbilityEffect::Control))
			{
				continue;
			}

			FVector ToJar = Origin - Candidate->GetActorLocation();
			if (ToJar.IsNearlyZero())
			{
				continue;
			}
			ToJar.Normalize();

			// LaunchCharacter, not a raw Velocity write: it is the engine's own "something threw this
			// pawn" entry point, it goes through the movement component's pending-launch path, and it
			// is what the rest of the project would use. See the report for the client-prediction
			// caveat that comes with it.
			Candidate->LaunchCharacter(ToJar * PullSpeed, /*bXYOverride*/ true, /*bZOverride*/ true);
			++PulledCount;
			TraceOyster::RecordEffect(Candidate, TEXT("Pickler pull"), &TraceOyster::FEffectTally::PicklerPulls);
		}
	}

	UE_LOG(LogTraceGame, Log,
		TEXT("[Oyster] Pickler landed at %s: %.0f damage to %d in %.0f uu, pulled %d in %.0f uu. The jar now stays "
		     "as a normal jar for %.1fs (the doc's own clarification)."),
		*Origin.ToCompactString(), ImpactDamage, DamagedCount, DamageRadius, PulledCount, PullRadius,
		UTraceSettings::Get().OysterJarLifetimeSeconds);
}

// =================================================================================================
// Breaking
// =================================================================================================

bool ATraceOysterJar::IsEnemyOfOwner(const ATraceCharacter* Candidate) const
{
	if (Candidate == nullptr)
	{
		return false;
	}
	const ETraceTeam TheirTeam = Candidate->GetTeam();
	return OwnerTeam != ETraceTeam::None && TheirTeam != ETraceTeam::None && TheirTeam != OwnerTeam;
}

ATraceCharacter* ATraceOysterJar::FindToucher() const
{
	UWorld* WorldPtr = GetWorld();
	if (WorldPtr == nullptr)
	{
		return nullptr;
	}

	const float BreakRadius = FMath::Max(1.f, UTraceSettings::Get().OysterJarBreakRadiusUU);
	const FVector Origin = GetActorLocation();

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive() || !IsEnemyOfOwner(Candidate))
		{
			continue;
		}
		if (FVector::Dist(Candidate->GetActorLocation(), Origin) <= BreakRadius)
		{
			return Candidate;
		}
	}
	return nullptr;
}

void ATraceOysterJar::ServerBreakNow(const TCHAR* Why)
{
	if (!HasAuthority())
	{
		return;
	}

	UE_LOG(LogTraceGame, Verbose, TEXT("[Oyster] Jar breaking (%s)."), Why);
	Burst();
	Destroy();
}

void ATraceOysterJar::Burst()
{
	UWorld* WorldPtr = GetWorld();
	UTraceAbilityComponent* SourceComp = SourceComponent.Get();
	if (WorldPtr == nullptr)
	{
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const float Radius = FMath::Max(1.f, Settings.OysterPoisonRadiusUU);
	const FVector Origin = GetActorLocation();
	int32 PoisonedCount = 0;

	// SPEC v16 §3: "Add a small, semi transparent cloud when oyster's poison jars break. This cloud
	// should be the radius of the explosion."
	//
	// Spawned from the SAME `Radius` local the loop below tests every candidate against — not from a
	// second read of the knob, and certainly not from a number of its own. That is the whole of §3's
	// second sentence: if the two were ever allowed to diverge the cloud would be telling players the
	// poison is somewhere it is not, which is worse than drawing nothing.
	//
	// FIRST, and unconditionally: the cloud is the shape of the BURST, not a report on who it caught.
	// A jar that breaks in an empty corridor still poisoned that volume for the next four seconds and
	// still has to say so. It is cosmetic — nothing below reads it, and it touches nobody.
	const ATraceOysterPoisonCloud* Cloud = ATraceOysterPoisonCloud::ServerSpawnForBurst(
		WorldPtr, Origin, Radius, Settings.OysterPoisonDurationSeconds);

	for (TActorIterator<ATraceCharacter> It(WorldPtr); It; ++It)
	{
		ATraceCharacter* Candidate = *It;
		if (Candidate == nullptr || !Candidate->IsAlive())
		{
			continue;
		}
		if (FVector::Dist(Candidate->GetActorLocation(), Origin) > Radius)
		{
			continue;
		}

		// THE CHOKE POINT, ASKED FOR BOTH HALVES OF WHAT POISON IS.
		//
		// Poison is 3 damage a tick (Damage) AND -30% speed (Control), so a target the choke point
		// refuses for BOTH is not poisoned at all — no component, no tick, nothing to leak. A target
		// it allows for either one gets the component, which then re-asks per tick and per frame and
		// applies only the half it is allowed. That is why a carrier ends up with no poison component
		// whatsoever rather than an inert one.
		const bool bMayDamage = (SourceComp != nullptr)
			? SourceComp->CanAffectTarget(Candidate, ETraceAbilityEffect::Damage)
			: UTraceAbilityComponent::CanAffect(nullptr, Candidate, ETraceAbilityEffect::Damage);
		const bool bMayControl = (SourceComp != nullptr)
			? SourceComp->CanAffectTarget(Candidate, ETraceAbilityEffect::Control)
			: UTraceAbilityComponent::CanAffect(nullptr, Candidate, ETraceAbilityEffect::Control);

		if (!bMayDamage && !bMayControl)
		{
			continue;
		}

		if (UTraceOysterPoisonComponent::ApplyTo(Candidate, SourceComp) != nullptr)
		{
			++PoisonedCount;
		}
	}

	// The cloud is reported from the POINTER, not from the fact that the call was made: "spawned a
	// cloud" and "asked for a cloud" are different claims and only one of them is worth logging.
	UE_LOG(LogTraceGame, Log, TEXT("[Oyster] Jar burst at %s: poisoned %d within %.0f uu; cloud %s."),
		*Origin.ToCompactString(), PoisonedCount, Radius,
		(Cloud != nullptr)
			? *FString::Printf(TEXT("spawned at %.0f uu (spec v16 §3)"), Cloud->GetCloudRadiusUU())
			: TEXT("NOT spawned"));
}
