@echo off
rem =============================================================================
rem  Trace - dump-stats.bat   (Windows twin of dump-stats.sh)
rem
rem  Regenerates docs\TraceStats.csv: every stat in the game, read out of the
rem  running game by Trace.DumpStats (Source\Trace\Debug\TraceStatsDump.cpp).
rem
rem  RUN IT FROM A TERMINAL (cmd or PowerShell), IN THE REPO:
rem
rem      Scripts\dump-stats.bat                       writes docs\TraceStats.csv
rem      Scripts\dump-stats.bat C:\Temp\stats.csv     writes there instead
rem      Scripts\dump-stats.bat --build               build the editor first, then dump
rem
rem  It runs this (and prints it first, with your engine path filled in):
rem
rem    "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" "<repo>\Trace.uproject" ^^
rem        /Game/Maps/Arena_Baked?game=/Script/Trace.TracePracticeGameMode ^^
rem        -game -log -nullrhi -RenderOffScreen -unattended -nosound -nosplash ^^
rem        -TraceExec="Trace.DumpStats docs/TraceStats.csv|Trace.VerifyStats docs/TraceStats.csv|quit" ^^
rem        -TraceExecAt=6 -TraceExecOn=Match -abslog="<repo>\Saved\Logs\dump-stats.log"
rem
rem  That is the practice range, headless. Six seconds into the match it writes
rem  the sheet, re-reads and parses it, and quits. Under a minute.
rem
rem  DEV / EDITOR BUILDS ONLY. It runs your EDITOR build of Trace (what
rem  Scripts\build.bat makes), not a packaged game: Trace.DumpStats is compiled
rem  out of Shipping. "Trace.DumpStats" typed at a command prompt does nothing -
rem  it is a console command inside the game, and this script is how a terminal
rem  reaches it.
rem
rem  It stops, with the reason, when:
rem    * no Unreal Engine is found        (UE_ROOT, .ue-root, registry, install folders)
rem    * there is no editor build         (Scripts\build.bat, or pass --build)
rem    * the baked arena is missing, or Content\ still holds Git LFS pointer files
rem                                       (git lfs pull)
rem    * the run does not end with both verdicts PASS (log: Saved\Logs\dump-stats.log)
rem  and it warns when the editor build is older than Source\, because the sheet
rem  shows the numbers of the build that RAN, not of the tree.
rem
rem  NOT YET RUN ON WINDOWS. Written on a Mac as a step-for-step mirror of
rem  dump-stats.sh (which was run for real), on top of _trace_common.bat. One
rem  deliberate difference: no time budget. cmd.exe cannot stop only the one
rem  process it started, so this waits for the game to quit on its own. If it
rem  ever hangs, close the game's log window (or end UnrealEditor.exe in Task
rem  Manager) and read Saved\Logs\dump-stats.log.
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "MAP=%TRACE_DEFAULT_MAP%"
set "OUT="
set "DO_BUILD=0"

rem -----------------------------------------------------------------------------
rem  Argument parsing
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="-b"        goto :o_build
if /i "!_a!"=="--build"   goto :o_build
if /i "!_a!"=="--budget"  goto :o_budget
if /i "!_a!"=="-n"        goto :o_dryrun
if /i "!_a!"=="--dry-run" goto :o_dryrun
if /i "!_a!"=="-h"        goto :o_help
if /i "!_a!"=="--help"    goto :o_help
if /i "!_a!"=="/?"        goto :o_help
if "!_a:~0,1!"=="-"       goto :unknown_option
if defined OUT            goto :two_paths
rem %~f1 makes it absolute against the folder you ran this from, as the .sh does.
set "OUT=%~f1"
shift
goto :parse

:o_build
set "DO_BUILD=1"
shift
goto :parse

:o_budget
if "%~2"=="" goto :need_budget_value
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" warn "--budget is ignored on Windows: this script waits for the game to quit on its own."
shift
shift
goto :parse
:need_budget_value
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "--budget needs a value"
exit /b 2

:o_dryrun
set "TRACE_DRY_RUN=1"
shift
goto :parse

:o_help
call :usage
exit /b 0

