// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "Core/TraceMatchTypes.h"       // ETraceMatchEndReason
#include "TraceTypes.h"                 // ETraceTeam, ETraceMatchState

#include "TraceGameState.generated.h"

class ATraceCore;

/**
 * Declared in TraceSettings.h. Forward declared (legal: the underlying type is fixed) so this
 * header stays free of the 110 kB settings header — exactly as TraceCore.h does, and for the same
 * reason: most includers of this file only ever want IsGoalMode().
 */
enum class ETraceScoringMode : uint8;

/**
 * Broadcast on EVERY machine — server and clients alike — when the active scoring mode changes.
 *
 * Systems whose WORLD has to change with the mode (the arena builder swapping endzones for goals,
 * the Core swapping between a status and a physical body) subscribe to this rather than polling,
 * because the change has to land on clients too and clients only learn about it through
 * replication. Systems that merely BRANCH on the mode per frame should not subscribe at all: call
 * ATraceGameState::GetScoringMode() at the point of use and stay correct for free.
 *
 * Declared at file scope rather than inside the UCLASS so UnrealHeaderTool never has to parse a
 * delegate macro inside a generated body.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FTraceScoringModeChanged, ETraceScoringMode /*NewMode*/);

/**
 * The replicated half of the match: scores, phase, the shared deadline and a handle to the one Core.
 *
 * Written only by ATraceGameMode (server). Everything else — HUD, endzones, characters, weapons —
 * reads it, on both server and clients.
 *
 * The clock is the inherited AGameStateBase::GetServerWorldTimeSeconds(), which the engine already
 * replicates and smooths (contract §7: do not hand-roll an RTT clock). The match deadline is stored
 * as an *absolute* server timestamp rather than a ticking countdown, so a client can render a smooth
 * clock from one replicated float and no per-second RPC traffic. That also means a late joiner is
 * immediately correct: they receive the deadline, not a snapshot of "time left".
 */
