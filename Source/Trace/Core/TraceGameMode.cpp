// Copyright (c) Trace. All Rights Reserved.

#include "Core/TraceGameMode.h"

#include "Components/BoxComponent.h"                     // endzone trigger extent (see CheckEndzoneScoreForCarrier)
#include "EngineUtils.h"                                 // TActorIterator
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"     // StopMovementImmediately on respawn
#include "Movement/TraceCharacterMovementComponent.h"    // StartDash (the -TraceTripTest harness)
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"                      // ParseOption / GetIntOption ("?bots=")
#include "Math/NumericLimits.h"
#include "HAL/IConsoleManager.h"                       // Trace.HalfTime.Verify (spec v9 §11)
#include "Misc/CommandLine.h"                            // -TraceBotDebug
#include "Misc/Parse.h"
#include "TimerManager.h"

#include "AI/TraceBotController.h"
#include "Abilities/TraceAbilityComponent.h"   // v14 §3 — the roster rules this class defers to
#include "Core/TraceCharacter.h"
#include "Core/TraceGameState.h"
#include "Core/TracePlayerController.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Gameplay/TraceEndzone.h"
#include "Gameplay/TraceHealthComponent.h"
#include "Gameplay/TraceTrailComponent.h"
#include "Trace.h"
#include "TraceSettings.h"
#include "TraceTypes.h"
#include "UI/TraceHUD.h"
#include "UI/TraceMatchOptions.h"
#include "World/TraceArenaBuilder.h"
#include "World/TraceTeamPlayerStart.h"

namespace
{
	/**
	 * THE RED ARM FOR THE CHARACTER-SELECT SLICE (spec v14 §3).
	 *
	 * 1 (default): this class enforces the two rules it owns — a request for a character a team-mate
	 * holds is REFUSED with a reason, and a select screen that runs out of time AUTO-ASSIGNS. 0
	 * removes both, and Trace.Characters.Verify must then FAIL on exactly those two assertions.
	 *
	 * *** WHAT THE ARM CAN AND CANNOT REACH, STATED PLAINLY, BECAUSE THE FIRST VERSION OF IT LIED. ***
	 * The first attempt bypassed only this class's pre-flight availability test — and the red run came
	 * back 19/19 GREEN. Two independent things were catching it: the post-verify below (which reads
	 * what the component actually holds rather than trusting the pre-flight), and
	 * UTraceAbilityComponent::ServerSetCharacter's own refusal, which lives in another ownership slice
	 * this switch cannot touch at all. A red arm that reports green is worse than no red arm — it is
	 * evidence that has been manufactured — so the arm now also skips the post-verify.
	 *
	 * That means the arm reaches THIS SLICE'S enforcement and nothing else. The assertion "the loser
	 * does not end up holding the contested character" therefore stays GREEN under the red arm, and
	 * that is a result rather than a defect: it says the roster rule is enforced twice, independently,
	 * and that only the REPORTING of it lives here.
	 *
	 * Cheat-only and compiled out of shipping, so no player can reach it and no ini can turn it off.
	 */
#if !UE_BUILD_SHIPPING
	int32 GTraceEnforceSelectRules = 1;

	FAutoConsoleVariableRef CVarTraceEnforceSelectRules(
		TEXT("Trace.Characters.EnforceSelectRules"),
		GTraceEnforceSelectRules,
		TEXT("1 (default, spec v14 3): the character-select slice refuses a pick a team-mate already "
		     "holds and auto-assigns on the timeout. 0 removes both so Trace.Characters.Verify can be "
		     "shown to FAIL - the red arm. Prefer 'Trace.Characters.VerifyRed', which scopes it to one "
		     "run. Dev only; no ini override, absent from shipping."),
		ECVF_Cheat);
#endif
}

namespace TraceGameModeConstants
{
	/** Fallback Core spawn height if the arena builder is missing or returns garbage. */
	static const FVector FallbackCoreLocation(0.f, 0.f, 200.f);

	/**
	 * Seconds between PollCharacterSelect() passes. 4 Hz: the screen it opens is a menu, not a
	 * mechanic, and a quarter second of latency on "your select screen appeared" is imperceptible
	 * against the auto-pick timeout it is measured against.
	 */
	static constexpr float CharacterSelectPollInterval = 0.25f;

	/** Smallest respawn delay we will honour; zero would respawn inside the death frame. */
	static constexpr float MinRespawnDelay = 0.1f;

	/**
	 * Treated as "no warm-up". A literal is used rather than KINDA_SMALL_NUMBER / UE_KINDA_SMALL_NUMBER
	 * because those spellings churned during the 5.x line and this file must compile unchanged on
	 * 5.4 through 5.8.
	 */
	static constexpr float ZeroDurationEpsilon = 0.001f;

	/** Seconds between -TraceBotDebug roster dumps. */
	static constexpr float BotDebugInterval = 3.f;

	/** Tag on the respawn pads this class builds inside the endzones. */
	static const FName EndzoneStartTag(TEXT("TraceEndzoneStart"));

	/**
	 * Capsule used to test a candidate endzone respawn pad for solid geometry.
	 *
	 * These are the shipped ATraceCharacter capsule dimensions. They are duplicated rather than read
	 * off the pawn CDO because the test runs during PreInitializeComponents, before any pawn exists,
	 * and being 2uu wrong here only costs a slightly conservative pad placement.
	 */
	static constexpr float SpawnProbeRadius = 34.f;
	static constexpr float SpawnProbeHalfHeight = 88.f;

	/** How many times a blocked endzone pad steps back towards the goal line before giving up. */
	static constexpr int32 SpawnProbeSteps = 4;

	/** Fraction of the endzone depth each of those steps covers. */
	static constexpr float SpawnProbeStepFraction = 0.18f;

	/**
	 * Minimum seconds between two accepted scores. See NotifyScored: the endzone volume and the
	 * possession-change check are two detectors for one event, and the reset the first one starts
	 * (kickoff + teleporting every pawn) does not finish inside a frame. Comfortably shorter than
	 * any real gap between captures — the kickoff delay alone is longer.
	 */
	static constexpr float ScoreDebounceSeconds = 0.5f;

	/**
	 * Seconds between checks of UTraceSettings::ScoringMode for a live edit (spec v4 §7).
	 *
	 * Half a second is imperceptible to somebody dragging a toggle in the details panel and is two
	 * ticks per second of one enum comparison, which does not register on any profile. It is not a
	 * hook because PostEditChangeProperty is editor-only and the toggle has to keep working in a
	 * packaged A/B build.
	 */
	static constexpr float ScoringModePollInterval = 0.5f;

	/**
	 * Spec v9 §11. Seconds between dead-ball checks while a half/full time is pending.
	 *
	 * 20 Hz. Two accessor reads on one actor, and only during the seconds between a clock expiring
	 * and the next break in play — a handful of times per match. Fast enough that the whistle lands
	 * within a frame or two of the turnover that caused it, which is what stops the break feeling
	 * detached from the event that earned it.
	 */
	static constexpr float PendingPeriodEndPollInterval = 0.05f;

	/**
	 * Spec v9 §11, mode B. Speed below which a LOOSE Core counts as "the ball has dropped".
	 *
	 * The Core's own rest test (ATraceCore::ServerTickLooseCore) is private to that class, so this
	 * is the same question asked from outside: a Core in flight is never this slow, including at the
	 * apex of a lob, where all of its speed is horizontal. Comfortably above the residual velocity a
	 * Core carries while it settles, so a landing is detected on the frame it lands rather than
	 * several frames of jitter later.
	 */
	static constexpr float PendingPeriodEndCoreRestSpeed = 150.f;
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
	BotControllerClass = ATraceBotController::StaticClass();

	// Players run around during warm-up rather than sitting in spectator.
	bStartPlayersAsSpectators = false;

	// ATracePlayerState::CopyProperties (which carries Team/Kills/Deaths) only runs if travel is
	// seamless, so enable it here rather than leaving the behaviour to whoever calls ServerTravel.
	bUseSeamlessTravel = true;
}

void ATraceGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	// Read once, here, rather than off OptionsString later: this is the only place the raw URL is
	// guaranteed to be exactly what the session was opened with.
	//   ?bots=0  -> no bots at all this session, whatever the config says
	//   ?bots=N  -> at most N bots in total
	//   (absent) -> UTraceSettings::bFillTeamsWithBots decides
	BotCountFromURL = UGameplayStatics::HasOption(Options, TEXT("bots"))
		? FMath::Max(0, UGameplayStatics::GetIntOption(Options, TEXT("bots"), 0))
		: -1;

	if (BotCountFromURL >= 0)
	{
		UE_LOG(LogTraceGame, Log, TEXT("URL option '?bots=%d' overrides the configured bot fill."), BotCountFromURL);
	}

	// "?difficulty=easy|normal|hard" arrives from the title screen's Play button. The option is
	// absent when the map is opened directly (a headless test run, a level-editor Play), and
	// UTraceSettings then falls back to "-difficulty=" on the command line and finally to the
	// configured default rather than leaving whoever launched it against the hardest bots.
	//
	// This is THE resolution point for bot difficulty, and it is forced. InitGame runs exactly once
	// per map load, before any bot's BeginPlay, so forcing here is what lets a player return to the
	// title screen, pick a different difficulty and actually get it — UTraceSettings otherwise
	// latches the first answer for the whole process, which silently pinned every match after the
	// first to the first match's bots.
	//
	// The bot controllers still call the same function unforced (see ATraceBotController::BeginPlay)
	// so a bot spawned into a map that somehow bypassed InitGame is not left unresolved. Those calls
	// are no-ops once this one has run.
	UTraceSettings::ResolveBotDifficultyFromOptions(Options, /*bForceReresolve=*/true);

	// Which of the two games (spec v4 §7). Same three-way resolution as the difficulty above and for
	// the same reason: the title screen sends it on the travel URL, a headless run sends it on the
	// command line, and everything else falls back to the Project Settings page. Must happen HERE —
	// InitGame is the last point before PreInitializeComponents builds the arena, and the arena is
	// one of the things the mode decides the shape of.
	ResolveScoringMode(Options);

	// Characters on or off for this match (spec v14 §3's settings toggle). AFTER ResolveScoringMode,
	// which is not merely tidy: AreCharactersEnabled() gates on mode B, and resolving the toggle
	// before the mode was published would log "characters are ON" for a mode A match that will never
	// have any. Same three-way resolution as the mode and the difficulty above.
	ResolveCharactersEnabled(Options);

	// Test hook for the MERCY RULE, and it earns its place for the same reason the half-length hooks
	// above do: at the shipped threshold of 8 the rule fires perhaps once in a long lopsided match,
	// which is not something an automated run can wait for. Lower it and the rule becomes reachable
	// in seconds; set it to 0 and a scripted run is guaranteed to reach full time instead.
	//
	//     /Game/Maps/Arena?mercy=2        (from a travel URL)
	//     -TraceMercyLead=2               (from the command line, for headless runs)
	//
	// Written into the CDO rather than into a member for the same reason the mode is: UTraceSettings
	// is the storage the rule is read from at its point of use, so there is one value, not two.
	// Note the '?' separator rule spelled out above — '&' silently swallows the next option.
	int32 MercyOverride = -1;
	if (UGameplayStatics::HasOption(Options, TEXT("mercy")))
	{
		MercyOverride = UGameplayStatics::GetIntOption(Options, TEXT("mercy"), -1);
	}
	else
	{
		FParse::Value(FCommandLine::Get(), TEXT("TraceMercyLead="), MercyOverride);
	}

	if (MercyOverride >= 0)
	{
		if (UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>())
		{
			MutableSettings->MercyRuleLead = MercyOverride;
			UE_LOG(LogTraceGame, Log, TEXT("Mercy-rule lead overridden to %d (%s)."),
				MercyOverride, (MercyOverride == 0) ? TEXT("disabled; the clock is the only end") : TEXT("enabled"));
		}
	}

	// Test hooks for the match format. A full match is now 2 x HalfDuration, which at the shipped
	// ten minutes a half is twenty minutes of wall clock — far too long for an automated run to ever
	// reach half time, let alone full time. These make the whole structure reachable in seconds:
	//
	//     /Game/Maps/Arena?half=20?breaklen=4            (from a travel URL)
	//     -TraceHalfSeconds=20 -TraceHalfTimeSeconds=4   (from the command line, for headless runs)
	//
	// Note the separator: UE URL options are chained with '?', NOT with '&'. Writing "?half=20&
	// breaklen=4" parses as ONE option called "half" whose value is "20&breaklen=4" — which
	// FCString::Atoi happily reads as 20, so the first option appears to work and the second is
	// silently ignored. That is exactly what happened the first time this was tested.
	//
	// Deliberately not gated on !UE_BUILD_SHIPPING: they are URL/CLI options nobody can reach by
	// accident, and a live server operator shortening a half is a legitimate thing to want.
	float OverrideSeconds = 0.f;
	if (UGameplayStatics::HasOption(Options, TEXT("half")))
	{
		OverrideSeconds = static_cast<float>(UGameplayStatics::GetIntOption(Options, TEXT("half"), 0));
	}
	else
	{
		FParse::Value(FCommandLine::Get(), TEXT("TraceHalfSeconds="), OverrideSeconds);
	}

	if (OverrideSeconds > 0.f)
	{
		HalfDuration = OverrideSeconds;
		UE_LOG(LogTraceGame, Log, TEXT("Half duration overridden to %.1fs."), HalfDuration);
	}

	float BreakOverrideSeconds = 0.f;
	if (UGameplayStatics::HasOption(Options, TEXT("breaklen")))
	{
		BreakOverrideSeconds = static_cast<float>(UGameplayStatics::GetIntOption(Options, TEXT("breaklen"), 0));
	}
	else
	{
		FParse::Value(FCommandLine::Get(), TEXT("TraceHalfTimeSeconds="), BreakOverrideSeconds);
	}

	if (BreakOverrideSeconds > 0.f)
	{
		HalfTimeBreakDuration = BreakOverrideSeconds;
		UE_LOG(LogTraceGame, Log, TEXT("Half-time interval overridden to %.1fs."), HalfTimeBreakDuration);
	}
}

void ATraceGameMode::PreInitializeComponents()
{
	// Super creates the GameState (AGameModeBase::PreInitializeComponents). SpawnCoreIfNeeded
	// publishes the Core on it, so Super has to run first.
	Super::PreInitializeComponents();

	// BEFORE the arena is built and before anybody logs in: the mode decides what the ends of the
	// field are (endzones or goals) and what the Core is, so every system that reads it during its
	// own construction has to find the right answer already published. Super created the GameState
	// immediately above, which is the earliest this can possibly run.
	PublishScoringMode();

	// See the header for the LoadMap ordering this exists to get ahead of. Short version: every
	// ATraceTeamPlayerStart must exist before AGameModeBase::Login calls FindPlayerStart, and Login
	// happens before any BeginPlay in the world.
	//
	// Re-entrancy is safe. ULevel::RouteActorInitialize already loops until the actor count
	// stabilises specifically because pre-initialisation may spawn actors, and every pass skips
	// actors that are already initialised. UWorld::bActorsInitialized is set before InitGame, so the
	// actors we spawn here get PreInitializeComponents/PostInitializeComponents inline.
	EnsureArenaBuilt();
	SpawnCoreIfNeeded();

	// Both of these must land before AGameModeBase::Login runs FindPlayerStart, for the same reason
	// the arena itself does: the very first spawn already has to happen on the right side, in the
	// right endzone. ApplyTeamSides is what makes "the right side" a runtime answer.
	ApplyTeamSides(GetNegativeSideTeamForHalf(1));
	BuildEndzoneSpawnPads();
}

void ATraceGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Belt and braces for paths that skip PreInitializeComponents' happy case — seamless travel, a
	// Blueprint subclass that forgets to call Super, PIE quirks. Both are idempotent.
	PublishScoringMode();
	EnsureArenaBuilt();
	SpawnCoreIfNeeded();
	ApplyTeamSides(GetNegativeSideTeamForHalf(1));
	BuildEndzoneSpawnPads();

	// Anyone who logged in before the arena existed is dragged onto a real pad here. This must be
	// ResetPlayersToSpawns() rather than a "does this controller have a pawn?" test: the engine's
	// WorldSettings fallback hands a MISPLACED player a perfectly valid pawn, so a pawn-existence
	// test never fires for the case that matters. With PreInitializeComponents doing its job this is
	// a cheap no-op teleport onto the pad the player is already standing on.
	ResetPlayersToSpawns();

	// Bots are filled in BEFORE the phase machine runs, on purpose: they carry real PlayerStates, so
	// they count towards MinPlayersToStart. A solo human plus nine bots therefore leaves
	// WaitingForPlayers exactly like a full lobby would, which is the whole point of the mode.
	UpdateBotFill();

	// The host is already logged in, so this is the first point at which the phase machine may run.
	CheckMatchStartConditions();

	// Makes the A/B toggle live for the rest of the session. See PollScoringModeSetting().
	GetWorldTimerManager().SetTimer(ScoringModePollHandle, this, &ATraceGameMode::PollScoringModeSetting,
		TraceGameModeConstants::ScoringModePollInterval, /*bLoop=*/true,
		TraceGameModeConstants::ScoringModePollInterval);

	// Character selection (spec v14 §3). Started here rather than from PostLogin because the listen
	// server's host logs in during map load, before this class has a GameState to read the mode from
	// — and because the poll has to keep running to close screens when the A/B toggle switches the
	// match to mode A. First fire is immediate so the host is offered a pick on the opening frames
	// rather than a quarter second in. See PollCharacterSelect().
	GetWorldTimerManager().SetTimer(CharacterSelectPollHandle, this, &ATraceGameMode::PollCharacterSelect,
		TraceGameModeConstants::CharacterSelectPollInterval, /*bLoop=*/true,
		TraceGameModeConstants::CharacterSelectPollInterval);

#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("TraceBotDebug")))
	{
		GetWorldTimerManager().SetTimer(BotDebugTimerHandle, this, &ATraceGameMode::LogBotRoster,
			TraceGameModeConstants::BotDebugInterval, /*bLoop=*/true, /*FirstDelay=*/1.f);
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("TraceTripTest")))
	{
		// One step per fire, at roughly one frame each: the scripted dash needs its start and end
		// positions on two CONSECUTIVE trip-test ticks, because what the test evaluates is the
		// swept segment between them.
		GetWorldTimerManager().SetTimer(VerifyTimerHandle, this, &ATraceGameMode::RunVerificationStep,
			0.016f, /*bLoop=*/true, /*FirstDelay=*/2.f);
	}
#endif
}

bool ATraceGameMode::ShouldSpawnAtStartSpot(AController* Player)
{
	// Never reuse AController::StartSpot — every respawn must re-evaluate ChoosePlayerStart so the
	// "furthest from any live enemy" rule keeps applying.
	return false;
}

void ATraceGameMode::EnsureArenaBuilt()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	if (ArenaBuilder == nullptr)
	{
		ArenaBuilder = ATraceArenaBuilder::Get(World);
	}

	if (ArenaBuilder == nullptr)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		UClass* ClassToSpawn = (ArenaBuilderClass != nullptr) ? ArenaBuilderClass.Get() : ATraceArenaBuilder::StaticClass();
		ArenaBuilder = World->SpawnActor<ATraceArenaBuilder>(ClassToSpawn, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);

		UE_LOG(LogTraceGame, Log, TEXT("No ATraceArenaBuilder in the level; spawned one at the origin."));
	}

	// The builder must have run before anyone can trust GetCoreSpawnLocation() or expect its player
	// starts to exist, and spawning it does NOT guarantee that. We are called from
	// PreInitializeComponents, where the world has not begun play and AActor::BeginPlayCallDepth is
	// zero, so AActor::PostActorConstruction will not dispatch BeginPlay for a freshly spawned actor
	// — and a level-placed builder has not reached BeginPlay either. EnsureBuilt() is the explicit,
	// idempotent way to make it build regardless of lifecycle stage.
	if (ArenaBuilder != nullptr)
	{
		ArenaBuilder->EnsureBuilt();
	}
}

