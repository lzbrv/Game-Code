@echo off
rem =============================================================================
rem  Trace - package.bat   (Windows twin of package.sh)
rem
rem  Cooks content and produces a standalone, runnable game build - the thing you
rem  send to a playtester, as opposed to the thing Scripts\build.bat produces,
rem  which is a binary that links.
rem
rem  THE DIFFERENCE MATTERS AND HAS BITTEN THIS PROJECT ALREADY. Until package.sh
rem  existed, "both build configs green" meant Scripts\build.bat had compiled and
rem  linked a Shipping binary. That binary CANNOT RUN: a UE game target with no
rem  cooked content on disk exits immediately. See docs\KNOWN_LIMITATIONS.md item
rem  29. Linking is not shipping. This script is the step that was missing.
rem
rem  It wraps exactly one command, which it prints first:
rem
rem    "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun ^^
rem        -project="<repo>\Trace.uproject" -noP4 -utf8output ^^
rem        -platform=Win64 -clientconfig=Shipping ^^
rem        -build -cook -stage -pak -package -archive ^^
rem        -archivedirectory="<output>" -nocompileeditor
rem
rem  Nothing here is magic. If the script ever gets in your way, copy the printed
rem  command and run it yourself.
rem
rem  ---------------------------------------------------------------------------
rem  *** THIS FILE HAS NEVER BEEN RUN ON WINDOWS. ***
rem
rem  It was written on macOS, against Scripts\package.sh line for line, on a
rem  machine that physically cannot execute it or test it: an Unreal install for
rem  macOS ships no Win64 engine binaries at all (Engine\Binaries holds DotNET,
rem  Mac and ThirdParty - no Win64 directory exists), and Unreal does not
rem  cross-compile a Windows game from macOS. Every other .bat in this directory
rem  has been run by somebody; this one has not. Treat its first run as a test of
rem  the script as much as of the project, and read the failure modes below before
rem  concluding the project is broken.
rem
rem  THE THREE FAILURES TO EXPECT FIRST, in the order they are likely to happen:
rem
rem   1. NO C++ TOOLCHAIN. The Epic Games Launcher installs an engine, not a
rem      compiler. Without Visual Studio 2022 + the "Game development with C++"
rem      workload + the "Unreal Engine installer" individual component, the -build
rem      stage dies inside UnrealBuildTool with an opaque "no compatible toolchain"
rem      message. This script calls _trace_common.bat check_toolchain up front and
rem      WARNS - it does not refuse, because a warn-only check cannot wrongly block
rem      a working machine. If you see that warning, fix it before reading further.
rem          winget install Microsoft.VisualStudio.2022.Community
rem
rem   2. UE_ROOT POINTS AT AN ENGINE WITHOUT THE WIN64 TOOLCHAIN. resolve_engine
rem      only checks that Engine\Build\BatchFiles\Build.bat exists, which a partial
rem      or still-downloading launcher install can satisfy. If BuildCookRun dies
rem      complaining about a missing target receipt or a missing platform, check
rem      that "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" exists and that the
rem      launcher lists the install as complete (Library -> UE_5.8 -> Verify).
rem
rem   3. THE EDITOR MODULES ARE NOT BUILT. This is the one most likely to bite,
rem      because it looks like a cook bug. -nocompileeditor is passed (matching
rem      package.sh), so UAT will NOT build TraceEditor for you - but the cook
rem      stage runs UnrealEditor-Cmd.exe against this project, which loads
rem      Binaries\Win64\UnrealEditor-Trace.dll. On a fresh Windows checkout that
rem      DLL does not exist and the cook fails minutes in. Run Scripts\build.bat
rem      once first. This script checks for that DLL and warns.
rem
rem  ONE DELIBERATE DIFFERENCE FROM THE .sh, and it is not an oversight.
rem  package.sh pipes UAT through `tee` so it can both stream a 40-minute cook live
rem  AND grep the captured log for UAT's own "BUILD SUCCESSFUL" verdict (its gate
rem  2). cmd.exe has no tee, and every way of faking one - redirect to a file, pipe
rem  into PowerShell Tee-Object, a temp wrapper .bat - either kills the live output
rem  (so a cold-DDC cook looks hung for an hour and gets killed) or eats the exit
rem  code (a pipe's %errorlevel% is the RIGHT-hand side's, and cmd has no
rem  PIPESTATUS). Streaming won, so gate 2 is implemented differently here: instead
rem  of grepping for the verdict, this script records the cooked content's
rem  fingerprint before the run and requires it to have CHANGED afterwards. That
rem  answers the same question - "is what is sitting in the output directory
rem  actually from THIS run, or from an earlier one?" - without needing the log.
rem  Gates 1 and 3 are identical to the .sh.
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "CONFIG=Shipping"
set "PLATFORM="
set "OUTPUT="
set "DO_PAK=1"
set "DO_COOK=1"
set "DO_BUILD=1"
set "DO_ITERATE=0"
set "EXTRA_ARGS="

