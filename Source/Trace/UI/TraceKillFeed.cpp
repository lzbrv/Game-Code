// Trace — the kill feed: capture and replication. Read TraceKillFeed.h first.

#include "UI/TraceKillFeed.h"

#include "Containers/Ticker.h"
#include "Core/TraceCharacter.h"
#include "Core/TraceGameMode.h"
#include "Core/TracePlayerState.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"                  // TActorIterator
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"   // GetServerWorldTimeSeconds — the replicated clock
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceHitZones.h"       // UTraceDamageSettings — the head/body/leg numbers
#include "Gameplay/TraceMelee.h"          // GetKnifeKillCause() / GetBackstabKillCause() — v10 §1
#include "Gameplay/TraceParry.h"          // GetParryKillCause()
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"
#include "Trace.h"                        // LogTraceGame

namespace TraceKillFeedCauses
{
	/** The two causes this file has to recognise by name. Constructed once. */
	static const FName Bullet(TEXT("Bullet"));
	static const FName Trail(TEXT("Trail"));

	/**
	 * Bullet deaths: head shot, or body/leg?
	 *
	 * WHAT IS ACTUALLY KNOWN. UTraceWeaponComponent resolves a zone and then throws it away — it
	 * calls ApplyDamage(DamageForZone(Zone), ..., "Bullet"), so the cause that reaches the death
	 * funnel is the same FName for a head shot and a shin. UTraceHealthComponent then clamps health
	 * at zero, so the magnitude of the killing blow is gone too.
	 *
	 * WHAT RECOVERS IT. The health the victim had on the previous frame, which the relay samples
	 * every tick. The zone damages are 100 / 40 / 30, so:
	 *
	 *     PreviousHealth > BodyDamage  =>  no body shot and no leg shot could have killed them,
	 *                                      and the only remaining zone is the head.
	 *
	 * That direction is EXACT — it is arithmetic, not a heuristic, and it holds for every head shot
	 * on a target above 40 health, which is every head shot on a healthy player.
	 *
	 * WHAT IT COSTS. The converse is not decidable: a head shot on a victim already down to 20
	 * looks identical to the body shot that would also have killed them, and is drawn with the
	 * ordinary round. The feed therefore UNDER-reports head shots and never over-reports one.
	 * Given the choice, a missing skull is much the better error than a skull on a leg shot.
	 *
	 * THE ONE-LINE FIX, for whoever owns Gameplay/TraceWeaponComponent.cpp: at the ApplyDamage call
	 * (~line 973) pass a cause of "Headshot" when Zone == ETraceHitZone::Head. IconForCause() below
	 * already accepts that name, so the moment that lands this inference stops being consulted for
	 * head shots at all and the remaining ambiguity disappears. Nothing here needs changing.
	 */
	static ETraceKillIcon ResolveBulletIcon(float PreviousHealth)
	{
		const float BodyDamage = UTraceDamageSettings::Get().BodyDamage;
		return (PreviousHealth > BodyDamage + 0.01f) ? ETraceKillIcon::Headshot : ETraceKillIcon::Bullet;
	}

	/** The whole cause -> glyph mapping, in one place. */
	static ETraceKillIcon IconForCause(FName Cause, bool bHasKiller, float PreviousHealth)
	{
		if (Cause == TraceParry::GetParryKillCause())
		{
			return ETraceKillIcon::Parry;
		}
		if (Cause == Trail)
		{
			return ETraceKillIcon::Dash;
		}
		if (Cause == Bullet)
		{
			return ResolveBulletIcon(PreviousHealth);
		}
		// Spec v10 §1. TWO causes, not one — the weapon component knows the approach angle exactly at
		// the moment of the hit and throws nothing away, so unlike the head shot above there is no
		// inference here and no under-reporting. A back-stab is the single most expensive thing that
		// can happen to a player in this game (100 damage, one swing) and the feed says so plainly.
		if (Cause == TraceMelee::GetBackstabKillCause())
		{
			return ETraceKillIcon::Backstab;
		}
		if (Cause == TraceMelee::GetKnifeKillCause())
		{
			return ETraceKillIcon::Knife;
		}

		// Accepted in advance so that the one-line weapon change described above needs no edit here.
		static const FName Headshot(TEXT("Headshot"));
		if (Cause == Headshot)
		{
			return ETraceKillIcon::Headshot;
		}

		// "Fell", and anything the server could not attribute at all.
		return bHasKiller ? ETraceKillIcon::Bullet : ETraceKillIcon::World;
	}

