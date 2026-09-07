#include "fpc_bsel/pair_reorder.hpp"
#include <algorithm>
#include <array>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
namespace fpc_bsel { namespace {
constexpr std::array<std::uint16_t,16> slot_mask{1,2,4,8,16,32,64,128,256,512,1024,2048,4096,8192,16384,32768};
std::uint16_t exponent(std::uint16_t value) { std::uint16_t e=0,n=value?value-1:0; while(n){n>>=1;++e;} return e; }
std::uint16_t next_target(std::uint16_t max_exp) { return max_exp>2 ? static_cast<std::uint16_t>(max_exp - ((max_exp&1U)?1U:2U)) : 0; }
std::uint8_t rightmost(std::uint16_t mask) {
#if defined(_MSC_VER)
 unsigned long bit=0; _BitScanReverse(&bit,mask); return static_cast<std::uint8_t>(bit);
#else
 return static_cast<std::uint8_t>(31U-__builtin_clz(static_cast<unsigned>(mask)));
#endif
}
std::uint16_t value_mask(const std::array<std::uint16_t,16>& v,std::uint8_t bad_slot,std::uint8_t good_pair,std::uint16_t target_exp){
 const auto target=static_cast<std::uint32_t>(1U<<target_exp); const auto big=v[bad_slot]; const auto small=v[bad_slot^1U]; const auto base=static_cast<std::uint8_t>(good_pair*2U); std::uint16_t mask=0;
 for(std::uint8_t j=0;j<2;++j){const auto valid=(small+v[base+j]<=target)&(big+v[base+(j^1U)]<=target);mask|=static_cast<std::uint16_t>(slot_mask[base+j]&static_cast<std::uint16_t>(0U-valid));} return mask;
}
bool round(std::array<std::uint16_t,16>& v,std::uint16_t target,TierReorderStrategy strategy,std::vector<PairReorderSwap>& out){
 const auto capacity=static_cast<std::uint32_t>(1U<<target);std::array<std::uint8_t,8> bad{},good{};std::uint8_t nb=0,ng=0;for(std::uint8_t p=0;p<8;++p){const auto n=static_cast<std::uint16_t>(v[p*2U]+v[p*2U+1U]);if(n>capacity)bad[nb++]=p;else good[ng++]=p;}if(!nb||!ng)return false;
 std::array<std::array<std::uint8_t,8>,8> eg{},eb{};std::array<std::uint8_t,8> ec{};
 for(std::uint8_t bi=0;bi<nb;++bi)for(std::int8_t gi=static_cast<std::int8_t>(ng)-1;gi>=0;--gi){const auto bp=bad[bi],gp=good[gi];for(std::int8_t s=static_cast<std::int8_t>(bp*2U+1U);s>=static_cast<std::int8_t>(bp*2U);--s){if(const auto m=value_mask(v,static_cast<std::uint8_t>(s),gp,target)){eg[bi][ec[bi]]=static_cast<std::uint8_t>(gi);eb[bi][ec[bi]]=static_cast<std::uint8_t>(s);++ec[bi];break;}}}
 std::array<int,8> owner{},partner{};owner.fill(-1);partner.fill(-1);auto visit=[&](auto&& self,std::uint8_t bi,std::uint8_t seen)->bool{for(std::uint8_t e=0;e<ec[bi];++e){const auto gi=eg[bi][e],bit=static_cast<std::uint8_t>(1U<<gi);if(seen&bit)continue;if(owner[gi]<0||self(self,static_cast<std::uint8_t>(owner[gi]),static_cast<std::uint8_t>(seen|bit))){owner[gi]=bi;return true;}}return false;};
 for(std::int8_t bi=static_cast<std::int8_t>(nb)-1;bi>=0;--bi)if(strategy==TierReorderStrategy::LocalGreedy){for(std::uint8_t e=0;e<ec[bi];++e)if(owner[eg[bi][e]]<0){owner[eg[bi][e]]=bi;partner[bi]=eg[bi][e];break;}}else(void)visit(visit,static_cast<std::uint8_t>(bi),0);if(strategy==TierReorderStrategy::MaximumMatching)for(std::uint8_t gi=0;gi<ng;++gi)if(owner[gi]>=0)partner[owner[gi]]=gi;
 bool changed=false;for(std::uint8_t bi=0;bi<nb;++bi)if(partner[bi]>=0){const auto gp=good[partner[bi]];std::uint8_t bs=0;for(std::uint8_t e=0;e<ec[bi];++e)if(eg[bi][e]==partner[bi]){bs=eb[bi][e];break;}const auto gs=rightmost(value_mask(v,bs,gp,target));std::swap(v[bs],v[gs]);out.push_back({bad[bi],gp,bs,gs});changed=true;}return changed;
}
} PairReorderResult optimize_adjacent_pairs(const TierReorderRegion& region,TierReorderStrategy strategy){
 auto v=region.payload_bits;PairReorderResult r;std::uint16_t max_bytes=0;for(std::uint8_t p=0;p<8;++p)max_bytes=std::max(max_bytes,static_cast<std::uint16_t>(v[p*2U]+v[p*2U+1U]));r.max_len_before=exponent(max_bytes);auto target=next_target(r.max_len_before);r.target_len=target;const auto capacity=target?static_cast<std::uint32_t>(1U<<target):0U;for(std::uint8_t p=0;p<8;++p)r.bad_pairs_before+=static_cast<std::uint8_t>(target&&(v[p*2U]+v[p*2U+1U]>capacity));while(target&&r.rounds<8){if(!round(v,target,strategy,r.swaps))break;++r.rounds;}r.reordered=!r.swaps.empty();std::uint16_t final_max=0;for(std::uint8_t p=0;p<8;++p){const auto n=static_cast<std::uint16_t>(v[p*2U]+v[p*2U+1U]);final_max=std::max(final_max,n);if(target&&n>capacity)++r.bad_pairs_after;}r.max_len_after=exponent(final_max);return r;}
} // namespace fpc_bsel
