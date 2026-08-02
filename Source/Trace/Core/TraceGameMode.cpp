// Copyright (c) Trace. All Rights Reserved.

#include "Core/TraceGameMode.h"

#include "EngineUtils.h"                                 // TActorIterator
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"     // StopMovementImmediately on respawn
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Math/NumericLimits.h"
#include "TimerManager.h"

#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "UI/TraceHUD.h"
#include "World/TraceArenaBuilder.h"
#include "World/TraceTeamPlayerStart.h"

namespace TraceGameModeConstants
{
	/** Fallback Core spawn height if the arena builder is missing or returns garbage. */
	static const FVector FallbackCoreLocation(0.f, 0.f, 200.f);

	/** Smallest respawn delay we will honour; zero would respawn inside the death frame. */
	static constexpr float MinRespawnDelay = 0.1f;

	/**
	 * Treated as "no warm-up". A literal is used rather than KINDA_SMALL_NUMBER / UE_KINDA_SMALL_NUMBER
	 * because those spellings churned during the 5.x line and this file must compile unchanged on
	 * 5.4 through 5.8.
	 */
	static constexpr float ZeroDurationEpsilon = 0.001f;
}

ATraceGameMode::ATraceGameMode()
{
	PrimaryActorTick.bCanEverTick = false;

	GameStateClass = ATraceGameState::StaticClass();
	PlayerStateClass = ATracePlayerState::StaticClass();
	PlayerControllerClass = ATracePlayerController::StaticClass();
	DefaultPawnClass = ATraceCharacter::StaticClass();
	HUDClass = ATraceHUD::StaticClass();

	ArenaBuilderClass = ATraceArenaBuilder::StaticClass();
	CoreClass = ATraceCore::StaticClass();

	// Players run around during warm-up rather than sitting in spectator.
	bStartPlayersAsSpectators = false;

	// ATracePlayerState::CopyProperties (which carries Team/Kills/Deaths) only runs if travel is
	// seamless, so enable it here rather than leaving the behaviour to whoever calls ServerTravel.
	bUseSeamlessTravel = true;
}

void ATraceGameMode::BeginPlay()
{
	Super::BeginPlay();

	SpawnArenaAndCore();

	// The listen-server host logs in during UEngine::LoadMap, i.e. *before* the world begins play,
	// so at their PostLogin the arena — and with it every ATraceTeamPlayerStart — did not exist yet
	// and AGameModeBase::RestartPlayer will have bailed out with "player start not found". Now that
	// the field is built, hand a pawn to anyone still without one. On a dedicated server this is a
	// no-op (remote clients only ever log in after BeginPlay).
	EnsurePlayersHavePawns();

	// Same reason: the host is already logged in, so this is the first point at which the phase
	// machine is allowed to run.
	CheckMatchStartConditions();
}

bool ATraceGameMode::ShouldSpawnAtStartSpot(AController* Player)
{
	// Never reuse AController::StartSpot — every respawn must re-evaluate ChoosePlayerStart so the
	// "furthest from any live enemy" rule keeps applying.
	return false;
}

