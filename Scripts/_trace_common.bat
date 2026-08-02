@echo off
rem =============================================================================
rem  Trace - shared batch helpers (Windows).
rem
rem  CALL this file with a subroutine name; do not run it on its own:
rem
rem      call "%~dp0_trace_common.bat" init
rem      call "%~dp0_trace_common.bat" resolve_engine
rem      if errorlevel 1 exit /b 1
rem
rem  This is the Windows twin of _trace_common.sh. Same responsibilities:
rem    * locate the project root and Trace.uproject
rem    * locate the Unreal Engine install (UE_ROOT) and, when it is missing, say
rem      exactly what to install rather than dying on "The system cannot find the
rem      path specified."
rem    * echo every command before running it, so reading the terminal teaches
rem      you the raw UnrealBuildTool / editor command line the wrapper is hiding
rem
rem  CONTRACT WITH CALLERS
rem    Every caller MUST have done `setlocal enabledelayedexpansion` first.
rem    This file deliberately does NOT setlocal at file scope, because the whole
rem    point of `init` and `resolve_engine` is to set variables in the caller.
rem    Internals are prefixed _tc_ so they are obviously ours.
rem
rem  NO ANSI COLOUR. _trace_common.sh colours output when stdout is a tty; cmd.exe
rem  has no portable equivalent (legacy consoles print the escape bytes as
rem  mojibake and there is no reliable isatty), so the Windows scripts stay plain
rem  text. The [trace] / [trace] warning: / [trace] error: prefixes are identical.
rem =============================================================================

rem Captured at file scope, where %~dp0 is unambiguously THIS file's directory.
rem Inside a CALLed :label the meaning of %0 changes, so do not rely on it there.
set "_tc_self_dir=%~dp0"

if "%~1"=="" goto :_tc_misuse

call :t_%~1 %2 %3 %4 %5 %6 %7 %8 %9
exit /b %errorlevel%

:_tc_misuse
>&2 echo [trace] error: _trace_common.bat is a library; CALL it with a subroutine name, do not run it.
exit /b 64


rem -----------------------------------------------------------------------------
rem  init - paths and constants. Call this first, from every script.
rem -----------------------------------------------------------------------------
:t_init
set "TRACE_SCRIPT_DIR=%_tc_self_dir%"
if "%TRACE_SCRIPT_DIR:~-1%"=="\" set "TRACE_SCRIPT_DIR=%TRACE_SCRIPT_DIR:~0,-1%"
for %%I in ("%TRACE_SCRIPT_DIR%\..") do set "TRACE_PROJECT_ROOT=%%~fI"
set "TRACE_PROJECT_NAME=Trace"
set "TRACE_UPROJECT=%TRACE_PROJECT_ROOT%\%TRACE_PROJECT_NAME%.uproject"
set "TRACE_DEFAULT_MAP=/Game/Maps/Arena"
set "TRACE_DEFAULT_PORT=7777"
rem Engine version this project is pinned to. Kept in sync with
rem "EngineAssociation" in Trace.uproject by hand - if you bump one, bump both.
rem Mirrors TRACE_ENGINE_VERSION in _trace_common.sh.
set "TRACE_ENGINE_VERSION=5.8"
rem Unreal's platform name for this host. The .sh derives this from `uname`;
rem on Windows there is only one answer, and it is Win64 (not "Windows").
set "TRACE_HOST_PLATFORM=Win64"
if not defined TRACE_DRY_RUN set "TRACE_DRY_RUN=0"
exit /b 0


rem -----------------------------------------------------------------------------
rem  Output.
rem
rem  Delayed expansion is used on purpose: it is the only way to echo a message
rem  containing > or | without cmd treating it as a redirection.
rem -----------------------------------------------------------------------------
:t_msg
set "_tc_m=%~1"
echo [trace] !_tc_m!
exit /b 0

:t_warn
set "_tc_m=%~1"
>&2 echo [trace] warning: !_tc_m!
exit /b 0

:t_err
set "_tc_m=%~1"
>&2 echo [trace] error: !_tc_m!
exit /b 0

