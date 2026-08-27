"""Contract tests for the T019 orchestration and reporting layer."""

from __future__ import annotations

import json
from pathlib import Path

from tools.cgra_bench.classify import classify
from tools.cgra_bench.check_baseline import check, check_expectations
from tools.cgra_bench.evidence import complete_stage_records, write_case_evidence
from tools.cgra_bench.inventory import PIN, inventory
from tools.cgra_bench.invocation import address_external_ids, synthesize_invocation
from tools.cgra_bench.report import report, target_contract_summary


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
    result = classify("BACKEND", "mapping budget exhausted")
    assert result["category"] == "MAPPING_BUDGET"
    result = classify("S10_MODULO_MAPPING", "modulo mapping verification failed")
    assert result["category"] == "MAPPING_VERIFY"


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


def test_synthetic_invocation_assigns_pointer_windows_from_dataflow() -> None:
    dfg = {
        "external_values": [
            {"id": 0, "name": "base", "type": {"kind": "integer", "bits": 32}},
            {"id": 1, "name": "scalar", "type": {"kind": "integer", "bits": 32}},
        ],
        "nodes": [
            {"id": 0, "opcode": "Add"},
            {"id": 1, "opcode": "Load"},
            {"id": 2, "opcode": "Add"},
        ],
        "external_bindings": [
            {"external": 0, "node": 0, "operand": 0},
            {"external": 1, "node": 2, "operand": 0},
        ],
        "edges": [{"src": 0, "dst": 1, "operand": 0, "kind": "data", "distance": 0}],
    }
    invocation = synthesize_invocation(dfg, 4)
    assert invocation["scalar_inputs"] == {"base": "0x00000000", "scalar": "0x00000101"}


def test_synthetic_pointer_roles_follow_memory_analysis_base() -> None:
    dfg = {
        "external_values": [
            {"id": 0, "name": "base", "type": {"kind": "integer", "bits": 32}},
            {"id": 1, "name": "offset", "type": {"kind": "integer", "bits": 32}},
        ]
    }
    memory = {"accesses": [{"base": "%base", "offset_words": 0, "stride_words": 1}]}
    assert address_external_ids(dfg, memory) == {0}


def test_target_contract_does_not_treat_icmp_as_literal_operation() -> None:
    operations = {"ADD": {}, "CMP_ULT": {}}
    assert target_contract_summary("Add", operations) == "declared"
    assert target_contract_summary("ICmp", operations) == "predicate-dependent"


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


def test_report_reconciles_discovered_loops(tmp_path: Path) -> None:
    case_dir = tmp_path / "cases" / "a"
    case_dir.mkdir(parents=True)
    (case_dir / "loop_inventory.json").write_text(json.dumps({
        "schema": "cgra.cgra_bench.loop_inventory.v1",
        "source": "kernels/a/a.c",
        "loops": [{"function": "kernel", "header": "loop"}],
    }))
    corpus = {
        "denominator": {"kernel_directories": 1, "source_translation_units": 1},
        "sources": [{"path": "kernels/a/a.c", "enabled": True}],
    }
    summary = report(tmp_path, corpus, [{
        "kernel": "a", "source": "kernels/a/a.c", "loop_header": None,
        "tier": "LLVM_BUILT", "status": "FAIL", "category": "LOOP_SELECTION",
        "owner": "FRONTEND", "diagnostic_code": "LOOP_SELECTION_FAILED",
    }])
    assert summary["reconciliation"]["ok"] is False
    assert summary["reconciliation"]["missing_loop_cases"] == ["kernels/a/a.c::kernel::loop"]


def test_failure_evidence_contains_metrics_reproducer_and_stage_hashes(tmp_path: Path) -> None:
    out = tmp_path / "run"
    artifact = out / "cases" / "case"
    (artifact / "frontend").mkdir(parents=True)
    (artifact / "frontend" / "generic_dfg.json").write_text("{}\n")
    corpus = tmp_path / "corpus"
    (corpus / "kernels" / "a").mkdir(parents=True)
    (corpus / "kernels" / "a" / "a.c").write_text("int a;\n")
    target = tmp_path / "target.json"
    target.write_text("{}\n")
    result = {
        "id": "kernels/a/a.c::kernel::loop",
        "source": "kernels/a/a.c",
        "artifact_directory": "cases/case",
        "source_artifact_directory": "cases/case",
        "status": "FAIL",
        "terminal_stage": "S10_MODULO_MAPPING",
        "category": "MAPPING_BUDGET",
        "owner": "MAPPER",
        "diagnostic_code": "MAPPING_BUDGET_EXCEEDED",
        "command": ["compiler", "input with space.ll"],
        "duration_ms": {"abi_backend": 12},
    }
    write_case_evidence(out, corpus, target, result, {"max_ii": 8})
    complete_stage_records(result)
    assert (artifact / "metrics.json").exists()
    reproducer = next((out / "reproducers" / "MAPPING_BUDGET_EXCEEDED").iterdir())
    assert (reproducer / "failure.json").exists()
    assert (reproducer / "command.txt").read_text() == "compiler 'input with space.ll'\n"
    generic_verify = next(stage for stage in result["stages"] if stage["stage"] == "S5_GENERIC_VERIFY")
    assert generic_verify["artifact_hashes"]


def test_known_supported_baseline_rejects_regression(tmp_path: Path) -> None:
    run = tmp_path / "run"
    run.mkdir()
    (run / "results.jsonl").write_text(json.dumps({
        "id": "case::loop", "tier": "FRONTEND_DFG", "diagnostic_code": "MAPPING_FAILED",
        "terminal_stage": "S10_MODULO_MAPPING", "category": "MAPPING_INFEASIBLE", "owner": "MAPPER",
    }) + "\n")
    baseline = tmp_path / "baseline.json"
    baseline.write_text(json.dumps({
        "schema": "cgra.cgra_bench.known_supported.v1",
        "cases": [{"id": "case::loop", "minimum_tier": "MAPPED"}],
    }))
    assert check(run, baseline) == [
        "case::loop: regressed to FRONTEND_DFG (MAPPING_FAILED); expected at least MAPPED"
    ]


def test_smoke_expectations_reject_classification_drift(tmp_path: Path) -> None:
    run = tmp_path / "run"
    run.mkdir()
    (run / "results.jsonl").write_text(json.dumps({
        "id": "case::loop", "terminal_stage": "S4_FRONTEND_LOWER",
        "diagnostic_code": "LLVM_FRONTEND_UNSUPPORTED_LOOP_SHAPE",
    }) + "\n")
    expectations = tmp_path / "expectations.json"
    expectations.write_text(json.dumps({
        "schema": "cgra.cgra_bench.smoke_expectations.v1",
        "cases": [{"id": "case::loop", "terminal_stage": "S4_FRONTEND_LOWER", "diagnostic_code": "LLVM_FRONTEND_UNSUPPORTED_LOOP_SHAPE"}],
    }))
    assert check_expectations(run, expectations) == []