rem Default output lives outside the repo tree's committed paths. Saved\ is already
rem gitignored, so an accidental `git add -A` cannot pick up a 2 GB build.
set "DEFAULT_OUTPUT=%TRACE_PROJECT_ROOT%\Saved\Packaged"

rem -----------------------------------------------------------------------------
rem  Argument parsing
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="-o"           goto :o_output
if /i "!_a!"=="--output"     goto :o_output
if /i "!_a!"=="-c"           goto :o_config
if /i "!_a!"=="--config"     goto :o_config
if /i "!_a!"=="-p"           goto :o_platform
if /i "!_a!"=="--platform"   goto :o_platform
if /i "!_a!"=="--no-pak"     goto :o_nopak
if /i "!_a!"=="--skip-cook"  goto :o_skipcook
if /i "!_a!"=="--skip-build" goto :o_skipbuild
if /i "!_a!"=="--iterate"    goto :o_iterate
if /i "!_a!"=="-n"           goto :o_dryrun
if /i "!_a!"=="--dry-run"    goto :o_dryrun
if /i "!_a!"=="-h"           goto :o_help
if /i "!_a!"=="--help"       goto :o_help
if /i "!_a!"=="/?"           goto :o_help
if /i "!_a!"=="--"           goto :o_extra
if "!_a:~0,1!"=="-"          goto :unknown_option
goto :unexpected_arg

:o_output
if "%~2"=="" goto :need_output_value
set "OUTPUT=%~2"
shift
shift
goto :parse
:need_output_value
call "%~dp0_trace_common.bat" err "--output needs a value"
exit /b 2

:o_config
if "%~2"=="" goto :need_config_value
set "CONFIG=%~2"
shift
shift
goto :parse
:need_config_value
call "%~dp0_trace_common.bat" err "--config needs a value"
exit /b 2

:o_platform
if "%~2"=="" goto :need_platform_value
set "PLATFORM=%~2"
shift
shift
goto :parse
:need_platform_value
call "%~dp0_trace_common.bat" err "--platform needs a value"
exit /b 2

:o_nopak
set "DO_PAK=0"
shift
goto :parse

:o_skipcook
set "DO_COOK=0"
shift
goto :parse

:o_skipbuild
set "DO_BUILD=0"
shift
goto :parse

:o_iterate
set "DO_ITERATE=1"
shift
goto :parse

:o_dryrun
set "TRACE_DRY_RUN=1"
shift
goto :parse

:o_help
call :usage
exit /b 0

:o_extra
shift
:o_extra_loop
if "%~1"=="" goto :parsed
set "EXTRA_ARGS=!EXTRA_ARGS! %1"
shift
goto :o_extra_loop

:unknown_option
call "%~dp0_trace_common.bat" err "Unknown option: !_a!"
echo(
call :usage
exit /b 2

:unexpected_arg
call "%~dp0_trace_common.bat" err "Unexpected argument: !_a!"
echo(
call :usage
exit /b 2

:parsed

rem -----------------------------------------------------------------------------
rem  Validate up front. A typo here otherwise surfaces forty minutes into a cook.
rem -----------------------------------------------------------------------------
call "%~dp0_trace_common.bat" is_config "!CONFIG!"
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "Unknown configuration '!CONFIG!'. Expected Debug, DebugGame, Development, Test or Shipping."
    exit /b 1
)
if not defined PLATFORM set "PLATFORM=%TRACE_HOST_PLATFORM%"
if not defined OUTPUT set "OUTPUT=%DEFAULT_OUTPUT%"

rem Strip a trailing backslash. "C:\out\" reaches UAT as -archivedirectory="C:\out\"
rem and the backslash escapes the closing quote, so the argument swallows the next
rem one. This is the single most common Windows-only quoting bug in wrapper scripts
rem and it costs a whole cook to discover.
:trim_output
if not defined OUTPUT goto :trimmed_output
if "!OUTPUT:~-1!"=="\" (
    set "OUTPUT=!OUTPUT:~0,-1!"
    goto :trim_output
)
:trimmed_output

