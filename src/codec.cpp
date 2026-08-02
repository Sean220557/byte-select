#include "byte_select/codec.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace bsel {
namespace {

bool value_fits_bits(std::size_t value, std::size_t bits) {
    if (bits >= std::numeric_limits<std::size_t>::digits) {
        return true;
    }
    return value < (std::size_t{1} << bits);
}

bool count_fits_bits(std::size_t count, std::size_t bits) {
    if (bits >= std::numeric_limits<std::size_t>::digits) {
        return true;
    }
    return count <= (std::size_t{1} << bits);
}

std::size_t low_bits_mask(std::size_t bits) {
    if (bits >= std::numeric_limits<std::size_t>::digits) {
        return std::numeric_limits<std::size_t>::max();
    }
    return (std::size_t{1} << bits) - 1U;
}

void validate_metadata_layout(const PatternSet& set) {
    if (set.metadata_bytes > std::numeric_limits<std::size_t>::max() / 8U) {
        throw std::invalid_argument("metadata width is too large");
    }
    const auto metadata_bits = set.metadata_bytes * 8U;
    if (set.metadata_tag_bits > metadata_bits ||
        set.metadata_tag_bits > std::numeric_limits<std::size_t>::digits) {
        throw std::invalid_argument("metadata tag layout is invalid");
    }
    const auto pattern_id_bits = metadata_bits - set.metadata_tag_bits;
    if ((set.metadata_tag_bits != 0 &&
         pattern_id_bits >= std::numeric_limits<std::size_t>::digits) ||
        !value_fits_bits(set.metadata_tag_value, set.metadata_tag_bits)) {
        throw std::invalid_argument("metadata tag layout is invalid");
    }
}

}  // namespace

