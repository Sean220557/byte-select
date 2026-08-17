#!/usr/bin/env python3
"""Leakage-free train/test pipeline for FPC-payload LZ Var-BSEL patches."""
from __future__ import annotations
import argparse,csv,json,math,sys,time
from collections import Counter,defaultdict
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import analyze_var_strings as lz
import experiment_fpc_payload_lz as pp

TIERS=(4096,3072,2048)

def feature(r):
 return tuple(x/64.0 for x in r.qvec)+(r.gap/64.0,)
def dist(a,b):return sum((x-y)**2 for x,y in zip(a,b))

def kmeans(items,k,iterations=20):
 if not items:return [],{}
 k=min(k,len(items)); feats=[feature(r) for r in items]
 centers=[feats[0]]
 while len(centers)<k:
  centers.append(max(feats,key=lambda x:min(dist(x,c) for c in centers)))
 assign={}
 for _ in range(iterations):
  new={r.i:min(range(k),key=lambda j:dist(feature(r),centers[j])) for r in items}
  groups=defaultdict(list)
  for r in items:groups[new[r.i]].append(feature(r))
  ncent=[]
  for j in range(k):
   fs=groups[j]
   ncent.append(tuple(sum(x[d] for x in fs)/len(fs) for d in range(len(fs[0]))) if fs else centers[j])
  if new==assign:break
  assign=new;centers=ncent
 return centers,assign

def atom_counts(r,use_lz,length_count):
 if use_lz:
  stream=b''.join(r.payloads);_,_,phr=lz.analyze_region(stream,4,64,8,3)
  support=vb.lz_length_support(phr)
  lengths=tuple(x for x,_ in sorted(support.items(),key=lambda x:(-x[1]*x[0],x[0]))[:length_count])
 else:lengths=vb.LENGTHS
 counts=Counter();windows=0
 for p in r.payloads:
  for L in lengths:
   windows+=max(0,len(p)-L+1)
   for pos in range(0,len(p)-L+1):
    atom=vb.canonical(p[pos:pos+L]);gain=max(0,L-(1+vb.rank(atom)))
    if gain:counts[atom]+=L*gain
 return counts,windows,lengths

def train_bank(rs,use_lz,args,cache):
 counts=Counter();windows=0;lengths=Counter()
 for r in rs:
  key=(r.i,use_lz)
  if key not in cache:cache[key]=atom_counts(r,use_lz,args.length_count)
  c,w,ls=cache[key];counts.update(c);windows+=w;lengths.update(ls)
 pool,_=vb.candidates(counts,args.dictionary_size,args.candidate_cap)
 chosen=[];used=0
 def obj(xs):
  vals=[pp.evalr(r,tuple(xs),args.mode_bytes) for r in rs]
  return (-sum(v[0]<=r.target for r,v in zip(rs,vals)),sum(v[0] for v in vals),sum(v[1] for v in vals))
 cur=obj(chosen);remaining=pool[:]
 while remaining and len(chosen)<args.max_entries:
  best=None
  for e in remaining:
   cost=vb.table_bytes(e.pattern)
   if used+cost>args.bank_budget-5:continue
   o=obj(chosen+[e])
   if best is None or o<best[0]:best=(o,e,cost)
  if best is None or best[0]>=cur:break
  cur=best[0];chosen.append(best[1]);used+=best[2];remaining.remove(best[1])
 if chosen:used+=5  # four direct marker bytes plus one literal escape byte
 return tuple(chosen),used,windows,lengths

def serialize(name,use_lz,models,args,path):
 data={'name':name,'use_lz':use_lz,'config':{'max_gap':args.max_gap,'clusters':args.clusters,
  'length_count':args.length_count,'bank_budget':args.bank_budget,'max_entries':args.max_entries,
  'candidate_cap':args.candidate_cap,'mode_bytes':args.mode_bytes},'tiers':{}}
 for tier,entries in models.items():
  data['tiers'][str(tier)]=[{'center':list(center),'patterns':[list(e.pattern) for e in bank],
   'model_bytes':used,'train_regions':[r.i for r in rs]} for center,bank,used,rs in entries]
 path.write_text(json.dumps(data,indent=2),encoding='utf-8')

