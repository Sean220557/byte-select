param(
    [Parameter(Mandatory=$true)][string]$FpcBselExe,
    [Parameter(Mandatory=$true)][string]$FpcModel,
    [Parameter(Mandatory=$true)][string]$TrainTrace,
    [Parameter(Mandatory=$true)][string]$TestTrace,
    [Parameter(Mandatory=$true)][string]$DatasetName,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [int]$TrainMiB = 1,
    [int]$Clusters = 4
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$trainPayload = Join-Path $OutputDir "$DatasetName-train.payloads"
$testPayload = Join-Path $OutputDir "$DatasetName-test.payloads"
& $FpcBselExe payloads-256 $FpcModel $TrainTrace $trainPayload
& $FpcBselExe payloads-256 $FpcModel $TestTrace $testPayload
$toolDir = Split-Path -Parent $MyInvocation.MyCommand.Path
python (Join-Path $toolDir 'experiment_payload_generalization.py') `
    $trainPayload $testPayload --name $DatasetName --train-mib $TrainMiB `
    --clusters $Clusters --output-dir $OutputDir
python (Join-Path $toolDir 'payload_var_codec.py') `
    (Join-Path $OutputDir "$DatasetName-lz.model.json") $testPayload
python (Join-Path $toolDir 'experiment_fpc_payload_lz.py') `
    $testPayload --name "$DatasetName-adaptive" --limit-mib 1 --output-dir $OutputDir
python (Join-Path $toolDir 'payload_var_codec.py') `
    (Join-Path $OutputDir "$DatasetName-adaptive-payload-lz.adaptive.json") $testPayload
