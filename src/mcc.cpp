#include "byte_select/mcc.hpp"

#include <limits>
#include <stdexcept>
#include <algorithm>
#include <vector>

namespace bsel {
namespace {

std::uint64_t align_up(std::uint64_t value, std::size_t alignment) {
    if (alignment <= 1) {
        return value;
    }
    const auto align = static_cast<std::uint64_t>(alignment);
    const auto remainder = value % align;
    if (remainder == 0) {
        return value;
    }
    if (value > std::numeric_limits<std::uint64_t>::max() - (align - remainder)) {
        throw std::overflow_error("MCC physical address overflow");
    }
    return value + (align - remainder);
}

}  // namespace

struct FreeRange {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
};

struct Segment {
    std::uint64_t base = 0;
    std::size_t used = 0;
};

struct TwoEndedSegment {
    std::uint64_t base = 0;
    std::size_t low_used = 0;
    std::size_t high_used = 0;
};

struct OccupiedRange {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct SpacedSegment {
    std::uint64_t base = 0;
    std::vector<OccupiedRange> occupied;
};

struct PendingRecord {
    std::size_t index = 0;
    std::size_t stored_bytes = 0;
};

double MccStats::quantized_compression_ratio() const {
    return physical_bytes == 0 ? 0.0
                               : static_cast<double>(original_bytes) /
                                     static_cast<double>(physical_bytes);
}

double MccStats::storage_ratio() const {
    return quantized_compression_ratio();
}

double MccStats::compressed_fraction() const {
    return blocks == 0 ? 0.0
                       : static_cast<double>(compressed_blocks) /
                             static_cast<double>(blocks);
}

void account_entry(MccLayout& layout, std::size_t block_size, std::size_t stored_bytes) {
    ++layout.stats.blocks;
    layout.stats.original_bytes += block_size;
    layout.stats.stored_bytes += stored_bytes;
    layout.stats.compressed_blocks += stored_bytes < block_size ? 1U : 0U;
}

void finish_layout(MccLayout& layout, std::uint64_t base_address,
                   std::uint64_t cursor, const MccConfig& config) {
    layout.stats.physical_bytes = align_up(cursor, config.alignment) - base_address;
    layout.stats.padding_bytes = layout.stats.physical_bytes - layout.stats.stored_bytes;
    if (layout.stats.physical_bytes != 0) {
        layout.stats.metadata_regions =
            (layout.stats.physical_bytes + config.metadata_granularity - 1U) /
            config.metadata_granularity;
    }
}

MccLayout place_region_ffd_v2(const std::vector<std::size_t>& stored_sizes,
                              std::size_t block_size, MccConfig config) {
    MccLayout layout;
    layout.entries.resize(stored_sizes.size());
    const auto logical_blocks_per_region =
        std::max<std::size_t>(1, config.metadata_granularity / block_size);
    std::uint64_t cursor = align_up(config.base_address, config.alignment);
    std::vector<Segment> bins;

    for (std::size_t begin = 0; begin < stored_sizes.size();
         begin += logical_blocks_per_region) {
        const auto end = std::min(stored_sizes.size(), begin + logical_blocks_per_region);
        std::vector<PendingRecord> small;
        small.reserve(end - begin);
        for (std::size_t i = begin; i < end; ++i) {
            const auto stored_bytes = stored_sizes[i];
            if (stored_bytes == 0 || stored_bytes > block_size) {
                throw std::invalid_argument("MCC stored size must be within the block size");
            }
            account_entry(layout, block_size, stored_bytes);
            if (stored_bytes <= config.alignment) {
                small.push_back({i, stored_bytes});
            } else {
                cursor = align_up(cursor, config.alignment);
                const auto region = (cursor - config.base_address) / config.metadata_granularity;
                layout.entries[i] = {static_cast<std::uint64_t>(i), cursor, region, 0,
                                     stored_bytes, stored_bytes < block_size, 0, 0};
                cursor += align_up(stored_bytes, config.alignment);
            }
        }

        std::sort(small.begin(), small.end(), [](const auto& left, const auto& right) {
            if (left.stored_bytes != right.stored_bytes) {
                return left.stored_bytes > right.stored_bytes;
            }
            return left.index < right.index;
        });

        for (const auto& record : small) {
            bool placed = false;
            for (auto& bin : bins) {
                if (record.stored_bytes <= config.alignment - bin.used) {
                    const auto address = bin.base + bin.used;
                    const auto region =
                        (address - config.base_address) / config.metadata_granularity;
                    layout.entries[record.index] = {
                        static_cast<std::uint64_t>(record.index), address, region,
                        bin.used, record.stored_bytes, record.stored_bytes < block_size,
                        0, 0};
                    bin.used += record.stored_bytes;
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                cursor = align_up(cursor, config.alignment);
                Segment bin{cursor, record.stored_bytes};
                const auto region = (cursor - config.base_address) / config.metadata_granularity;
                layout.entries[record.index] = {
                    static_cast<std::uint64_t>(record.index), cursor, region, 0,
                    record.stored_bytes, record.stored_bytes < block_size, 0, 0};
                bins.push_back(bin);
                cursor += config.alignment;
            }
        }
    }

    finish_layout(layout, config.base_address, cursor, config);
    return layout;
}

MccLayout place_tail_split_v3(const std::vector<std::size_t>& stored_sizes,
                              std::size_t block_size, MccConfig config) {
    MccLayout layout;
    layout.entries.resize(stored_sizes.size());
    const auto logical_blocks_per_region =
        std::max<std::size_t>(1, config.metadata_granularity / block_size);
    std::uint64_t cursor = align_up(config.base_address, config.alignment);
    std::vector<Segment> bins;

    for (std::size_t begin = 0; begin < stored_sizes.size();
         begin += logical_blocks_per_region) {
        const auto end = std::min(stored_sizes.size(), begin + logical_blocks_per_region);
        std::vector<PendingRecord> tails;
        tails.reserve(end - begin);
        for (std::size_t i = begin; i < end; ++i) {
            const auto stored_bytes = stored_sizes[i];
            if (stored_bytes == 0 || stored_bytes > block_size) {
                throw std::invalid_argument("MCC stored size must be within the block size");
            }
            account_entry(layout, block_size, stored_bytes);
            const auto full_segments = stored_bytes / config.alignment;
            const auto tail = stored_bytes % config.alignment;
            std::uint64_t address = cursor;
            std::size_t offset = 0;
            if (full_segments != 0) {
                cursor = align_up(cursor, config.alignment);
                address = cursor;
                cursor += full_segments * config.alignment;
            }
            if (tail != 0) {
                tails.push_back({i, tail});
                if (full_segments == 0) {
                    address = 0;
                    offset = 0;
                }
            } else {
                const auto region = (address - config.base_address) / config.metadata_granularity;
                layout.entries[i] = {static_cast<std::uint64_t>(i), address, region, offset,
                                     stored_bytes, stored_bytes < block_size, 0, 0};
            }
            if (full_segments != 0 && tail != 0) {
                const auto region = (address - config.base_address) / config.metadata_granularity;
                layout.entries[i] = {static_cast<std::uint64_t>(i), address, region, 0,
                                     stored_bytes, stored_bytes < block_size, 0, 0};
            }
        }

        std::sort(tails.begin(), tails.end(), [](const auto& left, const auto& right) {
            if (left.stored_bytes != right.stored_bytes) {
                return left.stored_bytes > right.stored_bytes;
            }
            return left.index < right.index;
        });

        for (const auto& tail : tails) {
            bool placed = false;
            for (auto& bin : bins) {
                if (tail.stored_bytes <= config.alignment - bin.used) {
                    const auto address = bin.base + bin.used;
                    const auto region =
                        (address - config.base_address) / config.metadata_granularity;
                    if (layout.entries[tail.index].stored_bytes == 0) {
                        layout.entries[tail.index] = {
                            static_cast<std::uint64_t>(tail.index), address, region,
                            bin.used, stored_sizes[tail.index],
                            stored_sizes[tail.index] < block_size, 0, 0};
                    }
                    bin.used += tail.stored_bytes;
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                cursor = align_up(cursor, config.alignment);
                Segment bin{cursor, tail.stored_bytes};
                const auto region = (cursor - config.base_address) / config.metadata_granularity;
                if (layout.entries[tail.index].stored_bytes == 0) {
                    layout.entries[tail.index] = {
                        static_cast<std::uint64_t>(tail.index), cursor, region, 0,
                        stored_sizes[tail.index], stored_sizes[tail.index] < block_size,
                        0, 0};
                }
                bins.push_back(bin);
                cursor += config.alignment;
            }
        }
    }

    finish_layout(layout, config.base_address, cursor, config);
    return layout;
}

MccLayout place_two_ended_tail_v4(const std::vector<std::size_t>& stored_sizes,
                                  std::size_t block_size, MccConfig config) {
    MccLayout layout;
    layout.entries.resize(stored_sizes.size());
    std::uint64_t cursor = align_up(config.base_address, config.alignment);
    std::vector<TwoEndedSegment> tail_segments;

    for (std::size_t i = 0; i < stored_sizes.size(); ++i) {
        const auto stored_bytes = stored_sizes[i];
        if (stored_bytes == 0 || stored_bytes > block_size) {
            throw std::invalid_argument("MCC stored size must be within the block size");
        }
        account_entry(layout, block_size, stored_bytes);
        const auto full_bytes = (stored_bytes / config.alignment) * config.alignment;
        const auto tail_bytes = stored_bytes % config.alignment;
        auto& entry = layout.entries[i];
        entry.logical_block = static_cast<std::uint64_t>(i);
        entry.stored_bytes = stored_bytes;
        entry.compressed = stored_bytes < block_size;

        if (full_bytes != 0) {
            cursor = align_up(cursor, config.alignment);
            entry.physical_address = cursor;
            entry.metadata_region =
                (cursor - config.base_address) / config.metadata_granularity;
            cursor += full_bytes;
        }
        if (tail_bytes == 0) {
            continue;
        }

        std::size_t best = tail_segments.size();
        std::size_t best_gap = 0;
        for (std::size_t segment = 0; segment < tail_segments.size(); ++segment) {
            const auto& candidate = tail_segments[segment];
            if (candidate.high_used != 0 ||
                candidate.low_used + tail_bytes > config.alignment) {
                continue;
            }
            const auto gap = config.alignment - candidate.low_used - tail_bytes;
            if (best == tail_segments.size() || gap > best_gap) {
                best = segment;
                best_gap = gap;
            }
        }

        std::uint64_t tail_address = 0;
        std::size_t tail_offset = 0;
        if (best != tail_segments.size()) {
            auto& segment = tail_segments[best];
            segment.high_used = tail_bytes;
            tail_offset = config.alignment - tail_bytes;
            tail_address = segment.base + tail_offset;
        } else {
            cursor = align_up(cursor, config.alignment);
            tail_address = cursor;
            tail_segments.push_back({cursor, tail_bytes, 0});
            cursor += config.alignment;
        }

        const auto tail_region =
            (tail_address - config.base_address) / config.metadata_granularity;
        if (full_bytes == 0) {
            entry.physical_address = tail_address;
            entry.metadata_region = tail_region;
            entry.segment_offset = tail_offset;
        } else {
            entry.tail_address = tail_address;
            entry.tail_metadata_region = tail_region;
            entry.tail_offset = tail_offset;
            entry.tail_bytes = tail_bytes;
        }
    }

    finish_layout(layout, config.base_address, cursor, config);
    return layout;
}

MccLayout place_spaced_padding_v5(const std::vector<std::size_t>& stored_sizes,
                                  std::size_t block_size, MccConfig config) {
    constexpr std::size_t guard_bytes = 1;
    struct Candidate {
        std::size_t segment = 0;
        std::size_t offset = 0;
        std::size_t distance = 0;
        bool found = false;
    };

    MccLayout layout;
    layout.entries.resize(stored_sizes.size());
    std::uint64_t cursor = align_up(config.base_address, config.alignment);
    std::vector<SpacedSegment> padding_segments;

    for (std::size_t i = 0; i < stored_sizes.size(); ++i) {
        const auto stored_bytes = stored_sizes[i];
        if (stored_bytes == 0 || stored_bytes > block_size) {
            throw std::invalid_argument("MCC stored size must be within the block size");
        }
        account_entry(layout, block_size, stored_bytes);
        const auto full_bytes = (stored_bytes / config.alignment) * config.alignment;
        const auto tail_bytes = stored_bytes % config.alignment;
        auto& entry = layout.entries[i];
        entry.logical_block = static_cast<std::uint64_t>(i);
        entry.stored_bytes = stored_bytes;
        entry.compressed = stored_bytes < block_size;

        if (full_bytes != 0) {
            cursor = align_up(cursor, config.alignment);
            entry.physical_address = cursor;
            entry.metadata_region =
                (cursor - config.base_address) / config.metadata_granularity;
            cursor += full_bytes;
        }
        if (tail_bytes == 0) {
            continue;
        }

        Candidate best;
        const auto active_region =
            (cursor - config.base_address) / config.metadata_granularity;
        for (std::size_t segment_index = 0;
             segment_index < padding_segments.size(); ++segment_index) {
            const auto& segment = padding_segments[segment_index];
            if ((segment.base - config.base_address) /
                    config.metadata_granularity != active_region) {
                continue;
            }
            for (std::size_t gap_index = 0;
                 gap_index <= segment.occupied.size(); ++gap_index) {
                const bool has_left = gap_index != 0;
                const bool has_right = gap_index != segment.occupied.size();
                const auto gap_begin = has_left ? segment.occupied[gap_index - 1].end : 0U;
                const auto gap_end =
                    has_right ? segment.occupied[gap_index].begin : config.alignment;
                const auto usable_begin = gap_begin + (has_left ? guard_bytes : 0U);
                const auto usable_end = gap_end -
                    ((has_right && gap_end != 0) ? guard_bytes : 0U);
                if (usable_end < usable_begin ||
                    tail_bytes > usable_end - usable_begin) {
                    continue;
                }

                std::size_t offset = usable_begin;
                if (has_left && !has_right) {
                    offset = usable_end - tail_bytes;
                } else if (has_left && has_right) {
                    offset = usable_begin +
                             (usable_end - usable_begin - tail_bytes) / 2U;
                }
                const auto left_distance =
                    has_left ? offset - gap_begin : config.alignment;
                const auto right_distance =
                    has_right ? gap_end - (offset + tail_bytes) : config.alignment;
                const auto distance = std::min(left_distance, right_distance);
                if (!best.found || distance > best.distance) {
                    best = {segment_index, offset, distance, true};
                }
            }
        }

        std::uint64_t tail_address = 0;
        std::size_t tail_offset = 0;
        if (best.found) {
            auto& segment = padding_segments[best.segment];
            tail_offset = best.offset;
            tail_address = segment.base + tail_offset;
            segment.occupied.push_back({tail_offset, tail_offset + tail_bytes});
            std::sort(segment.occupied.begin(), segment.occupied.end(),
                      [](const auto& left, const auto& right) {
                          return left.begin < right.begin;
                      });
        } else {
            cursor = align_up(cursor, config.alignment);
            tail_address = cursor;
            padding_segments.push_back({cursor, {{0, tail_bytes}}});
            cursor += config.alignment;
        }

        const auto tail_region =
            (tail_address - config.base_address) / config.metadata_granularity;
        if (full_bytes == 0) {
            entry.physical_address = tail_address;
            entry.metadata_region = tail_region;
            entry.segment_offset = tail_offset;
        } else {
            entry.tail_address = tail_address;
            entry.tail_metadata_region = tail_region;
            entry.tail_offset = tail_offset;
            entry.tail_bytes = tail_bytes;
        }
    }

    finish_layout(layout, config.base_address, cursor, config);
    return layout;
}

MccLayout place_blocks_in_memory(const std::vector<Block>& blocks, const Model& model,
                                 MccConfig config) {
    validate_model(model);
    std::vector<std::size_t> stored_sizes;
    stored_sizes.reserve(blocks.size());
    MccLayout layout;
    layout.entries.reserve(blocks.size());
    for (const auto& block : blocks) {
        const auto encoded = compress(block, model);
        stored_sizes.push_back(encoded.compressed ? encoded.encoded_size
                                                  : encoded.original_size);
        layout.stats.compressed_blocks += encoded.compressed ? 1U : 0U;
        layout.entries.push_back({0, 0, 0, 0, stored_sizes.back(), encoded.compressed,
                                  encoded.set_index, encoded.metadata});
    }

    auto placed = place_stored_blocks_in_memory(stored_sizes, model.block_size, config);
    placed.stats.compressed_blocks = layout.stats.compressed_blocks;
    for (std::size_t i = 0; i < placed.entries.size(); ++i) {
        placed.entries[i].compressed = layout.entries[i].compressed;
        placed.entries[i].set_index = layout.entries[i].set_index;
        placed.entries[i].metadata = layout.entries[i].metadata;
    }
    return placed;
}

MccLayout place_stored_blocks_in_memory(const std::vector<std::size_t>& stored_sizes,
                                        std::size_t block_size,
                                        MccConfig config) {
    if (config.alignment == 0) {
        throw std::invalid_argument("MCC alignment must be non-zero");
    }
    if (config.metadata_granularity == 0) {
        throw std::invalid_argument("MCC metadata granularity must be non-zero");
    }
    if (config.placement_mode == MccPlacementMode::RegionFfdV2) {
        return place_region_ffd_v2(stored_sizes, block_size, config);
    }
    if (config.placement_mode == MccPlacementMode::TailSplitV3) {
        return place_tail_split_v3(stored_sizes, block_size, config);
    }
    if (config.placement_mode == MccPlacementMode::TwoEndedTailV4) {
        return place_two_ended_tail_v4(stored_sizes, block_size, config);
    }
    if (config.placement_mode == MccPlacementMode::SpacedPaddingV5) {
        return place_spaced_padding_v5(stored_sizes, block_size, config);
    }

    MccLayout layout;
    layout.entries.reserve(stored_sizes.size());
    std::uint64_t cursor = config.base_address;
    std::vector<FreeRange> padding_ranges;
    std::vector<Segment> segments;

    for (std::size_t i = 0; i < stored_sizes.size(); ++i) {
        const auto stored_bytes = stored_sizes[i];
        if (stored_bytes == 0 || stored_bytes > block_size) {
            throw std::invalid_argument("MCC stored size must be within the block size");
        }
        std::uint64_t physical_address = 0;
        std::size_t segment_offset = 0;
        bool placed_in_padding = false;

        if (config.placement_mode == MccPlacementMode::SegmentPackingV1 &&
            stored_bytes <= config.alignment) {
            for (auto& segment : segments) {
                if (stored_bytes <= config.alignment - segment.used) {
                    segment_offset = segment.used;
                    physical_address = segment.base + segment_offset;
                    segment.used += stored_bytes;
                    placed_in_padding = true;
                    break;
                }
            }
            if (!placed_in_padding) {
                const auto aligned_cursor = align_up(cursor, config.alignment);
                if (stored_bytes > std::numeric_limits<std::uint64_t>::max() -
                                       aligned_cursor) {
                    throw std::overflow_error("MCC physical address overflow");
                }
                physical_address = aligned_cursor;
                segments.push_back({aligned_cursor, stored_bytes});
                cursor = aligned_cursor + config.alignment;
                placed_in_padding = true;
            }
        }

        if (!placed_in_padding && config.fill_padding) {
            for (auto it = padding_ranges.begin(); it != padding_ranges.end(); ++it) {
                if (stored_bytes <= it->bytes) {
                    physical_address = it->address;
                    it->address += static_cast<std::uint64_t>(stored_bytes);
                    it->bytes -= static_cast<std::uint64_t>(stored_bytes);
                    if (it->bytes == 0) {
                        padding_ranges.erase(it);
                    }
                    placed_in_padding = true;
                    break;
                }
            }
        }

        if (!placed_in_padding) {
            const auto aligned_cursor = align_up(cursor, config.alignment);
            const auto tail_padding = aligned_cursor - cursor;
            if (config.fill_padding && tail_padding != 0 && stored_bytes <= tail_padding) {
                physical_address = cursor;
                cursor += static_cast<std::uint64_t>(stored_bytes);
                placed_in_padding = true;
            } else {
                if (tail_padding != 0) {
                    padding_ranges.push_back({cursor, tail_padding});
                }
                cursor = aligned_cursor;
                if (stored_bytes > std::numeric_limits<std::uint64_t>::max() - cursor) {
                    throw std::overflow_error("MCC physical address overflow");
                }
                physical_address = cursor;
                cursor += static_cast<std::uint64_t>(stored_bytes);
            }
        }

        const auto metadata_region =
            (physical_address - config.base_address) / config.metadata_granularity;
        layout.entries.push_back(
            {static_cast<std::uint64_t>(i), physical_address, metadata_region,
             segment_offset, stored_bytes, stored_bytes < block_size, 0, 0});

        account_entry(layout, block_size, stored_bytes);
    }

    finish_layout(layout, config.base_address, cursor, config);
    return layout;
}

}  // namespace bsel
