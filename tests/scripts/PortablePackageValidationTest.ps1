param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot
)

$ErrorActionPreference = "Stop"

$verifyScript = Join-Path $ProjectRoot "scripts\verify-portable-package.ps1"
if (-not (Test-Path -LiteralPath $verifyScript)) {
    throw "Missing portable package verifier: $verifyScript"
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

    $requiredPaths = @(
        "airplay_receiver.exe",
        "Qt6Core.dll",
        "Qt6Gui.dll",
        "Qt6Widgets.dll",
        "platforms\qwindows.dll",
        "libgcc_s_seh-1.dll",
        "libstdc++-6.dll",
        "libwinpthread-1.dll",
        "libgstreamer-1.0-0.dll",
        "gstreamer-plugins\libgstapp.dll",
        "gstreamer-plugins\libgstplayback.dll",
        "gstreamer-plugins\libgstautodetect.dll",
        "gstreamer-plugins\libgstvideoparsersbad.dll",
        "gstreamer-plugins\libgstlibav.dll",
        "gstreamer-plugins\libgstd3d11.dll",
        "gstreamer-plugins\libgstwasapi.dll",
        "gstreamer-1.0\registry.x86_64.bin",
        "libqmdnsengine.dll"
    )

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
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
