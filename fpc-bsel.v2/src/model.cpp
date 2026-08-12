#include "fpc_bsel/model.hpp"
#include "fpc_bsel/fpc.hpp"

#include <algorithm>
#include <fstream>
#include <map>
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
    if (max_patterns == 0 || max_patterns > 256)
        throw std::invalid_argument("FPC-BSEL Top-256 requires 1..256 patterns");
    if (blocks.empty()) throw std::invalid_argument("training input has no complete blocks");
    std::vector<Bytes> prefixes;
    std::vector<Bytes> residuals;
    std::vector<std::size_t> prefix_baselines;
    std::vector<std::size_t> residual_baselines;
    prefixes.reserve(blocks.size());
    residuals.reserve(blocks.size());
    for (const auto& block : blocks) {
        const auto parts = split_fpc(block);
        prefixes.emplace_back(parts.tags.begin(), parts.tags.end());
        residuals.push_back(parts.residual_block);
        prefix_baselines.push_back(pack_tags(parts.tags).size());
        residual_baselines.push_back(parts.raw_residual.size());
    }
    return {train_bsel(prefixes, prefix_baselines, max_patterns),
            train_bsel(residuals, residual_baselines, max_patterns)};
}

Model train_model_stream(const std::string& path, std::size_t max_patterns,
                         std::size_t chunk_bytes, std::uint64_t* block_count,
                         std::size_t codebook_budget_bytes,
                         std::uint64_t offset_bytes,
                         std::uint64_t length_bytes) {
    if (max_patterns == 0 || max_patterns > 256)
        throw std::invalid_argument("FPC-BSEL Top-256 requires 1..256 patterns");
    if (chunk_bytes < kBlockSize) throw std::invalid_argument("stream chunk is too small");
    chunk_bytes -= chunk_bytes % kBlockSize;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open input: " + path);
    if (offset_bytes % kBlockSize != 0 ||
        (length_bytes != 0 && length_bytes % kBlockSize != 0))
        throw std::invalid_argument("stream range must be aligned to 64 bytes");
    input.seekg(static_cast<std::streamoff>(offset_bytes));
    if (!input) throw std::runtime_error("cannot seek stream input");

    struct Candidate {
        std::uint64_t gain = 0;
        std::uint64_t count = 0;
        Pattern pattern;
    };
    std::map<std::string, Candidate> prefix_counts;
    std::map<std::string, Candidate> residual_counts;
    std::vector<std::uint8_t> buffer(chunk_bytes);
    std::uint64_t blocks = 0;
    const auto id_bytes = max_patterns <= 256 ? 1U : 2U;

    auto update = [id_bytes](std::map<std::string, Candidate>& counts,
                             const Bytes& value, std::size_t baseline) {
        auto pattern = simplest_pattern(value);
        const std::string key(pattern.symbols.begin(), pattern.symbols.end());
        auto& candidate = counts[key];
        ++candidate.count;
        candidate.pattern = std::move(pattern);
        const auto encoded = id_bytes + candidate.pattern.rank();
        if (encoded < baseline) candidate.gain += baseline - encoded;
    };

    std::uint64_t remaining = length_bytes;
    while (input && (length_bytes == 0 || remaining != 0)) {
        const auto request = length_bytes == 0
            ? buffer.size()
            : static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(request));
        const auto bytes = static_cast<std::size_t>(input.gcount());
        if (bytes == 0) break;
        if (bytes % kBlockSize != 0)
            throw std::invalid_argument("input size must be a multiple of 64 bytes");
        if (length_bytes != 0) remaining -= bytes;
        for (std::size_t offset = 0; offset < bytes; offset += kBlockSize) {
            Bytes block(buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                        buffer.begin() + static_cast<std::ptrdiff_t>(offset + kBlockSize));
            const auto parts = split_fpc(block);
            update(prefix_counts, Bytes(parts.tags.begin(), parts.tags.end()),
                   pack_tags(parts.tags).size());
            update(residual_counts, parts.residual_block, parts.raw_residual.size());
            ++blocks;
        }
    }
    if (length_bytes != 0 && remaining != 0)
        throw std::runtime_error("stream range exceeds input size");
    if (blocks == 0) throw std::invalid_argument("training input has no complete blocks");

    auto rank = [](std::map<std::string, Candidate>& counts) {
        std::vector<Candidate> ranked;
        for (auto& item : counts)
            if (item.second.gain != 0) ranked.push_back(std::move(item.second));
        std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
            if (left.gain != right.gain) return left.gain > right.gain;
            if (left.count != right.count) return left.count > right.count;
            if (left.pattern.rank() != right.pattern.rank())
                return left.pattern.rank() < right.pattern.rank();
            return left.pattern.symbols < right.pattern.symbols;
        });
        return ranked;
    };

    auto prefix_ranked = rank(prefix_counts);
    auto residual_ranked = rank(residual_counts);
    BselModel prefix_model;
    prefix_model.block_size = kWordCount;
    BselModel residual_model;
    residual_model.block_size = kBlockSize;
    if (codebook_budget_bytes == 0) {
        for (std::size_t i = 0; i < std::min(max_patterns, prefix_ranked.size()); ++i)
            prefix_model.patterns.push_back(std::move(prefix_ranked[i].pattern));
        for (std::size_t i = 0; i < std::min(max_patterns, residual_ranked.size()); ++i)
            residual_model.patterns.push_back(std::move(residual_ranked[i].pattern));
    } else {
        std::size_t prefix = 0, residual = 0, used = 0;
        while (prefix < prefix_ranked.size() || residual < residual_ranked.size()) {
            const bool prefix_fits = prefix < prefix_ranked.size() &&
                                     used + kWordCount <= codebook_budget_bytes;
            const bool residual_fits = residual < residual_ranked.size() &&
                                       used + kBlockSize <= codebook_budget_bytes;
            if (!prefix_fits && !residual_fits) break;
            bool take_prefix = prefix_fits;
            if (prefix_fits && residual_fits) {
                take_prefix = prefix_ranked[prefix].gain * kBlockSize >=
                              residual_ranked[residual].gain * kWordCount;
            } else if (residual_fits) {
                take_prefix = false;
            }
            if (take_prefix) {
                prefix_model.patterns.push_back(
                    std::move(prefix_ranked[prefix++].pattern));
                used += kWordCount;
            } else {
                residual_model.patterns.push_back(
                    std::move(residual_ranked[residual++].pattern));
                used += kBlockSize;
            }
        }
    }
    if (block_count) *block_count = blocks;
    return {std::move(prefix_model), std::move(residual_model)};
}

void save_model(const Model& model, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create model: " + path);
    out.write("FPCBSEL1", 8);
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
    if (!in || std::string(magic, 8) != "FPCBSEL1" || read_u32(in) != 1)
        throw std::runtime_error("unsupported FPC-BSEL model");
    Model model{read_set(in), read_set(in)};
    if (model.prefix.block_size != kWordCount || model.residual.block_size != kBlockSize)
        throw std::runtime_error("model has incorrect block dimensions");
    if (in.peek() != EOF) throw std::runtime_error("trailing data in model");
    return model;
}

}
