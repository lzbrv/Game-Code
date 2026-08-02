// Copyright Trace. All Rights Reserved.

#include "Trace.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogTraceGame);

// Primary game module. The third argument is the "game name" UBT stamps into the build; it must
// match the .uproject / module name exactly or the module will fail to mount at startup.
IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, Trace, "Trace");
