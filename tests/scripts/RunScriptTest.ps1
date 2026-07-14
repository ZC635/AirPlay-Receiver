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

if ($buildContent -notmatch 'function Build-CTestPrerequisiteTargets') {
    throw "scripts\build.ps1 must define a CTest prerequisite target builder."
}
if ($buildContent -notmatch '--target\s+RendererSampleTapsTest') {
    throw "scripts\build.ps1 -Test must explicitly build RendererSampleTapsTest before CTest."
}
if ($buildContent -notmatch 'third_party\\uxplay\\tests\\RendererSampleTapsTest\.exe') {
    throw "scripts\build.ps1 must verify the excluded RendererSampleTapsTest executable exists."
}
$prerequisiteCalls = [regex]::Matches($buildContent, 'Build-CTestPrerequisiteTargets\s+-Directory').Count
if ($prerequisiteCalls -lt 3) {
    throw "scripts\build.ps1 must build CTest prerequisites for both -All directories and the single selected variant."
}
