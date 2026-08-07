#pragma once

#include <cstdint>
#include <vector>

namespace fpc_bsel {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kBlockSize = 64;
constexpr std::size_t kWordCount = 16;
}
