#!/usr/bin/env bash
# End-to-end algorithm-side reproduction demo: train the paper's three
# Byte-Select presets on GAP Benchmark Suite graph data and compare their
# quantized compression ratios against the FPC, BDI, C-Pack, and BPC size
# estimators on the held-out middle 10% of every trace (the paper's Fig. 11/12
# setup). Requires: a C++11 compiler, python3, and the bsel binary.
#
# Usage:
#   tools/run_gapbs_demo.sh [WORK_DIR]
# Environment overrides:
#   BSEL  - path to the bsel binary (default: build/bsel)
#   SCALES - space-separated "name flag scale" triples, default:
#            "kron16 g 16" "kron18 g 18" "urand18 u 18"
#   OFFSET_THRESHOLD / NEIGHBOR_THRESHOLD - phase-1 pattern count thresholds
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BSEL="${BSEL:-$ROOT/build/bsel}"
WORK="${1:-/tmp/bsel-demo}"
OFFSET_THRESHOLD="${OFFSET_THRESHOLD:-1}"
NEIGHBOR_THRESHOLD="${NEIGHBOR_THRESHOLD:-16}"
DUMP="$WORK/gapbs_dump_trace"
SCALES=( ${SCALES:-kron16 g 16 kron18 g 18 urand18 u 18} )
PRESETS=( "bsel-1024-1024-128" "bsel-256" "bsel-4096" )
mkdir -p "$WORK"

# Build the trace dumper against the GAPBS headers.
c++ -std=c++11 -O3 -Wall -I "$ROOT/gapbs-master/src" \
  "$ROOT/tools/gapbs_dump_trace.cc" -o "$DUMP" || exit 1

RESULTS="$WORK/results.tsv"
: > "$RESULTS"
printf 'trace\tarray\tpreset\tbsel_q\tfpc_q\tbdi_q\thybrid_q\tcpack_q\tbpc_q\n' >> "$RESULTS"

for ((i = 0; i < ${#SCALES[@]}; i += 3)); do
  name="${SCALES[i]}"; flag="${SCALES[i+1]}"; scale="${SCALES[i+2]}"
  echo "== $name"
  "$DUMP" -"$flag" "$scale" -N -o "$WORK/$name-offsets.trace" >/dev/null 2>&1
  "$DUMP" -"$flag" "$scale" -O -o "$WORK/$name-neighbors.trace" >/dev/null 2>&1
  for arr in offsets neighbors; do
    python3 - "$WORK" "$name" "$arr" <<'PYSPLIT'
import sys
work, name, arr = sys.argv[1], sys.argv[2], sys.argv[3]
bs = 64
data = open(f"{work}/{name}-{arr}.trace", "rb").read()
blocks = len(data) // bs
holdout = blocks * 10 // 100          # middle 10% held out for testing
begin = (blocks - holdout) // 2
open(f"{work}/{name}-{arr}.test.bin", "wb").write(
    data[begin * bs:(begin + holdout) * bs])
PYSPLIT
    for preset in "${PRESETS[@]}"; do
      # The paper uses a count threshold of 1024 for CPU traces and 16 for GPU
      # traces. Its traces are orders of magnitude larger than this demo's, so
      # offsets (regular integer data) use threshold 1 and the high-entropy
      # neighbor arrays use the paper's GPU threshold of 16 to keep training
      # tractable.
      if [ "$arr" = "offsets" ]; then th="$OFFSET_THRESHOLD"; else th="$NEIGHBOR_THRESHOLD"; fi
      model="$WORK/$name-$arr-$preset.model"
      if [ ! -f "$model" ]; then
        echo "  train $name $arr $preset (threshold $th)"
        "$BSEL" train "$WORK/$name-$arr.trace" "$model" \
          --block-size 64 --threshold "$th" --paper-config "$preset" \
          --holdout-middle 10 >/dev/null 2>&1
      fi
      bsel_q="$("$BSEL" evaluate "$model" "$WORK/$name-$arr.test.bin" 2>/dev/null |
        sed -n 's/.*quantized_ratio=\([0-9.]*\).*/\1/p' | head -1)"
      base="$("$BSEL" compare "$model" "$WORK/$name-$arr.test.bin" \
        --baseline fpc --baseline bdi --baseline hybrid \
        --baseline cpack --baseline bpc 2>/dev/null |
        sed -n 's/.*algorithm=\([a-z]*\).*quantized_ratio=\([0-9.]*\).*/\1 \2/p')"
      fpc_q=$(printf '%s\n' "$base" | awk '$1=="fpc"{print $2}')
      bdi_q=$(printf '%s\n' "$base" | awk '$1=="bdi"{print $2}')
      hyb_q=$(printf '%s\n' "$base" | awk '$1=="hybrid"{print $2}')
      cpa_q=$(printf '%s\n' "$base" | awk '$1=="cpack"{print $2}')
      bpc_q=$(printf '%s\n' "$base" | awk '$1=="bpc"{print $2}')
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$arr" "$preset" "$bsel_q" "$fpc_q" "$bdi_q" "$hyb_q" "$cpa_q" "$bpc_q" \
        >> "$RESULTS"
    done
  done
done

echo
echo "Quantized compression ratios (higher is better; middle-10% holdout):"
printf '%-8s %-10s %-18s %7s %7s %7s %7s %7s %7s\n' \
  trace array preset bsel fpc bdi hybrid cpack bpc
awk -F'\t' 'NR>1 {printf "%-8s %-10s %-18s %7.3f %7.3f %7.3f %7.3f %7.3f %7.3f\n",
  $1,$2,$3,$4,$5,$6,$7,$8,$9}' "$RESULTS"
echo
echo "Results saved to $RESULTS"
