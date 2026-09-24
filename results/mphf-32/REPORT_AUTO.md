# 自动选择 tap 预算的矩阵搜索实验

本次新增`tools/search_linear_mphf.py`，只输入码本路径，不要求用户指定cap或总tap预算。
历史脚本和用户提供的矩阵均未作为搜索初始解。算法说明与参数契约见
[使用说明](../../tools/README_linear_mphf.md)。

## 实测结果

| 码本 | 模式 | 总时间预算 | 实际耗时 | taps | 独立XOR | 完成搜索的最大cap | 终止原因 |
|---|---|---:|---:|---:|---:|---:|---|
| codebook1 | 默认扫描 | 60秒 | 60.00秒 | 17 | 12 | 5 | 时间限制 |
| codebook2 | 默认扫描 | 60秒 | 42.13秒 | 15 | 10 | 5 | 250000候选限制 |
| codebook1 | 扫描 | 30秒 | 30.00秒 | 17 | 12 | 5 | 时间限制 |
| codebook1 | 折半方程求解 | 30秒 | 30.00秒 | 17 | 12 | 5 | 时间限制 |
| codebook2 | 扫描 | 30秒 | 30.01秒 | 15 | 10 | 4 | 时间限制 |
| codebook2 | 折半方程求解 | 30秒 | 30.01秒 | 15 | 10 | 4 | 时间限制 |

计时包括生成候选、搜索以及继续尝试扩大范围，不是找到首个解的时间。
这些是本机单次实验，部分进程并发运行，不能将小幅时间差解释为算法加速比。
两种最后一行方法得到相同最好结果；没有充分证据支持将折半法设为默认。

另实现了 `tools/search_linear_mphf.cpp` 的 C++17 版本。它使用相同的 signature、
均分 group 和分支限界逻辑，但将候选与 group 压缩为整数结构；在本机 `-O3` 下，
cap≤4 的两个码本均在约 0.1～0.15 秒内完成并得到17/15 tap。C++ 状态
`optimal_in_pool` 表示已完整搜索当前生成候选池，不表示任意更大 cap 或共享电路
模型下的全局最优。

默认模式cap=4中的改进序列：codebook1为19→18→17 taps，codebook2为16→15 taps。
两者都在该阶段找到最终最好解，随后cap=5未改进。默认运行完成cap≤5的搜索，
因此能报告这个范围内的独立行最优值；cap=6未完整生成，不代表全局最优。
codebook1默认结束时保存121607个候选，codebook2为250000个；其中包含未完成的6-tap层。

## 得到的矩阵

X0为输入最低位，o0为ID最低位，ID直接拼接`o4 o3 o2 o1 o0`。

codebook1：

```text
o0 = X9 XOR X14 XOR X26
o1 = X3 XOR X9 XOR X15
o2 = X8 XOR X27 XOR X38
o3 = X2 XOR X11 XOR X23 XOR X25
o4 = X12 XOR X19 XOR X29 XOR X40
```

行重量3、3、3、4、4，总17 taps，独立计算12个二输入XOR。

codebook2：

```text
o0 = X52
o1 = X1 XOR X25 XOR X49
o2 = X32 XOR X42 XOR X61
o3 = X0 XOR X3 XOR X48 XOR X60
o4 = X0 XOR X14 XOR X16 XOR X45
```

行重量1、3、3、4、4，总15 taps，独立计算10个二输入XOR。
两个矩阵均与之前人工给定预算的独立搜索结果相同；本轮的收益是自动化流程，
不是进一步降低了已知XOR数。贪心共享也没有进一步减少这两个结果的XOR数。

## 复现和证据

