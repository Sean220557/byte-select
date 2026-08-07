param(
    [string]$BuildDir = "build-release",
    [string]$TraceDir = "datasets/spark-flink/traces",
    [string]$OutputDir = "results/spark-flink",
    [int]$Threshold = 1
)

$ErrorActionPreference = "Stop"
$bsel = Join-Path $BuildDir "bsel.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$workloads = @(
    @{ Name = "spark-kmeans"; Train = "spark-kmeans-smoke-train.trace"; Test = "spark-kmeans-smoke-test.trace" },
    @{ Name = "flink-state-machine"; Train = "flink-state-machine-smoke-train.trace"; Test = "flink-state-machine-smoke-test.trace" }
)
$presets = @("bsel-256", "bsel-4096", "bsel-1024-1024-128")
$baselines = @("fpc", "bdi", "hybrid", "cpack", "bpc", "huffman")
$rows = [System.Collections.Generic.List[object]]::new()

foreach ($workload in $workloads) {
    foreach ($preset in $presets) {
        $stem = "$($workload.Name)-$preset"
        $model = Join-Path $OutputDir "$stem.model"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        & $bsel train (Join-Path $TraceDir $workload.Train) $model `
            --block-size 64 --threshold $Threshold --paper-config $preset | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Training failed: $stem" }
        $timer.Stop()

        $args = @("compare", $model, (Join-Path $TraceDir $workload.Test))
        foreach ($baseline in $baselines) { $args += @("--baseline", $baseline) }
        $output = & $bsel @args
        if ($LASTEXITCODE -ne 0) { throw "Comparison failed: $stem" }
        $output | Set-Content (Join-Path $OutputDir "$stem-compare.log")
        $bselLine = $output | Where-Object { $_ -like "blocks=*" } | Select-Object -First 1
        $lines = @("algorithm=bsel $bselLine") + @($output | Where-Object { $_ -like "baseline algorithm=*" })

        foreach ($line in $lines) {
            $v = @{}
            foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
                $v[$match.Groups[1].Value] = $match.Groups[2].Value
            }
            $rawRatio = [double]$v.original_bytes / [double]$v.encoded_bytes
            $quantizedRatio = [double]$v.original_bytes / [double]$v.allocated_bytes
            $rows.Add([pscustomobject][ordered]@{
                workload = $workload.Name
                bsel_preset = $preset
                algorithm = $v.algorithm
                compressed_block_percent = [math]::Round([double]$v.compressed_fraction * 100, 4)
                compression_ratio = [math]::Round($rawRatio, 6)
                quantized_compression_ratio = [math]::Round($quantizedRatio, 6)
                raw_space_saving_percent = [math]::Round((1 - 1 / $rawRatio) * 100, 4)
                quantized_space_saving_percent = [math]::Round((1 - 1 / $quantizedRatio) * 100, 4)
                training_seconds = if ($v.algorithm -eq "bsel") { [math]::Round($timer.Elapsed.TotalSeconds, 3) } else { $null }
                train_blocks = 18000
                test_blocks = [uint64]$v.blocks
                threshold = $Threshold
            })
        }
    }
}

$rows | Export-Csv -NoTypeInformation -Encoding utf8 `
    (Join-Path $OutputDir "spark_flink_bsel_results.csv")
