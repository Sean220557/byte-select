#include "byte_select/pattern.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace bsel {

Pattern::Pattern(std::vector<std::uint16_t> symbols) : symbols_(std::move(symbols)) {
    canonicalize();
}

void Pattern::canonicalize() {
    std::unordered_map<std::uint16_t, std::uint16_t> mapping;
    std::uint16_t next = 0;
    for (auto& symbol : symbols_) {
        const auto [it, inserted] = mapping.emplace(symbol, next);
        if (inserted) {
            ++next;
        }
        symbol = it->second;
    }
}

Pattern Pattern::simplest(const Block& block) {
    std::array<int, 256> indices{};
    indices.fill(-1);
    std::vector<std::uint16_t> symbols;
    symbols.reserve(block.size());
    std::uint16_t next = 0;
    for (const auto byte : block) {
        if (indices[byte] < 0) {
            indices[byte] = next++;
        }
        symbols.push_back(static_cast<std::uint16_t>(indices[byte]));
    }
    return Pattern(std::move(symbols));
}

Pattern Pattern::parse(const std::string& text) {
    std::istringstream input(text);
    std::vector<std::uint16_t> symbols;
    unsigned int value = 0;
    while (input >> value) {
        if (value > 65535U) {
            throw std::runtime_error("pattern symbol exceeds uint16 range");
        }
        symbols.push_back(static_cast<std::uint16_t>(value));
    }
    if (!input.eof()) {
        throw std::runtime_error("invalid pattern");
    }
    if (symbols.empty() && !text.empty()) {
        throw std::runtime_error("invalid pattern");
    }
    return Pattern(std::move(symbols));
}

std::size_t Pattern::rank() const {
    std::size_t result = 0;
    for (const auto symbol : symbols_) {
        result = std::max(result, static_cast<std::size_t>(symbol) + 1);
    }
    return result;
}

bool Pattern::describes(const Block& block) const {
    if (block.size() != symbols_.size()) {
        return false;
    }
    std::vector<int> values(rank(), -1);
    for (std::size_t i = 0; i < block.size(); ++i) {
        auto& value = values[symbols_[i]];
        if (value < 0) {
            value = block[i];
        } else if (value != block[i]) {
            return false;
        }
    }
    return true;
}

bool Pattern::less_equal(const Pattern& other) const {
    if (size() != other.size()) {
        throw std::invalid_argument("cannot compare patterns with different sizes");
    }
    // Every equality required by 'other' must also hold in this pattern.
    std::vector<int> representative(other.rank(), -1);
    for (std::size_t i = 0; i < size(); ++i) {
        auto& expected = representative[other.symbols_[i]];
        if (expected < 0) {
            expected = symbols_[i];
        } else if (expected != symbols_[i]) {
            return false;
        }
    }
    return true;
}

Pattern Pattern::least_upper_bound(const Pattern& other) const {
    if (size() != other.size()) {
        throw std::invalid_argument("cannot combine patterns with different sizes");
    }
    std::unordered_map<std::uint32_t, std::uint16_t> pairs;
    std::vector<std::uint16_t> result;
    result.reserve(size());
    std::uint16_t next = 0;
    for (std::size_t i = 0; i < size(); ++i) {
        const auto key = (static_cast<std::uint32_t>(symbols_[i]) << 16U) |
                         static_cast<std::uint32_t>(other.symbols_[i]);
        const auto [it, inserted] = pairs.emplace(key, next);
        if (inserted) {
            ++next;
        }
        result.push_back(it->second);
    }
    return Pattern(std::move(result));
}

std::string Pattern::to_string() const {
    std::ostringstream output;
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
        if (i != 0) {
            output << ' ';
        }
        output << symbols_[i];
    }
    return output.str();
}

std::size_t PatternHash::operator()(const Pattern& pattern) const noexcept {
    std::size_t seed = pattern.size();
    for (const auto symbol : pattern.symbols()) {
        seed ^= static_cast<std::size_t>(symbol) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    }
    return seed;
}

}  // namespace bsel
