# 两个 32 项码本的两级 Displacement MPHF 实验报告

日期：2026-09-07

## 自动选择 tap 预算的程序

新增入口 `python3 tools/search_linear_mphf.py <码本路径>`，无需手动指定cap或总tap。
两个码本自动复现17 taps／12 XOR和15 taps／10 XOR；默认运行完成cap≤5范围搜索，
未证明全局最优。算法、终止条件及两种最后一行求解方式的对照见
[自动搜索实验报告](REPORT_AUTO.md)。

## 2026-09-11 更新：从零搜索得到更少 XOR 的矩阵

新的精确均分搜索得到 codebook1 **17 tap / 12 XOR**、codebook2 **15 tap / 10 XOR**，
分别比用户提供的 14 / 13 XOR 基线少 2 / 3 个 XOR。全部 32 项均独立验证无碰撞。
以下 X0 为输入最低位，输出 ID 直接拼接 `{o4,o3,o2,o1,o0}`：

```text
codebook1:
o0 = X9  XOR X14 XOR X26
o1 = X3  XOR X9  XOR X15
o2 = X8  XOR X27 XOR X38
o3 = X2  XOR X11 XOR X23 XOR X25
o4 = X12 XOR X19 XOR X29 XOR X40

codebook2:
o0 = X52
o1 = X1  XOR X25 XOR X49
o2 = X32 XOR X42 XOR X61
o3 = X0  XOR X3  XOR X48 XOR X60
o4 = X0  XOR X14 XOR X16 XOR X45
```

两者的最长平衡 XOR 树均为两层。每行最多 5 tap、五行独立计算时，更低 tap 预算的
完整候选搜索未找到解；一般共享 XOR 电路的全局最优性仍未证明。
详细掩码、全部 ID、实验日志、方法论修订和测试见
[2026-09-11 线性映射实验报告](/Users/leyang/Projects/byte-select/results/mphf-32/REPORT_LINEAR_20260911.md)。
该报告于 2026-09-14 补充了四项教学码本的逐步均分例子、签名去重表、回溯伪代码、
成本剪枝反例、零空间与输出基变换，以及共享 XOR 的门数对比；实验结果不变。

下文保留 displacement 实验及用户矩阵作为历史基线；14/13 XOR 不是此前工具独立
搜索得到的结果，不能与本轮新矩阵的来源混淆。

## 2026-10-10 更新：32 项搜索器的最后一行代数补全

将 64 项搜索器中的最后一行 GF(2) 补全移植到
`tools/search_linear_mphf.cpp`。当 DFS 的前四行已经形成 16 个二元组时，对每一对
`(x_a, x_b)` 建立：

```text
(x_a XOR x_b) · m = 1
```

程序直接在 64 个输入列的 signature 张成空间中回代求解第五行 `m`，并尝试第五行
与前四行的 16 种等价 XOR 表示。原有的候选扫描仍保留为补充路径。这样第五行不再
必须受当前 `cap` 的 tap 上限约束。

对当前两个码本重新搜索（30 秒、`cap≤4`、25,000 个候选上限）：

| 码本 | 状态 | taps | 独立 XOR | 候选数 | 完整验证 |
|---|---|---:|---:|---:|---|
| codebook1 | `optimal_in_pool` | 17 | 12 | 3,982 | 32 个 ID 覆盖 `0..31` |
| codebook2 | `optimal_in_pool` | 15 | 10 | 19,063 | 32 个 ID 覆盖 `0..31` |

优化没有改变两个码本当前的最好 taps，但增加了直接求解高 tap 第五行的能力，并保留
原有 DFS 的正确性。完整输出保存在：

```text
results/mphf-32/candidates/codebook1-linear-lastrow.json
results/mphf-32/candidates/codebook2-linear-lastrow.json
```

对应矩阵仍为：

```text
codebook1:
o0 = X9  XOR X14 XOR X26
o1 = X3  XOR X9  XOR X15
o2 = X8  XOR X27 XOR X38
o3 = X2  XOR X11 XOR X23 XOR X25
o4 = X12 XOR X19 XOR X29 XOR X40

codebook2:
o0 = X52
o1 = X1  XOR X25 XOR X49
o2 = X32 XOR X42 XOR X61
o3 = X0  XOR X3  XOR X48 XOR X60
o4 = X0  XOR X14 XOR X16 XOR X45
```

## 实验对象

输入文件为：

- [codebook1.txt](/Users/leyang/Projects/byte-select/results/mphf-32/codebook1.txt)
- [codebook2.txt](/Users/leyang/Projects/byte-select/results/mphf-32/codebook2.txt)

两个文件各包含 32 个唯一的 16 位十六进制字符串，即 32 个 64-bit pattern。每个
码本本身的反向存储量是：

```text
32 x 8B = 256B
```

## 算法

对每个 key 计算两个 hash：

```text
bucket = h1(key) & (B - 1)
id     = (h2(key) + displacement[bucket]) & 31
```

其中 `B` 是一级 bucket 数。构造阶段为每个 bucket 搜索一个 displacement，使所有
key 最终落到 `0..31` 的不同 ID。运行时只需两次 hash、一次小表读取和一次 5-bit
加法。若要识别非码本输入，还需读取 `codebook[id]` 并进行 64-bit 精确比较。

hash 函数为 `mix(key ^ seed)`，`mix` 有下文两个版本：无乘法 mixer（结果一）和
带一个常量乘法的 mixer（结果二）。**最小 7B 参数方案（8-bit seed）使用的是带
常量乘法的 mixer**；无乘法 mixer 在 8-bit seed 下已被穷举证明无解（见结果一）。

## 结果一：无乘法 mixer

原始硬件友好 mixer 只使用加法、移位、rotate 和 XOR（`-DMPHF_MIX_FOLD=0
-DMPHF_MIX_MULTIPLY=0`，与首次 search64 运行一致）：

