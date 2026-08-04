@echo off
rem =============================================================================
rem  Trace - import-mannequin.bat   (Windows twin of import-mannequin.sh)
rem
rem  Copies Epic's Mannequin (skeletal meshes, skeleton, anim blueprint, blend
rem  space, materials, textures) out of THIS developer's own Unreal install into
rem  the project. It does NOT download anything: the art ships with every UE
rem  install under Templates\TemplateResources.
rem
rem    source:  %%UE_ROOT%%\Templates\TemplateResources\High\Characters\Content\Mannequins
rem    dest:    ^<repo^>\Content\Characters\Mannequins        (gitignored)
rem
rem  WHY THE ART IS NOT IN THE REPO
rem  .gitignore excludes /Content/Characters/ deliberately. Committing ~126 MB of
rem  binary art would eat GitHub's Git-LFS free tier (roughly 1 GiB of storage AND
rem  1 GiB/month of bandwidth) and a team of four cloning would burn through it.
rem  Keeping it out leaves the repo around 1.3 MB and fully text-mergeable.
rem
rem  The assets reference themselves at /Game/Characters/Mannequins/..., which is
rem  why the destination path is not arbitrary - ATraceCharacter loads exactly
rem  that path. Get it wrong and every reference inside the assets breaks.
rem
rem  DIFFERENCE FROM THE SHELL VERSION: import-mannequin.sh reads the /Game path
rem  out of SKM_Manny_Simple.uasset and asserts it matches, so a future engine
rem  that moved the assets is caught rather than silently mis-imported. Batch has
rem  no reasonable way to scan binary files, so this script trusts the constant
rem  below. If characters render as fallback capsules AFTER a clean import, that
rem  assertion is the thing the shell version would have caught - run the .sh
rem  under WSL or Git Bash to check.
rem
rem  USAGE
rem    Scripts\import-mannequin.bat              copy anything missing or newer
rem    Scripts\import-mannequin.bat --verify     report only, copy nothing
rem    Scripts\import-mannequin.bat --force      recopy every file
rem    Scripts\import-mannequin.bat --dry-run    show what would be copied
rem
rem  Exit codes: 0 = imported/complete, 1 = failed or incomplete. build.bat runs
rem  --verify and then this script automatically, so a fresh clone just works.
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "TEMPLATE_SUBPATH=Templates\TemplateResources\High\Characters\Content\Mannequins"
set "DEST_SUBPATH=Content\Characters\Mannequins"

set "DO_VERIFY=0"
set "DO_FORCE=0"
set "DO_DRYRUN=0"

rem -----------------------------------------------------------------------------
rem  Argument parsing
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="--verify"  ( set "DO_VERIFY=1" & shift & goto :parse )
if /i "!_a!"=="--force"   ( set "DO_FORCE=1"  & shift & goto :parse )
if /i "!_a!"=="-n"        ( set "DO_DRYRUN=1" & shift & goto :parse )
if /i "!_a!"=="--dry-run" ( set "DO_DRYRUN=1" & shift & goto :parse )
if /i "!_a!"=="-h"        goto :usage
if /i "!_a!"=="--help"    goto :usage
if /i "!_a!"=="/?"        goto :usage
call "%~dp0_trace_common.bat" err "Unknown option: !_a!"
goto :usage_fail

:parsed

call "%~dp0_trace_common.bat" require_uproject
if errorlevel 1 exit /b 1
call "%~dp0_trace_common.bat" resolve_engine
if errorlevel 1 exit /b 1

set "SRC=%UE_ROOT%\%TEMPLATE_SUBPATH%"
set "DEST=%TRACE_PROJECT_ROOT%\%DEST_SUBPATH%"

rem -----------------------------------------------------------------------------
rem  --verify: report which required assets are present, copy nothing.
rem  These six are what ATraceCharacter actually loads; if they are all present
rem  the import is good enough to render animated characters.
rem -----------------------------------------------------------------------------
if "%DO_VERIFY%"=="1" (
    if not exist "%DEST%" (
        call "%~dp0_trace_common.bat" warn "Not imported yet -- %DEST% does not exist."
        call "%~dp0_trace_common.bat" warn "Run: Scripts\import-mannequin.bat"
        exit /b 1
    )
    set "MISSING=0"
    call :check "Meshes\SKM_Manny_Simple.uasset"
    call :check "Meshes\SKM_Quinn_Simple.uasset"
    call :check "Meshes\SK_Mannequin.uasset"
    call :check "Anims\Unarmed\ABP_Unarmed.uasset"
    call :check "Anims\Unarmed\BS_Idle_Walk_Run.uasset"
    call :check "Materials\M_Mannequin.uasset"
    if not "!MISSING!"=="0" (
        call "%~dp0_trace_common.bat" err "Import is incomplete. Re-run: Scripts\import-mannequin.bat --force"
        exit /b 1
    )
    call "%~dp0_trace_common.bat" msg "Mannequin import looks complete."
    exit /b 0
)

