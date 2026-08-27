#!/usr/bin/env python3
"""Zero-codebook causal dynamic-prefix experiment on FPC payload regions."""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
from experiment_2k_to_1k_algorithms import BitReader, BitWriter, Region, read_payloads


TIERS = ((0, 1024, 1), (1024, 2048, 2), (2048, 3072, 3), (3072, 4096, 4))


def tier_kib(size: int) -> int:
    if size == 0:
        return 0
    return math.ceil(size / 1024)


@dataclass
class PrefixCache:
    capacity: int
    prefix_lengths: tuple[int, ...]
    policy: str = "fifo"
    contextual: bool = False
    stats: dict | None = None

    def __post_init__(self) -> None:
        self.entries: list[bytes] = []
        self.scores: list[int] = []
        self.contexts: list[int] = []
        self.next_slot = 0

    def clear(self) -> None:
        self.entries.clear()
        self.scores.clear()
        self.contexts.clear()
        self.next_slot = 0
        if self.stats is not None:
            self.stats["resets"] += 1

    def best(self, chunk: bytes, context: int = 0) -> tuple[int, bytes] | None:
        matches = [
            (idx, prefix)
            for idx, prefix in enumerate(self.entries)
            if len(prefix) < len(chunk) and chunk.startswith(prefix)
            and (not self.contextual or self.contexts[idx] == context)
        ]
        result = max(matches, key=lambda item: len(item[1]), default=None)
        if self.stats is not None:
            self.stats["lookups"] += 1
            self.stats["occupancy_sum"] += len(self.entries)
            self.stats["peak_entries"] = max(self.stats["peak_entries"], len(self.entries))
            if result is None:
                self.stats["misses"] += 1
            else:
                self.stats["hits"] += 1
                self.stats["hits_by_prefix_length"][str(len(result[1]))] += 1
        return result

    def touch(self, index: int) -> None:
        if self.policy == "clock":
            self.scores[index] = min(3, self.scores[index] + 1)

    def _replacement_slot(self) -> int:
        if self.policy == "fifo":
            slot = self.next_slot
            self.next_slot = (self.next_slot + 1) % self.capacity
            return slot
        # Deterministic 2-bit CLOCK. Encoder and decoder observe the same
        # reference tokens and therefore reproduce identical replacement.
        while self.scores[self.next_slot] > 0:
            self.scores[self.next_slot] -= 1
            self.next_slot = (self.next_slot + 1) % self.capacity
        slot = self.next_slot
        self.next_slot = (self.next_slot + 1) % self.capacity
        return slot

    def update(self, chunk: bytes, context: int = 0) -> None:
        for length in self.prefix_lengths:
            if length >= len(chunk):
                continue
            prefix = chunk[:length]
            if any(prefix == value and (not self.contextual or self.contexts[idx] == context)
                   for idx, value in enumerate(self.entries)):
                continue
            if len(self.entries) < self.capacity:
                self.entries.append(prefix)
                self.scores.append(0)
                self.contexts.append(context)
                if self.stats is not None:
                    self.stats["insertions"] += 1
            else:
                slot = self._replacement_slot()
                self.entries[slot] = prefix
                self.scores[slot] = 0
                self.contexts[slot] = context
                if self.stats is not None:
                    self.stats["evictions"] += 1


def length_header(region: Region) -> bytes:
    return b"".join(len(payload).to_bytes(2, "little") for payload in region.payloads)


