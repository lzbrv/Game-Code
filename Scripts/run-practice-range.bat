@echo off
rem =============================================================================
rem  Trace - run-practice-range.bat   (Windows twin of run-practice-range.sh)
rem
rem  Opens THE PRACTICE RANGE: the ordinary arena, with the practice game mode.
rem
rem    "%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" ^^
rem        "<repo>\Trace.uproject" ^^
rem        /Game/Maps/Arena_Baked?game=/Script/Trace.TracePracticeGameMode ^^
rem        -game -log -windowed -ResX=1280 -ResY=720
rem
rem  The "?game=" suffix is a URL option, not a command-line switch, so it must
rem  stay glued to the map path. It is also the ONLY way into the range: no
rem  setting, cvar or .ini turns a real match into one, which is exactly what
rem  makes the range's cheats unable to reach a real match. It is passed
rem  unquoted: cmd does not treat ? or = specially, and quoting would only risk
rem  the quotes reaching Unreal's URL parser.
rem
rem  WHAT YOU GET
rem    * five stationary targets in a row across the field. Shoot them, knife
rem      them, hit them with abilities; they take damage and come back.
rem    * a CORE RACK pad on the centre pedestal.
rem    * an INFINITE ABILITIES pad, and a CHANGE CHARACTER pad.
rem
rem  THE OWNER'S FIRST-PERSON ARMS RIG (Demo 29 item 2) IS VISIBLE HERE AND
rem  NOWHERE ELSE. It is on by default. To compare it against the shipped pack
rem  hands without relaunching, open the console with the tilde key (`) and type:
rem
rem      Trace.Practice.ArmsRig 0      pack hands (what a real match uses)
rem      Trace.Practice.ArmsRig 1      the owner's rig (default here)
rem
rem  Switch weapons with 1 / 2 / 3 to see the pistol, SMG and knife holds.
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "MAP=%TRACE_DEFAULT_MAP%"
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

rem The practice game mode is what makes this the range. Append it unless the
rem caller already supplied URL options of their own.
set "URL=!MAP!?game=/Script/Trace.TracePracticeGameMode"
if not "!MAP:?=!"=="!MAP!" set "URL=!MAP!"

set "TRACE_CMD="!TRACE_EDITOR_BIN!" "%TRACE_UPROJECT%" !URL! -game -log -nosplash"
if "%WINDOWED%"=="1" goto :windowed
set "TRACE_CMD=!TRACE_CMD! -fullscreen"
goto :launch
:windowed
set "TRACE_CMD=!TRACE_CMD! -windowed -ResX=!RES_X! -ResY=!RES_Y!"
rem Window placement is meaningless fullscreen, so only pass it when windowed.
if defined WIN_X set "TRACE_CMD=!TRACE_CMD! -WinX=!WIN_X! -WinY=!WIN_Y!"
:launch
if defined EXTRA_ARGS set "TRACE_CMD=!TRACE_CMD!!EXTRA_ARGS!"

call "%~dp0_trace_common.bat" msg "Practice range, map !URL!"
call "%~dp0_trace_common.bat" run
exit /b %errorlevel%

rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% practice range
echo(
echo USAGE
echo   Scripts\run-practice-range.bat [options] [-- ^<extra engine args^>]
echo(
echo OPTIONS
echo   -m, --map ^<path^>      Map to open. Default: %TRACE_DEFAULT_MAP%
echo       --res ^<WxH^>       Window size. Default: 1280x720
echo       --pos ^<X,Y^>       Window position on screen
echo       --fullscreen      Launch fullscreen instead of windowed
echo   -b, --build           Build TraceEditor first (Scripts\build.bat)
echo   -n, --dry-run         Print the command; run nothing
echo   -h, --help            This text
echo(
echo THE FIRST-PERSON ARMS RIG
echo   The owner's hand rig is drawn HERE AND NOWHERE ELSE, and it is on by
echo   default. Press the tilde key (`) for the console:
echo(
echo     Trace.Practice.ArmsRig 0     the shipped pack hands
echo     Trace.Practice.ArmsRig 1     the owner's rig  (default)
echo(
echo   Weapon keys 1 / 2 / 3 switch between the pistol, SMG and knife holds.
echo(
echo NOTES
echo   * This runs the game with the EDITOR binary (-game), so no packaging step
echo     is needed. It loads your compiled TraceEditor module. If it fails with
echo     "The game module 'Trace' could not be found", build first:
echo     Scripts\run-practice-range.bat --build
echo   * The range is reached ONLY by the ?game= URL option above. There is no
echo     setting or cvar that turns a real match into one - which is what keeps
echo     the range's cheats out of real matches.
echo(
echo EXAMPLES
echo   Scripts\run-practice-range.bat
echo   Scripts\run-practice-range.bat --build --res 1600x900
echo   Scripts\run-practice-range.bat -- -LogCmds="LogTraceGame Verbose"
exit /b 0
