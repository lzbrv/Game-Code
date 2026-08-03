// Trace — title-screen game mode implementation. See TraceMenuGameMode.h.

#include "UI/TraceMenuGameMode.h"

#include "GameFramework/SpectatorPawn.h"

#include "UI/TraceMenuHUD.h"
#include "UI/TraceMenuPlayerController.h"

ATraceMenuGameMode::ATraceMenuGameMode()
{
	PrimaryActorTick.bCanEverTick = false;

	PlayerControllerClass = ATraceMenuPlayerController::StaticClass();
	HUDClass = ATraceMenuHUD::StaticClass();

	// A spectator pawn rather than nullptr. With no default pawn class AGameModeBase::RestartPlayer
	// logs a failed spawn on every login and the controller is left with no view target at all; the
	// spectator is the engine's own answer to "a player with nothing to possess", and on an empty
	// map it costs one invisible actor at the origin.
	//
	// bStartPlayersAsSpectators stays FALSE on purpose even though the pawn is a spectator: setting
	// it routes login through APlayerController::StartSpectatingOnly and the whole Spectating state
	// machine, when all this map needs is the ordinary RestartPlayer path with a pawn that happens
	// to have no mesh. Fewer moving parts under a screen that must never fail to appear.
	DefaultPawnClass = ASpectatorPawn::StaticClass();
	bStartPlayersAsSpectators = false;

	// The menu never travels *into* itself and carries no player state worth preserving, so the
	// seamless-travel machinery the match game mode needs would only add a transition map to
	// configure and one more thing to go wrong.
	bUseSeamlessTravel = false;
}
