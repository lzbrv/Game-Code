@echo off
rem =============================================================================
rem  Trace - build.bat   (Windows twin of build.sh)
rem
rem  Thin, honest wrapper around UnrealBuildTool. It resolves the engine, checks
rem  the toolchain, and then runs exactly one command, which it prints first:
rem
rem    "%UE_ROOT%\Engine\Build\BatchFiles\Build.bat" ^^
rem        TraceEditor Win64 Development "<repo>\Trace.uproject" -waitmutex
rem
rem  Note the path: on Windows the batch files live directly in
rem  Engine\Build\BatchFiles\. There is no Win64\ subdirectory, unlike the Mac and
rem  Linux trees where Build.sh sits under BatchFiles\<Platform>\.
rem
rem  Nothing here is magic. If the script ever gets in your way, copy the printed
rem  command and run it yourself.
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "TARGET=TraceEditor"
set "CONFIG=Development"
set "PLATFORM="
set "DO_CLEAN=0"
set "DO_PROJECTFILES=0"
rem Import Epic's Mannequin automatically when it is missing. See the art block below.
set "DO_IMPORT_ART=1"
if defined TRACE_SKIP_ART_IMPORT set "DO_IMPORT_ART=0"
set "EXTRA_ARGS="

rem -----------------------------------------------------------------------------
rem  Argument parsing
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="-t"             goto :o_target
if /i "!_a!"=="--target"       goto :o_target
if /i "!_a!"=="-c"             goto :o_config
if /i "!_a!"=="--config"       goto :o_config
if /i "!_a!"=="-p"             goto :o_platform
if /i "!_a!"=="--platform"     goto :o_platform
if /i "!_a!"=="--clean"        goto :o_clean
if /i "!_a!"=="--projectfiles" goto :o_projectfiles
if /i "!_a!"=="--no-art"       goto :o_noart
if /i "!_a!"=="-n"             goto :o_dryrun
if /i "!_a!"=="--dry-run"      goto :o_dryrun
if /i "!_a!"=="-h"             goto :o_help
if /i "!_a!"=="--help"         goto :o_help
if /i "!_a!"=="/?"             goto :o_help
if /i "!_a!"=="--"             goto :o_extra
if "!_a:~0,1!"=="-"            goto :unknown_option
goto :unexpected_arg

:o_target
if "%~2"=="" goto :need_target_value
set "TARGET=%~2"
shift
shift
goto :parse
:need_target_value
call "%~dp0_trace_common.bat" err "--target needs a value"
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

:o_clean
set "DO_CLEAN=1"
shift
goto :parse

:o_projectfiles
set "DO_PROJECTFILES=1"
shift
goto :parse

:o_noart
set "DO_IMPORT_ART=0"
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
rem  Validate up front - a typo here otherwise surfaces as a 200-line UBT stack
rem  trace forty seconds into a build.
rem -----------------------------------------------------------------------------
call "%~dp0_trace_common.bat" is_target "!TARGET!"
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "Unknown target '!TARGET!'. Expected Trace, TraceEditor or TraceServer."
    exit /b 1
)
call "%~dp0_trace_common.bat" is_config "!CONFIG!"
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "Unknown configuration '!CONFIG!'. Expected Debug, DebugGame, Development, Test or Shipping."
    exit /b 1
)
if not defined PLATFORM set "PLATFORM=%TRACE_HOST_PLATFORM%"

call "%~dp0_trace_common.bat" require_uproject
if errorlevel 1 exit /b 1
call "%~dp0_trace_common.bat" resolve_engine
if errorlevel 1 exit /b 1
call "%~dp0_trace_common.bat" check_toolchain

call "%~dp0_trace_common.bat" build_script
if errorlevel 1 exit /b 1

