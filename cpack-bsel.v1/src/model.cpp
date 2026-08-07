#include "fpc_bsel/model.hpp"
#include "fpc_bsel/cpack.hpp"

#include <fstream>
#include <stdexcept>

namespace fpc_bsel {
namespace {

void write_u32(std::ostream& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.put(static_cast<char>(value >> (8U * i)));
}

std::uint32_t read_u32(std::istream& in) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto c = in.get();
        if (c == EOF) throw std::runtime_error("truncated model");
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << (8U * i);
    }
    return value;
}

void write_set(std::ostream& out, const BselModel& set) {
    write_u32(out, static_cast<std::uint32_t>(set.block_size));
    write_u32(out, static_cast<std::uint32_t>(set.patterns.size()));
    for (const auto& pattern : set.patterns) {
        if (pattern.symbols.size() != set.block_size) throw std::invalid_argument("invalid model pattern");
        out.write(reinterpret_cast<const char*>(pattern.symbols.data()),
                  static_cast<std::streamsize>(pattern.symbols.size()));
    }
}

BselModel read_set(std::istream& in) {
    BselModel set;
    set.block_size = read_u32(in);
    const auto count = read_u32(in);
    if (set.block_size == 0 || set.block_size > 64 || count > 65535U)
        throw std::runtime_error("invalid model dimensions");
    for (std::uint32_t i = 0; i < count; ++i) {
        Pattern pattern;
        pattern.symbols.resize(set.block_size);
        in.read(reinterpret_cast<char*>(pattern.symbols.data()),
                static_cast<std::streamsize>(pattern.symbols.size()));
        if (!in) throw std::runtime_error("truncated model pattern");
        for (const auto symbol : pattern.symbols)
            if (symbol >= pattern.rank()) throw std::runtime_error("invalid pattern symbol");
        set.patterns.push_back(std::move(pattern));
    }
    return set;
}

}

Model train_model(const std::vector<Bytes>& blocks, std::size_t max_patterns) {
    if (blocks.empty()) throw std::invalid_argument("training input has no complete blocks");
    std::vector<Bytes> prefixes;
    std::vector<std::size_t> prefix_baselines;
    prefixes.reserve(blocks.size());
    for (const auto& block : blocks) {
        const auto parts = split_cpack(block);
        prefixes.emplace_back(parts.tags.begin(), parts.tags.end());
        prefix_baselines.push_back(pack_cpack_tags(parts.tags).size());
    }
    std::vector<Bytes> residuals = blocks;
    std::vector<std::size_t> residual_baselines(blocks.size(), kBlockSize);
    return {train_bsel(prefixes, prefix_baselines, max_patterns),
            train_bsel(residuals, residual_baselines, max_patterns)};
}

void save_model(const Model& model, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create model: " + path);
    out.write("CPKBSEL1", 8);
    write_u32(out, 1);
    write_set(out, model.prefix);
    write_set(out, model.residual);
    if (!out) throw std::runtime_error("failed while writing model");
}

Model load_model(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open model: " + path);
    char magic[8]{};
    in.read(magic, 8);
    if (!in || std::string(magic, 8) != "CPKBSEL1" || read_u32(in) != 1)
        throw std::runtime_error("unsupported C-Pack-BSEL model");
    Model model{read_set(in), read_set(in)};
    if (model.prefix.block_size != kWordCount || model.residual.block_size != kBlockSize)
        throw std::runtime_error("model has incorrect block dimensions");
    if (in.peek() != EOF) throw std::runtime_error("trailing data in model");
    return model;
}

}
