param(
    [switch]$Deploy
)

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$BuildDir = Join-Path $ProjectRoot "build-uxplay"
$ExePath = Join-Path $BuildDir "airplay_receiver.exe"
$MSys2Root = "C:\msys64\ucrt64"

$env:GST_PLUGIN_PATH = "$MSys2Root\lib\gstreamer-1.0"
$env:PATH = "$MSys2Root\bin;$env:PATH"

if (-not (Test-Path $ExePath)) {
    Write-Host "Not built yet. Running build..." -ForegroundColor Yellow
    & (Join-Path $ProjectRoot "scripts\build.ps1")
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

if ($Deploy) {
    Write-Host "=== windeployqt ===" -ForegroundColor Cyan
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & "$MSys2Root\bin\windeployqt.exe" $ExePath --no-translations --no-compiler-runtime 2>&1 | Out-Null
    $ErrorActionPreference = $prevEAP
    Write-Host "Done" -ForegroundColor Green
}

$standalone = (Test-Path "$BuildDir\libgcc_s_seh-1.dll") -and (Test-Path "$BuildDir\gstreamer-plugins")
if ($standalone) {
    Write-Host "Launching (standalone mode)" -ForegroundColor Green
    Start-Process -FilePath $ExePath -WindowStyle Normal
} else {
    Write-Host "Launching (MSYS2 PATH mode)" -ForegroundColor Green
    Start-Process -FilePath $ExePath -WindowStyle Normal
}
