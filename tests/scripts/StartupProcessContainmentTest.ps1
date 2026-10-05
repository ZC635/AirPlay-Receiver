param(
    [Parameter(Mandatory=$true)][string]$ReportDirectory,
    [string]$HelperPath,
    [ValidateSet('UnsafeRunning','Suspended','AssignmentFailure','TimeoutCleanup','OutputReadRed','OutputWait','OutputDeadline','Regression')][string]$Mode = 'Suspended'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
if ($Mode -eq 'Regression') {
    foreach ($caseMode in @('Suspended','AssignmentFailure','TimeoutCleanup','OutputWait','OutputDeadline')) {
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
    '$child = Start-Process -FilePath "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe" -ArgumentList ''-NoProfile -NonInteractive -Command "[Threading.Thread]::Sleep(30000)"'' -NoNewWindow -PassThru',
    '[IO.File]::WriteAllText($ChildIdFile, [string]$child.Id)',
    $(if ($Mode -in @('OutputReadRed','OutputWait','OutputDeadline')) { 'exit 0' } else { '[Threading.Thread]::Sleep(30000)' })
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
$cleanup = $null
$writerObserved = $null
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
        if ($Mode -eq 'OutputReadRed') {
            if (-not $native.WaitForExit(5000) -or $native.ExitCode -ne 0 -or $child.HasExited) { throw 'Output RED setup requires exited parent and live inherited writer' }
            Write-Output "Parent exited 0; owned inherited writer=$($child.Id) remains live"
            $sharing = $false
            try { [void][IO.File]::ReadAllText((Join-Path $case 'stdout.txt')) }
            catch {
                $cause = $_.Exception.GetBaseException()
                if ($cause -isnot [IO.IOException] -or ($cause.HResult -band 0xffff) -ne 32) { throw }
                $sharing = $true
                Write-Output "Observed actual ReadAllText sharing violation: $($cause.Message)"
            }
            if (-not $sharing) { throw 'Output RED setup did not reproduce an inherited writer sharing violation' }
            throw 'RED: immediate ReadAllText cannot finalize output while the inherited writer is still alive'
        }
        if ($Mode -in @('OutputWait','OutputDeadline')) {
            if (-not $native.WaitForExit(5000) -or $native.ExitCode -ne 0 -or $child.HasExited) { throw 'Output control requires exited parent and live inherited writer' }
            Write-Output "Parent exited 0; owned inherited writer=$($child.Id) remains live"
            $outputClock = [Diagnostics.Stopwatch]::StartNew()
            if ($Mode -eq 'OutputWait') {
                # Transfer sole job-handle ownership to bounded delayed cleanup.
                # Read while the real writer is still alive, just as it may be
                # after asynchronous kill-on-close has been initiated.
                $writerObserved = [Threading.ManualResetEvent]::new($false)
                $cleanup = [AirPlayStartupTestJob]::BeginDelayedJobCleanup($job, 250, $writerObserved)
                $job = [IntPtr]::Zero
                $captured = [AirPlayStartupTestJob]::ReadFinalOutput((Join-Path $case 'stdout.txt'), (Join-Path $case 'stderr.txt'), 5000, $writerObserved)
                if ($captured.Attempts -lt 2 -or $outputClock.ElapsedMilliseconds -lt 200 -or
                    $captured.Stdout -notmatch 'STDOUT_CAPTURED' -or $captured.Stderr -notmatch 'STDERR_CAPTURED') { throw 'Shared output finalizer did not wait for inherited writers and preserve both outputs' }
                if (-not $cleanup.Wait(5000) -or -not $cleanup.Result -or -not $child.WaitForExit(5000)) { throw 'Delayed job cleanup did not stop the owned writer' }
                Write-Output "PASS: shared output finalizer waited $($outputClock.ElapsedMilliseconds) ms, attempts=$($captured.Attempts); both outputs intact; owned writer stopped"
            } else {
                $timedOut = $false
                try { [void][AirPlayStartupTestJob]::ReadFinalOutput((Join-Path $case 'stdout.txt'), (Join-Path $case 'stderr.txt'), 100) }
                catch {
                    $cause = $_.Exception.GetBaseException()
                    # TimeoutException wraps the actual sharing violation; inspect
                    # the invocation's immediate inner exception, not its base.
                    if ($_.Exception.InnerException -isnot [TimeoutException]) { throw }
                    $timedOut = $true
                    Write-Output "Expected bounded output timeout: $($_.Exception.InnerException.Message); underlying error=$($cause.HResult -band 0xffff)"
                }
                if (-not $timedOut -or $outputClock.ElapsedMilliseconds -gt 2000 -or $child.HasExited) { throw 'Live writer was not rejected at the fixed output deadline' }
                [void][AirPlayStartupTestJob]::CloseHandle($job)
                $job = [IntPtr]::Zero
                if (-not $child.WaitForExit(5000)) { throw 'Output timeout cleanup did not stop the owned writer' }
                Write-Output "PASS: fixed 100 ms output deadline failed safely; elapsed=$($outputClock.ElapsedMilliseconds) ms; owned writer stopped"
            }
            return
        }
        if ($Mode -eq 'TimeoutCleanup') {
            if ($native.WaitForExit(100)) { throw 'Timeout control unexpectedly finished' }
            Write-Output 'Forced owned-parent timeout: 100 ms'
        }
        [void][AirPlayStartupTestJob]::CloseHandle($job)
        $job = [IntPtr]::Zero
        $captured = [AirPlayStartupTestJob]::ReadFinalOutput((Join-Path $case 'stdout.txt'), (Join-Path $case 'stderr.txt'), 5000)
        if (-not $parent.WaitForExit(5000) -or -not $child.WaitForExit(5000)) { throw 'Enrolled parent/descendant escaped job cleanup' }
        if ($captured.Stdout -notmatch 'STDOUT_CAPTURED' -or
            $captured.Stderr -notmatch 'STDERR_CAPTURED') { throw 'Native launch did not preserve redirected output' }
        if (-not $native.WaitForExit(0)) { throw "Native wait did not observe cleanup exit" }
        Write-Output "Native cleanup exit: $($native.ExitCode)"
        Write-Output "PASS: after enrollment/resume, job cleanup stopped owned parent $($parent.Id) and descendant $($child.Id); output captured without pipes"
    }
} finally {
    # Delayed cleanup owns its handle exclusively; never close it a second time.
    # Always finish the bounded task before disposing captured process handles.
    $cleanupFailed = $cleanup -and (-not $cleanup.Wait(5000) -or -not $cleanup.Result)
    if ($job -ne [IntPtr]::Zero) { [void][AirPlayStartupTestJob]::CloseHandle($job) }
    # The RED control deliberately creates an escaped child. Its only fallback
    # cleanup handle comes from the ID file written by our own parent script.
    if ($child) { if (-not $child.HasExited) { Write-Output "Cleaning only captured owned descendant $($child.Id)"; $child.Kill(); if (-not $child.WaitForExit(5000)) { throw "Owned descendant cleanup timed out" } }; Write-Output "Final cleanup confirmed owned descendant $($child.Id) exited"; $child.Dispose() }
    if ($native) { $native.Dispose() }
    if ($parent) { if (-not $parent.HasExited) { $parent.Kill(); [void]$parent.WaitForExit(5000) }; $parent.Dispose() }
    if ($writerObserved -and -not $cleanupFailed) { $writerObserved.Dispose() }
    if ($cleanupFailed) { throw 'Delayed owned-job cleanup failed' }
}
