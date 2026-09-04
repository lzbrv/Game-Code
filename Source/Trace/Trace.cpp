// Copyright Trace. All Rights Reserved.

#include "Trace.h"

#include "Modules/ModuleManager.h"

#include "UI/TraceNetworking.h"   // TraceNet::InstallNetVersionOverride

DEFINE_LOG_CATEGORY(LogTraceGame);

// =================================================================================================
// THE PRIMARY GAME MODULE
//
// This was IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, ...) — a module with no code of its
// own — until the cross-play pass needed ONE thing to happen before anything else in the process:
// pinning the network compatibility value.
//
// WHY HERE AND NOWHERE ELSE. TraceNet::InstallNetVersionOverride() binds a delegate that
// FNetworkVersion::GetLocalNetworkVersion consults on a CACHE MISS only (it memoises its answer the
// first time anything asks). So the bind has to land before the first ask, and it has to land in
// every configuration and every target — client, listen host, dedicated server, editor PIE,
// commandlet. A game module's StartupModule is the only hook that satisfies all of those: it runs
// once per process, at LoadingPhase Default (after config is loaded, so GConfig can be read, and
// long before a net driver exists), and it runs in the editor as well as in a packaged game.
//
// Putting it in a GameInstance subsystem or a HUD's BeginPlay would all have worked for the packaged
// game and quietly NOT worked for a dedicated server or for a PIE session that connects before the
// menu HUD is ever built — which is precisely the "it works here" trap this project keeps paying for.
// =================================================================================================
class FTraceGameModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();

		// Pin the value both sides of a Mac/Windows session compare during the connect handshake.
		// See the long block on TraceNet::NetProtocolVersion for what it is made of and why the
		// engine's own default is not good enough for a cross-platform playtest.
		TraceNet::InstallNetVersionOverride();
	}
};

// Primary game module. The third argument is the "game name" UBT stamps into the build; it must
// match the .uproject / module name exactly or the module will fail to mount at startup.
IMPLEMENT_PRIMARY_GAME_MODULE(FTraceGameModule, Trace, "Trace");
