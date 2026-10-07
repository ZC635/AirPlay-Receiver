[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$PackageDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'GStreamerRegistryProcess.ps1')
. (Join-Path $PSScriptRoot 'GStreamerRegistryLock.ps1')
$package = (Resolve-Path -LiteralPath $PackageDir).Path
function Assert-NoReparse([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse registry boundary: $cursor" }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}
Assert-NoReparse $package
$inspector = Join-Path $package 'gst-inspect-1.0.exe'
$scanner = Join-Path $package 'libexec/gstreamer-1.0/gst-plugin-scanner.exe'
foreach ($tool in @($inspector,$scanner)) {
    if (!(Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Required package-local GStreamer tool missing: $tool" }
}
$pluginDirectory = Join-Path $package 'gstreamer-plugins'
Assert-NoReparse $pluginDirectory
$plugins = @(Get-ChildItem -LiteralPath $pluginDirectory -Filter '*.dll' -File | Sort-Object Name)
if (!$plugins.Count) { throw "No bundled GStreamer plugins: $pluginDirectory" }
$registryDirectory = Join-Path $package 'gstreamer-1.0'
$destination = Join-Path $registryDirectory 'registry.x86_64.bin'
Assert-NoReparse $destination
if ((Test-Path -LiteralPath $destination) -and !(Test-Path -LiteralPath $destination -PathType Leaf)) { throw "Registry destination is not a file: $destination" }
if (!(Test-Path -LiteralPath $registryDirectory)) { New-Item -ItemType Directory -Path $registryDirectory | Out-Null }
$writeLease = Enter-AirPlayRegistryWriteLock $package
try {
$runId = [guid]::NewGuid().ToString('N')
$owned = Join-Path $registryDirectory ('.reg-' + $runId)
New-Item -ItemType Directory -Path $owned | Out-Null
$marker = Join-Path $owned '.owner'
[IO.File]::WriteAllText($marker, $runId)
$candidate = Join-Path $owned 'registry.bin'
$metadataRegistry = Join-Path $owned 'metadata.bin'
$probeIndex = 0
$scannerFailure = ''
$pending = Join-Path $registryDirectory ('.registry-' + $runId + '.bin')
$pendingCreated = $false
$cleanupAttempted = $false
function Remove-OwnedWork {
    # Only this freshly-created directory, with exact ownership marker and no links.
    $resolvedOwned = [IO.Path]::GetFullPath($owned)
    if (!$resolvedOwned.StartsWith([IO.Path]::GetFullPath($registryDirectory).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        [IO.File]::ReadAllText($marker) -cne $runId) { throw 'Registry temporary cleanup ownership failed' }
    Assert-NoReparse $resolvedOwned
    $linkedChildren = @(Get-ChildItem -LiteralPath $resolvedOwned -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
    if ($linkedChildren.Count) { throw 'Registry temporary directory contains reparse point; cleanup refused' }
    Remove-Item -LiteralPath $resolvedOwned -Recurse -Force
}
function Get-RegistryHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path); $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
function Invoke-Inspector([string]$Arguments, [string]$Registry, [bool]$Discover, [bool]$Update, [bool]$RequireScanner = $false) {
    $script:probeIndex++
    $stdoutPath = Join-Path $owned ("probe-$script:probeIndex-out.txt")
    $stderrPath = Join-Path $owned ("probe-$script:probeIndex-err.txt")
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $inspector; $start.Arguments = $Arguments; $start.WorkingDirectory = $package
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.EnvironmentVariables['PATH'] = $package + ';' + [Environment]::SystemDirectory + ';' + $env:SystemRoot
    foreach ($key in @($start.EnvironmentVariables.Keys)) { if ($key -match '^GST_') { $start.EnvironmentVariables.Remove($key) } }
    foreach ($suffix in @('', '_1_0')) {
        $start.EnvironmentVariables["GST_PLUGIN_PATH$suffix"] = if ($Discover) { $pluginDirectory } else { '' }
        $start.EnvironmentVariables["GST_PLUGIN_SYSTEM_PATH$suffix"] = ''
        $start.EnvironmentVariables["GST_REGISTRY$suffix"] = $Registry
        $start.EnvironmentVariables["GST_PLUGIN_SCANNER$suffix"] = $scanner
    }
    $start.EnvironmentVariables['GST_REGISTRY_UPDATE'] = if ($Update) { 'yes' } else { 'no' }
    $start.EnvironmentVariables['GST_REGISTRY_FORK'] = if ($Discover) { 'yes' } else { 'no' }
    if ($RequireScanner) {
        # GStreamer otherwise hides this warning at the default debug level.
        $start.EnvironmentVariables['GST_DEBUG'] = 'GST_PLUGIN_LOADING:2'
        $start.EnvironmentVariables['GST_DEBUG_NO_COLOR'] = '1'
    }
    $job = [AirPlayRegistryProcess]::Create(); $child = $null; $timedOut = $false
    try {
        $child = [AirPlayRegistryProcess]::StartSuspended($start, $stdoutPath, $stderrPath)
        $child.EnrollAndResume($job)
        $timedOut = !$child.WaitForExit(30000)
        if (!$timedOut) { $exit = $child.ExitCode }
    } finally {
        [void][AirPlayRegistryProcess]::CloseHandle($job)
        if ($child) { $child.Dispose() }
    }
    $captured = [AirPlayRegistryProcess]::ReadFinalOutput($stdoutPath, $stderrPath, 5000)
    # Native warnings remain visible even on a successful inspector exit.
    if ($captured.Stderr) { Write-Host $captured.Stderr }
    if ($timedOut) { throw 'GStreamer registry inspector timed out; no publication' }
    if ($RequireScanner -and $captured.Stderr -match '(?m)(?:WARN|ERROR)\s+GST_PLUGIN_LOADING\s+gstpluginloader(?:-win32)?\.c') {
        $script:scannerFailure = $captured.Stderr
    }
    if ($exit -ne 0) { throw "GStreamer registry inspector failed: exit=$exit; $($captured.Stdout) $($captured.Stderr)" }
    return $captured.Stdout
}
$baselineExists = Test-Path -LiteralPath $destination -PathType Leaf
$baselineHash = if ($baselineExists) { Get-RegistryHash $destination } else { '' }
try {
    # Candidate never reuses the destination or an inherited/default user cache.
    $scan = Invoke-Inspector '' $candidate $true $true $true
    if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) { throw 'GStreamer registry scan did not produce a cache' }
    $blacklist = Invoke-Inspector '-b' $candidate $true $false
    if ($blacklist -notmatch '(?m)^Total count: 0 blacklisted files\s*$') {
        throw "GStreamer registry validation failed: blacklist must be empty. $blacklist"
    }
    if ($scannerFailure) { throw "GStreamer registry scanner failed; fallback is not accepted. $scannerFailure" }
    $scannedHash = Get-RegistryHash $candidate
    foreach ($plugin in $plugins) {
        Assert-NoReparse $plugin.FullName
        # Metadata loading is separate: it cannot insert entries into the scan cache.
        $metadata = Invoke-Inspector ('"' + $plugin.FullName + '"') $metadataRegistry $false $true
        $name = [regex]::Match($metadata, '(?m)^\s*Name\s+([a-zA-Z0-9_-]+)\s*$')
        if (!$name.Success) { throw "GStreamer plugin metadata name missing: $($plugin.FullName)" }
        # Name lookup with registry updates disabled proves normal scanner discovery.
        $discovered = Invoke-Inspector ('--plugin "' + $name.Groups[1].Value + '"') $candidate $true $false
        $filename = [regex]::Match($discovered, '(?m)^\s*Filename\s+([^\r\n]+)\s*$')
        if (!$filename.Success -or [IO.Path]::GetFullPath($filename.Groups[1].Value.Trim()) -ine $plugin.FullName) {
            throw "GStreamer registry returned wrong plugin origin: $($plugin.FullName); $discovered"
        }
        Write-Verbose "Registry discovered $($name.Groups[1].Value): $($plugin.FullName)"
    }
    if ($scannedHash -cne (Get-RegistryHash $candidate)) { throw 'GStreamer registry changed during read-only discovery checks' }
    # Finish all fallible temporary-directory cleanup while the old cache is intact.
    Assert-NoReparse $pending
    [IO.File]::Move($candidate, $pending)
    $pendingCreated = $true
    $cleanupAttempted = $true
    Remove-OwnedWork
    Assert-NoReparse $destination
    $currentExists = Test-Path -LiteralPath $destination -PathType Leaf
    if ($currentExists -ne $baselineExists -or ($currentExists -and (Get-RegistryHash $destination) -cne $baselineHash)) { throw 'Registry shared baseline changed; publication refused' }
    try {
        if (Test-Path -LiteralPath $destination) { [IO.File]::Replace($pending, $destination, [System.Management.Automation.Language.NullString]::Value) }
        else { [IO.File]::Move($pending, $destination) }
    } catch { throw "Could not publish GStreamer registry; old cache retained. $($_.Exception.Message)" }
    $pendingCreated = $false
    Write-Output "GStreamer deployment registry published: $($plugins.Count) local plugins, zero blacklist."
} finally {
    try {
        if (!$cleanupAttempted) { $cleanupAttempted = $true; Remove-OwnedWork }
    } finally {
        # A failed pre-publication cleanup or replacement removes only our pending file.
        if ($pendingCreated) {
            Assert-NoReparse $pending
            [IO.File]::Delete($pending)
        }
    }
}

} finally {
    try { Exit-AirPlayRegistryWriteLock $writeLease }
    catch { throw "Registry write lease release failed; published cache remains published if replacement completed. $($_.Exception.Message)" }
}
