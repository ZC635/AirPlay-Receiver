param(
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [Parameter(Mandatory=$true)][string]$Inspector,
    [Parameter(Mandatory=$true)][string]$ScannerExecutable,
    [Parameter(Mandatory=$true)][string]$Objdump,
    [Parameter(Mandatory=$true)][string]$DependencyBinDirectory,
    [Parameter(Mandatory=$true)][string]$GStreamerPluginDirectory,
    [Parameter(Mandatory=$true)][string]$ReportDirectory,
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [Parameter(Mandatory=$true)][string]$StorageExecutable,
    [string]$Updater = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PeDependencyIsolation.ps1')
. (Join-Path $PSScriptRoot 'StartupProcessContainment.ps1')
if (!$Updater) { $Updater = Join-Path $ProjectRoot 'scripts/update-gstreamer-registry.ps1' }
if (!(Test-Path -LiteralPath $Updater -PathType Leaf)) { throw 'ASSERT: isolated deployment registry update must exist.' }
function Assert-True([bool]$Condition, [string]$Message) { if (!$Condition) { throw "ASSERT: $Message" } }
$runId = [guid]::NewGuid().ToString('N')
$report = Join-Path ([IO.Path]::GetFullPath($ReportDirectory)) ('run-' + $runId)
New-Item -ItemType Directory -Path $report -Force | Out-Null
$parent = [IO.Path]::GetFullPath($RuntimeDirectory).TrimEnd('\')
$root = Join-Path $parent ('cache-' + $runId)
if ($parent.Length -le 3 -or $root -match '[^\x00-\x7f]' -or !$root.StartsWith($parent + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe runtime root: $root" }
$cursor = $root
while ($cursor) {
    if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse ancestor: $cursor" }
    $cursor = [IO.Path]::GetDirectoryName($cursor)
}
# External-reader fault injection: hold only this run's ownership marker.
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
public sealed class RegistryMarkerReader : IDisposable {
    readonly ManualResetEventSlim held = new ManualResetEventSlim(false);
    readonly ManualResetEventSlim release = new ManualResetEventSlim(false);
    readonly Task worker;
    public string MarkerPath { get; private set; }
    public RegistryMarkerReader(string directory) {
        worker = Task.Factory.StartNew(delegate {
            var deadline = System.Diagnostics.Stopwatch.StartNew();
            while (deadline.ElapsedMilliseconds < 5000 && !release.IsSet) {
                foreach (var owned in Directory.GetDirectories(directory, ".reg-*")) {
                    var marker = Path.Combine(owned, ".owner");
                    if (!File.Exists(marker)) continue;
                    try {
                        using (var stream = new FileStream(marker, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
                            MarkerPath = marker;
                            held.Set();
                            release.Wait(80000);
                        }
                        return;
                    } catch (IOException) {}
                }
                Thread.Sleep(10);
            }
        });
    }
    public bool HasMarker { get { return held.IsSet; } }
    public void Dispose() {
        release.Set();
        if (!worker.Wait(5000)) throw new TimeoutException("Owned marker reader did not finish");
        held.Dispose(); release.Dispose();
    }
}
"@

$created = $false
$audit = [Collections.Generic.List[string]]::new()
function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path); $algorithm = [Security.Cryptography.SHA256]::Create()
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
    foreach ($file in Get-ChildItem -LiteralPath $template -Recurse -File) { Assert-Path (Join-Path $fixture $file.FullName.Substring($template.Length + 1)) }
    Assert-Path (Join-Path $fixture 'gstreamer-1.0/.reg-00000000000000000000000000000000/registry.bin')
    Copy-Item -LiteralPath $template -Destination $fixture -Recurse
    $cacheDir = Join-Path $fixture 'gstreamer-1.0'
    New-Item -ItemType Directory -Path (Join-Path $cacheDir '.reg-other') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $cacheDir 'user.txt'), 'USER-CACHE-NEIGHBOUR')
    [IO.File]::WriteAllText((Join-Path $cacheDir '.registry-other.bin'), 'OTHER-PENDING-CACHE')
    [IO.File]::WriteAllText((Join-Path $cacheDir '.reg-other/registry.bin'), 'OTHER-OWNERS-CACHE')
    return $fixture
}
function Invoke-Child([string]$Name, [string]$Fixture, [bool]$IsUpdater, [string]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = if ($IsUpdater) { (Get-Command powershell).Source } else { Join-Path $Fixture 'gst-inspect-1.0.exe' }
    $start.Arguments = if ($IsUpdater) { '-NoProfile -ExecutionPolicy Bypass -File "' + $Updater + '" -PackageDir "' + $Fixture + '"' } else { $Arguments }
    $start.WorkingDirectory = $Fixture
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    if ($IsUpdater) {
        # Host JSON is deliberately present; updater must not borrow it.
        $start.EnvironmentVariables['PATH'] = $DependencyBinDirectory + ';' + $env:PATH
        $start.EnvironmentVariables['GST_REGISTRY_1_0'] = Join-Path $root 'external-registry.bin'
        $start.EnvironmentVariables['GST_PLUGIN_SYSTEM_PATH_1_0'] = $GStreamerPluginDirectory
    } else {
        $start.EnvironmentVariables['PATH'] = $Fixture + ';' + [Environment]::SystemDirectory + ';' + $env:SystemRoot
        foreach ($key in @($start.EnvironmentVariables.Keys)) { if ($key -match '^GST_') { $start.EnvironmentVariables.Remove($key) } }
        foreach ($suffix in @('', '_1_0')) {
            $start.EnvironmentVariables["GST_PLUGIN_PATH$suffix"] = Join-Path $Fixture 'gstreamer-plugins'
            $start.EnvironmentVariables["GST_PLUGIN_SYSTEM_PATH$suffix"] = ''
            $start.EnvironmentVariables["GST_REGISTRY$suffix"] = Join-Path $Fixture 'gstreamer-1.0/registry.x86_64.bin'
            $start.EnvironmentVariables["GST_PLUGIN_SCANNER$suffix"] = Join-Path $Fixture 'libexec/gstreamer-1.0/gst-plugin-scanner.exe'
        }
        if ($Arguments) { $start.EnvironmentVariables['GST_REGISTRY_UPDATE'] = 'no' }
    }
    $stdoutPath = Join-Path $report "$Name-stdout.txt"; $stderrPath = Join-Path $report "$Name-stderr.txt"
    $job = [AirPlayStartupTestJob]::Create(); $child = $null; $timedOut = $false
    try {
        $child = [AirPlayStartupTestJob]::StartSuspended($start, $stdoutPath, $stderrPath)
        $child.EnrollAndResume($job)
        $timedOut = !$child.WaitForExit(80000)
        if (!$timedOut) { $exit = $child.ExitCode }
    } finally {
        [void][AirPlayStartupTestJob]::CloseHandle($job)
        if ($child) { $child.Dispose() }
    }
    $captured = [AirPlayStartupTestJob]::ReadFinalOutput($stdoutPath, $stderrPath, 5000)
    if ($timedOut) { throw "ASSERT: timed-out $Name is not valid rejection" }
    [pscustomobject]@{ name=$Name; exitCode=$exit; package=$Fixture; hostJsonExists=(Test-Path -LiteralPath (Join-Path $DependencyBinDirectory 'libjson-glib-1.0-0.dll')) } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $report "$Name.json") -Encoding utf8
    return [pscustomobject]@{ Exit=$exit; Out=$captured.Stdout; Error=$captured.Stderr }
}
function Assert-Protected([string]$Fixture, [bool]$ForeignProtocol = $false) {
    if ([IO.File]::ReadAllText((Join-Path $Fixture 'gstreamer-1.0/user.txt')) -cne 'USER-CACHE-NEIGHBOUR' -or
        [IO.File]::ReadAllText((Join-Path $Fixture 'gstreamer-1.0/.reg-other/registry.bin')) -cne 'OTHER-OWNERS-CACHE' -or
        [IO.File]::ReadAllText((Join-Path $Fixture 'gstreamer-1.0/.registry-other.bin')) -cne 'OTHER-PENDING-CACHE' -or
        [IO.File]::ReadAllText((Join-Path $root 'external-registry.bin')) -cne 'EXTERNAL-CACHE' -or
        [IO.File]::ReadAllText((Join-Path $root 'neighbour.txt')) -cne 'NEIGHBOUR') { throw 'ASSERT: cache ownership/adjacent file protection failed' }
    $remaining = @(Get-ChildItem -LiteralPath (Join-Path $Fixture 'gstreamer-1.0') -Directory | Where-Object { $_.Name -like '.reg-*' -and $_.Name -ne '.reg-other' })
    if ($remaining.Count) { throw 'ASSERT: updater owned temporary directories leaked' }
    $pendingFiles = @(Get-ChildItem -LiteralPath (Join-Path $Fixture 'gstreamer-1.0') -File -Force | Where-Object { $_.Name -like '.registry-*' -and $_.Name -cnotin @('.registry-other.bin','.registry-startup.lock') })
    if ($pendingFiles.Count) { throw 'ASSERT: updater owned pending registry files leaked' }
    $protocol = Join-Path $Fixture 'gstreamer-1.0/.registry-startup.lock'
    if ((Test-Path -LiteralPath $protocol) -and [IO.File]::ReadAllText($protocol) -cne $(if($ForeignProtocol){'UNKNOWN-OWNER'}else{"AIRPLAY-GSTREAMER-REGISTRY-LOCK/1`n"})) { throw 'ASSERT: persistent deployment lock protocol changed' }
}
function Assert-Healthy([string]$Name, [string]$Fixture) {
    $result = Invoke-Child $Name $Fixture $true ''
    if ($result.Exit -ne 0) { throw "ASSERT: $Name deployment update failed: $($result.Error)" }
    $blacklist = Invoke-Child "$Name-blacklist" $Fixture $false '-b'
    $plugin = Invoke-Child "$Name-plugin" $Fixture $false '--plugin codec2json'
    if ($blacklist.Exit -ne 0 -or $blacklist.Out -notmatch 'Total count: 0 blacklisted files' -or
        $plugin.Exit -ne 0 -or $plugin.Out -notmatch [regex]::Escape((Join-Path $Fixture 'gstreamer-plugins/libgstcodec2json.dll')) -or
        $plugin.Out -notmatch 'h2642json') { throw 'ASSERT: freshly deployed registry must expose actual local codec2json with zero blacklist, without rescanning' }
    Assert-Protected $Fixture
    $audit.Add("PASS: $Name actual cached plugin discovery, zero blacklist")
}
function Assert-Rejected([string]$Name, [string]$Fixture, [string]$ExpectedError) {
    $registry = Join-Path $Fixture 'gstreamer-1.0/registry.x86_64.bin'
    $before = Get-Sha256 $registry
    $result = Invoke-Child $Name $Fixture $true ''
    if ($result.Exit -ne 1 -or $result.Error -notmatch $ExpectedError) { throw "ASSERT: $Name must normally reject before publishing, not crash or falsely pass: exit=$($result.Exit); $($result.Error)" }
    if ($before -cne (Get-Sha256 $registry)) { throw "ASSERT: $Name replaced old cache on failure" }
    Assert-Protected $Fixture ($Name -ceq 'foreign-protocol')
    $audit.Add("PASS: $Name rejected; original registry intact")
}
try {
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    New-Item -ItemType Directory -Path $root | Out-Null; $created = $true
    [IO.File]::WriteAllText((Join-Path $root '.owner'), $runId)
    [IO.File]::WriteAllText((Join-Path $root 'external-registry.bin'), 'EXTERNAL-CACHE')
    [IO.File]::WriteAllText((Join-Path $root 'neighbour.txt'), 'NEIGHBOUR')
    $plugins = @('coreelements','codec2json') | ForEach-Object { Require-File (Join-Path $GStreamerPluginDirectory "libgst$_.dll") }
    $Inspector = Require-File $Inspector; $ScannerExecutable = Require-File $ScannerExecutable
    $deps = Get-PeDependencies -Roots (@($Inspector,$ScannerExecutable) + $plugins) -SearchDirectories @($DependencyBinDirectory) -Objdump $Objdump -RejectImport { param($name,$file) } -Audit $audit
    $template = Join-Path $root 'template'
    foreach ($source in $deps.Values) { Assert-Path (Join-Path $template (Split-Path -Leaf $source)) }
    foreach ($plugin in $plugins) { Assert-Path (Join-Path $template ('gstreamer-plugins/' + (Split-Path -Leaf $plugin))) }
    New-Item -ItemType Directory -Path (Join-Path $template 'gstreamer-plugins') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $template 'libexec/gstreamer-1.0') -Force | Out-Null
    Copy-PeDependencies -Resolved $deps -Directory $template -ExcludedFiles $plugins
    Copy-Item -LiteralPath $Inspector -Destination (Join-Path $template 'gst-inspect-1.0.exe')
    Copy-Item -LiteralPath $ScannerExecutable -Destination (Join-Path $template 'libexec/gstreamer-1.0/gst-plugin-scanner.exe')
    foreach ($plugin in $plugins) { Copy-Item -LiteralPath $plugin -Destination (Join-Path $template 'gstreamer-plugins') }
    $sourceHashes = @(@($deps.Values) + @($StorageExecutable) | ForEach-Object { [pscustomobject]@{path=$_; hash=(Get-Sha256 $_)} })
    $sourceHashes | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $report 'source-hashes.json') -Encoding utf8

    $fixture = New-Fixture 'space & ''literal'''
    $json = Join-Path $fixture 'libjson-glib-1.0-0.dll'
    Remove-Item -LiteralPath $json -Force
    $seed = Invoke-Child 'seed-blacklist' $fixture $false ''
    $seedBlacklist = Invoke-Child 'seed-query' $fixture $false '-b'
    if ($seed.Exit -ne 0 -or $seedBlacklist.Exit -ne 0 -or $seedBlacklist.Out -notmatch 'libgstcodec2json.dll' -or $seedBlacklist.Out -notmatch 'Total count: 1 blacklisted file') { throw 'ASSERT: setup must create actual native scanner blacklist, not synthetic cache' }
    $registry = Join-Path $fixture 'gstreamer-1.0/registry.x86_64.bin'
    Copy-Item -LiteralPath $registry -Destination (Join-Path $report 'old-blacklisted-registry.bin')
    Copy-Item -LiteralPath (Join-Path $template 'libjson-glib-1.0-0.dll') -Destination $json
    Assert-Healthy 'repaired-old-blacklist' $fixture

    # Genuine cross-language contention: actual C++ Storage lease stays live in our Job.
    $native = Join-Path $fixture 'native-lock'
    New-Item -ItemType Directory -Path $native | Out-Null
    $nonce = [guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText((Join-Path $native '.owner'), $nonce)
    $heldPath = Join-Path $native ("held-$nonce.json")
    $releasedPath = Join-Path $native ("released-$nonce.json")
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $StorageExecutable
    $start.Arguments = '--hold-lock "' + $fixture + '" "' + $native + '" ' + $nonce
    $start.WorkingDirectory = $fixture; $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.EnvironmentVariables['PATH'] = (Split-Path -Parent $StorageExecutable) + ';' + $DependencyBinDirectory + ';' + [Environment]::SystemDirectory + ';' + $env:SystemRoot
    $job = [AirPlayStartupTestJob]::Create(); $holder = $null; $heldReceipt = $null
    $cacheBefore = Get-Sha256 $registry
    $recordPath = Join-Path $fixture 'gstreamer-1.0/registry.x86_64.validation.json'
    [IO.File]::WriteAllText($recordPath, 'owned-previous-validation-record')
    $recordBefore = Get-Sha256 $recordPath
    try {
        $holder = [AirPlayStartupTestJob]::StartSuspended($start, (Join-Path $report 'native-holder-stdout.txt'), (Join-Path $report 'native-holder-stderr.txt'))
        $holder.EnrollAndResume($job)
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while ($clock.ElapsedMilliseconds -lt 5000 -and !$holder.WaitForExit(0)) {
            if (Test-Path -LiteralPath $heldPath) { $heldReceipt = Get-Content -LiteralPath $heldPath -Raw | ConvertFrom-Json; break }
            Start-Sleep -Milliseconds 10
        }
        Assert-True ($null -ne $heldReceipt -and !$holder.WaitForExit(0) -and $heldReceipt.nonce -ceq $nonce -and $heldReceipt.phase -ceq 'held' -and $heldReceipt.complete -and
            $heldReceipt.producer -ceq 'CacheStorage::RegistryWriteLease' -and $heldReceipt.package -ieq $fixture.Replace('\','/')) 'Native holder must produce a fresh complete actual lease receipt while alive'
        Copy-Item -LiteralPath $heldPath -Destination (Join-Path $report 'native-held.json')
        $update = Invoke-Child 'native-busy' $fixture $true ''
        $updateExit = $update.Exit; $cacheAfter = Get-Sha256 $registry
        Assert-True ($updateExit -eq 1 -and $update.Error -match 'Registry write lock') 'Busy writer must reject deployment normally'
        Assert-True ($cacheAfter -eq $cacheBefore) 'Busy deployment must preserve shared cache'
        Assert-True ((Get-Sha256 $recordPath) -ceq $recordBefore) 'Busy deployment must preserve validation record'
    } finally {
      try {
        if ($holder -and !$holder.WaitForExit(0) -and $heldReceipt) {
            $releasePath = Join-Path $native ("release-$nonce.json")
            [IO.File]::WriteAllText($releasePath + '.pending', (@{nonce=$nonce;phase='release'}|ConvertTo-Json -Compress))
            [IO.File]::Move($releasePath + '.pending', $releasePath)
            Assert-True ($holder.WaitForExit(5000) -and $holder.ExitCode -eq 0 -and (Test-Path -LiteralPath $releasedPath)) 'Native holder must release normally; timeout/crash is not mutex evidence'
            $released = Get-Content -LiteralPath $releasedPath -Raw | ConvertFrom-Json
            Assert-True ($released.nonce -ceq $nonce -and $released.phase -ceq 'released' -and $released.complete -and $released.producer -ceq 'CacheStorage::RegistryWriteLease') 'Native release must have fresh complete provenance'
            Copy-Item -LiteralPath $releasedPath -Destination (Join-Path $report 'native-released.json')
        }
        if ($holder -and $heldReceipt -and !(Test-Path -LiteralPath $releasedPath)) { throw 'ASSERT: native holder ended without normal released receipt' }
        if ($holder -and $heldReceipt) {
            [pscustomobject]@{ nonce=$nonce; executable=$StorageExecutable; binarySha256=(Get-Sha256 $StorageExecutable); exitCode=$holder.ExitCode; jobContained=$true; heldReceipt='native-held.json'; releasedReceipt='native-released.json' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $report 'native-holder-process.json') -Encoding utf8
            $allowed=@('.owner',"held-$nonce.json","release-$nonce.json","released-$nonce.json")
            foreach ($entry in Get-ChildItem -LiteralPath $native -Force) { Assert-True (!$entry.PSIsContainer -and $entry.Name -cin $allowed -and !($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Native control cleanup must contain only exact run protocol objects' }
        }
      } finally {
        [void][AirPlayStartupTestJob]::CloseHandle($job)
        if ($holder) { $holder.Dispose() }
      }
    }
    Assert-Protected $fixture
    $audit.Add('PASS: genuine native lease excluded deployment; shared cache/record/protected files unchanged; native lease released normally')
    $lockPath = Join-Path $fixture 'gstreamer-1.0/.registry-startup.lock'
    [IO.File]::WriteAllText($lockPath, 'UNKNOWN-OWNER')
    Assert-Rejected 'foreign-protocol' $fixture 'Registry write lock'
    Assert-True ([IO.File]::ReadAllText($lockPath) -ceq 'UNKNOWN-OWNER') 'Foreign protocol bytes must remain unchanged'
    [IO.File]::WriteAllText($lockPath,"AIRPLAY-GSTREAMER-REGISTRY-LOCK/1`n",[Text.UTF8Encoding]::new($false))

    Remove-Item -LiteralPath $json -Force
    Assert-Rejected 'missing-json' $fixture 'GStreamer registry validation failed.*blacklist'
    Copy-Item -LiteralPath (Join-Path $template 'libjson-glib-1.0-0.dll') -Destination $json

    $scanner = Join-Path $fixture 'libexec/gstreamer-1.0/gst-plugin-scanner.exe'
    Copy-Item -LiteralPath (Join-Path ([Environment]::SystemDirectory) 'where.exe') -Destination $scanner -Force
    Assert-Rejected 'failed-scanner' $fixture 'GStreamer registry scanner failed'
    Copy-Item -LiteralPath $ScannerExecutable -Destination $scanner -Force

    # Distinct real stale cache makes replacement observable even for identical scans.
    Copy-Item -LiteralPath (Join-Path $report 'old-blacklisted-registry.bin') -Destination $registry -Force
    $cacheBeforeCleanupFault = Get-Sha256 $registry
    $reader = [RegistryMarkerReader]::new((Join-Path $fixture 'gstreamer-1.0'))
    try {
        $cleanupResult = Invoke-Child 'blocked-cleanup' $fixture $true ''
        if (!$reader.HasMarker) { throw 'ASSERT: cleanup fault must actually own a live marker reader' }
        $heldMarker = $reader.MarkerPath
        if ($cleanupResult.Exit -ne 1 -or $cleanupResult.Error -notmatch '(?i)remove-item|cleanup') {
            throw "ASSERT: blocked cleanup must report failure: exit=$($cleanupResult.Exit) $($cleanupResult.Error)"
        }
        if ($cacheBeforeCleanupFault -cne (Get-Sha256 $registry)) { throw 'ASSERT: blocked-cleanup replaced old cache on failure' }
    } finally { $reader.Dispose() }
    # The injected external reader made this one temporary directory undeletable.
    # After releasing it, this test owns the fixture and removes exactly that residue.
    $heldDirectory = [IO.Path]::GetDirectoryName($heldMarker)
    if (!$heldDirectory.StartsWith([IO.Path]::GetFullPath((Join-Path $fixture 'gstreamer-1.0')) + '\', [StringComparison]::OrdinalIgnoreCase) -or
        [IO.File]::ReadAllText($heldMarker) -cne ([IO.Path]::GetFileName($heldDirectory)).Substring(5) -or
        ((Get-Item -LiteralPath $heldDirectory).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'ASSERT: controlled cleanup residue boundary failed' }
    Remove-Item -LiteralPath $heldDirectory -Recurse -Force
    Assert-Protected $fixture
    $audit.Add('PASS: blocked-cleanup rejected before cache replacement')

    $locked = [IO.File]::Open($registry, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Assert-Rejected 'locked-destination' $fixture 'Could not publish GStreamer registry' }
    finally { $locked.Dispose() }
    $recordBeforeDeployment = Get-Sha256 $recordPath
    $oldRecordCache = Get-Sha256 $registry
    Assert-Healthy 'after-failure' $fixture
    Assert-True ((Get-Sha256 $recordPath) -ceq $recordBeforeDeployment -and (Get-Sha256 $registry) -cne $oldRecordCache) 'Deployment cache replacement must preserve the old record, leaving its cache tuple mismatched'
    Assert-Healthy 'first-deployment' (New-Fixture 'new-package')
    foreach ($row in $sourceHashes) { if ($row.hash -cne (Get-Sha256 $row.path)) { throw "Source mutated: $($row.path)" } }
    Write-Output 'Deployment registry acceptance passed: real blacklist repair, isolated dependency failure, failed scanner, locked publication, first deployment, protection.'
} finally {
    $audit | Set-Content -LiteralPath (Join-Path $report 'audit.txt') -Encoding utf8
    if ($created) {
        $resolvedRoot = [IO.Path]::GetFullPath($root)
        if (!$resolvedRoot.StartsWith($parent + '\', [StringComparison]::OrdinalIgnoreCase) -or
            [IO.File]::ReadAllText((Join-Path $resolvedRoot '.owner')) -cne $runId -or
            ((Get-Item -LiteralPath $resolvedRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Owned fixture cleanup boundary failed' }
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
    Write-Output "Report: $report"
}
