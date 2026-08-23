@echo off
REM Trace - setup-hooks.bat. Windows twin of Scripts/setup-hooks.sh.
REM Git does not clone hooks, so every clone runs this once.
pushd "%~dp0.."
git config core.hooksPath .githooks
echo hooks: core.hooksPath = .githooks
dir /b .githooks
popd
