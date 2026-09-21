import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'tools' / 'search_linear_mphf.cpp'


class CppSearchTests(unittest.TestCase):
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
        path = ROOT / 'results' / 'mphf-32' / f'{name}.txt'
        result = subprocess.run(
            [str(self.binary), str(path), '--seconds', '5', '--max-cap', '4',
             '--max-candidates', '25000'],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertIsNotNone(payload['best'])
        self.assertEqual(sorted(payload['best']['ids']), list(range(32)))
        return payload

    def test_codebook1_finds_a_valid_sparse_map(self):
        result = self.run_codebook('codebook1')
        self.assertEqual(result['best']['taps'], 17)
        self.assertEqual(result['status'], 'optimal_in_pool')

    def test_codebook2_finds_a_valid_sparse_map(self):
        result = self.run_codebook('codebook2')
        self.assertEqual(result['best']['taps'], 15)
        self.assertEqual(result['status'], 'optimal_in_pool')

    def test_rejects_malformed_codebook(self):
        path = Path(self.tmp.name) / 'bad.txt'
        path.write_text('| 00 |\n')
        result = subprocess.run([str(self.binary), str(path)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