```text
x += 0x9e3779b97f4a7c15
x ^= rotl(x, 17)
x ^= x >> 29
x ^= rotl(x, 41)
```

对每个码本按 seed 位宽从低到高扫描，每种位宽在多个 bucket 配置
`B=4,8,16,32,64,128` 上随机搜索（8-bit 为穷举）：

| seed 位宽 | mixer 变体 | 预算（每个 bucket 配置） | codebook1 | codebook2 |
|---|---|---|---|---|
| 8 bit | FOLD=0 | **穷举**全部 65,536 组 (seed1,seed2)，B=4..128 | 无解 | 无解 |
| 8 bit | FOLD=1 | 同上穷举 | 无解 | 无解 |
| 16 bit | FOLD=0 | 历史 1M + 10M（B=4..32）；本次 200M（B=8/16/32）+ 30M（B=64/128） | 无解 | 无解 |
| 16 bit | FOLD=1 | 本次 200M（B=8/16） | 无解 | 无解 |
| 24 bit | FOLD=0 | 历史 1M + 10M（B=4..32）；本次 100M（B=8/16/32） | 无解 | 无解 |
| 32 bit | FOLD=0 | 同上 | 无解 | 无解 |
| 64 bit | FOLD=0 | 历史 1M + 20M；codebook2 本次再加 100M（B=8/16/32）+ 30M（B=64/128）+ FOLD=1 变体 100M（B=8/16） | **成功**：B=8 第 7,706 组，21B packed | 无解 |

要点：

- **8-bit seed 的无解是穷举证明**，不是抽样结论：两个码本、两种 mixer 变体、
  全部 bucket 配置下，所有 65,536 组 (seed1,seed2) 都无法构造出 displacement。
- 16-bit seed 的随机搜索覆盖了约 4.7% 的 2^32 参数空间（B=8 配置），24/32-bit
  覆盖率更低。这些是"大预算无解"，不能证明不存在解，但说明解（若存在）稀疏到
  随机搜索不可达。
- 无乘法 mixer 的**最小已验证 seed 位宽是 64-bit**，且只有 codebook1 找到解
  （两个 64-bit seed 共 16B + 8 个 5-bit displacement 5B = 21B packed）。
  codebook2 在无乘法 mixer 下累计约 2.4×10^9 组 seed 内无任何位宽成功。
- 作为对照，乘法 mixer 在 8-bit seed 下 codebook2 第 1,790 组即成功（见结果二）。
  瓶颈在 mixer 对这类结构化 key（重复字节、单字节差异族）的雪崩特性，而不在
  displacement 机制或 seed 位宽本身。


## 结果二：加入一个常量乘法的 mixer

为改善高度规则的 pattern 分布，测试了一个带固定乘法常量的 mixer
（`-DMPHF_MIX_FOLD=1 -DMPHF_MIX_MULTIPLY=1`）：

```text
x += 0x9e3779b97f4a7c15
x ^= x >> 32
x ^= rotl(x, 17)
x ^= x >> 29
x ^= rotl(x, 41)
x ^= x >> 23
x *= 0xbf58476d1ce4e5b9
x ^= x >> 27
```

这个版本增加一个 64-bit 常量乘法器，计算复杂度高于纯移位/XOR 版本，但两个码本
都能得到很小的 displacement 参数。**本节所有结果（包括 8-bit seed 的 7B 参数）
均使用该乘法 mixer。**

### 两个独立 seed，64-bit

| 码本 | 最小成功 B | 搜索尝试 | 最大 displacement | 参数 packed | byte 对齐 |
|---|---:|---:|---:|---:|---:|
| codebook1 | 8 | 939 | 26 | 21B | 24B |
| codebook2 | 8 | 1,509 | 31 | 21B | 24B |

参数计算为：两个 64-bit seed 共 16B，加上 8 个 5-bit displacement（5B packed），
总计 21B。按每项一个 byte 保存 displacement 时为 24B。

在最终采用的 8-bit seed 搜索中，同时扫描了 `B=4,8,16,32,64,128`。两个码本的
`B=4` 在 2,000 万组 seed 内均未成功；`B=8` 是本次搜索中最小的成功配置。

### seed 压缩结果

在 B=8 下限制两个 seed 的位宽，每种情况搜索最多 1,000,000 组：

| 码本 | seed 位宽 | 成功尝试 | 最大 displacement | 参数 packed | byte 对齐 |
|---|---:|---:|---:|---:|---:|
| codebook1 | 32 bit | 883 | 22 | **13B** | 16B |
| codebook1 | 24 bit | 7,512 | 25 | 11B | 14B |
| codebook1 | 16 bit | 8,939 | 22 | 9B | 12B |
| codebook1 | 8 bit | 20,787 | 31 | **7B** | 10B |
| codebook2 | 32 bit | 5,365 | 27 | 13B | 16B |
| codebook2 | 24 bit | 8,847 | 30 | 11B | 14B |
| codebook2 | 16 bit | 5,125 | 31 | 9B | 12B |
| codebook2 | 8 bit | 1,790 | 17 | **7B** | 10B |

8-bit seed 的参数为：

```text
seed1 + seed2              = 2B
8 x 5-bit displacement     = 40 bit = 5B packed
合计                                  = 7B packed
```

这两个码本都在 8-bit seed 下成功，说明对这两份固定码本可以把 MPHF 参数压到 7B。
不过 8-bit seed 的构造自由度很小，换码本后成功率可能明显下降；它需要在离线阶段
对真实码本重新搜索和完整验证。

最终采用的 8-bit 独立 seed 参数如下（逐项验证见下一节）：

