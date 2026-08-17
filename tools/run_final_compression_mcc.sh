#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/run_final_compression_mcc.sh --input FILE [options]

Options:
  --train-percent N       Prefix percentage used for training (default: 20)
  --output-dir DIR        Result directory (default: results/final-compression-mcc)
  --chunk-mib N           Test chunk size in MiB (default: 256)
  --fpc-exe FILE          FPC-BSEL executable (default: fpc-bsel.v2/build/fpc-bsel-v2)
  --bsel-exe FILE         Baseline/MCC executable (default: build/bsel)
  --ablation ALGORITHM    MCC ablation target (default: fpc-bsel-3k)
  --roundtrip             Verify FPC-BSEL decoding during evaluation and sizing
  --resume                Reuse existing models and evaluation logs
  -h, --help              Show this help
EOF
}

input=""
train_percent=20
output_dir="results/final-compression-mcc"
chunk_mib=256
fpc_exe="fpc-bsel.v2/build/fpc-bsel-v2"
bsel_exe="build/bsel"
ablation_algorithm="fpc-bsel-3k"
roundtrip=0
resume=0

while (($#)); do
  case "$1" in
    --input) input=${2:?missing value for --input}; shift 2 ;;
    --train-percent) train_percent=${2:?missing value for --train-percent}; shift 2 ;;
    --output-dir) output_dir=${2:?missing value for --output-dir}; shift 2 ;;
    --chunk-mib) chunk_mib=${2:?missing value for --chunk-mib}; shift 2 ;;
    --fpc-exe) fpc_exe=${2:?missing value for --fpc-exe}; shift 2 ;;
    --bsel-exe) bsel_exe=${2:?missing value for --bsel-exe}; shift 2 ;;
    --ablation) ablation_algorithm=${2:?missing value for --ablation}; shift 2 ;;
    --roundtrip) roundtrip=1; shift ;;
    --resume) resume=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$input" ]] || { echo "--input is required" >&2; exit 2; }
