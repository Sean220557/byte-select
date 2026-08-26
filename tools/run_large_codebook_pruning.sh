#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash tools/run_large_codebook_pruning.sh DATASET_DIR [OUTPUT_DIR]

Complete large-dataset pipeline:
  1. discover complete .trace/.bin/.dat datasets;
  2. split every file by 4KiB region into FPC/codebook train and test;
  3. build and test the latest fpc-bsel.v2;
  4. train Top-256 FPC+BSEL and export real 16x256B payloads;
  5. evaluate full and pruned Huffman banks plus Top256/Top4 repeat dictionaries;
  6. verify every tested LZ stream by decoding it;
  7. report exact dictionary/table/codebook bytes, algorithmic ratio and
     1KiB-quantized ratio in per-dataset JSON and one combined CSV/table.

Methods:
  top256-full8       Top256 repeat dictionary + complete 8-bucket Huffman bank
  top4-full8         Top4 repeat dictionary + complete 8-bucket Huffman bank
  top4-pruned8       Top4 + minimum 8-bucket table subset selected on train
  top4-pruned4       Top4 + minimum 4-bucket table subset selected on train
  dynamic-prefix-*   causal 128/256-entry prefix cache, with/without previous-word predictor
  implicit-prefix-*  all zero-codebook anchor/run/XOR/delta/bitmap prefix codecs

Environment:
  FPC_TRAIN_PERCENT=20  prefix used to train FPC+BSEL (default 20)
  CODEBOOK_PERCENT=20   following prefix used to train/select codebooks (default 20)
  LIMIT_MIB=0           cap each dataset before splitting; 0 uses all data
  JOBS=<nproc>          parallel compiler jobs
  LZ_CANDIDATES=64      bounded LZ candidate checks
  SKIP_BUILD=0          set to 1 to reuse the existing build
  ROUNDTRIP=1           verify FPC+BSEL on the test split

The remaining regions form the sealed test split. Percentages are calculated
from the complete input file and every boundary is aligned to 4KiB.
EOF
}