| 码本 | seed1 | seed2 | displacement（按 bucket 0..7） |
|---|---:|---:|---|
| codebook1 | 232 | 213 | `[5,19,11,12,0,10,22,31]` |
| codebook2 | 9 | 0 | `[3,3,14,15,6,1,0,17]` |

## 最小 7B 方案：具体参数与逐 key 计算过程

下表由 [mphf32_worked_example.py](/Users/leyang/Projects/byte-select/tools/mphf32_worked_example.py)
生成：参数取自 C++ 搜索结果，mixer 按结果二的运算序列在 Python 中独立重算，
并断言 32 个 id 恰为 `0..31` 的排列（双实现交叉验证）。

### codebook1：7B 参数与逐 key 计算过程

参数：`seed1 = 232`（0xe8），`seed2 = 213`（0xd5），B = 8。

displacement 表（bucket 0..7）：

```text
bucket :    0    1    2    3    4    5    6    7
disp   :    5   19   11   12    0   10   22   31
```

7B packed 参数布局（displacement 按 5-bit 低位在前打包）：

```text
byte  0    1    2    3    4    5    6
      0xe8 0xd5 0x65 0x2e 0x06 0x94 0xfd
      s1   s2   |---- 8 x 5-bit displacement ----|
```

代表 key `ffffffffffffffff` 的完整计算过程（每行一个 mixer 操作）：

```text
key ^ seed1        = 0xffffffffffffff17
input                    = 0xffffffffffffff17
x += 0x9e3779b97f4a7c15  = 0x9e3779b97f4a7b2c
x ^= x >> 32             = 0x9e3779b9e17d0295
x ^= rotl(x, 17)         = 0x6d44bb43e4563efb
x ^= x >> 29             = 0x6d44bb408e73e4e4
x ^= rotl(x, 41)         = 0x8a8d739a070565f8
x ^= x >> 23             = 0x8a8d728f1de251f6
x *= 0xbf58476d1ce4e5b9  = 0x8b154d6df2f648c6
x ^= x >> 27             = 0x8b154d7c905fe578
bucket = x1 & 7    = 0

key ^ seed2        = 0xffffffffffffff2a
input                    = 0xffffffffffffff2a
x += 0x9e3779b97f4a7c15  = 0x9e3779b97f4a7b3f
x ^= x >> 32             = 0x9e3779b9e17d0286
x ^= rotl(x, 17)         = 0x6d44bb43e4703ee8
x ^= x >> 29             = 0x6d44bb408e55e4f7
x ^= rotl(x, 41)         = 0xc68d559a072365eb
x ^= x >> 23             = 0xc68d54171d8851e5
x *= 0xbf58476d1ce4e5b9  = 0xb7a509fe44b9077d
x ^= x >> 27             = 0xb7a509e8b01838b5
base   = x2 & 31   = 21
d      = disp[0]  = 5
id     = (base + d) & 31 = 26
```

全部 32 个 key 的映射（`x1 = mix(key^seed1)`，`x2 = mix(key^seed2)`）：

```text
key               x1 (hex)             bk base   d  id
ffffffffffffffff  8b154d7c905fe578   0   21   5  26
0000000000000000  c17834b968e9b107   7    3  31   2
ff00111111ffffff  9f46b9abc019f54d   5   15  10  25
b0b0b0b0b0b0b0b0  f7cafe0779f55c7f   7    9  31   8
0b0b0b0b0b0b0b0b  3825656ed4c0f5d1   1   16  19   3
bbbbbbbbbbbbbbbb  3a79a3d8d2bd259c   4   22   0  22
f1f1f1f1f1f1f1f1  affb46c651815be7   7   28  31  27
1f1f1f1f1f1f1f1f  3662e33391e429d0   0    1   5   6
00ffbbbbbbffffff  1034ce16de1e791c   4   11   0  11
ffffff0000000000  b7f47c13945b5e82   2    7  11  18
1111111111111111  d7a332c2d8d6c74a   2    6  11  17
ff00111111fff6ff  5df572c7b628eac9   1   11  19  30
ff00111111ff6fff  9a2b55426e91d6cc   4    5   0   5
ff00111111ff9fff  2acd9491cb8c560c   4   21   0  21
ff00111111fff9ff  821c0fdc2082a9c4   4   23   0  23
ff001111117f5fff  27e8dddb623cc151   1   10  19  29
ff00111111f7f5ff  d8294a4475e8c426   6    9  22  31
ff00111111ffafff  bea460024b856c56   6   26  22  16
ff00111111fffaff  83f40001aa9d9572   2    1  11  12
ff0011111fffffff  ae315089b9aff1c7   7   20  31  19
3f3f3f3f3f3f3f3f  2c2de03fa8f2f53e   6   10  22   0
1b1b1b1b1b1b1b1f  c94b6434ebe39bf7   7   21  31  20
1717171717171717  fc40f4d457df7340   0    2   5   7
1b1b1b1b1b1b1b1b  2a5a7ccb931c1eb0   0   23   5  28
7f7f7f7f7f7f7f7f  f5d996456fd2e2b8   0   28   5   1
0b0b0b0b0b0b0b02  828ec3b697974a7c   4   10   0  10
bbbbbbbbbbbbbbb2  8c7303d274296e1f   7   15  31  14
ff00111111fff5ff  2debd0e79a16700c   4   15   0  15
ff00111171ffffff  72ddbdb830937485   5   31  10   9
ff00111117ffffff  7d255f85e0029300   0   19   5  24
ff00111113ffffff  0855f57290a90c3b   3   24  12   4
ff00111111ff5fff  e96fa40a32c45041   1   26  19  13
```

32 个 id 恰好是 `0..31` 的一个排列（脚本已断言验证），因此零冲突。

### codebook2：7B 参数与逐 key 计算过程

参数：`seed1 = 9`（0x09），`seed2 = 0`（0x00），B = 8。

