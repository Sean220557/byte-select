#include "byte_select/baseline.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <queue>
#include <stdexcept>
#include <utility>

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

class BitWriter {
public:
    void put(std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            if ((bits_ & 7U) == 0) {
                bytes_.push_back(0);
            }
            if (((value >> i) & 1U) != 0) {
                bytes_.back() |= static_cast<std::uint8_t>(1U << (bits_ & 7U));
            }
            ++bits_;
        }
    }

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t bits_ = 0;
};

class BitReader {
public:
    explicit BitReader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    std::uint64_t get(unsigned count) {
        if (bit_ + count > bytes_.size() * 8U) {
            throw std::runtime_error("truncated baseline bitstream");
        }
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i, ++bit_) {
            value |= static_cast<std::uint64_t>(
                         (bytes_[bit_ / 8U] >> (bit_ & 7U)) & 1U)
                     << i;
        }
        return value;
    }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t bit_ = 0;
};

std::int32_t sign_extend(std::uint32_t value, unsigned bits) {
    const auto sign = std::uint32_t{1} << (bits - 1U);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

struct HuffmanNode {
    std::uint64_t frequency = 0;
    int symbol = -1;
    int left = -1;
    int right = -1;
};

std::array<std::uint8_t, 256> huffman_code_lengths(const Block& block) {
    std::array<std::uint64_t, 256> frequencies{};
    for (const auto byte : block) {
        ++frequencies[byte];
    }
    std::vector<HuffmanNode> nodes;
    using Entry = std::pair<std::uint64_t, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
    for (int symbol = 0; symbol < 256; ++symbol) {
        if (frequencies[static_cast<std::size_t>(symbol)] != 0) {
            nodes.push_back({frequencies[static_cast<std::size_t>(symbol)], symbol, -1, -1});
            heap.push({nodes.back().frequency, static_cast<int>(nodes.size() - 1)});
        }
    }
    std::array<std::uint8_t, 256> lengths{};
    if (heap.size() == 1) {
        lengths[static_cast<std::size_t>(nodes[heap.top().second].symbol)] = 1;
        return lengths;
    }
    while (heap.size() > 1) {
        const auto left = heap.top();
        heap.pop();
        const auto right = heap.top();
        heap.pop();
        nodes.push_back({left.first + right.first, -1, left.second, right.second});
        heap.push({nodes.back().frequency, static_cast<int>(nodes.size() - 1)});
    }
    std::function<void(int, unsigned)> visit = [&](int index, unsigned depth) {
        const auto& node = nodes[static_cast<std::size_t>(index)];
        if (node.symbol >= 0) {
            lengths[static_cast<std::size_t>(node.symbol)] =
                static_cast<std::uint8_t>(depth);
            return;
        }
        visit(node.left, depth + 1);
        visit(node.right, depth + 1);
    };
    if (!heap.empty()) {
        visit(heap.top().second, 0);
    }
    return lengths;
}

struct HuffmanCode {
    std::uint32_t bits = 0;
    std::uint8_t length = 0;
};

std::array<HuffmanCode, 256> canonical_huffman_codes(
    const std::array<std::uint8_t, 256>& lengths) {
    std::vector<std::pair<unsigned, unsigned>> ordered;
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (lengths[symbol] != 0) {
            ordered.push_back({lengths[symbol], symbol});
        }
    }
    std::sort(ordered.begin(), ordered.end());
    std::array<HuffmanCode, 256> result{};
    std::uint32_t code = 0;
    unsigned previous_length = 0;
    for (const auto [length, symbol] : ordered) {
        code <<= length - previous_length;
        // BitWriter is LSB-first, so reverse the canonical MSB-first code.
        std::uint32_t reversed = 0;
        for (unsigned i = 0; i < length; ++i) {
            reversed |= ((code >> (length - 1U - i)) & 1U) << i;
        }
        result[symbol] = {reversed, static_cast<std::uint8_t>(length)};
        ++code;
        previous_length = length;
    }
    return result;
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
    if (text == "huffman") {
        return BaselineKind::Huffman;
    }
    throw std::invalid_argument(
        "baseline must be fpc, bdi, hybrid, cpack, bpc, or huffman");
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
        case BaselineKind::Huffman:
            return "huffman";
    }
    throw std::invalid_argument("unknown baseline kind");
}