	/** Log-friendly glyph name, so a headless run can assert on the feed without a screenshot. */
	static const TCHAR* IconToString(ETraceKillIcon Icon)
	{
		switch (Icon)
		{
		case ETraceKillIcon::Headshot: return TEXT("HEADSHOT");
		case ETraceKillIcon::Dash:     return TEXT("DASH");
		case ETraceKillIcon::Parry:    return TEXT("PARRY");
		case ETraceKillIcon::World:    return TEXT("WORLD");
		case ETraceKillIcon::Knife:    return TEXT("KNIFE");
		case ETraceKillIcon::Backstab: return TEXT("BACKSTAB");
		default:                       return TEXT("BULLET");
		}
	}
}

// ---------------------------------------------------------------------------------------------
// ATraceKillFeedRelay
// ---------------------------------------------------------------------------------------------

ATraceKillFeedRelay::ATraceKillFeedRelay()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	bReplicates = true;

	// LOAD-BEARING. A kill anywhere on a 33600 x 9600 field has to reach every player, including
	// one who can see neither participant. Without this the relay is distance-culled for most of
	// the match and the feed simply stops updating for whoever is furthest from the fight.
	bAlwaysRelevant = true;

	SetReplicateMovement(false);
	SetCanBeDamaged(false);
	SetActorEnableCollision(false);
	SetHidden(true);

	// Nothing here is spatial and the actor never moves; it exists purely to carry the replicated
	// feed. A high priority because a kill row is small, rare and time-sensitive.
	NetPriority = 3.f;
}

void ATraceKillFeedRelay::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		RescanCharacters();

		if (UWorld* const TheWorld = GetWorld())
		{
			ActorSpawnedHandle = TheWorld->AddOnActorSpawnedHandler(
				FOnActorSpawned::FDelegate::CreateUObject(this, &ATraceKillFeedRelay::HandleActorSpawned));
		}

		UE_LOG(LogTraceGame, Display, TEXT("[KillFeed] Relay up on the authority; %d character(s) tracked."),
			Tracked.Num());
	}
	else
	{
		UE_LOG(LogTraceGame, Display, TEXT("[KillFeed] Relay replicated to this client. Feed is live."));
	}
}

void ATraceKillFeedRelay::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ActorSpawnedHandle.IsValid())
	{
		if (UWorld* const TheWorld = GetWorld())
		{
			TheWorld->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
		}
		ActorSpawnedHandle.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

void ATraceKillFeedRelay::HandleActorSpawned(AActor* SpawnedActor)
{
	TrackCharacter(Cast<ATraceCharacter>(SpawnedActor));
}

void ATraceKillFeedRelay::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Clients hold the relay only to receive the replicated feed and to be read by the HUD; there
	// is nothing for them to do per frame. Ageing is done at draw time against Entry.ServerTime.
	if (!HasAuthority())
	{
		return;
	}

	TimeSinceRescan += DeltaSeconds;
	if (TimeSinceRescan >= RescanInterval)
	{
		TimeSinceRescan = 0.f;
		RescanCharacters();
	}

	SampleTrackedCharacters();
}

ATraceKillFeedRelay* ATraceKillFeedRelay::Find(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<ATraceKillFeedRelay> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (IsValid(*It))
		{
			return *It;
		}
	}
	return nullptr;
}

void ATraceKillFeedRelay::RescanCharacters()
{
	UWorld* const TheWorld = GetWorld();
	if (TheWorld == nullptr)
	{
		return;
	}

	// Drop dead weak entries first, so the array cannot grow across a long match of respawns.
	for (int32 Index = Tracked.Num() - 1; Index >= 0; --Index)
	{
		if (!Tracked[Index].Character.IsValid() || !Tracked[Index].HealthComponent.IsValid())
		{
			Tracked.RemoveAtSwap(Index);
		}
	}

	for (TActorIterator<ATraceCharacter> It(TheWorld); It; ++It)
	{
		TrackCharacter(*It);
	}
}

