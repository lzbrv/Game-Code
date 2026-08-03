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
class ATraceBotController;
class ATraceCharacter;
class ATraceCore;
class ATraceEndzone;
class ATraceGameState;
class ATraceTeamPlayerStart;

/**
 * Who gets the Core after a goal.
 *
 * The design doc specifies who starts each HALF but says nothing about what follows a score, so
 * this is an interpretation made configurable rather than a rule baked into code. The default is
 * American-football kickoff logic; changing the whole game's restart rule is one line in
 * DefaultGame.ini.
 */
UENUM()
enum class ECoreKickoffMode : uint8
{
	/** Kickoff to the team that was scored on, at their own endzone. The shipped assumption. */
	ScoredOnTeam = 0,
	/** Strict alternation regardless of who scored, starting from the team that kicked off the half. */
	AlternateTeams = 1,
	/** Nobody is granted the Core; it returns to the centre of the field and is contested. */
	Neutral = 2
};

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
 *
 * HALVES AND SIDES (spec §1)
 * InProgress is not one period any more, it is HalvesPerMatch periods of HalfDuration separated by
 * an interval:
 *
 *     BeginMatch -> BeginHalf(1) --(half expires)--> BeginHalfTimeBreak
 *                                                     [sides switch HERE, immediately]
 *                                 --(interval ends)-> BeginHalf(2) --(expires)--> FinishMatch
 *
 * The interval reuses ETraceMatchState::InProgress and is flagged by ATraceGameState::
 * bHalfTimeBreak, so every existing "is the match running?" test keeps working unchanged while
 * anything that must genuinely pause can ask.
 *
 * The side switch is the reason ATraceGameState::TeamOnNegativeSide exists. This class is its only
 * writer, and ApplyTeamSides() is the only place that re-points the endzone triggers and the spawn
 * pads at their new owners. Nothing in this codebase may map a team to an end any other way.
 */
