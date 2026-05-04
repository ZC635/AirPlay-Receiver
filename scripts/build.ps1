param(
    [switch]$Clean,
    [switch]$Test,
    [switch]$Run,
    [ValidateSet("Debug", "Release")]
    [string]$BuildType = "Release"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$BuildDir = Join-Path $ProjectRoot "build-uxplay"
$MSys2Root = "C:\msys64\ucrt64"

if (-not (Test-Path "$MSys2Root\bin\cmake.exe")) {
    Write-Error "MSYS2 UCRT64 not found at $MSys2Root"
    exit 1
}

$env:PATH = "$MSys2Root\bin;$env:PATH"
$env:GST_PLUGIN_PATH = "$MSys2Root\lib\gstreamer-1.0"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

Write-Host "=== CMake Configure (UxPlay=ON) ===" -ForegroundColor Cyan
cmake -S $ProjectRoot -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DAIRPLAY_WITH_UXPLAY=ON

if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }

Write-Host "`n=== CMake Build ===" -ForegroundColor Cyan
cmake --build $BuildDir

if ($LASTEXITCODE -ne 0) { Write-Error "CMake build failed"; exit 1 }

$ExePath = Join-Path $BuildDir "airplay_receiver.exe"
if (Test-Path $ExePath) {
    $info = Get-Item $ExePath
    Write-Host "`nBuild succeeded: $ExePath ($($info.Length) bytes)" -ForegroundColor Green
}

if ($Test) {
    Write-Host "`n=== CTest ===" -ForegroundColor Cyan
    ctest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { Write-Error "Tests failed"; exit 1 }
    Write-Host "Tests passed" -ForegroundColor Green
}

if ($Run) {
    Write-Host "`n=== Launching ===" -ForegroundColor Cyan
    Start-Process -FilePath $ExePath -WindowStyle Normal
}