[[ "$train_percent" =~ ^[0-9]+$ && "$train_percent" -ge 1 && "$train_percent" -le 99 ]] || {
  echo "--train-percent must be in [1, 99]" >&2; exit 2;
}
[[ "$chunk_mib" =~ ^[0-9]+$ && "$chunk_mib" -gt 0 ]] || {
  echo "--chunk-mib must be positive" >&2; exit 2;
}

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
resolve_path() { [[ "$1" = /* ]] && printf '%s\n' "$1" || printf '%s/%s\n' "$repo" "$1"; }
input=$(resolve_path "$input")
output_dir=$(resolve_path "$output_dir")
fpc_exe=$(resolve_path "$fpc_exe")
bsel_exe=$(resolve_path "$bsel_exe")

[[ -f "$input" ]] || { echo "missing dataset: $input" >&2; exit 1; }
[[ -x "$fpc_exe" ]] || { echo "missing executable: $fpc_exe" >&2; exit 1; }
[[ -x "$bsel_exe" ]] || { echo "missing executable: $bsel_exe" >&2; exit 1; }
mkdir -p "$output_dir/fpc-models"

total_bytes=$(stat -c %s -- "$input")
((total_bytes % 256 == 0)) || { echo "dataset size must be a multiple of 256B" >&2; exit 1; }
train_bytes=$((total_bytes * train_percent / 100))
train_bytes=$((train_bytes - train_bytes % 256))
test_offset=$train_bytes
test_bytes=$((total_bytes - train_bytes))
((train_bytes > 0 && test_bytes > 0)) || { echo "invalid train/test split" >&2; exit 1; }
chunk_bytes=$((chunk_mib * 1024 * 1024))
chunk_bytes=$((chunk_bytes - chunk_bytes % 256))
((chunk_bytes > 0)) || { echo "chunk size is smaller than 256B" >&2; exit 1; }

model_names=(4k 3k 2k 1k)
model_budgets=(4 3 2 1)
training_log="$output_dir/fpc-training-evaluation.log"
: > "$training_log"
for i in "${!model_names[@]}"; do
  name=${model_names[$i]}
  budget=${model_budgets[$i]}
  model="$output_dir/fpc-models/fpc-$name.model"
  train_log="$output_dir/fpc-models/train-$name.log"
  if ((resume == 0)) || [[ ! -s "$model" ]]; then
    "$fpc_exe" train-budget-range "$input" "$model" --budget-kib "$budget" \
      --offset-bytes 0 --length-bytes "$train_bytes" --chunk-mib "$chunk_mib" \
      | tee "$train_log" >> "$training_log"
  fi
  eval_log="$output_dir/fpc-models/evaluate-$name.log"
  if ((resume == 0)) || [[ ! -s "$eval_log" ]]; then
    eval_command=evaluate-range
    ((roundtrip)) && eval_command=roundtrip-range
    "$fpc_exe" "$eval_command" "$model" "$input" --offset-bytes "$test_offset" \
      --length-bytes "$test_bytes" --chunk-mib "$chunk_mib" \
      | tee "$eval_log" >> "$training_log"
  fi
done

top256_model="$output_dir/fpc-models/fpc-top256-word-v2.model"
if ((resume == 0)) || [[ ! -s "$top256_model" ]]; then
  "$bsel_exe" fpc-top256-train-range "$input" "$top256_model" 256 0 "$train_bytes" \
    | tee "$output_dir/fpc-models/train-top256.log" >> "$training_log"
fi

algorithms=(fpc-bsel-4k fpc-bsel-3k fpc-bsel-2k fpc-bsel-1k fpc-top256 fpc-top256-xor bdi hybrid-top256 hybrid-top256-xor cpack bpc huffman)
declare -A model_for=(
  [fpc-bsel-4k]="fpc-4k.model" [fpc-bsel-3k]="fpc-3k.model"
  [fpc-bsel-2k]="fpc-2k.model" [fpc-bsel-1k]="fpc-1k.model"
)

work="$output_dir/chunk-work"
rm -rf -- "$work"
mkdir -p "$work/logs" "$work/ablation"
trap 'rm -rf -- "$work"' EXIT

offset=$test_offset
remaining=$test_bytes
chunk_index=0
while ((remaining > 0)); do
  count=$chunk_bytes
  ((count > remaining)) && count=$remaining
  raw="$work/chunk.bin"
  dd if="$input" of="$raw" iflag=skip_bytes,count_bytes skip="$offset" count="$count" status=none
  [[ $(stat -c %s -- "$raw") -eq "$count" ]] || { echo "short dataset read" >&2; exit 1; }

  for algorithm in "${algorithms[@]}"; do
    mcc_log="$work/logs/$algorithm.log"
    if [[ -n "${model_for[$algorithm]:-}" ]]; then
      sizes="$work/$algorithm.sizes"
      size_args=(sizes-256 "$output_dir/fpc-models/${model_for[$algorithm]}" "$raw" "$sizes")
      ((roundtrip)) && size_args+=(--roundtrip)
      "$fpc_exe" "${size_args[@]}" >/dev/null
      "$bsel_exe" mcc-sizes "$sizes" 256 --guard-bytes 1 --region-lookback 1 >> "$mcc_log"
    elif [[ "$algorithm" == fpc-top256* || "$algorithm" == hybrid-top256* ]]; then
      sizes="$work/$algorithm.sizes"
      top256_args=(fpc-top256-sizes "$top256_model" "$raw" "$sizes")
      [[ "$algorithm" == hybrid-top256* ]] && top256_args+=(--hybrid)
      [[ "$algorithm" == *-xor ]] && top256_args+=(--spatial-xor)
      "$bsel_exe" "${top256_args[@]}" >/dev/null
      "$bsel_exe" mcc-sizes "$sizes" 256 --guard-bytes 1 --region-lookback 1 >> "$mcc_log"
    else
      "$bsel_exe" mcc-baseline "$algorithm" "$raw" 256 \
        --guard-bytes 1 --region-lookback 1 >> "$mcc_log"
    fi

    if [[ "$algorithm" == "$ablation_algorithm" ]]; then
      for setting in 0:0 1:0 0:1 1:1 0:2 1:2; do
        guard=${setting%%:*}; lookback=${setting##*:}
        ablation_log="$work/ablation/g${guard}-l${lookback}.log"
        if [[ -n "${model_for[$algorithm]:-}" ]]; then
          "$bsel_exe" mcc-sizes "$sizes" 256 --guard-bytes "$guard" \
            --region-lookback "$lookback" >> "$ablation_log"
        elif [[ "$algorithm" == fpc-top256* || "$algorithm" == hybrid-top256* ]]; then
          "$bsel_exe" mcc-sizes "$sizes" 256 --guard-bytes "$guard" \
            --region-lookback "$lookback" >> "$ablation_log"
        else
          "$bsel_exe" mcc-baseline "$algorithm" "$raw" 256 --guard-bytes "$guard" \
            --region-lookback "$lookback" >> "$ablation_log"
        fi
      done
    fi
  done

  offset=$((offset + count))
  remaining=$((remaining - count))
  chunk_index=$((chunk_index + 1))
  printf 'completed chunk %d: %d/%d test bytes\n' "$chunk_index" "$((test_bytes - remaining))" "$test_bytes"
done

aggregate_mcc() {
  local log=$1
  awk '
    function value(key, i, p) {
      for (i=2; i<=NF; ++i) { split($i,p,"="); if (p[1]==key) return p[2]+0 }
      return 0
    }
    /^mcc_sizes_before / { original+=value("original_bytes"); stored+=value("stored_bytes"); before+=value("physical_bytes"); chunks++ }
    /^mcc_sizes_v1 / { v1+=value("physical_bytes") }
    /^mcc_sizes_v2 / { v2+=value("physical_bytes") }
    /^mcc_sizes_v3 / { v3+=value("physical_bytes") }
    /^mcc_sizes_v4 / { v4+=value("physical_bytes") }
    /^mcc_sizes_v5 / { v5+=value("physical_bytes"); regions+=value("metadata_regions"); segments+=value("candidate_segments_scanned"); gaps+=value("candidate_gaps_scanned") }
    END { printf "%.0f,%.0f,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.0f,%.0f,%.0f,%.0f,%.0f", original,stored,original/stored,original/before,original/v1,original/v2,original/v3,original/v4,original/v5,v5,regions,segments,gaps,chunks }
  ' "$log"
}

summary="$output_dir/final-summary.csv"
echo "algorithm,subline_bytes,original_bytes,stored_bytes,algorithm_ratio,before_ratio,v1_ratio,v2_ratio,v3_ratio,v4_ratio,v5_ratio,v5_physical_bytes,v5_metadata_regions,candidate_segments,candidate_gaps,chunks" > "$summary"
for algorithm in "${algorithms[@]}"; do
  printf '%s,256,%s\n' "$algorithm" "$(aggregate_mcc "$work/logs/$algorithm.log")" >> "$summary"
done

ablation_summary="$output_dir/mcc-ablation-$ablation_algorithm.csv"
echo "setting,guard,lookback,stored_bytes,physical_bytes,quantized_ratio,metadata_regions,candidate_segments,candidate_gaps,chunks" > "$ablation_summary"
for setting in 0:0 1:0 0:1 1:1 0:2 1:2; do
  guard=${setting%%:*}; lookback=${setting##*:}
  awk -v key="g${guard}-l${lookback}" -v guard="$guard" -v lookback="$lookback" '
    function value(name, i, p) { for(i=2;i<=NF;++i){split($i,p,"=");if(p[1]==name)return p[2]+0} return 0 }
    /^mcc_sizes_before / { original+=value("original_bytes"); stored+=value("stored_bytes") }
    /^mcc_sizes_v5 / { physical+=value("physical_bytes"); regions+=value("metadata_regions"); segments+=value("candidate_segments_scanned"); gaps+=value("candidate_gaps_scanned"); chunks++ }
    END { printf "%s,%s,%s,%.0f,%.0f,%.17g,%.0f,%.0f,%.0f,%.0f\n",key,guard,lookback,stored,physical,original/physical,regions,segments,gaps,chunks }
  ' "$work/ablation/g${guard}-l${lookback}.log" >> "$ablation_summary"
done

trap - EXIT
rm -rf -- "$work"
printf 'summary=%s\nablation=%s\n' "$summary" "$ablation_summary"
