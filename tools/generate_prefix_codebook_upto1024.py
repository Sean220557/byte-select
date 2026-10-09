#!/usr/bin/env python3
"""Generate a reproducible synthetic Custom-FPC header codebook (1..1024 entries).

This is an experiment helper. It reuses the tag classifier and synthetic word
distribution from experiment_mphf_128.py, but allows a larger entry count.
"""

import argparse
import importlib.util
import json
from pathlib import Path


def load_generator():
    source = Path(__file__).with_name("experiment_mphf_128.py")
    spec = importlib.util.spec_from_file_location("experiment_mphf_128", source)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {source}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--entries", type=int, required=True)
    parser.add_argument("--seed", type=int, default=0x10241024)
    args = parser.parse_args()
    if not 1 <= args.entries <= 1024:
        parser.error("--entries must be in 1..1024")

    generator = load_generator()
    generator.N = args.entries
    rows = generator.make_codebook(args.seed)
    values = [generator.pack(row) for row in rows]
    if len(set(values)) != args.entries:
        raise RuntimeError("generator produced duplicate packed headers")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "codebook.txt").write_text(
        "".join(f"| {value:016x} |\n" for value in values), encoding="utf-8")
    (args.output_dir / "codebook.bin").write_bytes(
        b"".join(value.to_bytes(8, "big") for value in values))
    (args.output_dir / "codebook.json").write_text(json.dumps({
        "description": "synthetic Custom-FPC 16-tag headers",
        "seed": args.seed,
        "entries": [{"id": i, "tags": list(row), "key_hex": f"{value:016x}"}
                    for i, (row, value) in enumerate(zip(rows, values))],
    }, indent=2) + "\n", encoding="utf-8")
    print(f"entries={args.entries} seed={args.seed} output={args.output_dir}")


if __name__ == "__main__":
    main()
