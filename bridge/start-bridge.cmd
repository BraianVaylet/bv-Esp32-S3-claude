@echo off
REM Double-clickable launcher for the bridge.
REM
REM Keeps the window open when the bridge exits, so a crash or a port clash is
REM readable instead of a console that vanishes. For an unattended setup use
REM install-windows-task.ps1 instead, which starts it at every logon.

cd /d "%~dp0"
title Claude Usage Bridge

where node >nul 2>&1
if errorlevel 1 (
    echo.
    echo   node was not found on PATH.
    echo   Install Node 18 or newer from https://nodejs.org and try again.
    echo.
    pause
    exit /b 1
)

node bridge.mjs

echo.
echo   The bridge stopped. If the port was already in use, it is probably
echo   already running in another window or as a scheduled task.
echo.
pause