void ATraceKillFeedRelay::TrackCharacter(ATraceCharacter* Candidate)
{
	if (!HasAuthority() || !IsValid(Candidate) || Candidate->Health == nullptr)
	{
		return;
	}

	const bool bAlreadyTracked = Tracked.ContainsByPredicate(
		[Candidate](const FTrackedCharacter& Record) { return Record.Character.Get() == Candidate; });
	if (bAlreadyTracked)
	{
		return;
	}

	// AddUniqueDynamic as well as the guard above: belt and braces, because a double binding would
	// print the same kill twice on every machine and that is a very visible bug.
	Candidate->Health->OnDeath.AddUniqueDynamic(this, &ATraceKillFeedRelay::HandleDeath);

	FTrackedCharacter Record;
	Record.Character = Candidate;
	Record.HealthComponent = Candidate->Health;
	Record.PreviousHealth = Candidate->Health->Health;
	Record.Team = Candidate->GetTeam();

	// Usually null at this point when we got here from OnActorSpawned — possession has not happened
	// yet. SampleTrackedCharacters() fills it in on the next tick, long before anyone can die.
	if (const APlayerState* const State = Candidate->GetPlayerState())
	{
		Record.Name = State->GetPlayerName();
	}
	Tracked.Add(MoveTemp(Record));
}

void ATraceKillFeedRelay::SampleTrackedCharacters()
{
	for (FTrackedCharacter& Record : Tracked)
	{
		const ATraceCharacter* const TrackedChar = Record.Character.Get();
		const UTraceHealthComponent* const TrackedHealth = Record.HealthComponent.Get();
		if (TrackedChar == nullptr || TrackedHealth == nullptr)
		{
			continue;
		}

		// Only while alive. Once health has hit zero this must keep the value it held the frame
		// BEFORE the fatal hit — that number is the whole basis of the head-shot inference, and
		// overwriting it with the post-mortem zero would destroy it.
		if (TrackedHealth->Health > 0.f)
		{
			Record.PreviousHealth = TrackedHealth->Health;
		}

		if (const APlayerState* const State = TrackedChar->GetPlayerState())
		{
			Record.Name = State->GetPlayerName();
		}

		const ETraceTeam LiveTeam = TrackedChar->GetTeam();
		if (LiveTeam != ETraceTeam::None)
		{
			Record.Team = LiveTeam;
		}
	}
}

const ATraceKillFeedRelay::FTrackedCharacter* ATraceKillFeedRelay::FindTracked(const AActor* DeadActor) const
{
	return Tracked.FindByPredicate(
		[DeadActor](const FTrackedCharacter& Record) { return Record.Character.Get() == DeadActor; });
}

void ATraceKillFeedRelay::HandleDeath(AActor* DeadActor, AController* KillerController, FName Cause)
{
	if (!HasAuthority() || DeadActor == nullptr)
	{
		return;
	}

	const FTrackedCharacter* const Record = FindTracked(DeadActor);

	// --- Victim identity -----------------------------------------------------------------------
	// The cached copy first: this handler runs from inside the death funnel, alongside
	// ATraceGameMode::NotifyCharacterDied, and a name read a call too late can come back empty.
	FTraceKillFeedEntry Entry;
	Entry.VictimName = (Record != nullptr) ? Record->Name : FString();
	Entry.VictimTeam = (Record != nullptr) ? Record->Team : ETraceTeam::None;

	if (const ATraceCharacter* const VictimChar = Cast<ATraceCharacter>(DeadActor))
	{
		if (Entry.VictimName.IsEmpty())
		{
			if (const APlayerState* const VictimState = VictimChar->GetPlayerState())
			{
				Entry.VictimName = VictimState->GetPlayerName();
			}
		}
		if (Entry.VictimTeam == ETraceTeam::None)
		{
			Entry.VictimTeam = VictimChar->GetTeam();
		}
	}
	if (Entry.VictimName.IsEmpty())
	{
		Entry.VictimName = TEXT("?");
	}

	// --- Killer identity -----------------------------------------------------------------------
	const APlayerState* KillerState =
		(KillerController != nullptr) ? KillerController->GetPlayerState<APlayerState>() : nullptr;
	const APlayerState* VictimState = nullptr;
	if (const APawn* const VictimPawn = Cast<APawn>(DeadActor))
	{
		VictimState = VictimPawn->GetPlayerState();
	}
	if (VictimState != nullptr)
	{
		Entry.VictimPlayerId = VictimState->GetPlayerId();
	}

	// A suicide is not a kill. Matches ATraceGameMode::NotifyCharacterDied's own bSelfKill test, so
	// the feed and the scoreboard can never disagree about whether somebody got credit.
	const bool bSelfKill = (KillerState == nullptr) || (KillerState == VictimState);
	if (!bSelfKill)
	{
		Entry.bHasKiller = true;
		Entry.KillerName = KillerState->GetPlayerName();
		Entry.KillerPlayerId = KillerState->GetPlayerId();
		if (const ATracePlayerState* const KillerTraceState = Cast<ATracePlayerState>(KillerState))
		{
			Entry.KillerTeam = KillerTraceState->Team;
		}
		if (Entry.KillerName.IsEmpty())
		{
			Entry.KillerName = TEXT("?");
		}
	}

	const float PreviousHealth = (Record != nullptr) ? Record->PreviousHealth : 0.f;
	Entry.Icon = TraceKillFeedCauses::IconForCause(Cause, Entry.bHasKiller, PreviousHealth);

	UE_LOG(LogTraceGame, Display, TEXT("[KillFeed] %s %s %s  (cause '%s', victim health before the hit %.0f)"),
		Entry.bHasKiller ? *Entry.KillerName : TEXT("<world>"),
		TraceKillFeedCauses::IconToString(Entry.Icon),
		*Entry.VictimName,
		*Cause.ToString(),
		PreviousHealth);

	ServerAnnounceKill(Entry);
}

