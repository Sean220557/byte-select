param(
    [Alias("Input")][string]$Dataset = "",
    [string]$Train = "",
    [string]$Test = "",
    [ValidateRange(1, 99)][int]$TrainPercent = 20,
    [string]$OutputDir = "results/fpc-20g-stream",
    [int]$ChunkMiB = 256,
    [switch]$RoundTrip,
    [switch]$RunMcc,
    [switch]$Resume,
    [string]$FpcExe = "fpc-bsel.v2/build/fpc-bsel-v2.exe",
    [string]$BselExe = "build/bsel.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$out = [IO.Path]::GetFullPath((Join-Path $repo $OutputDir))
$fpc = [IO.Path]::GetFullPath((Join-Path $repo $FpcExe))
$bsel = [IO.Path]::GetFullPath((Join-Path $repo $BselExe))
New-Item -ItemType Directory -Force $out | Out-Null

[uint64]$trainOffset = 0; [uint64]$testOffset = 0
[uint64]$trainLength = 0; [uint64]$testLength = 0
if ($Dataset.Length -ne 0) {
    $inputPath = [IO.Path]::GetFullPath((Join-Path $repo $Dataset))
    if (!(Test-Path -LiteralPath $inputPath -PathType Leaf)) { throw "missing file: $inputPath" }
    [uint64]$totalBytes = (Get-Item -LiteralPath $inputPath).Length
    if ($totalBytes % 256 -ne 0) { throw "single input size must be a multiple of 256B" }
    $trainLength = [uint64][Math]::Floor($totalBytes * ($TrainPercent / 100.0))
    $trainLength -= $trainLength % 256
    if ($trainLength -eq 0 -or $trainLength -ge $totalBytes) { throw "invalid train/test split" }
    $testOffset = $trainLength; $testLength = $totalBytes - $trainLength
    $trainPath = $inputPath; $testPath = $inputPath
} else {
    if ($Train.Length -eq 0 -or $Test.Length -eq 0) {
        throw "provide either -Input or both -Train and -Test"
    }
    $trainPath = [IO.Path]::GetFullPath((Join-Path $repo $Train))
    $testPath = [IO.Path]::GetFullPath((Join-Path $repo $Test))
    $trainLength = (Get-Item -LiteralPath $trainPath).Length
    $testLength = (Get-Item -LiteralPath $testPath).Length
}

foreach ($path in @($trainPath, $testPath, $fpc)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing file: $path" }
}
if ($ChunkMiB -le 0) { throw "ChunkMiB must be positive" }
foreach ($path in @($trainPath, $testPath)) {
    if ((Get-Item -LiteralPath $path).Length % 64 -ne 0) {
        throw "input size must be a multiple of 64B: $path"
    }
}

function Parse-Values([string]$line) {
    $values = @{}
    foreach ($part in ($line -split "\s+")) {
        if ($part -match "^([^=]+)=(.+)$") { $values[$matches[1]] = $matches[2] }
    }
    return $values
}

$models = @(
    @{ Name = "4k"; Patterns = 256 },
    @{ Name = "3k"; Patterns = 192 },
    @{ Name = "2k"; Patterns = 128 },
    @{ Name = "1k"; Patterns = 64 }
)
foreach ($item in $models) {
    $model = Join-Path $out "fpc-$($item.Name).model"
    $budgetKiB = [int]($item.Patterns / 64)
    if (!$Resume -or !(Test-Path -LiteralPath $model)) {
        & $fpc train-budget-range $trainPath $model --budget-kib $budgetKiB `
            --offset-bytes $trainOffset --length-bytes $trainLength --chunk-mib $ChunkMiB |
            Set-Content (Join-Path $out "train-$($item.Name).log")
        if ($LASTEXITCODE -ne 0) { throw "budget stream training failed: $($item.Name)" }
    }
}

$summary = Join-Path $out "summary.csv"
Set-Content $summary "version,budget_kib,model_bytes,test_bytes,blocks,encoded_bytes,physical_encoded_bytes,ratio,raw_blocks,fpc_blocks,bitshuffle_fpc_blocks,fpc_bsel_blocks"
$evalCommand = if ($RoundTrip) { "roundtrip-stream" } else { "evaluate-stream" }
foreach ($item in $models) {
    $model = Join-Path $out "fpc-$($item.Name).model"
    $log = Join-Path $out "evaluate-$($item.Name).log"
    if (!$Resume -or !(Test-Path -LiteralPath $log)) {
        $rangeCommand = if ($RoundTrip) { "roundtrip-range" } else { "evaluate-range" }
        & $fpc $rangeCommand $model $testPath --offset-bytes $testOffset `
            --length-bytes $testLength --chunk-mib $ChunkMiB | Set-Content $log
        if ($LASTEXITCODE -ne 0) { throw "stream evaluation failed: $($item.Name)" }
    }
    $line = Get-Content -LiteralPath $log | Select-Object -First 1
    $v = Parse-Values $line
    $row = $item.Name,([int]($item.Patterns / 64)),(Get-Item $model).Length,$testLength,`
        $v.blocks,$v.encoded_bytes,$v.physical_encoded_bytes,$v.ratio,$v.raw_blocks,`
        $v.fpc_blocks,$v.bitshuffle_fpc_blocks,$v.fpc_bsel_blocks
    Add-Content $summary ($row -join ",")
}

if ($RunMcc) {
    if (!(Test-Path -LiteralPath $bsel -PathType Leaf)) { throw "missing MCC executable: $bsel" }
    $chunkBytes = [int64]$ChunkMiB * 1024 * 1024
    $chunkBytes -= $chunkBytes % 256
    $temp = Join-Path $out "mcc-temp"
    New-Item -ItemType Directory -Force $temp | Out-Null
    $mccSummary = Join-Path $out "mcc-v5-summary.csv"
    Set-Content $mccSummary "version,original_bytes,stored_bytes,physical_bytes,metadata_regions,quantized_ratio,chunks"
    try {
        foreach ($item in $models) {
            $model = Join-Path $out "fpc-$($item.Name).model"
            [uint64]$original = 0; [uint64]$stored = 0; [uint64]$physical = 0
            [uint64]$regions = 0; [uint64]$chunks = 0
            $stream = [IO.File]::OpenRead($testPath)
            try {
                $stream.Seek([int64]$testOffset, [IO.SeekOrigin]::Begin) | Out-Null
                [uint64]$remaining = $testLength
                $buffer = New-Object byte[] $chunkBytes
                while ($remaining -gt 0) {
                    $request = [int][Math]::Min([uint64]$buffer.Length, $remaining)
                    $read = $stream.Read($buffer, 0, $request)
                    if ($read -le 0) { throw "test range exceeds input size" }
                    $remaining -= [uint64]$read
                    if ($read % 256 -ne 0) { throw "MCC chunk is not a full 256B subline" }
                    $raw = Join-Path $temp "chunk.bin"
                    $container = Join-Path $temp "chunk.fpz"
                    $sizes = Join-Path $temp "chunk.sizes.txt"
                    [IO.File]::WriteAllBytes($raw, $buffer[0..($read - 1)])
                    & $fpc compress $model $raw $container | Out-Null
                    if ($LASTEXITCODE -ne 0) { throw "chunk compression failed" }
                    $bytes = [IO.File]::ReadAllBytes($container)
                    $offset = 16; $sum = 0; $count = 0
                    $lines = [Collections.Generic.List[string]]::new()
                    while ($offset -lt $bytes.Length) {
                        $size = [int]$bytes[$offset] -bor ([int]$bytes[$offset + 1] -shl 8)
                        $offset += 2 + $size; $sum += $size; ++$count
                        if ($count -eq 4) { $lines.Add([string][Math]::Min($sum, 256)); $sum = 0; $count = 0 }
                    }
                    [IO.File]::WriteAllLines($sizes, $lines)
                    $mcc = & $bsel mcc-sizes $sizes 256
                    if ($LASTEXITCODE -ne 0) { throw "chunk MCC failed" }
                    $v5Lines = @($mcc | Where-Object { $_ -match '^mcc_sizes_v5 ' })
                    if ($v5Lines.Count -ne 1) { throw "missing MCC v5 summary" }
                    $v5 = Parse-Values $v5Lines[0]
                    $original += [uint64]$v5.original_bytes; $stored += [uint64]$v5.stored_bytes
                    $physical += [uint64]$v5.physical_bytes; $regions += [uint64]$v5.metadata_regions
                    ++$chunks
                }
            } finally { $stream.Dispose() }
            $ratio = if ($physical -eq 0) { 0 } else { [double]$original / $physical }
            Add-Content $mccSummary "$($item.Name),$original,$stored,$physical,$regions,$ratio,$chunks"
        }
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
    }
}

Write-Output "summary=$summary"
