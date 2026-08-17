#!/usr/bin/env python3
"""Prototype subline-level mixing of spatial Top-256 and Var-String-Select."""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter, OrderedDict, defaultdict
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import analyze_top256_locality as top256
import analyze_var_strings as varstr

BLOCK = 256
REGION = 4096


def quantize(size: int) -> int:
    return min(BLOCK, max(64, ((size + 63) // 64) * 64))


def selected_strings(phrases, capacity: int) -> list[bytes]:
    counts = Counter(content for _, _, _, content in phrases)
    ranked = sorted(
        ((content, count, (len(content) - 2) * count - 3)
         for content, count in counts.items() if count >= 2),
        key=lambda item: (-item[2], -item[1], -len(item[0]), item[0]))
    return [content for content, _, gain in ranked[:capacity] if gain > 0]


def train_global_strings(path: Path, capacity: int, train_bytes: int) -> list[bytes]:
    counts = Counter()
    with path.open("rb") as source:
        data = source.read(train_bytes)
    data = data[:len(data) // REGION * REGION]
    for begin in range(0, len(data), REGION):
        _, _, phrases = varstr.analyze_region(data[begin:begin + REGION], 4, 64, 8, 3)
        counts.update(content for _, _, _, content in phrases)
    ranked = sorted(
        ((content, count, (len(content) - 1) * count)
         for content, count in counts.items() if count >= 2),
        key=lambda item: (-item[2], -item[1], -len(item[0]), item[0]))
    return [content for content, _, _ in ranked[:capacity]]


def train_global_strings_budget(path: Path, budget: int,
                                train_bytes: int) -> list[bytes]:
    counts = Counter()
    with path.open("rb") as source:
        data = source.read(train_bytes)
    data = data[:len(data) // REGION * REGION]
    for begin in range(0, len(data), REGION):
        _, _, phrases = varstr.analyze_region(data[begin:begin + REGION], 4, 64, 8, 3)
        counts.update(content for _, _, _, content in phrases)
    ranked = sorted(
        ((content, count, (len(content) - 1) * count, len(content) + 1)
         for content, count in counts.items() if count >= 2),
        key=lambda item: (-(item[2] / item[3]), -item[2], -item[1], item[0]))
    selected = []
    used = 0
    # Keep one-byte IDs so the per-occurrence cost is identical across budgets.
    for content, _, gain, model_bytes in ranked:
        if gain <= 0 or len(selected) == 256:
            break
        if used + model_bytes <= budget:
            selected.append(content)
            used += model_bytes
    return selected


def encode_subline(data: bytes, selected: list[bytes], reference_bytes: int = 2) -> int:
    by_prefix: dict[bytes, list[bytes]] = defaultdict(list)
    for content in selected:
        by_prefix[content[:4]].append(content)
    for values in by_prefix.values():
        values.sort(key=len, reverse=True)
    cost = [0] * (len(data) + 1)
    for pos in range(len(data) - 1, -1, -1):
        best = 1 + cost[pos + 1]
        if pos + 4 <= len(data):
            for content in by_prefix.get(data[pos:pos + 4], ()):
                end = pos + len(content)
                if end <= len(data) and data[pos:end] == content:
                    best = min(best, reference_bytes + cost[end])
        cost[pos] = best
    return cost[0]


def top_sizes(region: bytes, global_words: set[int], capacity: int) -> list[int]:
    lru: OrderedDict[int, None] = OrderedDict()
    result = []
    for begin in range(0, REGION, BLOCK):
        block = region[begin:begin + BLOCK]
        result.append(quantize(top256.encoded_size(block, global_words, set(lru))))
        residuals, _ = top256.parts(block)
        for value in residuals:
            if value in lru:
                lru.move_to_end(value)
            else:
                lru[value] = None
                if len(lru) > capacity:
                    lru.popitem(last=False)
    return result


def tier(value: int) -> int:
    if value <= 1024: return 1024
    if value <= 2048: return 2048
    if value <= 3072: return 3072
    return 4096


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("train", type=Path)
    parser.add_argument("test", type=Path)
    parser.add_argument("--test-mib", type=int, default=1)
    parser.add_argument("--top-capacity", type=int, default=16)
    parser.add_argument("--var-capacities", default="8,16,32,64")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--global-var-capacity", type=int, default=256)
    parser.add_argument("--global-train-mib", type=int, default=1)
    parser.add_argument("--global-var-budget", type=int, default=0)
    args = parser.parse_args()
    capacities = tuple(int(value) for value in args.var_capacities.split(","))
    global_words = top256.train(args.train, 16 * 1024 * 1024)
    if args.global_var_budget:
        global_strings = train_global_strings_budget(
            args.train, args.global_var_budget,
            args.global_train_mib * 1024 * 1024)
    else:
        global_strings = train_global_strings(
            args.train, args.global_var_capacity,
            args.global_train_mib * 1024 * 1024)
    print(f"global_var_entries={len(global_strings)} "
          f"global_var_model_bytes={sum(len(value) + 1 for value in global_strings)}")
    with args.test.open("rb") as source:
        data = source.read(args.test_mib * 1024 * 1024)
    data = data[:len(data) // REGION * REGION]

    rows = []
    for region_index, begin in enumerate(range(0, len(data), REGION)):
        region = data[begin:begin + REGION]
        _, _, phrases = varstr.analyze_region(region, 4, 64, 8, 3)
        tops = top_sizes(region, global_words, args.top_capacity)
        global_var_sizes = [quantize(encode_subline(
            region[offset:offset + BLOCK], global_strings, 1))
            for offset in range(0, REGION, BLOCK)]
        row = {
            "region": region_index,
            "top256": sum(tops),
            "global_var": sum(global_var_sizes),
            "mixed_global": sum(min(left, right) for left, right in
                                zip(tops, global_var_sizes)),
        }
        for capacity in capacities:
            selected = selected_strings(phrases, capacity)
            var_sizes = [quantize(encode_subline(
                region[offset:offset + BLOCK], selected))
                for offset in range(0, REGION, BLOCK)]
            table_bytes = 3 * len(selected)
            row[f"var_{capacity}"] = sum(var_sizes) + table_bytes
            row[f"mixed_{capacity}"] = (
                sum(min(left, right) for left, right in zip(tops, var_sizes)) +
                table_bytes)
            row[f"selected_{capacity}"] = len(selected)
        rows.append(row)

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=rows[0].keys())
            writer.writeheader(); writer.writerows(rows)

    print(f"regions={len(rows)}")
    for name in ("top256", "global_var", "mixed_global") + tuple(
            f"{kind}_{capacity}" for capacity in capacities
            for kind in ("var", "mixed")):
        values = [row[name] for row in rows]
        print(f"{name}=bytes:{sum(values)},<=1k:{sum(x<=1024 for x in values)},"
              f"<=2k:{sum(x<=2048 for x in values)},"
              f"<=3k:{sum(x<=3072 for x in values)}")

    # Capacity-aware result: smallest table that reaches the best available
    # tier; Top-256 wins ties and therefore avoids unnecessary local metadata.
    choices = Counter()
    values = []
    for row in rows:
        candidates = [("top256", row["top256"], 0),
                      ("mixed_global", row["mixed_global"], 0)] + [
            (f"mixed_{capacity}", row[f"mixed_{capacity}"], capacity)
            for capacity in capacities]
        target = min(tier(value) for _, value, _ in candidates)
        name, value, _ = min(
            (candidate for candidate in candidates if tier(candidate[1]) == target),
            key=lambda candidate: (candidate[2], candidate[1]))
        choices[name] += 1
        values.append(value)
    print("tier_aware=" + ",".join(f"{key}:{value}" for key, value in choices.items()))
    print(f"tier_aware_bytes={sum(values)},<=1k={sum(x<=1024 for x in values)},"
          f"<=2k={sum(x<=2048 for x in values)},"
          f"<=3k={sum(x<=3072 for x in values)}")


if __name__ == "__main__":
    main()
