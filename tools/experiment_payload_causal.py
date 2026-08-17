#!/usr/bin/env python3
"""Causal FPC-payload adaptation using only previously decoded test regions."""
from __future__ import annotations
import argparse,csv,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import experiment_fpc_payload_lz as pp

def feat(r):return tuple(x/64 for x in r.qvec)+(r.gap/64,)
def distance(a,b):return sum((x-y)**2 for x,y in zip(a,b))
def load_fixed(path):
 m=json.loads(path.read_text(encoding='utf-8'));out={}
 for tier,entries in m['tiers'].items():
  out[int(tier)]=[(tuple(e['center']),tuple(vb.Entry(tuple(p),0) for p in e['patterns'])) for e in entries]
 return out,m['config']['max_gap']
def fallback(r,fixed):
 es=fixed.get(r.tier,[])
 return min(es,key=lambda x:distance(feat(r),x[0]))[1] if es else ()

def main():
 ap=argparse.ArgumentParser();ap.add_argument('payloads',type=Path);ap.add_argument('fixed_model',type=Path)
 ap.add_argument('--name',required=True);ap.add_argument('--history',type=int,default=16)
 ap.add_argument('--min-history',type=int,default=2);ap.add_argument('--length-count',type=int,default=6)
 ap.add_argument('--model-budget',type=int,default=128);ap.add_argument('--max-entries',type=int,default=4)
 ap.add_argument('--candidate-cap',type=int,default=12);ap.add_argument('--dictionary-size',type=int,default=31)
 ap.add_argument('--mode-bytes',type=int,default=2);ap.add_argument('--output',type=Path,required=True);args=ap.parse_args()
 fixed,max_gap=load_fixed(args.fixed_model);rs=pp.regions(pp.read_payloads(args.payloads,10**9))
 # Match the argument names expected by pp.bank.
 args.max_gap=max_gap
 after={};sources={};bank_cache={}
 for pos,r in enumerate(rs):
  if not (0<r.gap<=max_gap):after[r.i]=r.total;sources[r.i]='baseline';continue
  prior=[x for x in rs[max(0,pos-args.history):pos] if x.tier==r.tier and 0<x.gap<=max_gap]
  if len(prior)>=args.min_history:
   key=tuple(x.i for x in prior)
   if key not in bank_cache:bank_cache[key]=pp.bank(prior,True,args)[0]
   causal_bank=bank_cache[key];fixed_bank=fallback(r,fixed)
   causal_result=pp.evalr(r,causal_bank,args.mode_bytes) if causal_bank else (r.total,0)
   fixed_result=pp.evalr(r,fixed_bank,args.mode_bytes) if fixed_bank else (r.total,0)
   if causal_result[0]<fixed_result[0]:bank=causal_bank;source='causal';result=causal_result
   else:bank=fixed_bank;source='fallback';result=fixed_result
  else:
   bank=fallback(r,fixed);source='fallback';result=pp.evalr(r,bank,args.mode_bytes) if bank else (r.total,0)
  after[r.i]=result[0];sources[r.i]=source
 rows=[]
 for r in rs:rows.append({'region':r.i,'before':r.total,'before_tier':r.tier,'gap':r.gap,
  'source':sources[r.i],'after':after[r.i],'after_tier':vb.tier(after[r.i])})
 args.output.parent.mkdir(parents=True,exist_ok=True)
 with args.output.open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)
 for tier,target in ((4096,3072),(3072,2048),(2048,1024)):
  es=[r for r in rs if r.tier==tier and 0<r.gap<=max_gap]
  print(f'tier={tier} eligible={len(es)} crossed={sum(after[r.i]<=target for r in es)} '
        f'before={sum(r.total for r in es)} after={sum(after[r.i] for r in es)} '
        f'causal={sum(sources[r.i]=="causal" for r in es)} fallback={sum(sources[r.i]=="fallback" for r in es)}')
if __name__=='__main__':main()
