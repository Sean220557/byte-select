#include "fpc_bsel/codec.hpp"
#include "fpc_bsel/fpc.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using fpc_bsel::Bytes;

Bytes read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open input: " + path);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

void write_file(const std::string& path, const Bytes& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create output: " + path);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) throw std::runtime_error("failed while writing: " + path);
}

void put_u16(Bytes& out, std::size_t value) {
    if (value > 65535U) throw std::runtime_error("encoded block is too large");
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put_u64(Bytes& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
}

std::size_t get_u16(const Bytes& data, std::size_t& offset) {
    if (offset + 2 > data.size()) throw std::runtime_error("truncated container");
    const auto value = data[offset] | (static_cast<std::size_t>(data[offset + 1]) << 8U);
    offset += 2;
    return value;
}

std::uint64_t get_u64(const Bytes& data, std::size_t& offset) {
    if (offset + 8 > data.size()) throw std::runtime_error("truncated container");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(data[offset++]) << (8U * i);
    return value;
}

std::vector<Bytes> blocks_from(const Bytes& data) {
    if (data.empty() || data.size() % fpc_bsel::kBlockSize != 0)
        throw std::invalid_argument("input size must be a non-zero multiple of 64 bytes");
    std::vector<Bytes> blocks;
    for (std::size_t offset = 0; offset < data.size(); offset += fpc_bsel::kBlockSize)
        blocks.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(offset),
                            data.begin() + static_cast<std::ptrdiff_t>(offset + fpc_bsel::kBlockSize));
    return blocks;
}

Bytes compress_file(const Bytes& input, const fpc_bsel::Model& model) {
    const auto blocks = blocks_from(input);
    Bytes output{'F','P','C','B','S','F','1','\0'};
    put_u64(output, input.size());
    for (const auto& block : blocks) {
        const auto encoded = fpc_bsel::encode_block(block, model);
        put_u16(output, encoded.bytes.size());
        output.insert(output.end(), encoded.bytes.begin(), encoded.bytes.end());
    }
    return output;
}

Bytes decompress_file(const Bytes& input, const fpc_bsel::Model& model) {
    if (input.size() < 16 || std::string(input.begin(), input.begin() + 8) != std::string("FPCBSF1\0", 8))
        throw std::runtime_error("unsupported FPC-BSEL container");
    std::size_t offset = 8;
    const auto original_size = get_u64(input, offset);
    if (original_size == 0 || original_size % fpc_bsel::kBlockSize != 0)
        throw std::runtime_error("invalid original size in container");
    Bytes output;
    output.reserve(static_cast<std::size_t>(original_size));
    while (offset < input.size()) {
        const auto size = get_u16(input, offset);
        if (offset + size > input.size()) throw std::runtime_error("truncated encoded block");
        Bytes encoded(input.begin() + static_cast<std::ptrdiff_t>(offset),
                      input.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
        const auto block = fpc_bsel::decode_block(encoded, model);
        output.insert(output.end(), block.begin(), block.end());
    }
    if (output.size() != original_size) throw std::runtime_error("container block count is inconsistent");
    return output;
}

std::size_t parse_count(const std::string& text) {
    std::size_t used = 0;
    const auto value = std::stoull(text, &used);
    if (used != text.size() || value == 0 || value > 65535U) throw std::invalid_argument("invalid max-patterns");
    return static_cast<std::size_t>(value);
}

std::uint64_t parse_u64(const std::string& text, const char* name) {
    std::size_t used = 0;
    const auto value = std::stoull(text, &used);
    if (used != text.size()) throw std::invalid_argument(std::string("invalid ") + name);
    return value;
}

std::size_t parse_chunk_bytes(const std::string& text) {
    const auto mib = parse_count(text);
    if (mib > std::numeric_limits<std::size_t>::max() / (1024U * 1024U))
        throw std::invalid_argument("chunk size is too large");
    return mib * 1024U * 1024U;
}

template <typename Callback>
std::uint64_t for_each_stream_block(const std::string& path, std::size_t chunk_bytes,
                                    Callback callback, std::uint64_t offset_bytes = 0,
                                    std::uint64_t length_bytes = 0) {
    chunk_bytes -= chunk_bytes % fpc_bsel::kBlockSize;
    if (chunk_bytes < fpc_bsel::kBlockSize)
        throw std::invalid_argument("stream chunk is too small");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open input: " + path);
    if (offset_bytes % fpc_bsel::kBlockSize != 0 ||
        (length_bytes != 0 && length_bytes % fpc_bsel::kBlockSize != 0))
        throw std::invalid_argument("stream range must be aligned to 64 bytes");
    input.seekg(static_cast<std::streamoff>(offset_bytes));
    if (!input) throw std::runtime_error("cannot seek stream input");
    Bytes buffer(chunk_bytes);
    std::uint64_t blocks = 0;
    std::uint64_t remaining = length_bytes;
    while (input && (length_bytes == 0 || remaining != 0)) {
        const auto request = length_bytes == 0
            ? buffer.size()
            : static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(request));
        const auto bytes = static_cast<std::size_t>(input.gcount());
        if (bytes == 0) break;
        if (bytes % fpc_bsel::kBlockSize != 0)
            throw std::invalid_argument("input size must be a multiple of 64 bytes");
        if (length_bytes != 0) remaining -= bytes;
        for (std::size_t offset = 0; offset < bytes; offset += fpc_bsel::kBlockSize) {
            callback(Bytes(buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                           buffer.begin() + static_cast<std::ptrdiff_t>(
                               offset + fpc_bsel::kBlockSize)));
            ++blocks;
        }
    }
    if (length_bytes != 0 && remaining != 0)
        throw std::runtime_error("stream range exceeds input size");
    return blocks;
}

