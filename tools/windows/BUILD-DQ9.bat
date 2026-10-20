@echo off
setlocal EnableExtensions
title DQ9-Decomp - build
rem BUILD-DQ9.bat ["C:\path\game.nds"] [--godot]
rem   No argument: asks for the ROM (you can drag the file into this window).
rem   --godot    : also builds the Godot extension and downloads the Godot 4.5 editor.
rem   DQ9_BACKUP_TYPE (environment) overrides the save chip written to abi.ini (default eeprom64k = EU release).
set "ROM="
set "GODOT_FLAGS="
set "INTERACTIVE="
set "ROOT=%~dp0"
if not exist "%ROOT%tools\build_port.py" set "ROOT=%~dp0..\.."

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--godot" (
  set "GODOT_FLAGS=--godot --fetch-godot-editor"
) else (
  if not defined ROM set "ROM=%~f1"
)
shift
goto parse
:parsed

echo.
echo  ============================================================
echo    DQ9-Decomp  -  Windows build kit
echo    Builds a private, local copy from YOUR OWN game file.
echo    Nothing from the game is ever uploaded or distributed.
echo  ============================================================
echo.

pushd "%ROOT%" || goto failure
if not exist "tools\build_port.py" goto failure_pop

if defined ROM goto havarom
set "INTERACTIVE=1"
echo  Drag your European Dragon Quest IX .nds file into this window,
set /p "ROM=  then press Enter: "
if not defined ROM goto usage
set "ROM=%ROM:"=%"
for %%F in ("%ROM%") do set "ROM=%%~fF"
:havarom
if not exist "%ROM%" (
  echo.
  echo  [FAIL] ROM file not found: "%ROM%" 1>&2
  goto failure_pop
)

rem ---- find Python 3.10+ ---------------------------------------------------
set "PY="
if exist ".venv\Scripts\python.exe" goto havevenv
call :findpython
if not defined PY goto nopython
echo  Setup: creating a private Python environment in .venv ...
%PY% -m venv .venv
if errorlevel 1 goto failure_pop
:havevenv

if exist ".venv\dq9-deps-v1.ok" goto depsok
echo  Setup: installing build tools (first run only; a few hundred MB) ...
".venv\Scripts\python.exe" -m pip install --quiet --disable-pip-version-check PyYAML capstone unicorn cmake ninja ziglang==0.16.0
if errorlevel 1 goto failure_pop
echo installed> ".venv\dq9-deps-v1.ok"
:depsok

if not defined DQ9_BACKUP_TYPE set "DQ9_BACKUP_TYPE=eeprom64k"
echo  Building. The compile step is long and shows its progress.
echo  Details are written to build\build-log.txt
".venv\Scripts\python.exe" tools\build_port.py --rom "%ROM%" --backup-type %DQ9_BACKUP_TYPE% --quiet %GODOT_FLAGS%
if errorlevel 1 goto failure_pop

echo.
echo  Next step: double-click PLAY-DQ9.bat to play.
if defined GODOT_FLAGS echo  Godot version: double-click PLAY-GODOT.bat (guide: docs\USING_GODOT.md).
if not defined GODOT_FLAGS echo  Want the Godot version too? Run BUILD-GODOT.bat afterwards.
echo.
popd
if defined INTERACTIVE pause
exit /b 0

:findpython
for %%P in ("py -3" "python" "python3") do (
  if not defined PY (
    %%~P -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
    if not errorlevel 1 set "PY=%%~P"
  )
)
exit /b 0

:nopython
echo.
echo  [FAIL] Python 3.10 or newer was not found. 1>&2
echo         Install it from https://www.python.org/downloads/windows/ and tick 1>&2
echo         "Add python.exe to PATH" in the installer, then run this again. 1>&2
echo         (The Microsoft Store shortcut named python does not count.) 1>&2
goto failure_pop

:usage
echo Usage: %~nx0 "C:\path\game.nds" [--godot] 1>&2
popd
if defined INTERACTIVE pause
exit /b 2

:failure_pop
popd
:failure
echo.
echo  BUILD-DQ9 failed. Read the message above; the full log is build\build-log.txt. 1>&2
echo  No game binary is distributed by this kit. 1>&2
if defined INTERACTIVE pause
exit /b 1