void ATraceKillFeedRelay::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None: everybody needs the whole feed. There is nothing here worth a conditional — it is
	// eight rows of public information, and any condition would be another way for one player's
	// feed to differ from another's.
	DOREPLIFETIME(ATraceKillFeedRelay, Entries);
}

void ATraceKillFeedRelay::ServerAnnounceKill(FTraceKillFeedEntry Entry)
{
	if (!HasAuthority())
	{
		return;
	}

	Entry.Sequence = NextSequence++;

	// The replicated clock, not GetTimeSeconds(). A client's world time is its own; this number has
	// to mean the same thing on every machine or the fade does not.
	if (const AGameStateBase* const StateBase = GetWorld() != nullptr ? GetWorld()->GetGameState() : nullptr)
	{
		Entry.ServerTime = static_cast<float>(StateBase->GetServerWorldTimeSeconds());
	}
	else if (const UWorld* const TheWorld = GetWorld())
	{
		Entry.ServerTime = TheWorld->GetTimeSeconds();
	}

	Entries.Insert(Entry, 0);
	if (Entries.Num() > MaxStoredEntries)
	{
		Entries.SetNum(MaxStoredEntries, EAllowShrinking::No);
	}

	// Push it now rather than waiting for the actor's next scheduled update. A kill feed row that
	// arrives 100 ms after the kill is fine; one that arrives after the killer has already crossed
	// the map is not.
	ForceNetUpdate();

	// BY HAND ON THE SERVER, exactly as UTraceHealthComponent does for OnRep_Health. This is what
	// guarantees the listen server's own HUD is fed by the same function a remote client's is,
	// rather than by a privileged branch that nobody would notice had rotted.
	OnRep_Entries();
}

void ATraceKillFeedRelay::OnRep_Entries()
{
	// Report each row exactly once per machine, and say out loud if a sequence number was skipped.
	// The gap check is the instrumentation that caught the previous implementation dropping rows on
	// the client only; it costs one comparison and it is the difference between "the feed looks
	// right" and "every kill the server announced arrived here".
	int32 HighestSeen = HighestReportedSequence;
	for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
	{
		const FTraceKillFeedEntry& Row = Entries[Index];
		if (Row.Sequence <= HighestReportedSequence)
		{
			continue;
		}

		const bool bGap = (HighestSeen > 0) && (Row.Sequence > HighestSeen + 1);
		UE_LOG(LogTraceGame, Verbose, TEXT("[KillFeed] row #%d: %s %s %s%s"),
			Row.Sequence,
			Row.bHasKiller ? *Row.KillerName : TEXT("<world>"),
			TraceKillFeedCauses::IconToString(Row.Icon),
			*Row.VictimName,
			bGap ? TEXT("   *** SEQUENCE GAP — a row never reached this machine ***") : TEXT(""));

		HighestSeen = FMath::Max(HighestSeen, Row.Sequence);
	}
	HighestReportedSequence = FMath::Max(HighestReportedSequence, HighestSeen);
}

