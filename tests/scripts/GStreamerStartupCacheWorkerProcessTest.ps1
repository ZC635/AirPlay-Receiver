param(
 [Parameter(Mandatory=$true)][string]$Executable,
 [string]$CoordinatorExecutable='',
 [Parameter(Mandatory=$true)][string]$ProbeExecutable,
 [Parameter(Mandatory=$true)][string]$ProcessFixtureExecutable,
 [Parameter(Mandatory=$true)][string]$NonPluginDll,
 [Parameter(Mandatory=$true)][string]$Objdump,
 [Parameter(Mandatory=$true)][string]$DependencyBinDirectory,
 [Parameter(Mandatory=$true)][string]$QtPluginDirectory,
 [Parameter(Mandatory=$true)][string]$GStreamerPluginDirectory,
 [Parameter(Mandatory=$true)][string]$ScannerExecutable,
 [Parameter(Mandatory=$true)][string]$ReportDirectory,
 [Parameter(Mandatory=$true)][string]$RuntimeDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PeDependencyIsolation.ps1')
. (Join-Path $PSScriptRoot 'StartupProcessContainment.ps1')
function Assert-True([bool]$Condition,[string]$Message) { if (!$Condition) { throw "ASSERT: $Message" } }
function Hash([string]$Path) { $s=[IO.File]::OpenRead($Path); $h=[Security.Cryptography.SHA256]::Create(); try {return [BitConverter]::ToString($h.ComputeHash($s)).Replace('-','').ToLowerInvariant()} finally {$s.Dispose(); $h.Dispose()} }
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class CacheWorkerFileIdentity {
 [StructLayout(LayoutKind.Sequential)] struct Info {public uint attributes; public System.Runtime.InteropServices.ComTypes.FILETIME creation,access,write; public uint volume,sizeHigh,sizeLow,links,indexHigh,indexLow;}
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetFileInformationByHandle(SafeFileHandle handle,out Info info);
 public static string Id(string path) {using (var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete)) {Info i; if (!GetFileInformationByHandle(file.SafeFileHandle,out i)) throw new System.ComponentModel.Win32Exception(); return i.volume+"/"+i.indexHigh+"/"+i.indexLow;}}
}
"@
$run=[guid]::NewGuid().ToString('N')
$report=Join-Path ([IO.Path]::GetFullPath($ReportDirectory)) ('run-'+$run)
$parent=[IO.Path]::GetFullPath($RuntimeDirectory).TrimEnd('\')
$root=Join-Path $parent ('w-'+$run.Substring(0,8))
Assert-True ($parent.Length -gt 3 -and $root.StartsWith($parent+'\',[StringComparison]::OrdinalIgnoreCase) -and $root -notmatch '[^\x00-\x7f]') 'Owned short ASCII fixture root'
for ($p=$parent; $p; $p=[IO.Path]::GetDirectoryName($p)) { if (Test-Path -LiteralPath $p) { Assert-True (-not ((Get-Item -LiteralPath $p).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'No reparse ancestors' } }
New-Item -ItemType Directory -Path $report,$root -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $root '.test-owner'),$run)
$audit=[Collections.Generic.List[string]]::new()
$package=Join-Path $root 'p'
$owned=Join-Path $root ('startup-'+[guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $package,$owned,(Join-Path $package 'gstreamer-plugins'),(Join-Path $package 'config'),(Join-Path $package 'gstreamer-1.0'),(Join-Path $package 'libexec/gstreamer-1.0') -Force | Out-Null
$marker=([guid]::NewGuid().ToString('N')+[guid]::NewGuid().ToString('N'))
[IO.File]::WriteAllText((Join-Path $owned '.owner'),$marker)
$plugins=@('coreelements','codec2json','app','libav','playback','autodetect','videoparsersbad') | ForEach-Object { Require-File (Join-Path $GStreamerPluginDirectory "libgst$_.dll") }
$inspector=Require-File (Join-Path $DependencyBinDirectory 'gst-inspect-1.0.exe')
$nativeRoots=@($Executable,$ProbeExecutable,$ProcessFixtureExecutable,$ScannerExecutable,$inspector); if ($CoordinatorExecutable) {$nativeRoots+=@($CoordinatorExecutable)}
$deps=Get-PeDependencies -Roots ($nativeRoots+$plugins) -SearchDirectories @($DependencyBinDirectory) -Objdump $Objdump -RejectImport {param($name,$file)} -Audit $audit
Copy-PeDependencies $deps $package $plugins
Copy-Item -LiteralPath $Executable -Destination (Join-Path $package 'airplay_receiver.exe')
if ($CoordinatorExecutable) {Copy-Item -LiteralPath $CoordinatorExecutable -Destination (Join-Path $package 'coordinator-test.exe')}
Copy-Item -LiteralPath $inspector -Destination (Join-Path $package 'gst-inspect-1.0.exe')
Copy-Item -LiteralPath $ProcessFixtureExecutable -Destination (Join-Path $package 'process-control.exe')
Copy-Item -LiteralPath $ScannerExecutable -Destination (Join-Path $package 'libexec/gstreamer-1.0/gst-plugin-scanner.exe')
foreach ($p in $plugins) { Copy-Item -LiteralPath $p -Destination (Join-Path $package 'gstreamer-plugins') }
foreach ($f in Get-ChildItem -LiteralPath $root -File -Recurse) { Assert-True ($f.FullName -notmatch '[^\x00-\x7f]' -and ($f.Extension -ine '.dll' -or $f.FullName.Length -le 202)) 'Actual DLL paths within 202 ASCII diagnostic bytes' }
$manifest=@(Get-ChildItem -LiteralPath $package -File -Recurse | Where-Object {$_.Extension -in @('.dll','.exe')} | ForEach-Object {$_.FullName.Substring($package.Length+1).Replace('\','/')})
[IO.File]::WriteAllLines((Join-Path $package 'config/portable-runtime-manifest.txt'),$manifest)
@($deps.Values | ForEach-Object {[pscustomobject]@{path=$_;sha256=(Hash $_)}}) | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $report 'input-hashes.json') -Encoding utf8
$shared=Join-Path $package 'gstreamer-1.0/registry.x86_64.bin'
function Child([string]$Name,[string]$Exe,[string]$Arguments,[string]$Registry,[bool]$ReadOnly=$false,[string]$PluginOverride='',[int]$WaitMs=10000) {
 $start=[Diagnostics.ProcessStartInfo]::new(); $start.FileName=$Exe; $start.Arguments=$Arguments
 $start.WorkingDirectory=$package; $start.UseShellExecute=$false; $start.CreateNoWindow=$true
 foreach ($k in @($start.EnvironmentVariables.Keys)) { if ($k -match '^GST_') {$start.EnvironmentVariables.Remove($k)} }
 $start.EnvironmentVariables['PATH']=$package+';'+[Environment]::SystemDirectory+';'+$env:SystemRoot
 foreach ($suffix in @('','_1_0')) {
  $start.EnvironmentVariables["GST_PLUGIN_PATH$suffix"]=if ($PluginOverride) {$PluginOverride} else {Join-Path $package 'gstreamer-plugins'}
  $start.EnvironmentVariables["GST_PLUGIN_SYSTEM_PATH$suffix"]=''
  $start.EnvironmentVariables["GST_PLUGIN_SCANNER$suffix"]=Join-Path $package 'libexec/gstreamer-1.0/gst-plugin-scanner.exe'
  $start.EnvironmentVariables["GST_REGISTRY$suffix"]=$Registry
 }
 if ($ReadOnly) {$start.EnvironmentVariables['GST_REGISTRY_UPDATE']='no'}
 $stdout=Join-Path $report "$Name-stdout.txt"; $stderr=Join-Path $report "$Name-stderr.txt"
 $job=[AirPlayStartupTestJob]::Create(); $child=$null; $exit=$null; $finished=$false
 try { $child=[AirPlayStartupTestJob]::StartSuspended($start,$stdout,$stderr); $child.EnrollAndResume($job); $finished=$child.WaitForExit($WaitMs); if ($finished) {$exit=$child.ExitCode} }
 finally { $cleanupClock=[Diagnostics.Stopwatch]::StartNew(); [void][AirPlayStartupTestJob]::CloseHandle($job); if ($child) {if (!$child.WaitForExit([Math]::Max(0,5000-[int]$cleanupClock.ElapsedMilliseconds))) {throw 'Owned child cleanup deadline exhausted'}; $child.Dispose()} }
 $captured=[AirPlayStartupTestJob]::ReadFinalOutput($stdout,$stderr,[Math]::Max(0,5000-[int]$cleanupClock.ElapsedMilliseconds))
 [pscustomobject]@{name=$Name;exitCode=$exit;finished=$finished;executable=$Exe;binaryHash=(Hash $Exe);arguments=$Arguments} | ConvertTo-Json | Set-Content (Join-Path $report "$Name-process.json") -Encoding utf8
 return [pscustomobject]@{Exit=$exit;Finished=$finished;Out=$captured.Stdout;Error=$captured.Stderr}
}
function Frame-Rejection($Child,$Result,[string]$Nonce,[string]$Stage) {
 if (!$Child.Finished -or $Child.Exit -ne 0) {return 'Worker did not exit normally with exit zero'}
 if ($null -eq $Result) {return 'Empty or missing worker result'}
 if (!$Result.PSObject.Properties['nonce'] -or $Result.nonce -cne $Nonce) {return 'Worker nonce differs from this invocation'}
 if (!$Result.PSObject.Properties['complete'] -or !$Result.complete) {return 'Worker result incomplete'}
 if (!$Result.PSObject.Properties['stage'] -or $Result.stage -cne $Stage) {return 'Worker stage differs from this invocation'}
 return ''
}
function Worker([string]$Name,[string]$Stage,[string]$InputRegistry,[string]$Output,[string]$Expected='',[string]$Preparation='',$Validation=@{}) {
 $nonce=[guid]::NewGuid().ToString('N'); $resultPath=Join-Path $root ("worker-"+$nonce+".json"); $requestPath=Join-Path $root ("worker-"+$nonce+".request.json")
 $authority=@{schemaVersion=1;phase='runtime-authority';nonce=[guid]::NewGuid().ToString('N');package=$package;runtimeRegistry=(Join-Path $owned 'runtime.bin');runtimeMarker=$marker;markerId=[CacheWorkerFileIdentity]::Id((Join-Path $owned '.owner'));runtimeId=$(if (Test-Path -LiteralPath (Join-Path $owned 'runtime.bin')) {[CacheWorkerFileIdentity]::Id((Join-Path $owned 'runtime.bin'))} else {''})}
 $request=@{schemaVersion=1;stage=$Stage;nonce=$nonce;packageDirectory=$package;ownedRoot=$owned;ownershipRequest=($authority|ConvertTo-Json -Compress);preparationRequest=$Preparation;validationRecord=$Validation;inputRegistry=$InputRegistry;outputRegistry=$Output;resultPath=$resultPath;baseline=@{exists=$true;sha256=(Hash $shared)};expectedInput=@{valid=($Expected -ne '');sha256=$Expected}}
 [IO.File]::WriteAllText($requestPath,($request|ConvertTo-Json -Depth 6))
 Copy-Item -LiteralPath $requestPath -Destination (Join-Path $report "$Name-request.json")
 $child=Child $Name (Join-Path $package 'airplay_receiver.exe') ('--gstreamer-cache-worker "'+$requestPath+'"') $Output
 # The frame gate below requires this owned process to finish and exit zero.
 $result=$null; if (Test-Path -LiteralPath $resultPath) { Copy-Item -LiteralPath $resultPath -Destination (Join-Path $report "$Name-result.json"); $result=Get-Content -Raw -LiteralPath $resultPath | ConvertFrom-Json }
 Assert-True ((Frame-Rejection $child $result $nonce $Stage) -eq '') 'Worker must finish normally with this-run result'
 Assert-True ($result.nonce -ceq $nonce -and $result.stage -ceq $Stage) 'Worker binds result nonce and stage'
 Assert-True ((Hash $shared) -ceq $sharedHashBefore) 'Worker cannot publish or implicitly update shared registry'
 return $result
}
# Real blacklist from a missing JSON dependency, then restore that dependency.
$json=Join-Path $package 'libjson-glib-1.0-0.dll'; Remove-Item -LiteralPath $json
$seed=Child 'seed' (Join-Path $package 'gst-inspect-1.0.exe') '' $shared
$query=Child 'seed-blacklist' (Join-Path $package 'gst-inspect-1.0.exe') '-b' $shared $true
Assert-True ($seed.Exit -eq 0 -and $query.Exit -eq 0 -and $query.Out -match 'libgstcodec2json.dll' -and $query.Out -match '1 blacklisted file') 'Real scanner creates old blacklist'
Copy-Item -LiteralPath (Join-Path $DependencyBinDirectory 'libjson-glib-1.0-0.dll') -Destination $json
$sharedHashBefore=Hash $shared
# Transport controls use Task2's real child fixture. They are not plugin-oracle results.
foreach ($mode in @('tree','normal','crash')) {
 $controlNonce=[guid]::NewGuid().ToString('N'); $controlResult=Join-Path $root ('control-'+$mode+'.json'); $controlMarker=Join-Path $owned ('control-'+$mode+'.marker')
 $sentNonce=if ($mode -eq 'normal') {'00000000000000000000000000000000'} else {$controlNonce}
 $control=Child ('transport-'+$mode) (Join-Path $package 'process-control.exe') ($mode+' "'+$controlResult+'" "'+$sentNonce+'" "'+$controlMarker+'"') ''
 $frame=$null; if (Test-Path -LiteralPath $controlResult) {Copy-Item -LiteralPath $controlResult -Destination (Join-Path $report ('transport-'+$mode+'-result.json')); $frame=Get-Content -Raw -LiteralPath $controlResult|ConvertFrom-Json}
 $rejection=Frame-Rejection $control $frame $controlNonce 'Scan'
 $expected=if ($mode -eq 'tree') {'Empty or missing worker result'} elseif ($mode -eq 'normal') {'Worker nonce differs from this invocation'} else {'Worker did not exit normally with exit zero'}
 Assert-True ($rejection -ceq $expected) 'Same actual receiver gate rejects empty result, wrong nonce and crash before any plugin-oracle claim'
 [pscustomobject]@{mode=$mode;expectedNonce=$controlNonce;sentNonce=$sentNonce;rejection=$rejection;pluginOracleRan=$false;completeGstFrame=$false} | ConvertTo-Json | Set-Content (Join-Path $report ('transport-'+$mode+'-rejection.json')) -Encoding utf8
}
$candidate=Join-Path $owned 'candidate.bin'
$scan=Worker 'scan' 'Scan' '' $candidate
Assert-True ($scan.blacklistFree -and -not $scan.scannerFallback -and $scan.readiness.ready -and $scan.reason -eq '') 'Candidate needs normal scanner'
Assert-True (@($scan.plugins | Where-Object {$_.name -eq 'codec2json'}).Count -eq 1) 'Scan discovers actual bundled codec2json'
$verify=Worker 'verify' 'Verify' $candidate (Join-Path $owned 'verify.bin') $scan.fingerprint.sha256
Assert-True ($verify.blacklistFree -and $verify.snapshotUnchanged -and $verify.readiness.ready -and $verify.reason -eq '') 'Independent candidate verification must load by name with matching sources'
$missing=Worker 'missing' 'Verify' (Join-Path $owned 'missing.bin') (Join-Path $owned 'missing-copy.bin') $scan.fingerprint.sha256
Assert-True ($missing.reason -ne '' -and -not $missing.snapshotUnchanged) 'Missing registry normally rejects'
[IO.File]::WriteAllText((Join-Path $owned 'corrupt.bin'),'corrupt old registry')
$corruptReadResult=Worker 'corrupt' 'Assess' (Join-Path $owned 'corrupt.bin') (Join-Path $owned 'corrupt-copy.bin')
Assert-True (-not $corruptReadResult.snapshotUnchanged) 'Rebuilt private copy cannot prove old cache healthy'
# Missing/corrupt old cache must still retain an actual required-plugin readiness outcome.
$absentAssess=Worker 'absent-assess' 'Assess' (Join-Path $owned 'absent-old.bin') (Join-Path $owned 'absent-assess.bin')
Assert-True ($absentAssess.readiness.ready -and -not $absentAssess.snapshotUnchanged) 'Absent old registry assesses actual required core privately'
Assert-True ($corruptReadResult.readiness.ready) 'Corrupt assessment retains actual required-plugin outcome'
$corruptVerify=Worker 'corrupt-verify' 'Verify' (Join-Path $owned 'corrupt.bin') (Join-Path $owned 'corrupt-verify.bin') $scan.fingerprint.sha256
Assert-True ($corruptVerify.reason -ne '' -and -not $corruptVerify.snapshotUnchanged) 'VERIFY SDK rewrite cannot validate a corrupt snapshot'
# A registry which never discovered a deployed plugin must fail name/source inventory.
$codec=Join-Path $package 'gstreamer-plugins/libgstcodec2json.dll'; $aside=Join-Path $root 'codec-aside.dll'
Move-Item -LiteralPath $codec -Destination $aside
$seedMissing=Child 'seed-missing-discovery' (Join-Path $package 'gst-inspect-1.0.exe') '' (Join-Path $owned 'incomplete.bin')
Move-Item -LiteralPath $aside -Destination $codec
Assert-True ($seedMissing.Exit -eq 0) 'Incomplete registry seed finishes normally'
$incomplete=Worker 'incomplete' 'Verify' (Join-Path $owned 'incomplete.bin') (Join-Path $owned 'incomplete-copy.bin') $scan.fingerprint.sha256
Assert-True ($incomplete.reason -ne '') 'Explicit DLL availability cannot rescue missing cached discovery'
# A healthy registry discovering a sibling directory is still a wrong-source registry.
$foreignPlugins=Join-Path $root 'foreignplugins'; New-Item -ItemType Directory -Path $foreignPlugins | Out-Null
foreach ($plugin in $plugins) {Copy-Item -LiteralPath $plugin -Destination $foreignPlugins}
$seedForeign=Child 'seed-foreign-source' (Join-Path $package 'gst-inspect-1.0.exe') '' (Join-Path $owned 'foreign.bin') $false $foreignPlugins
Assert-True ($seedForeign.Exit -eq 0) 'Foreign-source registry seed finishes normally'
$foreign=Worker 'foreign-source' 'Verify' (Join-Path $owned 'foreign.bin') (Join-Path $owned 'foreign-copy.bin') $scan.fingerprint.sha256
Assert-True ($foreign.reason -ne '' -and @($foreign.plugins | Where-Object {-not $_.local}).Count -gt 0) 'Wrong plugin source normally rejects'
# Exact pre-reserved objects and physical exclusive lock are retained by this parent.
$lockPath=Join-Path $package 'gstreamer-1.0/.registry-startup.lock'
[IO.File]::WriteAllText($lockPath,"AIRPLAY-GSTREAMER-REGISTRY-LOCK/1`n")
$physicalLock=[IO.FileStream]::new($lockPath,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
$pendingRegistry=Join-Path $package ('gstreamer-1.0/.startup-'+[guid]::NewGuid().ToString()+'.bin')
$pendingRecord=$pendingRegistry.Substring(0,$pendingRegistry.Length-4)+'.json'
[IO.File]::WriteAllBytes($pendingRegistry,[byte[]]@()); [IO.File]::WriteAllBytes($pendingRecord,[byte[]]@())
$fill=@{schemaVersion=1;phase='prepared-fill';nonce=[guid]::NewGuid().ToString('N');package=$package;runtimeRegistry=(Join-Path $owned 'runtime.bin');runtimeMarker=$marker;markerId=[CacheWorkerFileIdentity]::Id((Join-Path $owned '.owner'));runtimeId='';pendingRegistry=$pendingRegistry;pendingRecord=$pendingRecord;registryId=[CacheWorkerFileIdentity]::Id($pendingRegistry);recordId=[CacheWorkerFileIdentity]::Id($pendingRecord);inputSha256=$scan.fingerprint.sha256;baselineExists=$true;baselineSha256=$sharedHashBefore}
$validation=@{schemaVersion=1;validated=$true;inputSha256=$scan.fingerprint.sha256;registrySha256=$verify.registrySha256;plugins=$verify.plugins}
$seals=[Collections.Generic.List[IDisposable]]::new()
try {
 $filled=Worker 'prepare-fill' 'PrepareCommit' (Join-Path $owned 'verify.bin') '' '' ($fill|ConvertTo-Json -Compress) $validation
 Assert-True ($filled.reason -eq '' -and $filled.pendingRegistry.Replace('/','\') -ieq $pendingRegistry.Replace('/','\') -and $filled.pendingRecord.Replace('/','\') -ieq $pendingRecord.Replace('/','\')) 'Worker fills only parent reserved pending paths'
 Assert-True ((Hash $pendingRegistry) -eq $verify.registrySha256 -and (Hash (Join-Path $owned 'runtime.bin')) -eq $verify.registrySha256 -and (Hash $pendingRecord) -eq $filled.recordSha256) 'Pending registry/runtime equal actual verified candidate'
 foreach ($path in @($pendingRegistry,$pendingRecord,(Join-Path $owned 'runtime.bin'),(Join-Path $owned '.owner'),$shared)) {$seals.Add([IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::Read -bor [IO.FileShare]::Delete)))}
 $sealed=@{}; foreach($key in $fill.Keys) {$sealed[$key]=$fill[$key]}; $sealed.phase='sealed-verification'; $sealed.nonce=[guid]::NewGuid().ToString('N'); $sealed.registrySha256=$filled.registrySha256; $sealed.recordSha256=$filled.recordSha256
 $commitable=Worker 'prepare-sealed' 'PrepareCommit' '' '' '' ($sealed|ConvertTo-Json -Compress)
 $proof=$commitable.workerProof|ConvertFrom-Json
 Assert-True ($commitable.reason -eq '' -and $proof.validated -and $proof.cleanupComplete -and $proof.nonce -ceq $sealed.nonce) 'Second bounded invocation proves sealed hashes and precommit cleanup'
 Assert-True ($proof.runtimeSha256 -eq $verify.registrySha256 -and -not (Test-Path -LiteralPath $candidate)) 'Sealed worker preserves same runtime and cleans owned candidates'
} finally {foreach($seal in $seals) {$seal.Dispose()}; $physicalLock.Dispose()}
$scanner=Join-Path $package 'libexec/gstreamer-1.0/gst-plugin-scanner.exe'
Copy-Item -LiteralPath $NonPluginDll -Destination $scanner -Force
$bad=Worker 'bad-scanner' 'Scan' '' (Join-Path $owned 'bad.bin')
Assert-True ($bad.reason -ne '' -and $bad.scannerFallback) 'Invalid scanner fallback is normal explicit rejection'
$neighbor=Join-Path $root 'worker-neighbor.json'; [IO.File]::WriteAllText($neighbor,'foreign-neighbor')
$cleanup=Worker 'cleanup' 'Cleanup' '' ''
$cleanupProof=$cleanup.workerProof|ConvertFrom-Json
Assert-True ($cleanup.cleanup.complete -and $cleanupProof.complete -and $cleanupProof.nonce -ceq $cleanup.nonce -and -not (Test-Path -LiteralPath $owned)) 'Cleanup reports real this-run complete and removes only authorized tree'
Assert-True ([IO.File]::ReadAllText($neighbor) -ceq 'foreign-neighbor') 'Cleanup preserves protocol-like neighboring objects'
$audit.Add('PASS normal scan/verify, missing registry, corrupt private assess, invalid scanner fallback; shared old blacklist unchanged')
$audit | Set-Content (Join-Path $report 'assertions.txt') -Encoding utf8
Write-Output "PASS: evidence $report"

if ($CoordinatorExecutable) {
 Copy-Item -LiteralPath $ScannerExecutable -Destination (Join-Path $package 'libexec/gstreamer-1.0/gst-plugin-scanner.exe') -Force
 $nativeAssertions=Join-Path $report 'native-coordinator-assertions.txt'
 $nativeSuite=Child 'native-coordinator-suite' (Join-Path $package 'coordinator-test.exe') ('--native-coordinator-suite "'+$package+'" "'+$nativeAssertions+'"') (Join-Path $root 'unused-parent.bin') $false '' 90000
 Assert-True ($nativeSuite.Finished -and $nativeSuite.Exit -eq 0 -and (Test-Path $nativeAssertions) -and ([IO.File]::ReadAllText($nativeAssertions) -match '0 failed, 0 skipped')) 'Production coordinator native suite completes normally without failures or skips'
 Get-Content -LiteralPath $nativeAssertions | Write-Output
}
# This fixture root was created by this test; persist evidence first, then remove only its checked tree.
Assert-True ([IO.Path]::GetFullPath($root).StartsWith($parent+'\',[StringComparison]::OrdinalIgnoreCase) -and [IO.File]::ReadAllText((Join-Path $root '.test-owner')) -ceq $run) 'Exact fixture cleanup owner'
$observed=@(Get-ChildItem -LiteralPath $root -Recurse -Force)
foreach ($entry in $observed) {Assert-True (-not ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Fixture cleanup cannot traverse reparse entries'}
$longest=@($observed | Where-Object {$_.Extension -ieq '.dll'} | Sort-Object {$_.FullName.Length} -Descending | Select-Object -First 1)
if ($longest.Count) {$longestPath=$longest[0].FullName; $longestLength=$longestPath.Length} else {$longestPath='';$longestLength=0}
Assert-True ($longestLength -le 202 -and $longestPath -notmatch '[^\x00-\x7f]') 'Final actual DLL path diagnostic bound'
[pscustomobject]@{runtimeRoot=$root;longestDllPath=$longestPath;longestDllAsciiBytes=$longestLength;knownDiagnosticBound=202;wholeSystemSafetyClaim=$false} | ConvertTo-Json | Set-Content (Join-Path $report 'path-evidence.json') -Encoding utf8
Remove-Item -LiteralPath $root -Recurse -Force
Assert-True (-not (Test-Path -LiteralPath $root)) 'This fixture root removed after all evidence persisted'
