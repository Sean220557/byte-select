# Custom-FPC 64 项 MPHF 实验报告

日期：2026-09-04

## 1. 实验说明

本实验将前一份 128 项实验缩小为 64 项码表，输入仍来自同一套
Custom-FPC 4-bit tag 规则和种子 `0xC0570F0C`。每个 prefix 是 16 个 4-bit tag，
即 8B；目标是将 64 个不同 prefix 映射到 `0..63`，因此 ID 需要 6 bit。

生成器、C++ 搜索器和 CMPH adapter 与 128 项实验共用：

- [codebook.bin](/Users/leyang/Projects/byte-select/results/mphf-64/codebook.bin)：64 x 8B 输入码表；
- [codebook.json](/Users/leyang/Projects/byte-select/results/mphf-64/codebook.json)：tag 和 packed key；
- [experiment.json](/Users/leyang/Projects/byte-select/results/mphf-64/experiment.json)：Python/CMPH 结果；
- [cpp-search.json](/Users/leyang/Projects/byte-select/results/mphf-64/cpp-search.json)：C++ 结果；
- [seed-width-16.json](/Users/leyang/Projects/byte-select/results/mphf-64/seed-width-16.json)：16-bit seed 搜索结果；
- [seed-width-24.json](/Users/leyang/Projects/byte-select/results/mphf-64/seed-width-24.json)：24-bit seed 搜索结果；
- [seed-width-32.json](/Users/leyang/Projects/byte-select/results/mphf-64/seed-width-32.json)：32-bit seed 搜索结果；
- [experiment_mphf_128.py](/Users/leyang/Projects/byte-select/tools/experiment_mphf_128.py)：生成与实验脚本；
- [search_mphf128.cpp](/Users/leyang/Projects/byte-select/tools/search_mphf128.cpp)：C++ 搜索器（通过 `-DMPHF_N=64` 编译）。

这仍然是结构实验，不代表真实 trace 的高频分布。实际输入应替换为训练得到的
64 个 prefix 后重新构造 MPHF。

### 存储量口径

64 项反向码表大小为：

```text
64 x 8B = 512B
```

MPHF 只替代“输入 prefix -> ID”的查找结构；如果需要 `ID -> prefix` 或精确
membership 校验，512B 反向码表仍然需要保留。

## 2. 单乘法和双乘法公式

单公式采用：

```text
id = (((key ^ (key >> shift)) * odd_multiplier) >> 58) & 0x3f
```

双乘法采用：

```text
z  = ((key ^ (key >> shift)) * m1) ^ rotl(key * m2, rotate)
id = (z >> 58) & 0x3f
```

每种结构搜索 100,000,000 组随机参数：

| 结构 | 尝试次数 | 是否成功 | 最好唯一输出 | 搜索时间 |
|---|---:|---|---:|---:|
| 单乘法/移位 | 100,000,000 | 否 | 54/64 | 19.85 s |
| 双乘法 + XOR + rotate | 100,000,000 | 否 | 55/64 | 21.47 s |

若公式表现近似随机函数，任意一组参数对 64 个 key 无碰撞的概率是：

```text
64! / 64^64 ≈ 3.22 x 10^-27
```

即使搜索 1 亿组，期望成功概率也只有约 `3.22 x 10^-19`。因此增大搜索次数
不能将这类公式变成可靠的 MPHF 构造器；同时通用 64-bit 乘法器也不利于低功耗
硬件实现。

## 3. 两级 displacement MPHF

使用与 128 项实验相同的结构：

```text
bucket = h1(key) & (B - 1)
id     = (h2(key) + displacement[bucket]) & 63
```

`h1/h2` 使用常量加法、移位、rotate 和 XOR，不使用通用乘法器。离线构造时，先
按 `h1` 将 key 分到 bucket，再为每个 bucket 搜索一个共享 displacement，使该
bucket 中所有 key 的候选位置落到尚未占用的不同槽位。运行时只需计算两个 hash、
读取 displacement、做一次 6-bit 加法。

### 3.1 搜索结果

每个 bucket 数最多搜索 100,000,000 组 seed：

