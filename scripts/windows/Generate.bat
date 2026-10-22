@echo off
setlocal
set "DN_ARGS=%*"
pushd "%~dp0..\.."
call vendor\premake\bin\premake5.exe vs2026 %*
set RESULT=%ERRORLEVEL%
popd
if not defined DN_ARGS call :explorer "%~f0" && pause
exit /b %RESULT%

:explorer
rem Only a double-click pauses. PowerShell's "& script.bat" starts cmd with the same command line as Explorer's, so the launching process decides.
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~1" >nul || exit /b 1
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$cmd = Get-CimInstance Win32_Process -Filter ('ProcessId=' + (Get-CimInstance Win32_Process -Filter ('ProcessId=' + $PID)).ParentProcessId); if ((Get-Process -Id $cmd.ParentProcessId -ErrorAction SilentlyContinue).Name -eq 'explorer') { exit 0 } else { exit 1 }" >nul 2>&1
exit /b %ERRORLEVEL%
