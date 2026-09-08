#!/usr/bin/env bash
set -euo pipefail
[[ $# -ge 1 && $# -le 2 ]] || { echo "usage: bash tools/run_fpc_prefix_swap_comparison.sh DATASET_FILE_OR_DIR [OUTPUT_DIR]" >&2; exit 2; }
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd); input=$(realpath "$1"); output=${2:-"$repo/results/fpc-top256-swap-comparison"}; mkdir -p "$output"; output=$(realpath "$output")
fpc_build="$repo/fpc-bsel.v2/build-final-comparison"; bsel_build="$repo/build-top256-comparison"; jobs=${JOBS:-$(nproc)}; train_percent=${TOP256_TRAIN_PERCENT:-20}; max_train_mib=${TOP256_MAX_TRAIN_MIB:-256}; chunk_mib=${CHUNK_MIB:-64}; exact_matrix=${EXACT_MATRIX:-0}
log(){ printf '[fpc-top256-swap %s] %s\n' "$(date '+%H:%M:%S')" "$*"; }
[[ "$train_percent" =~ ^[0-9]+$ && "$train_percent" -ge 1 && "$train_percent" -le 99 ]] || { echo "TOP256_TRAIN_PERCENT must be 1..99" >&2; exit 2; }
[[ "$exact_matrix" == 0 || "$exact_matrix" == 1 ]] || { echo "EXACT_MATRIX must be 0 or 1" >&2; exit 2; }
build_started=$(date +%s); log "configure and build started jobs=$jobs"
cmake -S "$repo" -B "$bsel_build" -G Ninja -DCMAKE_BUILD_TYPE=Release; cmake --build "$bsel_build" --target bsel byte_select_tests -j "$jobs"
cmake -S "$repo/fpc-bsel.v2" -B "$fpc_build" -DCMAKE_BUILD_TYPE=Release; cmake --build "$fpc_build" --target top256-pair-eval fpc-bsel-tests -j "$jobs"
log "tests started"; "$bsel_build/byte_select_tests"; (cd "$fpc_build" && ctest --output-on-failure); log "build and tests completed elapsed_seconds=$(($(date +%s)-build_started))"
if [[ -f "$input" ]]; then files=("$input"); else mapfile -d '' files < <(find "$input" -type f \( -name '*.trace' -o -name '*.bin' -o -name '*.dat' \) -print0 | sort -z); fi
(( ${#files[@]} )) || { echo "no dataset files found" >&2; exit 2; }
summary="$output/summary.csv"; printf '%s\n' 'dataset,original_bytes,fpc_quantized_bytes,fpc_quantized_fraction,fpc_compression_x,fpc_top256_quantized_bytes,fpc_top256_quantized_fraction,fpc_top256_compression_x,fpc_top256_swap_quantized_bytes,fpc_top256_swap_quantized_fraction,fpc_top256_swap_compression_x,swap_saved_bytes,swap_gain_points,total_swaps,reordered_regions,elapsed_seconds' > "$summary"
matrix="$output/layout-matrix.csv"; printf '%s\n' 'dataset,fpc_shared_full_fraction,fpc_independent_full_fraction,fpc_independent_optimal_full_fraction,top256_shared_full_fraction,top256_local_shared_full_fraction,top256_global_shared_full_fraction,top256_independent_full_fraction,top256_local_independent_full_fraction,top256_global_independent_full_fraction,top256_greedy_independent_full_fraction,top256_optimal_independent_full_fraction,common_length_metadata_bytes,top256_model_bytes,reorder_bitmap_bytes' > "$matrix"
for file in "${files[@]}"; do
 name=$(basename "$file"); stem=${name%.*}; dir="$output/$stem"; mkdir -p "$dir"; total=$(stat -c %s -- "$file"); usable=$((total-total%256)); train=$((usable*train_percent/100)); train=$((train-train%4096))
 if ((max_train_mib>0)); then cap=$((max_train_mib*1024*1024)); ((train>cap)) && train=$((cap-cap%4096)); fi
 test_offset=$train; test_bytes=$((usable-test_offset)); test_bytes=$((test_bytes-test_bytes%4096)); ((train>0&&test_bytes>=4096)) || { echo "dataset too small: $file" >&2; exit 1; }
 model="$dir/top256.model"; plain="$dir/custom-fpc.sizes"; top="$dir/fpc-top256.sizes"; : > "$plain"; : > "$top"; started_ns=$(date +%s%N); log "[$name] Top-256 training bytes=$train"
 "$bsel_build/bsel" fpc-top256-train-range "$file" "$model" 256 0 "$train" | tee "$dir/train.log"
 work=$(mktemp -d "$dir/chunks.XXXXXX"); offset=$test_offset; remaining=$test_bytes; index=0; chunk_bytes=$((chunk_mib*1024*1024)); chunk_bytes=$((chunk_bytes-chunk_bytes%4096))
 while ((remaining>0)); do count=$chunk_bytes; ((count>remaining)) && count=$remaining; count=$((count-count%4096)); ((count>0)) || break; chunk="$work/chunk.bin"; chunk_top="$work/top.sizes"; chunk_plain="$work/plain.sizes"; dd if="$file" of="$chunk" iflag=skip_bytes,count_bytes skip="$offset" count="$count" status=none; "$bsel_build/bsel" fpc-top256-sizes "$model" "$chunk" "$chunk_top" --plain-output "$chunk_plain" >/dev/null; cat "$chunk_plain" >> "$plain"; cat "$chunk_top" >> "$top"; offset=$((offset+count)); remaining=$((remaining-count)); index=$((index+1)); elapsed=$((($(date +%s%N)-started_ns)/1000000000)); rate=$(((offset-test_offset)/1048576/(elapsed+1))); log "[$name] chunks=$index processed=$((test_bytes-remaining))/$test_bytes rate_mib_s=$rate"; done
 rm -rf -- "$work"; model_bytes=$(stat -c %s -- "$model"); exact_args=(); [[ "$exact_matrix" == 1 ]] && exact_args+=(--exact-matrix); result=$("$fpc_build/top256-pair-eval" "$plain" "$top" "$test_bytes" "$model_bytes" "${exact_args[@]}"); printf '%s\n' "$result" > "$dir/summary.txt"; elapsed_ms=$((($(date +%s%N)-started_ns)/1000000))
 awk -v dataset="$name" -v elapsed_ms="$elapsed_ms" 'function v(k,i,a){for(i=1;i<=NF;++i){split($i,a,"=");if(a[1]==k)return a[2]+0}} {before=v("top256_independent_full_fraction");after=v("top256_swap_full_fraction");printf "%s,%.0f,%.0f,%.9f,%.9f,%.0f,%.9f,%.9f,%.0f,%.9f,%.9f,%.0f,%.6f,%.0f,%.0f,%.3f\n",dataset,v("original_bytes"),v("fpc_independent_full_bytes"),v("fpc_independent_full_fraction"),v("fpc_independent_full_x"),v("top256_independent_full_bytes"),before,v("top256_independent_full_x"),v("top256_swap_full_bytes"),after,v("top256_swap_full_x"),v("top256_swap_saved_bytes"),100*(before-after),v("greedy_independent_swaps"),v("greedy_independent_regions"),elapsed_ms/1000}' <<< "$result" >> "$summary"
 awk -v dataset="$name" 'function v(k,i,a){for(i=1;i<=NF;++i){split($i,a,"=");if(a[1]==k)return a[2]+0}} {printf "%s,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.0f,%.0f,%.0f\n",dataset,v("fpc_shared_full_fraction"),v("fpc_independent_full_fraction"),v("fpc_independent_optimal_full_fraction"),v("top256_shared_full_fraction"),v("top256_local_shared_full_fraction"),v("top256_global_shared_full_fraction"),v("top256_independent_full_fraction"),v("top256_local_independent_full_fraction"),v("top256_global_independent_full_fraction"),v("top256_greedy_independent_full_fraction"),v("top256_optimal_independent_full_fraction"),v("common_length_metadata_bytes"),v("top256_model_bytes"),v("reorder_bitmap_bytes")}' <<< "$result" >> "$matrix"
done
log "completed summary=$summary diagnostic_matrix=$matrix"
awk -F, '
  NR == 1 { next }
  {
    printf "\n=== %s ===\n", $1
    printf "Original:                 %12.0f bytes  100.0000%%\n", $2
    printf "FPC:                      %12.0f bytes  %8.4f%%  saved=%7.4f%%  %.4fx\n", $3,100*$4,100*(1-$4),$5
    printf "FPC + Top256:             %12.0f bytes  %8.4f%%  saved=%7.4f%%  %.4fx\n", $6,100*$7,100*(1-$7),$8
    printf "FPC + Top256 + Swap:      %12.0f bytes  %8.4f%%  saved=%7.4f%%  %.4fx\n", $9,100*$10,100*(1-$10),$11
    printf "Swap incremental gain:   %12.0f bytes  %8.4f percentage-points\n", $12,$13
    printf "Swap activity:            %12.0f swaps  %8.0f reordered regions\n", $14,$15
    printf "Dataset elapsed:          %12.3f seconds\n", $16
  }
' "$summary"
printf '\nCSV summary: %s\nDiagnostic matrix: %s\n' "$summary" "$matrix"
