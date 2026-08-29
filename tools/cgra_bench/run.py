#!/usr/bin/env python3
"""Run the T019 audit without aborting when an individual case fails."""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import time
import platform
from datetime import datetime, timezone
from typing import Any

try:
    from .build_llvm import build
    from .admissibility import attach_terminal_status
    from .classify import classify
    from .evidence import STAGES, complete_stage_records, write_case_evidence
    from .functional import FunctionalCaseError, build_kernel_invocation, load_cases, validate_case
    from .inventory import PIN, inventory
    from .invocation import InvocationSynthesisError, synthesize_invocation
    from .report import report
    from .schemas import read_json, sha256_file, write_json
except ImportError:  # pragma: no cover - direct script execution
    from build_llvm import build
    from admissibility import attach_terminal_status
    from classify import classify
    from evidence import STAGES, complete_stage_records, write_case_evidence
    from functional import FunctionalCaseError, build_kernel_invocation, load_cases, validate_case
    from inventory import PIN, inventory
    from invocation import InvocationSynthesisError, synthesize_invocation
    from report import report
    from schemas import read_json, sha256_file, write_json


BASELINE_MAPPING_PROFILE = {
    "max_ii": 8,
    "max_node_candidates": 100000,
    "max_backtracks": 50000,
    "max_route_calls": 100000,
    "max_route_states": 10000,
}
MAPPING_PROFILES = {
    "baseline": BASELINE_MAPPING_PROFILE,
    "research": {
        # Keep the profile below the known pathological MII=49 cases so they
        # terminate through the mapper's explicit budget result instead of
        # consuming the external audit timeout.
        "max_ii": 48,
        "max_node_candidates": 100000,
        "max_backtracks": 50000,
        "max_route_calls": 100000,
        "max_route_states": 10000,
    },
    "smoke": {
        "max_ii": 4,
        "max_node_candidates": 500,
        "max_backtracks": 500,
        "max_route_calls": 1000,
        "max_route_states": 500,
    },
}


def tool_version(executable: str) -> str | None:
    path = shutil.which(executable)
    if not path:
        return None
    try:
        return subprocess.check_output([path, "--version"], text=True, stderr=subprocess.STDOUT).splitlines()[0]
    except (OSError, subprocess.CalledProcessError, IndexError):
        return None


def command_result(command: list[str], cwd: pathlib.Path, timeout: int) -> tuple[int, str, str, int]:
    started = time.monotonic()
    try:
        completed = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=timeout)
        return completed.returncode, completed.stdout, completed.stderr, int((time.monotonic() - started) * 1000)
    except subprocess.TimeoutExpired as error:
        return 124, error.stdout or "", error.stderr or "", int((time.monotonic() - started) * 1000)


def result_for_failure(case: dict[str, Any], stage: str, message: str, returncode: int = 1,
                       diagnostic_code: str | None = None,
                       tier: str = "DISCOVERED") -> dict[str, Any]:
    failure = classify(stage, message, returncode)
    if diagnostic_code:
        failure["diagnostic_code"] = diagnostic_code
    return {"id": case["id"], "kernel": case["kernel"], "source": case["source"], "loop_header": None, "tier": tier, "terminal_stage": stage, "status": "FAIL", "stages": [{"stage": stage, "status": "FAIL"}], "message": message[-4000:], **failure}