void ATraceGameMode::SpawnArenaAndCore()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	ArenaBuilder = ATraceArenaBuilder::Get(World);

	if (ArenaBuilder == nullptr)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		UClass* ClassToSpawn = (ArenaBuilderClass != nullptr) ? ArenaBuilderClass.Get() : ATraceArenaBuilder::StaticClass();
		ArenaBuilder = World->SpawnActor<ATraceArenaBuilder>(ClassToSpawn, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);

		UE_LOG(LogTraceGame, Log, TEXT("No ATraceArenaBuilder in the level; spawned one at the origin."));
	}

	// The builder must have run before we can trust GetCoreSpawnLocation() or expect its player
	// starts to exist, and neither path above guarantees that:
	//  - a level-placed builder may simply not have reached BeginPlay yet (actor order is undefined);
	//  - an actor spawned *during* the world's begin-play sweep does not begin play at spawn time,
	//    because UWorld::HasBegunPlay() is still false while that sweep is running.
	// AActor::DispatchBeginPlay self-guards and is a no-op once an actor has begun play, so forcing
	// it here is both safe and idempotent.
	if (ArenaBuilder != nullptr && !ArenaBuilder->HasActorBegunPlay())
	{
		ArenaBuilder->DispatchBeginPlay();
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("ATraceGameMode::SpawnArenaAndCore: no ATraceGameState, cannot publish the Core."));
		return;
	}

	if (TraceGameState->Core != nullptr)
	{
		return;
	}

	FVector CoreLocation = (ArenaBuilder != nullptr) ? ArenaBuilder->GetCoreSpawnLocation() : TraceGameModeConstants::FallbackCoreLocation;
	if (CoreLocation.ContainsNaN())
	{
		CoreLocation = TraceGameModeConstants::FallbackCoreLocation;
	}

	FActorSpawnParameters CoreSpawnParams;
	CoreSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	UClass* CoreClassToSpawn = (CoreClass != nullptr) ? CoreClass.Get() : ATraceCore::StaticClass();
	ATraceCore* SpawnedCore = World->SpawnActor<ATraceCore>(CoreClassToSpawn, CoreLocation, FRotator::ZeroRotator, CoreSpawnParams);
	if (SpawnedCore == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("ATraceGameMode::SpawnArenaAndCore: failed to spawn the Core."));
		return;
	}

	// The GameState owns the replicated handle; everything else (HUD, endzones, characters) reads
	// the Core from there rather than searching the world.
	TraceGameState->Core = SpawnedCore;
	TraceGameState->ForceNetUpdate();

	UE_LOG(LogTraceGame, Log, TEXT("Core spawned at %s"), *CoreLocation.ToCompactString());
}

// ---------------------------------------------------------------------------------------------
// Login / logout
// ---------------------------------------------------------------------------------------------

void ATraceGameMode::PostLogin(APlayerController* NewPlayer)
{
	// The team must exist before Super::PostLogin, because Super spawns the pawn and our
	// ChoosePlayerStart override reads the team to pick a side of the field.
	AssignTeamIfNeeded(NewPlayer);

	Super::PostLogin(NewPlayer);

	// A reconnecting player may have had their old (inactive) PlayerState swapped in by Super, so
	// re-check rather than assume the assignment above survived.
	AssignTeamIfNeeded(NewPlayer);

	if (NewPlayer != nullptr)
	{
		if (ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(NewPlayer->GetPawn()))
		{
			TraceCharacter->ApplyTeamColors();
		}
	}

	CheckMatchStartConditions();
}

void ATraceGameMode::AssignTeamIfNeeded(APlayerController* NewPlayer)
{
	if (!HasAuthority() || NewPlayer == nullptr)
	{
		return;
	}

	ATracePlayerState* TracePlayerState = NewPlayer->GetPlayerState<ATracePlayerState>();
	if (TracePlayerState == nullptr || TracePlayerState->Team != ETraceTeam::None)
	{
		return;
	}

	const ETraceTeam PickedTeam = PickTeamForNewPlayer();
	if (PickedTeam == ETraceTeam::None)
	{
		// Both sides are at PlayersPerTeam. The player still spawns (on a fallback start) but stays
		// teamless, which keeps them out of scoring and out of the friendly-fire tests.
		UE_LOG(LogTraceGame, Warning, TEXT("Both teams are full; '%s' joined without a team."), *TracePlayerState->GetPlayerName());
		return;
	}

	TracePlayerState->SetTeam(PickedTeam);
}

