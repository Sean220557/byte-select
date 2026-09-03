#!/usr/bin/env bash
set -euo pipefail

# Production path: Custom-FPC + 256-entry dynamic prefix optimization.
# No Python, model training, BSEL search, LZ, round-trip, per-subline logging,
# or multi-capacity sweep is executed here.
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec bash "$repo/tools/run_cpp_prefix.sh" "$@"
