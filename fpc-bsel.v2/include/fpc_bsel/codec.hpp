#pragma once

#include "fpc_bsel/model.hpp"

namespace fpc_bsel {

enum class BlockMode : std::uint8_t { Raw = 0, Fpc = 1, FpcBsel = 2, FpcBitshuffle = 3 };

struct EncodedBlock {
    BlockMode mode = BlockMode::Raw;
    Bytes bytes;
    bool prefix_bsel = false;
    bool residual_bsel = false;
    bool prefix_id_inline = false;
};

struct EncodeOptions {
    bool enable_prefix_bsel = true;
    bool enable_residual_bsel = true;
};

EncodedBlock encode_block(const Bytes& block, const Model& model,
                          EncodeOptions options = {});
Bytes decode_block(const Bytes& encoded, const Model& model);

}
