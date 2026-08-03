// Trace — automated screenshot harness.
//
// Nobody can verify a renderer by reading a log. Launch with `-TraceAutoShot=N` (optionally
// `-TraceAutoShotRepeat=M` for a second, third, ... capture every M seconds) and the owning HUD
// grabs a full-frame screenshot and logs the absolute path it landed at, at Display level:
//
//   UnrealEditor Trace.uproject /Game/Maps/Arena -game -windowed -TraceAutoShot=12
//
// Absent the switch this costs one FParse::Value per HUD and nothing else, and the whole thing
// compiles out of Shipping.
//
// This lives outside ATraceHUD because the title-screen HUD is a *sibling* class, not a subclass,
// and both need to be verifiable the same way. Everything is free functions over an AHUD — there
// is no state to own, so there is no base class to inherit and no component to attach.

#pragma once

#include "CoreMinimal.h"

class AHUD;

#if !UE_BUILD_SHIPPING

namespace TraceAutoShot
{
	/**
	 * Reads -TraceAutoShot=<seconds> / -TraceAutoShotRepeat=<seconds> off the command line and arms
	 * the timers on @p OwnerHUD's world. Safe to call from any HUD's BeginPlay: it returns
	 * immediately when the switch is absent, when the HUD has no local viewport, or when a HUD in
	 * this process has already armed (a HUD is recreated on every travel, and two of them would
	 * fight over the same filenames).
	 *
	 * @param Tag Short label baked into the filename, so menu captures and match captures are
	 *            told apart at a glance in Saved/Screenshots/.
	 */
	void Arm(AHUD* OwnerHUD, const TCHAR* Tag);
}

#endif // !UE_BUILD_SHIPPING
