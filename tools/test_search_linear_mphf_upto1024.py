"""Verify the small-input fast path and algebraic sixth-row completion."""
import json
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExtendedSearchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / 'search'
        subprocess.run(['c++', '-O3', '-std=c++17',
                        str(ROOT / 'tools/search_linear_mphf_upto1024.cpp'),
                        '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_keys(self, keys, cap=1):
        path = Path(self.temp.name) / 'keys.txt'
        path.write_text(''.join(f'| {x:016x} |\n' for x in keys))
        result = subprocess.run([str(self.binary), str(path), '--seconds', '10',
                                 '--max-cap', str(cap), '--beam-width', '512',
                                 '--max-candidates', '10000'],
                                check=True, capture_output=True, text=True)
        data = json.loads(result.stdout)
        self.assertIsNotNone(data['best'])
        masks = [int(m, 16) for m in data['best']['rows']]
        ids = [sum(((key & mask).bit_count() & 1) << r
                   for r, mask in enumerate(masks)) for key in keys]
        self.assertEqual(ids, data['best']['ids'])
        self.assertEqual(len(set(ids)), len(keys))
        self.assertEqual(sum(m.bit_count() for m in masks), data['best']['taps'])
        return data

    def test_smaller_and_shortened_inputs_keep_working(self):
        rng = random.Random(260)
        for n in (32, 33):
            with self.subTest(n=n):
                keys = [i | (rng.getrandbits(58) << 6) for i in range(n)]
                self.run_keys(keys)

    def test_strict64_completion_covers_all_ids(self):
        keys = [int(entry['key_hex'], 16) for entry in
                json.loads((ROOT / 'results/mphf-64/codebook.json').read_text())['entries']]
        data = self.run_keys(keys, cap=3)
        self.assertEqual(sorted(data['best']['ids']), list(range(64)))
        self.assertTrue(data['completion_last_row_unbounded'])