| bucket 数 B | 结果 | 成功尝试次数 | 最大 bucket | 最大 displacement | 表项位宽 | bit-packed 参数 | byte 对齐参数 |
|---:|---|---:|---:|---:|---:|---:|---:|
| 16 | 成功 | 361,105 | 9 | 60 | 6 bit | 28B | 32B |
| 32 | 成功 | 3 | 5 | 56 | 6 bit | 40B | 48B |
| 64 | 成功 | 1 | 3 | 59 | 6 bit | 64B | 80B |
| 128 | 成功 | 2 | 3 | 44 | 6 bit | 112B | 144B |

参数大小包含两个 64-bit seed（16B）。16 bucket 的成功实例最省：

```text
seed1/seed2             16B
16 x 6-bit displacement 12B packed
合计                    28B packed
```

如果每个 displacement 按一个 byte 存储，则为 `16B + 16B = 32B`，实现更简单，
但多出 4B。对应运行时路径为：`h1` 取低 4 bit 作为 16-entry 表索引，`h2` 取低
6 bit 得到初始位置，读取 6-bit displacement，做 6-bit 加法并取低 6 bit。

### 3.2 seed 位宽压缩实验

seed 是构造器的参数，不一定必须使用完整 64 bit。可以在离线搜索时限制 seed 的
取值范围，例如只允许低 16 bit 非零，其余高位固定为 0；只要在这个受限空间内仍能
找到无碰撞解，运行时就只需保存较短的 seed。对当前 64 项码表、16 bucket 结构的
实验结果如下（每种位宽最多搜索 1,000,000 组 seed 对）：

| 每个 seed 位宽 | 是否成功 | 成功尝试次数 | 最大 displacement | MPHF 参数（packed） | byte 对齐参数 |
|---:|---|---:|---:|---:|---:|
| 64 bit | 是 | 361,105 | 60 | 28B | 32B |
| 32 bit | 是 | 490,193 | 58 | 20B | 24B |
| 24 bit | 是 | 626,418 | 47 | 18B | 22B |
| 16 bit | 是 | 598,783 | 57 | **16B** | **20B** |
| 8 bit | 否（1M 内） | — | — | 14B（理论） | 18B（理论） |

16-bit seed 的参数计算为：

```text
seed1/seed2              2 x 16 bit = 4B
16 x 6-bit displacement  96 bit     = 12B
合计                                  16B packed
```

这里的成功只对当前合成码表成立；换一份真实训练码表，需要重新搜索和验证。不能
直接把已有 64-bit seed 截断为 16 bit，因为 hash 输出会改变，原 displacement 通常
也不再成立。正确做法是限制 seed 搜索空间，找到一组新 seed 和与之匹配的
displacement，再对全部 key 做完整验证。

进一步还可以尝试只保存一个 seed，例如令 `seed2 = seed1 xor 常量`，或者用固定的
domain-separation 常量派生两路 hash；这样可能把 seed 存储再减半，但会减少构造自由度，
必须针对真实码表单独搜索，当前实验尚未采用这种约束。

如果 seed 和 displacement 在综合时完全固定，也可以把它们写成 RTL 常量，避免独立
的可寻址 ROM 位；这并不意味着信息消失，只是把存储转化为常量连线/门级逻辑。若码表
需要运行时更新，则应保留可加载的 seed 和 displacement 寄存器或小 ROM。

### 3.3 硬件路径

```text
x1     = mix(key ^ seed1)
bucket = x1[3:0]

x2   = mix(key ^ seed2)
base = x2[5:0]

d  = displacement_rom[bucket]   // 16 x 6 bit
id = (base + d)[5:0]
```

两个 mixer 可以并行实现单周期吞吐，也可以复用一套 mixer 分两个周期计算，以节省
面积和动态功耗。若输入不保证属于码表，MPHF 对任意 64-bit key 都会给出一个 6-bit
ID，必须再读取 `codebook[id]`（512B ROM）并做一次 64-bit equality compare，才能
区分 hit 和 miss。

## 4. nibble 特征索引

对 16 个 tag 位置穷举选择：

| 选取 nibble 数 | 位置 | 不同 bucket | 最大候选数 | 命中平均候选数 | singleton bucket |
|---:|---|---:|---:|---:|---:|
| 1 | 10 | 12 | 15 | 8.72 | 0 |
| 2 | 9, 14 | 45 | 4 | 1.94 | 34 |
| 3 | 1, 11, 13 | 63 | 2 | 1.03 | 62 |
| 4 | 0, 1, 4, 12 | 64 | 1 | 1.00 | 64 |

