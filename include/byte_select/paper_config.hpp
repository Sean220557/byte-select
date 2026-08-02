#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace bsel {

struct PaperTargetConfig {
    std::size_t target_size;
    std::size_t max_patterns;
    std::size_t metadata_bytes;
    std::size_t metadata_tag_bits = 0;
    std::size_t metadata_tag_value = 0;
};

// Returns the configurations reported in Section 7.1 of Byte-Select Compression.
// Valid names are bsel-256, bsel-4096, and bsel-1024-1024-128.
std::vector<PaperTargetConfig> paper_config(const std::string& name);

}  // namespace bsel
