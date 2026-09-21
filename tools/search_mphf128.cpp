// Search hardware-friendly static MPHF families for a fixed 8-byte key set.
// Build with: c++ -O3 -std=c++17 search_mphf128.cpp -o search_mphf128
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
#ifndef MPHF_N
#define MPHF_N 128
#endif
#ifndef MPHF_SEED_BITS
#define MPHF_SEED_BITS 64
#endif
#ifndef MPHF_SHARED_SEED
#define MPHF_SHARED_SEED 0
#endif
#ifndef MPHF_MIX_FOLD
#define MPHF_MIX_FOLD 0
#endif
#ifndef MPHF_MIX_MULTIPLY
#define MPHF_MIX_MULTIPLY 0
#endif
#ifndef MPHF_EXHAUSTIVE
#define MPHF_EXHAUSTIVE 0
#endif
#if MPHF_EXHAUSTIVE && MPHF_SEED_BITS > 10
#error "exhaustive mode needs MPHF_SEED_BITS <= 10 (pair count 2^(2*bits))"
#endif
constexpr unsigned N = MPHF_N;
static_assert(N >= 2 && N <= 128 && (N & (N - 1)) == 0,
              "MPHF_N must be a power of two between 2 and 128");
constexpr unsigned ID_BITS = [] {
    unsigned bits = 0;
    for (unsigned value = N; value > 1; value >>= 1) ++bits;
    return bits;
}();
constexpr unsigned ID_SHIFT = 64 - ID_BITS;
constexpr unsigned MAX_BUCKETS = 128;
static_assert(MPHF_SEED_BITS >= 8 && MPHF_SEED_BITS <= 64,
              "MPHF_SEED_BITS must be between 8 and 64");
constexpr std::uint64_t SEED_MASK = MPHF_SEED_BITS == 64
    ? ~std::uint64_t{0}
    : ((std::uint64_t{1} << MPHF_SEED_BITS) - 1);

std::uint64_t rotl(std::uint64_t x, unsigned r) {
    return r ? ((x << r) | (x >> (64 - r))) : x;
}

std::uint64_t mix(std::uint64_t x) {
    // Hardware-oriented mixer: no general multiplier, only shifts, rotates,
    // XORs and one constant add. Construction and query use the same mixer.
    x += 0x9e3779b97f4a7c15ULL;
#if MPHF_MIX_FOLD
    x ^= x >> 32;
#endif
    x ^= rotl(x, 17);
    x ^= x >> 29;
    x ^= rotl(x, 41);
#if MPHF_MIX_FOLD
    x ^= x >> 23;
#endif
#if MPHF_MIX_MULTIPLY
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
#endif
    return x;
}

unsigned unique_count(const std::vector<std::uint64_t>& keys,
                      std::uint64_t m1, std::uint64_t m2,
                      unsigned shift, unsigned rotate) {
    std::array<bool, N> seen{};
    unsigned unique = 0;
    for (const auto key : keys) {
        const auto z = ((key ^ (key >> shift)) * m1) ^
                       rotl((key * m2), rotate);
        const auto id = static_cast<unsigned>((z >> ID_SHIFT) & (N - 1));
        if (!seen[id]) {
            seen[id] = true;
            ++unique;
        }
    }
    return unique;
}

unsigned unique_count_single(const std::vector<std::uint64_t>& keys,
                             std::uint64_t multiplier, unsigned shift) {
    std::array<bool, N> seen{};
    unsigned unique = 0;
    for (const auto key : keys) {
        const auto z = (key ^ (key >> shift)) * multiplier;
        const auto id = static_cast<unsigned>((z >> ID_SHIFT) & (N - 1));
        if (!seen[id]) {
            seen[id] = true;
            ++unique;
        }
    }
    return unique;
}

struct SearchResult {
    bool success = false;
    std::uint64_t attempts = 0;
    unsigned best_unique = 0;
    std::uint64_t m1 = 0, m2 = 0;
    unsigned shift = 0, rotate = 0;
    double ms = 0;
};

SearchResult search_formula(const std::vector<std::uint64_t>& keys,
                            std::uint64_t attempts, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    SearchResult result;
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < attempts; ++i) {
        const auto m1 = rng() | 1ULL;
        const auto m2 = rng() | 1ULL;
        const auto shift = static_cast<unsigned>(1 + rng() % 63);
        const auto rotate = static_cast<unsigned>(rng() % 64);
        const auto unique = unique_count(keys, m1, m2, shift, rotate);
        result.attempts = i + 1;
        if (unique > result.best_unique) {
            result.best_unique = unique;
            result.m1 = m1;
            result.m2 = m2;
            result.shift = shift;
            result.rotate = rotate;
        }
        if (unique == N) {
            result.success = true;
            break;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - begin).count();
    return result;
}

