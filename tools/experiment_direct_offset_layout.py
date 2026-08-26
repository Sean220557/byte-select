#!/usr/bin/env python3
"""Evaluate one FPC+BSEL path with a compact direct-offset region layout.

This is deliberately not a hybrid compressor. Every 256-byte subline is
encoded by the latest FPC+BSEL payload path. The only change from the baseline
is physical placement:

* baseline: each subline is independently rounded up to a 64-byte sector;
* direct-offset: the 16 encoded records are concatenated behind one fixed-size
  region header and the complete region is rounded once.

The header contains 16 offsets and 16 lengths, so any subline is located with
one bounded metadata lookup rather than a prefix sum. The payload round-trip
check proves that packing and random extraction preserve every encoded record.
Raw FPC+BSEL round-trip remains the responsibility of ``sizes-256 --roundtrip``.
"""

import argparse
import csv
import json
import struct
from pathlib import Path


MAGIC = b"FPCPAY1\0"
SUBLINES_PER_REGION = 16
REGION_BYTES = 4096
SECTOR_BYTES = 64
TIERS = (1024, 2048, 3072, 4096)


def align(value, quantum):
    return ((value + quantum - 1) // quantum) * quantum


def tier(value):
    for bound in TIERS:
        if value <= bound:
            return bound
    return REGION_BYTES


def read_payloads(path):
    data = path.read_bytes()
    if len(data) < 16 or data[:8] != MAGIC:
        raise ValueError("bad FPCPAY1 payload file: {}".format(path))
    count = struct.unpack_from("<Q", data, 8)[0]
    offset = 16
    payloads = []
    for _ in range(count):
        if offset + 2 > len(data):
            raise ValueError("truncated payload length")
        size = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        if offset + size > len(data):
            raise ValueError("truncated payload data")
        payloads.append(data[offset:offset + size])
        offset += size
    if offset != len(data):
        raise ValueError("trailing bytes in payload file")
    return payloads


def baseline_record(payload):
    # The current 256B format charges one shared control byte per subline.
    size = min(256, 1 + len(payload))
    # A deterministic synthetic control byte is sufficient for validating that
    # the layout preserves record boundaries and payload contents.
    return bytes((0,)) + payload[:size - 1]


def pack_region(records, header_bytes, sector_bytes):
    logical = header_bytes + sum(len(record) for record in records)
    physical = min(REGION_BYTES, align(logical, sector_bytes))
    offsets = []
    lengths = []
    cursor = header_bytes
    packed = bytearray(header_bytes)
    for record in records:
        offsets.append(cursor)
        lengths.append(len(record))
        packed.extend(record)
        cursor += len(record)
    return bytes(packed), offsets, lengths, logical, physical


def verify_region(records, packed, offsets, lengths):
    if len(offsets) != SUBLINES_PER_REGION or len(lengths) != SUBLINES_PER_REGION:
        raise AssertionError("invalid direct-offset table")
    for index, expected in enumerate(records):
        begin = offsets[index]
        actual = packed[begin:begin + lengths[index]]
        if actual != expected:
            raise AssertionError("payload round-trip mismatch at subline {}".format(index))


def summarize(rows):
    output = []
    for before_tier, target in ((4096, 3072), (3072, 2048), (2048, 1024)):
        all_rows = [row for row in rows if row["before_tier"] == before_tier]
        near_rows = [row for row in all_rows if 0 < row["gap"] <= 256]
        for population, selected in (("all", all_rows), ("near256", near_rows)):
            output.append({
                "method": "fpc-bsel-direct-offset",
                "population": population,
                "before_tier": before_tier,
                "target": target,
                "eligible": len(selected),
                "crossed": sum(row["crossed"] for row in selected),
                "baseline_bytes": sum(row["before"] for row in selected),
                "after_bytes": sum(row["after"] for row in selected),
                "saved_bytes": sum(row["before"] - row["after"] for row in selected),
                "baseline_allocated_bytes": sum(row["before_tier"] for row in selected),
                "after_allocated_bytes": sum(row["after_tier"] for row in selected),
                "allocated_saved_bytes": sum(
                    row["before_tier"] - row["after_tier"] for row in selected),
            })
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("payloads", type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--header-bytes", type=int, default=48,
                        help="charged in-region offset/length header (default: 48)")
    parser.add_argument("--sector-bytes", type=int, default=SECTOR_BYTES)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--no-roundtrip", action="store_true")
    args = parser.parse_args()

    if args.header_bytes < 40:
        raise ValueError("header must be >=40B: 16x12-bit byte offsets + 16x8-bit lengths")
    payloads = read_payloads(args.payloads)
    if len(payloads) % SUBLINES_PER_REGION:
        raise ValueError("payload count is not a multiple of 16")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    for begin in range(0, len(payloads), SUBLINES_PER_REGION):
        records = [baseline_record(payload)
                   for payload in payloads[begin:begin + SUBLINES_PER_REGION]]
        baseline = sum(align(len(record), args.sector_bytes) for record in records)
        baseline = min(REGION_BYTES, baseline)
        packed, offsets, lengths, logical, physical = pack_region(
            records, args.header_bytes, args.sector_bytes)
        if not args.no_roundtrip and physical < REGION_BYTES:
            verify_region(records, packed, offsets, lengths)
        before_tier = tier(baseline)
        target = before_tier - 1024 if before_tier > 1024 else 0
        gap = baseline - target if target else baseline
        crossed = int(target > 0 and physical <= target)
        rows.append({
            "dataset": args.name,
            "region": begin // SUBLINES_PER_REGION,
            "before": baseline,
            "before_tier": before_tier,
            "target": target,
            "gap": gap,
            "logical_payload_bytes": sum(len(record) for record in records),
            "header_bytes": args.header_bytes,
            "after": physical,
            "after_tier": tier(physical),
            "crossed": crossed,
            "saved_bytes": baseline - physical,
            "roundtrip": int(not args.no_roundtrip),
        })

    summary = summarize(rows)
    detail_path = args.output_dir / "{}-direct-offset-regions.csv".format(args.name)
    summary_path = args.output_dir / "{}-direct-offset-summary.csv".format(args.name)
    config_path = args.output_dir / "{}-direct-offset-config.json".format(args.name)
    with detail_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    with summary_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=summary[0].keys())
        writer.writeheader()
        writer.writerows(summary)
    config_path.write_text(json.dumps({
        "format": "fpc-bsel-direct-offset-v1",
        "single_codec_path": "latest-fpc-bsel",
        "header_bytes": args.header_bytes,
        "sector_bytes": args.sector_bytes,
        "offset_bits_per_subline": 12,
        "length_bits_per_subline": 8,
        "payload_roundtrip": not args.no_roundtrip,
        "raw_fallback_bytes": REGION_BYTES,
    }, indent=2), encoding="utf-8")
    print(json.dumps({"regions": len(rows), "summary": summary}, ensure_ascii=False))


if __name__ == "__main__":
    main()
