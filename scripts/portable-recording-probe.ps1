function Invoke-PortableRecordingProbe {
    param(
        [System.Diagnostics.ProcessStartInfo]$StartInfo,
        [ValidateRange(1, 2147483647)][int]$TimeoutMilliseconds = 30000,
        [ValidateRange(1, 2147483647)][int]$CleanupMilliseconds = 5000
    )
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $StartInfo
    $started = $false
    $streams = @()
    $failure = $null
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        try { $started = $process.Start() } catch {
            throw "Portable recording runtime probe could not be started: $($StartInfo.FileName): $($_.Exception.Message)"
        }
        if (-not $started) { throw "Portable recording runtime probe could not be started: $($StartInfo.FileName)" }
        foreach ($reader in @($process.StandardOutput, $process.StandardError)) {
            $buffer = New-Object char[] 4096
            $streams += @{ Reader = $reader; Buffer = $buffer; Text = (New-Object System.Text.StringBuilder); Task = $reader.ReadAsync($buffer, 0, $buffer.Length); Done = $false }
        }
        # Poll both pipes under the same deadline; a descendant can retain a pipe
        # after the process exits. Never wait synchronously for an unfinished read.
        while ($true) {
            foreach ($stream in $streams) {
                if (-not $stream.Done -and $stream.Task.IsCompleted) {
                    if ($stream.Task.IsFaulted -or $stream.Task.IsCanceled) {
                        $failure = 'Portable recording runtime probe output read failed.'
                        $stream.Done = $true
                    } else {
                        $count = $stream.Task.Result
                        if ($count -eq 0) { $stream.Done = $true } else {
                            [void]$stream.Text.Append($stream.Buffer, 0, $count)
                            $stream.Task = $stream.Reader.ReadAsync($stream.Buffer, 0, $stream.Buffer.Length)
                        }
                    }
                }
            }
            if ($failure) { break }
            if ($process.HasExited -and $streams[0].Done -and $streams[1].Done) { break }
            if ($clock.ElapsedMilliseconds -ge $TimeoutMilliseconds) {
                if ($process.HasExited) {
                    $failure = "Portable recording runtime probe output did not complete within $TimeoutMilliseconds ms."
                } else {
                    $failure = "Portable recording runtime probe timed out after $TimeoutMilliseconds ms."
                }
                break
            }
            Start-Sleep -Milliseconds 10
        }
        $output = @($streams | ForEach-Object { $_.Text.ToString() -split '\r?\n' | Where-Object { $_ } })
        if ($failure) { throw "$failure Available output: $($output -join ' ')" }
        if ($process.ExitCode -ne 0) {
            throw "Portable recording runtime probe failed with exit code $($process.ExitCode): $($output -join ' ')"
        }
        $output
    } finally {
        if ($started) {
            try {
                if (-not $process.HasExited) {
                    # Kill only this invocation's process, never its descendants or neighbors.
                    $process.Kill()
                    if (-not $process.WaitForExit($CleanupMilliseconds)) {
                        Write-Warning "Portable recording runtime probe cleanup did not complete within $CleanupMilliseconds ms."
                    }
                }
            } catch { Write-Warning "Portable recording runtime probe cleanup failed: $($_.Exception.Message)" }
        }
        foreach ($stream in $streams) { $stream.Reader.Dispose() }
        $process.Dispose()
    }
}
