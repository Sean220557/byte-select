#pragma once

#include "fpc_bsel/types.hpp"

#include <array>

namespace fpc_bsel {

struct FpcParts {
    std::array<std::uint8_t, kWordCount> tags{};
    Bytes regular_payload;
    Bytes raw_residual;
    Bytes residual_block;
};

FpcParts split_fpc(const Bytes& block);
Bytes join_fpc(const FpcParts& parts);
Bytes pack_tags(const std::array<std::uint8_t, kWordCount>& tags);
std::array<std::uint8_t, kWordCount> unpack_tags(const Bytes& packed);
std::size_t regular_payload_size(const std::array<std::uint8_t, kWordCount>& tags);
std::size_t raw_residual_size(const std::array<std::uint8_t, kWordCount>& tags);
Bytes fpc_encode(const Bytes& block);
Bytes fpc_decode(const Bytes& encoded);

}
