param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot,
    [Parameter(Mandatory = $true)]
    [string]$RuntimeDirectory
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
$requiredLinguistDependencies = @(
    '"qt6-tools"',
    '"qt6-declarative"',
    '"qt6-translations"',
    'Join-Path $BinPath "lupdate.exe"',
    'Join-Path $BinPath "lrelease.exe"',
    'Join-Path $BinPath "Qt6Qml.dll"',
    'share\qt6\translations\qtbase_zh_CN.qm'
)
foreach ($dependency in $requiredLinguistDependencies) {
    if (-not $buildContent.Contains($dependency)) {
        throw "scripts\build.ps1 must require $dependency so lupdate can run."
    }
}

$registryBlockStartsPortableOnly = $buildContent -match 'if \(\$IsPortable\) \{\s*Write-Host "  Generating(?: verified)? GStreamer registry cache\.\.\."'
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


$lockHelper = Join-Path $ProjectRoot 'scripts/GStreamerRegistryLock.ps1'
if (!(Test-Path -LiteralPath $lockHelper -PathType Leaf)) { throw 'Deployment must provide the persistent startup registry protocol lock helper.' }
. $lockHelper
function Assert-RunScriptRuntimeDirectory([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Length -le 3 -or $full -match '[^\x00-\x7f]') { throw 'Lock test runtime must be an absolute short ASCII directory.' }
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Lock test runtime ancestor/reparse boundary refused.' }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    if ((Test-Path -LiteralPath $full) -and (Resolve-Path -LiteralPath $full).Path -ine $full) { throw 'Lock test runtime resolved boundary refused.' }
}
$lockRuntime = [IO.Path]::GetFullPath($RuntimeDirectory).TrimEnd('\')
Assert-RunScriptRuntimeDirectory $lockRuntime
$ownedName = 'airplay-lock-test-' + [guid]::NewGuid().ToString('N')
$owned = Join-Path $lockRuntime $ownedName
$token = [guid]::NewGuid().ToString('N')
if (Test-Path -LiteralPath $owned) { throw 'Lock test owned directory collision refused.' }
Assert-RunScriptRuntimeDirectory $owned
# All runtime ancestors and the fresh child are checked before the first write.
New-Item -ItemType Directory -Path $lockRuntime -Force | Out-Null
Assert-RunScriptRuntimeDirectory $lockRuntime
New-Item -ItemType Directory -Path $owned | Out-Null
Assert-RunScriptRuntimeDirectory $owned
$owner = Join-Path $owned '.owner'
$ownerStream = [IO.FileStream]::new($owner,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
try { $ownerBytes=[Text.Encoding]::ASCII.GetBytes($token); $ownerStream.Write($ownerBytes,0,$ownerBytes.Length); $ownerStream.Flush($true) }
finally { $ownerStream.Dispose() }
$lease = $null
try {
    $lease = Enter-AirPlayRegistryWriteLock $owned
    $busy = $false
    try { $second = Enter-AirPlayRegistryWriteLock $owned; Exit-AirPlayRegistryWriteLock $second }
    catch { $busy = $_.Exception.Message -match 'Registry write lock' }
    if (!$busy) { throw 'Persistent writer lease must exclude another writer.' }
    Exit-AirPlayRegistryWriteLock $lease; $lease = $null
    $lockPath = Join-Path $owned 'gstreamer-1.0/.registry-startup.lock'
    if ([IO.File]::ReadAllText($lockPath) -cne "AIRPLAY-GSTREAMER-REGISTRY-LOCK/1`n") { throw 'Lock release must retain exact startup protocol.' }
    $lease = Enter-AirPlayRegistryWriteLock $owned
    Exit-AirPlayRegistryWriteLock $lease; $lease = $null
    [IO.File]::WriteAllText($lockPath, 'UNKNOWN-OWNER')
    $unknown = $false
    try { $second = Enter-AirPlayRegistryWriteLock $owned; Exit-AirPlayRegistryWriteLock $second }
    catch { $unknown = $_.Exception.Message -match 'Registry write lock' }
    if (!$unknown -or [IO.File]::ReadAllText($lockPath) -cne 'UNKNOWN-OWNER') { throw 'Foreign protocol must be refused and preserved.' }
    Write-Output 'PASS: persistent registry lock acquire, busy, release/reacquire, foreign preservation.'
} finally {
    if ($lease) { Exit-AirPlayRegistryWriteLock $lease }
    $resolved = [IO.Path]::GetFullPath($owned)
    Assert-RunScriptRuntimeDirectory $lockRuntime
    Assert-RunScriptRuntimeDirectory $resolved
    if ($resolved -cne [IO.Path]::GetFullPath((Join-Path $lockRuntime $ownedName)) -or
        !$resolved.StartsWith($lockRuntime + '\', [StringComparison]::OrdinalIgnoreCase) -or
        [IO.File]::ReadAllText($owner) -cne $token) { throw 'Lock test cleanup ownership refused.' }
    foreach ($entry in Get-ChildItem -LiteralPath $resolved -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            ($entry.Name -cne '.owner' -and $entry.Name -cne 'gstreamer-1.0') -or
            ($entry.Name -ceq '.owner' -and $entry.PSIsContainer) -or
            ($entry.Name -ceq 'gstreamer-1.0' -and !$entry.PSIsContainer)) { throw 'Lock test unknown cleanup object refused.' }
        if ($entry.PSIsContainer) {
            foreach ($cache in Get-ChildItem -LiteralPath $entry.FullName -Force) {
                if ($cache.PSIsContainer -or $cache.Name -cne '.registry-startup.lock' -or
                    ($cache.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Lock test unknown cache cleanup object refused.' }
            }
        }
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
