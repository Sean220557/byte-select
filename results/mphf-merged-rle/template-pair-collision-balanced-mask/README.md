# 关于 mask 设计：平衡 nibble 位置的贪心结果

## 复现

在仓库根目录运行：

```bash
python3 tools/experiment_template_pair_collision.py
```

输入为仓库中的 `results/mphf-32/codebook1.txt` 和 `codebook2.txt`。结果默认写入本目录；可用 `--output-dir PATH` 指定其他输出目录。脚本只使用 Python 标准库，并自带码本读取、游程解析、template 生成和线性映射搜索逻辑。

输入为两个固定码本，合并去重 63 项；剔除 16 项可单独编码的 pattern 后，47 项合为 14 条 template。

mask 贪心仍先满足每个 mask 特征最多对应两条 template，并优先选择碰撞对数变化较小的丢弃动作。平分时先选全局被丢弃次数较少的 nibble 位置，再按每条 template 的丢弃次数和成员编号轮转顺序决定，避免总从低位 nibble 开始。每条 template 最多额外丢弃 4 个 nibble。

| 指标 | 结果 |
| --- | ---: |
| Template | 14 |
| 额外丢弃 nibble 总数 | 56 |
| 各位置额外丢弃数（从左至右） | 4, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 3, 3, 4, 3 |
| 不同 mask 数 | 14 |
| H 槽位宽度 | 3 bit |
| H 线性映射二输入 XOR 数 | 0 |
| 最大槽占用 | 2 条 template |
| 映射和重建校验 | 通过 |
| 估算表空间 | 2482 bit，未计字节对齐和控制字段 |

相比此前固定从低位开始的 tie-break，位置分布已平衡；mask 字典增加到 14 个不同值，估算空间增加。`templates.csv`、`template_linear_map_full.csv`、`rle_pruned.csv`、`pattern_assignments.csv` 和 `result.json` 给出完整逐项数据。该平局规则降低位置偏置，不构成 mask 或 template 数量的全局最优证明；当前计算结果也不包含真实硬件中的 mask 选择和双项槽区分电路成本。
