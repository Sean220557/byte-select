# 关于 mask 设计：平衡 nibble 位置的贪心结果

## 复现

在仓库根目录运行：

```bash
python3 tools/experiment_template_pair_collision.py
```

输入为仓库中的 `results/mphf-32/codebook1.txt` 和 `codebook2.txt`。结果默认写入本目录；可用 `--output-dir PATH` 指定其他输出目录。脚本只使用 Python 标准库，并自带码本读取、游程解析、template 生成和线性映射搜索逻辑。

输入为两个固定码本，合并去重 63 项。当前 RLE 规则检查 8 个完整字节的最大连续重复段；同一字节连续出现至少 4 次即可命中。记录格式为 2-bit 模式、3-bit 段数；每段保存 3-bit 起始位置、8-bit 字节值和 3-bit（长度减 4），未覆盖字节逐个保存。8 字节输入中的实际游程长度为 4～8。短周期与单例外周期仍走原有独立编码路径。

本次运行结果：63 项去重码本中，16 项包含至少一个长度≥4 的同字节游程；余下 47 项进入 template 阶段，合成 14 条 template。线性映射为 3-bit H，7 路搜索得到映射，H 二输入 XOR 数为 0；6 个槽位各容纳 2 条 template，往返校验通过。重复字节规则优先于短周期规则，因此这些 16 项不重复计入短周期项。`rle_pruned.csv` 列出删除项，`rle_byte_run_audit.csv` 对全部 63 项逐项列出命中情况、游程起始字节、值和长度。

| 指标 | 结果 |
| --- | ---: |
| 合并去重码本 | 63 项 |
| 同字节连续重复≥4次 | 16 项 |
| 剩余 pattern / template | 47 / 14 |
| H 映射 | 3 bit；二输入 XOR = 0 |
| 双项碰撞槽 | 6 个，最大占用 2 |
| 额外 mask nibble | 56 个 |
| 估算元数据 | 2,482 bit（未计字节对齐与控制字段）|
| 搜索及往返验证 | 通过 |

mask 贪心仍先满足每个 mask 特征最多对应两条 template，并优先选择碰撞对数变化较小的丢弃动作。平分时先选全局被丢弃次数较少的 nibble 位置，再按每条 template 的丢弃次数和成员编号轮转顺序决定，避免总从低位 nibble 开始。每条 template 最多额外丢弃 4 个 nibble。

| 指标 | 结果 |
| --- | ---: |
| Template | 14 |
| 额外丢弃 nibble 总数 | 56 |
| 不同 mask 数 | 14 |
| H 槽位宽度 | 3 bit |
| H 线性映射二输入 XOR 数 | 0 |
| 最大槽占用 | 2 条 template |
| 映射和重建校验 | 通过 |
| 估算表空间 | 2,482 bit，未计字节对齐和控制字段 |

mask 贪心按丢弃 nibble 位置的全局使用次数平衡同分候选；每条 template 最多额外丢弃 4 个 nibble。`templates.csv`、`template_linear_map_full.csv`、`rle_pruned.csv`、`pattern_assignments.csv` 和 `result.json` 给出逐项结果。该平局规则降低位置偏置，不构成 mask 或 template 数量的全局最优证明；表空间估算未计字节对齐、控制字段、mask 选择和双项槽区分电路成本。
