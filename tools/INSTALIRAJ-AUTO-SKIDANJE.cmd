@echo off
rem Jednom pokreni (dupli klik). Pravi zakazani zadatak koji svakih 15 minuta proverava novu verziju
rem i skida je na Desktop u folder "Live Sampler".
set SCRIPT=%LOCALAPPDATA%\LiveSampler\LiveSampler-AutoUpdate.ps1
if not exist "%LOCALAPPDATA%\LiveSampler" mkdir "%LOCALAPPDATA%\LiveSampler"
copy /Y "%~dp0LiveSampler-AutoUpdate.ps1" "%SCRIPT%" >nul
schtasks /Create /F /SC MINUTE /MO 15 /TN "LiveSampler AutoUpdate" /TR "powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"%SCRIPT%\"" >nul
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"
echo.
echo Gotovo. Najnovija verzija je na Desktopu u folderu "Live Sampler".
echo Svaka nova verzija ce se sama skinuti (provera na 15 minuta).
pause
