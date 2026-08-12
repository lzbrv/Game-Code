// Trace — the practice range's game mode (spec v19 §2). See TracePracticeGameMode.h.

#include "Modes/TracePracticeGameMode.h"

#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"             // UGameplayStatics::HasOption
#include "TimerManager.h"

#include "Modes/TracePracticeRange.h"
#include "Trace.h"                              // LogTraceGame

namespace TracePracticeGameModeLocal
{
	/**
	 * One half, and it is a fortnight long.
	 *
	 * The clock is the only thing left that can end a practice session — the wipe bonus is off and
	 * UTracePracticeRangeSubsystem::SuppressScore keeps the scoreboard at 0-0 so the mercy rule can
	 * never fire either. A number this size is not a hack around a rule, it is the honest statement
	 * that the range has no period structure.
	 */
	constexpr float PracticeHalfSeconds = 1209600.f;   // 14 days

	/** Seconds after BeginPlay before the match-start condition is re-asked. See PokeMatchStart. */
	constexpr float MatchStartPokeSeconds = 1.0f;
}

ATracePracticeGameMode::ATracePracticeGameMode()
{
	// ONE ENORMOUS HALF. Half time would clear every cooldown, freeze abilities for the interval and
	// swap ends under the target row; full time would eject you to the main menu mid-session.
	HalvesPerMatch = 1;
	HalfDuration = TracePracticeGameModeLocal::PracticeHalfSeconds;
	HalfTimeBreakDuration = 0.f;

	// NO WIPE BONUS, AND THIS ONE IS LOAD-BEARING RATHER THAN TIDY. A solo player is the only member
	// of their team, so ATraceGameMode::EvaluateWipeBonus reads every one of their deaths as a team
	// wipe and pays the empty enemy side two points. Four deaths reach UTraceSettings::MercyRuleLead
	// and the range ends itself and travels to the title screen — i.e. dying while practising would
	// eject you from the practice range.
	WipeBonusPoints = 0;

	// No auto-assign timer nagging a player who is standing on the CHANGE CHARACTER pad reading the
	// cards. The range has no match to stall, so the one reason the timeout exists does not apply.
	CharacterSelectTimeout = 0.f;
}

void ATracePracticeGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	// NO ROAMING BOTS UNLESS THE CALLER ASKED FOR THEM. The range's targets stand still on purpose;
	// a squad of ATraceBotControllers hunting you across the arena is the opposite of a place to
	// test movement. Folded into the URL rather than set on a member because BotCountFromURL is
	// ATraceGameMode's private business and Super::InitGame is the one place it is parsed —
	// "?bots=5" on the command line therefore still wins, which is what you want when you
	// deliberately came here to practise against live AI.
	FString PracticeOptions = Options;
	if (!UGameplayStatics::HasOption(PracticeOptions, TEXT("bots")))
	{
		PracticeOptions += TEXT("?bots=0");
	}

	Super::InitGame(MapName, PracticeOptions, ErrorMessage);

	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] PRACTICE RANGE game mode on '%s'. One %.0f s half, no wipe bonus, "
		     "no scoreboard, stationary targets only. Everything range-only is gated on "
		     "TracePracticeRange::IsActive()."),
		*MapName, HalfDuration);
}

void ATracePracticeGameMode::BeginPlay()
{
	Super::BeginPlay();

	GetWorldTimerManager().SetTimer(
		PracticeStartHandle, this, &ATracePracticeGameMode::PokeMatchStart,
		TracePracticeGameModeLocal::MatchStartPokeSeconds, /*bLoop=*/true);
}

void ATracePracticeGameMode::PokeMatchStart()
{
	// The targets carry PlayerStates, so once they exist GetActivePlayerCount() clears
	// UTraceSettings::MinPlayersToStart. But they are spawned by the subsystem AFTER BeginPlay has
	// already asked, and nothing else re-asks — leaving the range in WaitingForPlayers with the
	// weapon and every ability refused, which reads as "the practice range is broken".
	//
	// Idempotent: CheckMatchStartConditions returns immediately once the phase has moved on.
	CheckMatchStartConditions();

	const UTracePracticeRangeSubsystem* const Range = UTracePracticeRangeSubsystem::Get(GetWorld());
	if (Range != nullptr && Range->IsBuilt() && Range->GetDummyCount() > 0)
	{
		// The range is furnished and the question has been asked with the targets present. Nothing
		// further to poke; the phase machine takes it from here.
		GetWorldTimerManager().ClearTimer(PracticeStartHandle);
	}
}

AActor* ATracePracticeGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	// *** "RESPAWN IN PLACE", IN FULL. *** Spec v19 §2: "Make sure they respawn in place a short time
	// after dying."
	//
	// There is no respawn code in this feature. The shipped pipeline — ATraceCharacter dies,
	// ATraceGameMode::NotifyCharacterDied schedules RespawnDelay, RespawnController calls
	// RestartPlayerFresh, which calls RestartPlayer, which calls this — runs for a target exactly as
	// it does for a human, and the ONLY thing that differs is the answer to "where?". That is
	// deliberate: a second respawn path racing the first is how a dummy ends up double-spawned, and
	// reusing this one also means the range automatically honours RespawnDelay, the pending-respawn
	// map's one-timer-per-controller rule and the death panel.
	if (const UTracePracticeRangeSubsystem* Range = UTracePracticeRangeSubsystem::Get(GetWorld()))
	{
		if (AActor* Post = Range->FindRespawnPostFor(Player))
		{
			return Post;
		}
	}

	return Super::ChoosePlayerStart_Implementation(Player);
}
