#pragma once

#include "fpc_bsel/codec.hpp"

#include <cstdint>

namespace fpc_bsel {

// A self-contained, dynamically-prefixed 256-byte payload.  Its cache is
// reset at the payload boundary, so the bitstream remains valid after moving
// it to another region.  No model or codebook is carried in the payload.
struct PrefixPayload {
    Bytes bytes;
    std::uint16_t bit_count = 0;
};

PrefixPayload prefix_encode_payload(const Bytes& input_256, const Model& model);
Bytes prefix_decode_payload(const PrefixPayload& payload, const Model& model);

}  // namespace fpc_bsel
