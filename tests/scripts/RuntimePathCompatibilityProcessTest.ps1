param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class AirPlayRuntimePathNative {
    [DllImport("kernel32.dll")]
    public static extern uint GetACP();
}
"@

if ([AirPlayRuntimePathNative]::GetACP() -eq 65001) {
    Write-Host "Skipping runtime-path compatibility process test because ACP is UTF-8."
    exit 77
}

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Executable was not found: $Executable"
}

$emoji = [string][char]0xD83D + [char]0xDE00
$tempPath = Join-Path ([System.IO.Path]::GetTempPath()) (
    "airplay-runtime-path-" + $emoji + "-" + [System.Guid]::NewGuid().ToString("N"))
$process = $null

try {
    New-Item -ItemType Directory -Path $tempPath -ErrorAction Stop | Out-Null
    $copiedExecutable = Join-Path $tempPath "airplay_receiver.exe"
    Copy-Item -LiteralPath $Executable -Destination $copiedExecutable -ErrorAction Stop

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $copiedExecutable
    $startInfo.WorkingDirectory = $tempPath
    $startInfo.Arguments = "--verify-recording-runtime"
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(10000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Timed out waiting 10 seconds for runtime-path compatibility process to exit."
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()

    if ($process.ExitCode -ne 3) {
        throw "Expected exit code 3, got $($process.ExitCode). Stdout: $stdout Stderr: $stderr"
    }
    if ($stderr -notmatch '(?i)unsupported application path') {
        throw "Expected stderr to mention unsupported application path. Stderr: $stderr"
    }
    if ($stderr -match '(?i)missing factories') {
        throw "Expected path rejection before GStreamer probing. Stderr: $stderr"
    }
} finally {
    if ($null -ne $process) {
        $process.Dispose()
    }
    if ([System.IO.Directory]::Exists($tempPath)) {
        Remove-Item -LiteralPath $tempPath -Recurse -Force -ErrorAction Stop
    }
}
