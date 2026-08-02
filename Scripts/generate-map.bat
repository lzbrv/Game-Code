@echo off
rem =============================================================================
rem  Trace - generate-map.bat   (Windows twin of generate-map.sh)
rem
rem  Creates the empty /Game/Maps/Arena level headlessly by driving
rem  Scripts\generate_map.py through Unreal's pythonscript commandlet. Nobody
rem  should have to click File ^> New Level ^> Empty Level ^> Save As to bootstrap
rem  a clone.
rem
rem  The command it runs (printed before every launch):
rem
rem    "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<repo>\Trace.uproject" ^^
rem        -run=pythonscript -script="<repo>\Scripts\generate_map.py" ^^
rem        -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
rem
rem  generate_map.py is shared verbatim with macOS and Linux; only the binary and
rem  the path separators differ. It reads TRACE_MAP_PATH and TRACE_FORCE_MAP from
rem  the environment, because -script= gives no reliable way to pass argv.
rem
rem  The arena itself is built in C++ by ATraceArenaBuilder at BeginPlay, so the
rem  .umap this produces is deliberately, permanently empty.
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "MAP=%TRACE_DEFAULT_MAP%"
set "FORCE=0"
set "NULL_RHI=0"
set "EXTRA_ARGS="

rem -----------------------------------------------------------------------------
rem  Argument parsing
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="-m"        goto :o_map
if /i "!_a!"=="--map"     goto :o_map
if /i "!_a!"=="-f"        goto :o_force
if /i "!_a!"=="--force"   goto :o_force
if /i "!_a!"=="--nullrhi" goto :o_nullrhi
if /i "!_a!"=="-n"        goto :o_dryrun
if /i "!_a!"=="--dry-run" goto :o_dryrun
if /i "!_a!"=="-h"        goto :o_help
if /i "!_a!"=="--help"    goto :o_help
if /i "!_a!"=="/?"        goto :o_help
if /i "!_a!"=="--"        goto :o_extra
if "!_a:~0,1!"=="-"       goto :unknown_option
goto :unexpected_arg

:o_map
if "%~2"=="" goto :need_map_value
set "MAP=%~2"
shift
shift
goto :parse
:need_map_value
call "%~dp0_trace_common.bat" err "--map needs a value"
exit /b 2

:o_force
set "FORCE=1"
shift
goto :parse

:o_nullrhi
set "NULL_RHI=1"
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

if /i not "!MAP:~0,6!"=="/Game/" (
    call "%~dp0_trace_common.bat" err "--map must be a /Game/... package path, got '!MAP!'."
    exit /b 1
)

call "%~dp0_trace_common.bat" require_uproject
if errorlevel 1 exit /b 1
call "%~dp0_trace_common.bat" resolve_engine
if errorlevel 1 exit /b 1

set "PY_SCRIPT=%TRACE_SCRIPT_DIR%\generate_map.py"
if not exist "!PY_SCRIPT!" (
    call "%~dp0_trace_common.bat" err "generate_map.py not found at !PY_SCRIPT!"
    exit /b 1
)

rem Trace.uproject is owned by the build agent; only warn, never edit it here.
findstr /c:"PythonScriptPlugin" "%TRACE_UPROJECT%" >nul 2>&1
if errorlevel 1 (
    call "%~dp0_trace_common.bat" warn "Trace.uproject does not mention PythonScriptPlugin."
    call "%~dp0_trace_common.bat" warn "If this run fails with 'could not be found' for the pythonscript commandlet,"
    call "%~dp0_trace_common.bat" warn "enable Edit > Plugins > Python Editor Script Plugin (or add it to Trace.uproject)."
)

call "%~dp0_trace_common.bat" editor_cmd_binary
if errorlevel 1 exit /b 1

rem generate_map.py reads these two; -script= gives no reliable way to pass argv.
set "TRACE_MAP_PATH=!MAP!"
set "TRACE_FORCE_MAP=!FORCE!"

set "TRACE_CMD="!TRACE_EDITOR_CMD_BIN!" "%TRACE_UPROJECT%" -run=pythonscript -script="!PY_SCRIPT!" -unattended -nosplash -nopause -stdout -FullStdOutLogOutput"
if "%NULL_RHI%"=="1" set "TRACE_CMD=!TRACE_CMD! -nullrhi"
if defined EXTRA_ARGS set "TRACE_CMD=!TRACE_CMD!!EXTRA_ARGS!"

call "%~dp0_trace_common.bat" msg "Creating !MAP! (TRACE_FORCE_MAP=!FORCE!)"
call "%~dp0_trace_common.bat" run
set "RC=!errorlevel!"

if "%TRACE_DRY_RUN%"=="1" exit /b 0

rem /Game/Maps/Arena -> Content\Maps\Arena.umap
set "REL=!MAP:~6!"
set "REL=!REL:/=\!"
if exist "%TRACE_PROJECT_ROOT%\Content\!REL!.umap" (
    call "%~dp0_trace_common.bat" msg "Wrote Content\!REL!.umap"
    call "%~dp0_trace_common.bat" msg "Next: Scripts\run-listen-server.bat"
    exit /b 0
)

call "%~dp0_trace_common.bat" err "Commandlet finished (exit code !RC!) but Content\!REL!.umap is not on disk."
call "%~dp0_trace_common.bat" err "Scroll up for the [Trace] lines from generate_map.py - they name the API that failed."
call "%~dp0_trace_common.bat" err "Manual fallback: open the editor, File > New Level > Empty Level, Save As Content\!REL!."
exit /b 1

rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% map generation
echo(
echo USAGE
echo   Scripts\generate-map.bat [options] [-- ^<extra engine args^>]
echo(
echo OPTIONS
echo   -m, --map ^<path^>    Package path to create. Default: %TRACE_DEFAULT_MAP%
echo   -f, --force         Delete and recreate the map if it already exists
echo       --nullrhi       Add -nullrhi (faster, headless; drop it if creation fails)
echo   -n, --dry-run       Print the command; run nothing
echo   -h, --help          This text
echo(
echo ENVIRONMENT
echo   UE_ROOT             Engine install. Default on Windows:
echo                       C:\Program Files\Epic Games\UE_%TRACE_ENGINE_VERSION%
echo(
echo REQUIRES
echo   * PythonScriptPlugin enabled in Trace.uproject (otherwise the commandlet
echo     does not exist and the run fails immediately).
echo   * A built editor target - run Scripts\build.bat first on a fresh clone.
echo(
echo WHAT IT WRAPS
echo   "%%UE_ROOT%%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^^
echo       "%TRACE_UPROJECT%" ^^
echo       -run=pythonscript -script="%TRACE_SCRIPT_DIR%\generate_map.py" ^^
echo       -unattended -nosplash -nopause -stdout -FullStdOutLogOutput
echo(
echo EXAMPLES
echo   Scripts\generate-map.bat
echo   Scripts\generate-map.bat --force
echo   Scripts\generate-map.bat --map /Game/Maps/ArenaTest
exit /b 0