displacement 表（bucket 0..7）：

```text
bucket :    0    1    2    3    4    5    6    7
disp   :    3    3   14   15    6    1    0   17
```

7B packed 参数布局（displacement 按 5-bit 低位在前打包）：

```text
byte  0    1    2    3    4    5    6
      0x09 0x00 0x63 0xb8 0x67 0x02 0x88
      s1   s2   |---- 8 x 5-bit displacement ----|
```

代表 key `2f02ffff02f2002f` 的完整计算过程（每行一个 mixer 操作）：

```text
key ^ seed1        = 0x2f02ffff02f20026
input                    = 0x2f02ffff02f20026
x += 0x9e3779b97f4a7c15  = 0xcd3a79b8823c7c3b
x ^= x >> 32             = 0xcd3a79b84f060583
x ^= rotl(x, 17)         = 0x3e4ae7b444019ff7
x ^= x >> 29             = 0x3e4ae7b5b656a255
x ^= rotl(x, 41)         = 0x930e4dc92399c939
x ^= x >> 23             = 0x930e4cef3f025b7e
x *= 0xbf58476d1ce4e5b9  = 0xb0d0e302e8c3d40e
x ^= x >> 27             = 0xb0d0e314f2dfb453
bucket = x1 & 7    = 3

key ^ seed2        = 0x2f02ffff02f2002f
input                    = 0x2f02ffff02f2002f
x += 0x9e3779b97f4a7c15  = 0xcd3a79b8823c7c44
x ^= x >> 32             = 0xcd3a79b84f0605fc
x ^= rotl(x, 17)         = 0x3e4ae7b444ff9f88
x ^= x >> 29             = 0x3e4ae7b5b6a8a22a
x ^= rotl(x, 41)         = 0x6f0eb3c92367c947
x ^= x >> 23             = 0x6f0eb3173e005b01
x *= 0xbf58476d1ce4e5b9  = 0xfa0a7ef4488da8b9
x ^= x >> 27             = 0xfa0a7eeb09c27630
base   = x2 & 31   = 16
d      = disp[3]  = 15
id     = (base + d) & 31 = 31
```

全部 32 个 key 的映射（`x1 = mix(key^seed1)`，`x2 = mix(key^seed2)`）：

```text
key               x1 (hex)             bk base   d  id
2f02ffff02f2002f  b0d0e314f2dfb453   3   16  15  31
0bbc0bbc0bbc0bbc  53d4ea0e2c683e9c   4   18   6  24
0bbc0bbc0bbc0f6f  36ccda979656e7d6   6   26   0  26
0bbc0bbc0bbd0bbc  1a002ff0dcb927d5   5   29   1  30
020f2f02ffff02f2  e67a0bb25c0b96c4   4    1   6   7
00020f2f02ffff02  d7f666a00e79378c   4    7   6  13
0bbd0bbc0bbc0bbc  30ec28a7307cefa1   1   24   3  27
ff02f2002f02ffff  b2d830013f52d502   2    1  14  15
02052f02ffff02f2  45d5b9e210a9677f   7   29  17  14
02f2002f02ffff02  f89a9f2ec1abb1ed   5   28   1  29
02ffff02f2002f02  693b33df514150ac   4    4   6  10
f2002f02ffff02f2  13a89ab3c233cb46   6    3   0   3
ffff02f2002f0200  09f372bb29f2c832   2    4  14  18
ffff02f2002f02ff  b7dd06acc47aae3a   2    2  14  16
202f02ffff02f200  9bf8217e94e2e1d6   6   19   0  19
0200020f2f02f200  706fa4c168eb9a48   0   29   3   0
0bbc0bbc0bbc0bbd  0dfdefbd23db6fce   6   17   0  17
0bbc0bbd0bbc0bbc  f02515788d14348c   4   15   6  21
000200000002000f  7b34fae6ce8d9ac8   0    8   3  11
006005f82225bcf2  aacc5bf4591640d6   6   28   0  28
0002000000020005  e82f14a6e123f2db   3   25  15   8
0bbc0bbc0bbd0f6f  d880f2c7e8e601f0   0    3   3   6
0bbd0bbc0bbc0f6f  8c9d7215121a85bd   5   19   1  20
0bbc0bbd0bbc0f6f  377da33a301d634f   7   20  17   5
0000000000000000  9f8bfeca28b5dc9f   7   17  17   2
0b0c0b0c0b0c0b0c  c7f328415293b463   3   26  15   9
006005f822255ff2  468460dee2f7605e   6   25   0  25
5ff200f202f20000  fc834d249a5fb563   3   21  15   4
5ff20ff202f20000  4c78b9c642ffb89e   6    1   0   1
000200000002000d  dee88ddb97e92b66   6   12   0  12
5ff22ff202f20000  1095523f1b35d254   4   16   6  22
002f02ffff02f200  cd5ed716f6306d80   0   20   3  23
```

32 个 id 恰好是 `0..31` 的一个排列（脚本已断言验证），因此零冲突。

## 运行时计算开销

推荐的两个码本配置相同：B=8、两个独立 8-bit seed、结果二的乘法 mixer。

```text
x1     = mix(key ^ seed1)
bucket = x1[2:0]

x2   = mix(key ^ seed2)
base = x2[4:0]

d  = displacement_rom[bucket]   // 8 x 5 bit
id = (base + d)[4:0]
```

每次查询需要：

- 两次 64-bit mixer。乘法 mixer 每次 mix 约为 1 次常量加法 + 4 次移位 + 2 次
  rotate + 6 次异或 + **1 次固定常量乘法**（乘法后还有一轮 `x ^= x >> 27`）；
- 一次 3-bit 地址的 8-entry displacement ROM；
- 一次 5-bit 加法；
- 如需零误判，再读取 256B 反向码本并做一次 64-bit 比较。

