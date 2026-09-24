// 针对 32 个固定 64 位 pattern 的精确稀疏 GF(2) 映射搜索。
//
// 该搜索有意地数据相关：一行（row）用其 64 位输入 mask 表示，而其 32 位
// signature 则用于快速的划分检查。候选（candidate）按 tap 权重递增生成，
// 按 signature 去重，并通过精确的组划分 DFS 进行组合。
//
// 构建：
//   c++ -O3 -std=c++17 tools/search_linear_mphf.cpp -o /tmp/search_linear_mphf
// 运行：
//   /tmp/search_linear_mphf results/mphf-32/codebook1.txt

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
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr unsigned kKeys = 32;
constexpr unsigned kRows = 5;
using Key = std::uint64_t;
using Signature = std::uint32_t;

// 功能：统计一个 64 位输入 mask 中置位的个数（即某行的 tap 数）。
// 输入：
//   - x：待统计的 64 位 Key。
// 输出/返回：x 中置位的个数。
unsigned popcount64(Key x) {
  return static_cast<unsigned>(__builtin_popcountll(x));
}

// 功能：统计一个 32 位 signature 中置位的个数（即对某行投 1 的 pattern 数量）。
// 输入：
//   - x：待统计的 32 位 Signature。
// 输出/返回：x 中置位的个数。
unsigned popcount32(Signature x) {
  return static_cast<unsigned>(__builtin_popcount(x));
}

// 含义：一个候选的输出行（row）。
//   - weight：该行的 tap 个数。
//   - signature：该行在整个 codebook 上的 32 位行为。
//   - mask：该行具体的 64 位输入 mask。
struct Candidate {
  unsigned weight;
  Signature signature;
  Key mask;
};

// 含义：目前找到的最优 5 行映射，以及它产生的 32 个 ID（一个置换）。
//   - rows：5 行输出行的输入 mask。
//   - ids：该映射对每个 pattern 产生的 32 位 ID（排列成 0..31）。
//   - taps：这 5 行的总 tap 权重。
struct Best {
  std::array<Key, kRows> rows{};
  std::array<unsigned, kKeys> ids{};
  unsigned taps = std::numeric_limits<unsigned>::max();
};

// 含义：每个 cap 阶段的搜索记账信息。
//   - cap：本阶段的目标 tap 权重上限。
//   - candidates：本阶段候选池（pool）的大小。
//   - nodes：本阶段 DFS 遍历的节点数。
//   - complete：本阶段是否在预算内完成。
//   - improvements：本阶段内发现的（总 tap）改进值集合。
struct Stage {
  unsigned cap = 0;
  std::size_t candidates = 0;
  std::uint64_t nodes = 0;
  bool complete = false;
  std::vector<unsigned> improvements;
};

// 含义：表示某个阶段或整个运行在正常完成前被什么原因中断。
//   - None：未被中断。
//   - Time：达到时间预算而中断。
//   - CandidateLimit：达到候选数量上限而中断。
enum class StopReason { None, Time, CandidateLimit };

// 含义：一次搜索运行的命令行配置。
//   - path：codebook 文件路径。
//   - seconds：总时间预算（秒）。
//   - max_cap：允许的最大 tap 权重上限。
//   - max_candidates：候选数量的上限。
struct Options {
  std::string path;
  double seconds = 300.0;
  unsigned max_cap = 5;
  std::size_t max_candidates = 1500000;
};

// 功能：生成命令行的用法说明字符串。
// 输入：无。
// 输出/返回：返回用法说明字符串。
std::string usage() {
  return "usage: search_linear_mphf CODEBOOK.txt [--seconds N] "
         "[--max-cap N] [--max-candidates N]";
}

// 功能：解析一个十进制无符号整数；若文本带有尾随垃圾字符则抛出异常。
// 输入：
//   - text：待解析的字符串（必须全部为数字）。
//   - option：选项名（用于错误信息）。
// 输出/返回：解析得到的数值。
std::uint64_t parse_uint(const std::string& text, const char* option) {
  std::size_t used = 0;
  const auto value = std::stoull(text, &used, 10);
  if (used != text.size()) throw std::runtime_error(std::string("bad ") + option);
  return value;
}

