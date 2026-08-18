#!/usr/bin/env python3
"""Fixed-slot plus exception layout experiment over FPC+BSEL 256B payloads.

The layout keeps 256B sublines independently addressable in the common case:
subline i starts at region_base + i * slot_bytes. Payloads that do not fit the
chosen slot are represented by a compact exception directory and an exception
area. This script is an accounting experiment: it measures whether this layout
can move regions across 4K/3K/2K/1K allocation tiers after FPC+BSEL.
"""
import argparse
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import experiment_fpc_payload_lz as pp


SLOTS = (16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192)
TIERS = ((4096, 3072), (3072, 2048), (2048, 1024))


def align(value: int, quantum: int) -> int:
    return ((value + quantum - 1) // quantum) * quantum


def exception_cost(original_size: int, payload_len: int, model: str, align_bytes: int) -> int:
    if model == "full256":
        return 256
    if model == "payload":
        return align(original_size, align_bytes)
    if model == "rawpayload":
        return original_size
    raise ValueError(f"unknown exception model: {model}")


def eval_slot(region, slot: int, args):
    exceptions = []
    for idx, size in enumerate(region.sizes):
        if size > slot:
            exceptions.append((idx, size, len(region.payloads[idx])))

    directory = 0
    if exceptions:
        # One mode byte, 16-bit exception bitmap, and fixed-width offset/length
        # entries. This is intentionally conservative but still hardware simple.
        directory = args.mode_bytes + args.bitmap_bytes + len(exceptions) * args.entry_bytes

    exception_bytes = sum(
        exception_cost(size, payload_len, args.exception_model, args.exception_align)
        for _idx, size, payload_len in exceptions
    )
    logical = 16 * slot + directory + exception_bytes
    physical = align(logical, args.region_align)
    return {
        "slot": slot,
        "logical": logical,
        "physical": physical,
        "exceptions": len(exceptions),
        "exception_bytes": exception_bytes,
        "directory_bytes": directory,
    }


def choose(region, args):
    candidates = [eval_slot(region, slot, args) for slot in args.slots]
    if args.allow_baseline:
        candidates.append(
            {
                "slot": 0,
                "logical": sum(region.sizes),
                "physical": region.total,
                "exceptions": 16,
                "exception_bytes": sum(region.sizes),
                "directory_bytes": 0,
            }
        )
    return min(candidates, key=lambda x: (x["physical"], x["logical"], x["exceptions"], x["slot"]))


def summarize(rows):
    out = []
    for tier, target in TIERS:
        selected = [r for r in rows if r["before_tier"] == tier and 0 < r["gap"] <= r["max_gap"]]
        crossed = sum(1 for r in selected if r["after"] <= target)
        changed = sum(1 for r in selected if r["after"] < r["before"])
        out.append(
            {
                "method": "slot-exception",
                "before_tier": tier,
                "target": target,
                "eligible": len(selected),
                "crossed": crossed,
                "changed": changed,
                "before_bytes": sum(r["before"] for r in selected),
                "after_bytes": sum(r["after"] for r in selected),
                "saved_bytes": sum(r["before"] - r["after"] for r in selected),
                "exceptions": sum(r["exceptions"] for r in selected),
            }
        )
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("payloads", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--limit-mib", type=int, default=0)
    ap.add_argument("--max-gap", type=int, default=256)
    ap.add_argument("--slots", default=",".join(map(str, SLOTS)))
    ap.add_argument("--exception-model", choices=("full256", "payload", "rawpayload"), default="payload")
    ap.add_argument("--exception-align", type=int, default=64)
    ap.add_argument("--region-align", type=int, default=64)
    ap.add_argument("--mode-bytes", type=int, default=1)
    ap.add_argument("--bitmap-bytes", type=int, default=2)
    ap.add_argument("--entry-bytes", type=int, default=4)
    ap.add_argument("--no-baseline", action="store_true")
    ap.add_argument("--output-dir", type=Path, required=True)
    args = ap.parse_args()

    args.slots = tuple(int(x) for x in args.slots.split(",") if x)
    args.allow_baseline = not args.no_baseline
    args.output_dir.mkdir(parents=True, exist_ok=True)

    limit = args.limit_mib * 256 if args.limit_mib else 10**9
    regions = pp.regions(pp.read_payloads(args.payloads, limit))
    detail = []
    for r in regions:
        best = choose(r, args)
        detail.append(
            {
                "dataset": args.name,
                "region": r.i,
                "before": r.total,
                "before_tier": r.tier,
                "target": r.target,
                "gap": r.gap,
                "max_gap": args.max_gap,
                "slot": best["slot"],
                "after": best["physical"],
                "after_tier": vb.tier(best["physical"]),
                "crossed": int(r.gap > 0 and best["physical"] <= r.target),
                "physical_saved": r.total - best["physical"],
                "logical_bytes": best["logical"],
                "exceptions": best["exceptions"],
                "exception_bytes": best["exception_bytes"],
                "directory_bytes": best["directory_bytes"],
            }
        )

    summary = summarize(detail)
    detail_path = args.output_dir / f"{args.name}-slot-exception-regions.csv"
    summary_path = args.output_dir / f"{args.name}-slot-exception-summary.csv"
    model_path = args.output_dir / f"{args.name}-slot-exception-config.json"

    with detail_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=detail[0].keys())
        w.writeheader()
        w.writerows(detail)
    with summary_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=summary[0].keys())
        w.writeheader()
        w.writerows(summary)
    model_path.write_text(
        json.dumps(
            {
                "format": "slot-exception-v1",
                "config": {
                    "slots": args.slots,
                    "exception_model": args.exception_model,
                    "exception_align": args.exception_align,
                    "region_align": args.region_align,
                    "mode_bytes": args.mode_bytes,
                    "bitmap_bytes": args.bitmap_bytes,
                    "entry_bytes": args.entry_bytes,
                    "allow_baseline": args.allow_baseline,
                    "independent_256b_common_case": True,
                    "exception_slow_path": True,
                },
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    print(json.dumps({"regions": len(regions), "summary": summary}, ensure_ascii=False))


if __name__ == "__main__":
    main()
