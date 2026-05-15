param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot
)

$ErrorActionPreference = "Stop"

$verifyScript = Join-Path $ProjectRoot "scripts\verify-portable-package.ps1"
if (-not (Test-Path -LiteralPath $verifyScript)) {
    throw "Missing portable package verifier: $verifyScript"
}

function Get-PortableRuntimeManifestPaths {
    $manifestPath = Join-Path $ProjectRoot "config\portable-runtime-manifest.txt"
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Portable runtime manifest was not found: $manifestPath"
    }

    return Get-Content -LiteralPath $manifestPath |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and (-not $_.StartsWith("#")) } |
        ForEach-Object { $_.Replace('/', '\') }
}

function New-RequiredPortableFile {
    param(
        [string]$Root,
        [string]$RelativePath
    )

    $path = Join-Path $Root $RelativePath
    $parent = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    New-Item -ItemType File -Path $path -Force | Out-Null
}

function New-CompletePortablePackageFixture {
    param([string]$Root)

    $requiredPaths = @(Get-PortableRuntimeManifestPaths)

    foreach ($relativePath in $requiredPaths) {
        New-RequiredPortableFile $Root $relativePath
    }
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("airplay-portable-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null

try {
    $validPackage = Join-Path $tempRoot "valid"
    New-Item -ItemType Directory -Path $validPackage -Force | Out-Null
    New-CompletePortablePackageFixture $validPackage

    & $verifyScript -PackageDir $validPackage

    $blockedPackage = Join-Path $tempRoot "blocked-d3dcompiler"
    New-Item -ItemType Directory -Path $blockedPackage -Force | Out-Null
    New-CompletePortablePackageFixture $blockedPackage
    New-RequiredPortableFile $blockedPackage "D3DCompiler_47.dll"

    $powershell = (Get-Command powershell -ErrorAction SilentlyContinue).Source
    if (-not $powershell) {
        $powershell = (Get-Command pwsh -ErrorAction Stop).Source
    }
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $blockedOutput = & $powershell -NoProfile -ExecutionPolicy Bypass -File $verifyScript -PackageDir $blockedPackage 2>&1
    $blockedExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference
    if ($blockedExitCode -eq 0) {
        throw "Expected portable verifier to reject bundled D3DCompiler_47.dll."
    }
    if (($blockedOutput -join "`n") -notmatch "D3DCompiler_47\.dll") {
        throw "Expected verifier output to mention D3DCompiler_47.dll. Output: $($blockedOutput -join ' ')"
    }

    $missingManifestPackage = Join-Path $tempRoot "missing-manifest"
    New-Item -ItemType Directory -Path $missingManifestPackage -Force | Out-Null
    New-CompletePortablePackageFixture $missingManifestPackage
    Remove-Item -LiteralPath (Join-Path $missingManifestPackage "config\portable-runtime-manifest.txt") -Force -ErrorAction SilentlyContinue

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $missingManifestOutput = & $powershell -NoProfile -ExecutionPolicy Bypass -File $verifyScript -PackageDir $missingManifestPackage 2>&1
    $missingManifestExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference
    if ($missingManifestExitCode -eq 0) {
        throw "Expected portable verifier to reject packages missing config\portable-runtime-manifest.txt."
    }
    if (($missingManifestOutput -join "`n") -notmatch "config\\portable-runtime-manifest\.txt") {
        throw "Expected verifier output to mention config\portable-runtime-manifest.txt. Output: $($missingManifestOutput -join ' ')"
    }
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
