#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash tools/run_single_path_codecs.sh \
    --trace DATA.trace --fpc-model FPC.model --cpack-model CPACK.model \
    --name DATASET --output-dir results/single-path

Builds current FPC+BSEL and C-Pack+BSEL from source, runs their tests and full
round-trips, exports sizes, then evaluates every independent lossless path.
No per-region or per-subline hybrid selection is performed.

Options:
  --trace FILE
  --fpc-model FILE
  --cpack-model FILE
  --name NAME
  --output-dir DIR       default: results/single-path-codecs
  --header-bytes N       direct-offset header charge; default: 48
  --jobs N               default: nproc
  --skip-build
  -h, --help
EOF
}

trace=""
fpc_model=""
cpack_model=""
name=""
output_dir="results/single-path-codecs"
header_bytes=48
jobs=$(nproc)
skip_build=0

while (($#)); do
  case "$1" in
    --trace) trace=${2:?missing value}; shift 2 ;;
    --fpc-model) fpc_model=${2:?missing value}; shift 2 ;;
    --cpack-model) cpack_model=${2:?missing value}; shift 2 ;;
    --name) name=${2:?missing value}; shift 2 ;;
    --output-dir) output_dir=${2:?missing value}; shift 2 ;;
    --header-bytes) header_bytes=${2:?missing value}; shift 2 ;;
    --jobs) jobs=${2:?missing value}; shift 2 ;;
    --skip-build) skip_build=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$trace" && -n "$fpc_model" && -n "$cpack_model" && -n "$name" ]] || {
  echo "--trace, --fpc-model, --cpack-model, and --name are required" >&2
  exit 2
}
[[ "$header_bytes" =~ ^[0-9]+$ && "$header_bytes" -ge 40 ]] || {
  echo "--header-bytes must be an integer >=40" >&2
  exit 2
}
[[ "$jobs" =~ ^[0-9]+$ && "$jobs" -gt 0 ]] || { echo "invalid --jobs" >&2; exit 2; }

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
resolve_path() { [[ "$1" = /* ]] && printf '%s\n' "$1" || printf '%s/%s\n' "$repo" "$1"; }
trace=$(resolve_path "$trace")
fpc_model=$(resolve_path "$fpc_model")
cpack_model=$(resolve_path "$cpack_model")
output_dir=$(resolve_path "$output_dir")
[[ -f "$trace" ]] || { echo "missing trace: $trace" >&2; exit 1; }
[[ -f "$fpc_model" ]] || { echo "missing FPC model: $fpc_model" >&2; exit 1; }
[[ -f "$cpack_model" ]] || { echo "missing C-Pack model: $cpack_model" >&2; exit 1; }
mkdir -p "$output_dir"

python_bin=""
for candidate in python3.11 python3.10 python3.9 python3.8 python3.7 python3; do
  if command -v "$candidate" >/dev/null; then python_bin=$candidate; break; fi
done
[[ -n "$python_bin" ]] || { echo "python3.7+ is required" >&2; exit 2; }
command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 2; }

fpc_build="$repo/fpc-bsel.v2/build-linux-single-path"
cpack_build="$repo/cpack-bsel.v2/build-linux-single-path"
if ((skip_build == 0)); then
  cmake -S "$repo/fpc-bsel.v2" -B "$fpc_build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$fpc_build" -j "$jobs"
  ctest --test-dir "$fpc_build" --output-on-failure
  cmake -S "$repo/cpack-bsel.v2" -B "$cpack_build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$cpack_build" -j "$jobs"
  ctest --test-dir "$cpack_build" --output-on-failure
fi

fpc_exe="$fpc_build/fpc-bsel-v2"
cpack_exe="$cpack_build/cpack-bsel-v2"
[[ -x "$fpc_exe" ]] || { echo "missing FPC executable: $fpc_exe" >&2; exit 1; }
[[ -x "$cpack_exe" ]] || { echo "missing C-Pack executable: $cpack_exe" >&2; exit 1; }

fpc_payloads="$output_dir/$name-fpc.payloads"
cpack_sizes="$output_dir/$name-cpack.sizes"
"$fpc_exe" roundtrip "$fpc_model" "$trace"
"$fpc_exe" payloads-256 "$fpc_model" "$trace" "$fpc_payloads"
"$cpack_exe" roundtrip "$cpack_model" "$trace"
"$cpack_exe" sizes-256 "$cpack_model" "$trace" "$cpack_sizes"

"$python_bin" "$repo/tools/experiment_single_path_codecs.py" \
  "$trace" "$fpc_payloads" --cpack-sizes "$cpack_sizes" --name "$name" \
  --header-bytes "$header_bytes" --output-dir "$output_dir"

echo "summary: $output_dir/$name-single-path-summary.csv"
echo "regions: $output_dir/$name-single-path-regions.csv"
