// PROTOTYPE: experimental sparse GF(2) index for 1..1024 distinct 64-bit patterns.
// Build: c++ -O3 -std=c++17 tools/search_linear_mphf_upto1024.cpp -o /tmp/search_linear_mphf_upto1024
// Run:   /tmp/search_linear_mphf_upto1024 codebook.txt [--seconds 30] [--max-cap 3]
// A found map is always checked on every input key. Beam failure is not a proof
// that no linear map exists. The 32-key exact searcher remains separate.

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr unsigned kMaxKeys = 1024;
constexpr unsigned kInputBits = 64;
constexpr unsigned kWords = kMaxKeys / 64;
using Key = std::uint64_t;
using Signature = std::array<std::uint64_t, kWords>;
using Clock = std::chrono::steady_clock;

unsigned popcount(Key x) { return static_cast<unsigned>(__builtin_popcountll(x)); }

struct Options {
  std::string path;
  double seconds = 30.0;
  unsigned max_cap = 3;
  unsigned beam_width = 32;
  unsigned extra_bits = 0;
  std::size_t max_candidates = 100000;
};

std::uint64_t parse_uint(const std::string& s, const char* name) {
  std::size_t used = 0;
  const auto value = std::stoull(s, &used);
  if (used != s.size()) throw std::runtime_error(std::string("bad ") + name);
  return value;
}

Options parse_options(int argc, char** argv) {
  if (argc < 2) throw std::runtime_error(
      "usage: search_linear_mphf_upto1024 CODEBOOK.txt [--seconds N] "
      "[--max-cap N] [--beam-width N] [--max-candidates N] [--extra-bits N]");
  Options o;
  o.path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (i + 1 >= argc) throw std::runtime_error("missing value: " + arg);
    const std::string value = argv[++i];
    if (arg == "--seconds") {
      std::size_t used = 0;
      o.seconds = std::stod(value, &used);
      if (used != value.size() || !std::isfinite(o.seconds) || o.seconds < 0)
        throw std::runtime_error("--seconds must be finite and nonnegative");
    } else if (arg == "--max-cap") {
      o.max_cap = static_cast<unsigned>(parse_uint(value, "--max-cap"));
      if (o.max_cap < 1 || o.max_cap > 6) throw std::runtime_error("--max-cap must be 1..6");
    } else if (arg == "--beam-width") {
      o.beam_width = static_cast<unsigned>(parse_uint(value, "--beam-width"));
      if (o.beam_width < 1 || o.beam_width > 512)
        throw std::runtime_error("--beam-width must be 1..512");
    } else if (arg == "--max-candidates") {
      o.max_candidates = static_cast<std::size_t>(parse_uint(value, "--max-candidates"));
      if (o.max_candidates < 1) throw std::runtime_error("--max-candidates must be positive");
    } else if (arg == "--extra-bits") {
      o.extra_bits = static_cast<unsigned>(parse_uint(value, "--extra-bits"));
      if (o.extra_bits > 4) throw std::runtime_error("--extra-bits must be 0..4");
    } else {
      throw std::runtime_error("unknown option: " + arg);
    }
  }
  return o;
}

std::vector<Key> read_keys(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open codebook: " + path);
  std::vector<Key> keys;
  std::unordered_set<Key> seen;
  std::string line;
  while (std::getline(input, line)) {
    std::string hex;
    for (unsigned char c : line) {
      if (std::isxdigit(c)) hex.push_back(static_cast<char>(c));
      else if (std::isspace(c) || c == '|') continue;
      else throw std::runtime_error("expected | 16 hexadecimal digits | per line");
    }
    if (hex.empty()) continue;
    if (hex.size() != 16 || keys.size() == kMaxKeys)
      throw std::runtime_error("expected 1..1024 distinct 64-bit keys");
    Key key = 0;
    for (unsigned char c : hex) {
      key <<= 4;
      key |= c <= '9' ? c - '0' : (std::tolower(c) - 'a' + 10);
    }
    if (!seen.insert(key).second) throw std::runtime_error("duplicate key");
    keys.push_back(key);
  }
  if (keys.empty()) throw std::runtime_error("empty codebook");
  return keys;
}

