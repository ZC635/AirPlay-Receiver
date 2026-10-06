param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectRoot
)

$ErrorActionPreference = "Stop"

$verifyScript = Join-Path $ProjectRoot "scripts\verify-portable-package.ps1"
if (-not (Test-Path -LiteralPath $verifyScript)) {
    throw "Missing portable package verifier: $verifyScript"
}
$verifyContent = Get-Content -LiteralPath $verifyScript -Raw
if ($verifyContent -notmatch 'System\.Diagnostics\.ProcessStartInfo') {
    throw "Portable verifier must launch runtime probe with ProcessStartInfo."
}
if ($verifyContent -match 'ComSpec|GetTempFileName') {
    throw "Portable verifier must not shell-expand package paths or use temporary redirection files."
}

$readmePath = Join-Path $ProjectRoot 'README.md'
$readmeContent = Get-Content -LiteralPath $readmePath -Raw
if ($readmeContent -notmatch [regex]::Escape('The [Diagnostic Logging] title suffix is the only continuous indication')) {
    throw 'README must identify the [Diagnostic Logging] title suffix as the only continuous diagnostic indicator.'
}

$diagnosticLauncherName = 'Start with Diagnostic Logging.cmd'
if ($diagnosticLauncherName -match '[^\x20-\x7E]') { throw 'Diagnostic launcher filename must contain printable ASCII characters only.' }
$diagnosticLauncher = Join-Path $ProjectRoot "scripts\$diagnosticLauncherName"
if (-not (Test-Path -LiteralPath $diagnosticLauncher)) { throw "Missing diagnostic launcher: $diagnosticLauncher" }
$launcherContent = Get-Content -LiteralPath $diagnosticLauncher -Raw
if ($launcherContent -notmatch '%~dp0airplay_receiver\.exe') { throw 'Diagnostic launcher must resolve the adjacent executable from its own directory.' }
if ($launcherContent -notmatch [regex]::Escape('--diagnostic-log')) { throw 'Diagnostic launcher must pass --diagnostic-log.' }
if ($launcherContent -match 'runas|AIRPLAY_DEBUG_LOG|AppData|TEMP|>') { throw 'Diagnostic launcher must not elevate, persist environment state, redirect, or choose another directory.' }

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

    $requiredPaths += $diagnosticLauncherName

    foreach ($relativePath in $requiredPaths) {
        New-RequiredPortableFile $Root $relativePath
    }
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("airplay-portable-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
$hadFixtureMode = Test-Path Env:AIRPLAY_PORTABLE_FIXTURE_TEST
$previousFixtureMode = $env:AIRPLAY_PORTABLE_FIXTURE_TEST
$env:AIRPLAY_PORTABLE_FIXTURE_TEST = '1'

try {
    $powershell = (Get-Command powershell -ErrorAction SilentlyContinue).Source
    if (-not $powershell) {
        $powershell = (Get-Command pwsh -ErrorAction Stop).Source
    }

    $validPackage = Join-Path $tempRoot "valid"
    New-Item -ItemType Directory -Path $validPackage -Force | Out-Null
    New-CompletePortablePackageFixture $validPackage

    & $verifyScript -PackageDir $validPackage -SkipRuntimeProbe

    $missingLauncherPackage = Join-Path $tempRoot "missing-diagnostic-launcher"
    New-Item -ItemType Directory -Path $missingLauncherPackage -Force | Out-Null
    New-CompletePortablePackageFixture $missingLauncherPackage
    Remove-Item -LiteralPath (Join-Path $missingLauncherPackage $diagnosticLauncherName) -Force

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $missingLauncherOutput = & $powershell -NoProfile -ExecutionPolicy Bypass -File $verifyScript -PackageDir $missingLauncherPackage -SkipRuntimeProbe 2>&1
    $missingLauncherExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference
    if ($missingLauncherExitCode -eq 0) {
        throw "Expected portable verifier to reject package missing $diagnosticLauncherName."
    }
    if (($missingLauncherOutput -join "`n") -notmatch [regex]::Escape($diagnosticLauncherName)) {
        throw "Expected verifier output to mention $diagnosticLauncherName. Output: $($missingLauncherOutput -join ' ')"
    }

    $runtimeProbePackage = Join-Path $tempRoot "runtime-probe-required"
    New-Item -ItemType Directory -Path $runtimeProbePackage -Force | Out-Null
    New-CompletePortablePackageFixture $runtimeProbePackage
    Copy-Item -LiteralPath (Join-Path $env:SystemRoot "System32\where.exe") `
        -Destination (Join-Path $runtimeProbePackage "airplay_receiver.exe") -Force
    # This structural fixture models a successful inspector so the existing
    # recording-capability probe must still run. Real DLL loads are tested ON.
    Remove-Item -LiteralPath (Join-Path $runtimeProbePackage 'gst-inspect-1.0.exe') -Force
    Add-Type -TypeDefinition 'public class PortableInspectorFixture { public static int Main(string[] args) { System.Console.WriteLine("Filename " + args[0]); return 0; } }' -OutputAssembly (Join-Path $runtimeProbePackage 'gst-inspect-1.0.exe') -OutputType ConsoleApplication
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $runtimeProbeOutput = & $powershell -NoProfile -ExecutionPolicy Bypass -File $verifyScript -PackageDir $runtimeProbePackage 2>&1
    $runtimeProbeExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference
    if ($runtimeProbeExitCode -eq 0) {
        throw "Expected production verifier mode to execute the runtime probe."
    }
    if (($runtimeProbeOutput -join "`n") -notmatch "Portable recording runtime probe failed") {
        throw "Expected verifier output to report runtime probe failure. Output: $($runtimeProbeOutput -join ' ')"
    }

    $blockedPackage = Join-Path $tempRoot "blocked-d3dcompiler"
    New-Item -ItemType Directory -Path $blockedPackage -Force | Out-Null
    New-CompletePortablePackageFixture $blockedPackage
    New-RequiredPortableFile $blockedPackage "D3DCompiler_47.dll"

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

    # Literal dependency contract: do not derive these expectations from the manifest.
    foreach ($dependencyPath in @('libjson-glib-1.0-0.dll', 'gstreamer-plugins\libgstcodec2json.dll', 'gst-inspect-1.0.exe')) {
        $fixture = Join-Path $tempRoot ('missing-plugin-dependency-' + [IO.Path]::GetFileNameWithoutExtension($dependencyPath))
        New-Item -ItemType Directory -Path $fixture -Force | Out-Null
        New-CompletePortablePackageFixture $fixture
        Remove-Item -LiteralPath (Join-Path $fixture $dependencyPath) -Force -ErrorAction SilentlyContinue
        $ErrorActionPreference = 'Continue'
        $dependencyOutput = & $powershell -NoProfile -ExecutionPolicy Bypass -File $verifyScript -PackageDir $fixture -SkipRuntimeProbe 2>&1
        $dependencyExit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($dependencyExit -eq 0) { throw "Expected portable verifier to reject package missing $dependencyPath." }
        if (($dependencyOutput -join "`n") -notmatch [regex]::Escape([IO.Path]::GetFileName($dependencyPath))) {
            throw "Missing dependency failure must identify $dependencyPath. Output: $($dependencyOutput -join ' ')"
        }
    }
    $recordingFailureFixtures = @(
        "gstreamer-plugins\libgstmediafoundation.dll",
        "gstreamer-plugins\libgstopenh264.dll",
        "libopenh264-7.dll",
        "gstreamer-plugins\libgstlibav.dll",
        "gstreamer-plugins\libgstisomp4.dll",
        "gstreamer-plugins\libgstmatroska.dll"
    )
    foreach ($missingPath in $recordingFailureFixtures) {
        $fixtureName = "missing-" + ([System.IO.Path]::GetFileNameWithoutExtension($missingPath))
        $fixture = Join-Path $tempRoot $fixtureName
        New-Item -ItemType Directory -Path $fixture -Force | Out-Null
        New-CompletePortablePackageFixture $fixture
        Remove-Item -LiteralPath (Join-Path $fixture $missingPath) -Force

        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $output = & $powershell -NoProfile -ExecutionPolicy Bypass -File $verifyScript -PackageDir $fixture -SkipRuntimeProbe 2>&1
        $exitCode = $LASTEXITCODE
        $ErrorActionPreference = $previousErrorActionPreference
        if ($exitCode -eq 0) {
            throw "Expected portable verifier to reject package missing $missingPath."
        }
        $fileName = [System.IO.Path]::GetFileName($missingPath)
        if (($output -join "`n") -notmatch [regex]::Escape($fileName)) {
            throw "Expected verifier output to mention $fileName. Output: $($output -join ' ')"
        }
    }
} finally {
    if ($hadFixtureMode) {
        $env:AIRPLAY_PORTABLE_FIXTURE_TEST = $previousFixtureMode
    } else {
        Remove-Item Env:AIRPLAY_PORTABLE_FIXTURE_TEST -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
