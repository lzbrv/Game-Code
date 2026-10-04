@echo off
rem =============================================================================
rem  Trace - lock.bat   (Windows twin of lock.sh)
rem
rem  Takes a Git LFS file lock BEFORE you open an asset in the Unreal editor.
rem
rem  The command it runs, printed before it runs:
rem
rem    git lfs lock Content/Maps/Arena_Baked.umap
rem
rem  Like lock.sh it takes a path, or the World Outliner LABEL of an actor in
rem  /Game/Maps/Arena_Baked - Cover_37, Wall_North_01, Goal_Ring_Rim_12 - and
rem  finds the One File Per Actor package that holds it, because nobody can type
rem  Content/__ExternalActors__/Maps/Arena_Baked/3/NV/E0T93XXVAQZHVSCVX0S44T.uasset
rem
rem    Scripts\lock.bat Cover_37
rem    Scripts\lock.bat Content/Trace/Audio/S_Dash.uasset Content/Trace/Audio/DA_TraceSoundBank.uasset
rem    Scripts\lock.bat --list
rem
rem  It refuses to guess. If a label matches more than one package it prints them
rem  all and stops, because locking the wrong actor is worse than locking nothing.
rem
rem  Paths are read from the repository root, as lock.sh reads them, wherever
rem  this is run from. Forward and back slashes both work; git is always handed
rem  forward slashes, the spelling the lock server records.
rem
rem  WHERE lock.sh LEANS ON A POSIX TOOL, THIS SCRIPT USES:
rem    command -v git              WHERE, which ships with Windows.
rem    grep -rlaE on the label     PowerShell, built into Windows 10 and 11. The
rem                                byte after a label inside a .uasset is a NUL,
rem                                and FINDSTR cannot be trusted to match a NUL
rem                                with a regex. PowerShell runs the same regex -
rem                                the label, then one byte that is not a letter,
rem                                digit or underscore - over each file's bytes.
rem    grep -q 'lockable: set'     FINDSTR /L, a plain literal match.
rem
rem  Plain cmd.exe; Git Bash is not needed. Double-clicked from Explorer it has
rem  nothing to lock, so it prints the usage and waits for a key.
rem
rem  See docs/GITHUB.md section 4 for the workflow this belongs to.
rem =============================================================================
setlocal EnableExtensions EnableDelayedExpansion

rem This file's folder, captured before the pushd below can change what a later
rem expansion of the batch-path parameter resolves against.
set "TRACE_HERE=%~dp0"
call "%TRACE_HERE%_trace_common.bat" init

rem Named after this file, per the project's no-collisions rule.
set "TRACE_LOCK_BAT_EXTERNAL_ACTORS=Content/__ExternalActors__"

set "TARGET_COUNT=0"
set "DO_LIST=0"
set "PUSHED=0"
set "RC=0"

rem Started by a double-click: hold the window open on the way out. Only
rem Explorer's exact form counts - cmd.exe /c ""C:\...\lock.bat" " - the full path
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
rem  Argument parsing. Options may come anywhere, as in lock.sh, so every target
rem  is stored first - TARGET_1, TARGET_2 ... - and locked after the last option.
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if /i "!_a!"=="-l"        goto :o_list
if /i "!_a!"=="--list"    goto :o_list
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
    call "%TRACE_HERE%_trace_common.bat" err "Nothing to lock."
    echo(
    call :usage
    set "RC=2"
    goto :finish
)

call "%TRACE_HERE%_trace_common.bat" msg "Reminder: 'git pull' first, so you lock the newest version."

set "STATUS=0"
for /l %%I in (1,1,%TARGET_COUNT%) do call :lock_one %%I

if "!STATUS!"=="0" if not "!TRACE_DRY_RUN!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" msg "Locked. The file is now writable for you and read-only for everyone else."
    call "%TRACE_HERE%_trace_common.bat" msg "Push before you unlock, and unlock as soon as you have pushed."
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
rem  lock_one N - resolve TARGET_N and lock it. A failure marks STATUS and moves
rem  on to the next target, as lock.sh does.
rem -----------------------------------------------------------------------------
:lock_one
set "_arg=!TARGET_%~1!"
call :resolve
if errorlevel 1 (
    set "STATUS=1"
    exit /b 0
)
if "!RESOLVED_BY_LABEL!"=="1" call "%TRACE_HERE%_trace_common.bat" msg "Actor !_arg! lives in !RESOLVED!"

