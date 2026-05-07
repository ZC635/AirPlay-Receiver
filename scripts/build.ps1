[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$Clean,
    [switch]$Test,
    [switch]$Run,
    [switch]$Deploy,
    [string]$MSys2Root,
    [switch]$AssumeYes,
    [switch]$SkipInstall,
    [switch]$Portable,
    [switch]$All,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"

# Normalize GNU-style aliases because PowerShell only binds -Name parameters.
$NormalizedArgs = @()
if ($RemainingArgs.Count -gt 0) { $NormalizedArgs += $RemainingArgs }

if ($NormalizedArgs.Count -gt 0) {
    for ($i = 0; $i -lt $NormalizedArgs.Count; $i++) {
        $arg = [string]$NormalizedArgs[$i]
        if ($arg -ieq "--Clean") { $Clean = $true }
        elseif ($arg -ieq "--Test") { $Test = $true }
        elseif ($arg -ieq "--Run") { $Run = $true }
        elseif ($arg -ieq "--Deploy") { $Deploy = $true }
        elseif ($arg -ieq "--AssumeYes") { $AssumeYes = $true }
        elseif ($arg -ieq "--SkipInstall") { $SkipInstall = $true }
        elseif ($arg -ieq "--Portable") { $Portable = $true }
        elseif ($arg -ieq "--All") { $All = $true }
        elseif ($arg -match "(?i)^--MSys2Root=(.*)$") { $MSys2Root = $Matches[1] }
        elseif ($arg -ieq "--MSys2Root") {
            if ($i + 1 -ge $NormalizedArgs.Count) {
                throw "--MSys2Root requires a value."
            }
            $i++
            $MSys2Root = [string]$NormalizedArgs[$i]
            if ($MSys2Root.StartsWith("--", [System.StringComparison]::Ordinal)) {
                throw "--MSys2Root requires a value."
            }
        }
        elseif ((-not $MSys2Root) -and (-not $arg.StartsWith("-", [System.StringComparison]::Ordinal))) { $MSys2Root = $arg }
        else {
            throw "Unknown argument: $arg. Use PowerShell parameters such as -All, or supported double-dash aliases such as --All."
        }
    }
}

if ($All -and ($Deploy -or $Portable)) {
    throw "-All is mutually exclusive with -Deploy and -Portable. -All already builds both variants."
}
if ($Portable -and $Deploy) {
    throw "-Portable and -Deploy are mutually exclusive. -Portable already implies deployment."
}

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
    "gst-libav",
    "qmdnsengine"
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
        "gstreamer-plugins\libgstwasapi.dll",
        "libqmdnsengine.dll"
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
    $hadGstRegistry = Test-Path Env:GST_REGISTRY
    $previousGstRegistry = $env:GST_REGISTRY
    $msys2InstallRoot = Split-Path $MSys2Prefix -Parent

    try {
        $env:PATH = Remove-PathTreeEntries $env:PATH $msys2InstallRoot
        Remove-Item Env:GST_PLUGIN_PATH -ErrorAction SilentlyContinue
        $portableRegistry = Join-Path $WorkingDirectory "gstreamer-1.0\registry.x86_64.bin"
        if (Test-Path $portableRegistry) {
            $env:GST_REGISTRY = $portableRegistry
        }
        Start-Process -FilePath $ExePath -WorkingDirectory $WorkingDirectory -WindowStyle Normal
    } finally {
        $env:PATH = $previousPath
        if ($hadGstPluginPath) {
            $env:GST_PLUGIN_PATH = $previousGstPluginPath
        } else {
            Remove-Item Env:GST_PLUGIN_PATH -ErrorAction SilentlyContinue
        }
        if ($hadGstRegistry) {
            $env:GST_REGISTRY = $previousGstRegistry
        } else {
            Remove-Item Env:GST_REGISTRY -ErrorAction SilentlyContinue
        }
    }
}

