@echo off
rem =============================================================================
rem  Trace - unlock.bat   (Windows twin of unlock.sh)
rem
rem  Releases a Git LFS file lock AFTER you have pushed.
rem
rem  The command it runs, printed before it runs:
rem
rem    git lfs unlock Content/Maps/Arena_Baked.umap
rem
rem  Takes the same arguments as Scripts\lock.bat: a path, or the World Outliner
rem  LABEL of an actor in /Game/Maps/Arena_Baked - Cover_37, Wall_North_01 -
rem  which it resolves to the One File Per Actor package holding that actor.
rem
rem    Scripts\unlock.bat Cover_37
rem    Scripts\unlock.bat Content/Trace/Audio/S_Dash.uasset Content/Trace/Audio/DA_TraceSoundBank.uasset
rem    Scripts\unlock.bat --list
rem    Scripts\unlock.bat --force Cover_37     someone else's lock - read section 4 first
rem
rem  ORDER MATTERS: push, THEN unlock. Unlocking first lets someone take the lock,
rem  edit the version you have already superseded, and produce two divergent
rem  binaries that cannot be merged.
rem
rem  WHERE unlock.sh LEANS ON A POSIX TOOL, THIS SCRIPT USES:
rem    command -v git              WHERE, which ships with Windows.
rem    grep -rlaE on the label     PowerShell, built into Windows 10 and 11 -
rem                                the same search lock.bat runs; its comments
rem                                say why FINDSTR is not used.
rem    test -t 0, read -r          SET /P, a cmd built-in. cmd cannot tell
rem                                whether its input is a terminal, so --force
rem                                always asks. Anything but 'force' - no input
rem                                at all included - aborts with exit 1, the
rem                                code unlock.sh exits with when there is no
rem                                terminal. One difference: 'force' piped in
rem                                is accepted here; unlock.sh refuses a pipe.
rem
rem  Plain cmd.exe; Git Bash is not needed. Double-clicked from Explorer it has
rem  nothing to unlock, so it prints the usage and waits for a key.
rem
rem  See docs/GITHUB.md section 4 for the workflow this belongs to, including who
rem  is allowed to force.
rem =============================================================================
setlocal EnableExtensions EnableDelayedExpansion

rem This file's folder, captured before the pushd below can change what a later
rem expansion of the batch-path parameter resolves against.
set "TRACE_HERE=%~dp0"
call "%TRACE_HERE%_trace_common.bat" init

rem Named after this file, per the project's no-collisions rule.
set "TRACE_UNLOCK_BAT_EXTERNAL_ACTORS=Content/__ExternalActors__"

set "TARGET_COUNT=0"
set "DO_LIST=0"
set "DO_FORCE=0"
set "ASSUME_YES=0"
set "PUSHED=0"
set "RC=0"

rem Started by a double-click: hold the window open on the way out. Only
rem Explorer's exact form counts - cmd.exe /c ""C:\...\unlock.bat" " - the full path
rem in two quotes, then a space and one more quote. PowerShell, Git Bash, a
rem VS Code task and the Task Scheduler also start a .bat through cmd /c, and
rem a pause there would be pointless or wait for a key forever.
set "TRACE_HOLD=0"
if not "%~1"=="" goto :hold_decided
if defined TRACE_NO_PAUSE goto :hold_decided
set "_ccl=!cmdcmdline!"
if not defined _ccl goto :hold_decided
set "_me=""%~f0" ""
set "_rest=!_ccl:%_me%=!"
if not "!_rest!"=="!_ccl!" set "TRACE_HOLD=1"
:hold_decided

rem -----------------------------------------------------------------------------
rem  Argument parsing. Options may come anywhere, as in unlock.sh, so every
rem  target is stored first - TARGET_1, TARGET_2 ... - and released afterwards.
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="-l"        goto :o_list
if /i "!_a!"=="--list"    goto :o_list
if /i "!_a!"=="-f"        goto :o_force
if /i "!_a!"=="--force"   goto :o_force
if /i "!_a!"=="-y"        goto :o_yes
if /i "!_a!"=="--yes"     goto :o_yes
if /i "!_a!"=="-n"        goto :o_dryrun
if /i "!_a!"=="--dry-run" goto :o_dryrun
if /i "!_a!"=="-h"        goto :o_help
if /i "!_a!"=="--help"    goto :o_help
if /i "!_a!"=="/?"        goto :o_help
if "!_a:~0,1!"=="-"       goto :unknown_option
set /a TARGET_COUNT+=1
set "TARGET_!TARGET_COUNT!=!_a!"
shift
goto :parse

:o_list
set "DO_LIST=1"
shift
goto :parse

:o_force
set "DO_FORCE=1"
shift
goto :parse

