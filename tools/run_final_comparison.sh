#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: bash tools/run_final_comparison.sh DATASET_DIR [OUTPUT_DIR]

Runs the full final comparison with one FPC+BSEL training pass per dataset.
The exported payloads are reused by the codebook, prefix, and pure-LZ77 tests.
Outputs include exact static codebook bytes, runtime state, training time,
algorithmic/1KiB-quantized ratios, per-tier crossings, and gains over FPC+BSEL.
The public final table keeps only one winner per dataset from each category:
current-best, prefix-best, and pure-lz77-best.
EOF
}
if [[ ${1:-} == "-h" || ${1:-} == "--help" ]]; then usage; exit 0; fi
[[ $# -ge 1 && $# -le 2 ]] || { usage >&2; exit 2; }
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dataset_dir=$(realpath "$1")
output_dir=${2:-"$repo/results/final-comparison"}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")
jobs=${JOBS:-$(nproc)}
lz_candidates=${LZ_CANDIDATES:-64}
skip_build=${SKIP_BUILD:-0}

echo "[phase 1/3] FPC+BSEL once + codebook/prefix paths"
build_args=()
if [[ "$skip_build" == 1 ]]; then build_args+=(--skip-build); fi
bash "$repo/tools/run_large_codebook_pruning.sh" "$dataset_dir" "$output_dir/large" "${build_args[@]}"

python_bin=$(command -v python3.11 || command -v python3.10 || command -v python3 || true)
[[ -n "$python_bin" ]] || { echo "python3 required" >&2; exit 2; }

echo "[phase 2/3] pure LZ77 variants on exported payloads"
while IFS=$'\t' read -r name _source; do
  [[ -n "$name" ]] || continue
  work="$output_dir/large/$name"
  mkdir -p "$work/pure-lz77"
  lz_start=$SECONDS
  "$python_bin" "$repo/tools/experiment_2k_to_1k_algorithms.py" \
    --payloads "$work/test.payloads" --train-payloads "$work/codebook-train.payloads" \
    --name "$name-pure-lz77" --output-dir "$work/pure-lz77" \
    --lz-candidates "$lz_candidates" | tee "$work/pure-lz77.log"
  echo "pure_lz77_training_and_eval_seconds=$((SECONDS - lz_start))" | tee -a "$work/timing.log"
done < "$output_dir/large/datasets.tsv"

echo "[phase 3/3] build final per-tier comparison"
"$python_bin" - "$output_dir/large" "$output_dir/final-comparison.csv" "$output_dir/final-comparison.md" <<'PY'
from pathlib import Path
import csv, json, sys, time
root, out_csv, out_md = map(Path, sys.argv[1:])
rows=[]
for work in sorted(p for p in root.iterdir() if p.is_dir() and (p/'codebook-summary.csv').exists()):
    name=work.name
    with (work/'codebook-summary.csv').open(newline='',encoding='utf8') as f:
        rows.extend(dict(r) for r in csv.DictReader(f))
    pure = work/'pure-lz77'/f'{name}-pure-lz77-2k-to-1k-summary.csv'
    if pure.exists():
        region_path = work/'pure-lz77'/f'{name}-pure-lz77-2k-to-1k-regions.csv'
        region_rows=list(csv.DictReader(region_path.open(newline='',encoding='utf8')))
        for r in csv.DictReader(pure.open(newline='',encoding='utf8')):
            method=r['method']
            if method not in {'bounded-lzss16','lazy-lzss16','optimal-lzss16','short-lzss','bounded-lzss24','lzss24'}:
                continue
            selected=[x for x in region_rows if x['method']==method]
            def q(v): return 0 if v==0 else ((v+1023)//1024)*1024
            original=len(selected)*4096; before=sum(int(x['before_raw']) for x in selected); after=sum(int(x['after_raw']) for x in selected); qb=sum(q(int(x['before_raw'])) for x in selected); qa=sum(q(int(x['after_raw'])) for x in selected)
            counts={k:0 for k in ('4K->3K','3K->2K','2K->1K','1K->0K')}
            for x in selected:
                for label in x['crossed'].split(';'):
                    if label in counts: counts[label]+=1
            rows.append({'dataset':name,'method':'pure-'+method,**counts,'total':sum(counts.values()),'algorithm_before_bytes':before,'algorithm_after_bytes':after,'algorithm_compression_ratio':after/original if original else 0,'algorithm_ratio_including_codebook':after/original if original else 0,'algorithm_saving_rate':1-after/original if original else 0,'quantized_before_bytes':qb,'quantized_after_bytes':qa,'quantized_compression_ratio':qa/original if original else 0,'quantized_ratio_including_codebook':qa/original if original else 0,'quantized_saving_rate':1-qa/original if original else 0,'repeat_dictionary_bytes':0,'huffman_tables':0,'huffman_code_lengths_bytes':0,'huffman_decoder_aux_bytes':0,'huffman_table_bytes':0,'total_static_codebook_bytes':0,'direct_rom_bytes':0,'runtime_state_bytes':2048})
if rows:
    # Keep one winner per category and dataset in the report.  Ranking is
    # crossing-first, then quantized bytes, then algorithm bytes.
    compact=[]
    for dataset in sorted({r['dataset'] for r in rows}):
        group=[r for r in rows if r['dataset']==dataset]
        categories=[
            ('current-best', [r for r in group if r['method']=='top4-pruned8']),
            ('prefix-best', [r for r in group if r['method'].startswith('prefix-profile-selector-')]),
            ('pure-lz77-best', [r for r in group if r['method']=='pure-optimal-lzss16']),
        ]
        for category, candidates in categories:
            if not candidates: continue
            winner=max(candidates, key=lambda r:(int(r['total']), -int(r['quantized_after_bytes'] or 10**30), -int(r['algorithm_after_bytes'] or 10**30)))
            winner=dict(winner); winner['method']=category; compact.append(winner)
    rows=compact
    fields=list(rows[0])
    with out_csv.open('w',newline='',encoding='utf8') as f:
        w=csv.DictWriter(f,fieldnames=fields); w.writeheader(); w.writerows(rows)
lines=['# Final comparison','', '| Dataset | Method | 4K→3K | 3K→2K | 2K→1K | 1K→0K | Total | Algorithm ratio | Quantized ratio | Static codebook | Runtime state |','|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
for r in rows:
    lines.append('| {dataset} | {method} | {4K->3K} | {3K->2K} | {2K->1K} | {1K->0K} | {total} | {algorithm_compression_ratio} | {quantized_compression_ratio} | {total_static_codebook_bytes} B | {runtime_state_bytes} B |'.format(**r))
lines += ['', '算法收益按测试region原始4KiB总量计算；量化收益按1KiB档位向上取整。码本为一次性静态开销，runtime state为每条解压流水线状态。', '', '各数据集的fpc_training_seconds、codebook_pruning_b*_seconds、pure_lz77_training_and_eval_seconds保存在对应目录的timing.log。']
out_md.write_text('\n'.join(lines)+'\n',encoding='utf8')
print(f'csv={out_csv}\nmarkdown={out_md}')
PY
echo "results=$output_dir/final-comparison.md"
