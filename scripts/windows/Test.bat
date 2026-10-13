@echo off
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Debug
pushd "%~dp0..\.."

call "%~dp0Build.bat" %CONFIG% || goto :fail
"build\bin\%CONFIG%-windows-x86_64\DingoNet-Tests\DingoNet-Tests.exe" || goto :fail

popd
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~f0" >nul && pause
exit /b 0

:fail
popd
echo %CMDCMDLINE% | "%SystemRoot%\System32\find.exe" /i "%~f0" >nul && pause
exit /b 1
