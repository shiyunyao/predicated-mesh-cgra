#!/usr/bin/env python3
"""Per-case stage, metrics, and reproducer evidence for T019 audits."""

from __future__ import annotations

import pathlib
import shlex
import shutil
from typing import Any

try:
    from .schemas import sha256_file, write_json
except ImportError:  # pragma: no cover - direct script execution
    from schemas import sha256_file, write_json


STAGES = [
    "S0_CORPUS_DISCOVERY",
    "S1_SOURCE_BUILD",
    "S2_LLVM_CANONICALIZE",
    "S3_LOOP_SELECTION",
    "S4_FRONTEND_LOWER",
    "S5_GENERIC_VERIFY",
    "S6_INVOCATION_SYNTHESIS",
    "S7_ABI_BIND",
    "S8_TARGET_LEGALIZE",
    "S9_MII_ANALYSIS",
    "S10_MODULO_MAPPING",
    "S11_STAGE_SCHEDULE",
    "S12_RF_ALLOCATION",
    "S13_MATERIALIZATION",
    "S14_TARGET_LOWERING",
    "S15_MANIFEST_VERIFY",
    "S16_OPTIONAL_FUNCTIONAL_RTL",
]


def complete_stage_records(result: dict[str, Any]) -> None:
    terminal = result.get("terminal_stage")
    terminal_index = STAGES.index(terminal) if terminal in STAGES else None
    final_status = result.get("status", "FAIL")
    durations = result.get("duration_ms", {})
    records = []
    for index, stage in enumerate(STAGES):
        if result.get("excluded"):
            status = "EXCLUDED" if index == 0 else "NOT_REACHED"
        elif terminal_index is None:
            status = "NOT_REACHED"
        elif index < terminal_index:
            status = "PASS"
        elif index == terminal_index:
            status = final_status
        else:
            status = (
                "NOT_RUN"
                if stage == "S16_OPTIONAL_FUNCTIONAL_RTL" and final_status == "PASS"
                else "NOT_REACHED"
            )
        duration = None
        if stage == "S1_SOURCE_BUILD":
            duration = durations.get("source_build")
        elif stage == "S2_LLVM_CANONICALIZE":
            duration = durations.get("llvm_build")
        elif stage == "S3_LOOP_SELECTION":
            duration = durations.get("loop_selection")
        elif stage == "S4_FRONTEND_LOWER":
            duration = durations.get("frontend")
        elif stage == "S16_OPTIONAL_FUNCTIONAL_RTL":
            duration = durations.get("functional")
        elif stage == terminal and terminal_index is not None and terminal_index >= 7:
            duration = durations.get("abi_backend")
        record = {
            "stage": stage,
            "status": status,
            "duration_ms": duration,
            "artifact_hashes": result.get("stage_artifact_hashes", {}).get(stage, {}),
        }
        if stage == terminal:
            record["diagnostic_code"] = result.get("diagnostic_code")
        records.append(record)
    result["stages"] = records


def evidence_status(result: dict[str, Any]) -> str:
    if result.get("status") == "PASS":
        return "success"
    if result.get("category") == "MAPPING_BUDGET":
        return "budget_exceeded"
    if result.get("category") == "MAPPING_INFEASIBLE":
        return "infeasible"
    if result.get("category") in {"INTERNAL", "TIMEOUT"}:
        return "internal_error"
    return "invalid_input"


def _stage_for_artifact(name: str, terminal_stage: str) -> str:
    normalized = name.removeprefix("source/")
    backend_name = pathlib.PurePosixPath(normalized).name
    if "/backend/backend/" in normalized:
        backend_stage = {
            "00_input.generic_dfg.json": "S5_GENERIC_VERIFY",
            "01_generic_dfg_verification.json": "S5_GENERIC_VERIFY",
            "02_legalization.json": "S8_TARGET_LEGALIZE",
            "03_target_dfg.json": "S8_TARGET_LEGALIZE",
            "04_target_dfg_verification.json": "S8_TARGET_LEGALIZE",
            "05_mii.json": "S9_MII_ANALYSIS",
            "06_mapper_report.json": "S10_MODULO_MAPPING",
            "07_modulo_mapping.json": "S10_MODULO_MAPPING",
            "08_modulo_mapping_verification.json": "S10_MODULO_MAPPING",
            "09_stage_report.json": "S11_STAGE_SCHEDULE",
            "10_staged_mapping.json": "S11_STAGE_SCHEDULE",
            "11_stage_verification.json": "S11_STAGE_SCHEDULE",
            "12_rf_report.json": "S12_RF_ALLOCATION",
            "13_rf_allocated_mapping.json": "S12_RF_ALLOCATION",
            "14_rf_verification.json": "S12_RF_ALLOCATION",
            "15_materialization_report.json": "S13_MATERIALIZATION",
            "16_materialized_schedule.json": "S13_MATERIALIZATION",
            "17_materialization_verification.json": "S13_MATERIALIZATION",
            "18_target_controls.json": "S14_TARGET_LOWERING",
            "19_target_control_verification.json": "S14_TARGET_LOWERING",
            "20_program_manifest.json": "S15_MANIFEST_VERIFY",
            "21_lowering_report.json": "S14_TARGET_LOWERING",
            "compiler_pipeline_report.json": terminal_stage,
        }
        if backend_name in backend_stage:
            return backend_stage[backend_name]
    if normalized.startswith("build/source.raw") or normalized.startswith("build/source-build"):
        return "S1_SOURCE_BUILD"
    if normalized.startswith("build/") or normalized == "build.json":
        return "S2_LLVM_CANONICALIZE"
    if normalized in {"loop_inventory.json", "llvm_feature_inventory.json"}:
        return "S3_LOOP_SELECTION"
    if name.startswith("frontend/"):
        return "S5_GENERIC_VERIFY" if "generic_dfg" in name or "verification" in name else "S4_FRONTEND_LOWER"
    if name == "invocation.json":
        return "S6_INVOCATION_SYNTHESIS"
    if name.startswith("abi/") and terminal_stage in STAGES:
        return terminal_stage
    return terminal_stage if terminal_stage in STAGES else "S0_CORPUS_DISCOVERY"


