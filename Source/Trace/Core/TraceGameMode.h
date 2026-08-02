// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Map.h"
#include "Engine/EngineTypes.h"                  // FTimerHandle
#include "GameFramework/GameModeBase.h"
#include "Templates/SubclassOf.h"
#include "UObject/ObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "TraceTypes.h"                          // ETraceTeam

#include "TraceGameMode.generated.h"

class AActor;
class AController;
class APlayerController;
class ATraceArenaBuilder;
class ATraceCharacter;
class ATraceCore;
class ATraceGameState;
class ATraceTeamPlayerStart;

/**
 * Server-authoritative rules for a Trace match.
 *
 * Owns: arena/Core spawning, team balancing, spawn-point selection, the match phase machine,
 * respawn scheduling and scoring. AGameModeBase only ever exists on the server, so every mutation
 * here is authoritative by construction; the HasAuthority() guards that remain are on the entry
 * points other systems call, so a mistake elsewhere fails quietly instead of desyncing.
 *
 * The phase machine lives on ATraceGameState (that is the replicated half); this class is the only
 * thing allowed to advance it:
 *
 *     WaitingForPlayers --(MinPlayersToStart reached)--> [WarmupDuration countdown]
 *                       --(warm-up expires)------------> InProgress
 *                       --(ScoreToWin or MatchDuration)-> PostMatch
 *
 * There is deliberately no separate "Warmup" state: the countdown is expressed through the shared
 * MatchEndServerTime deadline while the state stays WaitingForPlayers, which keeps ETraceMatchState
 * exactly as the contract defines it.
 */
UCLASS()
class TRACE_API ATraceGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ATraceGameMode();

	//~ Begin AGameModeBase interface
	virtual void BeginPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

	/**
	 * AGameModeBase::FindPlayerStart short-circuits to AController::StartSpot (the spot used for
	 * the *first* spawn) whenever this returns true, which would pin every respawn to one pad.
	 * Returning false forces ChoosePlayerStart to run on every RestartPlayer, which is what the
	 * "furthest from any live enemy" rule needs.
	 */
	virtual bool ShouldSpawnAtStartSpot(AController* Player) override;
	//~ End AGameModeBase interface

	/**
	 * Called by ATraceCharacter the moment it dies, while it is still possessed.
	 * Handles kill/death credit, dropping the Core, wiping the trail and scheduling the respawn.
	 * @param Cause "Bullet", "Trail" or "Fell".
	 */
	void NotifyCharacterDied(ATraceCharacter* Victim, AController* Killer, FName Cause);

	/** Called by ATraceEndzone when a carrier reaches the opposing endzone. */
	void NotifyScored(ETraceTeam ScoringTeam);

	void RegisterCharacter(ATraceCharacter* Character);
	void UnregisterCharacter(ATraceCharacter* Character);

	/**
	 * The server's roster of live characters. Used by the weapon's lag-compensated resolver and by
	 * the trail's trip test, so it is hot: entries are weak and may be stale — always .Get().
	 */
	const TArray<TWeakObjectPtr<ATraceCharacter>>& GetTrackedCharacters() const;

	ATraceCore* GetCore() const;

	/** Smaller team wins, ties go to Blue. Returns None when both teams are at PlayersPerTeam. */
	ETraceTeam PickTeamForNewPlayer() const;

protected:
	/** Spawned at the origin if the level does not already contain an ATraceArenaBuilder. */
	UPROPERTY(EditDefaultsOnly, Category = "Trace|Classes")
	TSubclassOf<ATraceArenaBuilder> ArenaBuilderClass;

	UPROPERTY(EditDefaultsOnly, Category = "Trace|Classes")
	TSubclassOf<ATraceCore> CoreClass;

	/** A player start with a live character inside this radius counts as occupied. */
	UPROPERTY(EditDefaultsOnly, Category = "Trace|Match")
	float StartOccupiedRadius = 150.f;

	UPROPERTY(Transient)
	TObjectPtr<ATraceArenaBuilder> ArenaBuilder = nullptr;

	// --- Match phase machine ---------------------------------------------------------------
	void CheckMatchStartConditions();
	void StartWarmup();
	void CancelWarmup();
	void BeginMatch();
	void HandleMatchTimeExpired();
	void FinishMatch(ETraceTeam WinningTeam);

	// --- Helpers ---------------------------------------------------------------------------
	void SpawnArenaAndCore();
	void AssignTeamIfNeeded(APlayerController* NewPlayer);
	void ResetPlayersToSpawns();

	/** Respawns even when a dead pawn is still possessed; teleports the pawn if it is still alive. */
	void RestartPlayerFresh(AController* Controller);

	/**
	 * Gives a pawn to any non-spectator controller that has none. Covers the listen-server host,
	 * who logs in before the world (and therefore before the arena's player starts) exists.
	 */
	void EnsurePlayersHavePawns();

	/** Timer callback; the weak payload makes a disconnect during the respawn delay harmless. */
	void RespawnController(TWeakObjectPtr<AController> ControllerPtr);

	/** Cancels and forgets a controller's pending respawn, if it has one. */
	void ClearPendingRespawn(AController* Controller);

	int32 GetActivePlayerCount() const;
	ATraceGameState* GetTraceGameState() const;

	static ETraceTeam GetTeamForController(const AController* Controller);

	bool IsStartOccupied(const AActor* Start, const AController* ForPlayer) const;

	/** Squared distance to the closest living enemy, or a very large value when there are none. */
	float DistSqToNearestEnemy(const FVector& Location, ETraceTeam Team) const;

	void CompactTrackedCharacters();

private:
	TArray<TWeakObjectPtr<ATraceCharacter>> TrackedCharacters;

	/**
	 * At most one live respawn timer per controller.
	 *
	 * Without this, a player who dies, gets restarted early by a capture reset, then dies again
	 * would have two timers in flight, and the older (shorter-remaining) one would cut the second
	 * respawn short — RespawnDelay would silently stop being honoured. Setting a new timer clears
	 * the controller's previous handle first.
	 */
	TMap<TWeakObjectPtr<AController>, FTimerHandle> PendingRespawns;

	FTimerHandle WarmupTimerHandle;
	FTimerHandle MatchTimerHandle;
};