:two_paths
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "One output path only; got '!OUT!' and '!_a!'."
echo(
call :usage
exit /b 2

:unknown_option
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "Unknown option: !_a!"
echo(
call :usage
exit /b 2

:parsed

rem -----------------------------------------------------------------------------
rem  Where the sheet goes. A path inside the repo is handed to the game
rem  repo-relative, with / separators (Unreal takes them on Windows): that keeps
rem  the sheet's own "Written to" row free of whose machine ran it, and keeps
rem  spaces in the clone's location out of -TraceExec.
rem -----------------------------------------------------------------------------
if not defined OUT set "OUT=%TRACE_PROJECT_ROOT%\docs\TraceStats.csv"
set "OUT=!OUT:/=\!"
if exist "!OUT!\" (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "!OUT! is a folder. Give a file name, e.g. !OUT!\TraceStats.csv"
    exit /b 1
)
for %%I in ("!OUT!") do set "OUT_DIR=%%~dpI"
if not exist "!OUT_DIR!" mkdir "!OUT_DIR!" >nul 2>&1
if not exist "!OUT_DIR!" (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "Cannot create the folder for the sheet: !OUT_DIR!"
    exit /b 1
)

set "GAME_PATH=!OUT!"
set "_rel=!OUT:%TRACE_PROJECT_ROOT%\=!"
if /i "%TRACE_PROJECT_ROOT%\!_rel!"=="!OUT!" set "GAME_PATH=!_rel!"
set "GAME_PATH=!GAME_PATH:\=/!"

rem -----------------------------------------------------------------------------
rem  Pre-flight. Each of these otherwise fails INSIDE the game, as a headless run
rem  that never reaches the dump and leaves nothing on screen to explain why.
rem -----------------------------------------------------------------------------
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" require_uproject
if errorlevel 1 exit /b 1
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" resolve_engine
if errorlevel 1 exit /b 1

if "%DO_BUILD%"=="1" (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" msg "Building the editor target first (--build)"
    call "%TRACE_SCRIPT_DIR%\build.bat" --target TraceEditor --config Development
    if errorlevel 1 exit /b 1
)

call "%TRACE_SCRIPT_DIR%\_trace_common.bat" editor_binary
if errorlevel 1 exit /b 1

rem --- the map, and Git LFS -----------------------------------------------------
rem  The baked arena is a small .umap plus one .uasset per actor, and every one
rem  of them is in Git LFS. A clone made without git-lfs, or not yet pulled, has
rem  ~130-byte text POINTER files in their place; the engine reads those as
rem  broken packages, the map never loads as a match, and the dump never runs.
set "MAP_REL=!MAP:~6!"
set "MAP_REL=!MAP_REL:/=\!"
if not exist "%TRACE_PROJECT_ROOT%\Content\!MAP_REL!.umap" (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The map is missing: Content\!MAP_REL!.umap"
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "It is committed (in Git LFS), so this clone is incomplete. From the repo:"
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "    git lfs install && git lfs pull        (first time: Scripts\setup-lfs.bat)"
    exit /b 1
)
set "LFS_COUNT=0"
set "LFS_FIRST="
for /r "%TRACE_PROJECT_ROOT%\Content" %%F in (*.uasset *.umap) do (
    if %%~zF LSS 400 (
        findstr /m /l /c:"git-lfs.github.com/spec" "%%F" >nul 2>&1
        if not errorlevel 1 (
            set /a LFS_COUNT+=1
            if not defined LFS_FIRST set "LFS_FIRST=%%F"
        )
    )
)
if !LFS_COUNT! GTR 0 (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "!LFS_COUNT! asset(s) under Content\ are Git LFS POINTER files, not the real assets. For example:"
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "    !LFS_FIRST!"
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The game cannot load the arena from those. Fetch the real files, from the repo:"
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "    git lfs install && git lfs pull        (first time: Scripts\setup-lfs.bat)"
    exit /b 1
)

rem --- the editor build ---------------------------------------------------------
rem  UnrealEditor.modules names the DLL the engine will load (the .sh reads the
rem  same file). The newest UnrealEditor-Trace*.dll that it names is the build
rem  this run will use.
set "BIN_DIR=%TRACE_PROJECT_ROOT%\Binaries\%TRACE_HOST_PLATFORM%"
set "MODULES_FILE=%BIN_DIR%\UnrealEditor.modules"
set "MODULE_LIB="
if exist "%MODULES_FILE%" (
    for /f "delims=" %%D in ('dir /b /a-d /o-d "%BIN_DIR%\UnrealEditor-%TRACE_PROJECT_NAME%*.dll" 2^>nul') do (
        if not defined MODULE_LIB (
            findstr /l /c:"%%D" "%MODULES_FILE%" >nul 2>&1
            if not errorlevel 1 set "MODULE_LIB=%BIN_DIR%\%%D"
        )
    )
)
if not defined MODULE_LIB (
    if "%TRACE_DRY_RUN%"=="1" (
        call "%TRACE_SCRIPT_DIR%\_trace_common.bat" warn "No editor build of %TRACE_PROJECT_NAME% yet (Binaries\%TRACE_HOST_PLATFORM%\UnrealEditor.modules); a real run would stop here."
    ) else (
        call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "There is no editor build of %TRACE_PROJECT_NAME% on this machine:"
        call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "    Binaries\%TRACE_HOST_PLATFORM%\UnrealEditor.modules is missing, or the DLL it names is."
        call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The stats are read out of the running game, so build it first:"
        call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "    Scripts\build.bat               (or: Scripts\dump-stats.bat --build)"
        exit /b 1
    )
)

rem  Older than Source\? cmd.exe cannot compare file times across a tree, so ask
rem  PowerShell (on every Windows 10/11). The paths travel as environment
rem  variables, never through cmd's quoting. No PowerShell: no warning, no harm.
set "STALE_SOURCE="
if defined MODULE_LIB (
    set "TRACE_SRC_DIR=%TRACE_PROJECT_ROOT%\Source"
    for /f "usebackq delims=" %%S in (`powershell -NoProfile -NonInteractive -Command "$t = (Get-Item -LiteralPath $env:MODULE_LIB).LastWriteTimeUtc; Get-ChildItem -LiteralPath $env:TRACE_SRC_DIR -Recurse -File | Where-Object { $_.LastWriteTimeUtc -gt $t } | Select-Object -First 1 -ExpandProperty FullName" 2^>nul`) do set "STALE_SOURCE=%%S"
)
if defined STALE_SOURCE call :warn_stale

rem -----------------------------------------------------------------------------
rem  The run
rem -----------------------------------------------------------------------------
set "LOG_DIR=%TRACE_PROJECT_ROOT%\Saved\Logs"
set "LOG=%LOG_DIR%\dump-stats.log"

rem ?game= is what makes this the practice range; it stays glued to the map path.
set "URL=!MAP!?game=/Script/Trace.TracePracticeGameMode"
rem The list lives in its own variable so its | characters are only ever
rem expanded late (!...!), never parsed by cmd as pipes.
set "EXEC_LIST=Trace.DumpStats !GAME_PATH!|Trace.VerifyStats !GAME_PATH!|quit"
set "TRACE_CMD="!TRACE_EDITOR_BIN!" "!TRACE_UPROJECT!" !URL! -game -log -nullrhi -RenderOffScreen -unattended -nosound -nosplash -TraceExec="!EXEC_LIST!" -TraceExecAt=6 -TraceExecOn=Match -abslog="!LOG!""

call "%TRACE_SCRIPT_DIR%\_trace_common.bat" msg "Writing every stat in the game to !OUT!"
if "%TRACE_DRY_RUN%"=="1" (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" run
    exit /b 0
)

if not exist "!LOG_DIR!" mkdir "!LOG_DIR!" >nul 2>&1
if exist "!LOG!" del /q "!LOG!" >nul 2>&1
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" msg "The game runs headless (a log window may open); this waits for it to quit, usually under a minute."
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" run
set "GAME_RC=!errorlevel!"

rem -----------------------------------------------------------------------------
rem  The verdict
rem -----------------------------------------------------------------------------
set "DUMP_VERDICT="
set "VERIFY_VERDICT="
set "ROWS_LINE="
if exist "%LOG%" (
    for /f "delims=" %%L in ('findstr /l /c:"[DumpStats] VERDICT:" "%LOG%" 2^>nul') do set "DUMP_VERDICT=%%L"
    for /f "delims=" %%L in ('findstr /l /c:"[VerifyStats] VERDICT:" "%LOG%" 2^>nul') do set "VERIFY_VERDICT=%%L"
    for /f "delims=" %%L in ('findstr /l /c:" sections -" "%LOG%" 2^>nul') do set "ROWS_LINE=%%L"
)
call :strip_prefix DUMP_VERDICT
call :strip_prefix VERIFY_VERDICT
call :strip_prefix ROWS_LINE

set "DUMP_OK=0"
set "VERIFY_OK=0"
if defined DUMP_VERDICT if not "!DUMP_VERDICT:VERDICT: PASS=!"=="!DUMP_VERDICT!" set "DUMP_OK=1"
if defined VERIFY_VERDICT if not "!VERIFY_VERDICT:VERDICT: PASS=!"=="!VERIFY_VERDICT!" set "VERIFY_OK=1"

set "DUMP_SHOW=(no verdict)"
set "VERIFY_SHOW=(no verdict)"
if defined DUMP_VERDICT set "DUMP_SHOW=!DUMP_VERDICT:[DumpStats] =!"
if defined VERIFY_VERDICT set "VERIFY_SHOW=!VERIFY_VERDICT:[VerifyStats] =!"

echo(
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" msg "Trace.DumpStats    !DUMP_SHOW!"
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" msg "Trace.VerifyStats  !VERIFY_SHOW!"
if exist "%LOG%" (
    for /f "delims=" %%L in ('findstr /l /c:"[DumpStats]   MISSING" "%LOG%" 2^>nul') do (
        set "_l=%%L"
        call :strip_prefix _l
        echo     !_l!
    )
)

if "!DUMP_OK!!VERIFY_OK!"=="11" if exist "!OUT!" goto :success
goto :failure

:success
set "ROWS="
if defined ROWS_LINE for /f "tokens=1" %%R in ("!ROWS_LINE:*[DumpStats] =!") do set "ROWS=%%R"
echo(
if defined ROWS (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" msg "Wrote !OUT!  (!ROWS! rows)"
) else (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" msg "Wrote !OUT!"
)
if /i "!GAME_PATH!"=="docs/TraceStats.csv" call "%TRACE_SCRIPT_DIR%\_trace_common.bat" msg "That is the committed copy: 'git diff --stat docs/TraceStats.csv' shows whether anything moved."
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" msg "Google Sheets: File > Import > Upload, then Replace spreadsheet."
if defined STALE_SOURCE (
    echo(
    call :warn_stale
)
exit /b 0

rem -----------------------------------------------------------------------------
rem  It failed. Say why, as specifically as the log allows.
rem -----------------------------------------------------------------------------
:failure
echo(
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "No usable sheet. !OUT! was not written and checked by this run."
if not exist "%LOG%" (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The game never got as far as writing a log (exit code !GAME_RC!)."
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "Run the command printed above by hand to see what it says."
    exit /b 1
)
findstr /l /c:"could not be found" /c:"Incompatible or missing module" /c:"modules are missing or built with a different engine" "%LOG%" >nul 2>&1
if not errorlevel 1 (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The editor build of %TRACE_PROJECT_NAME% would not load - missing, or built for another engine."
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "    Scripts\build.bat               (or: Scripts\dump-stats.bat --build)"
    goto :fail_tail
)
findstr /l /c:"Failed to load map" /c:"Couldn't find file for package !MAP!" "%LOG%" >nul 2>&1
if not errorlevel 1 (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The map !MAP! did not load. If git has it as LFS pointers: git lfs pull"
    goto :fail_tail
)
findstr /l /c:"[AutoExec] Armed" "%LOG%" >nul 2>&1
if errorlevel 1 (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The match never started, so the dump was never run."
    goto :fail_tail
)
if not defined DUMP_VERDICT (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The match started but Trace.DumpStats did not run. Is this a Shipping or Test build? The command"
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "exists in Development/DebugGame editor builds only."
    goto :fail_tail
)
if "!DUMP_OK!"=="0" (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The dump ran but is INCOMPLETE: a knob or settings class it reads by name is gone (listed above)."
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The cells that needed it say MISSING KNOB instead of a number. Fix the name in"
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "Source\Trace\Debug\TraceStatsDump.cpp and run this again."
    goto :fail_tail
)
if "!VERIFY_OK!"=="0" (
    call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "The file was written but does not parse as a clean table:"
    for /f "delims=" %%L in ('findstr /l /c:"[VerifyStats]" "%LOG%" 2^>nul') do (
        set "_l=%%L"
        call :strip_prefix _l
        >&2 echo     !_l!
    )
)
:fail_tail
set "_n=0"
for /f "delims=" %%L in ('findstr /l /c:"Error:" /c:"Fatal:" "%LOG%" 2^>nul ^| findstr /v /l /c:"UnifiedErrorTest"') do (
    if !_n! LSS 8 (
        if !_n! EQU 0 call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "First errors in the log:"
        set "_l=%%L"
        call :strip_prefix _l
        >&2 echo     !_l!
    )
    set /a _n+=1
)
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" err "Full log: %LOG%"
exit /b 1

rem -----------------------------------------------------------------------------
rem  Subroutines. Like every line after init, these reach the library through
rem  TRACE_SCRIPT_DIR, never %~dp0 (see the top of _trace_common.bat).
rem -----------------------------------------------------------------------------

rem  strip_prefix <var> - "[2026.10.04-14.36.38:879][875]LogTraceGame: Display: [DumpStats] ..."
rem  becomes "[DumpStats] ...". Lines from other log categories keep their category.
:strip_prefix
if not defined %~1 exit /b 0
set "_s=!%~1!"
if "!_s:~0,1!"=="[" for /f "tokens=2,* delims=]" %%A in ("!_s!") do set "_s=%%B"
if "!_s:~0,14!"=="LogTraceGame: " (
    set "_s=!_s:~14!"
    for /f "tokens=1,*" %%A in ("!_s!") do set "_s=%%B"
)
set "%~1=!_s!"
exit /b 0

:warn_stale
set "_st=!STALE_SOURCE:%TRACE_PROJECT_ROOT%\=!"
set "_ml=!MODULE_LIB:%TRACE_PROJECT_ROOT%\=!"
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" warn "The editor build is OLDER than the source: !_st!"
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" warn "changed after !_ml! was built. The sheet shows the numbers of"
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" warn "the build that runs, not of the code you have. For the current numbers:"
call "%TRACE_SCRIPT_DIR%\_trace_common.bat" warn "    Scripts\dump-stats.bat --build"
exit /b 0

:usage
echo %TRACE_PROJECT_NAME% stat sheet: every stat in the game, as a CSV
echo(
echo USAGE
echo   Scripts\dump-stats.bat [options] [^<output.csv^>]
echo(
echo   ^<output.csv^>     Where to write it. Default: docs\TraceStats.csv (the committed
echo                    copy). A relative path is relative to where you run this.
echo(
echo OPTIONS
echo   -b, --build      Build the editor target first (Scripts\build.bat), so the sheet
echo                    has the numbers of the code you have checked out
echo   -n, --dry-run    Print the command; run nothing
echo   -h, --help       This text
echo(
echo NOTES
echo   * Run it from a terminal, not from inside the game.
echo   * Dev/editor builds only: it runs the editor build of Trace with -game.
echo     A packaged Shipping build has no Trace.DumpStats at all.
echo   * Inside a running dev build, the console (`) takes the command directly:
echo     Trace.DumpStats [^<path^>], then Trace.VerifyStats [^<path^>]. On its own it
echo     writes Saved\Stats\TraceStats.csv.
echo   * No --budget here (dump-stats.sh has one): this waits for the game to quit.
echo   * Mac/Linux: Scripts/dump-stats.sh, same arguments.
exit /b 0
