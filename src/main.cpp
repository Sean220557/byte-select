#include "byte_select/baseline.hpp"
#include "byte_select/codec.hpp"
#include "byte_select/model_io.hpp"
#include "byte_select/mcc.hpp"
#include "byte_select/paper_config.hpp"
#include "byte_select/rtl.hpp"
#include "byte_select/trainer.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using bsel::Block;

struct TargetSpec {
    std::size_t size;
    std::size_t pattern_count;
    std::size_t metadata_bytes;
    std::size_t metadata_tag_bits = 0;
    std::size_t metadata_tag_value = 0;
};

struct TrainOptions {
    std::size_t block_size = 64;
    std::uint64_t threshold = 1;
    bsel::SelectionMode selection_mode = bsel::SelectionMode::LazyExact;
    std::size_t holdout_percent = 0;
    std::string paper_config;
    std::vector<TargetSpec> targets;
};

struct TraceGroup {
    std::string name;
    std::vector<std::string> paths;
};

struct EvaluationGroup {
    bsel::Evaluation aggregate;
    std::size_t trace_count = 0;
    double mean_compressed_fraction = 0.0;
    double mean_unquantized_ratio = 0.0;
    double mean_quantized_ratio = 0.0;
};

struct BaselineGroup {
    bsel::BaselineEvaluation aggregate;
    std::size_t trace_count = 0;
    double mean_compressed_fraction = 0.0;
    double mean_unquantized_ratio = 0.0;
    double mean_quantized_ratio = 0.0;
};

struct UpperBoundGroup {
    bsel::UpperBoundEvaluation aggregate;
    std::size_t trace_count = 0;
    double mean_unique_ratio = 0.0;
    double mean_unique_with_metadata_ratio = 0.0;
    double mean_quantized_ratio = 0.0;
};

std::size_t parse_size(const std::string& text, const std::string& name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value == 0 ||
        value > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("invalid " + name + ": " + text);
    }
    return static_cast<std::size_t>(value);
}

TargetSpec parse_target(const std::string& text) {
    const auto first = text.find(':');
    const auto second = first == std::string::npos ? first : text.find(':', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        throw std::runtime_error("target must be SIZE:PATTERNS:METADATA_BYTES");
    }
    return {parse_size(text.substr(0, first), "target size"),
            parse_size(text.substr(first + 1, second - first - 1), "pattern count"),
            parse_size(text.substr(second + 1), "metadata bytes")};
}

bool pattern_count_fits(std::size_t count, std::size_t metadata_bytes,
                        std::size_t metadata_tag_bits) {
    if (metadata_bytes > std::numeric_limits<std::size_t>::max() / 8U) {
        return true;
    }
    const auto metadata_bits = metadata_bytes * 8U;
    if (metadata_tag_bits > metadata_bits) {
        return false;
    }
    const auto pattern_id_bits = metadata_bits - metadata_tag_bits;
    if (pattern_id_bits >= std::numeric_limits<std::size_t>::digits) {
        return true;
    }
    return count <= (std::size_t{1} << pattern_id_bits);
}

std::vector<std::uint8_t> read_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open input: " + path);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void write_bytes(const std::string& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot open output: " + path);
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

std::vector<Block> split_blocks(const std::vector<std::uint8_t>& bytes,
                                std::size_t block_size, bool require_aligned) {
    if (require_aligned && bytes.size() % block_size != 0) {
        throw std::runtime_error("input size is not a multiple of block size");
    }
    std::vector<Block> blocks;
    for (std::size_t offset = 0; offset + block_size <= bytes.size();
         offset += block_size) {
        blocks.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset + block_size));
    }
    return blocks;
}

std::size_t full_block_count(const std::string& path, std::size_t block_size) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot stat input: " + path);
    }
    const std::streamoff bytes = input.tellg();
    if (bytes < 0 ||
        static_cast<unsigned long long>(bytes) > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("input is too large for this host: " + path);
    }
    return static_cast<std::size_t>(bytes) / block_size;
}

std::size_t parse_nonnegative_size(const std::string& text, const std::string& name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed);
    if (consumed != text.size() || value > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("invalid " + name + ": " + text);
    }
    return static_cast<std::size_t>(value);
}

template <typename Function>
void visit_full_blocks(const std::string& path, std::size_t block_size, Function&& function) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open input: " + path);
    }
    Block block(block_size);
    std::size_t index = 0;
    while (input.read(reinterpret_cast<char*>(block.data()),
                      static_cast<std::streamsize>(block.size()))) {
        function(block, index++);
    }
    if (!input.eof()) {
        throw std::runtime_error("failed while reading input: " + path);
    }
}

void accumulate_evaluation(bsel::Evaluation& result, const bsel::EncodedBlock& encoded) {
    ++result.blocks;
    result.original_bytes += encoded.original_size;
    result.encoded_bytes += encoded.encoded_size;
    result.allocated_bytes += encoded.allocated_size;
    result.compressed_blocks += encoded.compressed ? 1U : 0U;
    if (encoded.compressed) {
        ++result.set_blocks[encoded.set_index];
    }
}

void merge_evaluation(bsel::Evaluation& result, const bsel::Evaluation& other) {
    if (result.set_blocks.size() != other.set_blocks.size()) {
        throw std::invalid_argument("cannot merge evaluations for different models");
    }
    result.blocks += other.blocks;
    result.original_bytes += other.original_bytes;
    result.encoded_bytes += other.encoded_bytes;
    result.allocated_bytes += other.allocated_bytes;
    result.compressed_blocks += other.compressed_blocks;
    for (std::size_t i = 0; i < result.set_blocks.size(); ++i) {
        result.set_blocks[i] += other.set_blocks[i];
    }
}

void merge_upper_bound(bsel::UpperBoundEvaluation& result,
                       const bsel::UpperBoundEvaluation& other) {
    if (result.metadata_bytes != other.metadata_bytes ||
        result.target_sizes != other.target_sizes ||
        result.target_blocks.size() != other.target_blocks.size()) {
        throw std::invalid_argument("cannot merge upper bounds with different settings");
    }
    result.blocks += other.blocks;
    result.original_bytes += other.original_bytes;
    result.unique_bytes += other.unique_bytes;
    result.unique_with_metadata_bytes += other.unique_with_metadata_bytes;
    result.quantized_bytes += other.quantized_bytes;
    result.compressed_blocks += other.compressed_blocks;
    for (std::size_t i = 0; i < result.target_blocks.size(); ++i) {
        result.target_blocks[i] += other.target_blocks[i];
    }
}

void merge_baseline_evaluation(bsel::BaselineEvaluation& result,
                               const bsel::BaselineEvaluation& other) {
    if (result.kind != other.kind || result.target_sizes != other.target_sizes ||
        result.target_blocks.size() != other.target_blocks.size()) {
        throw std::invalid_argument("cannot merge baseline evaluations with different settings");
    }
    result.blocks += other.blocks;
    result.original_bytes += other.original_bytes;
    result.encoded_bytes += other.encoded_bytes;
    result.allocated_bytes += other.allocated_bytes;
    result.compressed_blocks += other.compressed_blocks;
    for (std::size_t i = 0; i < result.target_blocks.size(); ++i) {
        result.target_blocks[i] += other.target_blocks[i];
    }
}

std::vector<std::size_t> target_sizes_for(const bsel::Model& model) {
    std::vector<std::size_t> targets;
    targets.reserve(model.sets.size());
    for (const auto& set : model.sets) {
        targets.push_back(set.target_size);
    }
    return targets;
}

std::size_t set_index_for_target_size(const bsel::Model& model, std::size_t target_size) {
    for (std::size_t i = 0; i < model.sets.size(); ++i) {
        if (model.sets[i].target_size == target_size) {
            return i;
        }
    }
    throw std::runtime_error("compressed target size is not present in model");
}

std::vector<bsel::BaselineKind> parse_baseline_options(int argc, char** argv,
                                                       int first_option) {
    std::vector<bsel::BaselineKind> baselines;
    for (int i = first_option; i < argc; ++i) {
        const std::string option = argv[i];
        if (option != "--baseline" || i + 1 >= argc) {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
        const auto kind = bsel::parse_baseline_kind(argv[++i]);
        if (std::find(baselines.begin(), baselines.end(), kind) == baselines.end()) {
            baselines.push_back(kind);
        }
    }
    if (baselines.empty()) {
        baselines = {bsel::BaselineKind::Fpc, bsel::BaselineKind::Bdi};
    }
    return baselines;
}

std::vector<TraceGroup> parse_trace_groups(int argc, char** argv, int first_argument,
                                           int* first_option) {
    std::vector<TraceGroup> groups;
    int index = first_argument;
    while (index < argc && std::string(argv[index]) == "--group") {
        if (++index >= argc || std::string(argv[index]).rfind("--", 0) == 0) {
            throw std::runtime_error("--group requires a name");
        }
        TraceGroup group;
        group.name = argv[index++];
        while (index < argc && std::string(argv[index]).rfind("--", 0) != 0) {
            group.paths.emplace_back(argv[index++]);
        }
        if (group.paths.empty()) {
            throw std::runtime_error("--group requires at least one input trace");
        }
        if (std::any_of(groups.begin(), groups.end(), [&](const TraceGroup& existing) {
                return existing.name == group.name;
            })) {
            throw std::runtime_error("group names must be unique: " + group.name);
        }
        groups.push_back(std::move(group));
    }
    if (groups.empty()) {
        throw std::runtime_error("at least one --group NAME INPUT [INPUT ...] is required");
    }
    *first_option = index;
    return groups;
}

template <typename T>
void write_integer(std::ostream& output, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        output.put(static_cast<char>((value >> (i * 8U)) & 0xffU));
    }
}

