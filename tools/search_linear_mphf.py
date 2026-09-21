#!/usr/bin/env python3
"""Budgeted sparse GF(2) perfect-map search for 32 distinct uint64 keys.

Only the codebook path is required. stdout is a single JSON result; progress
goes to stderr. Optimality concerns independent output XOR trees, not circuits.
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
    if time.monotonic() >= deadline:
        raise TimeoutError


def describe(keys, masks):
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
    """Minimum-weight Dm=1 via syndrome meet-in-the-middle, no matrix inverse.

    Each half has at most ceil(cap/2) taps. Equal syndromes retain the cheapest
    representation; overlapping halves cancel, which can only reduce weight.
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
    """One branch-and-bound traversal; complete is scoped to this pool only."""
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
                    # d may be the LAST selected row. Do not charge its weight
                    # for all remaining rows (historical unsafe-pruning bug).
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
            # Cheap rows are generated together, never run a full cap=1/2 DFS.
            if cap < 3:
                continue
            stage_start = time.monotonic()
            # Brief sparse probe, then geometric slices. Leave room for expansion.
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
                # Every balanced row costs at least wmin, proven by enumeration
                # of all smaller weights. Any improvement has max row <= bound.
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
