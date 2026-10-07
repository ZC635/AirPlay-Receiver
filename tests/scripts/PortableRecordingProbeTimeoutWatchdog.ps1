param([string]$HelperPath, [string]$FixtureRoot, [string]$LogPath)
$ErrorActionPreference='Stop'
$env:PATH='C:/msys64/ucrt64/bin;' + $env:PATH
$si=New-Object Diagnostics.ProcessStartInfo
$si.FileName=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$si.Arguments="-NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot/PortableRecordingProbeTimeoutTest.ps1`" -HelperPath `"$HelperPath`" -FixtureRoot `"$FixtureRoot`""
$si.UseShellExecute=$false;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true;$si.CreateNoWindow=$true
$p=New-Object Diagnostics.Process;$p.StartInfo=$si
try{
 [void]$p.Start();$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync()
 if(-not $p.WaitForExit(8000)){$p.Kill();[void]$p.WaitForExit(1000);'WATCHDOG: test exceeded 8000 ms' | Set-Content $LogPath;exit 124}
 if(-not $o.Wait(1000) -or -not $e.Wait(1000)){throw 'watchdog output incomplete'}
 ($o.Result + $e.Result) | Set-Content $LogPath
 Get-Content $LogPath
 exit $p.ExitCode
} finally {
 $p.Dispose()
 $expected = [IO.Path]::GetFullPath((Join-Path (Resolve-Path -LiteralPath $FixtureRoot).Path 'fake-probe.exe'))
 foreach ($pidFile in @(Get-ChildItem -LiteralPath $FixtureRoot -Filter '*.pid' -ErrorAction SilentlyContinue)) {
  $owned = $null
  try {
   $owned = [Diagnostics.Process]::GetProcessById([int](Get-Content -LiteralPath $pidFile.FullName))
   $actual = [IO.Path]::GetFullPath($owned.MainModule.FileName)
   if ([string]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) {
    $owned.Kill(); [void]$owned.WaitForExit(1000)
   }
  } catch {} finally { if ($owned) { $owned.Dispose() } }
 }
}
