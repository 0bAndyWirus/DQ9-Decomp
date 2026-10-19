@echo off
setlocal
if not defined DQ9_ZIG_PYTHON set "DQ9_ZIG_PYTHON=%~dp0..\..\.venv\Scripts\python.exe"
"%DQ9_ZIG_PYTHON%" -m ziglang c++ -target x86_64-windows-gnu %*
exit /b %errorlevel%