若完全不能用乘法器：codebook1 可退回 64-bit seed 的无乘法方案（参数 21B
packed，见 `codebook1-search64.json`）；codebook2 在无乘法 mixer 下目前没有
已知解。继续搜索更多 shift/rotate/fold 组合，或使用 CMPH-BDZ 作为离线构造
基线后再做硬件化评估，是两个后续方向。

## CMPH 对照

使用相同的 CMPH harness，两个码本的结果完全一致：

| 算法 | packed 大小 |
|---|---:|
| FCH | 100B |
| BDZ | 37B |
| CHD | 120B |

CMPH-BDZ 的 37B 是软件序列化结果，运行时仍需要 3 个 hash、`g` 表和 rank 逻辑。
它适合作为离线基线；对于这两个固定码本，定制 displacement 可以达到 7B packed，
但使用了一个常量乘法 mixer。

## 线性矩阵映射方案

除 MPHF 外，也可将 64-bit pattern 映射为 5-bit 线性码：令输入为
`x[0..63]`，选择一个固定的 `A in GF(2)^(5 x 64)`，并计算：

```text
o_r = parity(x & row_r) = XOR_{j: A[r,j] = 1} x[j]
id  = o_0 + 2 o_1 + 4 o_2 + 8 o_3 + 16 o_4
```

这里的 `row_r` 是第 `r` 行的 64-bit 掩码，bit 编号以输入 word 的最低位为
`x[0]`。对固定码本，正确条件不是矩阵在全部 64-bit 输入上可逆，而是对任意两个
码字 `x_i != x_j` 都有 `A(x_i XOR x_j) != 0`。换言之，任意 pairwise difference
都不能落入 `A` 的 kernel。满足该条件时，32 个输出恰为 `0..31` 的一个排列。

### codebook1：19-tap 矩阵

codebook1 可使用非常稀疏的矩阵：

```text
o0 = x[14] XOR x[23] XOR x[63]
o1 = x[8]  XOR x[12] XOR x[18] XOR x[27]
o2 = x[2]  XOR x[6]  XOR x[9]  XOR x[29]
o3 = x[9]  XOR x[15] XOR x[19] XOR x[26]
o4 = x[0]  XOR x[11] XOR x[25] XOR x[47]
```

行权重为 `3, 4, 4, 4, 4`，共 19 个 tap；直接实现需要
`(3 - 1) + 4 * (4 - 1) = 14` 个 2-input XOR（未计跨行公共子表达式复用）。
该矩阵已在 32 个 codebook1 pattern 上验证为无碰撞，输出覆盖 `0..31`。

### codebook2：18-tap 矩阵

codebook2 也存在和 codebook1 同等级别的稀疏解。你给出的矩阵是：

```text
o0 = x[22] XOR x[43] XOR x[48]
o1 = x[5]  XOR x[45] XOR x[63]
o2 = x[0]  XOR x[8]  XOR x[51] XOR x[61]
o3 = x[3]  XOR x[10] XOR x[32] XOR x[44]
o4 = x[1]  XOR x[16] XOR x[22] XOR x[29]
```

对应的 5×64 行掩码为：

```text
row0 = 0x0001080000400000  (bits 22,43,48; weight 3)
row1 = 0x8000200000000020  (bits 5,45,63; weight 3)
row2 = 0x2008000000000101  (bits 0,8,51,61; weight 4)
row3 = 0x0000100100000408  (bits 3,10,32,44; weight 4)
row4 = 0x0000000020410002  (bits 1,16,22,29; weight 4)
```

这里仍采用最低位为 `x[0]` 的编号约定。逐项验证结果为：

```text
pattern index:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
id:             8 11 23 27  5 20 10 14  1 21  2  6 19 13 25 24

pattern index: 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
id:            15  3 28 26  4  7 22 31  0  9 30 17 16 12 18 29
```

这些 ID 恰好是 `0..31` 的一个排列。矩阵总 tap 数为 `3+3+4+4+4=18`，直接实现
需要 `(3-1)+(3-1)+3×(4-1)=13` 个二输入 XOR。验证脚本为
`tools/verify_linear_mphf32.py`。

这个结果修正了此前报告中的 94-tap 备用矩阵：对固定 codebook2，应优先采用这里的
18-tap/13-XOR 版本。

### 为什么可以这么稀疏：GF(2) 的列空间解释

令 `X` 是 32×64 的码本矩阵，第 `i` 行是第 `i` 个 pattern，第 `j` 列记为：

```text
c_j = (x_0[j], x_1[j], ..., x_31[j])^T ∈ GF(2)^32
```

选择一行 XOR 掩码 `m` 后，32 个码字在这一输出 bit 上的取值整体是：

```text
y = X m = XOR_{j:m[j]=1} c_j
```

因此搜索不是在 64-bit 数值上“猜一个神奇公式”，而是在 64 个 32-bit 列向量中，
寻找 5 个低重量组合 `y0..y4`，使得五列合起来的 32 行互不相同。上面的 codebook2
方案对应的五个输出列（按 pattern 顺序、低位在前）分别是：

```text
y0 = 0x8aa3731e
y1 = 0x44eb1cce
y2 = 0xa4f52ab4
y3 = 0xa68de0cb
y4 = 0xdcccd22c
```

五个 `y_r` 组合后形成 32 个不同的 5-bit 行向量，所以输出必然是 `0..31` 的排列。

对当前数据做 GF(2) 消元得到：

| 码本 | pattern 行空间秩 `rank(X)` | 列空间中不同列向量数 |
|---|---:|---:|
| codebook1 | 25 | 51 |
| codebook2 | 28 | 53 |

按输入文件顺序，一组由 pattern 本身构成的极大线性无关子集索引（0-based）为：

