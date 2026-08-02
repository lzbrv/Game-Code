// Copyright (c) Trace. All Rights Reserved.

#include "Gameplay/TraceEndzone.h"

#include "Trace.h"
#include "TraceTypes.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Gameplay/TraceCore.h"

#include "Components/BoxComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"

ATraceEndzone::ATraceEndzone()
{
	// Ticked only on the server, and only slowly - see BeginPlay for why the poll exists at all.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickInterval = 0.1f;

	// Pure server-side trigger. It is spawned by ATraceArenaBuilder on the authority only, and the
	// visible team-tinted floor patch that marks the zone is a component of the arena builder, so
	// there is nothing here worth a byte of bandwidth.
	bReplicates = false;
	SetCanBeDamaged(false);

	Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("Trigger"));
	SetRootComponent(Trigger);
	Trigger->SetBoxExtent(FVector(450.f, 2000.f, 300.f)); // Overwritten by ConfigureZone().
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Trigger->SetCollisionObjectType(ECC_WorldDynamic);
	Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	// Overlap pawns and nothing else: the zone must never block movement, stop a bullet
	// (ECC_Visibility stays Ignore) or bounce the Core off an invisible wall.
	Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Trigger->SetGenerateOverlapEvents(true);
	Trigger->SetCanEverAffectNavigation(false);
	Trigger->CanCharacterStepUpOn = ECB_No;
	Trigger->SetHiddenInGame(true);
}

void ATraceEndzone::ConfigureZone(ETraceTeam InOwningTeam, const FVector& BoxHalfExtent)
{
	OwningTeam = InOwningTeam;

	if (Trigger)
	{
		Trigger->SetBoxExtent(BoxHalfExtent, /*bUpdateOverlaps=*/false);
	}
}

bool ATraceEndzone::ScoresHere(ETraceTeam Team) const
{
	// THE scoring rule, in one place. OwningTeam defends this zone; its opponent scores in it.
	// Blue's zone is scored in by Orange, Orange's zone is scored in by Blue.
	return Team != ETraceTeam::None
		&& OwningTeam != ETraceTeam::None
		&& Team == TraceOpposingTeam(OwningTeam);
}

void ATraceEndzone::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority())
	{
		// Belt and braces for a level-placed zone that replicated (or was loaded) onto a client:
		// scoring is a server decision, so a client copy must be inert rather than merely quiet.
		if (Trigger)
		{
			Trigger->SetGenerateOverlapEvents(false);
			Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		SetActorTickEnabled(false);
		return;
	}

	if (Trigger)
	{
		Trigger->OnComponentBeginOverlap.AddDynamic(this, &ATraceEndzone::OnTriggerBeginOverlap);
	}

	// The overlap event only fires on entry, which misses the case that decides matches: a player
	// standing inside the enemy endzone who picks up (or intercepts) the Core there. The 10 Hz poll
	// in Tick() covers it, and doubles as a safety net if the overlap never fires at all.
	SetActorTickEnabled(true);

	UE_LOG(LogTraceGame, Verbose, TEXT("Endzone defended by %s is live at %s (scored in by %s)."),
		*TraceTeamName(OwningTeam).ToString(),
		*GetActorLocation().ToCompactString(),
		*TraceTeamName(TraceOpposingTeam(OwningTeam)).ToString());
}

void ATraceEndzone::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const UWorld* World = GetWorld();
	if (!HasAuthority() || World == nullptr || Trigger == nullptr)
	{
		return;
	}

	// There is exactly one Core and at most one carrier, so the poll is a single point-in-box test
	// against that one pawn - no overlap set to walk, and nothing to iterate when nobody is
	// carrying. Deliberately geometric rather than overlap-based: see IsInsideZone().
	const ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>();
	if (GameMode == nullptr)
	{
		return;
	}

	ATraceCore* TheCore = GameMode->GetCore();
	if (TheCore == nullptr)
	{
		return;
	}

	ATraceCharacter* CoreCarrier = TheCore->GetCarrier();
	if (CoreCarrier == nullptr || !IsInsideZone(CoreCarrier->GetActorLocation()))
	{
		return;
	}

	TryScore(CoreCarrier);
}

bool ATraceEndzone::IsInsideZone(const FVector& WorldLocation) const
{
	if (Trigger == nullptr)
	{
		return false;
	}

	// InverseTransformPosition undoes the component scale, so it must be compared against the
	// UNSCALED extent. (The arena builder leaves the scale at 1, but pairing them correctly means
	// a level-placed, scaled endzone still behaves.)
	const FVector Local = Trigger->GetComponentTransform().InverseTransformPosition(WorldLocation);
	const FVector Extent = Trigger->GetUnscaledBoxExtent();

	return FMath::Abs(Local.X) <= Extent.X
		&& FMath::Abs(Local.Y) <= Extent.Y
		&& FMath::Abs(Local.Z) <= Extent.Z;
}

void ATraceEndzone::OnTriggerBeginOverlap(UPrimitiveComponent* /*OverlappedComponent*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/, const FHitResult& /*SweepResult*/)
{
	if (!HasAuthority())
	{
		return;
	}

	// Immediate path: a carrier who runs in scores on the frame they cross the line rather than
	// waiting up to a tick interval for the poll above.
	TryScore(Cast<ATraceCharacter>(OtherActor));
}

void ATraceEndzone::TryScore(ATraceCharacter* Character)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || World == nullptr || !IsValid(Character))
	{
		return;
	}

	const ETraceTeam CarrierTeam = Character->GetTeam();
	if (!ScoresHere(CarrierTeam))
	{
		// Either an unteamed pawn, or a defender standing in their own endzone with the Core -
		// which is legal and does nothing. Only the attacking team scores here.
		return;
	}

	if (!Character->IsAlive() || !IsCoreCarrier(Character))
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	if ((Now - LastScoreTime) < FMath::Max(0.f, ScoreCooldown))
	{
		return;
	}

	ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>();
	if (GameMode == nullptr)
	{
		return;
	}

	LastScoreTime = Now;

	UE_LOG(LogTraceGame, Log, TEXT("%s carried the Core into the %s endzone - %s scores."),
		*GetNameSafe(Character),
		*TraceTeamName(OwningTeam).ToString(),
		*TraceTeamName(CarrierTeam).ToString());

	// The scoring team is the CARRIER's team, never OwningTeam.
	GameMode->NotifyScored(CarrierTeam);
}

bool ATraceEndzone::IsCoreCarrier(const ATraceCharacter* Character) const
{
	if (Character == nullptr)
	{
		return false;
	}

	// Ask the Core itself. ATraceCharacter::bIsCarrier is a replicated mirror that the Core sets,
	// so it agrees on the server - but the Core's own Carrier pointer is the one true source, and
	// checking it means a stale flag can never award a point.
	if (const UWorld* World = GetWorld())
	{
		if (const ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>())
		{
			if (const ATraceCore* TheCore = GameMode->GetCore())
			{
				return TheCore->GetCarrier() == Character;
			}
		}
	}

	return Character->IsCarrier();
}
