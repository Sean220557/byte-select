#include "fpc_bsel/fpc.hpp"

#include <stdexcept>

namespace fpc_bsel {
namespace {

class BitWriter {
public:
    void put(std::uint64_t value, unsigned bits) {
        for (unsigned i = 0; i < bits; ++i) {
            if (bit_ == 0) data_.push_back(0);
            data_.back() |= static_cast<std::uint8_t>(((value >> i) & 1U) << bit_);
            bit_ = (bit_ + 1U) & 7U;
        }
    }
    const Bytes& bytes() const { return data_; }
private:
    Bytes data_;
    unsigned bit_ = 0;
};

class BitReader {
public:
    explicit BitReader(const Bytes& data) : data_(data) {}
    std::uint64_t get(unsigned bits) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < bits; ++i) {
            if (byte_ >= data_.size()) throw std::runtime_error("truncated FPC bitstream");
            value |= static_cast<std::uint64_t>((data_[byte_] >> bit_) & 1U) << i;
            if (++bit_ == 8) { bit_ = 0; ++byte_; }
        }
        return value;
    }
private:
    const Bytes& data_;
    std::size_t byte_ = 0;
    unsigned bit_ = 0;
};

std::uint32_t load32(const Bytes& block, std::size_t offset) {
    return static_cast<std::uint32_t>(block[offset]) |
           (static_cast<std::uint32_t>(block[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(block[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(block[offset + 3]) << 24U);
}

void store32(Bytes& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
}

bool sign_extended(std::uint32_t value, unsigned bits) {
    const auto mask = (std::uint32_t{1} << bits) - 1U;
    const auto low = value & mask;
    const auto extended = (low & (std::uint32_t{1} << (bits - 1U))) ? (low | ~mask) : low;
    return value == extended;
}

std::int32_t extend(std::uint32_t value, unsigned bits) {
    const auto mask = (std::uint32_t{1} << bits) - 1U;
    value &= mask;
    if (value & (std::uint32_t{1} << (bits - 1U))) value |= ~mask;
    return static_cast<std::int32_t>(value);
}

bool halfword_extended_byte(std::uint16_t value) {
    const auto byte = static_cast<std::uint8_t>(value);
    const auto extended = (byte & 0x80U) ? static_cast<std::uint16_t>(0xff00U | byte) : byte;
    return value == extended;
}

std::uint8_t classify(std::uint32_t value) {
    if (value == 0) return 0;
    if (sign_extended(value, 4)) return 1;
    const auto byte = static_cast<std::uint8_t>(value);
    if (value == static_cast<std::uint32_t>(byte) * 0x01010101U) return 2;
    if (sign_extended(value, 8)) return 3;
    if (sign_extended(value, 16)) return 4;
    if ((value & 0xffffU) == 0) return 5;
    if (halfword_extended_byte(static_cast<std::uint16_t>(value)) &&
        halfword_extended_byte(static_cast<std::uint16_t>(value >> 16U))) return 6;
    return 7;
}

unsigned payload_bits(std::uint8_t tag) {
    static constexpr unsigned widths[] = {0, 4, 8, 8, 16, 16, 16, 32};
    if (tag > 7) throw std::runtime_error("invalid FPC tag");
    return widths[tag];
}

void write_payload(BitWriter& writer, std::uint8_t tag, std::uint32_t value) {
    switch (tag) {
        case 0: break;
        case 1: writer.put(value & 0xfU, 4); break;
        case 2: case 3: writer.put(value & 0xffU, 8); break;
        case 4: writer.put(value & 0xffffU, 16); break;
        case 5: writer.put(value >> 16U, 16); break;
        case 6:
            writer.put(value & 0xffU, 8);
            writer.put((value >> 16U) & 0xffU, 8);
            break;
        case 7: writer.put(value, 32); break;
        default: throw std::runtime_error("invalid FPC tag");
    }
}

std::uint32_t read_payload(BitReader& reader, std::uint8_t tag) {
    switch (tag) {
        case 0: return 0;
        case 1: return static_cast<std::uint32_t>(extend(reader.get(4), 4));
        case 2: { const auto b = static_cast<std::uint32_t>(reader.get(8)); return b * 0x01010101U; }
        case 3: return static_cast<std::uint32_t>(extend(reader.get(8), 8));
        case 4: return static_cast<std::uint32_t>(extend(reader.get(16), 16));
        case 5: return static_cast<std::uint32_t>(reader.get(16)) << 16U;
        case 6: {
            const auto low = static_cast<std::uint32_t>(extend(reader.get(8), 8));
            const auto high = static_cast<std::uint32_t>(extend(reader.get(8), 8));
            return (low & 0xffffU) | (high << 16U);
        }
        case 7: return static_cast<std::uint32_t>(reader.get(32));
        default: throw std::runtime_error("invalid FPC tag");
    }
}

}

Bytes bitshuffle_words16(const Bytes& block) {
    if (block.size() != kBlockSize)
        throw std::invalid_argument("bitshuffle requires a 64-byte block");
    Bytes shuffled(kBlockSize, 0);
    for (std::size_t word = 0; word < kWordCount; ++word) {
        const auto value = load32(block, word * 4);
        for (std::size_t bit = 0; bit < 32; ++bit) {
            const auto output_bit = bit * kWordCount + word;
            if ((value >> bit) & 1U)
                shuffled[output_bit / 8] |=
                    static_cast<std::uint8_t>(1U << (output_bit % 8));
        }
    }
    return shuffled;
}

Bytes bitunshuffle_words16(const Bytes& block) {
    if (block.size() != kBlockSize)
        throw std::invalid_argument("bitunshuffle requires a 64-byte block");
    Bytes restored;
    restored.reserve(kBlockSize);
    for (std::size_t word = 0; word < kWordCount; ++word) {
        std::uint32_t value = 0;
        for (std::size_t bit = 0; bit < 32; ++bit) {
            const auto input_bit = bit * kWordCount + word;
            value |= static_cast<std::uint32_t>(
                         (block[input_bit / 8] >> (input_bit % 8)) & 1U)
                     << bit;
        }
        store32(restored, value);
    }
    return restored;
}


FpcParts split_fpc(const Bytes& block) {
    if (block.size() != kBlockSize) throw std::invalid_argument("FPC-BSEL requires 64-byte blocks");
    FpcParts result;
    result.residual_block.assign(kBlockSize, 0);
    BitWriter regular;
    for (std::size_t word = 0; word < kWordCount; ++word) {
        const auto offset = word * 4;
        const auto value = load32(block, offset);
        const auto tag = classify(value);
        result.tags[word] = tag;
        if (tag == 7) {
            store32(result.raw_residual, value);
            for (unsigned i = 0; i < 4; ++i) result.residual_block[offset + i] = block[offset + i];
        } else {
            write_payload(regular, tag, value);
        }
    }
    result.regular_payload = regular.bytes();
    return result;
}

Bytes join_fpc(const FpcParts& parts) {
    if (parts.residual_block.size() != kBlockSize) throw std::runtime_error("invalid residual block");
    BitReader regular(parts.regular_payload);
    Bytes output;
    output.reserve(kBlockSize);
    for (std::size_t word = 0; word < kWordCount; ++word) {
        const auto tag = parts.tags[word];
        const auto value = tag == 7 ? load32(parts.residual_block, word * 4)
                                    : read_payload(regular, tag);
        store32(output, value);
    }
    return output;
}

Bytes pack_tags(const std::array<std::uint8_t, kWordCount>& tags) {
    BitWriter writer;
    for (const auto tag : tags) writer.put(tag, 3);
    return writer.bytes();
}

std::array<std::uint8_t, kWordCount> unpack_tags(const Bytes& packed) {
    if (packed.size() != 6) throw std::runtime_error("FPC prefix must occupy six bytes");
    BitReader reader(packed);
    std::array<std::uint8_t, kWordCount> tags{};
    for (auto& tag : tags) tag = static_cast<std::uint8_t>(reader.get(3));
    return tags;
}

std::size_t regular_payload_size(const std::array<std::uint8_t, kWordCount>& tags) {
    std::size_t bits = 0;
    for (const auto tag : tags) if (tag != 7) bits += payload_bits(tag);
    return (bits + 7U) / 8U;
}

std::size_t raw_residual_size(const std::array<std::uint8_t, kWordCount>& tags) {
    std::size_t count = 0;
    for (const auto tag : tags) count += tag == 7 ? 4U : 0U;
    return count;
}

Bytes fpc_encode(const Bytes& block) {
    if (block.size() != kBlockSize) throw std::invalid_argument("FPC requires 64-byte blocks");
    std::array<std::uint8_t, kWordCount> tags{};
    std::array<std::uint32_t, kWordCount> values{};
    for (std::size_t word = 0; word < kWordCount; ++word) {
        values[word] = load32(block, word * 4);
        tags[word] = classify(values[word]);
    }
    BitWriter writer;
    for (const auto tag : tags) writer.put(tag, 3);
    for (std::size_t word = 0; word < kWordCount; ++word) {
        write_payload(writer, tags[word], values[word]);
    }
    return writer.bytes();
}

Bytes fpc_decode(const Bytes& encoded) {
    BitReader reader(encoded);
    std::array<std::uint8_t, kWordCount> tags{};
    for (auto& tag : tags) tag = static_cast<std::uint8_t>(reader.get(3));
    Bytes output;
    output.reserve(kBlockSize);
    std::size_t used_bits = kWordCount * 3U;
    for (const auto tag : tags) {
        used_bits += payload_bits(tag);
        store32(output, read_payload(reader, tag));
    }
    if (encoded.size() != (used_bits + 7U) / 8U)
        throw std::runtime_error("invalid FPC stream length");
    return output;
}

}