```text
codebook1: [0,2,3,4,6,7,8,9,11,12,13,14,15,16,17,18,19,20,21,22,23,25,28,29,30]
codebook2: [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,25,26,27,28,29,30,31]
```

因此 codebook1 的其余 7 个 pattern 可以由这 25 个 pattern 线性表示，codebook2 的
其余 4 个 pattern 可以由这 28 个 pattern 线性表示。这种基适合做离线分析和降低
搜索维度，但不应直接理解成硬件需要保存 25/28 个 pattern；矩阵实现仍只读取原始
64 个输入 bit。

这解释了两个现象：

1. 码本存在大量线性结构，低重量 XOR 组合有机会得到有用的输出 bit；
2. `rank(X)>5` 并不妨碍在仅有 32 个指定点上做 5-bit 注入映射，因为我们不要求
   对整个 `span(S)` 可逆，只要求 496 个 pairwise difference 都不落入 kernel。

### “极大线性无关组”能否直接生成矩阵

可以用，但要准确理解用途。对 32 个 64-bit pattern 做高斯消元，可以选出一个极大
线性无关子集；当前两个码本的秩分别为 25 和 28。因此其他 pattern 可以表示为这组
基 pattern 的 GF(2) 线性组合。但这并不意味着只需选 5 个基 pattern：当前码本的
span 不是 5 维，不能把所有 pattern 的线性坐标压缩成 5 个线性坐标并保持全局可逆。

更适合硬件设计的是对 `X` 的 **列空间** 做基约化：

1. 用 64 个列向量 `c_j` 做消元，保留一组 pivot columns；
2. 将所有列表示为 pivot columns 的坐标，去掉重复/冗余搜索变量；
3. 在列的低重量 XOR 组合中搜索五个输出列；
4. 每加入一个输出列，就检查当前 32 行 ID 是否仍可能全部不同；
5. 用 beam search、meet-in-the-middle、SAT/ILP 或穷举低重量组合优化总 tap 数。

如果直接从 5 个原始输入 bit 组成输出，得到的只是普通 bit-select；这需要存在 5
个列向量使 32 个行投影全不同，通常很难。当前两个矩阵使用的是 3/4 个输入 bit
的列组合，因此比直接 bit-select 更灵活，但仍保持很低的 XOR 成本。

### 固定码本结构与稀疏解：注意结论边界

对任意非零 pairwise difference `d=x_i XOR x_j`，矩阵必须满足 `A d != 0`。随机的
5×64 矩阵对一个固定差分产生零输出的概率是 `1/32`；但 496 个差分对的事件存在
线性依赖，不能简单相乘估计整个矩阵的成功概率。秩、重复字节和局部字段也不能单独
证明稀疏解一定存在或随机搜索容易成功。应实际统计均衡签名、可均分分支并验证结果；
换码本后原有矩阵可能失效，需要重新搜索。

### 实践搜索结果与改进后的流程

早期的 bit-select、随机/局部、beam 和 pair-cover 搜索器只用于探索，容易产生重复
或不可复现的搜索结果，已不作为当前提交入口。当前线性映射工具保留与报告复现直接
相关的实现：

```text
tools/search_linear_mphf.cpp          # 高性能：签名候选 + 精确均分 DFS
tools/search_linear_mphf.py           # 自动调度、MITM 对照和 JSON 参考实现
tools/test_search_linear_mphf_cpp.py  # C++ 结果回归测试
tools/test_search_linear_mphf.py      # Python 参考实现测试
tools/verify_linear_mphf32.py         # 两个已知矩阵的逐项验证
```

历史基线 `19 tap/14 XOR`（codebook1）和 `18 tap/13 XOR`（codebook2）均由用户提供，
不是旧搜索器独立复现的低成本新解。本轮新结果见报告顶部。实践表明，应优先尝试
精确均分，而不是从随机 64-bit 矩阵开始，
而是：先穷举 bit-select 作下界判断，再预计算低重量列组合的 32-bit 输出签名，最后
用 pair-cover、beam search 或 SAT/ILP 做五行组合。这样搜索目标直接对应“无碰撞”，
并能把 tap 数作为第二优化目标。

需要注意：随机矩阵在 496 个 pair 上同时成功的概率很低，随机搜索命中一个稀疏解并不
代表该解接近全局最优；实际项目应固定候选池、搜索预算和随机种子，并报告“找到的最好
值”而不是未经证明的绝对最小值。

### 针对更少 XOR 的定向搜索

为检验当前结果是否还能降低，额外进行了以下定向实验：

- 直接 bit-select：完整枚举 `C(64,5)=7,624,512` 种选择，两个码本均无解；
- 部分总 tap 数不超过 17 的低重量 profile，使用预计算的
  pair-separation 位图进行 1,000 万次随机组合；未找到覆盖全部 496 对的矩阵；
- 对 `3,3,3,3,3`、`3,3,3,3,4`、`3,3,3,4,4` 等 profile 进行固定权重局部交换和
  beam search；能够得到接近解（通常剩 1--3 个冲突），但在既定预算内没有发现
  17 tap 或更低的可行矩阵。

以上为旧实验记录，既没有遍历全部低 tap profile，也不能否定更低成本解；本轮已经
得到 `17 tap/12 XOR` 和 `15 tap/10 XOR`。若需要一般共享电路的严格最优性，
应把低重量候选签名转成 SAT/ILP/精确 cover 问题，或对每个 tap profile 做完整的
branch-and-bound 穷举；随机搜索不能替代这个证明。

### 与 displacement MPHF 的取舍

