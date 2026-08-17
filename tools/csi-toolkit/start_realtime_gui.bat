@echo off
cd /d "%~dp0"
where pythonw >nul 2>nul
if %errorlevel%==0 (
    start "" pythonw "%~dp0csi_realtime_gui.py"
) else (
    start "" python "%~dp0csi_realtime_gui.py"
)
