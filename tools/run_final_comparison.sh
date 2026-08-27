#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: bash tools/run_final_comparison.sh DATASET_DIR [OUTPUT_DIR]

Complete run: split 4KiB regions, build/test, train FPC+BSEL once, run existing
baselines, compare FPC+BSEL+prefix with pure-FPC+prefix, sweep cache capacities,
verify round-trip, and report codebook/cache storage plus locality statistics.

Environment:
  PREFIX_CACHE_ENTRIES=64,128,256,512
  FPC_TRAIN_PERCENT=20  CODEBOOK_PERCENT=20  LIMIT_MIB=0
  JOBS=<nproc>  LZ_CANDIDATES=64  SKIP_BUILD=0  ROUNDTRIP=1
EOF
}
if [[ ${1:-} == -h || ${1:-} == --help ]]; then usage; exit 0; fi
[[ $# -ge 1 && $# -le 2 ]] || { usage >&2; exit 2; }

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export PYTHONPATH="$repo/tools${PYTHONPATH:+:$PYTHONPATH}"
dataset_dir=$(realpath "$1")
output_dir=${2:-"$repo/results/final-comparison"}
mkdir -p "$output_dir"; output_dir=$(realpath "$output_dir")
cache_entries_csv=${PREFIX_CACHE_ENTRIES:-64,128,256,512}
skip_build=${SKIP_BUILD:-0}; roundtrip=${ROUNDTRIP:-1}

python_bin=""
for candidate in python3.12 python3.11 python3.10 python3.9 python3.8 python3; do
  if command -v "$candidate" >/dev/null; then python_bin=$candidate; break; fi
done
[[ -n "$python_bin" ]] || { echo "python3.8+ is required" >&2; exit 2; }
"$python_bin" - <<'PY'
import experiment_2k_to_1k_algorithms
import experiment_codebook_pruning
import experiment_dynamic_prefix
print("python_import_preflight=ok")
PY

IFS=',' read -r -a cache_entries <<< "$cache_entries_csv"
for entries in "${cache_entries[@]}"; do
  [[ "$entries" =~ ^[0-9]+$ ]] || { echo "invalid cache capacity: $entries" >&2; exit 2; }
  ((entries > 0 && (entries & (entries - 1)) == 0)) || { echo "cache capacity must be power of two: $entries" >&2; exit 2; }
done

echo "[phase 1/4] build, split, train FPC+BSEL once, and run baselines"
bash "$repo/tools/run_large_codebook_pruning.sh" "$dataset_dir" "$output_dir/large"
exe="$repo/fpc-bsel.v2/build-linux-codebook-pruning/fpc-bsel-v2"
[[ -x "$exe" ]] || { echo "missing executable: $exe" >&2; exit 1; }

echo "[phase 2/4] export pure FPC and sweep cache capacities on both paths"
while IFS=$'\t' read -r name _source; do
  [[ -n "$name" ]] || continue
  work="$output_dir/large/$name"; pure_dir="$work/pure-fpc"
  mkdir -p "$pure_dir" "$work/prefix-space/fpc-bsel" "$work/prefix-space/pure-fpc"
  pure_model="$pure_dir/empty-bsel.model"; pure_payload="$pure_dir/test.payloads"
  "$python_bin" "$repo/tools/make_empty_fpc_model.py" "$pure_model"
  if [[ "$roundtrip" == 1 ]]; then
    "$exe" roundtrip-stream "$pure_model" "$work/prepared/test.trace" | tee "$pure_dir/roundtrip.log"
  fi
  "$exe" payloads-256 "$pure_model" "$work/prepared/test.trace" "$pure_payload"
  for entries in "${cache_entries[@]}"; do
    for path in fpc-bsel pure-fpc; do
      [[ "$path" == fpc-bsel ]] && payload="$work/test.payloads" || payload="$pure_payload"
      dir="$work/prefix-space/$path/e$entries"; mkdir -p "$dir"
      "$python_bin" "$repo/tools/experiment_dynamic_prefix.py" \
        --payloads "$payload" --name "$name-$path-p23-e$entries" --output-dir "$dir" \
        --entries "$entries" --resets 16 --word-bytes 4 --prefix-lengths 2,3 \
        --previous-prefix --cache-policy clock | tee "$dir/run.log"
    done
  done
done < "$output_dir/large/datasets.tsv"

echo "[phase 3/4] pure LZ77 reference"
lz_candidates=${LZ_CANDIDATES:-64}
while IFS=$'\t' read -r name _source; do
  [[ -n "$name" ]] || continue
  work="$output_dir/large/$name"; mkdir -p "$work/pure-lz77"; lz_start=$SECONDS
  "$python_bin" "$repo/tools/experiment_2k_to_1k_algorithms.py" \
    --payloads "$work/test.payloads" --train-payloads "$work/codebook-train.payloads" \
    --name "$name-pure-lz77" --output-dir "$work/pure-lz77" \
    --lz-candidates "$lz_candidates" | tee "$work/pure-lz77.log"
  echo "pure_lz77_training_and_eval_seconds=$((SECONDS-lz_start))" | tee -a "$work/timing.log"
done < "$output_dir/large/datasets.tsv"

echo "[phase 4/4] aggregate locality and storage overhead"
"$python_bin" - "$output_dir/large" "$output_dir" <<'PY'
from pathlib import Path
import csv,json,sys
root,output=map(Path,sys.argv[1:]); cache_rows=[]; result_rows=[]
def crossings(transitions):
    out={k:0 for k in ("4K->3K","3K->2K","2K->1K","1K->0K")}
    for transition,count in transitions.items():
        a,b=transition.split("->"); source,target=int(a[:-1]),int(b[:-1]); label=f"{source}K->{target}K"
        if label in out and target<source: out[label]+=int(count)
    return out
for work in sorted(p for p in root.iterdir() if p.is_dir() and (p/'fpc-bsel-top256.model').exists()):
    dataset=work.name; bsel_bytes=(work/'fpc-bsel-top256.model').stat().st_size
    candidates={"fpc-bsel":[],"pure-fpc":[]}
    for path in candidates:
        for summary_path in sorted((work/'prefix-space'/path).glob('e*/*-dynamic-prefix-summary.json')):
            for s in json.loads(summary_path.read_text(encoding='utf8')):
                stats=s['cache_stats']; counts=crossings(s['transitions']); static=bsel_bytes if path=='fpc-bsel' else 0
                state=int(s['runtime_state_bytes']); original=int(s['regions'])*4096
                r={'dataset':dataset,'path':path,'cache_entries':s['cache_entries'],
                   'cache_entry_bytes':s['cache_entry_bytes'],'clock_score_bytes':s['clock_score_bytes'],
                   'predictor_bytes':s['predictor_bytes'],'control_bytes':s['control_bytes'],
                   'runtime_state_bytes':state,'static_codebook_bytes':static,'total_extra_bytes':state+static,
                   'lookups':stats['lookups'],'hits':stats['hits'],'misses':stats['misses'],'hit_rate':stats['hit_rate'],
                   'hits_prefix_2B':stats['hits_by_prefix_length'].get('2',0),
                   'hits_prefix_3B':stats['hits_by_prefix_length'].get('3',0),
                   'insertions':stats['insertions'],'evictions':stats['evictions'],'resets':stats['resets'],
                   'peak_entries':stats['peak_entries'],'average_occupancy':stats['average_occupancy'],
                   'occupancy_ratio':stats['occupancy_ratio'],**counts,'total_crossings':sum(counts.values()),
                   'algorithm_before_bytes':s['algorithm_bytes_before'],'algorithm_after_bytes':s['algorithm_bytes_after'],
                   'algorithm_ratio':s['algorithm_ratio_after'],
                   'algorithm_ratio_with_static_codebook':(s['algorithm_bytes_after']+static)/original,
                   'quantized_before_bytes':s['quantized_bytes_before'],'quantized_after_bytes':s['quantized_bytes_after'],
                   'quantized_ratio':s['quantized_ratio_after'],
                   'quantized_ratio_with_static_codebook':(s['quantized_bytes_after']+static)/original,
                   'roundtrip':s['roundtrip']}
                cache_rows.append(r); candidates[path].append(r)
    for path,rows in candidates.items():
        if rows:
            w=max(rows,key=lambda r:(r['total_crossings'],-r['quantized_after_bytes'],-r['algorithm_after_bytes'],-r['total_extra_bytes']))
            result_rows.append(dict(w,method=f'{path}+prefix-best'))
if not cache_rows: raise SystemExit('no cache sweep results found')
cache_rows.sort(key=lambda r:(r['dataset'],r['path'],int(r['cache_entries'])))
result_rows.sort(key=lambda r:(r['dataset'],r['path']))
with (output/'cache-space-locality.csv').open('w',newline='',encoding='utf8') as f:
    w=csv.DictWriter(f,fieldnames=list(cache_rows[0])); w.writeheader(); w.writerows(cache_rows)
with (output/'prefix-final-comparison.csv').open('w',newline='',encoding='utf8') as f:
    w=csv.DictWriter(f,fieldnames=list(result_rows[0])); w.writeheader(); w.writerows(result_rows)
lines=['# FPC/BSEL prefix comparison','',
'| Dataset | Method | Cache | 4K→3K | 3K→2K | 2K→1K | 1K→0K | Total | Algorithm ratio | Quantized ratio | Static codebook | Cache state | Total extra |',
'|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
for r in result_rows:
    lines.append(f'| {r["dataset"]} | {r["method"]} | {r["cache_entries"]} | {r["4K->3K"]} | {r["3K->2K"]} | {r["2K->1K"]} | {r["1K->0K"]} | {r["total_crossings"]} | {r["algorithm_ratio"]:.4%} | {r["quantized_ratio"]:.4%} | {r["static_codebook_bytes"]} B | {r["runtime_state_bytes"]} B | {r["total_extra_bytes"]} B |')
lines += ['', '## Cache locality','',
'| Dataset | Path | Entries | State | Hit rate | 2B hits | 3B hits | Avg occupancy | Evictions | Crossings |',
'|---|---|---:|---:|---:|---:|---:|---:|---:|---:|']
for r in cache_rows:
    lines.append(f'| {r["dataset"]} | {r["path"]} | {r["cache_entries"]} | {r["runtime_state_bytes"]} B | {r["hit_rate"]:.3%} | {r["hits_prefix_2B"]} | {r["hits_prefix_3B"]} | {r["average_occupancy"]:.1f}/{r["cache_entries"]} | {r["evictions"]} | {r["total_crossings"]} |')
lines += ['', '静态码本按实际模型文件计费。Cache state按4 B/entry、2-bit CLOCK、previous-word寄存器和控制指针计费。完整数据见 `cache-space-locality.csv`。']
(output/'final-comparison.md').write_text('\n'.join(lines)+'\n',encoding='utf8')
print(f'cache_csv={output/"cache-space-locality.csv"}\ncomparison_csv={output/"prefix-final-comparison.csv"}\nmarkdown={output/"final-comparison.md"}')
PY
echo "results=$output_dir/final-comparison.md"
