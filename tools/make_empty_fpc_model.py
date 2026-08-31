#!/usr/bin/env python3
"""Write an FPC-BSEL model with empty BSEL tables for pure-FPC ablations."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    # FPCBSEL1, version 1, then {block_size, pattern_count} for the
    # 16-symbol FPC tag stream and the 64-byte residual stream.
    model = b"FPCBSEL1" + struct.pack("<IIIII", 1, 16, 0, 64, 0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(model)


if __name__ == "__main__":
    main()
