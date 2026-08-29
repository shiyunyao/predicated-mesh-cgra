import json
import pathlib
import tempfile
import unittest

from tools.cgra_bench.compare_runs import compare
from tools.cgra_bench.report import _mapping_verified
from tools.cgra_bench.run import tier_from_compile


class CompareRunsTest(unittest.TestCase):
    def _write(self, path: pathlib.Path, values: list[dict]) -> None:
        path.mkdir(parents=True)
        (path / "results.jsonl").write_text(
            "".join(json.dumps(value) + "\n" for value in values), encoding="utf-8"
        )

    def test_migration_and_denominator_delta_are_explicit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            self._write(root / "baseline", [
                {"id": "a", "terminal_status": "COMPILER_BUG", "diagnostic_code": "TIMEOUT"},
                {"id": "old", "terminal_status": "ARCH_UNSUPPORTED_OPERATION"},
            ])
            self._write(root / "candidate", [
                {"id": "a", "terminal_status": "FEASIBLE_II", "backend": {"stats": {"mii": 2, "safe_ii": 3}}},
                {"id": "new", "terminal_status": "ARCH_UNSUPPORTED_MEMORY"},
            ])
            summary, rows = compare(root / "baseline", root / "candidate")
            self.assertEqual(summary["added_cases"], ["new"])
            self.assertEqual(summary["removed_cases"], ["old"])
            self.assertEqual(summary["status_migrations"], {"COMPILER_BUG->FEASIBLE_II": 1})
            self.assertEqual(len(rows), 3)

    def test_raw_route_status_is_not_strict_mapping_evidence(self):
        self.assertFalse(_mapping_verified({
            "backend": {"mapping_status": "route_mapped_rf_infeasible"}
        }))
        self.assertFalse(_mapping_verified({
            "backend": {"mapping_status": "success"}
        }))
        self.assertFalse(_mapping_verified({"diagnostic_code": "MODULO_MAPPING_VERIFIED"}))
        self.assertTrue(_mapping_verified({
            "backend": {"mapping_status": "rf_constrained_success"}
        }))

    def test_compile_tier_requires_explicit_rf_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            artifact = pathlib.Path(temporary)
            (artifact / "kernel_compile_result.json").write_text(json.dumps({
                "status": "success",
                "backend": {"mode": "mapping_research", "mapping_status": "success"},
            }), encoding="utf-8")
            self.assertEqual(tier_from_compile(artifact, True), ("ROUTE_MAPPED", "S10_MODULO_MAPPING"))
            (artifact / "kernel_compile_result.json").write_text(json.dumps({
                "status": "success",
                "backend": {"mode": "mapping_research", "mapping_status": "rf_constrained_success"},
            }), encoding="utf-8")
            self.assertEqual(tier_from_compile(artifact, True), ("RF_CONSTRAINED_MAPPED", "S12_RF_ALLOCATION"))
