@echo off
setlocal
set APP=%~1
if "%APP%"=="" set APP=%~dp0..\emulator\build\smalltv-emulator.exe
set DEVICE=%~2
if "%DEVICE%"=="" set DEVICE=http://smalltv.local
schtasks /Create /SC ONLOGON /TN SmallTVUsageWorker /TR "\"%APP%\" --usage-worker --usage-device \"%DEVICE%\"" /F
if errorlevel 1 exit /b 1
echo Installed SmallTV usage worker scheduled task.
