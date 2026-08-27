#!/usr/bin/env python3
"""Create a valid model with no BSEL patterns for pure-FPC experiments."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    # magic, version, prefix block/count, residual block/count
    model = b"FPCBSEL1" + struct.pack("<IIIII", 1, 16, 0, 64, 0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(model)


if __name__ == "__main__":
    main()
