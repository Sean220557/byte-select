#!/usr/bin/env python3
"""Round-trip verifier for the direct-marker FPC-payload Var-BSEL stream."""
from __future__ import annotations
import argparse,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import analyze_lz_seeded_var_bsel as vb
import experiment_fpc_payload_lz as pp

MARKERS=(0xfb,0xfc,0xfd,0xfe); ESCAPE=0xff; RESERVED=set(MARKERS+(ESCAPE,))

def dictionary(data,pattern):
 values=[None]*vb.rank(pattern)
 for value,symbol in zip(data,pattern):
  if values[symbol] is None:values[symbol]=value
  elif values[symbol]!=value:return None
 return bytes(values)

def encode(data,patterns):
 n=len(data);cost=[0]*(n+1);choice=[None]*n
 for pos in range(n-1,-1,-1):
  literal=bytes((data[pos],)) if data[pos] not in RESERVED else bytes((ESCAPE,data[pos]))
  cost[pos]=len(literal)+cost[pos+1];choice[pos]=(1,literal)
  for pid,pattern in enumerate(patterns):
   L=len(pattern)
   if pos+L>n:continue
   values=dictionary(data[pos:pos+L],pattern)
   if values is None:continue
   token=bytes((MARKERS[pid],))+values
   candidate=len(token)+cost[pos+L]
   if candidate<cost[pos]:cost[pos]=candidate;choice[pos]=(L,token)
 out=bytearray();pos=0
 while pos<n:
  length,token=choice[pos];out+=token;pos+=length
 return bytes(out)

def decode(stream,patterns,expected):
 out=bytearray();pos=0
 while len(out)<expected:
  if pos>=len(stream):raise ValueError('truncated marker stream')
  value=stream[pos];pos+=1
  if value==ESCAPE:
   if pos>=len(stream):raise ValueError('truncated escaped literal')
   out.append(stream[pos]);pos+=1
  elif value in MARKERS:
   pid=MARKERS.index(value)
   if pid>=len(patterns):raise ValueError('unknown marker pattern')
   pattern=patterns[pid];rank=vb.rank(pattern)
   if pos+rank>len(stream):raise ValueError('truncated pattern dictionary')
   values=stream[pos:pos+rank];pos+=rank
   out.extend(values[s] for s in pattern)
  else:out.append(value)
 if len(out)!=expected or pos!=len(stream):raise ValueError('marker stream size mismatch')
 return bytes(out)

def feature(r):return tuple(x/64.0 for x in r.qvec)+(r.gap/64.0,)
def dist(a,b):return sum((x-y)**2 for x,y in zip(a,b))

def main():
 ap=argparse.ArgumentParser();ap.add_argument('model',type=Path);ap.add_argument('payloads',type=Path)
 ap.add_argument('--limit-regions',type=int,default=0);args=ap.parse_args()
 model=json.loads(args.model.read_text(encoding='utf-8'));limit=args.limit_regions or 10**9
 rs=pp.regions(pp.read_payloads(args.payloads,limit));tested=raw=encoded=chosen=chosen_raw=chosen_encoded=0
 adaptive='segments' in model
 max_gap=model.get('config',{}).get('max_gap',256)
 for r in rs:
  if adaptive:
   match=next((s for s in model['segments'] if s['begin']<=r.i<=s['end'] and s['patterns']),None)
   if match is None:continue
   patterns=[tuple(p) for p in match['patterns']]
  else:
   entries=model['tiers'].get(str(r.tier),[])
   if not entries or not (0<r.gap<=max_gap):continue
   bid=min(range(len(entries)),key=lambda j:dist(feature(r),entries[j]['center']))
   patterns=[tuple(p) for p in entries[bid]['patterns']]
  if len(patterns)>len(MARKERS):raise ValueError('model exceeds direct marker capacity')
  for payload in r.payloads:
   stream=encode(payload,patterns)
   if decode(stream,patterns,len(payload))!=payload:raise ValueError('round-trip mismatch')
   expected=pp.encode(payload,tuple(vb.Entry(p,0) for p in patterns))
   if len(stream)!=expected:raise ValueError('DP size and emitted stream disagree')
   tested+=1;raw+=len(payload);encoded+=len(stream)
   if len(stream)<len(payload):chosen+=1;chosen_raw+=len(payload);chosen_encoded+=len(stream)
 print(f'regions={len(rs)} tested_sublines={tested} payload_bytes={raw} encoded_bytes={encoded} '
       f'chosen_sublines={chosen} chosen_payload_bytes={chosen_raw} chosen_encoded_bytes={chosen_encoded} '
       f'chosen_saved={chosen_raw-chosen_encoded} roundtrip=ok')
if __name__=='__main__':main()
