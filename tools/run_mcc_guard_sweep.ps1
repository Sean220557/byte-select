param(
    [Parameter(Mandatory = $true)][string]$Sizes,
    [string]$Output = "results/mcc/guard-sweep.csv",
    [string]$BselExe = "build/bsel.exe",
    [int]$BlockSize = 256
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sizesPath = [IO.Path]::GetFullPath((Join-Path $repo $Sizes))
$outputPath = [IO.Path]::GetFullPath((Join-Path $repo $Output))
$bsel = [IO.Path]::GetFullPath((Join-Path $repo $BselExe))
New-Item -ItemType Directory -Force (Split-Path -Parent $outputPath) | Out-Null
Set-Content $outputPath "guard_bytes,stored_bytes,padding_bytes,physical_bytes,metadata_regions,quantized_ratio"

foreach ($guard in @(0, 1, 2, 4)) {
    $lines = & $bsel mcc-sizes $sizesPath $BlockSize --guard-bytes $guard
    if ($LASTEXITCODE -ne 0) { throw "MCC guard sweep failed: $guard" }
    $line = @($lines | Where-Object { $_ -match '^mcc_sizes_v5 ' })[0]
    $values = @{}
    foreach ($part in ($line -split "\s+")) {
        if ($part -match '^([^=]+)=(.+)$') { $values[$matches[1]] = $matches[2] }
    }
    Add-Content $outputPath "$guard,$($values.stored_bytes),$($values.padding_bytes),$($values.physical_bytes),$($values.metadata_regions),$($values.quantized_ratio)"
}

Write-Output "summary=$outputPath"
