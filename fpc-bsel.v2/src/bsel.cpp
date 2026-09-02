#include "fpc_bsel/bsel.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace fpc_bsel {
namespace {

std::string key_for(const Pattern& pattern) {
    return std::string(pattern.symbols.begin(), pattern.symbols.end());
}

void put_id(Bytes& out, std::size_t value, std::size_t width) {
    if (width == 1) {
        if (value > 0xffU) throw std::invalid_argument("BSEL value exceeds 8 bits");
        out.push_back(static_cast<std::uint8_t>(value));
        return;
    }
    if (value > 0xffffU) throw std::invalid_argument("BSEL value exceeds 16 bits");
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

std::size_t get_id(const Bytes& data, std::size_t offset, std::size_t width) {
    if (offset + width > data.size()) throw std::runtime_error("truncated BSEL stream");
    if (width == 1) return data[offset];
    return data[offset] | (static_cast<std::size_t>(data[offset + 1]) << 8U);
}

}

std::size_t Pattern::rank() const {
    if (symbols.empty()) return 0;
    return static_cast<std::size_t>(*std::max_element(symbols.begin(), symbols.end())) + 1;
}

Pattern simplest_pattern(const Bytes& block) {
    if (block.empty()) throw std::invalid_argument("BSEL block must not be empty");
    Pattern pattern;
    pattern.symbols.reserve(block.size());
    std::array<int, 256> symbol_for{};
    symbol_for.fill(-1);
    int next = 0;
    for (const auto value : block) {
        if (symbol_for[value] < 0) symbol_for[value] = next++;
        pattern.symbols.push_back(static_cast<std::uint8_t>(symbol_for[value]));
    }
    return pattern;
}

BselModel train_bsel(const std::vector<Bytes>& blocks,
                     const std::vector<std::size_t>& baseline_sizes,
                     std::size_t max_patterns) {
    if (blocks.empty() || blocks.size() != baseline_sizes.size() ||
        max_patterns == 0 || max_patterns > 65535U)
        throw std::invalid_argument("invalid BSEL training configuration");
    const auto block_size = blocks.front().size();
    struct Candidate { std::uint64_t gain = 0; std::uint64_t count = 0; Pattern pattern; };
    std::map<std::string, Candidate> counts;
    const auto id_bytes = max_patterns <= 256 ? 1U : 2U;
    for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        const auto& block = blocks[block_index];
        if (block.size() != block_size) throw std::invalid_argument("mixed BSEL block sizes");
        auto pattern = simplest_pattern(block);
        auto& entry = counts[key_for(pattern)];
        ++entry.count;
        entry.pattern = std::move(pattern);
        const auto encoded_size = id_bytes + entry.pattern.rank();
        if (encoded_size < baseline_sizes[block_index])
            entry.gain += baseline_sizes[block_index] - encoded_size;
    }
    std::vector<Candidate> ranked;
    for (auto& entry : counts) if (entry.second.gain != 0) ranked.push_back(std::move(entry.second));
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.gain != b.gain) return a.gain > b.gain;
        if (a.count != b.count) return a.count > b.count;
        if (a.pattern.rank() != b.pattern.rank()) return a.pattern.rank() < b.pattern.rank();
        return a.pattern.symbols < b.pattern.symbols;
    });
    BselModel model;
    model.block_size = block_size;
    for (std::size_t i = 0; i < std::min(max_patterns, ranked.size()); ++i)
        model.patterns.push_back(std::move(ranked[i].pattern));
    return model;
}

std::size_t bsel_id_bytes(const BselModel& model) {
    return model.patterns.size() <= 256 ? 1U : 2U;
}

std::optional<Bytes> bsel_encode(const Bytes& block, const BselModel& model) {
    if (block.size() != model.block_size) throw std::invalid_argument("incorrect BSEL block size");
    std::optional<Bytes> best;
    for (std::size_t id = 0; id < model.patterns.size(); ++id) {
        const auto& pattern = model.patterns[id];
        if (pattern.symbols.size() != block.size()) throw std::invalid_argument("invalid BSEL model");
        Bytes dictionary(pattern.rank());
        std::vector<bool> assigned(pattern.rank(), false);
        bool matches = true;
        for (std::size_t i = 0; i < block.size(); ++i) {
            const auto symbol = pattern.symbols[i];
            if (symbol >= dictionary.size()) throw std::invalid_argument("invalid BSEL pattern symbol");
            if (!assigned[symbol]) {
                dictionary[symbol] = block[i];
                assigned[symbol] = true;
            } else if (dictionary[symbol] != block[i]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            Bytes result;
            put_id(result, id, bsel_id_bytes(model));
            result.insert(result.end(), dictionary.begin(), dictionary.end());
            if (!best || result.size() < best->size()) best = std::move(result);
        }
    }
    return best;
}

Bytes bsel_decode(const Bytes& encoded, const BselModel& model) {
    const auto width = bsel_id_bytes(model);
    const auto id = get_id(encoded, 0, width);
    if (id >= model.patterns.size()) throw std::runtime_error("unknown BSEL pattern id");
    const auto& pattern = model.patterns[id];
    if (encoded.size() != width + pattern.rank()) throw std::runtime_error("invalid BSEL dictionary size");
    Bytes output;
    output.reserve(pattern.symbols.size());
    for (const auto symbol : pattern.symbols) output.push_back(encoded[width + symbol]);
    return output;
}

}