std::vector<std::uint8_t> fpc_encode(const Block& block) {
    if (block.empty() || block.size() % 4 != 0) {
        throw std::invalid_argument("FPC requires a non-empty block divisible by four bytes");
    }
    BitWriter writer;
    for (std::size_t offset = 0; offset < block.size(); offset += 4) {
        const auto value = static_cast<std::uint32_t>(load_little_endian(block, offset, 4));
        if (value == 0) {
            writer.put(0, 3);
        } else if (is_sign_extended(value, 4)) {
            writer.put(1, 3);
            writer.put(value & 0xfU, 4);
        } else {
            const auto byte = static_cast<std::uint8_t>(value);
            if (((value >> 8U) & 0xffU) == byte &&
                ((value >> 16U) & 0xffU) == byte &&
                ((value >> 24U) & 0xffU) == byte) {
                writer.put(2, 3);
                writer.put(byte, 8);
            } else if (is_sign_extended(value, 8)) {
                writer.put(3, 3);
                writer.put(value & 0xffU, 8);
            } else if (is_sign_extended(value, 16)) {
                writer.put(4, 3);
                writer.put(value & 0xffffU, 16);
            } else if ((value & 0xffffU) == 0) {
                writer.put(5, 3);
                writer.put(value >> 16U, 16);
            } else if (halfword_is_sign_extended_byte(static_cast<std::uint16_t>(value)) &&
                       halfword_is_sign_extended_byte(
                           static_cast<std::uint16_t>(value >> 16U))) {
                writer.put(6, 3);
                writer.put(value & 0xffU, 8);
                writer.put((value >> 16U) & 0xffU, 8);
            } else {
                writer.put(7, 3);
                writer.put(value, 32);
            }
        }
    }
    return writer.bytes();
}

Block fpc_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size) {
    if (output_size == 0 || output_size % 4 != 0) {
        throw std::invalid_argument("FPC output size must be divisible by four bytes");
    }
    BitReader reader(encoded);
    Block output;
    output.reserve(output_size);
    while (output.size() < output_size) {
        const auto tag = reader.get(3);
        std::uint32_t value = 0;
        switch (tag) {
            case 0: break;
            case 1: value = static_cast<std::uint32_t>(sign_extend(reader.get(4), 4)); break;
            case 2: {
                const auto byte = static_cast<std::uint32_t>(reader.get(8));
                value = byte * 0x01010101U;
                break;
            }
            case 3: value = static_cast<std::uint32_t>(sign_extend(reader.get(8), 8)); break;
            case 4: value = static_cast<std::uint32_t>(sign_extend(reader.get(16), 16)); break;
            case 5: value = static_cast<std::uint32_t>(reader.get(16)) << 16U; break;
            case 6: {
                const auto low = static_cast<std::uint32_t>(sign_extend(reader.get(8), 8));
                const auto high = static_cast<std::uint32_t>(sign_extend(reader.get(8), 8));
                value = (low & 0xffffU) | (high << 16U);
                break;
            }
            case 7: value = static_cast<std::uint32_t>(reader.get(32)); break;
            default: throw std::runtime_error("invalid FPC tag");
        }
        for (unsigned byte = 0; byte < 4; ++byte) {
            output.push_back(static_cast<std::uint8_t>(value >> (8U * byte)));
        }
    }
    return output;
}

std::size_t fpc_encoded_size(const Block& block) {
    return std::min(fpc_encode(block).size(), block.size());
}

