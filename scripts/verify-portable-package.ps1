[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir,
    [switch]$SkipRuntimeProbe
)

$ErrorActionPreference = "Stop"

if ($SkipRuntimeProbe -and $env:AIRPLAY_PORTABLE_FIXTURE_TEST -ne '1') {
    throw '-SkipRuntimeProbe is reserved for isolated validation fixtures.'
}

function Get-PortableRuntimeManifestPaths {
    $projectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
    $manifestPath = Join-Path $projectRoot "config\portable-runtime-manifest.txt"
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Portable runtime manifest was not found: $manifestPath"
    }

    return Get-Content -LiteralPath $manifestPath |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and (-not $_.StartsWith("#")) } |
        ForEach-Object { $_.Replace('/', '\') }
}

function Remove-MSys2PathEntries {
    param([string]$PathValue)

    $kept = @()
    foreach ($entry in ($PathValue -split ';')) {
        if (-not $entry) { continue }
        if ($entry -match '(?i)\\msys64\\' -or
            $entry -match '(?i)\\(ucrt64|mingw64|clang64|clangarm64|mingw32)\\bin$') {
            continue
        }
        $kept += $entry
    }
    return ($kept -join ';')
}

function Restore-EnvironmentVariable {
    param(
        [string]$Name,
        [bool]$WasSet,
        [string]$Value
    )

    if ($WasSet) {
        Set-Item -Path "Env:$Name" -Value $Value
    } else {
        Remove-Item -Path "Env:$Name" -ErrorAction SilentlyContinue
    }
}

if (-not (Test-Path -LiteralPath $PackageDir)) {
    throw "Portable package directory was not found: $PackageDir"
}

$requiredPaths = @(Get-PortableRuntimeManifestPaths)
$diagnosticLauncherName = 'Start with Diagnostic Logging.cmd'
$requiredPaths += $diagnosticLauncherName

$missing = @()
foreach ($relativePath in $requiredPaths) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageDir $relativePath))) {
        $missing += $relativePath
    }
}

$forbiddenRootDlls = @(
    "D3DCompiler_47.dll",
    "D3DCompiler_46.dll",
    "D3DCompiler_43.dll",
    "dxcompiler.dll",
    "dxil.dll"
)

$forbidden = @()
foreach ($dll in $forbiddenRootDlls) {
    if (Test-Path -LiteralPath (Join-Path $PackageDir $dll)) {
        $forbidden += $dll
    }
}

$errors = @()
if ($missing.Count -ne 0 -or $forbidden.Count -ne 0) {
    if ($missing.Count -ne 0) {
        $errors += "Portable package is missing required files: $($missing -join ', ')"
    }
    if ($forbidden.Count -ne 0) {
        $errors += "Portable package contains non-portable system DLLs: $($forbidden -join ', ')"
    }

    throw ($errors -join "`n")
}

if (-not $SkipRuntimeProbe) {
    & (Join-Path $PSScriptRoot 'verify-gstreamer-plugins.ps1') -PackageDir $PackageDir

    $trackedEnvironment = @(
        'PATH',
        'GST_PLUGIN_PATH', 'GST_PLUGIN_PATH_1_0',
        'GST_PLUGIN_SYSTEM_PATH', 'GST_PLUGIN_SYSTEM_PATH_1_0',
        'GST_REGISTRY', 'GST_REGISTRY_1_0',
        'GST_PLUGIN_SCANNER', 'GST_PLUGIN_SCANNER_1_0'
    )
    $savedEnvironment = @{}
    foreach ($name in $trackedEnvironment) {
        $savedEnvironment[$name] = @{
            WasSet = Test-Path "Env:$name"
            Value = [Environment]::GetEnvironmentVariable($name)
        }
    }

    try {
        $resolvedPackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
        $pluginDir = Join-Path $resolvedPackageDir 'gstreamer-plugins'
        $registryPath = Join-Path $resolvedPackageDir 'gstreamer-1.0\registry.x86_64.bin'
        $env:PATH = "$resolvedPackageDir;$(Remove-MSys2PathEntries $env:PATH)"
        $env:GST_PLUGIN_PATH = $pluginDir
        $env:GST_PLUGIN_PATH_1_0 = $pluginDir
        $env:GST_PLUGIN_SYSTEM_PATH = $pluginDir
        $env:GST_PLUGIN_SYSTEM_PATH_1_0 = $pluginDir
        $env:GST_REGISTRY = $registryPath
        $env:GST_REGISTRY_1_0 = $registryPath
        $pluginScanner = Join-Path $resolvedPackageDir 'libexec\gstreamer-1.0\gst-plugin-scanner.exe'
        $env:GST_PLUGIN_SCANNER = $pluginScanner
        $env:GST_PLUGIN_SCANNER_1_0 = $pluginScanner

        $probeExecutable = Join-Path $resolvedPackageDir 'airplay_receiver.exe'
        $probeStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $probeStartInfo.FileName = $probeExecutable
        $probeStartInfo.WorkingDirectory = $resolvedPackageDir
        $probeStartInfo.Arguments = '--verify-recording-runtime'
        $probeStartInfo.UseShellExecute = $false
        $probeStartInfo.RedirectStandardOutput = $true
        $probeStartInfo.RedirectStandardError = $true
        $probeStartInfo.CreateNoWindow = $true

        . (Join-Path $PSScriptRoot 'portable-recording-probe.ps1')
        $probeOutput = @(Invoke-PortableRecordingProbe -StartInfo $probeStartInfo)
        foreach ($line in $probeOutput) {
            $line
        }
    } finally {
        foreach ($name in $trackedEnvironment) {
            $saved = $savedEnvironment[$name]
            Restore-EnvironmentVariable $name $saved.WasSet $saved.Value
        }
    }
}

"Portable package verification passed: $PackageDir"
