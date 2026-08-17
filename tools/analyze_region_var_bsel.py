#!/usr/bin/env python3
"""Optimize a region-local Var-String-BSEL table for capacity-tier crossings.

This is a size-accurate prototype at the format assumptions documented below:
* 4KiB regions, sixteen independently quantized 256B sublines;
* exact entries use a 3-byte (source-offset,length) descriptor and 1-byte ID;
* canonical patterns store packed equality symbols and each use stores ID+rank bytes;
* local-table bytes consume existing subline slack first, then 64B segments;
* each subline chooses independently between its supplied baseline and Var-BSEL.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_var_strings as lzprobe

REGION = 4096
SUBLINE = 256


def quantize(value: int) -> int:
    return min(SUBLINE, max(64, ((value + 63) // 64) * 64))


def tier(value: int) -> int:
    if value <= 1024: return 1024
    if value <= 2048: return 2048
    if value <= 3072: return 3072
    return 4096


def canonical(data: bytes) -> tuple[tuple[int, ...], int]:
    ids: dict[int, int] = {}
    symbols = []
    for value in data:
        if value not in ids:
            ids[value] = len(ids)
        symbols.append(ids[value])
    return tuple(symbols), len(ids)


@dataclass(frozen=True)
class Candidate:
    kind: str
    value: bytes | tuple[int, ...]
    length: int
    rank: int
    occurrences: int
    table_bytes: int
    estimated_gain: int


def candidate_pool(region: bytes, phrases, exact_maximum: int,
                   pattern_maximum: int) -> list[Candidate]:
    exact_counts = Counter(content for _, _, _, content in phrases)
    pattern_counts: Counter[tuple[int, tuple[int, ...]]] = Counter()
    pattern_rank: dict[tuple[int, tuple[int, ...]], int] = {}

    # Mine structural equality patterns independently of exact LZ matches.
    # Windows never cross a 256B random-access boundary.
    pattern_lengths = (4, 5, 6, 8, 12, 16, 24, 32)
    for subline_begin in range(0, REGION, SUBLINE):
        subline = region[subline_begin:subline_begin + SUBLINE]
        for length in pattern_lengths:
            for offset in range(0, SUBLINE - length + 1):
                symbols, rank = canonical(subline[offset:offset + length])
                if rank >= length:
                    continue
                key = length, symbols
                pattern_counts[key] += 1
                pattern_rank[key] = rank

    exact_candidates = []
    for content, count in exact_counts.items():
        if count < 2 or len(content) < 4:
            continue
        table_bytes = 3  # 12-bit source offset + 6-bit length/mode, rounded.
        gain = count * (len(content) - 1) - table_bytes
        if gain > 0:
            exact_candidates.append(Candidate(
                "exact", content, len(content), 0, count, table_bytes, gain))

    pattern_candidates = []
    for (length, symbols), count in pattern_counts.items():
        rank = pattern_rank[(length, symbols)]
        if count < 2 or rank >= length:
            continue
        bits_per_symbol = max(1, math.ceil(math.log2(rank)))
        table_bytes = 2 + (length * bits_per_symbol + 7) // 8
        use_bytes = 1 + rank
        gain = count * (length - use_bytes) - table_bytes
        if gain > 0:
            pattern_candidates.append(Candidate(
                "pattern", symbols, length, rank, count, table_bytes, gain))

    # Benefit density makes the bounded search spend slots on compact patterns.
    sort_key = lambda item: (
        -(item.estimated_gain / item.table_bytes), -item.estimated_gain,
        -item.length, item.kind, item.value)
    exact_candidates.sort(key=sort_key)
    pattern_candidates.sort(key=sort_key)
    return (exact_candidates[:exact_maximum] +
            pattern_candidates[:pattern_maximum])


def encode_var_subline(data: bytes, selected: tuple[Candidate, ...]) -> int:
    exact_by_prefix: dict[bytes, list[Candidate]] = defaultdict(list)
    patterns_by_length: dict[int, list[Candidate]] = defaultdict(list)
    for candidate in selected:
        if candidate.kind == "exact":
            assert isinstance(candidate.value, bytes)
            exact_by_prefix[candidate.value[:4]].append(candidate)
        else:
            patterns_by_length[candidate.length].append(candidate)
    for values in exact_by_prefix.values():
        values.sort(key=lambda item: item.length, reverse=True)

    cost = [0] * (len(data) + 1)
    canonical_cache: dict[tuple[int, int], tuple[tuple[int, ...], int]] = {}
    for pos in range(len(data) - 1, -1, -1):
        best = 1 + cost[pos + 1]
        if pos + 4 <= len(data):
            for candidate in exact_by_prefix.get(data[pos:pos + 4], ()):
                end = pos + candidate.length
                if end <= len(data) and data[pos:end] == candidate.value:
                    best = min(best, 1 + cost[end])
        for length, candidates in patterns_by_length.items():
            end = pos + length
            if end > len(data):
                continue
            cache_key = pos, length
            if cache_key not in canonical_cache:
                canonical_cache[cache_key] = canonical(data[pos:end])
            symbols, rank = canonical_cache[cache_key]
            for candidate in candidates:
                if rank == candidate.rank and symbols == candidate.value:
                    best = min(best, 1 + rank + cost[end])
                    break
        cost[pos] = best
    return cost[0]


@dataclass
class Evaluation:
    physical: int
    payload_physical: int
    table_bytes: int
    table_spill_bytes: int
    var_sublines: int
    actual_bytes: int


def evaluate(region: bytes, baseline: list[int],
             selected: tuple[Candidate, ...]) -> Evaluation:
    actual = []
    var_sublines = 0
    for lane, begin in enumerate(range(0, REGION, SUBLINE)):
        var_size = encode_var_subline(region[begin:begin + SUBLINE], selected)
        if var_size < baseline[lane]:
            actual.append(var_size)
            var_sublines += 1
        else:
            actual.append(baseline[lane])
    quantized = [quantize(value) for value in actual]
    payload_physical = sum(quantized)
    slack = sum(qvalue - value for qvalue, value in zip(quantized, actual))
    table_bytes = sum(candidate.table_bytes for candidate in selected)
    spill = max(0, table_bytes - slack)
    spill_physical = ((spill + 63) // 64) * 64
    return Evaluation(payload_physical + spill_physical, payload_physical,
                      table_bytes, spill, var_sublines, sum(actual))


def optimize(region: bytes, baseline: list[int], pool: list[Candidate],
             beam_width: int, max_entries: int):
    base_eval = evaluate(region, baseline, ())
    target = {4096: 3072, 3072: 2048, 2048: 1024, 1024: 1024}[tier(base_eval.physical)]
    best = ((), base_eval)
    beam: list[tuple[tuple[int, ...], Evaluation]] = [((), base_eval)]
    seen = {()}
    for _ in range(max_entries):
        expanded = []
        for indices, _ in beam:
            start = indices[-1] + 1 if indices else 0
            for index in range(start, len(pool)):
                new_indices = indices + (index,)
                if new_indices in seen:
                    continue
                seen.add(new_indices)
                selected = tuple(pool[item] for item in new_indices)
                result = evaluate(region, baseline, selected)
                expanded.append((new_indices, result))
                if (tier(result.physical), result.physical, len(new_indices),
                        result.table_bytes) < (
                        tier(best[1].physical), best[1].physical, len(best[0]),
                        best[1].table_bytes):
                    best = (new_indices, result)
        if not expanded:
            break
        # Retain states close to the target as well as smallest-byte states.
        expanded.sort(key=lambda state: (
            0 if state[1].physical <= target else 1,
            max(0, state[1].physical - target),
            state[1].physical, len(state[0]), state[1].table_bytes))
        beam = expanded[:beam_width]
        successful = [state for state in beam if state[1].physical <= target]
        if successful:
            successful.sort(key=lambda state: (
                len(state[0]), state[1].table_bytes, state[1].physical))
            candidate = successful[0]
            if (len(candidate[0]), candidate[1].table_bytes,
                    candidate[1].physical) < (
                    len(best[0]), best[1].table_bytes, best[1].physical):
                best = candidate
            break
    # The objective is a capacity transition, not byte minimization within the
    # same tier. Do not emit a local table when it fails to reach the target.
    if best[1].physical > target:
        return base_eval, base_eval, ()
    selected = tuple(pool[index] for index in best[0])
    return base_eval, best[1], selected


def read_sizes(path: Path, count: int) -> list[int]:
    values = [int(line) for line in path.read_text().splitlines() if line.strip()]
    if len(values) < count:
        raise ValueError("baseline size list is shorter than the requested input")
    return values[:count]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("baseline_sizes", type=Path)
    parser.add_argument("--limit-mib", type=int, default=1)
    parser.add_argument("--limit-kib", type=int, default=0)
    parser.add_argument("--exact-pool", type=int, default=8)
    parser.add_argument("--pattern-pool", type=int, default=8)
    parser.add_argument("--beam-width", type=int, default=2)
    parser.add_argument("--max-entries", type=int, default=4)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    limit_bytes = (args.limit_kib * 1024 if args.limit_kib
                   else args.limit_mib * 1024 * 1024)
    with args.input.open("rb") as source:
        data = source.read(limit_bytes)
    data = data[:len(data) // REGION * REGION]
    sizes = read_sizes(args.baseline_sizes, len(data) // SUBLINE)
    rows = []
    kind_counts = Counter()
    for region_index, begin in enumerate(range(0, len(data), REGION)):
        region = data[begin:begin + REGION]
        baseline = sizes[begin // SUBLINE:begin // SUBLINE + REGION // SUBLINE]
        _, _, phrases = lzprobe.analyze_region(region, 4, 64, 8, 3)
        pool = candidate_pool(region, phrases, args.exact_pool, args.pattern_pool)
        before, after, selected = optimize(
            region, baseline, pool, args.beam_width, args.max_entries)
        kind_counts.update(candidate.kind for candidate in selected)
        rows.append({
            "region": region_index,
            "before": before.physical,
            "after": after.physical,
            "before_tier": tier(before.physical),
            "after_tier": tier(after.physical),
            "entries": len(selected),
            "exact_entries": sum(candidate.kind == "exact" for candidate in selected),
            "pattern_entries": sum(candidate.kind == "pattern" for candidate in selected),
            "table_bytes": after.table_bytes,
            "table_spill_bytes": after.table_spill_bytes,
            "var_sublines": after.var_sublines,
        })

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=rows[0].keys())
            writer.writeheader(); writer.writerows(rows)

    transitions = Counter((row["before_tier"], row["after_tier"])
                          for row in rows if row["before_tier"] != row["after_tier"])
    print(f"regions={len(rows)} before_bytes={sum(row['before'] for row in rows)} "
          f"after_bytes={sum(row['after'] for row in rows)}")
    print(f"crossed_regions={sum(transitions.values())} transitions={dict(transitions)}")
    print(f"selected_entries={sum(row['entries'] for row in rows)} "
          f"exact_entries={kind_counts['exact']} pattern_entries={kind_counts['pattern']}")
    print(f"table_bytes={sum(row['table_bytes'] for row in rows)} "
          f"table_spill_bytes={sum(row['table_spill_bytes'] for row in rows)}")
    for bound in (1024, 2048, 3072):
        print(f"tier_{bound}: before={sum(row['before'] <= bound for row in rows)} "
              f"after={sum(row['after'] <= bound for row in rows)}")


if __name__ == "__main__":
    main()
