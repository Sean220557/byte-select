#!/usr/bin/env python3
"""Prune short motifs and two-run values, then build templates and a linear map."""
from __future__ import annotations

import argparse
import csv
import itertools
import json
import math
import random
import time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "results/mphf-merged-rle/template-pair-collision-balanced-mask"
FULL = (1 << 64) - 1


def load_codebook(path: Path) -> list[int]:
    """Read one tracked 32-entry codebook without experiment-module imports."""
    keys = []
    for line_no, line in enumerate(path.read_text().splitlines(), 1):
        value = line.strip().strip("|").strip()
        if not value:
            continue
        if len(value) != 16:
            raise ValueError(f"{path}:{line_no}: expected a 64-bit hex value")
        keys.append(int(value, 16))
    if len(keys) != 32 or len(set(keys)) != 32:
        raise ValueError(f"{path}: expected 32 unique 64-bit values")
    return keys


def unpack_tags(value: int) -> tuple[int, ...]:
    return tuple((value >> (4 * (15 - i))) & 0xF for i in range(16))


def runs(tags: tuple[int, ...]) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    for tag in tags:
        if result and result[-1][0] == tag:
            old_tag, length = result[-1]
            result[-1] = old_tag, length + 1
        else:
            result.append((tag, 1))
    return result


@dataclass(frozen=True)
class Template:
    members: tuple[int, ...]
    mask: int
    value: int

    @property
    def wildcards(self) -> int:
        return 16 - sum(bool((self.mask >> (4 * n)) & 0xF) for n in range(16))


