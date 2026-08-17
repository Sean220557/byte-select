#!/usr/bin/env python3
"""Bounded real-LZ control experiment over each FPC+BSEL 256B payload."""
from __future__ import annotations
import argparse,csv,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import analyze_var_strings as lz
import experiment_fpc_payload_lz as pp

def grouped_pack(payloads,group):
 total=0
 for begin in range(0,len(payloads),group):
  values=payloads[begin:begin+group]
  # Preserve one FPC+BSEL control byte per 256B subline and add packed 9-bit
  # payload lengths (0 denotes 256). This is conservative and independently
  # decodable at the selected group granularity.
  metadata=len(values)+(9*len(values)+7)//8
  used=metadata+sum(len(x) for x in values)
  total+=(used+63)//64*64
 return total

def main():
 ap=argparse.ArgumentParser();ap.add_argument('payloads',type=Path);ap.add_argument('--name',required=True)
 ap.add_argument('--limit-mib',type=int,default=1);ap.add_argument('--min-match',type=int,default=4)
 ap.add_argument('--max-match',type=int,default=64);ap.add_argument('--chain-depth',type=int,default=64)
 ap.add_argument('--token-bytes',type=int,default=3);ap.add_argument('--mode-bytes',type=int,default=2)
 ap.add_argument('--output',type=Path,required=True);args=ap.parse_args()
 limit=args.limit_mib*256 if args.limit_mib else 10**9
 rs=pp.regions(pp.read_payloads(args.payloads,limit));rows=[]
 for r in rs:
  actual=[];changed=[];phrases=0
  for i,payload in enumerate(r.payloads):
   encoded,_literal,ps=lz.analyze_region(payload,args.min_match,args.max_match,args.chain_depth,args.token_bytes)
   candidate=1+encoded
   if candidate<r.sizes[i]:actual.append(candidate);changed.append(i);phrases+=len(ps)
   else:actual.append(r.sizes[i])
  if changed:actual[changed[0]]+=args.mode_bytes
  after=sum(vb.quantize(x) for x in actual)
  stream=b''.join(r.payloads)
  region_encoded,_region_literal,region_phrases=lz.analyze_region(
   stream,args.min_match,args.max_match,args.chain_depth,args.token_bytes)
  # Sequential region-LZ upper bound: one mode header plus a 32B table of the
  # original sixteen payload lengths. It gives up independent 256B decoding.
  region_bytes=1+args.mode_bytes+32+region_encoded
  region_after=(region_bytes+63)//64*64
  packed_after=(1+args.mode_bytes+32+len(stream)+63)//64*64
  packs={group:grouped_pack(r.payloads,group) for group in (2,4,8,16)}
  rows.append({'region':r.i,'before_tier':r.tier,'target':r.target,'gap':r.gap,'before':r.total,
   'after':after,'crossed':int(r.gap>0 and after<=r.target),'changed_payloads':len(changed),'phrases':phrases,
   'region_lz_after':region_after,'region_lz_crossed':int(r.gap>0 and region_after<=r.target),
   'region_lz_phrases':len(region_phrases),'packed_after':packed_after,
   'packed_crossed':int(r.gap>0 and packed_after<=r.target),
   **{f'pack{group}_after':value for group,value in packs.items()},
   **{f'pack{group}_crossed':int(r.gap>0 and value<=r.target) for group,value in packs.items()}})
 args.output.parent.mkdir(parents=True,exist_ok=True)
 with args.output.open('w',newline='') as f:
  w=csv.DictWriter(f,fieldnames=rows[0].keys());w.writeheader();w.writerows(rows)
 for tier,target in ((4096,3072),(3072,2048),(2048,1024)):
  selected=[x for x in rows if x['before_tier']==tier and 0<int(x['gap'])<=256]
  print(f'tier={tier} eligible={len(selected)} crossed={sum(x["crossed"] for x in selected)} '
   f'region_lz_crossed={sum(x["region_lz_crossed"] for x in selected)} '
   f'packed_crossed={sum(x["packed_crossed"] for x in selected)} '
   + ' '.join(f'pack{g}={sum(x[f"pack{g}_crossed"] for x in selected)}' for g in (2,4,8,16))+' '
   f'before={sum(x["before"] for x in selected)} after={sum(x["after"] for x in selected)}')
if __name__=='__main__':main()
