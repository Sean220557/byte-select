#include "fpc_bsel/tier_reorder.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace fpc_bsel {
namespace {
constexpr std::uint32_t kHeaderBits = 33U * 8U;
constexpr std::uint32_t kTierBits = 1024U * 8U;
constexpr std::array<std::uint16_t, kTierReorderPayloads> kValueMask{
    0x0001U, 0x0002U, 0x0004U, 0x0008U,
    0x0010U, 0x0020U, 0x0040U, 0x0080U,
    0x0100U, 0x0200U, 0x0400U, 0x0800U,
    0x1000U, 0x2000U, 0x4000U, 0x8000U,
};

struct Edge {
    std::uint32_t good_index;
    std::uint8_t bad_slot;
    std::uint8_t good_slot;
};

std::uint8_t first_set_slot(std::uint16_t mask) {
    // The input is known nonzero at every call site.  Keep selection in the
    // bit domain; this is a single instruction on the supported compilers.
#if defined(_MSC_VER)
    unsigned long slot = 0;
    _BitScanForward(&slot, mask);
    return static_cast<std::uint8_t>(slot);
#else
    return static_cast<std::uint8_t>(__builtin_ctz(static_cast<unsigned>(mask)));
#endif
}

// For a fixed bad payload, produce a 16-bit value-mask over donor payload
// categories.  Bit j says payload j makes both full regions fit target.  The
// static kValueMask table is the sole representation of slot selection.
std::uint16_t value_mask(const TierReorderRegion& bad, std::uint8_t bad_slot,
                         const TierReorderRegion& good, std::uint32_t capacity) {
    std::uint16_t mask = 0;
    const auto big = bad.payload_bits[bad_slot];
    const auto small = bad.payload_bits_total - big;
    for (std::uint8_t j = 0; j < kTierReorderPayloads; ++j) {
        const auto x = good.payload_bits[j];
        const auto y = good.payload_bits_total - x;
        const auto cat = static_cast<std::uint8_t>(j);
        const auto valid = (small + x <= capacity) & (big + y <= capacity);
        const auto valid_bit = static_cast<std::uint16_t>(0U - valid);
        mask |= static_cast<std::uint16_t>(kValueMask[cat] & valid_bit);
    }
    return mask;
}
}

TierReorderResult optimize_tier_reorder(const std::vector<TierReorderRegion>& regions,
                                        std::uint16_t target_bytes,
                                        TierReorderStrategy strategy) {
    if (target_bytes < 1024 || target_bytes > 3072 || target_bytes % 1024)
        throw std::invalid_argument("tier-reorder target must be 1024, 2048, or 3072");
    const auto capacity = static_cast<std::uint32_t>(target_bytes) * 8U - kHeaderBits;
    const auto lower = target_bytes == 1024 ? 0U :
        static_cast<std::uint32_t>(target_bytes - 1024U) * 8U - kHeaderBits;
    const auto upper = capacity + kTierBits;
    std::vector<std::size_t> bad, good;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto total = regions[i].payload_bits_total;
        if (total <= capacity && total > lower) good.push_back(i);
        else if (total > capacity && total <= upper) bad.push_back(i);
    }
    TierReorderResult result; result.target_bytes = target_bytes;
    result.eligible_bad_regions = bad.size(); result.eligible_good_regions = good.size();
    // Sparse edges eliminate the O(|bad|*|good|) action matrix and make the
    // matching pass proportional to actual feasible swaps.  The excess/slack
    // test is a necessary condition for every exchange, so it skips the 16x16
    // mask construction for impossible region pairs without changing the graph.
    std::vector<std::vector<Edge>> edges(bad.size());
    std::vector<bool> good_has_candidate(good.size());
    for (std::size_t bi = 0; bi < bad.size(); ++bi) for (std::size_t gi = 0; gi < good.size(); ++gi) {
        const auto excess = regions[bad[bi]].payload_bits_total - capacity;
        const auto slack = capacity - regions[good[gi]].payload_bits_total;
        if (excess > slack) continue;
        bool found = false;
        for (std::uint8_t b = 0; b < kTierReorderPayloads && !found; ++b) {
            const auto mask = value_mask(regions[bad[bi]], b, regions[good[gi]], capacity);
            if (mask) {
                edges[bi].push_back({static_cast<std::uint32_t>(gi), b, first_set_slot(mask)});
                good_has_candidate[gi] = true;
                found = true;
            }
        }
        result.compatible_edges += found;
    }
    for (const auto& candidates : edges) result.bad_regions_with_candidate += !candidates.empty();
    for (const auto present : good_has_candidate) result.good_regions_with_candidate += present;
    std::vector<int> owner(good.size(), -1), partner(bad.size(), -1);
    if (strategy == TierReorderStrategy::LocalGreedy) {
        for (std::size_t bi=0; bi<bad.size(); ++bi) for (const auto& edge : edges[bi])
            if (owner[edge.good_index] < 0) { owner[edge.good_index]=static_cast<int>(bi); partner[bi]=static_cast<int>(edge.good_index); break; }
    } else {
        std::function<bool(std::size_t,std::vector<bool>&)> visit = [&](std::size_t bi,std::vector<bool>& seen) {
            for (const auto& edge : edges[bi]) if (!seen[edge.good_index]) {
                seen[edge.good_index]=true;
                if (owner[edge.good_index]<0 || visit(static_cast<std::size_t>(owner[edge.good_index]),seen)) { owner[edge.good_index]=static_cast<int>(bi); return true; }
            } return false;
        };
        for (std::size_t bi=0;bi<bad.size();++bi) { std::vector<bool> seen(good.size()); (void)visit(bi,seen); }
        for (std::size_t gi=0;gi<good.size();++gi) if (owner[gi]>=0) partner[owner[gi]]=static_cast<int>(gi);
    }
    for (std::size_t bi=0;bi<bad.size();++bi) if (partner[bi]>=0) {
        const auto gi=static_cast<std::size_t>(partner[bi]);
        const auto& edge = *std::find_if(edges[bi].begin(), edges[bi].end(),
            [gi](const Edge& candidate) { return candidate.good_index == gi; });
        result.swaps.push_back({regions[bad[bi]].region_index,
                                regions[good[gi]].region_index,
                                edge.bad_slot, edge.good_slot});
    }
    return result;
}
}  // namespace fpc_bsel
