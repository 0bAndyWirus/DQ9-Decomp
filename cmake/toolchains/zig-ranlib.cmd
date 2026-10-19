@echo off
setlocal
if not defined DQ9_ZIG_PYTHON set "DQ9_ZIG_PYTHON=%~dp0..\..\.venv\Scripts\python.exe"
"%DQ9_ZIG_PYTHON%" -m ziglang ranlib %*
exit /b %errorlevel%
