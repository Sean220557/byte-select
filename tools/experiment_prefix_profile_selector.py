#!/usr/bin/env python3
"""Select a small fixed bank of zero-codebook prefix profiles."""
from __future__ import annotations

import argparse
import csv
import itertools
import json
import math
from pathlib import Path


def quantized(size: int) -> int:
    return 0 if size == 0 else math.ceil(size / 1024) * 1024


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input-dir", type=Path, required=True)
    ap.add_argument("--name-prefix", required=True)
    ap.add_argument("--max-profiles", type=int, default=4)
    ap.add_argument("--output", type=Path)
    args = ap.parse_args()

    paths = sorted(args.input_dir.rglob(f"{args.name_prefix}*-regions.csv"))
    if not paths:
        raise SystemExit("no matching region CSV files")
    profiles: dict[str, dict[int, tuple[int, int]]] = {}
    before: dict[int, int] = {}
    for path in paths:
        rows = list(csv.DictReader(path.open(newline="", encoding="utf-8")))
        if not rows:
            continue
        method = path.stem
        values = {}
        for row in rows:
            rid = int(row["region"])
            before[rid] = int(row["before_raw"])
            # One existing mode byte already identifies the selected profile,
            # so selecting among <=256 fixed profiles adds no stream bytes.
            size = int(row["chosen_raw"])
            values[rid] = (quantized(size), size)
        profiles[method] = values

    names = sorted(profiles)
    rows_out = []
    for count in range(1, min(args.max_profiles, len(names)) + 1):
        best = None
        for combo in itertools.combinations(names, count):
            chosen = {rid: min((profiles[name][rid] for name in combo)) for rid in before}
            qbytes = sum(value[0] for value in chosen.values())
            abytes = sum(value[1] for value in chosen.values())
            crossings = sum(quantized(chosen[rid][1]) < quantized(before[rid]) for rid in before)
            candidate = (qbytes, abytes, -crossings, combo)
            if best is None or candidate < best:
                best = candidate
        assert best is not None
        qbytes, abytes, neg_cross, combo = best
        chosen = {rid: min((profiles[name][rid] for name in combo)) for rid in before}
        original = len(before) * 4096
        transitions = {}
        for rid, (_qsize, size) in chosen.items():
            source, target = quantized(before[rid]) // 1024, quantized(size) // 1024
            key = f"{source}K->{target}K"
            transitions[key] = transitions.get(key, 0) + 1
        rows_out.append({
            "profiles": count,
            "selected": list(combo),
            "crossed_regions": -neg_cross,
            "algorithm_bytes_after": abytes,
            "quantized_bytes_after": qbytes,
            "algorithm_ratio": abytes / original,
            "quantized_ratio": qbytes / original,
            "static_codebook_bytes": 0,
            "profile_id_bits": max(1, math.ceil(math.log2(count))),
            "transitions": transitions,
        })
    text = json.dumps(rows_out, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
