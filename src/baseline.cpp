#include "byte_select/baseline.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace bsel {
namespace {

std::uint64_t load_little_endian(const Block& block, std::size_t offset,
                                 std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(block[offset + i]) << (8U * i);
    }
    return value;
}

std::vector<std::uint64_t> values_for_width(const Block& block, std::size_t width) {
    if (width == 0 || block.size() % width != 0) {
        throw std::invalid_argument("block size is not divisible by baseline value width");
    }
    std::vector<std::uint64_t> values;
    values.reserve(block.size() / width);
    for (std::size_t offset = 0; offset < block.size(); offset += width) {
        values.push_back(load_little_endian(block, offset, width));
    }
    return values;
}

bool is_sign_extended(std::uint32_t value, unsigned payload_bits) {
    const std::uint32_t payload_mask = (1U << payload_bits) - 1U;
    const std::uint32_t payload = value & payload_mask;
    const std::uint32_t sign_bit = 1U << (payload_bits - 1U);
    const std::uint32_t extended =
        (payload & sign_bit) == 0 ? payload : payload | ~payload_mask;
    return value == extended;
}

bool halfword_is_sign_extended_byte(std::uint16_t value) {
    const auto signed_value = static_cast<std::int16_t>(value);
    return signed_value >= -128 && signed_value <= 127;
}

unsigned fpc_payload_bits(std::uint32_t value) {
    if (value == 0) {
        return 0;
    }
    if (is_sign_extended(value, 4)) {
        return 4;
    }
    const std::uint8_t byte0 = static_cast<std::uint8_t>(value);
    if (((value >> 8U) & 0xffU) == byte0 && ((value >> 16U) & 0xffU) == byte0 &&
        ((value >> 24U) & 0xffU) == byte0) {
        return 8;
    }
    if (is_sign_extended(value, 8)) {
        return 8;
    }
    if (is_sign_extended(value, 16)) {
        return 16;
    }
    if ((value & 0xffffU) == 0) {
        return 16;
    }
    if (halfword_is_sign_extended_byte(static_cast<std::uint16_t>(value)) &&
        halfword_is_sign_extended_byte(static_cast<std::uint16_t>(value >> 16U))) {
        return 16;
    }
    return 32;
}

bool all_equal(const std::vector<std::uint64_t>& values) {
    return std::all_of(std::next(values.begin()), values.end(),
                       [&](std::uint64_t value) { return value == values.front(); });
}

bool delta_range_fits(const std::vector<std::uint64_t>& values,
                      std::size_t delta_bytes) {
    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    const auto span = static_cast<unsigned __int128>(*maximum) - *minimum;
    const auto limit = (static_cast<unsigned __int128>(1) << (delta_bytes * 8U)) - 1U;
    return span <= limit;
}

std::size_t quantized_size(std::size_t encoded_size, std::size_t block_size,
                           const std::vector<std::size_t>& targets,
                           std::size_t* target_index) {
    if (encoded_size >= block_size) {
        return block_size;
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (encoded_size <= targets[i]) {
            if (target_index != nullptr) {
                *target_index = i;
            }
            return targets[i];
        }
    }
    return block_size;
}

}  // namespace

double BaselineEvaluation::unquantized_compression_ratio() const {
    return encoded_bytes == 0 ? 0.0
                              : static_cast<double>(original_bytes) /
                                    static_cast<double>(encoded_bytes);
}

double BaselineEvaluation::quantized_compression_ratio() const {
    return allocated_bytes == 0 ? 0.0
                                : static_cast<double>(original_bytes) /
                                      static_cast<double>(allocated_bytes);
}

BaselineKind parse_baseline_kind(const std::string& text) {
    if (text == "fpc") {
        return BaselineKind::Fpc;
    }
    if (text == "bdi") {
        return BaselineKind::Bdi;
    }
    if (text == "hybrid") {
        return BaselineKind::HybridBdiFpc;
    }
    throw std::invalid_argument("baseline must be fpc, bdi, or hybrid");
}

