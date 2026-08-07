param(
    [string]$BuildDir = "build-release",
    [string]$TraceDir = "datasets/spark-flink/traces",
    [string]$OutputDir = "results/spark-flink/phases",
    [int]$Threshold = 4
)

$ErrorActionPreference = "Stop"
$bsel = Join-Path $BuildDir "bsel.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$workloads = @("spark-kmeans", "flink-state-machine")
$traceNames = @{ "spark-kmeans" = "spark-kmeans-smoke"; "flink-state-machine" = "flink-state-machine-smoke" }
$presets = @("bsel-256", "bsel-4096", "bsel-1024-1024-128")
$rows = [Collections.Generic.List[object]]::new()

function Parse-Evaluation([string[]]$output) {
    $line = $output | Where-Object { $_ -like "blocks=*" } | Select-Object -First 1
    $v = @{}
    foreach ($m in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
        $v[$m.Groups[1].Value] = $m.Groups[2].Value
    }
    return $v
}

foreach ($workload in $workloads) {
    foreach ($phase in 1..4) {
        $prefix = "$($traceNames[$workload])-phase$phase"
        $train = Join-Path $TraceDir "$prefix-train.trace"
        $test = Join-Path $TraceDir "$prefix-test.trace"
        foreach ($preset in $presets) {
            $model = Join-Path $OutputDir "$workload-phase$phase-$preset.model"
            $timer = [Diagnostics.Stopwatch]::StartNew()
            & $bsel train $train $model --block-size 64 --threshold $Threshold `
                --paper-config $preset | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Training failed: $workload phase $phase $preset" }
            $timer.Stop()
            $local = Parse-Evaluation (& $bsel evaluate $model $test)
            $globalModel = "results/spark-flink/$workload-$preset.model"
            $global = Parse-Evaluation (& $bsel evaluate $globalModel $test)
            $rows.Add([pscustomobject][ordered]@{
                workload = $workload
                phase = $phase
                bsel_preset = $preset
                local_compressed_block_percent = [math]::Round([double]$local.compressed_fraction * 100, 4)
                local_compression_ratio = [double]$local.unquantized_ratio
                local_quantized_ratio = [double]$local.quantized_ratio
                global_compressed_block_percent = [math]::Round([double]$global.compressed_fraction * 100, 4)
                global_compression_ratio = [double]$global.unquantized_ratio
                global_quantized_ratio = [double]$global.quantized_ratio
                training_seconds = [math]::Round($timer.Elapsed.TotalSeconds, 3)
                threshold = $Threshold
                train_blocks = 4500
                test_blocks = 500
            })
        }
    }
}

$rows | Export-Csv -NoTypeInformation -Encoding utf8 `
    (Join-Path $OutputDir "spark_flink_phase_training.csv")
