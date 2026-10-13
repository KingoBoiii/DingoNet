@echo off
setlocal
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
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~f0" >nul && pause
exit /b 0

:fail
popd
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~f0" >nul && pause
exit /b 1
