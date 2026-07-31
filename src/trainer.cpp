#include "byte_select/trainer.hpp"

#include <algorithm>
#include <numeric>
#include <queue>
#include <stdexcept>

namespace bsel {
namespace {

using CountMap = std::unordered_map<Pattern, std::uint64_t, PatternHash>;

std::uint64_t coverage(const Pattern& pattern, const CountMap& atoms) {
    std::uint64_t total = 0;
    for (const auto& [atom, count] : atoms) {
        if (atom.less_equal(pattern)) {
            total += count;
        }
    }
    return total;
}

std::vector<Pattern> maximal_patterns(const CountMap& atoms) {
    std::vector<Pattern> candidates;
    candidates.reserve(atoms.size());
    for (const auto& [pattern, count] : atoms) {
        (void)count;
        candidates.push_back(pattern);
    }
    std::sort(candidates.begin(), candidates.end());

    std::vector<Pattern> maximal;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        bool is_maximal = true;
        for (std::size_t j = 0; j < candidates.size(); ++j) {
            if (i != j && candidates[i].less_equal(candidates[j])) {
                is_maximal = false;
                break;
            }
        }
        if (is_maximal) {
            maximal.push_back(candidates[i]);
        }
    }
    return maximal;
}

void remove_below(std::vector<Pattern>& patterns, const Pattern& upper) {
    patterns.erase(
        std::remove_if(patterns.begin(), patterns.end(),
                       [&](const Pattern& pattern) {
                           return !(pattern == upper) && pattern.less_equal(upper);
                       }),
        patterns.end());
}

std::vector<Pattern> combine_greedily(std::vector<Pattern> patterns,
                                      std::size_t dictionary_size) {
    std::sort(patterns.begin(), patterns.end());
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < patterns.size() && !changed; ++i) {
            for (std::size_t j = i + 1; j < patterns.size(); ++j) {
                Pattern upper = patterns[i].least_upper_bound(patterns[j]);
                if (upper.rank() > dictionary_size) {
                    continue;
                }
                patterns.erase(patterns.begin() + static_cast<std::ptrdiff_t>(j));
                patterns.erase(patterns.begin() + static_cast<std::ptrdiff_t>(i));
                remove_below(patterns, upper);
                if (std::find(patterns.begin(), patterns.end(), upper) == patterns.end()) {
                    patterns.push_back(std::move(upper));
                }
                std::sort(patterns.begin(), patterns.end());
                changed = true;
                break;
            }
        }
    }
    return patterns;
}

struct LazyCandidate {
    std::size_t index;
    std::uint64_t upper_bound;
    std::size_t iteration;
};

struct LazyCandidateLess {
    const std::vector<Pattern>* patterns;

    bool operator()(const LazyCandidate& left, const LazyCandidate& right) const {
        if (left.upper_bound != right.upper_bound) {
            return left.upper_bound < right.upper_bound;
        }
        return (*patterns)[right.index] < (*patterns)[left.index];
    }
};

}  // namespace

Trainer::Trainer(TrainingConfig config) : config_(config) {
    if (config_.block_size == 0 || config_.dictionary_size == 0 ||
        config_.max_patterns == 0 || config_.frequency_threshold == 0) {
        throw std::invalid_argument("training parameters must be non-zero");
    }
}

TrainedPatterns Trainer::train(const std::vector<Block>& blocks) const {
    CountMap counts;
    for (const auto& block : blocks) {
        if (block.size() != config_.block_size) {
            throw std::invalid_argument("training block has incorrect size");
        }
        ++counts[Pattern::simplest(block)];
    }
    auto result = train_counts(counts);
    result.stats.blocks_seen = blocks.size();
    return result;
}

TrainedPatterns Trainer::train_counts(const CountMap& counts) const {
    TrainedPatterns result;
    result.stats.distinct_patterns = counts.size();

    // Phase 1: retain frequent simplest patterns.
    CountMap atoms;
    for (const auto& [pattern, count] : counts) {
        if (pattern.size() != config_.block_size) {
            throw std::invalid_argument("counted pattern has incorrect block size");
        }
        if (count >= config_.frequency_threshold &&
            pattern.rank() <= config_.dictionary_size) {
            atoms.emplace(pattern, count);
            result.stats.represented_blocks += count;
        }
    }
    result.stats.after_frequency_filter = atoms.size();

    // Phase 2: keep only maximal patterns while retaining all atoms and counts.
    auto candidates = maximal_patterns(atoms);
    result.stats.maximal_patterns = candidates.size();

    // Phase 3: greedily replace pairs by a legal least upper bound.
    candidates = combine_greedily(std::move(candidates), config_.dictionary_size);
    result.stats.combined_patterns = candidates.size();

    // Phase 4: repeatedly choose the pattern with greatest uncovered count.
    CountMap uncovered = atoms;
    if (config_.selection_mode == SelectionMode::Exhaustive) {
        while (!candidates.empty() && result.patterns.size() < config_.max_patterns) {
            auto best = candidates.begin();
            std::uint64_t best_count = coverage(*best, uncovered);
            ++result.stats.coverage_evaluations;
            for (auto it = std::next(candidates.begin()); it != candidates.end(); ++it) {
                const auto count = coverage(*it, uncovered);
                ++result.stats.coverage_evaluations;
                if (count > best_count || (count == best_count && *it < *best)) {
                    best = it;
                    best_count = count;
                }
            }
            if (best_count == 0) {
                break;
            }
            Pattern selected = *best;
            result.patterns.push_back(selected);
            result.marginal_counts.push_back(best_count);
            candidates.erase(best);
            for (auto it = uncovered.begin(); it != uncovered.end();) {
                it = it->first.less_equal(selected) ? uncovered.erase(it) : std::next(it);
            }
        }
    } else {
        // Counts only decrease as atoms are removed. Cached counts are admissible
        // upper bounds, so a max-heap can skip candidates that cannot beat the top.
        LazyCandidateLess less{&candidates};
        std::priority_queue<LazyCandidate, std::vector<LazyCandidate>, LazyCandidateLess>
            queue(less);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto count = coverage(candidates[i], uncovered);
            ++result.stats.coverage_evaluations;
            queue.push({i, count, 0});
        }
        std::vector<bool> selected(candidates.size(), false);
        std::size_t iteration = 0;
        while (!queue.empty() && result.patterns.size() < config_.max_patterns) {
            auto top = queue.top();
            queue.pop();
            if (selected[top.index]) {
                continue;
            }
            if (top.iteration != iteration) {
                top.upper_bound = coverage(candidates[top.index], uncovered);
                top.iteration = iteration;
                ++result.stats.coverage_evaluations;
                queue.push(top);
                continue;
            }
            if (top.upper_bound == 0) {
                break;
            }
            selected[top.index] = true;
            result.patterns.push_back(candidates[top.index]);
            result.marginal_counts.push_back(top.upper_bound);
            const auto& chosen = candidates[top.index];
            for (auto it = uncovered.begin(); it != uncovered.end();) {
                it = it->first.less_equal(chosen) ? uncovered.erase(it) : std::next(it);
            }
            ++iteration;
            result.stats.coverage_recomputations_skipped += queue.size();
        }
    }
    return result;
}

}  // namespace bsel