function Invoke-Build {
    param(
        [string]$VariantBuildDir,
        [bool]$IsPortable,
        [bool]$ShouldDeploy
    )

    if ($Clean -and (Test-Path $VariantBuildDir)) {
        Write-Host "Cleaning build directory..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $VariantBuildDir
    }

    $BuildNinja = Join-Path $VariantBuildDir "build.ninja"
    if (Test-Path $BuildNinja) {
        $BuildNinjaTimestamp = (Get-Item $BuildNinja).LastWriteTime
        $SrcFiles = Get-ChildItem (Join-Path $ProjectRoot "src") -Recurse -Include "*.cpp","*.h" -ErrorAction SilentlyContinue
        $BuildFiles = @(
            (Get-Item (Join-Path $ProjectRoot "CMakeLists.txt") -ErrorAction SilentlyContinue),
            (Get-ChildItem (Join-Path $ProjectRoot "cmake") -Filter "*.cmake" -ErrorAction SilentlyContinue)
        )
        $latestSource = ($SrcFiles + $BuildFiles) |
            Where-Object { $_ } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($latestSource -and ($latestSource.LastWriteTime -gt $BuildNinjaTimestamp)) {
            $relativeName = $latestSource.FullName.Replace("$ProjectRoot\", "")
            Write-Host "Source changes detected (newest: $relativeName), forcing build system regeneration..." -ForegroundColor Yellow
            Remove-Item $BuildNinja -Force
        }
    }

    Write-Host "`n=== [$VariantBuildDir] CMake Configure (UxPlay=ON) ===" -ForegroundColor Cyan
    cmake -S $ProjectRoot -B $VariantBuildDir -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DAIRPLAY_WITH_UXPLAY=ON

    if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }

    Write-Host "`n=== [$VariantBuildDir] CMake Build ===" -ForegroundColor Cyan
    cmake --build $VariantBuildDir

    if ($LASTEXITCODE -ne 0) { Write-Error "CMake build failed"; exit 1 }

    $VariantExePath = Join-Path $VariantBuildDir "airplay_receiver.exe"
    if (Test-Path $VariantExePath) {
        $info = Get-Item $VariantExePath
        Write-Host "`nBuild succeeded: $VariantExePath ($($info.Length) bytes)" -ForegroundColor Green
    } else {
        Write-Error "Exe not found after build"
        exit 1
    }

    if ($ShouldDeploy) {
        Write-Host "`n=== Deploy: bundling dependencies ===" -ForegroundColor Cyan

        Write-Host "  windeployqt..." -ForegroundColor Gray
        $prevEAP = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $windeployqtOutput = & "$MSys2Bin\windeployqt.exe" $VariantExePath --no-translations --no-compiler-runtime 2>&1
        $windeployqtExit = $LASTEXITCODE
        $ErrorActionPreference = $prevEAP
        if ($windeployqtExit -ne 0) {
            Write-Error "windeployqt.exe failed with exit code ${windeployqtExit}: $($windeployqtOutput -join ' ')"
            exit $windeployqtExit
        }

        Write-Host "  Bundling libqmdnsengine.dll..." -ForegroundColor Gray
        $qmdnsSource = Join-Path $MSys2Bin "libqmdnsengine.dll"
        if (Test-Path $qmdnsSource) {
            Copy-Item $qmdnsSource $VariantBuildDir -Force
            Write-Host "    Copied libqmdnsengine.dll from $qmdnsSource" -ForegroundColor Gray
        } else {
            throw "libqmdnsengine.dll was not found. Install qmdnsengine MSYS2 package before building."
        }

        if ($IsPortable) {
            Write-Host "  Copying ALL runtime DLLs (portable mode)..." -ForegroundColor Gray
            Get-ChildItem $MSys2Bin -Filter "*.dll" | Copy-Item -Destination $VariantBuildDir -Force
        } else {
            Write-Host "  Copying filtered runtime DLLs..." -ForegroundColor Gray
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
            } | Copy-Item -Destination $VariantBuildDir -Force
        }

        Write-Host "  Copying GStreamer plugins..." -ForegroundColor Gray
        $PluginOutDir = Join-Path $VariantBuildDir "gstreamer-plugins"
        if (-not (Test-Path $PluginOutDir)) { New-Item -ItemType Directory -Path $PluginOutDir -Force | Out-Null }
        Get-ChildItem $PluginDir -Filter "*.dll" | Copy-Item -Destination $PluginOutDir -Force

        if ($IsPortable) {
            Write-Host "  Generating GStreamer registry cache..." -ForegroundColor Gray
            $registryDir = Join-Path $VariantBuildDir "gstreamer-1.0"
            if (-not (Test-Path $registryDir)) { New-Item -ItemType Directory -Path $registryDir -Force | Out-Null }
            $portableRegistry = Join-Path $registryDir "registry.x86_64.bin"
            $hadGstRegistryForScan = Test-Path Env:GST_REGISTRY
            $previousGstRegistryForScan = $env:GST_REGISTRY
            $env:GST_REGISTRY = $portableRegistry
            $hadGstPluginPathForScan = Test-Path Env:GST_PLUGIN_PATH
            $previousGstPluginPathForScan = $env:GST_PLUGIN_PATH
            $env:GST_PLUGIN_PATH = $PluginOutDir
            $prevEAP = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            $gstInspectOutput = & "$MSys2Bin\gst-inspect-1.0.exe" --gst-disable-registry-fork 2>&1
            $gstInspectExit = $LASTEXITCODE
            $ErrorActionPreference = $prevEAP
            if ($gstInspectExit -ne 0) {
                Write-Warning "gst-inspect-1.0.exe exited with code $gstInspectExit. GStreamer registry may be incomplete. $($gstInspectOutput -join ' ')"
            }
            if ($hadGstPluginPathForScan) {
                $env:GST_PLUGIN_PATH = $previousGstPluginPathForScan
            } else {
                Remove-Item Env:GST_PLUGIN_PATH -ErrorAction SilentlyContinue
            }
            if ($hadGstRegistryForScan) {
                $env:GST_REGISTRY = $previousGstRegistryForScan
            } else {
                Remove-Item Env:GST_REGISTRY -ErrorAction SilentlyContinue
            }
            if (Test-Path $portableRegistry) {
                Write-Host "    Registry generated at gstreamer-1.0\registry.x86_64.bin" -ForegroundColor Gray
            } else {
                Write-Warning "GStreamer registry was not generated. Plugin discovery may be slow."
            }
        }

        $LauncherSrc = Join-Path $ProjectRoot "scripts\launcher.cmd"
        if (Test-Path $LauncherSrc) { Copy-Item $LauncherSrc $VariantBuildDir -Force }

        $dllCount = (Get-ChildItem $VariantBuildDir -Filter *.dll -ErrorAction SilentlyContinue | Measure-Object).Count
        $pluginCount = (Get-ChildItem $PluginOutDir -Filter *.dll -ErrorAction SilentlyContinue | Measure-Object).Count
        Write-Host "  Done. ${dllCount} DLLs, ${pluginCount} plugins" -ForegroundColor Green

        $missingStandalone = @(Get-StandaloneMissingRequirements $VariantBuildDir)
        if ($missingStandalone.Count -ne 0) {
            throw "Deploy output is incomplete. Missing: $($missingStandalone -join ', ')"
        }
    }

    return @{ BuildDir = $VariantBuildDir; ExePath = $VariantExePath }
}

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if ($All) {
    $BuildDir = Join-Path $ProjectRoot "build-uxplay-portable"
} else {
    $BuildDir = if ($Portable) { Join-Path $ProjectRoot "build-uxplay-portable" } else { Join-Path $ProjectRoot "build-uxplay" }
}
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

