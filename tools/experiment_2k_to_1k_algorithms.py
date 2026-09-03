#!/usr/bin/env python3
"""Raw algorithm experiments for tier-targeted region compression.

This intentionally ignores subline padding and region allocation layout. The
unit under test is a self-contained stream that can reconstruct the 16 payloads
of a 4KB region:

    16 x u16 payload lengths + payload bytes

The evaluator reports raw 4K->3K, 3K->2K, 2K->1K, and 1K->0K crossings.
"""
from __future__ import annotations

import argparse
import csv
import json
import struct
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


PHRASE_LENGTHS = (4, 6, 8, 12, 16, 24, 32, 48, 64)
RAW_TIERS = (
    ("1K->0K", 0, 1024, 0),
    ("2K->1K", 1024, 2048, 1024),
    ("3K->2K", 2048, 3072, 2048),
    ("4K->3K", 3072, 4096, 3072),
)


@dataclass
class Region:
    index: int
    payloads: list[bytes]

    def raw_blob(self) -> bytes:
        out = bytearray()
        for payload in self.payloads:
            out.extend(len(payload).to_bytes(2, "little"))
            out.extend(payload)
        return bytes(out)


def read_payloads(path: Path, limit: int = 0) -> list[Region]:
    data = path.read_bytes()
    if len(data) < 16 or data[:8] != b"FPCPAY1\0":
        raise ValueError(f"{path} is not an FPCPAY1 payload dump")
    records = struct.unpack_from("<Q", data, 8)[0]
    want = limit or records // 16
    pos = 16
    regions: list[Region] = []
    for ridx in range(want):
        payloads = []
        for _ in range(16):
            if pos + 2 > len(data):
                return regions
            size = struct.unpack_from("<H", data, pos)[0]
            pos += 2
            if pos + size > len(data):
                return regions
            payloads.append(data[pos : pos + size])
            pos += size
        regions.append(Region(ridx, payloads))
    return regions


def train_repeated_payload_dictionary(regions: list[Region], entries: int) -> set[bytes]:
    counts: Counter[bytes] = Counter()
    for region in regions:
        for payload in region.payloads:
            counts[payload] += 1
    return {payload for payload, _count in counts.most_common(entries)}


def train_literal_dictionary(regions: list[Region], entries: int = 16) -> list[int]:
    counts: Counter[int] = Counter()
    for region in regions:
        counts.update(region.raw_blob())
    return [value for value, _count in counts.most_common(entries)]


def raw_tier_label(size: int) -> str:
    return next((label for label, low, high, _target in RAW_TIERS if low < size <= high), ">4K")


def train_tier_literal_dictionaries(regions: list[Region], entries: int = 16) -> dict[str, list[int]]:
    counts_by_tier: dict[str, Counter[int]] = {}
    global_counts: Counter[int] = Counter()
    for region in regions:
        blob = region.raw_blob()
        counts_by_tier.setdefault(raw_tier_label(len(blob)), Counter()).update(blob)
        global_counts.update(blob)
    fallback = [value for value, _count in global_counts.most_common(entries)]
    out: dict[str, list[int]] = {}
    for label, _low, _high, _target in RAW_TIERS:
        tier_counts = counts_by_tier.get(label, Counter())
        values = [value for value, _count in tier_counts.most_common(entries)]
        for value in fallback:
            if len(values) >= entries:
                break
            if value not in values:
                values.append(value)
        out[label] = values
    out[">4K"] = fallback
    return out


def huffman_code_lengths(counts: Counter[int], max_len: int = 16) -> list[int]:
    import heapq

    heap = []
    serial = 0
    for symbol in range(256):
        heapq.heappush(heap, (counts.get(symbol, 0) + 1, serial, symbol))
        serial += 1
    parents: dict[int, tuple[int, int]] = {}
    next_node = 256
    while len(heap) > 1:
        w0, _s0, n0 = heapq.heappop(heap)
        w1, _s1, n1 = heapq.heappop(heap)
        node = next_node
        next_node += 1
        parents[n0] = (node, 0)
        parents[n1] = (node, 1)
        heapq.heappush(heap, (w0 + w1, serial, node))
        serial += 1
    lengths = [1] * 256
    for symbol in range(256):
        depth = 0
        node = symbol
        while node in parents:
            depth += 1
            node = parents[node][0]
        lengths[symbol] = min(max(depth, 1), max_len)
    return lengths


def train_huffman_lengths(regions: list[Region], max_len: int = 16) -> list[int]:
    counts: Counter[int] = Counter()
    for region in regions:
        counts.update(region.raw_blob())
    return huffman_code_lengths(counts, max_len=max_len)


def train_tier_huffman_lengths(regions: list[Region], max_len: int = 16) -> dict[str, list[int]]:
    counts_by_tier: dict[str, Counter[int]] = {}
    global_counts: Counter[int] = Counter()
    for region in regions:
        blob = region.raw_blob()
        counts_by_tier.setdefault(raw_tier_label(len(blob)), Counter()).update(blob)
        global_counts.update(blob)
    fallback = huffman_code_lengths(global_counts, max_len=max_len)
    out = {label: huffman_code_lengths(counts_by_tier.get(label, global_counts), max_len=max_len) for label, *_ in RAW_TIERS}
    out[">4K"] = fallback
    return out


def train_size_bucket_huffman_lengths(
    regions: list[Region], buckets: int = 8, max_len: int = 16
) -> list[list[int]]:
    if buckets <= 1:
        return [train_huffman_lengths(regions, max_len=max_len)]
    ordered = sorted(regions, key=lambda region: len(region.raw_blob()))
    out: list[list[int]] = []
    seen: set[tuple[int, ...]] = set()
    for bucket in range(buckets):
        lo = len(ordered) * bucket // buckets
        hi = len(ordered) * (bucket + 1) // buckets
        subset = ordered[lo:hi] or ordered
        counts: Counter[int] = Counter()
        for region in subset:
            counts.update(region.raw_blob())
        lengths = huffman_code_lengths(counts, max_len=max_len)
        key = tuple(lengths)
        if key not in seen:
            seen.add(key)
            out.append(lengths)
    global_lengths = train_huffman_lengths(regions, max_len=max_len)
    if tuple(global_lengths) not in seen:
        out.insert(0, global_lengths)
    return out