4-nibble 特征在这份合成码表上完全唯一，但直接构造 16-bit 全空间表会浪费空间。
实际可以用 8-bit 一级索引加短候选列表，再做精确比较；其功耗是否优于 displacement
需要在真实码表和目标工艺下综合验证。

## 5. CMPH 结果

使用 CMPH revision `312005b`，三种算法都对 64 个 key 构造成功：

| 算法 | packed MPHF 大小 | bits/key | 运行时结构概括 |
|---|---:|---:|---|
| FCH | 140B | 17.50 | 两个 hash、`g` 数据和参数 |
| BDZ | 46B | 5.75 | 三个 hash、`g` 表、rank table |
| CHD | 132B | 16.50 | bucket、displacement、compressed rank |

在该实例中 CMPH-BDZ 的序列化描述最小，但它的运行时并非“一个 hash + 一次查表”：
源码路径需要 3 个 hash、`g` 查询和 rank 计算。CHD 还需要 compressed rank/sequence
解码；FCH 虽然结构直观，但序列化开销较大。因此 CMPH 更适合作为离线构造器和正确性
基线，而不是直接将默认 packed blob 搬到 RTL。

## 6. 横向比较与建议

| 方案 | MPHF/索引辅助参数 | hash/计算 | 对 64 项实验的评价 |
|---|---:|---|---|
| 16-bucket 定制 displacement | 16B packed（16-bit seed）/ 20B byte 对齐 | 2 个无乘法 mixer + 1 次小 ROM + 6-bit 加法 | **最适合硬件** |
| CMPH-BDZ | 46B | 3 个 hash + `g` + rank | 空间基线，控制逻辑较复杂 |
| 单/双乘法公式 | 0B（若偶然成功） | 1/2 个通用乘法器 | 1 亿次均未成功，不可依赖 |
| 4-nibble + 候选表 | 取决于候选布局 | 少量比较 | 值得用真实码本继续评估 |

推荐优先使用 16-bucket 定制 displacement：在当前码表上可以把两个 seed 限制为 16 bit，
因此参数为 16B packed；若按 byte 对齐 displacement 存储则为 4B seed + 16B 表 = 20B。
若为了简单沿用 64-bit seed，则是 28B packed / 32B byte 对齐。与原来的 64 路 64-bit 并行 comparator 相比，
它将匹配路径改为两个轻量 hash、一个小 ROM 和一次比较。若要求零误判，仍需保留
512B 反向码表做最终验证。

### 端到端存储

| 用途 | 原并行匹配 | 16-bucket MPHF |
|---|---:|---:|
| `ID -> prefix` / 精确验证 ROM | 512B | 512B |
| 查找辅助参数 | 0B | 16--20B（16-bit seed）；或 28--32B（64-bit seed） |
| 总信息存储 | 512B | 528--532B（16-bit seed）；或 540--544B（64-bit seed） |

MPHF 不会减少保存任意 64 个 64-bit pattern 所需的信息量；它减少的是查找时的比较
阵列和切换活动。只有 prefix 本身存在可共享的生成规则、差分结构或更短 fingerprint
时，才能进一步压缩反向码表。

## 7. 当前 upto1024 稀疏 GF(2) 搜索

本节使用新的 `tools/search_linear_mphf_upto1024.cpp`，输入为本报告原有的
`codebook.json` 中 64 个 `key_hex`。为方便复现，已导出为
[codebook.txt](/Users/leyang/Projects/byte-select/results/mphf-64/codebook.txt)。
搜索器的输出位数默认为 `ceil(log2(64))=6`，也可以通过 `--extra-bits` 留出空
offset。结果 JSON 保存在：

```text
linear-search-6bit-60s.json
linear-search-7bit-120s.json
linear-search-8bit-60s.json
```

### 7.1 结果

| 输出位数 | offset 空间 | 搜索预算 | 结果 | 最好解 | 备注 |
|---:|---:|---|---|---|---|
| 6 | 64 | 60 s，cap≤6，beam=256 | `time_limit` | 无 | 821,297 个候选；cap=5 时最深到第 3 层 |
| 7 | 128 | 120 s，cap≤5，beam=256 | `candidate_limit` | 9 taps，2 次 XOR | 候选达到 1,000,000；64 个有效 offset、64 个空槽 |
| 8 | 256 | 60 s，cap≤3，beam=256 | `found_heuristic` | 8 taps，0 次 XOR | 0.014 s 找到；64 个有效 offset、192 个空槽 |

