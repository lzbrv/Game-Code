// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "TraceTypes.h"                 // ETraceTeam, ETraceMatchState

#include "TraceGameState.generated.h"

class ATraceCore;

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

	/** Server only. Sets the side assignment for the half about to be played. */
	void SetTeamSides(ETraceTeam InTeamOnNegativeSide);

	/** Server only. Publishes the half index and whether the interval is running. */
	void SetHalfState(int32 InCurrentHalf, int32 InNumHalves, bool bInHalfTimeBreak);

	/** Server only. Records a wipe bonus so clients can flash it. Does not touch the score. */
	void NotifyWipeBonus(ETraceTeam BonusTeam);

	UFUNCTION()
	void OnRep_Scores();

	UFUNCTION()
	void OnRep_HalfChanged();

	UFUNCTION()
	void OnRep_SidesChanged();

	UFUNCTION()
	void OnRep_WipeBonus();
};
