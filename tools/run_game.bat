@echo off
rem Usage: run_game.bat [--menu] [en^|fr^|de^|es^|it] [--rom PATH] [dq9 options...]
rem Select an already available compatible image with --rom PATH or DQ9_ROM.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_game.ps1" %*
exit /b %errorlevel%