template <typename T>
T read_integer(std::istream& input) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        const int byte = input.get();
        if (byte == EOF) {
            throw std::runtime_error("truncated compressed file");
        }
        value |= static_cast<T>(static_cast<unsigned int>(byte)) << (i * 8U);
    }
    return value;
}

void print_evaluation(const bsel::Evaluation& result, const bsel::Model& model);
void print_upper_bound(const bsel::UpperBoundEvaluation& upper);

TrainOptions parse_train_options(int argc, char** argv, int first_option) {
    TrainOptions options;
    for (int i = first_option; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--block-size" && i + 1 < argc) {
            options.block_size = parse_size(argv[++i], "block size");
        } else if (option == "--threshold" && i + 1 < argc) {
            options.threshold = parse_size(argv[++i], "threshold");
        } else if (option == "--target" && i + 1 < argc) {
            if (!options.paper_config.empty()) {
                throw std::runtime_error("--target cannot be combined with --paper-config");
            }
            options.targets.push_back(parse_target(argv[++i]));
        } else if (option == "--paper-config" && i + 1 < argc) {
            if (!options.targets.empty() || !options.paper_config.empty()) {
                throw std::runtime_error(
                    "--paper-config cannot be combined with --target or repeated");
            }
            options.paper_config = argv[++i];
            for (const auto& target : bsel::paper_config(options.paper_config)) {
                options.targets.push_back({target.target_size, target.max_patterns,
                                           target.metadata_bytes, target.metadata_tag_bits,
                                           target.metadata_tag_value});
            }
        } else if (option == "--selection" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode == "lazy") {
                options.selection_mode = bsel::SelectionMode::LazyExact;
            } else if (mode == "exhaustive") {
                options.selection_mode = bsel::SelectionMode::Exhaustive;
            } else {
                throw std::runtime_error("selection must be lazy or exhaustive");
            }
        } else if (option == "--holdout-middle" && i + 1 < argc) {
            options.holdout_percent = parse_size(argv[++i], "holdout percentage");
            if (options.holdout_percent >= 100) {
                throw std::runtime_error("holdout percentage must be below 100");
            }
        } else {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
    }
    if (options.targets.empty()) {
        options.targets.push_back({32, 256, 1});
    }
    return options;
}

void train_from_traces(const std::vector<std::string>& paths, const std::string& model_path,
                       const TrainOptions& options) {
    if (paths.empty()) {
        throw std::runtime_error("training requires at least one input trace");
    }

    struct TraceSplit {
        std::string path;
        std::size_t blocks;
        std::size_t holdout_begin;
        std::size_t holdout_end;
    };
    std::vector<TraceSplit> traces;
    traces.reserve(paths.size());
    std::size_t training_block_count = 0;
    std::size_t holdout_block_count = 0;

    for (const auto& path : paths) {
        const auto blocks = full_block_count(path, options.block_size);
        if (blocks == 0) {
            throw std::runtime_error("training input contains no full blocks: " + path);
        }
        const auto holdout_count = blocks * options.holdout_percent / 100;
        const auto holdout_begin = (blocks - holdout_count) / 2;
        const auto holdout_end = holdout_begin + holdout_count;
        traces.push_back({path, blocks, holdout_begin, holdout_end});
        training_block_count += blocks - holdout_count;
        holdout_block_count += holdout_count;
    }
    if (training_block_count == 0) {
        throw std::runtime_error("holdout split left no training blocks");
    }

    std::unordered_map<bsel::Pattern, std::uint64_t, bsel::PatternHash> counts;
    for (const auto& trace : traces) {
        visit_full_blocks(trace.path, options.block_size,
                          [&](const Block& block, std::size_t index) {
                              if (index < trace.holdout_begin ||
                                  index >= trace.holdout_end) {
                                  ++counts[bsel::Pattern::simplest(block)];
                              }
                          });
    }

    bsel::Model model;
    model.block_size = options.block_size;
    if (!options.paper_config.empty()) {
        std::cout << "paper_config=" << options.paper_config
                  << " targets=" << options.targets.size() << '\n';
    }
    for (const auto& target : options.targets) {
        if (target.metadata_bytes >= target.size) {
            throw std::runtime_error("metadata must be smaller than target");
        }
        if (target.size >= options.block_size) {
            throw std::runtime_error("compressed target must be smaller than block size");
        }
        if (!pattern_count_fits(target.pattern_count, target.metadata_bytes,
                                target.metadata_tag_bits)) {
            throw std::runtime_error("requested pattern count exceeds metadata capacity");
        }
        bsel::TrainingConfig config{options.block_size, target.size - target.metadata_bytes,
                                    target.pattern_count, options.threshold,
                                    options.selection_mode};
        auto trained = bsel::Trainer(config).train_counts(counts);
        trained.stats.blocks_seen = training_block_count;
        model.sets.push_back({target.size, target.metadata_bytes, trained.patterns,
                              target.metadata_tag_bits, target.metadata_tag_value});
        std::cout << "target=" << target.size
                  << " dictionary=" << config.dictionary_size
                  << " metadata_bits=" << target.metadata_bytes * 8U
                  << " pattern_id_bits="
                  << target.metadata_bytes * 8U - target.metadata_tag_bits
                  << " metadata_tag=" << target.metadata_tag_value
                  << " distinct=" << trained.stats.distinct_patterns
                  << " frequency_filtered=" << trained.stats.after_frequency_filter
                  << " rank_filtered=" << trained.stats.after_rank_filter
                  << " maximal=" << trained.stats.maximal_patterns
                  << " combined=" << trained.stats.combined_patterns
                  << " selected=" << trained.patterns.size()
                  << " represented_blocks=" << trained.stats.represented_blocks
                  << " coverage_evaluations="
                  << trained.stats.coverage_evaluations
                  << " recomputations_skipped="
                  << trained.stats.coverage_recomputations_skipped << '\n';
    }
    bsel::save_model(model, model_path);
    std::cout << "training_traces=" << paths.size()
              << " holdout_middle_percent=" << options.holdout_percent
              << " training_blocks=" << training_block_count
              << " holdout_blocks=" << holdout_block_count << '\n';
    for (const auto& trace : traces) {
        std::cout << "trace path=" << trace.path << " blocks=" << trace.blocks
                  << " training_blocks="
                  << trace.blocks - (trace.holdout_end - trace.holdout_begin)
                  << " holdout_blocks=" << trace.holdout_end - trace.holdout_begin << '\n';
    }
    if (holdout_block_count != 0) {
        bsel::Evaluation holdout;
        holdout.set_blocks.resize(model.sets.size(), 0);
        for (const auto& trace : traces) {
            visit_full_blocks(trace.path, options.block_size,
                              [&](const Block& block, std::size_t index) {
                                  if (index >= trace.holdout_begin &&
                                      index < trace.holdout_end) {
                                      accumulate_evaluation(holdout, bsel::compress(block, model));
                                  }
                              });
        }
        print_evaluation(holdout, model);
    }
}

void command_train(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("usage: bsel train INPUT MODEL [options]");
    }
    train_from_traces({argv[2]}, argv[3], parse_train_options(argc, argv, 4));
}

void command_train_list(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("usage: bsel train-list MODEL INPUT [INPUT ...] [options]");
    }
    std::vector<std::string> paths;
    int first_option = 3;
    while (first_option < argc && std::string(argv[first_option]).rfind("--", 0) != 0) {
        paths.emplace_back(argv[first_option]);
        ++first_option;
    }
    if (paths.empty()) {
        throw std::runtime_error("train-list requires at least one input trace");
    }
    train_from_traces(paths, argv[2], parse_train_options(argc, argv, first_option));
}

void print_evaluation(const bsel::Evaluation& result, const bsel::Model& model) {
    std::cout << "blocks=" << result.blocks
              << " compressed_blocks=" << result.compressed_blocks
              << " compressed_fraction=" << std::fixed << std::setprecision(6)
              << (result.blocks == 0 ? 0.0
                                     : static_cast<double>(result.compressed_blocks) /
                                           static_cast<double>(result.blocks))
              << " original_bytes=" << result.original_bytes
              << " encoded_bytes=" << result.encoded_bytes
              << " unquantized_ratio=" << result.unquantized_compression_ratio()
              << " allocated_bytes=" << result.allocated_bytes
              << " quantized_ratio=" << result.compression_ratio() << '\n';
    for (std::size_t i = 0; i < result.set_blocks.size(); ++i) {
        std::cout << "actual_target index=" << i
                  << " size=" << model.sets[i].target_size
                  << " blocks=" << result.set_blocks[i]
                  << " fraction="
                  << (result.blocks == 0
                          ? 0.0
                          : static_cast<double>(result.set_blocks[i]) /
                                static_cast<double>(result.blocks))
                  << '\n';
    }
}