void ATraceGameMode::Logout(AController* Exiting)
{
	if (HasAuthority() && Exiting != nullptr)
	{
		ClearPendingRespawn(Exiting);

		if (ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(Exiting->GetPawn()))
		{
			// Never let the one Core disappear with a quitter.
			if (ATraceCore* TheCore = GetCore())
			{
				if (TheCore->GetCarrier() == TraceCharacter)
				{
					TheCore->DropAt(TraceCharacter->GetActorLocation(), FVector::ZeroVector);
				}
			}

			if (UTraceTrailComponent* TrailComponent = TraceCharacter->Trail)
			{
				TrailComponent->SetEmitting(false);
				TrailComponent->ClearTrail();
			}

			UnregisterCharacter(TraceCharacter);
		}
	}

	Super::Logout(Exiting);

	// The leaving PlayerState is still in PlayerArray during Logout, so the player count is only
	// truthful next tick.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(this, &ATraceGameMode::CheckMatchStartConditions);
	}
}

ETraceTeam ATraceGameMode::PickTeamForNewPlayer() const
{
	const ATraceGameState* TraceGameState = GetGameState<ATraceGameState>();
	if (TraceGameState == nullptr)
	{
		// No GameState yet means no players yet; the first arrival goes Blue.
		return ETraceTeam::Blue;
	}

	const int32 TeamCap = FMath::Max(1, UTraceSettings::Get().PlayersPerTeam);
	const int32 NumBlue = TraceGameState->CountTeamMembers(ETraceTeam::Blue);
	const int32 NumOrange = TraceGameState->CountTeamMembers(ETraceTeam::Orange);

	const bool bBlueHasRoom = NumBlue < TeamCap;
	const bool bOrangeHasRoom = NumOrange < TeamCap;

	if (bBlueHasRoom && bOrangeHasRoom)
	{
		// Smaller team wins; a tie goes to Blue.
		return (NumOrange < NumBlue) ? ETraceTeam::Orange : ETraceTeam::Blue;
	}
	if (bBlueHasRoom)
	{
		return ETraceTeam::Blue;
	}
	if (bOrangeHasRoom)
	{
		return ETraceTeam::Orange;
	}

	return ETraceTeam::None;
}

// ---------------------------------------------------------------------------------------------
// Spawn selection
// ---------------------------------------------------------------------------------------------

AActor* ATraceGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	UWorld* World = GetWorld();
	if (World == nullptr || Player == nullptr)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	const ETraceTeam Team = GetTeamForController(Player);

	TArray<ATraceTeamPlayerStart*> TeamStarts;
	TArray<ATraceTeamPlayerStart*> AnyStarts;
	for (TActorIterator<ATraceTeamPlayerStart> It(World); It; ++It)
	{
		ATraceTeamPlayerStart* Start = *It;
		if (!IsValid(Start))
		{
			continue;
		}

		AnyStarts.Add(Start);
		if (Team != ETraceTeam::None && Start->Team == Team)
		{
			TeamStarts.Add(Start);
		}
	}

	// Teamless players (or a level with no team starts) fall back to any Trace start, then to the
	// engine's default APlayerStart selection.
	const TArray<ATraceTeamPlayerStart*>& Pool = (TeamStarts.Num() > 0) ? TeamStarts : AnyStarts;
	if (Pool.Num() == 0)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	// Pass 1: only unoccupied pads, best = furthest from the nearest living enemy.
	// Pass 2: if every pad is blocked, take the safest one anyway rather than failing to spawn —
	//         a player standing in a corpse's pad is far better than a player with no pawn.
	ATraceTeamPlayerStart* BestStart = nullptr;
	float BestScore = -1.f;
	ATraceTeamPlayerStart* BestBlockedStart = nullptr;
	float BestBlockedScore = -1.f;

	for (ATraceTeamPlayerStart* Start : Pool)
	{
		const FVector StartLocation = Start->GetActorLocation();
		const float Score = DistSqToNearestEnemy(StartLocation, Team);

		if (IsStartOccupied(Start, Player))
		{
			if (Score > BestBlockedScore)
			{
				BestBlockedScore = Score;
				BestBlockedStart = Start;
			}
			continue;
		}

		if (Score > BestScore)
		{
			BestScore = Score;
			BestStart = Start;
		}
	}

	if (BestStart == nullptr)
	{
		BestStart = BestBlockedStart;
	}

	return (BestStart != nullptr) ? static_cast<AActor*>(BestStart) : Super::ChoosePlayerStart_Implementation(Player);
}

