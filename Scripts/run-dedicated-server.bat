@echo off
rem =============================================================================
rem  Trace - run-dedicated-server.bat   (Windows twin of run-dedicated-server.sh)
rem
rem  Builds (unless --no-build) and runs the TraceServer target: a headless,
rem  authoritative server with no local player. This is the configuration that
rem  catches the bugs a listen server hides - anything that only works because
rem  the server also happened to be a client (local pawn assumptions, cosmetic
rem  code running on authority, missing HasAuthority() guards) breaks here first.
rem
rem  READ THIS FIRST
rem  ---------------
rem  A LAUNCHER-INSTALLED ENGINE CANNOT BUILD A DEDICATED-SERVER TARGET. Not on
rem  Windows, not on macOS. UnrealBuildTool refuses with:
rem
rem      Server targets are not currently supported from this engine distribution.
rem
rem  That is a property of the engine distribution, not a bug in
rem  TraceServer.Target.cs, and no combination of flags works around it. Building
rem  a real dedicated server needs a SOURCE build of Unreal from
rem  github.com/EpicGames/UnrealEngine, which in turn needs an Epic account linked
rem  to a GitHub account for access to that private repository.
rem
rem  Until someone sets that up, the working paths are:
rem      Scripts\run-listen-server.bat              one player hosts  (recommended)
rem      Scripts\run-dedicated-server.bat --editor  editor binary with -server
rem
rem  --editor is a genuine dedicated server (NM_DedicatedServer, no local player,
rem  no rendering); it just runs inside UnrealEditor-Cmd.exe instead of a
rem  purpose-built TraceServer.exe, so it needs no server-target build.
rem
rem  The commands it runs (printed before every step):
rem
rem    "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" ^^
rem        TraceServer Win64 Development "<repo>\Trace.uproject" -waitmutex
rem
rem    "<repo>\Binaries\Win64\TraceServer.exe" "<repo>\Trace.uproject" ^^
rem        /Game/Maps/Arena -log -port=7777
rem
rem  With --editor it instead runs:
rem
rem    "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^^
rem        "<repo>\Trace.uproject" /Game/Maps/Arena -server -log -port=7777
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "MAP=%TRACE_DEFAULT_MAP%"
set "PORT=%TRACE_DEFAULT_PORT%"
set "CONFIG=Development"
set "DO_BUILD=1"
set "USE_EDITOR=0"
set "EXTRA_ARGS="

rem -----------------------------------------------------------------------------
rem  Argument parsing
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="-m"         goto :o_map
if /i "!_a!"=="--map"      goto :o_map
if /i "!_a!"=="--port"     goto :o_port
if /i "!_a!"=="-c"         goto :o_config
if /i "!_a!"=="--config"   goto :o_config
if /i "!_a!"=="--no-build" goto :o_nobuild
if /i "!_a!"=="--editor"   goto :o_editor
if /i "!_a!"=="-n"         goto :o_dryrun
if /i "!_a!"=="--dry-run"  goto :o_dryrun
if /i "!_a!"=="-h"         goto :o_help
if /i "!_a!"=="--help"     goto :o_help
if /i "!_a!"=="/?"         goto :o_help
if /i "!_a!"=="--"         goto :o_extra
if "!_a:~0,1!"=="-"        goto :unknown_option
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

:o_port
if "%~2"=="" goto :need_port_value
set "PORT=%~2"
shift
shift
goto :parse
:need_port_value
call "%~dp0_trace_common.bat" err "--port needs a value"
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

:o_nobuild
set "DO_BUILD=0"
shift
goto :parse

:o_editor
set "USE_EDITOR=1"
set "DO_BUILD=0"
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
rem  The warning that saves an hour. Printed BEFORE anything slow happens, and
rem  only suppressed for --editor, which is the path that actually works.
rem -----------------------------------------------------------------------------
if not "%USE_EDITOR%"=="1" call :launcher_engine_warning

call "%~dp0_trace_common.bat" is_number "!PORT!"
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "--port must be a number, got '!PORT!'"
    exit /b 1
)
call "%~dp0_trace_common.bat" is_config "!CONFIG!"
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "Unknown configuration '!CONFIG!'. Expected Debug, DebugGame, Development, Test or Shipping."
    exit /b 1
)

