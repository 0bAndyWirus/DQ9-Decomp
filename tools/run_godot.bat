@echo off
rem Usage: run_godot.bat [--menu] [en^|fr^|de^|es^|it] [--rom PATH] [user options...]
rem Select an already available compatible image with --rom PATH or DQ9_ROM.
rem GODOT must point to the Godot 4.5 Windows x64 editor executable.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_godot.ps1" %*
exit /b %errorlevel%
