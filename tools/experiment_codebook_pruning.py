#!/usr/bin/env python3
"""Find the smallest static Huffman-table subset preserving tier crossings."""
from __future__ import annotations

import argparse
import itertools
import json
import math
from collections import Counter
from pathlib import Path

from experiment_2k_to_1k_algorithms import (
    huff_classmatch_lzss_decode,
    huff_classmatch_lzss_encode,
    huff_shortmatch_lzss_decode,
    huff_shortmatch_lzss_encode,
    read_payloads,
    train_3k_rescue_huffman_lengths,
    train_size_bucket_huffman_lengths,
)


def quantized(size: int) -> int:
    return 0 if size == 0 else math.ceil(size / 1024) * 1024


def encoded_size(blob: bytes, table: list[int], codec: str, candidates: int) -> int:
    if codec == "class":
        stream = huff_classmatch_lzss_encode(blob, table, candidates)
        decoded = huff_classmatch_lzss_decode(stream, len(blob), table)
    else:
        stream = huff_shortmatch_lzss_encode(blob, table, candidates)
        decoded = huff_shortmatch_lzss_decode(stream, len(blob), table)
    if decoded != blob:
        raise RuntimeError("round-trip mismatch")
    return min(len(blob), len(stream))


def tier_target(size: int):
    if 3072 < size <= 4096:
        return "4K->3K", 3072, "short"
    if 2048 < size <= 3072:
        return "3K->2K", 2048, "class"
    if 1024 < size <= 2048:
        return "2K->1K", 1024, "short"
    return None


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--train-payloads", type=Path, required=True)
    parser.add_argument("--payloads", type=Path, required=True)
    parser.add_argument("--buckets", type=int, default=4)
    parser.add_argument("--lz-candidates", type=int, default=64)
    args = parser.parse_args()
    train = read_payloads(args.train_payloads)
    test = read_payloads(args.payloads)
    multi = train_size_bucket_huffman_lengths(train, args.buckets)
    rescue = train_3k_rescue_huffman_lengths(train, args.buckets)
    named_tables = [(f"M{idx}", table) for idx, table in enumerate(multi)]
    named_tables += [(f"R{idx}", table) for idx, table in enumerate(rescue) if table not in multi]
    def collect(regions):
        coverage = {name: set() for name, _table in named_tables}
        savings = {name: {} for name, _table in named_tables}
        encoded = {name: {} for name, _table in named_tables}
        before_sizes = {}
        eligible = {}
        for region in regions:
            blob = region.raw_blob()
            before_sizes[region.index] = len(blob)
            target_info = tier_target(len(blob))
            if target_info is None:
                continue
            tier, target, codec = target_info
            eligible[tier] = eligible.get(tier, 0) + 1
            for name, table in named_tables:
                size = encoded_size(blob, table, codec, args.lz_candidates)
                encoded[name][region.index] = size
                if size <= target:
                    coverage[name].add((tier, region.index))
                    savings[name][(tier, region.index)] = target - size + 1
        return coverage, savings, encoded, before_sizes, eligible

    train_coverage, train_savings, _train_encoded, _train_before, train_eligible = collect(train)
    full_train = set().union(*train_coverage.values())
    universe = {item: bit for bit, item in enumerate(sorted(full_train))}
    table_masks = {
        name: sum(1 << universe[item] for item in covered)
        for name, covered in train_coverage.items()
    }
    full_mask = (1 << len(universe)) - 1
    # Tables whose coverage is a subset of another table can never improve a
    # minimum-cardinality cover.  Removing them keeps the exact result while
    # avoiding the previous set-heavy combinatorial search.
    useful_names = []
    all_names = [name for name, _table in named_tables]
    for name in all_names:
        mask = table_masks[name]
        dominated = any(
            other != name and mask != table_masks[other]
            and (mask | table_masks[other]) == table_masks[other]
            for other in all_names
        )
        duplicate = any(
            other < name and table_masks[other] == mask for other in all_names
        )
        if mask and not dominated and not duplicate:
            useful_names.append(name)
    best = None
    for count in range(1, len(useful_names) + 1):
        for combo in itertools.combinations(useful_names, count):
            union_mask = 0
            for name in combo:
                union_mask |= table_masks[name]
            if union_mask == full_mask:
                # Among equally small exact covers, prefer the combination
                # with the largest total threshold margin.  This is selected
                # entirely on training data and is more robust than a name-
                # order tie break for near-threshold regions.
                margin = sum(
                    max(train_savings[name].get(item, 0) for name in combo)
                    for item in full_train
                )
                candidate = (count, -margin, combo)
                if best is None or candidate < best:
                    best = candidate
        if best is not None:
            break
    selected = list(best[2]) if best else []
    test_coverage, _test_savings, test_encoded, test_before, test_eligible = collect(test)
    full_test = set().union(*test_coverage.values())
    selected_test = set().union(*(test_coverage[name] for name in selected)) if selected else set()
    add_one_test = {}
    for name, _table in named_tables:
        if name in selected:
            continue
        augmented = selected_test | test_coverage[name]
        add_one_test[name] = {
            tier: sum(1 for item in augmented if item[0] == tier)
            for tier in test_eligible
        }
    full_per_tier = {tier: sum(1 for item in full_test if item[0] == tier) for tier in test_eligible}
    selected_per_tier = {tier: sum(1 for item in selected_test if item[0] == tier) for tier in test_eligible}
    repeated = Counter(payload for region in train for payload in region.payloads)

    def dictionary(entries: int):
        values = [value for value, _count in repeated.most_common(entries)]
        return set(values), sum(len(value) + 2 for value in values)

    def metrics(table_names, repeat_entries: int):
        repeat_bank, dictionary_bytes = dictionary(repeat_entries)
        before_total = after_total = quantized_before = quantized_after = 0
        evaluated = [region for region in test if len(region.raw_blob()) <= 4096]
        for region in evaluated:
            before = test_before[region.index]
            after = before
            if table_names and tier_target(before) is not None:
                after = min([before] + [test_encoded[name][region.index] for name in table_names])
            if (0 < before <= 1024 and len(set(region.payloads)) == 1
                    and region.payloads[0] in repeat_bank):
                after = 0
            before_total += before
            after_total += after
            quantized_before += quantized(before)
            quantized_after += quantized(after)
        original = len(evaluated) * 4096
        table_count = len(table_names)
        # Exact size of the compact programmable serialization used here:
        # 256 four-bit code lengths + 15 uint16 values for each of bl_count,
        # first_code, and first_symbol.
        huffman_lengths_bytes = table_count * 128
        huffman_aux_bytes = table_count * 90
        codebook_bytes = dictionary_bytes + huffman_lengths_bytes + huffman_aux_bytes
        return {
            "regions": len(evaluated),
            "original_bytes": original,
            "algorithm_before_bytes": before_total,
            "algorithm_after_bytes": after_total,
            "quantized_before_bytes": quantized_before,
            "quantized_after_bytes": quantized_after,
            "algorithm_compression_ratio": after_total / original if original else 0.0,
            "quantized_compression_ratio": quantized_after / original if original else 0.0,
            "algorithm_ratio_including_codebook": (after_total + codebook_bytes) / original if original else 0.0,
            "quantized_ratio_including_codebook": (quantized_after + codebook_bytes) / original if original else 0.0,
            "algorithm_saving_rate": 1.0 - after_total / original if original else 0.0,
            "quantized_saving_rate": 1.0 - quantized_after / original if original else 0.0,
            "repeat_entries": repeat_entries,
            "repeat_dictionary_bytes": dictionary_bytes,
            "huffman_tables": table_count,
            "huffman_code_lengths_bytes": huffman_lengths_bytes,
            "huffman_decoder_aux_bytes": huffman_aux_bytes,
            "huffman_table_bytes": huffman_lengths_bytes + huffman_aux_bytes,
            "total_static_codebook_bytes": codebook_bytes,
            "direct_rom_bytes": dictionary_bytes + table_count * 768,
        }

    all_names = [name for name, _table in named_tables]
    output = {
        "train_eligible": train_eligible,
        "test_eligible": test_eligible,
        "tables_trained": len(named_tables),
        "table_names": [name for name, _table in named_tables],
        "full_test_crossings": full_per_tier,
        "selected_test_crossings": selected_per_tier,
        "minimum_tables_selected_on_train": best[0] if best else 0,
        "train_cover_margin_bytes": -best[1] if best else 0,
        "selected_tables": selected,
        "diagnostic_add_one_test_crossings": add_one_test,
        "method_metrics": {
            "top256-full": metrics(all_names, 256),
            "top4-full": metrics(all_names, 4),
            "top4-pruned": metrics(selected, 4),
        },
    }
    print(json.dumps(output, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
