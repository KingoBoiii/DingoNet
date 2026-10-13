@echo off
setlocal
set "DN_ARGS=%*"
pushd "%~dp0..\.."
call vendor\premake\bin\premake5.exe vs2026 %*
set RESULT=%ERRORLEVEL%
popd
rem Pause only when double-clicked from Explorer (no arguments, full path in the command line), so shells, scripts and CI never hang on it.
if not defined DN_ARGS echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~f0" >nul && pause
exit /b %RESULT%
