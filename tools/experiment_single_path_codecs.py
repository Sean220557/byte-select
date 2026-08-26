#!/usr/bin/env python3
"""Compare independent lossless paths against the latest FPC+BSEL baseline.

No result in this experiment selects between different codecs per region or
subline. Each named method is applied to the complete input. Raw fallback is a
part of every lossless codec and is charged as 256B per subline or 4KB per
region. All implemented codecs are decoded and checked byte-for-byte.
"""

import argparse
import csv
import json
import struct
import zlib
from collections import Counter
from pathlib import Path


REGION_BYTES = 4096
SUBLINE_BYTES = 256
SUBLINES_PER_REGION = 16
SECTOR_BYTES = 64
TIERS = (1024, 2048, 3072, 4096)
PAYLOAD_MAGIC = b"FPCPAY1\0"


def align(value, quantum=SECTOR_BYTES):
    return ((value + quantum - 1) // quantum) * quantum


def allocated_tier(value):
    for bound in TIERS:
        if value <= bound:
            return bound
    return REGION_BYTES


def read_fpc_payloads(path):
    data = path.read_bytes()
    if len(data) < 16 or data[:8] != PAYLOAD_MAGIC:
        raise ValueError("bad FPCPAY1 file: {}".format(path))
    count = struct.unpack_from("<Q", data, 8)[0]
    cursor = 16
    result = []
    for _ in range(count):
        if cursor + 2 > len(data):
            raise ValueError("truncated FPC payload length")
        size = struct.unpack_from("<H", data, cursor)[0]
        cursor += 2
        if cursor + size > len(data):
            raise ValueError("truncated FPC payload")
        result.append(data[cursor:cursor + size])
        cursor += size
    if cursor != len(data):
        raise ValueError("trailing FPC payload bytes")
    return result


def read_trace(path):
    data = path.read_bytes()
    if not data or len(data) % REGION_BYTES:
        raise ValueError("trace must be a non-empty multiple of 4KB")
    return data


def read_size_list(path):
    values = [int(line.strip()) for line in path.read_text().splitlines() if line.strip()]
    if any(value < 1 or value > SUBLINE_BYTES for value in values):
        raise ValueError("invalid 256B size list: {}".format(path))
    return values


def fpc_record(payload):
    size = min(SUBLINE_BYTES, 1 + len(payload))
    return bytes((0,)) + payload[:size - 1]


def raw_record(block):
    return bytes((0xFF,)) + block


def physical_record_size(record):
    # A raw fallback uses an out-of-band raw mode and occupies exactly 256B.
    return min(SUBLINE_BYTES, align(len(record)))


def encode_bdi(block):
    candidates = []
    if not any(block):
        candidates.append(bytes((0x00,)))

    for width, tag in ((1, 0x10), (2, 0x11), (4, 0x12), (8, 0x13)):
        pattern = block[:width]
        if pattern * (len(block) // width) == block:
            candidates.append(bytes((tag,)) + pattern)

    mode = 0
    for width in (2, 4, 8):
        values = [int.from_bytes(block[i:i + width], "little")
                  for i in range(0, len(block), width)]
        for delta_width in (1, 2, 4):
            if delta_width >= width:
                continue
            # Unsigned deltas from the minimum value.
            base = min(values)
            deltas = [value - base for value in values]
            if max(deltas) < (1 << (8 * delta_width)):
                tag = 0x20 + mode
                body = bytearray((tag,))
                body.extend(base.to_bytes(width, "little"))
                for delta in deltas:
                    body.extend(delta.to_bytes(delta_width, "little"))
                candidates.append(bytes(body))
            mode += 1
            # Signed deltas from the first value.
            base = values[0]
            deltas = [value - base for value in values]
            lo = -(1 << (8 * delta_width - 1))
            hi = (1 << (8 * delta_width - 1)) - 1
            if min(deltas) >= lo and max(deltas) <= hi:
                tag = 0x40 + mode
                body = bytearray((tag,))
                body.extend(base.to_bytes(width, "little"))
                for delta in deltas:
                    body.extend(delta.to_bytes(delta_width, "little", signed=True))
                candidates.append(bytes(body))
            mode += 1
    candidates.append(raw_record(block))
    return min(candidates, key=len)


def decode_bdi(record):
    tag = record[0]
    if tag == 0xFF:
        return record[1:]
    if tag == 0x00:
        return bytes(SUBLINE_BYTES)
    if 0x10 <= tag <= 0x13:
        width = (1, 2, 4, 8)[tag - 0x10]
        return record[1:1 + width] * (SUBLINE_BYTES // width)

    signed = tag >= 0x40
    encoded_mode = tag - (0x40 if signed else 0x20)
    mode = encoded_mode - 1 if signed else encoded_mode
    pairs = []
    counter = 0
    for width in (2, 4, 8):
        for delta_width in (1, 2, 4):
            if delta_width >= width:
                continue
            pairs.append((counter, width, delta_width))
            counter += 2
    lookup = {index: (width, delta_width) for index, width, delta_width in pairs}
    if mode not in lookup:
        raise ValueError("bad BDI tag")
    width, delta_width = lookup[mode]
    cursor = 1
    base = int.from_bytes(record[cursor:cursor + width], "little")
    cursor += width
    output = bytearray()
    modulus = 1 << (8 * width)
    for _ in range(SUBLINE_BYTES // width):
        delta = int.from_bytes(record[cursor:cursor + delta_width], "little", signed=signed)
        cursor += delta_width
        output.extend(((base + delta) % modulus).to_bytes(width, "little"))
    if cursor != len(record):
        raise ValueError("trailing BDI data")
    return bytes(output)


def encode_frequent_word(block):
    candidates = []
    for mode, width in enumerate((1, 2, 4, 8)):
        words = [block[i:i + width] for i in range(0, len(block), width)]
        base = Counter(words).most_common(1)[0][0]
        bitmap = bytearray((len(words) + 7) // 8)
        exceptions = bytearray()
        for index, word in enumerate(words):
            if word != base:
                bitmap[index // 8] |= 1 << (index % 8)
                exceptions.extend(word)
        candidates.append(bytes((0x60 + mode,)) + base + bytes(bitmap) + bytes(exceptions))
    candidates.append(raw_record(block))
    return min(candidates, key=len)


def decode_frequent_word(record):
    tag = record[0]
    if tag == 0xFF:
        return record[1:]
    if not 0x60 <= tag <= 0x63:
        raise ValueError("bad frequent-word tag")
    width = (1, 2, 4, 8)[tag - 0x60]
    words = SUBLINE_BYTES // width
    cursor = 1
    base = record[cursor:cursor + width]
    cursor += width
    bitmap_bytes = (words + 7) // 8
    bitmap = record[cursor:cursor + bitmap_bytes]
    cursor += bitmap_bytes
    output = bytearray()
    for index in range(words):
        if bitmap[index // 8] & (1 << (index % 8)):
            output.extend(record[cursor:cursor + width])
            cursor += width
        else:
            output.extend(base)
    if cursor != len(record):
        raise ValueError("trailing frequent-word data")
    return bytes(output)


def encode_delta_bitplane(block):
    words = [int.from_bytes(block[i:i + 4], "little") for i in range(0, len(block), 4)]
    xors = [words[i] ^ words[i - 1] for i in range(1, len(words))]
    planes = []
    for bit in range(32):
        plane = 0
        for index, value in enumerate(xors):
            plane |= ((value >> bit) & 1) << index
        planes.append(plane)
    full = (1 << 63) - 1
    body = bytearray((0x70,))
    body.extend(words[0].to_bytes(4, "little"))
    bit = 0
    while bit < 32:
        plane = planes[bit]
        if plane == 0:
            run = 1
            while bit + run < 32 and planes[bit + run] == 0 and run < 32:
                run += 1
            body.extend((0x00, run))
            bit += run
            continue
        if plane == full:
            body.append(0x01)
        elif bin(plane).count("1") == 1:
            body.extend((0x02, (plane & -plane).bit_length() - 1))
        elif (bin(plane).count("1") == 2
              and bin(plane & (plane >> 1)).count("1") == 1):
            body.extend((0x03, (plane & -plane).bit_length() - 1))
        else:
            body.append(0x04)
            body.extend(plane.to_bytes(8, "little"))
        bit += 1
    encoded = bytes(body)
    return encoded if len(encoded) < 257 else raw_record(block)


def decode_delta_bitplane(record):
    if record[0] == 0xFF:
        return record[1:]
    if record[0] != 0x70:
        raise ValueError("bad delta-bitplane tag")
    first = int.from_bytes(record[1:5], "little")
    cursor = 5
    planes = []
    full = (1 << 63) - 1
    while len(planes) < 32:
        token = record[cursor]
        cursor += 1
        if token == 0x00:
            run = record[cursor]
            cursor += 1
            planes.extend([0] * run)
        elif token == 0x01:
            planes.append(full)
        elif token == 0x02:
            position = record[cursor]
            cursor += 1
            planes.append(1 << position)
        elif token == 0x03:
            position = record[cursor]
            cursor += 1
            planes.append(3 << position)
        elif token == 0x04:
            planes.append(int.from_bytes(record[cursor:cursor + 8], "little"))
            cursor += 8
        else:
            raise ValueError("bad delta-bitplane token")
    if len(planes) != 32 or cursor != len(record):
        raise ValueError("bad delta-bitplane stream")
    xors = [0] * 63
    for bit, plane in enumerate(planes):
        for index in range(63):
            xors[index] |= ((plane >> index) & 1) << bit
    words = [first]
    for value in xors:
        words.append(words[-1] ^ value)
    return b"".join(value.to_bytes(4, "little") for value in words)


def encode_zlib(block, level):
    compressed = bytes((0x80 + level,)) + zlib.compress(block, level)
    return compressed if len(compressed) < 257 else raw_record(block)


def decode_zlib(record):
    if record[0] == 0xFF:
        return record[1:]
    return zlib.decompress(record[1:])


def encode_byte_rle(block):
    body = bytearray((0xA0,))
    cursor = 0
    while cursor < len(block):
        run = 1
        while cursor + run < len(block) and block[cursor + run] == block[cursor] and run < 130:
            run += 1
        if run >= 3:
            body.extend((0x80 | (run - 3), block[cursor]))
            cursor += run
            continue
        literal_begin = cursor
        cursor += run
        while cursor < len(block) and cursor - literal_begin < 128:
            next_run = 1
            while (cursor + next_run < len(block)
                   and block[cursor + next_run] == block[cursor]
                   and next_run < 3):
                next_run += 1
            if next_run >= 3:
                break
            room = 128 - (cursor - literal_begin)
            cursor += min(next_run, room)
        literal = block[literal_begin:cursor]
        body.append(len(literal) - 1)
        body.extend(literal)
    encoded = bytes(body)
    return encoded if len(encoded) < 257 else raw_record(block)


def decode_byte_rle(record):
    if record[0] == 0xFF:
        return record[1:]
    if record[0] != 0xA0:
        raise ValueError("bad byte-RLE tag")
    output = bytearray()
    cursor = 1
    while cursor < len(record):
        control = record[cursor]
        cursor += 1
        if control & 0x80:
            length = (control & 0x7F) + 3
            output.extend(bytes((record[cursor],)) * length)
            cursor += 1
        else:
            length = control + 1
            output.extend(record[cursor:cursor + length])
            cursor += length
    if len(output) != SUBLINE_BYTES:
        raise ValueError("bad byte-RLE output size")
    return bytes(output)


def put_uleb128(output, value):
    while value >= 0x80:
        output.append((value & 0x7F) | 0x80)
        value >>= 7
    output.append(value)


def get_uleb128(data, cursor):
    value = 0
    shift = 0
    while True:
        byte = data[cursor]
        cursor += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, cursor
        shift += 7
        if shift > 35:
            raise ValueError("invalid ULEB128")


def encode_xor_varint(block):
    words = [int.from_bytes(block[i:i + 4], "little") for i in range(0, len(block), 4)]
    body = bytearray((0xA1,))
    body.extend(words[0].to_bytes(4, "little"))
    previous = words[0]
    for value in words[1:]:
        put_uleb128(body, value ^ previous)
        previous = value
    encoded = bytes(body)
    return encoded if len(encoded) < 257 else raw_record(block)


def decode_xor_varint(record):
    if record[0] == 0xFF:
        return record[1:]
    if record[0] != 0xA1:
        raise ValueError("bad xor-varint tag")
    cursor = 1
    previous = int.from_bytes(record[cursor:cursor + 4], "little")
    cursor += 4
    words = [previous]
    for _ in range(63):
        delta, cursor = get_uleb128(record, cursor)
        previous ^= delta
        words.append(previous)
    if cursor != len(record):
        raise ValueError("trailing xor-varint data")
    return b"".join(value.to_bytes(4, "little") for value in words)


def encode_lzss(block):
    tokens = []
    positions = {}
    cursor = 0
    while cursor < len(block):
        best_length = 0
        best_distance = 0
        key = block[cursor:cursor + 4]
        if len(key) == 4:
            for previous in reversed(positions.get(key, [])[-32:]):
                distance = cursor - previous
                if distance <= 0 or distance > 255:
                    continue
                length = 4
                while (cursor + length < len(block)
                       and block[previous + length] == block[cursor + length]
                       and length < 259):
                    length += 1
                if length > best_length:
                    best_length = length
                    best_distance = distance
        if best_length >= 4:
            tokens.append((1, best_distance, best_length))
            advance = best_length
        else:
            tokens.append((0, block[cursor], 0))
            advance = 1
        for position in range(cursor, min(len(block) - 3, cursor + advance)):
            positions.setdefault(block[position:position + 4], []).append(position)
        cursor += advance

    body = bytearray((0xA2,))
    for begin in range(0, len(tokens), 8):
        group = tokens[begin:begin + 8]
        control = sum((token[0] & 1) << index for index, token in enumerate(group))
        body.append(control)
        for is_match, first, second in group:
            if is_match:
                body.extend((first, second - 4))
            else:
                body.append(first)
    encoded = bytes(body)
    return encoded if len(encoded) < 257 else raw_record(block)


def decode_lzss(record):
    if record[0] == 0xFF:
        return record[1:]
    if record[0] != 0xA2:
        raise ValueError("bad LZSS tag")
    output = bytearray()
    cursor = 1
    while cursor < len(record) and len(output) < SUBLINE_BYTES:
        control = record[cursor]
        cursor += 1
        for index in range(8):
            if len(output) >= SUBLINE_BYTES:
                break
            if control & (1 << index):
                distance = record[cursor]
                length = record[cursor + 1] + 4
                cursor += 2
                if distance == 0 or distance > len(output):
                    raise ValueError("bad LZSS distance")
                for _ in range(length):
                    output.append(output[-distance])
            else:
                output.append(record[cursor])
                cursor += 1
    if len(output) != SUBLINE_BYTES or cursor != len(record):
        raise ValueError("bad LZSS stream")
    return bytes(output)


def encode_anchor_xor(region):
    blocks = [region[i:i + SUBLINE_BYTES]
              for i in range(0, REGION_BYTES, SUBLINE_BYTES)]
    records = [raw_record(blocks[0])]
    anchor_words = [blocks[0][i:i + 8] for i in range(0, SUBLINE_BYTES, 8)]
    for block in blocks[1:]:
        words = [block[i:i + 8] for i in range(0, SUBLINE_BYTES, 8)]
        bitmap = bytearray(4)
        deltas = bytearray()
        for index, (anchor, word) in enumerate(zip(anchor_words, words)):
            delta = bytes(a ^ b for a, b in zip(anchor, word))
            if any(delta):
                bitmap[index // 8] |= 1 << (index % 8)
                deltas.extend(delta)
        record = bytes((0x90,)) + bytes(bitmap) + bytes(deltas)
        records.append(record if len(record) < 257 else raw_record(block))
    return records


def decode_anchor_xor(records):
    anchor = records[0][1:]
    output = [anchor]
    anchor_words = [anchor[i:i + 8] for i in range(0, SUBLINE_BYTES, 8)]
    for record in records[1:]:
        if record[0] == 0xFF:
            output.append(record[1:])
            continue
        if record[0] != 0x90:
            raise ValueError("bad anchor-xor tag")
        bitmap = record[1:5]
        cursor = 5
        block = bytearray()
        for index, anchor_word in enumerate(anchor_words):
            if bitmap[index // 8] & (1 << (index % 8)):
                delta = record[cursor:cursor + 8]
                cursor += 8
                block.extend(bytes(a ^ b for a, b in zip(anchor_word, delta)))
            else:
                block.extend(anchor_word)
        if cursor != len(record):
            raise ValueError("trailing anchor-xor data")
        output.append(bytes(block))
    return b"".join(output)


def subline_region_size(records):
    return min(REGION_BYTES, sum(physical_record_size(record) for record in records))


def direct_offset_size(records, header_bytes):
    return min(REGION_BYTES, align(header_bytes + sum(len(record) for record in records)))


def evaluate(trace, fpc_payloads, cpack_sizes, header_bytes, zlib_level):
    methods = [
        "fpc-bsel",
        "fpc-bsel-direct-offset",
        "bdi",
        "frequent-word",
        "delta-bitplane",
        "anchor-xor",
        "byte-rle",
        "xor-varint",
        "lzss-subline",
        "zlib-subline",
        "zlib-region",
    ]
    if cpack_sizes is not None:
        methods.extend(("cpack-bsel", "cpack-bsel-direct-offset"))
    rows = []
    for region_index in range(len(trace) // REGION_BYTES):
        region = trace[region_index * REGION_BYTES:(region_index + 1) * REGION_BYTES]
        blocks = [region[i:i + SUBLINE_BYTES]
                  for i in range(0, REGION_BYTES, SUBLINE_BYTES)]
        payload_slice = fpc_payloads[
            region_index * SUBLINES_PER_REGION:(region_index + 1) * SUBLINES_PER_REGION]
        fpc_records = [fpc_record(payload) for payload in payload_slice]
        baseline_size = subline_region_size(fpc_records)
        baseline_tier = allocated_tier(baseline_size)

        encoded = {}
        encoded["fpc-bsel"] = baseline_size
        encoded["fpc-bsel-direct-offset"] = direct_offset_size(fpc_records, header_bytes)
        if cpack_sizes is not None:
            cpack_slice = cpack_sizes[
                region_index * SUBLINES_PER_REGION:(region_index + 1) * SUBLINES_PER_REGION]
            encoded["cpack-bsel"] = min(
                REGION_BYTES, sum(align(size) for size in cpack_slice))
            encoded["cpack-bsel-direct-offset"] = min(
                REGION_BYTES, align(header_bytes + sum(cpack_slice)))

        codec_specs = (
            ("bdi", encode_bdi, decode_bdi),
            ("frequent-word", encode_frequent_word, decode_frequent_word),
            ("delta-bitplane", encode_delta_bitplane, decode_delta_bitplane),
            ("byte-rle", encode_byte_rle, decode_byte_rle),
            ("xor-varint", encode_xor_varint, decode_xor_varint),
            ("lzss-subline", encode_lzss, decode_lzss),
            ("zlib-subline", lambda block: encode_zlib(block, zlib_level), decode_zlib),
        )
        for name, encoder, decoder in codec_specs:
            records = [encoder(block) for block in blocks]
            for block_index, (block, record) in enumerate(zip(blocks, records)):
                if decoder(record) != block:
                    raise AssertionError("{} round-trip mismatch region={} subline={}".format(
                        name, region_index, block_index))
            encoded[name] = subline_region_size(records)

        anchor_records = encode_anchor_xor(region)
        if decode_anchor_xor(anchor_records) != region:
            raise AssertionError("anchor-xor round-trip mismatch region={}".format(region_index))
        encoded["anchor-xor"] = subline_region_size(anchor_records)

        region_record = zlib.compress(region, zlib_level)
        if zlib.decompress(region_record) != region:
            raise AssertionError("zlib-region round-trip mismatch region={}".format(region_index))
        encoded["zlib-region"] = min(REGION_BYTES, align(1 + len(region_record)))

        for method in methods:
            size = encoded[method]
            after_tier = allocated_tier(size)
            rows.append({
                "dataset": "",
                "region": region_index,
                "method": method,
                "baseline_bytes": baseline_size,
                "baseline_tier": baseline_tier,
                "encoded_bytes": size,
                "after_tier": after_tier,
                "tier_steps": max(0, (baseline_tier - after_tier) // 1024),
                "allocated_saved_bytes": baseline_tier - after_tier,
                "roundtrip": 1,
            })
    return rows


def summaries(rows, dataset):
    methods = sorted(set(row["method"] for row in rows))
    output = []
    for method in methods:
        method_rows = [row for row in rows if row["method"] == method]
        for before_tier, target in ((4096, 3072), (3072, 2048), (2048, 1024)):
            tier_rows = [row for row in method_rows if row["baseline_tier"] == before_tier]
            near_rows = [row for row in tier_rows
                         if 0 < row["baseline_bytes"] - target <= 256]
            for population, selected in (("all", tier_rows), ("near256", near_rows)):
                output.append({
                    "dataset": dataset,
                    "method": method,
                    "population": population,
                    "before_tier": before_tier,
                    "target": target,
                    "eligible": len(selected),
                    "crossed": sum(row["after_tier"] <= target for row in selected),
                    "allocated_saved_bytes": sum(row["allocated_saved_bytes"] for row in selected),
                })
        output.append({
            "dataset": dataset,
            "method": method,
            "population": "total",
            "before_tier": 0,
            "target": 0,
            "eligible": len(method_rows),
            "crossed": sum(row["tier_steps"] > 0 for row in method_rows),
            "allocated_saved_bytes": sum(row["allocated_saved_bytes"] for row in method_rows),
        })
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("payloads", type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--header-bytes", type=int, default=48)
    parser.add_argument("--cpack-sizes", type=Path)
    parser.add_argument("--zlib-level", type=int, default=1)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.header_bytes < 40:
        raise ValueError("direct-offset header must be at least 40B")
    trace = read_trace(args.trace)
    payloads = read_fpc_payloads(args.payloads)
    cpack_sizes = read_size_list(args.cpack_sizes) if args.cpack_sizes else None
    expected = len(trace) // SUBLINE_BYTES
    if len(payloads) != expected:
        raise ValueError("trace/payload subline count mismatch: {} != {}".format(
            expected, len(payloads)))
    if cpack_sizes is not None and len(cpack_sizes) != expected:
        raise ValueError("trace/C-Pack size count mismatch: {} != {}".format(
            expected, len(cpack_sizes)))
    rows = evaluate(trace, payloads, cpack_sizes, args.header_bytes, args.zlib_level)
    for row in rows:
        row["dataset"] = args.name
    summary = summaries(rows, args.name)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    detail_path = args.output_dir / "{}-single-path-regions.csv".format(args.name)
    summary_path = args.output_dir / "{}-single-path-summary.csv".format(args.name)
    config_path = args.output_dir / "{}-single-path-config.json".format(args.name)
    with detail_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    with summary_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=summary[0].keys())
        writer.writeheader()
        writer.writerows(summary)
    config_path.write_text(json.dumps({
        "format": "single-path-codecs-v1",
        "dataset": args.name,
        "regions": len(trace) // REGION_BYTES,
        "fpc_direct_offset_header_bytes": args.header_bytes,
        "zlib_level": args.zlib_level,
        "cpack_sizes": str(args.cpack_sizes) if args.cpack_sizes else None,
        "sector_bytes": SECTOR_BYTES,
        "all_methods_roundtrip": True,
        "hybrid_selection": False,
    }, indent=2), encoding="utf-8")
    print(json.dumps({"regions": len(trace) // REGION_BYTES, "summary": summary},
                     ensure_ascii=False))


if __name__ == "__main__":
    main()
