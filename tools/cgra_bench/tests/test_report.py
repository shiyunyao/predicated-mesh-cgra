import json
import pathlib
import tempfile
import unittest

from tools.cgra_bench.report import report


class ReportExclusionTest(unittest.TestCase):
    def test_reports_case_level_exclusions_separately(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = pathlib.Path(temporary)
            cases = out / "cases" / "source"
            cases.mkdir(parents=True)
            (cases / "loop_inventory.json").write_text(json.dumps({
                "schema": "cgra.cgra_bench.loop_inventory.v1",
                "source": "kernels/source.c",
                "loops": [],
            }), encoding="utf-8")
            corpus = {
                "denominator": {
                    "kernel_directories": 1,
                    "source_translation_units": 1,
                },
                "sources": [{"path": "kernels/source.c", "enabled": True}],
            }
            results = [{
                "id": "kernels/source.c::auto",
                "kernel": "source",
                "source": "kernels/source.c",
                "loop_header": None,
                "status": "EXCLUDED",
                "excluded": True,
                "category": "CORPUS",
                "diagnostic_code": "EXPLICIT_EXCLUSION",
                "terminal_status": "NOT_ACCELERATION_REGION",
                "admissibility_status": "NOT_ACCELERATION_REGION",
                "tier": "DISCOVERED",
            }]
            summary = report(out, corpus, results)
            self.assertEqual(summary["explicitly_excluded_case_count"], 1)
            self.assertEqual(summary["explicitly_excluded_sources"], ["kernels/source.c"])
            self.assertEqual(
                summary["reconciliation"]["explicitly_excluded_case_ids"],
                ["kernels/source.c::auto"],
            )


if __name__ == "__main__":
    unittest.main()
