@echo off
rem =============================================================================
rem  Trace - import-sounds.bat   (Windows twin of import-sounds.sh, spec v26 section 9)
rem
rem  Puts the game's SOUND on the wire. Runs the two halves of
rem  Scripts\import_sounds.py, exactly as import-sounds.sh does:
rem
rem    1. manifest  every Art\Sounds\**\*.wav, recursively - the footsteps are in
rem                 Footsteps\. Validated and printed: rate, channels, seconds,
rem                 side, and the MEASURED peak and RMS dBFS of the samples.
rem                 Plain Python, no editor.
rem    2. assets    Art\Sounds\Event.wav becomes Content\Trace\Audio\S_Event.uasset,
rem                 and Content\Trace\Audio\DA_TraceSoundBank.uasset maps the
rem                 event name to it. Inside UnrealEditor-Cmd.exe -run=pythonscript.
rem
rem  SWAPPING ONE SOUND FOR A NEW VERSION - cmd.exe in the repository root,
rem  Unreal editor closed:
rem
rem      git pull
rem      Scripts\lock.bat Content/Trace/Audio/S_Dash.uasset Content/Trace/Audio/DA_TraceSoundBank.uasset
rem      copy /Y NewDash.wav Art\Sounds\Dash.wav
rem      Scripts\import-sounds.bat --only Dash
rem
rem  No C++ edit and no rebuild: the game asks the BANK for a name. The editor
rem  and the Scripts\run-*.bat games pick the new sound up on their next launch;
rem  Trace.Audio.Reload picks it up in one that is running. A PACKAGED build
rem  keeps the old sound until Scripts\package.bat re-cooks it.
rem
rem  Why the set of sounds is discovered rather than listed, which machines hear
rem  which sound, and why every .uasset is read-only until it is locked: see the
rem  header of import-sounds.sh. Same pipeline, same Python, same outputs.
rem
rem  A SAVE THAT FAILS LEAVES THE OLD FILE ON DISK, so "the file exists" proves
rem  nothing on a re-import. This script therefore checks before the editor
rem  starts - no Unreal editor open, and every .uasset this run re-saves locked
rem  (writable) - and afterwards believes the editor's own log, not just
rem  the files: no "done" line from import_sounds.py, or any error it reported,
rem  is a failed run.
rem
rem  WHERE THE .sh LEANS ON A POSIX TOOL, THIS SCRIPT USES:
rem    find Art/Sounds -name *.wav    FOR /R, a cmd built-in. The extension is
rem                                   then compared exactly, because a cmd
rem                                   wildcard also matches 8.3 short names and
rem                                   *.wav would otherwise pick up a .wave file.
rem    head -c 16, grep version       SET /P from the file, a cmd built-in: a
rem                                   first line starting "version https" is an
rem                                   unfetched Git LFS pointer, not audio.
rem    grep -E on the editor log      The editor writes to a log file in the
rem                                   TEMP folder, and FINDSTR - which ships with
rem                                   Windows - shows the lines that matter. A
rem                                   file, not a pipe: FINDSTR cannot match a
rem                                   piped line of 8191 bytes or more.
rem    the --only membership test     FOR over the list, a cmd built-in. Every
rem                                   name is first matched to a real WAV; one
rem                                   in the wrong case is corrected to the
rem                                   file's own spelling, because the Python
rem                                   compares names case-sensitively.
rem    [ -w file ]                    the r in the file's attributes, a cmd
rem                                   built-in. Git LFS sets read-only on every
rem                                   lockable file nobody has locked for you.
rem    python3                        the first of py -3, python, python3 that
rem                                   really runs Python 3 - the Microsoft Store
rem                                   alias that only says "install me" fails
rem                                   that test - and failing all three, the
rem                                   Python the engine ships with:
rem                                   Engine\Binaries\ThirdParty\Python3\Win64\python.exe
rem
rem  Plain cmd.exe; Git Bash is not needed. Double-clicked from Explorer it does
rem  what running it with no options does - a full import, which needs every
rem  sound locked - and holds the window open at the end so the result can be
rem  read. TRACE_NO_PAUSE=1 stops that.
rem =============================================================================
setlocal EnableExtensions EnableDelayedExpansion

rem This file's folder, captured before anything else runs: re-expanding the
rem batch-path parameter later can resolve against the current directory instead.
set "TRACE_HERE=%~dp0"
call "%TRACE_HERE%_trace_common.bat" init