float ATraceKillFeedRelay::GetEntryAge(const FTraceKillFeedEntry& Entry, const UWorld* World)
{
	if (World == nullptr)
	{
		return 0.f;
	}

	if (const AGameStateBase* const StateBase = World->GetGameState())
	{
		return FMath::Max(0.f, static_cast<float>(StateBase->GetServerWorldTimeSeconds()) - Entry.ServerTime);
	}
	return FMath::Max(0.f, World->GetTimeSeconds() - Entry.ServerTime);
}

// ---------------------------------------------------------------------------------------------
// UTraceKillFeedSubsystem
// ---------------------------------------------------------------------------------------------

bool UTraceKillFeedSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* const OuterWorld = Cast<UWorld>(Outer);
	return OuterWorld != nullptr
		&& (OuterWorld->WorldType == EWorldType::Game || OuterWorld->WorldType == EWorldType::PIE);
}

void UTraceKillFeedSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// Clients receive the relay by replication; spawning a second one locally would give them a
	// duplicate the HUD might find first, and that copy would never receive the replicated feed.
	if (InWorld.GetNetMode() == NM_Client)
	{
		return;
	}

	// Match worlds only. ATraceMenuGameMode derives from AGameModeBase, not ATraceGameMode, so the
	// title screen gets nothing — there are no deaths there to report.
	if (InWorld.GetAuthGameMode<ATraceGameMode>() == nullptr)
	{
		return;
	}

	if (ATraceKillFeedRelay::Find(&InWorld) != nullptr)
	{
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;
	InWorld.SpawnActor<ATraceKillFeedRelay>(ATraceKillFeedRelay::StaticClass(), FTransform::Identity, SpawnParams);
}

// ---------------------------------------------------------------------------------------------
// Console verification
// ---------------------------------------------------------------------------------------------

#if !UE_BUILD_SHIPPING

