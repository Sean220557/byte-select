import importlib.util
import itertools
from pathlib import Path
import random
import subprocess
import sys
import time
import unittest

SCRIPT = Path(__file__).with_name('search_linear_mphf.py')


class SearchTests(unittest.TestCase):
    """ search_linear_mphf.py（自动 GF(2) 搜索）的单元测试。

    覆盖内容：在小规模池上与暴力法结果一致、timeout/candidate 上限语义、
    安全剪枝（绝不将 timeout 误判为不可行）、针对穷举 mask 的 last-row
    求解器，以及共享电路估计。
    """

    @classmethod
    def setUpClass(cls):
        cls.api = None
        if SCRIPT.exists():
            spec = importlib.util.spec_from_file_location('linear_auto', SCRIPT)
            cls.api = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.api)

    # 返回已加载的模块，若无法导入则使测试失败。
    def require_api(self):
        self.assertIsNotNone(self.api, 'automatic search implementation is missing')
        return self.api

    def test_fixed_pool_matches_brute_force(self):
        """功能：验证 optimize 对随机小规模 key 池求得的解与 Dm 暴力法穷举的最小代价一致。
        验证：结果的 'complete' 应为 True，且 best['taps'] 应等于暴力法在所有两行组合
        中求得的最小代价（若无可行的组合则为 None）。
        """
        a = self.require_api()
        rng = random.Random(17)
        for _ in range(25):
            keys = rng.sample(range(16), 4)
            masks = rng.sample(range(1, 16), 8)
            candidates = [(m.bit_count(), sum(((x&m).bit_count()%2)<<i
                          for i,x in enumerate(keys)), m) for m in masks]
            costs = [sum(m.bit_count() for m in rows)
                     for rows in itertools.combinations(masks, 2)
                     if len({tuple((x&m).bit_count()%2 for m in rows)
                             for x in keys}) == 4]
            result = a.optimize(keys, candidates, time.monotonic()+5)
            self.assertTrue(result['complete'])
            self.assertEqual(result['best']['taps'] if result['best'] else None,
                             min(costs) if costs else None)

    def test_timeout_preserves_incumbent_without_proof(self):
        """功能：验证零预算（budget）搜索会返回当前最优解但不标记为完整。
        验证：optimize 在预算为 0 时返回的 'complete' 应为 False，且 best['taps']
        应保持为给定 incumbent 的 2。
        """
        a = self.require_api()
        incumbent = a.describe([0, 1, 2, 3], [1, 2])
        r = a.optimize([0, 1, 2, 3], [], 0, incumbent)
        self.assertFalse(r['complete'])
        self.assertEqual(r['best']['taps'], 2)

    def test_candidate_expansion_can_improve_old_optimum(self):
        """功能：验证在可能的情况下，加入更便宜的候选必须降低旧的优化结果。
        验证：旧搜索的 best['taps'] 应为 3，加入新候选后的新搜索 best['taps'] 应降为 2。
        """
        a = self.require_api()
        keys = [0, 3, 5, 14]
        old = a.optimize(keys, [(2, 6, 6), (1, 10, 2)], time.monotonic()+5)
        new = a.optimize(keys, [(2, 6, 6), (1, 10, 2), (1, 6, 1)],
                         time.monotonic()+5, old['best'])
        self.assertEqual(old['best']['taps'], 3)
        self.assertEqual(new['best']['taps'], 2)

    def test_last_row_equations_match_exhaustive_masks(self):
        """功能：验证 last_row 必须找到与穷举 mask 相同的最轻权值解。
        验证：last_row 返回的 mask 的位数量应等于穷举可行 mask 集合的最低位数量
        （若没有可行解则为 None）。
        """
        a = self.require_api()
        rng = random.Random(19)
        for _ in range(25):
            diffs = rng.sample(range(1, 256), 4)
            expected = [m for m in range(1, 256) if m.bit_count() <= 4
                        and all((m&d).bit_count()%2 for d in diffs)]
            m = a.last_row(diffs, 8, 4, time.monotonic()+5)
            self.assertEqual(m.bit_count() if m is not None else None,
                             min(map(int.bit_count, expected)) if expected else None)

    def test_auto_stops_at_proved_absolute_lower_bound(self):
        """功能：验证自动 run 在达到 5-tap 绝对下界时能证明最优性并停止。
        验证：run 结果的 best['taps'] 应为 5、best['ids'] 排序后应等于 0..31，
        且 status 应为 'optimal'。
        """
        a = self.require_api()
        r = a.run(list(range(32)), seconds=5, max_candidates=10000)
        self.assertEqual(r['best']['taps'], 5)
        self.assertEqual(sorted(r['best']['ids']), list(range(32)))
        self.assertEqual(r['status'], 'optimal')

    def test_invalid_inputs_and_limits(self):
        """功能：验证非法的 keys 集合与非负的 seconds 会被拒绝，且 0 预算的 run 会超时。
        验证：非法的 keys 集合以及 seconds 为 -1 时都应抛出 ValueError；
        0 预算的 run 返回的 best 应为 None 且 status 应为 'time_limit'。
        """
        a = self.require_api()
        for keys in ([0]*32, list(range(31)), list(range(31))+[1<<64]):
            with self.assertRaises(ValueError):
                a.run(keys)
        with self.assertRaises(ValueError):
            a.run(list(range(32)), seconds=-1)
        r = a.run(list(range(32)), seconds=0)
        self.assertIsNone(r['best'])
        self.assertEqual(r['status'], 'time_limit')

    def test_candidate_limit_does_not_claim_infeasibility(self):
        """功能：验证命中 candidate 上限绝不能被报告为“无解/不可行”。
        验证：max_candidates 为 1 的 run 结果 status 应为 'candidate_limit'，
        best 应为 None，且 proved_cap 应为 0。
        """
        a = self.require_api()
        r = a.run(list(range(32)), seconds=5, max_candidates=1)
        self.assertEqual(r['status'], 'candidate_limit')
        self.assertIsNone(r['best'])
        self.assertEqual(r['proved_cap'], 0)

    def test_reported_shared_circuit_evaluates_like_matrix(self):
        """功能：验证贪心共享 XOR 电路求值得到的 ids 与矩阵求值结果一致。
        验证：describe 结果的 independent_xors 应为 4、shared_greedy 的 xors 应为 3，
        且按共享电路逐位求值得到的实际值应等于每个 key 的期望 id。
        """
        a = self.require_api()
        keys = [0, 1, 4, 5]
        r = a.describe(keys, [7, 11])
        self.assertEqual(r['independent_xors'], 4)
        self.assertEqual(r['shared_greedy']['xors'], 3)
        for x, expected in zip(keys, r['ids']):
            wires = [(x >> j)&1 for j in range(64)]
            for left,right in r['shared_greedy']['gates']:
                wires.append(wires[left] ^ wires[right])
            actual = sum((sum(wires[j] for j in row)%2) << i
                         for i,row in enumerate(r['shared_greedy']['outputs']))
            self.assertEqual(actual, expected)

    def test_cli_accepts_codebook_path_and_emits_json_on_timeout(self):
        """功能：验证 CLI 接受 codebook 路径，并且即使在 0 预算的 run 下也返回 JSON。
        验证：CLI 在 0 预算下运行时的 returncode 应为 0（退出码 0、status 为
        'time_limit'），且解析其 stdout 得到的 'status' 应为 'time_limit'。
        """
        self.require_api()
        import json
        path = SCRIPT.parent.parent/'results/mphf-32/codebook1.txt'
        result = subprocess.run([sys.executable, str(SCRIPT), str(path), '--seconds', '0'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['status'], 'time_limit')

    def test_historical_unequal_future_weights_regression(self):
        """功能：回归测试——验证不精确的剪枝下界不得拒绝 codebook1 池中曾找到的
        合法 18-tap 混合权值解。
        验证：optimize 结果的 'complete' 应为 True，且 best['taps'] 应为 18。
        """
        a = self.require_api()
        import re
        raw = (SCRIPT.parent.parent/'results/mphf-32/codebook1.txt').read_text()
        keys = [int(x,16) for x in re.findall(r'\|\s*([0-9a-fA-F]{16})\s*\|',raw)]
        masks = [0x2000004100,0x24001000,0x1000008004200,0x80088001,0x2800804]
        pool = [(m.bit_count(),sum(((x&m).bit_count()%2)<<i for i,x in enumerate(keys)),m)
                for m in masks]
        r = a.optimize(keys, pool, time.monotonic()+5)
        self.assertTrue(r['complete'])
        self.assertEqual(r['best']['taps'], 18)


if __name__ == '__main__':
    unittest.main()
