#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: bash tools/run_payload_crossing_experiment.sh DATASET_DIR [OUTPUT_DIR]

Builds latest fpc-bsel.v2 from source, runs its tests, discovers independent
train/test dataset pairs, trains a Top-256 FPC+BSEL baseline, exports the real
256B payload, and reports 4K->3K, 3K->2K and 2K->1K crossings for:
  baseline, fixed LZ, fixed no-LZ, causal-local LZ,
  adaptive region LZ, adaptive region no-LZ, bounded real LZ,
  and 2/4/8/16-subline grouped packing.

Recognized pairs (recursive):
  NAME-train.trace       + NAME-test.trace
  NAME-train.bin         + NAME-test.bin
  NAME-train-sample.bin  + NAME-test.bin

Environment variables:
  LIMIT_MIB=0       MiB retained from each train/test file; 0 means all
                    complete 4KiB regions (default: 0)
  TRAIN_MIB=0       regions used by fixed-bank training; 0 means all retained
  TEST_MIB=0        regions used by fixed-bank testing; 0 means all retained
  CLUSTERS=4        fixed-bank locality clusters per capacity tier
  HISTORY=16        preceding test regions available to causal method
  JOBS=<nproc>      parallel compile jobs
  SKIP_BUILD=0      set to 1 to reuse an existing Linux build
  ROUNDTRIP=1       verify fixed/adaptive direct-marker streams

Example:
  bash tools/run_payload_crossing_experiment.sh /data/traces results/crossings
  LIMIT_MIB=1 bash tools/run_payload_crossing_experiment.sh /data/traces
EOF
}