bool ATraceGameMode::IsStartOccupied(const AActor* Start, const AController* ForPlayer) const
{
	if (Start == nullptr)
	{
		return true;
	}

	const APawn* OwnPawn = (ForPlayer != nullptr) ? ForPlayer->GetPawn() : nullptr;
	const FVector StartLocation = Start->GetActorLocation();
	const float RadiusSquared = FMath::Square(FMath::Max(1.f, StartOccupiedRadius));

	for (const TWeakObjectPtr<ATraceCharacter>& WeakCharacter : TrackedCharacters)
	{
		const ATraceCharacter* Character = WeakCharacter.Get();
		if (Character == nullptr || Character == OwnPawn || !Character->IsAlive())
		{
			continue;
		}

		if (FVector::DistSquared(StartLocation, Character->GetActorLocation()) <= RadiusSquared)
		{
			return true;
		}
	}

	return false;
}

float ATraceGameMode::DistSqToNearestEnemy(const FVector& Location, ETraceTeam Team) const
{
	float BestDistanceSquared = TNumericLimits<float>::Max();

	for (const TWeakObjectPtr<ATraceCharacter>& WeakCharacter : TrackedCharacters)
	{
		const ATraceCharacter* Character = WeakCharacter.Get();
		if (Character == nullptr || !Character->IsAlive())
		{
			continue;
		}

		// A teamless spawner treats everyone as a threat; otherwise our own team is not one.
		if (Team != ETraceTeam::None && Character->GetTeam() == Team)
		{
			continue;
		}

		const float DistanceSquared = static_cast<float>(FVector::DistSquared(Location, Character->GetActorLocation()));
		BestDistanceSquared = FMath::Min(BestDistanceSquared, DistanceSquared);
	}

	return BestDistanceSquared;
}

ETraceTeam ATraceGameMode::GetTeamForController(const AController* Controller)
{
	if (Controller == nullptr)
	{
		return ETraceTeam::None;
	}

	const ATracePlayerState* TracePlayerState = Controller->GetPlayerState<ATracePlayerState>();
	return (TracePlayerState != nullptr) ? TracePlayerState->Team : ETraceTeam::None;
}

// ---------------------------------------------------------------------------------------------
// Character tracking
// ---------------------------------------------------------------------------------------------

void ATraceGameMode::RegisterCharacter(ATraceCharacter* Character)
{
	if (Character == nullptr)
	{
		return;
	}

	CompactTrackedCharacters();
	TrackedCharacters.AddUnique(Character);
}

void ATraceGameMode::UnregisterCharacter(ATraceCharacter* Character)
{
	if (Character == nullptr)
	{
		return;
	}

	TrackedCharacters.Remove(Character);
	CompactTrackedCharacters();
}

void ATraceGameMode::CompactTrackedCharacters()
{
	TrackedCharacters.RemoveAll([](const TWeakObjectPtr<ATraceCharacter>& WeakCharacter)
	{
		return !WeakCharacter.IsValid();
	});
}

const TArray<TWeakObjectPtr<ATraceCharacter>>& ATraceGameMode::GetTrackedCharacters() const
{
	return TrackedCharacters;
}

ATraceCore* ATraceGameMode::GetCore() const
{
	const ATraceGameState* TraceGameState = GetGameState<ATraceGameState>();
	if (TraceGameState == nullptr)
	{
		return nullptr;
	}

	return TraceGameState->Core;
}

ATraceGameState* ATraceGameMode::GetTraceGameState() const
{
	return GetGameState<ATraceGameState>();
}

