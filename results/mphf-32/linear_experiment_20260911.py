"""Exploratory experiment (not a production library): balanced GF(2) signatures and equivalent row minimization."""
import argparse, hashlib, itertools, json, time, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BASE = [
    [[14,23,63],[8,12,18,27],[2,6,9,29],[9,15,19,26],[0,11,25,47]],
    [[22,43,48],[5,45,63],[0,8,51,61],[3,10,32,44],[1,16,22,29]],
]

def rank(values):
    basis = {}
    for v in values:
        while v:
            p = v.bit_length()-1
            if p not in basis:
                basis[p] = v
                break
            v ^= basis[p]
    return len(basis)

def prepare(n, cap=4):
    keys = [int(x,16) for x in __import__('re').findall(r'\|\s*([0-9a-fA-F]{16})\s*\|', (ROOT/f'codebook{n}.txt').read_text())]
    assert len(keys) == len(set(keys)) == 32
    cols = [sum(((k>>j)&1)<<i for i,k in enumerate(keys)) for j in range(64)]
    shortest = {0: 0}
    counts = []
    for w in range(1,cap+1):
        for ix in itertools.combinations(range(64),w):
            y = 0; mask = 0
            for j in ix:
                y ^= cols[j]; mask |= 1<<j
            shortest.setdefault(y,mask)
        counts.append(sum(m.bit_count()==w and s.bit_count()==16 for s,m in shortest.items()))
    base = [sum(1<<i for i in r) for r in BASE[n-1]]
    sig = lambda m: sum(((k&m).bit_count()%2)<<i for i,k in enumerate(keys))
    combos = []
    for a in range(1,32):
        m = 0
        for j in range(5):
            if a>>j&1: m ^= base[j]
        y = sig(m)
        best = shortest.get(y, m)
        combos.append((best.bit_count(), a, best))
    chosen=[]
    for w,a,m in sorted(combos):
        if rank([v[1] for v in chosen]+[a])>len(chosen): chosen.append((w,a,m))
    print(json.dumps({'codebook':n,'max_row_weight':cap,'balanced_counts_by_min_weight':counts,
                      'input_sha256':hashlib.sha256((ROOT/f'codebook{n}.txt').read_bytes()).hexdigest(),
                      'equivalent_basis':[(w,a,hex(m)) for w,a,m in chosen],
                      'taps':sum(w for w,_,_ in chosen)}), flush=True)
    return keys, shortest

def circuit(masks):
    outputs=[set(j for j in range(64) if m>>j&1) for m in masks]
    gates=[]
    while True:
        counts={}
        for row in outputs:
            for pair in itertools.combinations(sorted(row),2): counts[pair]=counts.get(pair,0)+1
        if not counts or max(counts.values())<2: break
        pair=min(counts,key=lambda p:(-counts[p],p))
        idx=64+len(gates); gates.append(pair)
        for row in outputs:
            if set(pair)<=row:row.difference_update(pair);row.add(idx)
    return {'xor_count':len(gates)+sum(len(s)-1 for s in outputs),
            'shared_gates':gates,'outputs':[sorted(s) for s in outputs]}

def search(keys, shortest, bound, seconds, xor_bound=None):
    candidates = sorted((m.bit_count(),s,m) for s,m in shortest.items() if s.bit_count()==16)
    deadline=time.monotonic()+seconds
    nodes=0
    def dfs(pool, groups, chosen, cost):
        nonlocal nodes
        nodes+=1
        if time.monotonic()>deadline: raise TimeoutError
        if len(chosen)==5: return chosen
        for j,c in enumerate(pool):
            if j%64==0 and time.monotonic()>deadline: raise TimeoutError
            w,s,m=c
            if cost+w*(5-len(chosen))>bound: break
            halves=[]
            for g in groups:
                a=g&s
                if a.bit_count()*2!=g.bit_count(): break
                halves.extend((a,g^a))
            else:
                if len(chosen)==4:
                    sol=chosen+[c]
                    if xor_bound is None or circuit([m for w,s,m in sol])['xor_count']<=xor_bound:return sol
                    continue
                nxt=[]
                for d in pool[j+1:]:
                    # d may be selected LAST, not necessarily next. Other
                    # remaining rows can still have weight w. The stronger
                    # 'all future rows >= d.weight' bound is valid only in
                    # the outer loop when d is actually chosen next.
                    if cost+w+d[0]+w*(3-len(chosen))>bound: break
                    if all((g&d[1]).bit_count()*2==g.bit_count() for g in halves): nxt.append(d)
                if len(nxt)>=4-len(chosen):
                    ans=dfs(nxt,halves,chosen+[c],cost+w)
                    if ans:return ans
        return None
    begin=time.monotonic()
    try:
        answer=dfs(candidates,[0xffffffff],[],0); status='found' if answer else 'exhausted'
    except TimeoutError: answer=None;status='timeout'
    result={'status':status,'bound':bound,'nodes':nodes,'seconds':time.monotonic()-begin}
    if answer:
        masks=[m for w,s,m in answer]
        ids=[sum(((k&m).bit_count()%2)<<r for r,m in enumerate(masks)) for k in keys]
        assert sorted(ids)==list(range(32))
        result.update(rows=[hex(m) for m in masks],taps=sum(m.bit_count() for m in masks),ids=ids,circuit=circuit(masks))
    return result

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--codebook',type=int,choices=(1,2),required=True)
    ap.add_argument('--cap',type=int,choices=(1,2,3,4,5),default=4)
    ap.add_argument('--budgets',type=int,nargs='+',required=True)
    ap.add_argument('--seconds',type=float,default=45)
    ap.add_argument('--xor-bound',type=int)
    args=ap.parse_args()
    start=time.monotonic()
    keys,pool=prepare(args.codebook,args.cap)
    print(json.dumps({'precompute_seconds':time.monotonic()-start}),flush=True)
    for budget in args.budgets:
        result=search(keys,pool,budget,args.seconds,args.xor_bound)
        print(json.dumps({'codebook':args.codebook,'max_row_weight':args.cap,
                          'xor_bound':args.xor_bound,**result}),flush=True)
