#!/usr/bin/env python3
"""Metadata-only 1K->0K detector for complete 4KB regions.

0K means no data payload allocation. The region is represented by a compact
metadata tag plus small parameters or a nearby-region reference.
"""
import argparse
import csv
import json
import struct
import sys
from collections import OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import experiment_fpc_payload_lz as pp


REGION = 4096


def iter_regions(path: Path, limit_regions: int):
    with path.open("rb") as f:
        index = 0
        while limit_regions <= 0 or index < limit_regions:
            data = f.read(REGION)
            if not data:
                break
            if len(data) < REGION:
                break
            yield index, data
            index += 1


def all_words(data: bytes, width: int):
    return [int.from_bytes(data[i : i + width], "little") for i in range(0, len(data), width)]


def is_delta(data: bytes, width: int) -> bool:
    values = all_words(data, width)
    if len(values) < 2:
        return False
    delta = values[1] - values[0]
    return all(values[i] - values[i - 1] == delta for i in range(1, len(values)))


def classify(data: bytes, recent: OrderedDict[bytes, int], lookback: int):
    if data == b"\x00" * REGION:
        return "zero", 1
    if data == data[:1] * REGION:
        return "const-byte", 2
    for width in (2, 4, 8):
        tile = data[:width]
        if data == tile * (REGION // width):
            return f"const-{width * 8}", 1 + width
    for width in (8, 16, 32, 64, 128, 256):
        tile = data[:width]
        if data == tile * (REGION // width):
            return f"tile-{width}", 1 + width
    for width in (4, 8):
        if is_delta(data, width):
            return f"delta-{width * 8}", 1 + width * 2
    if lookback and data in recent:
        distance = len(recent) - recent[data]
        if 0 < distance <= lookback:
            return "same-as-nearby", 5
    return "none", 0


def load_baseline(payloads: Path, limit_regions: int):
    if payloads is None:
        return {}
    regions = pp.regions(pp.read_payloads(payloads, limit_regions if limit_regions else 10**12))
    return {r.i: r for r in regions}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("--payloads", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--limit-regions", type=int, default=0)
    ap.add_argument("--lookback", type=int, default=64)
    ap.add_argument("--max-meta-bytes", type=int, default=16)
    ap.add_argument("--count-all-without-payloads", action="store_true")
    ap.add_argument("--output-dir", type=Path, required=True)
    args = ap.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    baseline = load_baseline(args.payloads, args.limit_regions) if args.payloads else {}
    recent: OrderedDict[bytes, int] = OrderedDict()
    rows = []
    for index, data in iter_regions(args.input, args.limit_regions):
        kind, meta_bytes = classify(data, recent, args.lookback)
        r = baseline.get(index)
        before = r.total if r else (1024 if args.count_all_without_payloads else 4096)
        before_tier = r.tier if r else vb.tier(before)
        eligible = before_tier == 1024 and kind != "none" and meta_bytes <= args.max_meta_bytes
        rows.append(
            {
                "dataset": args.name,
                "region": index,
                "before": before,
                "before_tier": before_tier,
                "kind": kind,
                "meta_bytes": meta_bytes,
                "eligible_1k": int(before_tier == 1024),
                "zero_k": int(eligible),
                "saved_bytes": before if eligible else 0,
            }
        )
        recent[data] = index
        while len(recent) > args.lookback:
            recent.popitem(last=False)

    summary = {
        "method": "metadata-only-0k",
        "before_tier": 1024,
        "target": 0,
        "eligible": sum(r["eligible_1k"] for r in rows),
        "crossed": sum(r["zero_k"] for r in rows),
        "changed": sum(r["zero_k"] for r in rows),
        "before_bytes": sum(r["before"] for r in rows if r["eligible_1k"]),
        "after_bytes": 0,
        "saved_bytes": sum(r["saved_bytes"] for r in rows),
    }
    kind_counts = {}
    for row in rows:
        if row["zero_k"]:
            kind_counts[row["kind"]] = kind_counts.get(row["kind"], 0) + 1
    summary["kind_counts"] = json.dumps(kind_counts, sort_keys=True)

    prefix = f"{args.name}-metadata-only-0k"
    with (args.output_dir / f"{prefix}-regions.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)
    with (args.output_dir / f"{prefix}-summary.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=summary.keys())
        w.writeheader()
        w.writerow(summary)
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == "__main__":
    main()
