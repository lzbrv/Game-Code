// Trace — the practice range's game mode (spec v19 §2). See TracePracticeGameMode.h.

#include "Modes/TracePracticeGameMode.h"

#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "HAL/IConsoleManager.h"                // Trace.Practice.StartCountdown — demo 27's red arm
#include "GameFramework/PlayerController.h"     // ChoosePlayerStart's human test — demo 19 item 1
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

#if !UE_BUILD_SHIPPING
	/**
	 * *** DEMO 27's RED ARM, and it is one argument-free line. *** 1 gives the range the MATCH's
	 * warm-up back and changes nothing else, so the build that produced the complaint is available
	 * from the shipped binary: the range sits in WaitingForPlayers for five seconds with "MATCH
	 * STARTS IN n" counting down over the target row.
	 *
	 * Trace.Practice.Verify, unchanged, must then FAIL — and fail on its two demo-27 rows ONLY.
	 * The harness's expected side deliberately cannot see this switch: it compares the live mode's
	 * answer against GetDefault<ATraceGameMode>()'s, which is the base implementation and is not
	 * routed through this override at all. If both sides could see the arm they would move together
	 * and print PASS over a countdown, which is the exact identity failure this project has shipped
	 * before.
	 */
	TAutoConsoleVariable<int32> CVarPracticeStartCountdown(
		TEXT("Trace.Practice.StartCountdown"),
		0,
		TEXT("DEV ONLY. RED ARM for demo 27's 'no match start timer in the practice range'. 1 gives "
		     "the range the match's UTraceSettings::WarmupDuration countdown back, so the range waits "
		     "and draws MATCH STARTS IN again. 0 (default) is the shipped range: no countdown, live "
		     "the moment the targets are standing."),
		ECVF_Cheat);
#endif
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

	// GetHalfSeconds(), NOT HalfDuration. The property still reads 480 here because config sections
	// are inherited and DefaultGame.ini sets the match's half on the parent class; the mode ignores
	// it. Printing the property was how "One 480 s half" ended up in this log for a range that has
	// no match clock at all - an instrument saying the opposite of what the frame does, which is the
	// disagreement this project keeps paying for. Print what the clock is actually built from.
	UE_LOG(LogTraceGame, Display,
		TEXT("[Practice] PRACTICE RANGE game mode on '%s'. One %.0f s half (the property says "
		     "%.0f s and is overridden), no wipe bonus, no scoreboard, stationary targets only. "
		     "Everything range-only is gated on TracePracticeRange::IsActive()."),
		*MapName, GetHalfSeconds(), HalfDuration);
}

void ATracePracticeGameMode::BeginPlay()
{
	Super::BeginPlay();

	GetWorldTimerManager().SetTimer(
		PracticeStartHandle, this, &ATracePracticeGameMode::PokeMatchStart,
		TracePracticeGameModeLocal::MatchStartPokeSeconds, /*bLoop=*/true);
}

float ATracePracticeGameMode::GetWarmupSeconds() const
{
	// *** DEMO 27: "Don't have a match start timer in the practice range." ***
	//
	// Zero is not "a very short countdown": ATraceGameMode::StartWarmup treats it as the no-countdown
	// case and calls BeginMatch inside the same call, so the pair the HUD banner needs
	// (WaitingForPlayers with a deadline published) is never true on any tick. Measured before the
	// change: warm-up started 0.8 s after the range opened and the whistle went 5.6 s later, and the
	// captured frame reads "MATCH STARTS IN / 2" over the target row with "WARM UP" and 00:02 in the
	// header. After: no warm-up line in the log at all and the whistle 0.8 s after the range opened,
	// which is the first poke that finds the targets standing.
	//
	// The whistle itself is emphatically NOT skipped — it is what grants the Core, applies the sides
	// and stands everybody on their spawns — it simply happens on the frame the range is furnished
	// rather than five seconds after it.
#if !UE_BUILD_SHIPPING
	if (TracePracticeGameModeLocal::CVarPracticeStartCountdown.GetValueOnGameThread() != 0)
	{
		// THE RED ARM, and this is its only effect: the match's own answer, taken from the base
		// class rather than re-read from the settings, so the arm cannot drift from what a match does.
		return Super::GetWarmupSeconds();
	}
#endif

	return 0.f;
}

float ATracePracticeGameMode::GetHalfSeconds() const
{
	// *** THE RANGE HAS NO MATCH CLOCK, AND THE CONSTRUCTOR ALONE COULD NOT DELIVER THAT. ***
	//
	// The constructor already sets HalfDuration to PracticeHalfSeconds, and it was silently undone:
	// UE config sections are INHERITED, so `[/Script/Trace.TraceGameMode] HalfDuration=480` in
	// Config/DefaultGame.ini lands on this subclass too. The range therefore ran a 480 s half - it
	// said so itself, "One 480 s half", while the HUD drew a counting-down match clock - and it
	// would have ended after eight minutes of practice. Demo 27 asked for no match-start timer in
	// the range; a match clock that expires is the same complaint wearing a different hat.
	//
	// Returned from the mode rather than from the property because an ini can overwrite a property
	// and cannot overwrite a virtual. Same argument, same seam, as GetWarmupSeconds() above.
	return TracePracticeGameModeLocal::PracticeHalfSeconds;
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

		// *** DEMO 19 ITEM 1, "make it smaller" — the half of it that is about DYING. ***
		//
		// Without this a human takes the shipped endzone spawn, which is mid-endzone and about
		// 15600 uu from the range's target row: correct for a real match, and in the range it means
		// every death costs a 200 m walk back to the thing you were practising. The range's own spawn
		// line is a start spot like any other, so this is the same one-line answer to "where?" that
		// the targets already get, and the rest of the shipped respawn pipeline is untouched.
		//
		// Humans only: FindRespawnPostFor has already claimed every dummy above, and a null here
		// (the range is not furnished yet) falls through to the shipped pipeline rather than
		// refusing to spawn anybody.
		if (Player != nullptr && Player->IsA<APlayerController>())
		{
			if (AActor* StartPost = Range->GetPlayerStartPost())
			{
				return StartPost;
			}
		}
	}

	return Super::ChoosePlayerStart_Implementation(Player);
}