void ATraceGameMode::SpawnCoreIfNeeded()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("ATraceGameMode::SpawnCoreIfNeeded: no ATraceGameState, cannot publish the Core."));
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
		UE_LOG(LogTraceGame, Error, TEXT("ATraceGameMode::SpawnCoreIfNeeded: failed to spawn the Core."));
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

	// A human arriving on a listen server takes a slot back off the AI. Deferred by a tick because
	// PlayerArray is only truthful once Login/PostLogin have finished.
	ScheduleBotFill();

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

	// Before PickTeamForNewPlayer is consulted: bots occupy real team slots, so with a full AI fill
	// in place PickTeamForNewPlayer would correctly report "both teams full" and the human would
	// spawn teamless — no scoring, no friendly-fire protection, no spawn side.
	FreeSlotForHuman();

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
	// Last moment before AGameModeBase::FindPlayerStart silently falls back to
	// World->GetWorldSettings() (the origin, inside the floor and the pedestal). If anything ever
	// asks for a start before the arena exists, build it right here. Deliberately ahead of the
	// Player null-check: AGameModeBase::Login calls us with Player == nullptr, and that call is the
	// FIRST one, so guarding after the early-return would leave the important path unprotected.
	EnsureArenaBuilt();

	UWorld* World = GetWorld();
	if (World == nullptr || Player == nullptr)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	const ETraceTeam Team = GetTeamForController(Player);

	// Three pools, most specific first:
	//   EndzoneStarts - the pads this class builds INSIDE each endzone. Spec §1 puts respawns here.
	//   TeamStarts    - the arena builder's pads in front of the endzone. The fallback if a pad
	//                   could not be placed (blocked by geometry) or the feature is switched off.
	//   AnyStarts     - anything at all, for a teamless player or a hand-built level.
	//
	// Membership in all three is keyed off Start->Team, which ApplyTeamSides() rewrites at half
	// time. That is the whole side switch as far as spawning is concerned.
	TArray<ATraceTeamPlayerStart*> EndzoneTeamStarts;
	TArray<ATraceTeamPlayerStart*> TeamStarts;
	TArray<ATraceTeamPlayerStart*> AnyStarts;
	for (TActorIterator<ATraceTeamPlayerStart> It(World); It; ++It)
	{
		ATraceTeamPlayerStart* Start = *It;
		if (!IsValid(Start))
		{
			continue;
		}

		const bool bIsEndzonePad = Start->ActorHasTag(TraceGameModeConstants::EndzoneStartTag);

		// An endzone pad is never a generic fallback: it is deep in a scoring volume, which is the
		// last place a teamless spectator-ish pawn should materialise.
		if (!bIsEndzonePad)
		{
			AnyStarts.Add(Start);
		}

		if (Team != ETraceTeam::None && Start->Team == Team)
		{
			if (bIsEndzonePad)
			{
				EndzoneTeamStarts.Add(Start);
			}
			else
			{
				TeamStarts.Add(Start);
			}
		}
	}

	const TArray<ATraceTeamPlayerStart*>& Pool =
		(bRespawnInOwnEndzone && EndzoneTeamStarts.Num() > 0) ? EndzoneTeamStarts :
		(TeamStarts.Num() > 0) ? TeamStarts : AnyStarts;

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
		const ATraceCharacter* InCharacter = WeakCharacter.Get();
		if (InCharacter == nullptr || InCharacter == OwnPawn || !InCharacter->IsAlive())
		{
			continue;
		}

		if (FVector::DistSquared(StartLocation, InCharacter->GetActorLocation()) <= RadiusSquared)
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
		const ATraceCharacter* InCharacter = WeakCharacter.Get();
		if (InCharacter == nullptr || !InCharacter->IsAlive())
		{
			continue;
		}

		// A teamless spawner treats everyone as a threat; otherwise our own team is not one.
		if (Team != ETraceTeam::None && InCharacter->GetTeam() == Team)
		{
			continue;
		}

		const float DistanceSquared = static_cast<float>(FVector::DistSquared(Location, InCharacter->GetActorLocation()));
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
// InCharacter tracking
// ---------------------------------------------------------------------------------------------

void ATraceGameMode::RegisterCharacter(ATraceCharacter* InCharacter)
{
	if (InCharacter == nullptr)
	{
		return;
	}

	CompactTrackedCharacters();
	TrackedCharacters.AddUnique(InCharacter);
}

void ATraceGameMode::UnregisterCharacter(ATraceCharacter* InCharacter)
{
	if (InCharacter == nullptr)
	{
		return;
	}

	TrackedCharacters.Remove(InCharacter);
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
		// bIsCarrier is deliberately NOT written here any more. ATraceCharacter::SetCarrying() is the
		// sole writer of that mirror, and the Core's release path (ATraceCore::ReleaseHolder, which a
		// death always reaches) clears it — including through its cached PlayerState handle when the
		// pawn is already gone. A fourth writer in the death path is how the mirror ended up with
		// four of them and no owner.
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

	// =============================================================================================
	// SPEC v14 §6 — THE ABILITY LAYER'S KILL AND DEATH HOOKS. This function is the whole funnel.
	// =============================================================================================
	//
	// Rocco's passive ("3% speed boost from headshot kills") and Chut's Chud refresh ("the timer
	// refreshes on a knife kill") both hang off NotifyKill, and until this call existed neither had
	// any way to fire — Rocco's stack could never leave zero however correct its arithmetic was.
	//
	// HEADSHOT IS DERIVED FROM THE CAUSE, not from a new parameter. UTraceWeaponComponent already
	// distinguishes the two ("Headshot" vs "Bullet") when it names the damage, and that FName is
	// carried all the way here — so the kill feed, Rocco's stack and the death panel all read one
	// value rather than three that can disagree. UTraceAbilitySetChut applies the same reading on
	// the incoming-damage side (it matches "Knife" / "Backstab" by name), which is why the two
	// halves of a character's rules cannot drift.
	//
	// The SAME EXCLUSIONS the score uses: a suicide and a team kill are not kills, so they must not
	// feed a passive either. bSelfKill and bTeamKill are computed above and reused rather than
	// recomputed, so a change to what counts as a kill cannot move the scoreboard and leave the
	// abilities behind.
	if (TraceAbilityIntegration::IsEnabled())
	{
		static const FName HeadshotCause(TEXT("Headshot"));
		const bool bHeadshotKill = (Cause == HeadshotCause);

		if (!bSelfKill && !bTeamKill && KillerState != nullptr)
		{
			if (UTraceAbilityComponent* KillerAbilities = UTraceAbilityComponent::Get(KillerState))
			{
				++TraceAbilityIntegration::Counters().Kills;
				KillerAbilities->NotifyKill(Victim, Cause, bHeadshotKill);
			}
		}

		// The victim's own abilities are told unconditionally — a suicide is still a death, and a
		// character tearing down its world actors on death must not be skipped because nobody got
		// the credit. Oyster's jars and Mace's spike clean up through here.
		if (VictimState != nullptr)
		{
			if (UTraceAbilityComponent* VictimAbilities = UTraceAbilityComponent::Get(VictimState))
			{
				++TraceAbilityIntegration::Counters().PawnDied;
				VictimAbilities->NotifyPawnDied();
			}
		}
	}

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
		const float EffectiveRespawnDelay = FMath::Max(TraceGameModeConstants::MinRespawnDelay, RespawnDelay);
		const TWeakObjectPtr<AController> WeakController(VictimController);

		// Publish the deadline, not the duration: the death panel counts down against the same
		// replicated server clock the match timer uses, so it can never disagree with the timer that
		// actually fires. Guarded on the GameState because a death can outrace seamless travel.
		if (VictimState != nullptr)
		{
			if (const ATraceGameState* ClockState = GetTraceGameState())
			{
				VictimState->RespawnEndServerTime =
					static_cast<float>(ClockState->GetServerWorldTimeSeconds()) + EffectiveRespawnDelay;
			}
		}

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
			EffectiveRespawnDelay,
			false);
	}

	// 6. The wipe bonus (spec §1). Evaluated LAST, after the respawn above is on the clock, so the
	//    "are they all down?" count is taken at the true instant of the fifth death — with four
	//    teammates already waiting on timers and this one just added to them.
	if (VictimState != nullptr)
	{
		EvaluateWipeBonus(VictimState->Team);
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

	// The player is breathing again, so retire their respawn countdown and re-arm the wipe bonus
	// against their team. Both are done here rather than in RespawnController() because every path
	// that hands out a fresh pawn — a timed respawn, a kickoff reset, the half-time reset — funnels
	// through this function, and a latch that only cleared on the timed path would let one team be
	// wiped "twice" without ever standing up in between.
	if (ATracePlayerState* RestartedState = Controller->GetPlayerState<ATracePlayerState>())
	{
		RestartedState->RespawnEndServerTime = 0.f;

		const ATraceCharacter* FreshCharacter = Cast<ATraceCharacter>(Controller->GetPawn());
		if (FreshCharacter != nullptr && FreshCharacter->IsAlive())
		{
			ClearWipeLatchIfAlive(RestartedState->Team);
		}

		// SPEC v14 §5/§6. Every path that hands out a fresh pawn funnels through this function, which
		// is exactly why the hook belongs here and not in RespawnController().
		//
		// IT MUST NOT TOUCH THE COOLDOWN, and it does not: NotifyPawnSpawned() reaches only the
		// character's OnPawnSpawned() and the framework's cooldown lives on the PlayerState, which
		// survived the Destroy() twenty lines up. That is the whole reason "a player can spawn with
		// an ability timer still counting down" is true — see the cooldown contract in
		// Abilities/TraceAbilityComponent.h — and it is what Trace.Ability.CooldownPersistenceTest
		// measures across a real pawn identity change.
		if (TraceAbilityIntegration::IsEnabled())
		{
			if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(RestartedState))
			{
				++TraceAbilityIntegration::Counters().PawnSpawned;
				Abilities->NotifyPawnSpawned();
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Wipe bonus (spec §1)
//
// "Simultaneously" is interpreted as: the moment the fifth living enemy dies while the other four
// are still on respawn timers. That is a state test, not an event window — at the instant the last
// one falls, nobody on that team possesses a living pawn — which is both simpler and exactly right,
// because a team can only be "all dead at once" by being all dead at once.
// ---------------------------------------------------------------------------------------------

int32 ATraceGameMode::CountLivingOnTeam(ETraceTeam Team) const
{
	if (Team == ETraceTeam::None)
	{
		return 0;
	}

	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0;
	}

	// Walked over controllers rather than TrackedCharacters because a player waiting on a respawn
	// has NO pawn at all (RestartPlayerFresh destroys the corpse), and a roster of characters cannot
	// tell "dead" apart from "never existed".
	int32 Living = 0;
	for (FConstControllerIterator It = World->GetControllerIterator(); It; ++It)
	{
		const AController* Controller = It->Get();
		if (!IsValid(Controller))
		{
			continue;
		}

		const ATracePlayerState* State = Controller->GetPlayerState<ATracePlayerState>();
		if (State == nullptr || State->IsOnlyASpectator() || State->Team != Team)
		{
			continue;
		}

		const ATraceCharacter* InCharacter = Cast<ATraceCharacter>(Controller->GetPawn());
		if (InCharacter != nullptr && InCharacter->IsAlive())
		{
			++Living;
		}
	}

	return Living;
}

bool ATraceGameMode::IsWipeLatched(ETraceTeam Team) const
{
	switch (Team)
	{
	case ETraceTeam::Blue:   return bBlueWipeLatched;
	case ETraceTeam::Orange: return bOrangeWipeLatched;
	default:                 return true;   // No team can never be wiped; treat as already handled.
	}
}

void ATraceGameMode::SetWipeLatched(ETraceTeam Team, bool bLatched)
{
	switch (Team)
	{
	case ETraceTeam::Blue:   bBlueWipeLatched = bLatched; break;
	case ETraceTeam::Orange: bOrangeWipeLatched = bLatched; break;
	default: break;
	}
}

void ATraceGameMode::ClearWipeLatchIfAlive(ETraceTeam Team)
{
	if (Team == ETraceTeam::None || !IsWipeLatched(Team))
	{
		return;
	}

	if (CountLivingOnTeam(Team) > 0)
	{
		SetWipeLatched(Team, false);
		UE_LOG(LogTraceGame, Verbose, TEXT("Wipe latch on %s cleared; the bonus is armed again."),
			*TraceTeamName(Team).ToString());
	}
}

void ATraceGameMode::EvaluateWipeBonus(ETraceTeam DeadTeam)
{
	if (!HasAuthority() || DeadTeam == ETraceTeam::None || WipeBonusPoints == 0)
	{
		return;
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		return;
	}

	// Only live play pays. A warm-up scrum or a post-whistle tidy-up must not move the scoreboard,
	// and the interval has no play in it at all.
	if (TraceGameState->TraceMatchState != ETraceMatchState::InProgress || TraceGameState->IsHalfTimeBreak())
	{
		return;
	}

	if (IsWipeLatched(DeadTeam))
	{
		return;
	}

	// A team that has no members cannot be "wiped" — otherwise an empty side would pay a bonus every
	// time anybody at all died.
	if (TraceGameState->CountTeamMembers(DeadTeam) <= 0 || CountLivingOnTeam(DeadTeam) > 0)
	{
		return;
	}

	const ETraceTeam BonusTeam = TraceOpposingTeam(DeadTeam);
	SetWipeLatched(DeadTeam, true);

	TraceGameState->AddScore(BonusTeam, WipeBonusPoints);
	TraceGameState->NotifyWipeBonus(BonusTeam);

	UE_LOG(LogTraceGame, Display, TEXT("TEAM WIPE: %s is down to zero; %s +%d. Blue %d - Orange %d"),
		*TraceTeamName(DeadTeam).ToString(), *TraceTeamName(BonusTeam).ToString(), WipeBonusPoints,
		TraceGameState->BlueScore, TraceGameState->OrangeScore);

	// A wipe moves the scoreboard by WipeBonusPoints at once, so it is the single most likely way to
	// CROSS the mercy threshold rather than land on it — which is exactly why the rule is written as
	// "lead >= N" rather than "lead == N", and why this check is here at all.
	CheckMercyRule(TEXT("wipe bonus"));
}

// ---------------------------------------------------------------------------------------------
// Win conditions (spec v4 §6)
// ---------------------------------------------------------------------------------------------

bool ATraceGameMode::CheckMercyRule(const TCHAR* Cause)
{
	if (!HasAuthority())
	{
		return false;
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		return false;
	}

	// Read at the point of use, so dragging the slider in the Project Settings panel retunes the
	// rule with PIE running. Zero (or anything negative from a bad .ini) disables it outright and
	// leaves the clock as the only way a match can end.
	const int32 RequiredLead = UTraceSettings::Get().MercyRuleLead;
	if (RequiredLead <= 0)
	{
		return false;
	}

	// Only live play can trigger it. Warm-up scrums do not score, the interval has no play in it,
	// and a match already in PostMatch cannot end a second time — but each of those is reachable
	// through some path (a debug grant, a live settings edit), and a mercy "win" awarded on the
	// results screen would restart a match that has finished. That is the loop the contract forbids.
	if (TraceGameState->TraceMatchState != ETraceMatchState::InProgress || TraceGameState->IsHalfTimeBreak())
	{
		return false;
	}

	const int32 Lead = TraceGameState->BlueScore - TraceGameState->OrangeScore;
	if (FMath::Abs(Lead) < RequiredLead)
	{
		return false;
	}

	const ETraceTeam WinningTeam = (Lead > 0) ? ETraceTeam::Blue : ETraceTeam::Orange;

	UE_LOG(LogTraceGame, Display,
		TEXT("MERCY RULE: %s leads by %d (threshold %d) after a %s in the %s. Match ends immediately. Blue %d - Orange %d"),
		*TraceTeamName(WinningTeam).ToString(), FMath::Abs(Lead), RequiredLead,
		(Cause != nullptr) ? Cause : TEXT("score"), *TraceGameState->GetHalfLabel(),
		TraceGameState->BlueScore, TraceGameState->OrangeScore);

	// Same courtesy the old score cap paid: put everybody back on their pads before the results
	// screen takes the frame, so it is not drawn over ten pawns piled in one endzone. FinishMatch
	// stops every clock this class owns, including the half-time interval, so a mercy win taken in
	// the first half cannot have the second half start underneath the results screen.
	ResetPlayersToSpawns();
	FinishMatch(WinningTeam, ETraceMatchEndReason::Mercy);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Scoring mode (spec v4 §7)
// ---------------------------------------------------------------------------------------------

ETraceScoringMode ATraceGameMode::GetScoringMode() const
{
	const ATraceGameState* TraceGameState = GetGameState<ATraceGameState>();
	return (TraceGameState != nullptr) ? TraceGameState->GetScoringMode() : ETraceScoringMode::EndzoneStatusCore;
}

void ATraceGameMode::ResolveScoringMode(const FString& Options)
{
	// The default is whatever the settings page already says, so the two overrides below are genuine
	// overrides rather than a reset to the shipped mode.
	UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>();
	if (MutableSettings == nullptr)
	{
		return;
	}

	ETraceScoringMode ResolvedMode = MutableSettings->ScoringMode;
	const TCHAR* Source = TEXT("Project Settings");

	if (UGameplayStatics::HasOption(Options, TraceScoringModeUrlOption))
	{
		ResolvedMode = TraceScoringModeFromUrlValue(
			UGameplayStatics::ParseOption(Options, TraceScoringModeUrlOption));
		Source = TEXT("travel URL");
	}
	else
	{
		FString CommandLineValue;
		if (FParse::Value(FCommandLine::Get(), TEXT("TraceScoringMode="), CommandLineValue))
		{
			ResolvedMode = TraceScoringModeFromUrlValue(CommandLineValue);
			Source = TEXT("command line");
		}
	}

	// Written BACK into the CDO, which is the whole trick that makes this both authoritative and
	// live. After this line the Project Settings panel shows the mode actually being played, the
	// poll below has nothing to fight, and dragging the toggle there is a real change rather than a
	// value silently losing to a URL the player cannot see.
	MutableSettings->ScoringMode = ResolvedMode;

	UE_LOG(LogTraceGame, Display, TEXT("[Mode] %s selected (from the %s)."),
		*TraceScoringModeLabel(ResolvedMode), Source);
}

void ATraceGameMode::PublishScoringMode()
{
	if (!HasAuthority())
	{
		return;
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		return;
	}

	const ETraceScoringMode DesiredMode = UTraceSettings::Get().ScoringMode;

	// SetScoringMode early-outs when nothing changed, so this is free on the second and third call
	// and no OnRep storms out of the belt-and-braces re-runs in BeginPlay.
	TraceGameState->SetScoringMode(DesiredMode);

	// THE CORE HOLDS NO MODE OF ITS OWN. It used to, and the latch was deleted: a second copy of the
	// fact that decides what that actor IS meant the two could disagree for a frame, and the Core's
	// own log caught exactly that — mode A latched, then flipped to B a frame later. ATraceCore::
	// IsModeB() now asks ATraceGameState every time, so the GameState line above is the whole of the
	// publish.
	//
	// This call is therefore a RECONCILE-NOW hook, not a setter: it tells the Core to pick up the
	// new mode on the frame the mode is published rather than on its next tick, and it logs a
	// warning if what it is handed disagrees with the GameState instead of obeying it.
	if (ATraceCore* TheCore = GetCore())
	{
		TheCore->SetScoringMode(DesiredMode);
	}
}

void ATraceGameMode::PollScoringModeSetting()
{
	const ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		return;
	}

	const ETraceScoringMode DesiredMode = UTraceSettings::Get().ScoringMode;
	if (DesiredMode == TraceGameState->GetScoringMode())
	{
		return;
	}

	UE_LOG(LogTraceGame, Display,
		TEXT("[Mode] Live change: %s -> %s. Subscribers to OnScoringModeChanged rebuild from here."),
		*TraceScoringModeLabel(TraceGameState->GetScoringMode()), *TraceScoringModeLabel(DesiredMode));

	PublishScoringMode();
}

// ---------------------------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------------------------

void ATraceGameMode::NotifyScored(ETraceTeam ScoringTeam)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || World == nullptr || ScoringTeam == ETraceTeam::None)
	{
		return;
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		return;
	}

	// One capture, one point. Two independent detectors now feed this — the endzone's own trigger
	// and poll, and CheckEndzoneScoreForCarrier on a possession change — and a pass completed
	// inside the endzone trips both within a tenth of a second of each other. Everything below
	// (kickoff, teleporting ten pawns) is a field reset that takes several frames to settle, so
	// re-entering it before it has is never right, whichever detector got here first.
	const float NowWorld = static_cast<float>(World->GetTimeSeconds());
	if ((NowWorld - LastScoreProcessedWorldTime) < TraceGameModeConstants::ScoreDebounceSeconds)
	{
		UE_LOG(LogTraceGame, Verbose,
			TEXT("Score by %s ignored: %.2fs since the last one (debounce %.2fs)."),
			*TraceTeamName(ScoringTeam).ToString(), NowWorld - LastScoreProcessedWorldTime,
			TraceGameModeConstants::ScoreDebounceSeconds);
		return;
	}
	LastScoreProcessedWorldTime = NowWorld;

	// Warm-up, the interval and post-match carries still reset the field, but they never count.
	const bool bCounts = (TraceGameState->TraceMatchState == ETraceMatchState::InProgress)
		&& !TraceGameState->IsHalfTimeBreak();

	if (bCounts)
	{
		TraceGameState->AddScore(ScoringTeam, 1);
		UE_LOG(LogTraceGame, Log, TEXT("%s scored (%s). Blue %d - Orange %d"),
			*TraceTeamName(ScoringTeam).ToString(), *TraceGameState->GetHalfLabel(),
			TraceGameState->BlueScore, TraceGameState->OrangeScore);
	}

	// THE MERCY RULE IS DECIDED BEFORE ANYTHING IS RESTARTED (spec v4 §6). There is no point kicking
	// off into a match that has just ended, and CheckMercyRule does its own field reset before the
	// results screen takes the frame.
	//
	// This is the only remaining early exit from a match. The score cap that used to sit here is
	// deleted: the clock decides the match now.
	if (bCounts && CheckMercyRule(TEXT("capture")))
	{
		return;
	}

	// SPEC v9 §11: A GOAL IS THE DEAD BALL THE NOTE NAMES FIRST.
	//
	// After the score (the goal counts — the clock expired, the play did not) and after the mercy
	// check (a mercy win is not a half time), but BEFORE the kickoff: there is no point restarting
	// play into a whistle, and BeginHalfTimeBreak/FinishMatch do their own ReleaseCore and
	// ResetPlayersToSpawns, which is exactly the reset the kickoff branch below would have done.
	if (bCounts && TraceGameState->bPendingPeriodEnd)
	{
		ResolvePendingPeriodEnd(TEXT("goal"));
		return;
	}

	// Kickoff, then the reset. Both orders release the outgoing holder (and with them the trace);
	// this one also gets the grant delay lined up with the teleport — see GrantCoreToTeam.
	//
	// The Core only goes back into play while there IS play: after the whistle, and during the
	// interval, it stays out so nothing can be carried into a results screen or a side switch.
	if (TraceGameState->TraceMatchState == ETraceMatchState::PostMatch || TraceGameState->IsHalfTimeBreak())
	{
		ReleaseCore();
	}
	else
	{
		switch (KickoffMode)
		{
		case ECoreKickoffMode::ScoredOnTeam:
			// American-football logic, and the spec's stated assumption: the team that conceded
			// restarts with the Core in their own endzone.
			GrantCoreToTeam(TraceOpposingTeam(ScoringTeam));
			break;

		case ECoreKickoffMode::AlternateTeams:
			GrantCoreToTeam((LastKickoffTeam == ETraceTeam::Blue) ? ETraceTeam::Orange : ETraceTeam::Blue);
			break;

		case ECoreKickoffMode::Neutral:
		default:
			// Nobody is granted it. With a status Core that means "out of play until the next
			// event" rather than "loose on the ground", which no longer exists.
			ReleaseCore();
			break;
		}
	}

	ResetPlayersToSpawns();
}

