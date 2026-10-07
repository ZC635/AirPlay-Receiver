param([string]$HelperPath, [string]$FixtureRoot)
$ErrorActionPreference = 'Stop'
$env:PATH = 'C:/msys64/ucrt64/bin;' + $env:PATH
New-Item -ItemType Directory -Force $FixtureRoot | Out-Null
$exe = Join-Path (Resolve-Path -LiteralPath $FixtureRoot).Path 'fake-probe.exe'
# Discard stale metadata without reopening or stopping its recorded processes.
Get-ChildItem -LiteralPath $FixtureRoot -Filter '*.pid' | Remove-Item -Force
if (-not (Test-Path $exe)) {
Add-Type -OutputAssembly $exe -OutputType ConsoleApplication -TypeDefinition @"
using System;
using System.Diagnostics;
using System.Threading;
public class Fixture {
 public static int Main(string[] a) {
  if(a.Length>1) System.IO.File.WriteAllText(a[1]+"."+Process.GetCurrentProcess().Id+".pid",Process.GetCurrentProcess().Id.ToString());
  if(a[0]=="child") { Thread.Sleep(15000); return 0; }
  Console.WriteLine("available-output"); Console.Error.WriteLine("available-error");
  if(a[0]=="fail") return 7;
  if(a[0]=="hang") { Thread.Sleep(15000); return 0; }
  if(a[0]=="pipe") {
   var p=Process.Start(new ProcessStartInfo(Process.GetCurrentProcess().MainModule.FileName,"child " + a[1]) {UseShellExecute=false});
   System.IO.File.WriteAllText(a[1],p.Id.ToString());
  }
  return 0;
 }
}
"@
}
. $HelperPath
function Stop-FixturePidFile {
 param([string]$PidFile)
 if (-not (Test-Path -LiteralPath $PidFile)) { return $false }
 $owned = $null
 try {
  $owned = [Diagnostics.Process]::GetProcessById([int](Get-Content -LiteralPath $PidFile))
  $actual = [IO.Path]::GetFullPath($owned.MainModule.FileName)
  if (-not [string]::Equals($actual, $exe, [StringComparison]::OrdinalIgnoreCase)) { return $false }
  $owned.Kill()
  if (-not $owned.WaitForExit(1000)) { throw 'Fixture cleanup exceeded 1000 ms' }
  return $true
 } catch [ArgumentException] { return $false }
 finally { if ($owned) { $owned.Dispose() } }
}
# A stale file may identify a live unrelated executable. Never kill it.
$staleFile = Join-Path $FixtureRoot 'stale.pid'
[IO.File]::WriteAllText($staleFile, $PID.ToString())
if (Stop-FixturePidFile $staleFile) { throw 'Cleanup accepted mismatched live process' }
$self = [Diagnostics.Process]::GetProcessById($PID)
try { if ($self.HasExited) { throw 'Cleanup stopped unrelated PowerShell worker' } }
finally { $self.Dispose() }
Remove-Item -LiteralPath $staleFile
'PASS stale PID executable mismatch guard'
$neighborInfo=New-Object Diagnostics.ProcessStartInfo
$neighborInfo.FileName=$exe; $neighborInfo.Arguments='child ' + (Join-Path $FixtureRoot 'neighbor'); $neighborInfo.UseShellExecute=$false; $neighborInfo.RedirectStandardOutput=$true; $neighborInfo.RedirectStandardError=$true; $neighborInfo.CreateNoWindow=$true
$neighbor = [Diagnostics.Process]::Start($neighborInfo)
try {
 foreach ($mode in @('normal','fail','hang','pipe','missing')) {
  $si = New-Object Diagnostics.ProcessStartInfo
  $si.FileName = $exe
  if ($mode -eq 'missing') { $si.FileName = Join-Path $FixtureRoot 'missing.exe' }
  $childFile=Join-Path $FixtureRoot ($mode + '-child.pid')
  $si.Arguments = "$mode `"$childFile`""
  $si.UseShellExecute=$false; $si.RedirectStandardOutput=$true; $si.RedirectStandardError=$true; $si.CreateNoWindow=$true
  $sw=[Diagnostics.Stopwatch]::StartNew(); $message=''; $result=@()
  try { $result=@(Invoke-PortableRecordingProbe $si -TimeoutMilliseconds 400 -CleanupMilliseconds 200) } catch { $message=$_.Exception.Message }
  if ($sw.ElapsedMilliseconds -gt 2500) { throw "$mode exceeded bounded duration" }
  if ($mode -eq 'normal') {
   if ($message -or ($result -join ' ') -notmatch 'available-output.*available-error') { throw "normal output failure: $message $result" }
  } else {
   $expected=@{fail='exit code 7';hang='timed out';pipe='output did not complete';missing='could not be started'}[$mode]
   if ($message -notmatch $expected) { throw "$mode expected $expected got $message" }
   if ($mode -ne 'missing' -and $message -notmatch 'available-output' ) { throw "$mode lost partial output" }
  }
  if ($neighbor.HasExited) { throw 'Unrelated neighboring process was stopped' }
  if ($mode -eq 'hang') {
   foreach ($pidFile in @(Get-ChildItem -LiteralPath $FixtureRoot -Filter 'hang-child.pid.*.pid')) {
    $stillRunning = $null
    try {
     $stillRunning = [Diagnostics.Process]::GetProcessById([int](Get-Content -LiteralPath $pidFile.FullName))
     if ([string]::Equals([IO.Path]::GetFullPath($stillRunning.MainModule.FileName), $exe, [StringComparison]::OrdinalIgnoreCase)) {
      throw 'Timed-out probe survived bounded cleanup'
     }
    } catch [ArgumentException] {} finally { if ($stillRunning) { $stillRunning.Dispose() } }
   }
  }
  "PASS $mode $($sw.ElapsedMilliseconds) ms $message"
  if (Test-Path $childFile) {
   [void](Stop-FixturePidFile $childFile)
   Remove-Item -LiteralPath $childFile
  }
 }
} finally {
 if (-not $neighbor.HasExited) {$neighbor.Kill(); [void]$neighbor.WaitForExit(1000)}
 $neighbor.Dispose()
 foreach ($pidFile in @(Get-ChildItem -LiteralPath $FixtureRoot -Filter '*.pid')) { [void](Stop-FixturePidFile $pidFile.FullName) }
}
