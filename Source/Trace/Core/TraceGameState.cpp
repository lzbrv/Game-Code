// Copyright (c) Trace. All Rights Reserved.

#include "Core/TraceGameState.h"

#include "Net/UnrealNetwork.h"

#include "GameFramework/PlayerState.h"
#include "Core/TracePlayerState.h"
#include "Gameplay/TraceCore.h"
#include "Trace.h"

ATraceGameState::ATraceGameState()
{
	PrimaryActorTick.bCanEverTick = false;

	// AGameStateBase already sets both; stated explicitly per contract §8 so nobody has to go
	// reading engine source to be sure the match state actually reaches every client.
	bReplicates = true;
	bAlwaysRelevant = true;
}

void ATraceGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// All COND_None: every one of these is on the HUD of every player, including spectators.
	DOREPLIFETIME(ATraceGameState, BlueScore);
	DOREPLIFETIME(ATraceGameState, OrangeScore);
	DOREPLIFETIME(ATraceGameState, TraceMatchState);
	DOREPLIFETIME(ATraceGameState, MatchEndServerTime);
	DOREPLIFETIME(ATraceGameState, Core);
}

int32 ATraceGameState::GetScore(ETraceTeam Team) const
{
	switch (Team)
	{
	case ETraceTeam::Blue:
		return BlueScore;
	case ETraceTeam::Orange:
		return OrangeScore;
	default:
		return 0;
	}
}

void ATraceGameState::AddScore(ETraceTeam Team, int32 Amount)
{
	if (!HasAuthority())
	{
		return;
	}

	switch (Team)
	{
	case ETraceTeam::Blue:
		BlueScore = FMath::Max(0, BlueScore + Amount);
		break;
	case ETraceTeam::Orange:
		OrangeScore = FMath::Max(0, OrangeScore + Amount);
		break;
	default:
		// Nobody scores for ETraceTeam::None.
		return;
	}

	// OnRep callbacks never fire on the authority, so drive it by hand — a listen server's own HUD
	// and logs must see exactly what a remote client sees.
	OnRep_Scores();

	// A goal is a rare, high-value event: do not wait for the GameState's normal net update slot.
	ForceNetUpdate();
}

float ATraceGameState::GetMatchTimeRemaining() const
{
	// GetServerWorldTimeSeconds() returns double on 5.3+ (it was float before), so keep the maths in
	// double and narrow only at the end — that stays correct whichever signature the engine has.
	const double Remaining = static_cast<double>(MatchEndServerTime) - GetServerWorldTimeSeconds();
	return FMath::Max(0.f, static_cast<float>(Remaining));
}

int32 ATraceGameState::CountTeamMembers(ETraceTeam Team) const
{
	int32 Count = 0;
	for (const APlayerState* PlayerState : PlayerArray)
	{
		// Cast<To>(const From*) resolves to the const overload and yields const To*, so the template
		// argument must stay non-const.
		const ATracePlayerState* TracePlayerState = Cast<ATracePlayerState>(PlayerState);
		if (TracePlayerState == nullptr || TracePlayerState->IsOnlyASpectator())
		{
			continue;
		}

		if (TracePlayerState->Team == Team)
		{
			++Count;
		}
	}

	return Count;
}

void ATraceGameState::OnRep_Scores()
{
	// Nothing to push: the HUD polls GetScore() every frame. The log is the cheapest way to watch
	// score replication actually land on a client during a two-instance net test.
	UE_LOG(LogTraceGame, Verbose, TEXT("Scores replicated: Blue %d - Orange %d"), BlueScore, OrangeScore);
}