bool ATraceGameMode::CheckEndzoneScoreForCarrier(ATraceCharacter* InCharacter, const TCHAR* Reason)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || World == nullptr || !IsValid(InCharacter) || !InCharacter->IsAlive())
	{
		return false;
	}

	// The carrier is asked of the Core, not of the pawn's replicated flag: this runs on the frame
	// possession changes, and the flag is a mirror that may not have been written yet.
	const ATraceCore* TheCore = GetCore();
	if (TheCore == nullptr || TheCore->GetCarrier() != InCharacter)
	{
		return false;
	}

	const ETraceTeam CarrierTeam = InCharacter->GetTeam();
	if (CarrierTeam == ETraceTeam::None)
	{
		return false;
	}

	const FVector CarrierLocation = InCharacter->GetActorLocation();

	for (TActorIterator<ATraceEndzone> It(World); It; ++It)
	{
		ATraceEndzone* Zone = *It;
		if (!IsValid(Zone) || Zone->Trigger == nullptr)
		{
			continue;
		}

		// DISARMED ZONES DO NOT SCORE. The arena builds both shapes — full-width endzones for mode A
		// and narrow goals for mode B — and arms one pair, so two of the four ATraceEndzone actors
		// in the world are always inactive. Without this test a mode-B match could award a point off
		// the disarmed FULL-WIDTH endzone, i.e. score by walking over the goal line anywhere along a
		// 9600 uu sideline, which is precisely the thing mode B exists to stop.
		if (!Zone->IsZoneActive())
		{
			continue;
		}

		// ATraceEndzone::ScoresHere() is the single authority on scoring direction (OwningTeam
		// DEFENDS the zone; its opponent scores in it), and it is re-pointed at half time by
		// ApplyTeamSides. Asking it means this can never be the copy that goes stale.
		if (!Zone->ScoresHere(CarrierTeam))
		{
			continue;
		}

		// Point-in-box in the trigger's own space, against the UNSCALED extent (InverseTransform
		// has already undone the scale). Identical to the test the zone itself polls with, and it
		// takes the zone's live dimensions — widen the endzones and this widens with them.
		const FVector Local = Zone->Trigger->GetComponentTransform().InverseTransformPosition(CarrierLocation);
		const FVector Extent = Zone->Trigger->GetUnscaledBoxExtent();

		if (FMath::Abs(Local.X) > Extent.X || FMath::Abs(Local.Y) > Extent.Y || FMath::Abs(Local.Z) > Extent.Z)
		{
			continue;
		}

		UE_LOG(LogTraceGame, Log,
			TEXT("[ENDZONE] %s (%s) took the Core inside the %s endzone (%s) - %s scores without moving."),
			*GetNameSafe(InCharacter), *TraceTeamName(CarrierTeam).ToString(),
			*TraceTeamName(Zone->OwningTeam).ToString(),
			Reason != nullptr ? Reason : TEXT("possession change"),
			*TraceTeamName(CarrierTeam).ToString());

		NotifyScored(CarrierTeam);
		return true;
	}

	return false;
}

void ATraceGameMode::ResetPlayersToSpawns()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	// Controllers, not player controllers: bots are AAIControllers and are never in the player
	// controller list. Iterating only humans here would leave nine pawns standing where they were
	// after every capture reset and at the start of the match.
	//
	// The list is a snapshot of AController pointers; RestartPlayerFresh below destroys and respawns
	// *pawns*, never controllers, so it cannot invalidate the iteration.
	TArray<AController*> Controllers;
	Controllers.Reserve(16);
	for (FConstControllerIterator It = World->GetControllerIterator(); It; ++It)
	{
		if (AController* Controller = It->Get())
		{
			Controllers.Add(Controller);
		}
	}

	for (AController* Controller : Controllers)
	{
		if (!IsValid(Controller))
		{
			continue;
		}

		// Null for a bot. Only the human half of the reset (the rotation RPC) needs it.
		APlayerController* PlayerController = Cast<APlayerController>(Controller);

		ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(Controller->GetPawn());

		if (TraceCharacter == nullptr || !TraceCharacter->IsAlive())
		{
			// Dead or pawnless: hand them a brand new pawn at a freshly chosen start, and drop the
			// pending respawn timer that would otherwise fire into an already-live player.
			ClearPendingRespawn(Controller);
			RestartPlayerFresh(Controller);
			TraceCharacter = Cast<ATraceCharacter>(Controller->GetPawn());
		}
		else if (AActor* StartSpot = FindPlayerStart(Controller))
		{
			// AGameModeBase::RestartPlayerAtPlayerStart only re-possesses a pawn that already
			// exists — it never moves it — so a living player has to be teleported by hand.
			const FVector StartLocation = StartSpot->GetActorLocation();
			FRotator StartRotation = StartSpot->GetActorRotation();
			StartRotation.Pitch = 0.f;
			StartRotation.Roll = 0.f;

			TraceCharacter->TeleportTo(StartLocation, StartRotation);
			Controller->SetControlRotation(StartRotation);

			// Control rotation is client-authoritative, so the client has to be told to turn. Bots
			// have no client and their controller writes its own aim every tick anyway.
			if (PlayerController != nullptr)
			{
				PlayerController->ClientSetRotation(StartRotation, /*bResetCamera=*/true);
			}
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
// Bots
//
// The whole singleplayer mode is this section plus ATraceBotController. Bots are ordinary
// AControllers with ordinary ATracePlayerStates possessing the ordinary DefaultPawnClass, so they
// need no special cases anywhere else: ChoosePlayerStart already keys off the PlayerState team,
// NotifyCharacterDied already schedules a respawn per AController (not per APlayerController), and
// the HUD scoreboard already walks GameState->PlayerArray. That is deliberate — the moment a bot
// needs its own code path in a rules file, the rules stop being verifiable against the bots.
// ---------------------------------------------------------------------------------------------

bool ATraceGameMode::AreBotsEnabled() const
{
	if (BotCountFromURL == 0)
	{
		return false;
	}
	if (BotCountFromURL > 0)
	{
		return true;
	}
	return UTraceSettings::Get().bFillTeamsWithBots;
}

void ATraceGameMode::ScheduleBotFill()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(this, &ATraceGameMode::UpdateBotFill);
	}
}

int32 ATraceGameMode::CountBotsOnTeam(ETraceTeam Team) const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<ATraceBotController>& WeakBot : Bots)
	{
		const ATraceBotController* Bot = WeakBot.Get();
		if (Bot == nullptr)
		{
			continue;
		}
		if (const ATracePlayerState* BotState = Bot->GetPlayerState<ATracePlayerState>())
		{
			if (BotState->Team == Team)
			{
				++Count;
			}
		}
	}
	return Count;
}

void ATraceGameMode::CompactBots()
{
	Bots.RemoveAll([](const TWeakObjectPtr<ATraceBotController>& WeakBot)
	{
		return !WeakBot.IsValid();
	});
}

void ATraceGameMode::UpdateBotFill()
{
	if (!HasAuthority() || GetWorld() == nullptr)
	{
		return;
	}

	CompactBots();

	const ATraceGameState* TraceGameState = GetGameState<ATraceGameState>();
	if (TraceGameState == nullptr)
	{
		return;
	}

	if (!AreBotsEnabled())
	{
		// Config or "?bots=0" turned the fill off after bots already existed (a live config change,
		// or a travel). Clear them out rather than leaving a half-populated match.
		for (const ETraceTeam Team : { ETraceTeam::Blue, ETraceTeam::Orange })
		{
			while (CountBotsOnTeam(Team) > 0)
			{
				RemoveOneBotFromTeam(Team);
			}
		}
		return;
	}

	const UTraceSettings& Settings = UTraceSettings::Get();
	const int32 TeamCap = FMath::Max(1, Settings.PlayersPerTeam);

	// A hard cap on total bots, from whichever of the three sources is tightest. The URL value is a
	// total, the setting is a per-team target, and MaxBots is the safety net.
	int32 TotalBotBudget = FMath::Max(0, Settings.MaxBots);
	if (BotCountFromURL > 0)
	{
		TotalBotBudget = FMath::Min(TotalBotBudget, BotCountFromURL);
	}

	int32 SpawnedThisPass = 0;
	int32 RemovedThisPass = 0;

	for (const ETraceTeam Team : { ETraceTeam::Blue, ETraceTeam::Orange })
	{
		// BotsPerTeam < 0 means "fill to PlayersPerTeam", which is the self-correcting default: it
		// is expressed as a *target roster size*, so humans joining and leaving are absorbed for
		// free. A positive value is a literal bot count on top of whoever is there.
		const int32 CurrentTotal = TraceGameState->CountTeamMembers(Team);
		const int32 CurrentBots = CountBotsOnTeam(Team);
		const int32 Humans = FMath::Max(0, CurrentTotal - CurrentBots);

		const int32 DesiredBots = (Settings.BotsPerTeam < 0)
			? FMath::Max(0, TeamCap - Humans)
			: FMath::Clamp(Settings.BotsPerTeam, 0, FMath::Max(0, TeamCap - Humans));

		for (int32 Removal = CurrentBots; Removal > DesiredBots; --Removal)
		{
			RemoveOneBotFromTeam(Team);
			++RemovedThisPass;
		}

		for (int32 Addition = CurrentBots; Addition < DesiredBots; ++Addition)
		{
			if (Bots.Num() >= TotalBotBudget)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("Bot fill stopped at the MaxBots budget (%d); %s is short of PlayersPerTeam."),
					TotalBotBudget, *TraceTeamName(Team).ToString());
				break;
			}

			if (SpawnBotForTeam(Team) == nullptr)
			{
				break;
			}
			++SpawnedThisPass;
		}
	}

	if (SpawnedThisPass > 0 || RemovedThisPass > 0)
	{
		UE_LOG(LogTraceGame, Log,
			TEXT("Bot fill: +%d / -%d. Roster now Blue %d (%d bots) vs Orange %d (%d bots), %d players total."),
			SpawnedThisPass, RemovedThisPass,
			TraceGameState->CountTeamMembers(ETraceTeam::Blue), CountBotsOnTeam(ETraceTeam::Blue),
			TraceGameState->CountTeamMembers(ETraceTeam::Orange), CountBotsOnTeam(ETraceTeam::Orange),
			GetActivePlayerCount());

		// Bots carry PlayerStates and therefore count towards MinPlayersToStart.
		CheckMatchStartConditions();
	}
}

ATraceBotController* ATraceGameMode::SpawnBotForTeam(ETraceTeam Team)
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority() || Team == ETraceTeam::None)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;   // Runtime scaffolding; never saved into a level.

	UClass* ClassToSpawn = (BotControllerClass != nullptr) ? BotControllerClass.Get() : ATraceBotController::StaticClass();
	ATraceBotController* Bot = World->SpawnActor<ATraceBotController>(ClassToSpawn, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (Bot == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("Failed to spawn a %s bot controller."), *TraceTeamName(Team).ToString());
		return nullptr;
	}

	// ATraceBotController sets bWantsPlayerState, so AController::PostInitializeComponents has
	// already created and registered the PlayerState by the time SpawnActor returns. If it has not,
	// the bot is useless (no team, invisible to the scoreboard and to the team balancer), so bail
	// loudly rather than leaving a mute actor wandering the field.
	ATracePlayerState* BotState = Bot->GetPlayerState<ATracePlayerState>();
	if (BotState == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("Bot controller spawned without an ATracePlayerState; destroying it."));
		Bot->Destroy();
		return nullptr;
	}

	BotState->SetTeam(Team);

	// Spec v14 §3, verbatim: "Bots remain characterless, for now." bIsABot is the engine's own
	// replicated "this is not a person" flag and, until now, nothing in this project set it — the
	// PlayerState was indistinguishable from a human's on every machine. The character system asks
	// that question in three places (never offer a select screen, never consume a team's slot, never
	// count towards per-team uniqueness), so the answer has to be replicated rather than derived from
	// the controller, which does not exist on a client at all.
	BotState->SetIsABot(true);

	Bot->SetBotDisplayName(FString::Printf(TEXT("BOT %s %d"), *TraceTeamName(Team).ToString(), NextBotNumber++));

	Bots.Add(Bot);

	// Same entry point humans go through, so the bot lands on a real team pad chosen by the same
	// "furthest from a live enemy" rule.
	RestartPlayer(Bot);

	if (ATraceCharacter* BotCharacter = Cast<ATraceCharacter>(Bot->GetPawn()))
	{
		BotCharacter->ApplyTeamColors();

		// A fresh body on a wiped team re-arms that team's wipe bonus, exactly as a respawn does.
		// RestartPlayer() is called directly here rather than through RestartPlayerFresh(), so the
		// latch clearing has to be repeated — a bot filling in for an evicted one would otherwise
		// leave the latch stuck and cost the opposing team the next wipe they earn.
		ClearWipeLatchIfAlive(Team);
	}
	else
	{
		UE_LOG(LogTraceGame, Warning, TEXT("Bot '%s' did not receive a pawn."), *BotState->GetPlayerName());
	}

	return Bot;
}

void ATraceGameMode::RemoveOneBotFromTeam(ETraceTeam Team)
{
	for (int32 Index = Bots.Num() - 1; Index >= 0; --Index)
	{
		ATraceBotController* Bot = Bots[Index].Get();
		if (Bot == nullptr)
		{
			Bots.RemoveAt(Index);
			continue;
		}

		const ATracePlayerState* BotState = Bot->GetPlayerState<ATracePlayerState>();
		if (BotState == nullptr || BotState->Team != Team)
		{
			continue;
		}

		Bots.RemoveAt(Index);

		// Reuse the human departure path so the Core cannot leave the match in a bot's hands and the
		// trail cannot outlive its emitter. Called MANUALLY and BEFORE the pawn is torn down, because
		// the release path needs a live pawn to hand the Core off from.
		//
		// THE ENGINE WILL CALL Logout(Bot) AGAIN. AController::Destroyed() calls
		// GameMode->Logout(this) for any controller carrying a PlayerState, and a bot controller
		// does. So Bot->Destroy() below re-enters this same function a second time for the same
		// controller. That is harmless today only because every step happens to be idempotent — the
		// pawn is already null, Super::Logout casts to APlayerController and no-ops on a bot, and
		// ClearPendingRespawn tolerates a second call — and because the only visible cost is
		// CheckMatchStartConditions being scheduled twice.
		//
		// LOGOUT MUST THEREFORE STAY IDEMPOTENT. If you add state to it that cannot be applied twice
		// (a counter, a score adjustment, a queue push), this is the call site that will double it,
		// and the symptom will show up as a bot-count drift nowhere near this line.
		Logout(Bot);

		if (APawn* BotPawn = Bot->GetPawn())
		{
			Bot->UnPossess();
			BotPawn->Destroy();
		}

		Bot->Destroy();
		return;
	}
}

void ATraceGameMode::FreeSlotForHuman()
{
	const ATraceGameState* TraceGameState = GetGameState<ATraceGameState>();
	if (TraceGameState == nullptr || Bots.Num() == 0)
	{
		return;
	}

	const int32 TeamCap = FMath::Max(1, UTraceSettings::Get().PlayersPerTeam);
	const int32 NumBlue = TraceGameState->CountTeamMembers(ETraceTeam::Blue);
	const int32 NumOrange = TraceGameState->CountTeamMembers(ETraceTeam::Orange);

	if (NumBlue < TeamCap || NumOrange < TeamCap)
	{
		return;   // There is already room somewhere; PickTeamForNewPlayer will find it.
	}

	// Both full. Take the slot from whichever side still has a bot to give, preferring the larger
	// side so the eviction cannot itself unbalance the teams.
	const ETraceTeam First = (NumOrange >= NumBlue) ? ETraceTeam::Orange : ETraceTeam::Blue;
	const ETraceTeam Second = TraceOpposingTeam(First);

	if (CountBotsOnTeam(First) > 0)
	{
		RemoveOneBotFromTeam(First);
	}
	else if (CountBotsOnTeam(Second) > 0)
	{
		RemoveOneBotFromTeam(Second);
	}
	else
	{
		return;   // Ten humans. Nothing to evict; the newcomer legitimately has no slot.
	}

	UE_LOG(LogTraceGame, Log, TEXT("Evicted a bot to make room for a human."));
}

#if !UE_BUILD_SHIPPING
void ATraceGameMode::LogBotRoster()
{
	const ATraceGameState* TraceGameState = GetGameState<ATraceGameState>();
	if (TraceGameState == nullptr)
	{
		return;
	}

	const ATraceCore* TheCore = GetCore();
	const ATraceCharacter* TheCarrier = (TheCore != nullptr) ? TheCore->GetCarrier() : nullptr;

	UE_LOG(LogTraceGame, Display, TEXT("[BotDebug] %d players | Blue %d Orange %d | core=%s carrier=%s"),
		GetActivePlayerCount(),
		TraceGameState->CountTeamMembers(ETraceTeam::Blue),
		TraceGameState->CountTeamMembers(ETraceTeam::Orange),
		(TheCore != nullptr) ? *TheCore->GetActorLocation().ToCompactString() : TEXT("<none>"),
		(TheCarrier != nullptr) ? *GetNameSafe(TheCarrier->GetPlayerState<ATracePlayerState>()) : TEXT("<loose>"));

	for (const TWeakObjectPtr<ATraceCharacter>& WeakCharacter : TrackedCharacters)
	{
		const ATraceCharacter* InCharacter = WeakCharacter.Get();
		if (InCharacter == nullptr)
		{
			continue;
		}

		const ATracePlayerState* State = InCharacter->GetPlayerState<ATracePlayerState>();
		const ATraceBotController* Bot = Cast<ATraceBotController>(InCharacter->GetController());

		UE_LOG(LogTraceGame, Display, TEXT("[BotDebug]   %-16s %-6s %-14s pos=%s speed=%.0f %s%s"),
			(State != nullptr) ? *State->GetPlayerName() : TEXT("<no state>"),
			(State != nullptr) ? *TraceTeamName(State->Team).ToString() : TEXT("?"),
			(Bot != nullptr) ? ATraceBotController::StateToString(Bot->GetBotState()) : TEXT("HUMAN"),
			*InCharacter->GetActorLocation().ToCompactString(),
			InCharacter->GetVelocity().Size2D(),
			InCharacter->IsAlive() ? TEXT("") : TEXT("[DEAD] "),
			InCharacter->IsCarrier() ? TEXT("[CARRIER]") : TEXT(""));
	}
}

