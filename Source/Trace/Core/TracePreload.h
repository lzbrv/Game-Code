// =================================================================================================
// Trace — TracePreload.h   (P11, performance)
//
// WORK A MATCH WOULD OTHERWISE DO ON ITS FIRST CONTESTED FRAMES, MOVED TO MAP LOAD.
//
// A few process-lifetime tables are built the first time something asks for them, and in a match the
// first asker used to be the kickoff frame: the bots are handed their characters as the half starts,
// which synchronously loaded all ten DA_Character_* assets (a FlushAsyncLoading) and walked every
// UClass in the process for the ability roster, on the frame the Core goes live. Both latch for the
// process, so asking once while the map is loading makes kickoff a table read.
//
// Called from ATraceGameMode::BeginPlay (the server, which assigns) and ATraceHUD::BeginPlay (every
// machine that draws, which reads the same tables for names and colours). Idempotent: after the first
// call it is two lookups. A loading screen (P12) may call it earlier still.
// =================================================================================================

#pragma once

#include "CoreMinimal.h"

namespace TracePreload
{
	/**
	 * Resolves the character roster (TraceCharacterRoster::All — the DA_Character_* definitions and the
	 * wording laid over them) and the ability roster (UTraceCharacterAbilitySet's reflection walk).
	 * Game thread. Logs once per process how long the first call took.
	 */
	TRACE_API void WarmMatchTables();
}