namespace
{
	/** Whichever world is actually playing, matching the helper in TraceCharacter.cpp. */
	UWorld* FindKillFeedWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
				&& Context.World() != nullptr)
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	/**
	 * Alive, non-carrier characters, so the self-test never picks a target ApplyDamage would
	 * silently ignore (the carrier is bullet-proof by contract §3).
	 */
	void GatherSelfTestTargets(UWorld* TheWorld, TArray<ATraceCharacter*>& OutCharacters)
	{
		OutCharacters.Reset();
		if (TheWorld == nullptr)
		{
			return;
		}
		for (TActorIterator<ATraceCharacter> It(TheWorld); It; ++It)
		{
			ATraceCharacter* const Candidate = *It;
			if (IsValid(Candidate) && Candidate->IsAlive() && !Candidate->IsCarrier()
				&& Candidate->Health != nullptr)
			{
				OutCharacters.Add(Candidate);
			}
		}
	}

	/** An opponent of @p Victim if there is one, otherwise anybody else. */
	ATraceCharacter* FindKillFeedOpponent(const TArray<ATraceCharacter*>& Targets, const ATraceCharacter* Victim)
	{
		const ETraceTeam VictimTeam = (Victim != nullptr) ? Victim->GetTeam() : ETraceTeam::None;
		for (ATraceCharacter* const Candidate : Targets)
		{
			if (Candidate != Victim && Candidate->GetTeam() != VictimTeam)
			{
				return Candidate;
			}
		}
		for (ATraceCharacter* const Candidate : Targets)
		{
			if (Candidate != Victim)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	/** State the self-test ticker carries between steps. Shared so the ticker owns its own lifetime. */
	struct FKillFeedSelfTestState
	{
		TWeakObjectPtr<UWorld> WeakWorld;
		TWeakObjectPtr<ATraceCharacter> BodyVictim;
		TWeakObjectPtr<ATraceCharacter> BodyKiller;

		/** Phase 1 (the body-shot sequence) runs until this is set; only then does Step advance. */
		bool bBodyPhaseStarted = false;
		bool bBodyPhaseDone = false;

		/** Phase 2 step: 0 = head shot, 1 = trail, 2 = parry, 3 = fell. */
		int32 Step = 0;
	};

	/**
	 * Trace.KillFeed.Test — drive one real death of each of the five kinds, on the server.
	 *
	 * WHAT IS SYNTHETIC AND WHAT IS NOT, precisely, because that distinction is the whole value of
	 * this command. Synthetic: the shot. Real: everything after it. Each step calls the same
	 * UTraceHealthComponent entry point the weapon, the trail and the parry call, with the same
	 * cause FName and the same damage number, so the death funnel, OnDeath, the relay's head-shot
	 * inference, the replicated array, the wire and the client's HUD are all exercised as they are
	 * in a real fight. It is a way to make all five causes happen on demand, not a way to fake a
	 * feed — nothing here writes a feed entry directly, and there is no code path that could.
	 *
	 * Steps are spread over 0.6 s because the head-shot inference reads the PREVIOUS FRAME's
	 * health: three body shots fired inside one frame would all see 100 and the third would be
	 * misreported as a head shot. Real fire is 150 RPM, i.e. shots ~24 frames apart, so the spacing
	 * here is if anything tighter than the game's own.
	 *
	 * The body-shot sequence is health-driven rather than counted, so it produces a correct
	 * negative regardless of how shot-up the bot it picks already was: it keeps applying BodyDamage
	 * until the target dies, and the fatal one therefore always lands at or below BodyDamage.
	 */
	void RunKillFeedSelfTest()
	{
		UWorld* const StartWorld = FindKillFeedWorld();
		if (StartWorld == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[KillFeed] Self-test: no game world."));
			return;
		}
		if (StartWorld->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[KillFeed] Self-test must be run ON THE SERVER — deaths are authoritative. ")
				TEXT("Run it on the host; this client will receive the rows over the wire."));
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[KillFeed] Self-test: driving one death of each cause."));

		TSharedRef<FKillFeedSelfTestState> State = MakeShared<FKillFeedSelfTestState>();
		State->WeakWorld = StartWorld;

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([State](float /*DeltaTime*/) -> bool
			{
				UWorld* const TheWorld = State->WeakWorld.Get();
				if (TheWorld == nullptr)
				{
					return false;
				}

				TArray<ATraceCharacter*> Targets;
				GatherSelfTestTargets(TheWorld, Targets);
				if (Targets.Num() < 2)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[KillFeed] Self-test: fewer than two live targets; stopping."));
					return false;
				}

				const UTraceDamageSettings& Damage = UTraceDamageSettings::Get();

				// --- Phase 1: body shots on ONE victim until they die. The NEGATIVE half of the
				//     head-shot test, and the half most likely to break: this must draw the plain
				//     round, because the fatal hit lands with the victim at or below BodyDamage.
				if (!State->bBodyPhaseDone)
				{
					if (!State->bBodyPhaseStarted)
					{
						State->bBodyPhaseStarted = true;
						State->BodyVictim = Targets[0];
						State->BodyKiller = FindKillFeedOpponent(Targets, Targets[0]);
					}

					ATraceCharacter* const BodyVictim = State->BodyVictim.Get();
					if (BodyVictim != nullptr && BodyVictim->IsAlive() && BodyVictim->Health != nullptr)
					{
						ATraceCharacter* const BodyKiller = State->BodyKiller.Get();
						BodyVictim->Health->ApplyDamage(Damage.BodyDamage,
							(BodyKiller != nullptr) ? BodyKiller->GetController() : nullptr,
							TraceKillFeedCauses::Bullet);
						return true;
					}

					// They are down (or gone). Phase 2 starts on the next step.
					State->bBodyPhaseDone = true;
					return true;
				}

				// --- Phase 2: one death per step, in a fixed order, each on a fresh target.
				ATraceCharacter* Victim = Targets[0];
				if (State->Step == 0)
				{
					// A head shot needs a victim above BodyDamage or the inference is (correctly)
					// unable to distinguish it. Real head shots on healthy players are this case.
					for (ATraceCharacter* const Candidate : Targets)
					{
						if (Candidate->Health != nullptr && Candidate->Health->Health > Damage.BodyDamage + 0.01f)
						{
							Victim = Candidate;
							break;
						}
					}
				}
				ATraceCharacter* const Killer = FindKillFeedOpponent(Targets, Victim);
				AController* const KillerController = (Killer != nullptr) ? Killer->GetController() : nullptr;
				if (Victim->Health == nullptr)
				{
					return false;
				}

				switch (State->Step)
				{
				case 0:
					// A head shot: the head zone's own damage number, as the weapon applies it.
					Victim->Health->ApplyDamage(Damage.HeadDamage, KillerController, TraceKillFeedCauses::Bullet);
					break;

				case 1:
					// A trace kill. UTraceTrailComponent uses Kill(), not ApplyDamage, because the
					// carrier is invulnerable to damage — same call, same cause FName.
					Victim->Health->Kill(KillerController, TraceKillFeedCauses::Trail);
					break;

				case 2:
					// A parry kill, exactly as TraceParry::ServerPunishParriedDash issues it.
					Victim->Health->Kill(KillerController, TraceParry::GetParryKillCause());
					break;

				case 3:
					// The arena, with no killer at all.
					Victim->Health->Kill(nullptr, FName(TEXT("Fell")));
					break;

				default:
					UE_LOG(LogTraceGame, Display, TEXT("[KillFeed] Self-test complete."));
					return false;
				}

				++State->Step;
				return true;
			}),
			/*Delay=*/0.6f);
	}

	FAutoConsoleCommand CmdKillFeedTest(
		TEXT("Trace.KillFeed.Test"),
		TEXT("Server only. Drives one real death of each cause (body shots, headshot, trail, parry, fell) ")
		TEXT("through the ordinary death funnel, so every icon can be seen on every machine."),
		FConsoleCommandDelegate::CreateStatic(&RunKillFeedSelfTest));

	/**
	 * Trace.KillFeed.Dump — print THIS machine's feed buffer.
	 *
	 * The check that matters is running this on the CLIENT: it reads the replicated array itself,
	 * so a matching dump on a joining player is direct proof the feed arrived rather than an
	 * inference from a screenshot. Each row prints its sequence number, so the host's dump and the
	 * client's can be compared row for row.
	 */
	FAutoConsoleCommand CmdKillFeedDump(
		TEXT("Trace.KillFeed.Dump"),
		TEXT("Print this machine's kill feed buffer (newest first). Run it on a CLIENT to prove replication."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			UWorld* const TheWorld = FindKillFeedWorld();
			const ATraceKillFeedRelay* const Relay = ATraceKillFeedRelay::Find(TheWorld);
			if (Relay == nullptr)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[KillFeed] DUMP: no relay on this machine. On a client that means the actor has ")
					TEXT("not replicated yet; on the server it means no match world."));
				return;
			}

			const TCHAR* const Where = (TheWorld != nullptr && TheWorld->GetNetMode() == NM_Client)
				? TEXT("CLIENT") : TEXT("SERVER");
			const TArray<FTraceKillFeedEntry>& Rows = Relay->GetEntries();

			// The local player's id is printed alongside, because it is what decides which row gets
			// the "this was about you" border — and comparing the host's dump with the client's is
			// the only way to see that the two machines disagree about that, as they must.
			int32 LocalId = INDEX_NONE;
			if (const APlayerController* const PC = GEngine->GetFirstLocalPlayerController(TheWorld))
			{
				if (const APlayerState* const PS = PC->GetPlayerState<APlayerState>())
				{
					LocalId = PS->GetPlayerId();
				}
			}

			UE_LOG(LogTraceGame, Display, TEXT("[KillFeed] DUMP on %s: %d row(s). Local player id %d."),
				Where, Rows.Num(), LocalId);
			for (int32 Index = 0; Index < Rows.Num(); ++Index)
			{
				const FTraceKillFeedEntry& Row = Rows[Index];
				UE_LOG(LogTraceGame, Display,
					TEXT("[KillFeed]   %d: #%d %s [%s] %s   (age %.1fs, teams %d/%d, ids %d/%d)"),
					Index,
					Row.Sequence,
					Row.bHasKiller ? *Row.KillerName : TEXT("<world>"),
					TraceKillFeedCauses::IconToString(Row.Icon),
					*Row.VictimName,
					ATraceKillFeedRelay::GetEntryAge(Row, TheWorld),
					static_cast<int32>(Row.KillerTeam),
					static_cast<int32>(Row.VictimTeam),
					Row.KillerPlayerId,
					Row.VictimPlayerId);
			}
		}));
}

#endif // !UE_BUILD_SHIPPING
