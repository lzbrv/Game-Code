@echo off
rem =============================================================================
rem  Trace - setup-lfs.bat   (Windows twin of setup-lfs.sh)
rem
rem  One-time (per clone, per machine) Git LFS bootstrap. Run it right after you
rem  clone, before you commit anything binary.
rem
rem  What it actually does, in raw commands:
rem
rem    git lfs version                 :: is git-lfs installed at all?
rem    git lfs install                 :: install the clean/smudge filters + hooks
rem    git lfs track                   :: list the patterns from .gitattributes
rem    git lfs pull                    :: fetch the real bytes for this checkout
rem
rem  It is deliberately NON-DESTRUCTIVE: it never rewrites history. If binaries
rem  were already committed outside LFS it prints the `git lfs migrate` command
rem  for you to run deliberately, after the team has agreed to a force-push.
rem
rem  This is the one script in the set with no engine dependency, so it is also
rem  the one a Windows collaborator can run before Unreal has finished installing.
rem =============================================================================
setlocal enabledelayedexpansion

call "%~dp0_trace_common.bat" init

set "DO_PULL=1"
set "VERIFY_ONLY=0"

rem -----------------------------------------------------------------------------
rem  Argument parsing
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="--no-pull" goto :o_nopull
if /i "!_a!"=="--verify"  goto :o_verify
if /i "!_a!"=="-n"        goto :o_dryrun
if /i "!_a!"=="--dry-run" goto :o_dryrun
if /i "!_a!"=="-h"        goto :o_help
if /i "!_a!"=="--help"    goto :o_help
if /i "!_a!"=="/?"        goto :o_help
goto :unknown_option

:o_nopull
set "DO_PULL=0"
shift
goto :parse

:o_verify
set "VERIFY_ONLY=1"
shift
goto :parse

:o_dryrun
set "TRACE_DRY_RUN=1"
shift
goto :parse

:o_help
call :usage
exit /b 0

:unknown_option
call "%~dp0_trace_common.bat" err "Unknown option: !_a!"
echo(
call :usage
exit /b 2

:parsed

rem -----------------------------------------------------------------------------
rem  1. git
rem -----------------------------------------------------------------------------
where git >nul 2>&1
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "git is not installed or not on PATH."
    call "%~dp0_trace_common.bat" err "  winget install Git.Git"
    call "%~dp0_trace_common.bat" err "  Manual:  https://git-scm.com/download/win"
    exit /b 1
)

pushd "%TRACE_PROJECT_ROOT%" || exit /b 1

git rev-parse --git-dir >nul 2>&1
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "%TRACE_PROJECT_ROOT% is not a Git repository yet."
    call "%~dp0_trace_common.bat" err ""
    call "%~dp0_trace_common.bat" err "Start one, then re-run this script:"
    call :raw_err_git_init
    popd
    exit /b 1
)

for /f "usebackq delims=" %%R in (`git rev-parse --show-toplevel 2^>nul`) do set "REPO_ROOT=%%R"
rem git prints forward slashes; normalise before comparing with the batch path.
if defined REPO_ROOT set "REPO_ROOT=!REPO_ROOT:/=\!"
if /i not "!REPO_ROOT!"=="%TRACE_PROJECT_ROOT%" (
    call "%~dp0_trace_common.bat" warn "Git root (!REPO_ROOT!) is not the project root (%TRACE_PROJECT_ROOT%)."
    call "%~dp0_trace_common.bat" warn "LFS patterns in %TRACE_PROJECT_ROOT%\.gitattributes still apply, but paths in"
    call "%~dp0_trace_common.bat" warn "the messages below are relative to the Git root."
)

rem -----------------------------------------------------------------------------
rem  2. git-lfs
rem -----------------------------------------------------------------------------
git lfs version >nul 2>&1
if errorlevel 1 (
    call "%~dp0_trace_common.bat" err "git-lfs is not installed."
    call "%~dp0_trace_common.bat" err ""
    call "%~dp0_trace_common.bat" err "Install it, then re-run this script:"
    call "%~dp0_trace_common.bat" err "  winget install GitHub.GitLFS"
    call "%~dp0_trace_common.bat" err "  Or re-run the Git for Windows installer and tick 'Git LFS'."
    call "%~dp0_trace_common.bat" err "  Manual:  https://git-lfs.com"
    popd
    exit /b 1
)
for /f "usebackq delims=" %%V in (`git lfs version 2^>nul`) do (
    if not defined LFS_VERSION set "LFS_VERSION=%%V"
)
call "%~dp0_trace_common.bat" msg "Found !LFS_VERSION!"

