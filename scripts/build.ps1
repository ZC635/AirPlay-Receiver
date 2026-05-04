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
    $patterns = @(
        "libgst*.dll", "libgstreamer*.dll",
        "libglib*.dll", "libgio*.dll", "libgobject*.dll", "libgmodule*.dll",
        "libcrypto*.dll", "libssl*.dll",
        "avcodec*.dll", "avformat*.dll", "avutil*.dll", "avfilter*.dll",
        "libswresample*.dll", "libswscale*.dll", "libpostproc*.dll",
        "libopenh264*.dll", "libfdk*.dll", "libfaad*.dll",
        "libvorbis*.dll", "libogg*.dll", "libopus*.dll", "libvpx*.dll",
        "libx264*.dll", "libx265*.dll",
        "libmp3lame*.dll", "libflac*.dll", "libspeex*.dll",
        "libwavpack*.dll", "libsndfile*.dll",
        "libfreetype*.dll", "libharfbuzz*.dll", "libpng*.dll", "libjpeg*.dll",
        "libwebp*.dll", "libtiff*.dll",
        "libicuin*.dll", "libicuuc*.dll", "libicudt*.dll",
        "libpcre2*.dll", "libzstd*.dll", "libdouble-conversion*.dll",
        "libbrotli*.dll", "libbz2*.dll", "liblzma*.dll", "zlib*.dll",
        "liborc*.dll", "libffi*.dll", "libintl*.dll", "libiconv*.dll",
        "libplist*.dll", "libpugixml*.dll", "libxml2*.dll",
        "libsoup*.dll", "libpsl*.dll", "libsqlite*.dll",
        "libunistring*.dll", "libidn*.dll", "libtasn*.dll",
        "libnettle*.dll", "libgmp*.dll", "libhogweed*.dll", "libp11*.dll",
        "libcairo*.dll", "libpango*.dll", "libgdk_pixbuf*.dll",
        "librsvg*.dll", "libpixman*.dll",
        "libfontconfig*.dll", "libfribidi*.dll", "libthai*.dll",
        "libdatrie*.dll", "libexpat*.dll",
        "libcurl*.dll", "libnghttp*.dll", "libssh*.dll",
        "libd3dcompiler*.dll", "libdrm*.dll", "libGLESv2*.dll", "libEGL*.dll",
        "libgcc_s*.dll", "libstdc++*.dll", "libwinpthread*.dll", "libssp*.dll",
        "libgomp*.dll", "libatomic*.dll",
        "libtag*.dll", "libgraphene*.dll", "libepoxy*.dll",
        "libass*.dll", "libcaca*.dll", "libmodplug*.dll", "libopenmpt*.dll",
        "libsoundtouch*.dll", "libspandsp*.dll",
        "libopenal*.dll", "libfluidsynth*.dll",
        "libdca*.dll", "libfaac*.dll", "libmpcdec*.dll",
        "libdvdnav*.dll", "libdvdread*.dll", "libbluray*.dll",
        "libshout*.dll", "liblc3*.dll", "libsrtp*.dll",
        "libiex*.dll", "libimath*.dll", "libopenexr*.dll",
        "libzbar*.dll", "libvmaf*.dll", "libchromaprint*.dll",
        "libaom*.dll", "libdav1d*.dll", "librav1e*.dll",
        "libsvtav1enc*.dll", "libde265*.dll",
        "librtmp*.dll", "libsrt*.dll", "libmicrodns*.dll", "libnice*.dll",
        "libjson-glib*.dll", "libtheora*.dll", "libgsm*.dll", "libsbc*.dll",
        "libtwolame*.dll", "libmpg123*.dll", "libgme*.dll",
        "libopencore-amrnb*.dll", "libopencore-amrwb*.dll",
        "libvo-amrwbenc*.dll", "libopenjp2*.dll", "liblcms2*.dll",
        "libva*.dll", "libvidstab*.dll", "libvpl*.dll",
        "libzimg*.dll", "libzvbi*.dll", "libdeflate*.dll",
        "libplacebo*.dll", "libshaderc*.dll", "libunibreak*.dll",
        "libsharpyuv*.dll", "liblerc*.dll", "libjxl*.dll", "libjbig*.dll",
        "libgnutls*.dll", "libngtcp2*.dll",
        "libreadline*.dll", "libportaudio*.dll",
        "xvidcore*.dll", "sdl3*.dll", "libdvdcss*.dll",
        "libmd4c*.dll", "libgraphite2*.dll"
    )
    foreach ($patt in $patterns) {
        Get-ChildItem $MSys2Bin -Filter $patt -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName $BuildDir -Force
        }
    }

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
