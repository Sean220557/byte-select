#pragma once

#include "fpc_bsel/tier_reorder.hpp"

namespace fpc_bsel {

struct PairReorderSwap {
    std::uint8_t bad_pair = 0, good_pair = 0;
    std::uint8_t bad_payload = 0, good_payload = 0;
};

struct PairReorderResult {
    std::uint16_t max_len_before = 0, target_len = 0, max_len_after = 0;
    std::uint8_t bad_pairs_before = 0, bad_pairs_after = 0;
    std::uint8_t rounds = 0;
    bool early_reject = false;
    bool reordered = false;
    std::vector<PairReorderSwap> swaps;
};

PairReorderResult optimize_adjacent_pairs(const TierReorderRegion& region,
                                          TierReorderStrategy strategy);

}  // namespace fpc_bsel