SearchResult search_single_formula(const std::vector<std::uint64_t>& keys,
                                   std::uint64_t attempts, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    SearchResult result;
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < attempts; ++i) {
        const auto multiplier = rng() | 1ULL;
        const auto shift = static_cast<unsigned>(1 + rng() % 63);
        const auto unique = unique_count_single(keys, multiplier, shift);
        result.attempts = i + 1;
        if (unique > result.best_unique) {
            result.best_unique = unique;
            result.m1 = multiplier;
            result.shift = shift;
        }
        if (unique == N) {
            result.success = true;
            break;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - begin).count();
    return result;
}

struct DisplacementResult {
    bool success = false;
    std::uint64_t attempts = 0;
    unsigned buckets = 0;
    unsigned max_bucket = 0;
    unsigned max_displacement = 0;
    std::array<std::uint8_t, MAX_BUCKETS> displacement{};
    std::uint64_t seed1 = 0, seed2 = 0;
    double ms = 0;
};

unsigned bits_required(unsigned value) {
    unsigned bits = 0;
    do {
        ++bits;
        value >>= 1;
    } while (value);
    return bits;
}

bool construct_displacement(const std::vector<std::uint64_t>& keys,
                            unsigned bucket_count, std::uint64_t seed1,
                            std::uint64_t seed2, DisplacementResult& result) {
    std::vector<std::vector<unsigned>> buckets(bucket_count);
    std::array<unsigned, N> h2{};
    for (unsigned i = 0; i < keys.size(); ++i) {
        buckets[mix(keys[i] ^ seed1) & (bucket_count - 1)].push_back(i);
        h2[i] = static_cast<unsigned>(mix(keys[i] ^ seed2) & (N - 1));
    }
    std::vector<unsigned> order(bucket_count);
    for (unsigned i = 0; i < bucket_count; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return buckets[a].size() > buckets[b].size();
    });
    std::array<bool, N> used{};
    unsigned max_bucket = 0, max_d = 0;
    for (const auto bucket : order) {
        const auto& items = buckets[bucket];
        max_bucket = std::max<unsigned>(max_bucket, items.size());
        bool placed = false;
        for (unsigned d = 0; d < N; ++d) {
            std::array<bool, N> local{};
            bool ok = true;
            for (const auto index : items) {
                const auto slot = (h2[index] + d) & (N - 1);
                if (used[slot] || local[slot]) { ok = false; break; }
                local[slot] = true;
            }
            if (!ok) continue;
            result.displacement[bucket] = static_cast<std::uint8_t>(d);
            max_d = std::max(max_d, d);
            for (const auto index : items) used[(h2[index] + d) & (N - 1)] = true;
            placed = true;
            break;
        }
        if (!placed) return false;
    }
    result.success = true;
    result.buckets = bucket_count;
    result.max_bucket = max_bucket;
    result.max_displacement = max_d;
    result.seed1 = seed1;
    result.seed2 = seed2;
    return true;
}

DisplacementResult search_displacement(const std::vector<std::uint64_t>& keys,
                                       unsigned bucket_count,
                                       std::uint64_t attempts,
                                       std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    DisplacementResult result;
    result.buckets = bucket_count;
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < attempts; ++i) {
        DisplacementResult candidate;
        candidate.attempts = i + 1;
        const auto seed1 = rng() & SEED_MASK;
        const auto seed2 = MPHF_SHARED_SEED
            ? ((seed1 ^ (0x9e3779b97f4a7c15ULL & SEED_MASK)) & SEED_MASK)
            : (rng() & SEED_MASK);
        if (construct_displacement(keys, bucket_count, seed1, seed2, candidate)) {
            result = candidate;
            result.attempts = i + 1;
            break;
        }
        result.attempts = i + 1;
    }
    const auto end = std::chrono::steady_clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - begin).count();
    return result;
}

#if MPHF_EXHAUSTIVE
// Deterministic full enumeration of the seed pair space.  Used to prove that
// a (mixer, seed width, bucket count) configuration has no displacement
// solution: success=false after this loop means every pair was tried.
DisplacementResult search_displacement_exhaustive(
    const std::vector<std::uint64_t>& keys, unsigned bucket_count) {
    DisplacementResult result;
    result.buckets = bucket_count;
    const auto begin = std::chrono::steady_clock::now();
    std::uint64_t tried = 0;
    for (std::uint64_t seed1 = 0; seed1 <= SEED_MASK; ++seed1) {
        for (std::uint64_t seed2 = 0; seed2 <= SEED_MASK; ++seed2) {
            ++tried;
            DisplacementResult candidate;
            candidate.attempts = tried;
            if (construct_displacement(keys, bucket_count, seed1, seed2, candidate)) {
                result = candidate;
                result.attempts = tried;
                result.ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - begin).count();
                return result;
            }
        }
    }
    result.attempts = tried;
    result.ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - begin).count();
    return result;
}
#endif

