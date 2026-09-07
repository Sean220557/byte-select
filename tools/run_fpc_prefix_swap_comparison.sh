#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: bash tools/run_fpc_prefix_swap_comparison.sh DATASET_FILE_OR_DIR [OUTPUT_DIR]" >&2
  exit 2
fi

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
input=$(realpath "$1")
output=${2:-"$repo/results/fpc-prefix-swap-comparison"}
mkdir -p "$output"
output=$(realpath "$output")
build="$repo/fpc-bsel.v2/build-final-comparison"
jobs=${JOBS:-$(nproc)}
entries=${PREFIX_CACHE_ENTRIES:-256}
progress=${PROGRESS_EVERY:-65536}

log() { printf '[fpc-prefix-swap %s] %s\n' "$(date '+%H:%M:%S')" "$*"; }

build_started=$(date +%s)
log "configure started"
cmake -S "$repo/fpc-bsel.v2" -B "$build" -DCMAKE_BUILD_TYPE=Release
log "build started jobs=$jobs"
cmake --build "$build" --target fpc-bsel-v2 prefix-eval-cpp fpc-bsel-tests -j "$jobs"
log "tests started"
(cd "$build" && ctest --output-on-failure)
log "build and tests completed elapsed_seconds=$(($(date +%s)-build_started))"

codec="$build/fpc-bsel-v2"
evaluator="$build/prefix-eval-cpp"
model="$output/custom-fpc-empty.model"
"$codec" make-empty-model "$model"

if [[ -f "$input" ]]; then
  files=("$input")
else
  mapfile -d '' files < <(find "$input" -type f \( -name '*.trace' -o -name '*.bin' -o -name '*.dat' \) -print0 | sort -z)
fi
(( ${#files[@]} )) || { echo "no dataset files found" >&2; exit 2; }

summary="$output/summary.csv"
printf '%s\n' 'dataset,original_bytes,fpc_bytes,fpc_fraction,fpc_compression_x,fpc_prefix_bytes,fpc_prefix_fraction,fpc_prefix_compression_x,fpc_prefix_swap_bytes,fpc_prefix_swap_fraction,fpc_prefix_swap_compression_x,swap_metadata_bytes,total_swaps,elapsed_seconds' > "$summary"

for file in "${files[@]}"; do
  name=$(basename "$file")
  stem=${name%.*}
  log="$output/$stem.summary.txt"
  dataset_started_ns=$(date +%s%N)
  bytes=$(stat -c %s "$file")
  log "[$name] scan started bytes=$bytes cache_entries=$entries progress_every_sublines=$progress"
  "$evaluator" --raw "$model" "$file" "$entries" "$progress" | tee "$log" >/dev/null
  elapsed_ms=$((($(date +%s%N)-dataset_started_ns)/1000000))
  log "[$name] scan completed elapsed_ms=$elapsed_ms"
  awk -v dataset="$name" -v elapsed_ms="$elapsed_ms" '
    function val(key, i, a) {
      for (i=1; i<=NF; ++i) { split($i,a,"="); if (a[1]==key) return a[2]+0 }
      return 0
    }
    /^sublines=/ { regions=val("regions"); original=regions*4096 }
    /^algorithm_bytes_before=/ { fpc=val("algorithm_bytes_before"); prefix=val("algorithm_bytes_after") }
    /^adjacent_pair_reorder / { swaps+=val("swaps") }
    END {
      flag_bytes=int((regions+7)/8)
      swap_meta=flag_bytes+swaps
      optimized=prefix+swap_meta
      printf "%s,%.0f,%.0f,%.9f,%.9f,%.0f,%.9f,%.9f,%.0f,%.9f,%.9f,%.0f,%.0f,%.3f\n", dataset,original,fpc,fpc/original,original/fpc,prefix,prefix/original,original/prefix,optimized,optimized/original,original/optimized,swap_meta,swaps,elapsed_ms/1000
    }
  ' "$log" >> "$summary"
done

echo "summary=$summary"
column -s, -t "$summary" 2>/dev/null || cat "$summary"
