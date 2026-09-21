#!/usr/bin/env python3
"""Reproducible 128-entry Custom-FPC prefix MPHF experiment.

The generated key is the 64-bit packed form of sixteen 4-bit FPC tags.  The
tag values and their payload widths are taken from fpc-bsel.v2/src/fpc.cpp.
This script writes a deterministic synthetic codebook, evaluates cheap
nibble-feature indexes, and optionally invokes a locally built CMPH helper.
"""

from __future__ import annotations

import argparse
import itertools
import json
import random
import subprocess
import tempfile
from pathlib import Path

TAG_WEIGHTS = (0, 0, 8, 8, 16, 16, 8, 16, 16, 16, 16, 2, 10, 18, 18, 32)
N = 128
NIBBLES = 16


def make_codebook(seed: int) -> list[tuple[int, ...]]:
    # Generate prefixes by applying the actual Custom-FPC classifier to
    # synthetic 32-bit words.  This preserves position/history constraints on
    # tags 0..15 instead of sampling arbitrary nibble strings.
    rng = random.Random(seed)
    keys: list[tuple[int, ...]] = []
    while len(keys) < N:
        history: list[int] = []
        tags: list[int] = []
        for word in range(NIBBLES):
            value = synthetic_word(rng, history, word)
            tags.append(classify_tag(value, history, word))
            history.append(value)
        row = tuple(tags)
        if row not in keys:
            keys.append(row)
    return keys


def byte_at(value: int, index: int) -> int:
    return (value >> (index * 8)) & 0xFF


def classify_tag(value: int, history: list[int], word: int) -> int:
    b0, b1, b2, b3 = (byte_at(value, i) for i in range(4))
    best = 15

    def consider(tag: int) -> None:
        nonlocal best
        if TAG_WEIGHTS[tag] < TAG_WEIGHTS[best]:
            best = tag

    if value == 0: consider(0)
    if value == 0xFFFFFFFF: consider(1)
    if b3 == 0 and b2 == 0 and b1 == 0: consider(2)
    if b2 == 0 and b1 == 0 and b0 == 0: consider(3)
    if b3 == 0 and b2 == 0: consider(4)
    if b1 == 0 and b0 == 0: consider(5)
    if b3 == 0xFF and b2 == 0xFF and b1 == 0xFF: consider(6)
    if b3 == 0xFF and b2 == 0xFF: consider(7)
    if b3 == b1 and b2 == b0: consider(8)
    if b3 == 0 and b1 == 0: consider(9)
    if b2 == 0 and b0 == 0: consider(10)
    for distance in range(1, min(4, word) + 1):
        other = history[word - distance]
        o1, o2, o3 = byte_at(other, 1), byte_at(other, 2), byte_at(other, 3)
        tag = 15
        if value == other: tag = 11
        elif b3 == o3 and b2 == o2 and b1 == o1: tag = 12
        elif b3 == o3 and b2 == o2: tag = 13
        elif b3 == o3 and b1 == o1: tag = 14
        consider(tag)
    return best


def synthetic_word(rng: random.Random, history: list[int], word: int) -> int:
    typ = rng.randrange(20)
    if typ == 0: return 0
    if typ == 1: return 0xFFFFFFFF
    if typ == 2: return rng.randrange(256)
    if typ == 3: return rng.randrange(256) << 24
    if typ == 4: return rng.randrange(65536)
    if typ == 5: return rng.randrange(65536) << 16
    if typ == 6: return 0xFFFFFF00 | rng.randrange(256)
    if typ == 7: return 0xFFFF0000 | rng.randrange(65536)
    if typ == 8:
        a, b = rng.randrange(256), rng.randrange(256)
        return a | (b << 8) | (a << 16) | (b << 24)
    if typ == 9:
        a, b = rng.randrange(256), rng.randrange(256)
        return b | (a << 16)
    if typ == 10:
        a, b = rng.randrange(256), rng.randrange(256)
        return (b << 24) | (a << 8)
    if history and typ < 16:
        return history[rng.randrange(len(history))] ^ (rng.getrandbits(16) << 16)
    return rng.getrandbits(32)


def pack(row: tuple[int, ...]) -> int:
    # Match pack_tags(): first tag is the high nibble of byte 0.
    value = 0
    for i, tag in enumerate(row):
        value |= tag << ((NIBBLES - 1 - i) * 4)
    return value


def feature(row: tuple[int, ...], positions: tuple[int, ...]) -> int:
    value = 0
    for pos in positions:
        value = (value << 4) | row[pos]
    return value


