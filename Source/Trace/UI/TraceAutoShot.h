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

	/**
	 * Runs console commands a fixed number of seconds after a HUD comes up.
	 *
	 *     -TraceExec="Trace.VerifyKnobs|Trace.ModeB.Verify"  -TraceExecAt=8
	 *
	 * WHY THIS EXISTS. The project's verification commands (Trace.VerifyKnobs, Trace.ModeB.Verify,
	 * Trace.Trail.TestHeadGap, Trace.TestRecoil, ...) only mean anything once there is a match, a
	 * pawn and a Core. The engine's own -ExecCmds fires once during engine init, on the TITLE
	 * screen, where every one of them is a no-op - so headlessly there was no way to invoke them at
	 * all and they could only ever be run by a human typing into a console. That is exactly the kind
	 * of check that silently stops being run.
	 *
	 * Commands are separated by '|' rather than ',' because several of them take numeric arguments
	 * and a comma would have to be escaped past both the shell and FParse.
	 *
	 * Same arm-once-per-world rule as Arm(): a HUD is rebuilt on travel, so the menu HUD and the
	 * match HUD each get one chance, and @p Tag decides which. Dev-only; compiles out of Shipping.
	 *
	 * @param Tag Must match -TraceExecOn= (default "Match") or nothing is scheduled. This is what
	 *            stops a match-only command from firing against the title screen.
	 */
	void ArmDeferredExec(AHUD* OwnerHUD, const TCHAR* Tag);
}

#endif // !UE_BUILD_SHIPPING
