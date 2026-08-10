#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: tools/run_single_trace_all_codecs.sh DATASET_FILE_OR_DIR OUTPUT_DIR

Runs every standalone codec variant and all main-project baselines on one
binary trace file. If the input is a directory, every regular file directly
under it is treated as a dataset and run in parallel. Inputs are truncated to
a 64-byte block boundary, split into train/test, then verified with roundtrip
+ optional compress/decompress + sha256.

Environment variables:
  TRAIN_PERCENT   train split percentage, integer 1..99 (default: 90)
  MAX_PATTERNS    BSEL patterns for standalone variants (default: 256)
  BSEL_PRESET     main BSEL preset for baseline compare
                  (default: bsel-1024-1024-128)
  TRAIN_BLOCK_LIMIT
                  optional maximum train blocks after splitting
  TEST_BLOCK_LIMIT
                  optional maximum test blocks after splitting
  VERIFY_COMPRESS set to 1 to also run compress/decompress/sha256
                  (default: 0; roundtrip is always run)
  DATASET_JOBS    parallel datasets when input is a directory (default: 2)
  SKIP_BUILD      set to 1 to skip CMake builds

Example:
  TRAIN_PERCENT=90 MAX_PATTERNS=256 \
    TRAIN_BLOCK_LIMIT=10000000 TEST_BLOCK_LIMIT=1000000 \
    bash tools/run_single_trace_all_codecs.sh \
      /data/flink.delete_hole.log \
      results/delete-hole-all
EOF
}

