param(
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [Parameter(Mandatory=$true)][string]$Inspector,
    [Parameter(Mandatory=$true)][string]$NonPluginDll,
    [Parameter(Mandatory=$true)][string]$Objdump,
    [Parameter(Mandatory=$true)][string]$DependencyBinDirectory,
    [Parameter(Mandatory=$true)][string]$GStreamerPluginDirectory,
    [Parameter(Mandatory=$true)][string]$ReportDirectory,
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [string]$Verifier = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PeDependencyIsolation.ps1')
. (Join-Path $PSScriptRoot 'StartupProcessContainment.ps1')
if (!$Verifier) { $Verifier = Join-Path $ProjectRoot 'scripts/verify-gstreamer-plugins.ps1' }
if (!(Test-Path -LiteralPath $Verifier -PathType Leaf)) {
    throw 'ASSERT: the packaged GStreamer plugin loading verifier must exist.'
}
$runId = [guid]::NewGuid().ToString('N')
$report = Join-Path ([IO.Path]::GetFullPath($ReportDirectory)) ('run-' + $runId)
New-Item -ItemType Directory -Path $report -Force | Out-Null
$parent = [IO.Path]::GetFullPath($RuntimeDirectory).TrimEnd('\')
$root = Join-Path $parent ('plugins-' + $runId)
if ($parent.Length -le 3 -or $root -match '[^\x00-\x7f]' -or !$root.StartsWith($parent + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe runtime root: $root"
}
$cursor = $root
while($cursor) {
    if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse runtime ancestor: $cursor" }
    $cursor = [IO.Path]::GetDirectoryName($cursor)
}
$created = $false
$audit = [Collections.Generic.List[string]]::new()
function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
function Assert-Path([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (!$full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) -or $full.Length -gt 240 -or $full -match '[^\x00-\x7f]') { throw "Unsafe fixture path: $full" }
    if ([IO.Path]::GetExtension($full) -ieq '.dll' -and $full.Length -gt 202) { throw "RTSS known DLL-path risk: $full" }
}
function New-Fixture([string]$Name) {
    $fixture = Join-Path $root $Name
    foreach($file in Get-ChildItem -LiteralPath $template -Recurse -File) {
        Assert-Path (Join-Path $fixture $file.FullName.Substring($template.Length + 1))
    }
    Copy-Item -LiteralPath $template -Destination $fixture -Recurse
    return $fixture
}
function Invoke-Verifier([string]$Name, [string]$Fixture, [bool]$Healthy, [string]$ExpectedError = '') {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = (Get-Command powershell -ErrorAction Stop).Source
    $start.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $Verifier + '" -PackageDir "' + $Fixture + '"'
    $start.WorkingDirectory = $Fixture
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.CreateNoWindow = $true
    # Intentionally retain a usable host dependency in PATH: the verifier must isolate it.
    $start.EnvironmentVariables['PATH'] = $DependencyBinDirectory + ';' + $env:PATH
    $job = [AirPlayStartupTestJob]::Create()
    $child = $null
    $timedOut = $false
    $stdoutPath = Join-Path $report "$Name-stdout.txt"
    $stderrPath = Join-Path $report "$Name-stderr.txt"
    try {
        $child = [AirPlayStartupTestJob]::StartSuspended($start, $stdoutPath, $stderrPath)
        $child.EnrollAndResume($job)
        $timedOut = !$child.WaitForExit(80000)
        if (!$timedOut) { $exit = $child.ExitCode }
    } finally {
        [void][AirPlayStartupTestJob]::CloseHandle($job)
        if($child) { $child.Dispose() }
    }
    $captured = [AirPlayStartupTestJob]::ReadFinalOutput($stdoutPath, $stderrPath, 5000)
    if ($timedOut) { throw "Verifier timed out: $Name; invalid rejection" }
    $stdout=$captured.Stdout; $stderr=$captured.Stderr
        [pscustomobject]@{name=$Name; package=$Fixture; exitCode=$exit; hostJsonExists=(Test-Path -LiteralPath (Join-Path $DependencyBinDirectory 'libjson-glib-1.0-0.dll'))} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $report "$Name.json") -Encoding utf8
        if ($Healthy) {
            if ($exit -ne 0 -or $stdout -notmatch 'GStreamer plugins verified: 2' -or
                $stdout -notmatch [regex]::Escape((Join-Path $Fixture 'gstreamer-plugins/libgstcodec2json.dll'))) {
                throw "ASSERT: healthy package must load both actual local plugins. exit=$exit stdout=$stdout stderr=$stderr"
            }
        } else {
            if ($exit -ne 1 -or $stderr -notmatch 'GStreamer plugin runtime probe failed' -or $stderr -notmatch 'exit=-1' -or
                $stderr -notmatch $ExpectedError -or $stderr -notmatch 'libgstcodec2json.dll') {
                throw "ASSERT: $Name must normally report the genuine plugin load failure; crash/setup failure is invalid. exit=$exit stderr=$stderr"
            }
        }
        $audit.Add("PASS: $Name exit=$exit")
}
try {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    New-Item -ItemType Directory -Path $root | Out-Null
    $created = $true
    [IO.File]::WriteAllText((Join-Path $root '.owner'), $runId)
    $plugins = @('coreelements','codec2json') | ForEach-Object { Require-File (Join-Path $GStreamerPluginDirectory "libgst$_.dll") }
    $Inspector = Require-File $Inspector
    $NonPluginDll = Require-File $NonPluginDll
    $deps = Get-PeDependencies -Roots (@($Inspector,$NonPluginDll) + $plugins) -SearchDirectories @($DependencyBinDirectory) -Objdump $Objdump -RejectImport { param($name,$file) } -Audit $audit
    $template = Join-Path $root 'template'
    foreach($source in $deps.Values) { Assert-Path (Join-Path $template (Split-Path -Leaf $source)) }
    foreach($plugin in $plugins) { Assert-Path (Join-Path $template ('gstreamer-plugins/' + (Split-Path -Leaf $plugin))) }
    New-Item -ItemType Directory -Path (Join-Path $template 'gstreamer-plugins') -Force | Out-Null
    Copy-PeDependencies -Resolved $deps -Directory $template -ExcludedFiles ($plugins + @($NonPluginDll))
    Copy-Item -LiteralPath $Inspector -Destination (Join-Path $template 'gst-inspect-1.0.exe')
    foreach($plugin in $plugins) { Copy-Item -LiteralPath $plugin -Destination (Join-Path $template 'gstreamer-plugins') }
    $sourceHashes = @($deps.Values | ForEach-Object { [pscustomobject]@{path=$_; hash=(Get-Sha256 $_)} })
    $sourceHashes | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $report 'source-hashes.json') -Encoding utf8

    $healthy = New-Fixture 'healthy space & ''literal'''
    Invoke-Verifier 'healthy' $healthy $true

    $missing = New-Fixture 'missing-json'
    $json = Join-Path $missing 'libjson-glib-1.0-0.dll'
    if (!(Test-Path -LiteralPath $json)) { throw 'Fixture must originally contain the actual json-glib dependency' }
    Remove-Item -LiteralPath $json -Force
    Invoke-Verifier 'missing-json' $missing $false 'Opening module failed'

    Copy-Item -LiteralPath (Join-Path $template 'libjson-glib-1.0-0.dll') -Destination $json
    Invoke-Verifier 'repaired-json' $missing $true

    $nonPlugin = New-Fixture 'non-plugin'
    Copy-Item -LiteralPath $NonPluginDll -Destination (Join-Path $nonPlugin 'gstreamer-plugins/libgstcodec2json.dll') -Force
    Invoke-Verifier 'non-plugin' $nonPlugin $false 'not a GStreamer plugin'

    foreach($row in $sourceHashes) { if($row.hash -ne (Get-Sha256 $row.path)) { throw "Source mutated: $($row.path)" } }
    Write-Output 'GStreamer package plugin acceptance passed: healthy, missing-json, repaired-json, non-plugin.'
} finally {
    $audit | Set-Content -LiteralPath (Join-Path $report 'audit.txt') -Encoding utf8
    if($created) {
        $resolvedRoot = [IO.Path]::GetFullPath($root)
        if (!$resolvedRoot.StartsWith($parent + '\', [StringComparison]::OrdinalIgnoreCase) -or
            [IO.File]::ReadAllText((Join-Path $resolvedRoot '.owner')) -cne $runId -or
            ((Get-Item -LiteralPath $resolvedRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Owned fixture cleanup boundary failed' }
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
    Write-Output "Report: $report"
}
