#!/usr/bin/env python3
"""Region-local reversible transforms followed by crossing-first Var-BSEL."""
from __future__ import annotations
import argparse,csv,itertools,sys,time
from collections import Counter
from dataclasses import replace
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import experiment_fpc_payload_lz as pp

def delta8(data):return data[:1]+bytes((data[i]-data[i-1])&255 for i in range(1,len(data)))
def xor8(data):return data[:1]+bytes(data[i]^data[i-1] for i in range(1,len(data)))
def word_transform(data,op):
 out=bytearray();previous=0
 for pos in range(0,len(data),8):
  chunk=data[pos:pos+8]
  if len(chunk)<8:out+=chunk;break
  value=int.from_bytes(chunk,'little')
  coded=value if pos==0 else op(value,previous)&((1<<64)-1)
  out+=coded.to_bytes(8,'little');previous=value
 return bytes(out)
TRANSFORMS={
 'identity':lambda x:x,'delta8':delta8,'xor8':xor8,
 'delta64':lambda x:word_transform(x,lambda a,b:a-b),
 'xor64':lambda x:word_transform(x,lambda a,b:a^b)}

def pool_for(payloads,args):
 counts=Counter()
 for payload in payloads:
  for atom,n in pp.canon_windows(payload,vb.LENGTHS).items():
   gain=max(0,len(atom)-(1+vb.rank(atom)))
   if gain:counts[atom]+=n*gain
 pool,_=vb.candidates(counts,args.dictionary_size,args.candidate_cap)
 return pool[:args.candidate_cap]

def bank_bytes(combo):return 5+sum(vb.table_bytes(x.pattern) for x in combo) if combo else 0

def evaluate(r,payloads,combo,args):
 actual=[];changed=[]
 for i,payload in enumerate(payloads):
  new=1+pp.encode(payload,combo)
  if new<r.sizes[i]:actual.append(new);changed.append(i)
  else:actual.append(r.sizes[i])
 if changed:actual[changed[0]]+=args.mode_bytes
 physical=sum(vb.quantize(x) for x in actual);cost=bank_bytes(combo)
 slack=max(0,physical-sum(actual));spill=max(0,cost-slack)
 return physical+((spill+63)//64)*64

def search(r,payloads,args):
 pool=pool_for(payloads,args);best=(r.total,(),0);evaluations=0
 for count in range(1,min(args.max_entries,len(pool))+1):
  for combo in itertools.combinations(pool,count):
   if bank_bytes(combo)>args.model_budget:continue
   size=evaluate(r,payloads,combo,args);evaluations+=1
   key=(size>r.target,size,bank_bytes(combo))
   old=(best[0]>r.target,best[0],bank_bytes(best[1]))
   if key<old:best=(size,combo,evaluations)
 return best[0],best[1],evaluations

def main():
 ap=argparse.ArgumentParser();ap.add_argument('payloads',type=Path);ap.add_argument('--name',required=True)
 ap.add_argument('--limit-mib',type=int,default=1);ap.add_argument('--max-gap',type=int,default=256)
 ap.add_argument('--model-budget',type=int,default=128);ap.add_argument('--max-entries',type=int,default=2)
 ap.add_argument('--candidate-cap',type=int,default=8);ap.add_argument('--dictionary-size',type=int,default=31)
 ap.add_argument('--mode-bytes',type=int,default=2);ap.add_argument('--output-dir',type=Path,required=True)
 args=ap.parse_args();args.output_dir.mkdir(parents=True,exist_ok=True)
 limit=args.limit_mib*256 if args.limit_mib else 10**9
 rs=pp.regions(pp.read_payloads(args.payloads,limit));rows=[];start=time.perf_counter()
 for r in rs:
  if not (0<r.gap<=args.max_gap):continue
  best=(r.total,'baseline',(),0)
  for name,fn in TRANSFORMS.items():
   payloads=[fn(p) for p in r.payloads];size,combo,ev=search(r,payloads,args)
   candidate=(size,name,combo,ev)
   if (size>r.target,size,bank_bytes(combo))<(best[0]>r.target,best[0],bank_bytes(best[2])):best=candidate
  rows.append({'region':r.i,'before_tier':r.tier,'target':r.target,'gap':r.gap,'before':r.total,
   'after':best[0],'crossed':int(best[0]<=r.target),'transform':best[1],
   'patterns':len(best[2]),'model_bytes':bank_bytes(best[2])})
 output=args.output_dir/f'{args.name}-transforms.csv'
 with output.open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)
 print(f'regions={len(rs)} eligible={len(rows)} seconds={time.perf_counter()-start:.3f}')
 for tier,target in ((4096,3072),(3072,2048),(2048,1024)):
  selected=[x for x in rows if x['before_tier']==tier]
  print(f'tier={tier} eligible={len(selected)} crossed={sum(x["crossed"] for x in selected)} '
   f'transforms={Counter(x["transform"] for x in selected if x["crossed"])}')
if __name__=='__main__':main()
