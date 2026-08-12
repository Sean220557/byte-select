#pragma once

#include "byte_select/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bsel {

enum class MccPlacementMode {
    AlignedRecords,
    SegmentPackingV1,
    RegionFfdV2,
    TailSplitV3,
    TwoEndedTailV4,
    SpacedPaddingV5
};

struct MccConfig {
    std::uint64_t base_address = 0;
    std::size_t alignment = 64;
    std::size_t metadata_granularity = 4096;
    bool fill_padding = true;
    MccPlacementMode placement_mode = MccPlacementMode::RegionFfdV2;
};

struct MccEntry {
    std::uint64_t logical_block = 0;
    std::uint64_t physical_address = 0;
    std::uint64_t metadata_region = 0;
    std::size_t segment_offset = 0;
    std::size_t stored_bytes = 0;
    bool compressed = false;
    std::size_t set_index = 0;
    std::size_t metadata = 0;
    std::uint64_t tail_address = 0;
    std::uint64_t tail_metadata_region = 0;
    std::size_t tail_offset = 0;
    std::size_t tail_bytes = 0;
};

struct MccStats {
    std::uint64_t blocks = 0;
    std::uint64_t compressed_blocks = 0;
    std::uint64_t original_bytes = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t padding_bytes = 0;
    std::uint64_t physical_bytes = 0;
    std::uint64_t metadata_regions = 0;

    double quantized_compression_ratio() const;
    double storage_ratio() const;
    double compressed_fraction() const;
};

struct MccLayout {
    std::vector<MccEntry> entries;
    MccStats stats;
};

MccLayout place_blocks_in_memory(const std::vector<Block>& blocks, const Model& model,
                                 MccConfig config = {});
MccLayout place_stored_blocks_in_memory(const std::vector<std::size_t>& stored_sizes,
                                        std::size_t block_size,
                                        MccConfig config = {});

}  // namespace bsel
