#include "fpc_bsel/codec.hpp"
#include "fpc_bsel/fpc.hpp"
#include "fpc_bsel/model.hpp"
#include "fpc_bsel/pair_reorder.hpp"
#include "fpc_bsel/tier_reorder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

using Bytes = std::vector<std::uint8_t>;
static std::uint16_t u16(const Bytes& b, std::size_t& p){ if(p+2>b.size()) throw std::runtime_error("truncated payloads"); auto v=std::uint16_t(b[p])|(std::uint16_t(b[p+1])<<8); p+=2; return v; }
static std::uint64_t u64(const Bytes& b, std::size_t& p){ std::uint64_t v=0; for(unsigned i=0;i<8;++i){if(p>=b.size())throw std::runtime_error("truncated payloads");v|=std::uint64_t(b[p++])<<(8*i);}return v; }
static int tier(std::size_t n){return n?int((n+1023)/1024):0;}
static unsigned index_bits(std::uint64_t count){unsigned bits=0;for(std::uint64_t n=count>1?count-1:0;n;n>>=1)++bits;return bits;}
static std::uint32_t key(const std::uint8_t* p,int n){std::uint32_t v=std::uint32_t(n)<<24;for(int i=0;i<n;++i)v|=std::uint32_t(p[i])<<(8*i);return v;}
struct Cache {
  std::vector<std::uint32_t> keys; std::vector<std::uint8_t> lens,scores; std::unordered_map<std::uint32_t,int> map; int hand=0,cap;
  explicit Cache(int c):cap(c){keys.reserve(c);lens.reserve(c);scores.reserve(c);map.reserve(c*2);}
  void clear(){keys.clear();lens.clear();scores.clear();map.clear();hand=0;}
  int find(const std::uint8_t* p,int n,int& len){for(int l:{3,2})if(l<n){auto k=key(p,l);auto it=map.find(k);if(it!=map.end()){len=l;return it->second;}}return -1;}
  void touch(int i){scores[i]=std::min<int>(3,scores[i]+1);}
  int slot(){if((int)keys.size()<cap)return keys.size();while(scores[hand]){--scores[hand];hand=(hand+1)%cap;}int s=hand;hand=(hand+1)%cap;map.erase(keys[s]);return s;}
  void add(const std::uint8_t* p,int n){for(int l:{2,3})if(l<n){auto k=key(p,l);if(map.count(k))continue;int s=slot();if(s==(int)keys.size()){keys.push_back(k);lens.push_back(l);scores.push_back(0);}else{keys[s]=k;lens[s]=l;scores[s]=0;}map[k]=s;}}
};
static int evaluate_raw_stream(const char* model_path,const char* input_path,int entries,std::uint64_t progress){
  const auto model=fpc_bsel::load_model(model_path);std::array<char,1<<20> io_buffer{};std::ifstream in;in.rdbuf()->pubsetbuf(io_buffer.data(),io_buffer.size());in.open(input_path,std::ios::binary);if(!in)throw std::runtime_error("cannot open raw input");
  const auto bytes=std::filesystem::file_size(input_path);if(bytes<4096)throw std::runtime_error("raw input must contain at least one 4096-byte region");const std::uint64_t regions=bytes/4096,count=regions*16;
  const char* mcc_after_path=std::getenv("PREFIX_MCC_SIZES");const char* mcc_before_path=std::getenv("PREFIX_MCC_SIZES_BEFORE");const bool enable_tier_reorder=std::getenv("PREFIX_TIER_REORDER")!=nullptr;Cache cache(entries);std::vector<fpc_bsel::TierReorderRegion> tier_regions;if(enable_tier_reorder)tier_regions.reserve(regions);std::vector<std::uint16_t> mcc_sizes,mcc_sizes_before;if(mcc_after_path)mcc_sizes.reserve(count);if(mcc_before_path)mcc_sizes_before.reserve(count);std::uint64_t hits=0,lookups=0,selected=0,crossed=0,pair_physical_before=0,pair_physical_after=0;std::array<std::uint64_t,5> trans{},tier_algorithm_saved{},tier_quantized_saved{};std::array<std::uint64_t,4> pair_regions{},pair_bad_before{},pair_bad_after{},pair_swaps{};std::uint64_t ab=0,aa=0,qb=0,qa=0;auto started=std::chrono::steady_clock::now();std::array<std::uint8_t,4096> raw{};
  Bytes payload;payload.reserve(256);Bytes block;block.reserve(64);for(std::uint64_t rid=0;rid<regions;++rid){in.read(reinterpret_cast<char*>(raw.data()),raw.size());if(!in)throw std::runtime_error("short raw read");cache.clear();std::size_t raw_size=32,bits=0;fpc_bsel::TierReorderRegion tier_region{},pair_region{};tier_region.region_index=static_cast<std::uint32_t>(rid);pair_region.region_index=static_cast<std::uint32_t>(rid);std::array<std::uint8_t,4> prev{};bool have_prev=false;int prev_n=0;
    for(int sub=0;sub<16;++sub){const auto start_bits=bits;payload.clear();for(int lane=0;lane<4;++lane){const auto off=sub*256+lane*64;block.assign(raw.begin()+off,raw.begin()+off+64);auto enc=fpc_bsel::encode_block(block,model,{false,false});payload.insert(payload.end(),enc.bytes.begin()+1,enc.bytes.end());}raw_size+=payload.size();
      for(std::size_t off=0;off<payload.size();off+=4){int n=std::min<std::size_t>(4,payload.size()-off),len=0;int idx=cache.find(payload.data()+off,n,len);++lookups;int best=1+8*n;if(idx>=0)++hits;bool use_cache=false,use_prev=false;if(idx>=0){int cost=10+8*(n-len);if(cost<=best){best=cost;use_cache=true;}}
        if(false&&have_prev){int plen=0;for(int l:{3,2})if(l<n&&l<=prev_n&&std::equal(payload.begin()+off,payload.begin()+off+l,prev.begin())){plen=l;break;}if(plen){int cost=3+8*(n-plen);if(cost<best){best=cost;use_cache=false;use_prev=true;}}}if(use_cache){cache.touch(idx);++selected;}else if(use_prev)++selected;bits+=best;cache.add(payload.data()+off,n);std::fill(prev.begin(),prev.end(),0);std::copy_n(payload.begin()+off,n,prev.begin());have_prev=true;prev_n=n;}tier_region.payload_bits[sub]=static_cast<std::uint16_t>(bits-start_bits);pair_region.payload_bits[sub]=static_cast<std::uint16_t>((bits-start_bits+7U)/8U);if(mcc_before_path)mcc_sizes_before.push_back(std::min<std::uint16_t>(pair_region.payload_bits[sub],256));}
    tier_region.payload_bits_total=static_cast<std::uint32_t>(bits);pair_region.payload_bits_total=static_cast<std::uint32_t>((bits+7U)/8U);const auto pair_plan=fpc_bsel::optimize_adjacent_pairs(pair_region,fpc_bsel::TierReorderStrategy::MaximumMatching);pair_physical_before+=8ULL*(1ULL<<pair_plan.max_len_before);pair_physical_after+=8ULL*(1ULL<<pair_plan.max_len_after);if(pair_plan.target_len){const auto pi=pair_plan.target_len==8?0:pair_plan.target_len==6?1:pair_plan.target_len==4?2:3;++pair_regions[pi];pair_bad_before[pi]+=pair_plan.bad_pairs_before;pair_bad_after[pi]+=pair_plan.bad_pairs_after;pair_swaps[pi]+=pair_plan.swaps.size();}for(const auto& sw:pair_plan.swaps)std::swap(pair_region.payload_bits[sw.bad_payload],pair_region.payload_bits[sw.good_payload]);if(mcc_after_path)for(const auto n:pair_region.payload_bits)mcc_sizes.push_back(std::min<std::uint16_t>(n,256));const std::size_t before=std::min<std::size_t>(raw_size,4096),prefix_after=33+(bits+7)/8,after=std::min(before,prefix_after);if(enable_tier_reorder&&prefix_after<=before)tier_regions.push_back(tier_region);const int bt=tier(before),at=tier(after);ab+=before;aa+=after;qb+=bt*1024;qa+=at*1024;if(at<bt){++crossed;if(bt-at==1&&bt>=1&&bt<=4){++trans[bt];tier_algorithm_saved[bt]+=before-after;tier_quantized_saved[bt]+=1024;}}
    const auto done=(rid+1)*16;if(progress&&done%progress<16){double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();std::cerr<<"[prefix-cpp] sublines="<<done<<"/"<<count<<" ("<<std::fixed<<std::setprecision(1)<<100.0*done/count<<"%) rate="<<std::setprecision(0)<<done/std::max(sec,1e-9)<<"/s\n";}}
  if(mcc_after_path){std::uint64_t metadata_bytes=regions*34ULL;for(const auto n:pair_swaps)metadata_bytes+=n;for(auto& n:mcc_sizes){const auto room=std::uint64_t(256U-n);const auto add=std::min(room,metadata_bytes);n=static_cast<std::uint16_t>(n+add);metadata_bytes-=add;if(!metadata_bytes)break;}if(metadata_bytes)throw std::runtime_error("MCC metadata does not fit in region records");std::ofstream sizes(mcc_after_path);if(!sizes)throw std::runtime_error("cannot open MCC size output");for(const auto n:mcc_sizes)sizes<<n<<'\n';}
  if(mcc_before_path){std::ofstream sizes(mcc_before_path);if(!sizes)throw std::runtime_error("cannot open pre-swap MCC size output");for(const auto n:mcc_sizes_before)sizes<<n<<'\n';}
  const auto original=regions*4096ULL;std::cout<<std::fixed<<std::setprecision(8)<<"sublines="<<count<<" regions="<<regions<<" cache_entries="<<entries<<"\n"<<"algorithm_ratio_before="<<double(ab)/original<<" algorithm_ratio_after="<<double(aa)/original<<"\n"<<"algorithm_bytes_before="<<ab<<" algorithm_bytes_after="<<aa<<"\n"<<"quantized_ratio_before="<<double(qb)/original<<" quantized_ratio_after="<<double(qa)/original<<"\n"<<"crossed_regions="<<crossed<<" 4K_to_3K="<<trans[4]<<" 3K_to_2K="<<trans[3]<<" 2K_to_1K="<<trans[2]<<" 1K_to_0K="<<trans[1]<<"\n";
  std::cout<<std::setprecision(6)<<"tier_contribution_pp:\n";for(int from=4;from>=1;--from)std::cout<<from<<"K_to_"<<(from-1)<<"K count="<<trans[from]<<" algorithm_saved_bytes="<<tier_algorithm_saved[from]<<" algorithm_pp="<<(100.0*tier_algorithm_saved[from]/original)<<" quantized_saved_bytes="<<tier_quantized_saved[from]<<" quantized_pp="<<(100.0*tier_quantized_saved[from]/original)<<"\n";
  const auto crossing_algorithm=tier_algorithm_saved[1]+tier_algorithm_saved[2]+tier_algorithm_saved[3]+tier_algorithm_saved[4];
  std::cout<<"non_crossing algorithm_saved_bytes="<<(ab-aa-crossing_algorithm)<<" algorithm_pp="<<(100.0*(ab-aa-crossing_algorithm)/original)<<" quantized_pp=0.000000\n"
           <<"total algorithm_pp="<<(100.0*(ab-aa)/original)<<" quantized_pp="<<(100.0*(qb-qa)/original)<<"\n";
  std::cout<<"adjacent_pair_reorder target=8 regions="<<pair_regions[0]<<" bad_before="<<pair_bad_before[0]<<" bad_after="<<pair_bad_after[0]<<" swaps="<<pair_swaps[0]<<"\n"
           <<"adjacent_pair_reorder target=6 regions="<<pair_regions[1]<<" bad_before="<<pair_bad_before[1]<<" bad_after="<<pair_bad_after[1]<<" swaps="<<pair_swaps[1]<<"\n"
           <<"adjacent_pair_reorder target=4 regions="<<pair_regions[2]<<" bad_before="<<pair_bad_before[2]<<" bad_after="<<pair_bad_after[2]<<" swaps="<<pair_swaps[2]<<"\n"
           <<"adjacent_pair_reorder target=2 regions="<<pair_regions[3]<<" bad_before="<<pair_bad_before[3]<<" bad_after="<<pair_bad_after[3]<<" swaps="<<pair_swaps[3]<<"\n"
           <<"adjacent_pair_physical_bytes_before="<<pair_physical_before<<" after="<<pair_physical_after<<" saved="<<(pair_physical_before-pair_physical_after)<<" ratio_before="<<(double(pair_physical_before)/original)<<" ratio_after="<<(double(pair_physical_after)/original)<<"\n";
  std::cout<<std::setprecision(8)<<"lookups="<<lookups<<" hits="<<hits<<" hit_rate="<<(lookups?double(hits)/lookups:0)<<" selected_tokens="<<selected<<" runtime_state_bytes="<<(entries*4+(entries*2+7)/8+8)<<" static_codebook_bytes=0\n";if(enable_tier_reorder){const auto schedule_bits_per_swap=2U*index_bits(regions)+8U;for(const auto target:{std::uint16_t{3072},std::uint16_t{2048},std::uint16_t{1024}}){const auto greedy=fpc_bsel::optimize_tier_reorder(tier_regions,target,fpc_bsel::TierReorderStrategy::LocalGreedy);const auto optimal=fpc_bsel::optimize_tier_reorder(tier_regions,target,fpc_bsel::TierReorderStrategy::MaximumMatching);const auto saved=optimal.swaps.size()*1024ULL;const auto schedule_bytes=(32ULL+optimal.swaps.size()*schedule_bits_per_swap+7ULL)/8ULL;const auto quantized_after_swap=qa-saved+schedule_bytes;std::cout<<"tier_reorder target="<<target<<" prefix_regions="<<tier_regions.size()<<" bad_regions="<<optimal.eligible_bad_regions<<" donor_regions="<<optimal.eligible_good_regions<<" bad_with_candidate="<<optimal.bad_regions_with_candidate<<" donor_with_candidate="<<optimal.good_regions_with_candidate<<" compatible_edges="<<optimal.compatible_edges<<" greedy_swaps="<<greedy.swaps.size()<<" maximum_matching_swaps="<<optimal.swaps.size()<<" quantized_saved_bytes="<<saved<<" schedule_bits_per_swap="<<schedule_bits_per_swap<<" schedule_bytes="<<schedule_bytes<<" quantized_bytes_after_swap="<<quantized_after_swap<<" quantized_ratio_after_swap="<<double(quantized_after_swap)/original<<"\n";}}return 0;
}
int main(int argc,char**argv)try{
  bool raw=argc>1&&std::string(argv[1])=="--raw"; if((!raw&&(argc<2||argc>4))||(raw&&(argc<4||argc>6)))throw std::invalid_argument("usage: prefix-eval-cpp PAYLOADS [ENTRIES] [PROGRESS] | --raw MODEL INPUT [ENTRIES] [PROGRESS]");
  int ai=raw?4:2; int entries=argc>ai?std::stoi(argv[ai]):256; std::uint64_t progress=argc>ai+1?std::stoull(argv[ai+1]):1000000;std::vector<Bytes> payloads;std::uint64_t count=0;
  if(raw)return evaluate_raw_stream(argv[2],argv[3],entries,progress);
  else{std::ifstream in(argv[1],std::ios::binary);if(!in)throw std::runtime_error("cannot open payloads");Bytes b((std::istreambuf_iterator<char>(in)),{});if(b.size()<16||std::string((char*)b.data(),8)!=std::string("FPCPAY1\0",8))throw std::runtime_error("bad FPCPAY1 container");std::size_t p=8;count=u64(b,p);payloads.reserve(count);for(std::uint64_t i=0;i<count;++i){auto n=u16(b,p);if(p+n>b.size())throw std::runtime_error("truncated payload");payloads.emplace_back(b.begin()+p,b.begin()+p+n);p+=n;}}
  const auto usable=(count/16)*16; Cache cache(entries);std::uint64_t hits=0,lookups=0,selected=0,crossed=0;std::array<std::uint64_t,5> trans{};std::uint64_t ab=0,aa=0,qb=0,qa=0;auto start=std::chrono::steady_clock::now();
  for(std::uint64_t base=0;base<usable;base+=16){cache.clear();std::size_t raw=32,bits=0;std::array<std::uint8_t,4> prev{};bool have_prev=false;int prev_n=0;
    auto end=base+16;for(auto si=base;si<end;++si){auto& pl=payloads[si];raw+=pl.size();for(std::size_t off=0;off<pl.size();off+=4){int n=std::min<std::size_t>(4,pl.size()-off),len=0;int idx=cache.find(pl.data()+off,n,len);++lookups;int literal=1+8*n,best=literal;if(idx>=0)++hits;
      bool used_cache=false,used_previous=false;if(idx>=0){int cost=2+8+8*(n-len);if(cost<=best){best=cost;used_cache=true;}}
      if(have_prev){int plen=0;for(int l:{3,2})if(l<n&&l<=prev_n&&std::equal(pl.begin()+off,pl.begin()+off+l,prev.begin())){plen=l;break;}if(plen){int cost=3+8*(n-plen);if(cost<best){best=cost;used_cache=false;used_previous=true;}}}if(used_cache){cache.touch(idx);++selected;}else if(used_previous)++selected;bits+=best;cache.add(pl.data()+off,n);std::fill(prev.begin(),prev.end(),0);std::copy_n(pl.begin()+off,n,prev.begin());have_prev=true;prev_n=n;}}
    std::size_t before=std::min<std::size_t>(raw,4096),encoded=33+(bits+7)/8,after=std::min(before,encoded);int bt=tier(before),at=tier(after);ab+=before;aa+=after;qb+=bt*1024;qa+=at*1024;if(at<bt){++crossed;if(bt-at==1&&bt>=1&&bt<=4)++trans[bt];}
    if(progress&&end%progress<16){double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();std::cerr<<"[prefix-cpp pid=";
#ifdef _WIN32
      std::cerr<<"win";
#else
      std::cerr<<::getpid();
#endif
      std::cerr<<"] sublines="<<end<<"/"<<usable<<" ("<<std::fixed<<std::setprecision(1)<<100.0*end/usable<<"%) rate="<<std::setprecision(0)<<end/std::max(sec,1e-9)<<"/s\n";}}
  auto original=(usable/16)*4096ULL;std::cout<<std::fixed<<std::setprecision(8)
    <<"sublines="<<usable<<" regions="<<(usable/16)<<" cache_entries="<<entries<<"\n"
    <<"algorithm_ratio_before="<<double(ab)/original<<" algorithm_ratio_after="<<double(aa)/original<<"\n"
    <<"algorithm_bytes_before="<<ab<<" algorithm_bytes_after="<<aa<<"\n"
    <<"quantized_ratio_before="<<double(qb)/original<<" quantized_ratio_after="<<double(qa)/original<<"\n"
    <<"crossed_regions="<<crossed<<" 4K_to_3K="<<trans[4]<<" 3K_to_2K="<<trans[3]<<" 2K_to_1K="<<trans[2]<<" 1K_to_0K="<<trans[1]<<"\n"
    <<"lookups="<<lookups<<" hits="<<hits<<" hit_rate="<<(lookups?double(hits)/lookups:0)<<" selected_tokens="<<selected<<" runtime_state_bytes="<<(entries*4+(entries*2+7)/8+8)<<" static_codebook_bytes=0\n";
  return 0;
}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<'\n';return 2;}