// -------------------------------------------------------------------------------------------
// -TraceTripTest: scripted proof of the two rules this pass fixed.
//
// SCENARIO A, run VerifyTraceDashRuns times. Force a TURNOVER (the Core moves to the other team),
// which leaves the ex-carrier's trace lying on the field exactly as a completed pass or a steal
// does. Then take one of their enemies and sweep them across a segment of that residual trace
// while dashing. The trace is drawn, so by the invariant it must kill. Before this pass it never
// did: the trip test bailed out on !bEmitting and every visible segment of it was inert.
//
// SCENARIO B, once. Put a teammate of the carrier inside the endzone their team scores in, then
// complete a pass to them. They never move, so no trigger volume can ever see them arrive: the
// point has to come from the possession change itself.
//
// The dash is driven by two teleports on consecutive frames rather than by running at the trace,
// because what the trip test evaluates IS the swept segment between two ticks - this reproduces it
// exactly, deterministically, without depending on where a bot happens to be looking.
// -------------------------------------------------------------------------------------------

void ATraceGameMode::RunVerificationStep()
{
	UWorld* World = GetWorld();
	ATraceGameState* TraceGameState = GetTraceGameState();
	ATraceCore* TheCore = GetCore();
	if (World == nullptr || TraceGameState == nullptr || TheCore == nullptr)
	{
		return;
	}

	/**
	 * How many times scenario A runs. More than one, because the claim is "reliably" — and
	 * "-TraceTripRuns=<n>" raises it, because three samples is an anecdote and a hit RATE needs a
	 * denominator. Read once into a static: the value cannot change inside a session, and parsing
	 * the command line on a 60Hz timer would be silly.
	 */
	static const int32 VerifyTraceDashRuns = []()
	{
		int32 ParsedRuns = 3;
		FParse::Value(FCommandLine::Get(), TEXT("TraceTripRuns="), ParsedRuns);
		return FMath::Clamp(ParsedRuns, 1, 200);
	}();

	/** Steps between scenarios: long enough for the kill, the respawn and a fresh trace. */
	static constexpr int32 VerifyStepsBetweenRuns = 90;

	// Idle until there is a real match with a real carrier to take the Core off.
	if (TraceGameState->TraceMatchState != ETraceMatchState::InProgress)
	{
		return;
	}

	++VerifyStep;

	// --- SCENARIO A ---------------------------------------------------------------------------
	if (VerifyIteration < VerifyTraceDashRuns)
	{
		const int32 Phase = VerifyStep % VerifyStepsBetweenRuns;

		if (Phase == 1)
		{
			// Force the turnover. Any living player on the other team will do; the point is only
			// that the Core changes team, which is what leaves a residual trace behind.
			ATraceCharacter* Carrier = TheCore->GetCarrier();
			if (Carrier == nullptr || !Carrier->IsAlive() || Carrier->Trail == nullptr
				|| Carrier->Trail->TrailPoints.Items.Num() < 6)
			{
				--VerifyStep;   // Not ready - hold this phase rather than skipping the scenario.
				return;
			}

			ATraceCharacter* NewHolder = nullptr;
			for (const TWeakObjectPtr<ATraceCharacter>& Weak : TrackedCharacters)
			{
				ATraceCharacter* Candidate = Weak.Get();
				if (Candidate != nullptr && Candidate->IsAlive() && Candidate->GetTeam() != ETraceTeam::None
					&& Candidate->GetTeam() != Carrier->GetTeam())
				{
					NewHolder = Candidate;
					break;
				}
			}
			if (NewHolder == nullptr)
			{
				--VerifyStep;
				return;
			}

			VerifyTraceOwner = Carrier;

			UE_LOG(LogTraceGame, Display,
				TEXT("[TRIPTEST %d] Turnover: Core %s -> %s. %s keeps a residual trace of %d points (%d of them lethal AND drawn)."),
				VerifyIteration + 1, *GetNameSafe(Carrier), *GetNameSafe(NewHolder), *GetNameSafe(Carrier),
				Carrier->Trail->TrailPoints.Items.Num(), Carrier->Trail->ComputeLastLethalIndex() + 1);

			// Debug harness only. Normal play routes Core changes through ReleaseCore /
			// GrantCoreToTeam; this needs a specific receiver, which is a turnover, not a kickoff.
			TheCore->GrantTo(NewHolder, ETraceCoreGrantReason::Debug);
			return;
		}

		if (Phase == 3)
		{
			// Line the tripper up on one side of a segment of the residual trace.
			ATraceCharacter* TraceOwner = VerifyTraceOwner.Get();
			if (TraceOwner == nullptr || !TraceOwner->IsAlive() || TraceOwner->Trail == nullptr)
			{
				return;
			}

			const int32 LastLethal = TraceOwner->Trail->ComputeLastLethalIndex();
			if (LastLethal < 1)
			{
				return;
			}

			const int32 SegmentIndex = LastLethal / 2;
			const FVector SegmentStart = TraceOwner->Trail->TrailPoints.Items[SegmentIndex].Location;
			const FVector SegmentEnd = TraceOwner->Trail->TrailPoints.Items[SegmentIndex + 1].Location;

			FVector Along = SegmentEnd - SegmentStart;
			Along.Z = 0.0;
			if (!Along.Normalize())
			{
				return;
			}
			const FVector Across = FVector::CrossProduct(Along, FVector::UpVector).GetSafeNormal();
			const FVector Midpoint = (SegmentStart + SegmentEnd) * 0.5;

			// NEVER THE CARRIER. This loop used to take the first living enemy, which is the same
			// test phase 1 used to pick the new holder — so the pawn teleported onto the trace was
			// the pawn now carrying the Core. On the v4 field that is fatal to the test: the
			// residual trace of a turnover early in a half lies near a spawn, the endzone is 2400uu
			// deep (X -16800..-14400), and the dash positions land INSIDE it. Teleporting the
			// carrier there scores, which resets the field and clears the very trace the sweep was
			// about to cross — so every run reported "did nothing - FAIL" while the mechanic was
			// working perfectly (Trace.TestParry kills 4/4 unparried on the same build).
			//
			// A non-carrier crossing the same ground is inert, so the sweep resolves against the
			// trace instead of against the scoreboard. Falling back to the carrier keeps the test
			// running on a roster too small to offer anyone else rather than silently stalling.
			ATraceCharacter* Tripper = nullptr;
			ATraceCharacter* CarrierFallback = nullptr;
			const ATraceCharacter* CoreCarrier = TheCore->GetCarrier();
			for (const TWeakObjectPtr<ATraceCharacter>& Weak : TrackedCharacters)
			{
				ATraceCharacter* Candidate = Weak.Get();
				if (Candidate == nullptr || Candidate == TraceOwner || !Candidate->IsAlive()
					|| Candidate->GetTeam() == ETraceTeam::None || Candidate->GetTeam() == TraceOwner->GetTeam())
				{
					continue;
				}

				if (Candidate == CoreCarrier)
				{
					CarrierFallback = Candidate;
					continue;
				}

				Tripper = Candidate;
				break;
			}
			if (Tripper == nullptr)
			{
				Tripper = CarrierFallback;
			}
			if (Tripper == nullptr)
			{
				return;
			}

			const FVector DashStart = Midpoint + Across * 260.0;
			VerifyDashEnd = Midpoint - Across * 240.0;
			VerifyTripper = Tripper;

			Tripper->SetActorLocation(DashStart, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
			Tripper->SetActorRotation((-Across).Rotation());
			if (UTraceCharacterMovementComponent* Movement = Tripper->GetTraceMovement())
			{
				Movement->StartDash();
			}

			// isCarrier is printed because it is the one condition that invalidates the whole run:
			// a carrier teleported onto a trace that happens to lie in an endzone scores instead of
			// dashing, and the FAIL that follows is the harness's, not the game's.
			UE_LOG(LogTraceGame, Display,
				TEXT("[TRIPTEST %d] %s (enemy of %s, isCarrier=%d) starts a dash at %s, aimed across segment %d of the residual trace at %s. TraceOwner emitting=%d."),
				VerifyIteration + 1, *GetNameSafe(Tripper), *GetNameSafe(TraceOwner),
				(Tripper == CoreCarrier) ? 1 : 0, *DashStart.ToCompactString(),
				SegmentIndex, *Midpoint.ToCompactString(), TraceOwner->Trail->IsEmitting() ? 1 : 0);
			return;
		}

		if (Phase == 4)
		{
			// Second half of the sweep, one frame later: this is the segment the trip test sees.
			if (ATraceCharacter* Tripper = VerifyTripper.Get())
			{
				Tripper->SetActorLocation(VerifyDashEnd, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
				UE_LOG(LogTraceGame, Display, TEXT("[TRIPTEST %d] %s swept through to %s (dashing=%d)."),
					VerifyIteration + 1, *GetNameSafe(Tripper), *VerifyDashEnd.ToCompactString(),
					Tripper->IsDashing() ? 1 : 0);
			}
			return;
		}

		if (Phase == 10)
		{
			const ATraceCharacter* TraceOwner = VerifyTraceOwner.Get();
			const bool bKilled = (TraceOwner == nullptr) || !TraceOwner->IsAlive();

			UE_LOG(LogTraceGame, Display, TEXT("[TRIPTEST %d] RESULT: dash through the post-turnover trace %s."),
				VerifyIteration + 1, bKilled ? TEXT("KILLED the trace owner - PASS") : TEXT("did nothing - FAIL"));

			++VerifyIteration;
			VerifyStep = 0;
			return;
		}

		return;
	}

	// --- SCENARIO B ---------------------------------------------------------------------------
	{
		const int32 Phase = VerifyStep;

		if (Phase == 20)
		{
			ATraceCharacter* Carrier = TheCore->GetCarrier();
			if (Carrier == nullptr || !Carrier->IsAlive())
			{
				--VerifyStep;
				return;
			}

			// A teammate to receive the pass, and the endzone their team scores in.
			ATraceCharacter* Receiver = nullptr;
			for (const TWeakObjectPtr<ATraceCharacter>& Weak : TrackedCharacters)
			{
				ATraceCharacter* Candidate = Weak.Get();
				if (Candidate != nullptr && Candidate != Carrier && Candidate->IsAlive()
					&& Candidate->GetTeam() == Carrier->GetTeam())
				{
					Receiver = Candidate;
					break;
				}
			}

			ATraceEndzone* TargetZone = nullptr;
			for (TActorIterator<ATraceEndzone> It(World); It; ++It)
			{
				ATraceEndzone* Zone = *It;
				if (IsValid(Zone) && Zone->Trigger != nullptr && Zone->ScoresHere(Carrier->GetTeam()))
				{
					TargetZone = Zone;
					break;
				}
			}

			if (Receiver == nullptr || TargetZone == nullptr)
			{
				--VerifyStep;
				return;
			}

			// Stand them in the middle of the zone, on the floor plane the pads use.
			const FVector ZoneCentre = TargetZone->Trigger->GetComponentLocation();
			const FVector Standing(ZoneCentre.X, ZoneCentre.Y, Receiver->GetActorLocation().Z);
			Receiver->SetActorLocation(Standing, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);

			VerifyTripper = Receiver;
			VerifyScoreBefore = TraceGameState->GetScore(Carrier->GetTeam());

			UE_LOG(LogTraceGame, Display,
				TEXT("[ENDZONETEST] %s (%s) is standing still inside the %s endzone at %s. %s completes a pass to them. Score before: %d."),
				*GetNameSafe(Receiver), *TraceTeamName(Receiver->GetTeam()).ToString(),
				*TraceTeamName(TargetZone->OwningTeam).ToString(), *Standing.ToCompactString(),
				*GetNameSafe(Carrier), VerifyScoreBefore);
			return;
		}

		if (Phase == 22)
		{
			// The completed-pass path itself: ATraceCore::ServerTickPass ends in exactly this call.
			if (ATraceCharacter* Receiver = VerifyTripper.Get())
			{
				TheCore->GrantTo(Receiver, ETraceCoreGrantReason::Pass);
			}
			return;
		}

		if (Phase == 26)
		{
			const ATraceCharacter* Receiver = VerifyTripper.Get();
			const ETraceTeam Team = (Receiver != nullptr) ? Receiver->GetTeam() : ETraceTeam::None;
			const int32 ScoreAfter = (Team != ETraceTeam::None) ? TraceGameState->GetScore(Team) : -1;

			UE_LOG(LogTraceGame, Display,
				TEXT("[ENDZONETEST] RESULT: score %d -> %d. A pass completed into the enemy endzone %s."),
				VerifyScoreBefore, ScoreAfter,
				(ScoreAfter == VerifyScoreBefore + 1) ? TEXT("SCORED - PASS") : TEXT("did not score - FAIL"));

			GetWorldTimerManager().ClearTimer(VerifyTimerHandle);
			return;
		}
	}
}
#endif

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

	// Warm-up goals do not count, but nothing stops the scores being non-zero if the settings were
	// changed live, so start from a known state. Scores carry ACROSS halves (spec §9 q5) — they are
	// only cleared here, once, at the opening whistle.
	TraceGameState->BlueScore = 0;
	TraceGameState->OrangeScore = 0;
	TraceGameState->OnRep_Scores();

	// A match that is starting has not ended. Cleared for the same reason the scores are: nothing
	// guarantees this GameState is fresh, and a stale "MERCY RULE" on the results screen of the next
	// match would be a lie nobody would think to look for.
	TraceGameState->SetMatchResult(ETraceTeam::None, ETraceMatchEndReason::NotEnded);

	// The mode is latched for the match at the opening whistle in the sense that this is the last
	// unconditional publish; a live edit after this point still takes effect through the poll, which
	// is exactly what A/B playtesting needs.
	PublishScoringMode();

	TraceGameState->TraceMatchState = ETraceMatchState::InProgress;
	TraceGameState->ForceNetUpdate();

	bBlueWipeLatched = false;
	bOrangeWipeLatched = false;
	LastKickoffTeam = ETraceTeam::None;

	BeginHalf(1);
}

// ---------------------------------------------------------------------------------------------
// Halves and sides (spec §1)
// ---------------------------------------------------------------------------------------------

ETraceTeam ATraceGameMode::GetNegativeSideTeamForHalf(int32 HalfIndex) const
{
	// The arena is PAINTED with Blue at -X, so odd halves are played the way the field looks and
	// even halves are the swap. Expressed as parity rather than "if half == 2" so raising
	// HalvesPerMatch keeps alternating instead of silently sticking.
	const bool bSwapped = (FMath::Max(1, HalfIndex) % 2) == 0;
	const ETraceTeam PaintedNegativeSideTeam = ETraceTeam::Blue;

	return bSwapped ? TraceOpposingTeam(PaintedNegativeSideTeam) : PaintedNegativeSideTeam;
}

ETraceTeam ATraceGameMode::GetKickoffTeamForHalf(int32 HalfIndex) const
{
	// "The core starts with Team A in the first half, Team B in the second."
	const ETraceTeam TeamA = (FirstHalfCoreTeam == ETraceTeam::None) ? ETraceTeam::Blue : FirstHalfCoreTeam;
	const bool bTeamB = (FMath::Max(1, HalfIndex) % 2) == 0;

	return bTeamB ? TraceOpposingTeam(TeamA) : TeamA;
}

void ATraceGameMode::ApplyTeamSides(ETraceTeam TeamOnNegativeSide)
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority() || TeamOnNegativeSide == ETraceTeam::None)
	{
		return;
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		return;
	}

	// Publish first. Everything that asks "which way do I attack?" — bots, the HUD, anything added
	// later — reads the GameState, so it must be correct before the actors below start moving.
	TraceGameState->SetTeamSides(TeamOnNegativeSide);

	// The field runs goal to goal along X and GetFieldBounds() is an axis-aligned world box, so an
	// actor's side is the sign of its X relative to the field centre. Deriving it geometrically
	// rather than from the actor's previous Team is what makes this idempotent: calling it twice
	// with the same argument is a no-op, and calling it with the other argument is an exact swap,
	// however many times the sides have already been switched.
	float CentreX = 0.f;
	if (ArenaBuilder != nullptr)
	{
		CentreX = static_cast<float>(ArenaBuilder->GetFieldBounds().GetCenter().X);
	}

	int32 ZonesRepointed = 0;
	for (TActorIterator<ATraceEndzone> It(World); It; ++It)
	{
		ATraceEndzone* Zone = *It;
		if (!IsValid(Zone))
		{
			continue;
		}

		// ATraceEndzone::OwningTeam is the team that DEFENDS the zone; its opponent scores in it.
		Zone->OwningTeam = TraceGameState->GetTeamDefendingEnd(Zone->GetActorLocation().X - CentreX);
		++ZonesRepointed;
	}

	int32 StartsRepointed = 0;
	for (TActorIterator<ATraceTeamPlayerStart> It(World); It; ++It)
	{
		ATraceTeamPlayerStart* Start = *It;
		if (!IsValid(Start))
		{
			continue;
		}

		// A spawn pad belongs to the team that DEFENDS the end it sits at — you spawn at home, both
		// on the builder's pads and on the endzone pads this class adds.
		Start->Team = TraceGameState->GetTeamDefendingEnd(Start->GetActorLocation().X - CentreX);
		++StartsRepointed;
	}

	// Repaint the arena to match. On a listen host this is the server's own view; remote clients get
	// there through ATraceGameState::OnRep_SidesChanged, because the builder is not replicated.
	ATraceArenaBuilder::ApplyTeamSidesInWorld(World, TeamOnNegativeSide);

	UE_LOG(LogTraceGame, Log, TEXT("Sides set: %s defends -X, %s defends +X (%d endzones, %d spawn pads re-pointed)."),
		*TraceTeamName(TeamOnNegativeSide).ToString(),
		*TraceTeamName(TraceOpposingTeam(TeamOnNegativeSide)).ToString(),
		ZonesRepointed, StartsRepointed);
}

bool ATraceGameMode::IsSpawnLocationBlocked(const FVector& Location) const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraceEndzoneSpawnProbe), /*bTraceComplex=*/false);
	Params.bIgnoreTouches = true;

	return World->OverlapBlockingTestByChannel(
		Location, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeCapsule(TraceGameModeConstants::SpawnProbeRadius, TraceGameModeConstants::SpawnProbeHalfHeight),
		Params);
}

void ATraceGameMode::BuildEndzoneSpawnPads()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority() || !bRespawnInOwnEndzone)
	{
		return;
	}

	// Idempotent: called from both PreInitializeComponents and BeginPlay, and a second set of pads
	// would double every team's spawn choices for no benefit.
	EndzoneStarts.RemoveAll([](const TWeakObjectPtr<ATraceTeamPlayerStart>& Weak) { return !Weak.IsValid(); });
	if (EndzoneStarts.Num() > 0)
	{
		return;
	}

	if (ArenaBuilder == nullptr)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("No arena builder; respawns fall back to the generic team pads."));
		return;
	}

	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr)
	{
		return;
	}

	const FBox Bounds = ArenaBuilder->GetFieldBounds();
	const FVector Centre = Bounds.GetCenter();
	const float HalfX = FMath::Max(1.f, static_cast<float>(Bounds.Max.X - Bounds.Min.X) * 0.5f);

	// Ask the builder for the depth rather than re-clamping EndzoneDepth here. This was the third
	// independent copy of that clamp, and when the copies disagreed the pads landed on the dais.
	const float Depth = ArenaBuilder->ClampedEndzoneDepth();

	// Mid-endzone: half a depth in from the end wall, half a depth behind the goal line. The gate
	// towers and the goal line itself stand ON the line, so the middle of the box is the one part of
	// an endzone with nothing built in it.
	const float PadInsetFromCentre = HalfX - Depth * 0.5f;

	// The arena builder's own pads are the source of lateral spread, height and facing. Reusing them
	// means a pad line that has already been checked against every cover block in the arena, and it
	// costs nothing: only the X coordinate moves.
	TArray<ATraceTeamPlayerStart*> Templates;
	for (TActorIterator<ATraceTeamPlayerStart> It(World); It; ++It)
	{
		ATraceTeamPlayerStart* Start = *It;
		if (IsValid(Start) && !Start->ActorHasTag(TraceGameModeConstants::EndzoneStartTag))
		{
			Templates.Add(Start);
		}
	}

	if (Templates.Num() == 0)
	{
		UE_LOG(LogTraceGame, Warning, TEXT("No arena spawn pads to derive endzone respawns from."));
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;   // Runtime scaffolding; never saved into a level.

	int32 Placed = 0;
	int32 Skipped = 0;

	for (const ATraceTeamPlayerStart* Template : Templates)
	{
		const FVector TemplateLocation = Template->GetActorLocation();
		const float Sign = (TemplateLocation.X - Centre.X < 0.f) ? -1.f : 1.f;

		// Walk from mid-endzone back towards the goal line until the capsule fits. In the shipped
		// arena the first candidate is always clear; the walk exists so that changing EndzoneDepth
		// or adding endzone furniture degrades into a slightly shallower pad instead of spawning a
		// pawn inside a wall.
		FVector Candidate(Centre.X + Sign * PadInsetFromCentre, TemplateLocation.Y, TemplateLocation.Z);
		bool bPlaced = false;

		for (int32 Step = 0; Step <= TraceGameModeConstants::SpawnProbeSteps; ++Step)
		{
			Candidate.X = Centre.X + Sign * (PadInsetFromCentre
				- Depth * TraceGameModeConstants::SpawnProbeStepFraction * static_cast<float>(Step));

			if (!IsSpawnLocationBlocked(Candidate))
			{
				bPlaced = true;
				break;
			}
		}

		if (!bPlaced)
		{
			++Skipped;
			continue;
		}

		ATraceTeamPlayerStart* Pad = World->SpawnActor<ATraceTeamPlayerStart>(
			ATraceTeamPlayerStart::StaticClass(), Candidate, Template->GetActorRotation(), SpawnParams);

		if (Pad == nullptr)
		{
			++Skipped;
			continue;
		}

		Pad->Tags.Add(TraceGameModeConstants::EndzoneStartTag);
		Pad->Team = TraceGameState->GetTeamDefendingEnd(Sign);
		EndzoneStarts.Add(Pad);
		++Placed;
	}

	UE_LOG(LogTraceGame, Log, TEXT("Endzone respawn pads: %d placed, %d skipped (blocked)."), Placed, Skipped);
}

