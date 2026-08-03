// Trace — title-screen game mode.
//
// Everything about this class is subtraction. /Game/Maps/MainMenu is an empty level; the menu
// needs a controller that can take a key press and a HUD that can draw, and nothing else. No
// arena, no Core, no bots, no teams, no match phase machine.
//
// It is bound to the map by NAME, not by an asset: Config/DefaultEngine.ini carries
//   +GameModeMapPrefixes=(Name="MainMenu",GameMode="/Script/Trace.TraceMenuGameMode")
// which UGameInstance::CreateGameModeForURL consults for any map whose name starts with
// "MainMenu" and whose world settings name no game mode of their own. That keeps the .umap a
// genuinely empty, regenerable file (Scripts/generate_map.py) with no per-map override baked into
// binary — which is the same reason the arena is built from C++.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "UObject/ObjectMacros.h"

#include "TraceMenuGameMode.generated.h"

UCLASS()
class TRACE_API ATraceMenuGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ATraceMenuGameMode();
};