set "DRY_RUN=0"
set "LIST_ONLY=0"
set "ONLY_SOUNDS="
set "ONLY_MORE=0"
set "ENGINE_READY=0"
set "PY="
set "PY_SHOW="
set "PY_FROM="
set "RC=0"

rem Started by a double-click: Explorer runs this file as
rem     C:\Windows\system32\cmd.exe /c ""C:\...\import-sounds.bat" "
rem - the full path in two quotes, then a space and one more quote - and the
rem window closes the moment the script ends, so hold it open on the way out.
rem Only that exact form counts. PowerShell, Git Bash, a VS Code task and the
rem Task Scheduler also start a .bat through cmd /c, and a pause there would
rem either be pointless or wait for a key forever.
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
rem  Argument parsing
rem
rem  cmd splits arguments at commas as well as spaces, so --only Dash,Jump arrives
rem  here as --only, Dash, Jump. A bare name straight after an --only value is
rem  therefore taken as one more name. Quoting keeps it whole: --only "Dash,Jump".
rem -----------------------------------------------------------------------------
:parse
if "%~1"=="" goto :parsed
set "_a=%~1"
if "!ONLY_MORE!"=="1" if not "!_a:~0,1!"=="-" if not "!_a!"=="/?" goto :o_only_more
set "ONLY_MORE=0"
if /i "!_a!"=="--only"    goto :o_only
if /i "!_a!"=="--list"    goto :o_list
if /i "!_a!"=="-n"        goto :o_dryrun
if /i "!_a!"=="--dry-run" goto :o_dryrun
if /i "!_a!"=="-h"        goto :o_help
if /i "!_a!"=="--help"    goto :o_help
if /i "!_a!"=="/?"        goto :o_help
goto :unknown_option

:o_only
if "%~2"=="" goto :need_only_value
set "_v=%~2"
if defined ONLY_SOUNDS (set "ONLY_SOUNDS=!ONLY_SOUNDS!,!_v!") else (set "ONLY_SOUNDS=!_v!")
set "ONLY_MORE=1"
shift
shift
goto :parse

:o_only_more
if defined ONLY_SOUNDS (set "ONLY_SOUNDS=!ONLY_SOUNDS!,!_a!") else (set "ONLY_SOUNDS=!_a!")
shift
goto :parse

:need_only_value
call "%TRACE_HERE%_trace_common.bat" err "--only needs an event name (e.g. Dash)"
goto :fail

:o_list
set "LIST_ONLY=1"
shift
goto :parse

:o_dryrun
set "DRY_RUN=1"
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

rem The manifest prints section signs and dashes. UTF-8 cannot fail to encode
rem them when the output is redirected to a file under a narrow code page.
set "PYTHONIOENCODING=utf-8"

call "%TRACE_HERE%_trace_common.bat" require_uproject
if errorlevel 1 goto :fail

set "SRC_DIR=%TRACE_PROJECT_ROOT%\Art\Sounds"
set "OUT_DIR=%TRACE_PROJECT_ROOT%\Content\Trace\Audio"

if not exist "%SRC_DIR%\" (
    call "%TRACE_HERE%_trace_common.bat" err "Missing !SRC_DIR!"
    call "%TRACE_HERE%_trace_common.bat" err "That is where the WAVs live. Spec v29 section 1 stages twenty-eight there:"
    call "%TRACE_HERE%_trace_common.bat" err "    Bodyshot ButtonPress CorePickup CoreTurnover Dash Headshot Jump Parry WallJump"
    call "%TRACE_HERE%_trace_common.bat" err "    Goal Kill RoccoRipple PistolShoot1..4 SmgShoot1"
    call "%TRACE_HERE%_trace_common.bat" err "    Footsteps/Step1..Step11"
    goto :fail
)

rem THE FOOTSTEPS LIVE IN A SUBFOLDER (spec v29 section 1b), so every scan here is
rem recursive. The event name is still the STEM and never the folder:
rem Art\Sounds\Footsteps\Step7.wav is the event Step7 and the asset S_Step7.
rem
rem An LFS pointer is a text file of about 130 bytes starting 'version https://'.
rem Handing one to the importer produces a baffling 'not a valid sound' instead of
rem a useful error, so it is caught here, before anything runs.
set "WAV_COUNT=0"
set "LFS_POINTER="
for /r "%SRC_DIR%" %%W in (*.wav) do (
    if /i "%%~xW"==".wav" (
        set /a WAV_COUNT+=1
        if not defined LFS_POINTER (
            set "_head="
            set /p "_head=" 0<"%%W"
            if defined _head if "!_head:~0,13!"=="version https" set "LFS_POINTER=%%W"
        )
    )
)
if defined LFS_POINTER (
    call "%TRACE_HERE%_trace_common.bat" err "!LFS_POINTER! is an unfetched Git LFS pointer, not audio. Run: git lfs pull"
    goto :fail
)
if "!WAV_COUNT!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" err "No .wav files under !SRC_DIR!."
    goto :fail
)