// 功能：把 argv 解析成一个 Options 结构体。第一个位置参数是 codebook 路径。
// 输入：
//   - argc：参数个数。
//   - argv：命令行参数数组。
// 输出/返回：填充完成的 Options 结构体；遇到未知选项或非法取值时抛出
//            std::runtime_error。
Options parse_options(int argc, char** argv) {
  if (argc < 2) throw std::runtime_error(usage());
  Options options;
  options.path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--seconds" && i + 1 < argc) {
      options.seconds = std::stod(argv[++i]);
      if (!(options.seconds >= 0.0) || !std::isfinite(options.seconds))
        throw std::runtime_error("--seconds must be finite and nonnegative");
    } else if (arg == "--max-cap" && i + 1 < argc) {
      options.max_cap = static_cast<unsigned>(parse_uint(argv[++i], "--max-cap"));
      if (options.max_cap == 0 || options.max_cap > 64)
        throw std::runtime_error("--max-cap must be in 1..64");
    } else if (arg == "--max-candidates" && i + 1 < argc) {
      options.max_candidates = static_cast<std::size_t>(
          parse_uint(argv[++i], "--max-candidates"));
      if (options.max_candidates == 0)
        throw std::runtime_error("--max-candidates must be positive");
    } else {
      throw std::runtime_error("unknown option or missing value: " + arg);
    }
  }
  return options;
}

// 功能：从一个文本 codebook 文件中读取 32 个互不相同的 64 位 pattern。
// 输入：
//   - path：codebook 文件路径。
// 输出/返回：按文件顺序返回这 32 个 Key；任何畸形行、数量错误或重复 key 都会
//            抛出异常。
std::array<Key, kKeys> read_keys(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open codebook: " + path);
  std::array<Key, kKeys> keys{};
  unsigned count = 0;
  std::string line;
  while (std::getline(input, line)) {
    std::string hex;
    for (const unsigned char c : line) {
      if (std::isxdigit(c)) {
        hex.push_back(static_cast<char>(c));
      } else if (std::isspace(c) || c == '|') {
        continue;
      } else {
        throw std::runtime_error(
            "each nonempty line must be | 16 hexadecimal digits |");
      }
    }
    if (hex.empty()) continue;
    if (hex.size() != 16 || count == kKeys)
      throw std::runtime_error("expected exactly 32 lines of 16 hex digits");
    Key value = 0;
    for (const unsigned char c : hex) {
      value <<= 4;
      value |= c <= '9' ? c - '0' : (std::tolower(c) - 'a' + 10);
    }
    keys[count++] = value;
  }
  if (count != kKeys) throw std::runtime_error("expected exactly 32 keys");
  for (unsigned i = 0; i < kKeys; ++i)
    for (unsigned j = i + 1; j < kKeys; ++j)
      if (keys[i] == keys[j]) throw std::runtime_error("duplicate key in codebook");
  return keys;
}

class Searcher {
 public:
  // 功能：构造列签名：columns_[bit] 的第 i 位为 1 当且仅当 pattern[i] 的第
  // 'bit' 位为 1，因此对列做异或即可复现某行在 codebook 上的行为。
  // 输入：
  //   - keys：全部 32 个 pattern 的 Key 数组。
  //   - options：本次搜索运行的配置。
  // 输出/返回：无；仅初始化成员 columns_ 等。
  Searcher(const std::array<Key, kKeys>& keys, const Options& options)
      : keys_(keys), options_(options), deadline_(Clock::now()) {
    for (unsigned bit = 0; bit < 64; ++bit) {
      Signature column = 0;
      for (unsigned i = 0; i < kKeys; ++i)
        column |= static_cast<Signature>(((keys_[i] >> bit) & 1ULL) << i);
      columns_[bit] = column;
    }
  }

