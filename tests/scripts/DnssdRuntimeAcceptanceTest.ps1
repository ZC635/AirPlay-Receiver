param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$ProbeExecutable,
    [Parameter(Mandatory=$true)][string]$Objdump,
    [Parameter(Mandatory=$true)][string]$DependencyBinDirectory,
    [Parameter(Mandatory=$true)][string]$ReportDirectory,
    [ValidateSet('Acceptance','DirectImport','TransitiveImport','DynamicLoad')][string]$Mode = 'Acceptance',
    [string]$FixtureExecutable,
    [string]$FixtureDll
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'PeDependencyIsolation.ps1')
# Bonjour/Windows DNS-SD and common Avahi DLL basenames (case insensitive).
$externalPattern = '^(?:(?:lib)?(?:dns[-_]?sd|dnssd|bonjour|mdnsresponder|mdnsnsp)(?:[-_.].*)?|(?:lib)?avahi(?:[-_].*)?)\.dll$'
$audit = [System.Collections.Generic.List[string]]::new()
$rejectExternal = { param($name, $file)
    if ($name -match $externalPattern) { throw "External DNS-SD import rejected: $name (imported by $file)" }
}
try {
    New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
    $Objdump = Require-File $Objdump
    $Executable = Require-File $Executable
    $ProbeExecutable = Require-File $ProbeExecutable
    $search = @((Split-Path -Parent $Executable), (Split-Path -Parent $ProbeExecutable), $DependencyBinDirectory)
    if ($Mode -eq 'DirectImport' -or $Mode -eq 'TransitiveImport') {
        $FixtureExecutable = Require-File $FixtureExecutable
        $rejected = $false
        try {
            $unused = Get-PeDependencies -Objdump $Objdump -Roots @($FixtureExecutable) -SearchDirectories $search -Audit $audit -RejectImport $rejectExternal
        } catch {
            if ($_.Exception.Message -notlike 'External DNS-SD import rejected:*') { throw }
            $audit.Add($_.Exception.Message)
            $rejected = $true
        }
        if (-not $rejected) { throw 'Negative control FAILED: external DNS-SD import was accepted' }
        $audit.Add("PASS: $Mode negative control rejected a real PE import")
    } else {
        $deps = Get-PeDependencies -Objdump $Objdump -Roots @($Executable, $ProbeExecutable) -SearchDirectories $search -Audit $audit -RejectImport $rejectExternal
        $isolated = Join-Path ([IO.Path]::GetFullPath($ReportDirectory)) ('isolated-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $isolated -Force | Out-Null
        Copy-PeDependencies $deps $isolated
        $probe = Join-Path $isolated (Split-Path -Leaf $ProbeExecutable)
        Copy-Item -LiteralPath $ProbeExecutable -Destination $probe
        # Core-only probe needs no Qt/GStreamer plugins. Override inherited plugin search too.
        "[Paths]`nPlugins=plugins" | Set-Content (Join-Path $isolated 'qt.conf')
        $systemDirectory = [Environment]::SystemDirectory
        $windowsDirectory = Split-Path -Parent $systemDirectory
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $probe
        $start.WorkingDirectory = $isolated
        $qtReport = Join-Path $isolated 'probe-results.txt'
        $start.Arguments = '-o "' + $qtReport + ',txt"'
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $start.EnvironmentVariables['PATH'] = "$isolated;$systemDirectory;$windowsDirectory"
        $start.EnvironmentVariables['QT_PLUGIN_PATH'] = (Join-Path $isolated 'plugins')
        $start.EnvironmentVariables['QT_QPA_PLATFORM_PLUGIN_PATH'] = (Join-Path $isolated 'plugins/platforms')
        $start.EnvironmentVariables['GST_PLUGIN_PATH'] = (Join-Path $isolated 'gst-plugins')
        $start.EnvironmentVariables['GST_PLUGIN_SYSTEM_PATH'] = ''
        $start.EnvironmentVariables['GST_REGISTRY'] = (Join-Path $isolated 'gst-registry.bin')
        $start.EnvironmentVariables.Remove('DNSSD_TEST_LOAD')
        if ($Mode -eq 'DynamicLoad') {
            $FixtureDll = Require-File $FixtureDll
            $fixturePath = Join-Path $isolated (Split-Path -Leaf $FixtureDll)
            Copy-Item -LiteralPath $FixtureDll -Destination $fixturePath
            $start.EnvironmentVariables['DNSSD_TEST_LOAD'] = $fixturePath
        }
        $audit.Add("Isolated probe: $probe")
        $audit.Add("Isolated PATH: $($start.EnvironmentVariables['PATH'])")
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $start
        if (-not $process.Start()) { throw 'Could not start isolated DNS-SD probe' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(60000)) {
            $process.Kill()
            throw 'Isolated DNS-SD probe timed out after 60 seconds'
        }
        $output = $stdout.Result
        $output | Set-Content (Join-Path $isolated 'stdout.txt')
        $stderr.Result | Set-Content (Join-Path $isolated 'stderr.txt')
        Require-File $qtReport | Out-Null
        $audit.Add("Probe exit: $($process.ExitCode)")
        if ($Mode -eq 'DynamicLoad') {
            if ($process.ExitCode -eq 0 -or $output -notmatch 'DNSSD_FIXTURE_LOADED' -or $output -notmatch 'External DNS-SD module rejected:') {
                throw "Negative control FAILED: dynamically loaded external runtime was not rejected; see $isolated"
            }
            $audit.Add('PASS: DynamicLoad negative control loaded the fixture and the runtime check rejected it')
        } else {
            if ($process.ExitCode -ne 0) { throw "Isolated DNS-SD probe failed with exit $($process.ExitCode); see $isolated" }
            $phases = [System.Collections.Generic.HashSet[string]]::new()
            $moduleCount = 0
            foreach ($line in ($output -split '\r?\n')) {
                if (-not $line.StartsWith('DNSSD_MODULE ')) { continue }
                $entry = $line.Substring(13) | ConvertFrom-Json
                $path = [IO.Path]::GetFullPath($entry.path)
                if ((Split-Path -Leaf $path) -match $externalPattern) { throw "External DNS-SD module rejected: $path" }
                if (-not $path.StartsWith($isolated + '\', [StringComparison]::OrdinalIgnoreCase) -and
                    -not $path.StartsWith($windowsDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    # Import-resolved DLLs must come from our copy. Other modules can
                    # be injected by host software; record them without changing it.
                    if (@($deps.Values | ForEach-Object { Split-Path -Leaf $_ }) -contains (Split-Path -Leaf $path)) {
                        throw "Required dependency escaped isolated/system directories: $path"
                    }
                    $audit.Add("Host-injected/other module outside sandbox: $path")
                }
                [void]$phases.Add($entry.phase)
                ++$moduleCount
            }
            foreach ($phase in @('before-start','publishing','after-stop')) {
                if (-not $phases.Contains($phase)) { throw "Missing loaded-module snapshot: $phase" }
            }
            $audit.Add("PASS: recursive application/probe dependencies and isolated real discovery/publishing/stop; $($deps.Count) PE files, $moduleCount module observations")
        }
        $process.Dispose()
    }
    $audit | Set-Content (Join-Path $ReportDirectory 'dependency-audit.txt')
    Write-Output $audit[-1]
    exit 0
} catch {
    $audit.Add("FAIL: $($_.Exception.Message)")
    New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
    $audit | Set-Content (Join-Path $ReportDirectory 'dependency-audit.txt')
    Write-Error -Message $_.Exception.Message -ErrorAction Continue
    exit 1
}
