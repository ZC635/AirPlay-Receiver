@echo off
setlocal
start "" "%~dp0airplay_receiver.exe" --diagnostic-log
exit /b %errorlevel%
