#!/usr/bin/env python3
"""Train shared Var-BSEL patch banks specifically for FPC-hard regions."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import analyze_var_strings as lzprobe


@dataclass
class Region:
    index: int
    data: bytes
    baseline: list[int]
    physical: int
    target: int
    gap: int
    phrases: list


def target_for(physical: int) -> int:
    return {4096: 3072, 3072: 2048, 2048: 1024, 1024: 1024}[vb.tier(physical)]


def load_regions(data_path: Path, sizes_path: Path, limit_mib: int) -> list[Region]:
    data = data_path.read_bytes()[:limit_mib * 1024 * 1024]
    data = data[:len(data) // vb.REGION * vb.REGION]
    sizes = vb.read_sizes(sizes_path, len(data) // vb.SUBLINE)
    result = []
    for index, begin in enumerate(range(0, len(data), vb.REGION)):
        region = data[begin:begin + vb.REGION]
        baseline = sizes[begin // vb.SUBLINE:begin // vb.SUBLINE + 16]
        physical = sum(vb.quantize(value) for value in baseline)
        target = target_for(physical)
        _encoded, _matched, phrases = lzprobe.analyze_region(region, 4, 64, 8, 3)
        result.append(Region(index, region, baseline, physical, target,
                             max(0, physical - target), phrases))
    return result


def training_counts(regions: list[Region], hard_only: bool, max_gap: int,
                    length_count: int, tier_filter: int) -> tuple[Counter, int, Counter]:
    counts = Counter()
    lengths_used = Counter()
    selected_regions = 0
    for region in regions:
        if tier_filter and vb.tier(region.physical) != tier_filter:
            continue
        if region.target == 1024 and region.physical <= 1024:
            continue
        if hard_only and region.gap > max_gap:
            continue
        lengths = vb.lz_selected_lengths(
            region.data, region.baseline, region.phrases, length_count, "tier")
        atoms, _ = vb.scan_atoms(region.data, lengths)
        # Regions close to crossing receive larger training weight. Counts remain
        # integral so the existing BSEL lattice trainer can be reused unchanged.
        region_weight = 1 + max_gap // max(64, region.gap)
        for atom, value in atoms.items():
            gain = max(0, len(atom) - (1 + vb.rank(atom)))
            if gain:
                counts[atom] += value * gain * region_weight
        lengths_used.update(lengths)
        selected_regions += 1
    return counts, selected_regions, lengths_used


def build_bank(counts: Counter, budget: int, candidate_cap: int,
               dictionary_size: int) -> tuple[tuple[vb.Entry, ...], int]:
    pool, _ = vb.candidates(counts, dictionary_size, candidate_cap)
    chosen = []
    used = 0
    for entry in pool:
        cost = vb.table_bytes(entry.pattern)
        if used + cost <= budget:
            chosen.append(entry); used += cost
    return tuple(chosen), used


def evaluate_shared(region: Region, bank: tuple[vb.Entry, ...], mode_bytes: int):
    actual = []
    used_lanes = 0
    for lane, begin in enumerate(range(0, vb.REGION, vb.SUBLINE)):
        var = vb.encode_lane(region.data[begin:begin + vb.SUBLINE], bank)
        if var < region.baseline[lane]:
            actual.append(var); used_lanes += 1
        else:
            actual.append(region.baseline[lane])
    # Conservatively charge shared-mode selection bytes to the first used lane.
    if used_lanes and mode_bytes:
        first = next(i for i, value in enumerate(actual)
                     if value < region.baseline[i])
        actual[first] += mode_bytes
    physical = sum(vb.quantize(value) for value in actual)
    return physical, used_lanes


def summarize(name: str, regions: list[Region], bank, model_bytes: int,
              mode_bytes: int):
    rows = []
    transitions = Counter()
    for region in regions:
        after, used = evaluate_shared(region, bank, mode_bytes)
        before_tier, after_tier = vb.tier(region.physical), vb.tier(after)
        if after_tier < before_tier:
            transitions[(before_tier, after_tier)] += 1
        rows.append((region, after, used))
    print(f"bank={name} entries={len(bank)} model_bytes={model_bytes} "
          f"before={sum(r.physical for r in regions)} after={sum(a for r,a,_ in rows)} "
          f"crossings={sum(transitions.values())} transitions={dict(transitions)}")
    return rows


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("baseline_sizes", type=Path)
    ap.add_argument("--limit-mib", type=int, default=1)
    ap.add_argument("--max-gap", type=int, default=256)
    ap.add_argument("--length-count", type=int, default=6)
    ap.add_argument("--model-budget", type=int, default=2048)
    ap.add_argument("--candidate-cap", type=int, default=256)
    ap.add_argument("--dictionary-size", type=int, default=31)
    ap.add_argument("--mode-bytes", type=int, default=2)
    ap.add_argument("--target-tier", type=int, choices=(2048, 3072, 4096), default=0,
                    help="train/evaluate only this FPC before-tier")
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()

    regions = load_regions(args.input, args.baseline_sizes, args.limit_mib)
    all_counts, all_n, all_lengths = training_counts(
        regions, False, args.max_gap, args.length_count, args.target_tier)
    hard_counts, hard_n, hard_lengths = training_counts(
        regions, True, args.max_gap, args.length_count, args.target_tier)
    all_bank, all_bytes = build_bank(
        all_counts, args.model_budget, args.candidate_cap, args.dictionary_size)
    hard_bank, hard_bytes = build_bank(
        hard_counts, args.model_budget, args.candidate_cap, args.dictionary_size)
    print(f"training=all regions={all_n} atoms={len(all_counts)} lengths={dict(all_lengths)}")
    print(f"training=hard regions={hard_n} atoms={len(hard_counts)} lengths={dict(hard_lengths)}")
    evaluated = ([region for region in regions if vb.tier(region.physical) == args.target_tier]
                 if args.target_tier else regions)
    all_rows = summarize("all", evaluated, all_bank, all_bytes, args.mode_bytes)
    hard_rows = summarize("fpc-hard", evaluated, hard_bank, hard_bytes, args.mode_bytes)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as f:
        fields = ["region", "before", "before_tier", "target", "gap",
                  "eligible", "all_after", "all_tier", "all_used_lanes",
                  "hard_after", "hard_tier", "hard_used_lanes"]
        writer = csv.DictWriter(f, fieldnames=fields); writer.writeheader()
        for (region, all_after, all_used), (_, hard_after, hard_used) in zip(
                all_rows, hard_rows):
            writer.writerow({
                "region": region.index, "before": region.physical,
                "before_tier": vb.tier(region.physical), "target": region.target,
                "gap": region.gap, "eligible": int(0 < region.gap <= args.max_gap),
                "all_after": all_after, "all_tier": vb.tier(all_after),
                "all_used_lanes": all_used, "hard_after": hard_after,
                "hard_tier": vb.tier(hard_after), "hard_used_lanes": hard_used})


if __name__ == "__main__":
    main()
