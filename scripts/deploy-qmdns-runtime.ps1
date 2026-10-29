param([Parameter(Mandatory = $true)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
# Packaging step: the application must ship the DLL built from third_party/qmdnsengine.
# A pre-existing (for example MSYS2) libqmdnsengine.dll beside the executable is replaced.
$source = Join-Path $BuildDirectory 'third_party/qmdnsengine/libqmdnsengine.dll'
$destination = Join-Path $BuildDirectory 'libqmdnsengine.dll'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Built QMdnsEngine library is missing: $source. Rebuild; the system DLL is not a substitute."
}
Copy-Item -LiteralPath $source -Destination $destination -Force
