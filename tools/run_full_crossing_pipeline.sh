#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash tools/run_full_crossing_pipeline.sh --input FILE [options]
  bash tools/run_full_crossing_pipeline.sh --input-dir DIR [options]

One-command server pipeline for a complete dataset. The script splits the input
into train/test, builds latest fpc-bsel.v2, trains Top-256 FPC+BSEL, exports
real 256B payloads, and evaluates crossing-oriented methods.

Methods included:
  baseline-fpc-bsel
  slot-exception          hardware-friendly target for 2K->1K
  3k-boundary-effect      slack-funded region-local micro-table for 3K->2K
  metadata-only-0k        special-pattern metadata-only target for 1K->0K

Options:
  --input FILE            One complete dataset file.
  --input-dir DIR         Directory containing dataset files. Files are tested independently.
  --output-dir DIR        Output directory (default: results/full-crossing-pipeline)
  --train-percent N       Prefix percentage used for training (default: 20)
  --limit-mib N           Optional MiB cap per input file before split (default: 0, all)
  --chunk-mib N           Passed to FPC training/eval where supported (default: 256)
  --jobs N                Parallel build jobs (default: nproc)
  --skip-build            Reuse existing fpc-bsel.v2 build
  --roundtrip             Verify FPC roundtrip on test split
  -h, --help              Show help

Tuning environment variables:
  MAX_GAP=256
  EXCEPTION_MODEL=payload       payload, full256, or rawpayload
  THREEK_CANDIDATE_CAP=128
  THREEK_EFFECT_CAP=512
  THREEK_BANK_BUDGET=192
  THREEK_MAX_ENTRIES=4          must be <=4 for current direct-marker codec
  ZERO_K_LOOKBACK=64

Example:
  bash tools/run_full_crossing_pipeline.sh --input /data/spark.trace --output-dir results/spark-cross
  LIMIT_MIB=1024 bash tools/run_full_crossing_pipeline.sh --input-dir /data/traces
EOF
}

input=""
input_dir=""
output_dir="results/full-crossing-pipeline"
train_percent=20
limit_mib=0
chunk_mib=256
jobs=$(nproc)
skip_build=0
roundtrip=0

while (($#)); do
  case "$1" in
    --input) input=${2:?missing value for --input}; shift 2 ;;
    --input-dir) input_dir=${2:?missing value for --input-dir}; shift 2 ;;
    --output-dir) output_dir=${2:?missing value for --output-dir}; shift 2 ;;
    --train-percent) train_percent=${2:?missing value for --train-percent}; shift 2 ;;
    --limit-mib) limit_mib=${2:?missing value for --limit-mib}; shift 2 ;;
    --chunk-mib) chunk_mib=${2:?missing value for --chunk-mib}; shift 2 ;;
    --jobs) jobs=${2:?missing value for --jobs}; shift 2 ;;
    --skip-build) skip_build=1; shift ;;
    --roundtrip) roundtrip=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$input" || -n "$input_dir" ]] || { echo "--input or --input-dir is required" >&2; exit 2; }
[[ -z "$input" || -z "$input_dir" ]] || { echo "use only one of --input or --input-dir" >&2; exit 2; }
[[ "$train_percent" =~ ^[0-9]+$ && "$train_percent" -ge 1 && "$train_percent" -le 99 ]] || {
  echo "--train-percent must be in [1, 99]" >&2; exit 2;
}
for item in "$limit_mib" "$chunk_mib" "$jobs"; do
  [[ "$item" =~ ^[0-9]+$ ]] || { echo "numeric options must be non-negative integers" >&2; exit 2; }
done
((chunk_mib > 0 && jobs > 0)) || { echo "--chunk-mib and --jobs must be positive" >&2; exit 2; }

max_gap=${MAX_GAP:-256}
exception_model=${EXCEPTION_MODEL:-payload}
threek_candidate_cap=${THREEK_CANDIDATE_CAP:-128}
threek_effect_cap=${THREEK_EFFECT_CAP:-512}
threek_bank_budget=${THREEK_BANK_BUDGET:-192}
threek_max_entries=${THREEK_MAX_ENTRIES:-4}
zero_k_lookback=${ZERO_K_LOOKBACK:-64}
for item in "$max_gap" "$threek_candidate_cap" "$threek_effect_cap" "$threek_bank_budget" "$threek_max_entries" "$zero_k_lookback"; do
  [[ "$item" =~ ^[0-9]+$ ]] || { echo "tuning variables must be non-negative integers" >&2; exit 2; }