EvaluationGroup evaluate_trace_group(const TraceGroup& group, const bsel::Model& model) {
    EvaluationGroup result;
    result.aggregate.set_blocks.resize(model.sets.size(), 0);
    for (const auto& path : group.paths) {
        const auto blocks = split_blocks(read_bytes(path), model.block_size, false);
        if (blocks.empty()) {
            throw std::runtime_error("evaluation input contains no full blocks: " + path);
        }
        const auto evaluation = bsel::evaluate(blocks, model);
        merge_evaluation(result.aggregate, evaluation);
        result.mean_compressed_fraction +=
            static_cast<double>(evaluation.compressed_blocks) / evaluation.blocks;
        result.mean_unquantized_ratio += evaluation.unquantized_compression_ratio();
        result.mean_quantized_ratio += evaluation.compression_ratio();
        ++result.trace_count;
    }
    result.mean_compressed_fraction /= result.trace_count;
    result.mean_unquantized_ratio /= result.trace_count;
    result.mean_quantized_ratio /= result.trace_count;
    return result;
}

void print_evaluation_group_mean(const std::string& label, const EvaluationGroup& group) {
    std::cout << label << " sample_count=" << group.trace_count
              << " compressed_fraction=" << group.mean_compressed_fraction
              << " unquantized_ratio=" << group.mean_unquantized_ratio
              << " quantized_ratio=" << group.mean_quantized_ratio << '\n';
}

UpperBoundGroup analyze_trace_group(const TraceGroup& group, const bsel::Model& model,
                                    std::size_t metadata_bytes) {
    UpperBoundGroup result;
    for (const auto& path : group.paths) {
        const auto blocks = split_blocks(read_bytes(path), model.block_size, false);
        if (blocks.empty()) {
            throw std::runtime_error("analysis input contains no full blocks: " + path);
        }
        const auto upper = bsel::evaluate_byte_select_upper_bound(
            blocks, target_sizes_for(model), metadata_bytes);
        if (result.trace_count == 0) {
            result.aggregate = upper;
        } else {
            merge_upper_bound(result.aggregate, upper);
        }
        result.mean_unique_ratio += upper.unique_compression_ratio();
        result.mean_unique_with_metadata_ratio +=
            upper.unique_with_metadata_compression_ratio();
        result.mean_quantized_ratio += upper.quantized_compression_ratio();
        ++result.trace_count;
    }
    result.mean_unique_ratio /= result.trace_count;
    result.mean_unique_with_metadata_ratio /= result.trace_count;
    result.mean_quantized_ratio /= result.trace_count;
    return result;
}

void print_upper_bound_group_mean(const std::string& label, const UpperBoundGroup& group) {
    std::cout << label << " sample_count=" << group.trace_count
              << " unique_ratio=" << group.mean_unique_ratio
              << " unique_with_metadata_ratio="
              << group.mean_unique_with_metadata_ratio
              << " quantized_ratio=" << group.mean_quantized_ratio << '\n';
}

void command_evaluate_groups(int argc, char** argv) {
    if (argc < 6) {
        throw std::runtime_error(
            "usage: bsel evaluate-groups MODEL --group NAME INPUT [INPUT ...] [--group ...]");
    }
    int first_option = 0;
    const auto groups = parse_trace_groups(argc, argv, 3, &first_option);
    if (first_option != argc) {
        throw std::runtime_error("unknown option: " + std::string(argv[first_option]));
    }
    const auto model = bsel::load_model(argv[2]);
    EvaluationGroup mean_of_groups;
    for (const auto& group : groups) {
        const auto result = evaluate_trace_group(group, model);
        std::cout << "benchmark_group name=" << group.name
                  << " trace_count=" << result.trace_count << '\n';
        print_evaluation_group_mean("byte_select_group_mean", result);
        std::cout << "group_block_weighted" << '\n';
        print_evaluation(result.aggregate, model);
        mean_of_groups.mean_compressed_fraction += result.mean_compressed_fraction;
        mean_of_groups.mean_unquantized_ratio += result.mean_unquantized_ratio;
        mean_of_groups.mean_quantized_ratio += result.mean_quantized_ratio;
        ++mean_of_groups.trace_count;
    }
    mean_of_groups.mean_compressed_fraction /= mean_of_groups.trace_count;
    mean_of_groups.mean_unquantized_ratio /= mean_of_groups.trace_count;
    mean_of_groups.mean_quantized_ratio /= mean_of_groups.trace_count;
    std::cout << "mean_of_group_means group_count=" << mean_of_groups.trace_count << '\n';
    print_evaluation_group_mean("byte_select_mean_of_group_means", mean_of_groups);
}

void command_analyze_groups(int argc, char** argv) {
    if (argc < 6) {
        throw std::runtime_error(
            "usage: bsel analyze-groups MODEL --group NAME INPUT [INPUT ...] [--group ...] "
            "[--ideal-metadata-bytes N]");
    }
    int first_option = 0;
    const auto groups = parse_trace_groups(argc, argv, 3, &first_option);
    std::size_t metadata_bytes = 1;
    for (int i = first_option; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--ideal-metadata-bytes" && i + 1 < argc) {
            metadata_bytes = parse_size(argv[++i], "ideal metadata bytes");
        } else {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
    }

    const auto model = bsel::load_model(argv[2]);
    EvaluationGroup actual_mean_of_groups;
    UpperBoundGroup upper_mean_of_groups;
    for (const auto& group : groups) {
        const auto actual = evaluate_trace_group(group, model);
        const auto upper = analyze_trace_group(group, model, metadata_bytes);
        std::cout << "benchmark_group name=" << group.name
                  << " trace_count=" << actual.trace_count << '\n';
        print_evaluation_group_mean("byte_select_group_mean", actual);
        print_upper_bound_group_mean("upper_bound_group_mean", upper);
        std::cout << "group_block_weighted" << '\n';
        print_evaluation(actual.aggregate, model);
        print_upper_bound(upper.aggregate);

        actual_mean_of_groups.mean_compressed_fraction += actual.mean_compressed_fraction;
        actual_mean_of_groups.mean_unquantized_ratio += actual.mean_unquantized_ratio;
        actual_mean_of_groups.mean_quantized_ratio += actual.mean_quantized_ratio;
        ++actual_mean_of_groups.trace_count;
        upper_mean_of_groups.mean_unique_ratio += upper.mean_unique_ratio;
        upper_mean_of_groups.mean_unique_with_metadata_ratio +=
            upper.mean_unique_with_metadata_ratio;
        upper_mean_of_groups.mean_quantized_ratio += upper.mean_quantized_ratio;
        ++upper_mean_of_groups.trace_count;
    }

    actual_mean_of_groups.mean_compressed_fraction /= actual_mean_of_groups.trace_count;
    actual_mean_of_groups.mean_unquantized_ratio /= actual_mean_of_groups.trace_count;
    actual_mean_of_groups.mean_quantized_ratio /= actual_mean_of_groups.trace_count;
    upper_mean_of_groups.mean_unique_ratio /= upper_mean_of_groups.trace_count;
    upper_mean_of_groups.mean_unique_with_metadata_ratio /= upper_mean_of_groups.trace_count;
    upper_mean_of_groups.mean_quantized_ratio /= upper_mean_of_groups.trace_count;
    std::cout << "mean_of_group_means group_count=" << actual_mean_of_groups.trace_count
              << '\n';
    print_evaluation_group_mean("byte_select_mean_of_group_means", actual_mean_of_groups);
    print_upper_bound_group_mean("upper_bound_mean_of_group_means",
                                 upper_mean_of_groups);
    std::cout << "actual_fraction_of_quantized_upper="
              << (upper_mean_of_groups.mean_quantized_ratio == 0.0
                      ? 0.0
                      : actual_mean_of_groups.mean_quantized_ratio /
                            upper_mean_of_groups.mean_quantized_ratio)
              << '\n';
}

void command_evaluate(int argc, char** argv) {
    if (argc != 4) {
        throw std::runtime_error("usage: bsel evaluate MODEL INPUT");
    }
    const auto model = bsel::load_model(argv[2]);
    bsel::validate_model(model);
    const auto blocks = split_blocks(read_bytes(argv[3]), model.block_size, false);
    print_evaluation(bsel::evaluate(blocks, model), model);
}

void command_evaluate_list(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("usage: bsel evaluate-list MODEL INPUT [INPUT ...]");
    }
    const auto model = bsel::load_model(argv[2]);
    bsel::Evaluation aggregate;
    aggregate.set_blocks.resize(model.sets.size(), 0);
    double mean_unquantized_ratio = 0.0;
    double mean_quantized_ratio = 0.0;
    std::size_t nonempty_traces = 0;

    for (int i = 3; i < argc; ++i) {
        const std::string path = argv[i];
        const auto blocks = split_blocks(read_bytes(path), model.block_size, false);
        if (blocks.empty()) {
            throw std::runtime_error("evaluation input contains no full blocks: " + path);
        }
        const auto result = bsel::evaluate(blocks, model);
        std::cout << "trace path=" << path << '\n';
        print_evaluation(result, model);
        merge_evaluation(aggregate, result);
        mean_unquantized_ratio += result.unquantized_compression_ratio();
        mean_quantized_ratio += result.compression_ratio();
        ++nonempty_traces;
    }

    std::cout << "trace_mean count=" << nonempty_traces
              << " unquantized_ratio=" << mean_unquantized_ratio / nonempty_traces
              << " quantized_ratio=" << mean_quantized_ratio / nonempty_traces << '\n';
    std::cout << "trace_weighted" << '\n';
    print_evaluation(aggregate, model);
}

