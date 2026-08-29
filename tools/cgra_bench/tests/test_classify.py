import unittest

from tools.cgra_bench.classify import classify


class ClassificationContractTest(unittest.TestCase):
    def test_mapping_budget_is_not_infeasible(self):
        result = classify("BACKEND", "mapping budget exhausted")
        self.assertEqual(result["category"], "MAPPING_BUDGET")
        self.assertEqual(result["diagnostic_code"], "MAPPING_BUDGET_EXCEEDED")

    def test_mapping_infeasible_remains_distinct(self):
        result = classify("S10_MODULO_MAPPING", "no legal placement")
        self.assertEqual(result["category"], "MAPPING_INFEASIBLE")

    def test_timeout_is_explicit(self):
        result = classify("S10_MODULO_MAPPING", "process timed out", 124)
        self.assertEqual(result["category"], "TIMEOUT")
        self.assertEqual(result["diagnostic_code"], "STAGE_TIMEOUT")


if __name__ == "__main__":
    unittest.main()
