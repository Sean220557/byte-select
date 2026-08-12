#include "fpc_bsel/codec.hpp"
#include "fpc_bsel/fpc.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
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

void usage() {
    std::cout <<
        "usage:\n"
        "  fpc-bsel train INPUT MODEL [--max-patterns N]\n"
        "  fpc-bsel compress MODEL INPUT OUTPUT\n"
        "  fpc-bsel decompress MODEL INPUT OUTPUT\n"
        "  fpc-bsel evaluate MODEL INPUT\n"
        "  fpc-bsel roundtrip MODEL INPUT\n";
}
}

int main(int argc, char** argv) try {
    if (argc < 2) { usage(); return 1; }
    const std::string command = argv[1];
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
        std::uint64_t raw = 0, fpc = 0, bitshuffle = 0, combined = 0, prefix = 0, residual = 0;
        for (const auto& block : blocks) {
            const auto encoded = fpc_bsel::encode_block(block, model);
            physical_bytes += encoded.bytes.size();
            // Match the repository baseline convention: the per-cache-line
            // raw/compressed/method selector is external metadata. Internal
            // FPC-BSEL flags remain part of the charged payload.
            encoded_bytes += encoded.bytes.size() - 1;
            if (encoded.mode == fpc_bsel::BlockMode::Raw) ++raw;
            else if (encoded.mode == fpc_bsel::BlockMode::Fpc) ++fpc;
            else if (encoded.mode == fpc_bsel::BlockMode::FpcBitshuffle) ++bitshuffle;
            else { ++combined; prefix += encoded.prefix_bsel; residual += encoded.residual_bsel; }
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
                  << " residual_bsel_blocks=" << residual << '\n';
        return 0;
    }
    usage();
    return 1;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
}