rem -----------------------------------------------------------------------------
rem  Refuse the cross-compile that cannot work, with the reason, immediately.
rem
rem  This is not defensiveness. Unreal does not cross-compile a game for a desktop
rem  platform it is not running on, and BuildCookRun asked to do it fails LATE and
rem  with a message about a missing target receipt, which reads like a project bug.
rem  Say the true thing instead.
rem
rem  The mirror image of this arm in package.sh is the one that refuses Win64 on a
rem  Mac; this one refuses Mac on Windows. Both exist so that neither machine can
rem  waste forty minutes discovering it the hard way.
rem -----------------------------------------------------------------------------
if /i not "!PLATFORM!"=="%TRACE_HOST_PLATFORM%" (
    call "%~dp0_trace_common.bat" err "Cannot package !PLATFORM! from a %TRACE_HOST_PLATFORM% host."
    call "%~dp0_trace_common.bat" blank
    call "%~dp0_trace_common.bat" err "Unreal does not cross-compile a game for a desktop platform it is not running on."
    call "%~dp0_trace_common.bat" err "A Windows engine install ships no !PLATFORM! binaries at all - see for yourself:"
    call "%~dp0_trace_common.bat" err "    dir %%UE_ROOT%%\Engine\Binaries"
    call "%~dp0_trace_common.bat" blank
    if /i "!PLATFORM!"=="Mac" (
        call "%~dp0_trace_common.bat" err "For a Mac build, run this from a macOS checkout with a macOS engine:"
        call "%~dp0_trace_common.bat" err "    Scripts/package.sh"
    )
    exit /b 2
)

call "%~dp0_trace_common.bat" require_uproject
if errorlevel 1 exit /b 1
call "%~dp0_trace_common.bat" resolve_engine
if errorlevel 1 exit /b 1
call "%~dp0_trace_common.bat" check_toolchain

set "RUNUAT=!UE_ROOT!\Engine\Build\BatchFiles\RunUAT.bat"
if not exist "!RUNUAT!" (
    call "%~dp0_trace_common.bat" err "RunUAT.bat not found at: !RUNUAT!"
    call "%~dp0_trace_common.bat" err "That file ships with every engine distribution, so this install is broken or"
    call "%~dp0_trace_common.bat" err "UE_ROOT points somewhere that is not an engine. Verify it in the Epic Games"
    call "%~dp0_trace_common.bat" err "Launcher: Library, then UE_%TRACE_ENGINE_VERSION%, then Verify."
    exit /b 1
)

rem -----------------------------------------------------------------------------
rem  The editor modules have to be built BEFORE the cook, not after.
rem
rem  WINDOWS-ONLY GUARD; package.sh has no equivalent because the machine it runs
rem  on has always had its editor built. -nocompileeditor is passed below (matching
rem  the .sh), so UAT will not build TraceEditor for you - but the cook stage runs
rem  UnrealEditor-Cmd.exe against this project and that loads the project's editor
rem  DLL. On a fresh Windows checkout it does not exist and the cook dies minutes
rem  in with an error about failing to load a module, which reads like a content
rem  problem and is not one.
rem
rem  WARN, DO NOT REFUSE. This check has never been run, and a false positive that
rem  blocks a working machine would be worse than the failure it prevents. If the
rem  DLL is named something else on your engine version, the cook will still work
rem  and you will simply have read one wrong warning.
rem -----------------------------------------------------------------------------
if "%DO_COOK%"=="1" (
    if not exist "%TRACE_PROJECT_ROOT%\Binaries\Win64\UnrealEditor-%TRACE_PROJECT_NAME%.dll" (
        call "%~dp0_trace_common.bat" warn "Binaries\Win64\UnrealEditor-%TRACE_PROJECT_NAME%.dll is not there."
        call "%~dp0_trace_common.bat" warn "The cook runs UnrealEditor-Cmd.exe, which loads that DLL, and -nocompileeditor"
        call "%~dp0_trace_common.bat" warn "means UAT will not build it for you. Build it once first:"
        call "%~dp0_trace_common.bat" warn "    Scripts\build.bat"
        call "%~dp0_trace_common.bat" warn "Carrying on anyway - if the cook fails to load a module, this was why."
    )
)

