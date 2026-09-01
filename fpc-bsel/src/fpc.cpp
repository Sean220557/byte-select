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

unsigned payload_bits(std::uint8_t tag) {
    static constexpr unsigned widths[] = {
        0, 0, 8, 8, 16, 16, 8, 16, 16, 16, 16, 2, 10, 18, 18, 32};
    if (tag > 15) throw std::runtime_error("invalid FPC tag");
    return widths[tag];
}

std::uint8_t byte_at(std::uint32_t value, unsigned index) {
    return static_cast<std::uint8_t>(value >> (index * 8U));
}

struct Choice { std::uint8_t tag = 15; std::uint8_t dict = 0; };

Choice classify(std::uint32_t value, const std::array<std::uint32_t, kWordCount>& history,
                std::size_t word) {
    const auto b0=byte_at(value,0), b1=byte_at(value,1), b2=byte_at(value,2), b3=byte_at(value,3);
    Choice best{15,0};
    auto consider=[&](std::uint8_t tag) {
        if (payload_bits(tag)<payload_bits(best.tag)) best={tag,0};
    };
    if (value == 0) consider(0);
    if (value == 0xffffffffU) consider(1);
    if (b3==0 && b2==0 && b1==0) consider(2);                  // ZZZX
    if (b2==0 && b1==0 && b0==0) consider(3);                  // XZZZ
    if (b3==0 && b2==0) consider(4);                           // ZZXX
    if (b1==0 && b0==0) consider(5);                           // XXZZ
    if (b3==0xff && b2==0xff && b1==0xff) consider(6);         // FFFX
    if (b3==0xff && b2==0xff) consider(7);                     // FFXX
    if (b3==b1 && b2==b0) consider(8);                         // XYXY
    if (b3==0 && b1==0) consider(9);                           // ZXZX
    if (b2==0 && b0==0) consider(10);                          // XZXZ
    for (std::size_t distance=1; distance<=4 && distance<=word; ++distance) {
        const auto other=history[word-distance];
        const auto o1=byte_at(other,1), o2=byte_at(other,2), o3=byte_at(other,3);
        std::uint8_t tag=15;
        if (value==other) tag=11;
        else if (b3==o3 && b2==o2 && b1==o1) tag=12;
        else if (b3==o3 && b2==o2) tag=13;
        else if (b3==o3 && b1==o1) tag=14;
        if (payload_bits(tag)<payload_bits(best.tag)) best={tag,static_cast<std::uint8_t>(distance-1)};
    }
    return best;
}

void write_payload(BitWriter& writer, Choice choice, std::uint32_t value) {
    const auto tag=choice.tag;
    switch (tag) {
        case 0: case 1: break;
        case 2: case 6: writer.put(byte_at(value,0),8); break;
        case 3: writer.put(byte_at(value,3),8); break;
        case 4: case 7: writer.put(value & 0xffffU,16); break;
        case 5: writer.put(value >> 16U,16); break;
        case 8: writer.put(value & 0xffffU,16); break;
        case 9: writer.put(byte_at(value,2),8); writer.put(byte_at(value,0),8); break;
        case 10: writer.put(byte_at(value,3),8); writer.put(byte_at(value,1),8); break;
        case 11: writer.put(choice.dict,2); break;
        case 12: writer.put(choice.dict,2); writer.put(byte_at(value,0),8); break;
        case 13: writer.put(choice.dict,2); writer.put(value & 0xffffU,16); break;
        case 14: writer.put(choice.dict,2); writer.put(byte_at(value,2),8); writer.put(byte_at(value,0),8); break;
        case 15: writer.put(value,32); break;
        default: throw std::runtime_error("invalid FPC tag");
    }
}

