@echo off
rem Setup.bat - one-time setup: copies your own Army Men II files, lifts them and builds the host.
rem Needs your own copy of the game (retail disc, GOG or Steam install) and Python 3 (see README.md).
rem Fetches pcrecomp, cnc-ddraw and a .venv itself, and offers to install the Visual Studio 2022 Build Tools
rem (the C++ compiler) when they are missing.
setlocal
cd /d "%~dp0"

echo Enter the folder that holds your ArmyMen2.exe (the disc, a copy of its files, or your install).
echo Press Enter alone to let the script look for a GOG or Steam install.
set /p FROM=Game folder:
rem Typed quotes are dropped; a trailing backslash (E:\) would escape the closing quote of -From "..."
if defined FROM set "FROM=%FROM:"=%"
if defined FROM if "%FROM:~-1%"=="\" set "FROM=%FROM%."

rem Windows PowerShell 5.1, which every Windows 10 and 11 has (no PowerShell 7 needed)
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if "%FROM%"=="" (
  "%PS%" -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1
) else (
  "%PS%" -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1 -From "%FROM%"
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