def tier_from_compile(artifact_dir: pathlib.Path, frontend_ok: bool) -> tuple[str, str]:
    if not frontend_ok:
        return "DISCOVERED", "FRONTEND"
    result_path = artifact_dir / "kernel_compile_result.json"
    if not result_path.exists():
        return "FRONTEND_DFG", "S7_ABI_BIND"
    try:
        result = read_json(result_path)
        backend = result.get("backend") or {}
        status = backend.get("status", result.get("status", ""))
        if status == "success":
            if backend.get("mode") == "mapping_research":
                if backend.get("mapping_status") == "rf_constrained_success":
                    return "RF_CONSTRAINED_MAPPED", "S12_RF_ALLOCATION"
                return "ROUTE_MAPPED", "S10_MODULO_MAPPING"
            return "MANIFEST_COMPLETE", "SUCCESS"
        if status == "invalid_invocation":
            return "FRONTEND_DFG", "S7_ABI_BIND"
        if status in {"invalid_source_dfg", "generic_dfg_verification_failure"}:
            return "FRONTEND_DFG", "S5_GENERIC_VERIFY"
        if status in {"target_legalization_failure", "target_dfg_verification_failure"}:
            return "FRONTEND_DFG", "S8_TARGET_LEGALIZE"
        if "legalization" in status:
            return "FRONTEND_DFG", "S8_TARGET_LEGALIZE"
        if "mii" in status:
            return "TARGET_LEGAL", "S9_MII_ANALYSIS"
        if status in {"rf_constrained_mapping_failure", "rf_constrained_mapping_budget_failure"}:
            # A completed route candidate is still an L4 observation even if
            # every candidate was rejected by finite stage/RF feasibility.
            if backend.get("raw_mapping_found"):
                return "ROUTE_MAPPED", "S10_MODULO_MAPPING"
            return "TARGET_LEGAL", "S10_MODULO_MAPPING"
        if "rf_constrained" in status:
            return "TARGET_LEGAL", "S10_MODULO_MAPPING"
        if "mapping" in status:
            return "TARGET_LEGAL", "S10_MODULO_MAPPING"
        if "stage" in status:
            return "MAPPED", "S11_STAGE_SCHEDULE"
        if "rf" in status:
            return "MAPPED", "S12_RF_ALLOCATION"
        if "material" in status:
            return "RF_COMPLETE", "S13_MATERIALIZATION"
        if "lowering" in status or "control" in status:
            return "RF_COMPLETE", "S14_TARGET_LOWERING"
        if "manifest" in status:
            return "RF_COMPLETE", "S15_MANIFEST_VERIFY"
    except (OSError, ValueError, json.JSONDecodeError):
        pass
    return "FRONTEND_DFG", "INTERNAL"


def backend_observation(artifact_dir: pathlib.Path) -> dict[str, Any]:
    result_path = artifact_dir / "kernel_compile_result.json"
    if not result_path.exists():
        return {}
    try:
        result = read_json(result_path)
    except (OSError, ValueError, json.JSONDecodeError):
        return {}
    backend = result.get("backend") or {}
    stats = dict(backend.get("stats", {}))
    backend_dir = artifact_dir / "backend" / "backend"
    if not backend_dir.is_dir():
        backend_dir = artifact_dir / "backend"
    for filename in ("05_mii.json", "06_mapper_report.json", "09_stage_report.json", "12_rf_report.json", "15_materialization_report.json"):
        path = backend_dir / filename
        if not path.exists():
            continue
        try:
            report = read_json(path)
        except (OSError, ValueError, json.JSONDecodeError):
            continue
        report_stats = report.get("stats", {})
        if filename == "05_mii.json":
            stats.update({
                "resource_mii": report.get("resource_mii"),
                "recurrence_mii": report.get("recurrence_mii"),
                "mii_witness": report.get("recurrence_witness"),
            })
        elif filename == "06_mapper_report.json":
            for key in (
                "completed_modulo_mappings", "post_mapping_rejected", "stage_rejected",
                "rf_rejected", "rf_rejected_by_ii", "rf_rejected_by_reason", "ii_attempts", "backtracks",
                "route_search_calls", "route_no_paths", "route_budget_exceeded",
                "successful_placements", "rejected_placements",
            ):
                if key in report_stats:
                    stats[key] = report_stats[key]
            stats["mapping_diagnostics"] = len(report.get("diagnostics", []))
        elif filename == "12_rf_report.json":
            stats.update({f"rf_{key}": value for key, value in report_stats.items()})
        else:
            stats.update({f"{filename[:-5]}_{key}": value for key, value in report_stats.items()})
    return {
        "status": backend.get("status", result.get("status")),
        "mode": backend.get("mode", "hardware_executable"),
        "mapping_status": backend.get("mapping_status", "not_available"),
        "raw_mapping_found": bool(backend.get("raw_mapping_found", stats.get("completed_modulo_mappings", 0))),
        "rf_constrained_mapping_found": bool(backend.get("rf_constrained_mapping_found", stats.get("rf_constrained_mappings", 0))),
        "hardware_executable": bool(backend.get("hardware_executable", False)),
        "physical_realizability": backend.get("physical_realizability", {"status": "not_run"}),
        "message": backend.get("message", result.get("message", ""))[-4000:],
        "stats": stats,
    }


