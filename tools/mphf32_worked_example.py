#!/usr/bin/env python3
"""Reproduce the 7-byte two-level displacement MPHF for the 32-entry codebooks.

Reads the adopted parameters (seeds + displacements) from the final candidate
JSON files, recomputes every key's bucket/base/id with the exact mixer used by
tools/search_mphf128.cpp (MPHF_MIX_FOLD=1, MPHF_MIX_MULTIPLY=1), verifies the
mapping is a bijection onto 0..31, and emits a markdown walkthrough showing the
concrete seeds, displacement table, packed byte layout and per-key computation.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

MASK64 = (1 << 64) - 1
ADD_CONST = 0x9E3779B97F4A7C15
MUL_CONST = 0xBF58476D1CE4E5B9


def rotl(x: int, r: int) -> int:
    return ((x << r) | (x >> (64 - r))) & MASK64


def mix_steps(x: int) -> list[tuple[str, int]]:
    """The adopted mixer, one entry per operation, inputs included."""
    steps = [("input", x)]
    x = (x + ADD_CONST) & MASK64
    steps.append(("x += 0x9e3779b97f4a7c15", x))
    x ^= x >> 32
    steps.append(("x ^= x >> 32", x))
    x ^= rotl(x, 17)
    steps.append(("x ^= rotl(x, 17)", x))
    x ^= x >> 29
    steps.append(("x ^= x >> 29", x))
    x ^= rotl(x, 41)
    steps.append(("x ^= rotl(x, 41)", x))
    x ^= x >> 23
    steps.append(("x ^= x >> 23", x))
    x = (x * MUL_CONST) & MASK64
    steps.append(("x *= 0xbf58476d1ce4e5b9", x))
    x ^= x >> 27
    steps.append(("x ^= x >> 27", x))
    return steps


def mix(x: int) -> int:
    return mix_steps(x)[-1][1]


def mix_steps_nomul(x: int) -> list[tuple[str, int]]:
    """The multiplication-free mixer of REPORT.md 结果一 (FOLD=0)."""
    steps = [("input", x)]
    x = (x + ADD_CONST) & MASK64
    steps.append(("x += 0x9e3779b97f4a7c15", x))
    x ^= rotl(x, 17)
    steps.append(("x ^= rotl(x, 17)", x))
    x ^= x >> 29
    steps.append(("x ^= x >> 29", x))
    x ^= rotl(x, 41)
    steps.append(("x ^= rotl(x, 41)", x))
    return steps


def mix_nomul(x: int) -> int:
    return mix_steps_nomul(x)[-1][1]


def load_keys(path: Path) -> list[int]:
    raw = path.read_bytes()
    assert len(raw) % 8 == 0
    return [int.from_bytes(raw[i:i + 8], "big") for i in range(0, len(raw), 8)]


def packed_displacement(displacement: list[int]) -> int:
    value = 0
    for i, d in enumerate(displacement):
        value |= d << (5 * i)
    return value


def render(codebook: str, keys: list[int], seeds: tuple[int, int],
           displacement: list[int]) -> str:
    seed1, seed2 = seeds
    lines: list[str] = []
    lines.append(f"### {codebook}：7B 参数与逐 key 计算过程\n")
    lines.append(f"参数：`seed1 = {seed1}`（0x{seed1:02x}），"
                 f"`seed2 = {seed2}`（0x{seed2:02x}），B = 8。\n")
    lines.append("displacement 表（bucket 0..7）：\n")
    lines.append("```text")
    lines.append("bucket : " + " ".join(f"{b:>4d}" for b in range(8)))
    lines.append("disp   : " + " ".join(f"{d:>4d}" for d in displacement))
    lines.append("```\n")

    d_packed = packed_displacement(displacement)
    param_bytes = bytes([seed1, seed2]) + d_packed.to_bytes(5, "little")
    lines.append("7B packed 参数布局（displacement 按 5-bit 低位在前打包）：\n")
    lines.append("```text")
    lines.append("byte  0    1    2    3    4    5    6")
    lines.append("      " + " ".join(f"0x{b:02x}" for b in param_bytes))
    lines.append("      s1   s2   |---- 8 x 5-bit displacement ----|")
    lines.append("```\n")

    # Pick a representative key: first key that lands in a bucket with a
    # non-zero displacement, so the displacement add is visible.
    def locate(key: int) -> tuple[int, int, int]:
        x1 = mix(key ^ seed1)
        bucket = x1 & 7
        x2 = mix(key ^ seed2)
        base = x2 & 31
        return bucket, base, (base + displacement[bucket]) & 31

    rep = next(k for k in keys if displacement[locate(k)[0]] != 0)
    lines.append(f"代表 key `{rep:016x}` 的完整计算过程（每行一个 mixer 操作）：\n")
    lines.append("```text")
    lines.append(f"key ^ seed1        = 0x{rep ^ seed1:016x}")
    for name, value in mix_steps(rep ^ seed1):
        lines.append(f"{name:<24} = 0x{value:016x}")
    bucket = mix(rep ^ seed1) & 7
    lines.append(f"bucket = x1 & 7    = {bucket}")
    lines.append("")
    lines.append(f"key ^ seed2        = 0x{rep ^ seed2:016x}")
    for name, value in mix_steps(rep ^ seed2):
        lines.append(f"{name:<24} = 0x{value:016x}")
    base = mix(rep ^ seed2) & 31
    lines.append(f"base   = x2 & 31   = {base}")
    d = displacement[bucket]
    lines.append(f"d      = disp[{bucket}]  = {d}")
    lines.append(f"id     = (base + d) & 31 = {(base + d) & 31}")
    lines.append("```\n")

    lines.append("全部 32 个 key 的映射（`x1 = mix(key^seed1)`，"
                 "`x2 = mix(key^seed2)`）：\n")
    lines.append("```text")
    lines.append(f"{'key':<18}{'x1 (hex)':<20}{'bk':>3} {'base':>4} {'d':>3} "
                 f"{'id':>3}")
    ids = []
    for key in keys:
        x1 = mix(key ^ seed1)
        x2 = mix(key ^ seed2)
        bucket = x1 & 7
        base = x2 & 31
        d = displacement[bucket]
        entry_id = (base + d) & 31
        ids.append(entry_id)
        lines.append(f"{key:016x}  {x1:016x}  {bucket:>2d} {base:>4d} {d:>3d} "
                     f"{entry_id:>3d}")
    lines.append("```\n")
    assert sorted(ids) == list(range(32)), "mapping is not a bijection"
    lines.append("32 个 id 恰好是 `0..31` 的一个排列（脚本已断言验证），因此零冲突。\n")
    return "\n".join(lines)


def render_nomul64(codebook: str, keys: list[int], seed1: int, seed2: int,
                   displacement: list[int]) -> str:
    """Walkthrough for the multiplication-free mixer with 64-bit seeds."""
    lines: list[str] = []
    lines.append(f"### {codebook}：21B 参数与逐 key 计算过程（无乘法 mixer）\n")
    lines.append(f"参数：`seed1 = {seed1}`（0x{seed1:016x}），"
                 f"`seed2 = {seed2}`（0x{seed2:016x}），B = 8。mixer 为结果一的"
                 "四步无乘法版本（加法、两次 rotate 自异或、一次移位自异或）。\n")
    lines.append("displacement 表（bucket 0..7）：\n")
    lines.append("```text")
    lines.append("bucket : " + " ".join(f"{b:>4d}" for b in range(8)))
    lines.append("disp   : " + " ".join(f"{d:>4d}" for d in displacement))
    lines.append("```\n")

    d_packed = packed_displacement(displacement)
    param_bytes = (seed1.to_bytes(8, "big") + seed2.to_bytes(8, "big")
                   + d_packed.to_bytes(5, "little"))
    lines.append("21B packed 参数布局（seed 大端；displacement 按 5-bit 低位在"
                 "前打包）：\n")
    lines.append("```text")
    lines.append("byte  " + " ".join(f"{i:>2d}" for i in range(21)))
    lines.append("      " + " ".join(f"0x{b:02x}" for b in param_bytes))
    lines.append("      |---- seed1 (8B) ----||---- seed2 (8B) ----| |-- "
                 "8 x 5-bit disp --|")
    lines.append("```\n")

    def locate(key: int) -> tuple[int, int, int]:
        bucket = mix_nomul(key ^ seed1) & 7
        base = mix_nomul(key ^ seed2) & 31
        return bucket, base, (base + displacement[bucket]) & 31

    rep = next(k for k in keys if displacement[locate(k)[0]] != 0)
    lines.append(f"代表 key `{rep:016x}` 的完整计算过程（每行一个 mixer 操作）：\n")
    lines.append("```text")
    lines.append(f"key ^ seed1        = 0x{rep ^ seed1:016x}")
    for name, value in mix_steps_nomul(rep ^ seed1):
        lines.append(f"{name:<24} = 0x{value:016x}")
    bucket = mix_nomul(rep ^ seed1) & 7
    lines.append(f"bucket = x1 & 7    = {bucket}")
    lines.append("")
    lines.append(f"key ^ seed2        = 0x{rep ^ seed2:016x}")
    for name, value in mix_steps_nomul(rep ^ seed2):
        lines.append(f"{name:<24} = 0x{value:016x}")
    base = mix_nomul(rep ^ seed2) & 31
    lines.append(f"base   = x2 & 31   = {base}")
    d = displacement[bucket]
    lines.append(f"d      = disp[{bucket}]  = {d}")
    lines.append(f"id     = (base + d) & 31 = {(base + d) & 31}")
    lines.append("```\n")

    lines.append("全部 32 个 key 的映射（`x1 = mix(key^seed1)`，"
                 "`x2 = mix(key^seed2)`）：\n")
    lines.append("```text")
    lines.append(f"{'key':<18}{'x1 (hex)':<20}{'bk':>3} {'base':>4} {'d':>3} "
                 f"{'id':>3}")
    ids = []
    for key in keys:
        x1 = mix_nomul(key ^ seed1)
        x2 = mix_nomul(key ^ seed2)
        bucket = x1 & 7
        base = x2 & 31
        d = displacement[bucket]
        entry_id = (base + d) & 31
        ids.append(entry_id)
        lines.append(f"{key:016x}  {x1:016x}  {bucket:>2d} {base:>4d} {d:>3d} "
                     f"{entry_id:>3d}")
    lines.append("```\n")
    assert sorted(ids) == list(range(32)), "mapping is not a bijection"
    lines.append("32 个 id 恰好是 `0..31` 的一个排列（脚本已断言验证），因此零冲突。\n")
    return "\n".join(lines)


def render_appendix(candidates: Path) -> str:
    codebook = "codebook1"
    keys = load_keys(candidates / f"{codebook}.bin")
    data = json.loads((candidates / f"{codebook}-search64.json").read_text())
    row = next(r for r in data["two_level_displacement"]
               if r["bucket_count"] == 8 and r["success"])
    return render_nomul64(codebook, keys, row["seed1"], row["seed2"],
                          row["displacements"])


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--candidates", type=Path,
                    default=Path("results/mphf-32/candidates"))
    ap.add_argument("--out", type=Path)
    ap.add_argument("--appendix", action="store_true",
                    help="render the codebook1 multiplication-free 64-bit-seed "
                         "walkthrough (REPORT.md appendix) instead of the 7B "
                         "multiply-mixer sections")
    args = ap.parse_args()

    if args.appendix:
        text = render_appendix(args.candidates)
        if args.out:
            args.out.write_text(text)
            print(f"wrote {args.out}")
        else:
            print(text)
        return

    sections = []
    for codebook in ("codebook1", "codebook2"):
        keys = load_keys(args.candidates / f"{codebook}.bin")
        data = json.loads((args.candidates / f"{codebook}-final-seed8.json").read_text())
        row = next(r for r in data["two_level_displacement"]
                   if r["bucket_count"] == 8 and r["success"])
        sections.append(render(codebook, keys, (row["seed1"], row["seed2"]),
                               row["displacements"]))
    text = "\n".join(sections)
    if args.out:
        args.out.write_text(text)
        print(f"wrote {args.out}")
    else:
        print(text)


if __name__ == "__main__":
    main()