if [[ $# -ne 2 ]]; then
  usage
  exit 2
fi

input_path=$(realpath "$1")
out_dir=$2
train_percent=${TRAIN_PERCENT:-90}
max_patterns=${MAX_PATTERNS:-256}
bsel_preset=${BSEL_PRESET:-bsel-1024-1024-128}
skip_build=${SKIP_BUILD:-0}
train_block_limit=${TRAIN_BLOCK_LIMIT:-0}
test_block_limit=${TEST_BLOCK_LIMIT:-0}
verify_compress=${VERIFY_COMPRESS:-0}
dataset_jobs=${DATASET_JOBS:-2}

if ! [[ "$train_percent" =~ ^[0-9]+$ ]] || (( train_percent < 1 || train_percent > 99 )); then
  echo "TRAIN_PERCENT must be an integer in 1..99" >&2
  exit 2
fi
if ! [[ "$max_patterns" =~ ^[0-9]+$ ]] || (( max_patterns < 1 || max_patterns > 65535 )); then
  echo "MAX_PATTERNS must be an integer in 1..65535" >&2
  exit 2
fi
if ! [[ "$train_block_limit" =~ ^[0-9]+$ ]] || ! [[ "$test_block_limit" =~ ^[0-9]+$ ]]; then
  echo "TRAIN_BLOCK_LIMIT and TEST_BLOCK_LIMIT must be non-negative integers" >&2
  exit 2
fi
if [[ "$verify_compress" != "0" && "$verify_compress" != "1" ]]; then
  echo "VERIFY_COMPRESS must be 0 or 1" >&2
  exit 2
fi
if ! [[ "$dataset_jobs" =~ ^[0-9]+$ ]] || (( dataset_jobs < 1 )); then
  echo "DATASET_JOBS must be a positive integer" >&2
  exit 2
fi
[[ -e "$input_path" ]] || { echo "missing input: $input_path" >&2; exit 2; }

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$out_dir"
out_dir=$(realpath "$out_dir")

if [[ "$skip_build" != "1" ]]; then
  export CXXFLAGS="${CXXFLAGS:-} -O3 -DNDEBUG"
  export CFLAGS="${CFLAGS:-} -O3 -DNDEBUG"
  cmake -S "$repo_dir" -B "$repo_dir/build-release" -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build "$repo_dir/build-release" -j

  for dir in fpc-bsel fpc-bsel.v1 fpc-bsel.v2 cpack-bsel cpack-bsel.v1 cpack-bsel.v2; do
    cmake -S "$repo_dir/$dir" -B "$repo_dir/$dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$repo_dir/$dir/build" -j
  done
fi

if [[ -d "$input_path" ]]; then
  mapfile -t datasets < <(find "$input_path" -maxdepth 1 -type f | sort)
  if (( ${#datasets[@]} == 0 )); then
    echo "no regular dataset files found in: $input_path" >&2
    exit 2
  fi
  echo "[run] directory mode datasets=${#datasets[@]} jobs=$dataset_jobs"
  pids=()
  statuses=()
  for dataset_file in "${datasets[@]}"; do
    name=$(basename "$dataset_file")
    safe_name=$(printf '%s' "$name" | sed 's/[^A-Za-z0-9_.-]/_/g')
    dataset_out="$out_dir/$safe_name"
    echo "[run] launch dataset=$name out=$dataset_out"
    SKIP_BUILD=1 "$0" "$dataset_file" "$dataset_out" > "$out_dir/$safe_name.log" 2>&1 &
    pids+=("$!")
    if (( ${#pids[@]} >= dataset_jobs )); then
      if wait "${pids[0]}"; then statuses+=(0); else statuses+=("$?"); fi
      pids=("${pids[@]:1}")
    fi
  done
  for pid in "${pids[@]}"; do
    if wait "$pid"; then statuses+=(0); else statuses+=("$?"); fi
  done
  failed=0
  for status in "${statuses[@]}"; do
    if (( status != 0 )); then failed=1; fi
  done
  if (( failed )); then
    echo "one or more dataset runs failed; inspect logs in $out_dir" >&2
    exit 1
  fi
  echo "completed directory run: $out_dir"
  exit 0
fi

dataset="$input_path"

bsel="$repo_dir/build-release/bsel"
[[ -x "$bsel" ]] || { echo "missing executable: $bsel" >&2; exit 2; }

trace_dir="$out_dir/prepared"
mkdir -p "$trace_dir"
aligned="$trace_dir/input.aligned.trace"
train="$trace_dir/train.trace"
test="$trace_dir/test.trace"

python3 - "$dataset" "$aligned" "$train" "$test" "$train_percent" "$train_block_limit" "$test_block_limit" <<'PY'
from pathlib import Path
import sys

src, aligned, train, test = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
pct, train_limit, test_limit = int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7])
source = Path(src)
total_size = source.stat().st_size
usable = (total_size // 64) * 64
if usable == 0:
    raise SystemExit("input has no complete 64-byte blocks")
blocks = usable // 64
cut = max(1, min(blocks - 1, blocks * pct // 100)) if blocks > 1 else 1
if blocks == 1:
    train_start, train_blocks = 0, 1
    test_start, test_blocks = 0, 1
else:
    train_start, train_blocks = 0, cut
    test_start, test_blocks = cut, blocks - cut
if train_limit:
    train_blocks = min(train_blocks, train_limit)
if test_limit:
    test_blocks = min(test_blocks, test_limit)

def copy_region(out_path, start_block, block_count):
    remaining = block_count * 64
    offset = start_block * 64
    chunk = 64 * 1024 * 16
    with source.open("rb") as inp, Path(out_path).open("wb") as out:
        inp.seek(offset)
        while remaining:
            data = inp.read(min(chunk, remaining))
            if not data:
                raise RuntimeError("unexpected EOF while preparing split")
            out.write(data)
            remaining -= len(data)

copy_region(train, train_start, train_blocks)
copy_region(test, test_start, test_blocks)
Path(aligned + ".meta").write_text(
    f"source={source}\n"
    f"original_bytes={total_size}\n"
    f"aligned_bytes={usable}\n"
    f"dropped_tail_bytes={total_size - usable}\n"
    f"total_blocks={blocks}\n"
)
print(f"original_bytes={total_size}")
print(f"aligned_bytes={usable}")
print(f"dropped_tail_bytes={total_size - usable}")
print(f"total_blocks={blocks}")
print(f"train_blocks={train_blocks}")
print(f"test_blocks={test_blocks}")
PY

summary_csv="$out_dir/summary.csv"
integrity_csv="$out_dir/integrity.csv"
baseline_csv="$out_dir/baselines.csv"

printf 'family,version,algorithm,blocks,original_bytes,encoded_bytes,physical_encoded_bytes,compression_ratio,physical_compression_ratio,space_saving_percent,compressed_block_percent,raw_blocks,primary_blocks,combined_blocks,prefix_bsel_blocks,residual_bsel_blocks,raw_bsel_blocks,inline_prefix_id_blocks\n' > "$summary_csv"
printf 'family,version,roundtrip,compress,decompress,byte_compare,compressed_file_bytes,restored_bytes,original_sha256,restored_sha256\n' > "$integrity_csv"
printf 'preset,algorithm,blocks,original_bytes,encoded_bytes,compression_ratio,space_saving_percent,compressed_block_percent\n' > "$baseline_csv"

parse_value() {
  local line=$1 key=$2
  sed -n "s/.*${key}=\([^ ]*\).*/\1/p" <<< "$line"
}

safe_ratio() {
  awk -v n="${1:-0}" -v d="${2:-0}" 'BEGIN{if (d == "" || d == 0) printf "%.6f", 0; else printf "%.6f", n/d}'
}

safe_saving() {
  awk -v r="${1:-0}" 'BEGIN{if (r == "" || r == 0) printf "%.4f", 0; else printf "%.4f", (1 - 1/r) * 100}'
}

safe_compressed_percent() {
  awk -v b="${1:-0}" -v r="${2:-0}" 'BEGIN{if (b == "" || b == 0) printf "%.4f", 0; else printf "%.4f", (1 - r/b) * 100}'
}

run_codec() {
  local family=$1 version=$2 exe=$3
  local codec_dir="$out_dir/${family}-${version}"
  mkdir -p "$codec_dir"
  [[ -x "$exe" ]] || { echo "missing executable: $exe" >&2; exit 2; }

  local model="$codec_dir/model.bin"
  local packed="$codec_dir/test.packed"
  local restored="$codec_dir/test.restored.trace"
  echo "[run] $family $version train"
  "$exe" train "$train" "$model" --max-patterns "$max_patterns" | tee "$codec_dir/train.log"
  local eval_line
  echo "[run] $family $version evaluate"
  eval_line=$("$exe" evaluate "$model" "$test" | tee "$codec_dir/evaluate.log" | head -n1)
  echo "[run] $family $version roundtrip"
  "$exe" roundtrip "$model" "$test" | tee "$codec_dir/roundtrip.log"
  if [[ "$verify_compress" == "1" ]]; then
    echo "[run] $family $version compress"
    "$exe" compress "$model" "$test" "$packed" | tee "$codec_dir/compress.log"
    echo "[run] $family $version decompress"
    "$exe" decompress "$model" "$packed" "$restored" | tee "$codec_dir/decompress.log"

    local original_hash restored_hash cmp_status
    original_hash=$(sha256sum "$test" | awk '{print $1}')
    restored_hash=$(sha256sum "$restored" | awk '{print $1}')
    if [[ "$original_hash" == "$restored_hash" ]]; then
      cmp_status=PASS
    else
      cmp_status=FAIL
      echo "sha256 mismatch for $family $version" >&2
      exit 1
    fi
    printf '%s,%s,PASS,PASS,PASS,%s,%s,%s,%s,%s\n' \
      "$family" "$version" "$cmp_status" "$(stat -c %s "$packed")" "$(stat -c %s "$restored")" \
      "$original_hash" "$restored_hash" >> "$integrity_csv"
  else
    printf '%s,%s,PASS,SKIP,SKIP,SKIP,,,,\n' "$family" "$version" >> "$integrity_csv"
  fi

  local blocks original encoded physical ratio physical_ratio saving compressed raw primary combined prefix residual raw_bsel inline
  blocks=$(parse_value "$eval_line" blocks)
  original=$(parse_value "$eval_line" original_bytes)
  encoded=$(parse_value "$eval_line" encoded_bytes)
  physical=$(parse_value "$eval_line" physical_encoded_bytes)
  ratio=$(safe_ratio "$original" "$encoded")
  physical_ratio=$(safe_ratio "$original" "$physical")
  saving=$(safe_saving "$ratio")
  raw=$(parse_value "$eval_line" raw_blocks)
  primary=$(parse_value "$eval_line" fpc_blocks)
  combined=$(parse_value "$eval_line" fpc_bsel_blocks)
  prefix=$(parse_value "$eval_line" prefix_bsel_blocks)
  residual=$(parse_value "$eval_line" residual_bsel_blocks)
  raw_bsel=$(parse_value "$eval_line" raw_bsel_blocks)
  inline=$(parse_value "$eval_line" inline_prefix_id_blocks)
  raw_bsel=${raw_bsel:-0}
  inline=${inline:-0}
  compressed=$(safe_compressed_percent "$blocks" "$raw")
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$family" "$version" "$family-$version" "$blocks" "$original" "$encoded" "$physical" \
    "$ratio" "$physical_ratio" "$saving" "$compressed" "$raw" "$primary" "$combined" \
    "$prefix" "$residual" "$raw_bsel" "$inline" >> "$summary_csv"
}

run_codec fpc-bsel original "$repo_dir/fpc-bsel/build/fpc-bsel"
run_codec fpc-bsel v1 "$repo_dir/fpc-bsel.v1/build/fpc-bsel-v1"
run_codec fpc-bsel v2 "$repo_dir/fpc-bsel.v2/build/fpc-bsel-v2"
run_codec cpack-bsel original "$repo_dir/cpack-bsel/build/cpack-bsel"
run_codec cpack-bsel v1 "$repo_dir/cpack-bsel.v1/build/cpack-bsel-v1"
run_codec cpack-bsel v2 "$repo_dir/cpack-bsel.v2/build/cpack-bsel-v2"

bsel_model="$out_dir/bsel-${bsel_preset}.model"
echo "[run] baseline bsel train"
"$bsel" train "$train" "$bsel_model" --block-size 64 --threshold 16 --paper-config "$bsel_preset" \
  | tee "$out_dir/bsel-train.log"
baseline_log="$out_dir/baseline-compare.log"
echo "[run] baseline compare"
"$bsel" compare "$bsel_model" "$test" \
  --baseline fpc --baseline bdi --baseline hybrid --baseline cpack --baseline bpc --baseline huffman \
  | tee "$baseline_log"

bsel_line=$(grep -m1 '^blocks=' "$baseline_log")

write_baseline_row() {
  local algorithm=$1 line=$2
  local blocks original encoded ratio saving compressed
  blocks=$(parse_value "$line" blocks)
  original=$(parse_value "$line" original_bytes)
  encoded=$(parse_value "$line" encoded_bytes)
  ratio=$(safe_ratio "$original" "$encoded")
  saving=$(safe_saving "$ratio")
  compressed=$(awk -v f="$(parse_value "$line" compressed_fraction)" 'BEGIN{if (f == "") printf "%.4f", 0; else printf "%.4f", f * 100}')
  printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$bsel_preset" "$algorithm" "$blocks" "$original" "$encoded" "$ratio" "$saving" "$compressed" >> "$baseline_csv"
}

write_baseline_row bsel "$bsel_line"
while IFS= read -r line; do
  algorithm=$(parse_value "$line" algorithm)
  write_baseline_row "$algorithm" "$line"
done < <(grep '^baseline algorithm=' "$baseline_log")

echo "summary_csv=$summary_csv"
echo "integrity_csv=$integrity_csv"
echo "baseline_csv=$baseline_csv"
