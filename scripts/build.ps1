param(
    [switch]$Clean,
    [switch]$Test,
    [switch]$Run,
    [switch]$Deploy,
    [string]$MSys2Root,
    [switch]$AssumeYes,
    [switch]$SkipInstall
)

$ErrorActionPreference = "Stop"

$RequiredPackageSuffixes = @(
    "gcc",
    "cmake",
    "ninja",
    "qt6-base",
    "pkgconf",
    "libplist",
    "openssl",
    "gstreamer",
    "gst-plugins-base",
    "gst-plugins-good",
    "gst-plugins-bad",
    "gst-libav"
)

function Resolve-MSys2Root {
    param([string]$RequestedRoot)

    $rawCandidates = @(
        $RequestedRoot,
        $env:AIRPLAY_MSYS2_ROOT,
        "C:\msys64\ucrt64",
        "C:\msys64\mingw64",
        "C:\msys64"
    ) | Where-Object { $_ }

    foreach ($rawCandidate in $rawCandidates) {
        $candidate = [Environment]::ExpandEnvironmentVariables($rawCandidate.Trim('"'))
        $candidatePaths = @(
            $candidate,
            (Join-Path $candidate "ucrt64"),
            (Join-Path $candidate "mingw64")
        )

        foreach ($candidatePath in $candidatePaths) {
            $binPath = Join-Path $candidatePath "bin"
            if (Test-Path $binPath) {
                return (Resolve-Path $candidatePath).Path
            }
        }
    }

    return $null
}

function Get-MSys2PackagePrefix {
    param([string]$Root)

    $leaf = Split-Path $Root -Leaf
    if ($leaf -ieq "mingw64") {
        return "mingw-w64-x86_64"
    }
    return "mingw-w64-ucrt-x86_64"
}

