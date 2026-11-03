@echo off
chcp 65001 >nul
title Pikaso Workstation - provera nove verzije
rem Odmah proverava GitHub i instalira novu verziju u Desktop\Pikaso Workstation (ne ceka 15 minuta).
set SCRIPT=%LOCALAPPDATA%\LiveSampler\LiveSampler-AutoUpdate.ps1
if not exist "%SCRIPT%" set SCRIPT=%~dp0LiveSampler-AutoUpdate.ps1
echo Proveravam novu verziju...
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Pokazi
echo.
pause
