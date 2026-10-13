@echo off
setlocal
pushd "%~dp0..\.."
call vendor\premake\bin\premake5.exe vs2026 %*
set RESULT=%ERRORLEVEL%
popd
rem Pause only when double-clicked from Explorer, so shells and CI never hang on it.
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~f0" >nul && pause
exit /b %RESULT%