if [[ ${1:-} == "-h" || ${1:-} == "--help" ]]; then usage; exit 0; fi
[[ $# -ge 1 && $# -le 2 ]] || { usage >&2; exit 2; }

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dataset_dir=$(realpath "$1")
output_dir=${2:-"$repo/results/large-codebook-pruning"}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")

[[ -d "$dataset_dir" ]] || { echo "not a dataset directory: $dataset_dir" >&2; exit 2; }
command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 2; }
python_bin=""
for candidate in python3.12 python3.11 python3.10 python3.9 python3.8 python3; do
  if command -v "$candidate" >/dev/null; then python_bin=$candidate; break; fi
done
[[ -n "$python_bin" ]] || { echo "python3.8+ is required" >&2; exit 2; }
"$python_bin" - <<'PY'
import sys
if sys.version_info < (3, 8):
    raise SystemExit("python3.8+ is required")
PY

fpc_percent=${FPC_TRAIN_PERCENT:-20}
codebook_percent=${CODEBOOK_PERCENT:-20}
limit_mib=${LIMIT_MIB:-0}
jobs=${JOBS:-$(nproc)}
lz_candidates=${LZ_CANDIDATES:-64}
skip_build=${SKIP_BUILD:-0}
roundtrip=${ROUNDTRIP:-1}
for value in "$fpc_percent" "$codebook_percent" "$limit_mib" "$jobs" "$lz_candidates"; do
  [[ "$value" =~ ^[0-9]+$ ]] || { echo "numeric settings must be non-negative integers" >&2; exit 2; }
done
((fpc_percent > 0 && codebook_percent > 0 && fpc_percent + codebook_percent < 100)) || {
  echo "FPC_TRAIN_PERCENT and CODEBOOK_PERCENT must be positive and sum to less than 100" >&2; exit 2;
}
((jobs > 0 && lz_candidates > 0)) || { echo "JOBS and LZ_CANDIDATES must be positive" >&2; exit 2; }
[[ "$skip_build" == 0 || "$skip_build" == 1 ]] || { echo "SKIP_BUILD must be 0 or 1" >&2; exit 2; }
[[ "$roundtrip" == 0 || "$roundtrip" == 1 ]] || { echo "ROUNDTRIP must be 0 or 1" >&2; exit 2; }

build_dir="$repo/fpc-bsel.v2/build-linux-codebook-pruning"
exe="$build_dir/fpc-bsel-v2"
if ((skip_build == 0)); then
  echo "[build] latest fpc-bsel.v2 Release"
  cmake -S "$repo/fpc-bsel.v2" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" -j "$jobs"
  ctest --test-dir "$build_dir" --output-on-failure
fi
[[ -x "$exe" ]] || { echo "missing executable: $exe" >&2; exit 1; }

dataset_list="$output_dir/datasets.tsv"
"$python_bin" - "$dataset_dir" "$dataset_list" <<'PY'
from pathlib import Path
import sys
root, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
files = [p for p in sorted(root.rglob("*"))
         if p.is_file() and p.suffix.lower() in {".trace", ".bin", ".dat"}]
if not files:
    raise SystemExit("no .trace/.bin/.dat dataset files found")
with output.open("w", encoding="utf-8") as out:
    for path in files:
        rel = path.relative_to(root).with_suffix("")
        name = str(rel).replace("/", "__").replace("\\", "__").replace(" ", "_")
        out.write(f"{name}\t{path.resolve()}\n")
print(f"datasets={len(files)}")
PY

split_dataset() {
  local source=$1 fpc_train=$2 codebook_train=$3 test=$4 meta=$5
  "$python_bin" - "$source" "$fpc_train" "$codebook_train" "$test" "$meta" \
    "$fpc_percent" "$codebook_percent" "$limit_mib" <<'PY'
from pathlib import Path
import json, sys
src, fpc, codebook, test, meta = map(Path, sys.argv[1:6])
fpc_pct, cb_pct, limit_mib = map(int, sys.argv[6:9])
usable = src.stat().st_size
if limit_mib:
    usable = min(usable, limit_mib * 1024 * 1024)
usable = usable // 4096 * 4096
if usable < 3 * 4096:
    raise SystemExit(f"{src} needs at least three complete 4KiB regions")
fpc_bytes = usable * fpc_pct // 100 // 4096 * 4096
cb_bytes = usable * cb_pct // 100 // 4096 * 4096
test_bytes = usable - fpc_bytes - cb_bytes
if min(fpc_bytes, cb_bytes, test_bytes) <= 0:
    raise SystemExit("empty split; adjust percentages or LIMIT_MIB")
for path in (fpc, codebook, test):
    path.parent.mkdir(parents=True, exist_ok=True)
with src.open("rb") as inp:
    for path, size in ((fpc, fpc_bytes), (codebook, cb_bytes), (test, test_bytes)):
        with path.open("wb") as out:
            remaining = size
            while remaining:
                block = inp.read(min(64 * 1024 * 1024, remaining))
                if not block: raise RuntimeError("unexpected EOF")
                out.write(block); remaining -= len(block)
info = {"source": str(src), "usable_bytes": usable,
        "fpc_train_regions": fpc_bytes // 4096,
        "codebook_train_regions": cb_bytes // 4096,
        "test_regions": test_bytes // 4096}
meta.write_text(json.dumps(info, indent=2), encoding="utf-8")
print(info)
PY
}

summaries=()
while IFS=$'\t' read -r name source; do
  [[ -n "$name" ]] || continue
  work="$output_dir/$name"
  mkdir -p "$work/prepared"
  fpc_trace="$work/prepared/fpc-train.trace"
  codebook_trace="$work/prepared/codebook-train.trace"
  test_trace="$work/prepared/test.trace"
  split_dataset "$source" "$fpc_trace" "$codebook_trace" "$test_trace" "$work/split.json"

  model="$work/fpc-bsel-top256.model"
  codebook_payload="$work/codebook-train.payloads"
  test_payload="$work/test.payloads"
  echo "[dataset:$name] train latest Top256 FPC+BSEL"
  fpc_train_start=$SECONDS
  "$exe" train-stream "$fpc_trace" "$model" --max-patterns 256 | tee "$work/fpc-train.log"
  echo "fpc_training_seconds=$((SECONDS - fpc_train_start))" | tee "$work/timing.log"
  if ((roundtrip)); then
    "$exe" roundtrip-stream "$model" "$test_trace" | tee "$work/fpc-roundtrip.log"
  fi
  "$exe" payloads-256 "$model" "$codebook_trace" "$codebook_payload"
  "$exe" payloads-256 "$model" "$test_trace" "$test_payload"

  for buckets in 8 4; do
    echo "[dataset:$name] codebook pruning, buckets=$buckets"
    codebook_start=$SECONDS
    "$python_bin" "$repo/tools/experiment_codebook_pruning.py" \
      --train-payloads "$codebook_payload" --payloads "$test_payload" \
      --buckets "$buckets" --lz-candidates "$lz_candidates" \
      | tee "$work/pruning-b${buckets}.json"
    echo "codebook_pruning_b${buckets}_seconds=$((SECONDS - codebook_start))" | tee -a "$work/timing.log"
  done

  mkdir -p "$work/prefix/dynamic128" "$work/prefix/dynamic128-prev" \
    "$work/prefix/dynamic256" "$work/prefix/dynamic256-prev" \
    "$work/prefix/clock128-prev" "$work/prefix/clock256-prev" \
    "$work/prefix/clock-p23-e128" "$work/prefix/clock-p23-e256" \
    "$work/prefix/clock-p23-e512" "$work/prefix/implicit"
  echo "[dataset:$name] dynamic causal prefix baselines"
  "$python_bin" "$repo/tools/experiment_dynamic_prefix.py" --payloads "$test_payload" \
    --name "$name-dp128" --output-dir "$work/prefix/dynamic128" \
    --entries 128 --resets 16 --word-bytes 4 --prefix-lengths 1,2,3 \
    | tee "$work/prefix/dynamic128.log"
  "$python_bin" "$repo/tools/experiment_dynamic_prefix.py" --payloads "$test_payload" \
    --name "$name-dp128-prev" --output-dir "$work/prefix/dynamic128-prev" \
    --entries 128 --resets 16 --word-bytes 4 --prefix-lengths 1,2,3 --previous-prefix \
    | tee "$work/prefix/dynamic128-prev.log"
  "$python_bin" "$repo/tools/experiment_dynamic_prefix.py" --payloads "$test_payload" \
    --name "$name-dp256" --output-dir "$work/prefix/dynamic256" \
    --entries 256 --resets 16 --word-bytes 4 --prefix-lengths 1,2,3 \
    | tee "$work/prefix/dynamic256.log"
  "$python_bin" "$repo/tools/experiment_dynamic_prefix.py" --payloads "$test_payload" \
    --name "$name-dp256-prev" --output-dir "$work/prefix/dynamic256-prev" \
    --entries 256 --resets 16 --word-bytes 4 --prefix-lengths 1,2,3 --previous-prefix \
    | tee "$work/prefix/dynamic256-prev.log"
  "$python_bin" "$repo/tools/experiment_dynamic_prefix.py" --payloads "$test_payload" \
    --name "$name-clock128-prev" --output-dir "$work/prefix/clock128-prev" \
    --entries 128 --resets 16 --word-bytes 4 --prefix-lengths 1,2,3 \
    --previous-prefix --cache-policy clock | tee "$work/prefix/clock128-prev.log"
  "$python_bin" "$repo/tools/experiment_dynamic_prefix.py" --payloads "$test_payload" \
    --name "$name-clock256-prev" --output-dir "$work/prefix/clock256-prev" \
    --entries 256 --resets 16 --word-bytes 4 --prefix-lengths 1,2,3 \
    --previous-prefix --cache-policy clock | tee "$work/prefix/clock256-prev.log"
  for entries in 128 256 512; do
    "$python_bin" "$repo/tools/experiment_dynamic_prefix.py" --payloads "$test_payload" \
      --name "$name-clock-p23-e$entries" --output-dir "$work/prefix/clock-p23-e$entries" \
      --entries "$entries" --resets 16 --word-bytes 4 --prefix-lengths 2,3 \
      --previous-prefix --cache-policy clock | tee "$work/prefix/clock-p23-e$entries.log"
  done
  "$python_bin" "$repo/tools/experiment_prefix_profile_selector.py" \
    --input-dir "$work/prefix" --name-prefix "$name-clock-p23" --max-profiles 3 \
    --output "$work/prefix/profile-selector.json" | tee "$work/prefix/profile-selector.log"

  echo "[dataset:$name] zero-codebook implicit prefix baselines"
  "$python_bin" "$repo/tools/experiment_implicit_prefix_methods.py" --payloads "$test_payload" \
    --name "$name" --output-dir "$work/prefix/implicit" \
    | tee "$work/prefix/implicit.log"

  summary="$work/codebook-summary.csv"
  "$python_bin" - "$repo/tools" "$name" "$codebook_payload" "$test_payload" \
    "$work/pruning-b8.json" "$work/pruning-b4.json" \
    "$work/prefix/dynamic128/$name-dp128-dynamic-prefix-summary.json" \
    "$work/prefix/dynamic128-prev/$name-dp128-prev-dynamic-prefix-summary.json" \
    "$work/prefix/dynamic256/$name-dp256-dynamic-prefix-summary.json" \
    "$work/prefix/dynamic256-prev/$name-dp256-prev-dynamic-prefix-summary.json" \
    "$work/prefix/clock128-prev/$name-clock128-prev-dynamic-prefix-summary.json" \
    "$work/prefix/clock256-prev/$name-clock256-prev-dynamic-prefix-summary.json" \
    "$work/prefix/clock-p23-e128/$name-clock-p23-e128-dynamic-prefix-summary.json" \
    "$work/prefix/clock-p23-e256/$name-clock-p23-e256-dynamic-prefix-summary.json" \
    "$work/prefix/clock-p23-e512/$name-clock-p23-e512-dynamic-prefix-summary.json" \
    "$work/prefix/profile-selector.json" \
    "$work/prefix/implicit/$name-implicit-prefix-summary.json" "$summary" <<'PY'
from pathlib import Path
import csv, json, sys
tools, name, train_path, test_path, b8_path, b4_path, dp128_path, dp128p_path, dp256_path, dp256p_path, clock128p_path, clock256p_path, p23e128_path, p23e256_path, p23e512_path, selector_path, implicit_path, output = sys.argv[1:]
def load_json(path):
    text = Path(path).read_text(encoding="utf-8")
    return json.loads(text)
b8, b4 = load_json(b8_path), load_json(b4_path)
tiers = ("4K->3K", "3K->2K", "2K->1K")
rows = []
def repeat_counts():
    # 1K->0K is included in the method metrics through the repeat dictionary;
    # derive the count from the corresponding full/pruned crossing totals.
    # The pruning JSON reports only the three non-zero destination tiers, so
    # count repeat hits independently from the exact before/after byte delta.
    sys.path.insert(0, tools) if tools not in sys.path else None
    from experiment_2k_to_1k_algorithms import read_payloads
    from collections import Counter
    train, test = read_payloads(Path(train_path)), read_payloads(Path(test_path))
    counts = Counter(payload for region in train for payload in region.payloads)
    banks = {n: {value for value, _count in counts.most_common(n)} for n in (4, 256)}
    return {n: sum(0 < len(r.raw_blob()) <= 1024 and len(set(r.payloads)) == 1 and r.payloads[0] in bank for r in test)
            for n, bank in banks.items()}
repeats = repeat_counts()
reference = b8["method_metrics"]["top256-full"]
original = int(reference["original_bytes"])
baseline_alg = int(reference["algorithm_before_bytes"])
baseline_quant = int(reference["quantized_before_bytes"])
def add_plain(method, algorithm_bytes, quantized_bytes):
    rows.append({"dataset": name, "method": method,
        "4K->3K": 0, "3K->2K": 0, "2K->1K": 0, "1K->0K": 0, "total": 0,
        "algorithm_before_bytes": original, "algorithm_after_bytes": algorithm_bytes,
        "algorithm_compression_ratio": algorithm_bytes / original if original else 0.0,
        "algorithm_ratio_including_codebook": algorithm_bytes / original if original else 0.0,
        "algorithm_saving_rate": 1.0 - algorithm_bytes / original if original else 0.0,
        "quantized_before_bytes": original, "quantized_after_bytes": quantized_bytes,
        "quantized_compression_ratio": quantized_bytes / original if original else 0.0,
        "quantized_ratio_including_codebook": quantized_bytes / original if original else 0.0,
        "quantized_saving_rate": 1.0 - quantized_bytes / original if original else 0.0,
        "repeat_dictionary_bytes": 0, "huffman_tables": 0,
        "huffman_code_lengths_bytes": 0, "huffman_decoder_aux_bytes": 0,
        "huffman_table_bytes": 0, "total_static_codebook_bytes": 0,
        "direct_rom_bytes": 0, "runtime_state_bytes": 0})
add_plain("raw-4k", original, original)
add_plain("fpc-bsel", baseline_alg, baseline_quant)
def add(method, result, repeat_count, metrics):
    crossings = [int(result.get(tier, 0)) for tier in tiers] + [repeat_count]
    rows.append({"dataset": name, "method": method,
        "4K->3K": crossings[0], "3K->2K": crossings[1],
        "2K->1K": crossings[2], "1K->0K": crossings[3],
        "total": sum(crossings),
        "algorithm_before_bytes": metrics["algorithm_before_bytes"],
        "algorithm_after_bytes": metrics["algorithm_after_bytes"],
        "algorithm_compression_ratio": metrics["algorithm_compression_ratio"],
        "algorithm_ratio_including_codebook": metrics["algorithm_ratio_including_codebook"],
        "algorithm_saving_rate": metrics["algorithm_saving_rate"],
        "quantized_before_bytes": metrics["quantized_before_bytes"],
        "quantized_after_bytes": metrics["quantized_after_bytes"],
        "quantized_compression_ratio": metrics["quantized_compression_ratio"],
        "quantized_ratio_including_codebook": metrics["quantized_ratio_including_codebook"],
        "quantized_saving_rate": metrics["quantized_saving_rate"],
        "repeat_dictionary_bytes": metrics["repeat_dictionary_bytes"],
        "huffman_tables": metrics["huffman_tables"],
        "huffman_code_lengths_bytes": metrics["huffman_code_lengths_bytes"],
        "huffman_decoder_aux_bytes": metrics["huffman_decoder_aux_bytes"],
        "huffman_table_bytes": metrics["huffman_table_bytes"],
        "total_static_codebook_bytes": metrics["total_static_codebook_bytes"],
        "direct_rom_bytes": metrics["direct_rom_bytes"],
        "runtime_state_bytes": 2048})
add("top256-full8", b8["full_test_crossings"], repeats[256], b8["method_metrics"]["top256-full"])
add("top4-full8", b8["full_test_crossings"], repeats[4], b8["method_metrics"]["top4-full"])
add("top4-pruned8", b8["selected_test_crossings"], repeats[4], b8["method_metrics"]["top4-pruned"])
add("top4-pruned4", b4["selected_test_crossings"], repeats[4], b4["method_metrics"]["top4-pruned"])

def add_prefix(summary, runtime_state):
    by_source = {4: 0, 3: 0, 2: 0, 1: 0}
    for transition, count in summary.get("transitions", {}).items():
        source_text, target_text = transition.split("->")
        source, target = int(source_text[:-1]), int(target_text[:-1])
        if source in by_source and target < source:
            by_source[source] += int(count)
    original = int(summary["regions"]) * 4096
    alg_after = int(summary["algorithm_bytes_after"])
    quant_after = int(summary["quantized_bytes_after"])
    rows.append({"dataset": name, "method": summary["method"],
        "4K->3K": by_source[4], "3K->2K": by_source[3],
        "2K->1K": by_source[2], "1K->0K": by_source[1],
        "total": sum(by_source.values()),
        "algorithm_before_bytes": summary["algorithm_bytes_before"],
        "algorithm_after_bytes": alg_after,
        "algorithm_compression_ratio": summary["algorithm_ratio_after"],
        "algorithm_ratio_including_codebook": alg_after / original if original else 0.0,
        "algorithm_saving_rate": 1.0 - alg_after / original if original else 0.0,
        "quantized_before_bytes": summary["quantized_bytes_before"],
        "quantized_after_bytes": quant_after,
        "quantized_compression_ratio": summary["quantized_ratio_after"],
        "quantized_ratio_including_codebook": quant_after / original if original else 0.0,
        "quantized_saving_rate": 1.0 - quant_after / original if original else 0.0,
        "repeat_dictionary_bytes": 0, "huffman_tables": 0,
        "huffman_code_lengths_bytes": 0, "huffman_decoder_aux_bytes": 0,
        "huffman_table_bytes": 0, "total_static_codebook_bytes": 0,
        "direct_rom_bytes": 0, "runtime_state_bytes": runtime_state})

for path, state in ((dp128_path, 512), (dp128p_path, 516), (dp256_path, 1024),
                    (dp256p_path, 1028), (clock128p_path, 548), (clock256p_path, 1092),
                    (p23e128_path, 548), (p23e256_path, 1092), (p23e512_path, 2180)):
    for summary_row in load_json(path):
        add_prefix(summary_row, state)
for selected in load_json(selector_path):
    add_prefix({
        "method": f'prefix-profile-selector-{selected["profiles"]}',
        "regions": reference["regions"],
        "algorithm_bytes_before": baseline_alg,
        "algorithm_bytes_after": selected["algorithm_bytes_after"],
        "algorithm_ratio_after": selected["algorithm_ratio"],
        "quantized_bytes_before": baseline_quant,
        "quantized_bytes_after": selected["quantized_bytes_after"],
        "quantized_ratio_after": selected["quantized_ratio"],
        "transitions": selected["transitions"],
    }, 2180)
def implicit_state(method):
    if method.startswith("block-anchor-"): return 4
    if method.startswith("multi-anchor-"):
        return 16 if method.endswith("-a4") else 8
    if method.startswith("prefix-run-"): return 5
    if method.startswith(("xor-leading-w4", "delta-prefix-w4")): return 4
    if method.startswith(("xor-leading-w8", "delta-prefix-w8")): return 8
    if method.startswith("subline-bitmap-w4"): return 64
    if method.startswith("subline-bitmap-w8"): return 128
    return 0
for summary_row in load_json(implicit_path):
    add_prefix(summary_row, implicit_state(summary_row["method"]))
with open(output, "w", newline="", encoding="utf-8") as out:
    writer = csv.DictWriter(out, fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
PY
  summaries+=("$summary")
done < "$dataset_list"

combined="$output_dir/all-codebook-summary.csv"
head -n 1 "${summaries[0]}" > "$combined"
for summary in "${summaries[@]}"; do tail -n +2 "$summary" >> "$combined"; done

echo
echo "================ CODEBOOK PRUNING SUMMARY ================"
"$python_bin" - "$combined" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], newline="", encoding="utf-8")))
print(f'{"dataset":20} {"method":30} {"4K->3K":>8} {"3K->2K":>8} {"2K->1K":>8} {"1K->0K":>8} {"total":>7} {"alg_ratio":>10} {"quant_ratio":>11} {"codebook_B":>10} {"state_B":>8}')
for r in rows:
    print(f'{r["dataset"][:20]:20} {r["method"][:30]:30} {r["4K->3K"]:>8} {r["3K->2K"]:>8} {r["2K->1K"]:>8} {r["1K->0K"]:>8} {r["total"]:>7} {float(r["algorithm_compression_ratio"]):10.4%} {float(r["quantized_compression_ratio"]):11.4%} {r["total_static_codebook_bytes"]:>10} {r["runtime_state_bytes"]:>8}')
print(f"combined_csv={sys.argv[1]}")
PY
