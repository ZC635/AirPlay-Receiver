# Focused PE dependency primitives shared by OFF isolation and ON DNS-SD acceptance.
function Require-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required file is missing: $Path" }
    return (Get-Item -LiteralPath $Path).FullName
}

function Get-PeDependencies([string[]]$Roots, [string[]]$SearchDirectories, [string]$Objdump,
                            [scriptblock]$RejectImport, $Audit) {
    $systemDirectory = [Environment]::SystemDirectory
    $windowsDirectory = Split-Path -Parent $systemDirectory
    $resolved = [System.Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    $queue = [System.Collections.Generic.Queue[string]]::new()
    foreach ($root in $Roots) { $queue.Enqueue((Require-File $root)) }
    while ($queue.Count -gt 0) {
        $file = Require-File $queue.Dequeue()
        if ($resolved.ContainsKey($file)) { continue }
        $resolved.Add($file, $file)
        $output = @(& $Objdump -p $file 2>&1)
        if ($LASTEXITCODE -ne 0) { throw "objdump failed for $file`: $($output -join ' ')" }
        $imports = @($output | ForEach-Object { if ($_ -match '^\s*DLL Name:\s*(\S+)\s*$') { $Matches[1] } })
        if ($imports.Count -eq 0) { throw "No PE imports parsed from $file" }
        foreach ($name in $imports) {
            & $RejectImport $name $file
            if ($name -match '^(?:api|ext)-ms-.*\.dll$') {
                $Audit.Add("$file -> $name [Windows API-set]")
                continue
            }
            $candidate = $null
            foreach ($directory in @((Split-Path -Parent $file)) + $SearchDirectories[0] + @($systemDirectory, $windowsDirectory) + $SearchDirectories) {
                $path = Join-Path $directory $name
                if (Test-Path -LiteralPath $path -PathType Leaf) {
                    $candidate = (Get-Item -LiteralPath $path).FullName
                    break
                }
            }
            if (-not $candidate) { throw "Unresolved import: $name (imported by $file)" }
            $isSystem = $candidate.StartsWith($systemDirectory + '\', [StringComparison]::OrdinalIgnoreCase) -or
                        (Split-Path -Parent $candidate) -eq $windowsDirectory
            $Audit.Add("$file -> $candidate [system=$isSystem]")
            if (-not $isSystem) { $queue.Enqueue($candidate) }
        }
    }
    return ,$resolved
}

function Copy-PeDependencies($Resolved, [string]$Directory, [string[]]$ExcludedFiles = @()) {
    $copiedNames = [System.Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $Resolved.Values) {
        if ([IO.Path]::GetExtension($file) -ine '.dll' -or $ExcludedFiles -contains $file) { continue }
        $name = Split-Path -Leaf $file
        if ($copiedNames.ContainsKey($name) -and $copiedNames[$name] -ne $file) { throw "Conflicting isolated DLL paths for $name" }
        $copiedNames[$name] = $file
        Copy-Item -LiteralPath $file -Destination (Join-Path $Directory $name)
    }
}