:t_blank
>&2 echo(
exit /b 0


rem -----------------------------------------------------------------------------
rem  Command echoing.
rem
rem  The command lives in the TRACE_CMD variable rather than in arguments: batch
rem  has no argv array, and round-tripping a quoted engine path through %1..%9
rem  mangles it. Callers build TRACE_CMD, then `call ... run`.
rem -----------------------------------------------------------------------------
:t_print_cmd
echo ==^> !TRACE_CMD!
exit /b 0

:t_run
if not defined TRACE_CMD (
    >&2 echo [trace] error: internal - run was called with TRACE_CMD unset.
    exit /b 1
)
echo ==^> !TRACE_CMD!
if "%TRACE_DRY_RUN%"=="1" (
    echo [trace] dry run - not executed
    exit /b 0
)
rem CALL, not bare execution: Build.bat is itself a batch file, and running one
rem batch file from another WITHOUT `call` transfers control and never returns.
rem `call` is harmless for .exe targets, so it is used unconditionally.
call %TRACE_CMD%
exit /b %errorlevel%


rem -----------------------------------------------------------------------------
rem  Project file
rem -----------------------------------------------------------------------------
:t_require_uproject
if exist "%TRACE_UPROJECT%" exit /b 0
call :t_err "Trace.uproject not found at %TRACE_UPROJECT%."
call :t_err "Run these scripts from inside the Trace repository."
exit /b 1


rem -----------------------------------------------------------------------------
rem  Engine discovery
rem
rem  An engine root is valid when it contains Engine\Build\BatchFiles\Build.bat.
rem  Testing for Build.bat rather than just the directory is what catches the
rem  most common false positive: a launcher download still in progress, where
rem  UE_5.8\ already exists but holds nothing except .egstore.
rem -----------------------------------------------------------------------------
:t_is_engine_root
if "%~1"=="" exit /b 1
if exist "%~1\Engine\Build\BatchFiles\Build.bat" exit /b 0
exit /b 1

rem  resolve_engine - sets UE_ROOT. Resolution order mirrors the .sh, plus one
rem  Windows-only step (3) that the .sh has no equivalent for:
rem    1. %UE_ROOT% from the environment
rem    2. <repo>\.ue-root  (single line, gitignored, per-developer override)
rem    3. the registry key the Epic Games Launcher writes on install
rem    4. the well-known install locations, newest engine minor version first
:t_resolve_engine
set "_tc_src="

if defined UE_ROOT (
    set "_tc_src=the UE_ROOT environment variable"
    call :t_is_engine_root "!UE_ROOT!"
    if errorlevel 1 (
        call :t_err "UE_ROOT is set to a path that is not an Unreal Engine install:"
        call :t_err "  UE_ROOT=!UE_ROOT!"
        call :t_err "Expected to find: !UE_ROOT!\Engine\Build\BatchFiles\Build.bat"
        call :t_engine_help
        exit /b 1
    )
    goto :_tc_engine_found
)

if exist "%TRACE_PROJECT_ROOT%\.ue-root" (
    set "_tc_src=%TRACE_PROJECT_ROOT%\.ue-root"
    set "UE_ROOT="
    for /f "usebackq delims=" %%L in ("%TRACE_PROJECT_ROOT%\.ue-root") do (
        if not defined UE_ROOT set "UE_ROOT=%%L"
    )
    call :_tc_trim_path
    call :t_is_engine_root "!UE_ROOT!"
    if errorlevel 1 (
        call :t_err ".ue-root points at a path that is not an Unreal Engine install:"
        call :t_err "  !UE_ROOT!"
        call :t_engine_help
        exit /b 1
    )
    goto :_tc_engine_found
)

set "_tc_src=auto-detection"
set "UE_ROOT="
call :_tc_engine_roots

rem --- 3. The launcher records its install directory in the registry. That is
rem ---    the authoritative answer on Windows, so ask it before guessing paths.
call :_tc_try_registry HKLM
if not defined UE_ROOT call :_tc_try_registry HKCU
if defined UE_ROOT goto :_tc_engine_found

rem --- 4. Well-known locations. The pinned version is tried first in every root,
rem ---    then 5.99 down to 5.0 so that UE_5.10 beats UE_5.8 (a plain lexical
rem ---    sort gets that backwards - the .sh uses `sort -t. -k2 -n -r` for the
rem ---    same reason).
for %%R in (%_tc_roots%) do (
    if not defined UE_ROOT (
        call :t_is_engine_root "%%~R\UE_%TRACE_ENGINE_VERSION%"
        if not errorlevel 1 set "UE_ROOT=%%~R\UE_%TRACE_ENGINE_VERSION%"
    )
)
if not defined UE_ROOT (
    for /l %%N in (99,-1,0) do (
        if not defined UE_ROOT (
            for %%R in (%_tc_roots%) do (
                if not defined UE_ROOT (
                    call :t_is_engine_root "%%~R\UE_5.%%N"
                    if not errorlevel 1 set "UE_ROOT=%%~R\UE_5.%%N"
                )
            )
        )
    )
)
rem --- Source builds keep no version in the folder name.
if not defined UE_ROOT (
    for %%S in (%_tc_src_roots%) do (
        if not defined UE_ROOT (
            call :t_is_engine_root "%%~S"
            if not errorlevel 1 set "UE_ROOT=%%~S"
        )
    )
)

if not defined UE_ROOT (
    call :t_err "Could not find an Unreal Engine %TRACE_ENGINE_VERSION% installation."
    call :t_engine_looked_in
    call :t_engine_help
    exit /b 1
)

:_tc_engine_found
call :t_msg "Engine: !UE_ROOT!  (from !_tc_src!)"
set "_tc_needle=UE_%TRACE_ENGINE_VERSION%"
if "!UE_ROOT:%_tc_needle%=!"=="!UE_ROOT!" (
    call :t_warn "This project is pinned to UE %TRACE_ENGINE_VERSION%. The engine above does not look like that version - expect compile errors or an engine-association prompt."
)
exit /b 0

rem  _tc_try_registry <HKLM|HKCU>
:_tc_try_registry
for /f "tokens=2,*" %%A in ('reg query "%~1\SOFTWARE\EpicGames\Unreal Engine\%TRACE_ENGINE_VERSION%" /v InstalledDirectory 2^>nul ^| findstr /i /c:"InstalledDirectory"') do (
    if not defined UE_ROOT (
        call :t_is_engine_root "%%B"
        if not errorlevel 1 (
            set "UE_ROOT=%%B"
            set "_tc_src=the Epic Games Launcher registry key under %~1"
        )
    )
)
exit /b 0

rem  Launcher roots (quoted: 'Program Files' has a space) and source-build roots.
rem  Second-drive installs are normal on Windows - the engine is ~60 GB and the
rem  system SSD is usually the smallest disk in the machine.
:_tc_engine_roots
set "_tc_roots="
set "_tc_src_roots="
for %%R in (
    "%ProgramFiles%\Epic Games"
    "%ProgramW6432%\Epic Games"
    "C:\Program Files\Epic Games"
    "%SystemDrive%\Epic Games"
    "C:\Epic Games"
    "D:\Epic Games"
    "D:\Program Files\Epic Games"
    "E:\Epic Games"
) do call :_tc_add_root _tc_roots "%%~R"
for %%R in (
    "%USERPROFILE%\UnrealEngine"
    "C:\UnrealEngine"
    "D:\UnrealEngine"
    "%SystemDrive%\UnrealEngine"
) do call :_tc_add_root _tc_src_roots "%%~R"
exit /b 0

rem  _tc_add_root <listVarName> <path> - append if it looks like an absolute path
rem  and is not already in the list. The drive-letter test also discards entries
rem  built from an environment variable that does not exist on this machine
rem  (%ProgramW6432% on 32-bit cmd, for example).
:_tc_add_root
set "_tc_p=%~2"
if not defined _tc_p exit /b 0
if not "!_tc_p:~1,2!"==":\" exit /b 0
if not "!%~1:%_tc_p%=!"=="!%~1!" exit /b 0
set "%~1=!%~1! "%_tc_p%""
exit /b 0

:t_engine_looked_in
call :t_err "Looked in (in order):"
call :t_err "  - the registry: HKLM\SOFTWARE\EpicGames\Unreal Engine\%TRACE_ENGINE_VERSION% -> InstalledDirectory"
for %%R in (%_tc_roots%) do call :_tc_report_root "%%~R"
for %%S in (%_tc_src_roots%) do call :_tc_report_root "%%~S"
exit /b 0

:_tc_report_root
if not exist "%~1\" (
    call :t_err "  - %~1\UE_5.*"
    exit /b 0
)
call :t_err "  - %~1\UE_5.*   [directory exists]"
for /f "delims=" %%D in ('dir /b /ad "%~1\UE_*" 2^>nul') do (
    call :t_is_engine_root "%~1\%%D"
    if errorlevel 1 call :t_err "      %~1\%%D   [no Engine\Build\BatchFiles\Build.bat]"
)
exit /b 0

