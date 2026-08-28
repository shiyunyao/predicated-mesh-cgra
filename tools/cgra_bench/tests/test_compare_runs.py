import json
import pathlib
import tempfile
import unittest

from tools.cgra_bench.compare_runs import compare


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
