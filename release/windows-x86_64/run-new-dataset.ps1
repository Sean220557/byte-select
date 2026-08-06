param(
    [string]$InputPath = "",
    [string]$TrainPath = "",
    [string]$TestPath = "",
    [string]$OutputDir = ".\output",
    [ValidateRange(1, 99)]
    [int]$TrainPercent = 90
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $OutputDir | Out-Null

function Assert-Trace([string]$Path, [string]$Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label file does not exist: $Path"
    }
    $length = (Get-Item -LiteralPath $Path).Length
    if ($length -eq 0 -or ($length % 64) -ne 0) {
        throw "$Label must be non-empty and its size must be a multiple of 64 bytes: $Path"
    }
    return [uint64]$length
}

if (![string]::IsNullOrWhiteSpace($InputPath)) {
    if (![string]::IsNullOrWhiteSpace($TrainPath) -or
        ![string]::IsNullOrWhiteSpace($TestPath)) {
        throw "Use either InputPath or TrainPath/TestPath, not both"
    }
    $length = Assert-Trace $InputPath "Input"
    $blocks = [uint64]($length / 64)
    if ($blocks -lt 2) { throw "Input must contain at least two 64-byte blocks" }
    $trainBlocks = [uint64][math]::Floor($blocks * $TrainPercent / 100.0)
    if ($trainBlocks -lt 1) { $trainBlocks = 1 }
    if ($trainBlocks -ge $blocks) { $trainBlocks = $blocks - 1 }
    $trainBytes = [uint64]($trainBlocks * 64)
    $TrainPath = Join-Path $OutputDir "split-train.delete_hole"
    $TestPath = Join-Path $OutputDir "split-test.delete_hole"

    $source = [IO.File]::OpenRead((Resolve-Path -LiteralPath $InputPath))
    try {
        $train = [IO.File]::Create($TrainPath)
        try {
            $buffer = New-Object byte[] (1024 * 1024)
            $remaining = $trainBytes
            while ($remaining -gt 0) {
                $wanted = [int][math]::Min($buffer.Length, $remaining)
                $read = $source.Read($buffer, 0, $wanted)
                if ($read -le 0) { throw "Unexpected end of input while creating training split" }
                $train.Write($buffer, 0, $read)
                $remaining -= $read
            }
        } finally { $train.Dispose() }
        $test = [IO.File]::Create($TestPath)
        try { $source.CopyTo($test, 1024 * 1024) } finally { $test.Dispose() }
    } finally { $source.Dispose() }
    Write-Host "Split blocks: train=$trainBlocks test=$($blocks - $trainBlocks)"
} elseif ([string]::IsNullOrWhiteSpace($TrainPath) -or
          [string]::IsNullOrWhiteSpace($TestPath)) {
    Write-Host "Pass one InputPath for automatic splitting:"
    Write-Host '.\run-new-dataset.ps1 -InputPath "data.delete_hole"'
    Write-Host "Or pass existing TrainPath and TestPath files."
    exit 2
}

[void](Assert-Trace $TrainPath "Training")
[void](Assert-Trace $TestPath "Test")
$model = Join-Path $OutputDir "fpc-bsel-v2.model"
$evaluateLog = Join-Path $OutputDir "fpc-bsel-v2-evaluate.log"
$roundtripLog = Join-Path $OutputDir "fpc-bsel-v2-roundtrip.log"

& "$PSScriptRoot\fpc-bsel-v2.exe" train $TrainPath $model --max-patterns 256
if ($LASTEXITCODE -ne 0) { throw "FPC-BSEL v2 training failed" }
& "$PSScriptRoot\fpc-bsel-v2.exe" evaluate $model $TestPath |
    Tee-Object -FilePath $evaluateLog
if ($LASTEXITCODE -ne 0) { throw "FPC-BSEL v2 evaluation failed" }
& "$PSScriptRoot\fpc-bsel-v2.exe" roundtrip $model $TestPath |
    Tee-Object -FilePath $roundtripLog
if ($LASTEXITCODE -ne 0) { throw "FPC-BSEL v2 round trip failed" }

Write-Host "Finished. Results: $OutputDir"