def dynamic_prefix_encode(
    region: Region,
    cache_entries: int,
    reset_subline_interval: int,
    word_bytes: int,
    prefix_lengths: tuple[int, ...],
    previous_prefix: bool = False,
    previous_subline_prefix: bool = False,
    cache_policy: str = "fifo",
    cache_runs: bool = False,
    context_bits: int = 0,
    stats: dict | None = None,
) -> bytes:
    if cache_entries <= 0 or cache_entries & (cache_entries - 1):
        raise ValueError("cache_entries must be a positive power of two")
    index_bits = int(math.log2(cache_entries))
    cache = PrefixCache(cache_entries, prefix_lengths, cache_policy, context_bits > 0, stats)
    predictor_enabled = previous_prefix or previous_subline_prefix
    class_bits = max(1, math.ceil(math.log2(len(prefix_lengths))))
    previous: bytes | None = None
    previous_subline: bytes | None = None
    writer = BitWriter()
    for subline, payload in enumerate(region.payloads):
        if subline % reset_subline_interval == 0:
            cache.clear()
            previous = None
            previous_subline = None
        current_subline = payload
        context = (payload[0] & ((1 << context_bits) - 1)) if payload and context_bits else 0
        start = 0
        while start < len(payload):
            chunk = payload[start : start + word_bytes]
            # The decoder learns the current subline context from its first
            # reconstructed byte, so the first word cannot use a contextual
            # cache reference.
            match = None if context_bits and start == 0 else cache.best(chunk, context)
            previous_match = None
            predictor = previous if previous_prefix else (
                previous_subline[start : start + word_bytes] if previous_subline_prefix and previous_subline else None
            )
            if predictor is not None:
                valid_lengths = [
                    length
                    for length in prefix_lengths
                    if length < len(chunk) and length <= len(predictor) and chunk.startswith(predictor[:length])
                ]
                if valid_lengths:
                    length = max(valid_lengths)
                    previous_match = (prefix_lengths.index(length), predictor[:length])
            opcode_bits = 2 if cache_runs else 1
            literal_bits = opcode_bits + 8 * len(chunk)
            choices: list[tuple[int, str]] = [(literal_bits, "literal")]
            if match is not None:
                index, prefix = match
                cache_bits = (2 if cache_runs else (2 if predictor_enabled else 1)) + index_bits + 8 * (len(chunk) - len(prefix))
                choices.append((cache_bits, "cache"))
                if cache_runs and len(chunk) == word_bytes:
                    run = 1
                    while run < 9:
                        following = payload[start + run * word_bytes : start + (run + 1) * word_bytes]
                        if len(following) != word_bytes or not following.startswith(prefix):
                            break
                        run += 1
                    if run >= 2:
                        run_bits = 2 + index_bits + 3 + run * 8 * (word_bytes - len(prefix))
                        choices.append((run_bits, "cache-run"))
            if previous_match is not None:
                _length_class, prefix = previous_match
                choices.append(((2 if cache_runs else 2) + class_bits + 8 * (len(chunk) - len(prefix)), "previous"))
            _cost, choice = min(choices)
            if choice == "previous":
                length_class, prefix = previous_match  # type: ignore[misc]
                if cache_runs:
                    writer.write(1, 2)
                else:
                    writer.write(1, 1)
                    writer.write(0, 1)
                writer.write(length_class, class_bits)
                for value in chunk[len(prefix) :]:
                    writer.write(value, 8)
            elif choice == "cache":
                index, prefix = match  # type: ignore[misc]
                if cache_runs:
                    writer.write(2, 2)
                else:
                    writer.write(1, 1)
                if predictor_enabled and not cache_runs:
                    writer.write(1, 1)
                writer.write(index, index_bits)
                for value in chunk[len(prefix) :]:
                    writer.write(value, 8)
                cache.touch(index)
            elif choice == "cache-run":
                index, prefix = match  # type: ignore[misc]
                writer.write(3, 2)
                writer.write(index, index_bits)
                writer.write(run - 2, 3)
                cache.touch(index)
                for offset in range(run):
                    run_chunk = payload[start + offset * word_bytes : start + (offset + 1) * word_bytes]
                    for value in run_chunk[len(prefix) :]:
                        writer.write(value, 8)
                    cache.update(run_chunk, context)
                    previous = run_chunk
                start += run * word_bytes
                continue
            else:
                writer.write(0, opcode_bits)
                for value in chunk:
                    writer.write(value, 8)
            cache.update(chunk, context)
            previous = chunk
            start += len(chunk)
        previous_subline = current_subline
    # One byte accounts for the outer compression mode. Parameters are fixed by the deployed profile.
    return bytes([1]) + length_header(region) + writer.finish()


