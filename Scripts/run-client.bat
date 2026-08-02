@echo off
rem =============================================================================
rem  Trace - run-client.bat   (Windows twin of run-client.sh)
rem
rem  Connects a client to a running Trace server (listen or dedicated).
rem
rem  The command it runs (printed before every launch):
rem
rem    "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" ^^
rem        "<repo>\Trace.uproject" 127.0.0.1:7777 ^^
rem        -game -log -nosplash -windowed -ResX=1280 -ResY=720
rem
rem  The bare "IP:PORT" positional argument IS the travel URL - passing an address
rem  where a map path would normally go is how an Unreal client joins a server.
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "HOST="
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
goto :o_address

:o_address
if defined HOST goto :two_addresses
set "HOST=!_a!"
shift
goto :parse
:two_addresses
call "%~dp0_trace_common.bat" err "Only one server address may be given (got '!HOST!' and '!_a!')."
exit /b 1

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

:parsed

if not defined HOST set "HOST=127.0.0.1"

rem An explicit port inside the address argument takes precedence over --port.
set "ADDRESS=!HOST!:!PORT!"
if not "!HOST::=!"=="!HOST!" set "ADDRESS=!HOST!"

for /f "tokens=2 delims=:" %%A in ("!ADDRESS!") do set "ADDR_PORT=%%A"
call "%~dp0_trace_common.bat" is_number "!ADDR_PORT!"
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "Bad port in address '!ADDRESS!'."
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

rem The client still loads the map locally after travel, so a missing Arena.umap
rem fails at connect time rather than at launch - warn early.
call "%~dp0_trace_common.bat" warn_if_map_missing "%TRACE_DEFAULT_MAP%"

call "%~dp0_trace_common.bat" editor_binary
if errorlevel 1 exit /b 1

set "TRACE_CMD="!TRACE_EDITOR_BIN!" "%TRACE_UPROJECT%" !ADDRESS! -game -log -nosplash"
if "%WINDOWED%"=="1" goto :windowed
set "TRACE_CMD=!TRACE_CMD! -fullscreen"
goto :launch
:windowed
set "TRACE_CMD=!TRACE_CMD! -windowed -ResX=!RES_X! -ResY=!RES_Y!"
rem Window placement is meaningless fullscreen, so only pass it when windowed.
if defined WIN_X set "TRACE_CMD=!TRACE_CMD! -WinX=!WIN_X! -WinY=!WIN_Y!"
:launch
if defined EXTRA_ARGS set "TRACE_CMD=!TRACE_CMD!!EXTRA_ARGS!"

call "%~dp0_trace_common.bat" msg "Connecting to !ADDRESS!"
call "%~dp0_trace_common.bat" run
exit /b %errorlevel%

rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% client
echo(
echo USAGE
echo   Scripts\run-client.bat [^<ip^>[:^<port^>]] [options] [-- ^<extra engine args^>]
echo(
echo ARGUMENTS
echo   ^<ip^>[:^<port^>]         Server address. Default: 127.0.0.1:%TRACE_DEFAULT_PORT%
echo                         A port in the address wins over --port.
echo(
echo OPTIONS
echo       --port ^<n^>        Server port. Default: %TRACE_DEFAULT_PORT%
echo       --res ^<WxH^>       Window size. Default: 1280x720
echo       --pos ^<X,Y^>       Window position (tile several clients on one screen)
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
echo   * Find the server machine's LAN address with:  ipconfig
echo     (look for IPv4 Address under your active adapter)
echo   * Two clients on one PC is fine - give each a different --pos so the
echo     windows do not stack. Each is a separate process with its own prediction
echo     state, which is exactly what you want when eyeballing dash prediction
echo     and the trail.
echo   * If it hangs on "Pending connection", the server is not listening, the
echo     port is wrong, or Windows Defender Firewall blocked the server process.
echo(
echo EXAMPLES
echo   Scripts\run-client.bat
echo   Scripts\run-client.bat 192.168.1.42
echo   Scripts\run-client.bat 192.168.1.42:7778 --res 1600x900
echo   Scripts\run-client.bat 127.0.0.1 --pos 700,0
exit /b 0
