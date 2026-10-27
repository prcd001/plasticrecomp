@echo off
rem Setup.bat - one-time setup: copies your own Army Men II files, lifts them and builds the host.
rem Needs your own copy of the game (retail disc, GOG or Steam install), Git, Python 3, PowerShell 7 (pwsh)
rem and the Visual Studio 2022 Build Tools (see README.md). Fetches pcrecomp, cnc-ddraw and a .venv itself.
setlocal
cd /d "%~dp0"

echo Enter the folder that holds your ArmyMen2.exe (the disc, a copy of its files, or your install).
echo Press Enter alone to let the script look for a GOG or Steam install.
set /p FROM=Game folder:

where pwsh >nul 2>nul
if errorlevel 1 (
  echo PowerShell 7 ^(pwsh^) is needed. Get it from https://aka.ms/powershell
  pause
  exit /b 1
)
if "%FROM%"=="" (
  pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1
) else (
  pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1 -From "%FROM%"
)
if errorlevel 1 (
  echo.
  echo Setup did not finish. Read the messages above.
  pause
  exit /b 1
)
echo.
echo Done. Double-click Play.bat to start the game.
pause
endlocal