// ---------------------------------------------------------------------------------------------
// Deaths and respawns
// ---------------------------------------------------------------------------------------------

void ATraceGameMode::NotifyCharacterDied(ATraceCharacter* Victim, AController* Killer, FName Cause)
{
	if (!HasAuthority() || !IsValid(Victim))
	{
		return;
	}

	// This must be called while the victim is still possessed — APawn::UnPossessed() clears both
	// Controller and PlayerState, which would cost the player their death credit and their respawn.
	// The GetOwner() fallback covers a detach that already happened but has not yet propagated.
	AController* VictimController = Victim->GetController();
	if (VictimController == nullptr)
	{
		VictimController = Cast<AController>(Victim->GetOwner());
	}

	ATracePlayerState* VictimState = Victim->GetPlayerState<ATracePlayerState>();
	if (VictimState == nullptr && VictimController != nullptr)
	{
		VictimState = VictimController->GetPlayerState<ATracePlayerState>();
	}

	// 1. The Core must never vanish with the corpse — drop it where the carrier fell (contract §3).
	if (ATraceCore* TheCore = GetCore())
	{
		if (TheCore->GetCarrier() == Victim)
		{
			TheCore->DropAt(Victim->GetActorLocation(), FVector::ZeroVector);
		}
	}

	// 2. Rule: on carrier death the trail is cleared instantly. Non-carriers have an empty trail,
	//    so this is unconditional and cheap.
	if (UTraceTrailComponent* TrailComponent = Victim->Trail)
	{
		TrailComponent->SetEmitting(false);
		TrailComponent->ClearTrail();
	}

	// 3. Credit. Suicides, world deaths and team kills never award a kill.
	ATracePlayerState* KillerState = (Killer != nullptr) ? Killer->GetPlayerState<ATracePlayerState>() : nullptr;

	if (VictimState != nullptr)
	{
		++VictimState->Deaths;
		VictimState->bIsCarrier = false;
		VictimState->ForceNetUpdate();
	}

	const bool bSelfKill = (KillerState == nullptr) || (KillerState == VictimState);
	const bool bTeamKill = (KillerState != nullptr && VictimState != nullptr
		&& KillerState->Team != ETraceTeam::None
		&& KillerState->Team == VictimState->Team);

	if (!bSelfKill && !bTeamKill)
	{
		++KillerState->Kills;
		KillerState->ForceNetUpdate();
	}

	UE_LOG(LogTraceGame, Verbose, TEXT("Death: '%s' killed by '%s' (%s)"),
		(VictimState != nullptr) ? *VictimState->GetPlayerName() : TEXT("<unknown>"),
		(KillerState != nullptr) ? *KillerState->GetPlayerName() : TEXT("<world>"),
		*Cause.ToString());

	// 4. Tell the victim who got them, so their HUD can show the death panel.
	if (ATracePlayerController* VictimPC = Cast<ATracePlayerController>(VictimController))
	{
		const FString KillerName = (KillerState != nullptr && !bSelfKill) ? KillerState->GetPlayerName() : FString(TEXT("the arena"));
		VictimPC->ClientNotifyKilledBy(KillerName, Cause);
	}

	// 5. Schedule the respawn. One timer per controller (see PendingRespawns): a second death must
	//    replace the first death's timer, never race it.
	if (VictimController != nullptr)
	{
		const float RespawnDelay = FMath::Max(TraceGameModeConstants::MinRespawnDelay, UTraceSettings::Get().RespawnDelay);
		const TWeakObjectPtr<AController> WeakController(VictimController);

		// Drop entries whose controller has been destroyed, so the map cannot grow across a long
		// match of joins and leaves.
		for (auto It = PendingRespawns.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid())
			{
				GetWorldTimerManager().ClearTimer(It.Value());
				It.RemoveCurrent();
			}
		}

		FTimerHandle& RespawnHandle = PendingRespawns.FindOrAdd(WeakController);
		GetWorldTimerManager().ClearTimer(RespawnHandle);
		GetWorldTimerManager().SetTimer(
			RespawnHandle,
			FTimerDelegate::CreateUObject(this, &ATraceGameMode::RespawnController, WeakController),
			RespawnDelay,
			false);
	}
}

