@echo off
setlocal
if "%~1"=="" goto usage
set "ROM=%~f1"
set "ROOT=%~dp0"
if not exist "%ROOT%tools\build_port.py" set "ROOT=%~dp0..\.."
pushd "%ROOT%" || goto failure
if not exist "tools\build_port.py" goto failure_pop
if not exist "%ROM%" (
  echo ROM file not found: "%ROM%" 1>&2
  goto failure_pop
)
if not exist ".venv\Scripts\python.exe" (
  py -3 -m venv .venv
  if errorlevel 1 python -m venv .venv
  if errorlevel 1 goto failure_pop
)
".venv\Scripts\python.exe" -m pip install PyYAML capstone unicorn cmake ninja ziglang==0.16.0
if errorlevel 1 goto failure_pop
".venv\Scripts\python.exe" tools\build_port.py --rom "%ROM%"
if errorlevel 1 goto failure_pop
echo.
echo Build complete. Keep all generated files and game binaries private.
echo Run: PLAY-DQ9.bat
echo In the launcher, choose your ROM and set Save chip to EEPROM 64 KB for Dragon Quest IX EU.
popd
exit /b 0
:usage
echo Usage: %~nx0 "C:\path\game.nds" 1>&2
exit /b 2
:failure_pop
popd
:failure
echo BUILD-DQ9 failed. Read the error above; no game binary is distributed by this kit. 1>&2
exit /b 1
