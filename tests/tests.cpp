#include "byte_select/codec.hpp"
#include "byte_select/model_io.hpp"
#include "byte_select/trainer.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bsel::Pattern pattern(std::initializer_list<std::uint16_t> symbols) {
    return bsel::Pattern(std::vector<std::uint16_t>(symbols));
}

void test_pattern_algebra() {
    const bsel::Block block{7, 7, 9, 7};
    check(bsel::Pattern::simplest(block) == pattern({0, 0, 1, 0}),
          "simplest pattern follows first occurrence order");
    check(pattern({9, 9, 4, 9}) == pattern({0, 0, 1, 0}),
          "patterns are canonicalized");
    check(pattern({0, 0, 0, 0}).less_equal(pattern({0, 0, 1, 0})),
          "lattice partial order");
    check(!pattern({0, 0, 1, 0}).less_equal(pattern({0, 0, 0, 0})),
          "lattice order is directional");
    check(pattern({0, 0, 1, 0}).least_upper_bound(pattern({0, 0, 1, 1})) ==
              pattern({0, 0, 1, 2}),
          "least upper bound is common refinement");
    check(pattern({0, 0, 1, 0}).describes(block), "pattern describes matching block");
    check(!pattern({0, 0, 0, 0}).describes(block),
          "pattern rejects violated equality");
}

void test_paper_lattice_example() {
    using Counts =
        std::unordered_map<bsel::Pattern, std::uint64_t, bsel::PatternHash>;
    Counts counts{{pattern({0, 0, 0, 0}), 2},
                  {pattern({0, 0, 1, 0}), 3},
                  {pattern({0, 0, 1, 1}), 5}};
    const auto trained = bsel::Trainer({4, 3, 4, 1}).train_counts(counts);
    check(trained.stats.maximal_patterns == 2,
          "paper example removes non-maximal 0000");
    check(trained.stats.combined_patterns == 1,
          "paper example combines two maximal patterns");
    check(trained.patterns.size() == 1 &&
              trained.patterns[0] == pattern({0, 0, 1, 2}),
          "paper example yields 0012");
    check(trained.marginal_counts[0] == 10,
          "combined pattern count includes each atom once");
}

void test_greedy_selection_without_double_counting() {
    std::vector<bsel::Block> blocks{{1, 1, 2, 1}, {3, 3, 4, 3},
                                    {5, 5, 6, 6}, {7, 7, 8, 8}};
    const auto trained = bsel::Trainer({4, 2, 2, 1}).train(blocks);
    check(trained.patterns.size() == 2, "two incompatible rank-2 patterns selected");
    check(trained.marginal_counts.size() == 2 &&
              trained.marginal_counts[0] == 2 && trained.marginal_counts[1] == 2,
          "marginal counts do not double count");
}

void test_lazy_selection_matches_exhaustive() {
    using Counts =
        std::unordered_map<bsel::Pattern, std::uint64_t, bsel::PatternHash>;
    Counts counts{{pattern({0, 0, 1, 0}), 11},
                  {pattern({0, 0, 1, 1}), 7},
                  {pattern({0, 1, 0, 1}), 5},
                  {pattern({0, 1, 2, 0}), 3}};
    bsel::TrainingConfig lazy{4, 2, 4, 1, bsel::SelectionMode::LazyExact};
    bsel::TrainingConfig exhaustive{
        4, 2, 4, 1, bsel::SelectionMode::Exhaustive};
    const auto lazy_result = bsel::Trainer(lazy).train_counts(counts);
    const auto exhaustive_result = bsel::Trainer(exhaustive).train_counts(counts);
    check(lazy_result.patterns == exhaustive_result.patterns,
          "lazy A* selection matches exhaustive pattern order");
    check(lazy_result.marginal_counts == exhaustive_result.marginal_counts,
          "lazy A* selection matches exhaustive marginal counts");
}

void test_codec_and_quantization() {
    bsel::Model model{
        4,
        {{3, 1, {pattern({0, 0, 1, 0})}},
         {2, 1, {pattern({0, 0, 0, 0})}}}};
    const bsel::Block uniform{9, 9, 9, 9};
    const bsel::OnlineCompressor compressor(model);
    const bsel::OnlineDecompressor decompressor(model);
    const auto small = compressor.compress_block(uniform);
    check(small.compressed && small.allocated_size == 2,
          "compressor chooses smallest successful quantized target");
    check(decompressor.decompress_block(small) == uniform, "compressed round trip");

    const bsel::Block two_values{8, 8, 3, 8};
    const auto medium = bsel::compress(two_values, model);
    check(medium.compressed && medium.allocated_size == 3,
          "larger target used when smaller pattern fails");
    check(bsel::decompress(medium, model) == two_values, "second set round trip");

    const bsel::Block raw{1, 2, 3, 4};
    const auto uncompressed = bsel::compress(raw, model);
    check(!uncompressed.compressed && uncompressed.allocated_size == 4,
          "raw fallback");
    check(bsel::decompress(uncompressed, model) == raw, "raw round trip");

    const auto result = bsel::evaluate({uniform, two_values, raw}, model);
    check(result.original_bytes == 12 && result.allocated_bytes == 9,
          "quantized allocation accounting");
}

void test_model_io() {
    const std::string path = "bsel_test.model";
    const bsel::Model source{4, {{3, 1, {pattern({0, 0, 1, 0})}}}};
    bsel::save_model(source, path);
    const auto loaded = bsel::load_model(path);
    std::remove(path.c_str());
    check(loaded.block_size == 4 && loaded.sets.size() == 1 &&
              loaded.sets[0].patterns == source.sets[0].patterns,
          "model serialization round trip");
}

}  // namespace

int main() {
    test_pattern_algebra();
    test_paper_lattice_example();
    test_greedy_selection_without_double_counting();
    test_lazy_selection_matches_exhaustive();
    test_codec_and_quantization();
    test_model_io();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All Byte Select tests passed\n";
    return 0;
}
