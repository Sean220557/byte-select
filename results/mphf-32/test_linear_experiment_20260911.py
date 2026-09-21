"""Regression and independent result checks for the exploratory search."""
import itertools
import json
import random
import re
import unittest
from pathlib import Path

from linear_experiment_20260911 import ROOT, search


def load_keys(n):
    return [int(x, 16) for x in re.findall(
        r'\|\s*([0-9a-fA-F]{16})\s*\|', (ROOT/f'codebook{n}.txt').read_text())]


def evaluate(keys, rows):
    # Explicit per-bit XOR, independent of the search's popcount evaluation.
    ids = []
    for key in keys:
        value = 0
        for r, mask in enumerate(rows):
            bit = 0
            for j in range(64):
                if mask >> j & 1:
                    bit ^= key >> j & 1
            value |= bit << r
        ids.append(value)
    return ids


class ExperimentTests(unittest.TestCase):
    def test_pruning_regression_and_bruteforce_oracle(self):
        keys = load_keys(1)
        base = [0x2000004100, 0x24001000, 0x1000008004200,
                0x80088001, 0x2800804]
        self.assertEqual(sorted(evaluate(keys, base)), list(range(32)))
        rng = random.Random(77)
        for trial in range(12):
            # Add low-weight competitors as well as the known valid branch.
            masks = base + [sum(1 << j for j in rng.sample(range(64), rng.randint(1,4)))
                            for _ in range(5)]
            pool = {}
            for mask in masks:
                signature = sum(bit << i for i, bit in enumerate(evaluate(keys, [mask])))
                if signature not in pool or mask.bit_count() < pool[signature].bit_count():
                    pool[signature] = mask
            for bound in (17, 18, 19):
                expected = any(
                    sum(m.bit_count() for m in rows) <= bound
                    and len(set(evaluate(keys, rows))) == 32
                    for rows in itertools.combinations(pool.values(), 5))
                with self.subTest(trial=trial, bound=bound):
                    actual = search(keys, pool, bound, 10)
                    self.assertEqual(actual['status']=='found', expected)

    def test_saved_results(self):
        count = 0
        for path in ROOT.glob('linear-20260911-*.jsonl'):
            for line in path.read_text().splitlines():
                result = json.loads(line)
                if result.get('status') != 'found':
                    continue
                keys = load_keys(result['codebook'])
                rows = [int(m,16) for m in result['rows']]
                ids = evaluate(keys, rows)
                self.assertEqual(ids, result['ids'])
                self.assertEqual(sorted(ids), list(range(32)))
                self.assertEqual(sum(m.bit_count() for m in rows),result['taps'])
                for depth in range(1,6):
                    self.assertEqual([sum((v & ((1<<depth)-1))==g for v in ids)
                                      for g in range(1<<depth)], [32>>depth]*(1<<depth))
                # Every nonzero combination of output columns must be balanced.
                for combination in range(1,32):
                    self.assertEqual(sum((v&combination).bit_count()%2 for v in ids),16)
                circuit = result['circuit']
                for key, expected in zip(keys,ids):
                    wires = [(key>>j)&1 for j in range(64)]
                    for a,b in circuit['shared_gates']:
                        wires.append(wires[a]^wires[b])
                    out = sum((sum(wires[j] for j in term)%2)<<r
                              for r,term in enumerate(circuit['outputs']))
                    self.assertEqual(out,expected)
                self.assertEqual(circuit['xor_count'], len(circuit['shared_gates'])+
                                 sum(len(term)-1 for term in circuit['outputs']))
                count += 1
        self.assertGreaterEqual(count, 2, 'Missing successful experiment artifacts')


if __name__ == '__main__':
    unittest.main(verbosity=2)