done
((threek_max_entries <= 4)) || { echo "THREEK_MAX_ENTRIES must be <=4 for current direct-marker codec" >&2; exit 2; }
[[ "$exception_model" == "payload" || "$exception_model" == "full256" || "$exception_model" == "rawpayload" ]] || {
  echo "EXCEPTION_MODEL must be payload, full256, or rawpayload" >&2; exit 2;
}

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
resolve_path() { [[ "$1" = /* ]] && printf '%s\n' "$1" || printf '%s/%s\n' "$repo" "$1"; }
output_dir=$(resolve_path "$output_dir")
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")

command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 2; }
python_bin=""
for candidate in python3.11 python3.10 python3.9 python3.8 python3.7 python3; do
  if command -v "$candidate" >/dev/null; then
    python_bin=$candidate
    break
  fi
done
[[ -n "$python_bin" ]] || { echo "python3.7+ is required" >&2; exit 2; }
"$python_bin" - <<'PY'
import sys
if sys.version_info < (3, 7):
    raise SystemExit("python3.7+ is required")
PY

build_dir="$repo/fpc-bsel.v2/build-linux-full-crossing"
exe="$build_dir/fpc-bsel-v2"
if ((skip_build == 0)); then
  echo "[build] fpc-bsel.v2 Release"
  cmake -S "$repo/fpc-bsel.v2" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" -j "$jobs"
  ctest --test-dir "$build_dir" --output-on-failure
fi
[[ -x "$exe" ]] || { echo "missing executable: $exe" >&2; exit 1; }

dataset_list="$output_dir/datasets.tsv"
if [[ -n "$input" ]]; then
  input=$(resolve_path "$input")
  [[ -f "$input" ]] || { echo "missing input file: $input" >&2; exit 1; }
  "$python_bin" - "$input" "$dataset_list" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]).resolve()
name = p.stem.replace(" ", "_")
Path(sys.argv[2]).write_text(f"{name}\t{p}\n", encoding="utf-8")
PY
else
  input_dir=$(resolve_path "$input_dir")
  [[ -d "$input_dir" ]] || { echo "missing input dir: $input_dir" >&2; exit 1; }
  "$python_bin" - "$input_dir" "$dataset_list" <<'PY'
from pathlib import Path
import sys
root, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
suffixes = {".trace", ".bin", ".dat"}
files = [p for p in sorted(root.rglob("*")) if p.is_file() and p.suffix.lower() in suffixes]
if not files:
    raise SystemExit("no .trace/.bin/.dat files found")
with out.open("w", encoding="utf-8") as f:
    for p in files:
        rel = p.relative_to(root).with_suffix("")
        name = str(rel).replace("/", "__").replace("\\", "__").replace(" ", "_")
        f.write(f"{name}\t{p.resolve()}\n")
print(f"datasets={len(files)}")
PY
fi

prepare_split() {
  local source=$1 train=$2 test=$3 meta=$4
  "$python_bin" - "$source" "$train" "$test" "$meta" "$train_percent" "$limit_mib" <<'PY'
from pathlib import Path
import json
import sys

src, train, test, meta = map(Path, sys.argv[1:5])
train_percent, limit_mib = int(sys.argv[5]), int(sys.argv[6])
usable = src.stat().st_size
if limit_mib:
    usable = min(usable, limit_mib * 1024 * 1024)
usable = usable // 4096 * 4096
if usable < 8192:
    raise SystemExit(f"{src} must contain at least two complete 4KiB regions")
train_bytes = usable * train_percent // 100
train_bytes = train_bytes // 4096 * 4096
test_bytes = usable - train_bytes
if train_bytes <= 0 or test_bytes <= 0:
    raise SystemExit("invalid split; adjust --train-percent or --limit-mib")
train.parent.mkdir(parents=True, exist_ok=True)
test.parent.mkdir(parents=True, exist_ok=True)
with src.open("rb") as inp:
    train.write_bytes(inp.read(train_bytes))
    test.write_bytes(inp.read(test_bytes))
meta.write_text(json.dumps({
    "source": str(src),
    "usable_bytes": usable,
    "train_bytes": train_bytes,
    "test_bytes": test_bytes,
    "train_regions": train_bytes // 4096,
    "test_regions": test_bytes // 4096,
}, indent=2), encoding="utf-8")
print(f"split source={src} train_regions={train_bytes // 4096} test_regions={test_bytes // 4096}")
PY
}

dataset_summaries=()
while IFS=$'\t' read -r name source; do
  [[ -n "$name" ]] || continue
  work="$output_dir/$name"
  mkdir -p "$work/prepared" "$work/slot" "$work/3k"
  train="$work/prepared/train.trace"
  test="$work/prepared/test.trace"
  prepare_split "$source" "$train" "$test" "$work/split.json"

  model="$work/fpc-bsel-top256.model"
  train_payload="$work/train.payloads"
  test_payload="$work/test.payloads"
  echo "[dataset:$name] train Top-256 FPC+BSEL"
  "$exe" train-stream "$train" "$model" --max-patterns 256 | tee "$work/fpc-train.log"
  if ((roundtrip)); then
    "$exe" roundtrip-stream "$model" "$test" | tee "$work/fpc-roundtrip.log"
  fi
  "$exe" payloads-256 "$model" "$train" "$train_payload"
  "$exe" payloads-256 "$model" "$test" "$test_payload"

  echo "[dataset:$name] slot+exception"
  "$python_bin" "$repo/tools/experiment_slot_exception.py" \
    "$test_payload" --name "$name" --output-dir "$work/slot" \
    --exception-model "$exception_model" --max-gap "$max_gap" | tee "$work/slot.log"

  echo "[dataset:$name] 3K boundary effect micro-table"
  "$python_bin" "$repo/tools/experiment_3k_boundary_shaving.py" \
    "$train_payload" "$test_payload" --name "$name" --output-dir "$work/3k" \
    --oracle-region-local --pool-policy effect --stride 1 \
    --max-entries "$threek_max_entries" --bank-budget "$threek_bank_budget" \
    --candidate-cap "$threek_candidate_cap" --effect-candidate-cap "$threek_effect_cap" \
    --max-boundary-need 64 --max-gap "$max_gap" | tee "$work/3k-effect.log"

  echo "[dataset:$name] metadata-only 1K->0K"
  "$python_bin" "$repo/tools/experiment_metadata_only_0k.py" \
    "$test" --payloads "$test_payload" --name "$name" --output-dir "$work/0k" \
    --lookback "$zero_k_lookback" | tee "$work/0k.log"

  final_csv="$work/crossing-summary.csv"
  "$python_bin" - "$repo/tools" "$name" "$test_payload" \
    "$work/slot/$name-slot-exception-summary.csv" \
    "$work/3k/$name-3k-boundary-local-summary.csv" \
    "$work/0k/$name-metadata-only-0k-summary.csv" \
    "$final_csv" <<'PY'
from pathlib import Path
import csv
import sys

repo_tools, name, payload, slot_csv, threek_csv, zerok_csv, output = sys.argv[1:]
sys.path.insert(0, repo_tools)
import experiment_fpc_payload_lz as pp

regions = pp.regions(pp.read_payloads(Path(payload), 10**12))
tiers = ((4096, 3072), (3072, 2048), (2048, 1024))
rows = []
for tier, target in tiers:
    eligible = sum(r.tier == tier and 0 < r.gap <= 256 for r in regions)
    rows.append({
        "dataset": name, "method": "baseline-fpc-bsel", "before_tier": tier,
        "target": target, "eligible": eligible, "crossed": 0,
        "changed": 0, "saved_bytes": 0,
    })
with open(slot_csv, newline="") as f:
    for row in csv.DictReader(f):
        rows.append({
            "dataset": name, "method": "slot-exception",
            "before_tier": int(row["before_tier"]), "target": int(row["target"]),
            "eligible": int(row["eligible"]), "crossed": int(row["crossed"]),
            "changed": int(row["changed"]), "saved_bytes": int(row["saved_bytes"]),
        })
with open(threek_csv, newline="") as f:
    row = next(csv.DictReader(f))
    rows.append({
        "dataset": name, "method": "3k-boundary-effect",
        "before_tier": int(row["before_tier"]), "target": int(row["target"]),
        "eligible": int(row["eligible"]), "crossed": int(row["crossed"]),
        "changed": int(row["changed"]), "saved_bytes": int(row["saved_bytes"]),
    })
with open(zerok_csv, newline="") as f:
    row = next(csv.DictReader(f))
    rows.append({
        "dataset": name, "method": "metadata-only-0k",
        "before_tier": int(row["before_tier"]), "target": int(row["target"]),
        "eligible": int(row["eligible"]), "crossed": int(row["crossed"]),
        "changed": int(row["changed"]), "saved_bytes": int(row["saved_bytes"]),
    })
with open(output, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=rows[0].keys())
    w.writeheader()
    w.writerows(rows)
PY
  dataset_summaries+=("$final_csv")
done < "$dataset_list"

combined="$output_dir/all-crossing-summary.csv"
head -n 1 "${dataset_summaries[0]}" > "$combined"
for summary in "${dataset_summaries[@]}"; do
  tail -n +2 "$summary" >> "$combined"
done

echo
echo "================ CROSSING SUMMARY ================"
"$python_bin" - "$combined" <<'PY'
import csv
import sys
from collections import defaultdict

rows = list(csv.DictReader(open(sys.argv[1], newline="")))
data = defaultdict(lambda: [0, 0, 0, 0, 0, 0, 0, 0])
for row in rows:
    tier = int(row["before_tier"])
    idx = {4096: 0, 3072: 1, 2048: 2, 1024: 3}[tier]
    key = (row["dataset"], row["method"])
    data[key][idx] += int(row["crossed"])
    data[key][idx + 4] += int(row["eligible"])
print(f'{"dataset":30} {"method":22} {"4K->3K":>13} {"3K->2K":>13} {"2K->1K":>13} {"1K->0K":>13} {"total":>8}')
for (dataset, method), values in sorted(data.items()):
    parts = []
    total_crossed = 0
    for i in range(4):
        total_crossed += values[i]
        parts.append(f"{values[i]}/{values[i + 4]}")
    print(f'{dataset[:30]:30} {method:22} {parts[0]:>13} {parts[1]:>13} {parts[2]:>13} {parts[3]:>13} {total_crossed:8d}')
print(f"combined_csv={sys.argv[1]}")
PY