const char* baseline_kind_name(BaselineKind kind) {
    switch (kind) {
        case BaselineKind::Fpc:
            return "fpc";
        case BaselineKind::Bdi:
            return "bdi";
        case BaselineKind::HybridBdiFpc:
            return "hybrid";
    }
    throw std::invalid_argument("unknown baseline kind");
}

std::size_t fpc_encoded_size(const Block& block) {
    if (block.empty() || block.size() % 4 != 0) {
        throw std::invalid_argument("FPC requires a non-empty block divisible by four bytes");
    }
    const auto word_count = block.size() / 4;
    std::size_t bits = word_count * 3;
    for (std::size_t offset = 0; offset < block.size(); offset += 4) {
        bits += fpc_payload_bits(static_cast<std::uint32_t>(
            load_little_endian(block, offset, 4)));
    }
    const auto bytes = (bits + 7U) / 8U;
    return std::min(bytes, block.size());
}

std::size_t bdi_encoded_size(const Block& block) {
    if (block.empty() || block.size() % 8 != 0) {
        throw std::invalid_argument("BDI requires a non-empty block divisible by eight bytes");
    }
    std::size_t best = block.size();
    const auto values8 = values_for_width(block, 8);
    if (std::all_of(values8.begin(), values8.end(),
                    [](std::uint64_t value) { return value == 0; })) {
        best = 1;
    }
    if (all_equal(values8)) {
        best = std::min(best, std::size_t{8});
    }

    const std::array<std::array<std::size_t, 3>, 6> modes{{
        {{8, 1, 8}}, {{8, 2, 8}}, {{8, 4, 8}},
        {{4, 1, 4}}, {{4, 2, 4}}, {{2, 1, 2}},
    }};
    for (const auto& mode : modes) {
        const auto value_bytes = mode[0];
        const auto delta_bytes = mode[1];
        const auto base_bytes = mode[2];
        const auto values = values_for_width(block, value_bytes);
        if (delta_range_fits(values, delta_bytes)) {
            best = std::min(best, base_bytes + values.size() * delta_bytes);
        }
    }
    return best;
}

std::size_t baseline_encoded_size(const Block& block, BaselineKind kind) {
    switch (kind) {
        case BaselineKind::Fpc:
            return fpc_encoded_size(block);
        case BaselineKind::Bdi:
            return bdi_encoded_size(block);
        case BaselineKind::HybridBdiFpc:
            return std::min(fpc_encoded_size(block), bdi_encoded_size(block));
    }
    throw std::invalid_argument("unknown baseline kind");
}

BaselineEvaluation evaluate_baseline(const std::vector<Block>& blocks,
                                     BaselineKind kind,
                                     std::vector<std::size_t> target_sizes) {
    BaselineEvaluation result;
    result.kind = kind;
    std::sort(target_sizes.begin(), target_sizes.end());
    target_sizes.erase(std::unique(target_sizes.begin(), target_sizes.end()),
                       target_sizes.end());
    if (blocks.empty()) {
        result.target_sizes = std::move(target_sizes);
        result.target_blocks.resize(result.target_sizes.size(), 0);
        return result;
    }

    const auto block_size = blocks.front().size();
    if (block_size == 0) {
        throw std::invalid_argument("baseline blocks must be non-empty");
    }
    for (const auto target_size : target_sizes) {
        if (target_size == 0 || target_size >= block_size) {
            throw std::invalid_argument("baseline target must be smaller than block size");
        }
    }
    result.target_sizes = std::move(target_sizes);
    result.target_blocks.resize(result.target_sizes.size(), 0);

    for (const auto& block : blocks) {
        if (block.size() != block_size) {
            throw std::invalid_argument("baseline blocks have inconsistent sizes");
        }
        const auto encoded_size = baseline_encoded_size(block, kind);
        std::size_t target_index = 0;
        const auto allocated_size =
            quantized_size(encoded_size, block_size, result.target_sizes, &target_index);
        ++result.blocks;
        result.original_bytes += block_size;
        result.encoded_bytes += encoded_size;
        result.allocated_bytes += allocated_size;
        if (allocated_size < block_size) {
            ++result.compressed_blocks;
            ++result.target_blocks[target_index];
        }
    }
    return result;
}

}  // namespace bsel