std::vector<std::uint8_t> bdi_encode(const Block& block) {
    if (block.empty() || block.size() % 8 != 0) {
        throw std::invalid_argument("BDI requires a non-empty block divisible by eight bytes");
    }
    std::vector<std::uint8_t> best;
    best.reserve(block.size() + 1);
    best.push_back(0);  // raw
    best.insert(best.end(), block.begin(), block.end());
    const auto values8 = values_for_width(block, 8);
    if (std::all_of(values8.begin(), values8.end(),
                    [](std::uint64_t value) { return value == 0; })) {
        best = {1};
    }
    if (all_equal(values8)) {
        std::vector<std::uint8_t> repeated{2};
        repeated.insert(repeated.end(), block.begin(), block.begin() + 8);
        if (repeated.size() < best.size()) best = std::move(repeated);
    }

    const std::array<std::array<std::size_t, 3>, 6> modes{{
        {{8, 1, 8}}, {{8, 2, 8}}, {{8, 4, 8}},
        {{4, 1, 4}}, {{4, 2, 4}}, {{2, 1, 2}},
    }};
    for (std::size_t mode_index = 0; mode_index < modes.size(); ++mode_index) {
        const auto& mode = modes[mode_index];
        const auto value_bytes = mode[0];
        const auto delta_bytes = mode[1];
        const auto base_bytes = mode[2];
        const auto values = values_for_width(block, value_bytes);
        if (delta_range_fits(values, delta_bytes)) {
            const auto base = *std::min_element(values.begin(), values.end());
            std::vector<std::uint8_t> candidate{
                static_cast<std::uint8_t>(3 + mode_index)};
            for (std::size_t i = 0; i < base_bytes; ++i)
                candidate.push_back(static_cast<std::uint8_t>(base >> (8U * i)));
            for (const auto value : values) {
                const auto delta = value - base;
                for (std::size_t i = 0; i < delta_bytes; ++i)
                    candidate.push_back(static_cast<std::uint8_t>(delta >> (8U * i)));
            }
            if (candidate.size() < best.size()) best = std::move(candidate);
        }
    }
    return best;
}

