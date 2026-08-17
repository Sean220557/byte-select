#!/usr/bin/env python3
"""Crossing-first exhaustive and region-local search on FPC+BSEL payloads."""
from __future__ import annotations
import argparse,csv,itertools,sys,time
from collections import Counter
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import analyze_var_strings as lz
import experiment_fpc_payload_lz as pp

TIERS=((4096,3072),(3072,2048),(2048,1024))

def weighted_atoms(items,mode,args):
 counts=Counter();windows=0
 if mode=='lz-length':
  lengths,_=pp.lz_lengths(items,args.length_count)
  for r in items:
   for payload in r.payloads:
    atoms=pp.canon_windows(payload,lengths);counts.update(atoms)
    windows+=sum(max(0,len(payload)-n+1) for n in lengths)
 elif mode=='lz-position':
  for r in items:
   stream=b''.join(r.payloads)
   _,_,phrases=lz.analyze_region(stream,4,64,8,3)
   boundaries=[];begin=0
   for payload in r.payloads:
    boundaries.append((begin,begin+len(payload)));begin+=len(payload)
   seeded=set()
   for offset,length,_distance,_content in phrases:
    boundary=next(((left,right) for left,right in boundaries if left<=offset<right),None)
    if boundary is None:continue
    _left,right=boundary
    phrase_end=min(offset+length,right)
    for size in vb.LENGTHS:
     if offset+size<=phrase_end:
      for pos in range(offset,phrase_end-size+1):seeded.add((pos,size))
   for pos,size in seeded:counts[vb.canonical(stream[pos:pos+size])]+=size
   windows+=len(seeded)
 else:
  for r in items:
   for payload in r.payloads:
    atoms=pp.canon_windows(payload,vb.LENGTHS);counts.update(atoms)
    windows+=sum(max(0,len(payload)-n+1) for n in vb.LENGTHS)
 weighted=Counter()
 for atom,count in counts.items():
  gain=max(0,len(atom)-(1+vb.rank(atom)))
  if gain:weighted[atom]=count*gain
 pool,_=vb.candidates(weighted,args.dictionary_size,args.candidate_cap)
 return pool[:args.candidate_cap],windows,len(weighted)

def bank_bytes(combo):
 return (5+sum(vb.table_bytes(x.pattern) for x in combo)) if combo else 0

