#include "byte_select/codec.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace bsel {

void validate_model(const Model& model) {
    if (model.block_size == 0 || model.sets.empty()) {
        throw std::invalid_argument("model must have a block size and pattern sets");
    }
    for (const auto& set : model.sets) {
        if (set.target_size == 0 || set.metadata_bytes >= set.target_size) {
            throw std::invalid_argument("model has invalid target/metadata sizes");
        }
        std::size_t capacity = 1;
        for (std::size_t i = 0; i < set.metadata_bytes; ++i) {
            if (capacity > std::numeric_limits<std::size_t>::max() / 256U) {
                capacity = std::numeric_limits<std::size_t>::max();
                break;
            }
            capacity *= 256U;
        }
        if (set.patterns.size() > capacity) {
            throw std::invalid_argument("pattern count exceeds metadata capacity");
        }
        for (const auto& pattern : set.patterns) {
            if (pattern.size() != model.block_size ||
                pattern.rank() > set.dictionary_size()) {
                throw std::invalid_argument("pattern is incompatible with model dimensions");
            }
        }
    }
}

double Evaluation::compression_ratio() const {
    return allocated_bytes == 0 ? 0.0
                                : static_cast<double>(original_bytes) /
                                      static_cast<double>(allocated_bytes);
}

std::optional<Block> make_dictionary(const Block& block, const Pattern& pattern) {
    if (!pattern.describes(block)) {
        return std::nullopt;
    }
    Block dictionary(pattern.rank());
    std::vector<bool> assigned(pattern.rank(), false);
    for (std::size_t i = 0; i < block.size(); ++i) {
        const auto symbol = pattern.symbols()[i];
        if (!assigned[symbol]) {
            dictionary[symbol] = block[i];
            assigned[symbol] = true;
        }
    }
    return dictionary;
}

EncodedBlock compress(const Block& block, const Model& model) {
    if (block.size() != model.block_size) {
        throw std::invalid_argument("input block has incorrect size");
    }
    EncodedBlock result;
    result.original_size = block.size();
    result.allocated_size = block.size();
    result.raw = block;

    std::vector<std::size_t> order(model.sets.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return model.sets[a].target_size < model.sets[b].target_size;
    });

    for (const auto set_index : order) {
        const auto& set = model.sets[set_index];
        if (set.metadata_bytes >= set.target_size) {
            throw std::invalid_argument("model has invalid target/metadata sizes");
        }
        for (std::size_t pattern_index = 0; pattern_index < set.patterns.size();
             ++pattern_index) {
            auto dictionary = make_dictionary(block, set.patterns[pattern_index]);
            if (dictionary && dictionary->size() <= set.dictionary_size()) {
                dictionary->resize(set.dictionary_size(), 0);
                result.compressed = true;
                result.allocated_size = set.target_size;
                result.set_index = set_index;
                result.pattern_index = pattern_index;
                result.dictionary = std::move(*dictionary);
                result.raw.clear();
                return result;
            }
        }
    }
    return result;
}

Block decompress(const EncodedBlock& encoded, const Model& model) {
    if (!encoded.compressed) {
        if (encoded.raw.size() != encoded.original_size) {
            throw std::invalid_argument("invalid raw encoded block");
        }
        return encoded.raw;
    }
    if (encoded.set_index >= model.sets.size()) {
        throw std::invalid_argument("encoded set index is out of range");
    }
    const auto& set = model.sets[encoded.set_index];
    if (encoded.pattern_index >= set.patterns.size()) {
        throw std::invalid_argument("encoded pattern index is out of range");
    }
    const auto& pattern = set.patterns[encoded.pattern_index];
    if (pattern.size() != encoded.original_size ||
        encoded.dictionary.size() < pattern.rank()) {
        throw std::invalid_argument("encoded dictionary is inconsistent with pattern");
    }
    Block output(pattern.size());
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = encoded.dictionary[pattern.symbols()[i]];
    }
    return output;
}

Evaluation evaluate(const std::vector<Block>& blocks, const Model& model) {
    Evaluation result;
    for (const auto& block : blocks) {
        const auto encoded = compress(block, model);
        ++result.blocks;
        result.original_bytes += block.size();
        result.allocated_bytes += encoded.allocated_size;
        result.compressed_blocks += encoded.compressed ? 1U : 0U;
    }
    return result;
}

OnlineCompressor::OnlineCompressor(Model model) : model_(std::move(model)) {
    validate_model(model_);
}

EncodedBlock OnlineCompressor::compress_block(const Block& block) const {
    return bsel::compress(block, model_);
}

OnlineDecompressor::OnlineDecompressor(Model model) : model_(std::move(model)) {
    validate_model(model_);
}

Block OnlineDecompressor::decompress_block(const EncodedBlock& encoded) const {
    return bsel::decompress(encoded, model_);
}

}  // namespace bsel
