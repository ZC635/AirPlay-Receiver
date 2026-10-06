param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$ProbeExecutable,
    [Parameter(Mandatory=$true)][string]$NonPluginDll,
    [Parameter(Mandatory=$true)][string]$Objdump,
    [Parameter(Mandatory=$true)][string]$DependencyBinDirectory,
    [Parameter(Mandatory=$true)][string]$QtPluginDirectory,
    [Parameter(Mandatory=$true)][string]$GStreamerPluginDirectory,
    [Parameter(Mandatory=$true)][string]$ScannerExecutable,
    [Parameter(Mandatory=$true)][string]$ReportDirectory,
    [string]$RuntimeDirectory = '',
    [ValidateSet('Acceptance','DetectionBypass','DuplicateDialog','HealthyAsNegative','WrongReason')]
    [string]$Mode = 'Acceptance',
    [switch]$ExpectRejection
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PeDependencyIsolation.ps1')

. (Join-Path $PSScriptRoot 'StartupProcessContainment.ps1')

# These are literal contract values, deliberately independent of production's
# list and decisions. The positive control needs playback/core plugins only.
$required = @('app','libav','playback','autodetect','videoparsersbad')
$runtimePlugins = @('app','coreelements','playback','autodetect','videoconvertscale',
    'audioconvert','audioresample','videoparsersbad','libav','d3d11','wasapi')
$audit = [Collections.Generic.List[string]]::new()
$sourceHashes = @{}
function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)) }
    finally { $stream.Dispose(); $hash.Dispose() }
}
$root = $null
$runtimeParent = $null
$runtimeCreated = $false
$reportCreated = $false
$runId = [Guid]::NewGuid().ToString('N')
$pathEvidence = [ordered]@{
    mode = $Mode; runtimeParent = $null; runtimeRoot = $null; reportDirectory = $null
    maxPathLength = 240; longestPlannedLength = 0; longestPlannedPath = $null
    longestObservedLength = 0; longestObservedPath = $null
    # Observed fixed text (38) + vkDestroyDevice (15) + NUL in a 256-byte
    # buffer leaves 202 DLL-path bytes. Bounds known create/destroy diagnostics only.
    maxDiagnosticModulePathBytes = 202; diagnosticMessageCapacityBytes = 256
    diagnosticHookFunction = 'vkDestroyDevice'
    longestPlannedModulePathLength = 0; longestPlannedModulePath = $null
    longestObservedModulePathLength = 0; longestObservedModulePath = $null
    runtimeCreated = $false; runtimeRemoved = $false
}

function Assert-NoReparseAncestor([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Unsafe runtime path: reparse point in $cursor"
            }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}

function Assert-RuntimePath([string]$Path, [switch]$Observe) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -ine $root -and -not $full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe runtime path: outside this run: $full"
    }
    if ($full -match '[^\x00-\x7f]') { throw "Unsafe runtime path: must be ASCII: $full" }
    if ($full.Length -gt 240) { throw "Unsafe runtime path: exceeds 240 characters ($($full.Length)): $full" }
    $isModule = [IO.Path]::GetExtension($full) -ieq '.dll'
    if ($isModule -and $full.Length -gt $pathEvidence.maxDiagnosticModulePathBytes) {
        throw "Unsafe runtime path: RTSS diagnostic module path exceeds $($pathEvidence.maxDiagnosticModulePathBytes) ASCII bytes ($($full.Length)): $full"
    }
    if ($Observe -and $full.Length -gt $pathEvidence.longestObservedLength) {
        $pathEvidence.longestObservedLength = $full.Length
        $pathEvidence.longestObservedPath = $full
    }
    if ($Observe -and $isModule -and $full.Length -gt $pathEvidence.longestObservedModulePathLength) {
        $pathEvidence.longestObservedModulePathLength = $full.Length
        $pathEvidence.longestObservedModulePath = $full
    }
}