rem A --only name with no WAV behind it imports nothing, yet the run would still
rem end in 'Sound is imported' - the bank is re-saved either way. Check every
rem name against the real files before anything runs.
if not defined ONLY_SOUNDS goto :only_checked
call :check_only
if errorlevel 1 goto :fail
:only_checked

rem Both halves read the subset from TRACE_SOUNDS. It is set - or cleared - here,
rem so a TRACE_SOUNDS left in this window by something else cannot quietly narrow
rem a full import.
set "TRACE_SOUNDS=!ONLY_SOUNDS!"

rem -----------------------------------------------------------------------------
rem  1. The manifest - plain Python, and a real validation pass: it reads each
rem     RIFF header rather than trusting the extension, which is the only way to
rem     catch a renamed .mp3 (it imports as a zero-length sound that loads fine
rem     and plays nothing).
rem -----------------------------------------------------------------------------
call "%TRACE_HERE%_trace_common.bat" msg "Manifest !WAV_COUNT! wav(s) in Art/Sounds"
set "PY_SCRIPT=%TRACE_SCRIPT_DIR%\import_sounds.py"
call :find_python
if defined PY call "%TRACE_HERE%_trace_common.bat" msg "Python   !PY_SHOW!  (!PY_FROM!)"
if "!DRY_RUN!"=="1" goto :manifest_dry
if not defined PY goto :manifest_no_python

rem It exits non-zero when a declared event has no WAV. That is worth seeing, but
rem it must not stop the sounds that ARE fine from importing.
call %PY% "%PY_SCRIPT%"
set "MANIFEST_STATUS=!errorlevel!"
if not "!MANIFEST_STATUS!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" warn "The manifest reported a problem (exit !MANIFEST_STATUS!). Continuing - the"
    call "%TRACE_HERE%_trace_common.bat" warn "editor runs the same checks, and its verdict decides whether this run worked."
)
goto :manifest_done

:manifest_dry
if not defined PY (
    call "%TRACE_HERE%_trace_common.bat" warn "No Python 3 on PATH or in the engine. This is the command once there is one."
    set "TRACE_CMD=python "!PY_SCRIPT!""
) else (
    set "TRACE_CMD=!PY! "!PY_SCRIPT!""
)
call :print_sounds_env
call "%TRACE_HERE%_trace_common.bat" print_cmd
goto :manifest_done

:manifest_no_python
call "%TRACE_HERE%_trace_common.bat" warn "No Python 3 to run the manifest with. Tried py -3, python and python3 on PATH,"
call "%TRACE_HERE%_trace_common.bat" warn "then the engine's own Engine\Binaries\ThirdParty\Python3\Win64\python.exe."
call "%TRACE_HERE%_trace_common.bat" warn "Install Python 3:  winget install Python.Python.3.12"
if "!LIST_ONLY!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" err "--list IS the manifest, so nothing was checked."
    goto :fail
)
call "%TRACE_HERE%_trace_common.bat" warn "Continuing - the editor runs the same checks first, and its verdict decides"
call "%TRACE_HERE%_trace_common.bat" warn "whether this run worked."

:manifest_done
if "!LIST_ONLY!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" msg "--list: nothing was imported."
    goto :finish
)

rem -----------------------------------------------------------------------------
rem  2. WAV to USoundWave to the bank, inside the editor.
rem
rem  -NullRHI is safe here: importing a sound builds no shader map, so this needs
rem  no swap chain. -nosound is safe too and is NOT a contradiction - the editor
rem  is IMPORTING audio, not playing it, and the import path never opens an
rem  output device. The game is what must run WITHOUT -nosound (Trace.Audio.Probe).
rem
rem  -script="..." with the quotes AFTER the equals sign, as generate-map.bat
rem  does: Unreal reads a value that starts with a quote up to the closing quote,
rem  so a repository path with spaces in it survives.
rem -----------------------------------------------------------------------------
if "!ENGINE_READY!"=="1" goto :engine_ready
call "%TRACE_HERE%_trace_common.bat" resolve_engine
if errorlevel 1 goto :fail
set "ENGINE_READY=1"
:engine_ready
call "%TRACE_HERE%_trace_common.bat" editor_cmd_binary
if errorlevel 1 goto :fail