rem -----------------------------------------------------------------------------
rem  3. .gitattributes
rem -----------------------------------------------------------------------------
if not exist "%TRACE_PROJECT_ROOT%\.gitattributes" (
    call "%~dp0_trace_common.bat" err ".gitattributes is missing from %TRACE_PROJECT_ROOT%."
    call "%~dp0_trace_common.bat" err "It is committed to this repo - restore it before continuing:"
    call "%~dp0_trace_common.bat" err "  git checkout -- .gitattributes"
    popd
    exit /b 1
)
set "LFS_PATTERNS=0"
for /f %%C in ('findstr /c:"filter=lfs" "%TRACE_PROJECT_ROOT%\.gitattributes" ^| find /c /v ""') do set "LFS_PATTERNS=%%C"
if "!LFS_PATTERNS!"=="0" (
    call "%~dp0_trace_common.bat" err ".gitattributes exists but declares no 'filter=lfs' patterns."
    call "%~dp0_trace_common.bat" err "Something has overwritten it."
    popd
    exit /b 1
)
call "%~dp0_trace_common.bat" msg ".gitattributes declares !LFS_PATTERNS! LFS patterns"

rem -----------------------------------------------------------------------------
rem  4. Install the filters. Idempotent - safe to run on every clone.
rem -----------------------------------------------------------------------------
if "%VERIFY_ONLY%"=="1" (
    call "%~dp0_trace_common.bat" msg "Verify-only: skipping 'git lfs install' and 'git lfs pull'."
) else (
    set "TRACE_CMD=git lfs install"
    call "%~dp0_trace_common.bat" run
)

rem -----------------------------------------------------------------------------
rem  5. Report what is tracked and what is already stored in LFS.
rem -----------------------------------------------------------------------------
call "%~dp0_trace_common.bat" msg "Patterns Git LFS will intercept:"
git lfs track

git rev-parse --verify HEAD >nul 2>&1
if errorlevel 1 (
    call "%~dp0_trace_common.bat" msg "No commits yet - nothing to migrate. LFS will catch everything from the first commit."
    goto :pull
)

set "LFS_COUNT=0"
for /f %%C in ('git lfs ls-files 2^>nul ^| find /c /v ""') do set "LFS_COUNT=%%C"
call "%~dp0_trace_common.bat" msg "Files already stored in LFS at HEAD: !LFS_COUNT!"

rem Detect binaries that were committed BEFORE LFS was configured. .gitattributes
rem only affects files as they are staged, so anything already in history keeps
rem its fat blob until the history is rewritten.
set "TMP_IN_LFS=%TEMP%\trace-lfs-in-%RANDOM%%RANDOM%.txt"
set "TMP_STRAY=%TEMP%\trace-lfs-stray-%RANDOM%%RANDOM%.txt"
rem %VAR% and not !VAR! for the redirection targets: cmd resolves redirection
rem earlier in its parsing order than delayed expansion, and both variables are
rem set on plain top-level lines just above, so ordinary expansion is correct
rem here and unambiguous.
git lfs ls-files -n > "%TMP_IN_LFS%" 2>nul
git ls-files -- "*.uasset" "*.umap" "*.fbx" "*.png" "*.jpg" "*.jpeg" "*.tga" "*.psd" "*.wav" "*.mp3" "*.ogg" "*.ttf" "*.otf" "*.exr" "*.hdr" "*.bin" "*.dll" "*.dylib" "*.so" "*.pdb" > "%TMP_STRAY%" 2>nul

set "NEEDS_MIGRATE=0"
set "SHOWN=0"
for /f "usebackq delims=" %%F in ("%TMP_STRAY%") do call :check_stray "%%F"

if not "!NEEDS_MIGRATE!"=="0" (
    if !NEEDS_MIGRATE! gtr 20 call "%~dp0_trace_common.bat" warn "    ... and more (!NEEDS_MIGRATE! in total)"
    call "%~dp0_trace_common.bat" warn ""
    call "%~dp0_trace_common.bat" warn "They were committed before LFS was set up. Fixing this REWRITES HISTORY"
    call "%~dp0_trace_common.bat" warn "and requires a force-push plus a re-clone by everyone else, so agree on it"
    call "%~dp0_trace_common.bat" warn "with the team first, then run:"
    call "%~dp0_trace_common.bat" warn ""
    call "%~dp0_trace_common.bat" warn "  git lfs migrate import --everything --include=*.uasset,*.umap,*.fbx,*.png,*.jpg,*.tga,*.psd,*.wav,*.mp3,*.ogg,*.exr,*.hdr"
    call "%~dp0_trace_common.bat" warn "  git push --force-with-lease --all"
    call "%~dp0_trace_common.bat" warn ""
    call "%~dp0_trace_common.bat" warn "This script will not do that for you on purpose."
)