std::vector<std::uint64_t> read_keys(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open key file");
    std::vector<std::uint64_t> keys;
    while (true) {
        std::array<unsigned char, 8> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (input.gcount() == 0) break;
        if (input.gcount() != 8) throw std::runtime_error("truncated key file");
        std::uint64_t value = 0;
        for (const auto byte : bytes) value = (value << 8) | byte;
        keys.push_back(value);
    }
    if (keys.size() != N) throw std::runtime_error("expected exactly 128 keys");
    return keys;
}

void print_formula(const char* name, const SearchResult& r) {
    std::cout << "  \"" << name << "\": {\"success\": "
              << (r.success ? "true" : "false")
              << ", \"attempts\": " << r.attempts
              << ", \"best_unique\": " << r.best_unique
              << ", \"elapsed_ms\": " << std::fixed << std::setprecision(3) << r.ms
              << ", \"m1\": " << r.m1 << ", \"m2\": " << r.m2
              << ", \"shift\": " << r.shift << ", \"rotate\": " << r.rotate << "},\n";
}

void print_displacement(const DisplacementResult& r, bool trailing_comma) {
    const auto entry_bits = r.success ? bits_required(r.max_displacement) : 7;
    const auto table_bits = r.buckets * entry_bits;
    const auto seed_bytes = (MPHF_SHARED_SEED ? 1 : 2) * ((MPHF_SEED_BITS + 7) / 8);
    const auto parameter_bytes = (table_bits + 7) / 8 + seed_bytes;
    std::cout << "    {\"success\": " << (r.success ? "true" : "false")
              << ", \"attempts\": " << r.attempts
              << ", \"elapsed_ms\": " << std::fixed << std::setprecision(3) << r.ms
              << ", \"bucket_count\": " << r.buckets
              << ", \"max_bucket\": " << r.max_bucket
              << ", \"max_displacement\": " << r.max_displacement
              << ", \"entry_bits\": " << entry_bits
              << ", \"table_bits\": " << table_bits
              << ", \"parameter_bytes_bit_packed\": " << parameter_bytes
              << ", \"parameter_bytes_byte_entries\": " << (r.buckets + seed_bytes)
              << ", \"seed1\": " << r.seed1
              << ", \"seed2\": " << r.seed2
              << ", \"displacements\": [";
    for (unsigned i = 0; i < r.buckets; ++i) {
        if (i) std::cout << ", ";
        std::cout << static_cast<unsigned>(r.displacement[i]);
    }
    std::cout << "]}" << (trailing_comma ? "," : "") << "\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::cerr << "usage: search_mphf128 CODEBOOK.bin [formula_attempts] [disp_attempts]"
                  << " [bucket_counts, e.g. 8,16 | all]\n";
        return 2;
    }
    const auto keys = read_keys(argv[1]);
    const std::uint64_t formula_attempts = argc >= 3 ? std::stoull(argv[2]) : 10000000;
    const std::uint64_t displacement_attempts = argc >= 4 ? std::stoull(argv[3]) : 100000;
    std::vector<unsigned> bucket_counts;
    if (argc >= 5 && std::string(argv[4]) != "all") {
        std::string list = argv[4];
        for (std::size_t pos = 0; pos < list.size();) {
            const auto comma = list.find(',', pos);
            const auto token = list.substr(pos, comma == std::string::npos
                                                    ? std::string::npos : comma - pos);
            bucket_counts.push_back(static_cast<unsigned>(std::stoul(token)));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    } else {
        bucket_counts = {4, 8, 16, 32, 64, 128};
    }
    const auto single = search_single_formula(keys, formula_attempts, 0xC0570F0CULL);
    const auto formula = search_formula(keys, formula_attempts, 0xC0570F0CULL);
    std::vector<DisplacementResult> displacements(bucket_counts.size());
#if MPHF_EXHAUSTIVE
    for (unsigned i = 0; i < bucket_counts.size(); ++i)
        displacements[i] = search_displacement_exhaustive(keys, bucket_counts[i]);
#else
    for (unsigned i = 0; i < bucket_counts.size(); ++i) {
        displacements[i] = search_displacement(
            keys, bucket_counts[i], displacement_attempts,
            0xBADC0DEULL ^ (static_cast<std::uint64_t>(bucket_counts[i]) << 32));
    }
#endif
    std::cout << "{\n";
#if MPHF_EXHAUSTIVE
    std::cout << "  \"exhaustive\": true,\n";
#endif
    print_formula("single_multiply_shift", single);
    print_formula("two_multiply_xor", formula);
    std::cout << "  \"two_level_displacement\": [\n";
    for (unsigned i = 0; i < displacements.size(); ++i)
        print_displacement(displacements[i], i + 1 != displacements.size());
    std::cout << "  ]\n}";
    return std::any_of(displacements.begin(), displacements.end(),
                       [](const auto& result) { return result.success; }) ? 0 : 1;
}