def run_case(case: dict[str, Any], root: pathlib.Path, corpus: pathlib.Path, out: pathlib.Path,
             target: pathlib.Path, frontend_bin: pathlib.Path, compile_bin: pathlib.Path,
             timeout: int, functional_cases: dict[str, dict[str, Any]],
             mapping_profile: dict[str, int], pipeline_lane: str = "hardware",
             source_abi: str = "m32", mapping_objective: str = "optimize-ii",
             normalize_recurrence_ingress: bool = False) -> list[dict[str, Any]]:
    if not case.get("enabled", True):
        return [{"id": case["id"], "kernel": case["kernel"], "source": case["source"], "loop_header": None, "tier": "DISCOVERED", "terminal_stage": "S0_CORPUS_DISCOVERY", "status": "EXCLUDED", "stages": [{"stage": "S0_CORPUS_DISCOVERY", "status": "EXCLUDED"}], "category": "CORPUS", "owner": "HARNESS", "diagnostic_code": "EXPLICIT_EXCLUSION", "message": case.get("exclusion", "explicitly excluded"), "excluded": True}]
    source = corpus / case["source"]
    try:
        target_description = read_json(target)
        address_unit = target_description.get("memory", {}).get("address_unit", "word")
        address_unit_bytes = {"byte": 1, "word": 4}[address_unit]
    except (OSError, ValueError, json.JSONDecodeError, KeyError) as error:
        return [result_for_failure(case, "S4_FRONTEND_LOWER", f"invalid target address unit: {error}")]
    source_out = out / "cases" / case["id"].replace("/", "_").replace("::", "__")
    source_out.mkdir(parents=True, exist_ok=True)
    flags = list(case.get("compile_flags", []))
    flags.extend(f"-D{define}" for define in case.get("defines", []))
    flags.extend(f"-I{(corpus / include).resolve()}" for include in case.get("include_dirs", []))
    source_result = build(source, source_out / "build", timeout, flags, source_abi)
    write_json(source_out / "build.json", source_result)
    if source_result.get("status") != "LLVM_BUILT":
        canonicalization_failed = source_result.get("status") == "LLVM_CANONICALIZE_FAILED"
        stage = "S2_LLVM_CANONICALIZE" if canonicalization_failed else "S1_SOURCE_BUILD"
        result = result_for_failure(
            case,
            stage,
            source_result.get("message", "source build failed"),
            diagnostic_code=source_result.get("diagnostic_code"),
            tier="LLVM_BUILT" if canonicalization_failed else "DISCOVERED",
        )
        commands = source_result.get("commands", {})
        command_key = "canonicalize" if canonicalization_failed else "compile"
        result.update({
            "artifact_directory": source_out.relative_to(out).as_posix(),
            "source_artifact_directory": source_out.relative_to(out).as_posix(),
            "command": commands.get(command_key, []),
            "duration_ms": {
                "llvm_build" if canonicalization_failed else "source_build": source_result.get("duration_ms")
            },
        })
        return [result]
    canonical = pathlib.Path(source_result["ir"])
    rc, stdout, stderr, duration = command_result([str(frontend_bin), str(canonical), "--list-loops", "--json"], root, timeout)
    if rc != 0:
        result = result_for_failure(case, "S3_LOOP_SELECTION", stderr or stdout, rc, tier="LLVM_BUILT")
        result.update({"artifact_directory": source_out.relative_to(out).as_posix(), "source_artifact_directory": source_out.relative_to(out).as_posix(), "command": [str(frontend_bin), str(canonical), "--list-loops", "--json"], "duration_ms": {"llvm_build": source_result.get("duration_ms"), "loop_selection": duration}})
        return [result]
    try:
        loops = json.loads(stdout)["loops"]
    except (ValueError, KeyError, json.JSONDecodeError) as error:
        result = result_for_failure(case, "S3_LOOP_SELECTION", f"invalid loop inventory: {error}", tier="LLVM_BUILT")
        result.update({"artifact_directory": source_out.relative_to(out).as_posix(), "source_artifact_directory": source_out.relative_to(out).as_posix(), "command": [str(frontend_bin), str(canonical), "--list-loops", "--json"], "duration_ms": {"llvm_build": source_result.get("duration_ms"), "loop_selection": duration}})
        return [result]
    write_json(source_out / "llvm_feature_inventory.json", {
        "schema": "cgra.cgra_bench.llvm_feature_inventory.v1",
        "source": case["source"],
        "loops": [
            {
                "function": loop["function"],
                "header": loop["header"],
                "features": loop.get("features", {}),
            }
            for loop in loops
        ],
    })
    write_json(source_out / "loop_inventory.json", {"schema": "cgra.cgra_bench.loop_inventory.v1", "source": case["source"], "loops": loops})
    if not loops:
        result = result_for_failure(case, "S3_LOOP_SELECTION", "no innermost loop found", diagnostic_code="NO_INNERMOST_LOOP", tier="LLVM_BUILT")
        result.update({"artifact_directory": source_out.relative_to(out).as_posix(), "source_artifact_directory": source_out.relative_to(out).as_posix(), "command": [str(frontend_bin), str(canonical), "--list-loops", "--json"], "duration_ms": {"llvm_build": source_result.get("duration_ms"), "loop_selection": duration}})
        return [result]
    results = []
    for loop in loops:
        loop_case = dict(case)
        loop_case["id"] = f"{case['source']}::{loop['function']}::{loop['header']}"
        functional_spec = functional_cases.get(loop_case["id"])
        loop_out = source_out / "loops" / f"{loop['function']}__{loop['header']}"
        frontend_out = loop_out / "frontend"
        frontend_out.mkdir(parents=True, exist_ok=True)
        dfg_path = frontend_out / "generic_dfg.json"
        lower_command = [str(frontend_bin), str(canonical), "--function", loop["function"], "--loop-header", loop["header"], "--address-unit-bytes", str(address_unit_bytes), "--artifact-dir", str(frontend_out), "-o", str(dfg_path)]
        rc, stdout, stderr, lower_duration = command_result(lower_command, root, timeout)
        base = {
            "id": loop_case["id"], "kernel": case["kernel"], "source": case["source"],
            "function": loop["function"], "loop_header": loop["header"],
            "feature": loop.get("features", {}), "loop": loop,
            "duration_ms": {"llvm_build": source_result.get("duration_ms"), "loop_selection": duration, "frontend": lower_duration},
            "synthetic_invocation": functional_spec is None,
            "functional_required": functional_spec is not None,
            "functional_adapter": functional_spec.get("adapter") if functional_spec else None,
            "status": "FAIL",
            "artifact_directory": loop_out.relative_to(out).as_posix(),
            "source_artifact_directory": source_out.relative_to(out).as_posix(),
            "command": lower_command,
            "stages": [{"stage": "S0_CORPUS_DISCOVERY", "status": "PASS"}, {"stage": "S1_SOURCE_BUILD", "status": "PASS"}, {"stage": "S2_LLVM_CANONICALIZE", "status": "PASS"}, {"stage": "S3_LOOP_SELECTION", "status": "PASS"}],
        }
        if rc != 0 or not dfg_path.exists():
            failure = classify("S4_FRONTEND_LOWER", stderr or stdout, rc)
            results.append({**base, "tier": "LLVM_BUILT", "terminal_stage": "S4_FRONTEND_LOWER", "stages": [*base["stages"], {"stage": "S4_FRONTEND_LOWER", "status": "FAIL"}], "message": (stderr or stdout)[-4000:], "stdout": stdout[-4000:], "stderr": stderr[-4000:], **failure})
            continue
        try:
            dfg = read_json(dfg_path)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            results.append({**base, "tier": "LLVM_BUILT", "terminal_stage": "S5_GENERIC_VERIFY", "stages": [*base["stages"], {"stage": "S4_FRONTEND_LOWER", "status": "PASS"}, {"stage": "S5_GENERIC_VERIFY", "status": "FAIL"}], "message": str(error), "stdout": stdout[-4000:], "stderr": stderr[-4000:], **classify("S5_GENERIC_VERIFY", str(error))})
            continue
        memory_analysis = {}
        memory_analysis_path = frontend_out / "02_memory_analysis.json"
        if memory_analysis_path.exists():
            try:
                memory_analysis = read_json(memory_analysis_path)
            except (OSError, ValueError, json.JSONDecodeError) as error:
                results.append({**base, "tier": "FRONTEND_DFG", "terminal_stage": "S5_GENERIC_VERIFY", "message": str(error), **classify("S5_GENERIC_VERIFY", str(error))})
                continue
        edge_kinds: dict[str, int] = {}
        distances: dict[str, int] = {}
        operation_histogram: dict[str, int] = {}
        for edge in dfg.get("edges", []):
            kind = edge.get("kind", "unknown")
            edge_kinds[kind] = edge_kinds.get(kind, 0) + 1
            distance = str(edge.get("distance", 0))
            distances[distance] = distances.get(distance, 0) + 1
        for node in dfg.get("nodes", []):
            opcode = node.get("opcode", "unknown")
            operation_key = node.get("operation_key") if opcode == "Custom" else opcode
            operation_key = operation_key or opcode
            operation_histogram[operation_key] = operation_histogram.get(operation_key, 0) + 1
        base["dfg"] = {
            "node_count": len(dfg.get("nodes", [])), "edge_count": len(dfg.get("edges", [])),
            "external_values": len(dfg.get("external_values", [])), "liveouts": len(dfg.get("live_outs", [])),
            "opcode_histogram": {opcode: sum(node.get("opcode") == opcode for node in dfg.get("nodes", [])) for opcode in sorted({node.get("opcode") for node in dfg.get("nodes", [])})},
            "operation_histogram": dict(sorted(operation_histogram.items())),
            "edge_kind_histogram": dict(sorted(edge_kinds.items())),
            "distance_histogram": dict(sorted(distances.items(), key=lambda item: int(item[0]))),
            "recurrence_edges": sum(edge.get("distance", 0) > 0 and edge.get("kind") in {"data", "predicate"} for edge in dfg.get("edges", [])),
            "memory_edges": sum(edge.get("kind") == "memory" for edge in dfg.get("edges", [])),
            "max_recurrence_distance": max((edge.get("distance", 0) for edge in dfg.get("edges", []) if edge.get("kind") in {"data", "predicate"}), default=0),
            "max_memory_distance": max((edge.get("distance", 0) for edge in dfg.get("edges", []) if edge.get("kind") == "memory"), default=0),
        }
        try:
            target_spec = read_json(target)
            invocation = (
                build_kernel_invocation(functional_spec)
                if functional_spec
                else synthesize_invocation(
                    dfg,
                    int(loop.get("static_trip_count") or 4),
                    memory_analysis,
                    int(target_spec.get("memory", {}).get("depth", 0)) or None,
                )
            )
        except (InvocationSynthesisError, FunctionalCaseError) as error:
            results.append({**base, "tier": "FRONTEND_DFG", "terminal_stage": "S6_INVOCATION_SYNTHESIS", "message": str(error), "stdout": "", "stderr": "", **classify("S6_INVOCATION_SYNTHESIS", str(error))})
            continue
        write_json(loop_out / "invocation.json", invocation)
        abi_out = loop_out / "abi"
        compiler_output = abi_out / (
            "modulo_mapping.json" if pipeline_lane == "mapping-research" else "program_manifest.json"
        )
        compile_command = [
            str(compile_bin),
            str(dfg_path),
            "--target",
            str(target),
            "--invocation",
            str(loop_out / "invocation.json"),
            "--artifact-dir",
            str(abi_out),
            "--kernel-name",
            loop_case["id"],
            "-o",
            str(compiler_output),
            "--max-ii",
            str(mapping_profile["max_ii"]),
            "--max-node-candidates",
            str(mapping_profile["max_node_candidates"]),
            "--max-backtracks",
            str(mapping_profile["max_backtracks"]),
            "--max-route-calls",
            str(mapping_profile["max_route_calls"]),
            "--max-route-states",
            str(mapping_profile["max_route_states"]),
        ]
        if pipeline_lane == "mapping-research":
            compile_command.extend(["--mode", "mapping-research"])
            if normalize_recurrence_ingress:
                compile_command.append("--enable-recurrence-ingress")
            if mapping_objective == "find-any-feasible":
                compile_command.extend([
                    "--mapping-objective", "find-any-feasible",
                    "--enable-feasibility-fallback",
                ])
        rc, stdout, stderr, compile_duration = command_result(compile_command, root, timeout)
        base["command"] = compile_command
        tier, stage = tier_from_compile(abi_out, True)
        backend = backend_observation(abi_out)
        if rc != 0:
            classification_message = " ".join(
                part for part in [str(backend.get("status", "")), backend.get("message", ""), stderr or stdout]
                if part
            )
            failure = classify(stage, classification_message, rc)
            results.append({**base, "tier": tier, "terminal_stage": stage, "stages": [*base["stages"], {"stage": "S4_FRONTEND_LOWER", "status": "PASS"}, {"stage": "S5_GENERIC_VERIFY", "status": "PASS"}, {"stage": stage, "status": "FAIL"}], "message": (stderr or stdout)[-4000:], "stdout": stdout[-4000:], "stderr": stderr[-4000:], "duration_ms": {**base["duration_ms"], "abi_backend": compile_duration}, "backend": backend, **failure})
        elif pipeline_lane == "mapping-research":
            # ``success`` was used by pre-RF research runs and may describe a
            # raw placement/routing candidate.  A strict result must carry the
            # explicit finite-RF status so raw mappings cannot become L5 by
            # compatibility fallback.
            strict_mapping = backend.get("mapping_status") == "rf_constrained_success"
            results.append({**base,
                            "tier": "RF_CONSTRAINED_MAPPED" if strict_mapping else "ROUTE_MAPPED",
                            "terminal_stage": "S12_RF_ALLOCATION" if strict_mapping else "S10_MODULO_MAPPING",
                            "status": "PASS",
                            "stages": [*base["stages"], {"stage": "S4_FRONTEND_LOWER", "status": "PASS"}, {"stage": "S5_GENERIC_VERIFY", "status": "PASS"}, {"stage": "S7_ABI_BIND", "status": "PASS"}, {"stage": "S8_TARGET_LEGALIZE", "status": "PASS"}, {"stage": "S9_MII_ANALYSIS", "status": "PASS"}, {"stage": "S10_MODULO_MAPPING", "status": "PASS"}, {"stage": "S11_STAGE_SCHEDULE", "status": "PASS" if strict_mapping else "NOT_RUN"}, {"stage": "S12_RF_ALLOCATION", "status": "PASS" if strict_mapping else "NOT_RUN"}],
                            "diagnostic_code": "RF_CONSTRAINED_MAPPING_VERIFIED" if strict_mapping else "ROUTE_MAPPING_VERIFIED",
                            "category": "MAPPING" if strict_mapping else "MAPPING",
                            "owner": "MAPPER",
                            "message": stdout[-2000:],
                            "duration_ms": {**base["duration_ms"], "abi_backend": compile_duration},
                            "backend": backend,
                            "artifacts": [str(path) for path in abi_out.rglob("*") if path.is_file()]})
        else:
            results.append({**base, "tier": tier, "terminal_stage": "S15_MANIFEST_VERIFY", "status": "PASS", "stages": [*base["stages"], {"stage": "S4_FRONTEND_LOWER", "status": "PASS"}, {"stage": "S5_GENERIC_VERIFY", "status": "PASS"}, {"stage": "S7_ABI_BIND", "status": "PASS"}, {"stage": "S8_TARGET_LEGALIZE", "status": "PASS"}, {"stage": "S15_MANIFEST_VERIFY", "status": "PASS"}], "diagnostic_code": "SUCCESS", "category": "MANIFEST", "owner": "LOWERING", "message": stdout[-2000:], "duration_ms": {**base["duration_ms"], "abi_backend": compile_duration}, "backend": backend, "artifacts": [str(path) for path in abi_out.rglob("*") if path.is_file()]})
    return results


