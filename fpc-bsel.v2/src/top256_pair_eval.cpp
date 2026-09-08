#include "fpc_bsel/pair_reorder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>

namespace {
using Lengths = std::array<std::uint16_t, 16>;

bool read_region(std::istream& input, Lengths& result) {
    std::uint32_t size = 0;
    if (!(input >> size)) return false;
    result[0] = static_cast<std::uint16_t>(std::min(size, 256U));
    for (unsigned index = 1; index < result.size(); ++index) {
        if (!(input >> size)) throw std::runtime_error("truncated size-list region");
        result[index] = static_cast<std::uint16_t>(std::min(size, 256U));
    }
    return true;
}

std::uint32_t next_power_of_two(std::uint32_t value) {
    if (value <= 1) return 1;
    --value;
    value |= value >> 1U; value |= value >> 2U; value |= value >> 4U;
    value |= value >> 8U; value |= value >> 16U;
    return value + 1U;
}

std::uint64_t uniform_pair_bytes(const Lengths& lengths) {
    std::uint32_t maximum = 0;
    for (unsigned pair = 0; pair < 8; ++pair)
        maximum = std::max<std::uint32_t>(maximum,
            lengths[pair * 2] + lengths[pair * 2 + 1]);
    return 8ULL * next_power_of_two(maximum);
}

std::uint64_t independent_pair_bytes(const Lengths& lengths) {
    std::uint64_t total = 0;
    for (unsigned pair = 0; pair < 8; ++pair)
        total += next_power_of_two(lengths[pair * 2] + lengths[pair * 2 + 1]);
    return total;
}

// Exact minimum-cost perfect matching for independent pair tiers. There are
// only 16 payloads, so a 2^16 dynamic program is small and deterministic.
std::uint64_t optimal_independent_pair_bytes(const Lengths& lengths) {
    constexpr std::uint32_t all = (1U << 16U) - 1U;
    constexpr std::uint32_t infinity = 0xffffffffU;
    std::array<std::uint32_t, 1U << 16U> cost{};
    cost.fill(infinity);
    cost[0] = 0;
    for (std::uint32_t mask = 1; mask <= all; ++mask) {
        unsigned population = 0;
        for (auto bits = mask; bits; bits >>= 1U) population += bits & 1U;
        if (population & 1U) continue;
        unsigned first = 0;
        while (!(mask & (1U << first))) ++first;
        const auto without_first = mask & ~(1U << first);
        for (unsigned second = first + 1; second < 16; ++second) {
            if (!(without_first & (1U << second))) continue;
            const auto remainder = without_first & ~(1U << second);
            if (cost[remainder] == infinity) continue;
            cost[mask] = std::min(cost[mask], cost[remainder] + next_power_of_two(
                lengths[first] + lengths[second]));
        }
    }
    return cost[all];
}

// Extreme pairing is optimal for minimizing the maximum of all pair sums.
// Stable ordering makes the permutation reproducible from the existing
// ordered length vector; no permutation table is serialized.
Lengths globally_optimal_uniform_layout(const Lengths& lengths) {
    std::array<std::uint8_t, 16> order{};
    std::iota(order.begin(), order.end(), std::uint8_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::uint8_t lhs, std::uint8_t rhs) {
        return lengths[lhs] < lengths[rhs];
    });
    Lengths result{};
    for (unsigned pair = 0; pair < 8; ++pair) {
        result[pair * 2] = lengths[order[pair]];
        result[pair * 2 + 1] = lengths[order[15U - pair]];
    }
    return result;
}

Lengths apply_local_swaps(const Lengths& lengths,
                          const fpc_bsel::PairReorderResult& plan) {
    auto result = lengths;
    for (const auto& swap : plan.swaps)
        std::swap(result[swap.bad_payload], result[swap.good_payload]);
    return result;
}

struct IndependentGreedyResult {
    Lengths lengths{};
    std::uint32_t swaps = 0;
};

IndependentGreedyResult optimize_independent_greedy(const Lengths& input) {
    IndependentGreedyResult result{input, 0};
    for (unsigned round = 0; round < 16; ++round) {
        std::uint32_t best_gain = 0;
        std::uint8_t best_left = 0, best_right = 0;
        for (std::uint8_t left = 0; left < 16; ++left) {
            for (std::uint8_t right = left + 1; right < 16; ++right) {
                if ((left >> 1U) == (right >> 1U)) continue;
                const auto left_other = static_cast<std::uint8_t>(left ^ 1U);
                const auto right_other = static_cast<std::uint8_t>(right ^ 1U);
                const auto before = next_power_of_two(result.lengths[left] +
                                      result.lengths[left_other]) +
                                    next_power_of_two(result.lengths[right] +
                                      result.lengths[right_other]);
                const auto after = next_power_of_two(result.lengths[right] +
                                     result.lengths[left_other]) +
                                   next_power_of_two(result.lengths[left] +
                                     result.lengths[right_other]);
                const auto gain = before > after ? before - after : 0U;
                // Packed slot ids provide deterministic rightmost tie-breaking.
                const auto choice = static_cast<std::uint16_t>((left << 4U) | right);
                const auto best_choice = static_cast<std::uint16_t>(
                    (best_left << 4U) | best_right);
                if (gain > best_gain || (gain && gain == best_gain && choice > best_choice)) {
                    best_gain = gain;
                    best_left = left;
                    best_right = right;
                }
            }
        }
        if (!best_gain) break;
        std::swap(result.lengths[best_left], result.lengths[best_right]);
        ++result.swaps;
    }
    return result;
}

