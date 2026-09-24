#!/usr/bin/env python3
"""针对 32 个互不相同的 uint64 关键字、带预算约束的稀疏 GF(2) 完美映射搜索。

这里只需代码本（codebook）这条路径即可。stdout 输出单个 JSON 结果；进度信息
写入 stderr。所谓「最优」仅涉及相互独立的输出 XOR 树，与电路级优化无关。
"""
import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
import re
import sys
import time


def check_time(deadline):
    """功能：协作式时间限制检查，一旦超过 `deadline` 就抛出 TimeoutError。

    输入：
      - deadline：单调时钟（monotonic clock，time.monotonic 的返回值）代表的截止时刻。
    输出/返回：无返回值；若当前时间已超过截止时刻则抛出 TimeoutError 异常。
    """
    if time.monotonic() >= deadline:
        raise TimeoutError


def describe(keys, masks):
    """功能：在代码本上评估一组 XOR 行 `masks`，并给出其描述信息。

    将每个 mask 转换为 32 位 signature（第 i 位 = 在 32 个关键字上
    key[i] & mask 的奇偶性），校验这些 signature 是否构成到 0..nkeys-1 的双射，
    并报告 tap 数量、每行的表达式、贪心共享 XOR 成本估计以及生成的 32 个 ID。

    输入：
      keys  -- n 个互不相同的整数（即代码本 patterns）。
      masks -- 若干 int 输入 mask 组成的列表，每个 mask 对应一行输出。
    输出/返回：返回一个 dict，包含 rows/bits/expressions/row_taps/taps/
      independent_xors/ids 以及贪心共享门（shared-gate）估计值；若并非双射则抛出
      ValueError 异常。
    """
    bits = [[j for j in range(64) if m >> j & 1] for m in masks]
    ids = [sum(((x&m).bit_count() % 2) << j for j,m in enumerate(masks))
           for x in keys]
    if sorted(ids) != list(range(len(keys))):
        raise ValueError('matrix is not perfect on the supplied codebook')
    outputs = [set(row) for row in bits]
    gates = []
    while True:
        counts = {}
        for row in outputs:
            for pair in itertools.combinations(sorted(row), 2):
                counts[pair] = counts.get(pair, 0)+1
        if not counts or max(counts.values()) < 2:
            break
        pair = min(counts, key=lambda p: (-counts[p], p))
        wire = 64+len(gates)
        gates.append(list(pair))
        for row in outputs:
            if set(pair) <= row:
                row.difference_update(pair)
                row.add(wire)
    taps = sum(map(len, bits))
    return dict(rows=[f'0x{m:016x}' for m in masks], bits=bits,
                expressions=[' XOR '.join(f'X{j}' for j in row) for row in bits],
                row_taps=list(map(len, bits)), taps=taps,
                independent_xors=taps-len(masks), ids=ids,
                shared_greedy=dict(xors=len(gates)+sum(len(r)-1 for r in outputs),
                                   gates=gates, outputs=[sorted(r) for r in outputs]))


