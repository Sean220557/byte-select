#pragma once

#include "fpc_bsel/types.hpp"

#include <array>
#include <cstddef>

namespace fpc_bsel {

struct CpackParts {
    std::array<std::uint8_t, kWordCount> tags{};
    Bytes payload;
};

CpackParts split_cpack(const Bytes& block);
Bytes join_cpack(const CpackParts& parts);
Bytes cpack_encode(const Bytes& block);
Bytes cpack_decode(const Bytes& encoded);
Bytes pack_cpack_tags(const std::array<std::uint8_t, kWordCount>& tags);
std::array<std::uint8_t, kWordCount> unpack_cpack_tags(const Bytes& packed);
std::size_t cpack_payload_size(const std::array<std::uint8_t, kWordCount>& tags);

}
