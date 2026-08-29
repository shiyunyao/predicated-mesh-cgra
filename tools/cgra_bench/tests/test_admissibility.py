import unittest

from tools.cgra_bench.admissibility import terminal_status


class AdmissibilityTests(unittest.TestCase):
    def test_raw_mapping_without_rf_witness_is_a_compiler_bug(self):
        self.assertEqual(terminal_status({"status": "PASS", "tier": "ROUTE_MAPPED"}), "COMPILER_BUG")

    def test_legacy_strict_tier_without_backend_evidence_is_not_success(self):
        self.assertEqual(
            terminal_status({"status": "PASS", "tier": "RF_CONSTRAINED_MAPPED"}),
            "COMPILER_BUG",
        )

    def test_raw_mapping_with_rejected_rf_candidates_is_not_resource_proof(self):
        result = {"status": "PASS", "tier": "ROUTE_MAPPED", "backend": {"stats": {
            "rf_rejected_by_reason": {"rf_register_depth_infeasible": 1}}}}
        self.assertEqual(terminal_status(result), "COMPILER_BUG")

    def test_explicit_resource_lower_bound_is_resource_infeasible(self):
        result = {"status": "FAIL", "category": "RF", "witness": {
            "proof_kind": "PROVEN_RF_PRESSURE_LOWER_BOUND"}}
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

    def test_profile_limit_is_not_rf_budget(self):
        from tools.cgra_bench.classify import classify

        result = classify(
            "S10_MODULO_MAPPING",
            "RF_BUDGET: no_mapping_within_ii_limit modulo mapper "
            "MAP_NO_MAPPING_WITHIN_II_LIMIT: maxII is below the analyzer lower bound",
        )
        self.assertEqual(result["category"], "MAPPING_BUDGET")
        self.assertEqual(result["diagnostic_code"], "MAPPING_PROFILE_LIMIT")

    def test_rf_counter_name_does_not_trigger_rf_budget(self):
        from tools.cgra_bench.classify import classify

        result = classify(
            "S10_MODULO_MAPPING",
            '{"rf_budget_exceeded": 0, "completed_modulo_mappings": 0}',
        )
        self.assertNotEqual(result["diagnostic_code"], "RF_BUDGET")


if __name__ == "__main__":
    unittest.main()