rem -----------------------------------------------------------------------------
rem  The maps have to exist BEFORE the cook, not after.
rem
rem  Config\DefaultGame.ini lists three maps under MapsToCook. Two of them
rem  (Arena, MainMenu) are generated by Scripts\generate-map.bat rather than
rem  committed, so a fresh clone has no .umap for them. The cooker's behaviour when
rem  a MapsToCook entry is missing is to WARN and carry on, which produces a build
rem  that installs, launches, and then cannot open its own default map.
rem
rem  So check here, where the message can still be useful.
rem
rem  The parse is deliberately quote-free. Each line reads
rem      +MapsToCook=(FilePath="/Game/Maps/Arena")
rem  and pulling the path out with a quoted delimiter in `for /f` is the classic
rem  way to lose an argument to cmd's quote stripping. Instead: take everything
rem  after the literal /Game/ with a substring-after match (no quotes in the
rem  pattern), then chop the trailing ") with a two-character trim.
rem -----------------------------------------------------------------------------
set "MISSING_MAPS="
for /f "usebackq delims=" %%L in (`findstr /b /c:"+MapsToCook=" "%TRACE_PROJECT_ROOT%\Config\DefaultGame.ini" 2^>nul`) do (
    set "_line=%%L"
    rem Only /Game/ paths point at a file in this repository. Anything else - an
    rem /Engine/ map, a commented-out line that still matched - is skipped, exactly
    rem as the .sh's `case "$MapPath" in /Game/*)` arm does.
    if not "!_line:/Game/=!"=="!_line!" (
        set "_rel=!_line:*/Game/=!"
        set "_rel=!_rel:~0,-2!"
        set "_relwin=!_rel:/=\!"
        if not exist "%TRACE_PROJECT_ROOT%\Content\!_relwin!.umap" (
            set "MISSING_MAPS=!MISSING_MAPS! /Game/!_rel!"
        )
    )
)
if defined MISSING_MAPS (
    call "%~dp0_trace_common.bat" err "Maps listed in MapsToCook do not exist on disk:!MISSING_MAPS!"
    call "%~dp0_trace_common.bat" err "The cook would warn about these and carry on, and the finished build would fail"
    call "%~dp0_trace_common.bat" err "to open its own default map. Generate them first:"
    call "%~dp0_trace_common.bat" err "    %TRACE_SCRIPT_DIR%\generate-map.bat"
    exit /b 1
)

rem -----------------------------------------------------------------------------
rem  Build the RunUAT command line.
rem -----------------------------------------------------------------------------
set "TRACE_CMD="!RUNUAT!" BuildCookRun -project="%TRACE_UPROJECT%" -noP4 -utf8output -platform=!PLATFORM! -clientconfig=!CONFIG! -stage -package -archive -archivedirectory="!OUTPUT!" -nocompileeditor"

if "%DO_BUILD%"=="1" (set "TRACE_CMD=!TRACE_CMD! -build") else (set "TRACE_CMD=!TRACE_CMD! -skipbuild")
if "%DO_COOK%"=="1"  (set "TRACE_CMD=!TRACE_CMD! -cook")  else (set "TRACE_CMD=!TRACE_CMD! -skipcook")
if "%DO_PAK%"=="1"     set "TRACE_CMD=!TRACE_CMD! -pak"
if "%DO_ITERATE%"=="1" set "TRACE_CMD=!TRACE_CMD! -iterate"
if defined EXTRA_ARGS  set "TRACE_CMD=!TRACE_CMD!!EXTRA_ARGS!"

