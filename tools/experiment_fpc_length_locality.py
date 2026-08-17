#!/usr/bin/env python3
"""Complete FPC-length-locality experiment for segment-shared LZ Var-BSEL."""

from __future__ import annotations

import argparse, csv, json, math, statistics, sys, time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import analyze_var_strings as lzprobe


@dataclass
class R:
    i: int; data: bytes; sizes: list[int]; qvec: tuple[int, ...]
    total: int; tier: int; target: int; gap: int; phrases: list


def pearson(xs, ys):
    if len(xs) < 2: return 0.0
    mx, my = statistics.mean(xs), statistics.mean(ys)
    num = sum((x-mx)*(y-my) for x,y in zip(xs,ys))
    den = math.sqrt(sum((x-mx)**2 for x in xs)*sum((y-my)**2 for y in ys))
    return num/den if den else 0.0


def load(path, sizes_path, mib):
    data = path.read_bytes()[:mib*1024*1024]
    data = data[:len(data)//vb.REGION*vb.REGION]
    sizes = vb.read_sizes(sizes_path, len(data)//vb.SUBLINE)
    out=[]
    for i,b in enumerate(range(0,len(data),vb.REGION)):
        lane=sizes[i*16:i*16+16]; q=tuple(vb.quantize(x) for x in lane); total=sum(q)
        t=vb.tier(total); target={4096:3072,3072:2048,2048:1024,1024:1024}[t]
        _,_,phr=lzprobe.analyze_region(data[b:b+vb.REGION],4,64,8,3)
        out.append(R(i,data[b:b+vb.REGION],lane,q,total,t,target,max(0,total-target),phr))
    return out


def locality(rs):
    totals=[r.total for r in rs]
    corr={d:pearson(totals[:-d],totals[d:]) for d in (1,2,4,8,16) if d<len(rs)}
    runs=[]; start=0
    for i in range(1,len(rs)+1):
        if i==len(rs) or rs[i].tier!=rs[start].tier:
            runs.append(i-start); start=i
    near=[r for r in rs if 0<r.gap<=256]
    distances=[b.i-a.i for a,b in zip(near,near[1:])]
    return corr,runs,distances


def fixed_segments(rs,n): return [rs[i:i+n] for i in range(0,len(rs),n)]


def adaptive_segments(rs,total_thr,vec_thr):
    segs=[]; cur=[rs[0]]
    for r in rs[1:]:
        p=cur[-1]; l1=sum(abs(a-b) for a,b in zip(p.qvec,r.qvec))
        if r.tier==p.tier and abs(r.total-p.total)<=total_thr and l1<=vec_thr:
            cur.append(r)
        else: segs.append(cur); cur=[r]
    segs.append(cur); return segs


def weighted_atoms(r, use_lz, length_count):
    lengths=(vb.lz_selected_lengths(r.data,r.sizes,r.phrases,length_count,"tier")
             if use_lz else vb.LENGTHS)
    atoms,_=vb.scan_atoms(r.data,lengths)
    out=Counter()
    urgency=1+256//max(64,r.gap) if r.gap else 1
    for atom,count in atoms.items():
        gain=max(0,len(atom)-(1+vb.rank(atom)))
        if gain: out[atom]+=count*gain*urgency
    return out,len(atoms),sum(atoms.values()),lengths


def bank_for(segment, cache, use_lz, args):
    counts=Counter(); distinct=occ=0; lengths=Counter()
    # Learn only from FPC failures, but include all failure tiers in a segment;
    # evaluation below still reports each transition separately.
    train=[r for r in segment if 0<r.gap<=args.max_gap]
    for r in train:
        key=(r.i,use_lz)
        if key not in cache: cache[key]=weighted_atoms(r,use_lz,args.length_count)
        atoms,d,o,ls=cache[key]; counts.update(atoms); distinct+=d; occ+=o; lengths.update(ls)
    if not counts:return (),0,distinct,occ,lengths
    pool,_=vb.candidates(counts,args.dictionary_size,args.candidate_cap)
    chosen=[]; used=0
    eligible=[r for r in segment if 0<r.gap<=args.max_gap]
    def objective(bank):
        results=[shared_eval(r,tuple(bank),args.mode_bytes) for r in eligible]
        crossed=sum(physical<=r.target for r,(physical,_,_) in zip(eligible,results))
        return (-crossed,sum(x[0] for x in results),sum(x[2] for x in results))
    current=objective(chosen)
    remaining=pool[:]
    while remaining and len(chosen)<args.max_entries:
        best=None
        for e in remaining:
            cost=vb.table_bytes(e.pattern)
            if used+cost>args.model_budget: continue
            obj=objective(chosen+[e])
            if best is None or obj<best[0]: best=(obj,e,cost)
        if best is None or best[0]>=current: break
        current=best[0]; chosen.append(best[1]); used+=best[2]; remaining.remove(best[1])
    return tuple(chosen),used,distinct,occ,lengths


def shared_eval(r,bank,mode_bytes):
    actual=[]; changed=[]
    for lane,b in enumerate(range(0,vb.REGION,vb.SUBLINE)):
        var=vb.encode_lane(r.data[b:b+vb.SUBLINE],bank)
        if var<r.sizes[lane]: actual.append(var); changed.append(lane)
        else: actual.append(r.sizes[lane])
    if changed and mode_bytes: actual[changed[0]]+=mode_bytes
    return sum(vb.quantize(x) for x in actual),len(changed),sum(actual)


def run_method(name,segs,rs,use_lz,args,cache):
    after={r.i:r.total for r in rs}; used_lanes={r.i:0 for r in rs}
    model=atoms=occ=0; banks=0; seg_rows=[]
    for si,seg in enumerate(segs):
        bank,b,d,o,ls=bank_for(seg,cache,use_lz,args)
        if bank: banks+=1; model+=b; atoms+=d; occ+=o
        eligible=[r for r in seg if 0<r.gap<=args.max_gap]
        for r in eligible:
            after[r.i],used_lanes[r.i],_=shared_eval(r,bank,args.mode_bytes)
        seg_rows.append((si,seg[0].i,seg[-1].i,len(seg),len(eligible),len(bank),b))
    summary=[]
    for before_tier,target in ((4096,3072),(3072,2048),(2048,1024)):
        elig=[r for r in rs if r.tier==before_tier and 0<r.gap<=args.max_gap]
        crossed=sum(after[r.i]<=target for r in elig)
        summary.append({"method":name,"before_tier":before_tier,"target":target,
                        "eligible":len(elig),"crossed":crossed,
                        "cross_rate":crossed/len(elig) if elig else 0,
                        "before_bytes":sum(r.total for r in elig),
                        "after_bytes":sum(after[r.i] for r in elig),
                        "model_bytes":model,"banks":banks,"distinct_atoms":atoms,
                        "atom_occurrences":occ})
    return summary,after,used_lanes,seg_rows


def main():
    ap=argparse.ArgumentParser(); ap.add_argument("input",type=Path); ap.add_argument("sizes",type=Path)
    ap.add_argument("--name",required=True); ap.add_argument("--limit-mib",type=int,default=1)
    ap.add_argument("--max-gap",type=int,default=256); ap.add_argument("--length-count",type=int,default=6)
    ap.add_argument("--model-budget",type=int,default=128); ap.add_argument("--max-entries",type=int,default=8)
    ap.add_argument("--candidate-cap",type=int,default=32); ap.add_argument("--dictionary-size",type=int,default=31)
    ap.add_argument("--mode-bytes",type=int,default=2); ap.add_argument("--total-threshold",type=int,default=256)
    ap.add_argument("--vector-threshold",type=int,default=512); ap.add_argument("--output-dir",type=Path,required=True)
    ap.add_argument("--schemes",default="fixed4,fixed8,fixed16,adaptive,adaptive-no-lz")
    args=ap.parse_args(); args.output_dir.mkdir(parents=True,exist_ok=True)
    t=time.perf_counter(); rs=load(args.input,args.sizes,args.limit_mib); corr,runs,dists=locality(rs)
    schemes={"fixed4":fixed_segments(rs,4),"fixed8":fixed_segments(rs,8),
             "fixed16":fixed_segments(rs,16),
             "adaptive":adaptive_segments(rs,args.total_threshold,args.vector_threshold)}
    cache={}; summaries=[]; detail=[]; segment_dump={}
    # LZ prior for every segmentation; no-LZ is run on adaptive segments as the
    # direct search-framework control under identical table budgets.
    requested=[x.strip() for x in args.schemes.split(",") if x.strip()]
    jobs=[]
    for name in requested:
        if name=="adaptive-no-lz": jobs.append((name,schemes["adaptive"],False))
        elif name in schemes: jobs.append((name,schemes[name],True))
        else: raise ValueError(f"unknown scheme: {name}")
    for name,segs,use_lz in jobs:
        print(f"running={name} segments={len(segs)}",flush=True)
        s,a,u,srows=run_method(name,segs,rs,use_lz,args,cache); summaries+=s
        segment_dump[name]=srows
        for r in rs:
            detail.append({"dataset":args.name,"method":name,"region":r.i,"before":r.total,
                           "before_tier":r.tier,"target":r.target,"gap":r.gap,
                           "eligible":int(0<r.gap<=args.max_gap),"after":a[r.i],
                           "after_tier":vb.tier(a[r.i]),"used_lanes":u[r.i]})
    with (args.output_dir/f"{args.name}-summary.csv").open("w",newline="") as f:
        w=csv.DictWriter(f,fieldnames=summaries[0].keys());w.writeheader();w.writerows(summaries)
    with (args.output_dir/f"{args.name}-regions.csv").open("w",newline="") as f:
        w=csv.DictWriter(f,fieldnames=detail[0].keys());w.writeheader();w.writerows(detail)
    meta={"dataset":args.name,"regions":len(rs),"lag_correlation":corr,
          "tier_run_lengths":runs,"near_hard_distances":dists,
          "segments":segment_dump,"elapsed_seconds":time.perf_counter()-t,
          "config":vars(args)|{"input":str(args.input),"sizes":str(args.sizes),"output_dir":str(args.output_dir)}}
    (args.output_dir/f"{args.name}-locality.json").write_text(json.dumps(meta,indent=2),encoding="utf-8")
    for row in summaries: print(row)

if __name__=="__main__": main()