void print_upper_bound(const bsel::UpperBoundEvaluation& upper) {
    std::cout << "upper_bound blocks=" << upper.blocks
              << " ideal_metadata_bytes=" << upper.metadata_bytes
              << " unique_bytes=" << upper.unique_bytes
              << " unique_ratio=" << upper.unique_compression_ratio()
              << " unique_with_metadata_bytes=" << upper.unique_with_metadata_bytes
              << " unique_with_metadata_ratio="
              << upper.unique_with_metadata_compression_ratio()
              << " quantized_bytes=" << upper.quantized_bytes
              << " quantized_ratio=" << upper.quantized_compression_ratio()
              << " compressed_blocks=" << upper.compressed_blocks << '\n';
    for (std::size_t i = 0; i < upper.target_sizes.size(); ++i) {
        std::cout << "upper_bound_target size=" << upper.target_sizes[i]
                  << " blocks=" << upper.target_blocks[i]
                  << " fraction="
                  << (upper.blocks == 0
                          ? 0.0
                          : static_cast<double>(upper.target_blocks[i]) /
                                static_cast<double>(upper.blocks))
                  << '\n';
    }
}

void print_baseline_evaluation(const bsel::BaselineEvaluation& result) {
    std::cout << "baseline algorithm=" << bsel::baseline_kind_name(result.kind)
              << " blocks=" << result.blocks
              << " compressed_blocks=" << result.compressed_blocks
              << " compressed_fraction="
              << (result.blocks == 0
                      ? 0.0
                      : static_cast<double>(result.compressed_blocks) /
                            static_cast<double>(result.blocks))
              << " original_bytes=" << result.original_bytes
              << " encoded_bytes=" << result.encoded_bytes
              << " unquantized_ratio=" << result.unquantized_compression_ratio()
              << " allocated_bytes=" << result.allocated_bytes
              << " quantized_ratio=" << result.quantized_compression_ratio() << '\n';
    for (std::size_t i = 0; i < result.target_sizes.size(); ++i) {
        std::cout << "baseline_target algorithm=" << bsel::baseline_kind_name(result.kind)
                  << " size=" << result.target_sizes[i]
                  << " blocks=" << result.target_blocks[i]
                  << " fraction="
                  << (result.blocks == 0
                          ? 0.0
                          : static_cast<double>(result.target_blocks[i]) /
                                static_cast<double>(result.blocks))
                  << '\n';
    }
}

BaselineGroup evaluate_baseline_trace_group(const TraceGroup& group, bsel::BaselineKind kind,
                                            const std::vector<std::size_t>& target_sizes,
                                            std::size_t block_size) {
    BaselineGroup result;
    for (const auto& path : group.paths) {
        const auto blocks = split_blocks(read_bytes(path), block_size, false);
        if (blocks.empty()) {
            throw std::runtime_error("comparison input contains no full blocks: " + path);
        }
        const auto evaluation = bsel::evaluate_baseline(blocks, kind, target_sizes);
        if (result.trace_count == 0) {
            result.aggregate = evaluation;
        } else {
            merge_baseline_evaluation(result.aggregate, evaluation);
        }
        result.mean_compressed_fraction +=
            static_cast<double>(evaluation.compressed_blocks) / evaluation.blocks;
        result.mean_unquantized_ratio += evaluation.unquantized_compression_ratio();
        result.mean_quantized_ratio += evaluation.quantized_compression_ratio();
        ++result.trace_count;
    }
    result.mean_compressed_fraction /= result.trace_count;
    result.mean_unquantized_ratio /= result.trace_count;
    result.mean_quantized_ratio /= result.trace_count;
    return result;
}

void print_baseline_group_mean(const std::string& label, bsel::BaselineKind kind,
                               const BaselineGroup& group) {
    std::cout << label << " algorithm=" << bsel::baseline_kind_name(kind)
              << " sample_count=" << group.trace_count
              << " compressed_fraction=" << group.mean_compressed_fraction
              << " unquantized_ratio=" << group.mean_unquantized_ratio
              << " quantized_ratio=" << group.mean_quantized_ratio << '\n';
}

void command_analyze(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error(
            "usage: bsel analyze MODEL INPUT [--ideal-metadata-bytes N]");
    }
    std::size_t metadata_bytes = 1;
    for (int i = 4; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--ideal-metadata-bytes" && i + 1 < argc) {
            metadata_bytes = parse_size(argv[++i], "ideal metadata bytes");
        } else {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
    }

    const auto model = bsel::load_model(argv[2]);
    const auto blocks = split_blocks(read_bytes(argv[3]), model.block_size, false);
    print_evaluation(bsel::evaluate(blocks, model), model);

    const auto upper =
        bsel::evaluate_byte_select_upper_bound(blocks, target_sizes_for(model), metadata_bytes);
    print_upper_bound(upper);
}

void command_analyze_list(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error(
            "usage: bsel analyze-list MODEL INPUT [INPUT ...] [--ideal-metadata-bytes N]");
    }
    std::vector<std::string> paths;
    int first_option = 3;
    while (first_option < argc && std::string(argv[first_option]).rfind("--", 0) != 0) {
        paths.emplace_back(argv[first_option]);
        ++first_option;
    }
    if (paths.empty()) {
        throw std::runtime_error("analyze-list requires at least one input trace");
    }
    std::size_t metadata_bytes = 1;
    for (int i = first_option; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--ideal-metadata-bytes" && i + 1 < argc) {
            metadata_bytes = parse_size(argv[++i], "ideal metadata bytes");
        } else {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
    }

    const auto model = bsel::load_model(argv[2]);
    bsel::Evaluation aggregate;
    aggregate.set_blocks.resize(model.sets.size(), 0);
    bsel::UpperBoundEvaluation aggregate_upper;
    bool has_upper = false;
    double mean_unquantized_ratio = 0.0;
    double mean_quantized_ratio = 0.0;
    double mean_upper_unique_ratio = 0.0;
    double mean_upper_quantized_ratio = 0.0;

    for (const auto& path : paths) {
        const auto blocks = split_blocks(read_bytes(path), model.block_size, false);
        if (blocks.empty()) {
            throw std::runtime_error("analysis input contains no full blocks: " + path);
        }
        const auto actual = bsel::evaluate(blocks, model);
        const auto upper = bsel::evaluate_byte_select_upper_bound(
            blocks, target_sizes_for(model), metadata_bytes);
        std::cout << "trace path=" << path << '\n';
        print_evaluation(actual, model);
        print_upper_bound(upper);
        merge_evaluation(aggregate, actual);
        if (has_upper) {
            merge_upper_bound(aggregate_upper, upper);
        } else {
            aggregate_upper = upper;
            has_upper = true;
        }
        mean_unquantized_ratio += actual.unquantized_compression_ratio();
        mean_quantized_ratio += actual.compression_ratio();
        mean_upper_unique_ratio += upper.unique_compression_ratio();
        mean_upper_quantized_ratio += upper.quantized_compression_ratio();
    }

    const auto count = static_cast<double>(paths.size());
    std::cout << "trace_mean count=" << paths.size()
              << " unquantized_ratio=" << mean_unquantized_ratio / count
              << " quantized_ratio=" << mean_quantized_ratio / count << '\n';
    std::cout << "upper_bound_mean count=" << paths.size()
              << " unique_ratio=" << mean_upper_unique_ratio / count
              << " quantized_ratio=" << mean_upper_quantized_ratio / count
              << " actual_fraction_of_quantized_upper="
              << (mean_upper_quantized_ratio == 0.0
                      ? 0.0
                      : (mean_quantized_ratio / count) /
                            (mean_upper_quantized_ratio / count))
              << '\n';
    std::cout << "trace_weighted" << '\n';
    print_evaluation(aggregate, model);
    print_upper_bound(aggregate_upper);
}

void command_compare(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error(
            "usage: bsel compare MODEL INPUT "
            "[--baseline fpc|bdi|hybrid|cpack|bpc|huffman ...]");
    }
    const auto baselines = parse_baseline_options(argc, argv, 4);

    const auto model = bsel::load_model(argv[2]);
    const auto blocks = split_blocks(read_bytes(argv[3]), model.block_size, false);
    if (blocks.empty()) {
        throw std::runtime_error("comparison input contains no full blocks");
    }
    std::cout << "byte_select" << '\n';
    print_evaluation(bsel::evaluate(blocks, model), model);
    for (const auto kind : baselines) {
        print_baseline_evaluation(
            bsel::evaluate_baseline(blocks, kind, target_sizes_for(model)));
    }
}

