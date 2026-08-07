param(
    [string]$BuildDir = "build-release",
    [string]$TraceDir = "datasets/spark-flink/traces",
    [string]$OutputDir = "results/spark-flink/threshold-tuning"
)

$ErrorActionPreference = "Stop"
$bsel = Join-Path $BuildDir "bsel.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
# Threshold 1 is deliberately excluded: on these JVM traces it admits nearly
# every one-off atom and does not complete the maximal-pattern phase in a
# practical tuning budget. Threshold 2 is the lowest tractable candidate.
$thresholds = @(2, 4, 8, 16, 32, 64)
$workloads = @("spark-kmeans", "flink-state-machine")
$traceNames = @{ "spark-kmeans" = "spark-kmeans-smoke"; "flink-state-machine" = "flink-state-machine-smoke" }
$sweep = [Collections.Generic.List[object]]::new()
$final = [Collections.Generic.List[object]]::new()

function Parse-Line([string[]]$output, [string]$prefix) {
    $line = $output | Where-Object { $_ -like "$prefix*" } | Select-Object -First 1
    $v = @{}
    foreach ($m in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
        $v[$m.Groups[1].Value] = $m.Groups[2].Value
    }
    return $v
}

foreach ($workload in $workloads) {
    foreach ($phase in 1..4) {
        $prefix = "$($traceNames[$workload])-phase$phase"
        $train = Join-Path $TraceDir "$prefix-tune-train.trace"
        $validation = Join-Path $TraceDir "$prefix-validation.trace"
        $test = Join-Path $TraceDir "$prefix-test.trace"
        $bestThreshold = 0
        $bestRatio = -1.0
        foreach ($threshold in $thresholds) {
            $model = Join-Path $OutputDir "$workload-phase$phase-t$threshold.model"
            $timer = [Diagnostics.Stopwatch]::StartNew()
            & $bsel train $train $model --block-size 64 --threshold $threshold `
                --paper-config bsel-1024-1024-128 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Training failed: $workload phase $phase threshold $threshold" }
            $timer.Stop()
            $v = Parse-Line (& $bsel evaluate $model $validation) "blocks="
            $ratio = [double]$v.quantized_ratio
            $sweep.Add([pscustomobject]@{
                workload = $workload; phase = $phase; threshold = $threshold
                validation_compressed_block_percent = [math]::Round([double]$v.compressed_fraction * 100, 4)
                validation_compression_ratio = [double]$v.unquantized_ratio
                validation_quantized_ratio = $ratio
                training_seconds = [math]::Round($timer.Elapsed.TotalSeconds, 3)
            })
            if ($ratio -gt $bestRatio) { $bestRatio = $ratio; $bestThreshold = $threshold }
        }

        $finalModel = Join-Path $OutputDir "$workload-phase$phase-best.model"
        & $bsel train (Join-Path $TraceDir "$prefix-train.trace") $finalModel `
            --block-size 64 --threshold $bestThreshold `
            --paper-config bsel-1024-1024-128 | Out-Null
        $output = & $bsel compare $finalModel $test --baseline fpc
        $bselResult = Parse-Line $output "blocks="
        $fpcResult = Parse-Line $output "baseline algorithm=fpc"
        $final.Add([pscustomobject]@{
            workload = $workload; phase = $phase; selected_threshold = $bestThreshold
            bsel_compressed_block_percent = [math]::Round([double]$bselResult.compressed_fraction * 100, 4)
            bsel_compression_ratio = [double]$bselResult.unquantized_ratio
            bsel_quantized_ratio = [double]$bselResult.quantized_ratio
            fpc_compressed_block_percent = [math]::Round([double]$fpcResult.compressed_fraction * 100, 4)
            fpc_compression_ratio = [double]$fpcResult.unquantized_ratio
            fpc_quantized_ratio = [double]$fpcResult.quantized_ratio
        })
    }
}

$sweep | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $OutputDir "threshold_sweep.csv")
$final | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $OutputDir "threshold_selected_test.csv")
