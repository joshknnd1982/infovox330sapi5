@echo off
rem Opens the speech engine's log folder for whoever runs this, which is not necessarily
rem the account that ran the installer - hence a script rather than a fixed shortcut.
set "LOGDIR=%LOCALAPPDATA%\Infovox330 SAPI5\Logs"
if not exist "%LOGDIR%" (
    echo No logs have been written yet.
    echo They will appear in: %LOGDIR%
    echo.
    pause
    exit /b 0
)
start "" "%LOGDIR%"