void command_compare_groups(int argc, char** argv) {
    if (argc < 6) {
        throw std::runtime_error(
            "usage: bsel compare-groups MODEL --group NAME INPUT [INPUT ...] [--group ...] "
            "[--baseline fpc|bdi|hybrid|cpack|bpc|huffman ...]");
    }
    int first_option = 0;
    const auto groups = parse_trace_groups(argc, argv, 3, &first_option);
    const auto baselines = parse_baseline_options(argc, argv, first_option);
    const auto model = bsel::load_model(argv[2]);
    const auto target_sizes = target_sizes_for(model);
    EvaluationGroup byte_select_mean_of_groups;
    std::vector<BaselineGroup> baseline_means_of_groups(baselines.size());

    for (const auto& group : groups) {
        const auto byte_select = evaluate_trace_group(group, model);
        std::cout << "benchmark_group name=" << group.name
                  << " trace_count=" << byte_select.trace_count << '\n';
        print_evaluation_group_mean("byte_select_group_mean", byte_select);
        std::cout << "byte_select_group_block_weighted" << '\n';
        print_evaluation(byte_select.aggregate, model);
        byte_select_mean_of_groups.mean_compressed_fraction +=
            byte_select.mean_compressed_fraction;
        byte_select_mean_of_groups.mean_unquantized_ratio +=
            byte_select.mean_unquantized_ratio;
        byte_select_mean_of_groups.mean_quantized_ratio +=
            byte_select.mean_quantized_ratio;
        ++byte_select_mean_of_groups.trace_count;

        for (std::size_t i = 0; i < baselines.size(); ++i) {
            const auto baseline = evaluate_baseline_trace_group(
                group, baselines[i], target_sizes, model.block_size);
            print_baseline_group_mean("baseline_group_mean", baselines[i], baseline);
            std::cout << "baseline_group_block_weighted" << '\n';
            print_baseline_evaluation(baseline.aggregate);
            baseline_means_of_groups[i].mean_compressed_fraction +=
                baseline.mean_compressed_fraction;
            baseline_means_of_groups[i].mean_unquantized_ratio +=
                baseline.mean_unquantized_ratio;
            baseline_means_of_groups[i].mean_quantized_ratio +=
                baseline.mean_quantized_ratio;
            ++baseline_means_of_groups[i].trace_count;
        }
    }

    byte_select_mean_of_groups.mean_compressed_fraction /=
        byte_select_mean_of_groups.trace_count;
    byte_select_mean_of_groups.mean_unquantized_ratio /=
        byte_select_mean_of_groups.trace_count;
    byte_select_mean_of_groups.mean_quantized_ratio /=
        byte_select_mean_of_groups.trace_count;
    std::cout << "mean_of_group_means group_count="
              << byte_select_mean_of_groups.trace_count << '\n';
    print_evaluation_group_mean("byte_select_mean_of_group_means",
                                byte_select_mean_of_groups);
    for (std::size_t i = 0; i < baselines.size(); ++i) {
        baseline_means_of_groups[i].mean_compressed_fraction /=
            baseline_means_of_groups[i].trace_count;
        baseline_means_of_groups[i].mean_unquantized_ratio /=
            baseline_means_of_groups[i].trace_count;
        baseline_means_of_groups[i].mean_quantized_ratio /=
            baseline_means_of_groups[i].trace_count;
        print_baseline_group_mean("baseline_mean_of_group_means", baselines[i],
                                  baseline_means_of_groups[i]);
    }
}

void command_compare_list(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error(
            "usage: bsel compare-list MODEL INPUT [INPUT ...] "
            "[--baseline fpc|bdi|hybrid|cpack|bpc|huffman ...]");
    }
    std::vector<std::string> paths;
    int first_option = 3;
    while (first_option < argc && std::string(argv[first_option]).rfind("--", 0) != 0) {
        paths.emplace_back(argv[first_option]);
        ++first_option;
    }
    if (paths.empty()) {
        throw std::runtime_error("compare-list requires at least one input trace");
    }

    const auto baselines = parse_baseline_options(argc, argv, first_option);
    const auto model = bsel::load_model(argv[2]);
    const auto target_sizes = target_sizes_for(model);
    bsel::Evaluation byte_select_aggregate;
    byte_select_aggregate.set_blocks.resize(model.sets.size(), 0);
    std::vector<bsel::BaselineEvaluation> baseline_aggregates(baselines.size());
    std::vector<double> baseline_mean_compressed_fractions(baselines.size(), 0.0);
    std::vector<double> baseline_mean_unquantized_ratios(baselines.size(), 0.0);
    std::vector<double> baseline_mean_quantized_ratios(baselines.size(), 0.0);
    double byte_select_mean_compressed_fraction = 0.0;
    double byte_select_mean_unquantized_ratio = 0.0;
    double byte_select_mean_quantized_ratio = 0.0;

    for (const auto& path : paths) {
        const auto blocks = split_blocks(read_bytes(path), model.block_size, false);
        if (blocks.empty()) {
            throw std::runtime_error("comparison input contains no full blocks: " + path);
        }

        const auto byte_select = bsel::evaluate(blocks, model);
        std::cout << "trace path=" << path << '\n';
        std::cout << "byte_select" << '\n';
        print_evaluation(byte_select, model);
        merge_evaluation(byte_select_aggregate, byte_select);
        byte_select_mean_compressed_fraction +=
            static_cast<double>(byte_select.compressed_blocks) / byte_select.blocks;
        byte_select_mean_unquantized_ratio += byte_select.unquantized_compression_ratio();
        byte_select_mean_quantized_ratio += byte_select.compression_ratio();

        for (std::size_t i = 0; i < baselines.size(); ++i) {
            const auto result = bsel::evaluate_baseline(blocks, baselines[i], target_sizes);
            print_baseline_evaluation(result);
            if (baseline_aggregates[i].blocks == 0) {
                baseline_aggregates[i] = result;
            } else {
                merge_baseline_evaluation(baseline_aggregates[i], result);
            }
            baseline_mean_compressed_fractions[i] +=
                static_cast<double>(result.compressed_blocks) / result.blocks;
            baseline_mean_unquantized_ratios[i] += result.unquantized_compression_ratio();
            baseline_mean_quantized_ratios[i] += result.quantized_compression_ratio();
        }
    }

    const auto count = static_cast<double>(paths.size());
    std::cout << "byte_select_mean count=" << paths.size()
              << " compressed_fraction=" << byte_select_mean_compressed_fraction / count
              << " unquantized_ratio=" << byte_select_mean_unquantized_ratio / count
              << " quantized_ratio=" << byte_select_mean_quantized_ratio / count << '\n';
    for (std::size_t i = 0; i < baselines.size(); ++i) {
        std::cout << "baseline_mean algorithm=" << bsel::baseline_kind_name(baselines[i])
                  << " count=" << paths.size()
                  << " compressed_fraction=" << baseline_mean_compressed_fractions[i] / count
                  << " unquantized_ratio=" << baseline_mean_unquantized_ratios[i] / count
                  << " quantized_ratio=" << baseline_mean_quantized_ratios[i] / count << '\n';
    }
    std::cout << "trace_weighted" << '\n';
    std::cout << "byte_select" << '\n';
    print_evaluation(byte_select_aggregate, model);
    for (const auto& result : baseline_aggregates) {
        print_baseline_evaluation(result);
    }
}

void command_compress(int argc, char** argv) {
    if (argc != 5) {
        throw std::runtime_error("usage: bsel compress MODEL INPUT OUTPUT");
    }
    const auto model = bsel::load_model(argv[2]);
    const bsel::OnlineCompressor compressor(model);
    const auto bytes = read_bytes(argv[3]);
    const auto full_blocks = bytes.size() / model.block_size;
    const auto tail_size = bytes.size() % model.block_size;
    std::ofstream output(argv[4], std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot open compressed output");
    }
    output.write("BSEL", 4);
    write_integer<std::uint32_t>(output, 2);
    write_integer<std::uint64_t>(output, bytes.size());
    write_integer<std::uint32_t>(output, static_cast<std::uint32_t>(model.block_size));
    write_integer<std::uint64_t>(output, full_blocks);

    bsel::Evaluation stats;
    stats.set_blocks.resize(model.sets.size(), 0);
    for (std::size_t i = 0; i < full_blocks; ++i) {
        Block block(bytes.begin() + static_cast<std::ptrdiff_t>(i * model.block_size),
                    bytes.begin() + static_cast<std::ptrdiff_t>((i + 1) * model.block_size));
        const auto encoded = compressor.compress_block(block);
        output.put(encoded.compressed ? 1 : 0);
        if (encoded.compressed) {
            write_integer<std::uint32_t>(
                output, static_cast<std::uint32_t>(model.sets[encoded.set_index].target_size));
            write_integer<std::uint64_t>(output, static_cast<std::uint64_t>(encoded.metadata));
            output.write(reinterpret_cast<const char*>(encoded.dictionary.data()),
                         static_cast<std::streamsize>(encoded.dictionary.size()));
        } else {
            output.write(reinterpret_cast<const char*>(block.data()),
                         static_cast<std::streamsize>(block.size()));
        }
        ++stats.blocks;
        stats.original_bytes += block.size();
        stats.encoded_bytes += encoded.encoded_size;
        stats.allocated_bytes += encoded.allocated_size;
        stats.compressed_blocks += encoded.compressed ? 1U : 0U;
        if (encoded.compressed) {
            ++stats.set_blocks[encoded.set_index];
        }
    }
    write_integer<std::uint32_t>(output, static_cast<std::uint32_t>(tail_size));
    output.write(reinterpret_cast<const char*>(bytes.data() + full_blocks * model.block_size),
                 static_cast<std::streamsize>(tail_size));
    print_evaluation(stats, model);
}

