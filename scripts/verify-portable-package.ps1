[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $PackageDir)) {
    throw "Portable package directory was not found: $PackageDir"
}

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
