#!/usr/bin/env python3
"""Run the T019 audit without aborting when an individual case fails."""

from __future__ import annotations

import argparse
import hashlib
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
    from .classify import classify
    from .feature_scan import scan
    from .inventory import PIN, inventory
    from .report import report
    from .schemas import read_json, write_json
except ImportError:  # pragma: no cover - direct script execution
    from build_llvm import build
    from classify import classify
    from feature_scan import scan
    from inventory import PIN, inventory
    from report import report
    from schemas import read_json, write_json


BASELINE_MAPPING_PROFILE = {
    "max_ii": 8,
    "max_node_candidates": 100000,
    "max_backtracks": 50000,
    "max_route_calls": 100000,
    "max_route_states": 10000,
}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_result(command: list[str], cwd: pathlib.Path, timeout: int) -> tuple[int, str, str, int]:
    started = time.monotonic()
    try:
        completed = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=timeout)
        return completed.returncode, completed.stdout, completed.stderr, int((time.monotonic() - started) * 1000)
    except subprocess.TimeoutExpired as error:
        return 124, error.stdout or "", error.stderr or "", int((time.monotonic() - started) * 1000)


def result_for_failure(case: dict[str, Any], stage: str, message: str, returncode: int = 1, diagnostic_code: str | None = None) -> dict[str, Any]:
    failure = classify(stage, message, returncode)
    if diagnostic_code:
        failure["diagnostic_code"] = diagnostic_code
    return {"id": case["id"], "kernel": case["kernel"], "source": case["source"], "loop_header": None, "tier": "DISCOVERED", "terminal_stage": stage, "status": "FAIL", "stages": [{"stage": stage, "status": "FAIL"}], "message": message[-4000:], **failure}


def synthesize_invocation(dfg: dict[str, Any], trip_count: int) -> dict[str, Any]:
    scalar_inputs = {}
    for index, value in enumerate(sorted(dfg.get("external_values", []), key=lambda item: item["id"])):
        scalar_inputs[value["name"]] = f"0x{0x101 + index:08x}"
    return {"schema": "cgra.kernel_invocation.v1", "trip_count": trip_count, "scalar_inputs": scalar_inputs, "scratchpad_preload": [], "synthetic": True}


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
            return "MANIFEST_COMPLETE", "SUCCESS"
        if status == "invalid_invocation":
            return "FRONTEND_DFG", "S7_ABI_BIND"
        if status in {"invalid_source_dfg", "generic_dfg_verification_failure"}:
            return "FRONTEND_DFG", "S5_GENERIC_VERIFY"
        if status in {"target_legalization_failure", "target_dfg_verification_failure"}:
            return "FRONTEND_DFG", "S8_TARGET_LEGALIZE"
        if "legalization" in status:
            return "FRONTEND_DFG", "TARGET_ISA"
        if "mii" in status:
            return "TARGET_LEGAL", "MII"
        if "mapping" in status:
            return "TARGET_LEGAL", "MAPPING"
        if "stage" in status:
            return "MAPPED", "STAGE_SCHEDULE"
        if "rf" in status:
            return "MAPPED", "RF"
        if "material" in status:
            return "RF_COMPLETE", "MATERIALIZATION"
        if "lowering" in status or "manifest" in status:
            return "RF_COMPLETE", "TARGET_LOWERING"
    except (OSError, ValueError, json.JSONDecodeError):
        pass
    return "FRONTEND_DFG", "BACKEND"


def backend_observation(artifact_dir: pathlib.Path) -> dict[str, Any]:
    result_path = artifact_dir / "kernel_compile_result.json"
    if not result_path.exists():
        return {}
    try:
        result = read_json(result_path)
    except (OSError, ValueError, json.JSONDecodeError):
        return {}
    backend = result.get("backend") or {}
    return {
        "status": backend.get("status", result.get("status")),
        "message": backend.get("message", result.get("message", ""))[-4000:],
        "stats": backend.get("stats", {}),
    }


