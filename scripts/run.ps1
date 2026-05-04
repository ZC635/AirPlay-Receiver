param(
    [switch]$Deploy
)

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$BuildDir = Join-Path $ProjectRoot "build-uxplay"
$ExePath = Join-Path $BuildDir "airplay_receiver.exe"
$MSys2Root = "C:\msys64\ucrt64"

$env:PATH = "$MSys2Root\bin;$env:PATH"
$env:GST_PLUGIN_PATH = "$MSys2Root\lib\gstreamer-1.0"

if (-not (Test-Path $ExePath)) {
    Write-Host "Not built yet. Running build first..." -ForegroundColor Yellow
    & (Join-Path $ProjectRoot "scripts\build.ps1")
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

if ($Deploy) {
    Write-Host "=== windeployqt ===" -ForegroundColor Cyan
    & "$MSys2Root\bin\windeployqt.exe" $ExePath --no-translations 2>&1
    Write-Host "Deployment complete" -ForegroundColor Green
}

Write-Host "Launching $ExePath" -ForegroundColor Green
Start-Process -FilePath $ExePath -WindowStyle Normal