  // 功能：驱动整个搜索过程：对不超过 max_cap 的每个 cap，枚举新权重的候选，
  // 重建 pool，运行一个 DFS 阶段，并记录一个 Stage。
  // 输入：无。
  // 输出/返回：以字符串返回本次运行的 JSON 报告；当时间/候选限制触发或找到
  //            5-tap（最优）映射时提前停止。
  std::string run() {
    start_ = Clock::now();
    deadline_ = start_ + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double>(options_.seconds));
    for (unsigned cap = 1; cap <= options_.max_cap; ++cap) {
      if (stop_ != StopReason::None) break;
      const auto stage_begin = Clock::now();
      enumerate_weight(cap, 0, cap, 0, 0);
      if (stop_ != StopReason::None) break;
      generated_cap_ = cap;
      rebuild_pool();
      const auto old_nodes = nodes_;
      const auto old_improvements = improvements_.size();
      const bool complete = search_stage(cap);
      Stage stage;
      stage.cap = cap;
      stage.candidates = pool_.size();
      stage.nodes = nodes_ - old_nodes;
      stage.complete = complete;
      for (std::size_t i = old_improvements; i < improvements_.size(); ++i)
        stage.improvements.push_back(improvements_[i]);
      stages_.push_back(stage);
      std::cerr << "cap=" << cap << " candidates=" << pool_.size()
                << " nodes=" << stage.nodes
                << " complete=" << (complete ? "true" : "false")
                << " best=" << (best_ ? std::to_string(best_->taps) : "none")
                << " elapsed_ms="
                << std::chrono::duration<double, std::milli>(Clock::now() - stage_begin).count()
                << '\n';
      if (complete) proved_cap_ = cap;
      if (stop_ != StopReason::None) break;
      // Five taps is the absolute lower bound for five nonzero output rows.
      if (best_ && best_->taps == kRows) {
        status_ = "optimal";
        break;
      }
    }
    if (status_.empty()) {
      if (stop_ == StopReason::Time) status_ = "time_limit";
      else if (stop_ == StopReason::CandidateLimit) status_ = "candidate_limit";
      else if (best_) status_ = "optimal_in_pool";
      else status_ = "no_linear_map_in_pool";
    }
    return to_json();
  }

 private:
  using Clock = std::chrono::steady_clock;

  const std::array<Key, kKeys>& keys_;
  const Options& options_;
  std::array<Signature, 64> columns_{};
  std::unordered_map<Signature, Candidate> by_signature_;
  std::vector<Candidate> pool_;
  std::array<Key, kRows> chosen_{};
  std::optional<Best> best_;
  std::vector<unsigned> improvements_;
  std::vector<Stage> stages_;
  Clock::time_point start_;
  Clock::time_point deadline_;
  std::uint64_t enumerated_ = 0;
  std::uint64_t nodes_ = 0;
  std::uint64_t scanned_ = 0;
  unsigned generated_cap_ = 0;
  unsigned proved_cap_ = 0;
  unsigned stage_cap_ = 0;
  StopReason stop_ = StopReason::None;
  std::string status_;

  // 功能：协作式的时间预算检查；每 1024 次迭代采样一次。
  // 输入：无；依赖成员 enumerated_ 与 deadline_、stop_。
  // 输出/返回：一旦超过期限，将 stop_ 置为 Time；调用方收到后应尽快返回。
  void check_deadline() {
    if ((enumerated_ & 0x3ff) == 0 && Clock::now() >= deadline_)
      stop_ = StopReason::Time;
  }

  // 功能：递归枚举所有恰好含有 'target' 个 tap 的输入 mask，并把它们的
  // signature 插入 by_signature_。'left' 表示还需放置的 bit 数，'start' 是
  // 下一个 bit 下标，随着 bit 被选中逐步累积出 (mask, signature)。
  // 输入：
  //   - target：目标 tap 权重。
  //   - start：下一个候选 bit 的最小下标。
  //   - left：还需放置的 bit 个数。
  //   - mask：当前已累积的 64 位输入 mask。
  //   - signature：当前已累积的 32 位 signature。
  // 输出/返回：仅保留 16/16 平衡的 signature，按 signature 去重并保留第一个
  //            （权重最低的）mask；结果以副作用写入 by_signature_。
  void enumerate_weight(unsigned target, unsigned start, unsigned left,
                        Key mask, Signature signature) {
    if (stop_ != StopReason::None) return;
    check_deadline();
    if (stop_ != StopReason::None) return;
    if (left == 0) {
      ++enumerated_;
      if (popcount32(signature) == 16) {
        const auto found = by_signature_.find(signature);
        if (found == by_signature_.end()) {
          if (by_signature_.size() >= options_.max_candidates) {
            stop_ = StopReason::CandidateLimit;
            return;
          }
          by_signature_.emplace(signature, Candidate{target, signature, mask});
        }
      }
      return;
    }
    for (unsigned bit = start; bit <= 64 - left; ++bit) {
      enumerate_weight(target, bit + 1, left - 1, mask | (Key{1} << bit),
                       signature ^ columns_[bit]);
      if (stop_ != StopReason::None) return;
    }
  }

  // 功能：根据当前 by_signature_ 中的条目重建 pool_，并按 (weight, signature,
  // mask) 排序。
  // 输入：无；依赖成员 by_signature_。
  // 输出/返回：无返回值；结果为 pool_（即后续 DFS 的输入）。
  void rebuild_pool() {
    pool_.clear();
    pool_.reserve(by_signature_.size());
    for (const auto& entry : by_signature_) pool_.push_back(entry.second);
    std::sort(pool_.begin(), pool_.end(), [](const Candidate& a, const Candidate& b) {
      if (a.weight != b.weight) return a.weight < b.weight;
      if (a.signature != b.signature) return a.signature < b.signature;
      return a.mask < b.mask;
    });
    generated_cap_ = std::max(generated_cap_, pool_.empty() ? 0U : pool_.back().weight);
  }

  // 功能：分支定界剪枝。之前的 cap 已完整搜索，因此当前阶段的新解必须至少
  // 包含一行权重为 stage_cap_ 的候选；候选按权重递增，最后一行必为此权重。
  // 下界为最便宜的 remaining-1 行，加上一行 stage_cap_。
  // 输入：
  //   - cost：已固定行的累计 tap 权重。
  //   - remaining：仍需放置的行数。
  //   - pool：候选的下标池。
  //   - begin：起始偏移量。
  // 输出/返回：true 表示可以剪枝（无法超越 incumbent 或候选不足）。
  bool improves_bound(unsigned cost, unsigned remaining, const std::vector<int>& pool,
                      std::size_t begin) const {
    if (pool.size() - begin < remaining) return true;
    if (pool_[pool.back()].weight != stage_cap_) return true;
    if (!best_) return false;
    unsigned lower = cost + stage_cap_;
    for (unsigned i = 0; i + 1 < remaining; ++i)
      lower += pool_[pool[begin + i]].weight;
    return lower >= best_->taps;
  }

  // 功能：测试 'signature' 是否把每个 group 一分为二。成功且 halves 非空时，
  // 把产生的子 group 写入 *halves。
  // 输入：
  //   - groups：当前的若干 sample partition（每组为一个 bitmask）。
  //   - signature：用于划分的 32 位 signature。
  //   - halves：可空的输出指针，用于接收划分后的子 group。
  // 输出/返回：若某个 group 未通过 popcount(ones)*2 == popcount(group) 检查
  //            则返回 false，此时 *halves 中的内容不可用；全部通过则返回 true。
  bool splits(const std::vector<std::uint32_t>& groups, Signature signature,
              std::vector<std::uint32_t>* halves) const {
    // 每层的 group 等大：32、16、8、4、2。无需为每个 group 重算大小。
    const unsigned half_size = kKeys / (groups.size() * 2);
    for (const auto group : groups) {
      const auto ones = group & signature;
      if (popcount32(ones) != half_size) return false;
    }
    if (halves) {
      halves->clear();
      halves->reserve(groups.size() * 2);
      for (const auto group : groups) {
        const auto ones = group & signature;
        halves->push_back(ones);
        halves->push_back(group ^ ones);
      }
    }
    return true;
  }

  // 功能：在当前的 pool_ 上，从单一 32-pattern group 出发运行一个 DFS 阶段。
  // 输入：cap 是当前已完整生成的最大行权重；另依赖成员 pool_、deadline_ 等。
  // 输出/返回：若阶段在预算内完成则返回 true；若时限在搜索树耗尽前到达则
  //            返回 false。
  bool search_stage(unsigned cap) {
    if (Clock::now() >= deadline_) {
      stop_ = StopReason::Time;
      return false;
    }
    stage_cap_ = cap;
    const std::vector<int> all = indices(0, pool_.size());
    const std::vector<std::uint32_t> groups{0xffffffffU};
    dfs(all, groups, 0, 0);
    return stop_ == StopReason::None;
  }

  // 功能：构造一个由整数 begin, begin+1, ..., end-1 组成的向量（一个候选下标
  // 切片，用于保证所选行按严格递增的顺序选取）。
  // 输入：
  //   - begin：切片起始下标（含）。
  //   - end：切片结束下标（不含）。
  // 输出/返回：返回 [begin, end) 内整数序列构成的 vector<int>。
  static std::vector<int> indices(std::size_t begin, std::size_t end) {
    std::vector<int> result;
    result.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) result.push_back(static_cast<int>(i));
    return result;
  }

  // 功能：在固定的候选池中深度优先搜索 5 行。'pool' 保存与每个已选前缀均
  // 兼容的 pool_ 下标（按递增顺序）；'groups' 是当前的 sample partition；
  // 'depth' 是已固定行数；'cost' 是它们合并后的 tap 权重。
  // 输入：
  //   - pool：兼容当前所选前缀的候选下标集合。
  //   - groups：当前的划分分组。
  //   - depth：已固定的行数。
  //   - cost：已固定行的累计 tap 权重。
  // 输出/返回：副作用：填充 chosen_，通过 save_best 更新 best_/improvements_，
  //            并在到达时限时设置 stop_。绝不修改 'groups'。
  void dfs(const std::vector<int>& pool, const std::vector<std::uint32_t>& groups,
           unsigned depth, unsigned cost) {
    if (stop_ != StopReason::None) return;
    if ((nodes_++ & 0x3ff) == 0 && Clock::now() >= deadline_) {
      stop_ = StopReason::Time;
      return;
    }
    const unsigned remaining = kRows - depth;
    if (pool.size() < remaining || improves_bound(cost, remaining, pool, 0)) return;

    for (std::size_t j = 0; j < pool.size(); ++j) {
      if (stop_ != StopReason::None) return;
      if (pool.size() - j < remaining) break;
      if (improves_bound(cost, remaining, pool, j)) break;
      const Candidate& candidate = pool_[pool[j]];
      if (remaining == 1 && candidate.weight != stage_cap_) continue;
      std::vector<std::uint32_t> halves;
      if (!splits(groups, candidate.signature, &halves)) continue;
      chosen_[depth] = candidate.mask;
      if (remaining == 1) {
        save_best(cost + candidate.weight);
        continue;
      }

      if (remaining == 2) {
        // 最后一行只能来自本阶段新加入的权重层；直接检查，避免构造 next、
        // 递归以及再次检查同一签名的组划分。
        for (std::size_t k = j + 1; k < pool.size(); ++k) {
          if ((++scanned_ & 0x3ff) == 0 && Clock::now() >= deadline_) {
            stop_ = StopReason::Time;
            return;
          }
          const Candidate& last = pool_[pool[k]];
          if (last.weight != stage_cap_) continue;
          if (best_ && cost + candidate.weight + last.weight >= best_->taps) break;
          if (splits(halves, last.signature, nullptr)) {
            chosen_[depth + 1] = last.mask;
            save_best(cost + candidate.weight + last.weight);
          }
        }
        continue;
      }

      std::vector<int> next;
      next.reserve(pool.size() - j - 1);
      for (std::size_t k = j + 1; k < pool.size(); ++k) {
        if ((++scanned_ & 0x3ff) == 0 && Clock::now() >= deadline_) {
          stop_ = StopReason::Time;
          return;
        }
        const Candidate& next_candidate = pool_[pool[k]];
        // 下一个候选可能就是最后一行。其余剩余行仍然可以取当前权重，因此即使
        // next_candidate 更重，这个下界也仍然是安全的。
        if (best_ && cost + candidate.weight + next_candidate.weight +
                         candidate.weight * (remaining - 2) >= best_->taps)
          break;
        if (splits(halves, next_candidate.signature, nullptr))
          next.push_back(pool[k]);
      }
      if (next.size() >= remaining - 1)
        dfs(next, halves, depth + 1, cost + candidate.weight);
    }
  }

  // 功能：记录一个已完成的 5 行映射（chosen_）及其给定的总 tap 权重。
  // 从 keys 算出全部 32 个 ID，仅当每个 ID 0..31 都恰好出现一次、且 taps 优于
  // 当前 incumbent 时才保留，并把新的 tap 数压入 improvements_。
  // 输入：
  //   - taps：本映射的总 tap 权重。
  // 输出/返回：无返回值；通过更新成员 best_ 产生结果。
  void save_best(unsigned taps) {
    if (best_ && taps >= best_->taps) return;
    Best result;
    result.rows = chosen_;
    result.taps = taps;
    std::array<bool, kKeys> seen{};
    for (unsigned i = 0; i < kKeys; ++i) {
      unsigned id = 0;
      for (unsigned row = 0; row < kRows; ++row)
        id |= static_cast<unsigned>(popcount64(keys_[i] & result.rows[row]) & 1U) << row;
      result.ids[i] = id;
      seen[id] = true;
    }
    for (bool value : seen)
      if (!value) return;
    best_ = result;
    improvements_.push_back(taps);
  }

  // 功能：将一个 64 位值格式化为 16 位、零填充的 "0x" 十六进制字符串。
  // 输入：
  //   - value：待格式化的 64 位 Key。
  // 输出/返回：格式化后的十六进制字符串。
  static std::string hex(Key value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
  }

  // 功能：对字符串做转义以便嵌入 JSON，并用双引号包裹。
  // 输入：
  //   - value：待转义的原始字符串。
  // 输出/返回：转义并带双引号的字符串。
  static std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : value) {
      if (c == '"' || c == '\\') out << '\\';
      out << c;
    }
    out << '"';
    return out.str();
  }

  // 功能：将整个运行过程（status、best 映射、stages）序列化为 JSON 字符串。
  // 输入：无；读取成员 status_、best_、stages_ 等。
  // 输出/返回：返回 JSON 报告字符串；纯只读，不修改任何状态。
  std::string to_json() const {
    std::ostringstream out;
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start_).count();
    out << "{\"status\":" << json_string(status_)
        << ",\"time_budget_seconds\":" << options_.seconds
        << ",\"max_cap\":" << options_.max_cap
        << ",\"max_candidates\":" << options_.max_candidates
        << ",\"generated_cap\":" << generated_cap_
        << ",\"proved_cap\":" << proved_cap_
        << ",\"candidates\":" << by_signature_.size()
        << ",\"enumerated_masks\":" << enumerated_
        << ",\"nodes\":" << nodes_
        << ",\"elapsed_seconds\":" << elapsed
        << ",\"proof_scope\":\"independent XOR rows; completed candidate layers only\""
        << ",\"best\":";
    if (!best_) {
      out << "null";
    } else {
      out << "{\"rows\":[";
      for (unsigned row = 0; row < kRows; ++row) {
        if (row) out << ',';
        out << json_string(hex(best_->rows[row]));
      }
      out << "] ,\"row_taps\":[";
      for (unsigned row = 0; row < kRows; ++row) {
        if (row) out << ',';
        out << popcount64(best_->rows[row]);
      }
      out << "] ,\"taps\":" << best_->taps
          << ",\"independent_xors\":" << (best_->taps - kRows)
          << ",\"ids\":[";
      for (unsigned i = 0; i < kKeys; ++i) {
        if (i) out << ',';
        out << best_->ids[i];
      }
      out << "]}";
    }
    out << ",\"stages\":[";
    for (std::size_t i = 0; i < stages_.size(); ++i) {
      if (i) out << ',';
      const auto& stage = stages_[i];
      out << "{\"cap\":" << stage.cap
          << ",\"candidates\":" << stage.candidates
          << ",\"nodes\":" << stage.nodes
          << ",\"complete\":" << (stage.complete ? "true" : "false")
          << ",\"improvements\":[";
      for (std::size_t j = 0; j < stage.improvements.size(); ++j) {
        if (j) out << ',';
        out << stage.improvements[j];
      }
      out << "]}";
    }
    out << "]}";
    return out.str();
  }
};

}  // namespace

// 功能：程序入口：解析选项，加载 codebook，运行 Searcher 并打印其 JSON 报告。
  // 输入：
  //   - argc：参数个数。
  //   - argv：命令行参数数组。
  // 输出/返回：成功时返回 0（即使只得到部分结果）；任何输入错误返回 2。
int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    const auto keys = read_keys(options.path);
    Searcher searcher(keys, options);
    std::cout << searcher.run() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n' << usage() << '\n';
    return 2;
  }
}