```sh
python3 tools/search_linear_mphf.py results/mphf-32/codebook1.txt
python3 tools/search_linear_mphf.py results/mphf-32/codebook2.txt
python3 tools/search_linear_mphf.py results/mphf-32/codebook1.txt --seconds 30 --last-row mitm
c++ -O3 -std=c++17 tools/search_linear_mphf.cpp -o /tmp/search_linear_mphf
/tmp/search_linear_mphf results/mphf-32/codebook1.txt --max-cap 4
/tmp/search_linear_mphf results/mphf-32/codebook2.txt --max-cap 4
python3 -m unittest discover -s tools -p test_search_linear_mphf.py
python3 -m unittest tools.test_search_linear_mphf_cpp
python3 -m unittest discover -s results/mphf-32 -p test_linear_experiment_20260911.py
```

保存结果：`auto-cb1-default.json`、`auto-cb2-default.json`及四个`auto-cb{1,2}-{scan,mitm}.json`。
每个JSON含输入SHA256、完整矩阵、32个ID、候选层与搜索完成状态。
六份结果均使用逐bit XOR独立复算，验证ID恰好覆盖0～31。新测试10项通过，
旧实验回归测试2项通过；新测试内还有25组小池穷举与25组方程求解对照。

## 仍然保留的边界

- 当前调度是短试探加几何时间片，不是二分；超时不能推断无解。
- 不保证任意32项码本存在5-bit线性完美映射，预算耗尽时可能没有矩阵。
- 高tap候选枚举仍是组合复杂度；本轮不会声称已经解决大cap搜索爆炸。
- 最优性目标是独立行tap数，不是允许任意共享中间节点的最小XOR电路。
- 未实现跨层DFS缓存、兼容性评分排序、逐节点秩剪枝和方程缓存；这些不是本轮实测收益来源。
- 静态矩阵可以固化为RTL布线与XOR；真实面积、功耗和时序仍需综合评估。

## 2026-09-24：C++ 搜索预算与热路径复测

此节只针对 C++ 版本；上面的 Python 历史实验结果及默认参数不变。C++ 默认预算
由 60 秒/250000 个候选签名调整为 300 秒/1500000 个；`max-cap` 仍是 5。
当上一 cap 已完整搜索时，下一阶段只搜索至少含一行新权重的矩阵；按权重排序后
这行必在最后。成本下界计入此行，最后两行直接配对检查。均分检测利用每层
group 等大的不变量，不再为每个 group 重算大小。新逻辑不改变证明范围。

同机以 `c++ -O3 -std=c++17` 编译，两个版本均显式指定 `--seconds 10
--max-cap 5 --max-candidates 1500000`，每个样本各运行 7 次，以墙钟时间中位数比较
（包含启动、候选生成、DFS 和 JSON 输出）：

| 输入 | 修改前 | 修改后 | 总 taps | 候选数 | 修改前/后 DFS 节点 |
|---|---:|---:|---:|---:|---:|
| codebook1 | 0.200 s | 0.144 s | 17 | 27354 | 68233 / 55067 |
| codebook2 | 0.429 s | 0.325 s | 15 | 152335 | 156477 / 126902 |
| 固定随机样本 | 0.295 s | 0.289 s | 9 | 1161326 | 83 / 59 |

随机样本使用 Python `random.Random(20260924)` 连续生成 32 个不同的 64-bit 整数，
按 `| %016x |` 格式写入临时码本。旧的 250000 候选默认上限在该样本的 cap=5
触发 `candidate_limit`，只证明到 cap=4；新的 1500000 上限完整搜索到 cap=5。
在这个样本上，进程峰值常驻内存约 100 MiB（macOS `ru_maxrss`）。
若扩大 `max-cap` 或使用更难的码本，时间和内存仍可能显著增长；这些数字不是
硬件功耗或面积估计。

修改后两个真实码本和随机样本都返回 `optimal_in_pool`，分别为 17、15、9 taps，
只表示 cap≤5 的候选池已完整搜索。两个真实码本的矩阵与 ID 已用原始 pattern
逐项重新计算校验，32 个 ID 均恰好覆盖 0～31；相关 Python/C++ 单元测试共
14 项通过。
