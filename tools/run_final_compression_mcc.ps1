param(
    [Alias("Input")][Parameter(Mandatory = $true)][string]$Dataset,
    [ValidateRange(1, 99)][int]$TrainPercent = 20,
    [string]$OutputDir = "results/final-compression-mcc",
    [int]$ChunkMiB = 256,
    [string]$FpcExe = "fpc-bsel.v2/build/fpc-bsel-v2.exe",
    [string]$BselExe = "build/bsel.exe",
    [string]$AblationAlgorithm = "fpc-bsel-3k",
    [switch]$RoundTrip,
    [switch]$Resume
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$inputPath = [IO.Path]::GetFullPath((Join-Path $repo $Dataset))
$out = [IO.Path]::GetFullPath((Join-Path $repo $OutputDir))
$fpc = [IO.Path]::GetFullPath((Join-Path $repo $FpcExe))
$bsel = [IO.Path]::GetFullPath((Join-Path $repo $BselExe))
foreach ($path in @($inputPath, $fpc, $bsel)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing file: $path" }
}
New-Item -ItemType Directory -Force $out | Out-Null

[uint64]$totalBytes = (Get-Item -LiteralPath $inputPath).Length
if ($totalBytes % 256 -ne 0) { throw "dataset size must be a multiple of 256B" }
[uint64]$trainBytes = [uint64][Math]::Floor($totalBytes * ($TrainPercent / 100.0))
$trainBytes -= $trainBytes % 256
[uint64]$testOffset = $trainBytes
[uint64]$testBytes = $totalBytes - $trainBytes
if ($trainBytes -eq 0 -or $testBytes -eq 0) { throw "invalid train/test split" }

$fpcResults = Join-Path $out "fpc-models"
$fpcResultsArg = Join-Path $OutputDir "fpc-models"
$streamArgs = @(
    "-NoProfile", "-ExecutionPolicy", "Bypass",
    "-File", (Join-Path $repo "tools/run_fpc_20g_stream.ps1"),
    "-Input", $inputPath, "-TrainPercent", $TrainPercent,
    "-OutputDir", $fpcResultsArg, "-ChunkMiB", $ChunkMiB,
    "-FpcExe", $FpcExe
)
if ($RoundTrip) { $streamArgs += "-RoundTrip" }
if ($Resume) { $streamArgs += "-Resume" }
$powerShellHost = (Get-Process -Id $PID).Path
& $powerShellHost @streamArgs | Set-Content (Join-Path $out "fpc-training-evaluation.log")
if ($LASTEXITCODE -ne 0) { throw "FPC budget sweep failed" }

function Parse-Values([string]$line) {
    $values = @{}
    foreach ($part in ($line -split "\s+")) {
        if ($part -match '^([^=]+)=(.+)$') { $values[$matches[1]] = $matches[2] }
    }
    return $values
}

function New-Accumulator() {
    return @{
        original = [uint64]0; stored = [uint64]0; chunks = [uint64]0
        before = [uint64]0; v1 = [uint64]0; v2 = [uint64]0
        v3 = [uint64]0; v4 = [uint64]0; v5 = [uint64]0
        before_regions = [uint64]0; v1_regions = [uint64]0
        v2_regions = [uint64]0; v3_regions = [uint64]0
        v4_regions = [uint64]0; v5_regions = [uint64]0
        candidate_segments = [uint64]0; candidate_gaps = [uint64]0
    }
}

function Add-Mcc($acc, $lines) {
    $rows = @{}
    foreach ($line in $lines) {
        if ($line -match '^(mcc_sizes_(before|v1|v2|v3|v4|v5)) ') {
            $rows[$matches[1]] = Parse-Values $line
        }
    }
    foreach ($name in @("before", "v1", "v2", "v3", "v4", "v5")) {
        if (!$rows.ContainsKey("mcc_sizes_$name")) { throw "missing MCC row: $name" }
    }
    $before = $rows.mcc_sizes_before
    $acc.original += [uint64]$before.original_bytes
    $acc.stored += [uint64]$before.stored_bytes
    foreach ($name in @("before", "v1", "v2", "v3", "v4", "v5")) {
        $row = $rows["mcc_sizes_$name"]
        $acc[$name] += [uint64]$row.physical_bytes
        $acc["${name}_regions"] += [uint64]$row.metadata_regions
    }
    $acc.candidate_segments += [uint64]$rows.mcc_sizes_v5.candidate_segments_scanned
    $acc.candidate_gaps += [uint64]$rows.mcc_sizes_v5.candidate_gaps_scanned
    ++$acc.chunks
}

$algorithms = @(
    @{ Name = "fpc-bsel-4k"; Type = "fpc"; Model = "fpc-4k.model" },
    @{ Name = "fpc-bsel-3k"; Type = "fpc"; Model = "fpc-3k.model" },
    @{ Name = "fpc-bsel-2k"; Type = "fpc"; Model = "fpc-2k.model" },
    @{ Name = "fpc-bsel-1k"; Type = "fpc"; Model = "fpc-1k.model" },
    @{ Name = "fpc"; Type = "baseline"; Kind = "fpc" },
    @{ Name = "bdi"; Type = "baseline"; Kind = "bdi" },
    @{ Name = "hybrid"; Type = "baseline"; Kind = "hybrid" },
    @{ Name = "cpack"; Type = "baseline"; Kind = "cpack" },
    @{ Name = "bpc"; Type = "baseline"; Kind = "bpc" },
    @{ Name = "huffman"; Type = "baseline"; Kind = "huffman" }
)
$totals = @{}
foreach ($algorithm in $algorithms) { $totals[$algorithm.Name] = New-Accumulator }
$ablation = @{}
foreach ($key in @("g0-l0", "g1-l0", "g0-l1", "g1-l1", "g0-l2", "g1-l2")) {
    $ablation[$key] = New-Accumulator
}

$temp = Join-Path $out "chunk-work"
New-Item -ItemType Directory -Force $temp | Out-Null
$chunkBytes = [int64]$ChunkMiB * 1024 * 1024
$chunkBytes -= $chunkBytes % 256
$stream = [IO.File]::OpenRead($inputPath)
try {
    $stream.Seek([int64]$testOffset, [IO.SeekOrigin]::Begin) | Out-Null
    [uint64]$remaining = $testBytes
    $buffer = New-Object byte[] $chunkBytes
    while ($remaining -gt 0) {
        $request = [int][Math]::Min([uint64]$buffer.Length, $remaining)
        $read = $stream.Read($buffer, 0, $request)
        if ($read -le 0 -or $read % 256 -ne 0) { throw "invalid test chunk" }
        $remaining -= [uint64]$read
        $raw = Join-Path $temp "chunk.bin"
        $rawStream = [IO.File]::Open($raw, [IO.FileMode]::Create, [IO.FileAccess]::Write)
        try { $rawStream.Write($buffer, 0, $read) } finally { $rawStream.Dispose() }

        foreach ($algorithm in $algorithms) {
            if ($algorithm.Type -eq "fpc") {
                $model = Join-Path $fpcResults $algorithm.Model
                $sizes = Join-Path $temp "chunk.sizes.txt"
                $sizeArgs = @("sizes-256", $model, $raw, $sizes)
                if ($RoundTrip) { $sizeArgs += "--roundtrip" }
                & $fpc @sizeArgs | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "FPC 256B sizing failed: $($algorithm.Name)" }
                $mcc = & $bsel mcc-sizes $sizes 256 --guard-bytes 1 --region-lookback 1
            } else {
                $mcc = & $bsel mcc-baseline $algorithm.Kind $raw 256 `
                    --guard-bytes 1 --region-lookback 1
            }
            if ($LASTEXITCODE -ne 0) { throw "MCC failed: $($algorithm.Name)" }
            Add-Mcc $totals[$algorithm.Name] $mcc

            if ($algorithm.Name -eq $AblationAlgorithm) {
                foreach ($setting in @(
                    @{ Key="g0-l0"; G=0; L=0 }, @{ Key="g1-l0"; G=1; L=0 },
                    @{ Key="g0-l1"; G=0; L=1 }, @{ Key="g1-l1"; G=1; L=1 },
                    @{ Key="g0-l2"; G=0; L=2 }, @{ Key="g1-l2"; G=1; L=2 })) {
                    if ($algorithm.Type -eq "fpc") {
                        $a = & $bsel mcc-sizes $sizes 256 --guard-bytes $setting.G `
                            --region-lookback $setting.L
                    } else {
                        $a = & $bsel mcc-baseline $algorithm.Kind $raw 256 `
                            --guard-bytes $setting.G --region-lookback $setting.L
                    }
                    Add-Mcc $ablation[$setting.Key] $a
                }
            }
        }
    }
} finally {
    $stream.Dispose()
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}

$summary = Join-Path $out "final-summary.csv"
Set-Content $summary "algorithm,subline_bytes,original_bytes,stored_bytes,algorithm_ratio,before_ratio,v1_ratio,v2_ratio,v3_ratio,v4_ratio,v5_ratio,v5_physical_bytes,v5_metadata_regions,candidate_segments,candidate_gaps,chunks"
foreach ($algorithm in $algorithms) {
    $a = $totals[$algorithm.Name]
    $ratio = { param($bytes) if ($bytes -eq 0) { 0 } else { [double]$a.original / $bytes } }
    $row = $algorithm.Name,256,$a.original,$a.stored,(& $ratio $a.stored),`
        (& $ratio $a.before),(& $ratio $a.v1),(& $ratio $a.v2),`
        (& $ratio $a.v3),(& $ratio $a.v4),(& $ratio $a.v5),`
        $a.v5,$a.v5_regions,$a.candidate_segments,$a.candidate_gaps,$a.chunks
    Add-Content $summary ($row -join ",")
}

$ablationSummary = Join-Path $out "mcc-ablation-$AblationAlgorithm.csv"
Set-Content $ablationSummary "setting,guard,lookback,stored_bytes,physical_bytes,quantized_ratio,metadata_regions,candidate_segments,candidate_gaps,chunks"
foreach ($setting in @(
    @{ Key="g0-l0"; G=0; L=0 }, @{ Key="g1-l0"; G=1; L=0 },
    @{ Key="g0-l1"; G=0; L=1 }, @{ Key="g1-l1"; G=1; L=1 },
    @{ Key="g0-l2"; G=0; L=2 }, @{ Key="g1-l2"; G=1; L=2 })) {
    $a = $ablation[$setting.Key]
    $ratio = if ($a.v5 -eq 0) { 0 } else { [double]$a.original / $a.v5 }
    Add-Content $ablationSummary "$($setting.Key),$($setting.G),$($setting.L),$($a.stored),$($a.v5),$ratio,$($a.v5_regions),$($a.candidate_segments),$($a.candidate_gaps),$($a.chunks)"
}

Write-Output "summary=$summary"
Write-Output "ablation=$ablationSummary"