Block bdi_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size) {
    if (encoded.empty() || output_size == 0 || output_size % 8 != 0)
        throw std::invalid_argument("invalid BDI stream or output size");
    if (encoded[0] == 0) {
        if (encoded.size() != output_size + 1) throw std::runtime_error("invalid raw BDI stream");
        return Block(encoded.begin() + 1, encoded.end());
    }
    if (encoded[0] == 1) return Block(output_size, 0);
    if (encoded[0] == 2) {
        if (encoded.size() != 9) throw std::runtime_error("invalid repeated BDI stream");
        Block out;
        while (out.size() < output_size) out.insert(out.end(), encoded.begin() + 1, encoded.end());
        return out;
    }
    const std::array<std::array<std::size_t, 3>, 6> modes{{
        {{8, 1, 8}}, {{8, 2, 8}}, {{8, 4, 8}},
        {{4, 1, 4}}, {{4, 2, 4}}, {{2, 1, 2}},
    }};
    const auto index = static_cast<std::size_t>(encoded[0] - 3);
    if (index >= modes.size()) throw std::runtime_error("invalid BDI mode");
    const auto value_bytes = modes[index][0], delta_bytes = modes[index][1],
               base_bytes = modes[index][2];
    if (output_size % value_bytes != 0 ||
        encoded.size() != 1 + base_bytes + output_size / value_bytes * delta_bytes)
        throw std::runtime_error("invalid BDI payload size");
    std::uint64_t base = 0;
    for (std::size_t i = 0; i < base_bytes; ++i) base |= std::uint64_t(encoded[1 + i]) << (8U * i);
    Block out;
    out.reserve(output_size);
    std::size_t at = 1 + base_bytes;
    for (std::size_t word = 0; word < output_size / value_bytes; ++word) {
        std::uint64_t delta = 0;
        for (std::size_t i = 0; i < delta_bytes; ++i) delta |= std::uint64_t(encoded[at++]) << (8U * i);
        const auto value = base + delta;
        for (std::size_t i = 0; i < value_bytes; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
    return out;
}

std::size_t bdi_encoded_size(const Block& block) {
    return std::min(bdi_encode(block).size(), block.size());
}

std::vector<std::uint8_t> cpack_encode(const Block& block) {
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
    BitWriter writer;
    for (std::size_t offset = 0; offset < block.size(); offset += 4) {
        const auto word = static_cast<std::uint32_t>(
            load_little_endian(block, offset, 4));
        const auto byte0 = static_cast<std::uint8_t>(word & 0xffU);
        const auto byte1 = static_cast<std::uint8_t>((word >> 8U) & 0xffU);
        const auto byte2 = static_cast<std::uint8_t>((word >> 16U) & 0xffU);
        const auto byte3 = static_cast<std::uint8_t>((word >> 24U) & 0xffU);

        if (word == 0) {
            writer.put(0, 2);
            continue;
        }
        if (byte1 == 0 && byte2 == 0 && byte3 == 0) {
            writer.put(11, 4);  // 1101 in stream order
            writer.put(byte0, 8);
            continue;
        }

        // Pick the entry with the most matched bytes, counting from the most
        // significant byte: 4 matches -> mmmm, 3 -> mmmx, 2 -> mmxx, fewer
        // than 2 -> xxxx.
        int best_matches = 1;
        std::size_t best_index = 0;
        for (std::size_t dictionary_index = 0; dictionary_index < dictionary.size(); ++dictionary_index) {
            const auto entry = dictionary[dictionary_index];
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
            if (matches > best_matches) {
                best_matches = matches;
                best_index = dictionary_index;
            }
        }
        if (best_matches == 4) {
            writer.put(1, 2);  // 10
            writer.put(best_index, kIndexBits);
        } else if (best_matches == 3) {
            writer.put(7, 4);  // 1110
            writer.put(best_index, kIndexBits);
            writer.put(byte0, 8);
        } else if (best_matches == 2) {
            writer.put(3, 4);  // 1100
            writer.put(best_index, kIndexBits);
            writer.put(word & 0xffffU, 16);
        } else {
            writer.put(2, 2);  // 01
            writer.put(word, 32);
        }

        if (dictionary.size() < kDictionaryEntries) {
            dictionary.push_back(word);
        } else {
            dictionary.erase(dictionary.begin());
            dictionary.push_back(word);
        }
    }
    return writer.bytes();
}

Block cpack_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size) {
    if (output_size == 0 || output_size % 4 != 0) throw std::invalid_argument("invalid C-Pack output size");
    constexpr std::size_t kDictionaryEntries = 16;
    BitReader reader(encoded);
    std::vector<std::uint32_t> dictionary;
    Block out;
    out.reserve(output_size);
    while (out.size() < output_size) {
        const auto first = reader.get(2);
        std::uint32_t word = 0;
        bool update = false;
        if (first == 0) {
            word = 0;
        } else if (first == 2) {
            word = static_cast<std::uint32_t>(reader.get(32));
            update = true;
        } else if (first == 1) {
            const auto index = static_cast<std::size_t>(reader.get(4));
            if (index >= dictionary.size()) throw std::runtime_error("invalid C-Pack index");
            word = dictionary[index];
            update = true;
        } else {
            const auto subtype = reader.get(2);
            if (subtype == 2) {
                word = static_cast<std::uint32_t>(reader.get(8));
            } else {
                const auto index = static_cast<std::size_t>(reader.get(4));
                if (index >= dictionary.size()) throw std::runtime_error("invalid C-Pack index");
                word = dictionary[index];
                if (subtype == 0) word = (word & 0xffff0000U) | reader.get(16);
                else if (subtype == 1) word = (word & 0xffffff00U) | reader.get(8);
                else throw std::runtime_error("invalid C-Pack subtype");
                update = true;
            }
        }
        for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(word >> (8U * i)));
        if (update) {
            if (dictionary.size() == kDictionaryEntries) dictionary.erase(dictionary.begin());
            dictionary.push_back(word);
        }
    }
    return out;
}

std::size_t cpack_encoded_size(const Block& block) {
    return std::min(cpack_encode(block).size(), block.size());
}