def evaluate(name,models,test,args):
 rows=[];detail=[]
 for r in test:
  after=r.total;bank_id=-1
  entries=models.get(r.tier,[])
  if 0<r.gap<=args.max_gap and entries:
   bank_id=min(range(len(entries)),key=lambda j:dist(feature(r),entries[j][0]))
   after,_=pp.evalr(r,entries[bank_id][1],args.mode_bytes)
  detail.append({'method':name,'region':r.i,'before':r.total,'before_tier':r.tier,'gap':r.gap,
                 'bank_id':bank_id,'after':after,'after_tier':vb.tier(after)})
 for tier,target in ((4096,3072),(3072,2048),(2048,1024)):
  es=[r for r in test if r.tier==tier and 0<r.gap<=args.max_gap]
  ds=[d for d in detail if d['before_tier']==tier and 0<d['gap']<=args.max_gap]
  rows.append({'method':name,'before_tier':tier,'target':target,'eligible':len(es),
   'crossed':sum(d['after']<=target for d in ds),'before_bytes':sum(r.total for r in es),
   'after_bytes':sum(d['after'] for d in ds),'model_bytes':sum(x[2] for xs in models.values() for x in xs),
   'banks':sum(len(x) for x in models.values())})
 return rows,detail

def run_method(name,use_lz,train,test,args,outdir):
 models={};cache={};stats=[]
 for tier in TIERS:
  eligible=[r for r in train if r.tier==tier and 0<r.gap<=args.max_gap]
  centers,assignment=kmeans(eligible,args.clusters)
  entries=[]
  for j,c in enumerate(centers):
   rs=[r for r in eligible if assignment[r.i]==j]
   bank,used,windows,lengths=train_bank(rs,use_lz,args,cache)
   entries.append((c,bank,used,rs));stats.append((tier,j,len(rs),len(bank),used,windows,dict(lengths)))
  models[tier]=entries
 serialize(name,use_lz,models,args,outdir/f'{args.name}-{name}.model.json')
 rows,detail=evaluate(name,models,test,args)
 return rows,detail,stats

def main():
 ap=argparse.ArgumentParser();ap.add_argument('train_payloads',type=Path);ap.add_argument('test_payloads',type=Path)
 ap.add_argument('--name',required=True);ap.add_argument('--train-mib',type=int,default=0);ap.add_argument('--test-mib',type=int,default=0)
 ap.add_argument('--max-gap',type=int,default=256);ap.add_argument('--clusters',type=int,default=4)
 ap.add_argument('--length-count',type=int,default=6);ap.add_argument('--bank-budget',type=int,default=128)
 ap.add_argument('--max-entries',type=int,default=4);ap.add_argument('--candidate-cap',type=int,default=12)
 ap.add_argument('--dictionary-size',type=int,default=31);ap.add_argument('--mode-bytes',type=int,default=2)
 ap.add_argument('--methods',default='lz,no-lz')
 ap.add_argument('--output-dir',type=Path,required=True);args=ap.parse_args();args.output_dir.mkdir(parents=True,exist_ok=True)
 tr_limit=args.train_mib*256 if args.train_mib else 10**9;te_limit=args.test_mib*256 if args.test_mib else 10**9
 train=pp.regions(pp.read_payloads(args.train_payloads,tr_limit));test=pp.regions(pp.read_payloads(args.test_payloads,te_limit))
 allrows=[];alldetail=[];allstats=[];start=time.perf_counter()
 methods=[x.strip() for x in args.methods.split(',') if x.strip()]
 for name,use in [('lz',True),('no-lz',False)]:
  if name not in methods:continue
  print('training',name,flush=True);rows,detail,stats=run_method(name,use,train,test,args,args.output_dir)
  allrows+=rows;alldetail+=detail;allstats += [(name,)+x for x in stats]
 with (args.output_dir/f'{args.name}-summary.csv').open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=allrows[0].keys());w.writeheader();w.writerows(allrows)
 with (args.output_dir/f'{args.name}-regions.csv').open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=alldetail[0].keys());w.writeheader();w.writerows(alldetail)
 print('train_regions',len(train),'test_regions',len(test),'seconds',time.perf_counter()-start)
 [print(x) for x in allrows]
if __name__=='__main__':main()