function Assert-RuntimeLayout([string[]]$RelativeFiles) {
    # Reserve generated filenames before creating or copying fixtures.
    $generated = @(
        'airplay-settings.json', 'airplay_receiver.exe', (Split-Path -Leaf $ProbeExecutable),
        'config/portable-runtime-manifest.txt', 'qt.conf', '.airplay-test-owner',
        'platforms/qwindows.dll', 'platforms/qoffscreen.dll',
        'gstreamer-1.0/registry.x86_64.bin', 'libexec/gstreamer-1.0/gst-plugin-scanner.exe',
        'logs/AirPlay-Diagnostic-2000-01-01-000000-2147483647.log',
        'recording-output/.airplay-recording-00000000000000000000000000000000.video.mkv.part',
        'recording-output/.airplay-recording-00000000000000000000000000000000.audio.mka.part',
        'recording-output/.airplay-recording-00000000000000000000000000000000.lock',
        'recording-sibling/.airplay-recording-00000000000000000000000000000000.mp4.part'
    ) + $RelativeFiles
    $cases = @('template','recording-settings-guard','healthy',
               'DetectionBypass','DuplicateDialog','HealthyAsNegative','WrongReason') +
             @($required | ForEach-Object { "missing-$_" })
    $paths = @($root) + @(
        foreach ($case in $cases) {
            foreach ($relative in $generated) { [IO.Path]::GetFullPath((Join-Path (Join-Path $root $case) $relative)) }
        }
    )
    $longest = $paths | Sort-Object -Property Length -Descending | Select-Object -First 1
    $pathEvidence.longestPlannedPath = $longest
    $pathEvidence.longestPlannedLength = $longest.Length
    $longestModule = $paths | Where-Object { [IO.Path]::GetExtension($_) -ieq '.dll' } |
        Sort-Object -Property Length -Descending | Select-Object -First 1
    $pathEvidence.longestPlannedModulePath = $longestModule
    $pathEvidence.longestPlannedModulePathLength = $longestModule.Length
    foreach ($path in $paths) { Assert-RuntimePath $path }
}

function Assert-RuntimeTree {
    Assert-NoReparseAncestor $root
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($root)
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        Assert-RuntimePath $directory -Observe
        foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
            Assert-RuntimePath $item.FullName -Observe
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Unsafe runtime path: reparse point in $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        }
    }
}

function Assert-OwnedRuntime {
    $expected = [IO.Path]::GetFullPath((Join-Path $runtimeParent ('runtime-' + $runId)))
    if (-not $runtimeCreated -or $root -ine $expected) { throw 'Unsafe cleanup target: not created by this run' }
    Assert-RuntimeTree
    $marker = Join-Path $root '.airplay-test-owner'
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf) -or
        [IO.File]::ReadAllText($marker) -cne $runId) { throw 'Unsafe cleanup target: ownership marker mismatch' }
}

