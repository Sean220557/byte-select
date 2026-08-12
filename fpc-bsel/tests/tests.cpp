#include "fpc_bsel/codec.hpp"
#include "fpc_bsel/fpc.hpp"

#include <iostream>
#include <random>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

fpc_bsel::Bytes word_block(const std::vector<std::uint32_t>& values) {
    fpc_bsel::Bytes block;
    for (std::size_t i = 0; i < fpc_bsel::kWordCount; ++i) {
        const auto value = values[i % values.size()];
        for (unsigned byte = 0; byte < 4; ++byte)
            block.push_back(static_cast<std::uint8_t>(value >> (8U * byte)));
    }
    return block;
}
}

int main() try {
    const auto zeros = word_block({0});
    const auto patterns = word_block({0, 7, 0x7f, 0x01010101U, 0x7fff,
                                      0x12340000U, 0xff80ff7fU, 0x12345678U});
    const auto mixed_residual = word_block({0, 0x12345678U});
    check(fpc_bsel::fpc_decode(fpc_bsel::fpc_encode(zeros)) == zeros, "zero FPC round trip");
    check(fpc_bsel::fpc_decode(fpc_bsel::fpc_encode(patterns)) == patterns,
          "all-pattern FPC round trip");
    fpc_bsel::Bytes bitplane;
    for (std::uint32_t word = 0; word < fpc_bsel::kWordCount; ++word) {
        const auto value = 0xdeadbeefU ^ (1U << word);
        for (unsigned byte = 0; byte < 4; ++byte)
            bitplane.push_back(static_cast<std::uint8_t>(value >> (8U * byte)));
    }
    const auto shuffled = fpc_bsel::bitshuffle_words16(bitplane);
    check(fpc_bsel::bitunshuffle_words16(shuffled) == bitplane,
          "bitshuffle round trip");
    check(fpc_bsel::fpc_encode(shuffled).size() < fpc_bsel::fpc_encode(bitplane).size(),
          "bitshuffle improves bit-plane input");
    check(fpc_bsel::pack_tags(fpc_bsel::split_fpc(patterns).tags).size() == 6,
          "packed prefix size");

    std::vector<fpc_bsel::Bytes> training{zeros, patterns, mixed_residual};
    for (int i = 0; i < 30; ++i) training.push_back(patterns);
    for (int i = 0; i < 30; ++i) training.push_back(mixed_residual);
    const auto model = fpc_bsel::train_model(training, 32);
    bool top256_rejected = false;
    try {
        (void)fpc_bsel::train_model(training, 257);
    } catch (const std::invalid_argument&) {
        top256_rejected = true;
    }
    check(top256_rejected, "Top-256 model limit");
    const auto zero_encoded = fpc_bsel::encode_block(zeros, model);
    check(zero_encoded.mode == fpc_bsel::BlockMode::FpcBsel && zero_encoded.prefix_bsel,
          "prefix BSEL path selected");
    const auto pattern_encoded = fpc_bsel::encode_block(mixed_residual, model);
    check(pattern_encoded.mode == fpc_bsel::BlockMode::FpcBsel &&
              pattern_encoded.prefix_bsel && pattern_encoded.residual_bsel,
          "prefix and pattern-7 residual BSEL paths selected");
    for (const auto& block : training) {
        const auto encoded = fpc_bsel::encode_block(block, model);
        check(fpc_bsel::decode_block(encoded.bytes, model) == block, "trained block round trip");
        check(encoded.bytes.size() <= 65, "raw fallback bound");
    }

    std::mt19937 random(0x5eedU);
    for (int test = 0; test < 2000; ++test) {
        fpc_bsel::Bytes block(fpc_bsel::kBlockSize);
        for (auto& byte : block) byte = static_cast<std::uint8_t>(random());
        const auto encoded = fpc_bsel::encode_block(block, model);
        check(fpc_bsel::decode_block(encoded.bytes, model) == block, "random block round trip");
        check(encoded.bytes.size() <= 65, "random raw fallback bound");
    }

    bool rejected = false;
    try { (void)fpc_bsel::decode_block({}, model); } catch (const std::exception&) { rejected = true; }
    check(rejected, "empty stream rejection");
    rejected = false;
    try { (void)fpc_bsel::decode_block({2, 4}, model); } catch (const std::exception&) { rejected = true; }
    check(rejected, "invalid flags rejection");

    std::cout << "all fpc-bsel tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
}