[[ $# -ge 1 && $# -le 2 ]] || { usage >&2; exit 2; }

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dataset_dir=$(realpath "$1")
output_dir=${2:-"$repo/results/payload-crossings"}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")

[[ -d "$dataset_dir" ]] || { echo "not a dataset directory: $dataset_dir" >&2; exit 2; }
command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

limit_mib=${LIMIT_MIB:-0}
train_mib=${TRAIN_MIB:-0}
test_mib=${TEST_MIB:-0}
clusters=${CLUSTERS:-4}
history=${HISTORY:-16}
jobs=${JOBS:-$(nproc)}
skip_build=${SKIP_BUILD:-0}
roundtrip=${ROUNDTRIP:-1}

for item in "$limit_mib" "$train_mib" "$test_mib" "$clusters" "$history" "$jobs"; do
  [[ "$item" =~ ^[0-9]+$ ]] || { echo "numeric options must be non-negative integers" >&2; exit 2; }
done
((clusters > 0 && history > 0 && jobs > 0)) || { echo "CLUSTERS, HISTORY and JOBS must be positive" >&2; exit 2; }
[[ "$skip_build" == 0 || "$skip_build" == 1 ]] || { echo "SKIP_BUILD must be 0 or 1" >&2; exit 2; }
[[ "$roundtrip" == 0 || "$roundtrip" == 1 ]] || { echo "ROUNDTRIP must be 0 or 1" >&2; exit 2; }

build_dir="$repo/fpc-bsel.v2/build-linux-crossings"
exe="$build_dir/fpc-bsel-v2"
if [[ "$skip_build" == 0 ]]; then
  echo "[build] fpc-bsel.v2 Release"
  cmake -S "$repo/fpc-bsel.v2" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" -j "$jobs"
  ctest --test-dir "$build_dir" --output-on-failure
fi
[[ -x "$exe" ]] || { echo "missing executable: $exe" >&2; exit 1; }

# Emit tab-separated: dataset-name, train path, test path. Prefer the canonical
# .trace pair when duplicate representations of the same dataset exist.
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
    for name, (_, train, test) in sorted(pairs.items()):
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
  mkdir -p "$work/prepared" "$work/fixed" "$work/adaptive"
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

  echo "[dataset:$name] fixed LZ/no-LZ banks"
  python3 "$repo/tools/experiment_payload_generalization.py" \
    "$train_payload" "$test_payload" --name "$name" \
    --train-mib "$train_mib" --test-mib "$test_mib" --clusters "$clusters" \
    --output-dir "$work/fixed" | tee "$work/fixed.log"

  fixed_lz_model="$work/fixed/$name-lz.model.json"
  echo "[dataset:$name] causal local LZ"
  python3 "$repo/tools/experiment_payload_causal.py" \
    "$test_payload" "$fixed_lz_model" --name "$name-causal" --history "$history" \
    --output "$work/causal-regions.csv" | tee "$work/causal.log"

  echo "[dataset:$name] adaptive region LZ/no-LZ"
  python3 "$repo/tools/experiment_fpc_payload_lz.py" \
    "$test_payload" --name "$name-adaptive" --limit-mib "$test_mib" \
    --output-dir "$work/adaptive" | tee "$work/adaptive.log"

  echo "[dataset:$name] bounded LZ and grouped-packing controls"
  python3 "$repo/tools/experiment_payload_lz_oracle.py" \
    "$test_payload" --name "$name" --limit-mib "$test_mib" \
    --output "$work/lz-and-packing.csv" | tee "$work/lz-and-packing.log"

  if [[ "$roundtrip" == 1 ]]; then
    python3 "$repo/tools/payload_var_codec.py" "$fixed_lz_model" "$test_payload" | tee "$work/fixed-roundtrip.log"
    python3 "$repo/tools/payload_var_codec.py" \
      "$work/adaptive/$name-adaptive-payload-lz.adaptive.json" "$test_payload" | tee "$work/adaptive-lz-roundtrip.log"
    python3 "$repo/tools/payload_var_codec.py" \
      "$work/adaptive/$name-adaptive-payload-no-lz.adaptive.json" "$test_payload" | tee "$work/adaptive-no-lz-roundtrip.log"
  fi

  final_csv="$work/crossings.csv"
  python3 - "$repo/tools" "$name" "$test_payload" "$work/fixed/$name-summary.csv" \
    "$work/causal-regions.csv" "$work/adaptive/$name-adaptive-summary.csv" \
    "$work/lz-and-packing.csv" "$final_csv" <<'PY'
from pathlib import Path
import csv, sys

sys.path.insert(0, str(Path(__file__).resolve().parent) if "__file__" in globals() else ".")
repo_tools, name, payload, fixed_csv, causal_csv, adaptive_csv, packing_csv, output = sys.argv[1:]
sys.path.insert(0, repo_tools)
import experiment_fpc_payload_lz as pp

tiers = ((4096, 3072), (3072, 2048), (2048, 1024))
regions = pp.regions(pp.read_payloads(Path(payload), 10**9))
rows = []
for tier, target in tiers:
    eligible = sum(r.tier == tier and 0 < r.gap <= 256 for r in regions)
    rows.append(dict(dataset=name, method="baseline-fpc-bsel", before_tier=tier,
                     target=target, eligible=eligible, crossed=0))

def append_summary(path, rename):
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows.append(dict(dataset=name, method=rename.get(row["method"], row["method"]),
                             before_tier=int(row["before_tier"]), target=int(row["target"]),
                             eligible=int(row["eligible"]), crossed=int(row["crossed"])))

append_summary(fixed_csv, {"lz": "fixed-lz", "no-lz": "fixed-no-lz"})
with open(causal_csv, newline="") as f:
    causal = list(csv.DictReader(f))
for tier, target in tiers:
    selected = [r for r in causal if int(r["before_tier"]) == tier and 0 < int(r["gap"]) <= 256]
    rows.append(dict(dataset=name, method="causal-local-lz", before_tier=tier,
                     target=target, eligible=len(selected),
                     crossed=sum(int(r["after"]) <= target for r in selected)))
append_summary(adaptive_csv, {"payload-lz": "adaptive-region-lz",
                              "payload-no-lz": "adaptive-region-no-lz"})
with open(packing_csv, newline="") as f:
    packing = list(csv.DictReader(f))
for method, column in (("bounded-lz-256", "crossed"),
                       ("sequential-region-lz", "region_lz_crossed"),
                       ("pack-2-sublines", "pack2_crossed"),
                       ("pack-4-sublines", "pack4_crossed"),
                       ("pack-8-sublines", "pack8_crossed"),
                       ("pack-16-sublines", "pack16_crossed")):
    for tier, target in tiers:
        selected = [r for r in packing if int(r["before_tier"]) == tier and 0 < int(r["gap"]) <= 256]
        rows.append(dict(dataset=name, method=method, before_tier=tier, target=target,
                         eligible=len(selected), crossed=sum(int(r[column]) for r in selected)))
order = {m:i for i,m in enumerate(("baseline-fpc-bsel", "fixed-lz", "fixed-no-lz",
                                    "causal-local-lz", "adaptive-region-lz",
                                    "adaptive-region-no-lz", "bounded-lz-256",
                                    "sequential-region-lz", "pack-2-sublines",
                                    "pack-4-sublines", "pack-8-sublines",
                                    "pack-16-sublines"))}
rows.sort(key=lambda r: (order[r["method"]], -r["before_tier"]))
with open(output, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=("dataset", "method", "before_tier", "target", "eligible", "crossed"))
    writer.writeheader(); writer.writerows(rows)
print("\nCrossing summary:")
print(f"{'method':26} {'4K->3K':>8} {'3K->2K':>8} {'2K->1K':>8} {'total':>8}")
for method in order:
    values = [r["crossed"] for r in rows if r["method"] == method]
    print(f"{method:26} {values[0]:8d} {values[1]:8d} {values[2]:8d} {sum(values):8d}")
PY
  summaries+=("$final_csv")
done < "$pair_list"

combined="$output_dir/all-crossings.csv"
head -n 1 "${summaries[0]}" > "$combined"
for summary in "${summaries[@]}"; do tail -n +2 "$summary" >> "$combined"; done

echo
echo "================ ALL DATASETS ================"
python3 - "$combined" <<'PY'
import csv, sys
from collections import defaultdict
data = defaultdict(lambda: [0, 0, 0])
with open(sys.argv[1], newline="") as f:
    for row in csv.DictReader(f):
        idx = {4096: 0, 3072: 1, 2048: 2}[int(row["before_tier"])]
        data[(row["dataset"], row["method"])][idx] += int(row["crossed"])
print(f"{'dataset':34} {'method':26} {'4K->3K':>8} {'3K->2K':>8} {'2K->1K':>8} {'total':>8}")
for (dataset, method), values in data.items():
    print(f"{dataset[:34]:34} {method:26} {values[0]:8d} {values[1]:8d} {values[2]:8d} {sum(values):8d}")
PY
echo "csv=$combined"