std::size_t bpc_encoded_size_legacy(const Block& block) {
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

std::vector<std::uint8_t> bpc_encode(const Block& block) {
    if (block.size() < 8 || block.size() % 4 != 0 || block.size() / 4 > 32)
        throw std::invalid_argument("BPC requires 2..32 32-bit symbols");
    const auto symbols64 = values_for_width(block, 4);
    const auto symbol_count = symbols64.size();
    const auto width = symbol_count - 1;
    const auto mask = (std::uint32_t{1} << width) - 1U;
    const auto position_width = ceil_log2(symbol_count);
    BitWriter writer;
    const auto base_u = static_cast<std::uint32_t>(symbols64.front());
    const auto base = static_cast<std::int32_t>(base_u);
    if (base == 0) writer.put(0, 3);
    else if (fits_signed(base, 4)) { writer.put(4, 3); writer.put(base_u & 0xfU, 4); }
    else if (fits_signed(base, 8)) { writer.put(2, 3); writer.put(base_u & 0xffU, 8); }
    else if (fits_signed(base, 16)) { writer.put(6, 3); writer.put(base_u & 0xffffU, 16); }
    else { writer.put(1, 1); writer.put(base_u, 32); }

    std::vector<std::uint32_t> deltas(width);
    for (std::size_t i = 1; i < symbol_count; ++i)
        deltas[i - 1] = static_cast<std::uint32_t>(symbols64[i]) -
                        static_cast<std::uint32_t>(symbols64[i - 1]);
    std::array<std::uint32_t, 32> planes{};
    for (unsigned bit = 0; bit < 32; ++bit)
        for (std::size_t i = 0; i < width; ++i)
            planes[bit] |= ((deltas[i] >> bit) & 1U) << i;

    std::vector<std::uint32_t> dbx;
    dbx.push_back(planes[31]);
    for (unsigned bit = 31; bit-- > 0;) dbx.push_back(planes[bit] ^ planes[bit + 1]);
    for (std::size_t i = 0; i < dbx.size();) {
        if (dbx[i] == 0) {
            std::size_t run = 1;
            while (i + run < dbx.size() && dbx[i + run] == 0 && run < 33) ++run;
            if (run == 1) writer.put(4, 3);           // 001
            else { writer.put(2, 2); writer.put(run - 2, 5); } // 01 + length
            i += run;
            continue;
        }
        const auto ones = popcount(dbx[i]);
        const auto first = count_trailing_zeros(dbx[i]);
        if (dbx[i] == mask) writer.put(0, 5);          // 00000
        else if (ones == 2 && first + 1 < width && ((dbx[i] >> (first + 1)) & 1U)) {
            writer.put(8, 5); writer.put(first, position_width); // 00010
        } else if (ones == 1) {
            writer.put(24, 5); writer.put(first, position_width); // 00011
        } else {
            writer.put(1, 1); writer.put(dbx[i], width);
        }
        ++i;
    }
    return writer.bytes();
}

Block bpc_decode(const std::vector<std::uint8_t>& encoded, std::size_t output_size) {
    if (output_size < 8 || output_size % 4 != 0 || output_size / 4 > 32)
        throw std::invalid_argument("invalid BPC output size");
    const auto symbol_count = output_size / 4;
    const auto width = symbol_count - 1;
    const auto mask = (std::uint32_t{1} << width) - 1U;
    const auto position_width = ceil_log2(symbol_count);
    BitReader reader(encoded);
    std::uint32_t base = 0;
    if (reader.get(1) != 0) base = static_cast<std::uint32_t>(reader.get(32));
    else {
        const auto suffix = reader.get(2);
        if (suffix == 0) base = 0;
        else if (suffix == 2) base = static_cast<std::uint32_t>(sign_extend(reader.get(4), 4));
        else if (suffix == 1) base = static_cast<std::uint32_t>(sign_extend(reader.get(8), 8));
        else base = static_cast<std::uint32_t>(sign_extend(reader.get(16), 16));
    }
    std::vector<std::uint32_t> dbx;
    while (dbx.size() < 32) {
        if (reader.get(1) != 0) {
            dbx.push_back(static_cast<std::uint32_t>(reader.get(width)));
            continue;
        }
        if (reader.get(1) != 0) {
            const auto run = static_cast<std::size_t>(reader.get(5)) + 2;
            if (dbx.size() + run > 32) throw std::runtime_error("invalid BPC zero run");
            dbx.insert(dbx.end(), run, 0);
            continue;
        }
        if (reader.get(1) != 0) { dbx.push_back(0); continue; }
        const auto subtype = reader.get(2);
        if (subtype == 0) dbx.push_back(mask);
        else {
            const auto position = static_cast<unsigned>(reader.get(position_width));
            if (position >= width) throw std::runtime_error("invalid BPC position");
            if (subtype == 1) dbx.push_back(3U << position);
            else if (subtype == 3) dbx.push_back(1U << position);
            else throw std::runtime_error("unsupported BPC plane code");
        }
    }
    std::array<std::uint32_t, 32> planes{};
    planes[31] = dbx[0];
    for (std::size_t i = 1; i < 32; ++i) {
        const auto bit = 31U - static_cast<unsigned>(i);
        planes[bit] = dbx[i] ^ planes[bit + 1U];
    }
    std::vector<std::uint32_t> deltas(width, 0);
    for (unsigned bit = 0; bit < 32; ++bit)
        for (std::size_t i = 0; i < width; ++i)
            deltas[i] |= ((planes[bit] >> i) & 1U) << bit;
    Block out;
    out.reserve(output_size);
    auto append = [&](std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    };
    append(base);
    auto value = base;
    for (const auto delta : deltas) { value += delta; append(value); }
    return out;
}

std::size_t bpc_encoded_size(const Block& block) {
    return std::min(bpc_encode(block).size(), block.size());
}

std::vector<std::uint8_t> huffman_encode(const Block& block) {
    if (block.empty()) {
        throw std::invalid_argument("Huffman requires a non-empty block");
    }
    const auto lengths = huffman_code_lengths(block);
    const auto codes = canonical_huffman_codes(lengths);
    std::vector<std::uint8_t> output;
    const auto symbol_count = static_cast<std::uint16_t>(
        std::count_if(lengths.begin(), lengths.end(), [](std::uint8_t x) { return x != 0; }));
    output.push_back(static_cast<std::uint8_t>(symbol_count & 0xffU));
    output.push_back(static_cast<std::uint8_t>(symbol_count >> 8U));
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (lengths[symbol] != 0) {
            output.push_back(static_cast<std::uint8_t>(symbol));
            output.push_back(lengths[symbol]);
        }
    }
    BitWriter payload;
    for (const auto symbol : block) {
        payload.put(codes[symbol].bits, codes[symbol].length);
    }
    output.insert(output.end(), payload.bytes().begin(), payload.bytes().end());
    return output;
}

