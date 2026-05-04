@echo off
setlocal
cd /d "%~dp0"

if not exist "%~dp0libgcc_s_seh-1.dll" (
    echo [ERROR] Dependencies not deployed. Run: .\scripts\build.ps1 -Deploy
    pause
    exit /b 1
)
if not exist "%~dp0Qt6Core.dll" (
    echo [ERROR] Qt6 runtime not found. Run: .\scripts\build.ps1 -Deploy
    pause
    exit /b 1
)

"%~dp0airplay_receiver.exe"
exit /b %errorlevel%
