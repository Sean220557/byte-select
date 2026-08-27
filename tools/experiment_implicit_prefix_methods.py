#!/usr/bin/env python3
"""Reversible zero-codebook implicit-prefix codecs for FPC payload regions."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Callable

from experiment_2k_to_1k_algorithms import BitReader, BitWriter, Region, read_payloads


PREFIXES = (1, 2, 3)


def bits_for(values: int) -> int:
    return max(1, math.ceil(math.log2(values)))


def header(region: Region) -> bytes:
    return b"".join(len(payload).to_bytes(2, "little") for payload in region.payloads)


def lengths_from_stream(stream: bytes) -> list[int]:
    if len(stream) < 33:
        raise ValueError("truncated stream")
    return [int.from_bytes(stream[1 + 2 * i : 3 + 2 * i], "little") for i in range(16)]


def finish(region: Region, writer: BitWriter, mode: int) -> bytes:
    return bytes([mode]) + header(region) + writer.finish()


def rebuild(payloads: list[bytes]) -> bytes:
    out = bytearray()
    for payload in payloads:
        out.extend(len(payload).to_bytes(2, "little"))
        out.extend(payload)
    return bytes(out)


def chunks(payload: bytes, word: int) -> list[bytes]:
    return [payload[pos : pos + word] for pos in range(0, len(payload), word)]


def write_bytes(writer: BitWriter, data: bytes) -> None:
    for value in data:
        writer.write(value, 8)


def read_bytes(reader: BitReader, size: int) -> bytes:
    return bytes(reader.read(8) for _ in range(size))


def best_anchor_layout(group: list[bytes], anchor_indices: list[int], prefixes: tuple[int, ...]):
    prefix_bits = bits_for(len(prefixes))
    anchor_set = set(anchor_indices)
    nonanchors = [idx for idx in range(len(group)) if idx not in anchor_set]
    source_bits = bits_for(len(anchor_indices) + 1)
    raw_cost = 1 + sum(8 * len(chunk) for chunk in group)
    best = None
    for prefix_class, prefix_length in enumerate(prefixes):
        if any(prefix_length >= len(group[idx]) for idx in anchor_indices):
            continue
        sources = []
        data_bits = 0
        for idx in nonanchors:
            candidates = [
                anchor_id
                for anchor_id, anchor_idx in enumerate(anchor_indices)
                if prefix_length < len(group[idx])
                and group[idx].startswith(group[anchor_idx][:prefix_length])
            ]
            if candidates:
                source = candidates[0] + 1
                data_bits += 8 * (len(group[idx]) - prefix_length)
            else:
                source = 0
                data_bits += 8 * len(group[idx])
            sources.append(source)
        cost = (
            1 + prefix_bits
            + sum(8 * len(group[idx]) for idx in anchor_indices)
            + source_bits * len(nonanchors) + data_bits
        )
        if cost < raw_cost and (best is None or cost < best[0]):
            best = (cost, prefix_class, sources, nonanchors, source_bits)
    return best


def anchor_encode(region: Region, word: int, block_words: int, anchors: int) -> bytes:
    writer = BitWriter()
    prefix_bits = bits_for(len(PREFIXES))
    for payload in region.payloads:
        words = chunks(payload, word)
        for begin in range(0, len(words), block_words):
            group = words[begin : begin + block_words]
            anchor_indices = sorted(set(round(i * (len(group) - 1) / max(1, anchors - 1)) for i in range(anchors)))
            layout = best_anchor_layout(group, anchor_indices, PREFIXES) if len(group) > 1 else None
            if layout is None:
                writer.write(0, 1)
                for chunk in group:
                    write_bytes(writer, chunk)
                continue
            _cost, prefix_class, sources, nonanchors, source_bits = layout
            writer.write(1, 1)
            writer.write(prefix_class, prefix_bits)
            for idx in anchor_indices:
                write_bytes(writer, group[idx])
            for source in sources:
                writer.write(source, source_bits)
            prefix_length = PREFIXES[prefix_class]
            for idx, source in zip(nonanchors, sources):
                if source:
                    write_bytes(writer, group[idx][prefix_length:])
                else:
                    write_bytes(writer, group[idx])
    return finish(region, writer, 2 if anchors == 1 else 3)


def anchor_decode(stream: bytes, word: int, block_words: int, anchors: int) -> bytes:
    expected_lengths = lengths_from_stream(stream)
    reader = BitReader(stream[33:])
    prefix_bits = bits_for(len(PREFIXES))
    payloads = []
    for expected in expected_lengths:
        sizes = [min(word, expected - pos) for pos in range(0, expected, word)]
        words: list[bytes] = []
        for begin in range(0, len(sizes), block_words):
            group_sizes = sizes[begin : begin + block_words]
            compressed = reader.read(1)
            if not compressed:
                words.extend(read_bytes(reader, size) for size in group_sizes)
                continue
            prefix_class = reader.read(prefix_bits)
            if prefix_class >= len(PREFIXES):
                raise ValueError("invalid prefix class")
            prefix_length = PREFIXES[prefix_class]
            anchor_indices = sorted(set(round(i * (len(group_sizes) - 1) / max(1, anchors - 1)) for i in range(anchors)))
            anchor_set = set(anchor_indices)
            group: list[bytes | None] = [None] * len(group_sizes)
            for idx in anchor_indices:
                group[idx] = read_bytes(reader, group_sizes[idx])
            nonanchors = [idx for idx in range(len(group_sizes)) if idx not in anchor_set]
            source_bits = bits_for(len(anchor_indices) + 1)
            sources = [reader.read(source_bits) for _ in nonanchors]
            for idx, source in zip(nonanchors, sources):
                if source:
                    anchor_idx = anchor_indices[source - 1]
                    anchor = group[anchor_idx]
                    if anchor is None or prefix_length >= group_sizes[idx]:
                        raise ValueError("invalid anchor reference")
                    group[idx] = anchor[:prefix_length] + read_bytes(reader, group_sizes[idx] - prefix_length)
                else:
                    group[idx] = read_bytes(reader, group_sizes[idx])
            words.extend(item for item in group if item is not None)
        payloads.append(b"".join(words))
    return rebuild(payloads)


def run_encode(region: Region, word: int, max_run: int) -> bytes:
    writer = BitWriter()
    prefix_bits = bits_for(len(PREFIXES))
    run_bits = bits_for(max_run - 1)
    for payload in region.payloads:
        words = chunks(payload, word)
        pos = 0
        previous = None
        while pos < len(words):
            best = None
            if previous is not None:
                for prefix_class, prefix_length in enumerate(PREFIXES):
                    run = 0
                    while (
                        run < max_run and pos + run < len(words)
                        and prefix_length < len(words[pos + run])
                        and words[pos + run].startswith(previous[:prefix_length])
                    ):
                        run += 1
                    if run >= 2:
                        cost = 1 + prefix_bits + run_bits + sum(
                            8 * (len(words[pos + idx]) - prefix_length) for idx in range(run)
                        )
                        literal_cost = sum(1 + 8 * len(words[pos + idx]) for idx in range(run))
                        if cost < literal_cost and (best is None or cost < best[0]):
                            best = (cost, prefix_class, run)
            if best is None:
                writer.write(0, 1)
                write_bytes(writer, words[pos])
                previous = words[pos]
                pos += 1
            else:
                _cost, prefix_class, run = best
                writer.write(1, 1)
                writer.write(prefix_class, prefix_bits)
                writer.write(run - 2, run_bits)
                prefix_length = PREFIXES[prefix_class]
                for idx in range(run):
                    write_bytes(writer, words[pos + idx][prefix_length:])
                previous = words[pos + run - 1]
                pos += run
    return finish(region, writer, 4)


def run_decode(stream: bytes, word: int, max_run: int) -> bytes:
    expected_lengths = lengths_from_stream(stream)
    reader = BitReader(stream[33:])
    prefix_bits = bits_for(len(PREFIXES))
    run_bits = bits_for(max_run - 1)
    payloads = []
    for expected in expected_lengths:
        payload = bytearray()
        previous = None
        while len(payload) < expected:
            is_run = reader.read(1)
            if not is_run:
                chunk = read_bytes(reader, min(word, expected - len(payload)))
                payload.extend(chunk)
                previous = chunk
                continue
            if previous is None:
                raise ValueError("run without anchor")
            prefix_class = reader.read(prefix_bits)
            run = reader.read(run_bits) + 2
            prefix_length = PREFIXES[prefix_class]
            for _ in range(run):
                chunk_length = min(word, expected - len(payload))
                if prefix_length >= chunk_length:
                    raise ValueError("invalid run")
                chunk = previous[:prefix_length] + read_bytes(reader, chunk_length - prefix_length)
                payload.extend(chunk)
            previous = chunk
        payloads.append(bytes(payload))
    return rebuild(payloads)


def xor_encode(region: Region, word: int) -> bytes:
    writer = BitWriter()
    lead_bits = bits_for(word)
    for payload in region.payloads:
        previous = None
        for chunk in chunks(payload, word):
            if previous is None or len(previous) != len(chunk):
                writer.write(0, 1)
                write_bytes(writer, chunk)
            else:
                delta = bytes(a ^ b for a, b in zip(chunk, previous))
                leading = 0
                while leading < len(delta) - 1 and delta[leading] == 0:
                    leading += 1
                ref_cost = 1 + lead_bits + 8 * (len(delta) - leading)
                lit_cost = 1 + 8 * len(chunk)
                if ref_cost < lit_cost:
                    writer.write(1, 1)
                    writer.write(leading, lead_bits)
                    write_bytes(writer, delta[leading:])
                else:
                    writer.write(0, 1)
                    write_bytes(writer, chunk)
            previous = chunk
    return finish(region, writer, 5)


def xor_decode(stream: bytes, word: int) -> bytes:
    expected_lengths = lengths_from_stream(stream)
    reader = BitReader(stream[33:])
    lead_bits = bits_for(word)
    payloads = []
    for expected in expected_lengths:
        payload = bytearray()
        previous = None
        while len(payload) < expected:
            size = min(word, expected - len(payload))
            is_delta = reader.read(1)
            if not is_delta:
                chunk = read_bytes(reader, size)
            else:
                if previous is None or len(previous) != size:
                    raise ValueError("xor without predictor")
                leading = reader.read(lead_bits)
                delta = bytes(leading) + read_bytes(reader, size - leading)
                chunk = bytes(a ^ b for a, b in zip(delta, previous))
            payload.extend(chunk)
            previous = chunk
        payloads.append(bytes(payload))
    return rebuild(payloads)


def signed_width(value: int, word: int) -> int:
    for width in range(1, word + 1):
        low = -(1 << (8 * width - 1))
        high = (1 << (8 * width - 1)) - 1
        if low <= value <= high:
            return width
    return word


def delta_encode(region: Region, word: int) -> bytes:
    writer = BitWriter()
    width_bits = bits_for(word)
    modulo = 1 << (8 * word)
    half = modulo >> 1
    for payload in region.payloads:
        previous = None
        for chunk in chunks(payload, word):
            if previous is None or len(chunk) != word or len(previous) != word:
                writer.write(0, 1)
                write_bytes(writer, chunk)
            else:
                current_value = int.from_bytes(chunk, "little")
                previous_value = int.from_bytes(previous, "little")
                delta = ((current_value - previous_value + half) % modulo) - half
                width = signed_width(delta, word)
                ref_cost = 1 + width_bits + 8 * width
                lit_cost = 1 + 8 * word
                if width < word and ref_cost < lit_cost:
                    writer.write(1, 1)
                    writer.write(width - 1, width_bits)
                    write_bytes(writer, (delta % (1 << (8 * width))).to_bytes(width, "little"))
                else:
                    writer.write(0, 1)
                    write_bytes(writer, chunk)
            previous = chunk
    return finish(region, writer, 6)


def delta_decode(stream: bytes, word: int) -> bytes:
    expected_lengths = lengths_from_stream(stream)
    reader = BitReader(stream[33:])
    width_bits = bits_for(word)
    modulo = 1 << (8 * word)
    payloads = []
    for expected in expected_lengths:
        payload = bytearray()
        previous = None
        while len(payload) < expected:
            size = min(word, expected - len(payload))
            is_delta = reader.read(1)
            if not is_delta:
                chunk = read_bytes(reader, size)
            else:
                if previous is None or size != word:
                    raise ValueError("delta without predictor")
                width = reader.read(width_bits) + 1
                raw = int.from_bytes(read_bytes(reader, width), "little")
                if raw & (1 << (8 * width - 1)):
                    raw -= 1 << (8 * width)
                value = (int.from_bytes(previous, "little") + raw) % modulo
                chunk = value.to_bytes(word, "little")
            payload.extend(chunk)
            previous = chunk
        payloads.append(bytes(payload))
    return rebuild(payloads)


def subline_bitmap_encode(region: Region, word: int) -> bytes:
    writer = BitWriter()
    columns = max((math.ceil(len(payload) / word) for payload in region.payloads), default=0)
    prefix_bits = bits_for(len(PREFIXES))
    for column in range(columns):
        start = column * word
        active = [(idx, payload[start : start + word]) for idx, payload in enumerate(region.payloads) if start < len(payload)]
        group = [chunk for _idx, chunk in active]
        layout = best_anchor_layout(group, [0], PREFIXES) if len(group) > 1 else None
        if layout is None:
            writer.write(0, 1)
            for chunk in group:
                write_bytes(writer, chunk)
            continue
        _cost, prefix_class, sources, nonanchors, source_bits = layout
        writer.write(1, 1)
        writer.write(prefix_class, prefix_bits)
        write_bytes(writer, group[0])
        for source in sources:
            writer.write(source, source_bits)
        prefix_length = PREFIXES[prefix_class]
        for idx, source in zip(nonanchors, sources):
            write_bytes(writer, group[idx][prefix_length:] if source else group[idx])
    return finish(region, writer, 7)


def subline_bitmap_decode(stream: bytes, word: int) -> bytes:
    expected_lengths = lengths_from_stream(stream)
    reader = BitReader(stream[33:])
    columns = max((math.ceil(length / word) for length in expected_lengths), default=0)
    prefix_bits = bits_for(len(PREFIXES))
    payloads = [bytearray() for _ in range(16)]
    for column in range(columns):
        start = column * word
        active = [(idx, min(word, length - start)) for idx, length in enumerate(expected_lengths) if start < length]
        compressed = reader.read(1)
        if not compressed:
            for idx, size in active:
                payloads[idx].extend(read_bytes(reader, size))
            continue
        prefix_class = reader.read(prefix_bits)
        prefix_length = PREFIXES[prefix_class]
        anchor_idx, anchor_size = active[0]
        anchor = read_bytes(reader, anchor_size)
        payloads[anchor_idx].extend(anchor)
        sources = [reader.read(1) for _ in active[1:]]
        for (idx, size), source in zip(active[1:], sources):
            if source:
                chunk = anchor[:prefix_length] + read_bytes(reader, size - prefix_length)
            else:
                chunk = read_bytes(reader, size)
            payloads[idx].extend(chunk)
    return rebuild([bytes(payload) for payload in payloads])


def tier(size: int) -> int:
    return 0 if size == 0 else math.ceil(size / 1024)


def evaluate(regions: list[Region], name: str, encode: Callable[[Region], bytes], decode: Callable[[bytes], bytes]):
    rows = []
    before_total = after_total = q_before = q_after = crossed = 0
    transitions: dict[str, int] = {}
    eligible = [region for region in regions if len(region.raw_blob()) <= 4096]
    for region in eligible:
        baseline = region.raw_blob()
        encoded = encode(region)
        if decode(encoded) != baseline:
            raise RuntimeError(f"{name} round-trip mismatch at region {region.index}")
        chosen = min(len(baseline), len(encoded))
        source, target = tier(len(baseline)), tier(chosen)
        transitions[f"{source}K->{target}K"] = transitions.get(f"{source}K->{target}K", 0) + 1
        crossed += int(target < source)
        before_total += len(baseline)
        after_total += chosen
        q_before += source * 1024
        q_after += target * 1024
        rows.append({
            "region": region.index, "method": name, "before_raw": len(baseline),
            "encoded_raw": len(encoded), "chosen_raw": chosen,
            "before_tier_kib": source, "after_tier_kib": target,
            "crossed": int(target < source), "raw_saved": len(baseline) - chosen,
        })
    original = len(eligible) * 4096
    return {
        "method": name, "regions": len(eligible), "roundtrip": True,
        "algorithm_bytes_before": before_total, "algorithm_bytes_after": after_total,
        "algorithm_ratio_before": before_total / original, "algorithm_ratio_after": after_total / original,
        "quantized_bytes_before": q_before, "quantized_bytes_after": q_after,
        "quantized_ratio_before": q_before / original, "quantized_ratio_after": q_after / original,
        "crossed_regions": crossed, "transitions": transitions,
    }, rows


def write_csv(path: Path, rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--payloads", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    regions = read_payloads(args.payloads)
    configs: list[tuple[str, Callable[[Region], bytes], Callable[[bytes], bytes]]] = []
    for block in (4, 8, 16):
        configs.append((f"block-anchor-w4-b{block}", lambda r, b=block: anchor_encode(r, 4, b, 1), lambda s, b=block: anchor_decode(s, 4, b, 1)))
    for block, anchors in ((8, 2), (16, 2), (16, 4)):
        configs.append((f"multi-anchor-w4-b{block}-a{anchors}", lambda r, b=block, a=anchors: anchor_encode(r, 4, b, a), lambda s, b=block, a=anchors: anchor_decode(s, 4, b, a)))
    for max_run in (4, 8):
        configs.append((f"prefix-run-w4-r{max_run}", lambda r, m=max_run: run_encode(r, 4, m), lambda s, m=max_run: run_decode(s, 4, m)))
    for word in (4, 8):
        configs.append((f"xor-leading-w{word}", lambda r, w=word: xor_encode(r, w), lambda s, w=word: xor_decode(s, w)))
        configs.append((f"delta-prefix-w{word}", lambda r, w=word: delta_encode(r, w), lambda s, w=word: delta_decode(s, w)))
        configs.append((f"subline-bitmap-w{word}", lambda r, w=word: subline_bitmap_encode(r, w), lambda s, w=word: subline_bitmap_decode(s, w)))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    summaries = []
    for method, encoder, decoder in configs:
        summary, rows = evaluate(regions, method, encoder, decoder)
        summaries.append(summary)
        write_csv(args.output_dir / f"{args.name}-{method}-regions.csv", rows)
    (args.output_dir / f"{args.name}-implicit-prefix-summary.json").write_text(
        json.dumps(summaries, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(summaries, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