| 方案 | 可加载参数 | 查询关键路径 | codebook1 | codebook2 |
|---|---:|---|---:|---:|
| 线性矩阵（RTL 常量，2026-09-11） | 0B | 最多两层 XOR 树 | 17 tap / 12 XOR | 15 tap / 10 XOR |
| 线性矩阵（完整位图） | 40B | XOR 网络 | 40B | 40B |
| displacement MPHF | 7B packed | 两个 mixer + 8-entry ROM + 5-bit 加法 | 7B | 7B |

矩阵固定综合到 RTL 时，不需要独立参数存储，也不依赖乘法器或 ROM；这对优先控制
数据通路结构的实现很有价值。若矩阵需要运行时装载，完整位图为 `5 x 64 = 320 bit =
40B`，通常不如 7B MPHF 参数紧凑。矩阵输出是一个排列而非码本文件顺序的 ID；若接口
必须保持原始条目编号，还需增加一个固定的 32 项重编号表，或将该置换并入下游编码。
与 MPHF 一样，若要拒绝任意非码本输入，仍需 `id -> pattern` 反向表和 64-bit 精确比较。

## 结论

对当前两份 32 项码本：

- 最小已验证方案是 **两个独立 8-bit seed + 8 个 5-bit displacement =
  7B packed，10B byte 对齐（含 seed）**，mixer 需要一次固定常量乘法。具体
  seed、displacement 和逐 key 计算过程见"最小 7B 方案"一节。
- 若强制无乘法 mixer：最小 seed 位宽为 64-bit，且仅 codebook1 有解（21B
  packed）；8-bit seed 无解已被穷举证明，codebook2 无任何位宽成功。
- 若优先避免乘法器、ROM 和加法器，固定 RTL 的 GF(2) 矩阵是可行备选：codebook1
  本轮只需 17 tap / 12 XOR，codebook2 只需 15 tap / 10 XOR；两者都可能优于 MPHF 的
  复杂数据通路，最终仍需以目标工艺的综合、时序和功耗结果决定。
- 如果把 seed 和 displacement 都综合为 RTL 常量，则不需要独立的可加载参数
  存储；但 hash 逻辑中的常量乘法器仍然存在。若码表可运行时更新，应保存这
  7--10B 参数。
- 无论使用哪种 MPHF，256B 的 `ID -> prefix` 码表仍需保留用于解码或精确 miss
  检测。

## 复现

```sh
# 1) 码本二进制（大端 64-bit）
for n in 1 2; do
  awk -F'|' 'NF>=2 {gsub(/[[:space:]]/,"",$2); print $2}' \
    results/mphf-32/codebook$n.txt | xxd -r -p > /tmp/codebook$n.bin
done

# 2) 最终采用的乘法 mixer、8-bit seed、20M 组搜索
c++ -O3 -std=c++17 -DMPHF_N=32 -DMPHF_MIX_MULTIPLY=1 -DMPHF_MIX_FOLD=1 \
  -DMPHF_SEED_BITS=8 tools/search_mphf128.cpp -o /tmp/search_mphf32
/tmp/search_mphf32 /tmp/codebook1.bin 0 20000000 \
  > results/mphf-32/candidates/codebook1-final-seed8-20m.json
/tmp/search_mphf32 /tmp/codebook2.bin 0 20000000 \
  > results/mphf-32/candidates/codebook2-final-seed8-20m.json

# 3) 无乘法 mixer、8-bit seed 穷举证明（65,536 组 x 全部 B）
c++ -O3 -std=c++17 -DMPHF_N=32 -DMPHF_MIX_FOLD=0 -DMPHF_SEED_BITS=8 \
  -DMPHF_EXHAUSTIVE=1 tools/search_mphf128.cpp -o /tmp/mphf32_exh8
/tmp/mphf32_exh8 /tmp/codebook1.bin 0 0 all \
  > results/mphf-32/candidates/codebook1-seed8-exhaustive.json
/tmp/mphf32_exh8 /tmp/codebook2.bin 0 0 all \
  > results/mphf-32/candidates/codebook2-seed8-exhaustive.json

# 4) 无乘法 mixer、16-bit seed 大预算（覆盖约 4.7% 参数空间）
c++ -O3 -std=c++17 -DMPHF_N=32 -DMPHF_MIX_FOLD=0 -DMPHF_SEED_BITS=16 \
  tools/search_mphf128.cpp -o /tmp/mphf32_s16
/tmp/mphf32_s16 /tmp/codebook1.bin 0 200000000 8,16,32 \
  > results/mphf-32/candidates/codebook1-seed16-200m.json
/tmp/mphf32_s16 /tmp/codebook2.bin 0 200000000 8,16,32 \
  > results/mphf-32/candidates/codebook2-seed16-200m.json

# 5) codebook2 无乘法 64-bit 重试（100M）
c++ -O3 -std=c++17 -DMPHF_N=32 -DMPHF_MIX_FOLD=0 -DMPHF_SEED_BITS=64 \
  tools/search_mphf128.cpp -o /tmp/mphf32_s64
/tmp/mphf32_s64 /tmp/codebook2.bin 0 100000000 8,16,32 \
  > results/mphf-32/candidates/codebook2-seed64-retry-100m.json

# 6) 逐 key 计算过程与双射验证
python3 tools/mphf32_worked_example.py             # 乘法 mixer 7B 方案（两个码本）
python3 tools/mphf32_worked_example.py --appendix  # codebook1 无乘法 21B 方案
```

说明：

- `search_mphf128` 的第 4 个参数可选，用于只搜索指定的 bucket 配置（如
  `8,16,32`，缺省为 `all`）；`-DMPHF_EXHAUSTIVE=1` 启用 seed 全空间穷举模式。
- 24/32-bit、FOLD=1 变体、B=64/128 探针等其余运行与上面同理，JSON 结果保存在
  `results/mphf-32/candidates/`（文件名含位宽、预算与变体）。最终 8-bit 结果见
  `codebook1-final-seed8.json`、`codebook2-final-seed8.json`。