def train_3k_rescue_huffman_lengths(
    regions: list[Region], buckets: int = 8, max_len: int = 16
) -> list[list[int]]:
    tier_regions = [region for region in regions if raw_tier_label(len(region.raw_blob())) == "3K->2K"]
    if not tier_regions:
        return []
    ordered = sorted(tier_regions, key=lambda region: len(region.raw_blob()))
    groups: list[list[Region]] = [ordered]
    groups.append(ordered[: max(1, len(ordered) // 2)])
    groups.append(ordered[: max(1, len(ordered) // 4)])
    for bucket in range(buckets):
        lo = len(ordered) * bucket // buckets
        hi = len(ordered) * (bucket + 1) // buckets
        if lo < hi:
            groups.append(ordered[lo:hi])

    out: list[list[int]] = []
    seen: set[tuple[int, ...]] = set()
    for subset in groups:
        counts: Counter[int] = Counter()
        for region in subset:
            counts.update(region.raw_blob())
        lengths = huffman_code_lengths(counts, max_len=max_len)
        key = tuple(lengths)
        if key not in seen:
            seen.add(key)
            out.append(lengths)
    return out


def canonical_codes(lengths: list[int]) -> list[tuple[int, int]]:
    pairs = sorted((length, symbol) for symbol, length in enumerate(lengths) if length > 0)
    code = 0
    prev_len = 0
    out = [(0, 0)] * 256
    for length, symbol in pairs:
        code <<= length - prev_len
        rev = 0
        for bit in range(length):
            rev = (rev << 1) | ((code >> bit) & 1)
        out[symbol] = (rev, length)
        code += 1
        prev_len = length
    return out


def canonical_decode_tree(lengths: list[int]) -> dict:
    root: dict = {}
    for symbol, (code, length) in enumerate(canonical_codes(lengths)):
        node = root
        for bit_idx in range(length):
            bit = (code >> bit_idx) & 1
            node = node.setdefault(bit, {})
        node["sym"] = symbol
    return root


def parse_raw_blob(blob: bytes) -> list[bytes]:
    pos = 0
    payloads = []
    for _ in range(16):
        if pos + 2 > len(blob):
            raise ValueError("truncated length")
        size = int.from_bytes(blob[pos : pos + 2], "little")
        pos += 2
        if pos + size > len(blob):
            raise ValueError("truncated payload")
        payloads.append(blob[pos : pos + size])
        pos += size
    if pos != len(blob):
        raise ValueError("trailing bytes")
    return payloads


def zlib_region(blob: bytes) -> tuple[int, bytes]:
    stream = zlib.compress(blob, 1)
    if zlib.decompress(stream) != blob:
        raise ValueError("zlib roundtrip mismatch")
    return len(stream), stream


def template_exception_region(payloads: list[bytes]) -> tuple[int, bytes]:
    best_stream = None
    best_size = 10**9
    for template_id, template in enumerate(payloads):
        if len(template) == 0 or len(template) > 255:
            continue
        stream = bytearray()
        stream.extend((template_id, len(template) & 0xFF))
        stream.extend(template)
        ok = True
        for idx, payload in enumerate(payloads):
            stream.extend(len(payload).to_bytes(2, "little"))
            if idx == template_id:
                stream.append(0)
                continue
            if len(payload) != len(template) or len(payload) > 255:
                stream.append(0xFF)
                stream.extend(payload)
                continue
            diffs = [(pos, value) for pos, value in enumerate(payload) if value != template[pos]]
            if len(diffs) >= len(payload) or len(diffs) > 254:
                stream.append(0xFF)
                stream.extend(payload)
                continue
            stream.append(len(diffs))
            for pos, value in diffs:
                if pos > 255:
                    ok = False
                    break
                stream.extend((pos, value))
            if not ok:
                break
        if ok and len(stream) < best_size:
            best_size = len(stream)
            best_stream = bytes(stream)
    if best_stream is None:
        return sum(len(p) for p in payloads) + 32, b""
    decoded = template_exception_decode(best_stream)
    if decoded != payloads:
        raise ValueError("template-exception roundtrip mismatch")
    return best_size, best_stream


def template_exception_decode(stream: bytes) -> list[bytes]:
    pos = 0
    template_id = stream[pos]
    pos += 1
    template_len = stream[pos]
    pos += 1
    template = stream[pos : pos + template_len]
    pos += template_len
    payloads = []
    for idx in range(16):
        size = int.from_bytes(stream[pos : pos + 2], "little")
        pos += 2
        mode = stream[pos]
        pos += 1
        if idx == template_id:
            payload = bytes(template)
        elif mode == 0xFF:
            payload = stream[pos : pos + size]
            pos += size
        else:
            if size != len(template):
                raise ValueError("template diff used with mismatched size")
            buf = bytearray(template)
            for _ in range(mode):
                off = stream[pos]
                value = stream[pos + 1]
                pos += 2
                buf[off] = value
            payload = bytes(buf[:size])
        payloads.append(payload)
    if pos != len(stream):
        raise ValueError("template-exception trailing bytes")
    return payloads


def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value ^ sign) - sign


def bdi_region(blob: bytes) -> tuple[int, bytes]:
    variants = ((8, 1), (8, 2), (8, 4), (4, 1), (4, 2))
    best = (len(blob), b"")
    for word_bytes, delta_bytes in variants:
        if len(blob) % word_bytes:
            continue
        words = [int.from_bytes(blob[i : i + word_bytes], "little", signed=False) for i in range(0, len(blob), word_bytes)]
        if not words:
            continue
        base = words[0]
        bits = delta_bytes * 8
        lo = -(1 << (bits - 1))
        hi = (1 << (bits - 1)) - 1
        deltas = []
        ok = True
        for word in words:
            delta = word - base
            if not lo <= delta <= hi:
                ok = False
                break
            deltas.append(delta)
        if not ok:
            continue
        stream = bytearray()
        stream.extend((word_bytes, delta_bytes))
        stream.extend(len(blob).to_bytes(2, "little"))
        stream.extend(base.to_bytes(word_bytes, "little", signed=False))
        for delta in deltas:
            stream.extend(delta.to_bytes(delta_bytes, "little", signed=True))
        decoded = bdi_decode(bytes(stream))
        if decoded != blob:
            raise ValueError("BDI roundtrip mismatch")
        if len(stream) < best[0]:
            best = (len(stream), bytes(stream))
    return best


def bdi_decode(stream: bytes) -> bytes:
    word_bytes = stream[0]
    delta_bytes = stream[1]
    size = int.from_bytes(stream[2:4], "little")
    pos = 4
    base = int.from_bytes(stream[pos : pos + word_bytes], "little", signed=False)
    pos += word_bytes
    out = bytearray()
    while len(out) < size:
        delta = int.from_bytes(stream[pos : pos + delta_bytes], "little", signed=True)
        pos += delta_bytes
        value = (base + delta) & ((1 << (word_bytes * 8)) - 1)
        out.extend(value.to_bytes(word_bytes, "little", signed=False))
    return bytes(out[:size])


def byte_rle_encode(blob: bytes) -> bytes:
    out = bytearray()
    pos = 0
    while pos < len(blob):
        run = 1
        while pos + run < len(blob) and blob[pos + run] == blob[pos] and run < 255:
            run += 1
        if run >= 4 or blob[pos] == 0xFF:
            out.extend((0xFF, run, blob[pos]))
        else:
            out.extend(blob[pos : pos + run])
        pos += run
    return bytes(out)


def byte_rle_decode(stream: bytes) -> bytes:
    out = bytearray()
    pos = 0
    while pos < len(stream):
        value = stream[pos]
        pos += 1
        if value != 0xFF:
            out.append(value)
            continue
        if pos + 2 > len(stream):
            raise ValueError("truncated RLE escape")
        run = stream[pos]
        byte = stream[pos + 1]
        pos += 2
        out.extend([byte] * run)
    return bytes(out)


def lzss_encode(
    blob: bytes,
    window: int = 2047,
    min_match: int = 4,
    max_match: int = 35,
    token_bytes: int = 2,
    max_candidates: int = 32,
    lazy: bool = False,
) -> bytes:
    tokens: list[tuple[int, int, int]] = []
    index: dict[bytes, list[int]] = {}

    def remember(at: int) -> None:
        key = blob[at : at + min_match]
        if len(key) == min_match:
            bucket = index.setdefault(key, [])
            bucket.append(at)
            if len(bucket) > max_candidates * 4:
                del bucket[: len(bucket) - max_candidates * 4]

    def find_match(at: int) -> tuple[int, int]:
        best_len = 0
        best_dist = 0
        key = blob[at : at + min_match]
        candidates = index.get(key, [])[-max_candidates:] if len(key) == min_match else []
        for prev in reversed(candidates):
            if at - prev > window:
                continue
            length = 0
            limit = min(max_match, len(blob) - at)
            while length < limit and blob[prev + length] == blob[at + length]:
                length += 1
                if prev + length >= at:
                    break
            if length > best_len and length >= min_match:
                best_len = length
                best_dist = at - prev
        return best_len, best_dist

    pos = 0
    while pos < len(blob):
        best_len, best_dist = find_match(pos)
        if lazy and best_len >= min_match and pos + 1 < len(blob):
            remember(pos)
            next_len, _next_dist = find_match(pos + 1)
            if next_len > best_len + 1:
                tokens.append((0, blob[pos], 0))
                pos += 1
                continue
            # The current match is still better; keep pos in the index because
            # the emitted match bytes have now become visible to future output.
        if best_len >= min_match:
            tokens.append((1, best_dist, best_len))
            for add in range(best_len):
                if not lazy or add:
                    remember(pos + add)
            pos += best_len
        else:
            tokens.append((0, blob[pos], 0))
            remember(pos)
            pos += 1

    out = bytearray()
    for group_start in range(0, len(tokens), 8):
        group = tokens[group_start : group_start + 8]
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for bit, tok in enumerate(group):
            if tok[0]:
                flags |= 1 << bit
                dist = tok[1]
                length = tok[2]
                if not 1 <= dist <= window or not min_match <= length <= max_match:
                    raise ValueError("LZSS token out of range")
                if token_bytes == 2:
                    value = ((length - min_match) << 11) | dist
                    out.extend(value.to_bytes(2, "little"))
                else:
                    value = ((length - min_match) << 12) | dist
                    out.extend(value.to_bytes(3, "little"))
            else:
                out.append(tok[1])
        out[flag_pos] = flags
    return bytes(out)


def lzss_decode(stream: bytes, expected: int, min_match: int = 4, token_bytes: int = 2) -> bytes:
    out = bytearray()
    pos = 0
    while pos < len(stream):
        flags = stream[pos]
        pos += 1
        for bit in range(8):
            if pos >= len(stream):
                break
            if flags & (1 << bit):
                if pos + token_bytes > len(stream):
                    raise ValueError("truncated LZSS match")
                value = int.from_bytes(stream[pos : pos + token_bytes], "little")
                pos += token_bytes
                if token_bytes == 2:
                    dist = value & 0x7FF
                    length = (value >> 11) + min_match
                else:
                    dist = value & 0xFFF
                    length = (value >> 12) + min_match
                if dist == 0 or dist > len(out):
                    raise ValueError("invalid LZSS distance")
                for _ in range(length):
                    out.append(out[-dist])
            else:
                out.append(stream[pos])
                pos += 1
            if len(out) == expected:
                if pos != len(stream):
                    return bytes(out)
                break
    if len(out) != expected:
        raise ValueError("LZSS output length mismatch")
    return bytes(out)


def lzss_short_encode(
    blob: bytes,
    min_match: int = 3,
    max_match: int = 35,
    max_candidates: int = 64,
) -> bytes:
    tokens: list[tuple[int, int, int]] = []
    index: dict[bytes, list[int]] = {}

    def remember(at: int) -> None:
        key = blob[at : at + 3]
        if len(key) == 3:
            bucket = index.setdefault(key, [])
            bucket.append(at)
            if len(bucket) > max_candidates * 4:
                del bucket[: len(bucket) - max_candidates * 4]

    def find_match(at: int) -> tuple[int, int]:
        best_len = 0
        best_dist = 0
        key = blob[at : at + 3]
        for prev in reversed(index.get(key, [])[-max_candidates:] if len(key) == 3 else []):
            dist = at - prev
            if dist > 2047:
                continue
            limit = min(max_match, len(blob) - at)
            length = 0
            while length < limit and blob[prev + length] == blob[at + length]:
                length += 1
                if prev + length >= at:
                    break
            if length >= min_match and length > best_len:
                best_len = length
                best_dist = dist
        return best_len, best_dist

    pos = 0
    while pos < len(blob):
        length, dist = find_match(pos)
        if length >= min_match and (dist <= 32 or length >= 4):
            tokens.append((1, dist, length))
            for add in range(length):
                remember(pos + add)
            pos += length
        else:
            tokens.append((0, blob[pos], 0))
            remember(pos)
            pos += 1

    out = bytearray()
    for group_start in range(0, len(tokens), 4):
        group = tokens[group_start : group_start + 4]
        ctrl_pos = len(out)
        out.append(0)
        ctrl = 0
        for slot, tok in enumerate(group):
            if tok[0] == 0:
                out.append(tok[1])
                code = 0
            else:
                dist, length = tok[1], tok[2]
                if dist <= 32 and length <= 10:
                    # 5-bit distance-minus-1 + 3-bit length-minus-3.
                    out.append(((length - 3) << 5) | (dist - 1))
                    code = 1
                else:
                    value = ((length - 4) << 11) | dist
                    out.extend(value.to_bytes(2, "little"))
                    code = 2
            ctrl |= code << (slot * 2)
        out[ctrl_pos] = ctrl
    return bytes(out)


def lzss_short_decode(stream: bytes, expected: int) -> bytes:
    out = bytearray()
    pos = 0
    while pos < len(stream):
        ctrl = stream[pos]
        pos += 1
        for slot in range(4):
            if pos >= len(stream):
                break
            code = (ctrl >> (slot * 2)) & 0x3
            if code == 0:
                out.append(stream[pos])
                pos += 1
            elif code == 1:
                value = stream[pos]
                pos += 1
                dist = (value & 0x1F) + 1
                length = (value >> 5) + 3
                if dist > len(out):
                    raise ValueError("invalid short distance")
                for _ in range(length):
                    out.append(out[-dist])
            elif code == 2:
                if pos + 2 > len(stream):
                    raise ValueError("truncated normal token")
                value = int.from_bytes(stream[pos : pos + 2], "little")
                pos += 2
                dist = value & 0x7FF
                length = (value >> 11) + 4
                if dist == 0 or dist > len(out):
                    raise ValueError("invalid normal distance")
                for _ in range(length):
                    out.append(out[-dist])
            else:
                raise ValueError("reserved short LZSS token")
            if len(out) == expected:
                return bytes(out)
    if len(out) != expected:
        raise ValueError("short LZSS output length mismatch")
    return bytes(out)


def lzss_region(blob: bytes, token_bytes: int, max_candidates: int, lazy: bool = False) -> tuple[int, bytes]:
    stream = lzss_encode(
        blob,
        window=2047 if token_bytes == 2 else 2048,
        max_match=35 if token_bytes == 2 else 66,
        token_bytes=token_bytes,
        max_candidates=max_candidates,
        lazy=lazy,
    )
    if lzss_decode(stream, len(blob), token_bytes=token_bytes) != blob:
        raise ValueError("LZSS roundtrip mismatch")
    return len(stream), stream


def lzss_optimal_tokens(
    blob: bytes,
    window: int = 2047,
    min_match: int = 4,
    max_match: int = 35,
    max_candidates: int = 64,
) -> list[tuple[int, int, int]]:
    index: dict[bytes, list[int]] = {}
    matches: list[list[tuple[int, int]]] = [[] for _ in range(len(blob))]
    for pos in range(len(blob)):
        key = blob[pos : pos + min_match]
        if len(key) == min_match:
            for prev in reversed(index.get(key, [])[-max_candidates:]):
                if pos - prev > window:
                    continue
                length = 0
                limit = min(max_match, len(blob) - pos)
                while length < limit and blob[prev + length] == blob[pos + length]:
                    length += 1
                    if prev + length >= pos:
                        break
                if length >= min_match:
                    matches[pos].append((length, pos - prev))
            bucket = index.setdefault(key, [])
            bucket.append(pos)
            if len(bucket) > max_candidates * 4:
                del bucket[: len(bucket) - max_candidates * 4]

    n = len(blob)
    # Cost units are eighth-bytes: literal token is 1 data byte + 1/8 flag,
    # match token is 2 data bytes + 1/8 flag. This approximates grouped flags
    # while keeping the DP simple.
    dp = [0] * (n + 1)
    choice: list[tuple[int, int] | None] = [None] * n
    for pos in range(n - 1, -1, -1):
        best = 9 + dp[pos + 1]
        choice[pos] = (0, blob[pos])
        for length, dist in matches[pos]:
            cost = 17 + dp[pos + length]
            if cost < best:
                best = cost
                choice[pos] = (length, dist)
        dp[pos] = best

    tokens: list[tuple[int, int, int]] = []
    pos = 0
    while pos < n:
        item = choice[pos]
        if item is None or item[0] == 0:
            tokens.append((0, blob[pos], 0))
            pos += 1
        else:
            length, dist = item
            tokens.append((1, dist, length))
            pos += length
    return tokens


def lzss_cost_optimal_tokens(
    blob: bytes,
    literal_cost,
    match_cost: int,
    window: int = 2047,
    min_match: int = 4,
    max_match: int = 35,
    max_candidates: int = 64,
) -> list[tuple[int, int, int]]:
    index: dict[bytes, list[int]] = {}
    matches: list[list[tuple[int, int]]] = [[] for _ in range(len(blob))]
    for pos in range(len(blob)):
        key = blob[pos : pos + min_match]
        if len(key) == min_match:
            for prev in reversed(index.get(key, [])[-max_candidates:]):
                if pos - prev > window:
                    continue
                length = 0
                limit = min(max_match, len(blob) - pos)
                while length < limit and blob[prev + length] == blob[pos + length]:
                    length += 1
                    if prev + length >= pos:
                        break
                if length >= min_match:
                    matches[pos].append((length, pos - prev))
            bucket = index.setdefault(key, [])
            bucket.append(pos)
            if len(bucket) > max_candidates * 4:
                del bucket[: len(bucket) - max_candidates * 4]

    n = len(blob)
    dp = [0] * (n + 1)
    choice: list[tuple[int, int] | None] = [None] * n
    for pos in range(n - 1, -1, -1):
        best = literal_cost(blob[pos]) + dp[pos + 1]
        choice[pos] = (0, blob[pos])
        for length, dist in matches[pos]:
            token_cost = match_cost(length, dist) if callable(match_cost) else match_cost
            cost = token_cost + dp[pos + length]
            if cost < best:
                best = cost
                choice[pos] = (length, dist)
        dp[pos] = best

    tokens: list[tuple[int, int, int]] = []
    pos = 0
    while pos < n:
        item = choice[pos]
        if item is None or item[0] == 0:
            tokens.append((0, blob[pos], 0))
            pos += 1
        else:
            length, dist = item
            tokens.append((1, dist, length))
            pos += length
    return tokens


def lzss_optimal_encode(
    blob: bytes,
    window: int = 2047,
    min_match: int = 4,
    max_match: int = 35,
    max_candidates: int = 64,
) -> bytes:
    tokens = lzss_optimal_tokens(blob, window, min_match, max_match, max_candidates)
    return emit_lzss_tokens(tokens, window=window, min_match=min_match, max_match=max_match, token_bytes=2)


def emit_lzss_tokens(
    tokens: list[tuple[int, int, int]],
    window: int,
    min_match: int,
    max_match: int,
    token_bytes: int,
) -> bytes:
    out = bytearray()
    for group_start in range(0, len(tokens), 8):
        group = tokens[group_start : group_start + 8]
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for bit, tok in enumerate(group):
            if tok[0]:
                flags |= 1 << bit
                dist = tok[1]
                length = tok[2]
                if not 1 <= dist <= window or not min_match <= length <= max_match:
                    raise ValueError("LZSS token out of range")
                if token_bytes == 2:
                    value = ((length - min_match) << 11) | dist
                    out.extend(value.to_bytes(2, "little"))
                else:
                    value = ((length - min_match) << 12) | dist
                    out.extend(value.to_bytes(3, "little"))
            else:
                out.append(tok[1])
        out[flag_pos] = flags
    return bytes(out)


class BitWriter:
    def __init__(self) -> None:
        self.out = bytearray()
        self.acc = 0
        self.bits = 0
        self.total_bits = 0

    def write(self, value: int, width: int) -> None:
        self.acc |= value << self.bits
        self.bits += width
        self.total_bits += width
        while self.bits >= 8:
            self.out.append(self.acc & 0xFF)
            self.acc >>= 8
            self.bits -= 8

    def finish(self) -> bytes:
        if self.bits:
            self.out.append(self.acc & 0xFF)
            self.acc = 0
            self.bits = 0
        return bytes(self.out)


class BitReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0
        self.acc = 0
        self.bits = 0

    def read(self, width: int) -> int:
        while self.bits < width:
            if self.pos >= len(self.data):
                raise ValueError("truncated bitstream")
            self.acc |= self.data[self.pos] << self.bits
            self.pos += 1
            self.bits += 8
        value = self.acc & ((1 << width) - 1)
        self.acc >>= width
        self.bits -= width
        return value


def hf_literal_lzss_encode(
    blob: bytes, literal_dict: list[int], max_candidates: int, cost_optimal: bool = False
) -> bytes:
    lut = {value: idx for idx, value in enumerate(literal_dict[:16])}
    if cost_optimal:
        tokens = lzss_cost_optimal_tokens(
            blob,
            literal_cost=lambda value: 6 if value in lut else 10,
            match_cost=17,
            max_candidates=max_candidates,
        )
    else:
        tokens = lzss_optimal_tokens(blob, max_candidates=max_candidates)
    writer = BitWriter()
    for kind, a, b in tokens:
        if kind == 0:
            writer.write(0, 1)
            idx = lut.get(a)
            if idx is None:
                writer.write(1, 1)
                writer.write(a, 8)
            else:
                writer.write(0, 1)
                writer.write(idx, 4)
        else:
            dist = a
            length = b
            writer.write(1, 1)
            writer.write(((length - 4) << 11) | dist, 16)
    return writer.finish()


def hf_literal_lzss_decode(stream: bytes, expected: int, literal_dict: list[int]) -> bytes:
    reader = BitReader(stream)
    out = bytearray()
    while len(out) < expected:
        kind = reader.read(1)
        if kind == 0:
            escape = reader.read(1)
            if escape:
                out.append(reader.read(8))
            else:
                idx = reader.read(4)
                if idx >= len(literal_dict):
                    raise ValueError("literal dict index out of range")
                out.append(literal_dict[idx])
        else:
            value = reader.read(16)
            dist = value & 0x7FF
            length = (value >> 11) + 4
            if dist == 0 or dist > len(out):
                raise ValueError("invalid hf-lzss distance")
            for _ in range(length):
                out.append(out[-dist])
                if len(out) == expected:
                    break
    return bytes(out)


def huff_lzss_encode(blob: bytes, lengths: list[int], max_candidates: int) -> bytes:
    codes = canonical_codes(lengths)
    tokens = lzss_cost_optimal_tokens(
        blob,
        literal_cost=lambda value: 1 + lengths[value],
        match_cost=17,
        max_candidates=max_candidates,
    )
    writer = BitWriter()
    for kind, a, b in tokens:
        if kind == 0:
            writer.write(0, 1)
            code, width = codes[a]
            writer.write(code, width)
        else:
            writer.write(1, 1)
            writer.write(((b - 4) << 11) | a, 16)
    return writer.finish()


def huff_lzss_decode(stream: bytes, expected: int, lengths: list[int]) -> bytes:
    reader = BitReader(stream)
    tree = canonical_decode_tree(lengths)
    out = bytearray()
    while len(out) < expected:
        kind = reader.read(1)
        if kind == 0:
            node = tree
            while "sym" not in node:
                bit = reader.read(1)
                if bit not in node:
                    raise ValueError("invalid Huffman literal code")
                node = node[bit]
            out.append(node["sym"])
        else:
            value = reader.read(16)
            dist = value & 0x7FF
            length = (value >> 11) + 4
            if dist == 0 or dist > len(out):
                raise ValueError("invalid Huffman-LZSS distance")
            for _ in range(length):
                out.append(out[-dist])
                if len(out) == expected:
                    break
    return bytes(out)


def huff_shortmatch_lzss_encode(blob: bytes, lengths: list[int], max_candidates: int) -> bytes:
    codes = canonical_codes(lengths)

    def match_bits(length: int, dist: int) -> int:
        if dist <= 32 and length <= 10:
            return 10  # kind + short-kind + 5-bit distance + 3-bit length.
        return 18  # kind + long-kind + 16-bit normal LZSS token.

    tokens = lzss_cost_optimal_tokens(
        blob,
        literal_cost=lambda value: 1 + lengths[value],
        match_cost=match_bits,
        max_candidates=max_candidates,
    )
    writer = BitWriter()
    for kind, a, b in tokens:
        if kind == 0:
            writer.write(0, 1)
            code, width = codes[a]
            writer.write(code, width)
            continue
        writer.write(1, 1)
        if a <= 32 and b <= 10:
            writer.write(0, 1)
            writer.write(a - 1, 5)
            writer.write(b - 3, 3)
        else:
            writer.write(1, 1)
            writer.write(((b - 4) << 11) | a, 16)
    return writer.finish()


def huff_shortmatch_lzss_decode(stream: bytes, expected: int, lengths: list[int]) -> bytes:
    reader = BitReader(stream)
    tree = canonical_decode_tree(lengths)
    out = bytearray()
    while len(out) < expected:
        kind = reader.read(1)
        if kind == 0:
            node = tree
            while "sym" not in node:
                bit = reader.read(1)
                if bit not in node:
                    raise ValueError("invalid shortmatch Huffman literal code")
                node = node[bit]
            out.append(node["sym"])
            continue
        short = reader.read(1) == 0
        if short:
            dist = reader.read(5) + 1
            length = reader.read(3) + 3
        else:
            value = reader.read(16)
            dist = value & 0x7FF
            length = (value >> 11) + 4
        if dist == 0 or dist > len(out):
            raise ValueError("invalid shortmatch distance")
        for _ in range(length):
            out.append(out[-dist])
            if len(out) == expected:
                break
    return bytes(out)


def huff_longmatch_lzss_encode(blob: bytes, lengths: list[int], max_candidates: int) -> bytes:
    codes = canonical_codes(lengths)

    def match_bits(length: int, dist: int) -> int:
        if dist <= 32 and length <= 10:
            return 11  # kind + 2-bit class + 5-bit distance + 3-bit length.
        if length <= 35:
            return 19  # kind + 2-bit class + 16-bit normal token.
        return 21  # kind + 2-bit class + 11-bit distance + 7-bit length.

    tokens = lzss_cost_optimal_tokens(
        blob,
        literal_cost=lambda value: 1 + lengths[value],
        match_cost=match_bits,
        max_match=131,
        max_candidates=max_candidates,
    )
    writer = BitWriter()
    for kind, a, b in tokens:
        if kind == 0:
            writer.write(0, 1)
            code, width = codes[a]
            writer.write(code, width)
            continue
        writer.write(1, 1)
        if a <= 32 and b <= 10:
            writer.write(0, 2)
            writer.write(a - 1, 5)
            writer.write(b - 3, 3)
        elif b <= 35:
            writer.write(1, 2)
            writer.write(((b - 4) << 11) | a, 16)
        else:
            writer.write(2, 2)
            writer.write(a, 11)
            writer.write(b - 4, 7)
    return writer.finish()


def huff_longmatch_lzss_decode(stream: bytes, expected: int, lengths: list[int]) -> bytes:
    reader = BitReader(stream)
    tree = canonical_decode_tree(lengths)
    out = bytearray()
    while len(out) < expected:
        kind = reader.read(1)
        if kind == 0:
            node = tree
            while "sym" not in node:
                bit = reader.read(1)
                if bit not in node:
                    raise ValueError("invalid longmatch Huffman literal code")
                node = node[bit]
            out.append(node["sym"])
            continue
        cls = reader.read(2)
        if cls == 0:
            dist = reader.read(5) + 1
            length = reader.read(3) + 3
        elif cls == 1:
            value = reader.read(16)
            dist = value & 0x7FF
            length = (value >> 11) + 4
        elif cls == 2:
            dist = reader.read(11)
            length = reader.read(7) + 4
        else:
            raise ValueError("reserved longmatch class")
        if dist == 0 or dist > len(out):
            raise ValueError("invalid longmatch distance")
        for _ in range(length):
            out.append(out[-dist])
            if len(out) == expected:
                break
    return bytes(out)


LEN_CLASSES = (
    (4, 4, 0),
    (5, 6, 1),
    (7, 10, 2),
    (11, 18, 3),
    (19, 35, 5),
)
DIST_CLASSES = (
    (1, 4, 2),
    (5, 8, 2),
    (9, 16, 3),
    (17, 32, 4),
    (33, 64, 5),
    (65, 128, 6),
    (129, 512, 9),
    (513, 2047, 11),
)


def class_for_value(value: int, classes: tuple[tuple[int, int, int], ...]) -> tuple[int, int, int, int]:
    for idx, (lo, hi, bits) in enumerate(classes):
        if lo <= value <= hi:
            return idx, lo, hi, bits
    raise ValueError(f"value {value} out of class range")


def class_match_bits(length: int, dist: int) -> int:
    lidx, llo, _lhi, lbits = class_for_value(length, LEN_CLASSES)
    didx, dlo, _dhi, dbits = class_for_value(dist, DIST_CLASSES)
    # kind + 3-bit len class + len extra + 3-bit dist class + dist extra.
    return 1 + 3 + lbits + 3 + dbits


def huff_classmatch_lzss_encode(blob: bytes, lengths: list[int], max_candidates: int) -> bytes:
    codes = canonical_codes(lengths)
    tokens = lzss_cost_optimal_tokens(
        blob,
        literal_cost=lambda value: 1 + lengths[value],
        match_cost=class_match_bits,
        max_candidates=max_candidates,
    )
    writer = BitWriter()
    for kind, a, b in tokens:
        if kind == 0:
            writer.write(0, 1)
            code, width = codes[a]
            writer.write(code, width)
            continue
        writer.write(1, 1)
        lidx, llo, _lhi, lbits = class_for_value(b, LEN_CLASSES)
        didx, dlo, _dhi, dbits = class_for_value(a, DIST_CLASSES)
        writer.write(lidx, 3)
        if lbits:
            writer.write(b - llo, lbits)
        writer.write(didx, 3)
        writer.write(a - dlo, dbits)
    return writer.finish()


def huff_classmatch_lzss_decode(stream: bytes, expected: int, lengths: list[int]) -> bytes:
    reader = BitReader(stream)
    tree = canonical_decode_tree(lengths)
    out = bytearray()
    while len(out) < expected:
        kind = reader.read(1)
        if kind == 0:
            node = tree
            while "sym" not in node:
                bit = reader.read(1)
                if bit not in node:
                    raise ValueError("invalid classmatch Huffman literal code")
                node = node[bit]
            out.append(node["sym"])
            continue
        lidx = reader.read(3)
        if lidx >= len(LEN_CLASSES):
            raise ValueError("invalid length class")
        llo, _lhi, lbits = LEN_CLASSES[lidx]
        length = llo + (reader.read(lbits) if lbits else 0)
        didx = reader.read(3)
        if didx >= len(DIST_CLASSES):
            raise ValueError("invalid distance class")
        dlo, _dhi, dbits = DIST_CLASSES[didx]
        dist = dlo + reader.read(dbits)
        if dist == 0 or dist > len(out):
            raise ValueError("invalid classmatch distance")
        for _ in range(length):
            out.append(out[-dist])
            if len(out) == expected:
                break
    return bytes(out)


def phrase_encode(blob: bytes, phrases: list[bytes]) -> bytes:
    by_first: dict[int, list[tuple[int, bytes]]] = {}
    for idx, phrase in enumerate(phrases):
        by_first.setdefault(phrase[0], []).append((idx, phrase))
    for items in by_first.values():
        items.sort(key=lambda x: len(x[1]), reverse=True)
    out = bytearray()
    pos = 0
    while pos < len(blob):
        match = None
        for idx, phrase in by_first.get(blob[pos], []):
            if blob.startswith(phrase, pos):
                match = (idx, phrase)
                break
        if match is None:
            if blob[pos] == 0xFF:
                out.extend((0xFF, 0))
            else:
                out.append(blob[pos])
            pos += 1
        else:
            idx, phrase = match
            out.extend((0xFF, idx + 1))
            pos += len(phrase)
    return bytes(out)


def phrase_decode(stream: bytes, phrases: list[bytes]) -> bytes:
    out = bytearray()
    pos = 0
    while pos < len(stream):
        value = stream[pos]
        pos += 1
        if value != 0xFF:
            out.append(value)
            continue
        if pos >= len(stream):
            raise ValueError("truncated phrase escape")
        code = stream[pos]
        pos += 1
        if code == 0:
            out.append(0xFF)
        else:
            idx = code - 1
            if idx >= len(phrases):
                raise ValueError("phrase index out of range")
            out.extend(phrases[idx])
    return bytes(out)


def phrase_table_bytes(phrases: list[bytes]) -> int:
    return 1 + sum(1 + len(p) for p in phrases)


def candidate_phrases(blob: bytes, limit: int) -> list[bytes]:
    counts: Counter[bytes] = Counter()
    for length in PHRASE_LENGTHS:
        if length > len(blob):
            continue
        for pos in range(0, len(blob) - length + 1):
            phrase = blob[pos : pos + length]
            counts[phrase] += length - 2
    return [p for p, _ in counts.most_common(limit)]


def local_phrase_region(blob: bytes, max_phrases: int, candidates: int) -> tuple[int, bytes, list[bytes]]:
    pool = candidate_phrases(blob, candidates)
    chosen: list[bytes] = []

    def encoded_size(phrases: list[bytes]) -> int:
        return phrase_table_bytes(phrases) + len(phrase_encode(blob, phrases))

    current = encoded_size(chosen)
    while pool and len(chosen) < max_phrases:
        best = None
        for phrase in pool:
            if phrase in chosen:
                continue
            value = encoded_size(chosen + [phrase])
            gain = current - value
            key = (gain, len(phrase))
            if best is None or key > best[0]:
                best = (key, phrase, value)
        if best is None or best[0][0] <= 0:
            break
        chosen.append(best[1])
        current = best[2]
    stream = phrase_encode(blob, chosen)
    if phrase_decode(stream, chosen) != blob:
        raise ValueError("phrase roundtrip mismatch")
    return phrase_table_bytes(chosen) + len(stream), stream, chosen


def evaluate(
    regions: list[Region],
    args: argparse.Namespace,
    repeated_payload_dict: set[bytes] | None = None,
    literal_dict: list[int] | None = None,
    tier_literal_dicts: dict[str, list[int]] | None = None,
    huffman_lengths: list[int] | None = None,
    tier_huffman_lengths: dict[str, list[int]] | None = None,
    multi_huffman_lengths: list[list[int]] | None = None,
    rescue_3k_huffman_lengths: list[list[int]] | None = None,
) -> tuple[list[dict], list[dict]]:
    methods = (
        "metadata-repeat-dict",
        "template-exception",
        "bdi-region",
        "local-phrase",
        "byte-rle",
        "bounded-lzss16",
        "lazy-lzss16",
        "optimal-lzss16",
        "hf-literal-lzss16",
        "hf-literal-costdp",
        "tier-hf-literal-costdp",
        "huff-literal-lzss",
        "tier-huff-literal-lzss",
        "multi-huff-literal-lzss",
        "huff-shortmatch-lzss",
        "multi-huff-shortmatch-lzss",
        "rescue3k-huff-lzss",
        "rescue3k-shortmatch-lzss",
        "rescue3k-longmatch-lzss",
        "multi-huff-longmatch-lzss",
        "multi-huff-classmatch-lzss",
        "rescue3k-classmatch-lzss",
        "short-lzss",
        "phrase-lzss16",
        "rle-lzss16",
        "bounded-lzss24",
        "zlib-region",
        "tiered-zlib-dict",
        "tiered-hw-template-bdi-lz",
    )
    summary = {
        method: {
            "method": method,
            "regions": 0,
            "changed": 0,
            "raw_saved": 0,
            "best_after": 10**9,
            **{f"eligible_{label}": 0 for label, _lo, _hi, _target in RAW_TIERS},
            **{f"success_{label}": 0 for label, _lo, _hi, _target in RAW_TIERS},
        }
        for method in methods
    }
    rows = []
    for region in regions:
        blob = region.raw_blob()
        parse_raw_blob(blob)
        before = len(blob)
        results = {}
        phrase_size, _phrase_stream, phrases = local_phrase_region(blob, args.phrases, args.candidates)
        template_size, _template_stream = template_exception_region(region.payloads)
        bdi_size, _bdi_stream = bdi_region(blob)
        rle_stream = byte_rle_encode(blob)
        if byte_rle_decode(rle_stream) != blob:
            raise ValueError("RLE roundtrip mismatch")
        lzss16_size, _lzss16_stream = lzss_region(blob, token_bytes=2, max_candidates=args.lz_candidates)
        lazy_lzss16_size, _lazy_lzss16_stream = lzss_region(
            blob, token_bytes=2, max_candidates=args.lz_candidates, lazy=True
        )
        optimal_lzss16_stream = lzss_optimal_encode(blob, max_candidates=args.lz_candidates)
        if lzss_decode(optimal_lzss16_stream, len(blob), token_bytes=2) != blob:
            raise ValueError("optimal LZSS roundtrip mismatch")
        hf_lzss_stream = hf_literal_lzss_encode(blob, literal_dict or [], args.lz_candidates)
        if hf_literal_lzss_decode(hf_lzss_stream, len(blob), literal_dict or []) != blob:
            raise ValueError("HF literal LZSS roundtrip mismatch")
        hf_cost_stream = hf_literal_lzss_encode(blob, literal_dict or [], args.lz_candidates, cost_optimal=True)
        if hf_literal_lzss_decode(hf_cost_stream, len(blob), literal_dict or []) != blob:
            raise ValueError("HF cost-DP LZSS roundtrip mismatch")
        tier_dict = (tier_literal_dicts or {}).get(raw_tier_label(before), literal_dict or [])
        tier_hf_cost_stream = hf_literal_lzss_encode(blob, tier_dict, args.lz_candidates, cost_optimal=True)
        if hf_literal_lzss_decode(tier_hf_cost_stream, len(blob), tier_dict) != blob:
            raise ValueError("tier HF cost-DP LZSS roundtrip mismatch")
        huff_lengths = huffman_lengths or [8] * 256
        huff_lzss_stream = huff_lzss_encode(blob, huff_lengths, args.lz_candidates)
        if huff_lzss_decode(huff_lzss_stream, len(blob), huff_lengths) != blob:
            raise ValueError("Huffman LZSS roundtrip mismatch")
        tier_huff_lengths = (tier_huffman_lengths or {}).get(raw_tier_label(before), huff_lengths)
        tier_huff_lzss_stream = huff_lzss_encode(blob, tier_huff_lengths, args.lz_candidates)
        if huff_lzss_decode(tier_huff_lzss_stream, len(blob), tier_huff_lengths) != blob:
            raise ValueError("tier Huffman LZSS roundtrip mismatch")
        multi_huff_candidates = []
        multi_shortmatch_candidates = []
        multi_longmatch_candidates = []
        multi_classmatch_candidates = []
        for lengths in multi_huffman_lengths or [huff_lengths]:
            stream = huff_lzss_encode(blob, lengths, args.lz_candidates)
            if huff_lzss_decode(stream, len(blob), lengths) != blob:
                raise ValueError("multi Huffman LZSS roundtrip mismatch")
            multi_huff_candidates.append(len(stream))
            short_stream = huff_shortmatch_lzss_encode(blob, lengths, args.lz_candidates)
            if huff_shortmatch_lzss_decode(short_stream, len(blob), lengths) != blob:
                raise ValueError("multi Huffman shortmatch LZSS roundtrip mismatch")
            multi_shortmatch_candidates.append(len(short_stream))
            long_stream = huff_longmatch_lzss_encode(blob, lengths, args.lz_candidates)
            if huff_longmatch_lzss_decode(long_stream, len(blob), lengths) != blob:
                raise ValueError("multi Huffman longmatch LZSS roundtrip mismatch")
            multi_longmatch_candidates.append(len(long_stream))
            class_stream = huff_classmatch_lzss_encode(blob, lengths, args.lz_candidates)
            if huff_classmatch_lzss_decode(class_stream, len(blob), lengths) != blob:
                raise ValueError("multi Huffman classmatch LZSS roundtrip mismatch")
            multi_classmatch_candidates.append(len(class_stream))
        multi_huff_size = min(multi_huff_candidates) if multi_huff_candidates else before
        multi_shortmatch_size = min(multi_shortmatch_candidates) if multi_shortmatch_candidates else before
        multi_longmatch_size = min(multi_longmatch_candidates) if multi_longmatch_candidates else before
        multi_classmatch_size = min(multi_classmatch_candidates) if multi_classmatch_candidates else before
        shortmatch_stream = huff_shortmatch_lzss_encode(blob, huff_lengths, args.lz_candidates)
        if huff_shortmatch_lzss_decode(shortmatch_stream, len(blob), huff_lengths) != blob:
            raise ValueError("Huffman shortmatch LZSS roundtrip mismatch")
        rescue3k_huff_size = before
        rescue3k_shortmatch_size = before
        rescue3k_longmatch_size = before
        rescue3k_classmatch_size = before
        if raw_tier_label(before) == "3K->2K":
            rescue_huff_candidates = []
            rescue_shortmatch_candidates = []
            rescue_longmatch_candidates = []
            rescue_classmatch_candidates = []
            for lengths in rescue_3k_huffman_lengths or []:
                stream = huff_lzss_encode(blob, lengths, args.lz_candidates)
                if huff_lzss_decode(stream, len(blob), lengths) != blob:
                    raise ValueError("rescue3k Huffman LZSS roundtrip mismatch")
                rescue_huff_candidates.append(len(stream))
                short_stream = huff_shortmatch_lzss_encode(blob, lengths, args.lz_candidates)
                if huff_shortmatch_lzss_decode(short_stream, len(blob), lengths) != blob:
                    raise ValueError("rescue3k shortmatch LZSS roundtrip mismatch")
                rescue_shortmatch_candidates.append(len(short_stream))
                long_stream = huff_longmatch_lzss_encode(blob, lengths, args.lz_candidates)
                if huff_longmatch_lzss_decode(long_stream, len(blob), lengths) != blob:
                    raise ValueError("rescue3k longmatch LZSS roundtrip mismatch")
                rescue_longmatch_candidates.append(len(long_stream))
                class_stream = huff_classmatch_lzss_encode(blob, lengths, args.lz_candidates)
                if huff_classmatch_lzss_decode(class_stream, len(blob), lengths) != blob:
                    raise ValueError("rescue3k classmatch LZSS roundtrip mismatch")
                rescue_classmatch_candidates.append(len(class_stream))
            if rescue_huff_candidates:
                rescue3k_huff_size = min(rescue_huff_candidates)
            if rescue_shortmatch_candidates:
                rescue3k_shortmatch_size = min(rescue_shortmatch_candidates)
            if rescue_longmatch_candidates:
                rescue3k_longmatch_size = min(rescue_longmatch_candidates)
            if rescue_classmatch_candidates:
                rescue3k_classmatch_size = min(rescue_classmatch_candidates)
        short_lzss_stream = lzss_short_encode(blob, max_candidates=args.lz_candidates)
        if lzss_short_decode(short_lzss_stream, len(blob)) != blob:
            raise ValueError("short LZSS roundtrip mismatch")
        phrase_stream = phrase_encode(blob, phrases)
        phrase_lzss_stream = lzss_encode(
            phrase_stream, window=2047, max_match=35, token_bytes=2, max_candidates=args.lz_candidates, lazy=True
        )
        phrase_lzss_mid = lzss_decode(phrase_lzss_stream, len(phrase_stream), token_bytes=2)
        if phrase_decode(phrase_lzss_mid, phrases) != blob:
            raise ValueError("phrase+LZSS roundtrip mismatch")
        rle_lzss16_stream = lzss_encode(
            rle_stream, window=2047, max_match=35, token_bytes=2, max_candidates=args.lz_candidates
        )
        if lzss_decode(rle_lzss16_stream, len(rle_stream), token_bytes=2) != rle_stream:
            raise ValueError("RLE+LZSS roundtrip mismatch")
        lzss24_size, _lzss24_stream = lzss_region(blob, token_bytes=3, max_candidates=args.lz_candidates)
        zlib_size, _zlib_stream = zlib_region(blob)
        metadata_zero = len(set(region.payloads)) == 1 and region.payloads[0] in (repeated_payload_dict or set())
        hw_best = min(
            before,
            template_size,
            bdi_size,
            len(optimal_lzss16_stream),
            len(hf_lzss_stream),
            len(hf_cost_stream),
            len(tier_hf_cost_stream),
            len(huff_lzss_stream),
            len(tier_huff_lzss_stream),
            multi_huff_size,
            len(shortmatch_stream),
            multi_shortmatch_size,
            rescue3k_huff_size,
            rescue3k_shortmatch_size,
            rescue3k_longmatch_size,
            multi_longmatch_size,
            rescue3k_classmatch_size,
            multi_classmatch_size,
        )
        if metadata_zero:
            hw_best = 0
        results["local-phrase"] = (min(before, phrase_size), len(phrases))
        results["metadata-repeat-dict"] = (0 if metadata_zero else before, 0)
        results["template-exception"] = (min(before, template_size), 0)
        results["bdi-region"] = (min(before, bdi_size), 0)
        results["byte-rle"] = (min(before, len(rle_stream)), 0)
        results["bounded-lzss16"] = (min(before, lzss16_size), 0)
        results["lazy-lzss16"] = (min(before, lazy_lzss16_size), 0)
        results["optimal-lzss16"] = (min(before, len(optimal_lzss16_stream)), 0)
        results["hf-literal-lzss16"] = (min(before, len(hf_lzss_stream)), 0)
        results["hf-literal-costdp"] = (min(before, len(hf_cost_stream)), 0)
        results["tier-hf-literal-costdp"] = (min(before, len(tier_hf_cost_stream)), 0)
        results["huff-literal-lzss"] = (min(before, len(huff_lzss_stream)), 0)
        results["tier-huff-literal-lzss"] = (min(before, len(tier_huff_lzss_stream)), 0)
        results["multi-huff-literal-lzss"] = (min(before, multi_huff_size), len(multi_huffman_lengths or []))
        results["huff-shortmatch-lzss"] = (min(before, len(shortmatch_stream)), 0)
        results["multi-huff-shortmatch-lzss"] = (
            min(before, multi_shortmatch_size),
            len(multi_huffman_lengths or []),
        )
        results["rescue3k-huff-lzss"] = (min(before, rescue3k_huff_size), len(rescue_3k_huffman_lengths or []))
        results["rescue3k-shortmatch-lzss"] = (
            min(before, rescue3k_shortmatch_size),
            len(rescue_3k_huffman_lengths or []),
        )
        results["rescue3k-longmatch-lzss"] = (
            min(before, rescue3k_longmatch_size),
            len(rescue_3k_huffman_lengths or []),
        )
        results["multi-huff-longmatch-lzss"] = (min(before, multi_longmatch_size), len(multi_huffman_lengths or []))
        results["multi-huff-classmatch-lzss"] = (
            min(before, multi_classmatch_size),
            len(multi_huffman_lengths or []),
        )
        results["rescue3k-classmatch-lzss"] = (
            min(before, rescue3k_classmatch_size),
            len(rescue_3k_huffman_lengths or []),
        )
        results["short-lzss"] = (min(before, len(short_lzss_stream)), 0)
        results["phrase-lzss16"] = (min(before, phrase_table_bytes(phrases) + len(phrase_lzss_stream)), len(phrases))
        results["rle-lzss16"] = (min(before, len(rle_lzss16_stream)), 0)
        results["bounded-lzss24"] = (min(before, lzss24_size), 0)
        results["zlib-region"] = (min(before, zlib_size), 0)
        tiered_after = before
        if before <= 1024 and metadata_zero:
            tiered_after = 0
        elif before > 1024:
            tiered_after = min(before, zlib_size)
        results["tiered-zlib-dict"] = (tiered_after, 0)
        results["tiered-hw-template-bdi-lz"] = (hw_best, 0)
        for method, (after, extra) in results.items():
            s = summary[method]
            s["regions"] += 1
            crossed_labels = []
            for label, low, high, target in RAW_TIERS:
                eligible = low < before <= high
                if eligible:
                    s[f"eligible_{label}"] += 1
                    if after <= target:
                        s[f"success_{label}"] += 1
                        crossed_labels.append(label)
            if after < before:
                s["changed"] += 1
            s["raw_saved"] += before - after
            s["best_after"] = min(s["best_after"], after)
            rows.append(
                {
                    "region": region.index,
                    "method": method,
                    "before_raw": before,
                    "after_raw": after,
                    "before_tier": next((label.split("->")[0] for label, low, high, _target in RAW_TIERS if low < before <= high), ">4K"),
                    "crossed": ";".join(crossed_labels),
                    "raw_saved": before - after,
                    "extra": extra,
                }
            )
    return list(summary.values()), rows


def write_csv(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--payloads", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--limit-regions", type=int, default=0)
    parser.add_argument("--phrases", type=int, default=16)
    parser.add_argument("--candidates", type=int, default=96)
    parser.add_argument("--lz-candidates", type=int, default=32)
    parser.add_argument("--train-payloads", type=Path)
    parser.add_argument("--repeat-dict-entries", type=int, default=256)
    parser.add_argument("--huff-buckets", type=int, default=8)
    args = parser.parse_args()

    regions = read_payloads(args.payloads, args.limit_regions)
    repeat_dict = None
    literal_dict = None
    tier_literal_dicts = None
    huffman_lengths = None
    tier_huffman_lengths = None
    multi_huffman_lengths = None
    rescue_3k_huffman_lengths = None
    if args.train_payloads:
        train_regions = read_payloads(args.train_payloads)
        repeat_dict = train_repeated_payload_dictionary(train_regions, args.repeat_dict_entries)
        literal_dict = train_literal_dictionary(train_regions)
        tier_literal_dicts = train_tier_literal_dictionaries(train_regions)
        huffman_lengths = train_huffman_lengths(train_regions)
        tier_huffman_lengths = train_tier_huffman_lengths(train_regions)
        multi_huffman_lengths = train_size_bucket_huffman_lengths(train_regions, args.huff_buckets)
        rescue_3k_huffman_lengths = train_3k_rescue_huffman_lengths(train_regions, args.huff_buckets)
    else:
        literal_dict = train_literal_dictionary(regions)
        tier_literal_dicts = train_tier_literal_dictionaries(regions)
        huffman_lengths = train_huffman_lengths(regions)
        tier_huffman_lengths = train_tier_huffman_lengths(regions)
        multi_huffman_lengths = train_size_bucket_huffman_lengths(regions, args.huff_buckets)
        rescue_3k_huffman_lengths = train_3k_rescue_huffman_lengths(regions, args.huff_buckets)
    summary, rows = evaluate(
        regions,
        args,
        repeat_dict,
        literal_dict,
        tier_literal_dicts,
        huffman_lengths,
        tier_huffman_lengths,
        multi_huffman_lengths,
        rescue_3k_huffman_lengths,
    )
    write_csv(args.output_dir / f"{args.name}-2k-to-1k-summary.csv", summary)
    write_csv(args.output_dir / f"{args.name}-2k-to-1k-regions.csv", rows)
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
