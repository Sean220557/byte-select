param(
    [string]$BuildDir = "build-release",
    [string]$DataDir = "datasets/ann-benchmarks/benchmark-input",
    [string]$ShuffledDataDir = "datasets/ann-benchmarks/benchmark-input-byte-shuffle",
    [string]$OutputDir = "results/ann_lossless_corrected",
    [int]$Threshold = 16
)

$ErrorActionPreference = "Stop"
$bsel = Join-Path $BuildDir "bsel.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null

$datasets = @(
    @{ Name = "deep-image-96-angular"; Train = "deep-image-train-sample.bin"; Test = "deep-image-test.bin" },
    @{ Name = "fashion-mnist-784-euclidean"; Train = "fashion-train-sample.bin"; Test = "fashion-test.bin" },
    @{ Name = "gist-960-euclidean"; Train = "gist-train-sample.bin"; Test = "gist-test.bin" },
    @{ Name = "glove-100-angular"; Train = "glove-train-sample.bin"; Test = "glove-test.bin" },
    @{ Name = "sift-128-euclidean"; Train = "sift-train-sample.bin"; Test = "sift-test.bin" }
)
$presets = @("bsel-256", "bsel-4096", "bsel-1024-1024-128")
$baselines = @("fpc", "bdi", "hybrid", "cpack", "bpc", "huffman")
$rows = [System.Collections.Generic.List[object]]::new()

foreach ($layout in @(
    @{ Name = "raw-float32"; Directory = $DataDir },
    @{ Name = "byte-shuffle-64B"; Directory = $ShuffledDataDir }
)) {
foreach ($dataset in $datasets) {
    $train = Join-Path $layout.Directory $dataset.Train
    $test = Join-Path $layout.Directory $dataset.Test
    foreach ($preset in $presets) {
        $stem = "$($dataset.Name)-$($layout.Name)-$preset"
        $model = Join-Path $OutputDir "$stem.model"
        $trainLog = Join-Path $OutputDir "$stem-train.log"
        $compareLog = Join-Path $OutputDir "$stem-compare.log"

        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        & $bsel train $train $model --block-size 64 --threshold $Threshold --paper-config $preset 2>&1 |
            Set-Content $trainLog
        if ($LASTEXITCODE -ne 0) { throw "Training failed: $stem" }
        $timer.Stop()

        $args = @("compare", $model, $test)
        foreach ($baseline in $baselines) { $args += @("--baseline", $baseline) }
        $output = & $bsel @args 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Comparison failed: $stem" }
        $output | Set-Content $compareLog

        $bselLine = $output | Where-Object { $_ -like "blocks=*" } | Select-Object -First 1
        $baselineLines = $output | Where-Object { $_ -like "baseline algorithm=*" }
        $allLines = @("algorithm=bsel $bselLine") + $baselineLines
        foreach ($line in $allLines) {
            $values = @{}
            foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
                $values[$match.Groups[1].Value] = $match.Groups[2].Value
            }
            $rows.Add([pscustomobject]@{
                dataset = $dataset.Name
                layout = $layout.Name
                test_file = $dataset.Test
                train_file = $dataset.Train
                bsel_preset = $preset
                algorithm = $values.algorithm
                implementation = switch ($values.algorithm) {
                    "bsel" { "byte-select" }
                    default { "in-tree-round-trip-codec" }
                }
                block_size = 64
                threshold = $Threshold
                blocks = [uint64]$values.blocks
                compressed_blocks = [uint64]$values.compressed_blocks
                compressed_fraction = [double]$values.compressed_fraction
                original_bytes = [uint64]$values.original_bytes
                encoded_bytes = [uint64]$values.encoded_bytes
                unquantized_ratio = [double]$values.unquantized_ratio
                allocated_bytes = [uint64]$values.allocated_bytes
                quantized_ratio = [double]$values.quantized_ratio
                training_seconds = if ($values.algorithm -eq "bsel") { [math]::Round($timer.Elapsed.TotalSeconds, 3) } else { $null }
            })
        }
    }
}
}

$detailCsv = Join-Path $OutputDir "ann_baseline_bsel_results.csv"
$summaryCsv = Join-Path $OutputDir "ann_baseline_bsel_summary.csv"
$rows | Export-Csv -NoTypeInformation -Encoding utf8 $detailCsv
$rows | Group-Object layout, bsel_preset, algorithm, implementation | ForEach-Object {
    $group = $_.Group
    [pscustomobject]@{
        layout = $group[0].layout
        bsel_preset = $group[0].bsel_preset
        algorithm = $group[0].algorithm
        implementation = $group[0].implementation
        dataset_count = $group.Count
        mean_unquantized_ratio = ($group | Measure-Object unquantized_ratio -Average).Average
        mean_quantized_ratio = ($group | Measure-Object quantized_ratio -Average).Average
        total_original_bytes = ($group | Measure-Object original_bytes -Sum).Sum
        total_encoded_bytes = ($group | Measure-Object encoded_bytes -Sum).Sum
        total_allocated_bytes = ($group | Measure-Object allocated_bytes -Sum).Sum
    }
} | Export-Csv -NoTypeInformation -Encoding utf8 $summaryCsv

Write-Output $detailCsv
Write-Output $summaryCsv