std::uint32_t read_payload(BitReader& reader, std::uint8_t tag,
                           const std::array<std::uint32_t,kWordCount>& history,
                           std::size_t word) {
    auto dictionary=[&]() {
        const auto index=static_cast<std::size_t>(reader.get(2));
        if (index>=word || index>=4) throw std::runtime_error("invalid FPC dictionary index");
        return history[word-index-1];
    };
    switch (tag) {
        case 0: return 0;
        case 1: return 0xffffffffU;
        case 2: return static_cast<std::uint32_t>(reader.get(8));
        case 3: return static_cast<std::uint32_t>(reader.get(8))<<24U;
        case 4: return static_cast<std::uint32_t>(reader.get(16));
        case 7: return 0xffff0000U | static_cast<std::uint32_t>(reader.get(16));
        case 5: return static_cast<std::uint32_t>(reader.get(16))<<16U;
        case 6: return 0xffffff00U | static_cast<std::uint32_t>(reader.get(8));
        case 8: { const auto h=static_cast<std::uint32_t>(reader.get(16)); return h|(h<<16U); }
        case 9: { const auto a=reader.get(8), b=reader.get(8); return static_cast<std::uint32_t>((a<<16U)|b); }
        case 10:{ const auto a=reader.get(8), b=reader.get(8); return static_cast<std::uint32_t>((a<<24U)|(b<<8U)); }
        case 11: return dictionary();
        case 12: { const auto d=dictionary(); return (d&0xffffff00U)|reader.get(8); }
        case 13: { const auto d=dictionary(); return (d&0xffff0000U)|reader.get(16); }
        case 14: { const auto d=dictionary(); const auto b2=reader.get(8), b0=reader.get(8);
                   return (d&0xff00ff00U)|(static_cast<std::uint32_t>(b2)<<16U)|b0; }
        case 15: return static_cast<std::uint32_t>(reader.get(32));
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
    std::array<std::uint32_t,kWordCount> history{};
    for (std::size_t word = 0; word < kWordCount; ++word) {
        const auto offset = word * 4;
        const auto value = load32(block, offset);
        const auto choice = classify(value, history, word);
        const auto tag = choice.tag;
        result.tags[word] = tag;
        history[word]=value;
        if (tag == 15) {
            store32(result.raw_residual, value);
            for (unsigned i = 0; i < 4; ++i) result.residual_block[offset + i] = block[offset + i];
        } else {
            write_payload(regular, choice, value);
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
    std::array<std::uint32_t,kWordCount> history{};
    for (std::size_t word = 0; word < kWordCount; ++word) {
        const auto tag = parts.tags[word];
        const auto value = tag == 15 ? load32(parts.residual_block, word * 4)
                                     : read_payload(regular, tag, history, word);
        history[word]=value;
        store32(output, value);
    }
    return output;
}

Bytes pack_tags(const std::array<std::uint8_t, kWordCount>& tags) {
    Bytes packed(8,0);
    for (std::size_t i=0;i<tags.size();++i) {
        if (tags[i]>15) throw std::runtime_error("invalid FPC tag");
        if ((i&1U)==0) packed[i/2]=static_cast<std::uint8_t>(tags[i]<<4U);
        else packed[i/2]|=tags[i];
    }
    return packed;
}

std::array<std::uint8_t, kWordCount> unpack_tags(const Bytes& packed) {
    if (packed.size() != 8) throw std::runtime_error("FPC prefix must occupy eight bytes");
    std::array<std::uint8_t, kWordCount> tags{};
    for (std::size_t i=0;i<tags.size();++i)
        tags[i]=static_cast<std::uint8_t>((i&1U)?packed[i/2]&0xfU:packed[i/2]>>4U);
    return tags;
}

std::size_t regular_payload_size(const std::array<std::uint8_t, kWordCount>& tags) {
    std::size_t bits = 0;
    for (const auto tag : tags) if (tag != 15) bits += payload_bits(tag);
    return (bits + 7U) / 8U;
}

std::size_t raw_residual_size(const std::array<std::uint8_t, kWordCount>& tags) {
    std::size_t count = 0;
    for (const auto tag : tags) count += tag == 15 ? 4U : 0U;
    return count;
}

Bytes fpc_encode(const Bytes& block) {
    if (block.size() != kBlockSize) throw std::invalid_argument("FPC requires 64-byte blocks");
    std::array<std::uint8_t, kWordCount> tags{};
    std::array<std::uint32_t, kWordCount> values{};
    std::array<Choice,kWordCount> choices{};
    for (std::size_t word = 0; word < kWordCount; ++word) {
        values[word] = load32(block, word * 4);
        choices[word]=classify(values[word], values, word);
        tags[word] = choices[word].tag;
    }
    Bytes output=pack_tags(tags);
    BitWriter writer;
    for (std::size_t word = 0; word < kWordCount; ++word) {
        write_payload(writer, choices[word], values[word]);
    }
    output.insert(output.end(),writer.bytes().begin(),writer.bytes().end());
    return output;
}

Bytes fpc_decode(const Bytes& encoded) {
    if (encoded.size()<8) throw std::runtime_error("truncated FPC prefix");
    const auto tags=unpack_tags(Bytes(encoded.begin(),encoded.begin()+8));
    const Bytes payload(encoded.begin()+8,encoded.end());
    BitReader reader(payload);
    Bytes output;
    output.reserve(kBlockSize);
    std::array<std::uint32_t,kWordCount> history{};
    std::size_t used_bits = 64U;
    for (std::size_t word=0;word<kWordCount;++word) {
        const auto tag=tags[word];
        used_bits += payload_bits(tag);
        history[word]=read_payload(reader,tag,history,word);
        store32(output,history[word]);
    }
    if (encoded.size() != (used_bits + 7U) / 8U)
        throw std::runtime_error("invalid FPC stream length");
    return output;
}

}