void ATraceGameMode::BeginHalf(int32 HalfIndex)
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::InProgress)
	{
		return;
	}

	const int32 Halves = FMath::Max(1, HalvesPerMatch);
	const int32 ClampedHalf = FMath::Clamp(HalfIndex, 1, Halves);
	const float Duration = FMath::Max(1.f, HalfDuration);

	GetWorldTimerManager().ClearTimer(HalfTimeTimerHandle);
	GetWorldTimerManager().ClearTimer(MatchTimerHandle);

	// Spec v9 §11. A new period starts with a full clock and no whistle armed. Belt and braces —
	// every route into here has already resolved one — but a pending flag that survived into a new
	// half would end it on its first turnover, which is the worst possible way to find this bug.
	ClearPendingPeriodEnd();

	// A period of play opens with both wipe bonuses armed. The reset below stands everybody up, so a
	// latch carried in from the previous half would describe a team that is no longer down.
	bBlueWipeLatched = false;
	bOrangeWipeLatched = false;

	TraceGameState->SetHalfState(ClampedHalf, Halves, /*bInHalfTimeBreak=*/false);
	TraceGameState->MatchEndServerTime = static_cast<float>(TraceGameState->GetServerWorldTimeSeconds() + Duration);
	TraceGameState->ForceNetUpdate();

	// Sides are already correct for half 1 (set in PreInitializeComponents) and for half 2 (set the
	// moment the interval began), but assert them here anyway: this is the one function that starts
	// a period of play, and it must not depend on who called it to be correct.
	ApplyTeamSides(GetNegativeSideTeamForHalf(ClampedHalf));

	// Kickoff first, pawns second: ATraceCore::KickoffTo holds the grant back for a moment exactly
	// so the receiver is already standing on their new pad when it lands.
	GrantCoreToTeam(GetKickoffTeamForHalf(ClampedHalf));
	ResetPlayersToSpawns();

	// The authoritative deadline is the replicated MatchEndServerTime; this timer is merely what
	// fires on it server-side. Clients count down against the shared clock instead.
	GetWorldTimerManager().SetTimer(MatchTimerHandle, this, &ATraceGameMode::HandleHalfExpired, Duration, false);

	UE_LOG(LogTraceGame, Log, TEXT("%s started: %.0fs, %s kicks off, %s defends -X. Blue %d - Orange %d"),
		*TraceGameState->GetHalfLabel(), Duration,
		*TraceTeamName(GetKickoffTeamForHalf(ClampedHalf)).ToString(),
		*TraceTeamName(GetNegativeSideTeamForHalf(ClampedHalf)).ToString(),
		TraceGameState->BlueScore, TraceGameState->OrangeScore);
}

void ATraceGameMode::HandleHalfExpired()
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::InProgress)
	{
		return;
	}

	// SPEC v9 §11. THE CLOCK RUNNING OUT NO LONGER ENDS THE PERIOD.
	//
	// Verbatim: "Change halftime to trigger after the time runs out and the current play ends, so
	// that it doesn't cut people off in the middle of a run". So the expiry ARMS the whistle and
	// play carries on; ResolvePendingPeriodEnd() blows it at the next dead ball.
	//
	// Read at the point of use, so the switch retunes with the game running. Off restores the old
	// behaviour exactly — the period ends on the tick the clock does.
	const bool bDefer = UTraceSettings::Get().bDeferPeriodEndToPlayBreak;

	// Already pending means this is the hard cap or a second timer firing; end now rather than
	// arming a whistle on top of the one that is already armed.
	if (bDefer && !TraceGameState->bPendingPeriodEnd && !TraceGameState->IsHalfTimeBreak())
	{
		BeginPendingPeriodEnd();
		return;
	}

	EndPeriodNow(TEXT("clock"));
}

// ---------------------------------------------------------------------------------------------
// Deferred half time / full time (spec v9 §11). See the block comment in TraceGameMode.h for what
// counts as a dead ball and why, including the two [ASSUMPTION]s about passes and carrier deaths.
// ---------------------------------------------------------------------------------------------

void ATraceGameMode::BeginPendingPeriodEnd()
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || !HasAuthority())
	{
		return;
	}

	// The half's own timer has fired; make sure it cannot fire again underneath the pending state.
	GetWorldTimerManager().ClearTimer(MatchTimerHandle);

	// PIN THE CLOCK AT 0:00. GetMatchTimeRemaining() clamps at zero anyway, but the deadline is a
	// replicated float that a client extrapolates against a smoothed server clock: a client whose
	// clock runs a few milliseconds behind would otherwise draw 0:01 for a moment while the server
	// has already armed the whistle, and "the timer went back up" is exactly the confusion §11 is
	// trying to avoid.
	TraceGameState->MatchEndServerTime = static_cast<float>(TraceGameState->GetServerWorldTimeSeconds());

	// THE GUARD RAIL. A stalemate must not run forever: two teams that never turn the Core over and
	// never score would otherwise play on past the whistle indefinitely. Read live; 0 disables it.
	const float MaxDefer = FMath::Max(0.f, UTraceSettings::Get().PeriodEndMaxDeferSeconds);
	const float CapServerTime = (MaxDefer > 0.f)
		? static_cast<float>(TraceGameState->GetServerWorldTimeSeconds() + MaxDefer)
		: 0.f;

	TraceGameState->SetPendingPeriodEnd(true, CapServerTime);

	// Seed the turnover detector with WHO HAS IT NOW, so the possession that was already in progress
	// when the clock expired is not itself mistaken for a turnover on the first poll.
	const ATraceCore* TheCore = GetCore();
	PendingEndLastHolderTeam = (TheCore != nullptr) ? TheCore->GetHolderTeam() : ETraceTeam::None;

	GetWorldTimerManager().SetTimer(PendingPeriodEndPollHandle, this, &ATraceGameMode::PollPendingPeriodEnd,
		TraceGameModeConstants::PendingPeriodEndPollInterval, /*bLoop=*/true);

	if (MaxDefer > 0.f)
	{
		// CreateWeakLambda rather than a payload delegate: the cause is a literal, the capture is
		// only `this`, and the weak binding makes a GameMode torn down mid-defer harmless.
		GetWorldTimerManager().SetTimer(PendingPeriodEndCapHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				ResolvePendingPeriodEnd(TEXT("hard cap - no dead ball arrived"));
			}),
			MaxDefer, /*bLoop=*/false);
	}

	// Display, not Log. This is the line somebody reads when the clock "stopped working", and this
	// project has twice declared a working mechanic dead because its log line was suppressed.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Half] %s clock expired - PLAY CONTINUES. %s (hard cap %s). Core held by %s."),
		*TraceGameState->GetHalfLabel(), *TraceGameState->GetPendingPeriodEndLabel(),
		(MaxDefer > 0.f) ? *FString::Printf(TEXT("%.0fs"), MaxDefer) : TEXT("disabled"),
		*TraceTeamName(PendingEndLastHolderTeam).ToString());
}

void ATraceGameMode::ClearPendingPeriodEnd()
{
	GetWorldTimerManager().ClearTimer(PendingPeriodEndPollHandle);
	GetWorldTimerManager().ClearTimer(PendingPeriodEndCapHandle);
	PendingEndLastHolderTeam = ETraceTeam::None;

	if (ATraceGameState* TraceGameState = GetTraceGameState())
	{
		if (TraceGameState->bPendingPeriodEnd)
		{
			TraceGameState->SetPendingPeriodEnd(false, 0.f);
		}
	}
}

void ATraceGameMode::ResolvePendingPeriodEnd(const TCHAR* Cause)
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || !TraceGameState->bPendingPeriodEnd)
	{
		return;
	}

	UE_LOG(LogTraceGame, Display, TEXT("[Half] Dead ball (%s) - the deferred whistle goes now. Blue %d - Orange %d"),
		(Cause != nullptr) ? Cause : TEXT("unknown"), TraceGameState->BlueScore, TraceGameState->OrangeScore);

	// Cleared FIRST. EndPeriodNow() runs BeginHalfTimeBreak/FinishMatch, both of which reset the
	// field and could otherwise be observed by a poll that is still armed.
	ClearPendingPeriodEnd();

	EndPeriodNow(Cause);
}

void ATraceGameMode::PollPendingPeriodEnd()
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || !HasAuthority())
	{
		return;
	}

	// Anything that took the match out of live play took the whistle with it (a mercy win, a
	// forfeit, an interval started some other way). Stop rather than fire into it.
	if (!TraceGameState->bPendingPeriodEnd
		|| TraceGameState->TraceMatchState != ETraceMatchState::InProgress
		|| TraceGameState->IsHalfTimeBreak())
	{
		ClearPendingPeriodEnd();
		return;
	}

	const ATraceCore* TheCore = GetCore();
	if (TheCore == nullptr)
	{
		return;
	}

#if !UE_BUILD_SHIPPING
	// Counted only once the poll is actually LOOKING AT THE CORE, which is the only state in which
	// it could detect anything. Trace.HalfTime.Verify waits on this rather than on wall-clock: with
	// several headless instances on one machine the engine's frame-rate smoothing clamps the world's
	// delta, ticks arrive seconds apart, and a scripted step timed in seconds can land before the
	// detector has run even once — which produces a FAIL that says nothing about the code, and a
	// "did not fire" PASS that is vacuous. Waiting on polls makes both verdicts mean something.
	++PendingPeriodEndPollCount;
#endif

	// --- A TURNOVER BETWEEN THE TEAMS -----------------------------------------------------------
	//
	// The TEAM, not the holder. A completed pass moves the Core between two players of the same
	// team and play continues (spec §11 [ASSUMPTION]); a kill that hands it to the other side, a
	// mode-B interception, and a mode-B surface turnover all move it between teams and are breaks.
	//
	// None is skipped rather than treated as a turnover: a thrown Core is holderless while it is in
	// the air, and the flight is not a dead ball — the landing is, and it is caught below.
	const ETraceTeam HolderTeam = TheCore->GetHolderTeam();
	if (HolderTeam != ETraceTeam::None)
	{
		if (PendingEndLastHolderTeam != ETraceTeam::None && HolderTeam != PendingEndLastHolderTeam)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[Half] Turnover: %s -> %s."),
				*TraceTeamName(PendingEndLastHolderTeam).ToString(), *TraceTeamName(HolderTeam).ToString());

			PendingEndLastHolderTeam = HolderTeam;
			ResolvePendingPeriodEnd(TEXT("turnover between teams"));
			return;
		}

		PendingEndLastHolderTeam = HolderTeam;
		return;
	}

	// --- MODE B: THE CORE HITS THE GROUND -------------------------------------------------------
	//
	// "the ball drops in game mode b". Held separately from the turnover test above because the two
	// are not the same event: the surface turnover (spec v7 §4) usually grants the Core to the other
	// team on the frame it lands, but a landing with nobody eligible to receive it leaves the Core
	// lying there holderless — which is as dead as a ball gets, and must not leave the whistle
	// waiting. Mode A has no loose Core, so IsLoose() is permanently false there and this costs one
	// branch.
	if (TheCore->IsLoose() && TheCore->GetLooseVelocity().Size() < TraceGameModeConstants::PendingPeriodEndCoreRestSpeed)
	{
		ResolvePendingPeriodEnd(TEXT("mode B: the Core came down"));
	}
}

void ATraceGameMode::EndPeriodNow(const TCHAR* Cause)
{
	const ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::InProgress)
	{
		return;
	}

	// WHO HAD THE CORE WHEN THE WHISTLE WENT. This is the entire subject of spec v9 §11 — "it
	// doesn't cut people off in the middle of a run" — and it is the one fact that separates a
	// correct end from the behaviour the note complains about, so it goes in the log at the moment
	// of the end rather than being reconstructed afterwards from possession lines.
	//
	// With deferral ON the Cause names the dead ball and a carrier here is expected (the turnover's
	// new owner). With deferral OFF the Cause is "clock" and a carrier here IS the symptom.
	const ATraceCore* CoreAtWhistle = GetCore();
	const ETraceTeam TeamAtWhistle = (CoreAtWhistle != nullptr) ? CoreAtWhistle->GetHolderTeam() : ETraceTeam::None;
	UE_LOG(LogTraceGame, Display, TEXT("[Half] Whistle: %s ends on '%s'. Core %s at that instant."),
		*TraceGameState->GetHalfLabel(), (Cause != nullptr) ? Cause : TEXT("clock"),
		(TeamAtWhistle != ETraceTeam::None)
			? *FString::Printf(TEXT("WAS BEING CARRIED by %s"), *TraceTeamName(TeamAtWhistle).ToString())
			: TEXT("was not in anybody's hands"));

	if (TraceGameState->CurrentHalf < FMath::Max(1, HalvesPerMatch))
	{
		UE_LOG(LogTraceGame, Log, TEXT("[Half] Ending %s (%s)."), *TraceGameState->GetHalfLabel(),
			(Cause != nullptr) ? Cause : TEXT("clock"));
		BeginHalfTimeBreak();
		return;
	}

	// The clock is now the ONLY way a match reaches full time, and the highest score at that moment
	// wins. Equal scores are a genuine draw — there is no cap to break the tie against any more.
	ETraceTeam Winner = ETraceTeam::None;
	if (TraceGameState->BlueScore > TraceGameState->OrangeScore)
	{
		Winner = ETraceTeam::Blue;
	}
	else if (TraceGameState->OrangeScore > TraceGameState->BlueScore)
	{
		Winner = ETraceTeam::Orange;
	}

	FinishMatch(Winner, ETraceMatchEndReason::Clock);
}

void ATraceGameMode::BeginHalfTimeBreak()
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::InProgress
		|| TraceGameState->IsHalfTimeBreak())
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(MatchTimerHandle);

	// Spec v9 §11: the whistle has gone, so nothing is pending any more. Also covers the paths that
	// reach the interval without going through ResolvePendingPeriodEnd (deferral switched off, or a
	// direct call from a debug command).
	ClearPendingPeriodEnd();

	const float BreakDuration = FMath::Max(1.f, HalfTimeBreakDuration);
	const int32 NextHalf = TraceGameState->CurrentHalf + 1;

	TraceGameState->SetHalfState(TraceGameState->CurrentHalf, FMath::Max(1, HalvesPerMatch), /*bInHalfTimeBreak=*/true);
	TraceGameState->MatchEndServerTime = static_cast<float>(TraceGameState->GetServerWorldTimeSeconds() + BreakDuration);
	TraceGameState->ForceNetUpdate();

	// The switch happens NOW, at the top of the interval, not at the bottom of it. Spending the
	// break stood in the end you are about to defend, looking at the goal you are about to attack,
	// is the entire reason the interval is on screen — a swap that happened the instant play resumed
	// would be a swap nobody saw.
	ApplyTeamSides(GetNegativeSideTeamForHalf(NextHalf));

	ReleaseCore();
	ResetPlayersToSpawns();

	// Spec v14 §5, verbatim: "They should all reset at halftime." HERE, at the top of the interval,
	// and not at the start of the second half — a player who spends the break watching an ability
	// meter that is still counting down has been told the rule does not work, and the fact that it
	// would have snapped to ready twelve seconds later does not help them.
	//
	// This is also the ONLY reset in the codebase. Death, respawns and goals deliberately do not
	// touch ActivatedCooldownEndServerTime; see the note on that property in TracePlayerState.h.
	ResetAbilityCooldownsForHalfTime();

	// The scores deliberately survive: the second half continues the first (spec §9 q5).
	UE_LOG(LogTraceGame, Display, TEXT("HALF TIME (%.0fs). Sides switch: %s now defends -X. Blue %d - Orange %d"),
		BreakDuration, *TraceTeamName(GetNegativeSideTeamForHalf(NextHalf)).ToString(),
		TraceGameState->BlueScore, TraceGameState->OrangeScore);

	GetWorldTimerManager().SetTimer(HalfTimeTimerHandle, this, &ATraceGameMode::EndHalfTimeBreak, BreakDuration, false);
}

void ATraceGameMode::EndHalfTimeBreak()
{
	const ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || !TraceGameState->IsHalfTimeBreak())
	{
		return;
	}

	BeginHalf(TraceGameState->CurrentHalf + 1);
}

// ---------------------------------------------------------------------------------------------
// Core possession
//
// The only two places this class touches ATraceCore. See the header: the Core is being rewritten
// from a physical actor into a replicated possession status, and confining every call to these two
// functions is what keeps that a local edit.
// ---------------------------------------------------------------------------------------------

void ATraceGameMode::ReleaseCore()
{
	if (!HasAuthority())
	{
		return;
	}

	if (ATraceCore* TheCore = GetCore())
	{
		// "Out of play": no holder, parked at the centre. ETraceTeam::None is the argument that says
		// so, and it is the same call as a kickoff precisely so the Core has one entry point.
		//
		// KickoffTo honours None: it sets bOutOfPlay, grants the Core to nobody and parks it at the
		// centre. (There WAS a period where it rewrote None into DefaultKickoffTeam and handed the
		// Core to Blue a second into the interval; that is fixed at the source, and the Core's
		// out-of-play recovery is now also gated on ATraceGameState::IsHalfTimeBreak() so it cannot
		// force a kickoff mid-break either.)
		TheCore->KickoffTo(ETraceTeam::None);
	}
}

void ATraceGameMode::GrantCoreToTeam(ETraceTeam Team)
{
	if (!HasAuthority() || Team == ETraceTeam::None)
	{
		return;
	}

	ATraceCore* TheCore = GetCore();
	if (TheCore == nullptr)
	{
		return;
	}

	// KickoffTo releases the outgoing holder, parks the Core and queues the grant behind a short
	// delay. The delay is the reason this is called BEFORE the pawns are teleported rather than
	// after: granting first and moving second would lay a trace across the teleport.
	//
	// Which player on the team receives it is ATraceCore's decision (nearest to the Core's home
	// among the living). After a reset every candidate is stood on a pad in their own endzone, so
	// any of them is a legitimate kickoff receiver.
	TheCore->KickoffTo(Team);
	LastKickoffTeam = Team;

	UE_LOG(LogTraceGame, Log, TEXT("Kickoff: the Core goes to %s."), *TraceTeamName(Team).ToString());
}