UCLASS(config = Game)
class TRACE_API ATraceGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ATraceGameMode();

	//~ Begin AGameModeBase interface

	/**
	 * The arena is built here, NOT in BeginPlay. UEngine::LoadMap runs:
	 *
	 *   InitializeActorsForPlay [ InitGame -> RouteActorInitialize -> PreInitializeComponents ]
	 *   -> SpawnPlayActor [ Login -> FindPlayerStart, PostLogin -> RestartPlayer -> FindPlayerStart ]
	 *   -> World::BeginPlay [ GameMode::BeginPlay ]
	 *
	 * Building in BeginPlay is two steps too late: no ATraceTeamPlayerStart exists yet, so
	 * AGameModeBase::FindPlayerStart logs "PATHS NOT DEFINED" and falls back to
	 * World->GetWorldSettings() — the world origin, which is inside the floor slab and the centre
	 * pedestal. The player still *gets* a pawn there, so every "has no pawn?" safety net passes
	 * while the pawn is depenetration-stuck, the camera is inside the pedestal (black screen) and
	 * every hitscan starts inside geometry.
	 *
	 * PreInitializeComponents rather than InitGame because Super() creates the GameState here, and
	 * the Core is published on it.
	 */
	virtual void PreInitializeComponents() override;

	/** Captures the URL options so "?bots=0" can turn the AI fill off for one session. */
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;

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

	/**
	 * Server: award a point if @p InCharacter is the Core carrier RIGHT NOW and is standing inside an
	 * endzone their team scores in.
	 *
	 * Called by ATraceCore::GrantTo on every possession change, which is the case a trigger volume
	 * cannot see: a completed pass to a teammate who is ALREADY standing in the enemy endzone (or a
	 * kill steal taken in there). Nothing overlaps-begins for a player who is not moving, so the
	 * test has to be re-run against the possession event rather than against the movement event.
	 *
	 * The geometry is read off the ATraceEndzone volume itself — never from field bounds or any
	 * other second copy of where the endzone is — so it stays correct however the zones are sized.
	 *
	 * @return true when a point was awarded.
	 */
	bool CheckEndzoneScoreForCarrier(ATraceCharacter* InCharacter, const TCHAR* Reason);

	void RegisterCharacter(ATraceCharacter* InCharacter);
	void UnregisterCharacter(ATraceCharacter* InCharacter);

	/**
	 * The server's roster of live characters. Used by the weapon's lag-compensated resolver and by
	 * the trail's trip test, so it is hot: entries are weak and may be stale — always .Get().
	 */
	const TArray<TWeakObjectPtr<ATraceCharacter>>& GetTrackedCharacters() const;

	ATraceCore* GetCore() const;

	/** Smaller team wins, ties go to Blue. Returns None when both teams are at PlayersPerTeam. */
	ETraceTeam PickTeamForNewPlayer() const;

	/** Living, non-spectating members of @p Team that currently possess an alive ATraceCharacter. */
	int32 CountLivingOnTeam(ETraceTeam Team) const;

	// ------------------------------------------------------------------------------------------
	// Match structure (spec §1). All `config`, so DefaultGame.ini tunes the whole format.
	// ------------------------------------------------------------------------------------------

	/** Seconds of play in ONE half. Two of these is the match. */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	float HalfDuration = 600.f;

	/** Periods of play per match. 2 is the spec; 1 restores the old single-period behaviour. */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	int32 HalvesPerMatch = 2;

	/** Length of the interval between halves. Long enough to read the banner, short enough to sit through. */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	float HalfTimeBreakDuration = 12.f;

	/**
	 * Seconds between death and respawn. Spec §1 sets this to 3.
	 *
	 * Deliberately here and not in UTraceSettings: the respawn delay is a RULE, this class is the
	 * rules, and UTraceSettings::RespawnDelay has no enforcement behind it. Clients no longer need
	 * to know it either — the deadline replicates on ATracePlayerState::RespawnEndServerTime.
	 */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	float RespawnDelay = 3.f;

	/**
	 * "Team A" in the doc's phrase "the core starts with Team A in the first half". Team B (its
	 * opponent) starts the second half, and so on alternately if HalvesPerMatch is ever raised.
	 */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	ETraceTeam FirstHalfCoreTeam = ETraceTeam::Blue;

	/** Who receives the Core after a goal. See ECoreKickoffMode. */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	ECoreKickoffMode KickoffMode = ECoreKickoffMode::ScoredOnTeam;

	/** Points awarded for wiping the whole enemy team at once (spec §1). */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	int32 WipeBonusPoints = 2;

	/**
	 * Mercy rule. OFF by design: the format is now a fixed two halves of ten minutes, and ending at
	 * UTraceSettings::ScoreToWin would cut the second half — and the side switch that justifies it —
	 * out of most matches. Flip it on to restore "first to N wins outright".
	 */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	bool bEndMatchAtScoreToWin = false;

	/**
	 * Spec §1: players respawn in their OWN team's endzone rather than on the generic pads in front
	 * of it. The pads are built by this class (BuildEndzoneSpawnPads) because their ownership has to
	 * flip at half time and the arena builder's are baked at build time.
	 */
	UPROPERTY(config, EditDefaultsOnly, Category = "Trace|Match")
	bool bRespawnInOwnEndzone = true;

