@echo off
rem Collect-Nioh3FpsLogs.bat - gathers everything nioh3fps writes into one zip on the Desktop.
rem Usage: double-click it in the Nioh 3 folder, or: Collect-Nioh3FpsLogs.bat "D:\path\to\Nioh3"
rem Without an argument it also tries the default Steam library (registry SteamPath).
rem Read-only for the game folder: files are copied, nothing there is changed or deleted.
setlocal EnableExtensions

set "GAME=%~1"
if not defined GAME if exist "%~dp0Nioh3.exe" set "GAME=%~dp0"
if not defined GAME if exist "%~dp0dinput8.dll" set "GAME=%~dp0"
if not defined GAME (
  for /f "tokens=2,*" %%A in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul') do set "STEAM=%%B"
)
if defined STEAM set "STEAM=%STEAM:/=\%"
if not defined GAME if defined STEAM if exist "%STEAM%\steamapps\common\Nioh3\Nioh3.exe" set "GAME=%STEAM%\steamapps\common\Nioh3"
if not defined GAME (
  echo Could not find the Nioh 3 folder. Run: %~nx0 "path\to\Nioh3"
  pause
  exit /b 1
)
if "%GAME:~-1%"=="\" set "GAME=%GAME:~0,-1%"

for /f "usebackq delims=" %%T in (`powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"`) do set "STAMP=%%T"
set "STAGE=%TEMP%\nioh3fps_report_%STAMP%"
for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "[Environment]::GetFolderPath('Desktop')"`) do set "DESK=%%D"
set "ZIP=%DESK%\nioh3fps_report_%STAMP%.zip"
mkdir "%STAGE%\game" "%STAGE%\localappdata" 2>nul

echo Game folder: %GAME%
call :collect "%GAME%" "%STAGE%\game"
if exist "%LOCALAPPDATA%\nioh3fps" call :collect "%LOCALAPPDATA%\nioh3fps" "%STAGE%\localappdata"

rem folder listing (names, sizes, dates) of the game folder root and the fallback log folder
dir /a "%GAME%" > "%STAGE%\listing_game.txt" 2>&1
if exist "%LOCALAPPDATA%\nioh3fps" dir /a "%LOCALAPPDATA%\nioh3fps" > "%STAGE%\listing_localappdata.txt" 2>&1
ver > "%STAGE%\system.txt"
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,CurrentRefreshRate | Format-List" >> "%STAGE%\system.txt" 2>nul

rem Optional: convert flight-recorder files that have no CSV yet. Needs Python and trace2csv.py
rem next to this script; otherwise skipped (the .bin files are in the zip and the mod already
rem converts the previous session's trace itself at startup).
set "CONV=skipped: trace2csv.py not next to this script"
if exist "%~dp0trace2csv.py" (
  set "CONV=skipped: Python not found"
  where python >nul 2>&1 && set "CONV=done with %~dp0trace2csv.py"
)
if "%CONV:~0,4%"=="done" (
  for /r "%STAGE%" %%B in (nioh3fps_trace*.bin) do if not exist "%%~dpnB.csv" python "%~dp0trace2csv.py" "%%B" "%%~dpnB.csv" >> "%STAGE%\collect_notes.txt" 2>&1
)
echo trace conversion: %CONV%
echo trace conversion: %CONV%>> "%STAGE%\collect_notes.txt"

powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 (
  echo Zipping failed. The files are in %STAGE%
  pause
  exit /b 1
)
rmdir /s /q "%STAGE%"
echo.
echo Report written to: %ZIP%
echo Please attach this zip to your report.
pause
exit /b 0

:collect
rem %1 = source folder, %2 = destination folder
rem nioh3fps_ticks.csv.old (up to 64 MB) is left out to keep the zip small; the
rem current ticks file, the 30 s flight recorder and the summaries cover a report.
for %%F in (nioh3fps.log nioh3fps.log.old nioh3fps.ini nioh3fps_probe.csv nioh3fps_ticks.csv nioh3fps_hist.csv nioh3fps_hist.csv.old nioh3fps_trace.bin nioh3fps_trace.prev.bin nioh3fps_trace.prev.csv BUILD_ID.txt) do (
  if exist "%~1\%%F" copy /y "%~1\%%F" "%~2\" >nul
)
for %%F in ("%~1\nioh3fps_trace_dump_*.csv" "%~1\nioh3fps_crash_*.dmp") do copy /y "%%~F" "%~2\" >nul 2>&1
exit /b 0
