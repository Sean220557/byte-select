param(
    [string]$Root = "..",
    [string]$OutputDir = "../results/spark-flink/cpack-bsel-v1-all"
)

$ErrorActionPreference = "Stop"
$rootPath = (Resolve-Path $Root).Path
$bsel = Join-Path $rootPath "build-release/bsel.exe"
$cpackBsel = Join-Path $PSScriptRoot "build/cpack-bsel-v1.exe"
$traceDir = Join-Path $rootPath "datasets/spark-flink/traces"
$outputPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $OutputDir))
New-Item -ItemType Directory -Force $outputPath | Out-Null

$workloads = @(
    @{ Name = "spark-kmeans-large"; Train = "spark-kmeans-large-train.trace"; Test = "spark-kmeans-large-test.trace" },
    @{ Name = "flink-state-machine-large"; Train = "flink-state-machine-large-train.trace"; Test = "flink-state-machine-large-test.trace" }
)
$presets = @("bsel-256", "bsel-4096", "bsel-1024-1024-128")
$baselines = @("fpc", "bdi", "hybrid", "cpack", "bpc", "huffman")
$rows = [Collections.Generic.List[object]]::new()

function Parse-Line([string]$line) {
    $values = @{}
    foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
        $values[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $values
}

function Add-Row($workload, $preset, $algorithm, $values, $physicalBytes) {
    $ratio = [double]$values.original_bytes / [double]$values.encoded_bytes
    $rows.Add([pscustomobject][ordered]@{
        workload = $workload
        bsel_preset = $preset
        algorithm = $algorithm
        blocks = [uint64]$values.blocks
        original_bytes = [uint64]$values.original_bytes
        encoded_bytes = [uint64]$values.encoded_bytes
        physical_encoded_bytes = $physicalBytes
        compression_ratio = [math]::Round($ratio, 6)
        space_saving_percent = [math]::Round((1 - 1 / $ratio) * 100, 4)
        compressed_block_percent = [math]::Round([double]$values.compressed_fraction * 100, 4)
    })
}

foreach ($workload in $workloads) {
    $train = Join-Path $traceDir $workload.Train
    $test = Join-Path $traceDir $workload.Test
    $cpackModel = Join-Path $outputPath "$($workload.Name)-C-Pack-BSEL.model"
    & $cpackBsel train $train $cpackModel --max-patterns 256 |
        Set-Content (Join-Path $outputPath "$($workload.Name)-C-Pack-BSEL-train.log")
    if ($LASTEXITCODE -ne 0) { throw "C-Pack-BSEL training failed: $($workload.Name)" }
    $cpackOutput = & $cpackBsel evaluate $cpackModel $test
    if ($LASTEXITCODE -ne 0) { throw "C-Pack-BSEL evaluation failed: $($workload.Name)" }
    $cpackOutput | Set-Content (Join-Path $outputPath "$($workload.Name)-C-Pack-BSEL-evaluate.log")
    $cpackValues = Parse-Line $cpackOutput
    $cpackValues.compressed_fraction = 1 - ([double]$cpackValues.raw_blocks / [double]$cpackValues.blocks)

    foreach ($preset in $presets) {
        $model = Join-Path $rootPath "results/spark-flink/large/$($workload.Name)-$preset.model"
        if (!(Test-Path $model)) { throw "Missing existing BSEL model: $model" }
        $arguments = @("compare", $model, $test)
        foreach ($baseline in $baselines) { $arguments += @("--baseline", $baseline) }
        $output = & $bsel @arguments
        if ($LASTEXITCODE -ne 0) { throw "BSEL comparison failed: $($workload.Name) $preset" }
        $output | Set-Content (Join-Path $outputPath "$($workload.Name)-$preset-compare.log")

        $bselLine = $output | Where-Object { $_ -like "blocks=*" } | Select-Object -First 1
        $values = Parse-Line $bselLine
        Add-Row $workload.Name $preset "bsel" $values $null
        foreach ($line in @($output | Where-Object { $_ -like "baseline algorithm=*" })) {
            $values = Parse-Line $line
            Add-Row $workload.Name $preset $values.algorithm $values $null
        }
        Add-Row $workload.Name $preset "C-Pack-BSEL" $cpackValues ([uint64]$cpackValues.physical_encoded_bytes)
    }
}

$csv = Join-Path $outputPath "spark_flink_all_baselines.csv"
$rows | Export-Csv -NoTypeInformation -Encoding utf8 $csv
$rows | Sort-Object workload,bsel_preset,@{Expression="compression_ratio";Descending=$true} |
    Format-Table workload,bsel_preset,algorithm,compression_ratio,space_saving_percent,compressed_block_percent -AutoSize
Write-Output "csv=$csv"
