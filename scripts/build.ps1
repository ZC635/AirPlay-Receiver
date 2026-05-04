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

    Write-Host "  Copying runtime DLLs..." -ForegroundColor Gray
    $exclude = @("gcc", "g++", "cpp", "cc1", "cc1plus", "collect2", "ld", "ar", "as", "nm",
        "objdump", "objcopy", "ranlib", "strip", "readelf", "addr2line", "c++filt",
        "cmake", "ctest", "cpack", "ninja", "make", "pkg-config", "meson",
        "python", "perl", "bash", "git", "curl", "wget", "ssh", "scp")
    $exePatterns = ($exclude | ForEach-Object { "$_.exe" })
    Get-ChildItem $MSys2Bin -Filter "*.dll" | Where-Object {
        $n = $_.Name.ToLower()
        $exePatterns -notcontains $n -and
        ($n -like "lib*" -or $n -like "av*" -or $n -like "sw*" -or
         $n -like "zlib*" -or $n -like "xvid*" -or $n -like "sdl*" -or
         $n -like "D3D*" -or $n -like "Qt*" -or $n -like "dx*")
    } | Copy-Item -Destination $BuildDir -Force

    Write-Host "  Copying GStreamer plugins..." -ForegroundColor Gray
    $PluginOutDir = Join-Path $BuildDir "gstreamer-plugins"
    if (-not (Test-Path $PluginOutDir)) { New-Item -ItemType Directory -Path $PluginOutDir -Force | Out-Null }
    Get-ChildItem $PluginDir -Filter "*.dll" | Copy-Item -Destination $PluginOutDir -Force

    $LauncherSrc = Join-Path $ProjectRoot "scripts\launcher.cmd"
    if (Test-Path $LauncherSrc) { Copy-Item $LauncherSrc $BuildDir -Force }

    Write-Host "  Done. $((Get-ChildItem $BuildDir -Filter *.dll | Measure-Object).Count) DLLs, $((Get-ChildItem $PluginOutDir -Filter *.dll | Measure-Object).Count) plugins" -ForegroundColor Green
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