rem Locking a file that is not 'lockable' technically works but buys nothing: it
rem stays writable for everyone else, so nobody is stopped. Say so.
rem Percent expansion on the pipe line, never the exclamation form: each side of
rem a pipe runs in a child cmd.exe with delayed expansion off.
git check-attr lockable -- "%RESOLVED%" 2>nul | findstr /l /c:"lockable: set" >nul 2>&1
if errorlevel 1 (
    call "%TRACE_HERE%_trace_common.bat" warn "!RESOLVED! is not marked 'lockable' in .gitattributes."
    call "%TRACE_HERE%_trace_common.bat" warn "The lock will be recorded but will NOT make the file read-only for"
    call "%TRACE_HERE%_trace_common.bat" warn "anyone else, so it protects nothing. Check .gitattributes."
)

set "TRACE_CMD=git lfs lock "!RESOLVED!""
call "%TRACE_HERE%_trace_common.bat" run
if errorlevel 1 set "STATUS=1"
exit /b 0


rem -----------------------------------------------------------------------------
rem  resolve - _arg in; RESOLVED out, repo-relative with forward slashes, and
rem  RESOLVED_BY_LABEL=1 when it came from an actor label. errorlevel 1, with the
rem  reason printed, when it resolves to nothing or to more than one package.
rem -----------------------------------------------------------------------------
:resolve
set "RESOLVED="
set "RESOLVED_BY_LABEL=0"
set "_p=!_arg!"
if "!_p:~0,2!"=="./" set "_p=!_p:~2!"
if "!_p:~0,2!"==".\" set "_p=!_p:~2!"

rem A real path wins over a label, always.
if exist "!_p:/=\!" (
    set "RESOLVED=!_p:\=/!"
    exit /b 0
)

rem Otherwise treat it as an actor label and search the OFPA packages.
if not exist "Content\__ExternalActors__\" (
    call "%TRACE_HERE%_trace_common.bat" err "'!_arg!' is not a file, and !TRACE_LOCK_BAT_EXTERNAL_ACTORS!/ does not exist,"
    call "%TRACE_HERE%_trace_common.bat" err "so it cannot be an actor label either. The arena is baked on the Mac, by"
    call "%TRACE_HERE%_trace_common.bat" err "Scripts/bake-arena.sh: run git pull to get it, or pass the file's path instead."
    exit /b 1
)