def apply_functional_cases(results: list[dict[str, Any]], functional_cases: dict[str, dict[str, Any]],
                           root: pathlib.Path, out: pathlib.Path, timeout: int) -> list[str]:
    """Run S16 only for declared, non-synthetic cases that reached a manifest."""
    result_by_id = {result["id"]: result for result in results}
    missing = sorted(set(functional_cases) - set(result_by_id))
    if missing:
        raise FunctionalCaseError(
            "FUNCTIONAL_CASE_NOT_DISCOVERED",
            f"functional cases did not appear in this audit: {', '.join(missing)}",
        )
    failures = []
    for case_id, spec in functional_cases.items():
        result = result_by_id[case_id]
        if result.get("status") != "PASS":
            failures.append(f"{case_id}: did not reach S15 ({result.get('diagnostic_code')})")
            continue
        validation = validate_case(spec, result, root, out, timeout)
        result.setdefault("duration_ms", {})["functional"] = validation.duration_ms
        result["terminal_stage"] = "S16_OPTIONAL_FUNCTIONAL_RTL"
        result["functional"] = {
            "adapter": spec["adapter"],
            "code": validation.code,
            "message": validation.message,
        }
        if validation.ok:
            result.update({
                "tier": "FUNCTIONAL_RTL_VALIDATED",
                "status": "PASS",
                "category": "RTL",
                "owner": "HARDWARE",
                "diagnostic_code": validation.code,
                "message": validation.message,
            })
            continue
        timeout_failure = validation.code == "FUNCTIONAL_TIMEOUT"
        result.update({
            "tier": "MANIFEST_COMPLETE",
            "status": "FAIL",
            "category": "TIMEOUT" if timeout_failure else "RTL",
            "owner": "HARNESS" if timeout_failure else "HARDWARE",
            "diagnostic_code": validation.code,
            "message": validation.message,
        })
        failures.append(f"{case_id}: {validation.code}")
    return failures