6-bit 结果没有证明不存在 6-bit 线性映射；它只表示当前候选池和 beam 预算内未
找到。7-bit 和 8-bit 结果则是经过程序对全部 64 个 key 逐项验证的可行映射。

### 7.2 7-bit、9-tap 映射

搜索返回的 7 行为：

```text
o0 = X11
o1 = X15
o2 = X19
o3 = X26
o4 = X41
o5 = X13 XOR X28
o6 = X18 XOR X47
offset = o0 + 2*o1 + 4*o2 + 8*o3 + 16*o4 + 32*o5 + 64*o6
```

总 tap 数为 `1+1+1+1+1+2+2=9`，独立 XOR 树数为 2。64 个输入得到 64 个
不同的 7-bit offset，但这些 offset 不是连续的 `0..63`。

### 7.3 8-bit、8-tap 直连映射

更宽的 8-bit 输出空间允许搜索器在 cap=1 时直接找到 8 个输入 bit：

```text
o0 = X7
o1 = X10
o2 = X19
o3 = X21
o4 = X27
o5 = X43
o6 = X55
o7 = X63
offset = o0 + 2*o1 + 4*o2 + 8*o3
       + 16*o4 + 32*o5 + 64*o6 + 128*o7
```

这里没有 XOR 门，只有 8 根输入线接到 8 个输出位；64 个 key 在 256 个可能
offset 中占用 64 个位置。这个结果说明该码本的 64 项可以由少量原始 bit 区分，
但它不是直接的 6-bit MPHF：若下游必须得到连续 `0..63`，还需要一个 rank/remap
层，或者接受 8-bit 稀疏 offset。

### 7.4 对硬件的含义

对于允许稀疏 offset 的查找结构，8-bit 方案的运行时路径非常简单：读取 8 根输入
bit，拼成 offset，再访问 256-entry 的稀疏表。若仍然只保存 64-entry 反向码本，
则需要额外的 `offset -> compact_id` 映射；这个映射不能被忽略，否则输出的 8-bit
值不能直接当作 6-bit codebook index。因而 8-bit 方案的“8 taps、0 XOR”是计算复杂度
结果，不等价于最终存储开销最小。

## 8. 复现命令

```sh
python3 tools/experiment_mphf_128.py \
  --entries 64 \
  --output-dir results/mphf-64 \
  --cmph-helper /tmp/cmph-harness \
  --multiplier-attempts 100000

c++ -O3 -std=c++17 -DMPHF_N=64 \
  tools/search_mphf128.cpp -o /tmp/search_mphf64

/tmp/search_mphf64 results/mphf-64/codebook.bin 100000000 100000000 \
  > results/mphf-64/cpp-search.json
```

验证：

```sh
python3 -m py_compile tools/experiment_mphf_128.py
c++ -O3 -std=c++17 -Wall -Wextra -pedantic -DMPHF_N=64 \
  tools/search_mphf128.cpp -o /tmp/search_mphf64
python3 -m json.tool results/mphf-64/experiment.json >/dev/null
python3 -m json.tool results/mphf-64/cpp-search.json >/dev/null

c++ -O3 -std=c++17 -Wall -Wextra \
  tools/search_linear_mphf_upto1024.cpp \
  -o /tmp/search_linear_mphf_upto1024

/tmp/search_linear_mphf_upto1024 results/mphf-64/codebook.txt \
  --seconds 60 --max-cap 6 --beam-width 256 --max-candidates 1000000 \
  > results/mphf-64/linear-search-6bit-60s.json

/tmp/search_linear_mphf_upto1024 results/mphf-64/codebook.txt \
  --seconds 120 --extra-bits 1 --max-cap 5 --beam-width 256 \
  --max-candidates 1000000 \
  > results/mphf-64/linear-search-7bit-120s.json

/tmp/search_linear_mphf_upto1024 results/mphf-64/codebook.txt \
  --seconds 60 --extra-bits 2 --max-cap 3 --beam-width 256 \
  --max-candidates 1000000 \
  > results/mphf-64/linear-search-8bit-60s.json

python3 -m json.tool results/mphf-64/linear-search-6bit-60s.json >/dev/null
python3 -m json.tool results/mphf-64/linear-search-7bit-120s.json >/dev/null
python3 -m json.tool results/mphf-64/linear-search-8bit-60s.json >/dev/null
```

本次 Python 编译、C++ 编译、JSON 校验和 64-entry displacement 映射验证均通过。
