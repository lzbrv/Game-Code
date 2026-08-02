// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "UObject/ObjectMacros.h"

#include "TraceTypes.h"                 // ETraceTeam

#include "TracePlayerState.generated.h"

/**
 * Per-player replicated state: team, K/D and whether this player currently holds the Core.
 *
 * Written on the server only (ATraceGameMode assigns the team and the K/D; ATraceCore maintains
 * bIsCarrier), read everywhere. This is the one object that survives the pawn, which is why the
 * team lives here and not on ATraceCharacter — a dead player still belongs to a team, still shows
 * on the scoreboard, and still spawns on their own half.
 */
UCLASS()
class TRACE_API ATracePlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	ATracePlayerState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Team this player fights for. None until the GameMode balances them in during PostLogin. */
	UPROPERTY(ReplicatedUsing = OnRep_Team)
	ETraceTeam Team = ETraceTeam::None;

	UPROPERTY(Replicated)
	int32 Kills = 0;

	UPROPERTY(Replicated)
	int32 Deaths = 0;

	/**
	 * Mirror of ATraceCharacter::bIsCarrier, kept here so the scoreboard and HUD can show who has
	 * the Core without needing that player's pawn to be relevant. ATraceCore owns both writes.
	 */
	UPROPERTY(Replicated)
	bool bIsCarrier = false;

	/** Server only. Assigns the team and runs the OnRep locally so a listen server updates too. */
	void SetTeam(ETraceTeam NewTeam);

	UFUNCTION()
	void OnRep_Team();

	/**
	 * Carries Team/Kills/Deaths across seamless travel. Called on the OLD player state with the NEW
	 * one as the argument, so the copy goes outwards into @p PlayerState.
	 */
	virtual void CopyProperties(APlayerState* PlayerState) override;
};
