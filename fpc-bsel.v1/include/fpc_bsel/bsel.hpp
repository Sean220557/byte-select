#pragma once

#include "fpc_bsel/types.hpp"

#include <optional>

namespace fpc_bsel {

struct Pattern {
    Bytes symbols;
    std::size_t rank() const;
};

struct BselModel {
    std::size_t block_size = 0;
    std::vector<Pattern> patterns;
};

Pattern simplest_pattern(const Bytes& block);
BselModel train_bsel(const std::vector<Bytes>& blocks,
                     const std::vector<std::size_t>& baseline_sizes,
                     std::size_t max_patterns);
std::size_t bsel_id_bytes(const BselModel& model);
std::optional<Bytes> bsel_encode(const Bytes& block, const BselModel& model);
Bytes bsel_decode(const Bytes& encoded, const BselModel& model);

}
