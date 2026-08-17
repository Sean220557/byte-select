#!/usr/bin/env python3
"""LZ-seeded Var-BSEL over the actual payload emitted by latest FPC-BSEL."""
from __future__ import annotations
import argparse,csv,json,struct,sys,time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import analyze_var_strings as lz

@dataclass
class R:
 i:int; payloads:list[bytes]; sizes:list[int]; qvec:tuple[int,...]; total:int; tier:int; target:int; gap:int

def read_payloads(path,limit_regions):
 d=path.read_bytes()
 if d[:8]!=b'FPCPAY1\0': raise ValueError('bad payload file')
 n=struct.unpack_from('<Q',d,8)[0]; p=16; out=[]
 for _ in range(min(n,limit_regions*16)):
  size=struct.unpack_from('<H',d,p)[0];p+=2;out.append(d[p:p+size]);p+=size
 return out

def regions(payloads):
 out=[]
 for i in range(0,len(payloads),16):
  ps=payloads[i:i+16]
  if len(ps)<16:break
  sizes=[min(256,1+len(x)) for x in ps];q=tuple(vb.quantize(x) for x in sizes);total=sum(q);t=vb.tier(total)
  target={4096:3072,3072:2048,2048:1024,1024:1024}[t]
  out.append(R(i//16,ps,sizes,q,total,t,target,max(0,total-target)))
 return out

def segments(rs,total_thr,vec_thr):
 out=[];cur=[rs[0]]
 for r in rs[1:]:
  p=cur[-1];dist=sum(abs(a-b) for a,b in zip(p.qvec,r.qvec))
  if r.tier==p.tier and abs(r.total-p.total)<=total_thr and dist<=vec_thr:cur.append(r)
  else:out.append(cur);cur=[r]
 out.append(cur);return out

def canon_windows(data,lengths):
 c=Counter()
 for L in lengths:
  for pos in range(0,len(data)-L+1):c[vb.canonical(data[pos:pos+L])]+=L
 return c

def lz_lengths(segment,count):
 stream=b''.join(p for r in segment for p in r.payloads)
 _,_,phr=lz.analyze_region(stream,4,64,8,3)
 support=vb.lz_length_support(phr)
 return tuple(x for x,_ in sorted(support.items(),key=lambda x:(-x[1]*x[0],x[0]))[:count]),len(phr)

def encode(data,bank):
 groups={}
 for e in bank:groups.setdefault(len(e.pattern),[]).append(e.pattern)
 # Decodable direct-marker format for at most four entries. Bytes fb..fe are
 # direct pattern IDs (one byte, then rank dictionary); ff escapes a literal
 # in the reserved fb..ff range. The five marker values are model metadata.
 markers=(0xfb,0xfc,0xfd,0xfe);escape=0xff;reserved=set(markers+(escape,))
 cost=[0]*(len(data)+1)
 for pos in range(len(data)-1,-1,-1):
  references=[]
  for L,patterns in groups.items():
   if pos+L>len(data):continue
   atom=vb.canonical(data[pos:pos+L])
   for pattern in patterns:
    if vb.less_equal(atom,pattern):references.append((L,vb.rank(pattern)))
  best=1+(1 if data[pos] in reserved else 0)+cost[pos+1]
  for L,r in references:best=min(best,1+r+cost[pos+L])
  cost[pos]=best
 return cost[0]

def evalr(r,bank,mode_bytes):
 actual=[];changed=[]
 for i,p in enumerate(r.payloads):
  new=1+encode(p,bank)
  if new<r.sizes[i]:actual.append(new);changed.append(i)
  else:actual.append(r.sizes[i])
 if changed and mode_bytes:actual[changed[0]]+=mode_bytes
 return sum(vb.quantize(x) for x in actual),sum(actual)

def bank(segment,use_lz,args):
 elig=[r for r in segment if 0<r.gap<=args.max_gap]
 if not elig:return (),0,0,0,()
 lengths,phr=(lz_lengths(segment,args.length_count) if use_lz else (vb.LENGTHS,0))
 counts=Counter();wins=0
 for r in segment:
  for p in r.payloads:
   a=canon_windows(p,lengths);counts.update(a);wins+=sum(max(0,len(p)-L+1) for L in lengths)
 weighted=Counter()
 for atom,n in counts.items():
  gain=max(0,len(atom)-(1+vb.rank(atom)))
  if gain:weighted[atom]=n*gain
 pool,_=vb.candidates(weighted,args.dictionary_size,args.candidate_cap)
 chosen=[];used=0
 def obj(xs):
  rr=[evalr(r,tuple(xs),args.mode_bytes) for r in elig]
  return (-sum(x[0]<=r.target for r,x in zip(elig,rr)),sum(x[0] for x in rr),sum(x[1] for x in rr))
 cur=obj(chosen);remaining=pool[:]
 while remaining and len(chosen)<args.max_entries:
  best=None
  for e in remaining:
   b=vb.table_bytes(e.pattern)
   if used+b>args.model_budget-5:continue
   o=obj(chosen+[e])
   if best is None or o<best[0]:best=(o,e,b)
  if best is None or best[0]>=cur:break
  cur=best[0];chosen.append(best[1]);used+=best[2];remaining.remove(best[1])
 if chosen:used+=5
 return tuple(chosen),used,wins,len(weighted),lengths

def run(name,segs,rs,use_lz,args):
 after={r.i:r.total for r in rs};model=windows=atoms=banks=0;detail=[]
 for si,s in enumerate(segs):
  bk,b,w,a,ls=bank(s,use_lz,args);windows+=w;atoms+=a
  eligible=[r for r in s if 0<r.gap<=args.max_gap]
  trial={r.i:evalr(r,bk,args.mode_bytes)[0] for r in eligible}
  # The adaptive table is transmitted once per segment. Enable it only when
  # physical payload savings exceed its complete serialized model bytes.
  if bk and sum(r.total-trial[r.i] for r in eligible)>b:
   model+=b;banks+=1
   for r in eligible:after[r.i]=trial[r.i]
  else:bk=();b=0
  detail.append({'segment':si,'begin':s[0].i,'end':s[-1].i,'regions':len(s),
                 'patterns':[list(e.pattern) for e in bk],'model_bytes':b,
                 'lengths':list(ls)})
 rows=[]
 for t,target in ((4096,3072),(3072,2048),(2048,1024)):
  es=[r for r in rs if r.tier==t and 0<r.gap<=args.max_gap]
  rows.append({'method':name,'before_tier':t,'target':target,'eligible':len(es),
   'crossed':sum(after[r.i]<=target for r in es),'before_bytes':sum(r.total for r in es),
   'after_bytes':sum(after[r.i] for r in es),'model_bytes':model,'banks':banks,
   'searched_windows':windows,'distinct_atoms':atoms})
 return rows,after,detail

def main():
 ap=argparse.ArgumentParser();ap.add_argument('payloads',type=Path);ap.add_argument('--name',required=True)
 ap.add_argument('--limit-mib',type=int,default=1);ap.add_argument('--max-gap',type=int,default=256)
 ap.add_argument('--length-count',type=int,default=6);ap.add_argument('--model-budget',type=int,default=128)
 ap.add_argument('--max-entries',type=int,default=4);ap.add_argument('--candidate-cap',type=int,default=12)
 ap.add_argument('--dictionary-size',type=int,default=31);ap.add_argument('--mode-bytes',type=int,default=2)
 ap.add_argument('--total-threshold',type=int,default=256);ap.add_argument('--vector-threshold',type=int,default=512)
 ap.add_argument('--output-dir',type=Path,required=True);args=ap.parse_args();args.output_dir.mkdir(parents=True,exist_ok=True)
 limit=args.limit_mib*256 if args.limit_mib else 10**9
 rs=regions(read_payloads(args.payloads,limit));segs=segments(rs,args.total_threshold,args.vector_threshold)
 summaries=[];details=[]
 for name,use in [('payload-lz',True),('payload-no-lz',False)]:
  print('running',name,flush=True);rows,a,d=run(name,segs,rs,use,args);summaries+=rows
  (args.output_dir/f'{args.name}-{name}.adaptive.json').write_text(
      json.dumps({'method':name,'mode_bytes':args.mode_bytes,'segments':d},indent=2),encoding='utf-8')
  for r in rs:details.append({'method':name,'region':r.i,'before':r.total,'tier':r.tier,'gap':r.gap,'after':a[r.i],'after_tier':vb.tier(a[r.i])})
 with (args.output_dir/f'{args.name}-summary.csv').open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=summaries[0].keys());w.writeheader();w.writerows(summaries)
 with (args.output_dir/f'{args.name}-regions.csv').open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=details[0].keys());w.writeheader();w.writerows(details)
 print('segments',len(segs));[print(x) for x in summaries]
if __name__=='__main__':main()
