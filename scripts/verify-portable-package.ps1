[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir
)

$ErrorActionPreference = "Stop"

function Get-PortableRuntimeManifestPaths {
    $projectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
    $manifestPath = Join-Path $projectRoot "config\portable-runtime-manifest.txt"
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Portable runtime manifest was not found: $manifestPath"
    }

    return Get-Content -LiteralPath $manifestPath |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and (-not $_.StartsWith("#")) } |
        ForEach-Object { $_.Replace('/', '\') }
}

if (-not (Test-Path -LiteralPath $PackageDir)) {
    throw "Portable package directory was not found: $PackageDir"
}

$requiredPaths = @(Get-PortableRuntimeManifestPaths)

$missing = @()
foreach ($relativePath in $requiredPaths) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageDir $relativePath))) {
        $missing += $relativePath
    }
}

$forbiddenRootDlls = @(
    "D3DCompiler_47.dll",
    "D3DCompiler_46.dll",
    "D3DCompiler_43.dll",
    "dxcompiler.dll",
    "dxil.dll"
)

$forbidden = @()
foreach ($dll in $forbiddenRootDlls) {
    if (Test-Path -LiteralPath (Join-Path $PackageDir $dll)) {
        $forbidden += $dll
    }
}

$errors = @()
if ($missing.Count -ne 0 -or $forbidden.Count -ne 0) {
    if ($missing.Count -ne 0) {
        $errors += "Portable package is missing required files: $($missing -join ', ')"
    }
    if ($forbidden.Count -ne 0) {
        $errors += "Portable package contains non-portable system DLLs: $($forbidden -join ', ')"
    }

    throw ($errors -join "`n")
}

"Portable package verification passed: $PackageDir"