void ATraceGameMode::FinishMatch(ETraceTeam WinningTeam, ETraceMatchEndReason Reason)
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState == ETraceMatchState::PostMatch)
	{
		return;
	}

	// Published BEFORE the phase flips to PostMatch, so no client can ever draw one frame of the
	// results screen with a stale "NotEnded" reason on it. Both properties are on the same actor and
	// the same net update, but the ordering costs nothing and removes the question.
	TraceGameState->SetMatchResult(WinningTeam, Reason);

	// Every clock this class owns stops here, including the interval — a FinishMatch triggered early
	// (a mercy-rule win, a forfeit) during half time must not have the second half start underneath
	// the results screen. That would be the restart loop the contract forbids, wearing a new hat.
	GetWorldTimerManager().ClearTimer(WarmupTimerHandle);
	GetWorldTimerManager().ClearTimer(MatchTimerHandle);
	GetWorldTimerManager().ClearTimer(HalfTimeTimerHandle);

	// SPEC v9 §11 AND THE MERCY RULE, TOGETHER. The deferred whistle is a play boundary; the mercy
	// rule is not, and must still end the match on the frame the lead reaches the threshold even if
	// that frame is the middle of a run. So the mercy path does NOT consult the pending state — it
	// comes straight here — and this call is what makes that safe: whatever was pending is dropped,
	// its poll and its hard cap are cleared, and nothing is left armed under the results screen.
	ClearPendingPeriodEnd();

	TraceGameState->TraceMatchState = ETraceMatchState::PostMatch;
	TraceGameState->SetHalfState(TraceGameState->CurrentHalf, FMath::Max(1, HalvesPerMatch), /*bInHalfTimeBreak=*/false);
	TraceGameState->MatchEndServerTime = static_cast<float>(TraceGameState->GetServerWorldTimeSeconds());
	TraceGameState->ForceNetUpdate();

	// The objective leaves the field with the whistle: nobody should be able to "score" into the
	// results screen, and the reset that follows a goal would fight the post-match state.
	ReleaseCore();

	// Players keep their pawns and keep respawning after the whistle — the HUD switches to the FINAL
	// banner, and leaving everyone alive means nobody is staring at a corpse on the results screen.
	UE_LOG(LogTraceGame, Display, TEXT("Match over by %s. Winner: %s (Blue %d - Orange %d, %s)"),
		TraceMatchEndReasonHeadline(Reason),
		(WinningTeam == ETraceTeam::None) ? TEXT("draw") : *TraceTeamName(WinningTeam).ToString(),
		TraceGameState->BlueScore, TraceGameState->OrangeScore,
		*TraceScoringModeLabel(TraceGameState->GetScoringMode()));

	// PostMatch is a phase with an exit, not a dead end. The HUD renders the same countdown from
	// the same constant, so what the player is told is what actually happens.
	GetWorldTimerManager().SetTimer(ReturnToMenuTimerHandle, this, &ATraceGameMode::ReturnToMainMenu,
		TraceMatchFlow::PostMatchDuration, false);
}

void ATraceGameMode::ReturnToMainMenu()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	UE_LOG(LogTraceGame, Log, TEXT("Post-match window elapsed; returning to %s."), TraceMaps::MainMenu);

	// Send every REMOTE client home first, explicitly, before this machine leaves.
	//
	// The OpenLevel below is a purely local absolute travel. On a listen server that tears the net
	// driver down under any connected client with no notice at all: they lose the host mid-frame
	// and (before this) sat looking at a frozen post-match screen until something timed out. Now
	// they are told to go to the same place, by name, one frame earlier.
	//
	// ClientTravel with TRAVEL_Absolute and a local map path is a client RPC: the client loads the
	// menu map on its OWN machine and drops the connection as part of doing so. That is what we
	// want - the menu is a single-player screen, so ServerTravel (which would drag everybody into
	// a NETWORKED menu map still owned by this host) would be wrong.
	int32 RemotesSentHome = 0;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		// IsLocalController() is false for exactly the remote humans. Bots are AAIControllers and
		// never appear in this iterator at all.
		if (PC != nullptr && !PC->IsLocalController())
		{
			PC->ClientTravel(TraceMaps::MainMenu, ETravelType::TRAVEL_Absolute);
			++RemotesSentHome;
		}
	}
	if (RemotesSentHome > 0)
	{
		UE_LOG(LogTraceGame, Log, TEXT("[Net] Sent %d remote client(s) back to the menu before the host travels."),
			RemotesSentHome);
	}

	// Absolute travel: the menu runs a different game mode on a different map, and a relative
	// travel would carry this match's URL options (?difficulty=, ?bots=) into it.
	UGameplayStatics::OpenLevel(World, FName(TraceMaps::MainMenu), /*bAbsolute=*/true);
}

// =============================================================================================
// CHARACTER SELECTION (spec v14 §3)
// =============================================================================================

// ---------------------------------------------------------------------------------------------
// WHERE THE RULES ACTUALLY LIVE, and what this class contributes
//
// UTraceAbilityComponent owns the ROSTER RULE: per-team uniqueness, "None is always allowed", "bots
// never hold a character", and the master on/off switch. Those are the same rules five character
// agents are coding against, and re-implementing them here would give the project two answers to one
// question — the exact drift this codebase keeps getting bitten by.
//
// What this class owns is the SESSION and the CONVERSATION, neither of which the component has:
//   * WHO is being asked to pick, and WHEN their screen opens (PollCharacterSelect);
//   * the TIMEOUT that auto-assigns, so one idle player cannot stall the match (spec §3
//     [ASSUMPTION]) — the component supplies PickFreeCharacterFor, but nothing drives it;
//   * the VERDICT sent back to the requesting client, which is what makes spec §3's "the loser is
//     TOLD and re-picks" true. UTraceAbilityComponent::ServerRequestSetCharacter is fire-and-forget
//     and answers nothing, so a losing client would otherwise see its button do nothing at all.
//
// So RequestCharacter below is a REPORTER, not a second rulebook: it asks the component's own
// predicate, calls the component's own setter, and then CHECKS WHAT ACTUALLY HAPPENED rather than
// assuming its own pre-flight was right.
// ---------------------------------------------------------------------------------------------

void ATraceGameMode::ResolveCharactersEnabled(const FString& Options)
{
	// Priority order, highest first. Each source answers a different question, which is why there are
	// four of them and why the order is what it is:
	//
	//   1. "?characters=0|1" — what the player just chose in the settings menu, carried across the
	//      travel by the title screen. The most recent human decision wins over everything.
	//   2. "-TraceCharacters=0|1" — a headless run's override, so an automated pass can test both
	//      arms of the toggle in one build without touching any saved file.
	//   3. TraceCharacters::GetEnabledSetting() — the saved toggle, for the case the URL is dropped
	//      (a map opened directly, a PIE run, a listen server restarted from the console).
	//   4. the shipped default in Config/DefaultGame.ini.
	//
	// The RESOLVED answer is written onto the UTraceSettings CDO, which is where
	// UTraceAbilityComponent::AreCharactersEnabled reads it from — exactly the shape
	// TraceScoring::ApplyToSettings uses for the A/B mode, and for the same reason: writing it back
	// means the settings page, the travel URL and the framework are all reading one value rather than
	// three that can disagree.
	//
	// Note the '?' separator rule this file already learned the hard way (see the bots/difficulty
	// block in InitGame): UE URL options chain with '?', never with '&'.
	bool bResolved = UTraceSettings::Get().bCharactersEnabled;
	const TCHAR* Source = TEXT("project settings");

	if (UGameplayStatics::HasOption(Options, TraceCharacters::UrlOption))
	{
		bResolved = UGameplayStatics::GetIntOption(Options, TraceCharacters::UrlOption, 1) != 0;
		Source = TEXT("travel URL '?characters='");
	}
	else
	{
		int32 CommandLineValue = -1;
		if (FParse::Value(FCommandLine::Get(), TEXT("TraceCharacters="), CommandLineValue))
		{
			bResolved = (CommandLineValue != 0);
			Source = TEXT("command line '-TraceCharacters='");
		}
		else if (TraceCharacters::HasSavedSetting())
		{
			bResolved = TraceCharacters::GetEnabledSetting();
			Source = TEXT("saved settings toggle");
		}
	}

	if (UTraceSettings* MutableSettings = GetMutableDefault<UTraceSettings>())
	{
		MutableSettings->bCharactersEnabled = bResolved;
	}

	UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] Characters are %s for this match (%s)."),
		bResolved ? TEXT("ON") : TEXT("OFF"), Source);
}

bool ATraceGameMode::AreCharactersEnabled() const
{
	// FORWARDED, not re-derived. The framework's version already answers both halves — the settings
	// toggle and spec §2's mode A freeze — and it is the version every ability in the game obeys. A
	// second implementation here could say "on" in a match where no ability would ever fire.
	return HasAuthority() && UTraceAbilityComponent::AreCharactersEnabled(this);
}

bool ATraceGameMode::IsCharacterTakenOnTeam(ETraceTeam InTeam, uint8 CharacterId, const ATracePlayerState* Except) const
{
	if (InTeam == ETraceTeam::None || !TraceCharacterRoster::IsValidId(CharacterId))
	{
		return false;
	}

	const APlayerState* const Holder = UTraceAbilityComponent::FindTeammateHolding(
		this, InTeam, static_cast<ETraceCharacterId>(CharacterId));

	return (Holder != nullptr) && (Holder != Except);
}

uint8 ATraceGameMode::FindFreeCharacterForTeam(ETraceTeam InTeam, const ATracePlayerState* Except) const
{
	// The framework's own picker, so the auto-assign and a manual pick agree about what "free" means.
	// It takes the asking player state rather than a team, which is what makes "except me" implicit.
	const ETraceCharacterId Picked = UTraceAbilityComponent::PickFreeCharacterFor(Except);
	if (Picked != ETraceCharacterId::None)
	{
		return static_cast<uint8>(Picked);
	}

	// Five characters and five players per team, so this is only reachable if PlayersPerTeam is
	// raised above the roster size. Say so rather than handing back Rocco twice.
	UE_LOG(LogTraceGame, Warning,
		TEXT("[CharSelect] No free character left on %s - the team has more players than the roster has characters."),
		*TraceTeamName(InTeam).ToString());

	return TraceCharacterRoster::NoneId;
}

ETraceCharacterPickResult ATraceGameMode::RequestCharacter(ATracePlayerState* Requester, uint8 RequestedCharacter)
{
	if (!HasAuthority() || Requester == nullptr)
	{
		return ETraceCharacterPickResult::Disabled;
	}

	if (!AreCharactersEnabled())
	{
		return ETraceCharacterPickResult::Disabled;
	}

	if (!TraceCharacterRoster::IsValidId(RequestedCharacter))
	{
		return ETraceCharacterPickResult::InvalidId;
	}

	// LOCKED IS TESTED BEFORE "not selecting", and the order is the fix for a bug the scripted proof
	// caught on its first run: a locked-in player's screen is CLOSED (closing it is what a grant
	// does), so the not-selecting test below matched first and a player who tried to switch was told
	// "NOT PICKING RIGHT NOW" instead of "YOU HAVE ALREADY LOCKED IN". Both refuse, so nothing was
	// broken — but the message named the wrong reason, and a wrong reason on a screen whose entire job
	// is telling the player why they were refused is the failure this feature is most likely to be
	// reported for.
	if (Requester->bCharacterLocked)
	{
		return ETraceCharacterPickResult::AlreadyLocked;
	}

	// A bot has no select screen and cannot have sent this; if one ever does, refuse rather than
	// quietly giving it an ability set the spec says it must not have.
	if (Requester->IsABot() || !Requester->IsCharacterSelectOpen())
	{
		return ETraceCharacterPickResult::NotSelecting;
	}

	UTraceAbilityComponent* const Abilities = UTraceAbilityComponent::Get(Requester);
	if (Abilities == nullptr)
	{
		// The framework has not attached a component yet. Refusing as "not selecting" rather than
		// granting is the safe direction — a grant we cannot write is a grant the player would see
		// accepted and then lose.
		return ETraceCharacterPickResult::NotSelecting;
	}

	const ETraceCharacterId Wanted = static_cast<ETraceCharacterId>(RequestedCharacter);

	// THE RULE. Spec v14 §3, verbatim: "Do not allow players to select a character who has already
	// been chosen by a player on their team". Enemies mirroring the pick are explicitly fine, which
	// is why this asks IsCharacterAvailableFor (per-TEAM) rather than "is anybody holding it".
	//
	// Asked here as a PRE-FLIGHT so the refusal can be reported with a reason. The authoritative
	// enforcement is ServerSetCharacter's own, checked immediately below — see the post-verify.
	bool bAvailable = UTraceAbilityComponent::IsCharacterAvailableFor(Requester, Wanted);

	// THE RED ARM. See GTraceEnforceSelectRules at the top of this file for exactly what it reaches
	// and what it deliberately does not.
	bool bEnforceHere = true;
#if !UE_BUILD_SHIPPING
	bEnforceHere = (GTraceEnforceSelectRules != 0);
#endif

	if (!bEnforceHere)
	{
		bAvailable = true;
	}

	if (!bAvailable)
	{
		UE_LOG(LogTraceGame, Log, TEXT("[CharSelect] '%s' asked for %s; a team-mate already has it."),
			*Requester->GetPlayerName(), *TraceCharacterRoster::NameFor(RequestedCharacter));
		return ETraceCharacterPickResult::TakenByTeammate;
	}

	Abilities->ServerSetCharacter(Wanted);

	// THE POST-VERIFY, and it is the reason this function is trustworthy. The pre-flight above is a
	// separate read of a roster that another request may have changed in between; the only fact worth
	// reporting is what the component actually holds now. Without this, a losing client could be told
	// "granted" for a character the framework quietly refused.
	//
	// Skipped by the red arm as well as the pre-flight, and it has to be: with only the pre-flight
	// bypassed this check silently produced the correct refusal anyway and the red run reported 19/19
	// green. See GTraceEnforceSelectRules.
	if (bEnforceHere && Abilities->GetCharacterId() != Wanted)
	{
		UE_LOG(LogTraceGame, Log, TEXT("[CharSelect] '%s' asked for %s; the framework refused it."),
			*Requester->GetPlayerName(), *TraceCharacterRoster::NameFor(RequestedCharacter));
		return ETraceCharacterPickResult::TakenByTeammate;
	}

	Requester->ServerMarkCharacterResolved(/*bLocked=*/true, /*bWasChosen=*/true);
	Requester->ServerSetCharacterSelectOpen(/*bOpen=*/false, /*DeadlineServerTime=*/0.f);

	return ETraceCharacterPickResult::Granted;
}

void ATraceGameMode::PollCharacterSelect()
{
	if (!HasAuthority())
	{
		return;
	}

	const AGameStateBase* BaseGameState = GameState;
	if (BaseGameState == nullptr)
	{
		return;
	}

	const bool bEnabled = AreCharactersEnabled();
	const double NowServer = BaseGameState->GetServerWorldTimeSeconds();

	for (APlayerState* const EachState : BaseGameState->PlayerArray)
	{
		ATracePlayerState* Candidate = Cast<ATracePlayerState>(EachState);
		if (Candidate == nullptr)
		{
			continue;
		}

		// ---- Characters off, or mode A: force everyone back to the Mannequin --------------------
		//
		// Not just "do not open a screen". Spec v14 §2's [ASSUMPTION] is that loading mode A forces
		// every player to the default characterless Mannequin, and the toggle has to be able to undo
		// a selection made before it was flipped — otherwise turning characters off mid-session would
		// leave everyone holding the abilities it was supposed to remove.
		if (!bEnabled)
		{
			if (Candidate->IsCharacterSelectOpen())
			{
				Candidate->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
			}
			if (Candidate->GetSelectedCharacter() != TraceCharacterRoster::NoneId)
			{
				if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Candidate))
				{
					// Setting None is documented as always allowed and never refused, which is what
					// makes this a safe unconditional clear rather than a request that might bounce.
					Abilities->ServerSetCharacter(ETraceCharacterId::None);
				}
				Candidate->ServerMarkCharacterResolved(/*bLocked=*/false, /*bWasChosen=*/false);
			}
			continue;
		}

		// ---- Bots never see this screen and never hold a character ------------------------------
		if (Candidate->IsABot())
		{
			if (Candidate->GetSelectedCharacter() != TraceCharacterRoster::NoneId)
			{
				// Defensive, and worth keeping: a bot with an ability set is the single most
				// confusing outcome this feature could produce, and it would look like an AI bug.
				// The framework refuses to give a bot a character in the first place, so reaching
				// this is itself news.
				UE_LOG(LogTraceGame, Warning, TEXT("[CharSelect] Bot '%s' held %s; clearing it."),
					*Candidate->GetPlayerName(), *TraceCharacterRoster::NameFor(Candidate->GetSelectedCharacter()));

				if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Candidate))
				{
					Abilities->ServerSetCharacter(ETraceCharacterId::None);
				}
			}
			if (Candidate->IsCharacterSelectOpen())
			{
				Candidate->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
			}
			continue;
		}

		// A player with no team yet has no team-mates to be unique against, so the screen would be
		// drawing an answer it cannot enforce. Wait for the balancer.
		if (Candidate->Team == ETraceTeam::None)
		{
			continue;
		}

		// ---- Already sorted --------------------------------------------------------------------
		if (Candidate->HasCharacter())
		{
			if (Candidate->IsCharacterSelectOpen())
			{
				Candidate->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);
			}
			continue;
		}

		// ---- Open the screen -------------------------------------------------------------------
		if (!Candidate->IsCharacterSelectOpen())
		{
			const float Deadline = (CharacterSelectTimeout > 0.f)
				? static_cast<float>(NowServer + CharacterSelectTimeout)
				: 0.f;

			Candidate->ServerSetCharacterSelectOpen(/*bOpen=*/true, Deadline);

			UE_LOG(LogTraceGame, Log, TEXT("[CharSelect] Select screen opened for '%s' (%s)%s."),
				*Candidate->GetPlayerName(), *TraceTeamName(Candidate->Team).ToString(),
				(Deadline > 0.f) ? *FString::Printf(TEXT("; auto-pick in %.0fs"), CharacterSelectTimeout) : TEXT("; no timeout"));
			continue;
		}

		// ---- The timeout (spec v14 §3 [ASSUMPTION]) ---------------------------------------------
		//
		// The SECOND thing the red arm reaches. This one is wholly ours — nothing in the ability
		// framework drives a timeout — so switching it off makes the harness's auto-assign assertion
		// fail outright, with no second layer to mask it. That is what makes the red run informative
		// rather than merely different.
		bool bAutoAssign = true;
#if !UE_BUILD_SHIPPING
		bAutoAssign = (GTraceEnforceSelectRules != 0);
#endif

		const float Deadline = Candidate->CharacterSelectDeadlineServerTime;
		if (bAutoAssign && Deadline > 0.f && NowServer >= static_cast<double>(Deadline))
		{
			const uint8 AutoPick = FindFreeCharacterForTeam(Candidate->Team, Candidate);

			if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Candidate))
			{
				Abilities->ServerSetCharacter(static_cast<ETraceCharacterId>(AutoPick));
			}

			// Closed either way. A player the roster cannot serve still must not sit behind a screen
			// forever — they play the Mannequin, which is a worse outcome than picking and a much
			// better one than being stuck.
			Candidate->ServerMarkCharacterResolved(
				/*bLocked=*/TraceCharacterRoster::IsValidId(AutoPick), /*bWasChosen=*/false);
			Candidate->ServerSetCharacterSelectOpen(/*bOpen=*/false, 0.f);

			UE_LOG(LogTraceGame, Display, TEXT("[CharSelect] '%s' did not pick in time; auto-assigned %s."),
				*Candidate->GetPlayerName(), *TraceCharacterRoster::NameFor(AutoPick));
		}
	}
}

void ATraceGameMode::ResetAbilityCooldownsForHalfTime()
{
	if (!HasAuthority() || GameState == nullptr)
	{
		return;
	}

	// Spec v14 §5, verbatim: "They should all reset at halftime." EVERY player state, including ones
	// whose owner is dead and ones whose pawn does not exist — which is the whole reason the ability
	// component hangs off the player state rather than the pawn. A loop over live pawns would silently
	// skip exactly the players the rule was written to protect.
	//
	// UTraceAbilityWorldSubsystem also watches the GameState for the break and calls OnHalfTime()
	// itself. Both drivers are kept on purpose and are harmless together: OnHalfTime() zeroes an
	// absolute deadline and is idempotent, and this one fires on the SAME FRAME the break begins
	// rather than on whatever tick the subsystem next notices it — which is the difference between a
	// player watching their meter snap to ready and a player watching it keep counting for a moment.
	int32 ResetCount = 0;
	for (APlayerState* const EachState : GameState->PlayerArray)
	{
		if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(EachState))
		{
			if (Abilities->GetActivatedCooldownRemaining() > 0.f)
			{
				++ResetCount;
			}
			Abilities->OnHalfTime();
		}
	}

	UE_LOG(LogTraceGame, Log, TEXT("[Ability] Half time: %d running cooldown(s) reset."), ResetCount);
}

#if !UE_BUILD_SHIPPING

// =============================================================================================
// Trace.HalfTime.Verify — the scripted proof for spec v9 §11
//
// WHY THIS EXISTS, and it is the same reason spec v9 §0 exists. The organic run — shorten the half,
// watch the clock hit 0:00, watch play continue, watch the whistle go on the next turnover — only
// ever exercises the path that FIRES. It is the happy path, and a harness that only ever went green
// on the happy path is what shipped a "fixed" dash last pass.
//
// The two cases that decide whether §11 is right are the ones that must NOT fire:
//
//   1. A WITHIN-TEAM PASS. If a team-mate taking the Core ends the half, the note's own complaint
//      ("it doesn't cut people off in the middle of a run") is unfixed, and worse than before —
//      because a pass is the single most common possession event in the game.
//   2. NOTHING AT ALL. If the hard cap does not fire, a stalemate plays past full time forever.
//
// Neither is reachable by waiting: in a 10-bot match a between-team turnover arrives within a
// second or two of the clock expiring, so the interesting windows never open on their own. This
// drives both on purpose and prints PASS/FAIL per assertion.
//
// The cap scenario suppresses dead balls by RELEASING THE CORE every 0.1 s. With no holder there is
// no possession to turn over, nothing to carry into an endzone and (mode A) nothing loose to come
// down, so the only thing left that can end the period is the cap — which is exactly the state the
// cap exists for.
// =============================================================================================

