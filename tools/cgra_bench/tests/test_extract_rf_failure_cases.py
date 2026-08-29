import json
import pathlib
import tempfile
import unittest

from tools.cgra_bench.extract_rf_failure_cases import extract


class ExtractRfFailureCasesTest(unittest.TestCase):
    def test_preserves_same_address_bucket(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = pathlib.Path(temporary)
            (run / "results.jsonl").write_text(
                json.dumps({
                    "id": "case::loop",
                    "diagnostic_code": "RFA_SAME_ADDRESS_RW_CONFLICT",
                    "tier": "ROUTE_MAPPED",
                    "terminal_status": "COMPILER_BUG",
                }) + "\n",
                encoding="utf-8",
            )
            result = extract(run)
            self.assertEqual([item["id"] for item in result["same_address"]], ["case::loop"])


if __name__ == "__main__":
    unittest.main()
