"""Contract tests for the T019 orchestration and reporting layer."""

from __future__ import annotations

import json
from pathlib import Path

from tools.cgra_bench.classify import classify
from tools.cgra_bench.feature_scan import scan
from tools.cgra_bench.inventory import PIN, inventory
from tools.cgra_bench.report import report
from tools.cgra_bench.run import synthesize_invocation


ROOT = Path(__file__).resolve().parents[1]


def test_inventory_is_pinned_and_complete(tmp_path: Path) -> None:
    result = inventory(
        ROOT / "third_party/CGRA-Bench",
        tmp_path / "corpus.lock.json",
        tmp_path / "cases.v1.json",
    )
    assert result["commit"] == PIN
    assert result["scope"] == "kernels/"
    assert result["denominator"]["kernel_directories"] == 15
    assert result["denominator"]["source_translation_units"] == 34
    cases = json.loads((tmp_path / "cases.v1.json").read_text())
    assert len(cases["cases"]) == 34


def test_classifier_preserves_budget_vs_infeasible() -> None:
    result = classify("S10_MODULO_MAPPING", "mapping budget exhausted")
    assert result == {
        "category": "MAPPING_BUDGET",
        "owner": "MAPPER",
        "diagnostic_code": "MAPPING_BUDGET_EXCEEDED",
    }
    result = classify("S10_MODULO_MAPPING", "no legal mapping exists")
    assert result["category"] == "MAPPING_INFEASIBLE"


def test_classifier_preserves_production_diagnostic_and_timeout() -> None:
    result = classify("S4_FRONTEND_LOWER", "LLVM_FRONTEND_UNSUPPORTED_TYPE: float")
    assert result["category"] == "FRONTEND"
    assert result["diagnostic_code"] == "LLVM_FRONTEND_UNSUPPORTED_TYPE"
    result = classify("S4_FRONTEND_LOWER", "frontend timed out", 124)
    assert result == {
        "category": "TIMEOUT",
        "owner": "HARNESS",
        "diagnostic_code": "STAGE_TIMEOUT",
    }


def test_feature_scan_is_independent_and_counts_memory() -> None:
    result = scan(ROOT / "compiler/tests/Frontend/LLVM/fixtures/memory_raw_recurrence.ll")
    assert result["counts"]["loads"] == 1
    assert result["counts"]["stores"] == 1
    assert result["counts"]["geps"] == 2
    assert result["opcode_histogram"]["getelementptr"] == 2


def test_synthetic_invocation_is_deterministic() -> None:
    dfg = {
        "external_values": [
            {"id": 7, "name": "second"},
            {"id": 2, "name": "first"},
        ]
    }
    assert synthesize_invocation(dfg, 4) == {
        "schema": "cgra.kernel_invocation.v1",
        "trip_count": 4,
        "scalar_inputs": {"first": "0x00000101", "second": "0x00000102"},
        "scratchpad_preload": [],
        "synthetic": True,
    }


def test_report_reconciliation_exposes_missing_sources(tmp_path: Path) -> None:
    corpus = {
        "denominator": {
            "kernel_directories": 1,
            "source_translation_units": 2,
            "candidate_loops": None,
        },
        "sources": [
            {"path": "kernels/a/a.c", "enabled": True},
            {"path": "kernels/b/b.c", "enabled": True},
        ],
    }
    result = report(
        tmp_path,
        corpus,
        [
            {
                "kernel": "a",
                "source": "kernels/a/a.c",
                "loop_header": None,
                "tier": "DISCOVERED",
                "category": "BUILD",
                "owner": "HARNESS",
                "diagnostic_code": "SOURCE_BUILD_FAILED",
            }
        ],
    )
    assert result["reconciliation"]["ok"] is False
    assert result["reconciliation"]["missing_sources"] == ["kernels/b/b.c"]