UCLASS()
class TRACE_API ATraceGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	ATraceGameState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(ReplicatedUsing = OnRep_Scores)
	int32 BlueScore = 0;

	UPROPERTY(ReplicatedUsing = OnRep_Scores)
	int32 OrangeScore = 0;

	UPROPERTY(Replicated)
	ETraceMatchState TraceMatchState = ETraceMatchState::WaitingForPlayers;

	/**
	 * Absolute server time (GetServerWorldTimeSeconds) at which the current phase ends:
	 * the warm-up deadline while WaitingForPlayers, the deadline of the half being played while
	 * InProgress, the end of the interval while bHalfTimeBreak, and the moment the match ended once
	 * PostMatch. Zero means "no deadline running".
	 */
	UPROPERTY(Replicated)
	float MatchEndServerTime = 0.f;

	/** The single contested Core. Null until the GameMode has spawned it and it has replicated. */
	UPROPERTY(Replicated)
	TObjectPtr<ATraceCore> Core = nullptr;

	// ------------------------------------------------------------------------------------------
	// Halves and sides
	//
	// A match is HalvesPerMatch periods of play with the teams switching ends between them, so
	// "which end does Blue defend" is a RUNTIME property, not the constant it used to be. Nothing
	// anywhere may derive a side from a team identity any more; ask GetDefendedEndSign() /
	// GetAttackEndSign() instead. See ATraceGameMode::ApplyTeamSides for the one writer.
	// ------------------------------------------------------------------------------------------

	/** 1-based index of the half being played. 1 until the interval, then 2. */
	UPROPERTY(ReplicatedUsing = OnRep_HalfChanged)
	int32 CurrentHalf = 1;

	/** Mirror of ATraceGameMode::HalvesPerMatch, so a client can render "1 / 2" without the mode. */
	UPROPERTY(Replicated)
	int32 NumHalves = 2;

	/**
	 * True during the interval between halves: the clock is stopped, sides have already been
	 * switched, nothing scores and nobody carries the Core. TraceMatchState stays InProgress on
	 * purpose — the match has not ended, and every "am I playing?" test in the codebase keys off
	 * that enum. Anything that must actually pause asks IsHalfTimeBreak().
	 */
	UPROPERTY(ReplicatedUsing = OnRep_HalfChanged)
	bool bHalfTimeBreak = false;

	// ------------------------------------------------------------------------------------------
	// DEFERRED HALF TIME / FULL TIME (spec v9 §11)
	//
	// Verbatim: "Change halftime to trigger after the time runs out and the current play ends, so
	// that it doesn't cut people off in the middle of a run."
	//
	// The period's clock expiring no longer ENDS the period. It arms it. Play carries on, the clock
	// reads 0:00, and the whistle goes at the next DEAD BALL — a goal, a turnover between the two
	// teams, or (mode B) the Core coming to rest on the ground. ATraceGameMode owns the state
	// machine (BeginPendingPeriodEnd / PollPendingPeriodEnd / ResolvePendingPeriodEnd); this pair of
	// properties is the replicated half, and it exists so the HUD can say so — a clock frozen at
	// 0:00 with no explanation reads as a broken timer, which is a worse bug than the one §11 fixes.
	//
	// NOT a match state and NOT a half-time break: everything is still live. bHalfTimeBreak stays
	// false until the whistle actually goes, so every "is play running?" test in the codebase keeps
	// its existing answer for free.
	// ------------------------------------------------------------------------------------------

	/**
	 * True from the moment the period's clock expires until the deferred half/full time fires.
	 *
	 * Shares OnRep_HalfChanged with the rest of the half block: it changes at most twice a period
	 * and the callback is a log line, so a separate one would only be a second place to look.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_HalfChanged)
	bool bPendingPeriodEnd = false;

	/**
	 * Absolute server time (same clock as MatchEndServerTime) at which the hard cap gives up waiting
	 * for a dead ball and ends the period anyway. Zero when nothing is pending, or when the cap is
	 * disabled (UTraceSettings::PeriodEndMaxDeferSeconds = 0).
	 *
	 * Replicated so the HUD can count the guard rail down rather than leaving players guessing how
	 * long "next dead ball" might be.
	 */
	UPROPERTY(Replicated)
	float PendingPeriodEndCapServerTime = 0.f;

	/**
	 * The team defending the negative-X end of the field for the current half.
	 *
	 * Blue in the first half (which is how the arena is painted), the other team in the second.
	 * This single value IS the side assignment; everything else derives from it.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_SidesChanged)
	ETraceTeam TeamOnNegativeSide = ETraceTeam::Blue;

	/** Team that just earned the wipe bonus, for the HUD flash. None before the first one. */
	UPROPERTY(ReplicatedUsing = OnRep_WipeBonus)
	ETraceTeam LastWipeBonusTeam = ETraceTeam::None;

	/** Server time of that bonus, so the HUD can fade the flash out. */
	UPROPERTY(Replicated)
	float LastWipeBonusServerTime = 0.f;

	// ------------------------------------------------------------------------------------------
	// Scoring mode (spec v4 §7) — THE match-wide answer to "which game is this?"
	//
	// Written only by ATraceGameMode::PublishScoringMode(), which resolves it from the travel URL /
	// command line / UTraceSettings and then pushes the SAME value here and into ATraceCore. Read it
	// through GetScoringMode() / IsGoalMode().
	//
	// WHY IT IS ALSO HERE AND NOT ONLY ON ATraceCore. The Core latches the mode too (it has to: the
	// mode decides what the Core *is*), but the Core is a gameplay actor that is null before
	// PreInitializeComponents has run and on any client that has not received it yet — and the HUD
	// has to tell the player which mode they are in from the first drawn frame, including during
	// warm-up. The GameState is always there and always relevant, so this is the copy that UI, bots
	// and anything else asks. ATraceGameMode is the single writer of both, which is what keeps them
	// from ever disagreeing.
	//
	// Stored as a uint8 rather than the enum for the header-weight reason above; the accessors below
	// hand back the real type.
	// ------------------------------------------------------------------------------------------

	/** ETraceScoringMode as a uint8. 0 is mode A (endzones), which is the default. */
	UPROPERTY(ReplicatedUsing = OnRep_ScoringModeChanged)
	uint8 ReplicatedScoringMode = 0;

	// ------------------------------------------------------------------------------------------
	// Result (spec v4 §6)
	//
	// The results screen used to re-derive the winner from the two scores, which was fine while the
	// only way to end a match was the clock. The mercy rule adds a second way, and "who won" and
	// "why it stopped" are now two different facts — a 9-1 mercy win and a 9-1 full-time win are
	// the same scoreboard and a different result. Both are published here at the whistle.
	// ------------------------------------------------------------------------------------------

	/** Why the match ended. NotEnded until FinishMatch runs. */
	UPROPERTY(Replicated)
	ETraceMatchEndReason MatchEndReason = ETraceMatchEndReason::NotEnded;

	/** Who won. ETraceTeam::None is a genuine draw, and only ever happens on a clock finish. */
	UPROPERTY(Replicated)
	ETraceTeam MatchWinner = ETraceTeam::None;

	/** Fires on every machine when ScoringMode changes. See FTraceScoringModeChanged. */
	FTraceScoringModeChanged OnScoringModeChanged;

	int32 GetScore(ETraceTeam Team) const;

	/** Server only. Negative amounts are allowed but a team score never goes below zero. */
	void AddScore(ETraceTeam Team, int32 Amount);

	/** Seconds until MatchEndServerTime, clamped at zero. Valid on clients and on the server. */
	float GetMatchTimeRemaining() const;

	/** Non-spectator players currently assigned to Team. Drives GameMode team balancing. */
	int32 CountTeamMembers(ETraceTeam Team) const;

	// ------------------------------------------------------------------------------------------
	// Side queries. THE authority on "which way is forward" for this team, this half.
	//
	// Signs are along X in world space, matching ATraceArenaBuilder's layout (the field runs goal
	// to goal along X). -1 is the end the arena paints blue, +1 the end it paints orange.
	// ------------------------------------------------------------------------------------------

	/** X sign of the end @p Team DEFENDS this half. 0 for ETraceTeam::None. */
	float GetDefendedEndSign(ETraceTeam Team) const;

	/** X sign of the end @p Team ATTACKS this half, i.e. where it scores. 0 for None. */
	float GetAttackEndSign(ETraceTeam Team) const;

	/** Team defending the end at @p XSign (any non-zero value; the sign is all that is read). */
	ETraceTeam GetTeamDefendingEnd(float XSign) const;

	/** Team attacking (scoring in) the end at @p XSign. */
	ETraceTeam GetTeamAttackingEnd(float XSign) const;

	/** True once the teams have swapped away from the arena's painted arrangement. */
	bool AreSidesSwapped() const;

	bool IsHalfTimeBreak() const;
	int32 GetCurrentHalf() const;

	/** "1ST HALF" / "2ND HALF" / "HALF TIME" — for the HUD, banners and logs. */
	FString GetHalfLabel() const;

	// ------------------------------------------------------------------------------------------
	// Deferred half time / full time (spec v9 §11) — the HUD reads these three
	// ------------------------------------------------------------------------------------------

	/**
	 * True while the clock has expired but play continues until the next dead ball.
	 *
	 * THE HUD MUST DRAW SOMETHING WHEN THIS IS TRUE. The clock is at 0:00 and still running the
	 * match; without a note on screen that is indistinguishable from a timer that has hung. Draw
	 * GetPendingPeriodEndLabel() next to (or under) the clock — see that function.
	 */
	bool IsPendingPeriodEnd() const;

	/**
	 * "HALF ENDS AT NEXT DEAD BALL" / "MATCH ENDS AT NEXT DEAD BALL", or an empty string when
	 * nothing is pending. One spelling, shared by the HUD, the banners and the logs.
	 */
	FString GetPendingPeriodEndLabel() const;

	/**
	 * Seconds until the hard cap ends the period regardless of whether a dead ball arrived, clamped
	 * at zero. Returns 0 when nothing is pending or the cap is disabled — so a HUD that wants to
	 * show it should test IsPendingPeriodEnd() first and treat 0 as "no cap running".
	 */
	float GetPendingPeriodEndTimeRemaining() const;

	/** Server only. Arms or disarms the deferred whistle. @p InCapServerTime 0 = no hard cap. */
	void SetPendingPeriodEnd(bool bInPending, float InCapServerTime);

	/** Server only. Sets the side assignment for the half about to be played. */
	void SetTeamSides(ETraceTeam InTeamOnNegativeSide);

	/** Server only. Publishes the half index and whether the interval is running. */
	void SetHalfState(int32 InCurrentHalf, int32 InNumHalves, bool bInHalfTimeBreak);

	/** Server only. Records a wipe bonus so clients can flash it. Does not touch the score. */
	void NotifyWipeBonus(ETraceTeam BonusTeam);

	// ------------------------------------------------------------------------------------------
	// Scoring mode and result — the accessors every other system should use
	// ------------------------------------------------------------------------------------------

	/**
	 * THE question "which game is this?", answered from replicated state. Correct on the server and
	 * on every client, in every phase, including before the match starts.
	 */
	ETraceScoringMode GetScoringMode() const;

	/** Mode B: narrow goals, physical throwable Core. Sugar over GetScoringMode(). */
	bool IsGoalMode() const;

	/** Mode A: full-width endzones, the Core is a possession status. Sugar over GetScoringMode(). */
	bool IsEndzoneMode() const;

	/**
	 * The mode, from anything with a world — a component, an actor, a HUD pass — without each
	 * caller writing the same GetWorld()->GetGameState<>() null dance.
	 *
	 * Falls back to mode A when there is no GameState yet (a construction script, an early
	 * BeginPlay, a client that has not received it). That fallback is the shipped game, so a system
	 * that asks too early behaves like the default rather than like something nobody has played.
	 */
	static ETraceScoringMode GetScoringModeFor(const UObject* WorldContextObject);

	/** Server only. Publishes the mode and broadcasts OnScoringModeChanged. Idempotent. */
	void SetScoringMode(ETraceScoringMode InScoringMode);

	ETraceMatchEndReason GetMatchEndReason() const;
	ETraceTeam GetMatchWinner() const;

	/** True when the match was stopped early by the mercy rule rather than by the clock. */
	bool DidMatchEndByMercy() const;

	/** Server only. Records the result at the whistle. See MatchEndReason. */
	void SetMatchResult(ETraceTeam WinningTeam, ETraceMatchEndReason Reason);

	UFUNCTION()
	void OnRep_Scores();

	UFUNCTION()
	void OnRep_ScoringModeChanged();

	UFUNCTION()
	void OnRep_HalfChanged();

	UFUNCTION()
	void OnRep_SidesChanged();

	UFUNCTION()
	void OnRep_WipeBonus();
};
