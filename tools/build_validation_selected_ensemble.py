#!/usr/bin/env python3
"""Build leakage-safe Byte-Select ensembles from training-derived models.

Candidate patterns come only from models trained on training partitions.  The
validation traces are used solely to rank candidates by marginal allocated-byte
savings.  The held-out test trace is never opened here.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

import numpy as np

Pattern = Tuple[int, ...]
SetKey = Tuple[int, int, int, int]  # target, metadata bytes, tag bits, tag value


def read_model(path: Path) -> Tuple[int, Dict[SetKey, List[Pattern]]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    block_size = int(lines[1].split()[1])
    result: Dict[SetKey, List[Pattern]] = {}
    current: SetKey | None = None
    for line in lines[3:]:
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "set":
            current = tuple(map(int, fields[1:5]))  # type: ignore[assignment]
            result[current] = []
        elif fields[0] == "pattern" and current is not None:
            result[current].append(tuple(map(int, fields[1:])))
    return block_size, result


def describes(blocks: np.ndarray, pattern: Pattern) -> np.ndarray:
    ok = np.ones(len(blocks), dtype=bool)
    representatives: Dict[int, int] = {}
    for position, symbol in enumerate(pattern):
        if symbol in representatives:
            ok &= blocks[:, position] == blocks[:, representatives[symbol]]
        else:
            representatives[symbol] = position
    return ok


def prune_redundant(
    candidates: Sequence[Pattern], blocks: np.ndarray
) -> Tuple[List[Pattern], List[np.ndarray], int]:
    """Remove validation-equivalent patterns, retaining the lower-rank form.

    This is a conservative similarity prune: two candidates are merged only
    when they describe exactly the same validation blocks.  It cannot discard
    validation coverage merely because two masks have a high Jaccard score.
    """
    best_by_mask: Dict[bytes, Pattern] = {}
    mask_by_key: Dict[bytes, np.ndarray] = {}
    for pattern in sorted(set(candidates), key=lambda p: (max(p) + 1, p)):
        mask = describes(blocks, pattern)
        key = np.packbits(mask).tobytes()
        if key not in best_by_mask:
            best_by_mask[key] = pattern
            mask_by_key[key] = mask
    ordered_keys = sorted(best_by_mask, key=lambda k: (max(best_by_mask[k]) + 1, best_by_mask[k]))
    patterns = [best_by_mask[key] for key in ordered_keys]
    masks = [mask_by_key[key] for key in ordered_keys]
    return patterns, masks, len(set(candidates)) - len(patterns)


def select_by_marginal_savings(
    pools: Dict[SetKey, List[Pattern]], blocks: np.ndarray, limits: Dict[int, int]
) -> Tuple[Dict[SetKey, List[Pattern]], Dict[SetKey, int]]:
    """Jointly maximize validation-set 8/16/32/64-byte allocation savings."""
    candidates: Dict[SetKey, List[Pattern]] = {}
    masks: Dict[SetKey, List[np.ndarray]] = {}
    pruned: Dict[SetKey, int] = {}
    for key, pool in pools.items():
        candidates[key], masks[key], pruned[key] = prune_redundant(pool, blocks)

    selected: Dict[SetKey, List[Pattern]] = {key: [] for key in pools}
    remaining: Dict[SetKey, List[int]] = {
        key: list(range(len(candidates[key]))) for key in pools
    }
    allocated = np.full(len(blocks), 64, dtype=np.int16)

    while True:
        best = None
        best_score = (-1, -1, ())
        for key in sorted(pools):
            target = key[0]
            if len(selected[key]) >= limits[target]:
                continue
            improvable = allocated > target
            for index in remaining[key]:
                gain = int(np.sum((allocated - target) * (masks[key][index] & improvable)))
                rank = max(candidates[key][index]) + 1
                score = (gain, -rank, tuple(-x for x in candidates[key][index]))
                if score > best_score:
                    best_score = score
                    best = (key, index)
        if best is None or best_score[0] <= 0:
            break
        key, index = best
        target = key[0]
        selected[key].append(candidates[key][index])
        allocated[masks[key][index] & (allocated > target)] = target
        remaining[key].remove(index)
    return selected, pruned


def write_model(path: Path, block_size: int, sets: Sequence[Tuple[SetKey, Sequence[Pattern]]]) -> None:
    lines = ["BSEL_MODEL 2", f"block_size {block_size}", f"sets {len(sets)}"]
    for (target, metadata, tag_bits, tag_value), patterns in sets:
        lines.append(f"set {target} {metadata} {tag_bits} {tag_value} {len(patterns)}")
        lines.extend("pattern " + " ".join(map(str, pattern)) for pattern in patterns)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--thresholds", default="",
                        help="optional comma-separated thresholds; ignores best/other models")
    parser.add_argument("--include-best", action="store_true",
                        help="also include phase-best models in a threshold-filtered pool")
    parser.add_argument("--validation", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--keep-all", action="store_true")
    args = parser.parse_args()

    model_paths = sorted(args.models.glob(f"{args.prefix}*.model"))
    if args.thresholds:
        allowed = {int(value) for value in args.thresholds.split(",")}
        model_paths = [path for path in model_paths
                       if ((match := re.search(r"-t(\d+)\.model$", path.name))
                           and int(match.group(1)) in allowed)
                       or (args.include_best and path.name.endswith("-best.model"))]
    if not model_paths:
        raise SystemExit("no candidate models")
    block_size = 0
    pools: Dict[SetKey, List[Pattern]] = {}
    for path in model_paths:
        current_size, sets = read_model(path)
        if block_size and current_size != block_size:
            raise SystemExit("candidate model block-size mismatch")
        block_size = current_size
        for key, patterns in sets.items():
            pools.setdefault(key, []).extend(patterns)

    validation_blocks = []
    for validation in args.validation:
        raw = np.fromfile(validation, dtype=np.uint8)
        validation_blocks.append(raw[: len(raw) // block_size * block_size].reshape(-1, block_size))
    blocks = np.concatenate(validation_blocks)
    limits = {32: 1024, 16: 1024, 8: 128}
    selected_sets = []
    optimized = None
    pruned: Dict[SetKey, int] = {}
    if not args.keep_all:
        optimized, pruned = select_by_marginal_savings(pools, blocks, limits)
    for key in sorted(pools, reverse=True):
        if args.keep_all:
            unique = sorted(set(pools[key]))
            scores = {pattern: int(np.count_nonzero(describes(blocks, pattern))) for pattern in unique}
            # A lower-rank describing pattern has a smaller actual dictionary;
            # validation coverage breaks ties without consulting the test set.
            selected = sorted(unique, key=lambda pattern: (max(pattern) + 1, -scores[pattern], pattern))[
                : limits[key[0]]
            ]
        else:
            assert optimized is not None
            selected = sorted(optimized[key], key=lambda pattern: (max(pattern) + 1, pattern))
        selected_sets.append((key, selected))
        print(f"target={key[0]} candidates={len(set(pools[key]))} "
              f"similarity_pruned={pruned.get(key, 0)} selected={len(selected)}")
    write_model(args.output, block_size, selected_sets)


if __name__ == "__main__":
    main()
