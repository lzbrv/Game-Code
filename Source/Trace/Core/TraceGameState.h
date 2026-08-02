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
	 * the warm-up deadline while WaitingForPlayers, the match deadline while InProgress, and the
	 * moment the match ended once PostMatch. Zero means "no deadline running".
	 */
	UPROPERTY(Replicated)
	float MatchEndServerTime = 0.f;

	/** The single contested Core. Null until the GameMode has spawned it and it has replicated. */
	UPROPERTY(Replicated)
	TObjectPtr<ATraceCore> Core = nullptr;

	int32 GetScore(ETraceTeam Team) const;

	/** Server only. Negative amounts are allowed but a team score never goes below zero. */
	void AddScore(ETraceTeam Team, int32 Amount);

	/** Seconds until MatchEndServerTime, clamped at zero. Valid on clients and on the server. */
	float GetMatchTimeRemaining() const;

	/** Non-spectator players currently assigned to Team. Drives GameMode team balancing. */
	int32 CountTeamMembers(ETraceTeam Team) const;

	UFUNCTION()
	void OnRep_Scores();
};