protected:
	/** Spawned at the origin if the level does not already contain an ATraceArenaBuilder. */
	UPROPERTY(EditDefaultsOnly, Category = "Trace|Classes")
	TSubclassOf<ATraceArenaBuilder> ArenaBuilderClass;

	UPROPERTY(EditDefaultsOnly, Category = "Trace|Classes")
	TSubclassOf<ATraceCore> CoreClass;

	/** Possesses the same ATraceCharacter humans do, so every game rule applies to bots unchanged. */
	UPROPERTY(EditDefaultsOnly, Category = "Trace|Classes")
	TSubclassOf<ATraceBotController> BotControllerClass;

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
	void FinishMatch(ETraceTeam WinningTeam);

	// --- Halves ------------------------------------------------------------------------------

	/**
	 * Starts period @p HalfIndex: assigns sides, resets the field, restarts the clock and hands the
	 * Core to whichever team kicks off this half. Idempotent enough to be safe from a timer.
	 */
	void BeginHalf(int32 HalfIndex);

	/** The half clock ran out. Either the interval or full time, depending on HalvesPerMatch. */
	void HandleHalfExpired();

	/**
	 * Stops the clock, switches ends IMMEDIATELY (so the interval is spent looking at the field you
	 * are about to attack) and puts everyone on their new spawn pads with no Core in play.
	 */
	void BeginHalfTimeBreak();

	/** Interval over: rolls into the next half. */
	void EndHalfTimeBreak();

	/** Sides for @p HalfIndex: Blue on -X in odd halves, swapped in even ones. */
	ETraceTeam GetNegativeSideTeamForHalf(int32 HalfIndex) const;

	/** The team the Core is granted to at the start of @p HalfIndex (FirstHalfCoreTeam, alternating). */
	ETraceTeam GetKickoffTeamForHalf(int32 HalfIndex) const;

	/**
	 * Re-points every side-dependent actor at its new owner: the two ATraceEndzone triggers and
	 * every ATraceTeamPlayerStart, keyed off which side of the field's centre they physically sit
	 * on. Publishes the assignment on the GameState first, because that is what everything else
	 * (bots, HUD) reads.
	 */
	void ApplyTeamSides(ETraceTeam TeamOnNegativeSide);

	/**
	 * Spawns five respawn pads per side INSIDE each endzone, mid-depth, reusing the arena builder's
	 * own pads for lateral spread, height and facing. Tagged so ChoosePlayerStart can prefer them
	 * and so a rebuild never doubles them up. Idempotent.
	 */
	void BuildEndzoneSpawnPads();

	/** True when a pawn-sized capsule at @p Location would be stuck in world geometry. */
	bool IsSpawnLocationBlocked(const FVector& Location) const;

	// --- Core possession ----------------------------------------------------------------------
	//
	// EVERY call this class makes into ATraceCore goes through these two functions. The Core is
	// being rewritten from an actor with physics into a replicated possession status, so keeping the
	// coupling to exactly two call sites is what makes that swap a two-function edit here.

	/** Takes the Core out of play: no holder, parked at the arena centre. */
	void ReleaseCore();

	/** Kicks off to @p Team: the Core is released and granted to one of their living players. */
	void GrantCoreToTeam(ETraceTeam Team);

	// --- Wipe bonus (spec §1) ------------------------------------------------------------------

	/**
	 * Called after any death. Awards WipeBonusPoints to the opposing team the instant @p DeadTeam
	 * has nobody left alive, and latches so the same wipe cannot pay twice.
	 */
	void EvaluateWipeBonus(ETraceTeam DeadTeam);

	/** Clears @p Team's wipe latch once one of them is breathing again, re-arming the bonus. */
	void ClearWipeLatchIfAlive(ETraceTeam Team);

	bool IsWipeLatched(ETraceTeam Team) const;
	void SetWipeLatched(ETraceTeam Team, bool bLatched);

	/**
	 * Travels back to the title screen once the post-match results screen has had its time.
	 *
	 * The match used to end and simply sit there, which — with captures landing seconds apart —
	 * read to a player as the game restarting itself at random. PostMatch now has a defined exit,
	 * and TraceMatchFlow::PostMatchDuration is the single number both this timer and the HUD's
	 * on-screen countdown are driven from.
	 */
	void ReturnToMainMenu();

	// --- Helpers ---------------------------------------------------------------------------

	/** Finds or spawns the arena builder and makes it build. Idempotent; needs no GameState. */
	void EnsureArenaBuilt();

	/** Spawns the Core and publishes it on the GameState. Idempotent; requires the GameState. */
	void SpawnCoreIfNeeded();

	void AssignTeamIfNeeded(APlayerController* NewPlayer);

	/** Every controller with a pawn — humans AND bots — back onto a freshly chosen team pad. */
	void ResetPlayersToSpawns();

	// --- Bots --------------------------------------------------------------------------------

	/**
	 * Brings both teams up to (or back down to) PlayersPerTeam with AI.
	 *
	 * Idempotent and self-correcting in both directions, which is what makes it safe to call from
	 * BeginPlay, after a login and after a logout: a second human joining a listen server pushes a
	 * bot out rather than making an 11th player.
	 */
	void UpdateBotFill();

	/** Runs UpdateBotFill() next tick. PlayerArray is only truthful after Login/Logout complete. */
	void ScheduleBotFill();

	/** Config AND "?bots=0". The URL wins, so a session can be forced human-only without editing ini. */
	bool AreBotsEnabled() const;

	/** Spawns one bot on @p Team, names it, and gives it a pawn. Null on failure. */
	ATraceBotController* SpawnBotForTeam(ETraceTeam Team);

	/** Destroys one bot (and its pawn) on @p Team. Used when a human takes the slot back. */
	void RemoveOneBotFromTeam(ETraceTeam Team);

	/** Drops destroyed bots from the roster. */
	void CompactBots();

	/**
	 * If both teams are full of bots and a human is arriving, evict a bot so the human gets a team
	 * rather than spawning teamless (which would keep them out of scoring and friendly-fire tests).
	 */
	void FreeSlotForHuman();

	/** Living bots currently assigned to @p Team. */
	int32 CountBotsOnTeam(ETraceTeam Team) const;

