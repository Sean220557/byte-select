#!/usr/bin/env python3
"""Crossing-first static Var-BSEL banks with 256B-independent rescue mode."""
from __future__ import annotations
import argparse, csv, json, sys
from collections import Counter
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import analyze_var_strings as lz
import experiment_fpc_payload_lz as pp
import payload_var_codec as codec

LENS=(4,8,16,24,32)
TIER_NAME={4096:'4K->3K',3072:'3K->2K',2048:'2K->1K',1024:'already<=1K'}

def load(p): return pp.regions(pp.read_payloads(Path(p),10**9))

def lz_pool(rs,cap):
 c=Counter()
 for r in rs:
  # Only inspect lanes that are capable of shedding a 64B allocation segment.
  for i,data in enumerate(r.payloads):
   if r.sizes[i] in (64,128,192,256): continue
   _,_,phr=lz.analyze_region(data,4,64,8,3)
   for L in LENS:
    starts=set()
    for off,ml,_d,_x in phr:
     starts.update(range(off,min(len(data)-L+1,off+ml-L+1),2))
    for pos in starts:
     p=vb.canonical(data[pos:pos+L])
     gain=L-(1+vb.rank(p))
     if gain>0:c[p]+=gain*L
 return [p for p,_ in c.most_common(cap)]

def apply(r,patterns,meta,verify=False):
 sizes=list(r.sizes); rescued=[]
 for i,data in enumerate(r.payloads):
  stream=codec.encode(data,patterns)
  if verify and codec.decode(stream,patterns,len(data))!=data: raise ValueError('roundtrip mismatch')
  new=1+len(stream)
  if vb.quantize(new)<vb.quantize(sizes[i]): sizes[i]=new; rescued.append(i)
 if rescued and meta:sizes[rescued[0]]+=meta
 physical=sum(vb.quantize(x) for x in sizes)
 return physical,sum(sizes),rescued

def objective(rs,pats,meta):
 vals=[apply(r,pats,meta) for r in rs]
 crossed=sum(v[0]<=r.target for r,v in zip(rs,vals))
 seg_saved=sum((r.total-v[0])//64 for r,v in zip(rs,vals))
 raw=sum(v[1] for v in vals)
 return crossed,seg_saved,-raw

def train(rs,cap,width,meta):
 pool=lz_pool(rs,cap); chosen=[]; cur=objective(rs,chosen,meta)
 for _ in range(min(width,4)):
  best=None
  for p in pool:
   if p in chosen:continue
   val=objective(rs,chosen+[p],meta)
   key=(val[0]-cur[0],val[1]-cur[1],val[2]-cur[2],-len(p))
   if best is None or key>best[0]:best=(key,p,val)
  if best is None or best[0][:3]<=(0,0,0):break
  chosen.append(best[1]);cur=best[2]
 return chosen

def groups(rs,max_gap):
 eligible=[r for r in rs if r.tier>1024 and 0<r.gap<=max_gap]
 return [(name,[r for r in eligible if r.tier==tier])
         for tier,name in ((4096,'4K'),(3072,'3K'),(2048,'2K'))]

def evaluate(rs,banks,meta,out_csv):
 counts=Counter();rows=[];near=0;changed=0;physical_saved=0
 for r in rs:
  options=[(-1,r.total,sum(r.sizes),[])]
  for bid,pats in enumerate(banks):
   ph,raw,rescue=apply(r,pats,meta,verify=True);options.append((bid,ph,raw,rescue))
  bid,ph,raw,rescue=min(options,key=lambda x:(x[1],x[2],x[0]))
  crossed=r.tier>1024 and r.total>r.target and ph<=r.target
  if 0<r.gap<=256:near+=1
  if ph<r.total:changed+=1;physical_saved+=r.total-ph
  if crossed:counts[TIER_NAME[r.tier]]+=1
  rows.append(dict(region=r.i,tier=TIER_NAME[r.tier],gap=r.gap,bank=bid,
   baseline=r.total,new=ph,physical_saved=r.total-ph,rescued_sublines=';'.join(map(str,rescue)),crossed=int(crossed)))
 with Path(out_csv).open('w',newline='',encoding='utf-8') as f:
  w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)
 return dict(regions=len(rs),eligible_gap_le_256=near,changed_regions=changed,
  physical_saved=physical_saved,crossings={k:counts[k] for k in ('4K->3K','3K->2K','2K->1K')})

def main():
 ap=argparse.ArgumentParser();ap.add_argument('train');ap.add_argument('test')
 ap.add_argument('--max-gap',type=int,default=256);ap.add_argument('--train-regions',type=int,default=0)
 ap.add_argument('--candidate-cap',type=int,default=32);ap.add_argument('--patterns-per-bank',type=int,default=4)
 ap.add_argument('--metadata-bytes',type=int,default=1);ap.add_argument('--model',default='boundary-banks.json')
 ap.add_argument('--csv',default='boundary-banks.csv');a=ap.parse_args()
 tr=load(a.train)
 if a.train_regions:tr=tr[:a.train_regions]
 gs=groups(tr,a.max_gap);banks=[];labels=[]
 for label,g in gs:
  if g:banks.append(train(g,a.candidate_cap,a.patterns_per_bank,a.metadata_bytes));labels.append(label)
 if not banks:banks=[train(tr,a.candidate_cap,a.patterns_per_bank,a.metadata_bytes)];labels=['fallback']
 model={'format':'boundary-rescue-static-var-bsel-v1','labels':labels,'banks':banks,
  'config':{'independent_256b':True,'runtime_lz':False,'oracle_encoder':True,'metadata_bytes':a.metadata_bytes}}
 Path(a.model).write_text(json.dumps(model,indent=2),encoding='utf-8')
 result=evaluate(load(a.test),banks,a.metadata_bytes,a.csv);result.update(model=a.model,csv=a.csv,roundtrip='ok')
 print(json.dumps(result,ensure_ascii=False))
if __name__=='__main__':main()
