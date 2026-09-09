import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'mk'))
from benchmark import metrics, spread, validated_samples


class BenchmarkGate(unittest.TestCase):
    def test_late_failure_discards_earlier_times(self):
        with tempfile.TemporaryDirectory() as directory:
            counter = Path(directory)/'count'
            code = f'''from pathlib import Path
p = Path({str(counter)!r})
n = int(p.read_text()) if p.exists() else 0
p.write_text(str(n+1))
raise SystemExit(0 if n == 0 else 7)
'''
            rc, samples = validated_samples([sys.executable, '-c', code], 3)
            self.assertEqual(rc, 7)
            self.assertIsNone(samples)
            self.assertEqual(counter.read_text(), '2')

    def test_timeout_is_not_a_timing(self):
        rc, samples = validated_samples([sys.executable, '-c', 'import time; time.sleep(1)'], 3, timeout=0.02)
        self.assertEqual(rc, 124)
        self.assertIsNone(samples)

    def test_unexpected_output_is_not_a_timing(self):
        _, samples = validated_samples([sys.executable, '-c', 'print("wrong answer")'], 3)
        self.assertIsNone(samples)

    def test_success_retains_samples(self):
        rc, samples = validated_samples([sys.executable, '-c', 'pass'], 3)
        self.assertEqual(rc, 0)
        self.assertEqual(len(samples), 3)
        self.assertTrue(all(x > 0 for x in samples))

    def test_o0_identity_and_phase_nesting(self):
        data = metrics('DCC_BENCH phase parse 0.1\nDCC_BENCH phase frontend 0.3\nDCC_BENCH static before 50 10 5 20\n')
        self.assertEqual(data['ir_before'], data['ir_after'])
        self.assertAlmostEqual(data['phases_s']['sema_imports_other'], 0.2)
        self.assertEqual(data['static_after']['small_calls'], 5)
        json.dumps(data)

    def test_spread_is_median_and_mad(self):
        data = spread([1, 2, 3, 4, 100])
        self.assertEqual(data['median'], 3)
        self.assertEqual(data['mad'], 1)


if __name__ == '__main__':
    unittest.main()