:t_engine_help
call :t_blank
call :t_err "Fix it with ONE of:"
>&2 echo [trace] error:   1. set "UE_ROOT=C:\Program Files\Epic Games\UE_%TRACE_ENGINE_VERSION%"
call :t_err "  2. Write the path into a per-developer, gitignored override file:"
>&2 echo [trace] error:        ^> "%TRACE_PROJECT_ROOT%\.ue-root" echo C:\path\to\UE_%TRACE_ENGINE_VERSION%
call :t_err "  3. Install Unreal Engine %TRACE_ENGINE_VERSION% from the Epic Games Launcher"
call :t_err "     (Library -> + -> pick %TRACE_ENGINE_VERSION%). Budget ~60 GB, more with debug symbols."
call :t_err "     No launcher yet?   winget install EpicGames.EpicGamesLauncher"
call :t_blank
call :t_err "If the launcher is still downloading, the UE_%TRACE_ENGINE_VERSION% folder exists but only"
call :t_err "contains .egstore - that is the case this check is catching. Let it finish."
call :t_blank
call :t_err "You also need the C++ toolchain, which the launcher does NOT install for you:"
call :t_err "  Visual Studio 2022, workload 'Game development with C++',"
call :t_err "  plus individual components 'Unreal Engine installer' and a Windows 11 SDK."
call :t_err "  winget install Microsoft.VisualStudio.2022.Community"
exit /b 0