call "%~dp0_trace_common.bat" msg "Packaging %TRACE_PROJECT_NAME% | !PLATFORM! | !CONFIG! client"
call "%~dp0_trace_common.bat" msg "Output: !OUTPUT!"
call "%~dp0_trace_common.bat" msg "This is slow - a first cook of this project takes tens of minutes. It is not hung."

if not exist "!OUTPUT!" mkdir "!OUTPUT!" >nul 2>&1

rem Fingerprint of the cooked content BEFORE the run - see :cooked_stamp and
rem the gate 2 note in the header.
call :cooked_stamp "!OUTPUT!"
set "STAMP_BEFORE=!COOKED_STAMP!"

call "%~dp0_trace_common.bat" now_seconds
set "START=!TRACE_NOW_SECONDS!"

rem -----------------------------------------------------------------------------
rem  GATE 1 - the exit code.
rem
rem  Run live, unredirected, so a cold-DDC cook streams to the console and nobody
rem  kills it thinking it hung. Same reasoning as Scripts\build.bat: Epic's exit
rem  code is one signal, and this project has already been burned by a tool exiting
rem  0 without ever reaching a verdict, which is what gates 2 and 3 are for.
rem -----------------------------------------------------------------------------
call "%~dp0_trace_common.bat" run
set "RC=!errorlevel!"

if "%TRACE_DRY_RUN%"=="1" exit /b 0

call "%~dp0_trace_common.bat" now_seconds
set /a "ELAPSED=!TRACE_NOW_SECONDS!-!START!"
if !ELAPSED! lss 0 set /a "ELAPSED=!ELAPSED!+86400"

if not "!RC!"=="0" (
    call "%~dp0_trace_common.bat" err "Packaging failed with exit code !RC! after !ELAPSED!s. See the output above."
    call "%~dp0_trace_common.bat" err "The line that matters is usually the first one containing 'ERROR:' or"
    call "%~dp0_trace_common.bat" err "'AutomationException'. Scroll up to it rather than reading from the bottom -"
    call "%~dp0_trace_common.bat" err "UAT prints a long tail of unwinding after the real failure."
    exit /b !RC!
)

rem -----------------------------------------------------------------------------
rem  GATE 2 - is what is in the output directory actually from THIS run?
rem
rem  package.sh answers this by grepping the captured log for UAT's own "BUILD
rem  SUCCESSFUL" verdict, whose absence on a zero exit means UAT never got as far
rem  as having one. There is no captured log here (see the header), so this asks
rem  the question of the artefact instead: the cooked content's fingerprint - every
rem  .utoc and .pak with its timestamp - has to have changed. A run that exits 0
rem  without producing anything leaves the earlier build in place, unchanged, and
rem  this is what catches that.
rem
rem  Only meaningful when a cook was actually asked for. With --skip-cook the
rem  content is SUPPOSED to be from an earlier run, so the check is skipped and
rem  says so rather than firing a false alarm.
rem -----------------------------------------------------------------------------
if "%DO_COOK%"=="1" (
    call :cooked_stamp "!OUTPUT!"
    if defined STAMP_BEFORE if "!COOKED_STAMP!"=="!STAMP_BEFORE!" (
        call "%~dp0_trace_common.bat" err "UAT exited 0, but the cooked content in the output directory has not changed."
        call "%~dp0_trace_common.bat" err "  !OUTPUT!"
        call "%~dp0_trace_common.bat" err "Every .pak and .utoc in there still has the timestamp it had before this run"
        call "%~dp0_trace_common.bat" err "started, so all of it is from an EARLIER run and this one produced nothing."
        call "%~dp0_trace_common.bat" err "Do not ship it. Scroll up for what UAT actually did."
        exit /b 1
    )
) else (
    call "%~dp0_trace_common.bat" msg "--skip-cook: not checking whether the cooked content is fresh."
)