def encode_periodic(key: int) -> tuple[int, int, int | None, int | None, int] | None:
    """Encode a 1/2-nibble motif with at most one exceptional nibble.

    Four-nibble structures remain available for template merging.
    """
    digits = f"{key:016X}"
    candidates = []
    for period in (1, 2):
        motif = ''.join(Counter(digits[offset::period]).most_common(1)[0][0]
                        for offset in range(period))
        expanded = motif * (16 // period)
        exceptions = [index for index, (actual, expected)
                      in enumerate(zip(digits, expanded)) if actual != expected]
        if len(exceptions) > 1:
            continue
        index = exceptions[0] if exceptions else None
        value = int(digits[index], 16) if index is not None else None
        # 1 period bit, 1 exception flag, 4/8 motif bits, and optional
        # 4-bit position plus 4-bit replacement nibble.
        bits = 2 + 4 * period + (8 if index is not None else 0)
        candidates.append((bits, period, int(motif, 16), index, value))
    if not candidates:
        return None
    bits, period, motif, index, value = min(candidates)
    return period, motif, index, value, bits


def decode_periodic(period: int, motif: int, exception_index: int | None,
                    exception_value: int | None) -> int:
    digits = list(f"{motif:0{period}X}" * (16 // period))
    if exception_index is not None:
        if exception_value is None or not 0 <= exception_index < 16:
            raise ValueError("Invalid periodic exception")
        digits[exception_index] = f"{exception_value:X}"
    return int(''.join(digits), 16)


def encode_two_run(key: int) -> tuple[int, int, int] | None:
    """Encode exactly two constant-nibble runs: two symbols and a 4-bit split."""
    tag_runs = runs(unpack_tags(key))
    if len(tag_runs) != 2:
        return None
    (first, first_length), (second, second_length) = tag_runs
    if first_length + second_length != 16 or not 1 <= first_length <= 15:
        raise AssertionError("Invalid two-run pattern")
    decoded = int(f"{first:X}" * first_length + f"{second:X}" * second_length, 16)
    if decoded != key:
        raise AssertionError("Two-run codec failed round trip")
    return first, second, first_length


def best_residual_motif(key: int) -> tuple[int, str, int, int]:
    """Audit retained patterns without changing the exact/one-exception rule."""
    digits = f"{key:016X}"
    options = []
    for period in (1, 2, 4):
        motif = ''.join(Counter(digits[offset::period]).most_common(1)[0][0]
                        for offset in range(period))
        exceptions = sum(actual != expected for actual, expected
                         in zip(digits, motif * (16 // period)))
        # Generalized-cost diagnostic: 2 period + 4 count + motif + 8/exception.
        options.append((6 + 4 * period + 8 * exceptions,
                        period, motif, exceptions))
    bits, period, motif, exceptions = min(options)
    return period, motif, exceptions, bits


def prune_keys() -> tuple[list[int], list[dict], int]:
    books = [load_codebook(ROOT / f"results/mphf-32/codebook{i}.txt")
             for i in (1, 2)]
    merged = list(dict.fromkeys(books[0] + books[1]))
    residual = []
    removed = []
    for index, key in enumerate(merged):
        periodic = encode_periodic(key)
        if periodic is not None:
            period, motif, exception_index, exception_value, bits = periodic
            if decode_periodic(period, motif, exception_index, exception_value) != key:
                raise AssertionError("Periodic motif failed round trip")
            removed.append({"merged_index": index, "raw_hex": f"{key:016X}",
                            "run_count": len(runs(unpack_tags(key))),
                            "periodic_motif_nibbles": period,
                            "motif_hex": f"{motif:0{period}X}",
                            "exception_index": exception_index,
                            "exception_value_hex": f"{exception_value:X}"
                                                   if exception_value is not None else "",
                            "encoded_bits": bits,
                            "reason": ("periodic_1_2_nibbles_one_exception"
                                       if exception_index is not None else
                                       "periodic_1_2_nibbles")})
        elif (two_run := encode_two_run(key)) is not None:
            first, second, split = two_run
            removed.append({"merged_index": index, "raw_hex": f"{key:016X}",
                            "run_count": 2, "periodic_motif_nibbles": "",
                            "motif_hex": f"{first:X}{second:X}",
                            "exception_index": split,
                            "exception_value_hex": "", "encoded_bits": 12,
                            "reason": "two_nibble_runs"})
        else:
            residual.append(key)
    return residual, removed, len(merged)


def make_template(keys: list[int], members: tuple[int, ...]) -> Template:
    mask = 0
    value = 0
    for nibble in range(16):
        digits = {(keys[i] >> (4 * nibble)) & 0xF for i in members}
        if len(digits) == 1:
            mask |= 0xF << (4 * nibble)
            value |= next(iter(digits)) << (4 * nibble)
    return Template(members, mask, value)


def matches(template: Template, raw: int) -> bool:
    return (raw & template.mask) == template.value


def classify_raw(raw: int, keys: list[int], templates: list[Template]) -> tuple[int, int] | None:
    matched = [i for i, template in enumerate(templates)
               if matches(template, raw)]
    if len(matched) != 1:
        return None
    template_index = matched[0]
    members = templates[template_index].members
    for member_index, raw_index in enumerate(members):
        if keys[raw_index] == raw:
            return template_index, member_index
    return None


def construct_templates(keys: list[int], max_wildcards: int) -> tuple[list[Template], int]:
    templates = [make_template(keys, (i,)) for i in range(len(keys))]
    merges = 0
    while True:
        options = []
        for left_idx, left in enumerate(templates):
            for right_idx in range(left_idx + 1, len(templates)):
                right = templates[right_idx]
                absorbed = {left_idx, right_idx}
                while True:
                    members = tuple(sorted(index for group in absorbed
                                           for index in templates[group].members))
                    joined = make_template(keys, members)
                    if joined.wildcards > max_wildcards:
                        break
                    newly_absorbed = {group for group, template in enumerate(templates)
                                      if any(matches(joined, keys[index])
                                             for index in template.members)}
                    if newly_absorbed <= absorbed:
                        # Absorb the complete matching closure, so a third
                        # pattern never blocks an otherwise valid merge.
                        options.append((-len(absorbed) + 1, joined.wildcards,
                                        -len(joined.members), left.members,
                                        right.members, tuple(sorted(absorbed)), joined))
                        break
                    absorbed |= newly_absorbed
        if not options:
            break
        _, _, _, _, _, absorbed, joined = min(options)
        for group in reversed(absorbed):
            templates.pop(group)
        templates.append(joined)
        merges += len(absorbed) - 1
    templates.sort(key=lambda t: min(t.members))
    for index, raw in enumerate(keys):
        found = [t for t, template in enumerate(templates)
                 if matches(template, raw)]
        if len(found) != 1 or index not in templates[found[0]].members:
            raise AssertionError(f"Template matching ambiguous for {index}: {found}")
    return templates, merges


def design_masks(templates: list[Template], extra_limit: int) -> tuple[list[int], list[int]]:
    masks = [t.mask for t in templates]
    values = [t.value for t in templates]
    dropped = [0] * len(templates)
    position_drops = [0] * 16
    while True:
        options = []
        old_equal_pairs = sum(n * (n - 1) // 2 for n in Counter(values).values())
        for index, template in enumerate(templates):
            if dropped[index] >= extra_limit:
                continue
            for nibble in range(16):
                nibble_mask = 0xF << (4 * nibble)
                if not masks[index] & nibble_mask:
                    continue
                new_value = values[index] & ~nibble_mask
                candidate = Counter(values)
                candidate[values[index]] -= 1
                candidate[new_value] += 1
                if max(candidate.values()) > 2:
                    continue
                new_equal_pairs = sum(n * (n - 1) // 2
                                      for n in candidate.values())
                # Preserve separation, spread drops across nibble positions,
                # and rotate the last tie-break per template to avoid a
                # systematic low- or high-nibble preference.
                rotation = template.members[0] % 16
                rotated_nibble_rank = (nibble - rotation) % 16
                options.append((new_equal_pairs - old_equal_pairs,
                                position_drops[nibble], dropped[index],
                                rotated_nibble_rank, index, nibble, new_value))
        if not options:
            break
        _, _, _, _, index, nibble, new_value = min(options)
        masks[index] &= ~(0xF << (4 * nibble))
        values[index] = new_value
        dropped[index] += 1
        position_drops[nibble] += 1
    return masks, values


def bounded_linear_map(features: list[int], bits: int, seconds: float,
                       seed: int = 17) -> dict | None:
    # Exhaust single-tap row combinations first. If one works, zero XOR2 is
    # globally optimal for this fixed output width.
    started = time.monotonic()
    deadline = started + seconds
    direct_deadline = started + min(2.0, seconds * 0.25)
    varying_bits = [bit for bit in range(64)
                    if len({(value >> bit) & 1 for value in features}) > 1]
    zero_xor_exhaustive = True
    for attempt, chosen in enumerate(itertools.combinations(varying_bits, bits)):
        if attempt % 1024 == 0 and time.monotonic() >= direct_deadline:
            zero_xor_exhaustive = False
            break
        slots = [sum(((value >> input_bit) & 1) << output_bit
                     for output_bit, input_bit in enumerate(chosen))
                 for value in features]
        counts = Counter(slots)
        if max(counts.values()) <= 2:
            return {"rows_hex": [f"0x{1 << bit:016X}" for bit in chosen],
                    "slots": slots,
                    "slot_histogram": dict(sorted(counts.items())),
                    "output_bits": bits, "xor2": 0, "taps": bits,
                    "zero_xor_exhaustive": zero_xor_exhaustive}
    rng = random.Random(seed)
    if time.monotonic() >= deadline:
        return None
    # Single tap rows are cheapest. The other candidates are sparse XOR rows.
    candidate_rows = [1 << bit for bit in range(64)]
    for weight, wanted in ((2, 420), (3, 220)):
        seen = set(candidate_rows)
        while wanted and time.monotonic() < deadline:
            row = sum(1 << bit for bit in rng.sample(range(64), weight))
            if row not in seen:
                seen.add(row)
                candidate_rows.append(row)
                wanted -= 1
    signatures = {row: tuple((value & row).bit_count() & 1
                             for value in features) for row in candidate_rows}
    # A state is (row masks, output signatures). Keep many balanced prefixes.
    beam = [((), (0,) * len(features))]
    for depth in range(bits):
        remaining = bits - depth - 1
        expanded = []
        for rows, old in beam:
            for row in candidate_rows:
                if row in rows:
                    continue
                sig = signatures[row]
                new = tuple(v | (sig[i] << depth)
                            for i, v in enumerate(old))
                counts = Counter(new)
                if max(counts.values()) > 2 * (1 << remaining):
                    continue
                taps = sum(x.bit_count() for x in rows) + row.bit_count()
                # Rank by fit to <=2 per slot, then by XOR implementation.
                score = (sum(max(0, count - 2) for count in counts.values()),
                         max(counts.values()),
                         sum(count * count for count in counts.values()), taps,
                         rows + (row,))
                expanded.append((score, rows + (row,), new))
            if time.monotonic() >= deadline:
                break
        if not expanded:
            return None
        expanded.sort(key=lambda item: item[0])
        beam = [(rows, values) for _, rows, values in expanded[:128]]
        if time.monotonic() >= deadline and depth + 1 < bits:
            return None
    valid = [(rows, slots) for rows, slots in beam
             if max(Counter(slots).values()) <= 2]
    if not valid:
        return None
    rows, slots = min(valid, key=lambda item:
                      (sum(row.bit_count() - 1 for row in item[0]),
                       sum(row.bit_count() for row in item[0])))
    return {"rows_hex": [f"0x{row:016X}" for row in rows],
            "slots": list(slots), "slot_histogram": dict(sorted(Counter(slots).items())),
            "output_bits": bits,
            "xor2": sum(row.bit_count() - 1 for row in rows),
            "taps": sum(row.bit_count() for row in rows),
            "zero_xor_exhaustive": zero_xor_exhaustive}


def binary_template(template: Template) -> str:
    return ''.join('?' if not ((template.mask >> bit) & 1)
                   else str((template.value >> bit) & 1)
                   for bit in reversed(range(64)))


def hex_template(template: Template) -> str:
    return ''.join('?' if not ((template.mask >> (4 * nibble)) & 0xF)
                   else f"{(template.value >> (4 * nibble)) & 0xF:X}"
                   for nibble in reversed(range(16)))


def wildcard_residual(raw: int, template: Template) -> int:
    value = 0
    index = 0
    for nibble in range(16):
        if not ((template.mask >> (4 * nibble)) & 0xF):
            value |= ((raw >> (4 * nibble)) & 0xF) << (4 * index)
            index += 1
    return value


def restore_from_residual(template: Template, residual: int) -> int:
    raw = template.value
    index = 0
    for nibble in range(16):
        if not ((template.mask >> (4 * nibble)) & 0xF):
            raw |= ((residual >> (4 * index)) & 0xF) << (4 * nibble)
            index += 1
    return raw


def main() -> None:
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=OUT)
    parser.add_argument("--max-template-wildcards", type=int, default=6)
    parser.add_argument("--max-extra-mask-nibbles", type=int, default=4)
    parser.add_argument("--seconds-per-search", type=float, default=12.0)
    args = parser.parse_args()
    OUT = args.output_dir
    if not (0 <= args.max_template_wildcards <= 16 and
            0 <= args.max_extra_mask_nibbles <= 16):
        parser.error("nibble limits must be in 0..16")
    keys, removed, merged_count = prune_keys()
    removed_hex = {item["raw_hex"] for item in removed}
    for expected in ("1B1B1B1B1B1B1B1F", "BBBBBBBBBBBBBBB2"):
        if expected not in removed_hex:
            raise AssertionError(f"Near-periodic pattern was not pruned: {expected}")
    if any(encode_periodic(key) is not None for key in keys):
        raise AssertionError("A periodic or one-exception pattern reached templates")
    if "FFFFFF0000000000" not in removed_hex:
        raise AssertionError("Two-run pattern was not pruned")
    for protected in ("FF00111111F7F5FF", "FF001111117F5FFF",
                      "0BBC0BBC0BBC0F6F",
                      "0BBC0BBC0BBC0BBC"):
        if int(protected, 16) not in keys:
            raise AssertionError(f"Non-target pattern was pruned: {protected}")
    templates, merges = construct_templates(keys, args.max_template_wildcards)
    sample_a = int("0BBC0BBC0BBC0BBC", 16)
    sample_b = int("0BBC0BBC0BBC0F6F", 16)
    sample = make_template([sample_a, sample_b], (0, 1))
    if hex_template(sample) != "0BBC0BBC0BBC0???":
        raise AssertionError("Requested 0BBC template example differs")

    chosen = None
    attempts = []
    for extra in range(args.max_extra_mask_nibbles, -1, -1):
        masks, features = design_masks(templates, extra)
        feature_counts = Counter(features)
        if max(feature_counts.values()) > 2:
            raise AssertionError("More than two templates share a masked feature")
        minimum_bits = max(1, math.ceil(math.log2(math.ceil(len(templates) / 2))))
        for bits in range(minimum_bits, max(minimum_bits, math.ceil(math.log2(len(templates)))) + 1):
            mapping = bounded_linear_map(features, bits, args.seconds_per_search)
            attempts.append({"max_extra_mask_nibbles": extra,
                             "output_bits": bits, "found": mapping is not None})
            if mapping is not None:
                chosen = (extra, masks, features, mapping)
                break
        if chosen is not None:
            break
    if chosen is None:
        raise RuntimeError("No <=2-slot linear map found within search limits")
    extra, masks, features, mapping = chosen
    slots = mapping["slots"]
    slot_groups = {slot: sorted(i for i, value in enumerate(slots) if value == slot)
                   for slot in sorted(set(slots))}
    if max(len(group) for group in slot_groups.values()) > 2:
        raise AssertionError("Slot collision cap exceeded")
    assignments = []
    slot_count = 1 << mapping["output_bits"]
    occupied_bitmap = sum(1 << slot for slot in slot_groups)
    paired_bitmap = sum(1 << slot for slot, group in slot_groups.items()
                        if len(group) == 2)
    physical_table_order = [template_id for slot in range(slot_count)
                            for template_id in slot_groups.get(slot, [])]
    stored_residuals = [[wildcard_residual(keys[member], template)
                         for member in template.members] for template in templates]
    for raw_index, raw in enumerate(keys):
        classified = classify_raw(raw, keys, templates)
        if classified is None:
            raise AssertionError("Known pattern failed template classification")
        i, member_selector = classified
        if raw & masks[i] != features[i]:
            raise AssertionError("Masked runtime feature does not match training")
        calc = sum((((features[i] & int(row, 16)).bit_count() & 1) << bit)
                   for bit, row in enumerate(mapping["rows_hex"]))
        if calc != slots[i]:
            raise AssertionError("Linear map evaluation differs")
        bucket = slot_groups[calc]
        collision_selector = bucket.index(i)
        preceding = (1 << calc) - 1
        physical_entry = ((occupied_bitmap & preceding).bit_count()
                          + (paired_bitmap & preceding).bit_count()
                          + collision_selector)
        decoded_template = physical_table_order[physical_entry]
        decoded_raw = restore_from_residual(
            templates[decoded_template],
            stored_residuals[decoded_template][member_selector])
        if decoded_raw != raw:
            raise AssertionError("Slot, collision selector, and member selector do not round trip")
        collision_bits = int(len(bucket) == 2)
        member_bits = (len(templates[i].members) - 1).bit_length()
        assignments.append({"raw_index": raw_index,
                            "raw_hex": f"{raw:016X}",
                            "template_id": i,
                            "slot_binary": format(calc, f"0{mapping['output_bits']}b"),
                            "collision_selector": collision_selector,
                            "collision_selector_bits": collision_bits,
                            "member_selector": member_selector,
                            "member_selector_bits": member_bits,
                            "total_prefix_id_bits": mapping["output_bits"]
                                                    + collision_bits + member_bits})
    # Make one unseen value that matches a broad template, and verify that
    # exact member checking rejects it rather than silently misdecoding it.
    known = set(keys)
    unknown_escape_checked = False
    for template in templates:
        if template.wildcards == 0:
            continue
        free = [n for n in range(16) if not ((template.mask >> (4 * n)) & 0xF)]
        for digit in range(16):
            candidate = (template.value | (digit << (4 * free[0])))
            if candidate not in known and matches(template, candidate):
                if classify_raw(candidate, keys, templates) is not None:
                    raise AssertionError("Unknown wildcard match did not escape")
                unknown_escape_checked = True
                break
        if unknown_escape_checked:
            break

    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / "rle_pruned.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=["merged_index", "raw_hex",
                                                  "run_count", "periodic_motif_nibbles",
                                                  "motif_hex", "exception_index",
                                                  "exception_value_hex", "encoded_bits",
                                                  "reason"])
        writer.writeheader()
        writer.writerows(removed)
    with (OUT / "templates.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["template_id", "members_raw_hex", "member_count",
                         "template_hex", "template_binary_X63_to_X0",
                         "template_wildcard_nibbles", "hash_mask_hex",
                         "masked_feature_hex", "slot_binary", "slot_occupancy"])
        slot_counts = Counter(slots)
        for i, template in enumerate(templates):
            writer.writerow([i, ";".join(f"{keys[j]:016X}" for j in template.members),
                             len(template.members), hex_template(template),
                             binary_template(template), template.wildcards,
                             f"{masks[i]:016X}", f"{features[i]:016X}",
                             format(slots[i], f"0{mapping['output_bits']}b"),
                             slot_counts[slots[i]]])
    with (OUT / "template_rle_audit.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["template_id", "template_hex", "member_raw_hex",
                         "nibble_run_count", "singleton_runs", "full_run_rle_bits",
                         "best_motif_nibbles", "best_motif_hex",
                         "exception_nibbles", "hypothetical_multi_exception_bits",
                         "one_exception_rle_eligible"])
        for i, template in enumerate(templates):
            for member in template.members:
                raw = keys[member]
                period, motif, exceptions, bits = best_residual_motif(raw)
                if encode_periodic(raw) is not None:
                    raise AssertionError("Eligible 1/2-nibble pattern survived into template")
                run_list = runs(unpack_tags(raw))
                run_count = len(run_list)
                singleton_runs = sum(n == 1 for _, n in run_list)
                writer.writerow([i, hex_template(template), f"{raw:016X}",
                                 run_count, singleton_runs, 8 * run_count,
                                 period, motif, exceptions, bits + 2, False])
    # Wide inspection table: show each actual tapped input bit and its value,
    # then provide the evaluated reachable H and table offsets.
    row_masks = [int(value, 16) for value in mapping["rows_hex"]]
    physical_offsets = {template_id: offset for offset, template_id
                        in enumerate(physical_table_order)}
    with (OUT / "template_linear_map_full.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["template", "Max(hex)"]
                        + [f"X{bit}" for bit in range(63, -1, -1)]
                        + [f"H{bit}" for bit in range(mapping["output_bits"])]
                        + ["Reachable H", "Reachable offset"])
        for i, template in enumerate(templates):
            feature = features[i]
            expressions = []
            for output_bit, row_mask in enumerate(row_masks):
                taps = [bit for bit in range(63, -1, -1)
                        if row_mask & (1 << bit)]
                values = [(feature >> bit) & 1 for bit in taps]
                if (sum(values) & 1) != ((slots[i] >> output_bit) & 1):
                    raise AssertionError("Substituted XOR expression differs from slot")
                equation = " XOR ".join(
                    f"X{bit}={value}" for bit, value in zip(taps, values))
                expressions.append(f"H{output_bit} = {equation}")
            writer.writerow([hex_template(template), f"{masks[i]:016X}"]
                            + [(feature >> bit) & 1 for bit in range(63, -1, -1)]
                            + expressions
                            + [format(slots[i], f"0{mapping['output_bits']}b"),
                               ";".join(str(physical_offsets[template_id])
                                        for template_id in slot_groups[slots[i]])])
    with (OUT / "pattern_assignments.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(assignments[0]))
        writer.writeheader()
        writer.writerows(assignments)
    template_table_bits = sum(
        6 + 16 + 4 * (16 - t.wildcards) + 4 * t.wildcards * len(t.members)
        for t in templates)
    hash_mask_id_bits = max(1, (len(set(masks)) - 1).bit_length())
    hash_mask_table_bits = 16 * len(set(masks)) + hash_mask_id_bits * len(templates)
    address_index_width = max(1, (template_table_bits - 1).bit_length())
    occupancy_bits = slot_count * (1 if occupied_bitmap == (1 << slot_count) - 1 else 2)
    storage_estimate = {
        "raw_residual_patterns_x64": len(keys) * 64,
        "template_table_common_and_member_residuals": template_table_bits,
        "template_table_variable_entry_offsets": len(templates) * address_index_width,
        "hash_mask_dictionary_and_ids": hash_mask_table_bits,
        "linear_mapping_64bit_rows": mapping["output_bits"] * 64,
        "slot_collision_occupancy_bits": occupancy_bits,
    }
    storage_estimate["total_before_byte_padding_and_control"] = sum(
        storage_estimate[key] for key in storage_estimate
        if key != "raw_residual_patterns_x64")
    result = {
        "input_codebooks": ["results/mphf-32/codebook1.txt",
                            "results/mphf-32/codebook2.txt"],
        "merged_unique": merged_count,
        "pruning_policy": "exact or one-exception 1/2-nibble motif, or exactly two nibble runs; preserve multi-run and 4-nibble templates",
        "rle_pruned": len(removed),
        "rle_exact_periodic_pruned": sum(
            r["reason"] == "periodic_1_2_nibbles" for r in removed),
        "rle_one_exception_pruned": sum(
            r["reason"] == "periodic_1_2_nibbles_one_exception" for r in removed),
        "rle_two_run_pruned": sum(r["reason"] == "two_nibble_runs" for r in removed),
        "residual_patterns": len(keys),
        "periodic_motif_codec_roundtrip_verified": True,
        "all_pruned_modes_roundtrip_verified": True,
        "template_merges": merges,
        "template_count": len(templates),
        "template_member_histogram": dict(sorted(Counter(len(t.members)
                                                       for t in templates).items())),
        "template_wildcard_nibble_total": sum(t.wildcards for t in templates),
        "requested_example_template": hex_template(sample),
        "selected_max_extra_mask_nibbles": extra,
        "hash_mask_distinct_count": len(set(masks)),
        "additional_hash_mask_nibbles_total": sum(
            (template.mask ^ mask).bit_count() // 4
            for template, mask in zip(templates, masks)),
        "masked_feature_collision_pairs": sum(v * (v - 1) // 2
                                               for v in Counter(features).values()),
        "max_templates_per_slot": max(Counter(slots).values()),
        "colliding_slots": sum(v == 2 for v in Counter(slots).values()),
        "slot_to_template_ids": {format(slot, f"0{mapping['output_bits']}b"): group
                                 for slot, group in slot_groups.items()},
        "physical_table_order_by_slot": physical_table_order,
        "occupied_slot_bitmap_hex": hex(occupied_bitmap),
        "paired_slot_bitmap_hex": hex(paired_bitmap),
        "prefix_id_bits_by_pattern_histogram": dict(sorted(Counter(
            item["total_prefix_id_bits"] for item in assignments).items())),
        "mean_prefix_id_bits_over_known_patterns": (sum(
            item["total_prefix_id_bits"] for item in assignments) / len(assignments)),
        "storage_estimate_bits": storage_estimate,
        "map": mapping,
        "search_attempts": attempts,
        "known_pattern_template_match_ambiguities": 0,
        "unknown_wildcard_match_raw_escape_verified": unknown_escape_checked,
        "roundtrip_feature_slot_and_member_verified": True,
    }
    (OUT / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ("map", "search_attempts")}, indent=2))
    print(json.dumps({"map_output_bits": mapping["output_bits"],
                      "map_xor2": mapping["xor2"],
                      "colliding_slots": result["colliding_slots"],
                      "attempts": attempts}, indent=2))


if __name__ == "__main__":
    main()