call "%~dp0_trace_common.bat" require_uproject
if errorlevel 1 exit /b 1
call "%~dp0_trace_common.bat" resolve_engine
if errorlevel 1 exit /b 1
call "%~dp0_trace_common.bat" warn_if_map_missing "!MAP!"

if "%USE_EDITOR%"=="1" goto :path_editor

rem -----------------------------------------------------------------------------
rem  Path B - the real TraceServer target.
rem -----------------------------------------------------------------------------
if "%DO_BUILD%"=="1" (
    rem build.bat announces the target/platform/config itself, so do not duplicate it.
    call "%TRACE_SCRIPT_DIR%\build.bat" --target TraceServer --config "!CONFIG!"
    if errorlevel 1 exit /b 1
)

rem UnrealBuildTool names the output "TraceServer.exe" for the Development config
rem and "TraceServer-Win64-<Config>.exe" for every other one.
set "BIN_DIR=%TRACE_PROJECT_ROOT%\Binaries\%TRACE_HOST_PLATFORM%"
if /i "!CONFIG!"=="Development" (
    set "BASE=TraceServer"
) else (
    set "BASE=TraceServer-%TRACE_HOST_PLATFORM%-!CONFIG!"
)

set "SERVER_BIN="
if exist "!BIN_DIR!\!BASE!.exe" set "SERVER_BIN=!BIN_DIR!\!BASE!.exe"
if not defined SERVER_BIN if exist "!BIN_DIR!\!BASE!-Cmd.exe" set "SERVER_BIN=!BIN_DIR!\!BASE!-Cmd.exe"

if not defined SERVER_BIN (
    rem Nothing was built in a dry run, so an absent binary is expected there.
    if "%TRACE_DRY_RUN%"=="1" (
        set "SERVER_BIN=!BIN_DIR!\!BASE!.exe"
    ) else (
        goto :no_server_binary
    )
)

set "TRACE_CMD="!SERVER_BIN!" "%TRACE_UPROJECT%" !MAP! -log -nosplash -port=!PORT!"
if defined EXTRA_ARGS set "TRACE_CMD=!TRACE_CMD!!EXTRA_ARGS!"
call "%~dp0_trace_common.bat" msg "Dedicated server on port !PORT!, map !MAP!"
call "%~dp0_trace_common.bat" run
exit /b %errorlevel%

:no_server_binary
call "%~dp0_trace_common.bat" err "No dedicated-server binary found. Looked for:"
call "%~dp0_trace_common.bat" err "  !BIN_DIR!\!BASE!.exe"
call "%~dp0_trace_common.bat" err "  !BIN_DIR!\!BASE!-Cmd.exe"
call "%~dp0_trace_common.bat" err ""
call "%~dp0_trace_common.bat" err "On a launcher-installed engine this is EXPECTED - the server target cannot"
call "%~dp0_trace_common.bat" err "be built at all. Use one of these instead:"
call "%~dp0_trace_common.bat" err "  Scripts\run-listen-server.bat"
call "%~dp0_trace_common.bat" err "  Scripts\run-dedicated-server.bat --editor"
call "%~dp0_trace_common.bat" err ""
call "%~dp0_trace_common.bat" err "With a SOURCE build of the engine, build it with:"
call "%~dp0_trace_common.bat" err "  Scripts\build.bat --target TraceServer --config !CONFIG!"
exit /b 1

rem -----------------------------------------------------------------------------
rem  Path A - editor binary in dedicated-server mode. Needs no server build, so
rem  it works on a stock launcher engine.
rem -----------------------------------------------------------------------------
:path_editor
call "%~dp0_trace_common.bat" editor_cmd_binary
if errorlevel 1 exit /b 1
set "TRACE_CMD="!TRACE_EDITOR_CMD_BIN!" "%TRACE_UPROJECT%" !MAP! -server -log -nosplash -port=!PORT!"
if defined EXTRA_ARGS set "TRACE_CMD=!TRACE_CMD!!EXTRA_ARGS!"
call "%~dp0_trace_common.bat" msg "Dedicated server (editor binary) on port !PORT!, map !MAP!"
call "%~dp0_trace_common.bat" run
exit /b %errorlevel%

