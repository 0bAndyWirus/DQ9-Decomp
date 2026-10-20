@echo off
setlocal EnableExtensions
title DQ9-Decomp - Godot
rem PLAY-GODOT.bat ["C:\path\game.nds" [en|fr|de|es|it]]
rem Without arguments the Godot launcher opens; it remembers your ROM, language and options.
set "ROOT=%~dp0"
if not exist "%ROOT%tools\run_godot.ps1" set "ROOT=%~dp0..\.."
if not exist "%ROOT%tools\run_godot.ps1" goto nokit
if "%~1"=="" goto launcher
set "DQ9_LANGUAGE=en"
if not "%~2"=="" set "DQ9_LANGUAGE=%~2"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\run_godot.ps1" %DQ9_LANGUAGE% --rom "%~f1"
goto done
:launcher
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\run_godot.ps1" --menu
:done
set "RESULT=%errorlevel%"
if "%RESULT%"=="0" exit /b 0
echo.
echo  PLAY-GODOT stopped with status %RESULT%. 1>&2
echo  If the message above says a file or the editor is missing, run BUILD-GODOT.bat. 1>&2
pause
exit /b %RESULT%
:nokit
echo Cannot find the kit directory. 1>&2
exit /b 1