rem -----------------------------------------------------------------------------
rem  Character art - imported, not committed
rem
rem  Epic's Mannequin is deliberately NOT in the repo: .gitignore excludes
rem  /Content/Characters/, and import-mannequin copies it out of each developer's
rem  own UE install instead. That keeps the repo ~1.3 MB and off GitHub's LFS
rem  quota. The cost is that a fresh clone builds with fallback primitives until
rem  somebody runs the extra command - which is how a collaborator came to report
rem  that no character models were in the project. So do it for them.
rem
rem  DELIBERATELY NON-FATAL: the game runs with fallback shapes and warns on
rem  screen, so a partial engine install must degrade visuals, never block a build.
rem
rem  Skip with --no-art, or set TRACE_SKIP_ART_IMPORT=1 for CI.
rem -----------------------------------------------------------------------------
if "%DO_IMPORT_ART%"=="1" if not "%TRACE_DRY_RUN%"=="1" (
    if not exist "%~dp0import-mannequin.bat" (
        call "%~dp0_trace_common.bat" warn "Missing %~dp0import-mannequin.bat; skipping the character-art check."
    ) else (
        call "%~dp0import-mannequin.bat" --verify >nul 2>&1
        if errorlevel 1 (
            call "%~dp0_trace_common.bat" msg "Character art missing or incomplete - importing Epic's Mannequin"
            call "%~dp0import-mannequin.bat"
            if errorlevel 1 (
                call "%~dp0_trace_common.bat" warn "Mannequin import failed. Building anyway - characters will render as fallback shapes and the game will say so on screen."
            ) else (
                call "%~dp0_trace_common.bat" msg "Character art imported."
            )
        )
    )
)

if "%DO_PROJECTFILES%"=="1" goto :projectfiles

rem -----------------------------------------------------------------------------
rem  Build
rem -----------------------------------------------------------------------------
if /i "!TARGET!"=="TraceServer" call :server_target_warning

set "TRACE_CMD="!TRACE_BUILD_BAT!" !TARGET! !PLATFORM! !CONFIG! "%TRACE_UPROJECT%" -waitmutex"
if "%DO_CLEAN%"=="1" set "TRACE_CMD=!TRACE_CMD! -clean"
if defined EXTRA_ARGS set "TRACE_CMD=!TRACE_CMD!!EXTRA_ARGS!"

if "%DO_CLEAN%"=="1" (
    call "%~dp0_trace_common.bat" msg "Cleaning !TARGET! | !PLATFORM! | !CONFIG!"
) else (
    call "%~dp0_trace_common.bat" msg "Building !TARGET! | !PLATFORM! | !CONFIG!"
)

call "%~dp0_trace_common.bat" now_seconds
set "START=!TRACE_NOW_SECONDS!"

call "%~dp0_trace_common.bat" run
set "RC=!errorlevel!"

if "%TRACE_DRY_RUN%"=="1" exit /b 0
if not "!RC!"=="0" goto :build_failed

call "%~dp0_trace_common.bat" now_seconds
set /a "ELAPSED=!TRACE_NOW_SECONDS!-!START!"
if !ELAPSED! lss 0 set /a "ELAPSED=!ELAPSED!+86400"
call "%~dp0_trace_common.bat" msg "Success in !ELAPSED!s."

if /i "!TARGET!"=="TraceEditor" call "%~dp0_trace_common.bat" msg "Next: open the project, or run a listen server with Scripts\run-listen-server.bat"
if /i "!TARGET!"=="TraceServer" call "%~dp0_trace_common.bat" msg "Next: Scripts\run-dedicated-server.bat --no-build"
exit /b 0

:build_failed
call "%~dp0_trace_common.bat" err "Build failed with exit code !RC!."
if /i not "!TARGET!"=="TraceServer" exit /b !RC!
call "%~dp0_trace_common.bat" err "If the log says 'Server targets are not currently supported from this"
call "%~dp0_trace_common.bat" err "engine distribution', that is expected on a launcher-installed engine and"
call "%~dp0_trace_common.bat" err "is NOT a bug in TraceServer.Target.cs. Working alternatives:"
call "%~dp0_trace_common.bat" err "  Scripts\run-listen-server.bat              (one player hosts)"
call "%~dp0_trace_common.bat" err "  Scripts\run-dedicated-server.bat --editor  (editor binary, -server)"
call "%~dp0_trace_common.bat" err "  a SOURCE build of UE from github.com/EpicGames/UnrealEngine"
exit /b !RC!