:o_yes
set "ASSUME_YES=1"
shift
goto :parse

:o_dryrun
set "TRACE_DRY_RUN=1"
shift
goto :parse

:o_help
call :usage
goto :finish

:unknown_option
call "%TRACE_HERE%_trace_common.bat" err "Unknown option: !_a!"
echo(
call :usage
set "RC=2"
goto :finish

:parsed

where git >nul 2>&1
if errorlevel 1 (
    call "%TRACE_HERE%_trace_common.bat" err "git is not installed."
    goto :fail
)
git lfs version >nul 2>&1
if errorlevel 1 (
    call "%TRACE_HERE%_trace_common.bat" err "git-lfs is not installed. See docs/SETUP.md section 6."
    goto :fail
)

pushd "%TRACE_PROJECT_ROOT%" || goto :fail
set "PUSHED=1"

if "!DO_LIST!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" msg "Locks currently held across the team"
    set "TRACE_CMD=git lfs locks"
    call "%TRACE_HERE%_trace_common.bat" run
    set "RC=!errorlevel!"
    goto :finish
)

if "!TARGET_COUNT!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" err "Nothing to unlock."
    echo(
    call :usage
    set "RC=2"
    goto :finish
)

if "!DO_FORCE!"=="1" if not "!ASSUME_YES!"=="1" if not "!TRACE_DRY_RUN!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" warn "--force breaks a lock held by someone else."
    call "%TRACE_HERE%_trace_common.bat" warn "They will not be told. If they are mid-edit, their work is about to be lost."
    call "%TRACE_HERE%_trace_common.bat" warn "Have you posted in chat and waited?"
    set "TRACE_UNLOCK_BAT_REPLY="
    set /p "TRACE_UNLOCK_BAT_REPLY=Type 'force' to continue: "
    if not "!TRACE_UNLOCK_BAT_REPLY!"=="force" (
        call "%TRACE_HERE%_trace_common.bat" err "Aborted. Good."
        goto :fail
    )
)

set "STATUS=0"
for /l %%I in (1,1,%TARGET_COUNT%) do call :unlock_one %%I

if "!STATUS!"=="0" if not "!TRACE_DRY_RUN!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" msg "Unlocked. The file is read-only again for everyone, including you."
)
set "RC=!STATUS!"
goto :finish

:fail
set "RC=1"
:finish
if "!PUSHED!"=="1" popd
if "!TRACE_HOLD!"=="1" (
    echo(
    pause
)
exit /b %RC%


rem -----------------------------------------------------------------------------
rem  unlock_one N - resolve TARGET_N and release it. A failure marks STATUS and
rem  moves on to the next target, as unlock.sh does.
rem -----------------------------------------------------------------------------
:unlock_one
set "_arg=!TARGET_%~1!"
call :resolve
if errorlevel 1 (
    set "STATUS=1"
    exit /b 0
)
if "!RESOLVED_BY_LABEL!"=="1" call "%TRACE_HERE%_trace_common.bat" msg "Actor !_arg! lives in !RESOLVED!"

if "!DO_FORCE!"=="1" (
    set "TRACE_CMD=git lfs unlock --force "!RESOLVED!""
) else (
    set "TRACE_CMD=git lfs unlock "!RESOLVED!""
)
call "%TRACE_HERE%_trace_common.bat" run
if errorlevel 1 set "STATUS=1"
exit /b 0


rem -----------------------------------------------------------------------------
rem  resolve - the same rule as lock.bat: a real path wins, otherwise the
rem  argument is an actor label and must name exactly one OFPA package.
rem  _arg in; RESOLVED out, repo-relative with forward slashes, and
rem  RESOLVED_BY_LABEL=1 when it came from a label. errorlevel 1 when it fails.
rem -----------------------------------------------------------------------------
:resolve
set "RESOLVED="
set "RESOLVED_BY_LABEL=0"
set "_p=!_arg!"
if "!_p:~0,2!"=="./" set "_p=!_p:~2!"
if "!_p:~0,2!"==".\" set "_p=!_p:~2!"

if exist "!_p:/=\!" (
    set "RESOLVED=!_p:\=/!"
    exit /b 0
)

if not exist "Content\__ExternalActors__\" (
    call "%TRACE_HERE%_trace_common.bat" err "'!_arg!' is not a file, and !TRACE_UNLOCK_BAT_EXTERNAL_ACTORS!/ does not exist,"
    call "%TRACE_HERE%_trace_common.bat" err "so it cannot be an actor label either. The arena is baked on the Mac, by"
    call "%TRACE_HERE%_trace_common.bat" err "Scripts/bake-arena.sh: run git pull to get it, or pass the file's path instead."
    exit /b 1
)

