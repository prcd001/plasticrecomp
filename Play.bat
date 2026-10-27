@echo off
rem Play.bat - the everyday way to start Army Men II on plasticrecomp. Double-click it.
rem Keeps the last run's log in build\last_run.log (send it along if something goes wrong).
setlocal
cd /d "%~dp0"

if not exist "build\plasticrecomp.exe" (
  echo plasticrecomp has not been set up yet. Double-click Setup.bat first.
  pause
  exit /b 1
)
if not exist "game\ArmyMen2.exe" (
  echo The game folder "game" is missing. Double-click Setup.bat first.
  pause
  exit /b 1
)
tasklist /FI "IMAGENAME eq plasticrecomp.exe" 2>nul | find /I "plasticrecomp.exe" >nul
if not errorlevel 1 (
  echo Army Men II is already running. Close it first.
  pause
  exit /b 1
)

rem The retail disc build asks for the CD when you pick SINGLE PLAYER or BOOT CAMP.
rem Insert the disc, or mount its image, before starting.

rem Extra options can be passed on, e.g.:  Play.bat --fullscreen
rem The host writes its own log (--log) and lets go of this console, so no terminal window stays open while the game runs.
start "" /min "%~dp0build\plasticrecomp.exe" --game "%~dp0game" --log "%~dp0build\last_run.log" %*
endlocal
exit /b 0