void command_decompress(int argc, char** argv) {
    if (argc != 5) {
        throw std::runtime_error("usage: bsel decompress MODEL INPUT OUTPUT");
    }
    const auto model = bsel::load_model(argv[2]);
    const bsel::OnlineDecompressor decompressor(model);
    std::ifstream input(argv[3], std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open compressed input");
    }
    char magic[4]{};
    input.read(magic, 4);
    const auto version = read_integer<std::uint32_t>(input);
    if (std::string(magic, 4) != "BSEL" || (version != 1 && version != 2)) {
        throw std::runtime_error("unsupported compressed format");
    }
    const auto original_size = read_integer<std::uint64_t>(input);
    const auto block_size = read_integer<std::uint32_t>(input);
    const auto block_count = read_integer<std::uint64_t>(input);
    if (block_size != model.block_size) {
        throw std::runtime_error("compressed file and model block sizes differ");
    }
    std::vector<std::uint8_t> output;
    output.reserve(static_cast<std::size_t>(original_size));
    for (std::uint64_t i = 0; i < block_count; ++i) {
        const int flag = input.get();
        if (flag == EOF) {
            throw std::runtime_error("truncated compressed file");
        }
        if (flag != 0 && flag != 1) {
            throw std::runtime_error("invalid compressed block flag");
        }
        bsel::EncodedBlock encoded;
        encoded.original_size = model.block_size;
        encoded.compressed = flag != 0;
        if (encoded.compressed) {
            if (version == 1) {
                encoded.set_index = read_integer<std::uint32_t>(input);
                encoded.pattern_index = read_integer<std::uint32_t>(input);
                const auto dictionary_size = read_integer<std::uint32_t>(input);
                if (encoded.set_index >= model.sets.size() ||
                    dictionary_size != model.sets[encoded.set_index].dictionary_size()) {
                    throw std::runtime_error("invalid compressed dictionary size");
                }
                encoded.metadata = bsel::encode_pattern_metadata(
                    model.sets[encoded.set_index], encoded.pattern_index);
                encoded.dictionary.resize(dictionary_size);
            } else {
                const auto target_size = read_integer<std::uint32_t>(input);
                const auto metadata = read_integer<std::uint64_t>(input);
                if (metadata > std::numeric_limits<std::size_t>::max()) {
                    throw std::runtime_error("compressed metadata is too large for this host");
                }
                encoded.set_index = set_index_for_target_size(model, target_size);
                encoded.metadata = static_cast<std::size_t>(metadata);
                encoded.pattern_index = bsel::decode_pattern_metadata(
                    model.sets[encoded.set_index], encoded.metadata);
                encoded.dictionary.resize(model.sets[encoded.set_index].dictionary_size());
            }
            input.read(reinterpret_cast<char*>(encoded.dictionary.data()),
                       static_cast<std::streamsize>(encoded.dictionary.size()));
        } else {
            encoded.raw.resize(model.block_size);
            input.read(reinterpret_cast<char*>(encoded.raw.data()),
                       static_cast<std::streamsize>(model.block_size));
        }
        if (!input) {
            throw std::runtime_error("truncated compressed block");
        }
        const auto block = decompressor.decompress_block(encoded);
        output.insert(output.end(), block.begin(), block.end());
    }
    const auto tail_size = read_integer<std::uint32_t>(input);
    if (tail_size >= model.block_size) {
        throw std::runtime_error("invalid compressed tail size");
    }
    Block tail(tail_size);
    input.read(reinterpret_cast<char*>(tail.data()), tail_size);
    if (!input || output.size() + tail.size() != original_size) {
        throw std::runtime_error("compressed file size metadata is inconsistent");
    }
    output.insert(output.end(), tail.begin(), tail.end());
    write_bytes(argv[4], output);
}

void command_patterns(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("usage: bsel patterns MODEL [--limit N]");
    }
    const auto model = bsel::load_model(argv[2]);
    bsel::validate_model(model);
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    for (int i = 3; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--limit" && i + 1 < argc) {
            limit = parse_size(argv[++i], "limit");
        } else {
            throw std::runtime_error("unknown option: " + option);
        }
    }
    for (std::size_t set_index = 0; set_index < model.sets.size(); ++set_index) {
        const auto& set = model.sets[set_index];
        std::cout << "pattern_set index=" << set_index
                  << " target=" << set.target_size
                  << " metadata_bytes=" << set.metadata_bytes
                  << " dictionary=" << set.dictionary_size()
                  << " metadata_tag_bits=" << set.metadata_tag_bits
                  << " metadata_tag=" << set.metadata_tag_value
                  << " pattern_id_bits=" << set.pattern_id_bits()
                  << " patterns=" << set.patterns.size() << '\n';
        const auto count = std::min(limit, set.patterns.size());
        for (std::size_t p = 0; p < count; ++p) {
            const auto& pattern = set.patterns[p];
            std::cout << "pattern index=" << p
                      << " rank=" << pattern.rank()
                      << " pid=" << bsel::encode_pattern_metadata(set, p)
                      << " symbols=" << pattern.to_string();
            if (pattern.rank() <= 10) {
                std::cout << " digits=";
                for (const auto symbol : pattern.symbols()) {
                    std::cout << symbol;
                }
            }
            std::cout << '\n';
        }
    }
}

void command_mcc(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("usage: bsel mcc MODEL INPUT [--base N] [--alignment N] [--metadata-granularity N] [--guard-bytes N] [--region-lookback N] [--placement aligned|segment-v1|region-ffd-v2|tail-split-v3|two-ended-tail-v4|spaced-padding-v5] [--limit N] [--no-fill-padding]");
    }
    bsel::MccConfig config;
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    for (int i = 4; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--base" && i + 1 < argc) {
            config.base_address = parse_size(argv[++i], "base address");
        } else if (option == "--alignment" && i + 1 < argc) {
            config.alignment = parse_size(argv[++i], "alignment");
        } else if (option == "--metadata-granularity" && i + 1 < argc) {
            config.metadata_granularity = parse_size(argv[++i], "metadata granularity");
        } else if (option == "--guard-bytes" && i + 1 < argc) {
            config.guard_bytes = parse_nonnegative_size(argv[++i], "guard bytes");
        } else if (option == "--region-lookback" && i + 1 < argc) {
            config.region_lookback = parse_nonnegative_size(argv[++i], "region lookback");
        } else if (option == "--placement" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode == "aligned") {
                config.placement_mode = bsel::MccPlacementMode::AlignedRecords;
            } else if (mode == "segment" || mode == "segment-v1") {
                config.placement_mode = bsel::MccPlacementMode::SegmentPackingV1;
            } else if (mode == "region-ffd-v2" || mode == "v2") {
                config.placement_mode = bsel::MccPlacementMode::RegionFfdV2;
            } else if (mode == "tail-split-v3" || mode == "v3") {
                config.placement_mode = bsel::MccPlacementMode::TailSplitV3;
            } else if (mode == "two-ended-tail-v4" || mode == "v4") {
                config.placement_mode = bsel::MccPlacementMode::TwoEndedTailV4;
            } else if (mode == "spaced-padding-v5" || mode == "v5") {
                config.placement_mode = bsel::MccPlacementMode::SpacedPaddingV5;
            } else {
                throw std::runtime_error("MCC placement must be aligned, segment-v1, region-ffd-v2, tail-split-v3, two-ended-tail-v4, or spaced-padding-v5");
            }
        } else if (option == "--limit" && i + 1 < argc) {
            limit = parse_size(argv[++i], "limit");
        } else if (option == "--no-fill-padding") {
            config.fill_padding = false;
        } else {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
    }

    const auto model = bsel::load_model(argv[2]);
    const auto blocks = split_blocks(read_bytes(argv[3]), model.block_size, false);
    if (blocks.empty()) {
        throw std::runtime_error("MCC input contains no full blocks");
    }
    const auto layout = bsel::place_blocks_in_memory(blocks, model, config);
    std::cout << "mcc blocks=" << layout.stats.blocks
              << " compressed_blocks=" << layout.stats.compressed_blocks
              << " compressed_fraction=" << std::fixed << std::setprecision(6)
              << layout.stats.compressed_fraction()
              << " original_bytes=" << layout.stats.original_bytes
              << " stored_bytes=" << layout.stats.stored_bytes
              << " padding_bytes=" << layout.stats.padding_bytes
              << " physical_bytes=" << layout.stats.physical_bytes
              << " metadata_regions=" << layout.stats.metadata_regions
              << " candidate_segments_scanned="
              << layout.stats.candidate_segments_scanned
              << " candidate_gaps_scanned=" << layout.stats.candidate_gaps_scanned
              << " quantized_ratio=" << layout.stats.quantized_compression_ratio() << '\n';
    const auto count = std::min(limit, layout.entries.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& entry = layout.entries[i];
        std::cout << "mcc_entry logical_block=" << entry.logical_block
                  << " physical_address=" << entry.physical_address
                  << " metadata_region=" << entry.metadata_region
                  << " segment_offset=" << entry.segment_offset
                  << " stored_bytes=" << entry.stored_bytes
                  << " tail_address=" << entry.tail_address
                  << " tail_metadata_region=" << entry.tail_metadata_region
                  << " tail_offset=" << entry.tail_offset
                  << " tail_bytes=" << entry.tail_bytes
                  << " compressed=" << (entry.compressed ? 1 : 0)
                  << " set_index=" << entry.set_index
                  << " metadata=" << entry.metadata << '\n';
    }
}

