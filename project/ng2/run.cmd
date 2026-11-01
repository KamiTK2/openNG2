@echo off
rem Launch the ng2 recomp against ..\..\game. Log goes to notes\logs\play.log.
rem Usage: run.cmd [preset]   (default win-amd64-relwithdebinfo)
setlocal
set PRESET=%~1
if "%PRESET%"=="" set PRESET=win-amd64-relwithdebinfo
set ROOT=%~dp0..\..
cd /d "%~dp0out\build\%PRESET%" || exit /b 1
start "" ng2.exe --game_data_root="%ROOT%\game" --log_file="%ROOT%\notes\logs\play.log" --log_level=info