def write_case_evidence(out: pathlib.Path, corpus: pathlib.Path, target: pathlib.Path,
                        result: dict[str, Any], mapping_profile: dict[str, int]) -> None:
    if result.get("excluded"):
        return
    artifact_root = out / result.get("artifact_directory", "")
    source_root = out / result.get("source_artifact_directory", result.get("artifact_directory", ""))
    artifact_hashes = {}
    if artifact_root.is_dir():
        for path in sorted(artifact_root.rglob("*")):
            if path.is_file():
                artifact_hashes[path.relative_to(artifact_root).as_posix()] = sha256_file(path)
    if source_root.is_dir() and source_root != artifact_root:
        for path in sorted(source_root.rglob("*")):
            if path.is_file() and "loops" not in path.relative_to(source_root).parts:
                artifact_hashes[f"source/{path.relative_to(source_root).as_posix()}"] = sha256_file(path)
    result["artifact_hashes"] = artifact_hashes
    stage_hashes: dict[str, dict[str, str]] = {stage: {} for stage in STAGES}
    for name, digest in artifact_hashes.items():
        stage_hashes[_stage_for_artifact(name, result.get("terminal_stage", ""))][name] = digest
    result["stage_artifact_hashes"] = stage_hashes

    wall_time = sum(value for value in result.get("duration_ms", {}).values() if isinstance(value, int))
    backend = result.get("backend") if isinstance(result.get("backend"), dict) else {}
    backend_stats = backend.get("stats", {})
    metrics = {
        "schema": "cgra.compiler.metrics.v1",
        "case": result["id"],
        "component": "cgra_bench_audit",
        "status": evidence_status(result),
        "seed": 0,
        "wall_time_ms": wall_time,
        "metrics": {**backend_stats, "terminal_stage": result.get("terminal_stage")},
    }
    metrics_path = artifact_root / "metrics.json"
    write_json(metrics_path, metrics)
    artifact_hashes["metrics.json"] = sha256_file(metrics_path)
    stage_hashes.setdefault(result.get("terminal_stage", "S0_CORPUS_DISCOVERY"), {})[
        "metrics.json"
    ] = artifact_hashes["metrics.json"]
    result["artifact_hashes"] = artifact_hashes
    if result.get("status") == "PASS":
        return

    safe_case = result["id"].replace("/", "_").replace("::", "__")
    reproducer = out / "reproducers" / result.get("diagnostic_code", "UNCLASSIFIED") / safe_case
    reproducer.mkdir(parents=True, exist_ok=True)
    write_json(reproducer / "failure.json", {
        "schema": "cgra.compiler.failure.v1",
        "test": result["id"],
        "component": result.get("owner", "UNKNOWN").lower(),
        "reason": result.get("diagnostic_code", "UNCLASSIFIED_RESULT").lower(),
        "seed": 0,
        "status": "failure",
        "terminal_stage": result.get("terminal_stage"),
    })
    write_json(reproducer / "config.json", result.get("audit_profile", {"mapping": mapping_profile}))
    environment = {}
    environment_path = out / "environment.json"
    if environment_path.exists():
        try:
            import json
            environment = json.loads(environment_path.read_text(encoding="utf-8"))
        except (OSError, ValueError, json.JSONDecodeError):
            environment = {}
    write_json(reproducer / "context.json", {
        "compiler_sha": environment.get("project_sha"),
        "corpus_sha": environment.get("corpus_sha"),
        "target_sha256": environment.get("target_sha256"),
        "diagnostic_code": result.get("diagnostic_code"),
        "terminal_stage": result.get("terminal_stage"),
        "message": result.get("message", "")[-4000:],
        "stdout": result.get("stdout", "")[-4000:],
        "stderr": result.get("stderr", "")[-4000:],
    })
    (reproducer / "seed.txt").write_text("0\n", encoding="utf-8")
    shutil.copy2(target, reproducer / "target.json")
    source = corpus / result["source"]
    if source.is_file():
        shutil.copy2(source, reproducer / source.name)
    canonical = source_root / "build" / "source.canonical.ll"
    if canonical.is_file():
        shutil.copy2(canonical, reproducer / "input.ll")
    dfg = artifact_root / "frontend" / "generic_dfg.json"
    if dfg.is_file():
        shutil.copy2(dfg, reproducer / "input.generic_dfg.json")
    logs = reproducer / "logs"
    for path in sorted(source_root.rglob("*.log")):
        if path.is_file():
            logs.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, logs / path.name)
    for path in sorted(artifact_root.rglob("*.log")):
        if path.is_file():
            logs.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, logs / path.name)
    project_root = corpus.parent.parent
    portable_command = []
    for token in result.get("command", []):
        candidate = pathlib.Path(token)
        if candidate.is_absolute() and candidate.is_relative_to(project_root):
            portable_command.append(candidate.relative_to(project_root).as_posix())
        else:
            portable_command.append(token)
    (reproducer / "command.txt").write_text(
        shlex.join(portable_command) + "\n", encoding="utf-8"
    )
    write_json(reproducer / "metrics.json", metrics)