std::vector<std::size_t> baseline_stored_sizes(const std::vector<Block>& blocks,
                                               bsel::BaselineKind kind) {
    std::vector<std::size_t> sizes;
    sizes.reserve(blocks.size());
    for (const auto& block : blocks) {
        sizes.push_back(bsel::baseline_encoded_size(block, kind));
    }
    return sizes;
}

std::vector<std::size_t> fpc_top256_stored_sizes(const std::vector<Block>& blocks,
                                                 const bsel::FpcTop256Model& model) {
    std::vector<std::size_t> sizes;
    sizes.reserve(blocks.size());
    for (const auto& block : blocks) {
        sizes.push_back(bsel::fpc_top256_encoded_size(block, model));
    }
    return sizes;
}

std::vector<std::size_t> fpc_residual_word256_stored_sizes(
    const std::vector<Block>& blocks, const bsel::FpcResidualWord256Model& model) {
    std::vector<std::size_t> sizes;
    sizes.reserve(blocks.size());
    for (const auto& block : blocks) {
        sizes.push_back(bsel::fpc_residual_word256_encoded_size(block, model));
    }
    return sizes;
}

std::vector<std::size_t> read_size_list(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open size list: " + path);
    }
    std::vector<std::size_t> sizes;
    std::string token;
    while (input >> token) {
        sizes.push_back(parse_size(token, "stored size"));
    }
    return sizes;
}

void print_mcc_layout_summary(const std::string& label, const bsel::MccLayout& layout) {
    std::cout << label << " blocks=" << layout.stats.blocks
              << " compressed_blocks=" << layout.stats.compressed_blocks
              << " compressed_fraction=" << std::fixed << std::setprecision(6)
              << layout.stats.compressed_fraction()
              << " original_bytes=" << layout.stats.original_bytes
              << " stored_bytes=" << layout.stats.stored_bytes
              << " padding_bytes=" << layout.stats.padding_bytes
              << " physical_bytes=" << layout.stats.physical_bytes
              << " metadata_regions=" << layout.stats.metadata_regions
              << " candidate_segments_scanned="
              << layout.stats.candidate_segments_scanned
              << " candidate_gaps_scanned=" << layout.stats.candidate_gaps_scanned
              << " quantized_ratio=" << layout.stats.quantized_compression_ratio() << '\n';
}

void command_mcc_sizes(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("usage: bsel mcc-sizes SIZE_LIST BLOCK_SIZE [--base N] [--alignment N] [--metadata-granularity N] [--guard-bytes N] [--region-lookback N]");
    }
    const auto sizes = read_size_list(argv[2]);
    const auto block_size = parse_size(argv[3], "block size");
    bsel::MccConfig config;
    for (int i = 4; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--base" && i + 1 < argc) {
            config.base_address = parse_size(argv[++i], "base address");
        } else if (option == "--alignment" && i + 1 < argc) {
            config.alignment = parse_size(argv[++i], "alignment");
        } else if (option == "--metadata-granularity" && i + 1 < argc) {
            config.metadata_granularity = parse_size(argv[++i], "metadata granularity");
        } else if (option == "--guard-bytes" && i + 1 < argc) {
            config.guard_bytes = parse_nonnegative_size(argv[++i], "guard bytes");
        } else if (option == "--region-lookback" && i + 1 < argc) {
            config.region_lookback = parse_nonnegative_size(argv[++i], "region lookback");
        } else {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
    }
    auto before_config = config;
    before_config.fill_padding = false;
    before_config.placement_mode = bsel::MccPlacementMode::AlignedRecords;
    auto v1_config = config;
    v1_config.placement_mode = bsel::MccPlacementMode::SegmentPackingV1;
    auto v2_config = config;
    v2_config.placement_mode = bsel::MccPlacementMode::RegionFfdV2;
    auto v3_config = config;
    v3_config.placement_mode = bsel::MccPlacementMode::TailSplitV3;
    auto v4_config = config;
    v4_config.placement_mode = bsel::MccPlacementMode::TwoEndedTailV4;
    auto v5_config = config;
    v5_config.placement_mode = bsel::MccPlacementMode::SpacedPaddingV5;
    print_mcc_layout_summary(
        "mcc_sizes_before",
        bsel::place_stored_blocks_in_memory(sizes, block_size, before_config));
    print_mcc_layout_summary(
        "mcc_sizes_v1",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v1_config));
    print_mcc_layout_summary(
        "mcc_sizes_v2",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v2_config));
    print_mcc_layout_summary(
        "mcc_sizes_v3",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v3_config));
    print_mcc_layout_summary(
        "mcc_sizes_v4",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v4_config));
    print_mcc_layout_summary(
        "mcc_sizes_v5",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v5_config));
}

void command_mcc_baseline(int argc, char** argv) {
    if (argc < 5) {
        throw std::runtime_error(
            "usage: bsel mcc-baseline BASELINE INPUT BLOCK_SIZE "
            "[--guard-bytes N] [--region-lookback N]");
    }
    const auto kind = bsel::parse_baseline_kind(argv[2]);
    const auto block_size = parse_size(argv[4], "block size");
    bsel::MccConfig config;
    for (int i = 5; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--guard-bytes" && i + 1 < argc) {
            config.guard_bytes = parse_nonnegative_size(argv[++i], "guard bytes");
        } else if (option == "--region-lookback" && i + 1 < argc) {
            config.region_lookback = parse_nonnegative_size(argv[++i], "region lookback");
        } else {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
    }
    const auto blocks = split_blocks(read_bytes(argv[3]), block_size, false);
    if (blocks.empty()) throw std::runtime_error("baseline input has no full blocks");
    const auto sizes = baseline_stored_sizes(blocks, kind);
    auto before = config;
    before.fill_padding = false;
    before.placement_mode = bsel::MccPlacementMode::AlignedRecords;
    auto v1 = config; v1.placement_mode = bsel::MccPlacementMode::SegmentPackingV1;
    auto v2 = config; v2.placement_mode = bsel::MccPlacementMode::RegionFfdV2;
    auto v3 = config; v3.placement_mode = bsel::MccPlacementMode::TailSplitV3;
    auto v4 = config; v4.placement_mode = bsel::MccPlacementMode::TwoEndedTailV4;
    auto v5 = config; v5.placement_mode = bsel::MccPlacementMode::SpacedPaddingV5;
    print_mcc_layout_summary("mcc_sizes_before",
        bsel::place_stored_blocks_in_memory(sizes, block_size, before));
    print_mcc_layout_summary("mcc_sizes_v1",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v1));
    print_mcc_layout_summary("mcc_sizes_v2",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v2));
    print_mcc_layout_summary("mcc_sizes_v3",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v3));
    print_mcc_layout_summary("mcc_sizes_v4",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v4));
    print_mcc_layout_summary("mcc_sizes_v5",
        bsel::place_stored_blocks_in_memory(sizes, block_size, v5));
}

void print_mcc_comparison_row(const std::string& algorithm,
                              const bsel::MccLayout& before,
                              const bsel::MccLayout& after) {
    const auto before_ratio = before.stats.quantized_compression_ratio();
    const auto after_ratio = after.stats.quantized_compression_ratio();
    std::cout << "mcc_compare algorithm=" << algorithm
              << " before_quantized_ratio=" << before_ratio
              << " after_quantized_ratio=" << after_ratio
              << " delta=" << after_ratio - before_ratio
              << " before_physical_bytes=" << before.stats.physical_bytes
              << " after_physical_bytes=" << after.stats.physical_bytes
              << " before_metadata_regions=" << before.stats.metadata_regions
              << " after_metadata_regions=" << after.stats.metadata_regions
              << " saved_physical_bytes="
              << static_cast<std::int64_t>(before.stats.physical_bytes) -
                     static_cast<std::int64_t>(after.stats.physical_bytes)
              << " before_padding_bytes=" << before.stats.padding_bytes
              << " after_padding_bytes=" << after.stats.padding_bytes << '\n';
}