void usage() {
    std::cout <<
        "usage:\n"
        "  fpc-bsel train INPUT MODEL [--max-patterns N]\n"
        "  fpc-bsel train-stream INPUT MODEL --max-patterns N [--chunk-mib N]\n"
        "  fpc-bsel train-budget-stream INPUT MODEL --budget-kib N [--chunk-mib N]\n"
        "  fpc-bsel train-budget-range INPUT MODEL --budget-kib N --offset-bytes N --length-bytes N [--chunk-mib N]\n"
        "  fpc-bsel resize-model INPUT_MODEL OUTPUT_MODEL --max-patterns N\n"
        "  fpc-bsel compress MODEL INPUT OUTPUT\n"
        "  fpc-bsel decompress MODEL INPUT OUTPUT\n"
        "  fpc-bsel evaluate MODEL INPUT\n"
        "  fpc-bsel roundtrip MODEL INPUT\n"
        "  fpc-bsel evaluate-stream MODEL INPUT [--chunk-mib N]\n"
        "  fpc-bsel roundtrip-stream MODEL INPUT [--chunk-mib N]\n"
        "  fpc-bsel evaluate-range MODEL INPUT --offset-bytes N --length-bytes N [--chunk-mib N]\n"
        "  fpc-bsel roundtrip-range MODEL INPUT --offset-bytes N --length-bytes N [--chunk-mib N]\n"
        "  fpc-bsel sizes-256 MODEL INPUT OUTPUT [--roundtrip]\n"
        "  fpc-bsel payloads-256 MODEL INPUT OUTPUT\n"
        "  fpc-bsel lane-sizes-256 MODEL INPUT OUTPUT\n";
}
}

