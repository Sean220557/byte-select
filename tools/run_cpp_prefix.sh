#!/usr/bin/env bash
set -euo pipefail

[[ $# -ge 1 && $# -le 2 ]] || { echo "usage: bash tools/run_cpp_prefix.sh DATASET_PATH [OUTPUT_DIR]" >&2; exit 2; }
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
input=$(realpath "$1")
out=${2:-"$repo/results/cpp-prefix"}; mkdir -p "$out"; out=$(realpath "$out")
entries=${PREFIX_CACHE_ENTRIES:-256}
progress=${PROGRESS_EVERY:-1000000}
jobs=${JOBS:-$(nproc)}
skip_build=${SKIP_BUILD:-0}
build="$repo/fpc-bsel.v2/build-linux-cpp-prefix"
log(){ printf '[cpp-prefix pid=%s %s] %s\n' "$$" "$(date '+%H:%M:%S')" "$*"; }

if [[ "$skip_build" == 0 ]]; then
  log "build started"
  cmake -S "$repo/fpc-bsel.v2" -B "$build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build" --target fpc-bsel-v2 prefix-eval-cpp -j "$jobs"
fi
codec="$build/fpc-bsel-v2"; prefix="$build/prefix-eval-cpp"
mcc="$repo/build-mcc/bsel"
model="$out/custom-fpc.model"; "$codec" make-empty-model "$model"

if [[ -f "$input" ]]; then files=("$input"); else mapfile -d '' files < <(find "$input" -type f \( -name '*.trace' -o -name '*.bin' -o -name '*.dat' -o -name '*.log' \) -print0); fi
(( ${#files[@]} )) || { echo "no dataset files found" >&2; exit 2; }
for file in "${files[@]}"; do
  name=$(basename "$file"); name=${name%.*}; dir="$out/$name"; mkdir -p "$dir"
  log "[$name] fused custom-FPC + C++ prefix scan started entries=$entries"
  sizes="$dir/fpc-prefix-pair-swap.sizes"
  PREFIX_MCC_SIZES="$sizes" /usr/bin/time -f 'elapsed_seconds=%e max_rss_kib=%M' "$prefix" --raw "$model" "$file" "$entries" "$progress" 2> >(tee "$dir/progress.log" >&2) | tee "$dir/prefix-summary.txt"
  if [[ -x "$mcc" ]]; then
    "$mcc" mcc-sizes "$sizes" 256 --exact-sizes --ordered-only --guard-bytes 1 --region-lookback 1 \
      | tee "$dir/mcc-summary.txt"
  fi
  if [[ -x "$mcc" ]]; then
    "$mcc" mcc-sizes "$sizes" 256 --guard-bytes 1 --region-lookback 1 | tee "$dir/mcc-summary.txt"
  else
    log "[$name] MCC executable missing: $mcc"
  fi
  log "[$name] completed"
done
log "all results written to $out"
