#pragma once

#include "byte_select/pattern.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bsel {

enum class BaselineKind {
    Fpc,
    Bdi,
    HybridBdiFpc,
    Cpack,
    Bpc
};

struct BaselineEvaluation {
    BaselineKind kind = BaselineKind::Fpc;
    std::uint64_t blocks = 0;
    std::uint64_t original_bytes = 0;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t allocated_bytes = 0;
    std::uint64_t compressed_blocks = 0;
    std::vector<std::size_t> target_sizes;
    std::vector<std::uint64_t> target_blocks;

    double unquantized_compression_ratio() const;
    double quantized_compression_ratio() const;
};

BaselineKind parse_baseline_kind(const std::string& text);
const char* baseline_kind_name(BaselineKind kind);

// Returns the smaller of the baseline representation and a raw cache block.
// These are size estimators for the published formats, not bitstream encoders.
std::size_t fpc_encoded_size(const Block& block);
std::size_t bdi_encoded_size(const Block& block);
std::size_t cpack_encoded_size(const Block& block);
std::size_t bpc_encoded_size(const Block& block);
std::size_t baseline_encoded_size(const Block& block, BaselineKind kind);

BaselineEvaluation evaluate_baseline(const std::vector<Block>& blocks,
                                     BaselineKind kind,
                                     std::vector<std::size_t> target_sizes);

}  // namespace bsel
