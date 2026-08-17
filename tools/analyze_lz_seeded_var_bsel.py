#!/usr/bin/env python3
"""A/B test LZ-seeded search for region-local variable-length BSEL patterns.

LZ is used only as a search oracle: its target offsets identify training atoms.
The emitted entries are canonical BSEL equality patterns, never LZ references or
exact strings.  The all-window arm uses the same lengths, table budget, trainer,
encoder, and tier objective.
"""

from __future__ import annotations

import argparse
import csv
import math
import time
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

import analyze_var_strings as lzprobe

REGION = 4096
SUBLINE = 256
LENGTHS = (4, 5, 6, 8, 12, 16, 24, 32, 48, 64)


def quantize(n: int) -> int:
    return min(256, max(64, (n + 63) // 64 * 64))


def tier(n: int) -> int:
    return 1024 if n <= 1024 else 2048 if n <= 2048 else 3072 if n <= 3072 else 4096


def canonical(data: bytes) -> tuple[int, ...]:
    ids: dict[int, int] = {}
    out = []
    for value in data:
        if value not in ids:
            ids[value] = len(ids)
        out.append(ids[value])
    return tuple(out)


def rank(pattern: tuple[int, ...]) -> int:
    return max(pattern, default=-1) + 1


def less_equal(atom: tuple[int, ...], upper: tuple[int, ...]) -> bool:
    # Every equality required by upper also holds in atom.
    seen: dict[int, int] = {}
    for left, right in zip(atom, upper):
        old = seen.setdefault(right, left)
        if old != left:
            return False
    return True


def lub(a: tuple[int, ...], b: tuple[int, ...]) -> tuple[int, ...]:
    ids: dict[tuple[int, int], int] = {}
    out = []
    for pair in zip(a, b):
        if pair not in ids:
            ids[pair] = len(ids)
        out.append(ids[pair])
    return tuple(out)


def table_bytes(pattern: tuple[int, ...]) -> int:
    r = rank(pattern)
    width = max(1, math.ceil(math.log2(max(2, r))))
    return 2 + (len(pattern) * width + 7) // 8


@dataclass(frozen=True)
class Entry:
    pattern: tuple[int, ...]
    weight: int


def lz_atoms(region: bytes, phrases) -> tuple[Counter, int]:
    atoms: Counter[tuple[int, ...]] = Counter()
    seeded_windows: set[tuple[int, int]] = set()
    for offset, length, _distance, _content in phrases:
        lane_end = (offset // SUBLINE + 1) * SUBLINE
        for size in LENGTHS:
            if size > length:
                continue
            # The LZ evidence covers the complete target span, not merely the
            # phrase prefix. Every contained window is therefore a valid seed.
            last = min(offset + length - size, lane_end - size)
            for start in range(offset, last + 1):
                seeded_windows.add((start, size))
    for start, size in seeded_windows:
        atoms[canonical(region[start:start + size])] += size
    return atoms, len(seeded_windows)


def scan_atoms(region: bytes, lengths=LENGTHS) -> tuple[Counter, int]:
    atoms: Counter[tuple[int, ...]] = Counter()
    occurrences = 0
    for lane in range(0, REGION, SUBLINE):
        for size in lengths:
            for offset in range(lane, lane + SUBLINE - size + 1):
                atoms[canonical(region[offset:offset + size])] += size
                occurrences += 1
    return atoms, occurrences


def lz_length_support(phrases) -> Counter:
    # A length earns one vote for every byte window that an LZ match proves is
    # repeat-bearing. This makes the prior regional and naturally favors lengths
    # with both frequent support and useful byte coverage.
    scores = Counter()
    for _offset, match_length, _distance, _content in phrases:
        for size in LENGTHS:
            if size <= match_length:
                scores[size] += match_length - size + 1
    return scores


def lz_selected_lengths(region: bytes, baseline: list[int], phrases, count: int,
                        policy: str) -> tuple[int, ...]:
    support = lz_length_support(phrases)
    if policy == "coverage":
        scores = {size: votes * size for size, votes in support.items()}
    else:
        # Cheap structural probe (stride 4): LZ says which lengths are credible;
        # BSEL estimates whether their canonical rank can actually save bytes.
        # Windows in lanes close to shedding a 64B segment receive more weight.
        potential = Counter()
        for lane, begin in enumerate(range(0, REGION, SUBLINE)):
            q = quantize(baseline[lane])
            bytes_to_next_segment = max(1, baseline[lane] - max(0, q - 64))
            lane_weight = 1.0 + 64.0 / bytes_to_next_segment
            for size in support:
                for offset in range(begin, begin + SUBLINE - size + 1, 4):
                    r = rank(canonical(region[offset:offset + size]))
                    gain = max(0, size - (1 + r))
                    potential[size] += gain * lane_weight
        region_physical = sum(quantize(value) for value in baseline)
        next_tier = {4096: 3072, 3072: 2048, 2048: 1024, 1024: 1024}[tier(region_physical)]
        region_gap = max(1, region_physical - next_tier)
        tier_pressure = 1.0 + 1024.0 / region_gap
        scores = {size: math.log2(1 + votes) * potential[size] * tier_pressure
                  for size, votes in support.items()}
    return tuple(size for size, _ in sorted(
        scores.items(), key=lambda item: (-item[1], item[0]))[:count])


def candidates(atoms: Counter, dictionary_size: int, cap: int) -> tuple[list[Entry], int]:
    usable = Counter({p: w for p, w in atoms.items() if rank(p) < len(p)
                      and rank(p) <= dictionary_size})
    # Keep a bounded top set before lattice operations. This is identical in A/B.
    top = sorted(usable, key=lambda p: (-usable[p], rank(p), p))[:cap]
    by_length: dict[int, list[tuple[int, ...]]] = defaultdict(list)
    for p in top:
        by_length[len(p)].append(p)
    generated = set(top)
    lub_evals = 0
    # BSEL lattice generalization: add legal LUBs of the strongest atoms.
    for group in by_length.values():
        group = group[:min(48, len(group))]
        for i, left in enumerate(group):
            for right in group[i + 1:]:
                lub_evals += 1
                upper = lub(left, right)
                if rank(upper) <= dictionary_size:
                    generated.add(upper)
    entries = []
    search_atoms = [(atom, usable[atom]) for atom in top]
    for pattern in generated:
        weight = sum(w for atom, w in search_atoms
                     if len(atom) == len(pattern) and less_equal(atom, pattern))
        if weight:
            entries.append(Entry(pattern, weight))
    entries.sort(key=lambda e: (-(e.weight / table_bytes(e.pattern)), -e.weight,
                                rank(e.pattern), e.pattern))
    return entries, lub_evals


def encode_lane(data: bytes, selected: tuple[Entry, ...]) -> int:
    groups: dict[int, list[tuple[int, ...]]] = defaultdict(list)
    for entry in selected:
        groups[len(entry.pattern)].append(entry.pattern)
    cost = [0] * (SUBLINE + 1)
    cache: dict[tuple[int, int], tuple[int, ...]] = {}
    for pos in range(SUBLINE - 1, -1, -1):
        best = 1 + cost[pos + 1]
        for size, patterns in groups.items():
            end = pos + size
            if end > SUBLINE:
                continue
            atom = cache.setdefault((pos, size), canonical(data[pos:end]))
            for pattern in patterns:
                if less_equal(atom, pattern):
                    best = min(best, 1 + rank(pattern) + cost[end])
        cost[pos] = best
    return cost[0]


def evaluate(region: bytes, baseline: list[int], selected: tuple[Entry, ...]):
    actual = []
    used = 0
    for lane, begin in enumerate(range(0, REGION, SUBLINE)):
        var = encode_lane(region[begin:begin + SUBLINE], selected)
        if var < baseline[lane]:
            actual.append(var); used += 1
        else:
            actual.append(baseline[lane])
    physical = [quantize(n) for n in actual]
    slack = sum(q - n for q, n in zip(physical, actual))
    model = sum(table_bytes(e.pattern) for e in selected)
    spill = max(0, model - slack)
    total = sum(physical) + (spill + 63) // 64 * 64
    return total, model, spill, used, sum(actual)


def select(region: bytes, baseline: list[int], pool: list[Entry], budget: int,
           max_entries: int):
    chosen: list[Entry] = []
    remaining = pool[:]
    base = evaluate(region, baseline, ())
    current = base
    evals = 0
    while remaining and len(chosen) < max_entries:
        best = None
        for entry in remaining:
            trial = tuple(chosen + [entry])
            if sum(table_bytes(e.pattern) for e in trial) > budget:
                continue
            result = evaluate(region, baseline, trial); evals += 1
            key = (tier(result[0]), result[0], result[4], result[1])
            if best is None or key < best[0]:
                best = (key, entry, result)
        if best is None or (tier(best[2][0]), best[2][0], best[2][4]) >= (
                tier(current[0]), current[0], current[4]):
            break
        chosen.append(best[1]); remaining.remove(best[1]); current = best[2]
    return base, current, tuple(chosen), evals


def read_sizes(path: Path, count: int) -> list[int]:
    values = [int(x) for x in path.read_text().splitlines() if x.strip()]
    if len(values) < count:
        raise ValueError("baseline size list is shorter than input")
    return values[:count]


def run_arm(region: bytes, baseline: list[int], phrases, mode: str, args):
    start = time.perf_counter()
    selected_lengths = LENGTHS
    if mode == "lz-position":
        atoms, occurrences = lz_atoms(region, phrases)
    elif mode in ("lz-length", "lz-tier"):
        policy = "coverage" if mode == "lz-length" else "tier"
        selected_lengths = lz_selected_lengths(
            region, baseline, phrases, args.lz_length_count, policy)
        atoms, occurrences = scan_atoms(region, selected_lengths)
    else:
        atoms, occurrences = scan_atoms(region)
    pool, lub_evals = candidates(atoms, args.dictionary_size, args.candidate_cap)
    before, after, chosen, coverage_evals = select(
        region, baseline, pool, args.model_budget, args.max_entries)
    return {
        "mode": mode, "atom_occurrences": occurrences, "distinct_atoms": len(atoms),
        "candidate_patterns": len(pool), "lub_evaluations": lub_evals,
        "coverage_evaluations": coverage_evals, "before": before[0], "after": after[0],
        "before_tier": tier(before[0]), "after_tier": tier(after[0]),
        "entries": len(chosen), "model_bytes": after[1], "model_spill": after[2],
        "var_sublines": after[3], "elapsed_ms": (time.perf_counter() - start) * 1000,
        "searched_lengths": ";".join(map(str, selected_lengths)),
        "selected": ";".join(f"{len(e.pattern)}:{rank(e.pattern)}:"
                             + ".".join(map(str, e.pattern)) for e in chosen),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("baseline_sizes", type=Path)
    ap.add_argument("--limit-kib", type=int, default=256)
    ap.add_argument("--start-region", type=int, default=0)
    ap.add_argument("--model-budget", type=int, default=128)
    ap.add_argument("--max-entries", type=int, default=8)
    ap.add_argument("--candidate-cap", type=int, default=96)
    ap.add_argument("--dictionary-size", type=int, default=31)
    ap.add_argument("--lz-length-count", type=int, default=6)
    ap.add_argument("--modes", default="all,lz-position,lz-length,lz-tier",
                    help="comma-separated subset of all,lz-position,lz-length,lz-tier")
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    modes = tuple(item.strip() for item in args.modes.split(",") if item.strip())
    legal_modes = {"all", "lz-position", "lz-length", "lz-tier"}
    if not modes or not set(modes) <= legal_modes:
        raise ValueError("invalid --modes")
    raw = args.input.read_bytes()
    data_begin = args.start_region * REGION
    data = raw[data_begin:data_begin + args.limit_kib * 1024]
    data = data[:len(data) // REGION * REGION]
    all_sizes = read_sizes(args.baseline_sizes, data_begin // SUBLINE + len(data) // SUBLINE)
    sizes = all_sizes[data_begin // SUBLINE:]
    rows = []
    for ri, begin in enumerate(range(0, len(data), REGION)):
        region = data[begin:begin + REGION]
        baseline = sizes[begin // SUBLINE:begin // SUBLINE + 16]
        _encoded, _matched, phrases = lzprobe.analyze_region(region, 4, 64, 8, 3)
        for mode in modes:
            row = run_arm(region, baseline, phrases, mode, args)
            row["region"] = args.start_region + ri
            rows.append(row)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=rows[0].keys()); writer.writeheader(); writer.writerows(rows)
    for mode in modes:
        arm = [r for r in rows if r["mode"] == mode]
        crossings = Counter((r["before_tier"], r["after_tier"]) for r in arm
                            if r["before_tier"] != r["after_tier"])
        print(f"mode={mode} regions={len(arm)} before={sum(r['before'] for r in arm)} "
              f"after={sum(r['after'] for r in arm)} crossings={sum(crossings.values())} "
              f"transitions={dict(crossings)} atoms={sum(r['atom_occurrences'] for r in arm)} "
              f"distinct={sum(r['distinct_atoms'] for r in arm)} candidates={sum(r['candidate_patterns'] for r in arm)} "
              f"coverage_evals={sum(r['coverage_evaluations'] for r in arm)} "
              f"time_ms={sum(r['elapsed_ms'] for r in arm):.1f}")


if __name__ == "__main__":
    main()