del /q "%TMP_IN_LFS%" >nul 2>&1
del /q "%TMP_STRAY%" >nul 2>&1

rem -----------------------------------------------------------------------------
rem  6. Materialise LFS content for this checkout.
rem -----------------------------------------------------------------------------
:pull
if "%VERIFY_ONLY%"=="1" goto :done
if not "%DO_PULL%"=="1" goto :done

set "HAS_REMOTE=0"
for /f %%R in ('git remote 2^>nul ^| find /c /v ""') do set "HAS_REMOTE=%%R"
if "!HAS_REMOTE!"=="0" (
    call "%~dp0_trace_common.bat" msg "No Git remote configured - skipping 'git lfs pull'."
    goto :done
)
call "%~dp0_trace_common.bat" msg "Fetching LFS objects for the current checkout"
set "TRACE_CMD=git lfs pull"
call "%~dp0_trace_common.bat" run
if errorlevel 1 call "%~dp0_trace_common.bat" warn "git lfs pull failed (no LFS server configured yet?) - harmless on a brand-new repo."

:done
call "%~dp0_trace_common.bat" msg "Git LFS is ready."
call "%~dp0_trace_common.bat" msg "Reminder: lock before you edit a .umap  ->  git lfs lock Content/Maps/Arena.umap"
popd
exit /b 0

rem -----------------------------------------------------------------------------
rem  Lines that must contain literal double quotes go through echo directly:
rem  routing them through the message helpers would need doubled quotes, and
rem  those survive into the output.
rem -----------------------------------------------------------------------------
:raw_err_git_init
>&2 echo [trace] error:   cd /d "%TRACE_PROJECT_ROOT%"
>&2 echo [trace] error:   git init -b main
>&2 echo [trace] error:   Scripts\setup-lfs.bat
>&2 echo [trace] error:   git add . ^&^& git commit -m "Initial commit"
exit /b 0

rem -----------------------------------------------------------------------------
rem  check_stray <path> - is this tracked binary already an LFS pointer?
rem  findstr /x /c: does an exact whole-line match against the LFS file list.
rem -----------------------------------------------------------------------------
:check_stray
findstr /x /c:"%~1" "%TMP_IN_LFS%" >nul 2>&1
if not errorlevel 1 exit /b 0
set /a "NEEDS_MIGRATE+=1"
if !NEEDS_MIGRATE!==1 call "%~dp0_trace_common.bat" warn "These tracked files match an LFS pattern but are stored as ordinary Git blobs:"
if !SHOWN! lss 20 (
    set /a "SHOWN+=1"
    call "%~dp0_trace_common.bat" warn "    %~1"
)
exit /b 0

rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% Git LFS setup
echo(
echo USAGE
echo   Scripts\setup-lfs.bat [options]
echo(
echo OPTIONS
echo       --no-pull       Do not run 'git lfs pull' at the end
echo       --verify        Report status only; change nothing
echo   -n, --dry-run       Print the commands; run nothing
echo   -h, --help          This text
echo(
echo WHY
echo   Unreal projects accumulate .uasset / .umap / .fbx / textures / audio. Git
echo   stores a full copy of every version of every one of them forever; a year of
echo   that turns a clone into tens of gigabytes. LFS keeps pointers in history and
echo   the bytes in a side store, so history stays small and clones stay fast.
echo(
echo FILE LOCKING
echo   .uasset and .umap CANNOT be merged. Two people editing one map is guaranteed
echo   data loss for one of them, and Git will not warn you. Use locks:
echo(
echo       git lfs lock   Content/Maps/Arena.umap
echo       :: ... edit, commit, push ...
echo       git lfs unlock Content/Maps/Arena.umap
echo       git lfs locks                       :: who holds what
echo(
echo   .gitattributes marks those types 'lockable', so everyone else's copy is
echo   read-only until you release the lock. That is the point - the filesystem
echo   stops them before Git has to.
echo(
echo INSTALLING git-lfs
echo   winget:  winget install GitHub.GitLFS
echo   Bundled: re-run the Git for Windows installer and tick 'Git LFS'
echo   Manual:  https://git-lfs.com
echo(
echo WINDOWS NOTES
echo   * Long paths: Unreal asset paths get deep. Turn on long-path support once
echo     per machine, or clones fail with 'Filename too long':
echo         git config --system core.longpaths true
echo   * Line endings: .gitattributes governs them. Do NOT also set
echo     core.autocrlf globally, or you will fight it.
exit /b 0
