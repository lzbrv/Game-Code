// Copyright Trace. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

/**
 * Project-wide log category.
 *
 * Declared here, defined in Trace.cpp. Every module file that logs includes this header.
 * Per the build contract: prefer UE_LOG(LogTraceGame, Verbose, ...) and never log per-tick at Log
 * level -- a 10-player match ticking at 60 Hz will drown the server log otherwise.
 *
 * Runtime filtering (no recompile needed):
 *     Log LogTraceGame Verbose      // console
 *     -LogCmds="LogTraceGame Verbose"   // command line
 */
DECLARE_LOG_CATEGORY_EXTERN(LogTraceGame, Log, All);
