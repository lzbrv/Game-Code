@echo off
rem =============================================================================
rem  Trace - run-listen-server.bat   (Windows twin of run-listen-server.sh)
rem
rem  Launches a listen server: one process that is both the authoritative server
rem  and a playing client. This is the fastest loop for testing multiplayer by
rem  hand - start this, then point Scripts\run-client.bat at it.
rem
rem  On a launcher-installed engine this is also the ONLY networked server that
rem  actually builds (see run-dedicated-server.bat for why), so it is the default
rem  path for the whole team.
rem
rem  The command it runs (printed before every launch):
rem
rem    "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" ^^
rem        "<repo>\Trace.uproject" /Game/Maps/Arena?listen ^^
rem        -game -log -nosplash -port=7777 -windowed -ResX=1280 -ResY=720
rem
rem  The "?listen" suffix is what makes the map open in listen-server mode; it is
rem  a URL option, not a command-line switch, so it must stay glued to the map
rem  path. It is passed unquoted: cmd does not treat ? specially, and quoting it
rem  would only risk the quotes reaching Unreal's URL parser.
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "MAP=%TRACE_DEFAULT_MAP%"
set "PORT=%TRACE_DEFAULT_PORT%"
set "RES_X=1280"
set "RES_Y=720"
set "WIN_X="
set "WIN_Y="
set "WINDOWED=1"
set "DO_BUILD=0"
set "EXTRA_ARGS="

rem -----------------------------------------------------------------------------
rem  Argument parsing
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="-m"           goto :o_map
if /i "!_a!"=="--map"        goto :o_map
if /i "!_a!"=="--port"       goto :o_port
if /i "!_a!"=="--res"        goto :o_res
if /i "!_a!"=="--pos"        goto :o_pos
if /i "!_a!"=="--fullscreen" goto :o_fullscreen
if /i "!_a!"=="-b"           goto :o_build
if /i "!_a!"=="--build"      goto :o_build
if /i "!_a!"=="-n"           goto :o_dryrun
if /i "!_a!"=="--dry-run"    goto :o_dryrun
if /i "!_a!"=="-h"           goto :o_help
if /i "!_a!"=="--help"       goto :o_help
if /i "!_a!"=="/?"           goto :o_help
if /i "!_a!"=="--"           goto :o_extra
if "!_a:~0,1!"=="-"          goto :unknown_option
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

:o_res
if "%~2"=="" goto :need_res_value
for /f "tokens=1,2 delims=xX" %%A in ("%~2") do (set "RES_X=%%A" & set "RES_Y=%%B")
shift
shift
goto :parse
:need_res_value
call "%~dp0_trace_common.bat" err "--res needs a value, e.g. 1600x900"
exit /b 2

:o_pos
if "%~2"=="" goto :need_pos_value
for /f "tokens=1,2 delims=," %%A in ("%~2") do (set "WIN_X=%%A" & set "WIN_Y=%%B")
shift
shift
goto :parse
:need_pos_value
call "%~dp0_trace_common.bat" err "--pos needs a value, e.g. 700,0"
exit /b 2

:o_fullscreen
set "WINDOWED=0"
shift
goto :parse

:o_build
set "DO_BUILD=1"
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

call "%~dp0_trace_common.bat" is_number "!PORT!"
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "--port must be a number, got '!PORT!'"
    exit /b 1
)

call "%~dp0_trace_common.bat" require_uproject
if errorlevel 1 exit /b 1
call "%~dp0_trace_common.bat" resolve_engine
if errorlevel 1 exit /b 1

if "%DO_BUILD%"=="1" (
    call "%~dp0_trace_common.bat" msg "Building TraceEditor before launch"
    call "%TRACE_SCRIPT_DIR%\build.bat" --target TraceEditor --config Development
    if errorlevel 1 exit /b 1
)

call "%~dp0_trace_common.bat" warn_if_map_missing "!MAP!"

call "%~dp0_trace_common.bat" editor_binary
if errorlevel 1 exit /b 1

rem Append the listen option to the URL unless the caller already supplied options.
set "URL=!MAP!?listen"
if not "!MAP:?=!"=="!MAP!" set "URL=!MAP!"

set "TRACE_CMD="!TRACE_EDITOR_BIN!" "%TRACE_UPROJECT%" !URL! -game -log -nosplash -port=!PORT!"
if "%WINDOWED%"=="1" goto :windowed
set "TRACE_CMD=!TRACE_CMD! -fullscreen"
goto :launch
:windowed
set "TRACE_CMD=!TRACE_CMD! -windowed -ResX=!RES_X! -ResY=!RES_Y!"
rem Window placement is meaningless fullscreen, so only pass it when windowed.
if defined WIN_X set "TRACE_CMD=!TRACE_CMD! -WinX=!WIN_X! -WinY=!WIN_Y!"
:launch
if defined EXTRA_ARGS set "TRACE_CMD=!TRACE_CMD!!EXTRA_ARGS!"

call "%~dp0_trace_common.bat" msg "Listen server on port !PORT!, map !URL!"
call "%~dp0_trace_common.bat" run
exit /b %errorlevel%

rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% listen server
echo(
echo USAGE
echo   Scripts\run-listen-server.bat [options] [-- ^<extra engine args^>]
echo(
echo OPTIONS
echo   -m, --map ^<path^>      Map to open. Default: %TRACE_DEFAULT_MAP%
echo       --port ^<n^>        Listen port. Default: %TRACE_DEFAULT_PORT%
echo       --res ^<WxH^>       Window size. Default: 1280x720
echo       --pos ^<X,Y^>       Window position on screen (handy when tiling server
echo                         + clients on one machine)
echo       --fullscreen      Launch fullscreen instead of windowed
echo   -b, --build           Build TraceEditor first (Scripts\build.bat)
echo   -n, --dry-run         Print the command; run nothing
echo   -h, --help            This text
echo(
echo ENVIRONMENT
echo   UE_ROOT               Engine install. Default on Windows:
echo                         C:\Program Files\Epic Games\UE_%TRACE_ENGINE_VERSION%
echo(
echo NOTES
echo   * This runs the game with the EDITOR binary (-game), so no packaging step
echo     is needed. It loads your compiled TraceEditor module.
echo   * Other machines connect with:  Scripts\run-client.bat ^<this-machine-ip^>
echo     Find that address with:       ipconfig
echo   * Same machine, second window:  Scripts\run-client.bat 127.0.0.1 --pos 700,0
echo   * Firewall: Windows Defender Firewall will pop up the first time asking to
echo     allow UnrealEditor.exe. Tick Private networks and allow it, or nobody
echo     can join. If you dismissed it, re-allow it under
echo     Settings -^> Network ^& internet -^> Windows Firewall -^> Allow an app.
echo(
echo EXAMPLES
echo   Scripts\run-listen-server.bat
echo   Scripts\run-listen-server.bat --port 7778 --res 1600x900 --pos 0,0
echo   Scripts\run-listen-server.bat -- -NetDriverDebug -LogCmds="LogNet Verbose"
exit /b 0
