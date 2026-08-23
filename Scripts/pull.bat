@echo off
REM Trace - pull.bat. Windows twin of Scripts/pull.sh: classify Config churn,
REM discard only the editor's noise, keep real settings changes, then pull.
setlocal
pushd "%~dp0.."
where python3 >nul 2>&1 && (set PY=python3) || (set PY=python)
echo == Config/*.ini vs HEAD ==
%PY% Scripts\config-hygiene.py --status
if errorlevel 2 (
  echo.
  echo STOPPING. A Config file has REAL changes, listed above. Commit or stash them first.
  popd & endlocal & exit /b 2
)
echo.
echo == discarding editor noise (if any) ==
%PY% Scripts\config-hygiene.py --discard-benign
echo.
echo == git pull ==
REM --no-rebase explicitly: git refuses a divergent pull with no strategy configured,
REM and this repo merges rather than rebases because of the lockable LFS binaries.
git pull --no-rebase %*
popd & endlocal
