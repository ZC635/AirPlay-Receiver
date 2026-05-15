param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot
)

$ErrorActionPreference = "Stop"

$runScript = Join-Path $ProjectRoot "scripts\run.ps1"
if (-not (Test-Path -LiteralPath $runScript)) {
    throw "Missing run script: $runScript"
}

$content = Get-Content -LiteralPath $runScript -Raw
if ($content -notmatch '\$env:AIRPLAY_MSYS2_PATH_MODE\s*=\s*"1"') {
    throw "scripts\run.ps1 must mark MSYS2 PATH launches so the app skips standalone runtime checks."
}

if ($content -notmatch 'AIRPLAY_MSYS2_PATH_MODE') {
    throw "scripts\run.ps1 must restore AIRPLAY_MSYS2_PATH_MODE after launching."
}

$buildScript = Join-Path $ProjectRoot "scripts\build.ps1"
if (-not (Test-Path -LiteralPath $buildScript)) {
    throw "Missing build script: $buildScript"
}

$buildContent = Get-Content -LiteralPath $buildScript -Raw
$registryBlockStartsPortableOnly = $buildContent -match 'if \(\$IsPortable\) \{\s*Write-Host "  Generating GStreamer registry cache\.\.\."'
if ($registryBlockStartsPortableOnly) {
    throw "scripts\build.ps1 must generate the GStreamer registry for every deployed standalone build, not only portable builds."
}
