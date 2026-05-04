param(
    [switch]$Clean,
    [switch]$Test,
    [switch]$Run,
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$BuildDir = Join-Path $ProjectRoot "build-uxplay"
$MSys2Root = "C:\msys64\ucrt64"
$MSys2Bin = "$MSys2Root\bin"
$MSys2Lib = "$MSys2Root\lib"
$PluginDir = "$MSys2Lib\gstreamer-1.0"

if (-not (Test-Path "$MSys2Bin\cmake.exe")) {
    Write-Error "MSYS2 UCRT64 not found at $MSys2Root"
    exit 1
}

$env:PATH = "$MSys2Bin;$env:PATH"

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
} else {
    Write-Error "Exe not found after build"
    exit 1
}

if ($Deploy) {
    Write-Host "`n=== Deploy: bundling dependencies ===" -ForegroundColor Cyan

    Write-Host "  windeployqt..." -ForegroundColor Gray
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & "$MSys2Bin\windeployqt.exe" $ExePath --no-translations --no-compiler-runtime 2>&1 | Out-Null
    $ErrorActionPreference = $prevEAP
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "windeployqt failed (continuing)"
    }

    $RuntimeDlls = @(
        "libgcc_s_seh-1.dll",
        "libstdc++-6.dll",
        "libwinpthread-1.dll",
        "libssp-0.dll"
    )
    Write-Host "  MSYS2 runtime DLLs..." -ForegroundColor Gray
    foreach ($dll in $RuntimeDlls) {
        $src = Join-Path $MSys2Bin $dll
        if (Test-Path $src) { Copy-Item $src $BuildDir -Force }
    }

    $GstCoreDlls = Get-ChildItem $MSys2Bin -Filter "libgst*-1.0-0.dll" | ForEach-Object { $_.Name }
    $GstOtherDlls = @(
        "libglib-2.0-0.dll", "libgobject-2.0-0.dll", "libgio-2.0-0.dll",
        "libgmodule-2.0-0.dll", "libintl-8.dll", "libiconv-2.dll",
        "liborc-0.4-0.dll", "liborc-test-0.4-0.dll",
        "libpcre2-8-0.dll", "libffi-8.dll", "zlib1.dll",
        "libcrypto-3-x64.dll", "libssl-3-x64.dll",
        "libplist-2.0.dll", "libpugixml.dll", "libbrotlicommon.dll",
        "libbrotlidec.dll", "libbz2-1.dll", "liblzma-5.dll",
        "libxml2-16.dll", "libpng16-16.dll", "libjpeg-8.dll",
        "libtasn1-6.dll", "libnettle-8.dll", "libgmp-10.dll",
        "libhogweed-6.dll", "libp11-kit-0.dll",
        "libunistring-5.dll", "libidn2-0.dll",
        "libsoup-3.0-0.dll", "libpsl-5.dll",
        "libsqlite3-0.dll"
    )
    Write-Host "  GStreamer + system DLLs..." -ForegroundColor Gray
    foreach ($dll in ($GstCoreDlls + $GstOtherDlls)) {
        $src = Join-Path $MSys2Bin $dll
        if (Test-Path $src) { Copy-Item $src $BuildDir -Force }
    }

    $PluginOutDir = Join-Path $BuildDir "gstreamer-plugins"
    Write-Host "  GStreamer plugins..." -ForegroundColor Gray
    if (-not (Test-Path $PluginOutDir)) { New-Item -ItemType Directory -Path $PluginOutDir -Force | Out-Null }
    Get-ChildItem $PluginDir -Filter "*.dll" | Copy-Item -Destination $PluginOutDir -Force

    Write-Host "  Done. Output: $BuildDir" -ForegroundColor Green
}

if ($Test) {
    Write-Host "`n=== CTest ===" -ForegroundColor Cyan
    ctest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { Write-Error "Tests failed"; exit 1 }
    Write-Host "Tests passed" -ForegroundColor Green
}

if ($Run) {
    Write-Host "`n=== Launching ===" -ForegroundColor Cyan
    if ($Deploy) {
        Start-Process -FilePath $ExePath -WindowStyle Normal
    } else {
        $env:GST_PLUGIN_PATH = $PluginDir
        $env:PATH = "$MSys2Bin;$env:PATH"
        Start-Process -FilePath $ExePath -WindowStyle Normal
    }
}
