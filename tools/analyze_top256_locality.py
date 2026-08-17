#!/usr/bin/env python3
"""Compare spatial policies for the FPC residual-word Top-256 model."""

from __future__ import annotations

import argparse
import csv
import math
import struct
from collections import Counter, OrderedDict
from pathlib import Path

BLOCK = 256
REGION_BLOCKS = 16


def sign_extended(value: int, bits: int) -> bool:
    mask = (1 << bits) - 1
    low = value & mask
    sign = 1 << (bits - 1)
    extended = low | (~mask & 0xFFFFFFFF) if low & sign else low
    return value == extended & 0xFFFFFFFF


def tag(value: int) -> int:
    if value == 0: return 0
    if sign_extended(value, 4): return 1
    byte = value & 0xFF
    if value == byte * 0x01010101: return 2
    if sign_extended(value, 8): return 3
    if sign_extended(value, 16): return 4
    if value & 0xFFFF == 0: return 5
    lo, hi = value & 0xFFFF, value >> 16
    half_sext = lambda half: half == ((half & 0xFF) | (0xFF00 if half & 0x80 else 0))
    if half_sext(lo) and half_sext(hi): return 6
    return 7


PAYLOAD = (0, 4, 8, 8, 16, 16, 16, 0)


def parts(block: bytes):
    values = struct.unpack("<64I", block)
    tags = [tag(value) for value in values]
    residuals = [value for value, kind in zip(values, tags) if kind == 7]
    runs = 1 + sum(left != right for left, right in zip(tags, tags[1:]))
    tag_bits = 1 + min(len(tags) * 3, runs * (3 + math.ceil(math.log2(len(tags)))))
    regular_bits = sum(PAYLOAD[kind] for kind in tags)
    return residuals, tag_bits + regular_bits


def train(path: Path, maximum_bytes: int) -> set[int]:
    counts = Counter()
    with path.open("rb") as source:
        remaining = maximum_bytes
        while remaining >= BLOCK:
            block = source.read(BLOCK)
            if len(block) != BLOCK: break
            residuals, _ = parts(block)
            counts.update(residuals)
            remaining -= BLOCK
    return {value for value, _ in counts.most_common(256)}


def encoded_size(block: bytes, global_words: set[int], local_words: set[int]) -> int:
    values = struct.unpack("<64I", block)
    tags = [tag(value) for value in values]
    residuals, fixed_bits = parts(block)
    bits = fixed_bits
    for value in residuals:
        if value in local_words:
            bits += 1 + 4
        elif value in global_words:
            bits += 2 + 8
        else:
            bits += 2 + 32
    direct_bits = len(tags) * 3 + sum(
        32 if kind == 7 else PAYLOAD[kind] for kind in tags)
    return min(BLOCK, (bits + 7) // 8, (direct_bits + 7) // 8)


def q(size: int) -> int:
    return min(BLOCK, max(64, ((size + 63) // 64) * 64))


def evaluate(data: bytes, global_words: set[int], policy: str, capacity: int):
    result = []
    lru: OrderedDict[int, None] = OrderedDict()
    frequency = Counter()
    region_oracle: set[int] = set()
    for index in range(0, len(data), BLOCK):
        region_pos = (index // BLOCK) % REGION_BLOCKS
        if region_pos == 0:
            lru.clear(); frequency.clear()
            region = data[index:index + BLOCK * REGION_BLOCKS]
            counts = Counter()
            for off in range(0, len(region), BLOCK):
                residuals, _ = parts(region[off:off + BLOCK])
                counts.update(residuals)
            region_oracle = {value for value, _ in counts.most_common(capacity)}
        block = data[index:index + BLOCK]
        if policy == "global": local = set()
        elif policy == "lru": local = set(lru)
        elif policy == "lfu":
            local = {value for value, _ in frequency.most_common(capacity)}
        elif policy == "oracle": local = region_oracle
        else: raise ValueError(policy)
        result.append(q(encoded_size(block, global_words, local)))
        residuals, _ = parts(block)
        frequency.update(residuals)
        for value in residuals:
            if value in lru: lru.move_to_end(value)
            else:
                lru[value] = None
                if len(lru) > capacity: lru.popitem(last=False)
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("train", type=Path)
    parser.add_argument("test", type=Path)
    parser.add_argument("--train-mib", type=int, default=16)
    parser.add_argument("--test-mib", type=int, default=1)
    parser.add_argument("--capacity", type=int, default=16)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    global_words = train(args.train, args.train_mib * 1024 * 1024)
    with args.test.open("rb") as source:
        data = source.read(args.test_mib * 1024 * 1024)
    data = data[:len(data) // (BLOCK * REGION_BLOCKS) * (BLOCK * REGION_BLOCKS)]
    policies = {name: evaluate(data, global_words, name, args.capacity)
                for name in ("global", "lru", "lfu", "oracle")}
    rows = []
    for begin in range(0, len(data) // BLOCK, REGION_BLOCKS):
        row = {name: sum(values[begin:begin + REGION_BLOCKS])
               for name, values in policies.items()}
        row["region"] = begin // REGION_BLOCKS
        rows.append(row)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=("region", "global", "lru", "lfu", "oracle"))
            writer.writeheader(); writer.writerows(rows)
    print(f"regions={len(rows)} global_words={len(global_words)} capacity={args.capacity}")
    for name in policies:
        sizes = [row[name] for row in rows]
        print(name, f"bytes={sum(sizes)} <=1k={sum(x<=1024 for x in sizes)}",
              f"<=2k={sum(x<=2048 for x in sizes)} <=3k={sum(x<=3072 for x in sizes)}")
    for name in ("lru", "lfu", "oracle"):
        print(name + "_crossings_from_global=" + str(sum(
            (row[name] <= 1024 < row["global"]) or
            (row[name] <= 2048 < row["global"]) or
            (row[name] <= 3072 < row["global"]) for row in rows)))


if __name__ == "__main__": main()
