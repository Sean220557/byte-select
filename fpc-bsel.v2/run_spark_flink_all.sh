#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 TRACE_DIR [OUTPUT_DIR]" >&2
  exit 2
fi

trace_dir=$(realpath "$1")
output_dir=${2:-results/spark-flink/fpc-bsel-all}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bsel="$repo_dir/build-release/bsel"
fpc_bsel="$repo_dir/fpc-bsel.v2/build/fpc-bsel-v2"

[[ -x "$bsel" ]] || { echo "missing executable: $bsel" >&2; exit 2; }
[[ -x "$fpc_bsel" ]] || { echo "missing executable: $fpc_bsel" >&2; exit 2; }

baselines=(fpc bdi hybrid cpack bpc huffman)
presets=(bsel-256 bsel-4096 bsel-1024-1024-128)
workloads=(spark-kmeans-large flink-state-machine-large)
summary="$output_dir/summary.tsv"
printf 'workload\tpreset\talgorithm\tmetrics\n' > "$summary"

for workload in "${workloads[@]}"; do
  train="$trace_dir/${workload}-train.trace"
  test="$trace_dir/${workload}-test.trace"
  [[ -f "$train" && -f "$test" ]] || {
    echo "missing train/test files for $workload in $trace_dir" >&2
    exit 2
  }

  size=$(stat -c %s "$test")
  (( size > 0 && size % 64 == 0 )) || {
    echo "test input is not a non-empty multiple of 64 bytes: $test" >&2
    exit 2
  }

  fpc_model="$output_dir/${workload}-fpc-bsel.model"
  "$fpc_bsel" train "$train" "$fpc_model" --max-patterns 256 \
    | tee "$output_dir/${workload}-fpc-bsel-train.log"
  fpc_line=$("$fpc_bsel" evaluate "$fpc_model" "$test" \
    | tee "$output_dir/${workload}-fpc-bsel-evaluate.log")
  "$fpc_bsel" roundtrip "$fpc_model" "$test" \
    | tee "$output_dir/${workload}-fpc-bsel-roundtrip.log"

  for preset in "${presets[@]}"; do
    model="$output_dir/${workload}-${preset}.model"
    "$bsel" train "$train" "$model" --block-size 64 --threshold 16 \
      --paper-config "$preset" \
      | tee "$output_dir/${workload}-${preset}-train.log"

    args=(compare "$model" "$test")
    for baseline in "${baselines[@]}"; do
      args+=(--baseline "$baseline")
    done
    log="$output_dir/${workload}-${preset}-compare.log"
    "$bsel" "${args[@]}" | tee "$log"

    bsel_line=$(grep -m1 '^blocks=' "$log")
    printf '%s\t%s\tbsel\t%s\n' "$workload" "$preset" "$bsel_line" >> "$summary"
    while IFS= read -r line; do
      algorithm=$(sed -n 's/^baseline algorithm=\([^ ]*\).*/\1/p' <<< "$line")
      printf '%s\t%s\t%s\t%s\n' "$workload" "$preset" "$algorithm" "$line" >> "$summary"
    done < <(grep '^baseline algorithm=' "$log")
    printf '%s\t%s\tfpc-bsel\t%s\n' "$workload" "$preset" "$fpc_line" >> "$summary"
  done
done

echo "completed: $summary"