rem Same search as lock.bat, and the same rule for this line: no exclamation
rem mark on it, or the caret in the regex is eaten.
set "TRACE_UNLOCK_BAT_LABEL=!_arg!"
set "_hits=%TEMP%\trace-unlock-%RANDOM%-%RANDOM%.txt"
set "TRACE_UNLOCK_BAT_HITS=%_hits%"
del /q "%_hits%" >nul 2>&1
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; $rx = [regex]($env:TRACE_UNLOCK_BAT_LABEL + '[^A-Za-z0-9_]'); $latin1 = [System.Text.Encoding]::GetEncoding(28591); $root = (Get-Location).ProviderPath.TrimEnd('\') + '\'; Get-ChildItem -LiteralPath 'Content\__ExternalActors__' -Recurse -File | Where-Object { $rx.IsMatch($latin1.GetString([System.IO.File]::ReadAllBytes($_.FullName))) } | ForEach-Object { $_.FullName.Substring($root.Length).Replace('\', '/') } | Set-Content -LiteralPath $env:TRACE_UNLOCK_BAT_HITS -Encoding Ascii"
set "_ps=!errorlevel!"

set "_count=0"
set "_first="
if exist "%_hits%" for /f "usebackq delims=" %%M in ("%_hits%") do (
    set /a _count+=1
    if not defined _first set "_first=%%M"
)

if not "!_ps!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" err "PowerShell could not search !TRACE_UNLOCK_BAT_EXTERNAL_ACTORS! for '!_arg!' (exit !_ps!)."
    del /q "%_hits%" >nul 2>&1
    exit /b 1
)
if "!_count!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" err "No file and no actor labelled '!_arg!'."
    call "%TRACE_HERE%_trace_common.bat" err "Numbering is zero-padded to two digits: Cover_37, not Cover_037."
    del /q "%_hits%" >nul 2>&1
    exit /b 1
)
if not "!_count!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" err "'!_arg!' matches !_count! packages - refusing to guess:"
    for /f "usebackq delims=" %%M in ("%_hits%") do >&2 echo     %%M
    del /q "%_hits%" >nul 2>&1
    exit /b 1
)
del /q "%_hits%" >nul 2>&1
set "RESOLVED=!_first!"
set "RESOLVED_BY_LABEL=1"
exit /b 0


rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% - release a Git LFS lock after pushing
echo(
echo USAGE
echo   Scripts\unlock.bat ^<path-or-actor-label^> [more...]
echo   Scripts\unlock.bat --list
echo   Scripts\unlock.bat --force ^<path-or-actor-label^>
echo(
echo ARGUMENTS
echo   ^<path^>                A file in the repo, e.g. Content/Maps/Arena_Baked.umap
echo   ^<actor-label^>         A World Outliner label of an actor in /Game/Maps/Arena_Baked,
echo                         e.g. Cover_37. Resolved to its One File Per Actor package.
echo(
echo OPTIONS
echo   -l, --list            List all locks held across the team, then exit
echo   -f, --force           Break a lock held by SOMEONE ELSE. See below.
echo   -y, --yes             Skip the confirmation prompt for --force
echo   -n, --dry-run         Print the git commands; run nothing
echo   -h, --help            This text
echo(
echo BEFORE YOU UNLOCK
echo   Push first. Always. The lock is what guarantees the copy on the server is the
echo   only copy anyone will build on; releasing it before your work is pushed is how
echo   two divergent .uassets get created.
echo(
echo --force, AND WHO IS ALLOWED TO USE IT
echo   Force breaks a lock somebody else is holding. It does not merge anything and it
echo   does not warn them - it just makes the file writable for everyone again, so two
echo   people can now edit it and one of them will lose the work.
echo(
echo   The rule for this team: post in chat first and wait. If they are on holiday or
echo   otherwise unreachable, whoever is unblocking the team may force it - and must
echo   say in chat which file they forced, so the person who comes back knows to pull
echo   before they open anything.
echo(
echo   If they are merely asleep, work on something else. That is the correct answer
echo   and it is why locks are meant to be short.
echo(
echo WINDOWS NOTES
echo   * Plain cmd.exe; no Git Bash needed. Paths are taken from the repository
echo     root wherever you run this from, with / or \ - git gets forward slashes.
echo   * --force always asks: cmd cannot tell a terminal from a pipe. Anything but
echo     'force' at the prompt - no input at all included - aborts with exit 1.
echo     Use --yes when there is nobody at the keyboard.
echo   * Double-clicked, it prints this text and waits for a key.
echo(
echo EXAMPLES
echo   Scripts\unlock.bat Cover_37
echo   Scripts\unlock.bat Content/Maps/Arena_Baked.umap
echo   Scripts\unlock.bat --list
echo   Scripts\unlock.bat --force Content/Maps/Arena_Baked.umap
exit /b 0