def last_row(differences, width, cap, deadline):
    """功能：通过 syndrome 的 meet-in-the-middle 方式求最小权重的 Dm=1，无需矩阵求逆。

    每一半最多有 ceil(cap/2) 个 tap。相同的 syndrome 只保留代价最小的表示；
    两半之间重叠的部分会相互抵消，这只会让权重变得更小。

    输入：
      differences -- 输入关键字之间的差异值列表。
      width       -- 列的宽度（即输入列的数目）。
      cap         -- 允许的最大 tap 权重。
      deadline    -- 单调时钟截止时刻；超时会抛出 TimeoutError。
    输出/返回：返回权重最小的行 mask（满足 <= cap），若找不到则返回 None。
    """
    columns = [sum(((d >> j)&1) << i for i,d in enumerate(differences))
               for j in range(width)]
    table = {0: 0}
    for w in range(1, (cap+1)//2+1):
        for count, positions in enumerate(itertools.combinations(range(width), w)):
            if count % 256 == 0:
                check_time(deadline)
            syndrome = mask = 0
            for j in positions:
                syndrome ^= columns[j]
                mask |= 1 << j
            table.setdefault(syndrome, mask)
    target = (1 << len(differences))-1
    best = None
    for count, (s,m) in enumerate(table.items()):
        if count % 256 == 0:
            check_time(deadline)
        other = table.get(s ^ target)
        if other is not None:
            candidate = m ^ other
            if candidate.bit_count() <= cap and (best is None or
                    candidate.bit_count() < best.bit_count()):
                best = candidate
    return best


def optimize(keys, candidates, deadline, incumbent=None, last='scan', cap=4):
    """功能：在固定的候选池（candidate pool）上做分支定界（branch-and-bound）DFS。

    在平衡分组切分约束下探索一棵 DFS 树，以找到低 tap 的 5 行（或更少）映射。

    输入：
      keys       -- n 个代码本 patterns。
      candidates -- (weight, signature, mask) 元组组成的列表，已按序排好。
      deadline   -- 单调时钟截止时刻；超时会抛出 TimeoutError。
      incumbent  -- 可选的既有最佳结果，用于初始化上界 U。
      last       -- 最后一行采用的策略，取 'scan' 或 'mitm'。
      cap        -- 池中已经生成的每行最大 tap 权重。
    输出/返回：返回 dict，包含 best、complete（候选池是否已被完整搜索）、nodes，
      以及搜索过程中发现的 tap 计数改进序列 improvements。
    """
    k = (len(keys)-1).bit_length()
    pool = sorted(c for c in candidates if c[1].bit_count()*2 == len(keys))
    best = incumbent
    nodes = 0
    improvements = []

    def save(rows):
        nonlocal best
        result = describe(keys, rows)
        if best is None or result['taps'] < best['taps']:
            best = result
            improvements.append(result['taps'])

    def dfs(pool, groups, chosen, cost):
        nonlocal nodes
        nodes += 1
        check_time(deadline)
        remaining = k-len(chosen)
        upper = best['taps'] if best else math.inf
        if len(pool) < remaining or cost+sum(c[0] for c in pool[:remaining]) >= upper:
            return
        if remaining == 1 and last == 'mitm':
            differences = []
            for group in groups:
                indices = [i for i in range(len(keys)) if group >> i & 1]
                differences.append(keys[indices[0]] ^ keys[indices[1]])
            budget = min(cap, int(upper-cost-1) if math.isfinite(upper) else cap)
            if budget > 0:
                mask = last_row(differences, 64, budget, deadline)
                if mask is not None:
                    save(chosen+[mask])
            return
        for j, (w,s,m) in enumerate(pool):
            if j % 64 == 0:
                check_time(deadline)
            upper = best['taps'] if best else math.inf
            if len(pool)-j < remaining:
                break
            if cost+sum(c[0] for c in pool[j:j+remaining]) >= upper:
                break
            halves = []
            for g in groups:
                a = g&s
                if a.bit_count()*2 != g.bit_count():
                    break
                halves.extend((a,g^a))
            else:
                if remaining == 1:
                    save(chosen+[m])
                    continue
                nxt = []
                for index in range(j+1, len(pool)):
                    if index % 256 == 0:
                        check_time(deadline)
                    d = pool[index]
                    # d 可能是「最后」被选中的那一行。不要将其权重计入所有剩余行的成本
                    # （这是一处历史遗留的有风险剪枝 bug）。
                    if cost+w+d[0]+w*(remaining-2) >= upper:
                        break
                    if all((g&d[1]).bit_count()*2 == g.bit_count() for g in halves):
                        nxt.append(d)
                if len(nxt) >= remaining-1:
                    dfs(nxt, halves, chosen+[m], cost+w)
    try:
        check_time(deadline)
        dfs(pool, [(1 << len(keys))-1], [], 0)
        complete = True
    except TimeoutError:
        complete = False
    return dict(best=best, complete=complete, nodes=nodes, improvements=improvements)


def run(keys, seconds=60, max_candidates=250000, last='scan', progress=None):
    """功能：顶层自动搜索——逐步扩展 cap 层级、搜索并报告结果。

    按权重（1..）增量生成候选 mask，保留 balanced signature，并在每个已完成
    的候选池上运行分支定界 DFS，从而不断压低 incumbent。

    输入：
      keys           -- 32 个互不相同的 64 位关键字。
      seconds        -- 以秒计的墙钟（wall-clock）时间预算。
      max_candidates -- 存储候选的上限。
      last           -- 最后一行采用的方法。
      progress       -- 可选进度回调，接收每个阶段的 stage dict。

    输出/返回：返回结果 dict，包含 status、best、stages 列表以及完成性证明相关信息。
    """
    if len(keys) != 32 or len(set(keys)) != 32 or any(
            not isinstance(x,int) or x < 0 or x >= 1 << 64 for x in keys):
        raise ValueError('expected exactly 32 distinct unsigned 64-bit keys')
    if not math.isfinite(seconds) or seconds < 0 or max_candidates < 1:
        raise ValueError('seconds must be finite and nonnegative; candidate limit positive')
    if last not in ('scan', 'mitm'):
        raise ValueError('last must be scan or mitm')
    begin = time.monotonic()
    deadline = begin+seconds
    columns = [sum(((x >> j)&1) << i for i,x in enumerate(keys)) for j in range(64)]
    candidates = {}
    best = None
    stages = []
    proved_cap = 0
    generated_cap = 0
    status = 'time_limit'
    enumerated = 0
    try:
        for cap in range(1, 65):
            pre = time.monotonic()
            for count, positions in enumerate(itertools.combinations(range(64), cap)):
                if count % 256 == 0:
                    check_time(deadline)
                s = mask = 0
                for j in positions:
                    s ^= columns[j]
                    mask |= 1 << j
                enumerated += 1
                if s.bit_count() == 16 and s not in candidates:
                    if len(candidates) >= max_candidates:
                        status = 'candidate_limit'
                        raise OverflowError
                    candidates[s] = (cap,s,mask)
            generated_cap = cap
            generation_seconds = time.monotonic()-pre
            # 廉价的行会一起生成，因此从不对完整的 cap=1/2 层级运行 DFS。
            if cap < 3:
                continue
            stage_start = time.monotonic()
            # 先做一次简短稀疏探测，再按几何级数切片。为后续扩展预留时间。
            slice_seconds = min(0.5 if cap == 3 else 12*2**(cap-4),
                                max(0, deadline-stage_start))
            r = optimize(keys, list(candidates.values()), stage_start+slice_seconds,
                         best, last, cap)
            best = r['best']
            if r['complete']:
                proved_cap = cap
            stage = dict(cap=cap, candidates=len(candidates),
                         generation_seconds=generation_seconds,
                         search_seconds=time.monotonic()-stage_start,
                         complete=r['complete'], nodes=r['nodes'],
                         improvements=r['improvements'],
                         best_taps=best['taps'] if best else None)
            stages.append(stage)
            if progress:
                progress(stage)
            if best:
                # 每一行 balanced row 的代价至少为 wmin，这一点已通过对所有更小权重做穷举验证。
                # 任何改进方案的最大行（max row）权重都不会超过该 bound。
                wmin = min(c[0] for c in candidates.values())
                needed_cap = best['taps']-1-4*wmin
                if best['taps'] == 5 or (r['complete'] and cap >= needed_cap):
                    status = 'optimal'
                    break
        else:
            status = 'optimal' if best else 'no_linear_map'
    except (TimeoutError, OverflowError):
        pass
    return dict(status=status, best=best, elapsed_seconds=time.monotonic()-begin,
                generated_cap=generated_cap, proved_cap=proved_cap,
                proof_scope='independent XOR trees; exhaustive completed candidate layers only',
                candidates=len(candidates), enumerated_masks=enumerated, stages=stages,
                last_row_method=last)


def main():
    """功能：命令行入口——读取代码本文件并把 JSON 结果打印到 stdout。

    输入：来自 sys.argv 参数（--seconds / --max-candidates / --last-row），以及
      代码本文件内容（每行形如 "| 16 位十六进制数字 |" 的数据行）。
    输出/返回：在 stdout 上打印 JSON 报告（各阶段的进度写入 stderr）。任务有效时（哪怕
      超时）以状态码 0 退出；任何输入错误均以状态码 2 退出。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('codebook', type=Path)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--max-candidates', type=int, default=250000)
    parser.add_argument('--last-row', choices=['scan','mitm'], default='scan')
    args = parser.parse_args()
    try:
        raw = args.codebook.read_bytes()
        keys = []
        for line in raw.decode().splitlines():
            if not line.strip():
                continue
            match = re.fullmatch(r'\s*\|\s*([0-9a-fA-F]{16})\s*\|\s*', line)
            if not match:
                raise ValueError('each nonempty line must be | 16 hexadecimal digits |')
            keys.append(int(match[1], 16))
        result = run(keys, args.seconds, args.max_candidates, args.last_row,
                     lambda stage: print(json.dumps(stage), file=sys.stderr, flush=True))
        result.update(codebook=str(args.codebook.resolve()),
                      sha256=hashlib.sha256(raw).hexdigest())
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, UnicodeError) as error:
        parser.exit(2, f'error: {error}\n')


if __name__ == '__main__':
    main()
