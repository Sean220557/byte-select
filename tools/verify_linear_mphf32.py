#!/usr/bin/env python3
"""Verify the hand-written 5x64 GF(2) maps for both 32-entry codebooks."""
from pathlib import Path

ROWS = {
    "codebook1": ([14, 23, 63], [8, 12, 18, 27], [2, 6, 9, 29],
                  [9, 15, 19, 26], [0, 11, 25, 47]),
    "codebook2": ([22, 43, 48], [5, 45, 63], [0, 8, 51, 61],
                  [3, 10, 32, 44], [1, 16, 22, 29]),
}


def read_keys(name: str) -> list[int]:
    path = Path(__file__).parents[1] / "results" / "mphf-32" / f"{name}.txt"
    return [int(line.split("|")[1].strip(), 16)
            for line in path.read_text().splitlines() if "|" in line]


for name, rows in ROWS.items():
    keys = read_keys(name)
    ids = []
    for key in keys:
        value = 0
        for r, row in enumerate(rows):
            parity = sum((key >> bit) & 1 for bit in row) & 1
            value |= parity << r
        ids.append(value)
    assert len(keys) == 32
    assert len(set(ids)) == 32 and sorted(ids) == list(range(32))
    print(name)
    print("  rows:", rows)
    print("  weights:", [len(row) for row in rows],
          "taps:", sum(map(len, rows)),
          "2-input XORs:", sum(len(row) - 1 for row in rows))
    print("  ids:", ids)
    print("  verified: permutation 0..31")