def inline_size(region,combo,args):
 physical,actual=pp.evalr(region,combo,args.mode_bytes)
 cost=bank_bytes(combo)
 slack=max(0,physical-actual)
 spill=max(0,cost-slack)
 return physical+((spill+63)//64)*64,actual,cost

def exhaustive(items,pool,args,inline=False):
 base={r.i:(r.total,sum(r.sizes),0) for r in items}
 best_combo=();best_values=base
 def objective(values,combo):
  crossed=sum(values[r.i][0]<=r.target for r in items)
  # Crossing count is primary; bytes and model cost only break ties.
  return (-crossed,sum(values[r.i][0] for r in items),
          sum(values[r.i][1] for r in items),bank_bytes(combo))
 best_key=objective(base,())
 evaluations=0
 max_entries=min(args.max_entries,len(pool))
 for count in range(1,max_entries+1):
  for combo in itertools.combinations(pool,count):
   if bank_bytes(combo)>args.model_budget:continue
   if inline:values={r.i:inline_size(r,combo,args) for r in items}
   else:values={r.i:(*pp.evalr(r,combo,args.mode_bytes),bank_bytes(combo)) for r in items}
   key=objective(values,combo);evaluations+=1
   if key<best_key:best_key=key;best_combo=combo;best_values=values
 return best_combo,best_values,evaluations

def segment_exhaustive(rs,segs,args):
 after={r.i:r.total for r in rs};actual={r.i:sum(r.sizes) for r in rs}
 model=banks=evals=windows=atoms=0
 for segment in segs:
  eligible=[r for r in segment if 0<r.gap<=args.max_gap]
  if not eligible:continue
  pool,w,a=weighted_atoms(segment,'lz-length',args);windows+=w;atoms+=a
  combo,values,n=exhaustive(eligible,pool,args);evals+=n;b=bank_bytes(combo)
  # Shared segment table is admitted only when complete physical savings pay it.
  if combo and sum(r.total-values[r.i][0] for r in eligible)>b:
   model+=b;banks+=1
   for r in eligible:after[r.i]=values[r.i][0];actual[r.i]=values[r.i][1]
 return after,{'model_bytes':model,'banks':banks,'evaluations':evals,'windows':windows,'atoms':atoms}

def region_exhaustive(rs,args,mode):
 after={r.i:r.total for r in rs};actual={r.i:sum(r.sizes) for r in rs}
 model=banks=evals=windows=atoms=0
 for r in rs:
  if not (0<r.gap<=args.max_gap):continue
  pool,w,a=weighted_atoms([r],mode,args);windows+=w;atoms+=a
  combo,values,n=exhaustive([r],pool,args,inline=True);evals+=n
  # A private table is stored inside this region. Select it only if it crosses.
  if combo and values[r.i][0]<=r.target:
   after[r.i]=values[r.i][0];actual[r.i]=values[r.i][1]
   model+=bank_bytes(combo);banks+=1
 return after,{'model_bytes':model,'banks':banks,'evaluations':evals,'windows':windows,'atoms':atoms}

def rows(name,rs,after,stats):
 out=[]
 for tier,target in TIERS:
  eligible=[r for r in rs if r.tier==tier and 0<r.gap<=256]
  out.append({'method':name,'before_tier':tier,'target':target,'eligible':len(eligible),
   'crossed':sum(after[r.i]<=target for r in eligible),
   'before_bytes':sum(r.total for r in eligible),'after_bytes':sum(after[r.i] for r in eligible),**stats})
 return out

def main():
 ap=argparse.ArgumentParser();ap.add_argument('payloads',type=Path);ap.add_argument('--name',required=True)
 ap.add_argument('--limit-mib',type=int,default=1);ap.add_argument('--max-gap',type=int,default=256)
 ap.add_argument('--length-count',type=int,default=6);ap.add_argument('--model-budget',type=int,default=128)
 ap.add_argument('--max-entries',type=int,default=4);ap.add_argument('--candidate-cap',type=int,default=10)
 ap.add_argument('--dictionary-size',type=int,default=31);ap.add_argument('--mode-bytes',type=int,default=2)
 ap.add_argument('--total-threshold',type=int,default=256);ap.add_argument('--vector-threshold',type=int,default=512)
 ap.add_argument('--output-dir',type=Path,required=True);args=ap.parse_args();args.output_dir.mkdir(parents=True,exist_ok=True)
 limit=args.limit_mib*256 if args.limit_mib else 10**9
 rs=pp.regions(pp.read_payloads(args.payloads,limit));segs=pp.segments(rs,args.total_threshold,args.vector_threshold)
 start=time.perf_counter();allrows=[];details=[]
 methods=[]
 segment,segment_stats=segment_exhaustive(rs,segs,args);methods.append(('segment-exhaustive-lz',segment,segment_stats))
 for mode in ('lz-position','lz-length','all-length'):
  result,stats=region_exhaustive(rs,args,mode);methods.append((f'region-inline-{mode}',result,stats))
 hybrid={r.i:min([segment[r.i]]+[result[r.i] for _,result,_ in methods[1:]]) for r in rs}
 methods.append(('hybrid-best',hybrid,{'model_bytes':0,'banks':0,'evaluations':0,'windows':0,'atoms':0}))
 for name,result,stats in methods:
  allrows+=rows(name,rs,result,stats)
  for r in rs:details.append({'method':name,'region':r.i,'before':r.total,'target':r.target,
   'gap':r.gap,'after':result[r.i],'crossed':int(result[r.i]<=r.target and r.gap>0)})
 with (args.output_dir/f'{args.name}-crossing-search-summary.csv').open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=allrows[0].keys());w.writeheader();w.writerows(allrows)
 with (args.output_dir/f'{args.name}-crossing-search-regions.csv').open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=details[0].keys());w.writeheader();w.writerows(details)
 print(f'regions={len(rs)} segments={len(segs)} seconds={time.perf_counter()-start:.3f}')
 for row in allrows:print(row)
if __name__=='__main__':main()
