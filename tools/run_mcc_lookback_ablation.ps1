param(
    [Parameter(Mandatory = $true)][string]$Sizes,
    [string]$Output = "results/mcc/lookback-ablation.csv",
    [string]$BselExe = "build/bsel.exe",
    [int]$BlockSize = 256
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sizesPath = [IO.Path]::GetFullPath((Join-Path $repo $Sizes))
$outputPath = [IO.Path]::GetFullPath((Join-Path $repo $Output))
$bsel = [IO.Path]::GetFullPath((Join-Path $repo $BselExe))
New-Item -ItemType Directory -Force (Split-Path -Parent $outputPath) | Out-Null
Set-Content $outputPath "lookback,guard_bytes,physical_bytes,padding_bytes,metadata_regions,quantized_ratio,candidate_segments_scanned,candidate_gaps_scanned,elapsed_ms"

foreach ($lookback in @(0, 1, 2)) {
    foreach ($guard in @(0, 1)) {
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $lines = & $bsel mcc-sizes $sizesPath $BlockSize `
            --guard-bytes $guard --region-lookback $lookback
        $watch.Stop()
        if ($LASTEXITCODE -ne 0) { throw "MCC lookback ablation failed" }
        $line = @($lines | Where-Object { $_ -match '^mcc_sizes_v5 ' })[0]
        $values = @{}
        foreach ($part in ($line -split "\s+")) {
            if ($part -match '^([^=]+)=(.+)$') { $values[$matches[1]] = $matches[2] }
        }
        Add-Content $outputPath "$lookback,$guard,$($values.physical_bytes),$($values.padding_bytes),$($values.metadata_regions),$($values.quantized_ratio),$($values.candidate_segments_scanned),$($values.candidate_gaps_scanned),$($watch.ElapsedMilliseconds)"
    }
}

Write-Output "summary=$outputPath"
