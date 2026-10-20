@echo off
setlocal
rem PLAY-DQ9.bat ["C:\path\game.nds" [en|fr|de|es|it]]
rem Without arguments the launcher opens; it remembers your ROM path, language and options.
set "ROOT=%~dp0"
if not exist "%ROOT%tools\build_port.py" set "ROOT=%~dp0..\.."
pushd "%ROOT%" || goto failure
if not exist "build\game\game\dq9.exe" (
  echo Game executable not found. Run BUILD-DQ9.bat with your own ROM first. 1>&2
  popd
  exit /b 1
)
if "%~1"=="" goto launcher
set "ROM=%~f1"
set "LANG=en"
if not "%~2"=="" set "LANG=%~2"
start "" /wait "build\game\game\dq9.exe" --rom "%ROM%" --abi-config "build\gen\abi.ini" --language "%LANG%"
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
:launcher
start "" /wait "build\game\game\dq9.exe" --launcher --abi-config "build\gen\abi.ini"
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
:failure
echo Cannot open the kit directory. 1>&2
exit /b 1
