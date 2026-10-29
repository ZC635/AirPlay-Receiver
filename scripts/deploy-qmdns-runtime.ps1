param([Parameter(Mandatory = $true)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
function Get-MdnsRuntimeHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [Convert]::ToBase64String($sha.ComputeHash($stream)) }
    finally { $stream.Dispose(); $sha.Dispose() }
}
$source = Join-Path $BuildDirectory 'third_party/qmdnsengine/libqmdnsengine.dll'
$destination = Join-Path $BuildDirectory 'libqmdnsengine.dll'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Built QMdnsEngine library is missing: $source. Rebuild; the system DLL is not a substitute."
}
Copy-Item -LiteralPath $source -Destination $destination -Force
if ((Get-MdnsRuntimeHash $source) -ne (Get-MdnsRuntimeHash $destination)) {
    throw 'Packaged QMdnsEngine does not match this build'
}
