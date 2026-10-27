@echo off
setlocal enabledelayedexpansion
set "DN_ARGS=%*"
pushd "%~dp0..\.."

set CONFIGS=%*
if "%CONFIGS%"=="" set CONFIGS=Debug Release Distribution

set /p BASE=<VERSION
if not defined DN_VERSION set DN_VERSION=%BASE%.0
for /f "tokens=1-3 delims=." %%a in ("%DN_VERSION%") do (
	set V_MAJOR=%%a& set V_MINOR=%%b& set V_PATCH=%%c
)
if defined DN_BUILD_NUMBER (
	set BUILD=%DN_BUILD_NUMBER%
) else (
	for /f %%i in ('git rev-list --count HEAD') do set BUILD=%%i
)
if not defined BUILD (
	echo Could not work out the build number.
	goto :fail
)
if not defined V_PATCH (
	echo DN_VERSION must be MAJOR.MINOR.PATCH.
	goto :fail
)
set VER=%DN_VERSION%.%BUILD%

if not exist build\package mkdir build\package
copy /y include\DingoNet\BuildInfo.h build\package\BuildInfo.h.bak >nul || goto :fail
set RESTORE=1
(
	echo #pragma once
	echo.
	echo #define DN_VERSION_MAJOR %V_MAJOR%
	echo #define DN_VERSION_MINOR %V_MINOR%
	echo #define DN_VERSION_PATCH %V_PATCH%
	echo #define DN_VERSION_BUILD %BUILD%
) > include\DingoNet\BuildInfo.h

for %%C in (%CONFIGS%) do (
	call :norm %%C || goto :fail
	if not defined DN_NO_BUILD call "%~dp0Build.bat" !CFG! || goto :fail
	call :package || goto :fail
)

call :restore
popd
if not defined DN_ARGS call :explorer "%~f0" && pause
exit /b 0

:norm
set CFG=
if /i "%~1"=="debug" set CFG=Debug
if /i "%~1"=="release" set CFG=Release
if /i "%~1"=="distribution" set CFG=Distribution
if not defined CFG (
	echo Unknown configuration: %~1
	exit /b 1
)
exit /b 0

:package
set STAGE=build\package\stage-%CFG%-windows
set ZIP=build\package\DingoNet-%VER%-%CFG%-windows-x86_64.zip
set LIBDIR=build\bin\%CFG%-windows-x86_64\DingoNet
if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "%ZIP%" del "%ZIP%"
mkdir "%STAGE%\lib\%CFG%" || exit /b 1
xcopy /e /i /q /y include "%STAGE%\include" >nul || exit /b 1
xcopy /e /i /q /y steam "%STAGE%\steam" >nul || exit /b 1
copy /y "%LIBDIR%\DingoNet.lib" "%STAGE%\lib\%CFG%\" >nul || exit /b 1
if exist "%LIBDIR%\DingoNet.pdb" copy /y "%LIBDIR%\DingoNet.pdb" "%STAGE%\lib\%CFG%\" >nul
copy /y LICENSE.txt "%STAGE%\" >nul || exit /b 1
copy /y vendor\enet\LICENSE "%STAGE%\THIRD_PARTY.txt" >nul || exit /b 1
> "%STAGE%\VERSION.txt" echo %VER%
"%SystemRoot%\System32\tar.exe" -a -c -f "%ZIP%" -C "%STAGE%" include lib steam LICENSE.txt THIRD_PARTY.txt VERSION.txt || exit /b 1
rmdir /s /q "%STAGE%"
echo Packaged %ZIP%
exit /b 0

:restore
if defined RESTORE (
	copy /y build\package\BuildInfo.h.bak include\DingoNet\BuildInfo.h >nul
	rem copy keeps the backup's old timestamp, which would leave objects built from the stamped header looking current.
	copy /b include\DingoNet\BuildInfo.h +,, include\DingoNet\BuildInfo.h >nul
	del build\package\BuildInfo.h.bak
	set RESTORE=
)
exit /b 0

:fail
call :restore
popd
if not defined DN_ARGS call :explorer "%~f0" && pause
exit /b 1

:explorer
rem Only a double-click pauses. PowerShell's "& script.bat" starts cmd with the same command line as Explorer's, so the launching process decides.
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~1" >nul || exit /b 1
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command "$cmd = Get-CimInstance Win32_Process -Filter ('ProcessId=' + (Get-CimInstance Win32_Process -Filter ('ProcessId=' + $PID)).ParentProcessId); if ((Get-Process -Id $cmd.ParentProcessId -ErrorAction SilentlyContinue).Name -eq 'explorer') { exit 0 } else { exit 1 }" >nul 2>&1
exit /b %ERRORLEVEL%