set "TRACE_CMD="!TRACE_EDITOR_CMD_BIN!" "!TRACE_UPROJECT!" -run=pythonscript -script="!PY_SCRIPT!" -unattended -nosplash -nopause -nosound -NullRHI -stdout -FullStdOutLogOutput"

call "%TRACE_HERE%_trace_common.bat" msg "Assets   Art/Sounds/*.wav -> /Game/Trace/Audio/"

if not "!DRY_RUN!"=="1" goto :assets_run
call :print_sounds_env
call "%TRACE_HERE%_trace_common.bat" print_cmd
goto :finish

:assets_run
rem -----------------------------------------------------------------------------
rem  Before the editor starts: the two things that stop it saving. Neither can be
rem  seen in the files afterwards - a save that fails leaves the old .uasset on
rem  disk, and the editor logs 'imported' for it all the same.
rem
rem  1. An open editor - on any project: nothing here can tell which one it has
rem     open. UnrealEditor-Cmd.exe, which this script runs, is a different image
rem     name and does not trip it.
rem  2. Read-only assets. Git LFS checks every 'lockable' .uasset out read-only
rem     until you lock it. Only what this run re-saves is checked: S_Event for
rem     each sound it imports, and the bank, which every run re-saves. A file that
rem     does not exist yet is fine - the first import of a new sound creates it.
rem -----------------------------------------------------------------------------
set "PRECHECK_FAILED=0"
tasklist /fi "imagename eq UnrealEditor.exe" /nh 2>nul | findstr /i /c:"UnrealEditor.exe" >nul 2>&1
if errorlevel 1 goto :editor_checked
rem UnrealEditor.exe is also the GAME the Scripts\run-*.bat files start, with
rem -game, and a game may stay open: Trace.Audio.Reload in it is how a re-import
rem is heard without a relaunch. So PowerShell reads every UnrealEditor.exe's
rem command line, and exits 7 when one has no -game - that one is the editor.
rem A command line it is not allowed to read counts as the editor. The line
rem below must hold no exclamation mark, or delayed expansion would eat it.
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$e = @(Get-CimInstance Win32_Process -Filter 'Name = ''UnrealEditor.exe''' | Where-Object { $_.CommandLine -notmatch '\s-game\b' }); if ($e.Count -gt 0) { exit 7 }; exit 0" >nul 2>&1
set "_ps=!errorlevel!"
if "!_ps!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" msg "A game window is open - UnrealEditor.exe -game. It can stay open: after this, Trace.Audio.Reload"
    call "%TRACE_HERE%_trace_common.bat" msg "in it plays the new sound. If the import says it could not save, close the game and run this again."
    goto :editor_checked
)
if "!_ps!"=="7" (
    call "%TRACE_HERE%_trace_common.bat" err "The Unreal editor is open. Close it, then run this again. While it is open the import"
    call "%TRACE_HERE%_trace_common.bat" err "may not be able to save the sound assets, and the editor keeps its old copies - the"
    call "%TRACE_HERE%_trace_common.bat" err "next Save All in it writes the old sound back."
    set "PRECHECK_FAILED=1"
    goto :editor_checked
)
call "%TRACE_HERE%_trace_common.bat" warn "UnrealEditor.exe is running, and PowerShell could not say whether it is the editor or"
call "%TRACE_HERE%_trace_common.bat" warn "a game (exit !_ps!). If it is the editor, close it before this import saves anything."
:editor_checked
set "READ_ONLY="
for /r "%SRC_DIR%" %%W in (*.wav) do if /i "%%~xW"==".wav" call :check_wav_writable "%%~nW"
call :check_writable "Content/Trace/Audio/DA_TraceSoundBank.uasset"
if not defined READ_ONLY goto :writable_checked
call "%TRACE_HERE%_trace_common.bat" err "Read-only until you lock them, so the editor could not save over them. After"
call "%TRACE_HERE%_trace_common.bat" err "git pull, lock them - this is the command - then run this script again:"
>&2 echo     Scripts\lock.bat!READ_ONLY!
if defined ONLY_SOUNDS goto :writable_failed
call "%TRACE_HERE%_trace_common.bat" err "Without --only this re-imports EVERY sound, so every one needs a lock. To swap"
call "%TRACE_HERE%_trace_common.bat" err "one sound, lock only it and the bank, and name it:"
>&2 echo     Scripts\lock.bat Content/Trace/Audio/S_Dash.uasset Content/Trace/Audio/DA_TraceSoundBank.uasset
>&2 echo     Scripts\import-sounds.bat --only Dash
:writable_failed
set "PRECHECK_FAILED=1"
:writable_checked
if "!PRECHECK_FAILED!"=="1" goto :fail

