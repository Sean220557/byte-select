#!/usr/bin/env python3
"""Crossing-first 3K->2K boundary shaving over FPC+BSEL payloads.

This is a payload-level experiment. It does not implement a final hardware
codec; it tests whether a small var-string bank, seeded from boundary-critical
payloads, can shave enough 64B quantization segments to move 3K-tier regions
down to 2K.
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import analyze_var_strings as lz
import experiment_fpc_payload_lz as pp
import payload_var_codec as codec


LENGTHS = (4, 5, 6, 8, 12, 16, 24, 32, 48, 64)


def boundary_need(size: int) -> int:
    q = vb.quantize(size)
    return size - (q - 64)


def is_boundary_lane(size: int, max_need: int) -> bool:
    return vb.quantize(size) > 64 and 0 < boundary_need(size) <= max_need


def lane_candidates(payload: bytes, size: int, args):
    counts = Counter()
    lengths = LENGTHS
    if args.use_lz:
        _encoded, _literal, phrases = lz.analyze_region(
            payload, args.min_match, args.max_match, args.chain_depth, args.token_bytes
        )
        support = vb.lz_length_support(phrases)
        if support:
            lengths = tuple(
                x for x, _score in sorted(
                    support.items(), key=lambda item: (-item[1] * item[0], item[0])
                )[: args.length_count]
            )
    need = boundary_need(size)
    pressure = 1.0 + (args.max_boundary_need + 1 - need) / max(1, args.max_boundary_need)
    for length in lengths:
        if length > len(payload):
            continue
        for pos in range(0, len(payload) - length + 1, args.stride):
            pattern = vb.canonical(payload[pos : pos + length])
            gain = length - (1 + vb.rank(pattern))
            if gain > 0:
                counts[pattern] += int(gain * length * pressure)
    return counts


def eval_region(region, patterns, mode_bytes: int):
    sizes = list(region.sizes)
    shaved = []
    for idx, payload in enumerate(region.payloads):
        stream = codec.encode(payload, patterns)
        decoded = codec.decode(stream, patterns, len(payload))
        if decoded != payload:
            raise ValueError("roundtrip mismatch")
        candidate = 1 + len(stream)
        if candidate < sizes[idx] and vb.quantize(candidate) < vb.quantize(sizes[idx]):
            sizes[idx] = candidate
            shaved.append(idx)
    if shaved and mode_bytes:
        sizes[shaved[0]] += mode_bytes
    physical = sum(vb.quantize(x) for x in sizes)
    return physical, sum(sizes), shaved


def eval_region_with_table(region, patterns, mode_bytes: int):
    physical, raw, shaved = eval_region(region, patterns, mode_bytes)
    sizes = list(region.sizes)
    for idx in shaved:
        stream = codec.encode(region.payloads[idx], patterns)
        sizes[idx] = 1 + len(stream)
    if shaved and mode_bytes:
        sizes[shaved[0]] += mode_bytes
    slack = sum(vb.quantize(x) - x for x in sizes)
    table_bytes = sum(vb.table_bytes(p) for p in patterns) + (5 if patterns else 0)
    spill = max(0, table_bytes - slack)
    charged = physical + ((spill + 63) // 64) * 64
    return charged, raw, shaved, slack, table_bytes, spill


def pattern_effect(region, pattern):
    saved_segments = 0
    shaved_lanes = 0
    raw_saved = 0
    for payload, size in zip(region.payloads, region.sizes):
        if not is_boundary_lane(size, 64):
            continue
        candidate = 1 + len(codec.encode(payload, [pattern]))
        before_q = vb.quantize(size)
        after_q = vb.quantize(candidate)
        if candidate < size and after_q < before_q:
            saved_segments += (before_q - after_q) // 64
            shaved_lanes += 1
            raw_saved += size - candidate
    return saved_segments, shaved_lanes, raw_saved


def rank_by_effect(region, entries, cap):
    ranked = []
    for entry in entries:
        saved_segments, shaved_lanes, raw_saved = pattern_effect(region, entry.pattern)
        if saved_segments or raw_saved:
            ranked.append(
                (
                    saved_segments,
                    shaved_lanes,
                    raw_saved,
                    -vb.table_bytes(entry.pattern),
                    -len(entry.pattern),
                    entry.pattern,
                    entry,
                )
            )
    ranked.sort(reverse=True)
    return [x[-1] for x in ranked[:cap]]


def train_bank(regions, args):
    counts = Counter()
    for region in regions:
        for payload, size in zip(region.payloads, region.sizes):
            if is_boundary_lane(size, args.max_boundary_need):
                counts.update(lane_candidates(payload, size, args))
    pool, _lub = vb.candidates(counts, args.dictionary_size, args.candidate_cap)
    chosen = []

    def objective(patterns):
        values = [eval_region(r, patterns, args.mode_bytes) for r in regions]
        crossed = sum(v[0] <= r.target for r, v in zip(regions, values))
        seg_saved = sum((r.total - v[0]) // 64 for r, v in zip(regions, values))
        raw = sum(v[1] for v in values)
        shaved = sum(len(v[2]) for v in values)
        return crossed, seg_saved, shaved, -raw

    current = objective(chosen)
    remaining = pool[:]
    while remaining and len(chosen) < args.max_entries:
        best = None
        for entry in remaining:
            trial = chosen + [entry.pattern]
            table = sum(vb.table_bytes(p) for p in trial)
            if table > args.bank_budget:
                continue
            value = objective(trial)
            delta = tuple(value[i] - current[i] for i in range(len(value)))
            key = delta + (-vb.table_bytes(entry.pattern), -len(entry.pattern))
            if best is None or key > best[0]:
                best = (key, entry, value)
        if best is None or best[0][:3] <= (0, 0, 0):
            break
        chosen.append(best[1].pattern)
        remaining.remove(best[1])
        current = best[2]
    return chosen, len(pool), sum(vb.table_bytes(p) for p in chosen)


def train_region_local(region, args):
    if args.max_entries > 4:
        raise ValueError("current direct-marker payload codec supports at most 4 entries")
    counts = Counter()
    lane_pools = []
    for payload, size in zip(region.payloads, region.sizes):
        if is_boundary_lane(size, args.max_boundary_need):
            lane_counts = lane_candidates(payload, size, args)
            counts.update(lane_counts)
            if args.pool_policy == "lane-union":
                lane_pool, _lane_lub = vb.candidates(
                    lane_counts, args.dictionary_size, args.lane_candidate_cap
                )
                lane_pools.extend(lane_pool)
    if args.pool_policy == "lane-union":
        by_pattern = {}
        for entry in lane_pools:
            old = by_pattern.get(entry.pattern)
            if old is None or entry.weight > old.weight:
                by_pattern[entry.pattern] = entry
        pool = sorted(
            by_pattern.values(),
            key=lambda e: (-(e.weight / vb.table_bytes(e.pattern)), -e.weight, vb.rank(e.pattern), e.pattern),
        )[: args.candidate_cap]
    else:
        pool, _lub = vb.candidates(counts, args.dictionary_size, args.candidate_cap)
    if args.pool_policy == "effect":
        wide_pool, _wide_lub = vb.candidates(counts, args.dictionary_size, args.effect_candidate_cap)
        pool = rank_by_effect(region, wide_pool, args.candidate_cap)
    chosen = []
    current = eval_region_with_table(region, chosen, args.mode_bytes)
    remaining = pool[:]

    def score(value):
        return (
            value[0] <= region.target,
            -value[0],
            len(value[2]),
            -value[1],
            -value[4],
        )

    current_score = score(current)
    while remaining and len(chosen) < args.max_entries:
        best = None
        for entry in remaining:
            trial = chosen + [entry.pattern]
            if sum(vb.table_bytes(p) for p in trial) > args.bank_budget:
                continue
            value = eval_region_with_table(region, trial, args.mode_bytes)
            key = score(value) + (-len(entry.pattern),)
            if best is None or key > best[0]:
                best = (key, entry, value)
        if best is None or best[0] <= current_score:
            break
        chosen.append(best[1].pattern)
        remaining.remove(best[1])
        current = best[2]
        current_score = best[0][:-1]
    return chosen, current, len(pool)


def summarize(rows):
    selected = [r for r in rows if r["before_tier"] == 3072 and 0 < r["gap"] <= r["max_gap"]]
    return {
        "method": "3k-boundary-shaving",
        "before_tier": 3072,
        "target": 2048,
        "eligible": len(selected),
        "crossed": sum(r["after"] <= 2048 for r in selected),
        "changed": sum(r["after"] < r["before"] for r in selected),
        "before_bytes": sum(r["before"] for r in selected),
        "after_bytes": sum(r["after"] for r in selected),
        "saved_bytes": sum(r["before"] - r["after"] for r in selected),
        "shaved_sublines": sum(r["shaved_count"] for r in selected),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("train_payloads", type=Path)
    ap.add_argument("test_payloads", type=Path)
    ap.add_argument("--name", required=True)
    ap.add_argument("--train-mib", type=int, default=0)
    ap.add_argument("--test-mib", type=int, default=0)
    ap.add_argument("--max-gap", type=int, default=256)
    ap.add_argument("--max-boundary-need", type=int, default=48)
    ap.add_argument("--clusters", type=int, default=4)
    ap.add_argument("--bank-budget", type=int, default=128)
    ap.add_argument("--max-entries", type=int, default=4)
    ap.add_argument("--candidate-cap", type=int, default=48)
    ap.add_argument("--dictionary-size", type=int, default=31)
    ap.add_argument("--mode-bytes", type=int, default=2)
    ap.add_argument("--use-lz", action="store_true")
    ap.add_argument("--oracle-region-local", action="store_true")
    ap.add_argument("--pool-policy", choices=("global", "lane-union", "effect"), default="global")
    ap.add_argument("--lane-candidate-cap", type=int, default=32)
    ap.add_argument("--effect-candidate-cap", type=int, default=512)
    ap.add_argument("--length-count", type=int, default=6)
    ap.add_argument("--stride", type=int, default=1)
    ap.add_argument("--min-match", type=int, default=4)
    ap.add_argument("--max-match", type=int, default=64)
    ap.add_argument("--chain-depth", type=int, default=8)
    ap.add_argument("--token-bytes", type=int, default=3)
    ap.add_argument("--output-dir", type=Path, required=True)
    args = ap.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    train_limit = args.train_mib * 256 if args.train_mib else 10**9
    test_limit = args.test_mib * 256 if args.test_mib else 10**9
    train = pp.regions(pp.read_payloads(args.train_payloads, train_limit))
    test = pp.regions(pp.read_payloads(args.test_payloads, test_limit))

    eligible_train = [r for r in train if r.tier == 3072 and 0 < r.gap <= args.max_gap]
    banks = []
    if eligible_train and not args.oracle_region_local:
        centers, assignment = __import__("experiment_payload_generalization").kmeans(
            eligible_train, args.clusters
        )
        for bank_id, center in enumerate(centers):
            group = [r for r in eligible_train if assignment[r.i] == bank_id]
            patterns, pool_size, table_bytes = train_bank(group, args)
            banks.append(
                {
                    "bank_id": bank_id,
                    "center": center,
                    "patterns": patterns,
                    "train_regions": [r.i for r in group],
                    "pool_size": pool_size,
                    "table_bytes": table_bytes,
                }
            )

    rows = []
    for r in test:
        after = r.total
        raw = sum(r.sizes)
        shaved = []
        bank_id = -1
        slack = 0
        table_bytes = 0
        spill = 0
        pool_size = 0
        if r.tier == 3072 and 0 < r.gap <= args.max_gap and args.oracle_region_local:
            patterns, result, pool_size = train_region_local(r, args)
            after, raw, shaved, slack, table_bytes, spill = result
            bank_id = 0 if patterns else -1
        elif r.tier == 3072 and 0 < r.gap <= args.max_gap and banks:
            feature = __import__("experiment_payload_generalization").feature
            dist = __import__("experiment_payload_generalization").dist
            rid = min(range(len(banks)), key=lambda i: dist(feature(r), banks[i]["center"]))
            patterns = [tuple(p) for p in banks[rid]["patterns"]]
            if patterns:
                after, raw, shaved = eval_region(r, patterns, args.mode_bytes)
                bank_id = rid
        rows.append(
            {
                "dataset": args.name,
                "region": r.i,
                "before": r.total,
                "before_tier": r.tier,
                "target": r.target,
                "gap": r.gap,
                "max_gap": args.max_gap,
                "bank_id": bank_id,
                "after": after,
                "after_tier": vb.tier(after),
                "crossed": int(r.gap > 0 and after <= r.target),
                "physical_saved": r.total - after,
                "raw_after": raw,
                "shaved_sublines": ";".join(map(str, shaved)),
                "shaved_count": len(shaved),
                "slack_bytes": slack,
                "table_bytes": table_bytes,
                "spill_bytes": spill,
                "pool_size": pool_size,
            }
        )

    summary = summarize(rows)
    mode = "local" if args.oracle_region_local else ("lz" if args.use_lz else "all")
    prefix = f"{args.name}-3k-boundary-{mode}"
    with (args.output_dir / f"{prefix}-regions.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)
    with (args.output_dir / f"{prefix}-summary.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=summary.keys())
        w.writeheader()
        w.writerow(summary)
    config = vars(args).copy()
    config["train_payloads"] = str(config["train_payloads"])
    config["test_payloads"] = str(config["test_payloads"])
    config["output_dir"] = str(config["output_dir"])
    (args.output_dir / f"{prefix}-model.json").write_text(
        json.dumps(
            {
                "format": "3k-boundary-shaving-v1",
                "use_lz": args.use_lz,
                "config": config,
                "banks": banks,
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == "__main__":
    main()
