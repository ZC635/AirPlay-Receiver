param(
    [switch]$Deploy,
    [string]$MSys2Root,
    [switch]$AssumeYes,
    [switch]$SkipInstall
)

$ErrorActionPreference = "Stop"

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
            if (Test-Path (Join-Path $binPath "cmake.exe")) {
                return (Resolve-Path $candidatePath).Path
            }
        }
    }

    return $null
}

function Get-StandaloneMissingRequirements {
    param([string]$BuildDir)

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
        $fullPath = Join-Path $BuildDir $relativePath
        if (-not (Test-Path $fullPath)) {
            $missing += $relativePath
        }
    }

    return $missing
}

function Get-MSys2InstallRootCandidates {
    param([string]$RequestedRoot)

    $candidates = @($RequestedRoot, $env:AIRPLAY_MSYS2_ROOT, "C:\msys64") | Where-Object { $_ }
    $installRoots = @()
    foreach ($rawCandidate in $candidates) {
        $candidate = [Environment]::ExpandEnvironmentVariables($rawCandidate.Trim('"')).TrimEnd('\')
        $leaf = Split-Path $candidate -Leaf
        if (($leaf -ieq "ucrt64") -or ($leaf -ieq "mingw64")) {
            $candidate = Split-Path $candidate -Parent
        }
        if (Test-Path $candidate) {
            $installRoots += (Resolve-Path $candidate).Path
        }
    }

    return $installRoots | Select-Object -Unique
}

function Remove-PathTreeEntries {
    param(
        [string]$PathValue,
        [string[]]$Roots
    )

    $normalizedRoots = @()
    foreach ($root in $Roots) {
        $normalizedRoots += [System.IO.Path]::GetFullPath($root).TrimEnd('\')
    }

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
        $underRemovedRoot = $looksLikeMsys2Bin
        foreach ($root in $normalizedRoots) {
            if ($normalizedEntry.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
                $underRemovedRoot = $true
                break
            }
        }

        if (-not $underRemovedRoot) {
            $entries += $entry
        }
    }

    return ($entries -join ';')
}

function Start-PortableProcess {
    param(
        [string]$ExePath,
        [string]$WorkingDirectory,
        [string]$RequestedMSys2Root
    )

    $previousPath = $env:PATH
    $hadGstPluginPath = Test-Path Env:GST_PLUGIN_PATH
    $previousGstPluginPath = $env:GST_PLUGIN_PATH
    $msys2InstallRoots = @(Get-MSys2InstallRootCandidates $RequestedMSys2Root)

    try {
        $env:PATH = Remove-PathTreeEntries $env:PATH $msys2InstallRoots
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
$ExePath = Join-Path $BuildDir "airplay_receiver.exe"
$BuildScript = Join-Path $ProjectRoot "scripts\build.ps1"

if ((-not (Test-Path $ExePath)) -or $Deploy) {
    if (-not (Test-Path $ExePath)) {
        Write-Host "Not built yet. Running build..." -ForegroundColor Yellow
    } else {
        Write-Host "Refreshing deployed runtime..." -ForegroundColor Yellow
    }

    $buildArgs = @()
    if ($Deploy) { $buildArgs += "-Deploy" }
    if ($MSys2Root) { $buildArgs += @("-MSys2Root", $MSys2Root) }
    if ($AssumeYes) { $buildArgs += "-AssumeYes" }
    if ($SkipInstall) { $buildArgs += "-SkipInstall" }

    & $BuildScript @buildArgs
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

$standaloneMissing = @(Get-StandaloneMissingRequirements $BuildDir)
if ($standaloneMissing.Count -eq 0) {
    Write-Host "Launching (standalone mode)" -ForegroundColor Green
    Start-PortableProcess $ExePath $BuildDir $MSys2Root
    exit 0
}

if (Test-Path (Join-Path $BuildDir "gstreamer-plugins")) {
    Write-Warning "Standalone runtime is incomplete; falling back to MSYS2 PATH mode. Missing: $($standaloneMissing -join ', ')"
}

$ResolvedMSys2Root = Resolve-MSys2Root $MSys2Root
if (-not $ResolvedMSys2Root) {
    throw "Build is not deployed and MSYS2 was not found. Run .\scripts\build.ps1 -Deploy or pass -MSys2Root C:\msys64\ucrt64."
}

$MSys2Bin = Join-Path $ResolvedMSys2Root "bin"
$PluginDir = Join-Path $ResolvedMSys2Root "lib\gstreamer-1.0"
$env:GST_PLUGIN_PATH = $PluginDir
$env:PATH = "$MSys2Bin;$env:PATH"

Write-Host "Launching (MSYS2 PATH mode: $ResolvedMSys2Root)" -ForegroundColor Green
Start-Process -FilePath $ExePath -WindowStyle Normal
