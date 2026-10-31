@echo off
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 "%~dp0setup_ui.py" --config "%~dp0usage-daemon.json" %*
) else (
  python "%~dp0setup_ui.py" --config "%~dp0usage-daemon.json" %*
)
if errorlevel 1 pause
