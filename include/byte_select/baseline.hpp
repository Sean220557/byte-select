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
    Bpc,
    Zstd,
    Lz4,
    Lz77Lite,
    Huffman
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

// Real, round-trippable baseline bitstreams. The block size is external framing
// metadata, as it is for a cache-line compressor.
std::vector<std::uint8_t> fpc_encode(const Block& block);
Block fpc_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size);
std::vector<std::uint8_t> bdi_encode(const Block& block);
Block bdi_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size);
std::vector<std::uint8_t> cpack_encode(const Block& block);
Block cpack_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size);
std::vector<std::uint8_t> bpc_encode(const Block& block);
Block bpc_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size);
std::vector<std::uint8_t> lz77_lite_encode(const Block& block);
Block lz77_lite_decode(const std::vector<std::uint8_t>& encoded,
                       std::size_t output_size);
std::vector<std::uint8_t> lz4_encode(const Block& block);
Block lz4_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size);
std::vector<std::uint8_t> zstd_encode(const Block& block);
Block zstd_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size);
std::vector<std::uint8_t> huffman_encode(const Block& block);
Block huffman_decode(const std::vector<std::uint8_t>& encoded,
                     std::size_t output_size);

// Returns the smaller of an actually encoded representation and a raw cache
// block. Choosing raw versus compressed is external per-block metadata.
std::size_t fpc_encoded_size(const Block& block);
std::size_t bdi_encoded_size(const Block& block);
std::size_t cpack_encoded_size(const Block& block);
std::size_t bpc_encoded_size(const Block& block);
std::size_t zstd_encoded_size(const Block& block);
std::size_t lz4_encoded_size(const Block& block);
std::size_t lz77_lite_encoded_size(const Block& block);
std::size_t huffman_encoded_size(const Block& block);
std::size_t baseline_encoded_size(const Block& block, BaselineKind kind);

BaselineEvaluation evaluate_baseline(const std::vector<Block>& blocks,
                                     BaselineKind kind,
                                     std::vector<std::size_t> target_sizes);

}  // namespace bsel
