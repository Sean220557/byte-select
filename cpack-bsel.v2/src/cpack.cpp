#include "fpc_bsel/cpack.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace fpc_bsel {
namespace {

class BitWriter {
public:
    void put(std::uint64_t value, unsigned bits) {
        for (unsigned i = 0; i < bits; ++i) {
            if (bit_ == 0) bytes_.push_back(0);
            if ((value >> i) & 1ULL) bytes_.back() |= static_cast<std::uint8_t>(1U << bit_);
            bit_ = (bit_ + 1U) & 7U;
        }
    }
    Bytes bytes() const { return bytes_; }
private:
    Bytes bytes_;
    unsigned bit_ = 0;
};

class BitReader {
public:
    explicit BitReader(const Bytes& bytes) : bytes_(bytes) {}
    std::uint64_t get(unsigned bits) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < bits; ++i) {
            if (offset_ >= bytes_.size()) throw std::runtime_error("truncated C-Pack bitstream");
            if ((bytes_[offset_] >> bit_) & 1U) value |= 1ULL << i;
            bit_ = (bit_ + 1U) & 7U;
            if (bit_ == 0) ++offset_;
        }
        return value;
    }
private:
    const Bytes& bytes_;
    std::size_t offset_ = 0;
    unsigned bit_ = 0;
};

std::uint32_t load32(const Bytes& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

void store32(Bytes& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
}

void update_dictionary(std::vector<std::uint32_t>& dictionary, std::uint32_t word) {
    if (dictionary.size() == 16) dictionary.erase(dictionary.begin());
    dictionary.push_back(word);
}

}

CpackParts split_cpack(const Bytes& block) {
    if (block.size() != kBlockSize) throw std::invalid_argument("C-Pack-BSEL requires 64-byte blocks");
    CpackParts parts;
    std::vector<std::uint32_t> dictionary;
    dictionary.reserve(16);
    for (std::size_t word_index = 0; word_index < kWordCount; ++word_index) {
        const auto word = load32(block, word_index * 4);
        const auto byte0 = static_cast<std::uint8_t>(word & 0xffU);
        const auto byte1 = static_cast<std::uint8_t>((word >> 8U) & 0xffU);
        const auto byte2 = static_cast<std::uint8_t>((word >> 16U) & 0xffU);
        const auto byte3 = static_cast<std::uint8_t>((word >> 24U) & 0xffU);

        if (word == 0) {
            parts.tags[word_index] = 0; // zzzz
            continue;
        }
        if (byte1 == 0 && byte2 == 0 && byte3 == 0) {
            parts.tags[word_index] = 4; // zzzx
            parts.payload.push_back(byte0);
            continue;
        }

        int best_matches = 1;
        std::size_t best_index = 0;
        for (std::size_t i = 0; i < dictionary.size(); ++i) {
            const auto entry = dictionary[i];
            int matches = 0;
            if (byte3 == static_cast<std::uint8_t>((entry >> 24U) & 0xffU) &&
                byte2 == static_cast<std::uint8_t>((entry >> 16U) & 0xffU)) {
                matches = 2;
                if (byte1 == static_cast<std::uint8_t>((entry >> 8U) & 0xffU)) {
                    matches = 3;
                    if (byte0 == static_cast<std::uint8_t>(entry & 0xffU)) matches = 4;
                }
            }
            if (matches > best_matches) {
                best_matches = matches;
                best_index = i;
            }
        }

        if (best_matches == 4) {
            parts.tags[word_index] = 2; // mmmm
            parts.payload.push_back(static_cast<std::uint8_t>(best_index));
            update_dictionary(dictionary, word);
        } else if (best_matches == 3) {
            parts.tags[word_index] = 5; // mmmx
            parts.payload.push_back(static_cast<std::uint8_t>(best_index));
            parts.payload.push_back(byte0);
            update_dictionary(dictionary, word);
        } else if (best_matches == 2) {
            parts.tags[word_index] = 3; // mmxx
            parts.payload.push_back(static_cast<std::uint8_t>(best_index));
            parts.payload.push_back(byte0);
            parts.payload.push_back(byte1);
            update_dictionary(dictionary, word);
        } else {
            parts.tags[word_index] = 1; // xxxx
            store32(parts.payload, word);
            update_dictionary(dictionary, word);
        }
    }
    return parts;
}

Bytes join_cpack(const CpackParts& parts) {
    Bytes out;
    out.reserve(kBlockSize);
    std::vector<std::uint32_t> dictionary;
    dictionary.reserve(16);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < kWordCount; ++i) {
        std::uint32_t word = 0;
        bool update = false;
        switch (parts.tags[i]) {
            case 0:
                word = 0;
                break;
            case 1:
                if (offset + 4 > parts.payload.size()) throw std::runtime_error("truncated C-Pack payload");
                word = load32(parts.payload, offset);
                offset += 4;
                update = true;
                break;
            case 2: {
                if (offset + 1 > parts.payload.size()) throw std::runtime_error("truncated C-Pack payload");
                const auto index = parts.payload[offset++];
                if (index >= dictionary.size()) throw std::runtime_error("invalid C-Pack dictionary index");
                word = dictionary[index];
                update = true;
                break;
            }
            case 3: {
                if (offset + 3 > parts.payload.size()) throw std::runtime_error("truncated C-Pack payload");
                const auto index = parts.payload[offset++];
                if (index >= dictionary.size()) throw std::runtime_error("invalid C-Pack dictionary index");
                word = (dictionary[index] & 0xffff0000U) |
                       static_cast<std::uint32_t>(parts.payload[offset]) |
                       (static_cast<std::uint32_t>(parts.payload[offset + 1]) << 8U);
                offset += 2;
                update = true;
                break;
            }
            case 4:
                if (offset + 1 > parts.payload.size()) throw std::runtime_error("truncated C-Pack payload");
                word = parts.payload[offset++];
                break;
            case 5: {
                if (offset + 2 > parts.payload.size()) throw std::runtime_error("truncated C-Pack payload");
                const auto index = parts.payload[offset++];
                if (index >= dictionary.size()) throw std::runtime_error("invalid C-Pack dictionary index");
                word = (dictionary[index] & 0xffffff00U) | parts.payload[offset++];
                update = true;
                break;
            }
            default:
                throw std::runtime_error("invalid C-Pack tag");
        }
        store32(out, word);
        if (update) update_dictionary(dictionary, word);
    }
    if (offset != parts.payload.size()) throw std::runtime_error("trailing C-Pack payload");
    return out;
}

