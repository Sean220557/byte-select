#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: bash tools/run_slot_exception_experiment.sh DATASET_DIR [OUTPUT_DIR]

Build latest fpc-bsel.v2, train Top-256 FPC+BSEL for each train/test pair,
export real 256B payloads, and evaluate the hardware-friendly fixed-slot plus
exception layout.

Recognized pairs:
  NAME-train.trace       + NAME-test.trace
  NAME-train.bin         + NAME-test.bin
  NAME-train-sample.bin  + NAME-test.bin

Environment variables:
  LIMIT_MIB=0       MiB retained from each train/test file; 0 means all complete 4KiB regions
  JOBS=<nproc>      parallel compile jobs
  SKIP_BUILD=0      set to 1 to reuse existing Linux build
  EXCEPTION_MODEL=payload   payload, full256, or rawpayload
  MAX_GAP=256       only regions this many bytes from the next tier are counted eligible

Example:
  bash tools/run_slot_exception_experiment.sh /data/traces results/slot-exception
EOF
}

[[ $# -ge 1 && $# -le 2 ]] || { usage >&2; exit 2; }

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dataset_dir=$(realpath "$1")
output_dir=${2:-"$repo/results/slot-exception"}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")

[[ -d "$dataset_dir" ]] || { echo "not a dataset directory: $dataset_dir" >&2; exit 2; }
command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

limit_mib=${LIMIT_MIB:-0}
jobs=${JOBS:-$(nproc)}
skip_build=${SKIP_BUILD:-0}
exception_model=${EXCEPTION_MODEL:-payload}
max_gap=${MAX_GAP:-256}

for item in "$limit_mib" "$jobs" "$max_gap"; do
  [[ "$item" =~ ^[0-9]+$ ]] || { echo "numeric options must be non-negative integers" >&2; exit 2; }
done
((jobs > 0)) || { echo "JOBS must be positive" >&2; exit 2; }
[[ "$skip_build" == 0 || "$skip_build" == 1 ]] || { echo "SKIP_BUILD must be 0 or 1" >&2; exit 2; }
[[ "$exception_model" == "payload" || "$exception_model" == "full256" || "$exception_model" == "rawpayload" ]] || {
  echo "EXCEPTION_MODEL must be payload, full256, or rawpayload" >&2
  exit 2
}

build_dir="$repo/fpc-bsel.v2/build-linux-slot-exception"
exe="$build_dir/fpc-bsel-v2"
if [[ "$skip_build" == 0 ]]; then
  echo "[build] fpc-bsel.v2 Release"
  cmake -S "$repo/fpc-bsel.v2" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" -j "$jobs"
  ctest --test-dir "$build_dir" --output-on-failure
fi
[[ -x "$exe" ]] || { echo "missing executable: $exe" >&2; exit 1; }

pair_list="$output_dir/dataset-pairs.tsv"
python3 - "$dataset_dir" "$pair_list" <<'PY'
from pathlib import Path
import sys

root, output = Path(sys.argv[1]), Path(sys.argv[2])
pairs = {}
specs = [
    ("-train.trace", "-test.trace", 0),
    ("-train.bin", "-test.bin", 1),
    ("-train-sample.bin", "-test.bin", 2),
]
for path in sorted(root.rglob("*")):
    if not path.is_file():
        continue
    for train_suffix, test_suffix, priority in specs:
        if not path.name.endswith(train_suffix):
            continue
        stem = path.name[:-len(train_suffix)]
        test = path.with_name(stem + test_suffix)
        if test.is_file():
            key = str(path.parent.relative_to(root) / stem).replace("/", "__").replace("\\", "__")
            old = pairs.get(key)
            if old is None or priority < old[0]:
                pairs[key] = (priority, path.resolve(), test.resolve())
with output.open("w", encoding="utf-8") as f:
    for name, (_priority, train, test) in sorted(pairs.items()):
        f.write(f"{name}\t{train}\t{test}\n")
if not pairs:
    raise SystemExit("no recognized train/test pairs found")
print(f"discovered_pairs={len(pairs)}")
PY

prepare_trace() {
  local source=$1 destination=$2
  python3 - "$source" "$destination" "$limit_mib" <<'PY'
from pathlib import Path
import sys

src, dst, limit_mib = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])
usable = src.stat().st_size // 4096 * 4096
if limit_mib:
    usable = min(usable, limit_mib * 1024 * 1024)
    usable = usable // 4096 * 4096
if usable == 0:
    raise SystemExit(f"{src} has no complete 4KiB region")
dst.parent.mkdir(parents=True, exist_ok=True)
with src.open("rb") as inp, dst.open("wb") as out:
    remaining = usable
    while remaining:
        block = inp.read(min(16 * 1024 * 1024, remaining))
        if not block:
            raise RuntimeError("unexpected EOF")
        out.write(block)
        remaining -= len(block)
print(f"prepared={dst} bytes={usable} regions={usable // 4096}")
PY
}

summaries=()
while IFS=$'\t' read -r name train_source test_source; do
  [[ -n "$name" ]] || continue
  work="$output_dir/$name"
  mkdir -p "$work/prepared"
  train="$work/prepared/train.trace"
  test="$work/prepared/test.trace"
  prepare_trace "$train_source" "$train"
  prepare_trace "$test_source" "$test"

  model="$work/fpc-bsel-top256.model"
  train_payload="$work/train.payloads"
  test_payload="$work/test.payloads"
  echo "[dataset:$name] train latest Top-256 FPC+BSEL"
  "$exe" train-stream "$train" "$model" --max-patterns 256 | tee "$work/fpc-train.log"
  "$exe" roundtrip-stream "$model" "$test" | tee "$work/fpc-roundtrip.log"
  "$exe" payloads-256 "$model" "$train" "$train_payload"
  "$exe" payloads-256 "$model" "$test" "$test_payload"

  echo "[dataset:$name] slot+exception"
  python3 "$repo/tools/experiment_slot_exception.py" \
    "$test_payload" --name "$name" --output-dir "$work" \
    --exception-model "$exception_model" --max-gap "$max_gap" | tee "$work/slot-exception.log"
  summaries+=("$work/$name-slot-exception-summary.csv")
done < "$pair_list"

combined="$output_dir/all-slot-exception-summary.csv"
head -n 1 "${summaries[0]}" > "$combined"
for summary in "${summaries[@]}"; do
  tail -n +2 "$summary" >> "$combined"
done

echo
echo "================ SLOT+EXCEPTION SUMMARY ================"
python3 - "$combined" <<'PY'
import csv
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1], newline="")))
by_tier = defaultdict(lambda: {"eligible": 0, "crossed": 0, "changed": 0, "saved": 0})
for row in rows:
    key = f'{int(row["before_tier"]) // 1024}K->{int(row["target"]) // 1024}K'
    by_tier[key]["eligible"] += int(row["eligible"])
    by_tier[key]["crossed"] += int(row["crossed"])
    by_tier[key]["changed"] += int(row["changed"])
    by_tier[key]["saved"] += int(row["saved_bytes"])
print(f'{"tier":8} {"eligible":>9} {"crossed":>8} {"changed":>8} {"saved_B":>10}')
for key in ("4K->3K", "3K->2K", "2K->1K"):
    v = by_tier[key]
    print(f'{key:8} {v["eligible"]:9d} {v["crossed"]:8d} {v["changed"]:8d} {v["saved"]:10d}')
print(f"combined_csv={sys.argv[1]}")
PY