Block huffman_decode(const std::vector<std::uint8_t>& encoded,
                     std::size_t output_size) {
    if (encoded.size() < 2) {
        throw std::runtime_error("truncated Huffman header");
    }
    const auto count = static_cast<std::size_t>(encoded[0]) |
                       (static_cast<std::size_t>(encoded[1]) << 8U);
    if (count == 0 || count > 256 || encoded.size() < 2 + count * 2) {
        throw std::runtime_error("invalid Huffman header");
    }
    std::array<std::uint8_t, 256> lengths{};
    std::size_t at = 2;
    for (std::size_t i = 0; i < count; ++i) {
        const auto symbol = encoded[at++];
        const auto length = encoded[at++];
        if (length == 0 || lengths[symbol] != 0) {
            throw std::runtime_error("invalid Huffman code table");
        }
        lengths[symbol] = length;
    }
    const auto codes = canonical_huffman_codes(lengths);
    struct DecodeEntry { std::uint32_t bits; unsigned length; std::uint8_t symbol; };
    std::vector<DecodeEntry> table;
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (codes[symbol].length != 0) {
            table.push_back({codes[symbol].bits, codes[symbol].length,
                             static_cast<std::uint8_t>(symbol)});
        }
    }
    const std::vector<std::uint8_t> payload(encoded.begin() + static_cast<std::ptrdiff_t>(at),
                                            encoded.end());
    BitReader reader(payload);
    Block output;
    output.reserve(output_size);
    while (output.size() < output_size) {
        std::uint32_t bits = 0;
        bool found = false;
        for (unsigned length = 1; length <= 32 && !found; ++length) {
            bits |= static_cast<std::uint32_t>(reader.get(1)) << (length - 1U);
            for (const auto& entry : table) {
                if (entry.length == length && entry.bits == bits) {
                    output.push_back(entry.symbol);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            throw std::runtime_error("invalid Huffman payload");
        }
    }
    return output;
}

std::size_t huffman_encoded_size(const Block& block) {
    return std::min(huffman_encode(block).size(), block.size());
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
        case BaselineKind::Huffman:
            return huffman_encoded_size(block);
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