void ATraceGameMode::RespawnController(TWeakObjectPtr<AController> ControllerPtr)
{
	PendingRespawns.Remove(ControllerPtr);

	if (!HasAuthority())
	{
		return;
	}

	AController* Controller = ControllerPtr.Get();
	if (!IsValid(Controller))
	{
		return;
	}

	// Something else may already have restarted them (a capture reset, for example).
	if (const ATraceCharacter* ExistingCharacter = Cast<ATraceCharacter>(Controller->GetPawn()))
	{
		if (ExistingCharacter->IsAlive())
		{
			return;
		}
	}

	RestartPlayerFresh(Controller);
}

void ATraceGameMode::ClearPendingRespawn(AController* Controller)
{
	if (Controller == nullptr)
	{
		return;
	}

	const TWeakObjectPtr<AController> WeakController(Controller);
	if (FTimerHandle* ExistingHandle = PendingRespawns.Find(WeakController))
	{
		GetWorldTimerManager().ClearTimer(*ExistingHandle);
		PendingRespawns.Remove(WeakController);
	}
}

void ATraceGameMode::RestartPlayerFresh(AController* Controller)
{
	if (!IsValid(Controller))
	{
		return;
	}

	// AGameModeBase::RestartPlayerAtPlayerStart reuses whatever pawn the controller still owns
	// instead of spawning a new one, so a dead pawn has to be torn down first — otherwise the
	// player is "respawned" as their own corpse, standing where they died.
	if (APawn* CurrentPawn = Controller->GetPawn())
	{
		const ATraceCharacter* CurrentCharacter = Cast<ATraceCharacter>(CurrentPawn);
		if (CurrentCharacter == nullptr || !CurrentCharacter->IsAlive())
		{
			Controller->UnPossess();
			CurrentPawn->Destroy();
		}
	}

	RestartPlayer(Controller);
}

void ATraceGameMode::EnsurePlayersHavePawns()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PlayerController = It->Get();
		if (!IsValid(PlayerController) || PlayerController->GetPawn() != nullptr)
		{
			continue;
		}

		// Genuine spectators must stay pawnless.
		const APlayerState* ControllerPlayerState = PlayerController->GetPlayerState<APlayerState>();
		if (ControllerPlayerState == nullptr || ControllerPlayerState->IsOnlyASpectator())
		{
			continue;
		}

		RestartPlayerFresh(PlayerController);
	}
}

// ---------------------------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------------------------

void ATraceGameMode::NotifyScored(ETraceTeam ScoringTeam)
{
	if (!HasAuthority() || ScoringTeam == ETraceTeam::None)
	{
		return;
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		return;
	}

	// Warm-up and post-match carries still reset the field, but they never count.
	const bool bCounts = (TraceGameState->TraceMatchState == ETraceMatchState::InProgress);
	if (bCounts)
	{
		TraceGameState->AddScore(ScoringTeam, 1);
		UE_LOG(LogTraceGame, Log, TEXT("%s scored. Blue %d - Orange %d"),
			*TraceTeamName(ScoringTeam).ToString(), TraceGameState->BlueScore, TraceGameState->OrangeScore);
	}

	// Reset the Core first: it detaches from the carrier and clears their carrying state (and with
	// it the trail) before we start moving pawns around.
	if (ATraceCore* TheCore = TraceGameState->Core)
	{
		TheCore->ResetToCenter();
	}

	ResetPlayersToSpawns();

	if (bCounts)
	{
		const int32 ScoreToWin = FMath::Max(1, UTraceSettings::Get().ScoreToWin);
		if (TraceGameState->GetScore(ScoringTeam) >= ScoreToWin)
		{
			FinishMatch(ScoringTeam);
		}
	}
}

