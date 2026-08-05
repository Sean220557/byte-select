#pragma once

#include "fpc_bsel/model.hpp"

namespace fpc_bsel {

enum class BlockMode : std::uint8_t { Raw = 0, Fpc = 1, FpcBsel = 2 };

struct EncodedBlock {
    BlockMode mode = BlockMode::Raw;
    Bytes bytes;
    bool prefix_bsel = false;
    bool residual_bsel = false;
};

EncodedBlock encode_block(const Bytes& block, const Model& model);
Bytes decode_block(const Bytes& encoded, const Model& model);

}
