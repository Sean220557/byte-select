param(
    [string]$InputPath = "",
    [string]$TrainPath = "",
    [string]$TestPath = "",
    [string]$OutputDir = ".\output",
    [ValidateRange(1, 99)]
    [int]$TrainPercent = 90
)

$ErrorActionPreference = "Stop"
$runnerArgs = @{ OutputDir = $OutputDir; TrainPercent = $TrainPercent }
if (![string]::IsNullOrWhiteSpace($InputPath)) {
    $runnerArgs.InputPath = $InputPath
} else {
    $runnerArgs.TrainPath = $TrainPath
    $runnerArgs.TestPath = $TestPath
}
& "$PSScriptRoot\run-new-dataset.ps1" @runnerArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (![string]::IsNullOrWhiteSpace($InputPath)) {
    $TrainPath = Join-Path $OutputDir "split-train.delete_hole"
    $TestPath = Join-Path $OutputDir "split-test.delete_hole"
}

function Run-Checked([scriptblock]$Command, [string]$Message) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw $Message }
}

$bselModel = Join-Path $OutputDir "byte-select.model"
$bselTrainLog = Join-Path $OutputDir "byte-select-train.log"
$bselCompareLog = Join-Path $OutputDir "byte-select-and-baselines.log"
Run-Checked {
    & "$PSScriptRoot\bsel.exe" train $TrainPath $bselModel --block-size 64 `
        --threshold 16 --paper-config bsel-1024-1024-128 |
        Tee-Object -FilePath $bselTrainLog
} "Byte-Select training failed"
$compareArgs = @("compare", $bselModel, $TestPath)
foreach ($name in @("fpc", "bdi", "hybrid", "cpack", "bpc", "huffman")) {
    $compareArgs += @("--baseline", $name)
}
Run-Checked {
    & "$PSScriptRoot\bsel.exe" @compareArgs | Tee-Object -FilePath $bselCompareLog
} "Byte-Select/baseline comparison failed"

foreach ($version in @(
    @{ Name = "fpc-bsel-original"; Exe = "fpc-bsel.exe" },
    @{ Name = "fpc-bsel-v1"; Exe = "fpc-bsel-v1.exe" }
)) {
    $model = Join-Path $OutputDir "$($version.Name).model"
    $trainLog = Join-Path $OutputDir "$($version.Name)-train.log"
    $evaluateLog = Join-Path $OutputDir "$($version.Name)-evaluate.log"
    $roundtripLog = Join-Path $OutputDir "$($version.Name)-roundtrip.log"
    Run-Checked {
        & (Join-Path $PSScriptRoot $version.Exe) train $TrainPath $model --max-patterns 256 |
            Tee-Object -FilePath $trainLog
    } "$($version.Name) training failed"
    Run-Checked {
        & (Join-Path $PSScriptRoot $version.Exe) evaluate $model $TestPath |
            Tee-Object -FilePath $evaluateLog
    } "$($version.Name) evaluation failed"
    Run-Checked {
        & (Join-Path $PSScriptRoot $version.Exe) roundtrip $model $TestPath |
            Tee-Object -FilePath $roundtripLog
    } "$($version.Name) round trip failed"
}

function Parse-Fields([string]$Line) {
    $fields = @{}
    foreach ($match in [regex]::Matches($Line, '(\w+)=([^ ]+)')) {
        $fields[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $fields
}

$rows = [System.Collections.Generic.List[object]]::new()
$compareLines = Get-Content $bselCompareLog
$byteSelect = Parse-Fields (@($compareLines | Where-Object { $_ -like "blocks=*" })[0])
$rows.Add([pscustomobject]@{
    algorithm="byte-select"; blocks=$byteSelect.blocks; original_bytes=$byteSelect.original_bytes
    encoded_bytes=$byteSelect.encoded_bytes; physical_encoded_bytes=""
    ratio=$byteSelect.unquantized_ratio; compressed_blocks=$byteSelect.compressed_blocks
})
foreach ($line in $compareLines | Where-Object { $_ -like "baseline algorithm=*" }) {
    $v = Parse-Fields $line
    $rows.Add([pscustomobject]@{
        algorithm=$v.algorithm; blocks=$v.blocks; original_bytes=$v.original_bytes
        encoded_bytes=$v.encoded_bytes; physical_encoded_bytes=""
        ratio=$v.unquantized_ratio; compressed_blocks=$v.compressed_blocks
    })
}
foreach ($name in @("fpc-bsel-original", "fpc-bsel-v1", "fpc-bsel-v2")) {
    $line = @(Get-Content (Join-Path $OutputDir "$name-evaluate.log") |
        Where-Object { $_ -like "blocks=*" })[0]
    $v = Parse-Fields $line
    $rows.Add([pscustomobject]@{
        algorithm=$name; blocks=$v.blocks; original_bytes=$v.original_bytes
        encoded_bytes=$v.encoded_bytes; physical_encoded_bytes=$v.physical_encoded_bytes
        ratio=$v.ratio; compressed_blocks=([uint64]$v.blocks - [uint64]$v.raw_blocks)
    })
}
$summary = Join-Path $OutputDir "summary.csv"
$rows | Export-Csv -NoTypeInformation -Encoding UTF8 $summary
$rows | Format-Table -AutoSize
Write-Host "All results: $OutputDir"
Write-Host "Unified summary: $summary"