void ATraceGameMode::StartHalfTimeVerify(ETraceHalfTimeVerifyScenario Scenario)
{
	if (!HasAuthority())
	{
		return;
	}

	const ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::InProgress
		|| TraceGameState->IsHalfTimeBreak())
	{
		// WAITS RATHER THAN REFUSING, so this can be handed straight to -ExecCmds. Those run at
		// engine init, several seconds before warm-up ends and a half exists; a command that simply
		// said "no live half" there would make the whole harness unusable from a headless run, which
		// is the only way it ever gets used.
		UE_LOG(LogTraceGame, Log,
			TEXT("[HalfTimeVerify] No live half yet (state=%d, break=%d) - waiting."),
			(TraceGameState != nullptr) ? static_cast<int32>(TraceGameState->TraceMatchState) : -1,
			(TraceGameState != nullptr && TraceGameState->IsHalfTimeBreak()) ? 1 : 0);

		GetWorldTimerManager().SetTimer(HalfTimeVerifyHandle,
			FTimerDelegate::CreateWeakLambda(this, [this, Scenario]()
			{
				StartHalfTimeVerify(Scenario);
			}),
			0.5f, /*bLoop=*/false);
		return;
	}

	HalfTimeVerifyScenario = Scenario;
	HalfTimeVerifyStep = 0;
	HalfTimeVerifyPassed = 0;
	HalfTimeVerifyFailed = 0;
	HalfTimeVerifyHolder = nullptr;

	const TCHAR* ScenarioName = TEXT("TURNOVER vs WITHIN-TEAM PASS");
	if (Scenario == ETraceHalfTimeVerifyScenario::HardCap)
	{
		ScenarioName = TEXT("HARD CAP");
	}
	else if (Scenario == ETraceHalfTimeVerifyScenario::MercyWhilePending)
	{
		ScenarioName = TEXT("MERCY RULE WHILE A WHISTLE IS PENDING");
	}

	UE_LOG(LogTraceGame, Display, TEXT("[HalfTimeVerify] ===== %s scenario ====="), ScenarioName);

	// 0.2 s per step: four polls of PollPendingPeriodEnd (20 Hz) between each scripted event, so a
	// whistle that was going to fire has had every chance to, and short enough that ten bots have
	// little opportunity to produce an organic turnover in the middle of the scenario. When one
	// does anyway, the step that notices says so rather than reporting a false FAIL.
	GetWorldTimerManager().SetTimer(HalfTimeVerifyHandle, this, &ATraceGameMode::RunHalfTimeVerifyStep,
		0.2f, /*bLoop=*/true, /*FirstDelay=*/0.2f);
}

void ATraceGameMode::RunHalfTimeVerifyStep()
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	ATraceCore* TheCore = GetCore();
	if (TraceGameState == nullptr || TheCore == nullptr)
	{
		GetWorldTimerManager().ClearTimer(HalfTimeVerifyHandle);
		return;
	}

	// Local lambdas rather than members: nothing outside this scenario wants them, and keeping them
	// here means the whole harness is one contiguous block that can be deleted in one edit.
	const auto Report = [this](bool bCondition, const TCHAR* What)
	{
		if (bCondition)
		{
			++HalfTimeVerifyPassed;
			UE_LOG(LogTraceGame, Display, TEXT("[HalfTimeVerify]   PASS  %s"), What);
		}
		else
		{
			++HalfTimeVerifyFailed;
			UE_LOG(LogTraceGame, Error, TEXT("[HalfTimeVerify]   FAIL  %s"), What);
		}
	};

	/** A living pawn on @p Team (bAlly = true) or not on it (false), other than @p Except. */
	const auto FindPawnFor = [this](ETraceTeam Team, bool bAlly, const ATraceCharacter* Except) -> ATraceCharacter*
	{
		for (const TWeakObjectPtr<ATraceCharacter>& Weak : TrackedCharacters)
		{
			ATraceCharacter* Candidate = Weak.Get();
			if (Candidate == nullptr || Candidate == Except || !Candidate->IsAlive())
			{
				continue;
			}

			const ETraceTeam CandidateTeam = Candidate->GetTeam();
			if (CandidateTeam == ETraceTeam::None)
			{
				continue;
			}

			if ((CandidateTeam == Team) == bAlly)
			{
				return Candidate;
			}
		}
		return nullptr;
	};

	++HalfTimeVerifyStep;

	// The wait budget belongs to the STEP, not to the tick: DetectorHasRun() below re-enters the
	// same step by decrementing the counter, so the budget may only be reset when the step number
	// genuinely moves on.
	if (HalfTimeVerifyStep != HalfTimeVerifyLastStep)
	{
		HalfTimeVerifyLastStep = HalfTimeVerifyStep;
		HalfTimeVerifyWaitSteps = 0;
	}

	/**
	 * Holds the CURRENT step until the production detector has actually run, or until the wait
	 * budget is spent.
	 *
	 * WHY THIS EXISTS, and it is spec v9 §0's lesson in miniature. The scenarios used to be timed in
	 * seconds (a step every 0.2 s, four 20 Hz polls apart). On a machine running several headless
	 * instances the engine's frame-rate smoothing clamps the world delta, ticks arrive whole seconds
	 * apart in wall-clock, and three scripted steps can land on three consecutive FRAMES with the
	 * poll timer never getting one in between. That produced both failure modes at once:
	 *
	 *   * the positive case FAILED ("the turnover did not blow the whistle") when the detector had
	 *     simply never been given a tick — a red that says nothing about the code;
	 *   * the negative case PASSED ("a within-team pass did not end the half") for exactly the same
	 *     reason — a green that proves nothing, which is the failure §0 was written about.
	 *
	 * So the steps wait on POLLS, not on seconds. @return true when the step may give its verdict.
	 */
	const auto DetectorHasRun = [this](const TCHAR* Waiting) -> bool
	{
		if ((PendingPeriodEndPollCount - HalfTimeVerifyPollMark) >= HalfTimeVerifyPollsPerStep)
		{
			return true;
		}

		// The whistle going IS the detector having run — and it stops the poll, so waiting for more
		// polls after it would wait forever.
		const ATraceGameState* WaitState = GetTraceGameState();
		if (WaitState == nullptr || !WaitState->bPendingPeriodEnd)
		{
			return true;
		}

		if (++HalfTimeVerifyWaitSteps > HalfTimeVerifyMaxWaitSteps)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[HalfTimeVerify] TIMED OUT waiting for %s: only %d polls in %d steps. The 20Hz "
				     "detector is not running at all - that is the finding, not the assertion below."),
				Waiting, PendingPeriodEndPollCount - HalfTimeVerifyPollMark, HalfTimeVerifyMaxWaitSteps);
			return true;
		}

		// Re-enter the same step next tick.
		--HalfTimeVerifyStep;
		return false;
	};

	// ------------------------------------------------------------------------------------------
	// THE HARD CAP SCENARIO
	// ------------------------------------------------------------------------------------------
	// ------------------------------------------------------------------------------------------
	// THE MERCY SCENARIO — spec v9 §11's one explicit exception, and the one interaction that could
	// quietly break either rule.
	//
	// A mercy win is a MERCY, not a play boundary: it must still end the match on the frame the lead
	// reaches the threshold, even if that frame is the middle of a run and even if a deferred
	// whistle is already armed. And once it has, nothing of the pending state may survive under the
	// results screen — an orphaned cap timer would fire BeginHalfTimeBreak into a finished match and
	// restart it, which is the restart loop the contract forbids.
	//
	// Both halves are asserted here: the match ends immediately with reason Mercy, and BOTH pending
	// timers are checked directly with IsTimerActive() rather than inferred from the flag.
	// ------------------------------------------------------------------------------------------
	if (HalfTimeVerifyScenario == ETraceHalfTimeVerifyScenario::MercyWhilePending)
	{
		switch (HalfTimeVerifyStep)
		{
		case 1:
		{
			ATraceCharacter* Holder = TheCore->GetCarrier();
			if (Holder == nullptr)
			{
				Holder = FindPawnFor(ETraceTeam::Blue, /*bAlly=*/true, nullptr);
				if (Holder != nullptr)
				{
					TheCore->GrantTo(Holder, ETraceCoreGrantReason::Debug);
				}
			}

			HandleHalfExpired();

			Report(TraceGameState->bPendingPeriodEnd,
				TEXT("the clock expiring ARMED the whistle (so the mercy rule has something to pre-empt)"));
			Report(!TraceGameState->IsHalfTimeBreak() && TraceGameState->TraceMatchState == ETraceMatchState::InProgress,
				TEXT("play is still live with the whistle pending"));
			UE_LOG(LogTraceGame, Display,
				TEXT("[HalfTimeVerify]   pending, carrier is %s. Now taking an %d-point lead MID-RUN."),
				*GetNameSafe(TheCore->GetCarrier()), UTraceSettings::Get().MercyRuleLead);
			return;
		}

		case 2:
		{
			// The lead arrives the way any lead does — through the scoreboard — and then the rule is
			// asked the same question NotifyScored asks it. Nothing here reaches past CheckMercyRule
			// into FinishMatch, because what is under test is whether the mercy path is willing to
			// end a match while a whistle is pending, not whether FinishMatch works.
			const int32 RequiredLead = FMath::Max(1, UTraceSettings::Get().MercyRuleLead);
			TraceGameState->AddScore(ETraceTeam::Blue, RequiredLead);

			const bool bEnded = CheckMercyRule(TEXT("HalfTimeVerify mercy scenario"));

			Report(bEnded, TEXT("the mercy rule ended the match IMMEDIATELY - it did not wait for a dead ball"));
			Report(TraceGameState->TraceMatchState == ETraceMatchState::PostMatch,
				TEXT("the match is in PostMatch on the same frame the lead arrived"));
			Report(TraceGameState->GetMatchEndReason() == ETraceMatchEndReason::Mercy,
				TEXT("the recorded end reason is Mercy, not Clock"));
			Report(!TraceGameState->IsHalfTimeBreak(),
				TEXT("no half-time break was entered on the way out - a mercy win skips the interval"));
			return;
		}

		case 3:
		default:
		{
			// The residue check. The flag is the easy half; the timers are the half that would
			// actually restart a finished match.
			const bool bPollArmed = GetWorldTimerManager().IsTimerActive(PendingPeriodEndPollHandle);
			const bool bCapArmed = GetWorldTimerManager().IsTimerActive(PendingPeriodEndCapHandle);

			Report(!TraceGameState->bPendingPeriodEnd,
				TEXT("the pending flag was dropped by the mercy win"));
			Report(!bPollArmed && !bCapArmed,
				TEXT("NEITHER pending timer is still armed under the results screen"));
			Report(TraceGameState->TraceMatchState == ETraceMatchState::PostMatch,
				TEXT("the match is still finished a step later - nothing re-started it"));

			UE_LOG(LogTraceGame, Display,
				TEXT("[HalfTimeVerify] ===== MERCY WHILE PENDING: %d passed, %d failed ====="),
				HalfTimeVerifyPassed, HalfTimeVerifyFailed);
			GetWorldTimerManager().ClearTimer(HalfTimeVerifyHandle);
			return;
		}
		}
	}

	if (HalfTimeVerifyScenario == ETraceHalfTimeVerifyScenario::HardCap)
	{
		if (HalfTimeVerifyStep == 1)
		{
			HandleHalfExpired();

			Report(TraceGameState->bPendingPeriodEnd, TEXT("the clock expiring ARMED the whistle"));
			Report(!TraceGameState->IsHalfTimeBreak(), TEXT("play continued - no half-time break yet"));
			UE_LOG(LogTraceGame, Display, TEXT("[HalfTimeVerify]   cap armed for %.1fs (setting says %.1f)"),
				TraceGameState->GetPendingPeriodEndTimeRemaining(),
				UTraceSettings::Get().PeriodEndMaxDeferSeconds);
			return;
		}

		// Hold every dead ball off. See the block comment: no holder means no turnover, no capture
		// and nothing loose, so the cap is the only thing left that can end the period.
		if (TraceGameState->bPendingPeriodEnd)
		{
			ReleaseCore();

			// Roughly PeriodEndMaxDeferSeconds should elapse. Give it a generous ceiling so a
			// misconfigured run ends with a FAIL rather than a hung harness.
			if (HalfTimeVerifyStep > 400)
			{
				Report(false, TEXT("the hard cap fired - IT DID NOT, after 80s of suppressed dead balls"));
				GetWorldTimerManager().ClearTimer(HalfTimeVerifyHandle);
			}
			return;
		}

		Report(TraceGameState->IsHalfTimeBreak() || TraceGameState->TraceMatchState == ETraceMatchState::PostMatch,
			TEXT("the hard cap ended the period with no dead ball available"));

		UE_LOG(LogTraceGame, Display,
			TEXT("[HalfTimeVerify] ===== HARD CAP: %d passed, %d failed (whistle went %.1fs after arming) ====="),
			HalfTimeVerifyPassed, HalfTimeVerifyFailed, (HalfTimeVerifyStep - 1) * 0.2f);
		GetWorldTimerManager().ClearTimer(HalfTimeVerifyHandle);
		return;
	}

	// ------------------------------------------------------------------------------------------
	// THE TURNOVER / WITHIN-TEAM-PASS SCENARIO
	// ------------------------------------------------------------------------------------------
	switch (HalfTimeVerifyStep)
	{
	case 1:
	{
		// Make sure somebody is actually carrying, so "pass to a team-mate" has a meaning.
		ATraceCharacter* Holder = TheCore->GetCarrier();
		if (Holder == nullptr)
		{
			Holder = FindPawnFor(ETraceTeam::Blue, /*bAlly=*/true, nullptr);
			if (Holder != nullptr)
			{
				TheCore->GrantTo(Holder, ETraceCoreGrantReason::Debug);
			}
		}
		HalfTimeVerifyHolder = Holder;

		UE_LOG(LogTraceGame, Display, TEXT("[HalfTimeVerify] step 1: carrier is %s (%s). Clock %.1fs remaining."),
			*GetNameSafe(Holder), Holder ? *TraceTeamName(Holder->GetTeam()).ToString() : TEXT("none"),
			TraceGameState->GetMatchTimeRemaining());
		return;
	}

	case 2:
		// THE CLOCK RUNS OUT. Not by waiting for it — by calling exactly what the half's own timer
		// calls, so what is under test is the real path and not a stand-in for it.
		HandleHalfExpired();

		Report(TraceGameState->bPendingPeriodEnd,
			TEXT("the clock expiring ARMED the whistle instead of ending the half"));
		Report(!TraceGameState->IsHalfTimeBreak() && TraceGameState->TraceMatchState == ETraceMatchState::InProgress,
			TEXT("PLAY CONTINUED past 0:00 - no half-time break, match still InProgress"));
		Report(TraceGameState->GetMatchTimeRemaining() <= 0.01f,
			TEXT("the clock reads 0:00"));
		Report(!TraceGameState->GetPendingPeriodEndLabel().IsEmpty(),
			TEXT("the HUD has a label to draw (GetPendingPeriodEndLabel is non-empty)"));
		UE_LOG(LogTraceGame, Display, TEXT("[HalfTimeVerify]   label: \"%s\""),
			*TraceGameState->GetPendingPeriodEndLabel());

		// From here on every verdict waits on the detector rather than on the clock.
		HalfTimeVerifyPollMark = PendingPeriodEndPollCount;
		return;

	case 3:
	{
		// THE NEGATIVE CASE. Pass WITHIN the team. This must not end the half.
		//
		// Gated first on the detector having run since the whistle was armed, so the pass is made
		// into a poll that is demonstrably awake — otherwise "it did not fire" would be measuring
		// a detector that was never asked.
		if (!DetectorHasRun(TEXT("the detector to wake up after arming")))
		{
			return;
		}

		ATraceCharacter* Holder = TheCore->GetCarrier();
		if (Holder == nullptr || !TraceGameState->bPendingPeriodEnd)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[HalfTimeVerify] step 3 INCONCLUSIVE: an organic event beat the script (carrier=%s pending=%d)."),
				*GetNameSafe(Holder), TraceGameState->bPendingPeriodEnd ? 1 : 0);
			GetWorldTimerManager().ClearTimer(HalfTimeVerifyHandle);
			return;
		}

		ATraceCharacter* Mate = FindPawnFor(Holder->GetTeam(), /*bAlly=*/true, Holder);
		if (Mate == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[HalfTimeVerify] step 3 INCONCLUSIVE: no living team-mate to pass to."));
			GetWorldTimerManager().ClearTimer(HalfTimeVerifyHandle);
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[HalfTimeVerify] step 3: WITHIN-TEAM pass %s -> %s (both %s)."),
			*GetNameSafe(Holder), *GetNameSafe(Mate), *TraceTeamName(Holder->GetTeam()).ToString());
		TheCore->GrantTo(Mate, ETraceCoreGrantReason::Pass);
		HalfTimeVerifyPollMark = PendingPeriodEndPollCount;
		return;
	}

	case 4:
		// "It did not fire" only means something once the detector has SEEN the pass. Waiting on
		// polls is what makes this negative case evidence instead of a coincidence.
		if (!DetectorHasRun(TEXT("the detector to inspect the within-team pass")))
		{
			return;
		}

		Report(TraceGameState->bPendingPeriodEnd && !TraceGameState->IsHalfTimeBreak(),
			TEXT("a WITHIN-TEAM pass did NOT end the half - play continues, as spec v9 §11 asks"));
		UE_LOG(LogTraceGame, Display,
			TEXT("[HalfTimeVerify]   (the detector ran %d times over that pass and stayed quiet)"),
			PendingPeriodEndPollCount - HalfTimeVerifyPollMark);
		return;

	case 5:
	{
		// THE POSITIVE CASE. Hand it to the other team.
		ATraceCharacter* Holder = TheCore->GetCarrier();
		if (Holder == nullptr || !TraceGameState->bPendingPeriodEnd)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[HalfTimeVerify] step 5 INCONCLUSIVE: an organic event beat the script (carrier=%s pending=%d)."),
				*GetNameSafe(Holder), TraceGameState->bPendingPeriodEnd ? 1 : 0);
			GetWorldTimerManager().ClearTimer(HalfTimeVerifyHandle);
			return;
		}

		ATraceCharacter* Enemy = FindPawnFor(Holder->GetTeam(), /*bAlly=*/false, Holder);
		if (Enemy == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[HalfTimeVerify] step 5 INCONCLUSIVE: no living enemy to turn it over to."));
			GetWorldTimerManager().ClearTimer(HalfTimeVerifyHandle);
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[HalfTimeVerify] step 5: BETWEEN-TEAM turnover %s (%s) -> %s (%s)."),
			*GetNameSafe(Holder), *TraceTeamName(Holder->GetTeam()).ToString(),
			*GetNameSafe(Enemy), *TraceTeamName(Enemy->GetTeam()).ToString());
		TheCore->GrantTo(Enemy, ETraceCoreGrantReason::Kill);
		HalfTimeVerifyPollMark = PendingPeriodEndPollCount;
		return;
	}

	case 6:
	default:
		// Same gate, and here it is the difference between a real red and a starved one: the whistle
		// going clears the pending flag, which satisfies DetectorHasRun() immediately, so a working
		// build falls straight through and only a build that genuinely misses the turnover waits.
		if (!DetectorHasRun(TEXT("the detector to inspect the between-team turnover")))
		{
			return;
		}

		Report(!TraceGameState->bPendingPeriodEnd,
			TEXT("the BETWEEN-TEAM turnover blew the deferred whistle"));
		Report(TraceGameState->IsHalfTimeBreak() || TraceGameState->TraceMatchState == ETraceMatchState::PostMatch,
			TEXT("the period actually ended (half-time break, or full time)"));

		UE_LOG(LogTraceGame, Display,
			TEXT("[HalfTimeVerify] ===== TURNOVER vs PASS: %d passed, %d failed ====="),
			HalfTimeVerifyPassed, HalfTimeVerifyFailed);
		GetWorldTimerManager().ClearTimer(HalfTimeVerifyHandle);
		return;
	}
}

