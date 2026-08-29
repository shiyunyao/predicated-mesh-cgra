import json
import pathlib
import tempfile
import unittest

from tools.cgra_bench.rf_closure_report import generate


class RFClosureReportTest(unittest.TestCase):
    def _run(self, path: pathlib.Path, tier: str, status: str) -> None:
        path.mkdir(parents=True)
        (path / "summary.json").write_text(json.dumps({"timeout_count": 0, "unknown_count": 0}), encoding="utf-8")
        (path / "results.jsonl").write_text(json.dumps({
            "id": "kernel::f::loop",
            "tier": tier,
            "status": "PASS",
            "terminal_status": status,
            "diagnostic_code": "SUCCESS" if tier == "RF_CONSTRAINED_MAPPED" else "RF_READ_PORT_CONFLICT",
            "backend": {"stats": {"mii": 2, "safe_ii": 4}},
        }) + "\n", encoding="utf-8")

    def test_generate_aligns_case_ids_and_writes_all_machine_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            runs = {}
            for name, tier, status in (
                ("baseline", "ROUTE_MAPPED", "COMPILER_BUG"),
                ("port_only", "ROUTE_MAPPED", "COMPILER_BUG"),
                ("mve_only", "RF_CONSTRAINED_MAPPED", "FEASIBLE_II"),
                ("combined", "RF_CONSTRAINED_MAPPED", "FEASIBLE_II"),
            ):
                runs[name] = root / name
                self._run(runs[name], tier, status)
            output = root / "report"
            summary = generate(runs, output)
            self.assertEqual(summary["combined"]["strict_rf_mapped"], 1)
            self.assertEqual(summary["case_ids"], ["kernel::f::loop"])
            for filename in ("impact_summary.json", "per_case_delta.csv",
                             "rf_failure_migration.csv", "RF_CLOSURE_IMPACT_REPORT.md"):
                self.assertTrue((output / filename).is_file())


if __name__ == "__main__":
    unittest.main()