## 附录：codebook1 无乘法 mixer 方案（21B）

作为"最小 7B 方案"一节的对照，这里完整展示结果一得到的 codebook1 无乘法
mixer 解：两个 64-bit seed、B=8、第 7,706 组搜索命中，参数 21B packed。参数
取自 `codebook1-search64.json`，由
`python3 tools/mphf32_worked_example.py --appendix` 用与 C++ 一致的四步无乘法
mixer 独立重算并断言双射。

与乘法版对比：displacement 部分同为 5B，多出的 14B 全部在 seed 上（16B vs
2B）。若 seed 综合为 RTL 常量（码本出厂固定），实际需要保存的只有 5B
displacement ROM，且省掉乘法器；仅当参数需要作为可加载数据存储时，21B 与
7B 的差距才成立。

### codebook1：21B 参数与逐 key 计算过程（无乘法 mixer）

参数：`seed1 = 911749452187074594`（0x0ca72f4bcdc48022），`seed2 = 17597228146858026228`（0xf435e9c3f4fa64f4），B = 8。mixer 为结果一的四步无乘法版本（加法、两次 rotate 自异或、一次移位自异或）。

displacement 表（bucket 0..7）：

```text
bucket :    0    1    2    3    4    5    6    7
disp   :    2    0   17    6   20   22    0   13
```

21B packed 参数布局（seed 大端；displacement 按 5-bit 低位在前打包）：

```text
byte   0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20
      0x0c 0xa7 0x2f 0x4b 0xcd 0xc4 0x80 0x22 0xf4 0x35 0xe9 0xc3 0xf4 0xfa 0x64 0xf4 0x02 0x44 0x43 0x2d 0x68
      |---- seed1 (8B) ----||---- seed2 (8B) ----| |-- 8 x 5-bit disp --|
```

代表 key `ffffffffffffffff` 的完整计算过程（每行一个 mixer 操作）：

```text
key ^ seed1        = 0xf358d0b4323b7fdd
input                    = 0xf358d0b4323b7fdd
x += 0x9e3779b97f4a7c15  = 0x91904a6db185fbf2
x ^= rotl(x, 17)         = 0x054b29664660d8d2
x ^= x >> 29             = 0x054b29666c3993e0
x ^= rotl(x, 41)         = 0x766ce96cfa6b5f38
bucket = x1 & 7    = 0

key ^ seed2        = 0x0bca163c0b059b0b
input                    = 0x0bca163c0b059b0b
x += 0x9e3779b97f4a7c15  = 0xaa018ff58a501720
x ^= rotl(x, 17)         = 0xb5ea9b55a4114323
x ^= x >> 29             = 0xb5ea9b500b45998e
x ^= rotl(x, 41)         = 0x3ed9863bde733998
base   = x2 & 31   = 24
d      = disp[0]  = 2
id     = (base + d) & 31 = 26
```

全部 32 个 key 的映射（`x1 = mix(key^seed1)`，`x2 = mix(key^seed2)`）：

```text
key               x1 (hex)           bk base  d  id
ffffffffffffffff  766ce96cfa6b5f38   0   24   2  26
0000000000000000  78b6aceedba60fa8   0   19   2  21
ff00111111ffffff  40b76adb4fce91f5   5   11  22   1
b0b0b0b0b0b0b0b0  e71a0818a1068bad   5   27  22  17
0b0b0b0b0b0b0b0b  219206f696fc57fa   2   28  17  13
bbbbbbbbbbbbbbbb  1c6e41a8d5604567   7   14  13  27
f1f1f1f1f1f1f1f1  e43400dbe9bab4e0   0   30   2   0
1f1f1f1f1f1f1f1f  d3e6338ead078008   0   26   2  28
00ffbbbbbbffffff  48c460c83bb4ccba   2   26  17  11
ffffff0000000000  6b257413ac176ee1   1   12   0  12
1111111111111111  f93563c59299dc00   0   23   2  25
ff00111111fff6ff  40a56adb5dce98d1   1   14   0  14
ff00111111ff6fff  479718dc6fcd0f8c   4   18  20   6
ff00111111ff9fff  407766db8fcef073   3   13   6  19
ff00111111fff9ff  40ab6adb53ce9fcd   5   19  22   9
ff001111117f5fff  470711dc08534740   0   16   2  18
ff00111111f7f5ff  b0bc6b2b5b35745c   4    2  20  22
ff00111111ffafff  401760dbefcec0b0   0   14   2  16
ff00111111fffaff  40ad6adb55ce9cc1   1   31   0  31
ff0011111fffffff  41f77edb45e631e1   1   23   0  23
3f3f3f3f3f3f3f3f  b448152f0e65a5ec   4    0  20  20
1b1b1b1b1b1b1b1f  873b8f8af9b3dc29   1   10   0  10
1717171717171717  7b68ab9704df08f3   3   28   6   2
1b1b1b1b1b1b1b1b  f73bb78af98bdc35   5   14  22   4
7f7f7f7f7f7f7f7f  7fb35ae56d24fdef   7   16  13  29
0b0b0b0b0b0b0b02  0d9210f696ea57f1   1    3   0   3
bbbbbbbbbbbbbbb2  306e57a8d576456c   4   17  20   5
ff00111111fff5ff  40a36adb5bce9bdd   5    2  22  24
ff00111171ffffff  4cb7acdb2e489136   6    8   0   8
ff00111117ffffff  40f76edb4dc6b1f1   1    7   0   7
ff00111113ffffff  407766db49d6f1f9   1   15   0  15
ff00111111ff5fff  47f71edc0fcd3f4f   7   17  13  30
```

32 个 id 恰好是 `0..31` 的一个排列（脚本已断言验证），因此零冲突。
