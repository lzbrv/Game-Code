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
#include "Misc/CommandLine.h"                            // -TraceBotDebug
#include "Misc/Parse.h"
#include "TimerManager.h"

#include "AI/TraceBotController.h"
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

	// A wipe can be the point that wins the match, but only if the mercy rule is switched on.
	if (bEndMatchAtScoreToWin)
	{
		const int32 ScoreToWin = FMath::Max(1, UTraceSettings::Get().ScoreToWin);
		if (TraceGameState->GetScore(BonusTeam) >= ScoreToWin)
		{
			FinishMatch(BonusTeam);
		}
	}
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

	// The mercy rule, if it is switched on at all, is decided before anything is restarted: there is
	// no point kicking off into a match that has just ended.
	if (bCounts && bEndMatchAtScoreToWin)
	{
		const int32 ScoreToWin = FMath::Max(1, UTraceSettings::Get().ScoreToWin);
		if (TraceGameState->GetScore(ScoringTeam) >= ScoreToWin)
		{
			// Still put everyone back on their pads: the results screen looks past the players, and
			// leaving ten pawns piled in one endzone behind it reads as the game having frozen.
			ResetPlayersToSpawns();
			FinishMatch(ScoringTeam);
			return;
		}
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

			ATraceCharacter* Tripper = nullptr;
			for (const TWeakObjectPtr<ATraceCharacter>& Weak : TrackedCharacters)
			{
				ATraceCharacter* Candidate = Weak.Get();
				if (Candidate != nullptr && Candidate != TraceOwner && Candidate->IsAlive()
					&& Candidate->GetTeam() != ETraceTeam::None && Candidate->GetTeam() != TraceOwner->GetTeam())
				{
					Tripper = Candidate;
					break;
				}
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

			UE_LOG(LogTraceGame, Display,
				TEXT("[TRIPTEST %d] %s (enemy of %s) starts a dash at %s, aimed across segment %d of the residual trace at %s. TraceOwner emitting=%d."),
				VerifyIteration + 1, *GetNameSafe(Tripper), *GetNameSafe(TraceOwner), *DashStart.ToCompactString(),
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
	const ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::InProgress)
	{
		return;
	}

	if (TraceGameState->CurrentHalf < FMath::Max(1, HalvesPerMatch))
	{
		BeginHalfTimeBreak();
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

void ATraceGameMode::BeginHalfTimeBreak()
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState != ETraceMatchState::InProgress
		|| TraceGameState->IsHalfTimeBreak())
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(MatchTimerHandle);

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

void ATraceGameMode::FinishMatch(ETraceTeam WinningTeam)
{
	ATraceGameState* TraceGameState = GetTraceGameState();
	if (TraceGameState == nullptr || TraceGameState->TraceMatchState == ETraceMatchState::PostMatch)
	{
		return;
	}

	// Every clock this class owns stops here, including the interval — a FinishMatch triggered early
	// (a mercy-rule win, a forfeit) during half time must not have the second half start underneath
	// the results screen. That would be the restart loop the contract forbids, wearing a new hat.
	GetWorldTimerManager().ClearTimer(WarmupTimerHandle);
	GetWorldTimerManager().ClearTimer(MatchTimerHandle);
	GetWorldTimerManager().ClearTimer(HalfTimeTimerHandle);

	TraceGameState->TraceMatchState = ETraceMatchState::PostMatch;
	TraceGameState->SetHalfState(TraceGameState->CurrentHalf, FMath::Max(1, HalvesPerMatch), /*bInHalfTimeBreak=*/false);
	TraceGameState->MatchEndServerTime = static_cast<float>(TraceGameState->GetServerWorldTimeSeconds());
	TraceGameState->ForceNetUpdate();

	// The objective leaves the field with the whistle: nobody should be able to "score" into the
	// results screen, and the reset that follows a goal would fight the post-match state.
	ReleaseCore();

	// Players keep their pawns and keep respawning after the whistle — the HUD switches to the FINAL
	// banner, and leaving everyone alive means nobody is staring at a corpse on the results screen.
	UE_LOG(LogTraceGame, Log, TEXT("Match over. Winner: %s (Blue %d - Orange %d)"),
		(WinningTeam == ETraceTeam::None) ? TEXT("draw") : *TraceTeamName(WinningTeam).ToString(),
		TraceGameState->BlueScore, TraceGameState->OrangeScore);

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

	// Absolute travel: the menu runs a different game mode on a different map, and a relative
	// travel would carry this match's URL options (?difficulty=, ?bots=) into it.
	UGameplayStatics::OpenLevel(World, FName(TraceMaps::MainMenu), /*bAbsolute=*/true);
}