rem -----------------------------------------------------------------------------
rem  Source must exist. A partial engine install is the usual reason it does not,
rem  and the fix is in the launcher, not in this repo - so say exactly that.
rem -----------------------------------------------------------------------------
if not exist "%SRC%" (
    call "%~dp0_trace_common.bat" err "Epic's Mannequin art is not in this engine install."
    call "%~dp0_trace_common.bat" err "  looked for: %SRC%"
    call "%~dp0_trace_common.bat" blank
    call "%~dp0_trace_common.bat" err "That folder ships with the engine's template resources. If it is"
    call "%~dp0_trace_common.bat" err "missing, the install is partial: open the Epic Games Launcher, find"
    call "%~dp0_trace_common.bat" err "Unreal Engine %TRACE_ENGINE_VERSION%, choose Options, and make sure"
    call "%~dp0_trace_common.bat" err "\"Templates and Feature Packs\" (or \"Starter Content\") is ticked."
    call "%~dp0_trace_common.bat" blank
    call "%~dp0_trace_common.bat" err "The game still runs without it -- characters fall back to plain"
    call "%~dp0_trace_common.bat" err "team-coloured capsules -- but they will not be animated."
    exit /b 1
)

call "%~dp0_trace_common.bat" msg "Source : %SRC%"
call "%~dp0_trace_common.bat" msg "Dest   : %DEST%"
call "%~dp0_trace_common.bat" msg "Mounts at /Game/Characters/Mannequins"

rem -----------------------------------------------------------------------------
rem  Copy with robocopy.
rem    /E   include subdirectories, empty ones included
rem    /XO  skip files older than the destination (idempotent re-runs); dropped
rem         under --force so everything is rewritten
rem    /L   list only, change nothing (--dry-run)
rem    /NFL /NDL /NJH /NJS /NP  quiet: no per-file/dir spam, no header/summary
rem  Robocopy's exit codes are a BITMASK, not a status: 0-7 all mean success
rem  (1 = files copied, 2 = extras, 4 = mismatches). Only 8 and above are real
rem  failures, which is why this is `if errorlevel 8` and not `if errorlevel 1`.
rem  Getting this wrong is the classic robocopy-in-a-script bug.
rem -----------------------------------------------------------------------------
set "RC_FLAGS=/E /NFL /NDL /NJH /NJS /NP /R:2 /W:1"
if "%DO_FORCE%"=="0" set "RC_FLAGS=%RC_FLAGS% /XO"
if "%DO_DRYRUN%"=="1" set "RC_FLAGS=%RC_FLAGS% /L"

call "%~dp0_trace_common.bat" msg "Importing Epic's Mannequin..."
call "%~dp0_trace_common.bat" print_cmd robocopy "%SRC%" "%DEST%" %RC_FLAGS%

robocopy "%SRC%" "%DEST%" %RC_FLAGS%
if errorlevel 8 (
    call "%~dp0_trace_common.bat" err "robocopy failed copying %SRC% to %DEST%."
    exit /b 1
)

if "%DO_DRYRUN%"=="1" (
    call "%~dp0_trace_common.bat" msg "Dry run - nothing was written."
    exit /b 0
)

rem Confirm the import actually produced what the game needs, rather than
rem trusting that a zero exit code meant the right files landed.
set "MISSING=0"
call :check "Meshes\SKM_Manny_Simple.uasset"
call :check "Anims\Unarmed\ABP_Unarmed.uasset"
if not "!MISSING!"=="0" (
    call "%~dp0_trace_common.bat" err "Copy reported success but required assets are still missing."
    exit /b 1
)

call "%~dp0_trace_common.bat" msg "Character art imported. Build and run - characters will be animated."
exit /b 0

rem -----------------------------------------------------------------------------
:check
if exist "%DEST%\%~1" (
    echo     ok      %~1
) else (
    echo     MISSING %~1
    set /a MISSING+=1
)
exit /b 0

rem -----------------------------------------------------------------------------
:usage
call :print_usage
exit /b 0

:usage_fail
call :print_usage
exit /b 2

:print_usage
echo.
echo %TRACE_PROJECT_NAME% - import character art
echo.
echo USAGE
echo   Scripts\import-mannequin.bat [options]
echo.
echo OPTIONS
echo       --verify      Report what is already imported; copy nothing
echo       --force       Recopy every file even if it looks current
echo   -n, --dry-run     Show what would be copied; write nothing
echo   -h, --help        This text
echo.
echo ENVIRONMENT
echo   UE_ROOT           Engine install to import from. Default:
echo                     C:\Program Files\Epic Games\UE_%TRACE_ENGINE_VERSION%
echo.
echo NOTES
echo   The art is NOT in the repo - it is copied from your own Unreal install,
echo   so this needs no network access. build.bat runs it automatically when the
echo   art is missing; pass --no-art to build.bat to skip that.
echo.
exit /b 0
