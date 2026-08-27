"""Contract tests for the T019 orchestration and reporting layer."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from tools.cgra_bench.classify import classify
from tools.cgra_bench.check_baseline import check, check_expectations
from tools.cgra_bench.evidence import complete_stage_records, write_case_evidence
from tools.cgra_bench.freeze_supported import freeze, load_completed_run
from tools.cgra_bench.functional import (
    FunctionalCaseError,
    load_cases,
    validate_case,
)
from tools.cgra_bench.inventory import PIN, inventory
from tools.cgra_bench.invocation import address_external_ids, synthesize_invocation
from tools.cgra_bench.report import report, target_contract_summary
from tools.cgra_bench.run import (
    MAPPING_PROFILES,
    active_functional_cases,
    apply_functional_cases,
    backend_observation,
    tier_from_compile,
)


ROOT = Path(__file__).resolve().parents[1]


def test_smoke_mapping_profile_is_bounded_below_full_audit() -> None:
    baseline = MAPPING_PROFILES["baseline"]
    smoke = MAPPING_PROFILES["smoke"]
    assert baseline == {
        "max_ii": 8,
        "max_node_candidates": 100000,
        "max_backtracks": 50000,
        "max_route_calls": 100000,
        "max_route_states": 10000,
    }
    assert 0 < smoke["max_ii"] < baseline["max_ii"]
    for budget in (
        "max_node_candidates",
        "max_backtracks",
        "max_route_calls",
        "max_route_states",
    ):
        assert 0 < smoke[budget] < baseline[budget]


def test_mapping_research_success_is_mapped_not_manifest_complete(tmp_path: Path) -> None:
    artifact = tmp_path / "abi"
    backend = artifact / "backend" / "backend"
    backend.mkdir(parents=True)
    (artifact / "kernel_compile_result.json").write_text(json.dumps({
        "schema": "cgra.kernel_compile.result.v1",
        "status": "success",
        "backend": {
            "schema": "cgra.compiler_pipeline.result.v1",
            "mode": "mapping_research",
            "status": "success",
            "mapping_status": "success",
            "hardware_executable": False,
            "physical_realizability": {
                "status": "infeasible",
                "reason_code": "rf_infeasible",
                "message": "fixed register overlap",
            },
            "stats": {"mii": 1, "mapped_ii": 2},
        },
    }))

    assert tier_from_compile(artifact, True) == ("MAPPED", "S10_MODULO_MAPPING")
    observation = backend_observation(artifact)
    assert observation["mapping_status"] == "success"
    assert observation["hardware_executable"] is False
    assert observation["physical_realizability"]["reason_code"] == "rf_infeasible"


def functional_spec(
    *,
    native_multiplier: int = 2,
    golden_multiplier: int = 2,
    rtl_multiplier: int = 2,
) -> dict[str, object]:
    """Create a command adapter case with three independent observations."""
    program = (
        "import json,sys; data=json.load(open(sys.argv[-2])); "
        "json.dump({'output': data['input'] * MULTIPLIER}, open(sys.argv[-1], 'w'))"
    )
    return {
        "id": "kernels/example.c::kernel::loop",
        "adapter": "command_observation_v1",
        "invocation": {
            "schema": "cgra.kernel_invocation.v1",
            "trip_count": 1,
            "scalar_inputs": {},
            "scratchpad_preload": [],
        },
        "inputs": {"input": 1},
        "expected": {"output": 2},
        "commands": {
            "native": [sys.executable, "-c", program.replace("MULTIPLIER", str(native_multiplier)), "{input}", "{native}"],
            "golden": [sys.executable, "-c", program.replace("MULTIPLIER", str(golden_multiplier)), "{manifest}", "{input}", "{golden}"],
            "rtl": [sys.executable, "-c", program.replace("MULTIPLIER", str(rtl_multiplier)), "{manifest}", "{input}", "{rtl}"],
        },
    }


def functional_result() -> dict[str, object]:
    return {
        "id": "kernels/example.c::kernel::loop",
        "artifact_directory": "case",
        "synthetic_invocation": False,
    }


def prepare_functional_manifest(tmp_path: Path) -> Path:
    out = tmp_path / "out"
    manifest = out / "case" / "abi" / "program_manifest.json"
    manifest.parent.mkdir(parents=True)
    manifest.write_text("{}\n")
    return out


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


def test_report_tracks_linear_multiblock_outcomes(tmp_path: Path) -> None:
    case_dir = tmp_path / "cases" / "a"
    case_dir.mkdir(parents=True)
    loop = {
        "function": "kernel",
        "header": "loop",
        "shape": {"kind": "linear_multiblock"},
    }
    (case_dir / "loop_inventory.json").write_text(json.dumps({
        "schema": "cgra.cgra_bench.loop_inventory.v1",
        "source": "kernels/a/a.c",
        "loops": [loop],
    }))
    corpus = {
        "denominator": {"kernel_directories": 1, "source_translation_units": 1},
        "sources": [{"path": "kernels/a/a.c", "enabled": True}],
    }
    summary = report(tmp_path, corpus, [{
        "id": "kernels/a/a.c::kernel::loop",
        "kernel": "a",
        "source": "kernels/a/a.c",
        "function": "kernel",
        "loop_header": "loop",
        "loop": loop,
        "tier": "MAPPED",
        "status": "FAIL",
        "category": "RF",
        "owner": "RF",
        "diagnostic_code": "RF_ALLOCATION_FAILED",
    }])
    assert summary["linear_multiblock"] == {
        "candidate_count": 1,
        "frontend_dfg_count": 1,
        "mapped_count": 1,
        "shape_rejection_count": 0,
    }
    assert summary["t020_outcome"] == {
        "frontend_dfg_or_higher": 1,
        "mapped_or_higher": 1,
        "mapped_kernel_directories": 1,
        "required_frontend_dfg_or_higher": 10,
        "required_mapped_or_higher": 8,
        "required_mapped_kernel_directories": 5,
        "pass": False,
    }


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


def test_freeze_supported_retains_only_l4_or_higher_cases() -> None:
    frozen = freeze(
        [
            {"id": "case-z", "tier": "MANIFEST_COMPLETE"},
            {"id": "case-a", "tier": "MAPPED"},
            {"id": "case-b", "tier": "TARGET_LEGAL"},
        ],
        {
            "project_sha": "project",
            "corpus_sha": "corpus",
            "target_sha256": "target",
            "profile": {"name": "baseline"},
        },
    )
    assert frozen["source"] == {
        "project_sha": "project",
        "corpus_sha": "corpus",
        "target_sha256": "target",
        "profile": "baseline",
    }
    assert frozen["cases"] == [
        {"id": "case-a", "minimum_tier": "MAPPED"},
        {"id": "case-z", "minimum_tier": "MAPPED"},
    ]


def test_freeze_supported_requires_complete_clean_audit(tmp_path: Path) -> None:
    run = tmp_path / "run"
    run.mkdir()
    result = {"id": "case-a", "tier": "MAPPED"}
    (run / "results.jsonl").write_text(json.dumps(result) + "\n")
    (run / "environment.json").write_text(json.dumps({
        "schema": "cgra.cgra_bench.environment.v1",
        "project_dirty": False,
    }))
    summary = {
        "schema": "cgra.cgra_bench.summary.v1",
        "reconciliation": {"ok": True},
        "unknown_count": 0,
        "timeout_count": 0,
        "denominator": {"terminal_results": 1},
    }
    (run / "summary.json").write_text(json.dumps(summary))
    assert load_completed_run(run)[0] == [result]

    summary["timeout_count"] = 1
    (run / "summary.json").write_text(json.dumps(summary))
    try:
        load_completed_run(run)
    except ValueError as error:
        assert "timeouts" in str(error)
    else:
        raise AssertionError("timed-out audit must not become the supported baseline")


def test_functional_manifest_rejects_unknown_adapter_and_missing_manifest_argument(tmp_path: Path) -> None:
    invalid = functional_spec()
    invalid["adapter"] = "unknown"
    manifest = tmp_path / "functional.json"
    manifest.write_text(json.dumps({"schema": "cgra.cgra_bench.functional_cases.v1", "cases": [invalid]}))
    try:
        load_cases(manifest)
    except FunctionalCaseError as error:
        assert error.code == "FUNCTIONAL_UNKNOWN_ADAPTER"
    else:
        raise AssertionError("unknown adapter must be rejected")

    invalid = functional_spec()
    commands = invalid["commands"]
    assert isinstance(commands, dict)
    commands["golden"] = [sys.executable, "-c", "pass"]
    manifest.write_text(json.dumps({"schema": "cgra.cgra_bench.functional_cases.v1", "cases": [invalid]}))
    try:
        load_cases(manifest)
    except FunctionalCaseError as error:
        assert error.code == "FUNCTIONAL_COMMANDS_INVALID"
    else:
        raise AssertionError("Golden must consume a compiler-generated manifest")

    invalid = functional_spec()
    commands = invalid["commands"]
    assert isinstance(commands, dict)
    commands["native"] = [sys.executable, "-c", "pass", "{input}", "{expected}", "{native}"]
    manifest.write_text(json.dumps({"schema": "cgra.cgra_bench.functional_cases.v1", "cases": [invalid]}))
    try:
        load_cases(manifest)
    except FunctionalCaseError as error:
        assert error.code == "FUNCTIONAL_ORACLE_LEAK"
    else:
        raise AssertionError("native adapter must not consume the expected oracle")


def test_functional_adapter_validates_native_golden_and_rtl(tmp_path: Path) -> None:
    out = prepare_functional_manifest(tmp_path)
    validation = validate_case(functional_spec(), functional_result(), tmp_path, out, 5)
    assert validation.ok
    assert validation.code == "FUNCTIONAL_RTL_VALIDATED"
    assert (out / "case" / "functional" / "golden.json").is_file()
    assert (out / "case" / "functional" / "rtl.json").is_file()


def test_functional_adapter_classifies_reference_golden_and_rtl_failures(tmp_path: Path) -> None:
    out = prepare_functional_manifest(tmp_path)
    native_mismatch = validate_case(
        functional_spec(native_multiplier=3), functional_result(), tmp_path, out, 5
    )
    assert native_mismatch.code == "FUNCTIONAL_NATIVE_MISMATCH"

    golden_mismatch = validate_case(
        functional_spec(golden_multiplier=3), functional_result(), tmp_path, out, 5
    )
    assert golden_mismatch.code == "FUNCTIONAL_GOLDEN_MISMATCH"

    rtl_mismatch = validate_case(
        functional_spec(rtl_multiplier=3), functional_result(), tmp_path, out, 5
    )
    assert rtl_mismatch.code == "FUNCTIONAL_RTL_MISMATCH"

    failed = functional_spec()
    commands = failed["commands"]
    assert isinstance(commands, dict)
    commands["native"] = [sys.executable, "-c", "raise SystemExit(3)", "{input}", "{native}"]
    command_failure = validate_case(failed, functional_result(), tmp_path, out, 5)
    assert command_failure.code == "FUNCTIONAL_COMMAND_FAILED"


def test_functional_adapter_rejects_missing_manifest_and_synthetic_result(tmp_path: Path) -> None:
    out = tmp_path / "out"
    missing_manifest = validate_case(functional_spec(), functional_result(), tmp_path, out, 5)
    assert missing_manifest.code == "FUNCTIONAL_MANIFEST_MISSING"

    out = prepare_functional_manifest(tmp_path)
    synthetic = functional_result()
    synthetic["synthetic_invocation"] = True
    validation = validate_case(functional_spec(), synthetic, tmp_path, out, 5)
    assert validation.code == "FUNCTIONAL_SYNTHETIC_INVOCATION_FORBIDDEN"


def test_functional_timeout_stays_distinct_and_subset_ignores_other_sources(tmp_path: Path) -> None:
    out = prepare_functional_manifest(tmp_path)
    spec = functional_spec()
    commands = spec["commands"]
    assert isinstance(commands, dict)
    commands["native"] = [
        sys.executable,
        "-c",
        "import time; time.sleep(1)",
        "{input}",
        "{native}",
    ]
    result = {
        **functional_result(),
        "status": "PASS",
        "tier": "MANIFEST_COMPLETE",
    }
    failures = apply_functional_cases([result], {spec["id"]: spec}, tmp_path, out, 0)
    assert failures
    assert result["category"] == "TIMEOUT"
    assert result["owner"] == "HARNESS"

    cases = {
        "kernels/a/a.c::kernel::loop": {},
        "kernels/b/b.c::kernel::loop": {},
    }
    assert set(active_functional_cases(cases, {"kernels/a/a.c"}, True)) == {
        "kernels/a/a.c::kernel::loop"
    }
    assert active_functional_cases(cases, {"kernels/a/a.c"}, False) == cases


def test_stage_records_include_functional_duration() -> None:
    result = {
        "terminal_stage": "S16_OPTIONAL_FUNCTIONAL_RTL",
        "status": "PASS",
        "diagnostic_code": "FUNCTIONAL_RTL_VALIDATED",
        "duration_ms": {"functional": 17},
    }
    complete_stage_records(result)
    functional = next(stage for stage in result["stages"] if stage["stage"] == "S16_OPTIONAL_FUNCTIONAL_RTL")
    assert functional["status"] == "PASS"
    assert functional["duration_ms"] == 17


def test_t019_workflows_run_feature_full_audit_and_hardware_regression() -> None:
    audit = (ROOT / ".github/workflows/cgra-bench-audit.yml").read_text()
    hardware = (ROOT / ".github/workflows/hardware-regression.yml").read_text()
    assert "compiler/cgra-bench-audit-v0" in audit
    assert "make cgra-bench-audit" in audit
    assert ".unknown_count == 0" in audit
    assert ".timeout_count == 0" in audit
    assert "compiler/cgra-bench-audit-v0" in hardware
    assert "tools/" in hardware
    assert r"\.github/workflows/.*\.yml" in hardware