if defined ONLY_SOUNDS call "%TRACE_HERE%_trace_common.bat" msg "--only !ONLY_SOUNDS!: no other sound asset will be written."
call :print_sounds_env
call "%TRACE_HERE%_trace_common.bat" print_cmd

rem THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN - it is non-zero
rem if ANY error was logged in the whole session, including engine warnings at
rem startup that have nothing to do with this. What import_sounds.py says in the
rem log, and what reached disk, decide - so the exit code is ignored.
rem
rem All of the editor's output goes to one log file, and FINDSTR then shows the
rem lines that matter: import_sounds.py's own [Trace] lines, and the editor's
rem errors that mean it never got that far - a Trace module missing or built
rem for another engine version (a pull changed C++ and build.bat has not run),
rem a Python error, a crash, a commandlet it cannot find. A file rather than a
rem pipe: FINDSTR has no line-length limit on a file, and the editor is not left
rem writing into a pipe. /I is deliberate - a case-sensitive FINDSTR with several
rem literal strings of different lengths can miss matches.
set "_log=%TEMP%\trace-import-sounds.log"
call "%TRACE_HERE%_trace_common.bat" msg "The editor runs headless now - a minute or two. Its full output: !_log!"
%TRACE_CMD% > "%_log%" 2>&1
findstr /l /i /c:"[Trace]" /c:"LogPythonScriptCommandlet" /c:"LogPython: Error" /c:"Fatal error" /c:"Critical error" /c:"missing module" /c:"built with a different engine version" /c:"looked like a commandlet" "%_log%"

rem import_sounds.py's verdict, read from the log. It prints "done - N sound(s)
rem imported" only when nothing failed, "FAILED" when anything did, and "could
rem not save" for each asset that did not reach the disk.
set "PY_DONE=0"
set "PY_FAILED=0"
set "PY_UNSAVED=0"
findstr /l /i /c:"[Trace] done - " "%_log%" >nul 2>&1
if not errorlevel 1 set "PY_DONE=1"
findstr /l /i /c:"[Trace] FAILED" "%_log%" >nul 2>&1
if not errorlevel 1 set "PY_FAILED=1"
findstr /l /i /c:"[Trace] ERROR: could not save" "%_log%" >nul 2>&1
if not errorlevel 1 set "PY_UNSAVED=1"

rem -----------------------------------------------------------------------------
rem  3. Verify what landed
rem -----------------------------------------------------------------------------
set "MISSING=0"
call "%TRACE_HERE%_trace_common.bat" msg "Verifying:"
call :verify_one "Content/Trace/Audio/DA_TraceSoundBank.uasset"
for /r "%SRC_DIR%" %%W in (*.wav) do if /i "%%~xW"==".wav" call :verify_wav "%%~nW"

if not "!MISSING!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" err "!MISSING! output(s) did not land. Search the output above for '[Trace]' lines."
    call "%TRACE_HERE%_trace_common.bat" err "Two things cause this more than anything else:"
    call "%TRACE_HERE%_trace_common.bat" err "  * the editor is already open on this project - close it and re-run; two processes"
    call "%TRACE_HERE%_trace_common.bat" err "    cannot both write Content/Trace/Audio."
    call "%TRACE_HERE%_trace_common.bat" err "  * the .uasset is checked out read-only (it is 'lockable' in .gitattributes) -"
    call "%TRACE_HERE%_trace_common.bat" err "    run Scripts\lock.bat on the file first."
    call "%TRACE_HERE%_trace_common.bat" err "Full editor log: !_log!"
    goto :fail
)
if "!PY_UNSAVED!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" err "The editor could not save every asset - the 'could not save' lines above. A file"
    call "%TRACE_HERE%_trace_common.bat" err "listed ok may still be the OLD one. Lock the file, close any Unreal editor or game"
    call "%TRACE_HERE%_trace_common.bat" err "window that has it loaded, and run this again."
    call "%TRACE_HERE%_trace_common.bat" err "Full editor log: !_log!"
    goto :fail
)
if "!PY_FAILED!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" err "import_sounds.py reported errors - the '[Trace] ERROR' lines above. A file listed"
    call "%TRACE_HERE%_trace_common.bat" err "ok may still be the OLD one. Full editor log: !_log!"
    goto :fail
)
if "!PY_DONE!"=="0" (
    call "%TRACE_HERE%_trace_common.bat" err "import_sounds.py never said it finished, so the files listed ok are most likely the"
    call "%TRACE_HERE%_trace_common.bat" err "OLD ones: the editor stopped before or during the import. The usual causes:"
    call "%TRACE_HERE%_trace_common.bat" err "  * the Trace editor module is missing or out of date - a pull changed C++."
    call "%TRACE_HERE%_trace_common.bat" err "    Run Scripts\build.bat, then this again."
    call "%TRACE_HERE%_trace_common.bat" err "  * the editor crashed - see any 'Fatal error' or 'Critical error' lines above."
    call "%TRACE_HERE%_trace_common.bat" err "Full editor log: !_log!"
    goto :fail
)