void validate_model(const Model& model) {
    if (model.block_size == 0 || model.sets.empty()) {
        throw std::invalid_argument("model must have a block size and pattern sets");
    }
    std::unordered_set<std::size_t> target_sizes;
    for (const auto& set : model.sets) {
        if (set.target_size == 0 || set.metadata_bytes >= set.target_size) {
            throw std::invalid_argument("model has invalid target/metadata sizes");
        }
        if (set.target_size >= model.block_size) {
            throw std::invalid_argument("compressed target must be smaller than block size");
        }
        if (set.metadata_bytes > std::numeric_limits<std::size_t>::max() / 8U ||
            set.metadata_tag_bits > set.metadata_bits() ||
            set.metadata_tag_bits > std::numeric_limits<std::size_t>::digits ||
            (set.metadata_tag_bits != 0 &&
             set.pattern_id_bits() >= std::numeric_limits<std::size_t>::digits) ||
            !value_fits_bits(set.metadata_tag_value, set.metadata_tag_bits)) {
            throw std::invalid_argument("model has invalid metadata tag layout");
        }
        if (!target_sizes.insert(set.target_size).second) {
            throw std::invalid_argument("model has duplicate target sizes");
        }
        if (!count_fits_bits(set.patterns.size(), set.pattern_id_bits())) {
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

double Evaluation::unquantized_compression_ratio() const {
    return encoded_bytes == 0 ? 0.0
                              : static_cast<double>(original_bytes) /
                                    static_cast<double>(encoded_bytes);
}

double Evaluation::compression_ratio() const {
    return allocated_bytes == 0 ? 0.0
                                : static_cast<double>(original_bytes) /
                                      static_cast<double>(allocated_bytes);
}

double UpperBoundEvaluation::unique_compression_ratio() const {
    return unique_bytes == 0 ? 0.0
                             : static_cast<double>(original_bytes) /
                                   static_cast<double>(unique_bytes);
}

double UpperBoundEvaluation::unique_with_metadata_compression_ratio() const {
    return unique_with_metadata_bytes == 0
               ? 0.0
               : static_cast<double>(original_bytes) /
                     static_cast<double>(unique_with_metadata_bytes);
}

double UpperBoundEvaluation::quantized_compression_ratio() const {
    return quantized_bytes == 0 ? 0.0
                                : static_cast<double>(original_bytes) /
                                      static_cast<double>(quantized_bytes);
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

std::size_t encode_pattern_metadata(const PatternSet& set, std::size_t pattern_index) {
    validate_metadata_layout(set);
    if (pattern_index >= set.patterns.size()) {
        throw std::invalid_argument("pattern index is not present in the pattern set");
    }
    if (!value_fits_bits(pattern_index, set.pattern_id_bits())) {
        throw std::invalid_argument("pattern index exceeds metadata capacity");
    }
    if (set.metadata_tag_bits == 0) {
        return pattern_index;
    }
    return (set.metadata_tag_value << set.pattern_id_bits()) | pattern_index;
}

std::size_t decode_pattern_metadata(const PatternSet& set, std::size_t metadata) {
    validate_metadata_layout(set);
    if (!value_fits_bits(metadata, set.metadata_bits())) {
        throw std::invalid_argument("metadata exceeds its declared width");
    }
    if (set.metadata_tag_bits == 0) {
        if (metadata >= set.patterns.size()) {
            throw std::invalid_argument("metadata identifies no pattern in the pattern set");
        }
        return metadata;
    }
    const auto pattern_index = metadata & low_bits_mask(set.pattern_id_bits());
    const auto tag = metadata >> set.pattern_id_bits();
    if (tag != set.metadata_tag_value) {
        throw std::invalid_argument("metadata tag does not match target pattern set");
    }
    if (pattern_index >= set.patterns.size()) {
        throw std::invalid_argument("metadata identifies no pattern in the pattern set");
    }
    return pattern_index;
}

EncodedBlock compress(const Block& block, const Model& model) {
    if (block.size() != model.block_size) {
        throw std::invalid_argument("input block has incorrect size");
    }
    EncodedBlock result;
    result.original_size = block.size();
    result.encoded_size = block.size();
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
                result.encoded_size = set.metadata_bytes + set.patterns[pattern_index].rank();
                result.allocated_size = set.target_size;
                result.set_index = set_index;
                result.pattern_index = pattern_index;
                result.metadata = encode_pattern_metadata(set, pattern_index);
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
    if (encoded.metadata != encode_pattern_metadata(set, encoded.pattern_index)) {
        throw std::invalid_argument("encoded metadata is inconsistent with pattern index");
    }
    const auto& pattern = set.patterns[encoded.pattern_index];
    if (pattern.size() != encoded.original_size ||
        encoded.dictionary.size() != set.dictionary_size()) {
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
    result.set_blocks.resize(model.sets.size(), 0);
    for (const auto& block : blocks) {
        const auto encoded = compress(block, model);
        ++result.blocks;
        result.original_bytes += block.size();
        result.encoded_bytes += encoded.encoded_size;
        result.allocated_bytes += encoded.allocated_size;
        result.compressed_blocks += encoded.compressed ? 1U : 0U;
        if (encoded.compressed) {
            ++result.set_blocks[encoded.set_index];
        }
    }
    return result;
}

UpperBoundEvaluation evaluate_byte_select_upper_bound(
    const std::vector<Block>& blocks, std::vector<std::size_t> target_sizes,
    std::size_t metadata_bytes) {
    UpperBoundEvaluation result;
    result.metadata_bytes = metadata_bytes;
    if (blocks.empty()) {
        std::sort(target_sizes.begin(), target_sizes.end());
        target_sizes.erase(std::unique(target_sizes.begin(), target_sizes.end()),
                           target_sizes.end());
        result.target_sizes = std::move(target_sizes);
        result.target_blocks.resize(result.target_sizes.size(), 0);
        return result;
    }

    const auto block_size = blocks.front().size();
    if (block_size == 0) {
        throw std::invalid_argument("upper-bound blocks must be non-empty");
    }
    std::sort(target_sizes.begin(), target_sizes.end());
    target_sizes.erase(std::unique(target_sizes.begin(), target_sizes.end()),
                       target_sizes.end());
    for (const auto target_size : target_sizes) {
        if (target_size == 0 || target_size >= block_size ||
            metadata_bytes >= target_size) {
            throw std::invalid_argument("invalid upper-bound target size");
        }
    }
    result.target_sizes = std::move(target_sizes);
    result.target_blocks.resize(result.target_sizes.size(), 0);

    for (const auto& block : blocks) {
        if (block.size() != block_size) {
            throw std::invalid_argument("upper-bound blocks have inconsistent sizes");
        }
        const auto unique = Pattern::simplest(block).rank();
        const auto unique_with_metadata =
            unique > block_size - std::min(metadata_bytes, block_size)
                ? block_size
                : std::min(block_size, unique + metadata_bytes);

        ++result.blocks;
        result.original_bytes += block_size;
        result.unique_bytes += unique;
        result.unique_with_metadata_bytes += unique_with_metadata;

        std::size_t allocated = block_size;
        for (std::size_t i = 0; i < result.target_sizes.size(); ++i) {
            if (unique <= result.target_sizes[i] - metadata_bytes) {
                allocated = result.target_sizes[i];
                ++result.target_blocks[i];
                ++result.compressed_blocks;
                break;
            }
        }
        result.quantized_bytes += allocated;
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
