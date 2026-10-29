param([Parameter(Mandatory = $true)][string]$ProjectRoot)
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([System.IO.Path]::GetTempPath()) ('mdns-runtime-test-' + [guid]::NewGuid().ToString('N'))
try {
    $sourceDir = Join-Path $fixture 'third_party/qmdnsengine'
    New-Item -ItemType Directory -Force $sourceDir | Out-Null
    $source = Join-Path $sourceDir 'libqmdnsengine.dll'
    $destination = Join-Path $fixture 'libqmdnsengine.dll'
    [IO.File]::WriteAllText($source, 'patched build fixture')
    [IO.File]::WriteAllText($destination, 'old system fixture')
    & (Join-Path $ProjectRoot 'scripts/deploy-qmdns-runtime.ps1') -BuildDirectory $fixture
    if ([IO.File]::ReadAllText($source) -ne [IO.File]::ReadAllText($destination)) {
        throw 'Deployment left the old system QMdnsEngine DLL in place'
    }
    # A stale DLL in the destination cannot substitute for missing build output.
    $missing = Join-Path $fixture 'missing'
    New-Item -ItemType Directory -Force $missing | Out-Null
    [IO.File]::WriteAllText((Join-Path $missing 'libqmdnsengine.dll'), 'old system fixture')
    $rejected = $false
    try { & (Join-Path $ProjectRoot 'scripts/deploy-qmdns-runtime.ps1') -BuildDirectory $missing }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'Missing build output must fail deployment' }
    Write-Output 'PASS: deployment uses the built library and rejects missing build output'
}
finally {
    Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
}
