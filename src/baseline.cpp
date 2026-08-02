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

std::size_t rounded_up_bytes(std::size_t bits) { return (bits + 7U) / 8U; }

bool fits_signed(std::int64_t value, unsigned bits) {
    const std::int64_t limit = std::int64_t{1} << (bits - 1U);
    return value >= -limit && value <= limit - 1;
}

unsigned ceil_log2(std::size_t value) {
    unsigned bits = 0;
    while ((std::size_t{1} << bits) < value) {
        ++bits;
    }
    return bits;
}

unsigned count_trailing_zeros(std::uint32_t value) {
    unsigned count = 0;
    while ((value & 1U) == 0) {
        value >>= 1U;
        ++count;
    }
    return count;
}

unsigned popcount(std::uint32_t value) {
    unsigned count = 0;
    while (value != 0) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
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
    if (text == "cpack") {
        return BaselineKind::Cpack;
    }
    if (text == "bpc") {
        return BaselineKind::Bpc;
    }
    throw std::invalid_argument("baseline must be fpc, bdi, hybrid, cpack, or bpc");
}

const char* baseline_kind_name(BaselineKind kind) {
    switch (kind) {
        case BaselineKind::Fpc:
            return "fpc";
        case BaselineKind::Bdi:
            return "bdi";
        case BaselineKind::HybridBdiFpc:
            return "hybrid";
        case BaselineKind::Cpack:
            return "cpack";
        case BaselineKind::Bpc:
            return "bpc";
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

std::size_t cpack_encoded_size(const Block& block) {
    // C-Pack (Chen et al., TVLSI 18(8), 2010) treats a cache line as 4-byte
    // words and encodes each word with one of six patterns (z = zero byte,
    // m = byte matched against the dictionary, x = unmatched byte):
    //   zzzz -> 00 (2 bits)
    //   xxxx -> 01 + 4 bytes (34 bits)
    //   mmmm -> 10 + 4-bit dictionary index (6 bits)
    //   mmxx -> 1100 + 4-bit index + 2 bytes (24 bits)
    //   zzzx -> 1101 + 1 byte (12 bits)
    //   mmmx -> 1110 + 4-bit index + 1 byte (16 bits)
    // The authors' implementation uses a 64-byte (16-entry) FIFO dictionary,
    // so dictionary indices are four bits and metadata totals 2-8 bits per
    // word. Static patterns are checked first; every other word is pushed
    // into the dictionary (FIFO replacement).
    if (block.empty() || block.size() % 4 != 0) {
        throw std::invalid_argument(
            "C-Pack requires a non-empty block divisible by four bytes");
    }
    constexpr std::size_t kDictionaryEntries = 16;
    constexpr std::size_t kIndexBits = 4;
    std::vector<std::uint32_t> dictionary;
    dictionary.reserve(kDictionaryEntries);
    std::size_t bits = 0;
    for (std::size_t offset = 0; offset < block.size(); offset += 4) {
        const auto word = static_cast<std::uint32_t>(
            load_little_endian(block, offset, 4));
        const auto byte0 = static_cast<std::uint8_t>(word & 0xffU);
        const auto byte1 = static_cast<std::uint8_t>((word >> 8U) & 0xffU);
        const auto byte2 = static_cast<std::uint8_t>((word >> 16U) & 0xffU);
        const auto byte3 = static_cast<std::uint8_t>((word >> 24U) & 0xffU);

        if (word == 0) {
            bits += 2;
            continue;
        }
        if (byte1 == 0 && byte2 == 0 && byte3 == 0) {
            bits += 4 + 8;
            continue;
        }

        // Pick the entry with the most matched bytes, counting from the most
        // significant byte: 4 matches -> mmmm, 3 -> mmmx, 2 -> mmxx, fewer
        // than 2 -> xxxx.
        int best_matches = 1;
        for (const auto entry : dictionary) {
            const auto e0 = static_cast<std::uint8_t>(entry & 0xffU);
            const auto e1 = static_cast<std::uint8_t>((entry >> 8U) & 0xffU);
            const auto e2 = static_cast<std::uint8_t>((entry >> 16U) & 0xffU);
            const auto e3 = static_cast<std::uint8_t>((entry >> 24U) & 0xffU);
            int matches = 0;
            if (byte3 == e3 && byte2 == e2) {
                matches = 2;
                if (byte1 == e1) {
                    matches = 3;
                    if (byte0 == e0) {
                        matches = 4;
                    }
                }
            }
            best_matches = std::max(best_matches, matches);
        }
        if (best_matches == 4) {
            bits += 2 + kIndexBits;
        } else if (best_matches == 3) {
            bits += 4 + kIndexBits + 8;
        } else if (best_matches == 2) {
            bits += 4 + kIndexBits + 16;
        } else {
            bits += 2 + 32;
        }

        if (dictionary.size() < kDictionaryEntries) {
            dictionary.push_back(word);
        } else {
            dictionary.erase(dictionary.begin());
            dictionary.push_back(word);
        }
    }
    return std::min(rounded_up_bytes(bits), block.size());
}

std::size_t bpc_encoded_size(const Block& block) {
    // Bit-Plane Compression (Kim et al., ISCA 2016) transforms a block of
    // 32-bit symbols into a base symbol plus neighboring deltas, rotates the
    // deltas into 32 bit-planes, XORs adjacent planes (DBX), and encodes the
    // planes with the paper's fixed codes (Table 3b):
    //   base symbol: {3'b000} / {3'b001,4b} / {3'b010,8b} / {3'b011,16b} /
    //                {1'b1,32b} when it is zero or fits signed 4/8/16 bits
    //   zero plane run of 1:    3 bits {3'b001}
    //   zero plane run of 2-33: 7 bits {2'b01,(run-2)[4:0]}
    //   all-ones plane:         5 bits {5'b00000}
    //   DBX != 0 with zero DBP: 5 bits {5'b00001}
    //   two consecutive ones:   10 bits {5'b00010,position}
    //   single one:             10 bits {5'b00011,position}
    //   other planes:           1 + plane width bits
    // Position fields use ceil(log2(symbol count)) bits, which reproduces the
    // paper's 5-bit fields for its 32-symbol (128-byte) blocks.
    if (block.size() < 8 || block.size() % 4 != 0) {
        throw std::invalid_argument(
            "BPC requires a block of at least two 32-bit symbols");
    }
    const auto symbols = values_for_width(block, 4);
    const std::size_t symbol_count = symbols.size();
    if (symbol_count > 32) {
        throw std::invalid_argument(
            "BPC estimator supports blocks of at most 32 symbols (128 bytes)");
    }
    const std::size_t plane_width = symbol_count - 1;
    const std::uint32_t plane_mask = (1U << plane_width) - 1U;
    const std::size_t position_width = ceil_log2(symbol_count);

    std::size_t bits = 0;
    const auto base = static_cast<std::int64_t>(symbols.front());
    if (base == 0) {
        bits += 3;
    } else if (fits_signed(base, 4)) {
        bits += 3 + 4;
    } else if (fits_signed(base, 8)) {
        bits += 3 + 8;
    } else if (fits_signed(base, 16)) {
        bits += 3 + 16;
    } else {
        bits += 1 + 32;
    }

    // Neighboring deltas wrap modulo 2^32, matching the paper's fixed-width
    // subtraction.
    std::vector<std::uint32_t> deltas(plane_width);
    for (std::size_t i = 1; i < symbol_count; ++i) {
        deltas[i - 1] = static_cast<std::uint32_t>(symbols[i]) -
                        static_cast<std::uint32_t>(symbols[i - 1]);
    }

    // Bit-plane j holds bit j of every delta; plane 31 is the base plane
    // (the sign bits). DBX_j = plane_j XOR plane_{j+1}. Planes are emitted
    // from the base plane downward so the decoder can recover each plane
    // from the previously decoded one.
    std::array<std::uint32_t, 32> planes{};
    for (unsigned bit = 0; bit < 32; ++bit) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < plane_width; ++i) {
            value |= ((deltas[i] >> bit) & 1U) << i;
        }
        planes[bit] = value;
    }

    struct CodedPlane {
        bool is_zero = false;
        std::size_t bits = 0;
    };
    std::vector<CodedPlane> coded;
    coded.reserve(32);

    auto encode_plane = [&](std::uint32_t dbx, std::uint32_t dbp) {
        CodedPlane entry;
        if (dbx == 0) {
            entry.is_zero = true;
            coded.push_back(entry);
            return;
        }
        if (dbx == plane_mask) {
            entry.bits = 5;
        } else if (dbp == 0) {
            entry.bits = 5;
        } else {
            const auto first = count_trailing_zeros(dbx);
            const auto ones = popcount(dbx);
            if (ones == 2 && ((dbx >> (first + 1U)) & 1U) != 0) {
                entry.bits = 5 + position_width;
            } else if (ones == 1) {
                entry.bits = 5 + position_width;
            } else {
                entry.bits = 1 + plane_width;
            }
        }
        coded.push_back(entry);
    };

    // Base plane (signs) first, then DBX planes from the top down.
    encode_plane(planes[31], planes[31]);
    for (unsigned bit = 31; bit-- > 0;) {
        encode_plane(planes[bit] ^ planes[bit + 1U], planes[bit + 1U]);
    }

    // Merge consecutive zero planes into ZBP-RLE codes.
    std::size_t run = 0;
    for (std::size_t i = 0; i <= coded.size(); ++i) {
        const bool is_last = i == coded.size();
        const bool is_zero = !is_last && coded[i].is_zero;
        if (is_zero) {
            ++run;
            continue;
        }
        while (run > 0) {
            if (run == 1) {
                bits += 3;
                run = 0;
            } else {
                const auto chunk = std::min<std::size_t>(run, 33);
                bits += 2 + 5;
                run -= chunk;
            }
        }
        if (!is_last) {
            bits += coded[i].bits;
        }
    }

    return std::min(rounded_up_bytes(bits), block.size());
}

std::size_t baseline_encoded_size(const Block& block, BaselineKind kind) {
    switch (kind) {
        case BaselineKind::Fpc:
            return fpc_encoded_size(block);
        case BaselineKind::Bdi:
            return bdi_encoded_size(block);
        case BaselineKind::HybridBdiFpc:
            return std::min(fpc_encoded_size(block), bdi_encoded_size(block));
        case BaselineKind::Cpack:
            return cpack_encoded_size(block);
        case BaselineKind::Bpc:
            return bpc_encoded_size(block);
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
