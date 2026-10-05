param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

try {
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    $cache = [IO.File]::ReadAllText((Join-Path $BuildDirectory 'CMakeCache.txt'))
    if ($cache -notmatch '(?m)^NO_MARCH_NATIVE:BOOL=ON\s*$') {
        throw 'Compatible builds require NO_MARCH_NATIVE=ON'
    }
    $entries = [IO.File]::ReadAllText((Join-Path $BuildDirectory 'compile_commands.json')) | ConvertFrom-Json
    $entries = @($entries)
    $checked = 0
    $cCount = 0
    $cxxCount = 0
    foreach ($entry in $entries) {
        $extension = [IO.Path]::GetExtension($entry.file).ToLowerInvariant()
        if ($extension -notin @('.c', '.cc', '.cpp', '.cxx')) { continue }
        if ($entry.PSObject.Properties.Name -contains 'arguments') {
            $command = $entry.arguments -join ' '
        } elseif ($entry.PSObject.Properties.Name -contains 'command') {
            $command = $entry.command
        } else { throw 'Compile command has neither command nor arguments' }
        $architectures = [regex]::Matches($command, '(?:^|\s)-march=([^\s"'']+)')
        $tunings = [regex]::Matches($command, '(?:^|\s)-mtune=([^\s"'']+)')
        if ($architectures.Count -eq 0 -or $tunings.Count -eq 0) {
            throw ('CPU baseline not explicit: ' + $entry.file)
        }
        foreach ($architecture in $architectures) {
            if ($architecture.Groups[1].Value -ne 'x86-64') {
                throw ('Non-portable CPU architecture: ' + $entry.file + ' ' + $architecture.Value.Trim())
            }
        }
        foreach ($tuning in $tunings) {
            if ($tuning.Groups[1].Value -ne 'generic') {
                throw ('Host-specific CPU tuning: ' + $entry.file + ' ' + $tuning.Value.Trim())
            }
        }
        if ($command -match '(?:^|\s)-m(?:avx\S*|fma\S*|sse3|ssse3|sse4\S*|popcnt|bmi\S*|aes|pclmul|f16c)(?:\s|$)') {
            throw ('Higher instruction set enabled: ' + $entry.file)
        }
        $checked++
        if ($extension -eq '.c') { $cCount++ } else { $cxxCount++ }
    }
    if ($checked -eq 0 -or $cxxCount -eq 0) { throw 'No C++ compile commands checked' }
    if ($cache -match '(?m)^AIRPLAY_WITH_UXPLAY:BOOL=ON\s*$' -and $cCount -eq 0) {
        throw 'ON build must include real UxPlay C compile commands'
    }
    $resourceBlock = $false
    $resourceCount = 0
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $BuildDirectory 'build.ninja'))) {
        if ($line -match '^build .*: ') {
            $resourceBlock = $line -match ': RC_COMPILER'
            if ($resourceBlock) { $resourceCount++ }
        } elseif ($resourceBlock -and $line -match '^\s*FLAGS\s*=.*-m(?:arch|tune)=') {
            throw 'C/C++ CPU flags leaked into Windows resource compilation'
        }
    }
    if ($resourceCount -eq 0) { throw 'No Windows resource compile commands checked' }
    Write-Output "Compatible generated commands: C=$cCount C++=$cxxCount RC=$resourceCount"
} catch {
    Write-Output ('FAIL: ' + $_.Exception.Message)
    exit 1
}