rem -----------------------------------------------------------------------------
rem  GATE 3 - THE ONE THAT ACTUALLY MATTERS.
rem
rem  Gates 1 and 2 are about whether the tool finished. This one is about whether
rem  what it produced can run, and it exists because of a specific measured
rem  failure: the project shipped a Shipping binary that linked cleanly, passed
rem  every check anyone had, and could not start, because there was no cooked
rem  content anywhere on disk (docs\KNOWN_LIMITATIONS.md item 29). A green tool is
rem  not a build.
rem
rem  So: find the staged build, and require cooked content inside it. Under -pak
rem  that is a .pak/.utoc; under --no-pak it is loose .uasset files. Zero of both
rem  means the cooker produced nothing and the "successful" build is a shell that
rem  will exit at launch.
rem -----------------------------------------------------------------------------

rem THE STAGE DIRECTORY IS NOT ALWAYS CALLED Win64, and this is the Windows twin of
rem the .app-name trap that cost package.sh a run. UAT archives a Win64 build into
rem a folder named after the PLATFORM GROUP, not the platform:
rem
rem     UE 5.x            -> <output>\Windows\
rem     UE 4.2x and older -> <output>\WindowsNoEditor\
rem
rem so "Win64" - the name on the command line - is the one spelling that is
rem probably wrong. Try all three, then fall back to whatever single directory is
rem actually there. Listing the directory in the failure arm matters more than the
rem guess; that is what told us the real name on macOS.
set "STAGE="
for %%D in (Windows Win64 WindowsNoEditor) do (
    if not defined STAGE if exist "!OUTPUT!\%%D\" set "STAGE=!OUTPUT!\%%D"
)
if not defined STAGE (
    for /f "delims=" %%D in ('dir /b /ad "!OUTPUT!" 2^>nul') do (
        if not defined STAGE set "STAGE=!OUTPUT!\%%D"
    )
)
if not defined STAGE (
    call "%~dp0_trace_common.bat" err "UAT reported success but there is no staged build under:"
    call "%~dp0_trace_common.bat" err "  !OUTPUT!"
    call "%~dp0_trace_common.bat" err "Contents of !OUTPUT!:"
    dir /s "!OUTPUT!" 1>&2 2>nul
    if errorlevel 1 call "%~dp0_trace_common.bat" err "  the output directory does not exist"
    exit /b 1
)

rem The executable. A staged Windows build puts a launcher next to the stage root
rem and the real binary under <Project>\Binaries\Win64\, and the real binary's name
rem carries the configuration for every config except Development:
rem
rem     Shipping    -> Trace-Win64-Shipping.exe
rem     Test        -> Trace-Win64-Test.exe
rem     Development -> Trace.exe          (the "default" config gets the bare name)
set "EXE="
for %%E in (
    "!STAGE!\%TRACE_PROJECT_NAME%.exe"
    "!STAGE!\%TRACE_PROJECT_NAME%\Binaries\Win64\%TRACE_PROJECT_NAME%-Win64-!CONFIG!.exe"
    "!STAGE!\%TRACE_PROJECT_NAME%\Binaries\Win64\%TRACE_PROJECT_NAME%.exe"
) do (
    if not defined EXE if exist "%%~E" set "EXE=%%~E"
)
if not defined EXE (
    rem Restricted to the project's own binaries directory on purpose: a plain
    rem `dir /s *.exe` over the stage would happily return CrashReportClient.exe
    rem and call the build good.
    for /f "delims=" %%E in ('dir /b "!STAGE!\%TRACE_PROJECT_NAME%\Binaries\Win64\*.exe" 2^>nul') do (
        if not defined EXE set "EXE=!STAGE!\%TRACE_PROJECT_NAME%\Binaries\Win64\%%E"
    )
)
if not defined EXE (
    call "%~dp0_trace_common.bat" err "No game executable in the staged build at:"
    call "%~dp0_trace_common.bat" err "  !STAGE!"
    call "%~dp0_trace_common.bat" err "Contents:"
    dir /s /b "!STAGE!" 1>&2 2>nul
    exit /b 1
)

rem Cooked content, counted. Both spellings, because -pak and --no-pak produce
rem different ones and a check that only knows about the one you happened to run is
rem a check that passes by accident on the other.
set "PAK_COUNT=0"
set "UASSET_COUNT=0"
for /f %%N in ('dir /s /b "!STAGE!\*.pak" "!STAGE!\*.utoc" 2^>nul ^| find /c /v ""') do set "PAK_COUNT=%%N"
for /f %%N in ('dir /s /b "!STAGE!\*.uasset" 2^>nul ^| find /c /v ""') do set "UASSET_COUNT=%%N"