function Resolve-Pacman {
    param([string]$Root)

    $installRoot = Split-Path $Root -Parent
    $candidates = @(
        (Join-Path $installRoot "usr\bin\pacman.exe"),
        (Join-Path $Root "bin\pacman.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    return $null
}

function Test-PkgConfigModule {
    param(
        [string]$PkgConfigPath,
        [string]$Module
    )

    & $PkgConfigPath --exists $Module 2>$null
    return $LASTEXITCODE -eq 0
}

function Get-MSys2MissingRequirements {
    param(
        [string]$BinPath,
        [string]$LibPath,
        [string]$PluginPath
    )

    $checks = @(
        @{ Name = "C++ compiler"; Path = (Join-Path $BinPath "c++.exe") },
        @{ Name = "C compiler"; Path = (Join-Path $BinPath "cc.exe") },
        @{ Name = "CMake"; Path = (Join-Path $BinPath "cmake.exe") },
        @{ Name = "Ninja"; Path = (Join-Path $BinPath "ninja.exe") },
        @{ Name = "windeployqt"; Path = (Join-Path $BinPath "windeployqt.exe") },
        @{ Name = "pkg-config"; Path = (Join-Path $BinPath "pkg-config.exe") },
        @{ Name = "Qt6 runtime"; Path = (Join-Path $BinPath "Qt6Core.dll") },
        @{ Name = "GStreamer runtime"; Path = (Join-Path $BinPath "libgstreamer-1.0-0.dll") },
        @{ Name = "GStreamer plugins directory"; Path = $PluginPath },
        @{ Name = "GStreamer app plugin"; Path = (Join-Path $PluginPath "libgstapp.dll") },
        @{ Name = "GStreamer playback plugin"; Path = (Join-Path $PluginPath "libgstplayback.dll") },
        @{ Name = "GStreamer autodetect plugin"; Path = (Join-Path $PluginPath "libgstautodetect.dll") },
        @{ Name = "GStreamer video parsers plugin"; Path = (Join-Path $PluginPath "libgstvideoparsersbad.dll") },
        @{ Name = "GStreamer libav plugin"; Path = (Join-Path $PluginPath "libgstlibav.dll") }
    )

    $missing = @()
    foreach ($check in $checks) {
        if (-not (Test-Path $check.Path)) {
            $missing += "$($check.Name): $($check.Path)"
        }
    }

    $pkgConfigPath = Join-Path $BinPath "pkg-config.exe"
    if (Test-Path $pkgConfigPath) {
        foreach ($module in @("libplist-2.0", "gstreamer-1.0", "gstreamer-sdp-1.0", "gstreamer-video-1.0", "gstreamer-app-1.0")) {
            if (-not (Test-PkgConfigModule $pkgConfigPath $module)) {
                $missing += "pkg-config module: $module"
            }
        }
    }

    return $missing
}

function Ensure-MSys2Requirements {
    param(
        [string]$Root,
        [string]$BinPath,
        [string]$LibPath,
        [string]$PluginPath,
        [switch]$AssumeYes,
        [switch]$SkipInstall
    )

    $missing = @(Get-MSys2MissingRequirements $BinPath $LibPath $PluginPath)
    if ($missing.Count -eq 0) {
        Write-Host "MSYS2 dependencies found at $Root" -ForegroundColor Green
        return
    }

    Write-Warning "Missing MSYS2 build/runtime requirements:"
    foreach ($item in $missing) {
        Write-Warning "  $item"
    }

    $prefix = Get-MSys2PackagePrefix $Root
    $packages = $RequiredPackageSuffixes | ForEach-Object { "$prefix-$_" }
    $packageCommand = "pacman -S --needed " + ($packages -join " ")

    if ($SkipInstall) {
        throw "Install the missing MSYS2 packages, then rerun this script. Suggested command: $packageCommand"
    }

    $pacman = Resolve-Pacman $Root
    if (-not $pacman) {
        throw "pacman.exe was not found. Install packages manually from an MSYS2 shell: $packageCommand"
    }

    Write-Host "`nThe build can install/update the required MSYS2 packages with:" -ForegroundColor Cyan
    Write-Host "  $packageCommand" -ForegroundColor Gray

    $install = $AssumeYes
    if (-not $install) {
        $answer = Read-Host "Install these packages now? [y/N]"
        $install = $answer -match "^(y|yes)$"
    }

    if (-not $install) {
        throw "Missing MSYS2 packages. Run this command manually, then rerun build.ps1: $packageCommand"
    }

    & $pacman -S --needed --noconfirm @packages
    if ($LASTEXITCODE -ne 0) {
        throw "MSYS2 package installation failed"
    }

    $missingAfterInstall = @(Get-MSys2MissingRequirements $BinPath $LibPath $PluginPath)
    if ($missingAfterInstall.Count -ne 0) {
        throw "MSYS2 package installation completed, but requirements are still missing: $($missingAfterInstall -join '; ')"
    }
}

function Resolve-BonjourSdkRoot {
    $candidates = @(
        $env:BONJOUR_SDK_HOME,
        "C:\Program Files\Bonjour SDK",
        "C:\Program Files (x86)\Bonjour SDK"
    ) | Where-Object { $_ }

    foreach ($rawCandidate in $candidates) {
        $candidate = [Environment]::ExpandEnvironmentVariables($rawCandidate.Trim('"').TrimEnd('\'))
        $header = Join-Path $candidate "Include\dns_sd.h"
        $library = Join-Path $candidate "Lib\x64\dnssd.lib"
        if ((Test-Path $header) -and (Test-Path $library)) {
            return (Resolve-Path $candidate).Path
        }
    }

    return $null
}

function Ensure-BonjourSdk {
    $sdkRoot = Resolve-BonjourSdkRoot
    if (-not $sdkRoot) {
        throw @"
Bonjour SDK was not found. Install Bonjour SDK 3.0 so these files exist:
  C:\Program Files\Bonjour SDK\Include\dns_sd.h
  C:\Program Files\Bonjour SDK\Lib\x64\dnssd.lib

If the SDK is installed elsewhere, set BONJOUR_SDK_HOME to that directory before running build.ps1.
See README.md for Bonjour SDK/runtime installation notes.
"@
    }

    $env:BONJOUR_SDK_HOME = $sdkRoot
    Write-Host "Bonjour SDK found at $sdkRoot" -ForegroundColor Green
}

function Write-BonjourRuntimeStatus {
    try {
        $service = Get-Service -Name "Bonjour Service" -ErrorAction SilentlyContinue
    } catch {
        $service = $null
    }

    if (-not $service) {
        Write-Warning "Bonjour runtime service was not found. Build can continue, but AirPlay discovery needs Bonjour Print Services, iTunes, or another compatible mDNS service."
        return
    }

    if ($service.Status -ne "Running") {
        Write-Warning "Bonjour runtime service is $($service.Status). Build can continue, but start the service before using AirPlay discovery."
        return
    }

    Write-Host "Bonjour runtime service is running" -ForegroundColor Green
}

function Get-StandaloneMissingRequirements {
    param([string]$Directory)

    $requiredPaths = @(
        "airplay_receiver.exe",
        "Qt6Core.dll",
        "Qt6Gui.dll",
        "Qt6Widgets.dll",
        "platforms\qwindows.dll",
        "libgcc_s_seh-1.dll",
        "libstdc++-6.dll",
        "libwinpthread-1.dll",
        "libgstreamer-1.0-0.dll",
        "gstreamer-plugins\libgstapp.dll",
        "gstreamer-plugins\libgstplayback.dll",
        "gstreamer-plugins\libgstautodetect.dll",
        "gstreamer-plugins\libgstvideoparsersbad.dll",
        "gstreamer-plugins\libgstlibav.dll",
        "gstreamer-plugins\libgstd3d11.dll",
        "gstreamer-plugins\libgstwasapi.dll"
    )

    $missing = @()
    foreach ($relativePath in $requiredPaths) {
        $fullPath = Join-Path $Directory $relativePath
        if (-not (Test-Path $fullPath)) {
            $missing += $relativePath
        }
    }

    return $missing
}

function Remove-PathTreeEntries {
    param(
        [string]$PathValue,
        [string]$Root
    )

    $normalizedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd('\')
    $entries = @()
    foreach ($entry in ($PathValue -split ';')) {
        if (-not $entry) { continue }
        try {
            $normalizedEntry = [System.IO.Path]::GetFullPath($entry).TrimEnd('\')
        } catch {
            $entries += $entry
            continue
        }

        $looksLikeMsys2Bin = $normalizedEntry -match "(?i)\\(ucrt64|mingw64|clang64|clangarm64|mingw32)\\bin$" -or
            $normalizedEntry -match "(?i)\\msys64\\usr\\bin$"

        if ((-not $looksLikeMsys2Bin) -and (-not $normalizedEntry.StartsWith($normalizedRoot, [System.StringComparison]::OrdinalIgnoreCase))) {
            $entries += $entry
        }
    }

    return ($entries -join ';')
}

function Start-PortableProcess {
    param(
        [string]$ExePath,
        [string]$WorkingDirectory,
        [string]$MSys2Prefix
    )

    $previousPath = $env:PATH
    $hadGstPluginPath = Test-Path Env:GST_PLUGIN_PATH
    $previousGstPluginPath = $env:GST_PLUGIN_PATH
    $msys2InstallRoot = Split-Path $MSys2Prefix -Parent

    try {
        $env:PATH = Remove-PathTreeEntries $env:PATH $msys2InstallRoot
        Remove-Item Env:GST_PLUGIN_PATH -ErrorAction SilentlyContinue
        Start-Process -FilePath $ExePath -WorkingDirectory $WorkingDirectory -WindowStyle Normal
    } finally {
        $env:PATH = $previousPath
        if ($hadGstPluginPath) {
            $env:GST_PLUGIN_PATH = $previousGstPluginPath
        } else {
            Remove-Item Env:GST_PLUGIN_PATH -ErrorAction SilentlyContinue
        }
    }
}

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$BuildDir = Join-Path $ProjectRoot "build-uxplay"
$ResolvedMSys2Root = Resolve-MSys2Root $MSys2Root
if (-not $ResolvedMSys2Root) {
    throw "MSYS2 UCRT64 was not found. Install MSYS2, then rerun this script or pass -MSys2Root C:\msys64\ucrt64."
}

$MSys2Root = $ResolvedMSys2Root
$MSys2Bin = Join-Path $MSys2Root "bin"
$MSys2Lib = Join-Path $MSys2Root "lib"
$PluginDir = Join-Path $MSys2Lib "gstreamer-1.0"

$env:PATH = "$MSys2Bin;$env:PATH"

Ensure-MSys2Requirements $MSys2Root $MSys2Bin $MSys2Lib $PluginDir -AssumeYes:$AssumeYes -SkipInstall:$SkipInstall
Ensure-BonjourSdk
Write-BonjourRuntimeStatus

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

    $missingStandalone = @(Get-StandaloneMissingRequirements $BuildDir)
    if ($missingStandalone.Count -ne 0) {
        throw "Deploy output is incomplete. Missing: $($missingStandalone -join ', ')"
    }
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
        Start-PortableProcess $ExePath $BuildDir $MSys2Root
    } else {
        $env:GST_PLUGIN_PATH = $PluginDir
        $env:PATH = "$MSys2Bin;$env:PATH"
        Start-Process -FilePath $ExePath -WindowStyle Normal
    }
}