void ATraceGameMode::ResetPlayersToSpawns()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PlayerController = It->Get();
		if (!IsValid(PlayerController))
		{
			continue;
		}

		ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(PlayerController->GetPawn());

		if (TraceCharacter == nullptr || !TraceCharacter->IsAlive())
		{
			// Dead or pawnless: hand them a brand new pawn at a freshly chosen start, and drop the
			// pending respawn timer that would otherwise fire into an already-live player.
			ClearPendingRespawn(PlayerController);
			RestartPlayerFresh(PlayerController);
			TraceCharacter = Cast<ATraceCharacter>(PlayerController->GetPawn());
		}
		else if (AActor* StartSpot = FindPlayerStart(PlayerController))
		{
			// AGameModeBase::RestartPlayerAtPlayerStart only re-possesses a pawn that already
			// exists — it never moves it — so a living player has to be teleported by hand.
			const FVector StartLocation = StartSpot->GetActorLocation();
			FRotator StartRotation = StartSpot->GetActorRotation();
			StartRotation.Pitch = 0.f;
			StartRotation.Roll = 0.f;

			TraceCharacter->TeleportTo(StartLocation, StartRotation);
			PlayerController->SetControlRotation(StartRotation);

			// Control rotation is client-authoritative, so the client has to be told to turn.
			PlayerController->ClientSetRotation(StartRotation, /*bResetCamera=*/true);
		}

		if (TraceCharacter == nullptr)
		{
			continue;
		}

		if (UTraceHealthComponent* HealthComponent = TraceCharacter->Health)
		{
			HealthComponent->ResetHealth();
		}

		if (UTraceTrailComponent* TrailComponent = TraceCharacter->Trail)
		{
			TrailComponent->SetEmitting(false);
			TrailComponent->ClearTrail();
		}

		// A teleport preserves velocity, which would fling a sprinting player straight off the pad.
		if (UCharacterMovementComponent* MovementComponent = TraceCharacter->GetCharacterMovement())
		{
			MovementComponent->StopMovementImmediately();
		}

		TraceCharacter->ApplyTeamColors();
	}
}

// ---------------------------------------------------------------------------------------------
// Match phase machine
// ---------------------------------------------------------------------------------------------

int32 ATraceGameMode::GetActivePlayerCount() const
{
	const ATraceGameState* TraceGameState = GetGameState<ATraceGameState>();
	if (TraceGameState == nullptr)
	{
		return 0;
	}

	int32 Count = 0;
	for (const APlayerState* PlayerState : TraceGameState->PlayerArray)
	{
		if (PlayerState != nullptr && !PlayerState->IsOnlyASpectator())
		{
			++Count;
		}
	}

	return Count;
}

void ATraceGameMode::CheckMatchStartConditions()
{
	if (!HasAuthority() || !HasActorBegunPlay())
	{
		// Players can log in before the world begins play; BeginPlay re-runs this.
		return;
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::WaitingForPlayers)
	{
		return;
	}

	const int32 MinPlayers = FMath::Max(1, UTraceSettings::Get().MinPlayersToStart);
	if (GetActivePlayerCount() >= MinPlayers)
	{
		StartWarmup();
	}
	else
	{
		CancelWarmup();
	}
}