namespace
{
	/**
	 * Trace.HalfTime.Verify [cap]
	 *
	 * FAutoConsoleCommandWithWorldAndArgs because the harness lives on the GameMode and a GameMode
	 * only exists inside a world — a world-less console command would have nothing to talk to.
	 */
	void TraceHalfTimeVerifyCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}

		ATraceGameMode* GameMode = World->GetAuthGameMode<ATraceGameMode>();
		if (GameMode == nullptr)
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[HalfTimeVerify] No authoritative ATraceGameMode here - run this on the server."));
			return;
		}

		ETraceHalfTimeVerifyScenario Scenario = ETraceHalfTimeVerifyScenario::TurnoverVsPass;
		if (Args.Num() > 0)
		{
			const FString Arg = Args[0].TrimStartAndEnd();
			if (Arg.Equals(TEXT("cap"), ESearchCase::IgnoreCase))
			{
				Scenario = ETraceHalfTimeVerifyScenario::HardCap;
			}
			else if (Arg.Equals(TEXT("mercy"), ESearchCase::IgnoreCase))
			{
				Scenario = ETraceHalfTimeVerifyScenario::MercyWhilePending;
			}
		}

		GameMode->StartHalfTimeVerify(Scenario);
	}

	FAutoConsoleCommandWithWorldAndArgs CmdTraceHalfTimeVerify(
		TEXT("Trace.HalfTime.Verify"),
		TEXT("Dev only. Spec v9 §11. No argument: prove the clock expiring only ARMS the whistle, that a "
		     "within-team pass does NOT end the half, and that a between-team turnover does. "
		     "'cap': prove the hard cap ends the period when no dead ball is ever available. "
		     "'mercy': prove a mercy win still ends the match immediately with a whistle pending, and "
		     "leaves neither pending timer armed."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&TraceHalfTimeVerifyCommand));
}

// =============================================================================================
// Trace.Characters.Verify — the scripted proof for spec v14 §3
//
// WHAT ORGANIC PLAY CANNOT SHOW YOU. Every interesting case here is a RACE or a REFUSAL, and neither
// is reachable by playing: two team-mates pressing the same key inside one frame does not happen on
// demand, an enemy mirroring a pick looks identical to a bug unless you already know it is legal,
// and the auto-assign timeout takes thirty seconds of doing nothing. So all four are driven here.
//
// HOW IT GETS TWO HUMANS. A headless run has one human at most. The harness borrows bots: it clears
// APlayerState::bIsABot on three of them for the duration, which makes them ordinary players as far
// as every rule in this file is concerned — the SAME RequestCharacter path a real client's RPC lands
// in, not a shortcut past it — and restores the flag at the end. Borrowing rather than faking is the
// point: a harness that called the uniqueness test directly would still pass with the game mode's
// entire request path disconnected.
//
// THE RED ARM. `Trace.Characters.VerifyRed` removes THIS SLICE's enforcement. The team-mate assertion
// must then FAIL, and the enemy-mirror assertion must still PASS — that pair is what shows the
// harness is measuring the rule rather than measuring nothing.
// =============================================================================================

bool ATraceGameMode::ReportCharacterVerify(bool bCondition, const TCHAR* What)
{
	if (bCondition)
	{
		++CharacterVerifyPassed;
		UE_LOG(LogTraceGame, Display, TEXT("[CharVerify] PASS: %s"), What);
	}
	else
	{
		++CharacterVerifyFailed;
		UE_LOG(LogTraceGame, Error, TEXT("[CharVerify] FAIL: %s"), What);
	}

	return bCondition;
}

void ATraceGameMode::DumpCharacterState() const
{
	const AGameStateBase* BaseGameState = GameState;

	UE_LOG(LogTraceGame, Display, TEXT("[CharDump] ===== characters %s, mode %s, timeout %.0fs ====="),
		AreCharactersEnabled() ? TEXT("ON") : TEXT("OFF"),
		(GetTraceGameState() != nullptr && GetTraceGameState()->IsGoalMode()) ? TEXT("B (goals)") : TEXT("A (endzones)"),
		CharacterSelectTimeout);

	if (BaseGameState == nullptr)
	{
		return;
	}

	for (APlayerState* const EachState : BaseGameState->PlayerArray)
	{
		const ATracePlayerState* Candidate = Cast<ATracePlayerState>(EachState);
		if (Candidate == nullptr)
		{
			continue;
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[CharDump]   %-22s %-7s %-10s %-10s select=%s%s  cooldown=%.1f"),
			*Candidate->GetPlayerName(),
			*TraceTeamName(Candidate->Team).ToString(),
			Candidate->IsABot() ? TEXT("BOT") : TEXT("HUMAN"),
			*TraceCharacterRoster::NameFor(Candidate->GetSelectedCharacter()),
			Candidate->IsCharacterSelectOpen() ? TEXT("OPEN") : TEXT("closed"),
			Candidate->WasCharacterChosen() ? TEXT(" chosen") : TEXT(" auto"),
			Candidate->GetActivatedCooldownRemaining());
	}
}

void ATraceGameMode::StartCharacterSelectVerify(bool bRedArm)
{
	CharacterVerifyPassed = 0;
	CharacterVerifyFailed = 0;

	// Scoped, and restored on every exit path below, so a red run cannot leave the rule off for the
	// rest of the session.
	TGuardValue<int32> SelectRulesGuard(GTraceEnforceSelectRules, bRedArm ? 0 : GTraceEnforceSelectRules);

	const AGameStateBase* BaseGameState = GameState;
	if (BaseGameState == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[CharVerify] No GameState; cannot run."));
		return;
	}

	const bool bEnabledNow = AreCharactersEnabled();
	const bool bGoalModeNow = (GetTraceGameState() != nullptr) && GetTraceGameState()->IsGoalMode();

	UE_LOG(LogTraceGame, Display,
		TEXT("[CharVerify] ===== START: mode %s, characters %s, select-slice rules %s ====="),
		bGoalModeNow ? TEXT("B (goals)") : TEXT("A (endzones)"),
		bEnabledNow ? TEXT("ON") : TEXT("OFF"),
		(GTraceEnforceSelectRules != 0) ? TEXT("ENFORCED") : TEXT("*** DISABLED (RED ARM) ***"));

	// ---- Borrow three player states: two on one team, one on the other ------------------------
	ATracePlayerState* TeammateA = nullptr;
	ATracePlayerState* TeammateB = nullptr;
	ATracePlayerState* Opponent  = nullptr;
	ATracePlayerState* RealBot   = nullptr;

	ETraceTeam HomeTeam = ETraceTeam::None;

	for (APlayerState* const EachState : BaseGameState->PlayerArray)
	{
		ATracePlayerState* Candidate = Cast<ATracePlayerState>(EachState);
		if (Candidate == nullptr || Candidate->Team == ETraceTeam::None)
		{
			continue;
		}

		if (TeammateA == nullptr)
		{
			TeammateA = Candidate;
			HomeTeam = Candidate->Team;
			continue;
		}

		if (TeammateB == nullptr && Candidate->Team == HomeTeam)
		{
			TeammateB = Candidate;
			continue;
		}

		if (Opponent == nullptr && Candidate->Team != HomeTeam)
		{
			Opponent = Candidate;
			continue;
		}

		if (RealBot == nullptr && Candidate->IsABot())
		{
			RealBot = Candidate;
		}
	}

	if (TeammateA == nullptr || TeammateB == nullptr || Opponent == nullptr)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[CharVerify] Needs at least two players on one team and one on the other "
			     "(found %s / %s / %s). Run with bots enabled."),
			(TeammateA != nullptr) ? TEXT("A") : TEXT("-"),
			(TeammateB != nullptr) ? TEXT("B") : TEXT("-"),
			(Opponent != nullptr) ? TEXT("enemy") : TEXT("-"));
		return;
	}

	// Snapshot everything the harness is about to disturb, so a mid-match run leaves no trace.
	struct FVerifySnapshot
	{
		ATracePlayerState* State = nullptr;
		bool bWasBot = false;
		uint8 Character = TraceCharacterRoster::NoneId;
		bool bWasLocked = false;
		bool bSelectWasOpen = false;
	};

	TArray<FVerifySnapshot> Snapshots;
	for (ATracePlayerState* const Borrowed : { TeammateA, TeammateB, Opponent })
	{
		FVerifySnapshot Snapshot;
		Snapshot.State = Borrowed;
		Snapshot.bWasBot = Borrowed->IsABot();
		Snapshot.Character = Borrowed->GetSelectedCharacter();
		Snapshot.bWasLocked = Borrowed->bCharacterLocked;
		Snapshot.bSelectWasOpen = Borrowed->IsCharacterSelectOpen();
		Snapshots.Add(Snapshot);

		// Become a person for the duration, with a clean slate and an open screen. SetIsABot FIRST:
		// the framework refuses to give a bot any character at all, so clearing the character before
		// the flag would be refused for the wrong reason.
		Borrowed->SetIsABot(false);
		if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Borrowed))
		{
			Abilities->ServerSetCharacter(ETraceCharacterId::None);
		}
		Borrowed->ServerMarkCharacterResolved(/*bLocked=*/false, /*bWasChosen=*/false);
		Borrowed->ServerSetCharacterSelectOpen(/*bOpen=*/true, /*DeadlineServerTime=*/0.f);
	}

	// The poll would close the screens the harness just opened (a bot with no character is not
	// offered one) and would auto-assign underneath the assertions. Pause it for the run.
	GetWorldTimerManager().ClearTimer(CharacterSelectPollHandle);

	const uint8 Contested = TraceCharacterRoster::FirstId;   // ROCCO

	if (!bEnabledNow)
	{
		// MODE A / TOGGLE OFF is not a skipped run, it is its own assertion — spec v14 §2 and §6 both
		// require that no selection is possible here at all.
		const ETraceCharacterPickResult ModeAResult = RequestCharacter(TeammateA, Contested);
		ReportCharacterVerify(ModeAResult == ETraceCharacterPickResult::Disabled,
			TEXT("with characters off (mode A or the toggle), a pick is REFUSED as Disabled"));
		ReportCharacterVerify(!TeammateA->HasCharacter(),
			TEXT("with characters off, nobody ends up holding a character"));
	}
	else
	{
		// ---- 1. FIRST REQUEST WINS ------------------------------------------------------------
		const ETraceCharacterPickResult FirstResult = RequestCharacter(TeammateA, Contested);
		ReportCharacterVerify(FirstResult == ETraceCharacterPickResult::Granted,
			TEXT("the first request for a free character is GRANTED"));
		ReportCharacterVerify(TeammateA->GetSelectedCharacter() == Contested,
			TEXT("the winner actually holds the character they asked for"));
		ReportCharacterVerify(!TeammateA->IsCharacterSelectOpen(),
			TEXT("the winner's select screen closes on the grant"));

		// ---- 2. THE TEAM-MATE LOSES, AND IS TOLD ----------------------------------------------
		//
		// THIS IS THE ASSERTION THE RED ARM DELETES. With the red arm on
		// it must fail; if it passes with the rule switched off, the rule is not what is producing
		// the refusal and the whole verification is worthless.
		const ETraceCharacterPickResult SecondResult = RequestCharacter(TeammateB, Contested);
		ReportCharacterVerify(SecondResult == ETraceCharacterPickResult::TakenByTeammate,
			TEXT("*** a TEAM-MATE asking for the same character is REFUSED (TakenByTeammate) ***"));
		ReportCharacterVerify(TeammateB->GetSelectedCharacter() != Contested,
			TEXT("*** the loser does not end up holding the contested character ***"));

		// ---- 3. AN ENEMY MAY MIRROR THE PICK ---------------------------------------------------
		//
		// Spec v14 §3: uniqueness is PER TEAM. This is the assertion that stops a future "fix" from
		// making the roster globally unique, which would silently halve the game.
		const ETraceCharacterPickResult EnemyResult = RequestCharacter(Opponent, Contested);
		ReportCharacterVerify(EnemyResult == ETraceCharacterPickResult::Granted,
			TEXT("an ENEMY asking for the same character is GRANTED (mirroring is legal)"));
		ReportCharacterVerify(Opponent->GetSelectedCharacter() == Contested,
			TEXT("the enemy holds the mirrored character"));

		// ---- 4. THE LOSER RE-PICKS SUCCESSFULLY ------------------------------------------------
		const uint8 SecondChoice = TraceCharacterRoster::FirstId + 1;   // CHUT
		const ETraceCharacterPickResult RepickResult = RequestCharacter(TeammateB, SecondChoice);
		ReportCharacterVerify(RepickResult == ETraceCharacterPickResult::Granted,
			TEXT("the loser's second pick, of a free character, is GRANTED"));

		// ---- 5. LOCK-IN IS FINAL ---------------------------------------------------------------
		const ETraceCharacterPickResult RelockResult = RequestCharacter(TeammateA, SecondChoice + 1);
		ReportCharacterVerify(RelockResult == ETraceCharacterPickResult::AlreadyLocked,
			TEXT("a locked-in player cannot switch character"));

		// ---- 6. BOTS -----------------------------------------------------------------------
		if (RealBot != nullptr)
		{
			const ETraceCharacterPickResult BotResult = RequestCharacter(RealBot, TraceCharacterRoster::LastId);
			ReportCharacterVerify(BotResult == ETraceCharacterPickResult::NotSelecting,
				TEXT("a BOT cannot take a character"));
		}

		int32 BotsWithCharacters = 0;
		int32 BotsSeen = 0;
		for (APlayerState* const EachState : BaseGameState->PlayerArray)
		{
			const ATracePlayerState* Candidate = Cast<ATracePlayerState>(EachState);
			if (Candidate != nullptr && Candidate->IsABot())
			{
				++BotsSeen;
				if (Candidate->HasCharacter())
				{
					++BotsWithCharacters;
				}
			}
		}
		ReportCharacterVerify(BotsSeen > 0 && BotsWithCharacters == 0,
			TEXT("every bot in the match is characterless"));

		// ---- 7. THE AUTO-ASSIGN TIMEOUT --------------------------------------------------------
		//
		// Driven by planting a deadline in the PAST and running one poll, rather than by waiting
		// thirty seconds. The code path is identical; only the clock is cheated.
		if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(TeammateA))
		{
			Abilities->ServerSetCharacter(ETraceCharacterId::None);
		}
		TeammateA->ServerMarkCharacterResolved(/*bLocked=*/false, /*bWasChosen=*/false);
		TeammateA->ServerSetCharacterSelectOpen(/*bOpen=*/true,
			static_cast<float>(BaseGameState->GetServerWorldTimeSeconds() - 1.0));

		PollCharacterSelect();

		ReportCharacterVerify(TeammateA->HasCharacter(),
			TEXT("an expired select screen AUTO-ASSIGNS a character"));
		ReportCharacterVerify(!TeammateA->IsCharacterSelectOpen(),
			TEXT("the auto-assign closes the select screen"));
		ReportCharacterVerify(!TeammateA->WasCharacterChosen(),
			TEXT("an auto-assigned character is flagged as NOT chosen"));
		ReportCharacterVerify(
			!IsCharacterTakenOnTeam(TeammateA->Team, TeammateA->GetSelectedCharacter(), TeammateA),
			TEXT("*** the auto-assigned character is one no team-mate already holds ***"));
	}

	// ---- 8. COOLDOWNS (spec v14 §5) -------------------------------------------------------------
	//
	// Included in this harness rather than in one of its own because it is the same fact from another
	// angle: the cooldown, like the character, has to live somewhere that outlives the pawn.
	if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(TeammateA))
	{
		Abilities->DebugSetActivatedCooldown(20.f);
		const float AfterStart = TeammateA->GetActivatedCooldownRemaining();
		ReportCharacterVerify(AfterStart > 19.f && AfterStart <= 20.f,
			TEXT("starting a 20s cooldown leaves ~20s remaining, read through the PLAYER STATE"));

		// "A player can spawn with an ability timer still counting down" — the respawn path itself,
		// not an imitation of it. RestartPlayerFresh destroys the pawn, which is precisely the event
		// that used to be able to take a cooldown with it.
		if (AController* const Respawning = TeammateA->GetOwningController())
		{
			RestartPlayerFresh(Respawning);
		}
		ReportCharacterVerify(TeammateA->GetActivatedCooldownRemaining() > 19.f,
			TEXT("*** a RESPAWN does not reset the activated cooldown ***"));

		ResetAbilityCooldownsForHalfTime();
		ReportCharacterVerify(TeammateA->GetActivatedCooldownRemaining() <= 0.f,
			TEXT("*** HALF TIME resets the activated cooldown ***"));
		ReportCharacterVerify(TeammateA->IsActivatedAbilityReady(),
			TEXT("the ability reads as ready after half time"));
	}
	else
	{
		ReportCharacterVerify(false, TEXT("the borrowed player has an ability component to test with"));
	}

	// ---- Restore ----------------------------------------------------------------------------
	for (const FVerifySnapshot& Snapshot : Snapshots)
	{
		if (Snapshot.State == nullptr)
		{
			continue;
		}

		if (UTraceAbilityComponent* Abilities = UTraceAbilityComponent::Get(Snapshot.State))
		{
			Abilities->ServerSetCharacter(static_cast<ETraceCharacterId>(Snapshot.Character));
		}
		Snapshot.State->ServerMarkCharacterResolved(Snapshot.bWasLocked, /*bWasChosen=*/false);
		Snapshot.State->ServerSetCharacterSelectOpen(Snapshot.bSelectWasOpen, 0.f);

		// The cooldown is deliberately NOT restored: the harness ends by proving half time clears
		// every cooldown, and it did that by actually clearing them. Putting a stale deadline back
		// would undo the one assertion whose evidence is the live state.
		Snapshot.State->SetIsABot(Snapshot.bWasBot);
	}

	GetWorldTimerManager().SetTimer(CharacterSelectPollHandle, this, &ATraceGameMode::PollCharacterSelect,
		TraceGameModeConstants::CharacterSelectPollInterval, /*bLoop=*/true,
		TraceGameModeConstants::CharacterSelectPollInterval);

	UE_LOG(LogTraceGame, Display, TEXT("[CharVerify] ===== %d passed, %d failed ====="),
		CharacterVerifyPassed, CharacterVerifyFailed);
}

namespace
{
	void TraceCharactersVerifyCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}

		const bool bRedArm = (Args.Num() > 0) && Args[0].TrimStartAndEnd().Equals(TEXT("red"), ESearchCase::IgnoreCase);

		if (ATraceGameMode* Rules = World->GetAuthGameMode<ATraceGameMode>())
		{
			Rules->StartCharacterSelectVerify(bRedArm);
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CharVerify] No authoritative ATraceGameMode here - run this on the server."));
		}
	}

	void TraceCharactersDumpCommand(UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}

		if (const ATraceGameMode* Rules = World->GetAuthGameMode<ATraceGameMode>())
		{
			Rules->DumpCharacterState();
		}
		else
		{
			UE_LOG(LogTraceGame, Warning,
				TEXT("[CharDump] No authoritative ATraceGameMode here - run this on the server."));
		}
	}

	// NOTE: neither of these may share a name with a CVar. Trace.Characters.EnforceSelectRules is
	// a CVar and these two are COMMANDS; a collision on the same string is a fatal error at module
	// load, which this project has hit before.
	FAutoConsoleCommandWithWorldAndArgs CmdTraceCharactersVerify(
		TEXT("Trace.Characters.Verify"),
		TEXT("Dev only. Spec v14 3. Proves first-request-wins, that a team-mate is refused, that an "
		     "ENEMY may mirror the pick, that bots stay characterless, that the timeout auto-assigns "
		     "a free character, and that a respawn does not reset an ability cooldown while half time "
		     "does. Argument 'red' removes this slice's uniqueness test for the run, and the team-mate "
		     "assertion must then FAIL - that is how the harness is shown to be able to go red."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&TraceCharactersVerifyCommand));

	/**
	 * The red arm as its own COMMAND rather than only as an argument.
	 *
	 * Not redundancy. The headless harness delivers commands through -TraceExec, whose values are read
	 * with FParse::Value and therefore end at the first space — so "Trace.Characters.Verify red" is
	 * unreachable from a command line without quoting, and this project's own contract notes that a
	 * quoted argument has already once broken a command line into the URL parser and made a
	 * verification "pass" because its commands never ran. A second, space-free name removes the
	 * quoting from the problem entirely.
	 */
	void TraceCharactersVerifyRedCommand(UWorld* World)
	{
		if (World != nullptr)
		{
			if (ATraceGameMode* Rules = World->GetAuthGameMode<ATraceGameMode>())
			{
				Rules->StartCharacterSelectVerify(/*bRedArm=*/true);
				return;
			}
		}

		UE_LOG(LogTraceGame, Warning,
			TEXT("[CharVerify] No authoritative ATraceGameMode here - run this on the server."));
	}

	FAutoConsoleCommandWithWorld CmdTraceCharactersVerifyRed(
		TEXT("Trace.Characters.VerifyRed"),
		TEXT("Dev only. Trace.Characters.Verify with the red arm on, as one space-free token so it can "
		     "be delivered by -TraceExec. The team-mate assertion MUST fail."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&TraceCharactersVerifyRedCommand));

	FAutoConsoleCommandWithWorld CmdTraceCharactersDump(
		TEXT("Trace.Characters.Dump"),
		TEXT("Dev only. Every player: team, human/bot, character, select state and ability cooldown."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&TraceCharactersDumpCommand));
}

#endif // !UE_BUILD_SHIPPING