def dynamic_prefix_decode(
    stream: bytes,
    cache_entries: int,
    reset_subline_interval: int,
    word_bytes: int,
    prefix_lengths: tuple[int, ...],
    previous_prefix: bool = False,
    previous_subline_prefix: bool = False,
    cache_policy: str = "fifo",
    cache_runs: bool = False,
    context_bits: int = 0,
) -> bytes:
    if not stream or stream[0] != 1 or len(stream) < 33:
        raise ValueError("invalid dynamic-prefix stream")
    lengths = [int.from_bytes(stream[1 + 2 * i : 3 + 2 * i], "little") for i in range(16)]
    index_bits = int(math.log2(cache_entries))
    predictor_enabled = previous_prefix or previous_subline_prefix
    class_bits = max(1, math.ceil(math.log2(len(prefix_lengths))))
    reader = BitReader(stream[33:])
    cache = PrefixCache(cache_entries, prefix_lengths, cache_policy, context_bits > 0)
    previous: bytes | None = None
    previous_subline: bytes | None = None
    payloads: list[bytes] = []
    for subline, expected in enumerate(lengths):
        if subline % reset_subline_interval == 0:
            cache.clear()
            previous = None
            previous_subline = None
        payload = bytearray()
        context = 0
        while len(payload) < expected:
            chunk_length = min(word_bytes, expected - len(payload))
            opcode = reader.read(2) if cache_runs else reader.read(1)
            if cache_runs and opcode == 3:
                index = reader.read(index_bits)
                run = reader.read(3) + 2
                if index >= len(cache.entries):
                    raise ValueError("invalid prefix run index")
                prefix = cache.entries[index]
                cache.touch(index)
                if len(prefix) >= word_bytes or len(payload) + run * word_bytes > expected:
                    raise ValueError("invalid prefix run")
                for _ in range(run):
                    chunk = bytearray(prefix)
                    while len(chunk) < word_bytes:
                        chunk.append(reader.read(8))
                    payload.extend(chunk)
                    cache.update(bytes(chunk), context)
                    previous = bytes(chunk)
                continue
            is_reference = opcode != 0
            if is_reference:
                source_cache = (opcode == 2) if cache_runs else (reader.read(1) if predictor_enabled else 1)
                if source_cache:
                    index = reader.read(index_bits)
                    if index >= len(cache.entries):
                        raise ValueError("invalid prefix index")
                    prefix = cache.entries[index]
                    cache.touch(index)
                else:
                    length_class = reader.read(class_bits)
                    predictor = previous if previous_prefix else (
                        previous_subline[len(payload) : len(payload) + chunk_length]
                        if previous_subline_prefix and previous_subline else None
                    )
                    if predictor is None or length_class >= len(prefix_lengths):
                        raise ValueError("invalid previous-prefix token")
                    prefix = predictor[: prefix_lengths[length_class]]
                if len(prefix) >= chunk_length:
                    raise ValueError("invalid prefix length")
                chunk = bytearray(prefix)
                while len(chunk) < chunk_length:
                    chunk.append(reader.read(8))
            else:
                chunk = bytearray(reader.read(8) for _ in range(chunk_length))
            payload.extend(chunk)
            if len(payload) == len(chunk) and context_bits:
                context = payload[0] & ((1 << context_bits) - 1)
            cache.update(bytes(chunk), context)
            previous = bytes(chunk)
        payloads.append(bytes(payload))
        previous_subline = bytes(payload)
    out = bytearray()
    for payload in payloads:
        out.extend(len(payload).to_bytes(2, "little"))
        out.extend(payload)
    return bytes(out)


