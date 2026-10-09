# 1～1024 项稀疏线性映射原型

本次实现是一个实验原型，目标是验证当前 32 项搜索方法如何扩展到
`N <= 1024`，并支持非 2 的幂次码本。程序位于
`tools/search_linear_mphf_upto1024.cpp`，输入仍是每行一个 64-bit pattern：

```text
| 0123456789abcdef |
```

编译与运行：

```sh
c++ -O3 -std=c++17 tools/search_linear_mphf_upto1024.cpp \
  -o /tmp/search_linear_mphf_upto1024
/tmp/search_linear_mphf_upto1024 codebook.txt \
  --seconds 30 --max-cap 3 --beam-width 32
```

`K=ceil(log2(N))` 个输出位用于生成 `0..2^K-1` 的 offset。当前原型用 64-bit
输入 mask 生成候选行，签名保存为最多 1024-bit；搜索阶段采用带容量约束的
beam search。它会逐项复算所有输出并检查无碰撞。`not_found_within_beam` 和
`candidate_limit` 都表示搜索预算或候选池不足，不能解释为不存在矩阵。

对于第 `d` 层，每个当前分组最多容纳：

```text
capacity = 2^(K-d-1)
```

候选行 `s` 将分组 `G` 分成 `ones=popcount(G&s)` 和 `zeros=|G|-ones`，要求
`ones <= capacity` 且 `zeros <= capacity`。当 `N=2^K` 时，这个不等式退化为
原 32 项算法使用的严格均分；当 `N<2^K` 时，剩余叶子就是空 offset。

原型还计算 pattern 差分的 GF(2) 仿射秩。如果差分秩不超过输出位数，直接从
仿射张成空间中选投影行，避免候选搜索。这个快捷路径只说明找到了一个可行映射，
不说明共享 XOR 电路最小。

## 可控结构化输入

用 `i` 的低 10 位作为可直接读取的索引，高 54 位填入固定种子的随机值：

| N | K | 空 offset | 结果 | taps | 独立 XOR | 实际耗时 |
|---:|---:|---:|---|---:|---:|---:|
| 1000 | 10 | 24 | `optimal` | 10 | 0 | 约 8.7 ms |
| 1024 | 10 | 0 | `optimal` | 10 | 0 | 约 0.44 ms |

两组都找到低位直通矩阵：`o0=X0, o1=X1, ..., o9=X9`。每行至少需要一个
tap，而 10 个输出位必须都有效，所以 10 taps 是这个实例的绝对下界。1000 项
码本使用 1024 个 offset 中的 1000 个，留下 24 个空槽。

## Custom-FPC 合成输入

`tools/generate_prefix_codebook_upto1024.py` 复用了现有
`experiment_mphf_128.py` 的 Custom-FPC 4-bit tag 分类规则，允许生成最多 1024
项合成头部：

```sh
python3 tools/generate_prefix_codebook_upto1024.py \
  --output-dir results/mphf-1024/custom-fpc-1000 \
  --entries 1000 --seed 270537764
python3 tools/generate_prefix_codebook_upto1024.py \
  --output-dir results/mphf-1024/custom-fpc-1024 \
  --entries 1024 --seed 270537764
```

在 `--max-cap 4 --beam-width 32 --max-candidates 100000 --seconds 10` 下：

| N | cap=3 候选 | cap=4 候选 | 状态 | 说明 |
|---:|---:|---:|---|---|
| 1000 | 15463 | 达到 100000 上限 | `candidate_limit` | 没有找到映射 |
| 1024 | 629 | 13969 | `not_found_within_beam` | 没有找到映射 |

1000 项在进入完整 cap=4 搜索前就达到候选上限；1024 项完成了 cap=4 的候选
生成和 beam 搜索，但最终只到第 3 层，未找到 10 行映射。这不是不可行性证明。
它说明直接把 32 项 DFS 扩展到 1024 项后，候选生成和搜索树都会成为主要瓶颈。

## 现阶段结论

原型已经验证了三个接口性质：

1. `N<2^K` 可以直接使用 K-bit offset，空叶子不需要添加真实 pattern；
2. `N=1024` 可以使用 10-bit 输出，容量约束自动退化为严格均分；
3. 只要返回 `best`，程序会对全部输入逐项验证，避免把启发式搜索结果误报为合法映射。

下一步若要处理真正的 512～1024 项 Custom-FPC 码本，需要把候选签名去重从完整
1024-bit 签名改成码本列空间的 GF(2) 坐标，并加入更强的候选排序、随机重启或
meet-in-the-middle。需要严格最优性时，应在找到的候选子集上再运行精确 DFS 或
SAT/ILP；当前 beam 原型只能提供可行解和搜索边界。