void command_mcc_compare(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("usage: bsel mcc-compare MODEL INPUT [--base N] [--alignment N] [--metadata-granularity N]");
    }
    bsel::MccConfig config;
    for (int i = 4; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--base" && i + 1 < argc) {
            config.base_address = parse_size(argv[++i], "base address");
        } else if (option == "--alignment" && i + 1 < argc) {
            config.alignment = parse_size(argv[++i], "alignment");
        } else if (option == "--metadata-granularity" && i + 1 < argc) {
            config.metadata_granularity = parse_size(argv[++i], "metadata granularity");
        } else {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
    }

    const auto model = bsel::load_model(argv[2]);
    const auto blocks = split_blocks(read_bytes(argv[3]), model.block_size, false);
    if (blocks.empty()) {
        throw std::runtime_error("MCC comparison input contains no full blocks");
    }
    auto before_config = config;
    before_config.fill_padding = false;
    before_config.placement_mode = bsel::MccPlacementMode::AlignedRecords;
    auto v1_config = config;
    v1_config.fill_padding = true;
    v1_config.placement_mode = bsel::MccPlacementMode::SegmentPackingV1;
    auto v2_config = config;
    v2_config.fill_padding = true;
    v2_config.placement_mode = bsel::MccPlacementMode::RegionFfdV2;
    auto v3_config = config;
    v3_config.fill_padding = true;
    v3_config.placement_mode = bsel::MccPlacementMode::TailSplitV3;
    auto v4_config = config;
    v4_config.fill_padding = true;
    v4_config.placement_mode = bsel::MccPlacementMode::TwoEndedTailV4;
    auto v5_config = config;
    v5_config.fill_padding = true;
    v5_config.placement_mode = bsel::MccPlacementMode::SpacedPaddingV5;

    print_mcc_comparison_row("bsel",
                             bsel::place_blocks_in_memory(blocks, model, before_config),
                             bsel::place_blocks_in_memory(blocks, model, v1_config));
    print_mcc_comparison_row("bsel_v2",
                             bsel::place_blocks_in_memory(blocks, model, before_config),
                             bsel::place_blocks_in_memory(blocks, model, v2_config));
    print_mcc_comparison_row("bsel_v3",
                             bsel::place_blocks_in_memory(blocks, model, before_config),
                             bsel::place_blocks_in_memory(blocks, model, v3_config));
    print_mcc_comparison_row("bsel_v4",
                             bsel::place_blocks_in_memory(blocks, model, before_config),
                             bsel::place_blocks_in_memory(blocks, model, v4_config));
    print_mcc_comparison_row("bsel_v5",
                             bsel::place_blocks_in_memory(blocks, model, before_config),
                             bsel::place_blocks_in_memory(blocks, model, v5_config));
    const std::vector<bsel::BaselineKind> baselines{
        bsel::BaselineKind::Fpc, bsel::BaselineKind::Bdi,
        bsel::BaselineKind::HybridBdiFpc, bsel::BaselineKind::Cpack,
        bsel::BaselineKind::Bpc, bsel::BaselineKind::Huffman};
    for (const auto kind : baselines) {
        try {
            const auto sizes = baseline_stored_sizes(blocks, kind);
            print_mcc_comparison_row(
                bsel::baseline_kind_name(kind),
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, before_config),
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, v1_config));
            print_mcc_comparison_row(
                std::string(bsel::baseline_kind_name(kind)) + "_v2",
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, before_config),
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, v2_config));
            print_mcc_comparison_row(
                std::string(bsel::baseline_kind_name(kind)) + "_v3",
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, before_config),
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, v3_config));
            print_mcc_comparison_row(
                std::string(bsel::baseline_kind_name(kind)) + "_v4",
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, before_config),
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, v4_config));
            print_mcc_comparison_row(
                std::string(bsel::baseline_kind_name(kind)) + "_v5",
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, before_config),
                bsel::place_stored_blocks_in_memory(sizes, model.block_size, v5_config));
        } catch (const std::exception& error) {
            std::cout << "mcc_compare algorithm=" << bsel::baseline_kind_name(kind)
                      << " status=unsupported reason=\"" << error.what() << "\"\n";
        }
    }
    const auto top256 = bsel::train_fpc_top256(blocks);
    const auto top256_sizes = fpc_top256_stored_sizes(blocks, top256);
    print_mcc_comparison_row(
        "fpc-top256",
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, v1_config));
    print_mcc_comparison_row(
        "fpc-top256_v2",
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, v2_config));
    print_mcc_comparison_row(
        "fpc-top256_v3",
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, v3_config));
    print_mcc_comparison_row(
        "fpc-top256_v4",
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, v4_config));
    print_mcc_comparison_row(
        "fpc-top256_v5",
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(top256_sizes, model.block_size, v5_config));

    const auto word256 = bsel::train_fpc_residual_word256(blocks);
    const auto word256_sizes = fpc_residual_word256_stored_sizes(blocks, word256);
    print_mcc_comparison_row(
        "fpc-resword256",
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, v1_config));
    print_mcc_comparison_row(
        "fpc-resword256_v2",
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, v2_config));
    print_mcc_comparison_row(
        "fpc-resword256_v3",
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, v3_config));
    print_mcc_comparison_row(
        "fpc-resword256_v4",
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, v4_config));
    print_mcc_comparison_row(
        "fpc-resword256_v5",
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word256_sizes, model.block_size, v5_config));

    const auto word64 = bsel::train_fpc_residual_word_dict(blocks, 64);
    const auto word64_sizes = fpc_residual_word256_stored_sizes(blocks, word64);
    print_mcc_comparison_row(
        "fpc-resword64",
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, v1_config));
    print_mcc_comparison_row(
        "fpc-resword64_v2",
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, v2_config));
    print_mcc_comparison_row(
        "fpc-resword64_v3",
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, v3_config));
    print_mcc_comparison_row(
        "fpc-resword64_v4",
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, v4_config));
    print_mcc_comparison_row(
        "fpc-resword64_v5",
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, before_config),
        bsel::place_stored_blocks_in_memory(word64_sizes, model.block_size, v5_config));
}

void command_generate_rtl(int argc, char** argv) {
    if (argc != 4) {
        throw std::runtime_error("usage: bsel generate-rtl MODEL OUTPUT_DIRECTORY");
    }
    const auto model = bsel::load_model(argv[2]);
    bsel::write_rtl(model, argv[3]);
    std::cout << "generated_rtl_sets=" << model.sets.size()
              << " output_directory=" << argv[3] << '\n';
}

void print_usage() {
    std::cout
        << "Byte Select paper reproduction\n"
        << "  bsel train INPUT MODEL [--block-size N] [--threshold N]\n"
        << "             [--target SIZE:PATTERNS:METADATA_BYTES ...]\n"
        << "             [--paper-config bsel-256|bsel-4096|bsel-1024-1024-128]\n"
        << "             [--selection lazy|exhaustive]\n"
        << "             [--holdout-middle PERCENT]\n"
        << "  bsel train-list MODEL INPUT [INPUT ...] [training options]\n"
        << "  bsel evaluate MODEL INPUT\n"
        << "  bsel evaluate-list MODEL INPUT [INPUT ...]\n"
        << "  bsel evaluate-groups MODEL --group NAME INPUT [INPUT ...] [--group ...]\n"
        << "  bsel analyze MODEL INPUT [--ideal-metadata-bytes N]\n"
        << "  bsel analyze-list MODEL INPUT [INPUT ...] [--ideal-metadata-bytes N]\n"
        << "  bsel analyze-groups MODEL --group NAME INPUT [INPUT ...] [--group ...]\n"
        << "                      [--ideal-metadata-bytes N]\n"
        << "  bsel compare MODEL INPUT [--baseline fpc|bdi|hybrid|cpack|bpc|huffman ...]\n"
        << "  bsel compare-list MODEL INPUT [INPUT ...] [--baseline fpc|bdi|hybrid|cpack|bpc|huffman ...]\n"
        << "  bsel compare-groups MODEL --group NAME INPUT [INPUT ...] [--group ...]\n"
        << "                      [--baseline fpc|bdi|hybrid|cpack|bpc|huffman ...]\n"
        << "  bsel compress MODEL INPUT OUTPUT\n"
        << "  bsel decompress MODEL INPUT OUTPUT\n"
        << "  bsel patterns MODEL [--limit N]\n"
        << "  bsel mcc MODEL INPUT [--base N] [--alignment N] [--metadata-granularity N]\n"
        << "           [--placement aligned|segment-v1|region-ffd-v2|tail-split-v3|two-ended-tail-v4|spaced-padding-v5]\n"
        << "           [--limit N] [--no-fill-padding]\n"
        << "  bsel mcc-compare MODEL INPUT [--base N] [--alignment N]\n"
        << "                   [--metadata-granularity N]\n"
        << "  bsel mcc-sizes SIZE_LIST BLOCK_SIZE [--base N] [--alignment N]\n"
        << "  bsel mcc-baseline BASELINE INPUT BLOCK_SIZE [--guard-bytes N] [--region-lookback N]\n"
        << "                 [--metadata-granularity N]\n"
        << "  bsel generate-rtl MODEL OUTPUT_DIRECTORY\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_usage();
            return 0;
        }
        const std::string command = argv[1];
        if (command == "train") {
            command_train(argc, argv);
        } else if (command == "train-list") {
            command_train_list(argc, argv);
        } else if (command == "evaluate") {
            command_evaluate(argc, argv);
        } else if (command == "evaluate-list") {
            command_evaluate_list(argc, argv);
        } else if (command == "evaluate-groups") {
            command_evaluate_groups(argc, argv);
        } else if (command == "analyze") {
            command_analyze(argc, argv);
        } else if (command == "analyze-list") {
            command_analyze_list(argc, argv);
        } else if (command == "analyze-groups") {
            command_analyze_groups(argc, argv);
        } else if (command == "compare") {
            command_compare(argc, argv);
        } else if (command == "compare-list") {
            command_compare_list(argc, argv);
        } else if (command == "compare-groups") {
            command_compare_groups(argc, argv);
        } else if (command == "compress") {
            command_compress(argc, argv);
        } else if (command == "decompress") {
            command_decompress(argc, argv);
        } else if (command == "patterns") {
            command_patterns(argc, argv);
        } else if (command == "mcc") {
            command_mcc(argc, argv);
        } else if (command == "mcc-compare") {
            command_mcc_compare(argc, argv);
        } else if (command == "mcc-sizes") {
            command_mcc_sizes(argc, argv);
        } else if (command == "mcc-baseline") {
            command_mcc_baseline(argc, argv);
        } else if (command == "generate-rtl") {
            command_generate_rtl(argc, argv);
        } else {
            throw std::runtime_error("unknown command: " + command);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