void ATraceGameMode::StartWarmup()
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::WaitingForPlayers)
	{
		return;
	}

	if (GetWorldTimerManager().IsTimerActive(WarmupTimerHandle))
	{
		return;
	}

	const float WarmupDuration = FMath::Max(0.f, UTraceSettings::Get().WarmupDuration);

	// There is no dedicated warm-up match state, so the phase stays WaitingForPlayers and the shared
	// deadline doubles as the warm-up countdown clients can render.
	TraceGameState->MatchEndServerTime = static_cast<float>(TraceGameState->GetServerWorldTimeSeconds() + WarmupDuration);
	TraceGameState->ForceNetUpdate();

	if (WarmupDuration <= TraceGameModeConstants::ZeroDurationEpsilon)
	{
		BeginMatch();
		return;
	}

	UE_LOG(LogTraceGame, Log, TEXT("Enough players; warm-up started (%.1fs)."), WarmupDuration);
	GetWorldTimerManager().SetTimer(WarmupTimerHandle, this, &ATraceGameMode::BeginMatch, WarmupDuration, false);
}

void ATraceGameMode::CancelWarmup()
{
	if (!GetWorldTimerManager().IsTimerActive(WarmupTimerHandle))
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(WarmupTimerHandle);

	if (ATraceGameState* TraceGameState = GetTraceGameState())
	{
		TraceGameState->MatchEndServerTime = 0.f;
		TraceGameState->ForceNetUpdate();
	}

	UE_LOG(LogTraceGame, Log, TEXT("Not enough players; warm-up cancelled."));
}

void ATraceGameMode::BeginMatch()
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::WaitingForPlayers)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(WarmupTimerHandle);

	const float MatchDuration = FMath::Max(1.f, UTraceSettings::Get().MatchDuration);

	// Warm-up goals do not count, but nothing stops the scores being non-zero if the settings were
	// changed live, so start from a known state.
	TraceGameState->BlueScore = 0;
	TraceGameState->OrangeScore = 0;
	TraceGameState->OnRep_Scores();

	TraceGameState->TraceMatchState = ETraceMatchState::InProgress;
	TraceGameState->MatchEndServerTime = static_cast<float>(TraceGameState->GetServerWorldTimeSeconds() + MatchDuration);
	TraceGameState->ForceNetUpdate();

	if (ATraceCore* TheCore = TraceGameState->Core)
	{
		TheCore->ResetToCenter();
	}
	ResetPlayersToSpawns();

	// The authoritative deadline is the replicated MatchEndServerTime; this timer is merely what
	// fires on it server-side. Clients count down against the shared clock instead.
	GetWorldTimerManager().SetTimer(MatchTimerHandle, this, &ATraceGameMode::HandleMatchTimeExpired, MatchDuration, false);

	UE_LOG(LogTraceGame, Log, TEXT("Match started (%.0fs, first to %d)."), MatchDuration, UTraceSettings::Get().ScoreToWin);
}

void ATraceGameMode::HandleMatchTimeExpired()
{
	const ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::InProgress)
	{
		return;
	}

	ETraceTeam Winner = ETraceTeam::None;
	if (TraceGameState->BlueScore > TraceGameState->OrangeScore)
	{
		Winner = ETraceTeam::Blue;
	}
	else if (TraceGameState->OrangeScore > TraceGameState->BlueScore)
	{
		Winner = ETraceTeam::Orange;
	}

	FinishMatch(Winner);
}

void ATraceGameMode::FinishMatch(ETraceTeam WinningTeam)
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState == ETraceMatchState::PostMatch)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(WarmupTimerHandle);
	GetWorldTimerManager().ClearTimer(MatchTimerHandle);

	TraceGameState->TraceMatchState = ETraceMatchState::PostMatch;
	TraceGameState->MatchEndServerTime = static_cast<float>(TraceGameState->GetServerWorldTimeSeconds());
	TraceGameState->ForceNetUpdate();

	// Players keep their pawns and keep respawning after the whistle — the HUD switches to the FINAL
	// banner, and leaving everyone alive means nobody is staring at a corpse on the results screen.
	UE_LOG(LogTraceGame, Log, TEXT("Match over. Winner: %s (Blue %d - Orange %d)"),
		(WinningTeam == ETraceTeam::None) ? TEXT("draw") : *TraceTeamName(WinningTeam).ToString(),
		TraceGameState->BlueScore, TraceGameState->OrangeScore);
}
