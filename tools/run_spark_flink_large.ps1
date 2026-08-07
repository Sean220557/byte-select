$ErrorActionPreference = "Stop"
$bsel = ".\build-release\bsel.exe"
$traceDir = "datasets/spark-flink/traces"
$outputDir = "results/spark-flink/large"
$threshold = 16
New-Item -ItemType Directory -Force $outputDir | Out-Null
$workloads = @(
    @{ Name = "spark-kmeans-large"; Source = "spark-kmeans-large.trace" },
    @{ Name = "flink-state-machine-large"; Source = "flink-state-machine-large.trace" }
)
$presets = @("bsel-256", "bsel-4096", "bsel-1024-1024-128")
$baselines = @("fpc", "bdi", "hybrid", "cpack", "bpc", "huffman")
$rows = [Collections.Generic.List[object]]::new()

foreach ($workload in $workloads) {
    $source = [IO.File]::ReadAllBytes((Resolve-Path "$traceDir/$($workload.Source)"))
    $blocks = [math]::Floor($source.Length / 64)
    $testBlocks = [math]::Floor($blocks / 10)
    $testBegin = [math]::Floor(($blocks - $testBlocks) / 2)
    $testBytes = $testBlocks * 64
    $beginBytes = $testBegin * 64
    $train = New-Object byte[] ($source.Length - $testBytes)
    [Array]::Copy($source, 0, $train, 0, $beginBytes)
    [Array]::Copy($source, $beginBytes + $testBytes, $train, $beginBytes,
        $source.Length - $beginBytes - $testBytes)
    $test = New-Object byte[] $testBytes
    [Array]::Copy($source, $beginBytes, $test, 0, $testBytes)
    $trainPath = "$traceDir/$($workload.Name)-train.trace"
    $testPath = "$traceDir/$($workload.Name)-test.trace"
    [IO.File]::WriteAllBytes((Join-Path (Resolve-Path $traceDir) "$($workload.Name)-train.trace"), $train)
    [IO.File]::WriteAllBytes((Join-Path (Resolve-Path $traceDir) "$($workload.Name)-test.trace"), $test)

    foreach ($preset in $presets) {
        $model = "$outputDir/$($workload.Name)-$preset.model"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        & $bsel train $trainPath $model --block-size 64 --threshold $threshold `
            --paper-config $preset | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Training failed: $($workload.Name) $preset" }
        $timer.Stop()
        $arguments = @("compare", $model, $testPath)
        foreach ($baseline in $baselines) { $arguments += @("--baseline", $baseline) }
        $output = & $bsel @arguments
        $bselLine = $output | Where-Object { $_ -like "blocks=*" } | Select-Object -First 1
        $lines = @("algorithm=bsel $bselLine") + @(
            $output | Where-Object { $_ -like "baseline algorithm=*" })
        foreach ($line in $lines) {
            $v = @{}
            foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
                $v[$match.Groups[1].Value] = $match.Groups[2].Value
            }
            $raw = [double]$v.original_bytes / [double]$v.encoded_bytes
            $quantized = [double]$v.original_bytes / [double]$v.allocated_bytes
            $rows.Add([pscustomobject][ordered]@{
                workload = $workload.Name; bsel_preset = $preset; algorithm = $v.algorithm
                compressed_block_percent = [math]::Round([double]$v.compressed_fraction * 100, 4)
                compression_ratio = [math]::Round($raw, 6)
                quantized_compression_ratio = [math]::Round($quantized, 6)
                raw_space_saving_percent = [math]::Round((1 - 1 / $raw) * 100, 4)
                quantized_space_saving_percent = [math]::Round((1 - 1 / $quantized) * 100, 4)
                training_seconds = if ($v.algorithm -eq "bsel") { [math]::Round($timer.Elapsed.TotalSeconds, 3) } else { $null }
                train_blocks = $blocks - $testBlocks; test_blocks = [uint64]$v.blocks; threshold = $threshold
            })
        }
    }
}
$rows | Export-Csv -NoTypeInformation -Encoding utf8 "$outputDir/spark_flink_large_results.csv"
