@echo off
call "%~dp0install_windows.bat" %*
if errorlevel 1 (
  echo Auto-start setup failed.
  pause
  exit /b 1
)
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 "%~dp0setup_ui.py" --config "%~dp0usage-daemon.json"
) else (
  python "%~dp0setup_ui.py" --config "%~dp0usage-daemon.json"
)
if errorlevel 1 pause
