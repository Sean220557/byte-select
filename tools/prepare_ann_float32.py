#!/usr/bin/env python3
"""Create lossless, cache-line-local byte-shuffled ANN traces.

Every 64-byte line contains 16 float32 values.  The transform changes
    f0[b0,b1,b2,b3], ..., f15[b0,b1,b2,b3]
into four 16-byte byte planes.  It is exactly reversible, preserves file size
and cache-line boundaries, and is applied identically to training and test.
"""

from pathlib import Path
import argparse
import hashlib
import json
import numpy as np


def transform(source: Path, destination: Path) -> dict:
    data = np.fromfile(source, dtype=np.uint8)
    full_size = data.size - data.size % 64
    shuffled = data.copy()
    shuffled[:full_size] = (data[:full_size].reshape(-1, 16, 4)
                            .transpose(0, 2, 1).copy().reshape(-1))
    destination.parent.mkdir(parents=True, exist_ok=True)
    shuffled.tofile(destination)
    restored = shuffled.copy()
    restored[:full_size] = (shuffled[:full_size].reshape(-1, 4, 16)
                            .transpose(0, 2, 1).copy().reshape(-1))
    if not np.array_equal(restored, data):
        raise RuntimeError(f"round-trip verification failed for {source}")
    return {
        "source": source.name,
        "output": destination.name,
        "bytes": int(data.size),
        "transformed_bytes": int(full_size),
        "unchanged_tail_bytes": int(data.size - full_size),
        "source_sha256": hashlib.sha256(data.tobytes()).hexdigest(),
        "output_sha256": hashlib.sha256(shuffled.tobytes()).hexdigest(),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path,
                        default=Path("datasets/ann-benchmarks/benchmark-input"))
    parser.add_argument("--output", type=Path,
                        default=Path("datasets/ann-benchmarks/benchmark-input-byte-shuffle"))
    args = parser.parse_args()
    records = []
    for source in sorted(args.input.glob("*.bin")):
        records.append(transform(source, args.output / source.name))
    (args.output / "manifest.json").write_text(
        json.dumps({"transform": "float32-byte-shuffle-within-64B-line",
                    "records": records}, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(records)} verified files to {args.output}")


if __name__ == "__main__":
    main()
