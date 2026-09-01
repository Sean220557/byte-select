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
    check(fpc_bsel::pack_tags(fpc_bsel::split_fpc(patterns).tags).size() == 8,
          "packed prefix size");
    const std::vector<std::uint32_t> fixed_values{
        0x00000000U,0xffffffffU,0x000000abU,0xab000000U,
        0x0000abcdU,0xabcd0000U,0xffffffabU,0xffffabcdU,
        0xabcdabcdU,0x00ab00cdU,0xab00cd00U};
    for (std::uint8_t tag=0;tag<fixed_values.size();++tag) {
        const auto block=word_block({fixed_values[tag]});
        check(fpc_bsel::split_fpc(block).tags[0]==tag,"fixed 4-bit pattern classification");
        if (fpc_bsel::fpc_decode(fpc_bsel::fpc_encode(block))!=block)
            throw std::runtime_error("fixed 4-bit pattern round trip tag " +
                                     std::to_string(tag));
    }
    const auto complete=word_block({0x12345678U,0x12345678U});
    const auto match3=word_block({0x12345678U,0x123456aaU});
    const auto match2=word_block({0x12345678U,0x1234aabbU});
    const auto alternate=word_block({0x12345678U,0x12aa56bbU});
    check(fpc_bsel::split_fpc(complete).tags[1]==11,"MMMM classification");
    check(fpc_bsel::split_fpc(match3).tags[1]==12,"MMMX classification");
    check(fpc_bsel::split_fpc(match2).tags[1]==13,"MMXX classification");
    check(fpc_bsel::split_fpc(alternate).tags[1]==14,"MXMX classification");

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
    check(fpc_bsel::bsel_id_bytes(model.prefix) == 1, "compact pattern id");
    const auto zero_encoded = fpc_bsel::encode_block(zeros, model);
    check(zero_encoded.mode == fpc_bsel::BlockMode::FpcBsel && zero_encoded.prefix_bsel,
          "prefix BSEL path selected");
    check((zero_encoded.bytes[1] >> 2U) < 63, "prefix id inlined into control byte");
    const auto pattern_encoded = fpc_bsel::encode_block(mixed_residual, model);
    check(fpc_bsel::decode_block(pattern_encoded.bytes, model) == mixed_residual,
          "mixed residual round trip");
    for (const auto& block : training) {
        const auto encoded = fpc_bsel::encode_block(block, model);
        check(fpc_bsel::decode_block(encoded.bytes, model) == block, "trained block round trip");
        check(encoded.bytes.size() <= 65, "raw fallback bound");
    }
    const auto prefix_only = fpc_bsel::encode_block(mixed_residual, model, {true, false});
    const auto residual_only = fpc_bsel::encode_block(mixed_residual, model, {false, true});
    check(fpc_bsel::decode_block(prefix_only.bytes, model) == mixed_residual,
          "prefix-only round trip");
    check(fpc_bsel::decode_block(residual_only.bytes, model) == mixed_residual,
          "residual-only round trip");

    std::mt19937 random(0x5eedU);
    for (int test = 0; test < 2000; ++test) {
        fpc_bsel::Bytes block(fpc_bsel::kBlockSize);
        for (auto& byte : block) byte = static_cast<std::uint8_t>(random());
        check(fpc_bsel::fpc_decode(fpc_bsel::fpc_encode(block)) == block,
              "random direct FPC round trip");
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
