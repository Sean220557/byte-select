import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools' / 'search_linear_mphf.cpp'


class CppSearchTests(unittest.TestCase):
    """已编译的 search_linear_mphf.cpp 二进制的集成测试。

    先编译一次 C++ 源码，然后检查两个 codebook 是否都生成了具有预期 tap 计数的
    合法稀疏映射，以及格式错误的 codebook 是否以非零退出码被拒绝。
    """

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / 'search_linear_mphf'
        result = subprocess.run(
            ['c++', '-O3', '-std=c++17', str(SOURCE), '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_codebook(self, name):
        """在预算内对给定 codebook 运行二进制并校验 JSON。

        输入：codebook 名称。输出：解析后的 JSON；断言退出码为 0，且报告的
        ids 是 0..31 的一个 permutation（排列），失败则抛错。
        """
        path = ROOT / 'results' / 'mphf-32' / f'{name}.txt'
        result = subprocess.run(
            [str(self.binary), str(path), '--seconds', '5', '--max-cap', '4',
             '--max-candidates', '25000'],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertIsNotNone(payload['best'])
        self.assertEqual(sorted(payload['best']['ids']), list(range(32)))
        keys = [int(line.strip().strip('|').strip(), 16)
                for line in path.read_text().splitlines() if line.strip()]
        masks = [int(row, 16) for row in payload['best']['rows']]
        calculated = [sum(((key & mask).bit_count() & 1) << bit
                          for bit, mask in enumerate(masks)) for key in keys]
        self.assertEqual(calculated, payload['best']['ids'])
        self.assertEqual(sum(mask.bit_count() for mask in masks),
                         payload['best']['taps'])
        return payload

    def test_codebook1_finds_a_valid_sparse_map(self):
        """功能：验证 codebook1 必须在 cap-4 池中用一张 17-tap 的映射完成映射。
        验证：run_codebook 返回结果中的 best['taps'] 应为 17，且 status 应为
        'optimal_in_pool'。
        """
        result = self.run_codebook('codebook1')
        self.assertEqual(result['best']['taps'], 17)
        self.assertEqual(result['status'], 'optimal_in_pool')

    def test_codebook2_finds_a_valid_sparse_map(self):
        """功能：验证 codebook2 必须在 cap-4 池中用一张 15-tap 的映射完成映射。
        验证：run_codebook 返回结果中的 best['taps'] 应为 15，且 status 应为
        'optimal_in_pool'。
        """
        result = self.run_codebook('codebook2')
        self.assertEqual(result['best']['taps'], 15)
        self.assertEqual(result['status'], 'optimal_in_pool')

    def test_rejects_malformed_codebook(self):
        """功能：验证截断/非 16 位十六进制的 codebook 行必须使二进制以非零退出码退出。
        验证：用含 '| 00 |' 的坏 codebook 运行该二进制时，returncode 不应为 0。
        """
        path = Path(self.tmp.name) / 'bad.txt'
        path.write_text('| 00 |\n')
        result = subprocess.run([str(self.binary), str(path)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)

    def test_default_resource_budget_and_timeout(self):
        path = ROOT / 'results' / 'mphf-32' / 'codebook1.txt'
        result = subprocess.run([str(self.binary), str(path), '--max-cap', '1'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload['time_budget_seconds'], 300)
        self.assertEqual(payload['max_candidates'], 1500000)

        result = subprocess.run([str(self.binary), str(path), '--seconds', '0'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['status'], 'time_limit')


if __name__ == '__main__':
    unittest.main()
