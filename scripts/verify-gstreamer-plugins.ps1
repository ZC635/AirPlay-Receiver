[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$package = (Resolve-Path -LiteralPath $PackageDir -ErrorAction Stop).Path
$inspector = Join-Path $package 'gst-inspect-1.0.exe'
if (!(Test-Path -LiteralPath $inspector -PathType Leaf)) { throw "Missing package-local GStreamer inspector: $inspector" }
$pluginDirectory = Join-Path $package 'gstreamer-plugins'
$plugins = @(Get-ChildItem -LiteralPath $pluginDirectory -Filter '*.dll' -File | Sort-Object Name)
if (!$plugins.Count) { throw "No bundled GStreamer plugins found: $pluginDirectory" }
$runId = [guid]::NewGuid().ToString('N')
$registryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('airplay-gst-probe-' + $runId)
New-Item -ItemType Directory -Path $registryDirectory | Out-Null
$registry = Join-Path $registryDirectory 'registry.bin'
try {
    foreach($plugin in $plugins) {
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $inspector
        $start.WorkingDirectory = $package
        $start.Arguments = '"' + $plugin.FullName + '"'
        $start.UseShellExecute = $false
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $start.CreateNoWindow = $true
        # No host DLL or plugin paths. Explicit filenames bypass a cached blacklist.
        $start.EnvironmentVariables['PATH'] = $package + ';' + [Environment]::SystemDirectory + ';' + $env:SystemRoot
        foreach($key in @($start.EnvironmentVariables.Keys)) {
            if ($key -match '^GST_') { $start.EnvironmentVariables.Remove($key) }
        }
        foreach($suffix in @('', '_1_0')) {
            $start.EnvironmentVariables["GST_PLUGIN_PATH$suffix"] = ''
            $start.EnvironmentVariables["GST_PLUGIN_SYSTEM_PATH$suffix"] = ''
            $start.EnvironmentVariables["GST_REGISTRY$suffix"] = $registry
        }
        $start.EnvironmentVariables['GST_REGISTRY_FORK'] = 'no'
        $p = [Diagnostics.Process]::new()
        $p.StartInfo = $start
        try {
            if (!$p.Start()) { throw "GStreamer plugin runtime probe could not start: $($plugin.FullName)" }
            $stdoutTask = $p.StandardOutput.ReadToEndAsync()
            $stderrTask = $p.StandardError.ReadToEndAsync()
            if (!$p.WaitForExit(30000)) {
                $p.Kill()
                if (!$p.WaitForExit(5000)) { throw "Owned GStreamer probe could not stop: $($plugin.FullName)" }
                throw "GStreamer plugin runtime probe timed out: $($plugin.FullName)"
            }
            if (!$stdoutTask.Wait(5000) -or !$stderrTask.Wait(5000)) { throw "GStreamer probe output did not close: $($plugin.FullName)" }
            $stdout = $stdoutTask.Result
            $stderr = $stderrTask.Result
            if ($p.ExitCode -ne 0) {
                throw "GStreamer plugin runtime probe failed: $($plugin.FullName); exit=$($p.ExitCode); $stdout $stderr"
            }
            $filename = [regex]::Match($stdout, '(?m)^\s*Filename\s+([^\r\n]+)\s*$')
            if (!$filename.Success -or [IO.Path]::GetFullPath($filename.Groups[1].Value.Trim()) -ine $plugin.FullName) {
                throw "GStreamer plugin runtime probe returned an unexpected plugin origin: $($plugin.FullName); $stdout $stderr"
            }
            # Keep successful-process native warnings visible as well.
            if ($stderr) { Write-Output $stderr }
            Write-Output "Verified GStreamer plugin: $($plugin.FullName)"
        } finally { $p.Dispose() }
    }
    Write-Output "GStreamer plugins verified: $($plugins.Count)"
} finally {
    # Only known files and this newly-created directory; never a recursive delete.
    if (Test-Path -LiteralPath $registry) { Remove-Item -LiteralPath $registry -Force }
    Remove-Item -LiteralPath $registryDirectory -Force
}
