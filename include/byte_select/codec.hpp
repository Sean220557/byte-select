#pragma once

#include "byte_select/pattern.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bsel {

struct PatternSet {
    std::size_t target_size = 32;
    std::size_t metadata_bytes = 1;
    std::vector<Pattern> patterns;
    // The high metadata_tag_bits encode a target-class tag. Pattern indices
    // occupy the remaining low bits. This is used by the paper's multi-size
    // BSel-1024-1024-128 format.
    std::size_t metadata_tag_bits = 0;
    std::size_t metadata_tag_value = 0;

    std::size_t dictionary_size() const { return target_size - metadata_bytes; }
    std::size_t metadata_bits() const { return metadata_bytes * 8U; }
    std::size_t pattern_id_bits() const { return metadata_bits() - metadata_tag_bits; }
};

struct Model {
    std::size_t block_size = 64;
    std::vector<PatternSet> sets;
};

struct EncodedBlock {
    bool compressed = false;
    std::size_t original_size = 0;
    std::size_t encoded_size = 0;
    std::size_t allocated_size = 0;
    std::size_t set_index = 0;
    std::size_t pattern_index = 0;
    std::size_t metadata = 0;
    Block dictionary;
    Block raw;
};

struct Evaluation {
    std::uint64_t blocks = 0;
    std::uint64_t original_bytes = 0;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t allocated_bytes = 0;
    std::uint64_t compressed_blocks = 0;
    std::vector<std::uint64_t> set_blocks;

    double unquantized_compression_ratio() const;
    double compression_ratio() const;
};

struct UpperBoundEvaluation {
    std::uint64_t blocks = 0;
    std::uint64_t original_bytes = 0;
    std::uint64_t unique_bytes = 0;
    std::uint64_t unique_with_metadata_bytes = 0;
    std::uint64_t quantized_bytes = 0;
    std::uint64_t compressed_blocks = 0;
    std::size_t metadata_bytes = 0;
    std::vector<std::size_t> target_sizes;
    std::vector<std::uint64_t> target_blocks;

    double unique_compression_ratio() const;
    double unique_with_metadata_compression_ratio() const;
    double quantized_compression_ratio() const;
};

void validate_model(const Model& model);
std::optional<Block> make_dictionary(const Block& block, const Pattern& pattern);
std::size_t encode_pattern_metadata(const PatternSet& set, std::size_t pattern_index);
std::size_t decode_pattern_metadata(const PatternSet& set, std::size_t metadata);
EncodedBlock compress(const Block& block, const Model& model);
Block decompress(const EncodedBlock& encoded, const Model& model);
Evaluation evaluate(const std::vector<Block>& blocks, const Model& model);
UpperBoundEvaluation evaluate_byte_select_upper_bound(
    const std::vector<Block>& blocks, std::vector<std::size_t> target_sizes,
    std::size_t metadata_bytes);

class OnlineCompressor {
public:
    explicit OnlineCompressor(Model model);
    EncodedBlock compress_block(const Block& block) const;
    const Model& model() const { return model_; }

private:
    Model model_;
};

class OnlineDecompressor {
public:
    explicit OnlineDecompressor(Model model);
    Block decompress_block(const EncodedBlock& encoded) const;

private:
    Model model_;
};

}  // namespace bsel
