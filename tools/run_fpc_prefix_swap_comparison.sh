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
progress=${PROGRESS_EVERY:-1000000}

cmake -S "$repo/fpc-bsel.v2" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --target fpc-bsel-v2 prefix-eval-cpp fpc-bsel-tests -j "$jobs"
(cd "$build" && ctest --output-on-failure)

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
printf '%s\n' 'dataset,original_bytes,custom_fpc_bytes,custom_fpc_fraction,custom_fpc_compression_x,prefix_swap_bytes,prefix_swap_fraction,prefix_swap_compression_x,swap_metadata_bytes,total_swaps,pair_physical_before,pair_physical_after,pair_saved_bytes' > "$summary"

for file in "${files[@]}"; do
  name=$(basename "$file")
  stem=${name%.*}
  log="$output/$stem.summary.txt"
  "$evaluator" --raw "$model" "$file" "$entries" "$progress" | tee "$log"
  awk -v dataset="$name" '
    function val(key, i, a) {
      for (i=1; i<=NF; ++i) { split($i,a,"="); if (a[1]==key) return a[2]+0 }
      return 0
    }
    /^sublines=/ { regions=val("regions"); original=regions*4096 }
    /^algorithm_bytes_before=/ { fpc=val("algorithm_bytes_before"); prefix=val("algorithm_bytes_after") }
    /^adjacent_pair_reorder / { swaps+=val("swaps") }
    /^adjacent_pair_physical_bytes_before=/ {
      pair_before=val("adjacent_pair_physical_bytes_before")
      pair_after=val("after")
      pair_saved=val("saved")
    }
    END {
      flag_bytes=int((regions+7)/8)
      swap_meta=flag_bytes+swaps
      optimized=prefix+swap_meta
      printf "%s,%.0f,%.0f,%.9f,%.9f,%.0f,%.9f,%.9f,%.0f,%.0f,%.0f,%.0f,%.0f\n", dataset,original,fpc,fpc/original,original/fpc,optimized,optimized/original,original/optimized,swap_meta,swaps,pair_before,pair_after,pair_saved
    }
  ' "$log" >> "$summary"
done

echo "summary=$summary"
column -s, -t "$summary" 2>/dev/null || cat "$summary"