function Test-RuntimePathGuard {
    # Validate the exact known diagnostic boundary without creating these files.
    $moduleAtLimit = Join-Path $root (('d' * (202 - $root.Length - 5)) + '.dll')
    Assert-RuntimePath $moduleAtLimit
    $invalid = @(
        (Join-Path $runtimeParent 'neighbor.txt'),
        ($root + '-neighbor\sentinel'),
        (Join-Path $root ('x' * 241)),
        (Join-Path $root (('d' * (203 - $root.Length - 5)) + '.dll')),
        (Join-Path $root ([string][char]0x4e2d))
    )
    foreach ($path in $invalid) {
        $rejected = $false
        try { Assert-RuntimePath $path }
        catch {
            if (-not $_.Exception.Message.StartsWith('Unsafe runtime path:')) { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw "Unsafe runtime path accepted: $path" }
    }
    # The exact UUID path is insufficient proof without its ownership marker.
    $marker = Join-Path $root '.airplay-test-owner'
    [IO.File]::WriteAllText($marker, 'foreign owner')
    try {
        $rejected = $false
        try { Assert-OwnedRuntime }
        catch {
            if ($_.Exception.Message -ne 'Unsafe cleanup target: ownership marker mismatch') { throw }
            $rejected = $true
        }
        if (-not $rejected) { throw 'Foreign runtime ownership was accepted for cleanup' }
    } finally { [IO.File]::WriteAllText($marker, $runId) }
    $audit.Add('PASS: runtime guard rejected outside/sibling-prefix/overlength/non-ASCII paths and foreign ownership')
    $audit.Add('PASS: RTSS diagnostic DLL path accepted at 202 ASCII bytes and rejected at 203 before launch')
}

function Assert-FixtureRecordingDirectory([string]$Fixture) {
    try {
        $fixturePath = [IO.Path]::GetFullPath($Fixture).TrimEnd('\','/')
        $ownedRoot = [IO.Path]::GetFullPath($root).TrimEnd('\','/') + '\'
        if (-not $fixturePath.StartsWith($ownedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'case escaped the owned runtime root' }
        $json = [Text.UTF8Encoding]::new($false, $true).GetString([IO.File]::ReadAllBytes((Join-Path $fixturePath 'airplay-settings.json')))
        if (-not $json.TrimStart().StartsWith('{')) { throw 'settings root must be a JSON object' }
        $settings = $json | ConvertFrom-Json
        $language = @($settings.PSObject.Properties | Where-Object { $_.Name -ceq 'language' })
        $recording = @($settings.PSObject.Properties | Where-Object { $_.Name -ceq 'recording' })
        if ($language.Count -ne 1 -or $language[0].Value -cne 'en') { throw 'settings must retain language en' }
        if ($recording.Count -ne 1 -or $recording[0].Value -isnot [pscustomobject]) { throw 'recording must be an explicit JSON object' }
        $output = @($recording[0].Value.PSObject.Properties | Where-Object { $_.Name -ceq 'outputDirectory' })
        if ($output.Count -ne 1 -or $output[0].Value -isnot [string] -or $output[0].Value -notmatch '^[A-Za-z]:[\\/]') { throw 'recording.outputDirectory must be an explicit absolute Windows path' }
        # PowerShell accepts JSON dialects Qt rejects; require our exact UTF-8
        # fixture serialization rather than treating its permissive parser as Qt.
        $canonical = [ordered]@{ language='en'; recording=[ordered]@{ outputDirectory=$output[0].Value } } | ConvertTo-Json -Depth 3
        if ($json -cne $canonical) { throw 'settings must use the generated UTF-8 fixture JSON serialization' }
        $path = [IO.Path]::GetFullPath($output[0].Value).TrimEnd('\','/')
        $expected = Join-Path $fixturePath 'recording-output'
        if (-not $path.StartsWith($fixturePath + '\', [StringComparison]::OrdinalIgnoreCase) -or $path -ine $expected) { throw 'recording.outputDirectory must name this case recording-output directory' }
        foreach ($directory in @($fixturePath, $path)) {
            if (-not [IO.Directory]::Exists($directory) -or
                ([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint)) { throw 'owned case/output directories must exist without reparse points' }
        }
        return $path
    } catch { throw "Unsafe recording fixture: $($_.Exception.Message)" }
}

function Invoke-Probe([string]$Fixture, [string]$ProbeMode, [string]$Mutation = '') {
    Assert-RuntimeTree
    $recordingDirectory = Assert-FixtureRecordingDirectory $Fixture
    $caseName = Split-Path -Leaf $Fixture
    Copy-Item -LiteralPath (Join-Path $Fixture 'airplay-settings.json') -Destination (Join-Path $ReportDirectory "$caseName-settings.json")
    $audit.Add("$caseName pre-launch recording.outputDirectory=$recordingDirectory")
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $Fixture (Split-Path -Leaf $ProbeExecutable)
    $start.WorkingDirectory = $Fixture
    $start.Arguments = '--diagnostic-log'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    # Remove inherited GStreamer settings (including registry-update/debug and
    # scanner controls), Qt overrides and application dev/diagnostic overrides.
    foreach ($key in @($start.EnvironmentVariables.Keys)) {
        if ($key -match '^(GST_|QT_|AIRPLAY_)') { $start.EnvironmentVariables.Remove($key) }
    }
    $start.EnvironmentVariables['PATH'] = "$Fixture;$([Environment]::SystemDirectory);$(Split-Path -Parent ([Environment]::SystemDirectory))"
    $start.EnvironmentVariables['QT_PLUGIN_PATH'] = $Fixture
    $start.EnvironmentVariables['QT_QPA_PLATFORM_PLUGIN_PATH'] = Join-Path $Fixture 'platforms'
    $start.EnvironmentVariables['QT_QPA_PLATFORM'] = 'offscreen'
    $start.EnvironmentVariables['AIRPLAY_STARTUP_TEST_MODE'] = $ProbeMode
    $start.EnvironmentVariables['AIRPLAY_STARTUP_TEST_MUTATION'] = $Mutation
    foreach ($suffix in @('', '_1_0')) {
        $start.EnvironmentVariables["GST_PLUGIN_PATH$suffix"] = Join-Path $Fixture 'gstreamer-plugins'
        $start.EnvironmentVariables["GST_PLUGIN_SYSTEM_PATH$suffix"] = Join-Path $Fixture 'gstreamer-plugins'
        $start.EnvironmentVariables["GST_REGISTRY$suffix"] = Join-Path $Fixture 'gstreamer-1.0/registry.x86_64.bin'
        $start.EnvironmentVariables["GST_PLUGIN_SCANNER$suffix"] = Join-Path $Fixture 'libexec/gstreamer-1.0/gst-plugin-scanner.exe'
    }
    $name = Split-Path -Leaf $Fixture
    $stdoutFile = Join-Path $ReportDirectory "$name-stdout.txt"
    $stderrFile = Join-Path $ReportDirectory "$name-stderr.txt"
    $process = $null
    $native = $null
    $job = [AirPlayStartupTestJob]::Create()
    try {
        $native = [AirPlayStartupTestJob]::StartSuspended($start, $stdoutFile, $stderrFile)
        $process = $native
        $native.EnrollAndResume($job)
        if (-not $process.WaitForExit(45000)) { throw 'Startup probe timed out after 45 seconds' }
        # Initiate descendant cleanup, then boundedly await closed output writers.
        # Kill-on-close termination can complete after CloseHandle returns.
        [void][AirPlayStartupTestJob]::CloseHandle($job)
        $job = [IntPtr]::Zero
        $captured = [AirPlayStartupTestJob]::ReadFinalOutput($stdoutFile, $stderrFile, 5000)
        Assert-RuntimeTree
        $out = $captured.Stdout
        $err = $captured.Stderr
        $logs = @(Get-ChildItem -LiteralPath (Join-Path $Fixture 'logs') -Filter '*.log' -ErrorAction SilentlyContinue)
        $diagnostics = ($logs | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
        $diagnostics | Set-Content (Join-Path $ReportDirectory "$name-diagnostics.log")
        $json = @($out -split '\r?\n' | Where-Object { $_.StartsWith('{') })
        if ($json.Count -ne 1) { throw "Expected one probe result, got $($json.Count); exit $($process.ExitCode), stderr $err" }
        $audit.Add("$name child exit=$($process.ExitCode) PATH=$($start.EnvironmentVariables['PATH'])")
        return [pscustomobject]@{ ExitCode=$process.ExitCode; Result=($json[0] | ConvertFrom-Json); Diagnostics=$diagnostics }
    } finally {
        if ($job -ne [IntPtr]::Zero) { [void][AirPlayStartupTestJob]::CloseHandle($job) }
        if ($native) { $native.Dispose() }
    }
}

function Assert-StartupFailure($Observation, [string]$Missing) {
    $r = $Observation.Result
    $failures = [Collections.Generic.List[string]]::new()
    if ($Observation.ExitCode -ne 1 -or $r.startupExit -ne 1) { $failures.Add('actual startup exit must be 1') }
    if (@($r.dialogs).Count -ne 1) { $failures.Add('exactly one dialog required') }
    foreach ($dialog in $r.dialogs) {
        if ($dialog.icon -ne 3) { $failures.Add('dialog must be Critical') }
        if ($dialog.title -ne 'GStreamer plugins unavailable') { $failures.Add('wrong dialog title/failure reason') }
        if ($dialog.text -notmatch ('could not be loaded:\s*' + [regex]::Escape($Missing) + '\s*Re-extract')) {
            $failures.Add('dialog must report exactly the expected missing plugin')
        }
    }
    if (-not $r.pendingEventsProcessed) { $failures.Add('pending events were not processed') }
    if ($r.mainWindowSeen -or $r.receiverStartCalled) { $failures.Add('window/receiver reached') }
    $d = $Observation.Diagnostics
    if ($d -match '\b(window_constructed|receiver_start_requested)\b') { $failures.Add('startup diagnostic reached window/receiver') }
    $readiness = @($d -split '\r?\n' | Where-Object { $_ -match '\bstartup gstreamer_plugin_readiness\b' })
    if ($readiness.Count -ne 1 -or $readiness[0] -notmatch ('missing_count=1 missing_plugins=' + [regex]::Escape($Missing) + ' reason=plugin_load_failure result=no$')) {
        $failures.Add('wrong real readiness result/missing list')
    }
    if ($d -notmatch '\bstartup_aborted\b.*reason=gstreamer_plugin_load_failure') { $failures.Add('wrong startup abort reason') }
    if ($d -notmatch '\bstartup gstreamer_package_environment result=yes') { $failures.Add('package environment was not configured') }
    if ($d -notmatch '\bstartup runtime_path_compatibility\b.*has_non_ascii=no result=yes') { $failures.Add('ASCII path did not pass compatibility') }
    $manifest = @($d -split '\r?\n' | Where-Object { $_ -match '\bstartup runtime_manifest_entry\b' })
    if ($manifest.Count -ne 23 -or @($manifest | Where-Object { $_ -notmatch ' result=present$' }).Count -ne 0) {
        $failures.Add('runtime file presence check was incomplete')
    }
    if ($failures.Count -gt 0) { throw ('Acceptance rejected: ' + ($failures -join '; ')) }
}

function New-Fixture([string]$Name) {
    $fixture = Join-Path $root $Name
    New-Item -ItemType Directory -Path $fixture | Out-Null
    # Copies, never hardlinks: substitutions cannot affect template or sources.
    Get-ChildItem -LiteralPath $template -Force | Copy-Item -Destination $fixture -Recurse
    $recordingDirectory = [IO.Path]::GetFullPath((Join-Path $fixture 'recording-output'))
    New-Item -ItemType Directory -Path $recordingDirectory | Out-Null
    $settings = [ordered]@{ language='en'; recording=[ordered]@{ outputDirectory=$recordingDirectory } }
    [IO.File]::WriteAllText((Join-Path $fixture 'airplay-settings.json'), ($settings | ConvertTo-Json -Depth 3), [Text.UTF8Encoding]::new($false))
    [void](Assert-FixtureRecordingDirectory $fixture)
    return $fixture
}

function Test-RecordingDirectoryGuard {
    # Guard-only owned fixture: contains no executable and never launches GUI.
    $fixture = Join-Path $root 'recording-settings-guard'
    $output = Join-Path $fixture 'recording-output'
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $settingsFile = Join-Path $fixture 'airplay-settings.json'
    $valid = [ordered]@{ language='en'; recording=[ordered]@{ outputDirectory=$output } } | ConvertTo-Json -Depth 3
    [IO.File]::WriteAllText($settingsFile, $valid, [Text.UTF8Encoding]::new($false))
    [void](Assert-FixtureRecordingDirectory $fixture)
    $invalid = [ordered]@{
        omitted = '{"language":"en"}'
        empty = '{"language":"en","recording":{"outputDirectory":""}}'
        relative = '{"language":"en","recording":{"outputDirectory":"recording-output"}}'
        outside = ([ordered]@{ language='en'; recording=@{ outputDirectory=(Join-Path $root 'outside-recording-output') } } | ConvertTo-Json -Depth 3)
        miscased_recording = ([ordered]@{ language='en'; Recording=@{ outputDirectory=$output } } | ConvertTo-Json -Depth 3)
        miscased_output = ([ordered]@{ language='en'; recording=@{ OutputDirectory=$output } } | ConvertTo-Json -Depth 3)
        wrong_type = '{"language":"en","recording":{"outputDirectory":42}}'
        malformed = '{"language":"en",'
        single_quotes = $valid.Replace('"', "'")
        utf16 = $valid
    }
    foreach ($entry in $invalid.GetEnumerator()) {
        $encoding = if ($entry.Key -eq 'utf16') { [Text.Encoding]::Unicode } else { [Text.UTF8Encoding]::new($false) }
        [IO.File]::WriteAllText($settingsFile, $entry.Value, $encoding)
        $rejected = $false
        try { [void](Assert-FixtureRecordingDirectory $fixture) }
        catch {
            if (-not $_.Exception.Message.StartsWith('Unsafe recording fixture:')) { throw }
            $rejected = $true
            $audit.Add("PASS: pre-launch guard rejected $($entry.Key); no process launched")
        }
        if (-not $rejected) { throw "Unsafe recording settings were accepted: $($entry.Key)" }
    }
}

function New-RecordingCleanupSentinels([string]$Fixture) {
    $directory = Assert-FixtureRecordingDirectory $Fixture
    $id = [Guid]::NewGuid().ToString('N')
    $targets = @('video.mkv','audio.mka') | ForEach-Object { Join-Path $directory ".airplay-recording-$id.$_.part" }
    $siblingDirectory = Join-Path $Fixture 'recording-sibling'
    New-Item -ItemType Directory -Path $siblingDirectory | Out-Null
    $sibling = Join-Path $siblingDirectory ".airplay-recording-$id.mp4.part"
    foreach ($path in @($targets) + @($sibling)) { [IO.File]::WriteAllText($path, "Owned startup-cleanup sentinel $id", [Text.UTF8Encoding]::new($false)) }
    $audit.Add("Owned cleanup sentinels created: targets=$($targets.Count) directory=$directory; sibling=$sibling")
    return [pscustomobject]@{ Directory=$directory; Targets=@($targets); Sibling=$sibling; SiblingHash=(Get-Sha256 $sibling) }
}

function Assert-RecordingCleanupSentinels($Sentinels, $Observation) {
    $cleanup = @($Observation.Diagnostics -split '\r?\n' | Where-Object { $_ -match '\bstartup recording_startup_cleanup\b' })
    $targets = @($Sentinels.Targets | ForEach-Object { [ordered]@{ path=$_; presentBefore=$true; presentAfter=[IO.File]::Exists($_) } })
    $siblingExists = [IO.File]::Exists($Sentinels.Sibling)
    $siblingHash = if ($siblingExists) { Get-Sha256 $Sentinels.Sibling } else { '' }
    $evidence = [ordered]@{ outputDirectory=$Sentinels.Directory; staleFiles=$targets; sibling=[ordered]@{ path=$Sentinels.Sibling; presentBefore=$true; presentAfter=$siblingExists; hashBefore=$Sentinels.SiblingHash; hashAfter=$siblingHash }; cleanupDiagnostics=$cleanup }
    [IO.File]::WriteAllText((Join-Path $ReportDirectory 'DetectionBypass-recording-cleanup.json'), ($evidence | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    if ($cleanup.Count -ne 1 -or $cleanup[0] -notmatch '\bstartup recording_startup_cleanup count=2 result=completed$' -or
        $Observation.Diagnostics -notmatch '\bstartup settings_loaded\b' -or
        @($targets | Where-Object { $_.presentAfter }).Count -ne 0 -or -not $siblingExists -or $siblingHash -cne $Sentinels.SiblingHash) {
        throw 'Real bypass startup did not clean exactly the owned target sentinels while preserving the sibling sentinel'
    }
    $audit.Add('PASS: real settings_loaded and recording_startup_cleanup count=2; owned target sentinels deleted; sibling sentinel hash unchanged')
}

try {
    $reportBase = [IO.Path]::GetFullPath($ReportDirectory)
    New-Item -ItemType Directory -Path $reportBase -Force | Out-Null
    $ReportDirectory = Join-Path $reportBase ('run-' + $runId)
    New-Item -ItemType Directory -Path $ReportDirectory | Out-Null
    $reportCreated = $true
    $pathEvidence.reportDirectory = $ReportDirectory
    Write-Output "Report directory: $ReportDirectory"
    if (-not $RuntimeDirectory) { $RuntimeDirectory = Join-Path (Split-Path -Parent $Executable) 'tr' }
    if ($RuntimeDirectory -notmatch '^[A-Za-z]:[\\/]') { throw 'Unsafe runtime path: parent must be an absolute Windows directory' }
    $runtimeParent = [IO.Path]::GetFullPath($RuntimeDirectory).TrimEnd('\','/')
    if ($runtimeParent.Length -le 3) { throw 'Unsafe runtime path: parent must not be a drive root' }
    $root = Join-Path $runtimeParent ('runtime-' + $runId)
    $pathEvidence.runtimeParent = $runtimeParent
    $pathEvidence.runtimeRoot = $root
    if ($root.StartsWith($reportBase + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $ReportDirectory.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe runtime path: report and runtime directories must be separate'
    }
    Assert-NoReparseAncestor $root
    Assert-RuntimeLayout @()
    New-Item -ItemType Directory -Path $runtimeParent -Force | Out-Null
    New-Item -ItemType Directory -Path $root | Out-Null
    $runtimeCreated = $true
    $pathEvidence.runtimeCreated = $true
    [IO.File]::WriteAllText((Join-Path $root '.airplay-test-owner'), $runId)
    $audit.Add("Runtime directory: $root; report directory: $ReportDirectory; file path limit=240; RTSS diagnostic module path limit=$($pathEvidence.maxDiagnosticModulePathBytes) ASCII bytes")
    $template = Join-Path $root 'template'
    foreach ($directory in @('','config','platforms','gstreamer-plugins','gstreamer-1.0','libexec/gstreamer-1.0')) {
        New-Item -ItemType Directory -Path (Join-Path $template $directory) -Force | Out-Null
    }
    $Executable = Require-File $Executable
    $ProbeExecutable = Require-File $ProbeExecutable
    $NonPluginDll = Require-File $NonPluginDll
    $ScannerExecutable = Require-File $ScannerExecutable
    $Objdump = Require-File $Objdump
    $plugins = @($runtimePlugins | ForEach-Object { Require-File (Join-Path $GStreamerPluginDirectory "libgst$_.dll") })
    $platforms = @('qwindows.dll','qoffscreen.dll') | ForEach-Object { Require-File (Join-Path $QtPluginDirectory "platforms/$_") }
    $roots = @($Executable,$ProbeExecutable,$ScannerExecutable,$NonPluginDll) + $plugins + $platforms
    foreach ($source in $roots) { $sourceHashes[$source] = (Get-Sha256 $source) }
    $search = @((Split-Path -Parent $Executable),(Split-Path -Parent $ProbeExecutable),$DependencyBinDirectory)
    $deps = Get-PeDependencies -Roots $roots -SearchDirectories $search -Objdump $Objdump -RejectImport { param($name,$file) } -Audit $audit
    foreach ($source in $deps.Values) { $sourceHashes[$source] = (Get-Sha256 $source) }
    $relativeFiles = @($deps.Values | ForEach-Object { Split-Path -Leaf $_ }) +
        @($plugins | ForEach-Object { 'gstreamer-plugins/' + (Split-Path -Leaf $_) })
    Assert-RuntimeLayout $relativeFiles
    Copy-PeDependencies -Resolved $deps -Directory $template -ExcludedFiles ($plugins + $platforms + @($NonPluginDll))
    Copy-Item -LiteralPath $Executable -Destination (Join-Path $template 'airplay_receiver.exe')
    Copy-Item -LiteralPath $ProbeExecutable -Destination (Join-Path $template (Split-Path -Leaf $ProbeExecutable))
    Copy-Item -LiteralPath $ScannerExecutable -Destination (Join-Path $template 'libexec/gstreamer-1.0/gst-plugin-scanner.exe')
    foreach ($plugin in $plugins) { Copy-Item -LiteralPath $plugin -Destination (Join-Path $template 'gstreamer-plugins') }
    foreach ($platform in $platforms) { Copy-Item -LiteralPath $platform -Destination (Join-Path $template 'platforms') }
    Copy-Item -LiteralPath (Require-File (Join-Path (Split-Path -Parent $Executable) 'config/portable-runtime-manifest.txt')) -Destination (Join-Path $template 'config/portable-runtime-manifest.txt')
    # Invalid empty registry is intentional: file presence succeeds, GStreamer
    # scans afresh in each process rather than inheriting a source/host registry.
    [IO.File]::WriteAllBytes((Join-Path $template 'gstreamer-1.0/registry.x86_64.bin'), [byte[]]@())
    "[Paths]`nPlugins=." | Set-Content (Join-Path $template 'qt.conf')
    # Settings are written per case after copying, with its own absolute output directory.

    if ($Mode -eq 'Acceptance') {
        Test-RuntimePathGuard
        Test-RecordingDirectoryGuard
        $healthy = New-Fixture 'healthy'
        $positive = Invoke-Probe $healthy 'core'
        $p = $positive.Result
        if ($positive.ExitCode -ne 0 -or -not $p.ready -or -not $p.allLoaded -or -not $p.configured -or
            -not $p.manifestComplete -or @($p.missing).Count -ne 0 -or @($p.origins).Count -ne 5) { throw 'Healthy core-plugin control failed' }
        foreach ($name in $required) {
            $origin = @($p.origins | Where-Object { $_.name -eq $name })
            $expectedFile = Join-Path $healthy "gstreamer-plugins/libgst$name.dll"
            if ($origin.Count -ne 1 -or -not $origin[0].loaded -or [IO.Path]::GetFullPath($origin[0].filename) -ine $expectedFile) {
                throw "Plugin $name did not load from fixture: $($origin | ConvertTo-Json -Compress)"
            }
            $audit.Add("PASS: loaded $name from $($origin[0].filename)")
        }
        foreach ($missing in $required) {
            $fixture = New-Fixture "missing-$missing"
            Copy-Item -LiteralPath $NonPluginDll -Destination (Join-Path $fixture "gstreamer-plugins/libgst$missing.dll") -Force
            Assert-StartupFailure (Invoke-Probe $fixture 'startup') $missing
            $audit.Add("PASS: $missing real GUI startup exit=1; one Critical dialog after pending events; no window/receiver; complete file check")
        }
    } else {
        $fixture = New-Fixture $Mode
        $probeMode = 'startup'
        $mutation = ''
        if ($Mode -eq 'HealthyAsNegative') { $probeMode = 'core' }
        elseif ($Mode -eq 'WrongReason') { Remove-Item -LiteralPath (Join-Path $fixture 'gstreamer-plugins/libgstapp.dll') }
        else {
            Copy-Item -LiteralPath $NonPluginDll -Destination (Join-Path $fixture 'gstreamer-plugins/libgstapp.dll') -Force
            if ($Mode -eq 'DetectionBypass') { $mutation = 'detection-bypass' }
            if ($Mode -eq 'DuplicateDialog') { $mutation = 'duplicate-dialog' }
        }
        $sentinels = if ($Mode -eq 'DetectionBypass') { New-RecordingCleanupSentinels $fixture } else { $null }
        $observation = Invoke-Probe $fixture $probeMode $mutation
        # Controls count as RED only after their intended real behavior happened.
        # Setup/loader failures cannot accidentally qualify as oracle rejection.
        switch ($Mode) {
            'DetectionBypass' {
                Assert-RecordingCleanupSentinels $sentinels $observation
                if ($observation.ExitCode -ne 0 -or -not $observation.Result.receiverStartCalled -or
                    -not $observation.Result.mainWindowSeen -or
                    $observation.Diagnostics -notmatch 'startup receiver_start_requested' -or
                    $observation.Diagnostics -notmatch 'missing_count=0 missing_plugins= reason=ready result=yes') {
                    throw 'Bypass control did not reach the intended successful startup behavior'
                }
            }
            'DuplicateDialog' {
                if ($observation.ExitCode -ne 1 -or @($observation.Result.dialogs).Count -ne 2 -or
                    @($observation.Result.dialogs | Where-Object { $_.icon -ne 3 }).Count -ne 0 -or
                    $observation.Diagnostics -notmatch 'reason=gstreamer_plugin_load_failure') {
                    throw 'Duplicate control did not produce two Critical dialogs for plugin failure'
                }
            }
            'HealthyAsNegative' {
                if ($observation.ExitCode -ne 0 -or -not $observation.Result.ready -or
                    -not $observation.Result.allLoaded -or -not $observation.Result.manifestComplete -or
                    @($observation.Result.missing).Count -ne 0 -or @($observation.Result.origins).Count -ne 5) {
                    throw 'Healthy-as-negative control did not load the real healthy core runtime'
                }
                foreach ($origin in $observation.Result.origins) {
                    if (-not $origin.loaded -or [IO.Path]::GetFullPath($origin.filename) -ine
                        (Join-Path $fixture "gstreamer-plugins/libgst$($origin.name).dll")) {
                        throw 'Healthy-as-negative plugin origin escaped the fixture'
                    }
                }
            }
            'WrongReason' {
                if ($observation.ExitCode -ne 1 -or @($observation.Result.dialogs).Count -ne 1 -or
                    $observation.Diagnostics -notmatch 'reason=missing_runtime' -or
                    $observation.Diagnostics -match 'startup gstreamer_plugin_readiness') {
                    throw 'Wrong-reason control did not produce the intended manifest failure'
                }
            }
        }
        if ($Mode -eq 'HealthyAsNegative') {
            # A real healthy registry result cannot satisfy the negative oracle.
            # Give it actual core results, without running recording/startup.
            $observation.Result | Add-Member startupExit $observation.ExitCode
            $observation.Result | Add-Member dialogs @()
            $observation.Result | Add-Member pendingEventsProcessed $false
            $observation.Result | Add-Member mainWindowSeen $false
            $observation.Result | Add-Member receiverStartCalled $false
        }
        $rejection = $null
        try { Assert-StartupFailure $observation 'app' }
        catch { if ($_.Exception.Message -notlike 'Acceptance rejected:*') { throw }; $rejection = $_.Exception.Message }
        if (-not $rejection) { throw "Behavioral control $Mode was unexpectedly accepted" }
        $audit.Add("RED: $Mode $rejection")
        if (-not $ExpectRejection) { throw $rejection }
        $audit.Add("PASS: $Mode real acceptance oracle rejected the behavioral control")
    }
    $audit.Add("PASS: $Mode")
} catch {
    $audit.Add("FAIL: $($_.Exception.Message)")
    Write-Error -Message $_.Exception.Message -ErrorAction Continue
} finally {
    foreach ($source in $sourceHashes.Keys) {
        if ((Get-Sha256 $source) -ne $sourceHashes[$source]) {
            $audit.Add("FAIL: source was modified: $source")
        }
    }
    if ($sourceHashes.Count -gt 0) { $audit.Add("Source SHA256 verified: $($sourceHashes.Count) files") }
    if ($runtimeCreated -and (Test-Path -LiteralPath $root)) {
        try {
            Assert-OwnedRuntime
            Remove-Item -LiteralPath $root -Recurse -Force
            $pathEvidence.runtimeRemoved = $true
            $audit.Add('PASS: removed only this run owned runtime directory')
        } catch {
            $audit.Add("FAIL: cleanup refused/failed: $($_.Exception.Message)")
            Write-Error -Message $_.Exception.Message -ErrorAction Continue
        }
    }
    if ($reportCreated) {
        [IO.File]::WriteAllText((Join-Path $ReportDirectory 'runtime-paths.json'),
            ($pathEvidence | ConvertTo-Json -Depth 3), [Text.UTF8Encoding]::new($false))
        $audit | Set-Content (Join-Path $ReportDirectory 'audit.txt')
    }
}
if (@($audit | Where-Object { $_.StartsWith('FAIL:') }).Count -gt 0) { exit 1 }
Write-Output $audit[-2]
exit 0
