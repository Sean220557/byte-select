#!/usr/bin/env python3
"""Measure whether bounded LZ-style locality crosses MCC capacity tiers.

This is intentionally an estimator rather than a new codec.  It combines a
real per-256B baseline size list with exact 256B/64B matches inside each 4KiB
region, charges reference metadata, and reports only 64B-quantized outcomes.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Iterable


SUBLINE = 256
LANE = 64
REGION = 4096
TIERS = (1024, 2048, 3072, 4096)


def quantize(size: int) -> int:
    if size >= SUBLINE:
        return SUBLINE
    return max(LANE, ((size + LANE - 1) // LANE) * LANE)


def read_sizes(path: Path) -> list[int]:
    values = [int(line.strip()) for line in path.read_text().splitlines() if line.strip()]
    if not values or any(value < 1 or value > SUBLINE for value in values):
        raise ValueError("baseline sizes must contain integers in [1, 256]")
    return values


def read_lane_sizes(path: Path) -> list[tuple[int, int, int, int]]:
    result = []
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        values = tuple(int(value) for value in line.split(","))
        if len(values) != 4 or any(value < 0 or value > LANE for value in values):
            raise ValueError("lane sizes must contain four comma-separated values in [0, 64]")
        result.append(values)
    return result


def tier(value: int) -> int:
    return next(limit for limit in TIERS if value <= limit)


def rows(data: bytes, baseline: list[int],
         lane_sizes: list[tuple[int, int, int, int]] | None = None,
         delta_words: bool = False,
         word_history: bool = False
         ) -> Iterable[dict[str, int]]:
    history: list[bytes] = []
    for index, base_size in enumerate(baseline):
        block = data[index * SUBLINE : (index + 1) * SUBLINE]
        if index % (REGION // SUBLINE) == 0:
            history.clear()

        # Whole-subline copy: one mode byte plus a 4-bit region distance.
        whole_match = any(block == old for old in reversed(history))
        whole_size = 2 if whole_match else SUBLINE

        # Lane stream: one mode-map byte.  A copied lane charges one reference
        # byte; a literal lane charges its full 64 bytes.  This is deliberately
        # conservative and does not claim savings from partial phrases.
        copied_lanes = 0
        delta_lanes = 0
        word_history_lanes = 0
        lane_size = 1
        for lane in range(4):
            value = block[lane * LANE : (lane + 1) * LANE]
            matched = any(
                value == old[old_lane * LANE : (old_lane + 1) * LANE]
                for old in reversed(history)
                for old_lane in range(4)
            )
            copied_lanes += int(matched)
            literal_size = lane_sizes[index][lane] if lane_sizes else LANE
            local_size = 1 if matched else literal_size
            if delta_words and history and not matched:
                best_changed = 16
                for old in reversed(history):
                    for old_lane in range(4):
                        reference = old[old_lane * LANE : (old_lane + 1) * LANE]
                        changed = sum(
                            value[word : word + 4] != reference[word : word + 4]
                            for word in range(0, LANE, 4)
                        )
                        best_changed = min(best_changed, changed)
                # One reference byte and a 16-bit changed-word bitmap.
                delta_size = 3 + 4 * best_changed
                if delta_size < local_size:
                    local_size = delta_size
                    delta_lanes += 1
            if word_history and history and not matched:
                hits = 0
                for word in range(0, LANE, 4):
                    absolute = lane * LANE + word
                    token = block[absolute : absolute + 4]
                    if any(token == old[absolute : absolute + 4]
                           for old in reversed(history)):
                        hits += 1
                # Lane mode + 16-bit hit bitmap + packed 4-bit distances;
                # misses remain literal 32-bit words.
                history_size = 3 + (hits + 1) // 2 + (16 - hits) * 4
                if history_size < local_size:
                    local_size = history_size
                    word_history_lanes += 1
            lane_size += local_size

        lz_size = min(whole_size, lane_size)
        chosen = min(base_size, lz_size)
        yield {
            "index": index,
            "baseline_bytes": base_size,
            "lz_candidate_bytes": lz_size,
            "chosen_bytes": chosen,
            "baseline_quantized": quantize(base_size),
            "chosen_quantized": quantize(chosen),
            "whole_match": int(whole_match),
            "copied_lanes": copied_lanes,
            "delta_lanes": delta_lanes,
            "word_history_lanes": word_history_lanes,
        }
        history.append(block)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("baseline_sizes", type=Path)
    parser.add_argument("--limit-mib", type=int, default=0)
    parser.add_argument("--csv", type=Path)
    parser.add_argument("--lane-sizes", type=Path)
    parser.add_argument("--delta-words", action="store_true")
    parser.add_argument("--word-history", action="store_true")
    args = parser.parse_args()

    baseline = read_sizes(args.baseline_sizes)
    lane_sizes = read_lane_sizes(args.lane_sizes) if args.lane_sizes else None
    if lane_sizes is not None and len(lane_sizes) != len(baseline):
        raise ValueError("lane-size and baseline-size lists have different lengths")
    limit = len(baseline) * SUBLINE
    if args.limit_mib:
        limit = min(limit, args.limit_mib * 1024 * 1024)
    limit -= limit % REGION
    data = args.input.read_bytes()[:limit]
    data = data[: len(data) // REGION * REGION]
    count = min(len(data) // SUBLINE, len(baseline))
    data = data[: count * SUBLINE]
    baseline = baseline[:count]
    if lane_sizes is not None:
        lane_sizes = lane_sizes[:count]
    if not baseline:
        raise ValueError("input and baseline have no aligned sublines")

    result = list(rows(data, baseline, lane_sizes, args.delta_words,
                       args.word_history))
    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=result[0].keys())
            writer.writeheader()
            writer.writerows(result)

    regions = len(result) // (REGION // SUBLINE)
    transitions: dict[tuple[int, int], int] = {}
    base_tiers = {value: 0 for value in TIERS}
    chosen_tiers = {value: 0 for value in TIERS}
    exact = lanes = deltas = history_lanes = crossed = 0
    base_total = chosen_total = 0
    for begin in range(0, regions * 16, 16):
        group = result[begin : begin + 16]
        before = sum(row["baseline_quantized"] for row in group)
        after = sum(row["chosen_quantized"] for row in group)
        bt, at = tier(before), tier(after)
        base_tiers[bt] += 1
        chosen_tiers[at] += 1
        transitions[(bt, at)] = transitions.get((bt, at), 0) + 1
        crossed += at < bt
        base_total += before
        chosen_total += after
        exact += sum(row["whole_match"] for row in group)
        lanes += sum(row["copied_lanes"] for row in group)
        deltas += sum(row["delta_lanes"] for row in group)
        history_lanes += sum(row["word_history_lanes"] for row in group)

    print(f"sublines={len(result)} regions={regions}")
    print(f"whole_matches={exact} copied_lanes={lanes} delta_lanes={deltas} "
          f"word_history_lanes={history_lanes}")
    print(f"baseline_quantized_bytes={base_total} lz_quantized_bytes={chosen_total}")
    print(f"regions_crossing_tier={crossed} ({crossed / regions:.6%})")
    for value in TIERS:
        print(f"tier_{value}: baseline={base_tiers[value]} lz={chosen_tiers[value]}")
    for (before, after), amount in sorted(transitions.items()):
        if before != after:
            print(f"transition_{before}_to_{after}={amount}")


if __name__ == "__main__":
    main()