int main(int argc, char** argv) try {
    if (argc < 2) { usage(); return 1; }
    const std::string command = argv[1];
    if (command == "make-empty-model") {
        if (argc != 3) throw std::invalid_argument("usage: fpc-bsel make-empty-model OUTPUT");
        fpc_bsel::Model model;
        model.prefix.block_size = 16;
        model.residual.block_size = 64;
        fpc_bsel::save_model(model, argv[2]);
        return 0;
    }
    if (command == "payloads-256") {
        if (argc != 5)
            throw std::invalid_argument("usage: fpc-bsel payloads-256 MODEL INPUT OUTPUT");
        const auto model = fpc_bsel::load_model(argv[2]);
        const auto input = read_file(argv[3]);
        if (input.empty() || input.size() % 256 != 0)
            throw std::invalid_argument("payloads-256 input must be a multiple of 256 bytes");
        Bytes output;
        output.insert(output.end(), {'F','P','C','P','A','Y','1','\0'});
        put_u64(output, input.size() / 256);
        for (std::size_t subline = 0; subline < input.size(); subline += 256) {
            Bytes payload;
            for (std::size_t lane = 0; lane < 4; ++lane) {
                const auto begin = input.begin() + static_cast<std::ptrdiff_t>(
                    subline + lane * fpc_bsel::kBlockSize);
                const Bytes block(begin, begin + fpc_bsel::kBlockSize);
                const auto encoded = fpc_bsel::encode_block(block, model);
                payload.insert(payload.end(), encoded.bytes.begin() + 1, encoded.bytes.end());
            }
            put_u16(output, payload.size());
            output.insert(output.end(), payload.begin(), payload.end());
        }
        write_file(argv[4], output);
        std::cout << "sublines=" << input.size() / 256
                  << " payload_bytes=" << output.size() - 16 << '\n';
        return 0;
    }
    if (command == "lane-sizes-256") {
        if (argc != 5)
            throw std::invalid_argument("usage: fpc-bsel lane-sizes-256 MODEL INPUT OUTPUT");
        const auto model = fpc_bsel::load_model(argv[2]);
        const auto input = read_file(argv[3]);
        if (input.empty() || input.size() % 256 != 0)
            throw std::invalid_argument("lane-sizes-256 input must be a multiple of 256 bytes");
        std::ofstream sizes(argv[4]);
        if (!sizes) throw std::runtime_error("cannot create lane size list");
        for (std::size_t subline = 0; subline < input.size(); subline += 256) {
            for (std::size_t lane = 0; lane < 4; ++lane) {
                const auto begin = input.begin() + static_cast<std::ptrdiff_t>(
                    subline + lane * fpc_bsel::kBlockSize);
                const Bytes block(begin, begin + fpc_bsel::kBlockSize);
                const auto encoded = fpc_bsel::encode_block(block, model);
                if (lane != 0) sizes << ',';
                // The 256B format replaces four lane mode bytes with one shared
                // mode map, so report only each lane's payload contribution.
                sizes << encoded.bytes.size() - 1;
            }
            sizes << '\n';
        }
        return 0;
    }
    if (command == "sizes-256") {
        if (argc != 5 && argc != 6)
            throw std::invalid_argument("invalid sizes-256 arguments");
        const bool roundtrip = argc == 6 && std::string(argv[5]) == "--roundtrip";
        if (argc == 6 && !roundtrip)
            throw std::invalid_argument("expected --roundtrip");
        const auto model = fpc_bsel::load_model(argv[2]);
        const auto input = read_file(argv[3]);
        if (input.empty() || input.size() % 256 != 0)
            throw std::invalid_argument("sizes-256 input must be a multiple of 256 bytes");
        std::ofstream sizes(argv[4]);
        if (!sizes) throw std::runtime_error("cannot create size list");
        std::uint64_t stored_bytes = 0;
        for (std::size_t subline = 0; subline < input.size(); subline += 256) {
            std::size_t payload_bytes = 0;
            for (std::size_t lane = 0; lane < 4; ++lane) {
                const auto begin = input.begin() + static_cast<std::ptrdiff_t>(
                    subline + lane * fpc_bsel::kBlockSize);
                const Bytes block(begin, begin + fpc_bsel::kBlockSize);
                const auto encoded = fpc_bsel::encode_block(block, model);
                if (roundtrip && fpc_bsel::decode_block(encoded.bytes, model) != block)
                    throw std::runtime_error("sizes-256 lane round-trip mismatch");
                payload_bytes += encoded.bytes.size() - 1;
            }
            const auto stored = std::min<std::size_t>(256, 1 + payload_bytes);
            sizes << stored << '\n';
            stored_bytes += stored;
        }
        std::cout << "sublines=" << input.size() / 256
                  << " original_bytes=" << input.size()
                  << " stored_bytes=" << stored_bytes
                  << std::fixed << std::setprecision(6)
                  << " ratio=" << static_cast<double>(input.size()) / stored_bytes
                  << " control_bytes_per_subline=1\n";
        return 0;
    }
    if (command == "train-budget-stream" || command == "train-budget-range") {
        const bool range = command == "train-budget-range";
        if ((!range && argc != 6 && argc != 8) ||
            (range && argc != 10 && argc != 12))
            throw std::invalid_argument("invalid budget stream training arguments");
        if (std::string(argv[4]) != "--budget-kib")
            throw std::invalid_argument("expected --budget-kib");
        const auto budget_kib = parse_count(argv[5]);
        const auto budget_bytes = budget_kib * 1024U;
        std::uint64_t offset_bytes = 0, length_bytes = 0;
        if (range) {
            if (std::string(argv[6]) != "--offset-bytes" ||
                std::string(argv[8]) != "--length-bytes")
                throw std::invalid_argument("expected stream range options");
            offset_bytes = parse_u64(argv[7], "offset");
            length_bytes = parse_u64(argv[9], "length");
        }
        std::size_t chunk_bytes = 64U * 1024U * 1024U;
        const auto chunk_option = range ? 10 : 6;
        if (argc == chunk_option + 2) {
            if (std::string(argv[chunk_option]) != "--chunk-mib")
                throw std::invalid_argument("expected --chunk-mib");
            chunk_bytes = parse_chunk_bytes(argv[chunk_option + 1]);
        }
        std::uint64_t blocks = 0;
        const auto model = fpc_bsel::train_model_stream(
            argv[2], 256, chunk_bytes, &blocks, budget_bytes,
            offset_bytes, length_bytes);
        fpc_bsel::save_model(model, argv[3]);
        const auto table_bytes = model.prefix.patterns.size() * fpc_bsel::kWordCount +
                                 model.residual.patterns.size() * fpc_bsel::kBlockSize;
        std::cout << "trained_budget_stream blocks=" << blocks
                  << " prefix_patterns=" << model.prefix.patterns.size()
                  << " residual_patterns=" << model.residual.patterns.size()
                  << " table_bytes=" << table_bytes
                  << " budget_bytes=" << budget_bytes
                  << " chunk_bytes=" << chunk_bytes << '\n';
        return 0;
    }
    if (command == "resize-model") {
        if (argc != 6 || std::string(argv[4]) != "--max-patterns")
            throw std::invalid_argument("invalid resize-model arguments");
        const auto max_patterns = parse_count(argv[5]);
        if (max_patterns > 256)
            throw std::invalid_argument("FPC-BSEL Top-256 requires at most 256 patterns");
        auto model = fpc_bsel::load_model(argv[2]);
        if (model.prefix.patterns.size() > max_patterns)
            model.prefix.patterns.resize(max_patterns);
        if (model.residual.patterns.size() > max_patterns)
            model.residual.patterns.resize(max_patterns);
        fpc_bsel::save_model(model, argv[3]);
        std::cout << "resized prefix_patterns=" << model.prefix.patterns.size()
                  << " residual_patterns=" << model.residual.patterns.size() << '\n';
        return 0;
    }
    if (command == "train-stream") {
        if (argc != 6 && argc != 8) throw std::invalid_argument("invalid stream training arguments");
        if (std::string(argv[4]) != "--max-patterns")
            throw std::invalid_argument("expected --max-patterns");
        const auto max_patterns = parse_count(argv[5]);
        std::size_t chunk_bytes = 64U * 1024U * 1024U;
        if (argc == 8) {
            if (std::string(argv[6]) != "--chunk-mib")
                throw std::invalid_argument("expected --chunk-mib");
            chunk_bytes = parse_chunk_bytes(argv[7]);
        }
        std::uint64_t blocks = 0;
        const auto model = fpc_bsel::train_model_stream(
            argv[2], max_patterns, chunk_bytes, &blocks);
        fpc_bsel::save_model(model, argv[3]);
        std::cout << "trained_stream blocks=" << blocks
                  << " prefix_patterns=" << model.prefix.patterns.size()
                  << " residual_patterns=" << model.residual.patterns.size()
                  << " chunk_bytes=" << chunk_bytes << '\n';
        return 0;
    }
    if (command == "evaluate-stream" || command == "roundtrip-stream" ||
        command == "evaluate-range" || command == "roundtrip-range") {
        const bool range = command == "evaluate-range" || command == "roundtrip-range";
        if ((!range && argc != 4 && argc != 6) ||
            (range && argc != 8 && argc != 10))
            throw std::invalid_argument("invalid stream evaluation arguments");
        std::uint64_t offset_bytes = 0, length_bytes = 0;
        if (range) {
            if (std::string(argv[4]) != "--offset-bytes" ||
                std::string(argv[6]) != "--length-bytes")
                throw std::invalid_argument("expected stream range options");
            offset_bytes = parse_u64(argv[5], "offset");
            length_bytes = parse_u64(argv[7], "length");
        }
        std::size_t chunk_bytes = 64U * 1024U * 1024U;
        const auto chunk_option = range ? 8 : 4;
        if (argc == chunk_option + 2) {
            if (std::string(argv[chunk_option]) != "--chunk-mib")
                throw std::invalid_argument("expected --chunk-mib");
            chunk_bytes = parse_chunk_bytes(argv[chunk_option + 1]);
        }
        const auto model = fpc_bsel::load_model(argv[2]);
        std::uint64_t physical_bytes = 0, encoded_bytes = 0;
        std::uint64_t prefix_only_bytes = 0, residual_only_bytes = 0;
        std::uint64_t raw = 0, fpc = 0, bitshuffle = 0, combined = 0;
        std::uint64_t prefix = 0, residual = 0, inline_prefix = 0;
        std::uint64_t checked_blocks = 0;
        const auto blocks = for_each_stream_block(
            argv[3], chunk_bytes, [&](const Bytes& block) {
                const auto encoded = fpc_bsel::encode_block(block, model);
                physical_bytes += encoded.bytes.size();
                encoded_bytes += encoded.bytes.size() - 1;
                const auto prefix_only =
                    fpc_bsel::encode_block(block, model, {true, false});
                const auto residual_only =
                    fpc_bsel::encode_block(block, model, {false, true});
                prefix_only_bytes += prefix_only.bytes.size() - 1;
                residual_only_bytes += residual_only.bytes.size() - 1;
                if (encoded.mode == fpc_bsel::BlockMode::Raw) ++raw;
                else if (encoded.mode == fpc_bsel::BlockMode::Fpc) ++fpc;
                else if (encoded.mode == fpc_bsel::BlockMode::FpcBitshuffle) ++bitshuffle;
                else {
                    ++combined;
                    prefix += encoded.prefix_bsel;
                    residual += encoded.residual_bsel;
                    inline_prefix += encoded.prefix_id_inline;
                }
                if ((command == "roundtrip-stream" || command == "roundtrip-range") &&
                    fpc_bsel::decode_block(encoded.bytes, model) != block)
                    throw std::runtime_error("stream round-trip mismatch at block " +
                                             std::to_string(checked_blocks));
                ++checked_blocks;
            }, offset_bytes, length_bytes);
        const auto original_bytes = blocks * fpc_bsel::kBlockSize;
        std::cout << "blocks=" << blocks << " original_bytes=" << original_bytes
                  << " encoded_bytes=" << encoded_bytes
                  << " physical_encoded_bytes=" << physical_bytes
                  << " chunk_bytes=" << chunk_bytes << std::fixed
                  << std::setprecision(4)
                  << " ratio=" << static_cast<double>(original_bytes) / encoded_bytes
                  << " raw_blocks=" << raw << " fpc_blocks=" << fpc
                  << " bitshuffle_fpc_blocks=" << bitshuffle
                  << " fpc_bsel_blocks=" << combined
                  << " prefix_bsel_blocks=" << prefix
                  << " residual_bsel_blocks=" << residual
                  << " inline_prefix_id_blocks=" << inline_prefix << '\n';
        std::cout << "ablation prefix_only_encoded_bytes=" << prefix_only_bytes
                  << " prefix_only_ratio="
                  << static_cast<double>(original_bytes) / prefix_only_bytes
                  << " residual_only_encoded_bytes=" << residual_only_bytes
                  << " residual_only_ratio="
                  << static_cast<double>(original_bytes) / residual_only_bytes << '\n';
        return 0;
    }
    if (command == "train") {
        if (argc != 4 && argc != 6) throw std::invalid_argument("invalid train arguments");
        std::size_t max_patterns = 256;
        if (argc == 6) {
            if (std::string(argv[4]) != "--max-patterns") throw std::invalid_argument("expected --max-patterns");
            max_patterns = parse_count(argv[5]);
        }
        const auto blocks = blocks_from(read_file(argv[2]));
        const auto model = fpc_bsel::train_model(blocks, max_patterns);
        fpc_bsel::save_model(model, argv[3]);
        std::cout << "trained blocks=" << blocks.size()
                  << " prefix_patterns=" << model.prefix.patterns.size()
                  << " residual_patterns=" << model.residual.patterns.size() << '\n';
        return 0;
    }
    if (command == "compress" || command == "decompress") {
        if (argc != 5) throw std::invalid_argument("invalid codec arguments");
        const auto model = fpc_bsel::load_model(argv[2]);
        const auto input = read_file(argv[3]);
        const auto output = command == "compress" ? compress_file(input, model)
                                                   : decompress_file(input, model);
        write_file(argv[4], output);
        std::cout << command << " input_bytes=" << input.size()
                  << " output_bytes=" << output.size() << '\n';
        return 0;
    }
    if (command == "evaluate" || command == "roundtrip") {
        if (argc != 4) throw std::invalid_argument("invalid evaluation arguments");
        const auto model = fpc_bsel::load_model(argv[2]);
        const auto input = read_file(argv[3]);
        const auto blocks = blocks_from(input);
        std::uint64_t physical_bytes = 0, encoded_bytes = 0;
        std::uint64_t prefix_only_bytes = 0, residual_only_bytes = 0;
        std::uint64_t raw = 0, fpc = 0, bitshuffle = 0, combined = 0, prefix = 0, residual = 0, inline_prefix = 0;
        for (const auto& block : blocks) {
            const auto encoded = fpc_bsel::encode_block(block, model);
            physical_bytes += encoded.bytes.size();
            // Match the repository baseline convention: the per-cache-line
            // raw/compressed/method selector is external metadata. Internal
            // FPC-BSEL flags remain part of the charged payload.
            encoded_bytes += encoded.bytes.size() - 1;
            const auto prefix_only = fpc_bsel::encode_block(block, model, {true, false});
            const auto residual_only = fpc_bsel::encode_block(block, model, {false, true});
            prefix_only_bytes += prefix_only.bytes.size() - 1;
            residual_only_bytes += residual_only.bytes.size() - 1;
            if (encoded.mode == fpc_bsel::BlockMode::Raw) ++raw;
            else if (encoded.mode == fpc_bsel::BlockMode::Fpc) ++fpc;
            else if (encoded.mode == fpc_bsel::BlockMode::FpcBitshuffle) ++bitshuffle;
            else { ++combined; prefix += encoded.prefix_bsel; residual += encoded.residual_bsel;
                   inline_prefix += encoded.prefix_id_inline; }
            if (command == "roundtrip" && fpc_bsel::decode_block(encoded.bytes, model) != block)
                throw std::runtime_error("round-trip mismatch");
        }
        std::cout << "blocks=" << blocks.size() << " original_bytes=" << input.size()
                  << " encoded_bytes=" << encoded_bytes
                  << " physical_encoded_bytes=" << physical_bytes << std::fixed << std::setprecision(4)
                  << " ratio=" << static_cast<double>(input.size()) / encoded_bytes
                  << " raw_blocks=" << raw << " fpc_blocks=" << fpc
                  << " bitshuffle_fpc_blocks=" << bitshuffle
                  << " fpc_bsel_blocks=" << combined << " prefix_bsel_blocks=" << prefix
                  << " residual_bsel_blocks=" << residual
                  << " inline_prefix_id_blocks=" << inline_prefix << '\n';
        std::cout << "ablation prefix_only_encoded_bytes=" << prefix_only_bytes
                  << " prefix_only_ratio=" << static_cast<double>(input.size()) / prefix_only_bytes
                  << " residual_only_encoded_bytes=" << residual_only_bytes
                  << " residual_only_ratio=" << static_cast<double>(input.size()) / residual_only_bytes
                  << '\n';
        return 0;
    }
    usage();
    return 1;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
}
