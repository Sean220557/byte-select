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

    std::size_t dictionary_size() const { return target_size - metadata_bytes; }
};

struct Model {
    std::size_t block_size = 64;
    std::vector<PatternSet> sets;
};

struct EncodedBlock {
    bool compressed = false;
    std::size_t original_size = 0;
    std::size_t allocated_size = 0;
    std::size_t set_index = 0;
    std::size_t pattern_index = 0;
    Block dictionary;
    Block raw;
};

struct Evaluation {
    std::uint64_t blocks = 0;
    std::uint64_t original_bytes = 0;
    std::uint64_t allocated_bytes = 0;
    std::uint64_t compressed_blocks = 0;
    double compression_ratio() const;
};

void validate_model(const Model& model);
std::optional<Block> make_dictionary(const Block& block, const Pattern& pattern);
EncodedBlock compress(const Block& block, const Model& model);
Block decompress(const EncodedBlock& encoded, const Model& model);
Evaluation evaluate(const std::vector<Block>& blocks, const Model& model);

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