def feature_stats(keys: list[tuple[int, ...]], positions: tuple[int, ...]) -> dict:
    buckets: dict[int, list[int]] = {}
    for i, key in enumerate(keys):
        buckets.setdefault(feature(key, positions), []).append(i)
    sizes = [len(v) for v in buckets.values()]
    return {
        "positions": list(positions),
        "bucket_count": len(buckets),
        "max_candidates": max(sizes),
        "mean_candidates_on_hit": sum(s * s for s in sizes) / len(keys),
        "mean_bucket_size": sum(sizes) / len(sizes),
        "singletons": sum(s == 1 for s in sizes),
        "buckets": {str(k): v for k, v in buckets.items()},
    }


def best_features(keys: list[tuple[int, ...]], width: int) -> dict:
    best = None
    for positions in itertools.combinations(range(NIBBLES), width):
        stats = feature_stats(keys, positions)
        score = (stats["max_candidates"], stats["mean_candidates_on_hit"], -stats["singletons"])
        if best is None or score < best[0]:
            best = (score, stats)
    assert best is not None
    return best[1]


def multiplier_search(keys: list[tuple[int, ...]], attempts: int, seed: int) -> dict:
    # Test the Gemini-style family, using a full 64-bit mix and the top 7 bits.
    # This is deliberately measured rather than assumed to succeed.
    values = [pack(x) for x in keys]
    rng = random.Random(seed)
    best = {"attempts": attempts, "success": False, "best_unique": 0}
    for _ in range(attempts):
        multiplier = rng.getrandbits(64) | 1
        shift = rng.randrange(1, 64)
        out = [(((x ^ (x >> shift)) * multiplier) >> 57) & 0x7F for x in values]
        unique = len(set(out))
        if unique > best["best_unique"]:
            best.update({"best_unique": unique, "multiplier": multiplier, "shift": shift})
        if unique == N:
            best.update({"success": True})
            break
    return best


def run_cmph(helper: Path, keys: list[tuple[int, ...]], algo: str) -> dict:
    with tempfile.TemporaryDirectory(prefix="mphf-keys-") as td:
        keyfile = Path(td) / "keys.bin"
        keyfile.write_bytes(b"".join(pack(k).to_bytes(8, "big") for k in keys))
        result = subprocess.run([str(helper), algo, str(keyfile)], text=True,
                                capture_output=True, check=False)
        if result.returncode:
            return {"algorithm": algo, "success": False, "stderr": result.stderr.strip()}
        data = json.loads(result.stdout)
        data["algorithm"] = algo
        return data


def main() -> None:
    global N
    ap = argparse.ArgumentParser()
    ap.add_argument("--output-dir", type=Path, default=Path("results/mphf-128"))
    ap.add_argument("--seed", type=int, default=0xC0570F0C)
    ap.add_argument("--entries", type=int, default=128,
                    help="number of generated codebook entries (1..128)")
    ap.add_argument("--cmph-helper", type=Path)
    ap.add_argument("--multiplier-attempts", type=int, default=100_000)
    args = ap.parse_args()
    if not 1 <= args.entries <= 128:
        ap.error("--entries must be between 1 and 128")
    N = args.entries
    args.output_dir.mkdir(parents=True, exist_ok=True)

    keys = make_codebook(args.seed)
    values = [pack(k) for k in keys]
    assert len(set(values)) == N
    (args.output_dir / "codebook.bin").write_bytes(
        b"".join(value.to_bytes(8, "big") for value in values))
    (args.output_dir / "codebook.json").write_text(json.dumps({
        "description": "128 packed Custom-FPC headers; 16 legal 4-bit tags per 8-byte key",
        "seed": args.seed,
        "tag_payload_bits": list(TAG_WEIGHTS),
        "entries": [{"id": i, "tags": list(row), "key_hex": f"{value:016x}"}
                    for i, (row, value) in enumerate(zip(keys, values))],
    }, indent=2) + "\n")

    report = {
        "seed": args.seed,
        "entries": N,
        "key_bits": 64,
        "feature_search": {str(w): best_features(keys, w) for w in range(1, 5)},
        "multiplier_search": multiplier_search(keys, args.multiplier_attempts, args.seed ^ 0x1234),
    }
    if args.cmph_helper:
        report["cmph"] = {algo: run_cmph(args.cmph_helper, keys, algo)
                          for algo in ("FCH", "BDZ", "CHD")}
    (args.output_dir / "experiment.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
