# Persistent protocol shared with CacheStorage::tryRegistryWriteLock.
# Releasing a lease closes the handle; the known protocol file remains in place.
function Enter-AirPlayRegistryWriteLock([string]$PackageDirectory) {
    $package = [IO.Path]::GetFullPath($PackageDirectory)
    $directory = Join-Path $package 'gstreamer-1.0'
    $path = Join-Path $directory '.registry-startup.lock'
    $stream = $null
    try {
        foreach ($boundary in @($package,$directory,$path)) {
            $cursor = [IO.Path]::GetFullPath($boundary)
            while ($cursor) {
                if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Reparse boundary refused' }
                $cursor = [IO.Path]::GetDirectoryName($cursor)
            }
        }
        if (!(Test-Path -LiteralPath $package -PathType Container)) { throw 'Package directory unavailable' }
        if (!(Test-Path -LiteralPath $directory)) { [void][IO.Directory]::CreateDirectory($directory) }
        if (!(Test-Path -LiteralPath $directory -PathType Container)) { throw 'Registry directory unavailable' }
        $created = $false
        try {
            $stream = [IO.FileStream]::new($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
            $created = $true
        } catch [IO.IOException] {
            $nativeCode = $_.Exception.HResult -band 0xffff
            if ($nativeCode -notin @(80,183)) { throw }
            $stream = [IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
        }
        $protocol = [Text.Encoding]::ASCII.GetBytes("AIRPLAY-GSTREAMER-REGISTRY-LOCK/1`n")
        if ($created) {
            $stream.Write($protocol,0,$protocol.Length)
            $stream.Flush($true)
        } else {
            if ($stream.Length -ne $protocol.Length) { throw 'Unknown protocol content' }
            $bytes = New-Object byte[] $protocol.Length
            $count = $stream.Read($bytes,0,$bytes.Length)
            if ($count -ne $protocol.Length -or [Convert]::ToBase64String($bytes) -cne [Convert]::ToBase64String($protocol)) { throw 'Unknown protocol content' }
        }
        return $stream
    } catch {
        if ($stream) { $stream.Dispose() }
        throw "Registry write lock refused: $($_.Exception.Message)"
    }
}
function Exit-AirPlayRegistryWriteLock([IO.FileStream]$Lease) {
    if ($Lease) { $Lease.Dispose() }
}
