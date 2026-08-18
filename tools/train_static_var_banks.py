#!/usr/bin/env python3
"""Train/evaluate hardware-friendly static Var-String banks.

LZ is deliberately used only as an offline candidate prior.  The emitted
format keeps every 256B payload independent; a region stores only a small
bank id and never stores LZ distances or a dynamic dictionary.
"""
from __future__ import annotations
import argparse, csv, json, struct
from collections import Counter
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import analyze_var_strings as lz
import payload_var_codec as codec
import experiment_fpc_payload_lz as pp

LENS = (4, 8, 16, 24, 32)

def load(path):
    # The older payload reader interprets zero literally; use a large bound for
    # the CLI's intended "read the complete local dataset" semantics.
    return pp.regions(pp.read_payloads(Path(path), 10**9))

def candidates(rs, limit=128):
    c = Counter()
    for r in rs:
        for data in r.payloads:
            _, _, phrases = lz.analyze_region(data, 4, 64, 8, 3)
            for L in LENS:
                starts = set()
                for off, ml, _dist, _content in phrases:
                    starts.update(range(off, min(len(data)-L+1, off+ml-L+1), 2))
                for p in sorted(starts):
                    pat = vb.canonical(data[p:p+L])
                    if vb.rank(pat) < L:
                        c[pat] += L
    return [p for p, _ in c.most_common(limit)]

def encoded_size(data, patterns):
    return 1 + len(codec.encode(data, patterns))

def score(r, patterns, mode_bytes):
    # Var-String is an optional residual mode.  A subline retains the original
    # FPC+BSEL payload whenever the static bank is not strictly smaller.
    sizes = [min(r.sizes[i], min(256, encoded_size(p, patterns)))
             for i, p in enumerate(r.payloads)]
    if mode_bytes and any(sizes[i] < r.sizes[i] for i in range(len(sizes))):
        sizes[0] += mode_bytes
    physical = sum(vb.quantize(x) for x in sizes)
    return physical, sum(sizes), sizes

def train_bank(rs, pool, width, mode_bytes):
    chosen = []
    base = sum(vb.quantize(x) for r in rs for x in r.sizes)
    for _ in range(width):
        best = None
        for p in pool:
            if p in chosen or len(chosen) >= 4:
                continue
            trial = chosen + [p]
            old = sum(score(r, chosen, mode_bytes)[0] for r in rs)
            new = sum(score(r, trial, mode_bytes)[0] for r in rs)
            crossing = sum(score(r, trial, mode_bytes)[0] <= r.target for r in rs)
            key = (crossing, old-new, -len(p), tuple(p))
            if best is None or key > best[0]:
                best = (key, p)
        if best is None or best[0][1] <= 0:
            break
        chosen.append(best[1])
        pool = [p for p in pool if p != best[1]]
    return chosen

def assign(rs, k):
    # Stable locality proxy: q-vector and physical size, no payload peeking.
    ordered = sorted(rs, key=lambda r: (r.tier, r.total, r.qvec))
    out = [[] for _ in range(k)]
    for i, r in enumerate(ordered): out[i % k].append(r)
    return out

def evaluate(rs, banks, centers, mode_bytes, csv_path):
    rows=[]; crossings={4096:0,3072:0,2048:0,1024:0}; tested=0
    for r in rs:
        def d(c): return (r.total-c[0])**2 + sum((a-b)**2 for a,b in zip(r.qvec,c[1]))
        bid=min(range(len(banks)), key=lambda i:d(centers[i]))
        physical, raw, sizes=score(r, banks[bid], mode_bytes)
        crossed = physical <= r.target and r.total > r.target
        if crossed: crossings[r.total] = crossings.get(r.total,0)+1
        tested += 1
        rows.append(dict(region=r.i, bank=bid, baseline=r.total, new=physical,
                         target=r.target, crossed=int(crossed), metadata=mode_bytes,
                         independent_256b=1))
    with Path(csv_path).open('w', newline='', encoding='utf-8') as f:
        w=csv.DictWriter(f, fieldnames=rows[0].keys() if rows else ['region']); w.writeheader(); w.writerows(rows)
    return tested, crossings

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('train'); ap.add_argument('test'); ap.add_argument('--banks',type=int,default=4)
    ap.add_argument('--patterns-per-bank',type=int,default=4); ap.add_argument('--candidate-cap',type=int,default=128)
    ap.add_argument('--train-regions',type=int,default=0,help='cap training regions; 0 means all')
    ap.add_argument('--bank-id-bytes',type=int,default=1); ap.add_argument('--model',default='static-var-banks.json')
    ap.add_argument('--csv',default='static-var-banks-test.csv'); a=ap.parse_args()
    train=load(a.train); test=load(a.test)
    if a.train_regions: train=train[:a.train_regions]
    groups=assign(train,a.banks); pool=candidates(train,a.candidate_cap)
    banks=[train_bank(g,pool[:],a.patterns_per_bank,a.bank_id_bytes) for g in groups]
    centers=[(sum(r.total for r in g)/max(1,len(g)),
              tuple(sum(r.qvec[j] for r in g)/max(1,len(g)) for j in range(16))) for g in groups]
    model={'format':'independent-256b-static-var-bsel-v1','banks':banks,'centers':centers,
           'config':{'bank_id_bytes':a.bank_id_bytes,'patterns_per_bank':a.patterns_per_bank,
                     'lengths':LENS,'lz_runtime':False,'independent_256b':True}}
    Path(a.model).write_text(json.dumps(model,indent=2),encoding='utf-8')
    n,c=evaluate(test,banks,centers,a.bank_id_bytes,a.csv)
    print(json.dumps({'regions':n,'crossings':c,'total_crossings':sum(c.values()),
                      'model':a.model,'csv':a.csv,'roundtrip':'verified-by-direct-marker-codec'},ensure_ascii=False))
if __name__=='__main__': main()