def active_functional_cases(functional_cases: dict[str, dict[str, Any]], case_sources: set[str],
                            allow_subset: bool) -> dict[str, dict[str, Any]]:
    """Select functional cases relevant to this corpus view."""
    if not allow_subset:
        return functional_cases
    return {
        case_id: spec
        for case_id, spec in functional_cases.items()
        if case_id.partition("::")[0] in case_sources
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=pathlib.Path, default=pathlib.Path("third_party/CGRA-Bench"))
    parser.add_argument("--target", type=pathlib.Path, default=pathlib.Path("target/cgra_v3.json"))
    parser.add_argument("--cases", type=pathlib.Path, default=pathlib.Path("benchmarks/cgra-bench/cases.v1.json"))
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("build/cgra-bench/run"))
    parser.add_argument("--frontend-bin", type=pathlib.Path, default=pathlib.Path("build/compiler-llvm/bin/cgra-llvm-loop-lower"))
    parser.add_argument("--compile-kernel-bin", type=pathlib.Path, default=pathlib.Path("build/compiler/bin/cgrac-compile-kernel"))
    parser.add_argument(
        "--functional-cases",
        type=pathlib.Path,
        default=pathlib.Path("benchmarks/cgra-bench/functional_cases.v1.json"),
    )
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--profile", choices=sorted(MAPPING_PROFILES), default="baseline")
    parser.add_argument("--lane", choices=("hardware", "mapping-research"), default="hardware")
    parser.add_argument("--mapping-objective", choices=("optimize-ii", "find-any-feasible"), default="optimize-ii")
    parser.add_argument("--enable-recurrence-ingress", action="store_true")
    parser.add_argument(
        "--source-abi",
        choices=("m32", "native"),
        default="m32",
        help="LLVM source ABI profile; native is explicit and never an automatic m32 fallback",
    )
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--allow-subset", action="store_true", help="allow a smoke manifest to cover a corpus subset")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    corpus = (root / args.corpus).resolve()
    cases_path = (root / args.cases).resolve()
    out = (root / args.out).resolve()
    target = (root / args.target).resolve()
    frontend_bin = (root / args.frontend_bin).resolve()
    compile_bin = (root / args.compile_kernel_bin).resolve()
    functional_cases_path = (root / args.functional_cases).resolve()
    mapping_profile = MAPPING_PROFILES[args.profile]
    try:
        if not args.all:
            parser.error("--all is required for an audit run")
        lock_path = root / "benchmarks/cgra-bench/corpus.lock.json"
        if not lock_path.exists():
            inventory(
                corpus,
                lock_path,
                cases_path,
                root / "benchmarks/cgra-bench/cases.overrides.v1.json",
                root / "benchmarks/cgra-bench/cases.base.v1.json",
            )
        lock = read_json(lock_path)
        if lock.get("commit") != PIN:
            raise ValueError("corpus lock commit does not match pinned upstream")
        if subprocess.check_output(["git", "-C", str(corpus), "status", "--porcelain"], text=True):
            raise ValueError("CGRA-Bench submodule is dirty")
        case_manifest = read_json(cases_path)
        if case_manifest.get("schema") != "cgra.cgra_bench.cases.v1":
            raise ValueError("cases manifest schema mismatch")
        cases = case_manifest.get("cases", [])
        functional_cases = load_cases(functional_cases_path)
        expected_sources = {item["path"] for item in lock.get("sources", [])}
        case_sources = {item.get("source") for item in cases}
        if not case_sources.issubset(expected_sources):
            extra = sorted(case_sources - expected_sources)
            raise ValueError(f"cases manifest contains sources outside corpus (extra={extra})")
        if not args.allow_subset and case_sources != expected_sources:
            missing = sorted(expected_sources - case_sources)
            raise ValueError(f"cases manifest does not cover corpus sources (missing={missing})")
        functional_cases = active_functional_cases(functional_cases, case_sources, args.allow_subset)
        for case in cases:
            source_name = case.get("source", "")
            source_path = pathlib.PurePosixPath(source_name)
            if source_path.is_absolute() or ".." in source_path.parts:
                raise ValueError(f"case source escapes corpus: {source_name}")
        case_ids = [case.get("id") for case in cases]
        if len(case_ids) != len(set(case_ids)):
            raise ValueError("cases manifest contains duplicate case IDs")
        for case in cases:
            if not case.get("enabled", True) and not case.get("exclusion"):
                raise ValueError(f"disabled case has no exclusion reason: {case.get('id', '<unknown>')}")
        if out.exists():
            owned_output_root = (root / "build" / "cgra-bench").resolve()
            if not out.is_relative_to(owned_output_root):
                raise ValueError(f"refusing to replace audit output outside {owned_output_root}: {out}")
            shutil.rmtree(out)
        out.mkdir(parents=True)
        report_corpus = lock
        if args.allow_subset:
            report_corpus = dict(lock)
            report_corpus["sources"] = [item for item in lock.get("sources", []) if item.get("path") in case_sources]
            report_corpus["denominator"] = dict(lock.get("denominator", {}))
            report_corpus["denominator"]["source_translation_units"] = len(report_corpus["sources"])
            report_corpus["denominator"]["scope"] = "smoke subset"
        write_json(out / "corpus.json", report_corpus)
        target_description = read_json(target)
        address_unit = target_description.get("memory", {}).get("address_unit", "word")
        address_unit_bytes = {"byte": 1, "word": 4}.get(address_unit)
        if address_unit_bytes is None:
            raise ValueError(f"unsupported target memory address unit: {address_unit}")
        write_json(out / "environment.json", {
            "schema": "cgra.cgra_bench.environment.v1",
            "project_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
            "project_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True)),
            "corpus_sha": lock["commit"],
            "target": str(target),
            "target_sha256": sha256_file(target),
            "target_address_unit": address_unit,
            "frontend_address_unit_bytes": address_unit_bytes,
            "clang": shutil.which("clang-14"),
            "clang_version": tool_version("clang-14"),
            "clangxx": shutil.which("clang++-14"),
            "clangxx_version": tool_version("clang++-14"),
            "opt": shutil.which("opt-14"),
            "opt_version": tool_version("opt-14"),
            "cmake_version": tool_version("cmake"),
            "host": platform.platform(),
            "timestamp_utc": datetime.now(timezone.utc).isoformat(),
            "profile": {
                "name": args.profile,
                "lane": args.lane,
                "source_abi": args.source_abi,
                "mapping_objective": args.mapping_objective,
                "normalize_recurrence_ingress": args.enable_recurrence_ingress,
                "timeout_seconds": args.timeout,
                "mapping": mapping_profile,
            },
        })
        results: list[dict[str, Any]] = []
        for case in cases:
            try:
                results.extend(
                    run_case(
                        case,
                        root,
                        corpus,
                        out,
                        target,
                        frontend_bin,
                        compile_bin,
                        args.timeout,
                        functional_cases,
                        mapping_profile,
                        args.lane,
                        args.source_abi,
                        args.mapping_objective,
                        args.enable_recurrence_ingress,
                    )
                )
            except Exception as error:  # every case must have a terminal result
                result = result_for_failure(case, "INTERNAL", f"unclassified harness exception: {error}")
                case_dir = out / "cases" / case["id"].replace("/", "_").replace("::", "__")
                result.update({
                    "artifact_directory": case_dir.relative_to(out).as_posix(),
                    "source_artifact_directory": case_dir.relative_to(out).as_posix(),
                    "command": [],
                })
                results.append(result)
        functional_failures = apply_functional_cases(
            results, functional_cases, root, out, args.timeout
        )
        for result in results:
            attach_terminal_status(result)
            result["audit_profile"] = {
                "name": args.profile,
                "lane": args.lane,
                "source_abi": args.source_abi,
                "mapping_objective": args.mapping_objective,
                "normalize_recurrence_ingress": args.enable_recurrence_ingress,
                "timeout_seconds": args.timeout,
                "mapping": mapping_profile,
            }
            write_case_evidence(out, corpus, target, result, mapping_profile)
            complete_stage_records(result)
            if [stage["stage"] for stage in result["stages"]] != STAGES:
                raise ValueError(f"incomplete stage record for {result['id']}")
        (out / "results.jsonl").write_text("".join(json.dumps(item, sort_keys=True) + "\n" for item in results), encoding="utf-8")
        summary = report(out, report_corpus, results)
        if (summary["unknown_count"] or summary["timeout_count"]
                or not summary["reconciliation"]["ok"] or functional_failures):
            if functional_failures:
                print("functional validation failures:\n" + "\n".join(functional_failures), file=sys.stderr)
            return 1
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError, FunctionalCaseError) as error:
        print(f"cgra-bench audit: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
