@echo off
rem Installs Polish for every user on this PC (OBS reads %ProgramData%\obs-studio\plugins).
rem Asks for administrator rights once. Restart OBS afterwards.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "DEST=%ProgramData%\obs-studio\plugins"
if not exist "%DEST%" mkdir "%DEST%"
if exist "%DEST%\obs-polish" rmdir /s /q "%DEST%\obs-polish"
xcopy /e /i /q /y "%~dp0obs-polish" "%DEST%\obs-polish" >nul
echo Installed to %DEST%\obs-polish
echo Restart OBS, then use Tools ^> Polish, or add the Polish filter to a camera source.
pause