call "%TRACE_HERE%_trace_common.bat" msg "Sound is imported (!WAV_COUNT! wav(s) seen, bank at /Game/Trace/Audio/DA_TraceSoundBank)."
call "%TRACE_HERE%_trace_common.bat" msg "Next launch picks it up (Trace.Audio.Reload in a running game). No rebuild. Check in game (WITHOUT -nosound): Trace.Audio.Report / Trace.Audio.Probe"
call "%TRACE_HERE%_trace_common.bat" msg "Packaged builds keep the old sound until Scripts\package.bat re-cooks them (--iterate re-cooks only what changed)."
goto :finish

:fail
set "RC=1"
:finish
if "!TRACE_HOLD!"=="1" (
    echo(
    pause
)
exit /b %RC%


rem -----------------------------------------------------------------------------
rem  find_python - sets PY (the command prefix, quoted when it is a path),
rem  PY_SHOW (the same, unquoted, for messages) and PY_FROM. Leaves PY empty when
rem  there is no Python 3 anywhere. A candidate counts only if it actually runs
rem  Python 3 and imports the modules import_sounds.py needs: the Microsoft Store
rem  alias python.exe exists on a clean Windows and only prints how to install.
rem  Every run goes through CALL: a Python manager can put a python.bat shim
rem  first on PATH, and a batch file run WITHOUT call never returns to this one.
rem
rem  Exit code exactly 0, not "not errorlevel 1": that test is also true for a
rem  NEGATIVE code, which is how a Python that crashes on start exits - a broken
rem  install missing a DLL ends with -1073741515.
rem -----------------------------------------------------------------------------
:find_python
set "PY="
set "PY_SHOW="
set "PY_FROM="
call py -3 -c "import sys, glob, wave; sys.exit(sys.version_info[0] < 3)" >nul 2>&1
if "!errorlevel!"=="0" (
    set "PY=py -3"
    set "PY_SHOW=py -3"
    set "PY_FROM=the py launcher"
    exit /b 0
)
call python -c "import sys, glob, wave; sys.exit(sys.version_info[0] < 3)" >nul 2>&1
if "!errorlevel!"=="0" (
    set "PY=python"
    set "PY_SHOW=python"
    set "PY_FROM=python on PATH"
    exit /b 0
)
call python3 -c "import sys, glob, wave; sys.exit(sys.version_info[0] < 3)" >nul 2>&1
if "!errorlevel!"=="0" (
    set "PY=python3"
    set "PY_SHOW=python3"
    set "PY_FROM=python3 on PATH"
    exit /b 0
)
rem No Python 3 on PATH. Every engine install carries one; step 2 needs the
rem engine anyway, so finding it now costs nothing.
if "!ENGINE_READY!"=="1" goto :find_python_engine
call "%TRACE_HERE%_trace_common.bat" resolve_engine
if errorlevel 1 exit /b 1
set "ENGINE_READY=1"
:find_python_engine
set "_ue_py=!UE_ROOT!\Engine\Binaries\ThirdParty\Python3\Win64\python.exe"
if not exist "!_ue_py!" exit /b 1
call "%_ue_py%" -c "import sys, glob, wave; sys.exit(sys.version_info[0] < 3)" >nul 2>&1
if not "!errorlevel!"=="0" exit /b 1
set "PY="!_ue_py!""
set "PY_SHOW=!_ue_py!"
set "PY_FROM=no Python 3 on PATH, so the engine's own copy"
exit /b 0


rem -----------------------------------------------------------------------------
rem  check_only - every --only name must be the stem of a real WAV. A name that
rem  differs from one only in case is corrected to the file's own spelling: the
rem  Python compares names case-sensitively, and Windows users rightly expect
rem  dash to mean Dash.wav. ONLY_SOUNDS comes back rewritten with the corrected
rem  names. errorlevel 1, with the reason printed, when any name has no WAV.
rem -----------------------------------------------------------------------------
:check_only
set "_fixed="
set "_bad="
for %%T in (!ONLY_SOUNDS!) do call :check_only_one "%%~T"
if defined _bad (
    call "%TRACE_HERE%_trace_common.bat" err "--only names a sound that has no WAV:!_bad!"
    call "%TRACE_HERE%_trace_common.bat" err "An event name is a WAV's file name without .wav, in any folder under Art\Sounds:"
    call "%TRACE_HERE%_trace_common.bat" err "Dash is Art\Sounds\Dash.wav, Step7 is Art\Sounds\Footsteps\Step7.wav."
    call "%TRACE_HERE%_trace_common.bat" err "Scripts\import-sounds.bat --list prints every one."
    exit /b 1
)
rem A wildcard in a name makes FOR look for files, so it can come back empty.
if not defined _fixed (
    call "%TRACE_HERE%_trace_common.bat" err "--only !ONLY_SOUNDS! names no sound. Give event names, e.g. --only Dash"
    exit /b 1
)
set "ONLY_SOUNDS=!_fixed!"
exit /b 0

rem  check_only_one NAME - append NAME, in the file's own spelling, to _fixed;
rem  or to _bad when no WAV has that stem in any case.
:check_only_one
set "_want=%~1"
set "_exact="
set "_loose="
for /r "%SRC_DIR%" %%W in (*.wav) do if /i "%%~xW"==".wav" (
    if "%%~nW"=="!_want!" set "_exact=%%~nW"
    if /i "%%~nW"=="!_want!" if not defined _loose set "_loose=%%~nW"
)
if defined _exact goto :check_only_keep
if defined _loose goto :check_only_case
set "_bad=!_bad! !_want!"
exit /b 0
:check_only_case
call "%TRACE_HERE%_trace_common.bat" msg "--only !_want!: the file is !_loose!.wav, so the event is !_loose! - using that."
set "_exact=!_loose!"
:check_only_keep
if defined _fixed (set "_fixed=!_fixed!,!_exact!") else (set "_fixed=!_exact!")
exit /b 0


rem -----------------------------------------------------------------------------
rem  is_selected STEM - _hit=1 when this run imports STEM: every sound without
rem  --only, otherwise only the ones it names. check_only has already put every
rem  name in its file's own spelling, so the match is exact.
rem -----------------------------------------------------------------------------
:is_selected
set "_hit=1"
if not defined ONLY_SOUNDS exit /b 0
set "_hit=0"
for %%T in (!ONLY_SOUNDS!) do if "%%T"=="%~1" set "_hit=1"
exit /b 0

rem  print_sounds_env - the selection reaches the editor through TRACE_SOUNDS, not
rem  its command line, so it is printed as a step of its own. Copying only the
rem  editor line below it would re-import EVERY sound.
:print_sounds_env
if defined ONLY_SOUNDS echo ==^> set "TRACE_SOUNDS=!ONLY_SOUNDS!"
exit /b 0


rem -----------------------------------------------------------------------------
rem  check_wav_writable STEM - check S_STEM.uasset, if this run re-saves it.
rem  check_writable REPO/RELATIVE/PATH - add the path to READ_ONLY when the file
rem  exists and carries the read-only attribute: the second letter of the
rem  attribute string, as in -ra------. A file that does not exist yet is fine.
rem -----------------------------------------------------------------------------
:check_wav_writable
call :is_selected "%~1"
if "!_hit!"=="0" exit /b 0
call :check_writable "Content/Trace/Audio/S_%~1.uasset"
exit /b 0

:check_writable
set "_rel=%~1"
set "_f=%TRACE_PROJECT_ROOT%\!_rel:/=\!"
if not exist "!_f!" exit /b 0
set "_at="
for %%F in ("!_f!") do set "_at=%%~aF"
if /i "!_at:~1,1!"=="r" set "READ_ONLY=!READ_ONLY! !_rel!"
exit /b 0


rem -----------------------------------------------------------------------------
rem  verify_wav STEM - expect S_STEM.uasset, unless --only left this sound out:
rem  then this run deliberately did not touch it, so it is evidence of nothing.
rem -----------------------------------------------------------------------------
:verify_wav
call :is_selected "%~1"
if "!_hit!"=="0" exit /b 0
call :verify_one "Content/Trace/Audio/S_%~1.uasset"
exit /b 0

rem  verify_one REPO/RELATIVE/PATH - print ok or MISSING, count the misses.
:verify_one
set "_rel=%~1"
set "_file=%TRACE_PROJECT_ROOT%\!_rel:/=\!"
if exist "!_file!" (
    echo     ok      !_rel!
) else (
    echo     MISSING !_rel!
    set /a MISSING+=1
)
exit /b 0


rem -----------------------------------------------------------------------------
:usage
echo %TRACE_PROJECT_NAME% import-sounds
echo(
echo Imports Art\Sounds\**\*.wav (recursively - the footsteps live in Footsteps\) into
echo Content\Trace\Audio as USoundWave assets and writes the one asset that maps an
echo event name to a sound: DA_TraceSoundBank. An event name is the file's STEM; the
echo folder is filing, not naming.
echo(
echo USAGE
echo   Scripts\import-sounds.bat [options]
echo(
echo OPTIONS
echo       --only NAME   Import only this event (repeatable, or comma-separated). The BANK
echo                     still keeps every other row - it is read-modify-write, never a
echo                     wholesale replace. Use this to swap ONE sound without touching
echo                     eight .uasset files you have not locked (.uasset is lockable in
echo                     .gitattributes, so it is read-only until Scripts\lock.bat says
echo                     otherwise). cmd splits Dash,Jump at the comma, so a bare name
echo                     right after --only NAME counts as another name; --only "Dash,Jump"
echo                     works too. A name with no WAV stops the run; one in the wrong
echo                     case (dash) is corrected to the file's spelling (Dash).
echo       --list        Validate and print the manifest only. No editor, instant.
echo   -n, --dry-run     Print what would run; run nothing
echo   -h, --help        This text
echo(
echo AFTER RUNNING (no C++ rebuild: the game asks the bank, and the bank is data)
echo   git status Art/Sounds Content/Trace/Audio
echo   The editor and the Scripts\run-*.bat games pick it up on their next launch;
echo   Trace.Audio.Reload picks it up in one that is running. Packaged builds keep
echo   the old sound until Scripts\package.bat re-cooks them (--iterate re-cooks
echo   only what changed).
echo(
echo IN GAME (drop -nosound, or none of this is audible)
echo   Trace.Audio.Report      every event: side, which asset it resolved to, the device
echo   Trace.Audio.Test Dash   fire one event through the real API
echo   Trace.Audio.Probe       THE evidence: reads the mixer's source count back
echo   Trace.Audio.Reload      pick up a re-import without relaunching
echo(
echo   spec v29 section 1:
echo   Trace.Audio.Sides       1a: the nine v26 events still route as v26 shipped them
echo   Trace.Audio.Loudness    1b: MEASURES every clip and proves footsteps are quieter
echo                           (red arm: Trace.Audio.FootstepVolume 1)
echo   Trace.Audio.Footsteps   1b: the randomiser and the stride, driven
echo                           (red arm: Trace.Audio.FootstepRepeatGuard 0)
echo   Trace.Audio.GunLadder   1c/1d/1e: the clip name per shot, fast and gapped
echo                           (red arms: Trace.Audio.PistolResetFloor 0, Trace.Audio.ShotWatch 0)
echo   Trace.Audio.V29Integ    1f: Goal, Kill and RoccoRipple from their real triggers
echo   Trace.Audio.Integ       section 9: the nine v26 call sites (run it in a MATCH, not the range)
echo(
echo WINDOWS NOTES
echo   * Before the editor starts, it stops if the Unreal editor is open or if a
echo     .uasset this run re-saves is read-only (not locked by you), and prints the
echo     Scripts\lock.bat command that fixes it. A failed save cannot be seen in the
echo     files afterwards: the old one is still there. A game started by
echo     Scripts\run-*.bat (UnrealEditor.exe -game) may stay open.
echo   * The editor's full output goes to %TEMP%\trace-import-sounds.log;
echo     the [Trace] lines and the editor's fatal errors are shown. The run counts as
echo     a success only when import_sounds.py says it finished with no errors.
echo   * Python: the manifest runs under the first of py -3, python, python3 that is
echo     really Python 3, else the engine's own Python3\Win64\python.exe.
echo   * Engine: found the way every Scripts\*.bat finds it - UE_ROOT, then .ue-root,
echo     then the Epic Games Launcher registry key, then the usual install folders.
echo   * Double-clicked, it runs the full import - every sound must be locked - and
echo     waits for a key at the end. Set TRACE_NO_PAUSE=1 to skip the wait. Run
echo     from cmd, PowerShell or a task, it never waits.
exit /b 0
