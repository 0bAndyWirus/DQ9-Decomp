@echo off
setlocal EnableExtensions
title DQ9-Decomp - build the Godot version
rem BUILD-GODOT.bat
rem Run BUILD-DQ9.bat first: this reuses its generated code, builds the Godot extension
rem (dq9_godot DLL) and downloads the Godot 4.5 editor with a pinned SHA-512 check.
set "ROOT=%~dp0"
if not exist "%ROOT%tools\build_port.py" set "ROOT=%~dp0..\.."
echo.
echo  ============================================================
echo    DQ9-Decomp  -  Godot version
echo    Guide: docs\USING_GODOT.md
echo  ============================================================
echo.
pushd "%ROOT%" || goto failure
if not exist ".venv\Scripts\python.exe" goto needbuild
if not exist "build\gen\abi.ini" goto needbuild
".venv\Scripts\python.exe" tools\build_port.py --godot-only --fetch-godot-editor --quiet
if errorlevel 1 goto failure_pop
echo.
echo  Next step: double-click PLAY-GODOT.bat
echo.
popd
exit /b 0

:needbuild
echo  [FAIL] Run BUILD-DQ9.bat "C:\path\game.nds" first. It creates the generated code this step needs. 1>&2
goto failure_pop

:failure_pop
popd
:failure
echo.
echo  BUILD-GODOT failed. The full log is build\build-log.txt. 1>&2
exit /b 1
