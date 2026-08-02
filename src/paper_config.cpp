#include "byte_select/paper_config.hpp"

#include <stdexcept>

namespace bsel {

std::vector<PaperTargetConfig> paper_config(const std::string& name) {
    if (name == "bsel-256") {
        return {{32, 256, 1}};
    }
    if (name == "bsel-4096") {
        return {{32, 4096, 2}};
    }
    if (name == "bsel-1024-1024-128") {
        return {{32, 1024, 2, 1, 0}, {16, 1024, 2, 1, 0}, {8, 128, 1, 1, 1}};
    }
    throw std::invalid_argument(
        "paper config must be bsel-256, bsel-4096, or bsel-1024-1024-128");
}

}  // namespace bsel
