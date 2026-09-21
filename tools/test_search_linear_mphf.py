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
    @classmethod
    def setUpClass(cls):
        cls.api = None
        if SCRIPT.exists():
            spec = importlib.util.spec_from_file_location('linear_auto', SCRIPT)
            cls.api = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.api)

    def require_api(self):
        self.assertIsNotNone(self.api, 'automatic search implementation is missing')
        return self.api

    def test_fixed_pool_matches_brute_force(self):
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
        a = self.require_api()
        incumbent = a.describe([0, 1, 2, 3], [1, 2])
        r = a.optimize([0, 1, 2, 3], [], 0, incumbent)
        self.assertFalse(r['complete'])
        self.assertEqual(r['best']['taps'], 2)

    def test_candidate_expansion_can_improve_old_optimum(self):
        a = self.require_api()
        keys = [0, 3, 5, 14]
        old = a.optimize(keys, [(2, 6, 6), (1, 10, 2)], time.monotonic()+5)
        new = a.optimize(keys, [(2, 6, 6), (1, 10, 2), (1, 6, 1)],
                         time.monotonic()+5, old['best'])
        self.assertEqual(old['best']['taps'], 3)
        self.assertEqual(new['best']['taps'], 2)

    def test_last_row_equations_match_exhaustive_masks(self):
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
        a = self.require_api()
        r = a.run(list(range(32)), seconds=5, max_candidates=10000)
        self.assertEqual(r['best']['taps'], 5)
        self.assertEqual(sorted(r['best']['ids']), list(range(32)))
        self.assertEqual(r['status'], 'optimal')

    def test_invalid_inputs_and_limits(self):
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
        a = self.require_api()
        r = a.run(list(range(32)), seconds=5, max_candidates=1)
        self.assertEqual(r['status'], 'candidate_limit')
        self.assertIsNone(r['best'])
        self.assertEqual(r['proved_cap'], 0)

    def test_reported_shared_circuit_evaluates_like_matrix(self):
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
        self.require_api()
        import json
        path = SCRIPT.parent.parent/'results/mphf-32/codebook1.txt'
        result = subprocess.run([sys.executable, str(SCRIPT), str(path), '--seconds', '0'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['status'], 'time_limit')

    def test_historical_unequal_future_weights_regression(self):
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