rem -----------------------------------------------------------------------------
rem  Engine executables
rem
rem  Windows layout, for contrast with the Mac one in _trace_common.sh:
rem    Engine\Binaries\Win64\UnrealEditor.exe        (GUI, also runs -game)
rem    Engine\Binaries\Win64\UnrealEditor-Cmd.exe    (console, commandlets)
rem  No .app bundles, no platform subfolder under BatchFiles.
rem -----------------------------------------------------------------------------
:t_editor_binary
set "TRACE_EDITOR_BIN=%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe"
if exist "%TRACE_EDITOR_BIN%" exit /b 0
set "TRACE_EDITOR_BIN="
call :t_err "No UnrealEditor.exe under %UE_ROOT%\Engine\Binaries\Win64."
call :t_err "The engine install looks incomplete. Verify it in the Epic Games Launcher"
call :t_err "(Library -> UE_%TRACE_ENGINE_VERSION% -> ... -> Verify)."
exit /b 1

:t_editor_cmd_binary
set "TRACE_EDITOR_CMD_BIN=%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if exist "%TRACE_EDITOR_CMD_BIN%" exit /b 0
rem Partial installs sometimes ship only UnrealEditor.exe. It accepts the same
rem commandlet switches; it just also opens a window.
set "TRACE_EDITOR_CMD_BIN=%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe"
if exist "%TRACE_EDITOR_CMD_BIN%" (
    call :t_warn "UnrealEditor-Cmd.exe not found; falling back to UnrealEditor.exe."
    exit /b 0
)
set "TRACE_EDITOR_CMD_BIN="
call :t_err "No UnrealEditor-Cmd.exe under %UE_ROOT%\Engine\Binaries\Win64."
call :t_err "The engine install looks incomplete - verify it in the Epic Games Launcher."
exit /b 1

rem  Engine\Build\BatchFiles\Build.bat - note there is NO platform subdirectory on
rem  Windows. The Mac and Linux trees put Build.sh under BatchFiles\<Platform>\;
rem  the Windows batch files sit directly in BatchFiles\.
:t_build_script
set "TRACE_BUILD_BAT=%UE_ROOT%\Engine\Build\BatchFiles\Build.bat"
if exist "%TRACE_BUILD_BAT%" exit /b 0
set "TRACE_BUILD_BAT="
call :t_err "Build.bat not found at: %UE_ROOT%\Engine\Build\BatchFiles\Build.bat"
call :t_err "That file ships with every engine distribution, so this install is broken."
exit /b 1


rem -----------------------------------------------------------------------------
rem  Toolchain sanity.
rem
rem  The Windows counterpart of the .sh's xcode-select check. On macOS the classic
rem  failure is xcode-select pointing at the Command Line Tools instead of Xcode;
rem  on Windows it is Visual Studio installed WITHOUT the C++ workload, which
rem  fails deep inside UnrealBuildTool with an opaque "no compatible toolchain"
rem  message. Warn only, never fail - exactly like trace_check_toolchain.
rem -----------------------------------------------------------------------------
:t_check_toolchain
set "_tc_vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%_tc_vswhere%" (
    call :t_warn "vswhere.exe not found - Visual Studio does not appear to be installed."
    call :t_warn "Unreal needs the MSVC C++ toolchain to compile anything:"
    call :t_warn "  Visual Studio 2022 -> workload 'Game development with C++',"
    call :t_warn "  plus individual component 'Unreal Engine installer'."
    call :t_warn "  winget install Microsoft.VisualStudio.2022.Community"
    exit /b 0
)
rem pushd into the Installer directory so vswhere can be invoked by bare name.
rem Quoting an .exe path inside a `for /f` command block is a well-known way to
rem lose an argument to cmd's quote stripping; this sidesteps it entirely.
set "_tc_vs="
for %%I in ("%_tc_vswhere%") do set "_tc_vsdir=%%~dpI"
pushd "%_tc_vsdir%" >nul 2>&1
for /f "usebackq delims=" %%P in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "_tc_vs=%%P"
popd >nul 2>&1
if not defined _tc_vs (
    call :t_warn "Visual Studio is installed, but no instance carries the MSVC C++ toolset."
    call :t_warn "Visual Studio Installer -> Modify -> workload 'Game development with C++',"
    call :t_warn "and under Individual components tick 'Unreal Engine installer'."
    exit /b 0
)
call :t_msg "Toolchain: !_tc_vs!"
exit /b 0


