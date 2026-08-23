@echo off
REM Trace - setup-hooks.bat. Windows twin of Scripts/setup-hooks.sh.
REM Git does not clone hooks, so every clone runs this once.
pushd "%~dp0.."
git config core.hooksPath .githooks
REM core.hooksPath makes git ignore .git/hooks, where Git LFS keeps the pre-push hook that
REM uploads LFS objects. Re-install LFS into the new directory or LFS silently stops running.
git lfs install --force
echo hooks: core.hooksPath = .githooks
dir /b .githooks
popd
