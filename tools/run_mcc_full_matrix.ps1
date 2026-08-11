param(
    [string]$Trace = "results\spark-flink\full-rerun-v2\spark-kmeans-large.restored.trace",
    [string]$OutputDir = "results\mcc\full-matrix",
    [int]$SublineSize = 256,
    [int]$MaxPatterns = 256
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$bsel = Join-Path $repo "build\bsel.exe"
$out = Join-Path $repo $OutputDir
New-Item -ItemType Directory -Force $out | Out-Null

function Parse-Values($line) {
    $map = @{}
    foreach ($part in ($line -split "\s+")) {
        if ($part -match "^([^=]+)=(.+)$") {
            $map[$matches[1]] = $matches[2]
        }
    }
    return $map
}

function Add-SummaryRow($path, $algorithm, $version, $mccOutput) {
    $rows = @{}
    foreach ($line in ($mccOutput -split "`r?`n")) {
        if ($line.Trim().Length -eq 0) { continue }
        $parts = $line -split "\s+", 2
        $rows[$parts[0]] = Parse-Values $line
    }
    $before = $rows["mcc_sizes_before"]
    $v1 = $rows["mcc_sizes_v1"]
    $v2 = $rows["mcc_sizes_v2"]
    $v3 = $rows["mcc_sizes_v3"]
    $csv = "{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17},{18}" -f `
        $algorithm,$version,$before.blocks,$before.original_bytes,$before.stored_bytes,`
        $before.physical_bytes,$before.quantized_ratio,$v1.physical_bytes,`
        $v1.quantized_ratio,$v1.metadata_regions,$v2.physical_bytes,`
        $v2.quantized_ratio,$v2.metadata_regions,$v3.physical_bytes,`
        $v3.quantized_ratio,$v3.metadata_regions,`
        ([int64]$before.physical_bytes - [int64]$v1.physical_bytes),`
        ([int64]$before.physical_bytes - [int64]$v2.physical_bytes),`
        ([int64]$before.physical_bytes - [int64]$v3.physical_bytes)
    Add-Content $path $csv
}

function Add-CompareRows($path, $mccCompareOutput, $originalBytes, $blocks) {
    $pending = @{}
    foreach ($line in ($mccCompareOutput -split "`r?`n")) {
        if ($line -notmatch "^mcc_compare ") { continue }
        $v = Parse-Values $line
        $name = $v.algorithm
        $isV2 = $name.EndsWith("_v2")
        $isV3 = $name.EndsWith("_v3")
        $base = if ($isV2 -or $isV3) { $name.Substring(0, $name.Length - 3) } else { $name }
        if (!$pending.ContainsKey($base)) { $pending[$base] = @{} }
        if ($isV2) { $pending[$base].v2 = $v }
        elseif ($isV3) { $pending[$base].v3 = $v }
        else { $pending[$base].v1 = $v }
    }
    foreach ($name in ($pending.Keys | Sort-Object)) {
        $pair = $pending[$name]
        if (!$pair.v1 -or !$pair.v2 -or !$pair.v3) { continue }
        $v1 = $pair.v1
        $v2 = $pair.v2
        $v3 = $pair.v3
        $csv = "{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17},{18}" -f `
            $name,"builtin",$blocks,$originalBytes,"",$v1.before_physical_bytes,`
            $v1.before_quantized_ratio,$v1.after_physical_bytes,`
            $v1.after_quantized_ratio,$v1.after_metadata_regions,`
            $v2.after_physical_bytes,$v2.after_quantized_ratio,`
            $v2.after_metadata_regions,$v3.after_physical_bytes,`
            $v3.after_quantized_ratio,$v3.after_metadata_regions,`
            $v1.saved_physical_bytes,$v2.saved_physical_bytes,$v3.saved_physical_bytes
        Add-Content $path $csv
    }
}

function Export-ContainerSublineSizes($container, $sizesPath, $sublineSize) {
    $bytes = [IO.File]::ReadAllBytes($container)
    $magic = [Text.Encoding]::ASCII.GetString($bytes, 0, 7)
    if ($magic -ne "FPCBSF1") { throw "unsupported container: $container" }
    $offset = 16
    $perSubline = [Math]::Max(1, [int]($sublineSize / 64))
    $current = 0
    $count = 0
    $sizes = New-Object System.Collections.Generic.List[string]
    while ($offset -lt $bytes.Length) {
        if ($offset + 2 -gt $bytes.Length) { throw "truncated size in $container" }
        $size = [int]$bytes[$offset] -bor ([int]$bytes[$offset + 1] -shl 8)
        $offset += 2
        if ($offset + $size -gt $bytes.Length) { throw "truncated block in $container" }
        $offset += $size
        $current += $size
        $count += 1
        if ($count -eq $perSubline) {
            $sizes.Add([string][Math]::Min($current, $sublineSize))
            $current = 0
            $count = 0
        }
    }
    if ($count -ne 0) { throw "container block count is not a full subline multiple: $container" }
    Set-Content -Path $sizesPath -Value $sizes
}

function Run-Standalone($family, $version, $dir, $exeName, $summaryPath) {
    $exe = Join-Path $repo "$dir\build\$exeName"
    if (!(Test-Path $exe)) { throw "missing executable: $exe" }
    $name = "$family-$version"
    $model = Join-Path $out "$name.model"
    $container = Join-Path $out "$name.container"
    $sizes = Join-Path $out "$name.sizes.txt"
    & $exe train (Join-Path $repo $Trace) $model --max-patterns $MaxPatterns |
        Set-Content (Join-Path $out "$name.train.log")
    if ($LASTEXITCODE -ne 0) { throw "$name train failed" }
    & $exe compress $model (Join-Path $repo $Trace) $container |
        Set-Content (Join-Path $out "$name.compress.log")
    if ($LASTEXITCODE -ne 0) { throw "$name compress failed" }
    Export-ContainerSublineSizes $container $sizes $SublineSize
    $mcc = & $bsel mcc-sizes $sizes $SublineSize
    if ($LASTEXITCODE -ne 0) { throw "$name mcc failed" }
    $mcc | Set-Content (Join-Path $out "$name.mcc.log")
    Add-SummaryRow $summaryPath $family $version ($mcc -join "`n")
}

$summary = Join-Path $out "summary.csv"
Set-Content $summary "algorithm,version,blocks,original_bytes,stored_bytes,before_physical_bytes,before_quantized_ratio,v1_physical_bytes,v1_quantized_ratio,v1_metadata_regions,v2_physical_bytes,v2_quantized_ratio,v2_metadata_regions,v3_physical_bytes,v3_quantized_ratio,v3_metadata_regions,v1_saved_physical_bytes,v2_saved_physical_bytes,v3_saved_physical_bytes"

# Built-in baselines and BSel.
$model = Join-Path $out "byte-select.model"
& $bsel train (Join-Path $repo $Trace) $model --block-size $SublineSize --threshold 4 --target 128:256:1 |
    Set-Content (Join-Path $out "byte-select.train.log")
& $bsel mcc-compare $model (Join-Path $repo $Trace) |
    Set-Content (Join-Path $out "builtin-baselines.mcc-compare.log")
$builtinCompare = Get-Content (Join-Path $out "builtin-baselines.mcc-compare.log") -Raw
$traceBytes = (Get-Item (Join-Path $repo $Trace)).Length
Add-CompareRows $summary $builtinCompare $traceBytes ([int]($traceBytes / $SublineSize))

Run-Standalone "fpc-bsel" "original" "fpc-bsel" "fpc-bsel.exe" $summary
Run-Standalone "fpc-bsel" "v1" "fpc-bsel.v1" "fpc-bsel-v1.exe" $summary
Run-Standalone "fpc-bsel" "v2" "fpc-bsel.v2" "fpc-bsel-v2.exe" $summary
Run-Standalone "cpack-bsel" "original" "cpack-bsel" "cpack-bsel.exe" $summary
Run-Standalone "cpack-bsel" "v1" "cpack-bsel.v1" "cpack-bsel-v1.exe" $summary
Run-Standalone "cpack-bsel" "v2" "cpack-bsel.v2" "cpack-bsel-v2.exe" $summary

Write-Output "summary=$summary"
