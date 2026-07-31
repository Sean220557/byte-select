#include "byte_select/codec.hpp"
#include "byte_select/model_io.hpp"
#include "byte_select/trainer.hpp"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using bsel::Block;

struct TargetSpec {
    std::size_t size;
    std::size_t pattern_count;
    std::size_t metadata_bytes;
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

void print_evaluation(const bsel::Evaluation& result);

void command_train(int argc, char** argv) {
    if (argc < 4) {
        throw std::runtime_error("usage: bsel train INPUT MODEL [options]");
    }
    std::size_t block_size = 64;
    std::uint64_t threshold = 1;
    bsel::SelectionMode selection_mode = bsel::SelectionMode::LazyExact;
    std::size_t holdout_percent = 0;
    std::vector<TargetSpec> targets;
    for (int i = 4; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--block-size" && i + 1 < argc) {
            block_size = parse_size(argv[++i], "block size");
        } else if (option == "--threshold" && i + 1 < argc) {
            threshold = parse_size(argv[++i], "threshold");
        } else if (option == "--target" && i + 1 < argc) {
            targets.push_back(parse_target(argv[++i]));
        } else if (option == "--selection" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode == "lazy") {
                selection_mode = bsel::SelectionMode::LazyExact;
            } else if (mode == "exhaustive") {
                selection_mode = bsel::SelectionMode::Exhaustive;
            } else {
                throw std::runtime_error("selection must be lazy or exhaustive");
            }
        } else if (option == "--holdout-middle" && i + 1 < argc) {
            holdout_percent = parse_size(argv[++i], "holdout percentage");
            if (holdout_percent >= 100) {
                throw std::runtime_error("holdout percentage must be below 100");
            }
        } else {
            throw std::runtime_error("unknown or incomplete option: " + option);
        }
    }
    if (targets.empty()) {
        targets.push_back({32, 256, 1});
    }
    const auto all_blocks = split_blocks(read_bytes(argv[2]), block_size, false);
    if (all_blocks.empty()) {
        throw std::runtime_error("training input contains no blocks");
    }
    std::vector<Block> training_blocks = all_blocks;
    std::vector<Block> holdout_blocks;
    if (holdout_percent != 0) {
        const auto holdout_count = all_blocks.size() * holdout_percent / 100;
        const auto holdout_begin = (all_blocks.size() - holdout_count) / 2;
        const auto holdout_end = holdout_begin + holdout_count;
        holdout_blocks.assign(all_blocks.begin() + static_cast<std::ptrdiff_t>(holdout_begin),
                              all_blocks.begin() + static_cast<std::ptrdiff_t>(holdout_end));
        training_blocks.clear();
        training_blocks.insert(training_blocks.end(), all_blocks.begin(),
                               all_blocks.begin() + static_cast<std::ptrdiff_t>(holdout_begin));
        training_blocks.insert(training_blocks.end(),
                               all_blocks.begin() + static_cast<std::ptrdiff_t>(holdout_end),
                               all_blocks.end());
    }

    bsel::Model model;
    model.block_size = block_size;
    for (const auto& target : targets) {
        if (target.metadata_bytes >= target.size) {
            throw std::runtime_error("metadata must be smaller than target");
        }
        bsel::TrainingConfig config{block_size, target.size - target.metadata_bytes,
                                    target.pattern_count, threshold, selection_mode};
        const auto trained = bsel::Trainer(config).train(training_blocks);
        model.sets.push_back({target.size, target.metadata_bytes, trained.patterns});
        std::cout << "target=" << target.size
                  << " dictionary=" << config.dictionary_size
                  << " distinct=" << trained.stats.distinct_patterns
                  << " filtered=" << trained.stats.after_frequency_filter
                  << " maximal=" << trained.stats.maximal_patterns
                  << " combined=" << trained.stats.combined_patterns
                  << " selected=" << trained.patterns.size()
                  << " represented_blocks=" << trained.stats.represented_blocks
                  << " coverage_evaluations="
                  << trained.stats.coverage_evaluations
                  << " recomputations_skipped="
                  << trained.stats.coverage_recomputations_skipped << '\n';
    }
    bsel::save_model(model, argv[3]);
    if (!holdout_blocks.empty()) {
        std::cout << "holdout_middle_percent=" << holdout_percent
                  << " training_blocks=" << training_blocks.size()
                  << " holdout_blocks=" << holdout_blocks.size() << '\n';
        print_evaluation(bsel::evaluate(holdout_blocks, model));
    }
}