#if !UE_BUILD_SHIPPING
	/** -TraceBotDebug: logs the whole roster with positions every few seconds. Verification aid. */
	void LogBotRoster();
#endif

	/** Respawns even when a dead pawn is still possessed; teleports the pawn if it is still alive. */
	void RestartPlayerFresh(AController* Controller);

	// NOTE: there used to be an EnsurePlayersHavePawns() here, meant to rescue the listen-server host
	// who logs in before the world begins play. It could never fire: the engine's WorldSettings
	// fallback always hands out a pawn, so "GetPawn() == nullptr" was never true for the case it was
	// written for. ResetPlayersToSpawns() replaces it — it relocates a misplaced *living* player as
	// well as restarting a pawnless one.

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

	/** Every bot this GameMode has spawned. Weak: a bot destroyed elsewhere must not be resurrected. */
	TArray<TWeakObjectPtr<ATraceBotController>> Bots;

	/** Monotonic, so two bots never share a scoreboard name even after churn. */
	int32 NextBotNumber = 1;

	/** From "?bots=N": -1 when the option is absent, 0 to disable, >0 as a total bot cap. */
	int32 BotCountFromURL = -1;

	/** The endzone respawn pads this class spawned, so a re-run does not double them. */
	TArray<TWeakObjectPtr<ATraceTeamPlayerStart>> EndzoneStarts;

	/**
	 * Wipe-bonus latches, indexed by team. A wipe pays once; the latch clears the moment one of the
	 * wiped team is alive again, which is what "do not re-award until at least one has respawned"
	 * means in code.
	 */
	bool bBlueWipeLatched = false;
	bool bOrangeWipeLatched = false;

	/** Team the Core was granted to at the last kickoff. Only read by ECoreKickoffMode::AlternateTeams. */
	ETraceTeam LastKickoffTeam = ETraceTeam::None;

	/**
	 * World time of the last score this class actually processed, for the one-score-at-a-time guard
	 * in NotifyScored.
	 *
	 * There are now two ways in: the ATraceEndzone trigger/poll, and CheckEndzoneScoreForCarrier on
	 * a possession change. A pass completed inside the endzone satisfies both within the same tenth
	 * of a second, and a capture must never pay twice. Seeded far in the past (world time starts at
	 * 0) so the first score of a match is never swallowed.
	 */
	float LastScoreProcessedWorldTime = -10000.f;

	FTimerHandle WarmupTimerHandle;
	FTimerHandle MatchTimerHandle;
	FTimerHandle HalfTimeTimerHandle;
	FTimerHandle BotFillTimerHandle;
	FTimerHandle ReturnToMenuTimerHandle;

#if !UE_BUILD_SHIPPING
	FTimerHandle BotDebugTimerHandle;

	// --- -TraceTripTest: scripted verification of the two rules this pass fixed ------------------
	//
	// Off unless the switch is on the command line, and compiled out of a shipping build entirely.
	// It exists because both fixes are about cases that are rare in organic play but decide matches:
	// dashing through the trace a turnover left behind, and a pass completed to a teammate who is
	// already standing in the enemy endzone. Waiting for ten bots to stumble into them is not
	// evidence. This drives each one on purpose and logs the outcome.
	FTimerHandle VerifyTimerHandle;

	/** Step counter within one scripted scenario; see RunVerificationStep(). */
	int32 VerifyStep = 0;

	/** How many trace-dash scenarios have been run, so "reliably" means more than once. */
	int32 VerifyIteration = 0;

	/** The player whose trace is being dashed through, and the enemy doing the dashing. */
	TWeakObjectPtr<ATraceCharacter> VerifyTraceOwner;
	TWeakObjectPtr<ATraceCharacter> VerifyTripper;

	/** Far side of the scripted dash, applied one frame after the near side so the sweep crosses. */
	FVector VerifyDashEnd = FVector::ZeroVector;

	/** Score snapshot taken before the scripted endzone pass, so the point can be measured. */
	int32 VerifyScoreBefore = 0;

	/** Drives the scenarios. Fires every frame-ish; each call advances one step. */
	void RunVerificationStep();
#endif
};
