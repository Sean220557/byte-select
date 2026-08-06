#include "fpc_bsel/codec.hpp"
#include "fpc_bsel/fpc.hpp"

#include <algorithm>
#include <stdexcept>

namespace fpc_bsel {
namespace {

Bytes slice(const Bytes& data, std::size_t& offset, std::size_t size) {
    if (offset + size > data.size()) throw std::runtime_error("truncated FPC-BSEL block");
    Bytes result(data.begin() + static_cast<std::ptrdiff_t>(offset),
                 data.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
    return result;
}

std::size_t bsel_stream_size(const Bytes& data, std::size_t offset, const BselModel& model) {
    const auto width = bsel_id_bytes(model);
    if (offset + width > data.size()) throw std::runtime_error("truncated BSEL segment");
    const auto id = width == 1 ? data[offset]
                               : data[offset] | (static_cast<std::size_t>(data[offset + 1]) << 8U);
    if (id >= model.patterns.size()) throw std::runtime_error("unknown BSEL pattern id");
    return width + model.patterns[id].rank();
}

Bytes expanded_tags(const std::array<std::uint8_t, kWordCount>& tags) {
    return Bytes(tags.begin(), tags.end());
}

}

EncodedBlock encode_block(const Bytes& block, const Model& model, EncodeOptions options) {
    if (block.size() != kBlockSize) throw std::invalid_argument("FPC-BSEL requires 64-byte blocks");
    EncodedBlock best;
    best.mode = BlockMode::Raw;
    best.bytes.reserve(1 + block.size());
    best.bytes.push_back(static_cast<std::uint8_t>(BlockMode::Raw));
    best.bytes.insert(best.bytes.end(), block.begin(), block.end());

    const auto fpc = fpc_encode(block);
    if (1 + fpc.size() < best.bytes.size()) {
        best.mode = BlockMode::Fpc;
        best.bytes = {static_cast<std::uint8_t>(BlockMode::Fpc)};
        best.bytes.insert(best.bytes.end(), fpc.begin(), fpc.end());
    }

    const auto parts = split_fpc(block);
    auto prefix = pack_tags(parts.tags);
    bool prefix_bsel = false;
    if (const auto encoded = options.enable_prefix_bsel
                                 ? bsel_encode(expanded_tags(parts.tags), model.prefix)
                                 : std::optional<Bytes>{};
        encoded && encoded->size() < prefix.size()) {
        prefix = *encoded;
        prefix_bsel = true;
    }
    auto residual = parts.raw_residual;
    bool residual_bsel = false;
    if (const auto encoded = options.enable_residual_bsel
                                 ? bsel_encode(parts.residual_block, model.residual)
                                 : std::optional<Bytes>{};
        encoded && encoded->size() < residual.size()) {
        residual = *encoded;
        residual_bsel = true;
    }
    Bytes combined;
    combined.push_back(static_cast<std::uint8_t>(BlockMode::FpcBsel));
    combined.push_back(static_cast<std::uint8_t>((prefix_bsel ? 1U : 0U) |
                                                 (residual_bsel ? 2U : 0U)));
    combined.insert(combined.end(), prefix.begin(), prefix.end());
    combined.insert(combined.end(), parts.regular_payload.begin(), parts.regular_payload.end());
    combined.insert(combined.end(), residual.begin(), residual.end());
    if (combined.size() < best.bytes.size()) {
        best = {BlockMode::FpcBsel, std::move(combined), prefix_bsel, residual_bsel};
    }
    return best;
}

Bytes decode_block(const Bytes& encoded, const Model& model) {
    if (encoded.empty()) throw std::runtime_error("empty encoded block");
    const auto mode = static_cast<BlockMode>(encoded[0]);
    if (mode == BlockMode::Raw) {
        if (encoded.size() != 1 + kBlockSize) throw std::runtime_error("invalid raw block size");
        return Bytes(encoded.begin() + 1, encoded.end());
    }
    if (mode == BlockMode::Fpc) return fpc_decode(Bytes(encoded.begin() + 1, encoded.end()));
    if (mode != BlockMode::FpcBsel || encoded.size() < 2)
        throw std::runtime_error("unknown FPC-BSEL block mode");

    const auto flags = encoded[1];
    if (flags & ~3U) throw std::runtime_error("invalid FPC-BSEL flags");
    std::size_t offset = 2;
    std::array<std::uint8_t, kWordCount> tags{};
    if (flags & 1U) {
        const auto size = bsel_stream_size(encoded, offset, model.prefix);
        const auto decoded = bsel_decode(slice(encoded, offset, size), model.prefix);
        std::copy(decoded.begin(), decoded.end(), tags.begin());
        for (const auto tag : tags) if (tag > 7) throw std::runtime_error("invalid decoded FPC tag");
    } else {
        tags = unpack_tags(slice(encoded, offset, 6));
    }
    FpcParts parts;
    parts.tags = tags;
    parts.regular_payload = slice(encoded, offset, regular_payload_size(tags));
    parts.residual_block.assign(kBlockSize, 0);
    if (flags & 2U) {
        const auto size = bsel_stream_size(encoded, offset, model.residual);
        parts.residual_block = bsel_decode(slice(encoded, offset, size), model.residual);
    } else {
        const auto raw = slice(encoded, offset, raw_residual_size(tags));
        std::size_t raw_offset = 0;
        for (std::size_t word = 0; word < kWordCount; ++word) {
            if (tags[word] == 7) {
                std::copy_n(raw.begin() + static_cast<std::ptrdiff_t>(raw_offset), 4,
                            parts.residual_block.begin() + static_cast<std::ptrdiff_t>(word * 4));
                raw_offset += 4;
            }
        }
    }
    if (offset != encoded.size()) throw std::runtime_error("trailing data in encoded block");
    return join_fpc(parts);
}

}
