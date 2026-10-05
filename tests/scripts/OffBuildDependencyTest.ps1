param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$SmokeTestExecutable,
    [Parameter(Mandatory = $true)][string]$Objdump,
    [Parameter(Mandatory = $true)][string]$DependencyBinDirectory,
    [Parameter(Mandatory = $true)][string]$QtPluginDirectory,
    [Parameter(Mandatory = $true)][string]$ReportDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$audit = [System.Collections.Generic.List[string]]::new()

function Require-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
    return (Get-Item -LiteralPath $Path).FullName
}

try {
    $ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
    $Executable = Require-File $Executable
    $SmokeTestExecutable = Require-File $SmokeTestExecutable
    $Objdump = Require-File $Objdump
    $plugin = Require-File (Join-Path $QtPluginDirectory 'platforms/qoffscreen.dll')
    $buildRoot = Split-Path -Parent $Executable
    $systemDirectory = [Environment]::SystemDirectory
    $windowsDirectory = Split-Path -Parent $systemDirectory
    $searchDirectories = @($buildRoot, (Split-Path -Parent $SmokeTestExecutable), $DependencyBinDirectory)
    $resolved = [System.Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    $queue = [System.Collections.Generic.Queue[string]]::new()
    foreach ($root in @($Executable, $SmokeTestExecutable, $plugin)) { $queue.Enqueue($root) }

    # Inspect real PE imports before metadata: an ON binary must fail on its GST import.
    while ($queue.Count -gt 0) {
        $file = Require-File $queue.Dequeue()
        if ($resolved.ContainsKey($file)) { continue }
        $resolved.Add($file, $file)
        $output = @(& $Objdump -p $file 2>&1)
        if ($LASTEXITCODE -ne 0) { throw "objdump failed for $file`: $($output -join ' ')" }
        $imports = @($output | ForEach-Object {
            if ($_ -match '^\s*DLL Name:\s*(\S+)\s*$') { $Matches[1] }
        })
        if ($imports.Count -eq 0) { throw "No PE imports parsed from $file" }
        foreach ($name in $imports) {
            if ($name -match '^(lib)?(?:gst|gstreamer).*\.dll$') {
                throw "GStreamer import rejected: $name (imported by $file)"
            }
            if ($name -match '^(?:api|ext)-ms-.*\.dll$') {
                $audit.Add("$file -> $name [Windows API-set]")
                continue
            }
            # Follow application-local dependencies before the Windows system and PATH directories.
            $candidate = $null
            foreach ($directory in @((Split-Path -Parent $file), $buildRoot, $systemDirectory, $windowsDirectory) + $searchDirectories) {
                $path = Join-Path $directory $name
                if (Test-Path -LiteralPath $path -PathType Leaf) {
                    $candidate = (Get-Item -LiteralPath $path).FullName
                    break
                }
            }
            if (-not $candidate) { throw "Unresolved import: $name (imported by $file)" }
            $isSystem = $candidate.StartsWith($systemDirectory + '\', [StringComparison]::OrdinalIgnoreCase) -or
                        (Split-Path -Parent $candidate) -eq $windowsDirectory
            $audit.Add("$file -> $candidate [system=$isSystem]")
            if (-not $isSystem) { $queue.Enqueue($candidate) }
        }
    }

    $compileCommands = Require-File (Join-Path $buildRoot 'compile_commands.json')
    $ninjaFile = Require-File (Join-Path $buildRoot 'build.ninja')
    $dependencyPattern = '(?i)gstreamer|(?:^|[\\/\s])(?:lib)?gst[^\\/\s;"'']*\.(?:a|lib|dll)|(?:^|\s)-l(?:gst|gstreamer)\S*'
    $commands = Get-Content -LiteralPath $compileCommands -Raw | ConvertFrom-Json
    $commands = @($commands)
    if ($commands.Count -eq 0) { throw 'compile_commands.json contains no commands' }
    foreach ($entry in $commands) {
        if ($entry.PSObject.Properties.Name -contains 'arguments') { $command = $entry.arguments -join ' ' }
        elseif ($entry.PSObject.Properties.Name -contains 'command') { $command = $entry.command }
        else { throw 'Compile command has neither command nor arguments' }
        if ($command -match $dependencyPattern) { throw "GStreamer compile dependency rejected: $command" }
    }
    $argumentLines = @(Get-Content -LiteralPath $ninjaFile | Where-Object { $_ -match '^\s*(?:INCLUDES|LINK_LIBRARIES|LINK_FLAGS|FLAGS)\s*=' })
    if ($argumentLines.Count -eq 0) { throw 'build.ninja contains no generated compiler/link arguments' }
    foreach ($line in $argumentLines) {
        if ($line -match $dependencyPattern) { throw "GStreamer build dependency rejected: $line" }
    }
    $audit.Add("Metadata passed: $($commands.Count) compile commands and $($argumentLines.Count) Ninja argument lines")

    New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
    $isolatedDirectory = Join-Path $ReportDirectory ('isolated-' + [Guid]::NewGuid().ToString('N'))
    $isolatedPlugins = Join-Path $isolatedDirectory 'plugins'
    $isolatedPlatforms = Join-Path $isolatedPlugins 'platforms'
    New-Item -ItemType Directory -Path $isolatedPlatforms -Force | Out-Null
    $copiedNames = [System.Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $resolved.Values) {
        if ([IO.Path]::GetExtension($file) -ine '.dll' -or $file -eq $plugin) { continue }
        $name = Split-Path -Leaf $file
        if ($copiedNames.ContainsKey($name) -and $copiedNames[$name] -ne $file) {
            throw "Conflicting isolated DLL paths for $name"
        }
        $copiedNames[$name] = $file
        Copy-Item -LiteralPath $file -Destination (Join-Path $isolatedDirectory $name)
    }
    Copy-Item -LiteralPath $plugin -Destination $isolatedPlatforms
    # Override Qt's compiled-in plugin prefix as well as the process environment.
    "[Paths]`nPlugins=plugins" | Set-Content -LiteralPath (Join-Path $isolatedDirectory 'qt.conf')
    $isolatedExecutable = Join-Path $isolatedDirectory (Split-Path -Leaf $SmokeTestExecutable)
    Copy-Item -LiteralPath $SmokeTestExecutable -Destination $isolatedExecutable
    $qtReport = Join-Path $isolatedDirectory 'VideoFrameBridgeTest-results.txt'
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $isolatedExecutable
    $start.Arguments = '-o "' + $qtReport + ',txt"'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WorkingDirectory = $isolatedDirectory
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['PATH'] = "$isolatedDirectory;$systemDirectory;$windowsDirectory"
    $start.EnvironmentVariables['QT_PLUGIN_PATH'] = $isolatedPlugins
    $start.EnvironmentVariables['QT_QPA_PLATFORM_PLUGIN_PATH'] = $isolatedPlatforms
    $start.EnvironmentVariables['QT_QPA_PLATFORM'] = 'offscreen'
    $audit.Add("Isolated executable: $isolatedExecutable")
    $audit.Add("Isolated PATH: $($start.EnvironmentVariables['PATH'])")
    $audit.Add("Isolated QT_PLUGIN_PATH: $isolatedPlugins")
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw 'Could not start isolated frame test' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(60000)) {
        $process.Kill()
        throw 'Isolated frame test timed out after 60 seconds'
    }
    $stdout.Result | Set-Content -LiteralPath (Join-Path $isolatedDirectory 'stdout.txt')
    $stderr.Result | Set-Content -LiteralPath (Join-Path $isolatedDirectory 'stderr.txt')
    $audit.Add("Isolated frame test exit: $($process.ExitCode)")
    if ($process.ExitCode -ne 0) { throw "Isolated frame test failed with exit $($process.ExitCode); see $isolatedDirectory" }
    Require-File $qtReport | Out-Null
    $audit.Add("PASS: recursive imports, compile/link metadata, and isolated simulated frames ($($resolved.Count) PE files)")
    $audit | Set-Content -LiteralPath (Join-Path $ReportDirectory 'dependency-audit.txt')
    Write-Output $audit[-1]
    Write-Output "Report: $ReportDirectory"
    exit 0
}
catch {
    $audit.Add("FAIL: $($_.Exception.Message)")
    New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
    $audit | Set-Content -LiteralPath (Join-Path $ReportDirectory 'dependency-audit.txt')
    Write-Error -Message $_.Exception.Message -ErrorAction Continue
    exit 1
}
