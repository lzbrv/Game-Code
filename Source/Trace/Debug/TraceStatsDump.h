// =================================================================================================
// Trace — TraceStatsDump.h   (spec v29 §4)
//
// "Please create a google sheet with a full breakdown of every single stat in the game and its
//  value. E.g. I want movement speed, roxie E cooldown, pistol headshot dmg, etc."
//
// *** WE CANNOT CREATE A GOOGLE SHEET FROM HERE. *** There is no authenticated Drive access in this
// environment, so nothing in this repo can write to a Google account. What this file delivers is
// the thing that becomes one in a single click: a CSV. File > Import > Upload in Sheets, or drag it
// into Drive. docs/TraceStats.csv is the committed copy and GitHub renders it as a TABLE in the
// browser with no download at all.
//
// =================================================================================================
// THE ONE DESIGN RULE: THIS SHEET IS GENERATED FROM THE LIVE GAME, NEVER TYPED
// =================================================================================================
//
// A hand-written stat sheet is stale the day it is written — the first knob anyone retunes makes it
// a lie, and a lie in a spreadsheet is worse than no spreadsheet, because it is quoted. So every
// row here comes out of the running process:
//
//   * the knob pages are walked BY REFLECTION (TFieldIterator over each UCLASS), not from a list.
//     A knob added tomorrow appears in the sheet tomorrow with NO change to this file. A knob
//     deleted tomorrow leaves the sheet. There is no table here to forget to update, deliberately —
//     Trace.VerifyKnobs' curated table is the right shape for "does this specific slider still
//     exist" and the wrong shape for "show me everything".
//   * the per-character cooldowns are asked of the ABILITY SET CDO — the same virtual the game
//     charges on activation — and printed beside the number the select card prints, so the sheet
//     shows the disagreement rather than hiding it.
//   * every DERIVED number (RPM, the effective air caps, the knife's stowed speed, shots-to-kill)
//     is COMPUTED at dump time from the live bases. Not one of them is a literal in this file.
//     That is the project's standing rule — a value that modifies a base must move when the base
//     moves — applied to the sheet itself.
//
// The settings classes are reached BY /Script PATH rather than by #including their headers, exactly
// as FKnobSpec::OwnerPath does in TraceSettings.cpp. Two reasons, both practical: this file keeps
// compiling while somebody else is editing a settings header, and a page that is added later needs
// only one string here instead of an include and a link dependency.
//
// =================================================================================================
// HOW TO RUN IT
// =================================================================================================
//
// FROM A TERMINAL, IN THE REPO. Not from inside the game, and not by typing "Trace.DumpStats" at a
// shell prompt (it is a console command inside the game, not a program):
//
//     Scripts/dump-stats.sh                 Mac/Linux. Writes docs/TraceStats.csv, the committed copy
//     Scripts\dump-stats.bat                Windows. Same thing
//     Scripts/dump-stats.sh <path>          writes wherever you say instead
//
// The script finds the engine the way every other script does (Scripts/_trace_common.sh: UE_ROOT,
// then .ue-root, then the usual install folders), opens the practice range headlessly with the EDITOR
// build, runs Trace.DumpStats and then Trace.VerifyStats on the same path, quits, and prints both
// verdicts and where the file went. It stops with the reason when the engine, the editor build
// (Scripts/build.sh) or the baked arena (git lfs pull) is missing, and warns when the editor build is
// older than Source/ - the sheet shows the numbers of the build that RAN, not of the tree.
//
// DEV / EDITOR BUILDS ONLY. This whole file compiles out of Shipping, like every other Trace.*
// console command, so a packaged release build has no such command at all. In a packaged
// Development build on a Mac the default Saved/ path is inside the app's own container, not the
// repo, which is one more reason to use the script.
//
// The console commands themselves, for a dev build that is already running (` opens the console):
//
//     Trace.DumpStats                 writes <Project>/Saved/Stats/TraceStats.csv
//     Trace.DumpStats <path>          writes wherever you say (absolute, or project-relative)
//     Trace.VerifyStats [<path>]      re-reads that file (default: the Saved/Stats one) and PARSES
//                                     it, column by column, so a quoting bug cannot ship as a
//                                     silently column-shifted spreadsheet
//
// What the script runs, printed in full before it runs it (one command, wrapped here). The engine
// binary has to be the FULL path: a bare "UnrealEditor", which is what this comment used to show,
// is not on PATH on a stock Mac or Windows install, so pasting that line failed with "command not
// found" before the game ever started.
//
//     "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor" "<repo>/Trace.uproject"
//         "/Game/Maps/Arena_Baked?game=/Script/Trace.TracePracticeGameMode"
//         -game -log -nullrhi -RenderOffScreen -unattended -nosound -nosplash
//         -TraceExec="Trace.DumpStats docs/TraceStats.csv|Trace.VerifyStats docs/TraceStats.csv|quit"
//         -TraceExecAt=6 -TraceExecOn=Match -abslog=<repo>/Saved/Logs/dump-stats.log
//
// (Windows: Engine\Binaries\Win64\UnrealEditor.exe. Copy the line the script prints rather than
// this one; it has your paths in it.)
// =================================================================================================

#pragma once

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

namespace TraceStatsDump
{
	/** What one run produced. Everything here is counted, never assumed. */
	struct FStatsDumpReport
	{
		/** Absolute path actually written. Empty when the write failed. */
		FString Path;

		/** Data rows in the file, excluding the header line. */
		int32 RowCount = 0;

		/** Distinct human-readable sections (the first column). */
		int32 SectionCount = 0;

		/** UPROPERTYs found on UTraceSettings by reflection — the "registry" the spec's 367 refers to. */
		int32 TraceSettingsPropertyCount = 0;

		/** How many of those carry CPF_Config, i.e. how many DefaultGame.ini can actually reach. */
		int32 TraceSettingsConfigCount = 0;

		/** Rows contributed by settings pages other than UTraceSettings. */
		int32 OtherSettingsRowCount = 0;

		/** Rows that are NOT knobs — roster cards, ability-set CDO answers, engine-owned defaults. */
		int32 NonKnobRowCount = 0;

		/** Rows computed from other rows at dump time. */
		int32 DerivedRowCount = 0;

		/**
		 * Every knob a derived row asked for BY NAME and did not find. Non-empty means somebody
		 * renamed a property and a derived cell in the sheet now reads "<MISSING KNOB: x>" instead
		 * of a number. That is the failure this list exists to make loud rather than plausible.
		 */
		TArray<FString> MissingKnobs;

		/** Settings classes named in the walk that do not exist in this build. */
		TArray<FString> MissingClasses;
	};

	/**
	 * Write the sheet.
	 *
	 * @param InPath  Absolute, or relative to the project directory. Empty means DefaultCsvPath().
	 * @param OutReport  Always filled, including on failure.
	 * @return true when the file is on disk.
	 */
	TRACE_API bool WriteStatsCsv(const FString& InPath, FStatsDumpReport& OutReport);

	/** <Project>/Saved/Stats/TraceStats.csv, absolute. */
	TRACE_API FString DefaultCsvPath();

	/**
	 * Re-read a written CSV and prove it is a well-formed table: quote-aware parse, every row the
	 * same width as the header, no empty stat names. Logs a VERDICT line either way.
	 *
	 * This is not ceremony. The one way a generated sheet fails silently is a value containing a
	 * comma or a quote that was not escaped: the file still opens, and every column after it is
	 * shifted by one for that row only. Nobody reading the sheet would catch it.
	 */
	TRACE_API bool VerifyStatsCsv(const FString& InPath, FString& OutSummary);
}

#endif // !UE_BUILD_SHIPPING