void print_evaluation(const bsel::Evaluation& result) {
    std::cout << "blocks=" << result.blocks
              << " compressed_blocks=" << result.compressed_blocks
              << " compressed_fraction=" << std::fixed << std::setprecision(6)
              << (result.blocks == 0 ? 0.0
                                     : static_cast<double>(result.compressed_blocks) /
                                           static_cast<double>(result.blocks))
              << " original_bytes=" << result.original_bytes
              << " allocated_bytes=" << result.allocated_bytes
              << " quantized_ratio=" << result.compression_ratio() << '\n';
}

void command_evaluate(int argc, char** argv) {
    if (argc != 4) {
        throw std::runtime_error("usage: bsel evaluate MODEL INPUT");
    }
    const auto model = bsel::load_model(argv[2]);
    bsel::validate_model(model);
    const auto blocks = split_blocks(read_bytes(argv[3]), model.block_size, false);
    print_evaluation(bsel::evaluate(blocks, model));
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
    write_integer<std::uint32_t>(output, 1);
    write_integer<std::uint64_t>(output, bytes.size());
    write_integer<std::uint32_t>(output, static_cast<std::uint32_t>(model.block_size));
    write_integer<std::uint64_t>(output, full_blocks);

    bsel::Evaluation stats;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        Block block(bytes.begin() + static_cast<std::ptrdiff_t>(i * model.block_size),
                    bytes.begin() + static_cast<std::ptrdiff_t>((i + 1) * model.block_size));
        const auto encoded = compressor.compress_block(block);
        output.put(encoded.compressed ? 1 : 0);
        if (encoded.compressed) {
            write_integer<std::uint32_t>(
                output, static_cast<std::uint32_t>(encoded.set_index));
            write_integer<std::uint32_t>(
                output, static_cast<std::uint32_t>(encoded.pattern_index));
            write_integer<std::uint32_t>(
                output, static_cast<std::uint32_t>(encoded.dictionary.size()));
            output.write(reinterpret_cast<const char*>(encoded.dictionary.data()),
                         static_cast<std::streamsize>(encoded.dictionary.size()));
        } else {
            output.write(reinterpret_cast<const char*>(block.data()),
                         static_cast<std::streamsize>(block.size()));
        }
        ++stats.blocks;
        stats.original_bytes += block.size();
        stats.allocated_bytes += encoded.allocated_size;
        stats.compressed_blocks += encoded.compressed ? 1U : 0U;
    }
    write_integer<std::uint32_t>(output, static_cast<std::uint32_t>(tail_size));
    output.write(reinterpret_cast<const char*>(bytes.data() + full_blocks * model.block_size),
                 static_cast<std::streamsize>(tail_size));
    print_evaluation(stats);
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
    if (std::string(magic, 4) != "BSEL" || read_integer<std::uint32_t>(input) != 1) {
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
        bsel::EncodedBlock encoded;
        encoded.original_size = model.block_size;
        encoded.compressed = flag != 0;
        if (encoded.compressed) {
            encoded.set_index = read_integer<std::uint32_t>(input);
            encoded.pattern_index = read_integer<std::uint32_t>(input);
            const auto dictionary_size = read_integer<std::uint32_t>(input);
            encoded.dictionary.resize(dictionary_size);
            input.read(reinterpret_cast<char*>(encoded.dictionary.data()), dictionary_size);
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
    Block tail(tail_size);
    input.read(reinterpret_cast<char*>(tail.data()), tail_size);
    if (!input || output.size() + tail.size() != original_size) {
        throw std::runtime_error("compressed file size metadata is inconsistent");
    }
    output.insert(output.end(), tail.begin(), tail.end());
    write_bytes(argv[4], output);
}

void print_usage() {
    std::cout
        << "Byte Select paper reproduction\n"
        << "  bsel train INPUT MODEL [--block-size N] [--threshold N]\n"
        << "             [--target SIZE:PATTERNS:METADATA_BYTES ...]\n"
        << "             [--selection lazy|exhaustive]\n"
        << "             [--holdout-middle PERCENT]\n"
        << "  bsel evaluate MODEL INPUT\n"
        << "  bsel compress MODEL INPUT OUTPUT\n"
        << "  bsel decompress MODEL INPUT OUTPUT\n";
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
        } else if (command == "evaluate") {
            command_evaluate(argc, argv);
        } else if (command == "compress") {
            command_compress(argc, argv);
        } else if (command == "decompress") {
            command_decompress(argc, argv);
        } else {
            throw std::runtime_error("unknown command: " + command);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
