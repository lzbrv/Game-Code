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
rem  WHERE THE .sh LEANS ON A POSIX TOOL, THIS SCRIPT USES:
rem    find Art/Sounds -name *.wav    FOR /R, a cmd built-in. The extension is
rem                                   then compared exactly, because a cmd
rem                                   wildcard also matches 8.3 short names and
rem                                   *.wav would otherwise pick up a .wave file.
rem    head -c 16, grep version       SET /P from the file, a cmd built-in: a
rem                                   first line starting "version https" is an
rem                                   unfetched Git LFS pointer, not audio.
rem    grep -E on the editor log      FINDSTR, which ships with Windows.
rem    the --only membership test     FOR over the list, a cmd built-in, compared
rem                                   case-sensitively, as the Python compares.
rem    python3                        the first of py -3, python, python3 that
rem                                   really runs Python 3 - the Microsoft Store
rem                                   alias that only says "install me" fails
rem                                   that test - and failing all three, the
rem                                   Python the engine ships with:
rem                                   Engine\Binaries\ThirdParty\Python3\Win64\python.exe
rem
rem  Plain cmd.exe; Git Bash is not needed. Double-clicked from Explorer it does
rem  what running it with no options does - imports every sound - and holds the
rem  window open at the end so the result can be read. TRACE_NO_PAUSE=1 stops that.
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

rem Started by a double-click: cmd.exe was launched just to run this file, so
rem its window closes the moment the script ends. Hold it open on the way out.
set "TRACE_HOLD=0"
set "_ccl=!cmdcmdline!"
if "%~1"=="" if not defined TRACE_NO_PAUSE if defined _ccl if /i not "!_ccl:%~nx0=!"=="!_ccl!" set "TRACE_HOLD=1"

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

rem Both halves read the subset from TRACE_SOUNDS. It is set - or cleared - here,
rem so a TRACE_SOUNDS left in this window by something else cannot quietly narrow
rem a full import.
set "TRACE_SOUNDS=!ONLY_SOUNDS!"
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
    call "%TRACE_HERE%_trace_common.bat" warn "'Verifying' block at the end is what decides whether this run worked."
)
goto :manifest_done

:manifest_dry
if not defined PY (
    call "%TRACE_HERE%_trace_common.bat" warn "No Python 3 on PATH or in the engine. This is the command once there is one."
    set "TRACE_CMD=python "!PY_SCRIPT!""
) else (
    set "TRACE_CMD=!PY! "!PY_SCRIPT!""
)
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
call "%TRACE_HERE%_trace_common.bat" warn "Continuing - the editor runs the same checks first, and the 'Verifying' block at"
call "%TRACE_HERE%_trace_common.bat" warn "the end is what decides whether this run worked."

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

if "!DRY_RUN!"=="1" (
    call "%TRACE_HERE%_trace_common.bat" print_cmd
    goto :finish
)

if defined ONLY_SOUNDS call "%TRACE_HERE%_trace_common.bat" msg "--only !ONLY_SOUNDS!: no other sound asset will be written."
call "%TRACE_HERE%_trace_common.bat" print_cmd

rem THE EXIT CODE OF THE COMMANDLET IS NOT THE RESULT OF THE RUN - it is non-zero
rem if ANY error was logged in the whole session, including engine warnings at
rem startup that have nothing to do with this. What reached disk, below, is the
rem authoritative check, so the exit code of this pipe is ignored.
rem
rem Percent expansion on this line, never the exclamation form: each side of a
rem pipe runs in a child cmd.exe with delayed expansion off. /I is deliberate -
rem a case-sensitive FINDSTR with several literal strings of different lengths
rem can miss matches; the case-insensitive search does not have that bug.
%TRACE_CMD% 2>&1 | findstr /l /i /c:"[Trace]" /c:"LogPythonScriptCommandlet"

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
rem -----------------------------------------------------------------------------
:find_python
set "PY="
set "PY_SHOW="
set "PY_FROM="
call py -3 -c "import sys, glob, wave; sys.exit(sys.version_info[0] < 3)" >nul 2>&1
if not errorlevel 1 (
    set "PY=py -3"
    set "PY_SHOW=py -3"
    set "PY_FROM=the py launcher"
    exit /b 0
)
call python -c "import sys, glob, wave; sys.exit(sys.version_info[0] < 3)" >nul 2>&1
if not errorlevel 1 (
    set "PY=python"
    set "PY_SHOW=python"
    set "PY_FROM=python on PATH"
    exit /b 0
)
call python3 -c "import sys, glob, wave; sys.exit(sys.version_info[0] < 3)" >nul 2>&1
if not errorlevel 1 (
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
if errorlevel 1 exit /b 1
set "PY="!_ue_py!""
set "PY_SHOW=!_ue_py!"
set "PY_FROM=no Python 3 on PATH, so the engine's own copy"
exit /b 0


rem -----------------------------------------------------------------------------
rem  verify_wav STEM - expect S_STEM.uasset, unless --only left this sound out:
rem  then this run deliberately did not touch it, so it is evidence of nothing.
rem -----------------------------------------------------------------------------
:verify_wav
if not defined ONLY_SOUNDS goto :verify_wav_expect
set "_hit=0"
for %%T in (!ONLY_SOUNDS!) do if "%%T"=="%~1" set "_hit=1"
if "!_hit!"=="0" exit /b 0
:verify_wav_expect
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
echo                     works too.
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
echo   * Python: the manifest runs under the first of py -3, python, python3 that is
echo     really Python 3, else the engine's own Python3\Win64\python.exe.
echo   * Engine: found the way every Scripts\*.bat finds it - UE_ROOT, then .ue-root,
echo     then the Epic Games Launcher registry key, then the usual install folders.
echo   * Double-clicked, it imports every sound and waits for a key at the end.
echo     Set TRACE_NO_PAUSE=1 to skip the wait.
exit /b 0
