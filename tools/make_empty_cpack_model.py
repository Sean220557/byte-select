#!/usr/bin/env python3
"""Write a zero-pattern C-Pack model for pure C-Pack evaluation."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    # CPKBSEL1, version, then {block_size, pattern_count} for tag and data sets.
    blob = b"CPKBSEL1" + struct.pack("<IIIII", 1, 16, 0, 64, 0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)


if __name__ == "__main__":
    main()
