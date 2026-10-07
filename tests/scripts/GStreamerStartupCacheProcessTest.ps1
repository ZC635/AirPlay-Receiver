param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$ProbeExecutable,
    [Parameter(Mandatory=$true)][string]$NonPluginDll,
    [Parameter(Mandatory=$true)][string]$Objdump,
    [Parameter(Mandatory=$true)][string]$DependencyBinDirectory,
    [Parameter(Mandatory=$true)][string]$QtPluginDirectory,
    [Parameter(Mandatory=$true)][string]$GStreamerPluginDirectory,
    [Parameter(Mandatory=$true)][string]$ScannerExecutable,
    [Parameter(Mandatory=$true)][string]$ReportDirectory,
    [string]$RuntimeDirectory = '',
    [string]$CacheCase = ''
)
& (Join-Path $PSScriptRoot 'GStreamerPluginReadinessProcessTest.ps1') @PSBoundParameters -Mode CacheAcceptance
exit $LASTEXITCODE
