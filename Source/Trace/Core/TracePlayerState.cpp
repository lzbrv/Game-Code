// Copyright (c) Trace. All Rights Reserved.

#include "Core/TracePlayerState.h"

#include "Net/UnrealNetwork.h"

#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Core/TraceCharacter.h"
#include "Trace.h"

ATracePlayerState::ATracePlayerState()
{
	PrimaryActorTick.bCanEverTick = false;

	// APlayerState is an AInfo and already replicates; stated explicitly per contract §8.
	bReplicates = true;
}

void ATracePlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None throughout: the scoreboard shows every player's team, K/D and carrier flag, so all
	// of this has to reach every client, not just the owner.
	DOREPLIFETIME(ATracePlayerState, Team);
	DOREPLIFETIME(ATracePlayerState, Kills);
	DOREPLIFETIME(ATracePlayerState, Deaths);
	DOREPLIFETIME(ATracePlayerState, bIsCarrier);
}

void ATracePlayerState::SetTeam(ETraceTeam NewTeam)
{
	if (!HasAuthority())
	{
		return;
	}

	Team = NewTeam;

	// Replication callbacks never fire on the authority, and a listen server's local pawn still
	// needs its team colours; run the OnRep by hand so both sides take the identical path.
	OnRep_Team();

	// PlayerStates replicate on a slow net update rate by default. Team is a one-shot, highly
	// visible change (it decides colours, spawn side and friendly fire), so push it immediately.
	ForceNetUpdate();

	UE_LOG(LogTraceGame, Verbose, TEXT("PlayerState '%s' assigned to team %d"), *GetPlayerName(), static_cast<int32>(Team));
}

void ATracePlayerState::OnRep_Team()
{
	// Best-effort only: the PlayerState usually replicates before the pawn exists. ATraceCharacter
	// re-applies its colours from BeginPlay and OnRep_PlayerState as well, so whichever of the two
	// arrives last is the one that actually paints the character.
	APawn* OwnedPawn = GetPawn();
	if (OwnedPawn == nullptr)
	{
		// Covers the window where PawnPrivate has not been set yet but the owning controller is
		// already possessing (server and owning connection).
		if (const AController* OwningController = Cast<AController>(GetOwner()))
		{
			OwnedPawn = OwningController->GetPawn();
		}
	}

	if (ATraceCharacter* TraceCharacter = Cast<ATraceCharacter>(OwnedPawn))
	{
		TraceCharacter->ApplyTeamColors();
	}
}

void ATracePlayerState::CopyProperties(APlayerState* PlayerState)
{
	Super::CopyProperties(PlayerState);

	// Runs on the OLD state during seamless travel, with the NEW state as the argument.
	if (ATracePlayerState* TracePlayerState = Cast<ATracePlayerState>(PlayerState))
	{
		// Direct assignment rather than SetTeam(): the destination has no pawn and is not yet
		// replicating, so the OnRep/ForceNetUpdate work SetTeam does would be wasted.
		TracePlayerState->Team = Team;
		TracePlayerState->Kills = Kills;
		TracePlayerState->Deaths = Deaths;

		// bIsCarrier is deliberately NOT carried: the Core does not survive travel, and a stale
		// "carrier" flag would make the HUD claim someone is holding an object that no longer exists.
	}
}