rem -----------------------------------------------------------------------------
rem  Project files
rem
rem  Installed (launcher) engines ship Build.bat but not always
rem  GenerateProjectFiles.bat, which is a source-build convenience. Prefer it when
rem  present, otherwise drive UnrealBuildTool's -projectfiles mode directly - same
rem  result either way.
rem
rem  No -rocket: that switch is UE4-era. Modern UBT detects an installed engine
rem  from Engine\Build\InstalledBuild.txt on its own, and passing an argument UBT
rem  does not recognise is a hard error in project-files mode.
rem -----------------------------------------------------------------------------
:projectfiles
set "GPF=!UE_ROOT!\Engine\Build\BatchFiles\GenerateProjectFiles.bat"
if exist "!GPF!" goto :projectfiles_gpf
call "%~dp0_trace_common.bat" msg "Regenerating IDE project files (via UnrealBuildTool -projectfiles)"
set "TRACE_CMD="!TRACE_BUILD_BAT!" -projectfiles -project="%TRACE_UPROJECT%" -game -progress"
goto :projectfiles_run
:projectfiles_gpf
call "%~dp0_trace_common.bat" msg "Regenerating IDE project files"
set "TRACE_CMD="!GPF!" -project="%TRACE_UPROJECT%" -game -progress"
:projectfiles_run
call "%~dp0_trace_common.bat" run
if errorlevel 1 exit /b 1
if "%TRACE_DRY_RUN%"=="1" exit /b 0
call "%~dp0_trace_common.bat" msg "Done. Open %TRACE_PROJECT_NAME%.sln in Visual Studio 2022."
call "%~dp0_trace_common.bat" msg "Same thing from Explorer: right-click Trace.uproject -> Generate Visual Studio project files."
exit /b 0

rem -----------------------------------------------------------------------------
:server_target_warning
call "%~dp0_trace_common.bat" warn "TraceServer will not build from a launcher-installed engine."
call "%~dp0_trace_common.bat" warn "Expect: 'Server targets are not currently supported from this engine distribution.'"
call "%~dp0_trace_common.bat" warn "See Scripts\run-dedicated-server.bat --help for what does work."
exit /b 0

rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% build
echo(
echo USAGE
echo   Scripts\build.bat [options] [-- ^<extra UnrealBuildTool args^>]
echo(
echo OPTIONS
echo   -t, --target ^<name^>     Build target. Default: TraceEditor
echo                           One of: Trace ^| TraceEditor ^| TraceServer
echo   -c, --config ^<name^>     Build configuration. Default: Development
echo                           One of: Debug ^| DebugGame ^| Development ^| Test ^| Shipping
echo   -p, --platform ^<name^>   Target platform. Default: this host (%TRACE_HOST_PLATFORM%)
echo       --clean             Clean the target instead of building it (-clean)
echo       --projectfiles      Regenerate IDE project files (Trace.sln)
echo                           instead of building
echo       --no-art            Skip the automatic Mannequin import
echo   -n, --dry-run           Print the command that would run; run nothing
echo   -h, --help              This text
echo(
echo ENVIRONMENT
echo   UE_ROOT                 Engine install to use. Default on Windows:
echo                           C:\Program Files\Epic Games\UE_%TRACE_ENGINE_VERSION%
echo                           A per-developer override can also live in ^<repo^>\.ue-root
echo(
echo WHAT IT WRAPS
echo   build:
echo     "%%UE_ROOT%%\Engine\Build\BatchFiles\Build.bat" ^^
echo         TraceEditor %TRACE_HOST_PLATFORM% Development "%TRACE_UPROJECT%" -waitmutex
echo(
echo   clean:
echo     ... same, plus -clean
echo(
echo   project files:
echo     "%%UE_ROOT%%\Engine\Build\BatchFiles\Build.bat" ^^
echo         -projectfiles -project="%TRACE_UPROJECT%" -game -progress
echo(
echo   On macOS and Linux the equivalent is Engine/Build/BatchFiles/Mac/Build.sh
echo   (or Linux/Build.sh), with Mac or Linux in place of %TRACE_HOST_PLATFORM%.
echo(
echo TOOLCHAIN
echo   Windows needs Visual Studio 2022 with the 'Game development with C++'
echo   workload and the 'Unreal Engine installer' individual component. The Epic
echo   Games Launcher does not install a compiler for you.
echo(
echo   -t TraceServer will NOT build on a launcher-installed engine. See
echo   Scripts\run-dedicated-server.bat --help.
echo(
echo EXAMPLES
echo   Scripts\build.bat                          :: editor target, Development
echo   Scripts\build.bat -t TraceServer           :: dedicated server target
echo   Scripts\build.bat -c DebugGame             :: game code with optimisations off
echo   Scripts\build.bat --clean -t TraceEditor   :: force a full rebuild next time
echo   Scripts\build.bat --projectfiles           :: after adding/removing a .cpp file
exit /b 0
