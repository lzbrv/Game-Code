// Copyright (c) Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerStart.h"
#include "TraceTypes.h"
#include "TraceTeamPlayerStart.generated.h"

/**
 * A PlayerStart that knows which team owns it.
 *
 * ATraceArenaBuilder spawns five of these per team at the two ends of the field, and
 * ATraceGameMode::ChoosePlayerStart_Implementation filters on Team so players always spawn on
 * their own half. Nothing here is replicated: the starts are only ever consulted on the server
 * (the arena builder skips spawning them on clients entirely).
 */
UCLASS()
class TRACE_API ATraceTeamPlayerStart : public APlayerStart
{
	GENERATED_BODY()

public:
	// APlayerStart only declares the FObjectInitializer constructor, so a derived class must
	// declare one too - the compiler cannot synthesise a default constructor from it.
	ATraceTeamPlayerStart(const FObjectInitializer& ObjectInitializer);

	/** Team that spawns here. ETraceTeam::None means "usable by anyone". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trace")
	ETraceTeam Team = ETraceTeam::None;
};
