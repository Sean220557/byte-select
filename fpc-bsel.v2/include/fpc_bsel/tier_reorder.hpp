#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fpc_bsel {

constexpr std::size_t kTierReorderPayloads = 16;

struct TierReorderRegion {
    std::array<std::uint16_t, kTierReorderPayloads> payload_bits{};
    std::uint32_t payload_bits_total = 0;
    // Position in the serialized region stream; carried into the reversible
    // swap schedule rather than the compact optimizer-vector index.
    std::uint32_t region_index = 0;
};

enum class TierReorderStrategy { LocalGreedy, MaximumMatching };

struct TierReorderSwap {
    std::size_t bad_region = 0, good_region = 0;
    std::uint8_t bad_payload = 0, good_payload = 0;
};

struct TierReorderResult {
    std::uint16_t target_bytes = 0;
    std::size_t eligible_bad_regions = 0, eligible_good_regions = 0;
    std::size_t bad_regions_with_candidate = 0, good_regions_with_candidate = 0;
    std::size_t compatible_edges = 0;
    std::vector<TierReorderSwap> swaps;
};

// Regions contain the 16 post-prefix payload lengths. A bad region is in the
// tier immediately above target; a good region is already in target's tier.
// A selected swap makes both regions fit target, so it saves one 1KiB tier.
TierReorderResult optimize_tier_reorder(const std::vector<TierReorderRegion>& regions,
                                        std::uint16_t target_bytes,
                                        TierReorderStrategy strategy);

}  // namespace fpc_bsel
