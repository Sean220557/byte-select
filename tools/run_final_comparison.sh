#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: bash tools/run_final_comparison.sh DATASET_DIR [OUTPUT_DIR]

Prefix-only pipeline. It builds FPC+BSEL, trains one model per dataset, and
compares two paths on the same test split:
  1. FPC+BSEL + dynamic 2B/3B CLOCK prefix cache
  2. pure FPC + dynamic 2B/3B CLOCK prefix cache

Outputs:
  prefix-all-caches.csv       every dataset/path/cache capacity
  prefix-best-results.csv     crossing-first winner per dataset/path
  prefix-report.md            compact human-readable report
  timing.csv                  build/train/export/prefix elapsed time
  <dataset>/split.json        exact train/test split
  <dataset>/<path>/...        round-trip logs, summaries, and region CSVs

Environment:
  PREFIX_CACHE_ENTRIES=64,128,256,512
  TRAIN_PERCENT=20       FPC+BSEL training prefix; remainder is sealed test
  LIMIT_MIB=0            0 uses the complete input file
  JOBS=<nproc>           compiler jobs
  SKIP_BUILD=0           reuse existing Linux build when 1
  ROUNDTRIP=1            verify FPC+BSEL and pure-FPC test streams
EOF
}

if [[ ${1:-} == -h || ${1:-} == --help ]]; then usage; exit 0; fi
[[ $# -ge 1 && $# -le 2 ]] || { usage >&2; exit 2; }

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export PYTHONPATH="$repo/tools${PYTHONPATH:+:$PYTHONPATH}"
dataset_dir=$(realpath "$1")
output_dir=${2:-"$repo/results/prefix-only"}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")

train_percent=${TRAIN_PERCENT:-20}
limit_mib=${LIMIT_MIB:-0}
jobs=${JOBS:-$(nproc)}
skip_build=${SKIP_BUILD:-0}
roundtrip=${ROUNDTRIP:-1}
cache_entries_csv=${PREFIX_CACHE_ENTRIES:-64,128,256,512}

[[ -d "$dataset_dir" ]] || { echo "not a dataset directory: $dataset_dir" >&2; exit 2; }
for value in "$train_percent" "$limit_mib" "$jobs"; do
  [[ "$value" =~ ^[0-9]+$ ]] || { echo "numeric settings must be non-negative integers" >&2; exit 2; }
done
((train_percent > 0 && train_percent < 100 && jobs > 0)) || { echo "invalid TRAIN_PERCENT or JOBS" >&2; exit 2; }
[[ "$skip_build" == 0 || "$skip_build" == 1 ]] || { echo "SKIP_BUILD must be 0 or 1" >&2; exit 2; }
[[ "$roundtrip" == 0 || "$roundtrip" == 1 ]] || { echo "ROUNDTRIP must be 0 or 1" >&2; exit 2; }

python_bin=""
for candidate in python3.12 python3.11 python3.10 python3.9 python3.8 python3; do
  if command -v "$candidate" >/dev/null; then python_bin=$candidate; break; fi
done
[[ -n "$python_bin" ]] || { echo "python3.8+ is required" >&2; exit 2; }
command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 2; }

"$python_bin" - <<'PY'
import experiment_2k_to_1k_algorithms
import experiment_dynamic_prefix
print("python_import_preflight=ok")
PY

IFS=',' read -r -a cache_entries <<< "$cache_entries_csv"
for entries in "${cache_entries[@]}"; do
  [[ "$entries" =~ ^[0-9]+$ ]] || { echo "invalid cache capacity: $entries" >&2; exit 2; }
  ((entries > 0 && (entries & (entries - 1)) == 0)) || { echo "cache capacity must be a power of two: $entries" >&2; exit 2; }
done

build_dir="$repo/fpc-bsel.v2/build-linux-prefix-only"
exe="$build_dir/fpc-bsel-v2"
build_seconds=0
if [[ "$skip_build" == 0 ]]; then
  start=$SECONDS
  cmake -S "$repo/fpc-bsel.v2" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" -j "$jobs"
  ctest --test-dir "$build_dir" --output-on-failure
  build_seconds=$((SECONDS-start))
fi
[[ -x "$exe" ]] || { echo "missing executable: $exe" >&2; exit 1; }

dataset_list="$output_dir/datasets.tsv"
"$python_bin" - "$dataset_dir" "$dataset_list" <<'PY'
from pathlib import Path
import sys
root,output=map(Path,sys.argv[1:])
files=[p for p in sorted(root.rglob('*')) if p.is_file() and p.suffix.lower() in {'.trace','.bin','.dat','.log'}]
if not files: raise SystemExit('no .trace/.bin/.dat/.log dataset files found')
with output.open('w',encoding='utf8') as out:
    for path in files:
        rel=path.relative_to(root).with_suffix('')
        name=str(rel).replace('/','__').replace('\\','__').replace(' ','_')
        out.write(f'{name}\t{path.resolve()}\n')
print(f'datasets={len(files)}')
PY

split_dataset() {
  local source=$1 train=$2 test=$3 meta=$4
  "$python_bin" - "$source" "$train" "$test" "$meta" "$train_percent" "$limit_mib" <<'PY'
from pathlib import Path
import json,sys
src,train,test,meta=map(Path,sys.argv[1:5]); train_pct,limit_mib=map(int,sys.argv[5:7])
usable=src.stat().st_size
if limit_mib: usable=min(usable,limit_mib*1024*1024)
usable=usable//4096*4096
if usable<2*4096: raise SystemExit(f'{src} needs at least two complete 4KiB regions')
train_bytes=usable*train_pct//100//4096*4096; test_bytes=usable-train_bytes
if min(train_bytes,test_bytes)<=0: raise SystemExit('empty train/test split')
for p in (train,test): p.parent.mkdir(parents=True,exist_ok=True)
with src.open('rb') as inp:
    for path,size in ((train,train_bytes),(test,test_bytes)):
        with path.open('wb') as out:
            left=size
            while left:
                block=inp.read(min(left,64*1024*1024))
                if not block: raise RuntimeError('unexpected EOF')
                out.write(block); left-=len(block)
info={'source':str(src),'usable_bytes':usable,'train_regions':train_bytes//4096,'test_regions':test_bytes//4096}
meta.write_text(json.dumps(info,indent=2),encoding='utf8'); print(info)
PY
}

timing_raw="$output_dir/timing.raw.csv"
echo 'dataset,stage,seconds' > "$timing_raw"
echo "__all__,build,$build_seconds" >> "$timing_raw"

while IFS=$'\t' read -r name source; do
  [[ -n "$name" ]] || continue
  work="$output_dir/$name"; mkdir -p "$work/prepared"
  train_trace="$work/prepared/train.trace"; test_trace="$work/prepared/test.trace"
  split_dataset "$source" "$train_trace" "$test_trace" "$work/split.json"

  bsel_model="$work/fpc-bsel.model"; pure_model="$work/pure-fpc.model"
  start=$SECONDS
  "$exe" train-stream "$train_trace" "$bsel_model" --max-patterns 256 | tee "$work/fpc-bsel-train.log"
  echo "$name,fpc_bsel_train,$((SECONDS-start))" >> "$timing_raw"
  "$python_bin" "$repo/tools/make_empty_fpc_model.py" "$pure_model"

  for path in fpc-bsel pure-fpc; do
    [[ "$path" == fpc-bsel ]] && model=$bsel_model || model=$pure_model
    path_dir="$work/$path"; mkdir -p "$path_dir"
    if [[ "$roundtrip" == 1 ]]; then
      "$exe" roundtrip-stream "$model" "$test_trace" | tee "$path_dir/roundtrip.log"
    fi
    start=$SECONDS
    "$exe" payloads-256 "$model" "$test_trace" "$path_dir/test.payloads"
    echo "$name,${path}_export,$((SECONDS-start))" >> "$timing_raw"
    for entries in "${cache_entries[@]}"; do
      run_dir="$path_dir/cache-e$entries"; mkdir -p "$run_dir"; start=$SECONDS
      "$python_bin" "$repo/tools/experiment_dynamic_prefix.py" \
        --payloads "$path_dir/test.payloads" --name "$name-$path-e$entries" --output-dir "$run_dir" \
        --entries "$entries" --resets 16 --word-bytes 4 --prefix-lengths 2,3 \
        --previous-prefix --cache-policy clock | tee "$run_dir/run.log"
      echo "$name,${path}_prefix_e${entries},$((SECONDS-start))" >> "$timing_raw"
    done
  done
done < "$dataset_list"

"$python_bin" - "$output_dir" "$timing_raw" <<'PY'
from pathlib import Path
import csv,json,sys
root,timing_raw=map(Path,sys.argv[1:]); rows=[]
def tier_counts(transitions):
    out={k:0 for k in ('4K->3K','3K->2K','2K->1K','1K->0K')}
    for label,count in transitions.items():
        a,b=label.split('->'); source,target=int(a[:-1]),int(b[:-1]); direct=f'{source}K->{target}K'
        if direct in out and target<source: out[direct]+=int(count)
    return out
for work in sorted(p for p in root.iterdir() if p.is_dir() and (p/'fpc-bsel.model').exists()):
    dataset=work.name; bsel_bytes=(work/'fpc-bsel.model').stat().st_size
    for path in ('fpc-bsel','pure-fpc'):
        static=bsel_bytes if path=='fpc-bsel' else 0
        for summary_path in sorted((work/path).glob('cache-e*/*-dynamic-prefix-summary.json')):
            for s in json.loads(summary_path.read_text(encoding='utf8')):
                stats=s['cache_stats']; counts=tier_counts(s['transitions']); original=int(s['regions'])*4096
                contribution={f'{tier}_gain_points':count*1024/original*100 for tier,count in counts.items()}
                state=int(s['runtime_state_bytes'])
                rows.append({'dataset':dataset,'path':path,'cache_entries':s['cache_entries'],
                  **counts,**contribution,'total_crossings':sum(counts.values()),'regions':s['regions'],
                  'algorithm_before_bytes':s['algorithm_bytes_before'],'algorithm_after_bytes':s['algorithm_bytes_after'],
                  'algorithm_ratio_before':s['algorithm_ratio_before'],'algorithm_ratio_after':s['algorithm_ratio_after'],
                  'algorithm_gain_points':(s['algorithm_ratio_before']-s['algorithm_ratio_after'])*100,
                  'quantized_before_bytes':s['quantized_bytes_before'],'quantized_after_bytes':s['quantized_bytes_after'],
                  'quantized_ratio_before':s['quantized_ratio_before'],'quantized_ratio_after':s['quantized_ratio_after'],
                  'quantized_gain_points':(s['quantized_ratio_before']-s['quantized_ratio_after'])*100,
                  'static_codebook_bytes':static,'cache_entry_bytes':s['cache_entry_bytes'],
                  'clock_score_bytes':s['clock_score_bytes'],'predictor_bytes':s['predictor_bytes'],
                  'control_bytes':s['control_bytes'],'runtime_state_bytes':state,'total_extra_bytes':static+state,
                  'algorithm_ratio_with_codebook':(s['algorithm_bytes_after']+static)/original,
                  'quantized_ratio_with_codebook':(s['quantized_bytes_after']+static)/original,
                  'lookups':stats['lookups'],'hits':stats['hits'],'misses':stats['misses'],'hit_rate':stats['hit_rate'],
                  'hits_2B':stats['hits_by_prefix_length'].get('2',0),'hits_3B':stats['hits_by_prefix_length'].get('3',0),
                  'insertions':stats['insertions'],'evictions':stats['evictions'],'resets':stats['resets'],
                  'peak_entries':stats['peak_entries'],'average_occupancy':stats['average_occupancy'],
                  'occupancy_ratio':stats['occupancy_ratio'],'roundtrip':s['roundtrip']})
if not rows: raise SystemExit('no prefix results found')
rows.sort(key=lambda r:(r['dataset'],r['path'],int(r['cache_entries'])))
all_csv=root/'prefix-all-caches.csv'
with all_csv.open('w',newline='',encoding='utf8') as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
best=[]
for dataset in sorted({r['dataset'] for r in rows}):
    for path in ('fpc-bsel','pure-fpc'):
        candidates=[r for r in rows if r['dataset']==dataset and r['path']==path]
        winner=max(candidates,key=lambda r:(r['total_crossings'],-r['quantized_after_bytes'],-r['algorithm_after_bytes'],-r['total_extra_bytes']))
        best.append(winner)
with (root/'prefix-best-results.csv').open('w',newline='',encoding='utf8') as f:
    w=csv.DictWriter(f,fieldnames=list(best[0])); w.writeheader(); w.writerows(best)
timing_rows=list(csv.DictReader(timing_raw.open(encoding='utf8')))
with (root/'timing.csv').open('w',newline='',encoding='utf8') as f:
    w=csv.DictWriter(f,fieldnames=('dataset','stage','seconds')); w.writeheader(); w.writerows(timing_rows)
lines=['# Prefix-only compression results','',
'| Dataset | Path | Cache | 4K→3K | 3K→2K | 2K→1K | 1K→0K | Total | Algorithm ratio | Quantized ratio | Static codebook | Cache state | Total extra |',
'|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
for r in best:
    lines.append(f'| {r["dataset"]} | {r["path"]}+prefix | {r["cache_entries"]} | {r["4K->3K"]} | {r["3K->2K"]} | {r["2K->1K"]} | {r["1K->0K"]} | {r["total_crossings"]} | {r["algorithm_ratio_after"]:.4%} | {r["quantized_ratio_after"]:.4%} | {r["static_codebook_bytes"]} B | {r["runtime_state_bytes"]} B | {r["total_extra_bytes"]} B |')
lines += ['', '## Cache locality','',
'| Dataset | Path | Entries | Hit rate | 2B hits | 3B hits | Avg occupancy | Evictions | Crossings |',
'|---|---|---:|---:|---:|---:|---:|---:|---:|']
for r in rows:
    lines.append(f'| {r["dataset"]} | {r["path"]} | {r["cache_entries"]} | {r["hit_rate"]:.3%} | {r["hits_2B"]} | {r["hits_3B"]} | {r["average_occupancy"]:.1f}/{r["cache_entries"]} | {r["evictions"]} | {r["total_crossings"]} |')
lines += ['', '## Quantized contribution by tier','',
'| Dataset | Path | 4K→3K | 3K→2K | 2K→1K | 1K→0K | Total gain |',
'|---|---|---:|---:|---:|---:|---:|']
for r in best:
    lines.append(f'| {r["dataset"]} | {r["path"]} | {r["4K->3K_gain_points"]:.4f} pp | {r["3K->2K_gain_points"]:.4f} pp | {r["2K->1K_gain_points"]:.4f} pp | {r["1K->0K_gain_points"]:.4f} pp | {r["quantized_gain_points"]:.4f} pp |')
lines += ['', '所有配置均按实际payload字节计数并执行round-trip。BSEL静态码本按模型文件实际大小计费；动态缓存按4 B/entry、2-bit CLOCK、previous-word和控制寄存器计费。']
(root/'prefix-report.md').write_text('\n'.join(lines)+'\n',encoding='utf8')
timing_raw.unlink(missing_ok=True)
print(f'all_caches={all_csv}\nbest={root/"prefix-best-results.csv"}\nreport={root/"prefix-report.md"}\ntiming={root/"timing.csv"}')
PY

echo "results=$output_dir/prefix-report.md"