def run_case(case: dict[str, Any], root: pathlib.Path, corpus: pathlib.Path, out: pathlib.Path, target: pathlib.Path, frontend_bin: pathlib.Path, compile_bin: pathlib.Path, timeout: int) -> list[dict[str, Any]]:
    if not case.get("enabled", True):
        return [{"id": case["id"], "kernel": case["kernel"], "source": case["source"], "loop_header": None, "tier": "DISCOVERED", "terminal_stage": "S0_CORPUS_DISCOVERY", "status": "EXCLUDED", "stages": [{"stage": "S0_CORPUS_DISCOVERY", "status": "EXCLUDED"}], "category": "CORPUS", "owner": "HARNESS", "diagnostic_code": "EXPLICIT_EXCLUSION", "message": case.get("exclusion", "explicitly excluded"), "excluded": True}]
    source = corpus / case["source"]
    source_out = out / "cases" / case["id"].replace("/", "_").replace("::", "__")
    source_out.mkdir(parents=True, exist_ok=True)
    flags = list(case.get("compile_flags", []))
    flags.extend(f"-D{define}" for define in case.get("defines", []))
    flags.extend(f"-I{(corpus / include).resolve()}" for include in case.get("include_dirs", []))
    source_result = build(source, source_out / "build", timeout, flags)
    write_json(source_out / "build.json", source_result)
    if source_result.get("status") != "LLVM_BUILT":
        return [result_for_failure(case, "S1_SOURCE_BUILD", source_result.get("message", "source build failed"), diagnostic_code=source_result.get("diagnostic_code"))]
    canonical = pathlib.Path(source_result["ir"])
    feature = scan(canonical)
    write_json(source_out / "llvm_feature_inventory.json", feature)
    rc, stdout, stderr, duration = command_result([str(frontend_bin), str(canonical), "--list-loops", "--json"], root, timeout)
    if rc != 0:
        return [result_for_failure(case, "S3_LOOP_SELECTION", stderr or stdout, rc)]
    try:
        loops = json.loads(stdout)["loops"]
    except (ValueError, KeyError, json.JSONDecodeError) as error:
        return [result_for_failure(case, "S3_LOOP_SELECTION", f"invalid loop inventory: {error}")]
    write_json(source_out / "loop_inventory.json", {"schema": "cgra.cgra_bench.loop_inventory.v1", "source": case["source"], "loops": loops})
    if not loops:
        return [result_for_failure(case, "S3_LOOP_SELECTION", "no innermost loop found", diagnostic_code="NO_INNERMOST_LOOP")]
    results = []
    for loop in loops:
        loop_case = dict(case)
        loop_case["id"] = f"{case['id']}::{loop['function']}::{loop['header']}"
        loop_out = source_out / "loops" / f"{loop['function']}__{loop['header']}"
        frontend_out = loop_out / "frontend"
        frontend_out.mkdir(parents=True, exist_ok=True)
        dfg_path = frontend_out / "generic_dfg.json"
        rc, stdout, stderr, lower_duration = command_result([str(frontend_bin), str(canonical), "--function", loop["function"], "--loop-header", loop["header"], "--artifact-dir", str(frontend_out), "-o", str(dfg_path)], root, timeout)
        base = {"id": loop_case["id"], "kernel": case["kernel"], "source": case["source"], "function": loop["function"], "loop_header": loop["header"], "feature": feature, "loop": loop, "duration_ms": {"loop_selection": duration, "frontend": lower_duration}, "synthetic_invocation": True, "status": "FAIL", "stages": [{"stage": "S0_CORPUS_DISCOVERY", "status": "PASS"}, {"stage": "S1_SOURCE_BUILD", "status": "PASS"}, {"stage": "S2_LLVM_CANONICALIZE", "status": "PASS"}, {"stage": "S3_LOOP_SELECTION", "status": "PASS"}]}
        if rc != 0 or not dfg_path.exists():
            failure = classify("S4_FRONTEND_LOWER", stderr or stdout, rc)
            results.append({**base, "tier": "LLVM_BUILT", "terminal_stage": "S4_FRONTEND_LOWER", "stages": [*base["stages"], {"stage": "S4_FRONTEND_LOWER", "status": "FAIL"}], "message": (stderr or stdout)[-4000:], **failure})
            continue
        try:
            dfg = read_json(dfg_path)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            results.append({**base, "tier": "LLVM_BUILT", "terminal_stage": "S5_GENERIC_VERIFY", "stages": [*base["stages"], {"stage": "S4_FRONTEND_LOWER", "status": "PASS"}, {"stage": "S5_GENERIC_VERIFY", "status": "FAIL"}], "message": str(error), **classify("S5_GENERIC_VERIFY", str(error))})
            continue
        base["dfg"] = {"node_count": len(dfg.get("nodes", [])), "edge_count": len(dfg.get("edges", [])), "external_values": len(dfg.get("external_values", [])), "liveouts": len(dfg.get("live_outs", [])), "opcode_histogram": {opcode: sum(node.get("opcode") == opcode for node in dfg.get("nodes", [])) for opcode in sorted({node.get("opcode") for node in dfg.get("nodes", [])})}}
        invocation = synthesize_invocation(dfg, int(loop.get("static_trip_count") or 4))
        write_json(loop_out / "invocation.json", invocation)
        abi_out = loop_out / "abi"
        manifest = abi_out / "program_manifest.json"
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
            str(manifest),
            "--max-ii",
            str(BASELINE_MAPPING_PROFILE["max_ii"]),
            "--max-node-candidates",
            str(BASELINE_MAPPING_PROFILE["max_node_candidates"]),
            "--max-backtracks",
            str(BASELINE_MAPPING_PROFILE["max_backtracks"]),
            "--max-route-calls",
            str(BASELINE_MAPPING_PROFILE["max_route_calls"]),
            "--max-route-states",
            str(BASELINE_MAPPING_PROFILE["max_route_states"]),
        ]
        rc, stdout, stderr, compile_duration = command_result(compile_command, root, timeout)
        tier, stage = tier_from_compile(abi_out, True)
        backend = backend_observation(abi_out)
        if rc != 0:
            failure = classify(stage, stderr or stdout, rc)
            results.append({**base, "tier": tier, "terminal_stage": stage, "stages": [*base["stages"], {"stage": "S4_FRONTEND_LOWER", "status": "PASS"}, {"stage": "S5_GENERIC_VERIFY", "status": "PASS"}, {"stage": stage, "status": "FAIL"}], "message": (stderr or stdout)[-4000:], "duration_ms": {**base["duration_ms"], "abi_backend": compile_duration}, "backend": backend, **failure})
        else:
            results.append({**base, "tier": tier, "terminal_stage": "S15_MANIFEST_VERIFY", "status": "PASS", "stages": [*base["stages"], {"stage": "S4_FRONTEND_LOWER", "status": "PASS"}, {"stage": "S5_GENERIC_VERIFY", "status": "PASS"}, {"stage": "S7_ABI_BIND", "status": "PASS"}, {"stage": "S8_TARGET_LEGALIZE", "status": "PASS"}, {"stage": "S15_MANIFEST_VERIFY", "status": "PASS"}], "diagnostic_code": "SUCCESS", "category": "MANIFEST", "owner": "LOWERING", "message": stdout[-2000:], "duration_ms": {**base["duration_ms"], "abi_backend": compile_duration}, "backend": backend, "artifacts": [str(path) for path in abi_out.rglob("*") if path.is_file()]})
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=pathlib.Path, default=pathlib.Path("third_party/CGRA-Bench"))
    parser.add_argument("--target", type=pathlib.Path, default=pathlib.Path("target/cgra_v3.json"))
    parser.add_argument("--cases", type=pathlib.Path, default=pathlib.Path("benchmarks/cgra-bench/cases.v1.json"))
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("build/cgra-bench/run"))
    parser.add_argument("--frontend-bin", type=pathlib.Path, default=pathlib.Path("build/compiler-llvm/bin/cgra-llvm-loop-lower"))
    parser.add_argument("--compile-kernel-bin", type=pathlib.Path, default=pathlib.Path("build/compiler/bin/cgrac-compile-kernel"))
    parser.add_argument("--timeout", type=int, default=120)
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
    try:
        if not args.all:
            parser.error("--all is required for an audit run")
        lock_path = root / "benchmarks/cgra-bench/corpus.lock.json"
        if not lock_path.exists():
            inventory(corpus, lock_path, cases_path)
        lock = read_json(lock_path)
        if lock.get("commit") != PIN:
            raise ValueError("corpus lock commit does not match pinned upstream")
        if subprocess.check_output(["git", "-C", str(corpus), "status", "--porcelain"], text=True):
            raise ValueError("CGRA-Bench submodule is dirty")
        case_manifest = read_json(cases_path)
        if case_manifest.get("schema") != "cgra.cgra_bench.cases.v1":
            raise ValueError("cases manifest schema mismatch")
        cases = case_manifest.get("cases", [])
        expected_sources = {item["path"] for item in lock.get("sources", [])}
        case_sources = {item.get("source") for item in cases}
        if not case_sources.issubset(expected_sources):
            extra = sorted(case_sources - expected_sources)
            raise ValueError(f"cases manifest contains sources outside corpus (extra={extra})")
        if not args.allow_subset and case_sources != expected_sources:
            missing = sorted(expected_sources - case_sources)
            raise ValueError(f"cases manifest does not cover corpus sources (missing={missing})")
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
        out.mkdir(parents=True, exist_ok=True)
        report_corpus = lock
        if args.allow_subset:
            report_corpus = dict(lock)
            report_corpus["sources"] = [item for item in lock.get("sources", []) if item.get("path") in case_sources]
            report_corpus["denominator"] = dict(lock.get("denominator", {}))
            report_corpus["denominator"]["source_translation_units"] = len(report_corpus["sources"])
            report_corpus["denominator"]["scope"] = "smoke subset"
        write_json(out / "corpus.json", report_corpus)
        write_json(out / "environment.json", {
            "schema": "cgra.cgra_bench.environment.v1",
            "project_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
            "corpus_sha": lock["commit"],
            "target": str(target),
            "target_sha256": sha256(target),
            "clang": shutil.which("clang-14"),
            "clangxx": shutil.which("clang++-14"),
            "opt": shutil.which("opt-14"),
            "host": platform.platform(),
            "timestamp_utc": datetime.now(timezone.utc).isoformat(),
            "profile": {"name": "baseline", "timeout_seconds": args.timeout, "mapping": BASELINE_MAPPING_PROFILE},
        })
        results: list[dict[str, Any]] = []
        for case in cases:
            try:
                results.extend(run_case(case, root, corpus, out, target, frontend_bin, compile_bin, args.timeout))
            except Exception as error:  # every case must have a terminal result
                results.append(result_for_failure(case, "INTERNAL", f"unclassified harness exception: {error}"))
        (out / "results.jsonl").write_text("".join(json.dumps(item, sort_keys=True) + "\n" for item in results), encoding="utf-8")
        summary = report(out, report_corpus, results)
        if summary["unknown_count"] or not summary["reconciliation"]["ok"]:
            return 1
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"cgra-bench audit: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