if "!PAK_COUNT!"=="0" if "!UASSET_COUNT!"=="0" (
    call "%~dp0_trace_common.bat" err "The staged build contains NO COOKED CONTENT - zero .pak, zero .utoc, zero .uasset."
    call "%~dp0_trace_common.bat" err "  !STAGE!"
    call "%~dp0_trace_common.bat" err "It will exit immediately at launch. This is exactly the failure recorded as"
    call "%~dp0_trace_common.bat" err "item 29 in docs\KNOWN_LIMITATIONS.md: a build that links, reports success, and"
    call "%~dp0_trace_common.bat" err "cannot start. Check the cook stage of the output above for 'Cook failed' or a"
    call "%~dp0_trace_common.bat" err "missing map."
    exit /b 1
)

call "%~dp0_trace_common.bat" msg "Success in !ELAPSED!s."
call "%~dp0_trace_common.bat" msg "Build:      !STAGE!"
call "%~dp0_trace_common.bat" msg "Executable: !EXE!"
if not "!PAK_COUNT!"=="0" (
    call "%~dp0_trace_common.bat" msg "Cooked content: !PAK_COUNT! pak/utoc files inside the staged build."
) else (
    call "%~dp0_trace_common.bat" msg "Cooked content: !UASSET_COUNT! loose .uasset files inside the staged build."
)

rem Signing status is a distribution fact the recipient will hit within ten seconds
rem of double-clicking, so say it here rather than letting them find out. There is
rem nothing to query: this build is unsigned, unconditionally - UAT does not sign a
rem Windows game and the project has no certificate configured. The macOS twin can
rem at least ask `codesign`; here the answer is known in advance.
call "%~dp0_trace_common.bat" warn "This build is UNSIGNED. Windows SmartScreen will show 'Windows protected your"
call "%~dp0_trace_common.bat" warn "PC' the first time somebody runs it - they must click More info, then Run anyway."
call "%~dp0_trace_common.bat" warn "See docs\PLAYTEST.md for the full instructions to send with it."

call "%~dp0_trace_common.bat" msg "Next: run it with  !EXE!"
call "%~dp0_trace_common.bat" msg "      Zip !STAGE! whole - the .exe alone is not the game."
exit /b 0

rem -----------------------------------------------------------------------------
rem  cooked_stamp <outputDir> -> COOKED_STAMP
rem
rem  A fingerprint of the cooked content under the output directory: every .utoc
rem  and .pak path found there, each with its modification timestamp, concatenated.
rem  Empty when there is none. Gate 2 calls this once before the run and once
rem  after, and only ever asks whether the two strings DIFFER.
rem
rem  IT IS NOT "THE NEWEST FILE", and an earlier draft of this called itself that.
rem  Deliberately not: a fingerprint over the whole set also changes when a file is
rem  added or removed, which a single newest-timestamp would miss on a run that
rem  produced one extra pak and touched nothing else.
rem
rem  %%~tG, not a date library: cmd has no date arithmetic worth the name and none
rem  is needed here, because the string is only ever compared against ITSELF from
rem  earlier in the same process. Its locale-dependent format therefore does not
rem  matter, and neither does the ordering, as long as `dir /s /b` is deterministic
rem  for an unchanged tree - which it is.
rem
rem  ONE THING IT CANNOT SEE: a cook that rewrites identical content inside the
rem  same clock minute, because %%~t has minute granularity. A cook takes minutes,
rem  so this is theoretical; gate 3 is the one that would still catch an empty
rem  build.
rem -----------------------------------------------------------------------------
:cooked_stamp
set "COOKED_STAMP="
for /f "delims=" %%F in ('dir /s /b "%~1\*.utoc" "%~1\*.pak" 2^>nul') do (
    for %%G in ("%%F") do set "COOKED_STAMP=!COOKED_STAMP!|%%~nxG@%%~tG"
)
exit /b 0

rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% package - cook content and produce a runnable game build
echo(
echo USAGE
echo   Scripts\package.bat [options] [-- ^<extra RunUAT args^>]
echo(
echo OPTIONS
echo   -o, --output ^<dir^>      Archive directory for the finished build.
echo                           Default: %DEFAULT_OUTPUT%
echo                           The build lands in ^<dir^>\Windows\.
echo   -c, --config ^<name^>     Client configuration. Default: Shipping
echo                           One of: Debug ^| DebugGame ^| Development ^| Test ^| Shipping
echo                           Use Development when you need logs - Shipping compiles
echo                           logging out, which is why a broken Shipping build is
echo                           silent (docs\KNOWN_LIMITATIONS.md item 29).
echo   -p, --platform ^<name^>   Target platform. Default: this host (%TRACE_HOST_PLATFORM%)
echo       --no-pak            Stage loose cooked files instead of a .pak. Slower to
echo                           load, but you can see and diff what actually cooked.
echo       --skip-cook         Reuse the existing cook. Only valid if one exists.
echo       --skip-build        Do not compile the game target; use what is on disk.
echo       --iterate           Iterative cook - only recook what changed. Much faster
echo                           on a re-run, and occasionally wrong; if a packaged run
echo                           disagrees with the editor, re-cook without this first.
echo   -n, --dry-run           Print the command that would run; run nothing
echo   -h, --help              This text
echo(
echo ENVIRONMENT
echo   UE_ROOT                 Engine install to use. Default on Windows:
echo                           C:\Program Files\Epic Games\UE_%TRACE_ENGINE_VERSION%
echo                           A per-developer override can also live in ^<repo^>\.ue-root
echo(
echo WHAT COMES OUT
echo   ^<output^>\Windows\                                  - zip and send THIS folder
echo   ^<output^>\Windows\%TRACE_PROJECT_NAME%.exe                       - what the player double-clicks
echo   ^<output^>\Windows\%TRACE_PROJECT_NAME%\Content\Paks\*.pak + *.ucas + *.utoc
echo                                                      - the cooked content
echo   The engine content it needs is in there too, so the folder is self-contained
echo   and does not read anything out of your engine install or this repository. The
echo   .exe on its own is NOT the game and will not start.
echo(
echo HOW IT DECIDES IT WORKED
echo   Three gates, all of which must pass, because none of them is sufficient alone:
echo     1. RunUAT exits 0.
echo     2. The cooked content in the output directory is NEWER than it was before
echo        the run. UnrealBuildTool has been observed exiting 0 after a segfault
echo        with no verdict line at all, so an exit code is a hint. (package.sh
echo        greps its captured log for UAT's "BUILD SUCCESSFUL" here; cmd has no
echo        tee, so this asks the artefact instead. Same question.)
echo     3. The staged build exists AND contains cooked content. This is the gate
echo        that matters: gates 1 and 2 both passed historically on builds that
echo        produced no cooked content whatsoever and therefore could not start.
echo(
echo FIRST RUN ON A NEW WINDOWS MACHINE
echo   1. Scripts\build.bat          :: builds TraceEditor; the cook needs it
echo   2. Scripts\package.bat        :: this script
echo   Without step 1 the cook fails minutes in with a module-load error.
echo(
echo   THIS SCRIPT HAS NEVER BEEN RUN ON WINDOWS. It was written on macOS against
echo   Scripts/package.sh, on a machine that cannot execute it. Read the comment
echo   block at the top of the file before concluding the project is broken.
echo(
echo EXAMPLES
echo   Scripts\package.bat                          :: Shipping build, default output
echo   Scripts\package.bat -o D:\trace-build        :: somewhere else
echo   Scripts\package.bat -c Development           :: a build that can print a log
echo   Scripts\package.bat --iterate                :: fast re-cook after a change
echo   Scripts\package.bat -n                       :: show the RunUAT command, run nothing
echo(
echo AFTERWARDS
echo   Sending it to somebody? The build is UNSIGNED - SmartScreen will block it and
echo   Windows Defender Firewall will prompt the first time somebody hosts. Read
echo   docs\PLAYTEST.md; both have one-time fixes and the firewall one is the
echo   difference between a joinable game and a silent one.
exit /b 0
