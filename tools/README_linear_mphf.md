# 自动稀疏矩阵 MPHF 搜索

## 使用

```sh
python3 tools/search_linear_mphf.py results/mphf-32/codebook1.txt
python3 tools/search_linear_mphf.py results/mphf-32/codebook2.txt
```

唯一必填参数是码本路径。输入为32个不同的64-bit整数，每行格式为
`| 0123456789abcdef |`；允许空行，不接受标题或静默忽略的非法行。
X0为整数最低位，第一行矩阵产生ID最低位。只依赖Python标准库。

默认总时间预算60秒、最多保存250000个不同的均衡候选签名。
这是候选数量限制，不是严格的进程内存上限。时间限制为协作式检查，
排序、JSON输出等可能产生少量超时；枚举和搜索都检查截止时间。

```sh
python3 tools/search_linear_mphf.py codebook.txt --seconds 120 --max-candidates 500000
python3 tools/search_linear_mphf.py codebook.txt --last-row mitm
```

stdout是一个JSON结果，可用shell重定向保存；阶段进度输出到stderr。
程序不自动覆盖码本或历史实验。合法任务即使预算耗尽也返回退出码0，
调用方必须检查JSON的`status`和`best`；输入错误退出码2。

## C++ 高性能版本

C++ 版本与 Python 版本使用相同的候选签名和均分 DFS，但把32条 pattern 的列签名、
候选池和 group 操作保存在紧凑的整数结构中，适合做批量离线搜索：

```sh
c++ -O3 -std=c++17 tools/search_linear_mphf.cpp -o /tmp/search_linear_mphf
/tmp/search_linear_mphf results/mphf-32/codebook1.txt
/tmp/search_linear_mphf results/mphf-32/codebook2.txt
```

输入仍是每行 `| 16个十六进制字符 |` 的文本码本。可通过以下参数限制资源：

```sh
/tmp/search_linear_mphf codebook.txt \
  --seconds 300 --max-cap 5 --max-candidates 1500000
```

这是 C++ 版本的默认值（Python 版本仍为 60 秒、250000 个候选）。
增加时间只允许更久地搜索，并不扩大单行的 5-tap 上限；如需搜索更重的行，
还须显式提高 `--max-cap`。1500000 是候选签名数量限制，不是严格内存上限，
处理新码本时应同时检查 JSON 的 `status`、`generated_cap` 和 `proved_cap`。

stdout 输出 JSON，阶段进度输出 stderr。C++ 版本当前实现候选扫描最后一行，不包含
Python 版本的 `--last-row mitm` 实验分支；它输出矩阵 mask、行 tap、总 tap、独立 XOR、
32个 ID、候选层、实际耗时、预算和搜索状态。

C++ 版在每个 cap 都完整搜索后，下一层只需检查至少含一行新增权重的矩阵；
由于候选按权重排序，这一行必在最后。这样不会漏掉更优解，并可收紧成本下界。
最后两行直接配对检查，避免构造中间候选池和重复划分；均分测试只在成功时
创建下一层分组。这些优化不改变独立行 tap 目标或证明范围。

## 实际实现的算法

1. 将64个输入位分别表示为32-bit列签名；行mask的签名是所选列的XOR。
2. 按重量增量枚举mask，只保留16/16均衡签名；同签名保留首次出现的最短表示。
   不删除低重量行，也不对cap=1、2单独运行完整搜索。
3. cap=3仅试探最多0.5秒；cap=4最多12秒；随后阶段预算按倍数增加，
   均受剩余总时间约束。这是可复现的启发式时间调度，不是学习得到的最优调度。
4. 在固定候选池内进行一次分支限界DFS。候选按重量、签名排序；每一行
   必须均分所有当前分组：32→2×16→4×8→8×4→16×2→32×1。
5. 找到解就降低上界U，不为每个总tap预算重启DFS。下界使用剩余候选中
   最便宜的若干行的成本和；忽略相互兼容性只会使这个下界更保守。
6. 候选过滤保留安全的重量界：候选d可能最后才被选中，不能假定所有后续行
   都至少和d一样重。测试包含历史上这个剪枝错误的18-tap反例。
7. 扩展候选层时复用全部已生成签名和最好矩阵，但DFS树重新搜索；当前版本
   没有实现跨候选层的搜索树缓存、兼容性评分排序或逐节点秩剪枝。

当每个均衡行至少需要w个tap（完整低重量枚举提供证明），任何优于U的五行
矩阵的单行重量最多是U−1−4w。若已经完整生成到这个范围并完成搜索，就可以
证明独立行tap目标下的全局最优。否则仅报告已完成候选范围内的最优性。

## 最后一行的两种方法

默认`scan`直接扫描兼容候选。可选`mitm`在前四行确定后，令16对输入之差
组成D，求`D m = 全1`的低重量解。对64个输入列计算16-bit syndrome，枚举
不超过ceil(cap/2) tap的半边，按syndrome建立最短表示表，再匹配
`left_syndrome XOR right_syndrome = 全1`。

这是精确的有界低重量求解，不是仅用高斯消元就能得到最稀疏解。半边重叠时
mask按XOR合并，重复位抵消。该方法未实现高斯消元预检查或跨节点缓存。
在本次两个码本上没有观察到明确加速收益，故保留扫描作为默认值。

## 输出和证明边界

- `best`：矩阵mask、bit列表、表达式、每行tap、总tap、32个ID。
- `independent_xors`：总tap−5；输出拼接不需要加法器。
- `shared_greedy`：重复输入对的贪心共享结果，不保证全局最少XOR电路。
  `gates[i]`的输出线编号为64+i，0～63代表原始输入位。
- `stages`：各层候选数量、耗时、搜索节点、改进序列及是否完成。
- `generated_cap`：完整生成到的层；中断时可能还保存有下一层部分候选。
- `proved_cap`：完整完成搜索的最大层；0表示未完成任何层。
- Python 的`status=optimal`：满足该脚本实现的最优性停止条件；C++ 的
  `status=optimal_in_pool`：生成到`max-cap`且该候选池已完整搜索，最优性只限于该池。
  只有总 tap 达到绝对下界5时，C++才输出`optimal`。
- `time_limit`/`candidate_limit`：资源耗尽；best可以为空，不能推断无解。
- `no_linear_map_in_pool`：已完整穷尽当前候选池但没有解，不代表任意矩阵都无解。

候选签名去重只保证独立行tap目标：多个等价mask可能具有不同的共享电路成本，
所以不能据此证明共享XOR最优。此工具不保证任意32个key存在5-bit线性完美映射。
映射仅在输入码本上无碰撞；未命中的输入仍需精确校验，解码仍需反向码本。

## 验证

```sh
python3 -m unittest discover -s tools -p test_search_linear_mphf.py
```

覆盖小候选池穷举最优值对照、最后一行方程对照、候选扩展后改进、
超时保留解、候选限制不误判无解、共享电路计算、CLI解析和历史剪枝反例。
实际32项输入的实验记录见`results/mphf-32/REPORT_AUTO.md`。
