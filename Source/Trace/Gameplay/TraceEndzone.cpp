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

void ATraceEndzone::ConfigureZone(ETraceTeam InOwningTeam, const FVector& BoxHalfExtent, bool bInGoalVolume)
{
	OwningTeam = InOwningTeam;
	bGoalVolume = bInGoalVolume;

	if (Trigger)
	{
		Trigger->SetBoxExtent(BoxHalfExtent, /*bUpdateOverlaps=*/false);
	}
}

void ATraceEndzone::SetZoneActive(bool bInActive)
{
	if (bZoneActive == bInActive)
	{
		return;
	}

	bZoneActive = bInActive;

	// A client copy is already fully inert (BeginPlay strips its collision and tick), so the flag is
	// all it needs; re-enabling collision here would arm a scoring trigger on a machine that has no
	// authority to score.
	if (!HasAuthority())
	{
		return;
	}

	if (Trigger)
	{
		Trigger->SetCollisionEnabled(bZoneActive ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
		Trigger->SetGenerateOverlapEvents(bZoneActive);
	}
	SetActorTickEnabled(bZoneActive);

	// Log, not Verbose. A volume that stops scoring is indistinguishable from a broken trigger from
	// inside the game, and the A/B toggle is exactly the moment somebody will be looking for that.
	UE_LOG(LogTraceGame, Log, TEXT("%s %s (defended by %s) is now %s."),
		bGoalVolume ? TEXT("Goal") : TEXT("Endzone"),
		*GetNameSafe(this),
		*TraceTeamName(OwningTeam).ToString(),
		bZoneActive ? TEXT("ARMED") : TEXT("inert"));
}

FBox ATraceEndzone::GetZoneBounds() const
{
	if (Trigger == nullptr)
	{
		return FBox(ForceInit);
	}

	// Built from the component transform rather than from the actor's, so a scaled or rotated
	// trigger still reports the box it actually occupies.
	const FBox Local(-Trigger->GetUnscaledBoxExtent(), Trigger->GetUnscaledBoxExtent());
	return Local.TransformBy(Trigger->GetComponentTransform());
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
	//
	// Gated on bZoneActive: in mode B the endzone pair is built but inert, and vice versa, so the
	// volume belonging to the other mode must not tick, overlap or score.
	if (Trigger)
	{
		Trigger->SetCollisionEnabled(bZoneActive ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
		Trigger->SetGenerateOverlapEvents(bZoneActive);
	}
	SetActorTickEnabled(bZoneActive);

	// Log, not Verbose. The half extent is the whole contract of this actor - "the zone spans the
	// full width of the field", or in mode B "the goal is a third of it and 700 uu tall" - is either
	// true or the game silently refuses points somewhere, and a fact nobody can see must at least be
	// one nobody has to raise a verbosity level to read. This project has twice declared a working
	// mechanic dead over exactly that.
	const FVector Extent = (Trigger != nullptr) ? Trigger->GetUnscaledBoxExtent() : FVector::ZeroVector;
	UE_LOG(LogTraceGame, Log, TEXT("%s defended by %s is %s at %s, half extent %s (scored in by %s)."),
		bGoalVolume ? TEXT("Goal (mode B)") : TEXT("Endzone (mode A)"),
		*TraceTeamName(OwningTeam).ToString(),
		bZoneActive ? TEXT("LIVE") : TEXT("built but inert"),
		*GetActorLocation().ToCompactString(),
		*Extent.ToCompactString(),
		*TraceTeamName(TraceOpposingTeam(OwningTeam)).ToString());
}

void ATraceEndzone::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const UWorld* World = GetWorld();
	if (!HasAuthority() || !bZoneActive || World == nullptr || Trigger == nullptr)
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
	// An inactive volume contains nothing. Without this, mode B's throw code could ask the mode-A
	// endzone (which is still built, just disarmed) whether the Core is inside it and get a yes for
	// a point that must not be awarded.
	if (Trigger == nullptr || !bZoneActive)
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
	if (!HasAuthority() || !bZoneActive || World == nullptr || !IsValid(Character))
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

	// The lateral coordinate is in the line on purpose. A mode-A endzone spans the FULL width of the
	// field (ATraceArenaBuilder::EndzoneHalfWidth), and the only cheap way to prove that from a match
	// log - rather than by walking into a corner and hoping - is to see scores land at Y values out
	// near the sidelines as well as down the middle. If every mode-A score in a log sits near Y=0,
	// the trigger has quietly gone back to being a partial box. In mode B the opposite reading
	// applies: every score MUST sit inside the printed half width, or the goal is not a goal.
	const FVector ScoreLocation = Character->GetActorLocation();
	const float ZoneHalfWidth = (Trigger != nullptr) ? Trigger->GetUnscaledBoxExtent().Y : 0.f;

	UE_LOG(LogTraceGame, Log, TEXT("%s carried the Core into the %s %s at X=%.0f Y=%.0f (zone half width %.0f) - %s scores."),
		*GetNameSafe(Character),
		*TraceTeamName(OwningTeam).ToString(),
		bGoalVolume ? TEXT("goal") : TEXT("endzone"),
		ScoreLocation.X, ScoreLocation.Y, ZoneHalfWidth,
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
