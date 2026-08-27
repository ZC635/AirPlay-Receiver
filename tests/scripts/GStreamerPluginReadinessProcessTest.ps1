param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class AirPlayPluginReadinessNative {
    [DllImport("kernel32.dll")]
    public static extern uint GetACP();
}
"@

function Invoke-RuntimeVerification {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.Arguments = "--verify-recording-runtime"
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true

    $process = [System.Diagnostics.Process]::new()
    try {
        $process.StartInfo = $startInfo
        [void]$process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Timed out waiting 30 seconds for runtime verification."
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdoutTask.GetAwaiter().GetResult()
            Stderr = $stderrTask.GetAwaiter().GetResult()
        }
    } finally {
        $process.Dispose()
    }
}

function Add-RuntimeFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $destinationDirectory = Split-Path -Parent $Destination
    if (-not [System.IO.Directory]::Exists($destinationDirectory)) {
        [System.IO.Directory]::CreateDirectory($destinationDirectory) | Out-Null
    }
    try {
        New-Item -ItemType HardLink -Path $Destination -Target $Source -ErrorAction Stop | Out-Null
    } catch {
        Copy-Item -LiteralPath $Source -Destination $Destination -ErrorAction Stop
    }
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$packageRoot = Split-Path -Parent $resolvedExecutable
$sourceResult = Invoke-RuntimeVerification -FilePath $resolvedExecutable -WorkingDirectory $packageRoot
if ($sourceResult.ExitCode -ne 0) {
    throw "ASCII source package is invalid. Exit: $($sourceResult.ExitCode) Stdout: $($sourceResult.Stdout) Stderr: $($sourceResult.Stderr)"
}

$sample = "中文路径"
$encoding = [System.Text.Encoding]::GetEncoding(
    [int][AirPlayPluginReadinessNative]::GetACP(),
    [System.Text.EncoderFallback]::ExceptionFallback,
    [System.Text.DecoderFallback]::ExceptionFallback)
try {
    $roundTrip = $encoding.GetString($encoding.GetBytes($sample))
} catch {
    Write-Host "Skipping because the active ANSI code page cannot represent the Chinese fixture path."
    exit 77
}
if ($roundTrip -ne $sample) {
    Write-Host "Skipping because the Chinese fixture path does not round-trip through the active ANSI code page."
    exit 77
}

$tempBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
$testRoot = Join-Path $tempBase ("airplay-plugin-readiness-中文路径-" + [System.Guid]::NewGuid().ToString("N"))
$runtimeDirectories = @(
    "config", "generic", "gstreamer-1.0", "gstreamer-plugins", "imageformats",
    "libexec", "networkinformation", "platforms", "styles", "tls"
)

try {
    [System.IO.Directory]::CreateDirectory($testRoot) | Out-Null
    Get-ChildItem -LiteralPath $packageRoot -File | Where-Object {
        $_.Extension -in @(".dll", ".exe")
    } | ForEach-Object {
        Add-RuntimeFile -Source $_.FullName -Destination (Join-Path $testRoot $_.Name)
    }

    foreach ($directoryName in $runtimeDirectories) {
        $sourceDirectory = Join-Path $packageRoot $directoryName
        if (-not [System.IO.Directory]::Exists($sourceDirectory)) {
            continue
        }
        Get-ChildItem -LiteralPath $sourceDirectory -Recurse -File | ForEach-Object {
            $relative = $_.FullName.Substring($sourceDirectory.Length).TrimStart('\')
            Add-RuntimeFile -Source $_.FullName -Destination (
                Join-Path (Join-Path $testRoot $directoryName) $relative)
        }
    }

    $copiedExecutable = Join-Path $testRoot "airplay_receiver.exe"
    $result = Invoke-RuntimeVerification -FilePath $copiedExecutable -WorkingDirectory $testRoot
    if ($result.ExitCode -eq 0) {
        Write-Host "Skipping because this host loads GStreamer plugins from the Chinese fixture path."
        exit 77
    }
    if ($result.ExitCode -ne 4) {
        throw "Expected exit code 4, got $($result.ExitCode). Stdout: $($result.Stdout) Stderr: $($result.Stderr)"
    }
    if ($result.Stderr -notmatch '(?i)missing GStreamer plugins') {
        throw "Expected readiness failure text. Stderr: $($result.Stderr)"
    }
    foreach ($plugin in @("app", "libav", "playback", "autodetect", "videoparsersbad")) {
        if ($result.Stderr -notmatch [regex]::Escape($plugin)) {
            throw "Expected missing plugin '$plugin'. Stderr: $($result.Stderr)"
        }
    }
    if ($result.Stderr -match '(?i)unsupported application path') {
        throw "Expected actual plugin probing, not early path rejection. Stderr: $($result.Stderr)"
    }
} finally {
    if ([System.IO.Directory]::Exists($testRoot)) {
        $resolvedTarget = [System.IO.Path]::GetFullPath($testRoot)
        if (-not $resolvedTarget.StartsWith($tempBase + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove unexpected test path: $resolvedTarget"
        }
        Remove-Item -LiteralPath $resolvedTarget -Recurse -Force -ErrorAction Stop
    }
}
