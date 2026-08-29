import json
import pathlib
import tempfile
import unittest

from tools.cgra_bench.impact_report import generate


class ImpactReportTest(unittest.TestCase):
    def _write(self, path: pathlib.Path, values: list[dict]) -> None:
        path.mkdir(parents=True)
        (path / "results.jsonl").write_text(
            "".join(json.dumps(value) + "\n" for value in values), encoding="utf-8"
        )

    def test_generation_keeps_status_and_writes_machine_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            before = [{"id": "a", "terminal_status": "COMPILER_BUG", "diagnostic_code": "TIMEOUT"}]
            after = [{"id": "a", "terminal_status": "FEASIBLE_II", "diagnostic_code": "SUCCESS",
                      "backend": {"stats": {"mii": 2, "safe_ii": 3, "mapped_ii": 3}},
                      "duration_ms": {"frontend": 4, "abi_backend": 6}}]
            self._write(root / "b0", before)
            self._write(root / "b1", before)
            self._write(root / "final", after)
            output = root / "out" / "LOCAL_MAPPER_COVERAGE_COMPLETION_REPORT.md"
            result = generate(root / "b0", root / "b1", root / "final", output)
            self.assertEqual(result["final"]["strict_feasible_ii_loops"], 1)
            self.assertTrue(output.is_file())
            self.assertTrue((output.parent / "impact_summary.json").is_file())
            self.assertTrue((output.parent / "terminal_status_migration.csv").is_file())


if __name__ == "__main__":
    unittest.main()