unsigned bits_for(std::size_t n) {
  unsigned bits = 0;
  for (std::size_t slots = 1; slots < n; slots <<= 1) ++bits;
  return bits;
}

std::string hex(Key value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

struct Candidate {
  Key mask = 0;
  unsigned weight = 0;
  Signature signature{};
};

struct Candidate64 {
  Key mask = 0;
  unsigned weight = 0;
  Key signature = 0;
};

struct State {
  std::vector<std::uint16_t> ids;
  std::vector<unsigned> rows;  // candidate indices in increasing order
  unsigned taps = 0;
  std::uint64_t squared_occupancy = 0;
};

// 对不超过 64 个 pattern 的严格 MPHF 搜索，直接保存当前 prefix 分组的
// pattern 集合。这样每次候选只需对每个分组做一次 AND 和 popcount，避免把
// 每个 pattern 的 prefix ID 逐项重建。
struct FastState64 {
  std::array<std::uint64_t, 64> groups{};
  std::array<unsigned, 6> rows{};
  unsigned group_count = 1;
  unsigned row_count = 0;
  unsigned taps = 0;
  std::uint64_t squared_occupancy = 0;
};

struct Stage {
  unsigned cap = 0;
  std::size_t candidates = 0;
  std::uint64_t checked = 0;
  unsigned deepest = 0;
  bool complete = false;  // beam stage completed, not exhaustive proof
};

class Searcher {
 public:
  Searcher(std::vector<Key> keys, Options options)
      : keys_(std::move(keys)), options_(std::move(options)),
        key_bits_(bits_for(keys_.size())), output_bits_(key_bits_ + options_.extra_bits),
        slots_(1U << output_bits_), start_(Clock::now()),
        deadline_(start_ + std::chrono::duration_cast<Clock::duration>(
                                 std::chrono::duration<double>(options_.seconds))) {
    if (output_bits_ > 12) throw std::runtime_error("output width exceeds 12 bits");
    for (unsigned bit = 0; bit < kInputBits; ++bit) {
      for (unsigned i = 0; i < keys_.size(); ++i)
        if ((keys_[i] >> bit) & 1ULL) columns_[bit][i / 64] |= Key{1} << (i % 64);
    }
    active_words_ = (static_cast<unsigned>(keys_.size()) + 63U) / 64U;
    make_column_coordinates();
    make_signature_solver();
  }

  std::string run() {
    if (keys_.size() == 1) {
      best_rows_ = {};
      method_ = "single_key";
      status_ = "optimal";
      return result_json();
    }
    const auto difference_basis = make_difference_basis();
    difference_rank_ = static_cast<unsigned>(difference_basis.size());
    if (difference_rank_ <= output_bits_) {
      best_rows_ = projection_rows(difference_basis);
      method_ = "affine_span_projection";
      status_ = "found_affine_projection";
      if (!verify(*best_rows_)) throw std::runtime_error("projection verification failed");
      return result_json();
    }
    for (unsigned cap = 1; cap <= options_.max_cap; ++cap) {
      if (Clock::now() >= deadline_) { status_ = "time_limit"; break; }
      Stage stage;
      stage.cap = cap;
      if (!generate_weight(cap, 0, cap, 0, 0)) {
        stage.candidates = candidate_count();
        stages_.push_back(stage);
        status_ = candidate_limit_ ? "candidate_limit" : "time_limit";
        break;
      }
      stage.candidates = candidate_count();
      if (keys_.size() <= 64 && output_bits_ <= 6)
        search_fast64(stage);
      else
        search_beam(stage);
      stages_.push_back(stage);
      std::cerr << "cap=" << cap << " candidates=" << stage.candidates
                << " checked=" << stage.checked << " deepest=" << stage.deepest
                << " complete=" << (stage.complete ? "true" : "false")
                << " best=" << (best_rows_ ? std::to_string(best_taps_) : "none") << '\n';
      if (timed_out_) { status_ = "time_limit"; break; }
      if (best_taps_ == key_bits_) { status_ = "optimal"; break; }
    }
    if (status_.empty()) status_ = best_rows_ ? "found_heuristic" : "not_found_within_beam";
    return result_json();
  }

 private:
  std::vector<Key> keys_;
  Options options_;
  unsigned key_bits_ = 0;
  unsigned output_bits_ = 0;
  unsigned slots_ = 0;
  unsigned difference_rank_ = 0;
  unsigned signature_rank_ = 0;
  unsigned active_words_ = 0;
  std::array<Signature, kInputBits> columns_{};
  std::array<Key, kInputBits> column_coordinates_{};
  std::array<Key, kMaxKeys> basis_coordinates_{};
  std::array<Key, kInputBits> signature_basis_{};
  std::array<Key, kInputBits> signature_basis_masks_{};
  std::vector<Key> signature_null_masks_;
  std::vector<Candidate> candidates_;
  std::vector<Candidate64> candidates64_;
  std::unordered_set<Key> seen_behaviors_;
  Signature scratch_{};
  std::vector<Stage> stages_;
  std::optional<std::vector<Key>> best_rows_;
  unsigned best_taps_ = std::numeric_limits<unsigned>::max();
  std::string method_ = "bounded_beam";
  std::string status_;
  Clock::time_point start_;
  Clock::time_point deadline_;
  std::uint64_t generated_masks_ = 0;
  bool timed_out_ = false;
  bool candidate_limit_ = false;

  bool fast_mode() const { return keys_.size() <= 64 && output_bits_ <= 6; }

  std::size_t candidate_count() const {
    return fast_mode() ? candidates64_.size() : candidates_.size();
  }

  static bool bit_at(const Signature& s, unsigned i) {
    return (s[i / 64] >> (i % 64)) & 1ULL;
  }

  void xor_signature(Signature& a, const Signature& b) const {
    for (unsigned w = 0; w < active_words_; ++w) a[w] ^= b[w];
  }

  void make_column_coordinates() {
    std::vector<Signature> basis(keys_.size());
    std::vector<int> basis_index(keys_.size(), -1);
    for (unsigned j = 0; j < kInputBits; ++j) {
      Signature residue = columns_[j];
      Key coordinate = 0;
      for (int pivot = static_cast<int>(keys_.size()) - 1; pivot >= 0; --pivot) {
        if (!bit_at(residue, static_cast<unsigned>(pivot))) continue;
        if (basis_index[pivot] >= 0) {
          xor_signature(residue, basis[pivot]);
          coordinate ^= basis_coordinates_[pivot];
        } else {
          basis[pivot] = residue;
          basis_index[pivot] = static_cast<int>(signature_rank_);
          coordinate ^= Key{1} << signature_rank_;
          basis_coordinates_[pivot] = coordinate;
          ++signature_rank_;
          break;
        }
      }
      column_coordinates_[j] = coordinate;
    }
  }

  void make_signature_solver() {
    if (!fast_mode()) return;
    for (unsigned bit = 0; bit < kInputBits; ++bit) {
      Key residue = columns_[bit][0];
      Key mask = Key{1} << bit;
      for (int pivot = 63; pivot >= 0 && residue; --pivot) {
        if (!(residue & (Key{1} << pivot))) continue;
        if (signature_basis_[pivot]) {
          residue ^= signature_basis_[pivot];
          mask ^= signature_basis_masks_[pivot];
        } else {
          signature_basis_[pivot] = residue;
          signature_basis_masks_[pivot] = mask;
          break;
        }
      }
      if (!residue && mask) signature_null_masks_.push_back(mask);
    }
  }

  std::optional<Key> solve_signature64(Key target) const {
    if (!fast_mode()) return std::nullopt;
    Key mask = 0;
    for (int pivot = 63; pivot >= 0 && target; --pivot) {
      if (!(target & (Key{1} << pivot))) continue;
      if (!signature_basis_[pivot]) return std::nullopt;
      target ^= signature_basis_[pivot];
      mask ^= signature_basis_masks_[pivot];
    }
    return mask;
  }

  std::vector<Key> make_difference_basis() const {
    std::array<Key, 64> echelon{};
    std::vector<Key> independent;
    for (std::size_t i = 1; i < keys_.size(); ++i) {
      Key value = keys_[i] ^ keys_[0];
      for (int bit = 63; bit >= 0 && value; --bit) {
        if (!(value & (Key{1} << bit))) continue;
        if (echelon[bit]) value ^= echelon[bit];
        else {
          echelon[bit] = value;
          independent.push_back(value);
          break;
        }
      }
    }
    return independent;
  }

  std::vector<Key> projection_rows(const std::vector<Key>& difference_basis) const {
    const unsigned rank = static_cast<unsigned>(difference_basis.size());
    std::array<Key, 64> echelon{};
    std::vector<Key> rows;
    for (unsigned bit = 0; bit < 64 && rows.size() < rank; ++bit) {
      Key column = 0;
      for (unsigned i = 0; i < rank; ++i)
        column |= ((difference_basis[i] >> bit) & 1ULL) << i;
      Key value = column;
      for (int pivot = static_cast<int>(rank) - 1; pivot >= 0 && value; --pivot) {
        if (!(value & (Key{1} << pivot))) continue;
        if (echelon[pivot]) value ^= echelon[pivot];
        else {
          echelon[pivot] = value;
          rows.push_back(Key{1} << bit);
          break;
        }
      }
    }
    if (rows.size() != rank) throw std::runtime_error("cannot project affine span");
    rows.resize(output_bits_, 0);
    return rows;
  }

  bool generate_weight(unsigned target, unsigned begin, unsigned left,
                       Key mask, Key coordinate) {
    if ((generated_masks_ & 0xfff) == 0 && Clock::now() >= deadline_) {
      timed_out_ = true;
      return false;
    }
    if (left == 0) {
      ++generated_masks_;
      if (seen_behaviors_.find(coordinate) != seen_behaviors_.end()) return true;
      unsigned ones = 0;
      for (unsigned w = 0; w < (keys_.size() + 63) / 64; ++w) ones += popcount(scratch_[w]);
      const unsigned first_capacity = slots_ / 2;
      if (ones == 0 || ones == keys_.size() || ones > first_capacity ||
          keys_.size() - ones > first_capacity) return true;
      if (candidate_count() == options_.max_candidates) {
        candidate_limit_ = true;
        return false;
      }
      seen_behaviors_.insert(coordinate);
      if (fast_mode())
        candidates64_.push_back(Candidate64{mask, target, scratch_[0]});
      else
        candidates_.push_back(Candidate{mask, target, scratch_});
      return true;
    }
    for (unsigned bit = begin; bit <= 64 - left; ++bit) {
      xor_signature(scratch_, columns_[bit]);
      const bool okay = generate_weight(target, bit + 1, left - 1,
                                        mask | (Key{1} << bit),
                                        coordinate ^ column_coordinates_[bit]);
      xor_signature(scratch_, columns_[bit]);
      if (!okay) return false;
    }
    return true;
  }

  bool verify(const std::vector<Key>& rows) const {
    std::vector<bool> seen(slots_, false);
    for (Key key : keys_) {
      unsigned id = 0;
      for (unsigned r = 0; r < rows.size(); ++r) id |= (popcount(key & rows[r]) & 1U) << r;
      if (seen[id]) return false;
      seen[id] = true;
    }
    return true;
  }

  static bool better(const State& a, const State& b) {
    if (a.taps != b.taps) return a.taps < b.taps;
    if (a.squared_occupancy != b.squared_occupancy)
      return a.squared_occupancy < b.squared_occupancy;
    return a.rows < b.rows;
  }

  void retain(std::vector<State>& beam, State&& state) const {
    if (beam.size() < options_.beam_width) {
      beam.push_back(std::move(state));
      return;
    }
    auto worst = std::max_element(beam.begin(), beam.end(), better);
    if (better(state, *worst)) *worst = std::move(state);
  }

  void search_beam(Stage& stage) {
    std::vector<State> beam{State{std::vector<std::uint16_t>(keys_.size(), 0), {}, 0, 0}};
    for (unsigned depth = 0; depth < output_bits_; ++depth) {
      std::vector<State> next;
      const unsigned count_size = 1U << (depth + 1);
      const unsigned capacity = slots_ >> (depth + 1);
      std::vector<unsigned> counts(count_size, 0);
      std::vector<unsigned> touched;
      for (const auto& state : beam) {
        const unsigned begin = state.rows.empty() ? 0 : state.rows.back() + 1;
        if (candidates_.size() - begin < output_bits_ - depth) continue;
        for (unsigned ci = begin; ci < candidates_.size(); ++ci) {
          if ((++stage.checked & 0x7ff) == 0 && Clock::now() >= deadline_) {
            timed_out_ = true;
            return;
          }
          const auto& candidate = candidates_[ci];
          if (best_rows_ && state.taps + candidate.weight +
                                (output_bits_ - depth - 1) >= best_taps_) continue;
          touched.clear();
          bool valid = true;
          for (unsigned i = 0; i < keys_.size(); ++i) {
            const unsigned id = state.ids[i] | (unsigned(bit_at(candidate.signature, i)) << depth);
            if (counts[id]++ == 0) touched.push_back(id);
            if (counts[id] > capacity) { valid = false; break; }
          }
          std::uint64_t squared = 0;
          if (valid) {
            for (unsigned id : touched) squared += std::uint64_t(counts[id]) * counts[id];
            State child;
            child.ids.resize(keys_.size());
            for (unsigned i = 0; i < keys_.size(); ++i)
              child.ids[i] = state.ids[i] |
                  (unsigned(bit_at(candidate.signature, i)) << depth);
            child.rows = state.rows;
            child.rows.push_back(ci);
            child.taps = state.taps + candidate.weight;
            child.squared_occupancy = squared;
            retain(next, std::move(child));
          }
          for (unsigned id : touched) counts[id] = 0;
        }
      }
      if (next.empty()) { stage.complete = true; return; }
      beam = std::move(next);
      stage.deepest = depth + 1;
    }
    stage.complete = true;
    for (const auto& state : beam) {
      if (best_rows_ && state.taps >= best_taps_) continue;
      std::vector<Key> rows;
      for (unsigned ci : state.rows) rows.push_back(candidates_[ci].mask);
      if (!verify(rows)) throw std::runtime_error("beam verification failed");
      best_taps_ = state.taps;
      best_rows_ = std::move(rows);
    }
  }

  static bool better_fast64(const FastState64& a, const FastState64& b) {
    if (a.taps != b.taps) return a.taps < b.taps;
    if (a.squared_occupancy != b.squared_occupancy)
      return a.squared_occupancy < b.squared_occupancy;
    for (unsigned i = 0; i < a.row_count && i < b.row_count; ++i) {
      if (a.rows[i] != b.rows[i]) return a.rows[i] < b.rows[i];
    }
    return a.row_count < b.row_count;
  }

  void retain_fast64(std::vector<FastState64>& beam, FastState64&& state) const {
    if (beam.size() < options_.beam_width) {
      beam.push_back(std::move(state));
      return;
    }
    auto worst = std::max_element(beam.begin(), beam.end(), better_fast64);
    if (better_fast64(state, *worst)) *worst = std::move(state);
  }

  bool complete_last_fast64(const FastState64& state) {
    if (state.group_count != 32 || state.row_count != 5) return false;
    Key target = 0;
    for (unsigned g = 0; g < state.group_count; ++g) {
      const Key pair = state.groups[g];
      if (__builtin_popcountll(pair) != 2) return false;
      target |= pair & (~pair + 1);
    }

    auto try_target = [&](Key signature) {
      const auto mask = solve_signature64(signature);
      if (!mask || *mask == 0) return false;
      std::vector<Key> rows;
      rows.reserve(6);
      for (unsigned i = 0; i < state.row_count; ++i)
        rows.push_back(candidates64_[state.rows[i]].mask);
      rows.push_back(*mask);
      if (!verify(rows)) return false;
      // Adding any earlier output row to the final row only relabels IDs.
      // Also try input masks with identical behavior (the signature kernel).
      Key cheapest = *mask;
      for (unsigned subset = 0; subset < (1U << state.row_count); ++subset) {
        Key variant = *mask;
        for (unsigned i = 0; i < state.row_count; ++i)
          if (subset & (1U << i)) variant ^= rows[i];
        if (popcount(variant) < popcount(cheapest)) cheapest = variant;
        // Bound enumeration when the input nullspace is large.
        const unsigned null_count = std::min<unsigned>(signature_null_masks_.size(), 8);
        for (unsigned ns = 1; ns < (1U << null_count); ++ns) {
          Key equivalent = variant;
          for (unsigned i = 0; i < null_count; ++i)
            if (ns & (1U << i)) equivalent ^= signature_null_masks_[i];
          if (popcount(equivalent) < popcount(cheapest)) cheapest = equivalent;
        }
      }
      rows.back() = cheapest;
      if (!verify(rows)) throw std::runtime_error("completion relabel verification failed");
      const unsigned taps = state.taps + popcount(cheapest);
      if (!best_rows_ || taps < best_taps_) {
        best_taps_ = taps;
        best_rows_ = std::move(rows);
      }
      return true;
    };

    if (try_target(target)) return true;
    // 对 codimension-1 的 signature span，一次翻转一个 pair 的方向就能
    // 覆盖另一种 parity；逐 pair 尝试仍然只需 32 次 GF(2) 回代。
    for (unsigned g = 0; g < state.group_count; ++g) {
      if (try_target(target ^ state.groups[g])) return true;
    }
    return false;
  }

  // 64-pattern 快路径。groups[g] 是当前 prefix 的一个 pattern 集合，候选
  // signature 与其相交后直接得到下一层的两个子集合。
  void search_fast64(Stage& stage) {
    if (output_bits_ > 6 || keys_.size() > 64) {
      search_beam(stage);
      return;
    }
    const std::uint64_t all_keys = keys_.size() == 64
        ? ~std::uint64_t{0}
        : ((std::uint64_t{1} << keys_.size()) - 1);
    std::vector<FastState64> beam;
    FastState64 root;
    root.groups[0] = all_keys;
    beam.push_back(root);
    for (unsigned depth = 0; depth < output_bits_; ++depth) {
      std::vector<FastState64> next;
      const unsigned capacity = slots_ >> (depth + 1);
      if (keys_.size() == 64 && output_bits_ == 6 && depth == 5) {
        bool found = false;
        for (const auto& state : beam) {
          if ((++stage.checked & 0x3fff) == 0 && Clock::now() >= deadline_) {
            timed_out_ = true;
            return;
          }
          found = complete_last_fast64(state) || found;
        }
        if (found) stage.deepest = output_bits_;
        stage.complete = true;
        return;
      }
      for (const auto& state : beam) {
        const unsigned begin = state.row_count == 0 ? 0 : state.rows[state.row_count - 1] + 1;
        if (candidates64_.size() - begin < output_bits_ - depth) continue;
        for (unsigned ci = begin; ci < candidates64_.size(); ++ci) {
          if ((++stage.checked & 0x3fff) == 0 && Clock::now() >= deadline_) {
            timed_out_ = true;
            return;
          }
          const auto& candidate = candidates64_[ci];
          if (best_rows_ && state.taps + candidate.weight +
                                (output_bits_ - depth - 1) >= best_taps_)
            continue;
          const std::uint64_t signature = candidate.signature;
          FastState64 child = state;
          child.group_count = 0;
          child.row_count = state.row_count + 1;
          child.rows[state.row_count] = ci;
          child.taps = state.taps + candidate.weight;
          child.squared_occupancy = 0;
          bool valid = true;
          for (unsigned g = 0; g < state.group_count; ++g) {
            const std::uint64_t ones = state.groups[g] & signature;
            const unsigned one_count = static_cast<unsigned>(__builtin_popcountll(ones));
            const unsigned group_size = static_cast<unsigned>(
                __builtin_popcountll(state.groups[g]));
            const unsigned zero_count = group_size - one_count;
            if (one_count > capacity || zero_count > capacity) {
              valid = false;
              break;
            }
            child.groups[child.group_count++] = ones;
            child.groups[child.group_count++] = state.groups[g] ^ ones;
            child.squared_occupancy += std::uint64_t(one_count) * one_count +
                                       std::uint64_t(zero_count) * zero_count;
          }
          if (valid) retain_fast64(next, std::move(child));
        }
      }
      if (next.empty()) {
        stage.complete = true;
        return;
      }
      beam = std::move(next);
      stage.deepest = depth + 1;
    }
    stage.complete = true;
    for (const auto& state : beam) {
      if (best_rows_ && state.taps >= best_taps_) continue;
      std::vector<Key> rows;
      rows.reserve(output_bits_);
      for (unsigned i = 0; i < state.row_count; ++i)
        rows.push_back(candidates64_[state.rows[i]].mask);
      if (!verify(rows)) throw std::runtime_error("fast beam verification failed");
      best_taps_ = state.taps;
      best_rows_ = std::move(rows);
    }
  }

  std::string result_json() const {
    std::ostringstream out;
    out << "{\"status\":\"" << status_ << "\",\"method\":\"" << method_
        << "\",\"key_count\":" << keys_.size()
        << ",\"output_bits\":" << output_bits_
        << ",\"output_slots\":" << slots_
        << ",\"empty_slots\":" << slots_ - keys_.size()
        << ",\"difference_rank\":" << difference_rank_
        << ",\"signature_rank\":" << signature_rank_
        << ",\"max_cap\":" << options_.max_cap
        << ",\"beam_width\":" << options_.beam_width
        << ",\"candidates\":" << candidate_count()
        << ",\"generated_masks\":" << generated_masks_
        << ",\"completion_last_row_unbounded\":"
        << (keys_.size() == 64 && output_bits_ == 6 ? "true" : "false")
        << ",\"elapsed_seconds\":"
        << std::chrono::duration<double>(Clock::now() - start_).count()
        << ",\"proof_scope\":\"heuristic beam; only taps==output_bits reaches the trivial lower bound\""
        << ",\"best\":";
    if (!best_rows_) out << "null";
    else {
      out << "{\"rows\":[";
      for (unsigned r = 0; r < best_rows_->size(); ++r) {
        if (r) out << ',';
        out << '"' << hex((*best_rows_)[r]) << '"';
      }
      unsigned taps = 0, active = 0;
      for (Key row : *best_rows_) if (row) { taps += popcount(row); ++active; }
      out << "],\"taps\":" << taps
          << ",\"independent_xors\":" << taps - active
          << ",\"ids\":[";
      for (unsigned i = 0; i < keys_.size(); ++i) {
        unsigned id = 0;
        for (unsigned r = 0; r < best_rows_->size(); ++r)
          id |= (popcount(keys_[i] & (*best_rows_)[r]) & 1U) << r;
        if (i) out << ',';
        out << id;
      }
      out << "]}";
    }
    out << ",\"stages\":[";
    for (unsigned i = 0; i < stages_.size(); ++i) {
      if (i) out << ',';
      const auto& s = stages_[i];
      out << "{\"cap\":" << s.cap << ",\"candidates\":" << s.candidates
          << ",\"checked\":" << s.checked << ",\"deepest\":" << s.deepest
          << ",\"complete\":" << (s.complete ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
  }
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    Searcher searcher(read_keys(options.path), options);
    std::cout << searcher.run() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
