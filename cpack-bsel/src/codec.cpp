#include "fpc_bsel/codec.hpp"
#include "fpc_bsel/cpack.hpp"

#include <algorithm>
#include <stdexcept>

namespace fpc_bsel {
namespace {

constexpr std::uint8_t kInlinePrefixEscape = 63;

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

    const auto cpack = cpack_encode(block);
    if (1 + cpack.size() < best.bytes.size()) {
        best.mode = BlockMode::Fpc;
        best.bytes = {static_cast<std::uint8_t>(BlockMode::Fpc)};
        best.bytes.insert(best.bytes.end(), cpack.begin(), cpack.end());
    }

    if (const auto raw_bsel = options.enable_residual_bsel
                                  ? bsel_encode(block, model.residual)
                                  : std::optional<Bytes>{};
        raw_bsel && 1 + raw_bsel->size() < best.bytes.size()) {
        best.mode = BlockMode::RawBsel;
        best.bytes = {static_cast<std::uint8_t>(BlockMode::RawBsel)};
        best.bytes.insert(best.bytes.end(), raw_bsel->begin(), raw_bsel->end());
        best.residual_bsel = true;
    }

    const auto parts = split_cpack(block);
    auto prefix = pack_cpack_tags(parts.tags);
    bool prefix_bsel = false;
    bool prefix_id_inline = false;
    std::uint8_t inline_prefix_id = 0;
    if (const auto encoded = options.enable_prefix_bsel
                                 ? bsel_encode(expanded_tags(parts.tags), model.prefix)
                                 : std::optional<Bytes>{};
        encoded) {
        const bool can_inline = false;
        const auto charged_size = encoded->size() - (can_inline ? 1U : 0U);
        if (charged_size < prefix.size()) {
            prefix = *encoded;
            prefix_bsel = true;
            prefix_id_inline = can_inline;
            if (can_inline) {
                inline_prefix_id = prefix.front();
                prefix.erase(prefix.begin());
            }
        }
    }
    auto residual = parts.payload;
    bool residual_bsel = false;
    Bytes combined;
    combined.push_back(static_cast<std::uint8_t>(BlockMode::FpcBsel));
    const auto prefix_code = prefix_bsel
        ? (prefix_id_inline ? inline_prefix_id : kInlinePrefixEscape)
        : 0U;
    combined.push_back(static_cast<std::uint8_t>((prefix_bsel ? 1U : 0U) |
                                                 (residual_bsel ? 2U : 0U) |
                                                 (prefix_code << 2U)));
    combined.insert(combined.end(), prefix.begin(), prefix.end());
    combined.insert(combined.end(), residual.begin(), residual.end());
    if (combined.size() < best.bytes.size()) {
        best = {BlockMode::FpcBsel, std::move(combined), prefix_bsel, residual_bsel};
        best.prefix_id_inline = prefix_id_inline;
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
    if (mode == BlockMode::Fpc) return cpack_decode(Bytes(encoded.begin() + 1, encoded.end()));
    if (mode == BlockMode::RawBsel) {
        return bsel_decode(Bytes(encoded.begin() + 1, encoded.end()), model.residual);
    }
    if (mode != BlockMode::FpcBsel || encoded.size() < 2)
        throw std::runtime_error("unknown FPC-BSEL block mode");

    const auto flags = encoded[1];
    const auto prefix_code = static_cast<std::uint8_t>(flags >> 2U);
    if (!(flags & 1U) && prefix_code != 0) throw std::runtime_error("invalid inline prefix id");
    std::size_t offset = 2;
    std::array<std::uint8_t, kWordCount> tags{};
    if (flags & 1U) {
        Bytes decoded;
        if (prefix_code < kInlinePrefixEscape) {
            if (prefix_code >= model.prefix.patterns.size())
                throw std::runtime_error("unknown inline prefix id");
            const auto rank = model.prefix.patterns[prefix_code].rank();
            auto dictionary = slice(encoded, offset, rank);
            Bytes segment{prefix_code};
            segment.insert(segment.end(), dictionary.begin(), dictionary.end());
            decoded = bsel_decode(segment, model.prefix);
        } else {
            const auto size = bsel_stream_size(encoded, offset, model.prefix);
            decoded = bsel_decode(slice(encoded, offset, size), model.prefix);
        }
        std::copy(decoded.begin(), decoded.end(), tags.begin());
        for (const auto tag : tags) if (tag > 7) throw std::runtime_error("invalid decoded FPC tag");
    } else {
        tags = unpack_cpack_tags(slice(encoded, offset, 6));
    }
    CpackParts parts;
    parts.tags = tags;
    parts.payload = slice(encoded, offset, cpack_payload_size(tags));
    if (offset != encoded.size()) throw std::runtime_error("trailing data in encoded block");
    return join_cpack(parts);
}

}
