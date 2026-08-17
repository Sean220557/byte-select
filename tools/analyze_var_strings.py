#!/usr/bin/env python3
"""Use bounded LZ matching as ground truth for variable-string candidates.

The analysis resets every 4KiB, finds the longest previous match at every byte
position, and computes an optimal parse.  It is a discovery tool: the emitted
phrases are intended to seed a Var-String-Select search, not prescribe an LZ
bitstream.
"""

from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path

REGION = 4096


def bucket(length: int) -> str:
    for limit in (4, 8, 16, 32, 64, 128, 256):
        if length <= limit:
            return f"<={limit}"
    return ">256"


def analyze_region(data: bytes, minimum: int, maximum: int,
                   chain_depth: int, token_bytes: int):
    positions: dict[bytes, list[int]] = defaultdict(list)
    matches: list[tuple[int, int]] = [(0, 0)] * len(data)
    for pos in range(len(data)):
        if pos + minimum > len(data):
            break
        key = data[pos : pos + minimum]
        best_length = best_distance = 0
        for source in reversed(positions[key][-chain_depth:]):
            limit = min(maximum, len(data) - pos)
            length = minimum
            while length < limit and data[source + length] == data[pos + length]:
                length += 1
            if length > best_length:
                best_length, best_distance = length, pos - source
                if length == limit:
                    break
        matches[pos] = best_length, best_distance
        positions[key].append(pos)

    # Literal bytes cost one byte. A match token has a fixed, deliberately
    # explicit cost. DP prevents greedy long matches from hiding better parses.
    n = len(data)
    cost = [0] * (n + 1)
    choice = [0] * n
    tree_size = 1
    while tree_size < n + 1:
        tree_size *= 2
    infinity = n + 1
    tree = [(infinity, 0)] * (2 * tree_size)

    def update(index: int, value: int) -> None:
        node = tree_size + index
        tree[node] = (value, index)
        node //= 2
        while node:
            tree[node] = min(tree[node * 2], tree[node * 2 + 1])
            node //= 2

    def range_min(left: int, right: int) -> tuple[int, int]:
        result = (infinity, 0)
        left += tree_size
        right += tree_size
        while left < right:
            if left & 1:
                result = min(result, tree[left])
                left += 1
            if right & 1:
                right -= 1
                result = min(result, tree[right])
            left //= 2
            right //= 2
        return result

    update(n, 0)
    for pos in range(n - 1, -1, -1):
        cost[pos] = 1 + cost[pos + 1]
        best_length, _ = matches[pos]
        if best_length >= minimum:
            first = pos + max(minimum, token_bytes + 1)
            value, endpoint = range_min(first, pos + best_length + 1)
            candidate = token_bytes + value
            if candidate < cost[pos]:
                cost[pos] = candidate
                choice[pos] = endpoint - pos
        update(pos, cost[pos])

    phrases = []
    literal_bytes = 0
    pos = 0
    while pos < n:
        length = choice[pos]
        if length:
            phrases.append((pos, length, matches[pos][1], data[pos : pos + length]))
            pos += length
        else:
            literal_bytes += 1
            pos += 1
    return cost[0], literal_bytes, phrases


def select_and_encode(data: bytes, phrases, capacity: int,
                      reference_bytes: int = 2, entry_bytes: int = 3) -> int:
    counts = Counter(content for _, _, _, content in phrases)
    ranked = sorted(
        ((content, count,
          (len(content) - reference_bytes) * count - entry_bytes)
         for content, count in counts.items() if count >= 2),
        key=lambda item: (-item[2], -item[1], -len(item[0]), item[0]))
    selected = [content for content, _, gain in ranked[:capacity] if gain > 0]
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
    return cost[0] + len(selected) * entry_bytes


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--candidates", type=Path)
    parser.add_argument("--limit-mib", type=int, default=0)
    parser.add_argument("--min-match", type=int, default=4)
    parser.add_argument("--max-match", type=int, default=256)
    parser.add_argument("--chain-depth", type=int, default=64)
    parser.add_argument("--token-bytes", type=int, default=3)
    parser.add_argument("--select-k", default="8,16,32,64")
    args = parser.parse_args()

    limit = args.limit_mib * 1024 * 1024 if args.limit_mib else None
    with args.input.open("rb") as source:
        raw = source.read(limit)
    raw = raw[: len(raw) // REGION * REGION]
    if not raw:
        raise ValueError("input has no complete 4KiB region")

    histogram = Counter()
    content_frequency = Counter()
    capacities = tuple(int(value) for value in args.select_k.split(",") if value)
    rows = []
    total_encoded = total_matched = total_phrases = 0
    for region_index, begin in enumerate(range(0, len(raw), REGION)):
        encoded, literals, phrases = analyze_region(
            raw[begin : begin + REGION], args.min_match, args.max_match,
            args.chain_depth, args.token_bytes)
        matched = sum(length for _, length, _, _ in phrases)
        for _, length, _, content in phrases:
            histogram[bucket(length)] += 1
            # Limit the key to the selected phrase. This measures whether the
            # same variable string recurs beyond a single source/use pair.
            content_frequency[content] += 1
        row = {
            "region": region_index,
            "encoded_bytes": encoded,
            "matched_bytes": matched,
            "literal_bytes": literals,
            "phrases": len(phrases),
            "tier_1k": int(encoded <= 1024),
            "tier_2k": int(encoded <= 2048),
            "tier_3k": int(encoded <= 3072),
        }
        for capacity in capacities:
            row[f"varsel_{capacity}"] = select_and_encode(
                raw[begin : begin + REGION], phrases, capacity)
        rows.append(row)
        total_encoded += encoded
        total_matched += matched
        total_phrases += len(phrases)

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)

    if args.candidates:
        args.candidates.parent.mkdir(parents=True, exist_ok=True)
        ranked = sorted(
            ((content, count, (len(content) - args.token_bytes) * count)
             for content, count in content_frequency.items() if count >= 2),
            key=lambda item: (-item[2], -item[1], -len(item[0]), item[0]))
        with args.candidates.open("w", newline="") as output:
            writer = csv.writer(output)
            writer.writerow(("length", "count", "gross_saved_bytes", "hex"))
            for content, count, gain in ranked:
                writer.writerow((len(content), count, gain, content.hex()))

    repeated_instances = sum(count for count in content_frequency.values() if count >= 2)
    repeated_kinds = sum(count >= 2 for count in content_frequency.values())
    print(f"regions={len(rows)} original_bytes={len(raw)} encoded_bytes={total_encoded}")
    print(f"matched_bytes={total_matched} coverage={total_matched / len(raw):.6%} "
          f"phrases={total_phrases}")
    print(f"repeated_phrase_kinds={repeated_kinds} repeated_phrase_instances={repeated_instances}")
    print("length_histogram=" + ",".join(
        f"{name}:{histogram[name]}" for name in
        ("<=4", "<=8", "<=16", "<=32", "<=64", "<=128", "<=256", ">256")))
    print("tiers=" + ",".join(
        f"{name}:{sum(row[name] for row in rows)}"
        for name in ("tier_1k", "tier_2k", "tier_3k")))
    for capacity in capacities:
        name = f"varsel_{capacity}"
        values = [row[name] for row in rows]
        print(f"{name}=bytes:{sum(values)},<=1k:{sum(x <= 1024 for x in values)},"
              f"<=2k:{sum(x <= 2048 for x in values)},"
              f"<=3k:{sum(x <= 3072 for x in values)}")


if __name__ == "__main__":
    main()