Bytes pack_cpack_tags(const std::array<std::uint8_t, kWordCount>& tags) {
    Bytes out(6, 0);
    unsigned bit = 0;
    for (const auto tag : tags) {
        if (tag > 7) throw std::runtime_error("invalid C-Pack tag");
        for (unsigned i = 0; i < 3; ++i) {
            if ((tag >> i) & 1U) out[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
            ++bit;
        }
    }
    return out;
}

std::array<std::uint8_t, kWordCount> unpack_cpack_tags(const Bytes& packed) {
    if (packed.size() != 6) throw std::runtime_error("C-Pack tags must occupy six bytes");
    std::array<std::uint8_t, kWordCount> tags{};
    unsigned bit = 0;
    for (auto& tag : tags) {
        tag = 0;
        for (unsigned i = 0; i < 3; ++i) {
            if ((packed[bit / 8] >> (bit % 8)) & 1U) tag |= static_cast<std::uint8_t>(1U << i);
            ++bit;
        }
        if (tag > 5) throw std::runtime_error("invalid C-Pack tag");
    }
    return tags;
}

std::size_t cpack_payload_size(const std::array<std::uint8_t, kWordCount>& tags) {
    std::size_t size = 0;
    for (const auto tag : tags) {
        switch (tag) {
            case 0: break;
            case 1: size += 4; break;
            case 2: size += 1; break;
            case 3: size += 3; break;
            case 4: size += 1; break;
            case 5: size += 2; break;
            default: throw std::runtime_error("invalid C-Pack tag");
        }
    }
    return size;
}

Bytes cpack_encode(const Bytes& block) {
    if (block.size() != kBlockSize) throw std::invalid_argument("C-Pack requires 64-byte blocks");
    std::vector<std::uint32_t> dictionary;
    dictionary.reserve(16);
    BitWriter writer;
    for (std::size_t offset = 0; offset < block.size(); offset += 4) {
        const auto word = load32(block, offset);
        const auto byte0 = static_cast<std::uint8_t>(word & 0xffU);
        const auto byte1 = static_cast<std::uint8_t>((word >> 8U) & 0xffU);
        const auto byte2 = static_cast<std::uint8_t>((word >> 16U) & 0xffU);
        const auto byte3 = static_cast<std::uint8_t>((word >> 24U) & 0xffU);
        if (word == 0) {
            writer.put(0, 2);
            continue;
        }
        if (byte1 == 0 && byte2 == 0 && byte3 == 0) {
            writer.put(11, 4);
            writer.put(byte0, 8);
            continue;
        }
        int best_matches = 1;
        std::size_t best_index = 0;
        for (std::size_t i = 0; i < dictionary.size(); ++i) {
            const auto entry = dictionary[i];
            int matches = 0;
            if (byte3 == static_cast<std::uint8_t>((entry >> 24U) & 0xffU) &&
                byte2 == static_cast<std::uint8_t>((entry >> 16U) & 0xffU)) {
                matches = 2;
                if (byte1 == static_cast<std::uint8_t>((entry >> 8U) & 0xffU)) {
                    matches = 3;
                    if (byte0 == static_cast<std::uint8_t>(entry & 0xffU)) matches = 4;
                }
            }
            if (matches > best_matches) {
                best_matches = matches;
                best_index = i;
            }
        }
        if (best_matches == 4) {
            writer.put(1, 2);
            writer.put(best_index, 4);
        } else if (best_matches == 3) {
            writer.put(7, 4);
            writer.put(best_index, 4);
            writer.put(byte0, 8);
        } else if (best_matches == 2) {
            writer.put(3, 4);
            writer.put(best_index, 4);
            writer.put(word & 0xffffU, 16);
        } else {
            writer.put(2, 2);
            writer.put(word, 32);
        }
        update_dictionary(dictionary, word);
    }
    return writer.bytes();
}

Bytes cpack_decode(const Bytes& encoded) {
    BitReader reader(encoded);
    std::vector<std::uint32_t> dictionary;
    dictionary.reserve(16);
    Bytes out;
    out.reserve(kBlockSize);
    while (out.size() < kBlockSize) {
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
            if (index >= dictionary.size()) throw std::runtime_error("invalid C-Pack dictionary index");
            word = dictionary[index];
            update = true;
        } else {
            const auto subtype = reader.get(2);
            if (subtype == 2) {
                word = static_cast<std::uint32_t>(reader.get(8));
            } else {
                const auto index = static_cast<std::size_t>(reader.get(4));
                if (index >= dictionary.size()) throw std::runtime_error("invalid C-Pack dictionary index");
                word = dictionary[index];
                if (subtype == 0) word = (word & 0xffff0000U) | static_cast<std::uint32_t>(reader.get(16));
                else if (subtype == 1) word = (word & 0xffffff00U) | static_cast<std::uint32_t>(reader.get(8));
                else throw std::runtime_error("invalid C-Pack subtype");
                update = true;
            }
        }
        store32(out, word);
        if (update) update_dictionary(dictionary, word);
    }
    return out;
}

}
