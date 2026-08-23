// Trace — the practice range's game mode (spec v19 §2).

#pragma once

#include "CoreMinimal.h"

#include "Core/TraceGameMode.h"

#include "TracePracticeGameMode.generated.h"

class AActor;
class AController;

/**
 * THE RANGE. Open it with:
 *
 *     open /Game/Maps/Arena_Baked?game=/Script/Trace.TracePracticeGameMode
 *
 * or from the shell with Scripts/run-practice-range.sh.
 *
 * This class is deliberately thin. It exists to be a TYPE — TracePracticeRange::IsActive() asks
 * "is the authoritative game mode one of these?", and that question having exactly one answer is the
 * whole reason the range's cheats cannot reach a real match. Everything the range actually DOES
 * lives in UTracePracticeRangeSubsystem; see the block at the top of TracePracticeRange.h for why
 * that separation is what makes the leak test able to go red.
 *
 * What it changes about a match, and why each one:
 *
 *   NO ROAMING BOTS. `?bots=0` is folded into the travel URL unless the caller asked for bots
 *   explicitly. The range's targets are ATracePracticeDummyControllers that stand still; a squad of
 *   ATraceBotControllers hunting you across the arena is the opposite of a place to test movement.
 *
 *   NO MATCH-START COUNTDOWN. GetWarmupSeconds() returns 0, so the range is LIVE THE INSTANT it can
 *   be — demo 27, verbatim: "Don't have a match start timer in the practice range." A countdown to
 *   a start that means nothing here is noise, and it was five seconds of "MATCH STARTS IN 3" over
 *   the target row. Live-immediately rather than merely not-drawn, because the warm-up is not only
 *   a banner: play is genuinely held until the whistle (BeginMatch is what grants the Core, applies
 *   the sides and puts everybody on their spawns), so hiding the number would have left the range
 *   with five seconds of nothing happening for no stated reason.
 *
 *   NO WIPE BONUS. WipeBonusPoints = 0. Without it, a solo player's every death pays the empty enemy
 *   side two points (ATraceGameMode::EvaluateWipeBonus counts a one-player team with nobody alive as
 *   a wipe), the lead reaches UTraceSettings::MercyRuleLead after four deaths, and the practice
 *   session ends itself and travels to the main menu. That was measured, not guessed.
 *
 *   ONE ENORMOUS HALF. HalvesPerMatch = 1 and HalfDuration is a fortnight, so the clock never
 *   expires, there is no half-time break to freeze your abilities in, and no side switch moves the
 *   dummies' end of the field out from under them.
 *
 *   RESPAWN IN PLACE. ChoosePlayerStart returns the dummy's own post. That is the ENTIRE
 *   implementation of the spec's "respawn in place a short time after dying": the shipped death →
 *   RespawnDelay → RestartPlayerFresh → ChoosePlayerStart pipeline runs exactly as it does for a
 *   human, and only the answer to "where?" changes. A second respawn path racing the first is the
 *   bug this avoids.
 */
UCLASS()
class TRACE_API ATracePracticeGameMode : public ATraceGameMode
{
	GENERATED_BODY()

public:
	ATracePracticeGameMode();

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;

	/**
	 * A practice dummy respawns on its own post; everybody else takes the shipped answer.
	 *
	 * ShouldSpawnAtStartSpot() already returns false on ATraceGameMode, so this runs on EVERY
	 * restart rather than only the first — which is what makes it a respawn rule and not a spawn
	 * rule.
	 */
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

	/**
	 * ZERO. The range does not count down to anything.
	 *
	 * ATraceGameMode::StartWarmup reads this the moment the start condition is first met, sees zero
	 * and calls BeginMatch in the same call — so the range goes live on the frame its targets
	 * appear, and the HUD banner that needs (WaitingForPlayers && MatchEndServerTime > 0) never has
	 * a frame in which both are true. Nothing is skipped: the whistle still runs in full, which is
	 * how the Core gets granted, the sides get applied and everybody gets stood on their spawns.
	 *
	 * A real match is untouched — this override is the only caller-visible difference, and
	 * UTraceSettings::WarmupDuration is still what every other mode returns.
	 *
	 * RED ARM: Trace.Practice.StartCountdown 1 hands the match's warm-up back to the range and does
	 * nothing else, and Trace.Practice.Verify then fails its two demo-27 rows and only those.
	 */
	virtual float GetWarmupSeconds() const override;

	virtual float GetHalfSeconds() const override;

protected:
	/**
	 * The range's targets have PlayerStates and therefore count towards
	 * UTraceSettings::MinPlayersToStart — but they are spawned by the subsystem AFTER BeginPlay has
	 * already asked whether the match can start, and nothing else re-asks. This timer re-asks once
	 * the range has been furnished, which is the difference between a range that plays and a range
	 * that sits in WaitingForPlayers with the weapon disabled.
	 */
	void PokeMatchStart();

	FTimerHandle PracticeStartHandle;
};