if ($All) {
    $deployResult = Invoke-Build -VariantBuildDir (Join-Path $ProjectRoot "build-uxplay") -IsPortable:$false -ShouldDeploy:$true
    $portableResult = Invoke-Build -VariantBuildDir (Join-Path $ProjectRoot "build-uxplay-portable") -IsPortable:$true -ShouldDeploy:$true
    $BuildDir = $portableResult.BuildDir
    $ExePath = $portableResult.ExePath
} else {
    $result = Invoke-Build -VariantBuildDir $BuildDir -IsPortable:$Portable -ShouldDeploy:($Portable -or $Deploy)
    $BuildDir = $result.BuildDir
    $ExePath = $result.ExePath
}

if ($Test) {
    if ($All) {
        Write-Host "`n=== CTest [build-uxplay] ===" -ForegroundColor Cyan
        ctest --test-dir (Join-Path $ProjectRoot "build-uxplay") --output-on-failure
        if ($LASTEXITCODE -ne 0) { Write-Error "Tests failed in build-uxplay"; exit 1 }

        Write-Host "`n=== CTest [build-uxplay-portable] ===" -ForegroundColor Cyan
        ctest --test-dir (Join-Path $ProjectRoot "build-uxplay-portable") --output-on-failure
        if ($LASTEXITCODE -ne 0) { Write-Error "Tests failed in build-uxplay-portable"; exit 1 }
    } else {
        Write-Host "`n=== CTest ===" -ForegroundColor Cyan
        ctest --test-dir $BuildDir --output-on-failure
        if ($LASTEXITCODE -ne 0) { Write-Error "Tests failed"; exit 1 }
    }
    Write-Host "Tests passed" -ForegroundColor Green
}

if ($Run) {
    Write-Host "`n=== Launching ===" -ForegroundColor Cyan
    if ($Portable -or $Deploy -or $All) {
        Start-PortableProcess $ExePath $BuildDir $MSys2Root
    } else {
        $env:GST_PLUGIN_PATH = $PluginDir
        $env:PATH = "$MSys2Bin;$env:PATH"
        Start-Process -FilePath $ExePath -WindowStyle Normal
    }
}
