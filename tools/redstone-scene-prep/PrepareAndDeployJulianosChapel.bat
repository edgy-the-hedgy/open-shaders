@echo off
setlocal
title Redstone - Prepare Julianos chapel
where pwsh.exe >nul 2>&1
if errorlevel 1 (
    set "REDSTONE_POWERSHELL=powershell.exe"
) else (
    set "REDSTONE_POWERSHELL=pwsh.exe"
)
"%REDSTONE_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0PrepareAndDeployJulianosChapel.ps1"
set "REDSTONE_RESULT=%ERRORLEVEL%"
echo.
if not "%REDSTONE_RESULT%"=="0" echo Preparation or deployment failed. Read the preparation log.
pause
exit /b %REDSTONE_RESULT%
