#include "byte_select/baseline.hpp"
#include "byte_select/codec.hpp"
#include "byte_select/model_io.hpp"
#include "byte_select/paper_config.hpp"
#include "byte_select/trainer.hpp"
#include "byte_select/rtl.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <fstream>
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

template <typename Function>
void check_throws(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    check(false, message);
}

bsel::Pattern pattern(std::initializer_list<std::uint16_t> symbols) {
    return bsel::Pattern(std::vector<std::uint16_t>(symbols));
}

void store_little_endian(bsel::Block& block, std::size_t offset, std::uint64_t value,
                         std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        block[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
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

void test_lazy_selection_property() {
    for (std::uint32_t seed = 1; seed <= 16; ++seed) {
        std::uint32_t state = seed;
        std::vector<bsel::Block> blocks;
        for (std::size_t sample = 0; sample < 48; ++sample) {
            bsel::Block block;
            for (std::size_t byte = 0; byte < 5; ++byte) {
                state = state * 1664525U + 1013904223U;
                block.push_back(static_cast<std::uint8_t>((state >> 16U) % 3U));
            }
            blocks.push_back(std::move(block));
        }
        const bsel::TrainingConfig lazy{5, 3, 8, 1, bsel::SelectionMode::LazyExact};
        const bsel::TrainingConfig exhaustive{
            5, 3, 8, 1, bsel::SelectionMode::Exhaustive};
        const auto lazy_result = bsel::Trainer(lazy).train(blocks);
        const auto exhaustive_result = bsel::Trainer(exhaustive).train(blocks);
        check(lazy_result.patterns == exhaustive_result.patterns &&
                  lazy_result.marginal_counts == exhaustive_result.marginal_counts,
              "lazy selection matches exhaustive selection for deterministic pattern sets");
    }
}

void test_baseline_size_evaluators() {
    const bsel::Block zeros(64, 0);
    check(bsel::fpc_encoded_size(zeros) == 6,
          "FPC stores an all-zero 64-byte line as the 48-bit prefix header");
    check(bsel::bdi_encoded_size(zeros) == 1,
          "BDI zero encoding uses the Table-2 one-byte representation");

    bsel::Block sequential64(64, 0);
    for (std::size_t i = 0; i < 8; ++i) {
        store_little_endian(sequential64, i * 8, 0x1000U + i, 8);
    }
    check(bsel::bdi_encoded_size(sequential64) == 16,
          "BDI Base8-Delta1 uses an eight-byte base and eight one-byte deltas");

    bsel::Block small_words(64, 0);
    for (std::size_t i = 0; i < 16; ++i) {
        store_little_endian(small_words, i * 4, 0x0000007fU, 4);
    }
    check(bsel::fpc_encoded_size(small_words) == 22,
          "FPC accounts for its 48-bit header and sixteen one-byte payloads");

    bsel::Block raw(64);
    std::uint32_t state = 0x31415926U;
    for (auto& byte : raw) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>(state >> 24U);
    }
    check(bsel::fpc_encoded_size(raw) == 64 && bsel::bdi_encoded_size(raw) == 64,
          "FPC and BDI fall back to an uncompressed random cache line");

    const auto bdi = bsel::evaluate_baseline({zeros, sequential64, raw},
                                              bsel::BaselineKind::Bdi, {32, 16, 8});
    check(bdi.original_bytes == 192 && bdi.encoded_bytes == 81 &&
              bdi.allocated_bytes == 88 && bdi.compressed_blocks == 2,
          "BDI evaluation separates encoded and quantized sizes");
    check(bdi.target_sizes == std::vector<std::size_t>({8, 16, 32}) &&
              bdi.target_blocks == std::vector<std::uint64_t>({1, 1, 0}),
          "BDI evaluation attributes blocks to the smallest fitting target");
    check(bsel::baseline_encoded_size(zeros, bsel::BaselineKind::HybridBdiFpc) == 1 &&
              bsel::parse_baseline_kind("hybrid") == bsel::BaselineKind::HybridBdiFpc,
          "hybrid baseline chooses the smaller published size estimate");
}

void test_paper_configs() {
    const auto bsel256 = bsel::paper_config("bsel-256");
    check(bsel256.size() == 1 && bsel256[0].target_size == 32 &&
              bsel256[0].max_patterns == 256 && bsel256[0].metadata_bytes == 1,
          "BSel-256 matches the paper's 32-byte target and one-byte metadata");

    const auto bsel4096 = bsel::paper_config("bsel-4096");
    check(bsel4096.size() == 1 && bsel4096[0].target_size == 32 &&
              bsel4096[0].max_patterns == 4096 && bsel4096[0].metadata_bytes == 2,
          "BSel-4096 matches the paper's 32-byte target and two-byte metadata");

    const auto multi_target = bsel::paper_config("bsel-1024-1024-128");
    check(multi_target.size() == 3 &&
              multi_target[0].target_size == 32 && multi_target[0].max_patterns == 1024 &&
              multi_target[0].metadata_bytes == 2 && multi_target[0].metadata_tag_bits == 1 &&
              multi_target[0].metadata_tag_value == 0 &&
              multi_target[1].target_size == 16 && multi_target[1].max_patterns == 1024 &&
              multi_target[1].metadata_bytes == 2 && multi_target[1].metadata_tag_bits == 1 &&
              multi_target[1].metadata_tag_value == 0 &&
              multi_target[2].target_size == 8 && multi_target[2].max_patterns == 128 &&
              multi_target[2].metadata_bytes == 1 && multi_target[2].metadata_tag_bits == 1 &&
              multi_target[2].metadata_tag_value == 1,
          "BSel-1024-1024-128 preserves each target's metadata and dictionary budget");
    check_throws([] { bsel::paper_config("bsel-unknown"); },
                 "paper config rejects unknown configurations");
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
    check(small.compressed && small.allocated_size == 2 && small.metadata == 0,
          "compressor chooses smallest successful quantized target and metadata");
    check(decompressor.decompress_block(small) == uniform, "compressed round trip");

    const bsel::Block two_values{8, 8, 3, 8};
    const auto medium = bsel::compress(two_values, model);
    check(medium.compressed && medium.allocated_size == 3,
          "larger target used when smaller pattern fails");
    check(bsel::decompress(medium, model) == two_values, "second set round trip");

    const bsel::PatternSet tagged_set{
        3, 1,
        {pattern({0, 0, 0, 0}), pattern({0, 0, 0, 0}), pattern({0, 0, 0, 0}),
         pattern({0, 0, 0, 0}), pattern({0, 0, 0, 0}), pattern({0, 0, 0, 0})},
        1, 1};
    check(bsel::encode_pattern_metadata(tagged_set, 5) == 133 &&
              bsel::decode_pattern_metadata(tagged_set, 133) == 5,
          "metadata uses high target tag and low pattern index bits");
    check_throws(
        [&] { bsel::decode_pattern_metadata(tagged_set, 5); },
        "metadata decoder rejects a mismatched target tag");
    check_throws(
        [&] { bsel::encode_pattern_metadata(tagged_set, 6); },
        "metadata encoder rejects an unavailable pattern index");
    check_throws(
        [&] { bsel::decode_pattern_metadata(tagged_set, 134); },
        "metadata decoder rejects an unavailable pattern index");

    const bsel::Block raw{1, 2, 3, 4};
    const auto uncompressed = bsel::compress(raw, model);
    check(!uncompressed.compressed && uncompressed.allocated_size == 4,
          "raw fallback");
    check(bsel::decompress(uncompressed, model) == raw, "raw round trip");

    const auto result = bsel::evaluate({uniform, two_values, raw}, model);
    check(result.original_bytes == 12 && result.encoded_bytes == 9 &&
              result.allocated_bytes == 9,
          "quantized allocation accounting");
    check(result.set_blocks == std::vector<std::uint64_t>({1, 1}),
          "evaluation attributes blocks to quantized targets");

    const bsel::Model padded_model{
        4, {{3, 1, {pattern({0, 0, 0, 0})}}}};
    const auto padded = bsel::evaluate({uniform}, padded_model);
    check(padded.encoded_bytes == 2 && padded.allocated_bytes == 3 &&
              padded.unquantized_compression_ratio() == 2.0 &&
              padded.compression_ratio() > 1.3 && padded.compression_ratio() < 1.4,
          "unquantized and quantized ratios use different byte counts");
}

void test_upper_bound_evaluation() {
    const std::vector<bsel::Block> blocks{
        {9, 9, 9, 9}, {8, 8, 3, 8}, {1, 2, 3, 4}};
    const auto upper = bsel::evaluate_byte_select_upper_bound(blocks, {3, 2}, 1);
    check(upper.original_bytes == 12 && upper.unique_bytes == 7 &&
              upper.unique_with_metadata_bytes == 9 && upper.quantized_bytes == 9,
          "upper bound accounts for unique bytes, metadata, and quantization");
    check(upper.target_sizes == std::vector<std::size_t>({2, 3}) &&
              upper.target_blocks == std::vector<std::uint64_t>({1, 1}) &&
              upper.compressed_blocks == 2,
          "upper bound assigns each block to the smallest fitting target");
    check(upper.unique_compression_ratio() > 1.7 &&
              upper.unique_compression_ratio() < 1.8 &&
              upper.quantized_compression_ratio() > 1.3 &&
              upper.quantized_compression_ratio() < 1.4,
          "upper bound exposes paper-comparable ratios");
}

void test_128_byte_blocks() {
    bsel::Block uniform(128, 0x5a);
    const auto pattern_128 = bsel::Pattern::simplest(uniform);
    const bsel::Model model{128, {{64, 1, {pattern_128}}}};
    const auto encoded = bsel::compress(uniform, model);
    check(encoded.compressed && encoded.encoded_size == 2 &&
              encoded.allocated_size == 64,
          "128-byte cache lines use the same dictionary and quantization logic");
    check(bsel::decompress(encoded, model) == uniform,
          "128-byte cache-line round trip");
    const auto upper = bsel::evaluate_byte_select_upper_bound({uniform}, {64}, 1);
    check(upper.quantized_bytes == 64 && upper.target_blocks ==
                                                std::vector<std::uint64_t>({1}),
          "128-byte upper bound uses the supplied quantized target");
}

void test_phase_stats_and_validation() {
    using Counts =
        std::unordered_map<bsel::Pattern, std::uint64_t, bsel::PatternHash>;
    Counts counts{{pattern({0, 1, 2, 3}), 5},
                  {pattern({0, 0, 1, 0}), 5},
                  {pattern({0, 0, 1, 1}), 1}};
    const auto trained = bsel::Trainer({4, 2, 4, 2}).train_counts(counts);
    check(trained.stats.after_frequency_filter == 2 &&
              trained.stats.after_rank_filter == 1 &&
              trained.stats.represented_blocks == 5,
          "training reports paper phase-1 and rank filters separately");

    check_throws(
        [] { bsel::validate_model({4, {{4, 1, {pattern({0, 0, 1, 0})}}}}); },
        "model rejects a compressed target as large as the source block");
    check_throws(
        [] {
            bsel::validate_model(
                {4,
                 {{3, 1, {pattern({0, 0, 1, 0})}},
                  {3, 1, {pattern({0, 0, 0, 0})}}}});
        },
        "model rejects duplicate target sizes");
    check_throws(
        [] {
            bsel::validate_model(
                {4, {{3, 1, {pattern({0, 0, 0, 0})}, 9, 0}}});
        },
        "model rejects metadata tags wider than the metadata field");
    check_throws(
        [] {
            bsel::validate_model(
                {4, {{3, 1, {pattern({0, 0, 0, 0})}, 1, 2}}});
        },
        "model rejects metadata tag values outside their declared width");
    std::vector<bsel::Pattern> too_many_tagged_patterns(
        129, pattern({0, 0, 0, 0}));
    check_throws(
        [&] {
            bsel::validate_model(
                {4, {{3, 1, too_many_tagged_patterns, 1, 1}}});
        },
        "model reserves metadata tag bits when validating pattern-index capacity");
    check_throws([] { bsel::Pattern::parse("0 1 trailing"); },
                 "pattern parser rejects trailing data");
}

void test_model_io() {
    const std::string path = "bsel_test.model";
    const bsel::Model source{4, {{3, 1, {pattern({0, 0, 1, 0})}, 1, 1}}};
    bsel::save_model(source, path);
    const auto loaded = bsel::load_model(path);
    std::remove(path.c_str());
    check(loaded.block_size == 4 && loaded.sets.size() == 1 &&
              loaded.sets[0].patterns == source.sets[0].patterns &&
              loaded.sets[0].metadata_tag_bits == 1 &&
              loaded.sets[0].metadata_tag_value == 1,
          "version-2 model serialization preserves metadata tags");

    {
        std::ofstream legacy(path);
        legacy << "BSEL_MODEL 1\nblock_size 4\nsets 1\nset 3 1 1\n"
               << "pattern 0 0 1 0\n";
    }
    const auto legacy = bsel::load_model(path);
    std::remove(path.c_str());
    check(legacy.sets.size() == 1 && legacy.sets[0].metadata_tag_bits == 0 &&
              legacy.sets[0].metadata_tag_value == 0,
          "version-1 models retain their implicit untagged metadata layout");
}

void test_rtl_generation() {
    const bsel::Model model{4, {{3, 1, {pattern({0, 0, 1, 0})}}}};
    const auto compressor = bsel::generate_compressor_sv(model, 0);
    const auto decompressor = bsel::generate_decompressor_sv(model, 0);
    check(compressor.find("block_i[8 +: 8] == block_i[0 +: 8]") !=
              std::string::npos,
          "RTL compressor emits equality checks");
    check(compressor.find("dictionary_o[8 +: 8] = block_i[16 +: 8]") !=
              std::string::npos,
          "RTL compressor emits dictionary selectors");
    check(decompressor.find("block_o[24 +: 8] = dictionary_i[0 +: 8]") !=
              std::string::npos,
          "RTL decompressor emits byte selectors");
    const auto top_compressor = bsel::generate_top_compressor_sv(model);
    const auto top_decompressor = bsel::generate_top_decompressor_sv(model);
    check(top_compressor.find("target_size_o = 16'd3") !=
              std::string::npos,
          "RTL top compressor selects quantized target");
    check(top_compressor.find("set_id_o") == std::string::npos,
          "RTL top compressor does not emit unaccounted set metadata");
    check(top_decompressor.find("case (target_size_i)") !=
               std::string::npos,
          "RTL top decompressor selects patterns from the allocated target size");

    const bsel::Model tagged_model{
        4, {{3, 1, {pattern({0, 0, 1, 0})}, 1, 1}}};
    check(bsel::generate_compressor_sv(tagged_model, 0).find("pattern_id_o = 8'd128") !=
              std::string::npos &&
              bsel::generate_decompressor_sv(tagged_model, 0).find("8'd128: begin") !=
                  std::string::npos,
          "RTL encodes and decodes metadata target tags above the pattern index");
}

}  // namespace

int main() {
    test_pattern_algebra();
    test_paper_lattice_example();
    test_greedy_selection_without_double_counting();
    test_lazy_selection_matches_exhaustive();
    test_lazy_selection_property();
    test_baseline_size_evaluators();
    test_paper_configs();
    test_codec_and_quantization();
    test_upper_bound_evaluation();
    test_128_byte_blocks();
    test_phase_stats_and_validation();
    test_model_io();
    test_rtl_generation();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All Byte Select tests passed\n";
    return 0;
}
