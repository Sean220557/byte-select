#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/export_chunk_work_csv.sh CHUNK_WORK [OUTPUT_DIR]

Converts the MCC logs of every fully completed chunk into partial CSV files.
OUTPUT_DIR defaults to CHUNK_WORK's parent directory.
EOF
}

if (($# < 1 || $# > 2)); then
  usage >&2
  exit 2
fi

work=$(realpath "$1")
output_dir=${2:-$(dirname "$work")}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")

[[ -d "$work/logs" ]] || { echo "missing log directory: $work/logs" >&2; exit 1; }

aggregate_algorithm() {
  awk '
    function value(key, i, part) {
      for (i = 2; i <= NF; ++i) {
        split($i, part, "=")
        if (part[1] == key) return part[2] + 0
      }
      return 0
    }
    /^mcc_sizes_before / {
      pending_original = value("original_bytes")
      pending_stored = value("stored_bytes")
      pending_before = value("physical_bytes")
      have_before = 1
      have_v1 = have_v2 = have_v3 = have_v4 = 0
    }
    /^mcc_sizes_v1 / && have_before { pending_v1 = value("physical_bytes"); have_v1 = 1 }
    /^mcc_sizes_v2 / && have_v1 { pending_v2 = value("physical_bytes"); have_v2 = 1 }
    /^mcc_sizes_v3 / && have_v2 { pending_v3 = value("physical_bytes"); have_v3 = 1 }
    /^mcc_sizes_v4 / && have_v3 { pending_v4 = value("physical_bytes"); have_v4 = 1 }
    /^mcc_sizes_v5 / && have_v4 {
      original += pending_original
      stored += pending_stored
      before += pending_before
      v1 += pending_v1
      v2 += pending_v2
      v3 += pending_v3
      v4 += pending_v4
      v5 += value("physical_bytes")
      regions += value("metadata_regions")
      segments += value("candidate_segments_scanned")
      gaps += value("candidate_gaps_scanned")
      chunks++
      have_before = have_v1 = have_v2 = have_v3 = have_v4 = 0
    }
    END {
      if (!chunks) exit 3
      printf "%.0f,%.0f,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.0f,%.0f,%.0f,%.0f,%.0f", \
        original, stored, original/stored, original/before, original/v1, \
        original/v2, original/v3, original/v4, original/v5, v5, regions, \
        segments, gaps, chunks
    }
  ' "$1"
}

summary="$output_dir/partial-final-summary.csv"
echo "algorithm,subline_bytes,original_bytes,stored_bytes,algorithm_ratio,before_ratio,v1_ratio,v2_ratio,v3_ratio,v4_ratio,v5_ratio,v5_physical_bytes,v5_metadata_regions,candidate_segments,candidate_gaps,chunks" > "$summary"

algorithm_count=0
shopt -s nullglob
for log in "$work"/logs/*.log; do
  algorithm=$(basename "$log" .log)
  if row=$(aggregate_algorithm "$log"); then
    printf '%s,256,%s\n' "$algorithm" "$row" >> "$summary"
    algorithm_count=$((algorithm_count + 1))
  else
    echo "warning: no complete chunk in $log" >&2
  fi
done
((algorithm_count > 0)) || { echo "no complete algorithm logs found" >&2; exit 1; }

ablation="$output_dir/partial-mcc-ablation.csv"
echo "setting,guard,lookback,stored_bytes,physical_bytes,quantized_ratio,metadata_regions,candidate_segments,candidate_gaps,chunks" > "$ablation"
ablation_count=0
for log in "$work"/ablation/g*-l*.log; do
  setting=$(basename "$log" .log)
  if [[ "$setting" =~ ^g([0-9]+)-l([0-9]+)$ ]]; then
    guard=${BASH_REMATCH[1]}
    lookback=${BASH_REMATCH[2]}
  else
    echo "warning: unexpected ablation name: $setting" >&2
    continue
  fi
  if row=$(aggregate_algorithm "$log"); then
    IFS=, read -r original stored _ _ _ _ _ _ ratio physical regions segments gaps chunks <<< "$row"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$setting" "$guard" "$lookback" "$stored" "$physical" "$ratio" \
      "$regions" "$segments" "$gaps" "$chunks" >> "$ablation"
    ablation_count=$((ablation_count + 1))
  else
    echo "warning: no complete chunk in $log" >&2
  fi
done

if ((ablation_count == 0)); then
  rm -f "$ablation"
  echo "summary=$summary"
  echo "ablation=not available"
else
  echo "summary=$summary"
  echo "ablation=$ablation"
fi