double fraction(std::uint64_t bytes, std::uint64_t original) {
    return static_cast<double>(bytes) / static_cast<double>(original);
}
}  // namespace

int main(int argc, char** argv) try {
    if (argc < 4 || argc > 6) throw std::invalid_argument(
        "usage: top256-pair-eval PLAIN_SIZES TOP256_SIZES ORIGINAL_BYTES [MODEL_BYTES] [--exact-matrix]");
    std::ifstream plain_input(argv[1]);
    std::ifstream top256_input(argv[2]);
    if (!plain_input || !top256_input) throw std::runtime_error("cannot open size list");
    const auto original = std::stoull(argv[3]);
    const auto model_bytes = argc >= 5 ? std::stoull(argv[4]) : 0ULL;
    const bool exact_matrix = argc == 6 && std::string(argv[5]) == "--exact-matrix";
    if (argc == 6 && !exact_matrix) throw std::invalid_argument("unknown option");
    std::uint64_t regions = 0;

    std::uint64_t plain_uniform = 0, top256_uniform = 0, local_uniform = 0;
    std::uint64_t global_uniform = 0, independent_original = 0;
    std::uint64_t plain_independent = 0, plain_independent_optimal = 0;
    std::uint64_t local_independent = 0, global_independent = 0;
    std::uint64_t top256_independent_optimal = 0;
    std::uint64_t top256_independent_greedy = 0;
    std::uint64_t independent_greedy_swaps = 0, independent_greedy_regions = 0;
    std::uint64_t local_swaps = 0, local_reordered_regions = 0;
    std::uint64_t global_reordered_regions = 0;
    for (;;) {
        Lengths plain_lengths{}, top256_lengths{};
        const bool have_plain = read_region(plain_input, plain_lengths);
        const bool have_top256 = read_region(top256_input, top256_lengths);
        if (have_plain != have_top256) throw std::runtime_error("incompatible size lists");
        if (!have_plain) break;
        ++regions;
        fpc_bsel::TierReorderRegion local_region{};
        for (unsigned index = 0; index < 16; ++index) {
            local_region.payload_bits[index] = top256_lengths[index];
        }
        plain_uniform += uniform_pair_bytes(plain_lengths);
        top256_uniform += uniform_pair_bytes(top256_lengths);
        plain_independent += independent_pair_bytes(plain_lengths);
        if (exact_matrix)
            plain_independent_optimal += optimal_independent_pair_bytes(plain_lengths);
        independent_original += independent_pair_bytes(top256_lengths);
        const auto independent_greedy = optimize_independent_greedy(top256_lengths);
        top256_independent_greedy += independent_pair_bytes(independent_greedy.lengths);
        independent_greedy_swaps += independent_greedy.swaps;
        independent_greedy_regions += independent_greedy.swaps != 0;

        const auto local_plan = fpc_bsel::optimize_adjacent_pairs(
            local_region, fpc_bsel::TierReorderStrategy::MaximumMatching);
        const auto local_lengths = apply_local_swaps(top256_lengths, local_plan);
        local_uniform += uniform_pair_bytes(local_lengths);
        local_independent += independent_pair_bytes(local_lengths);
        local_swaps += local_plan.swaps.size();
        local_reordered_regions += local_plan.reordered;

        const auto global_lengths = globally_optimal_uniform_layout(top256_lengths);
        const auto global_bytes = uniform_pair_bytes(global_lengths);
        global_uniform += global_bytes;
        global_independent += independent_pair_bytes(global_lengths);
        global_reordered_regions += global_bytes < uniform_pair_bytes(top256_lengths);
        if (exact_matrix)
            top256_independent_optimal += optimal_independent_pair_bytes(top256_lengths);
    }
    if (!regions) throw std::runtime_error("empty size lists");

    // Reorder flags cost one bit per region. Independent pair tiers are
    // derived from the existing lengths, so they add no serialized metadata.
    const auto bitmap = (regions + 7U) / 8U;
    const auto local_total = local_uniform + bitmap;
    const auto global_total = global_uniform + bitmap;
    const auto common_length_bytes = regions * 32ULL;
    const auto top256_common_bytes = common_length_bytes + model_bytes;
    const auto optimal_independent_total = exact_matrix
        ? top256_independent_optimal + bitmap : 0ULL;
    const auto greedy_independent_total = top256_independent_greedy + bitmap;
    const auto fpc_independent_full_bytes = plain_independent + common_length_bytes;
    const auto top256_independent_full_bytes = independent_original + top256_common_bytes;
    const auto top256_swap_full_bytes = greedy_independent_total + top256_common_bytes;
    std::cout << std::fixed << std::setprecision(9)
      << "original_bytes=" << original
      << " fpc_quantized_bytes=" << plain_uniform
      << " fpc_quantized_fraction=" << fraction(plain_uniform, original)
      << " fpc_quantized_x=" << 1.0 / fraction(plain_uniform, original)
      << " top256_quantized_bytes=" << top256_uniform
      << " top256_quantized_fraction=" << fraction(top256_uniform, original)
      << " top256_quantized_x=" << 1.0 / fraction(top256_uniform, original)
      << " local_swap_bytes=" << local_total
      << " local_swap_fraction=" << fraction(local_total, original)
      << " local_swap_x=" << 1.0 / fraction(local_total, original)
      << " local_padding_eliminated_bytes=" << (top256_uniform - local_total)
      << " local_swaps=" << local_swaps
      << " local_reordered_regions=" << local_reordered_regions
      << " global_pair_bytes=" << global_total
      << " global_pair_fraction=" << fraction(global_total, original)
      << " global_pair_x=" << 1.0 / fraction(global_total, original)
      << " global_padding_eliminated_bytes=" << (top256_uniform - global_total)
      << " global_reordered_regions=" << global_reordered_regions
      << " independent_pair_bytes=" << independent_original
      << " independent_pair_fraction=" << fraction(independent_original, original)
      << " independent_pair_x=" << 1.0 / fraction(independent_original, original)
      << " independent_padding_eliminated_bytes="
      << (top256_uniform - independent_original)
      << " plain_independent_bytes=" << plain_independent
      << " plain_independent_optimal_bytes=" << plain_independent_optimal
      << " local_independent_bytes=" << (local_independent + bitmap)
      << " global_independent_bytes=" << (global_independent + bitmap)
      << " optimal_independent_bytes=" << optimal_independent_total
      << " optimal_independent_fraction="
      << (exact_matrix ? fraction(optimal_independent_total, original) : 0.0)
      << " optimal_independent_x="
      << (exact_matrix ? 1.0 / fraction(optimal_independent_total, original) : 0.0)
      << " greedy_independent_bytes=" << greedy_independent_total
      << " greedy_independent_fraction=" << fraction(greedy_independent_total, original)
      << " greedy_independent_x=" << 1.0 / fraction(greedy_independent_total, original)
      << " greedy_independent_swaps=" << independent_greedy_swaps
      << " greedy_independent_regions=" << independent_greedy_regions
      << " fpc_shared_full_fraction="
      << fraction(plain_uniform + common_length_bytes, original)
      << " fpc_independent_full_fraction="
      << fraction(plain_independent + common_length_bytes, original)
      << " fpc_independent_optimal_full_fraction="
      << (exact_matrix ? fraction(plain_independent_optimal + common_length_bytes + bitmap,
                                  original) : 0.0)
      << " top256_shared_full_fraction="
      << fraction(top256_uniform + top256_common_bytes, original)
      << " top256_local_shared_full_fraction="
      << fraction(local_total + top256_common_bytes, original)
      << " top256_global_shared_full_fraction="
      << fraction(global_total + top256_common_bytes, original)
      << " top256_independent_full_fraction="
      << fraction(independent_original + top256_common_bytes, original)
      << " top256_local_independent_full_fraction="
      << fraction(local_independent + bitmap + top256_common_bytes, original)
      << " top256_global_independent_full_fraction="
      << fraction(global_independent + bitmap + top256_common_bytes, original)
      << " top256_optimal_independent_full_fraction="
      << (exact_matrix ? fraction(optimal_independent_total + top256_common_bytes,
                                  original) : 0.0)
      << " top256_greedy_independent_full_fraction="
      << fraction(greedy_independent_total + top256_common_bytes, original)
      << " fpc_independent_full_bytes=" << fpc_independent_full_bytes
      << " fpc_independent_full_x="
      << 1.0 / fraction(fpc_independent_full_bytes, original)
      << " top256_independent_full_bytes=" << top256_independent_full_bytes
      << " top256_independent_full_x="
      << 1.0 / fraction(top256_independent_full_bytes, original)
      << " top256_swap_full_bytes=" << top256_swap_full_bytes
      << " top256_swap_full_fraction=" << fraction(top256_swap_full_bytes, original)
      << " top256_swap_full_x="
      << 1.0 / fraction(top256_swap_full_bytes, original)
      << " top256_swap_saved_bytes="
      << (top256_independent_full_bytes - top256_swap_full_bytes)
      << " common_length_metadata_bytes=" << common_length_bytes
      << " top256_model_bytes=" << model_bytes
      << " reorder_bitmap_bytes=" << bitmap
      << " independent_tier_metadata_bytes=0\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
}