rem The label reaches PowerShell through the environment, so no quoting of it
rem can go wrong. The regex is grep's: the label, then one byte that is not a
rem letter, digit or underscore - that is what stops Cover_1 matching Cover_110.
rem Latin-1 maps every byte to one character, so a binary .uasset is searched
rem byte for byte. PowerShell writes the hits file itself, as ASCII: output
rem redirected by cmd would be written in the console code page, which can
rem start with a byte-order mark that would end up inside the first path.
rem
rem This line must contain no exclamation mark: with delayed expansion on, one
rem would make cmd treat the caret inside the regex as an escape and drop it,
rem silently turning the negated class into a plain one.
set "TRACE_LOCK_BAT_LABEL=!_arg!"
set "_hits=%TEMP%\trace-lock-%RANDOM%-%RANDOM%.txt"
set "TRACE_LOCK_BAT_HITS=%_hits%"
del /q "%_hits%" >nul 2>&1
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; $rx = [regex]($env:TRACE_LOCK_BAT_LABEL + '[^A-Za-z0-9_]'); $latin1 = [System.Text.Encoding]::GetEncoding(28591); $root = (Get-Location).ProviderPath.TrimEnd('\') + '\'; Get-ChildItem -LiteralPath 'Content\__ExternalActors__' -Recurse -File | Where-Object { $rx.IsMatch($latin1.GetString([System.IO.File]::ReadAllBytes($_.FullName))) } | ForEach-Object { $_.FullName.Substring($root.Length).Replace('\', '/') } | Set-Content -LiteralPath $env:TRACE_LOCK_BAT_HITS -Encoding Ascii"
set "_ps=!errorlevel!"

set "_count=0"
set "_first="
if exist "%_hits%" for /f "usebackq delims=" %%M in ("%_hits%") do (
    set /a _count+=1
    if not defined _first set "_first=%%M"
)

if not "!_ps!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" err "PowerShell could not search !TRACE_LOCK_BAT_EXTERNAL_ACTORS! for '!_arg!' (exit !_ps!)."
    del /q "%_hits%" >nul 2>&1
    exit /b 1
)
if "!_count!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" err "No file and no actor labelled '!_arg!'."
    call "%TRACE_HERE%_trace_common.bat" err "Check the spelling in the World Outliner. Note the numbering is"
    call "%TRACE_HERE%_trace_common.bat" err "zero-padded to two digits: Cover_37, not Cover_037."
    del /q "%_hits%" >nul 2>&1
    exit /b 1
)
if not "!_count!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" err "'!_arg!' matches !_count! packages - refusing to guess:"
    for /f "usebackq delims=" %%M in ("%_hits%") do >&2 echo     %%M
    call "%TRACE_HERE%_trace_common.bat" err "Give the full label, or the path of the one you want."
    del /q "%_hits%" >nul 2>&1
    exit /b 1
)
del /q "%_hits%" >nul 2>&1
set "RESOLVED=!_first!"
set "RESOLVED_BY_LABEL=1"
exit /b 0


rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% - take a Git LFS lock before editing an asset
echo(
echo USAGE
echo   Scripts\lock.bat ^<path-or-actor-label^> [more...]
echo   Scripts\lock.bat --list
echo(
echo ARGUMENTS
echo   ^<path^>                A file in the repo, e.g. Content/Maps/Arena_Baked.umap
echo   ^<actor-label^>         A World Outliner label of an actor in /Game/Maps/Arena_Baked,
echo                         e.g. Cover_37, Wall_North_01, Goal_Ring_Rim_12.
echo                         Resolved to the One File Per Actor package holding it.
echo(
echo OPTIONS
echo   -l, --list            List all locks held across the team, then exit
echo   -n, --dry-run         Print the git commands; run nothing
echo   -h, --help            This text
echo(
echo WHY YOU LOCK
echo   A .uasset is an opaque binary. Git cannot merge two edits to one - the loser's
echo   work is deleted, not merged. So the team locks BEFORE editing rather than
echo   resolving after. Files marked 'lockable' in .gitattributes are checked out
echo   read-only; taking the lock is what makes yours writable.
echo(
echo THE FULL LOOP
echo   git pull                       (always first - lock the newest version)
echo   Scripts\lock.bat Cover_37      (BEFORE you open it in the editor)
echo   ... edit in Unreal, save ...
echo   git add -A ^&^& git commit -m "Arena: nudge Cover_37 out of the sightline"
echo   git push                       (push BEFORE you unlock)
echo   Scripts\unlock.bat Cover_37
echo(
echo NOTES
echo   * Write access to the GitHub repo is required to take a lock. Read-only
echo     collaborators cannot lock, which defeats the whole workflow.
echo   * Unlock as soon as you have pushed. A forgotten lock blocks a teammate
echo     silently - they just see a read-only file and no explanation.
echo   * Plain cmd.exe; no Git Bash needed. Paths are taken from the repository
echo     root wherever you run this from, with / or \ - git gets forward slashes.
echo     An actor label is looked up with PowerShell, built into Windows 10 and 11.
echo   * Double-clicked, it prints this text and waits for a key.
echo(
echo EXAMPLES
echo   Scripts\lock.bat Cover_37
echo   Scripts\lock.bat Content/Trace/Materials/Parents/M_TraceNeon.uasset
echo   Scripts\lock.bat Wall_North_01 Goal_Ring_Rim_12
echo   Scripts\lock.bat --list
exit /b 0
