param([Parameter(Mandatory = $true)][string]$ProjectRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $ProjectRoot 'tests/scripts/PeDependencyIsolation.ps1')
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('pe-copy-test-' + [guid]::NewGuid().ToString('N'))
try {
    foreach ($directory in @('app', 'tests', 'output')) {
        New-Item -ItemType Directory -Force (Join-Path $fixture $directory) | Out-Null
    }
    $first = Join-Path $fixture 'app/libqmdnsengine.dll'
    $second = Join-Path $fixture 'tests/libqmdnsengine.dll'
    $output = Join-Path $fixture 'output'
    [IO.File]::WriteAllText($first, 'same build')
    [IO.File]::WriteAllText($second, 'same build')
    $resolved = [System.Collections.Generic.Dictionary[string, string]]::new()
    $resolved.Add($first, $first)
    $resolved.Add($second, $second)
    Copy-PeDependencies -Resolved $resolved -Directory $output
    if ([IO.File]::ReadAllText($first) -ne [IO.File]::ReadAllText((Join-Path $output 'libqmdnsengine.dll'))) {
        throw 'Identical DLLs were not copied correctly'
    }
    [IO.File]::WriteAllText($second, 'different build')
    $rejected = $false
    try { Copy-PeDependencies -Resolved $resolved -Directory $output }
    catch {
        if ($_.Exception.Message -notlike 'Conflicting isolated DLL*') { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Different binaries with the same DLL name must be rejected' }
    Write-Output 'PASS: identical DLL copies accepted, conflicting contents rejected'
}
finally {
    Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
}
