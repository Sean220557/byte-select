#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bsel {

using Block = std::vector<std::uint8_t>;

class Pattern {
public:
    Pattern() = default;
    explicit Pattern(std::vector<std::uint16_t> symbols);

    static Pattern simplest(const Block& block);
    static Pattern parse(const std::string& text);

    std::size_t size() const { return symbols_.size(); }
    std::size_t rank() const;
    const std::vector<std::uint16_t>& symbols() const { return symbols_; }

    bool describes(const Block& block) const;
    bool less_equal(const Pattern& other) const;
    Pattern least_upper_bound(const Pattern& other) const;
    std::string to_string() const;

    bool operator==(const Pattern& other) const { return symbols_ == other.symbols_; }
    bool operator<(const Pattern& other) const { return symbols_ < other.symbols_; }

private:
    std::vector<std::uint16_t> symbols_;
    void canonicalize();
};

struct PatternHash {
    std::size_t operator()(const Pattern& pattern) const noexcept;
};

}  // namespace bsel
