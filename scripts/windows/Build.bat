@echo off
setlocal
set "DN_ARGS=%*"
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Debug
pushd "%~dp0..\.."

if not exist DingoNet.slnx (
	call vendor\premake\bin\premake5.exe vs2026 || goto :fail
)

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if not defined MSBUILD (
	echo MSBuild not found. Install Visual Studio with the C++ workload.
	goto :fail
)

"%MSBUILD%" DingoNet.slnx /p:Configuration=%CONFIG% /p:Platform=x64 /m /v:minimal /nologo || goto :fail

popd
if not defined DN_ARGS call :explorer "%~f0" && pause
exit /b 0

:fail
popd
if not defined DN_ARGS call :explorer "%~f0" && pause
exit /b 1

:explorer
rem Only a double-click pauses. PowerShell's "& script.bat" starts cmd with the same command line as Explorer's, so the launching process decides.
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~1" >nul || exit /b 1
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$cmd = Get-CimInstance Win32_Process -Filter ('ProcessId=' + (Get-CimInstance Win32_Process -Filter ('ProcessId=' + $PID)).ParentProcessId); if ((Get-Process -Id $cmd.ParentProcessId -ErrorAction SilentlyContinue).Name -eq 'explorer') { exit 0 } else { exit 1 }" >nul 2>&1
exit /b %ERRORLEVEL%
