@echo off
setlocal
title Redstone - Prepare guest-room materials
set "REDSTONE_ROOT=D:\pcmaster\openshadersfsrredstone"
where pwsh.exe >nul 2>&1
if errorlevel 1 (
    set "REDSTONE_POWERSHELL=powershell.exe"
) else (
    set "REDSTONE_POWERSHELL=pwsh.exe"
)
"%REDSTONE_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%REDSTONE_ROOT%\tools\redstone-scene-prep\PrepareAndDeployGuestRoom.ps1"
set "REDSTONE_RESULT=%ERRORLEVEL%"
echo.
if not "%REDSTONE_RESULT%"=="0" echo Preparation or deployment failed. Read the message above and the preparation log.
pause
exit /b %REDSTONE_RESULT%
