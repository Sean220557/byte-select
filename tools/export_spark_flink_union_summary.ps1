$ErrorActionPreference = "Stop"
$resultDir = "results/spark-flink/training-union"
$traceDir = "datasets/spark-flink/traces"
$bsel = ".\build-release\bsel.exe"
$baselines = @("fpc", "bdi", "hybrid", "cpack", "bpc", "huffman")
$workloads = @(
    @{ Name = "spark-kmeans"; Trace = "spark-kmeans-smoke" },
    @{ Name = "flink-state-machine"; Trace = "flink-state-machine-smoke" }
)

Copy-Item "$resultDir/training_union_test.csv" `
    "$resultDir/training_union_phase_test.csv" -Force

foreach ($workload in $workloads) {
    $parts = [Collections.Generic.List[byte[]]]::new()
    foreach ($phase in 1..4) {
        $parts.Add([IO.File]::ReadAllBytes(
            (Resolve-Path "$traceDir/$($workload.Trace)-phase$phase-test.trace")))
    }
    $length = ($parts | ForEach-Object Length | Measure-Object -Sum).Sum
    $combined = New-Object byte[] $length
    $offset = 0
    foreach ($part in $parts) {
        [Array]::Copy($part, 0, $combined, $offset, $part.Length)
        $offset += $part.Length
    }
    [IO.File]::WriteAllBytes(
        (Join-Path (Resolve-Path $traceDir) "$($workload.Trace)-stratified-test.trace"),
        $combined)
}

$rows = [Collections.Generic.List[object]]::new()
foreach ($workload in $workloads) {
    $model = "$resultDir/$($workload.Name).model"
    $test = "$traceDir/$($workload.Trace)-stratified-test.trace"
    $arguments = @("compare", $model, $test)
    foreach ($baseline in $baselines) { $arguments += @("--baseline", $baseline) }
    $output = & $bsel @arguments
    if ($LASTEXITCODE -ne 0) { throw "Comparison failed: $($workload.Name)" }
    $bselLine = $output | Where-Object { $_ -like "blocks=*" } | Select-Object -First 1
    $lines = @("algorithm=bsel $bselLine") + @(
        $output | Where-Object { $_ -like "baseline algorithm=*" })

    foreach ($line in $lines) {
        $v = @{}
        foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) {
            $v[$match.Groups[1].Value] = $match.Groups[2].Value
        }
        $rawRatio = [double]$v.original_bytes / [double]$v.encoded_bytes
        $quantizedRatio = [double]$v.original_bytes / [double]$v.allocated_bytes
        $rows.Add([pscustomobject][ordered]@{
            workload = $workload.Name
            bsel_preset = "bsel-1024-1024-128-training-union"
            algorithm = $v.algorithm
            compressed_block_percent = [math]::Round([double]$v.compressed_fraction * 100, 4)
            compression_ratio = [math]::Round($rawRatio, 6)
            quantized_compression_ratio = [math]::Round($quantizedRatio, 6)
            raw_space_saving_percent = [math]::Round((1 - 1 / $rawRatio) * 100, 4)
            quantized_space_saving_percent = [math]::Round((1 - 1 / $quantizedRatio) * 100, 4)
            training_seconds = $null
            train_blocks = 18000
            test_blocks = [uint64]$v.blocks
            threshold = "2|4|8|16|32|64"
        })
    }
}

$rows | Export-Csv -NoTypeInformation -Encoding utf8 `
    "$resultDir/training_union_test.csv"