def evaluate(
    regions: list[Region], method: str, entries: int, reset: int, word: int,
    prefixes: tuple[int, ...], previous_prefix: bool = False,
    previous_subline_prefix: bool = False, cache_policy: str = "fifo",
    cache_runs: bool = False,
    context_bits: int = 0,
):
    regions = [region for region in regions if len(region.raw_blob()) <= 4096]
    rows = []
    algorithm_before = algorithm_after = quantized_before = quantized_after = 0
    transitions: dict[str, int] = {}
    crossed = 0
    cache_stats = {
        "lookups": 0, "hits": 0, "misses": 0, "insertions": 0,
        "evictions": 0, "resets": 0, "occupancy_sum": 0,
        "peak_entries": 0, "hits_by_prefix_length": Counter(),
    }
    for region in regions:
        baseline = region.raw_blob()
        encoded = dynamic_prefix_encode(
            region, entries, reset, word, prefixes, previous_prefix, previous_subline_prefix,
            cache_policy, cache_runs, context_bits, cache_stats
        )
        decoded = dynamic_prefix_decode(
            encoded, entries, reset, word, prefixes, previous_prefix, previous_subline_prefix, cache_policy, cache_runs, context_bits
        )
        if decoded != baseline:
            raise RuntimeError(f"round-trip mismatch in region {region.index}")
        chosen = min(len(baseline), len(encoded))
        before_tier = tier_kib(len(baseline))
        after_tier = tier_kib(chosen)
        transition = f"{before_tier}K->{after_tier}K"
        transitions[transition] = transitions.get(transition, 0) + 1
        if after_tier < before_tier:
            crossed += 1
        algorithm_before += len(baseline)
        algorithm_after += chosen
        quantized_before += before_tier * 1024
        quantized_after += after_tier * 1024
        rows.append({
            "region": region.index,
            "method": method,
            "before_raw": len(baseline),
            "encoded_raw": len(encoded),
            "chosen_raw": chosen,
            "before_tier_kib": before_tier,
            "after_tier_kib": after_tier,
            "crossed": int(after_tier < before_tier),
            "raw_saved": len(baseline) - chosen,
        })
    original = len(regions) * 4096
    # Hardware estimate used throughout the project: 4 bytes per prefix entry,
    # 2 CLOCK bits per entry, one 4-byte previous-word register, and a rounded
    # 4-byte control/pointer register. FIFO omits the CLOCK score array.
    entry_bytes = entries * 4
    clock_bytes = math.ceil(entries * 2 / 8) if cache_policy == "clock" else 0
    predictor_bytes = word if (previous_prefix or previous_subline_prefix) else 0
    control_bytes = 4
    runtime_state_bytes = entry_bytes + clock_bytes + predictor_bytes + control_bytes
    lookups = cache_stats["lookups"]
    cache_stats["hit_rate"] = cache_stats["hits"] / lookups if lookups else 0.0
    cache_stats["average_occupancy"] = cache_stats["occupancy_sum"] / lookups if lookups else 0.0
    cache_stats["occupancy_ratio"] = cache_stats["average_occupancy"] / entries if entries else 0.0
    cache_stats["hits_by_prefix_length"] = dict(cache_stats["hits_by_prefix_length"])
    cache_stats.pop("occupancy_sum")
    summary = {
        "method": method,
        "regions": len(regions),
        "roundtrip": True,
        "algorithm_bytes_before": algorithm_before,
        "algorithm_bytes_after": algorithm_after,
        "algorithm_ratio_before": algorithm_before / original,
        "algorithm_ratio_after": algorithm_after / original,
        "quantized_bytes_before": quantized_before,
        "quantized_bytes_after": quantized_after,
        "quantized_ratio_before": quantized_before / original,
        "quantized_ratio_after": quantized_after / original,
        "crossed_regions": crossed,
        "transitions": transitions,
        "cache_entries": entries,
        "cache_entry_bytes": entry_bytes,
        "clock_score_bytes": clock_bytes,
        "predictor_bytes": predictor_bytes,
        "control_bytes": control_bytes,
        "runtime_state_bytes": runtime_state_bytes,
        "static_codebook_bytes": 0,
        "cache_stats": cache_stats,
    }
    return summary, rows


def write_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--payloads", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--entries", default="16,32")
    parser.add_argument("--resets", default="1,4,16")
    parser.add_argument("--word-bytes", type=int, default=8)
    parser.add_argument("--prefix-lengths", default="2,4,6")
    parser.add_argument("--previous-prefix", action="store_true")
    parser.add_argument("--previous-subline-prefix", action="store_true")
    parser.add_argument("--cache-policy", choices=("fifo", "clock"), default="fifo")
    parser.add_argument("--cache-runs", action="store_true")
    parser.add_argument("--context-bits", type=int, choices=range(0, 9), default=0)
    args = parser.parse_args()
    regions = read_payloads(args.payloads)
    prefixes = tuple(int(value) for value in args.prefix_lengths.split(","))
    summaries = []
    for entries in (int(value) for value in args.entries.split(",")):
        for reset in (int(value) for value in args.resets.split(",")):
            suffix = "-prev" if args.previous_prefix else ("-prevsub" if args.previous_subline_prefix else "")
            run_suffix = "-run" if args.cache_runs else ""
            context_suffix = f"-ctx{args.context_bits}" if args.context_bits else ""
            method = f"dynamic-prefix-{args.cache_policy}-e{entries}-r{reset}{suffix}{run_suffix}{context_suffix}"
            summary, rows = evaluate(
                regions, method, entries, reset, args.word_bytes, prefixes,
                args.previous_prefix, args.previous_subline_prefix, args.cache_policy, args.cache_runs, args.context_bits
            )
            summaries.append(summary)
            write_csv(args.output_dir / f"{args.name}-{method}-regions.csv", rows)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / f"{args.name}-dynamic-prefix-summary.json").write_text(
        json.dumps(summaries, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(summaries, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
