param(
    [Parameter(Mandatory=$true)][string]$ReportDirectory,
    [string]$HelperPath,
    [ValidateSet('UnsafeRunning','Suspended','AssignmentFailure','TimeoutCleanup','Regression')][string]$Mode = 'Suspended'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
if ($Mode -eq 'Regression') {
    foreach ($caseMode in @('Suspended','AssignmentFailure','TimeoutCleanup')) {
        & 'C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe' -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Mode $caseMode -ReportDirectory $ReportDirectory
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    exit 0
}
$helper = if ($HelperPath) { $HelperPath } else { Join-Path $PSScriptRoot 'StartupProcessContainment.ps1' }
if (Test-Path -LiteralPath $helper) { . $helper }
else {
    # Before-fix RED uses the exact existing job helper, not a mock job.
    $source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'GStreamerPluginReadinessProcessTest.ps1'))
    $code = [regex]::Match($source, 'Add-Type @"\r?\n([\s\S]*?)\r?\n"@').Groups[1].Value
    Add-Type $code
}
$case = Join-Path $ReportDirectory ($Mode + '-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $case | Out-Null
$marker = Join-Path $case 'parent-started.txt'
$childIdFile = Join-Path $case 'owned-child-id.txt'
$parentScript = Join-Path $case 'parent.ps1'
@(
    'param([string]$Marker, [string]$ChildIdFile)',
    '[IO.File]::WriteAllText($Marker, "PARENT_STARTED")',
    '[Console]::WriteLine("STDOUT_CAPTURED")',
    '[Console]::Error.WriteLine("STDERR_CAPTURED")',
    '$child = [Diagnostics.Process]::Start("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe", ''-NoProfile -NonInteractive -Command "[Threading.Thread]::Sleep(30000)"'')',
    '[IO.File]::WriteAllText($ChildIdFile, [string]$child.Id)',
    '[Threading.Thread]::Sleep(30000)'
) | Set-Content -LiteralPath $parentScript
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = 'C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe'
$start.Arguments = '-NoProfile -NonInteractive -File "' + $parentScript + '" -Marker "' + $marker + '" -ChildIdFile "' + $childIdFile + '"'
$start.WorkingDirectory = $case
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$job = [AirPlayStartupTestJob]::Create()
$parent = $null
$child = $null
$native = $null
try {
    if ($Mode -eq 'UnsafeRunning') {
        $parent = [Diagnostics.Process]::new()
        $parent.StartInfo = $start
        [void]$parent.Start()
    } else {
        $native = [AirPlayStartupTestJob]::StartSuspended($start, (Join-Path $case 'stdout.txt'), (Join-Path $case 'stderr.txt'))
        $parent = [Diagnostics.Process]::GetProcessById($native.Id)
    }
    $delay = [Diagnostics.Stopwatch]::StartNew()
    [Threading.Thread]::Sleep(1500)
    Write-Output "Forced pre-enrollment delay: $($delay.ElapsedMilliseconds) ms; mode=$Mode; owned parent=$($parent.Id)"
    if ($Mode -eq 'UnsafeRunning') {
        if (-not [IO.File]::Exists($childIdFile)) { throw 'RED setup did not create an owned descendant in the launch gap' }
        $child = [Diagnostics.Process]::GetProcessById([int][IO.File]::ReadAllText($childIdFile))
        [AirPlayStartupTestJob]::Assign($job, $parent.Handle)
        [void][AirPlayStartupTestJob]::CloseHandle($job)
        $job = [IntPtr]::Zero
        if (-not $parent.WaitForExit(5000) -or $child.HasExited) { throw 'RED setup did not reproduce the pre-enrollment escape' }
        throw 'RED: child ran before enrollment and its already-created descendant escaped job cleanup'
    }
    if ([IO.File]::Exists($marker) -or [IO.File]::Exists($childIdFile)) {
        # A deliberately unsafe launch mutation may have entered the script just
        # before the observation. Let its ID handoff finish before cleanup.
        $handoff = [Diagnostics.Stopwatch]::StartNew()
        while (-not [IO.File]::Exists($childIdFile) -and $handoff.ElapsedMilliseconds -lt 5000 -and -not $parent.HasExited) { [Threading.Thread]::Sleep(10) }
        if ([IO.File]::Exists($childIdFile)) { $child = [Diagnostics.Process]::GetProcessById([int][IO.File]::ReadAllText($childIdFile)) }
        throw 'Child executed before job enrollment'
    }
    Write-Output 'PASS: no parent code or descendant spawn during the forced delay'
    if ($Mode -eq 'AssignmentFailure') {
        $rejected = $false
        try { $native.EnrollAndResume([IntPtr]::Zero) }
        catch { $rejected = $true; Write-Output ('Expected assignment failure: ' + $_.Exception.InnerException.Message) }
        if (-not $rejected -or -not $parent.WaitForExit(5000) -or [IO.File]::Exists($marker) -or [IO.File]::Exists($childIdFile)) {
            throw 'Assignment failure did not terminate the still-suspended owned child'
        }
        if (-not $native.WaitForExit(0) -or $native.ExitCode -ne 1) { throw 'Native assignment-failure exit status was not 1' }
        Write-Output 'PASS: failed assignment terminated the suspended parent without executing child code'
    } else {
        $native.EnrollAndResume($job)
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        while (-not [IO.File]::Exists($childIdFile) -and $deadline.ElapsedMilliseconds -lt 5000) { [Threading.Thread]::Sleep(10) }
        if (-not [IO.File]::Exists($childIdFile)) { throw 'Enrolled parent did not start its owned descendant after resume' }
        $child = [Diagnostics.Process]::GetProcessById([int][IO.File]::ReadAllText($childIdFile))
        if ($Mode -eq 'TimeoutCleanup') {
            if ($native.WaitForExit(100)) { throw 'Timeout control unexpectedly finished' }
            Write-Output 'Forced owned-parent timeout: 100 ms'
        }
        [void][AirPlayStartupTestJob]::CloseHandle($job)
        $job = [IntPtr]::Zero
        if (-not $parent.WaitForExit(5000) -or -not $child.WaitForExit(5000)) { throw 'Enrolled parent/descendant escaped job cleanup' }
        if ([IO.File]::ReadAllText((Join-Path $case 'stdout.txt')) -notmatch 'STDOUT_CAPTURED' -or
            [IO.File]::ReadAllText((Join-Path $case 'stderr.txt')) -notmatch 'STDERR_CAPTURED') { throw 'Native launch did not preserve redirected output' }
        if (-not $native.WaitForExit(0)) { throw "Native wait did not observe cleanup exit" }
        Write-Output "Native cleanup exit: $($native.ExitCode)"
        Write-Output "PASS: after enrollment/resume, job cleanup stopped owned parent $($parent.Id) and descendant $($child.Id); output captured without pipes"
    }
} finally {
    if ($job -ne [IntPtr]::Zero) { [void][AirPlayStartupTestJob]::CloseHandle($job) }
    # The RED control deliberately creates an escaped child. Its only fallback
    # cleanup handle comes from the ID file written by our own parent script.
    if ($child) { if (-not $child.HasExited) { Write-Output "Cleaning only captured owned descendant $($child.Id)"; $child.Kill(); if (-not $child.WaitForExit(5000)) { throw "Owned descendant cleanup timed out" } }; $child.Dispose() }
    if ($native) { $native.Dispose() }
    if ($parent) { if (-not $parent.HasExited) { $parent.Kill(); [void]$parent.WaitForExit(5000) }; $parent.Dispose() }
}
