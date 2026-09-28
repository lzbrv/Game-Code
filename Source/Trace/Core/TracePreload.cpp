// Trace — see TracePreload.h.

#include "Core/TracePreload.h"

#include "Abilities/TraceCharacterAbilitySet.h"
#include "Core/TraceCharacterRoster.h"
#include "HAL/PlatformTime.h"
#include "Trace.h"

void TracePreload::WarmMatchTables()
{
	static bool bLoggedFirstWarm = false;
	const double Start = FPlatformTime::Seconds();

	// The roster latches for the process (a Trace.Text.Reload or the asset cvar re-resolves it, and the
	// next call here or anywhere else rebuilds it the same way).
	const int32 Characters = TraceCharacterRoster::All().Num();

	// Any valid id builds the whole map; the class itself is not needed here.
	UTraceCharacterAbilitySet::FindClassFor(static_cast<ETraceCharacterId>(TraceCharacterRoster::FirstId));

	if (!bLoggedFirstWarm)
	{
		bLoggedFirstWarm = true;
		UE_LOG(LogTraceGame, Display,
			TEXT("[Preload] match tables warm at map load (%d characters, ability roster) in %.1f ms - "
			     "kickoff reads them instead of loading them."),
			Characters, (FPlatformTime::Seconds() - Start) * 1000.0);
	}
}
