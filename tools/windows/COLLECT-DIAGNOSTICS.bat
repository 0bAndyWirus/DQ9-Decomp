@echo off
setlocal DisableDelayedExpansion
set "ROOT=%~dp0"
if not exist "%ROOT%tools\collect_diagnostics.py" set "ROOT=%~dp0..\..\"
if exist "%ROOT%.venv\Scripts\python.exe" goto venv
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)" >nul 2>&1
if not errorlevel 1 goto py
python -c "import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)" >nul 2>&1
if not errorlevel 1 goto python
echo Python 3.10 or newer was not found. Install Python and add it to PATH.
exit /b 2
:venv
"%ROOT%.venv\Scripts\python.exe" "%ROOT%tools\collect_diagnostics.py" --kit-dir "%ROOT%."
goto result
:py
py -3 "%ROOT%tools\collect_diagnostics.py" --kit-dir "%ROOT%."
goto result
:python
python "%ROOT%tools\collect_diagnostics.py" --kit-dir "%ROOT%."
:result
set "RESULT=%errorlevel%"
exit /b %RESULT%
