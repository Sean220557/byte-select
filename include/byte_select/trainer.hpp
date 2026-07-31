#pragma once

#include "byte_select/pattern.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bsel {

enum class SelectionMode {
    LazyExact,
    Exhaustive
};

struct TrainingConfig {
    std::size_t block_size = 64;
    std::size_t dictionary_size = 31;
    std::size_t max_patterns = 256;
    std::uint64_t frequency_threshold = 1;
    SelectionMode selection_mode = SelectionMode::LazyExact;
};

struct TrainingStats {
    std::uint64_t blocks_seen = 0;
    std::size_t distinct_patterns = 0;
    std::size_t after_frequency_filter = 0;
    std::size_t maximal_patterns = 0;
    std::size_t combined_patterns = 0;
    std::uint64_t represented_blocks = 0;
    std::uint64_t coverage_evaluations = 0;
    std::uint64_t coverage_recomputations_skipped = 0;
};

struct TrainedPatterns {
    std::vector<Pattern> patterns;
    std::vector<std::uint64_t> marginal_counts;
    TrainingStats stats;
};

class Trainer {
public:
    explicit Trainer(TrainingConfig config);

    TrainedPatterns train(const std::vector<Block>& blocks) const;
    TrainedPatterns train_counts(
        const std::unordered_map<Pattern, std::uint64_t, PatternHash>& counts) const;

private:
    TrainingConfig config_;
};

}  // namespace bsel
