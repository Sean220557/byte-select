#include "fpc_bsel/prefix_payload.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>

namespace fpc_bsel {
namespace {
class BitWriter {
public:
    void put(std::uint32_t value, unsigned count) {
        for (unsigned bit = 0; bit < count; ++bit) {
            if ((bits_ & 7U) == 0) data_.push_back(0);
            data_.back() |= static_cast<std::uint8_t>(((value >> bit) & 1U) << (bits_ & 7U));
            ++bits_;
        }
    }
    std::uint16_t bits() const { return static_cast<std::uint16_t>(bits_); }
    Bytes take() { return std::move(data_); }
private:
    Bytes data_;
    unsigned bits_ = 0;
};

class BitReader {
public:
    BitReader(const Bytes& data, std::uint16_t bits) : data_(data), bits_(bits) {
        if (bits > data.size() * 8U) throw std::runtime_error("truncated prefix payload");
    }
    std::uint32_t get(unsigned count) {
        if (position_ + count > bits_) throw std::runtime_error("truncated prefix token");
        std::uint32_t value = 0;
        for (unsigned bit = 0; bit < count; ++bit)
            value |= std::uint32_t((data_[(position_ + bit) >> 3U] >> ((position_ + bit) & 7U)) & 1U) << bit;
        position_ += count;
        return value;
    }
private:
    const Bytes& data_;
    unsigned bits_ = 0, position_ = 0;
};

std::uint32_t cache_key(const std::uint8_t* bytes, unsigned length) {
    std::uint32_t value = length << 24U;
    for (unsigned i = 0; i < length; ++i) value |= std::uint32_t(bytes[i]) << (8U * i);
    return value;
}

struct Cache {
    std::vector<std::uint32_t> keys;
    std::vector<std::uint8_t> lengths, scores;
    std::unordered_map<std::uint32_t, unsigned> index;
    unsigned hand = 0;

    int find(const std::uint8_t* bytes, unsigned count, unsigned& length) const {
        for (const auto candidate : {3U, 2U}) if (candidate < count) {
            const auto it = index.find(cache_key(bytes, candidate));
            if (it != index.end()) { length = candidate; return static_cast<int>(it->second); }
        }
        return -1;
    }
    void touch(unsigned slot) { scores[slot] = std::min<unsigned>(3, scores[slot] + 1); }
    void add(const std::uint8_t* bytes, unsigned count) {
        for (const auto length : {2U, 3U}) if (length < count) {
            const auto key = cache_key(bytes, length);
            if (index.count(key)) continue;
            unsigned slot = 0;
            if (keys.size() < 256) {
                slot = static_cast<unsigned>(keys.size());
                keys.push_back(key); lengths.push_back(static_cast<std::uint8_t>(length)); scores.push_back(0);
            } else {
                while (scores[hand]) { --scores[hand]; hand = (hand + 1U) & 255U; }
                slot = hand; hand = (hand + 1U) & 255U;
                index.erase(keys[slot]); keys[slot] = key; lengths[slot] = static_cast<std::uint8_t>(length); scores[slot] = 0;
            }
            index.emplace(key, slot);
        }
    }
};

Bytes encoded_fpc_stream(const Bytes& input, const Model& model, std::array<unsigned, 4>& sizes) {
    if (input.size() != 256) throw std::invalid_argument("prefix payload must be exactly 256 bytes");
    Bytes stream;
    for (unsigned lane = 0; lane < 4; ++lane) {
        const Bytes block(input.begin() + lane * 64, input.begin() + (lane + 1U) * 64);
        // Production prefix path is Custom-FPC (including the bitshuffle
        // candidate) followed by dynamic prefix coding; BSEL is deliberately
        // disabled here so this payload matches run_final_comparison.sh.
        const auto encoded = encode_block(block, model, {false, false}).bytes;
        if (encoded.size() > 127) throw std::runtime_error("FPC payload length overflow");
        sizes[lane] = static_cast<unsigned>(encoded.size());
        stream.insert(stream.end(), encoded.begin(), encoded.end());
    }
    return stream;
}
}  // namespace

PrefixPayload prefix_encode_payload(const Bytes& input, const Model& model) {
    std::array<unsigned, 4> sizes{};
    const auto stream = encoded_fpc_stream(input, model, sizes);
    BitWriter writer;
    for (const auto size : sizes) writer.put(size, 7);
    Cache cache;
    for (std::size_t offset = 0; offset < stream.size(); offset += 4) {
        const auto count = static_cast<unsigned>(std::min<std::size_t>(4, stream.size() - offset));
        unsigned matched = 0;
        const auto slot = cache.find(stream.data() + offset, count, matched);
        const auto literal_cost = 1U + 8U * count;
        const auto cache_cost = slot >= 0 ? 10U + 8U * (count - matched) : literal_cost + 1;
        if (cache_cost <= literal_cost) {
            writer.put(0b01U, 2); writer.put(static_cast<unsigned>(slot), 8);
            for (unsigned i = matched; i < count; ++i) writer.put(stream[offset + i], 8);
            cache.touch(static_cast<unsigned>(slot));
        } else {
            writer.put(0U, 1);
            for (unsigned i = 0; i < count; ++i) writer.put(stream[offset + i], 8);
        }
        cache.add(stream.data() + offset, count);
    }
    return {writer.take(), writer.bits()};
}

Bytes prefix_decode_payload(const PrefixPayload& payload, const Model& model) {
    BitReader reader(payload.bytes, payload.bit_count);
    std::array<unsigned, 4> sizes{};
    unsigned total = 0;
    for (auto& size : sizes) { size = reader.get(7); if (!size || size > 65) throw std::runtime_error("invalid FPC lane size"); total += size; }
    Bytes stream; stream.reserve(total);
    Cache cache;
    while (stream.size() < total) {
        const auto count = static_cast<unsigned>(std::min<std::size_t>(4, total - stream.size()));
        const auto first = reader.get(1);
        std::array<std::uint8_t, 4> chunk{};
        if (!first) {
            for (unsigned i = 0; i < count; ++i) chunk[i] = static_cast<std::uint8_t>(reader.get(8));
        } else if (!reader.get(1)) {
            const auto slot = reader.get(8);
            if (slot >= cache.keys.size()) throw std::runtime_error("invalid prefix cache slot");
            const auto length = cache.lengths[slot];
            if (length >= count) throw std::runtime_error("invalid prefix cache length");
            const auto key = cache.keys[slot];
            for (unsigned i = 0; i < length; ++i) chunk[i] = static_cast<std::uint8_t>(key >> (8U * i));
            for (unsigned i = length; i < count; ++i) chunk[i] = static_cast<std::uint8_t>(reader.get(8));
            cache.touch(slot);
        } else throw std::runtime_error("reserved prefix token");
        stream.insert(stream.end(), chunk.begin(), chunk.begin() + count);
        cache.add(chunk.data(), count);
    }
    Bytes output; output.reserve(256);
    std::size_t offset = 0;
    for (const auto size : sizes) {
        Bytes encoded(stream.begin() + static_cast<std::ptrdiff_t>(offset), stream.begin() + static_cast<std::ptrdiff_t>(offset + size));
        const auto block = decode_block(encoded, model);
        output.insert(output.end(), block.begin(), block.end());
        offset += size;
    }
    return output;
}

}  // namespace fpc_bsel
