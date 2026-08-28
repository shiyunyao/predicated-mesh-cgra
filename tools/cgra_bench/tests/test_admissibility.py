import unittest

from tools.cgra_bench.admissibility import terminal_status


class AdmissibilityTests(unittest.TestCase):
    def test_raw_mapping_without_rf_witness_is_a_compiler_bug(self):
        self.assertEqual(terminal_status({"status": "PASS", "tier": "ROUTE_MAPPED"}), "COMPILER_BUG")

    def test_raw_mapping_with_rf_witness_is_resource_infeasible(self):
        result = {"status": "PASS", "tier": "ROUTE_MAPPED", "backend": {"stats": {
            "rf_rejected_by_reason": {"rf_register_depth_infeasible": 1}}}}
        self.assertEqual(terminal_status(result), "RESOURCE_INFEASIBLE")

    def test_strict_mapping_is_feasible(self):
        self.assertEqual(
            terminal_status({"status": "PASS", "tier": "RF_CONSTRAINED_MAPPED", "backend": {"rf_constrained_mapping_found": True}}),
            "FEASIBLE_II",
        )

    def test_timeout_is_compiler_bug(self):
        self.assertEqual(terminal_status({"status": "FAIL", "category": "TIMEOUT"}), "COMPILER_BUG")

    def test_source_build_failure_is_not_architecture_coverage(self):
        self.assertEqual(
            terminal_status({"status": "FAIL", "category": "BUILD", "diagnostic_code": "SOURCE_BUILD_FAILED"}),
            "COMPILER_BUG",
        )

    def test_predicated_load_is_arch_memory(self):
        self.assertEqual(
            terminal_status({"status": "FAIL", "category": "FRONTEND", "diagnostic_code": "LLVM_FRONTEND_PREDICATED_LOAD_UNSUPPORTED"}),
            "ARCH_UNSUPPORTED_MEMORY",
        )


if __name__ == "__main__":
    unittest.main()