rem -----------------------------------------------------------------------------
:launcher_engine_warning
call "%~dp0_trace_common.bat" warn "==================================================================="
call "%~dp0_trace_common.bat" warn "A LAUNCHER-INSTALLED ENGINE CANNOT BUILD A DEDICATED-SERVER TARGET."
call "%~dp0_trace_common.bat" warn "==================================================================="
call "%~dp0_trace_common.bat" warn "UnrealBuildTool refuses with:"
call "%~dp0_trace_common.bat" warn "    Server targets are not currently supported from this engine distribution."
call "%~dp0_trace_common.bat" warn "This applies to Windows exactly as it does to macOS. It is a property of"
call "%~dp0_trace_common.bat" warn "the engine DISTRIBUTION, not a bug in TraceServer.Target.cs, and there is"
call "%~dp0_trace_common.bat" warn "no flag that works around it."
call "%~dp0_trace_common.bat" warn ""
call "%~dp0_trace_common.bat" warn "What to do instead:"
call "%~dp0_trace_common.bat" warn "  Scripts\run-listen-server.bat              one player hosts (recommended)"
call "%~dp0_trace_common.bat" warn "  Scripts\run-dedicated-server.bat --editor  real dedicated server, no build"
call "%~dp0_trace_common.bat" warn ""
call "%~dp0_trace_common.bat" warn "A true TraceServer.exe needs a SOURCE build of Unreal from"
call "%~dp0_trace_common.bat" warn "github.com/EpicGames/UnrealEngine, which requires linking your Epic"
call "%~dp0_trace_common.bat" warn "account to your GitHub account to get access to that private repo."
call "%~dp0_trace_common.bat" warn "Continuing anyway - if you HAVE a source build, this will just work."
call "%~dp0_trace_common.bat" warn ""
exit /b 0

rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% dedicated server
echo(
echo USAGE
echo   Scripts\run-dedicated-server.bat [options] [-- ^<extra engine args^>]
echo(
echo   *** A launcher-installed engine CANNOT build the TraceServer target, on
echo       Windows or macOS. UnrealBuildTool refuses with "Server targets are not
echo       currently supported from this engine distribution." Use --editor, or
echo       Scripts\run-listen-server.bat, unless you have a SOURCE build of UE.
echo(
echo OPTIONS
echo   -m, --map ^<path^>      Map to open. Default: %TRACE_DEFAULT_MAP%
echo       --port ^<n^>        Listen port. Default: %TRACE_DEFAULT_PORT%
echo   -c, --config ^<name^>   Build configuration. Default: Development
echo                         One of: Debug ^| DebugGame ^| Development ^| Test ^| Shipping
echo       --no-build        Skip the build step, run whatever binary is already there
echo       --editor          Do not use the TraceServer binary; run the editor
echo                         binary with -server instead (no server target build
echo                         needed - THIS IS THE ONE THAT WORKS TODAY)
echo   -n, --dry-run         Print the commands; run nothing
echo   -h, --help            This text
echo(
echo ENVIRONMENT
echo   UE_ROOT               Engine install. Default on Windows:
echo                         C:\Program Files\Epic Games\UE_%TRACE_ENGINE_VERSION%
echo(
echo NOTES
echo   * Connect to it with:  Scripts\run-client.bat ^<server-ip^>
echo   * A dedicated server never renders. Do not expect a window; watch the log.
echo   * Stop it with Ctrl-C.
echo   * Server builds strip client-only content. If something is visible on a
echo     listen server but missing here, the code that creates it is almost
echo     certainly not guarded for NM_DedicatedServer.
echo(
echo EXAMPLES
echo   Scripts\run-dedicated-server.bat --editor          :: quickest, always works
echo   Scripts\run-dedicated-server.bat --editor --port 7778
echo   Scripts\run-dedicated-server.bat                   :: needs a SOURCE engine build
echo   Scripts\run-dedicated-server.bat --no-build --port 7778
echo   Scripts\run-dedicated-server.bat --editor -- -LogCmds="LogNet Verbose"
exit /b 0