rem -----------------------------------------------------------------------------
rem  Map / URL helpers
rem -----------------------------------------------------------------------------

rem  warn_if_map_missing "/Game/Maps/Arena" - the map is generated, not committed
rem  as a .uasset, so a fresh clone will not have it until generate-map.bat runs.
:t_warn_if_map_missing
set "_tc_map=%~1"
if not defined _tc_map exit /b 0
if /i not "!_tc_map:~0,6!"=="/Game/" exit /b 0
set "_tc_rel=!_tc_map:~6!"
rem Drop any ?listen / ?options suffix before touching the filesystem.
for /f "delims=?" %%A in ("!_tc_rel!") do set "_tc_rel=%%A"
set "_tc_rel=!_tc_rel:/=\!"
if exist "%TRACE_PROJECT_ROOT%\Content\!_tc_rel!.umap" exit /b 0
call :t_warn "Content\!_tc_rel!.umap does not exist yet."
call :t_warn "Generate it headlessly:  %TRACE_SCRIPT_DIR%\generate-map.bat"
exit /b 0


rem -----------------------------------------------------------------------------
rem  Validation helpers. Each one is a predicate: errorlevel 0 means "yes".
rem  Validating up front is the whole point - a typo otherwise surfaces as a
rem  200-line UnrealBuildTool stack trace.
rem -----------------------------------------------------------------------------

:t_is_number
set "_tc_n=%~1"
if not defined _tc_n exit /b 1
for %%D in (0 1 2 3 4 5 6 7 8 9) do if defined _tc_n set "_tc_n=!_tc_n:%%D=!"
if defined _tc_n exit /b 1
exit /b 0

:t_is_config
for %%C in (Debug DebugGame Development Test Shipping) do if /i "%~1"=="%%C" exit /b 0
exit /b 1

:t_is_target
for %%T in (Trace TraceEditor TraceServer) do if /i "%~1"=="%%T" exit /b 0
exit /b 1


rem -----------------------------------------------------------------------------
rem  Misc
rem -----------------------------------------------------------------------------

rem  Strip quotes, trailing whitespace and a trailing backslash from UE_ROOT, the
rem  way the .sh does with sed when it reads .ue-root. Quotes matter because
rem  Explorer's "Copy as path" wraps the path in them and people paste that in.
:_tc_trim_path
if not defined UE_ROOT exit /b 0
set UE_ROOT=!UE_ROOT:"=!
:_tc_trim_loop
if not defined UE_ROOT exit /b 0
rem Parenthesised, NOT `if ... set ... & goto ...`: with the & form cmd treats the
rem goto as a separate statement that runs whatever the condition evaluated to,
rem which turns this into an infinite loop.
if "!UE_ROOT:~-1!"==" " (
    set "UE_ROOT=!UE_ROOT:~0,-1!"
    goto :_tc_trim_loop
)
if "!UE_ROOT:~-1!"=="\" (
    set "UE_ROOT=!UE_ROOT:~0,-1!"
    goto :_tc_trim_loop
)
exit /b 0

rem  now_seconds -> TRACE_NOW_SECONDS, seconds since midnight, for the build
rem  timer. %TIME% is HH:MM:SS.CC with a space-padded hour before 10:00; the
rem  1xx-100 arithmetic stops cmd reading "08" and "09" as invalid octal.
:t_now_seconds
set "TRACE_NOW_SECONDS="
set "_tc_t=%time%"
set "_tc_t=%_tc_t: =0%"
set /a "TRACE_NOW_SECONDS=(1%_tc_t:~0,2%-100)*3600+(1%_tc_t:~3,2%-100)*60+(1%_tc_t:~6,2%-100)" >nul 2>&1
if not defined TRACE_NOW_SECONDS set "TRACE_NOW_SECONDS=0"
exit /b 0
