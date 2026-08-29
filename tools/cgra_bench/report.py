#!/usr/bin/env python3
"""Aggregate structured T019 case results into reproducible reports."""

from __future__ import annotations

import argparse
import csv
import fnmatch
import json
import pathlib
import statistics
from collections import Counter, defaultdict
from typing import Any

try:
    from .schemas import read_json, write_json
except ImportError:  # pragma: no cover - direct script execution
    from schemas import read_json, write_json


TIERS = {
    "DISCOVERED": 0,
    "LLVM_BUILT": 1,
    "FRONTEND_DFG": 2,
    "TARGET_LEGAL": 3,
    # MAPPED/RF_COMPLETE are retained as compatibility aliases for existing
    # audit manifests. New research results use the more precise names.
    "ROUTE_MAPPED": 4,
    "MAPPED": 4,
    "RF_CONSTRAINED_MAPPED": 5,
    "RF_COMPLETE": 5,
    "MANIFEST_COMPLETE": 6,
    "FUNCTIONAL_RTL_VALIDATED": 7,
}
COMPUTATIONAL_FAMILY_SCHEMA = "cgra.cgra_bench.computational_families.v1"


def target_contract_summary(operation_key: str, target_operations: dict[str, Any], opcode: str | None = None) -> str:
    """Report the declared target operation for a Generic operation key.

    Generic ``Custom`` nodes carry their concrete operation key (for example
    ``FADD``), while ``ICmp`` is legalized to a predicate-specific ``CMP_*``
    operation.  Keep this helper conservative and diagnostic-only: target
    legalization remains the authority for whether a case is executable.
    """
    names = {name.upper() for name in target_operations}
    normalized_key = operation_key.upper()
    normalized_opcode = (opcode or operation_key).upper()
    if normalized_opcode == "ICMP" or normalized_key == "ICMP":
        return "predicate-dependent" if any(name.startswith("CMP_") for name in names) else "not-declared"
    return "declared" if normalized_key in names else "not-declared"


def _operation_histogram(item: dict[str, Any], out: pathlib.Path) -> dict[str, int]:
    """Read typed operation keys from a result, with an old-run fallback."""
    summary = item.get("dfg", {}).get("operation_histogram")
    if isinstance(summary, dict) and summary:
        return {str(key): int(value) for key, value in summary.items()}
    artifact_directory = item.get("artifact_directory")
    if artifact_directory:
        dfg_path = out / artifact_directory / "frontend" / "generic_dfg.json"
        try:
            dfg = read_json(dfg_path)
        except (OSError, ValueError, json.JSONDecodeError):
            dfg = None
        if isinstance(dfg, dict):
            histogram: Counter[str] = Counter()
            for node in dfg.get("nodes", []):
                opcode = node.get("opcode", "unknown")
                key = node.get("operation_key") if opcode == "Custom" else opcode
                histogram[str(key or opcode)] += 1
            if histogram:
                return dict(histogram)
    return {
        str(opcode): int(count)
        for opcode, count in item.get("dfg", {}).get("opcode_histogram", {}).items()
    }


def _metric(item: dict[str, Any], name: str) -> Any:
    """Read a metric from the stable result envelope or backend stats."""
    if name in item:
        return item[name]
    backend = item.get("backend", {})
    if isinstance(backend, dict):
        stats = backend.get("stats", {})
        if isinstance(stats, dict) and name in stats:
            return stats[name]
    if name == "compile_ms":
        durations = item.get("duration_ms", {})
        if isinstance(durations, dict):
            return sum(value for value in durations.values() if isinstance(value, (int, float)))
    return None


def _find_family_manifest(out: pathlib.Path) -> pathlib.Path | None:
    """Find the repository audit contract without hard-coding the checkout."""
    for parent in (out, *out.parents):
        candidate = parent / "benchmarks/cgra-bench/computational_families.v1.json"
        if candidate.is_file():
            return candidate
    return None


def _mapping_verified(item: dict[str, Any]) -> bool:
    backend = item.get("backend")
    if not isinstance(backend, dict):
        return item.get("diagnostic_code") == "MODULO_MAPPING_VERIFIED"
    return backend.get("mapping_status") in {"rf_constrained_success", "success"}


def _computational_family_gate(
    out: pathlib.Path, corpus: dict[str, Any], results: list[dict[str, Any]],
    family_manifest: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Evaluate integration families from audit metadata, never compiler code."""
    manifest_path = _find_family_manifest(out)
    if family_manifest is None and manifest_path is not None:
        family_manifest = read_json(manifest_path)
    if family_manifest is None:
        return {
            "schema": COMPUTATIONAL_FAMILY_SCHEMA,
            "configured": False,
            "enforced": False,
            "required_family_count": 0,
            "mapped_family_count": 0,
            "families": [],
            "pass": True,
        }
    if family_manifest.get("schema") != COMPUTATIONAL_FAMILY_SCHEMA:
        raise ValueError("computational family manifest schema mismatch")
    families = family_manifest.get("families")
    if not isinstance(families, list) or not families:
        raise ValueError("computational family manifest must contain families")
    enforced = corpus.get("denominator", {}).get("scope") != "smoke subset"
    details = []
    family_ids: set[str] = set()
    for family in families:
        family_id = family.get("id")
        source_glob = family.get("source_glob")
        function_glob = family.get("function_glob", "*")
        if not isinstance(family_id, str) or not isinstance(source_glob, str):
            raise ValueError("computational family needs string id and source_glob")
        if family_id in family_ids:
            raise ValueError(f"duplicate computational family id: {family_id}")
        family_ids.add(family_id)
        minimum_tier = family.get("minimum_tier", "MAPPED")
        required_level = TIERS.get(minimum_tier)
        if required_level is None:
            raise ValueError(f"unknown computational family tier: {minimum_tier}")
        candidates = [
            item for item in results
            if item.get("loop_header") is not None
            and fnmatch.fnmatchcase(str(item.get("source", "")), source_glob)
            and fnmatch.fnmatchcase(str(item.get("function", "")), function_glob)
        ]
        tiered = [
            item for item in candidates
            if TIERS.get(item.get("tier", "DISCOVERED"), 0) >= required_level
        ]
        mapped = [
            item for item in tiered
            if _mapping_verified(item)
        ]
        details.append({
            "id": family_id,
            "source_glob": source_glob,
            "function_glob": function_glob,
            "minimum_tier": minimum_tier,
            "candidate_count": len(candidates),
            "minimum_tier_count": len(tiered),
            "mapped_count": len(mapped),
            "mapped_case_ids": [item["id"] for item in mapped],
            "pass": bool(mapped),
        })
    mapped_count = sum(item["pass"] for item in details)
    return {
        "schema": COMPUTATIONAL_FAMILY_SCHEMA,
        "configured": True,
        "enforced": enforced,
        "required_family_count": len(details),
        "mapped_family_count": mapped_count,
        "families": details,
        "pass": not enforced or mapped_count == len(details),
    }


def report(out: pathlib.Path, corpus: dict[str, Any], results: list[dict[str, Any]], family_manifest: dict[str, Any] | None = None) -> dict[str, Any]:
    terminal = Counter(item.get("category", "INTERNAL") for item in results)
    strict_terminal = Counter(item.get("terminal_status", "COMPILER_BUG") for item in results)
    tiers = Counter(item.get("tier", "DISCOVERED") for item in results)
    first_blockers = Counter(
        item.get("diagnostic_code", "UNCLASSIFIED_RESULT")
        for item in results
        if item.get("status") not in {"PASS", "EXCLUDED"}
    )
    features = Counter()
    feature_types = Counter()
    feature_predicates = Counter()
    feature_counts = Counter()
    isa = Counter()
    operations = Counter()
    operation_histograms = [_operation_histogram(item, out) for item in results]
    for item, operation_histogram in zip(results, operation_histograms):
        for opcode, count in item.get("feature", {}).get("opcode_histogram", {}).items():
            features[opcode] += count
        for value_type, count in item.get("feature", {}).get("type_histogram", {}).items():
            feature_types[value_type] += count
        for predicate, count in item.get("feature", {}).get("icmp_predicate_histogram", {}).items():
            feature_predicates[predicate] += count
        for construct, count in item.get("feature", {}).get("counts", {}).items():
            feature_counts[construct] += count
        for opcode, count in item.get("dfg", {}).get("opcode_histogram", {}).items():
            isa[opcode] += count
        for operation_key, count in operation_histogram.items():
            operations[operation_key] += count
    target_operations: dict[str, Any] = {}
    environment_path = out / "environment.json"
    if environment_path.exists():
        try:
            environment = read_json(environment_path)
            target_path = pathlib.Path(environment.get("target", ""))
            if target_path.exists():
                target_operations = read_json(target_path).get("operations", {})
        except (OSError, ValueError, json.JSONDecodeError) as error:
            raise ValueError(f"cannot read target contract for ISA coverage: {error}") from error
    expected_loops: set[tuple[str, str, str]] = set()
    for inventory_path in out.glob("cases/*/loop_inventory.json"):
        try:
            loop_inventory = read_json(inventory_path)
            if loop_inventory.get("schema") != "cgra.cgra_bench.loop_inventory.v1":
                raise ValueError(f"loop inventory schema mismatch: {inventory_path}")
            source = loop_inventory["source"]
            expected_loops.update(
                (source, loop["function"], loop["header"])
                for loop in loop_inventory.get("loops", [])
            )
        except (OSError, KeyError, ValueError, json.JSONDecodeError) as error:
            raise ValueError(f"invalid loop inventory {inventory_path}: {error}") from error
    represented_loops = {
        (item["source"], item["function"], item["loop_header"])
        for item in results
        if item.get("loop_header") is not None and item.get("function")
    }
    missing_loops = sorted("::".join(loop) for loop in expected_loops - represented_loops)
    unexpected_loops = sorted("::".join(loop) for loop in represented_loops - expected_loops)
    candidate_loop_count = len(expected_loops)
    represented_sources = {item.get("source") for item in results if item.get("source")}
    expected_sources = {
        item.get("path") for item in corpus.get("sources", []) if item.get("enabled", True)
    }
    excluded_sources = {
        item.get("path") for item in corpus.get("sources", []) if not item.get("enabled", True)
    }
    missing_sources = sorted(expected_sources - represented_sources)
    unexpected_sources = sorted(represented_sources - expected_sources - excluded_sources)
    source_count = len(represented_sources)
    kernels = defaultdict(list)
    for item in results:
        kernels[item.get("kernel", "unknown")].append(item)
    backend_stats = [
        item.get("backend", {}).get("stats", {})
        for item in results
        if isinstance(item.get("backend"), dict) and item.get("backend", {}).get("stats")
    ]
    backend_totals: Counter[str] = Counter()
    for stats in backend_stats:
        backend_totals.update({key: value for key, value in stats.items() if isinstance(value, int)})
    dfg_edge_kinds: Counter[str] = Counter()
    dfg_distances: Counter[str] = Counter()
    for item in results:
        dfg_edge_kinds.update(item.get("dfg", {}).get("edge_kind_histogram", {}))
        dfg_distances.update(item.get("dfg", {}).get("distance_histogram", {}))
    linear_results = [
        item for item in results
        if item.get("loop", {}).get("shape", {}).get("kind") == "linear_multiblock"
    ]
    frontend_dfg_or_higher = sum(
        TIERS.get(item.get("tier", "DISCOVERED"), 0) >= TIERS["FRONTEND_DFG"]
        for item in results
    )
    mapped_or_higher = sum(
        TIERS.get(item.get("tier", "DISCOVERED"), 0) >= TIERS["MAPPED"]
        for item in results
    )
    mapped_kernel_directories = len({
        item.get("kernel")
        for item in results
        if item.get("kernel") and
        TIERS.get(item.get("tier", "DISCOVERED"), 0) >= TIERS["MAPPED"]
    })
    def mapper_was_invoked(item: dict[str, Any]) -> bool:
        backend = item.get("backend", {})
        if not isinstance(backend, dict):
            return False
        stats = backend.get("stats", {})
        if isinstance(stats, dict) and "mapper_invoked" in stats:
            return bool(stats["mapper_invoked"])
        return backend.get("mapping_status") in {
            "route_mapped_rf_budget", "route_mapped_rf_infeasible", "rf_constrained_success", "success"
        }

    mapper_entered = sum(mapper_was_invoked(item) for item in results)
    mapper_entered_kernel_directories = len({
        item.get("kernel")
        for item in results
        if item.get("kernel") and mapper_was_invoked(item)
    })
    mapping_statuses = Counter(
        item.get("backend", {}).get("mapping_status", "not_available")
        for item in results
        if isinstance(item.get("backend"), dict)
    )
    def raw_mapping_observed(item: dict[str, Any]) -> bool:
        backend = item.get("backend")
        if not isinstance(backend, dict):
            return False
        if "raw_mapping_found" in backend:
            return bool(backend["raw_mapping_found"])
        return backend.get("mapping_status") in {
            "route_mapped_rf_infeasible", "rf_constrained_success", "success"
        }

    def strict_mapping_observed(item: dict[str, Any]) -> bool:
        backend = item.get("backend")
        if not isinstance(backend, dict):
            return False
        if "rf_constrained_mapping_found" in backend:
            return bool(backend["rf_constrained_mapping_found"])
        return backend.get("mapping_status") in {"rf_constrained_success", "success"}

    completed_modulo_mappings = sum(
        int(stats.get("completed_modulo_mappings", 0)) for stats in backend_stats
    )
    rf_rejected_mappings = sum(int(stats.get("rf_rejected", 0)) for stats in backend_stats)
    rf_constrained_mappings = sum(
        int(stats.get("rf_constrained_mappings", 0)) for stats in backend_stats
    )
    rf_rejected_by_ii: Counter[str] = Counter()
    rf_rejected_by_reason: Counter[str] = Counter()
    for stats in backend_stats:
        per_ii = stats.get("rf_rejected_by_ii", {})
        if isinstance(per_ii, dict):
            rf_rejected_by_ii.update({str(ii): int(count) for ii, count in per_ii.items()})
        per_reason = stats.get("rf_rejected_by_reason", {})
        if isinstance(per_reason, dict):
            rf_rejected_by_reason.update(
                {str(reason): int(count) for reason, count in per_reason.items()}
            )
    physical_statuses = Counter()
    physical_reasons = Counter()
    for item in results:
        if not isinstance(item.get("backend"), dict):
            continue
        physical = item["backend"].get("physical_realizability", {})
        status = physical.get("status", "not_run")
        physical_statuses[status] += 1
        if physical.get("reason_code"):
            physical_reasons[physical["reason_code"]] += 1
    computational_family_gate = _computational_family_gate(out, corpus, results, family_manifest)
    def metric_values(name: str) -> list[float]:
        values = []
        for item in results:
            value = _metric(item, name)
            if isinstance(value, (int, float)):
                values.append(float(value))
        return values

    def quantile(values: list[float], fraction: float) -> float | None:
        if not values:
            return None
        ordered = sorted(values)
        index = min(len(ordered) - 1, int(round((len(ordered) - 1) * fraction)))
        return ordered[index]

    mii_values = metric_values("mii")
    safe_ii_values = [
        value for item in results
        if item.get("terminal_status") == "FEASIBLE_II"
        for value in [_metric(item, "safe_ii")]
        if isinstance(value, (int, float)) and value > 0
    ]
    compile_values = [
        float(sum(value for value in item.get("duration_ms", {}).values()
                  if isinstance(value, (int, float))))
        for item in results
    ]
    ratio_values = []
    for item in results:
        mii = _metric(item, "mii")
        safe_ii = _metric(item, "safe_ii")
        if isinstance(mii, (int, float)) and isinstance(safe_ii, (int, float)) and mii > 0 and safe_ii > 0:
            ratio_values.append(float(safe_ii) / float(mii))
    summary = {
        "schema": "cgra.cgra_bench.summary.v1",
        "denominator": {
            "kernel_directories": corpus["denominator"]["kernel_directories"],
            "source_translation_units": corpus["denominator"]["source_translation_units"],
            "candidate_loops": candidate_loop_count,
            "terminal_results": len(results),
            "source_results": source_count,
        },
        "reconciliation": {
            "expected_enabled_sources": len(expected_sources),
            "represented_sources": source_count,
            "expected_candidate_loops": len(expected_loops),
            "represented_candidate_loops": len(represented_loops),
            "excluded_sources": len(excluded_sources),
            "missing_sources": missing_sources,
            "unexpected_sources": unexpected_sources,
            "missing_loop_cases": missing_loops,
            "unexpected_loop_cases": unexpected_loops,
            "ok": not missing_sources and not unexpected_sources and not missing_loops and not unexpected_loops,
        },
        "tiers": dict(sorted(tiers.items())),
        "terminal_categories": dict(sorted(terminal.items())),
        "terminal_status_histogram": dict(sorted(strict_terminal.items())),
        "architecture_admissible_loops": sum(
            item.get("admissibility_status") == "ADMISSIBLE" for item in results
        ),
        "architecture_admissible_case_ids": [
            item["id"] for item in results
            if item.get("admissibility_status") == "ADMISSIBLE"
        ],
        "unknown_due_to_compiler_loops": sum(
            item.get("admissibility_status") == "UNKNOWN_DUE_TO_COMPILER" for item in results
        ),
        "unknown_due_to_compiler_case_ids": [
            item["id"] for item in results
            if item.get("admissibility_status") == "UNKNOWN_DUE_TO_COMPILER"
        ],
        "strict_feasible_ii_loops": sum(
            item.get("terminal_status") == "FEASIBLE_II" for item in results
        ),
        "strict_feasible_ii_case_ids": [
            item["id"] for item in results if item.get("terminal_status") == "FEASIBLE_II"
        ],
        "compiler_bug_count": sum(
            item.get("terminal_status") == "COMPILER_BUG" for item in results
        ),
        "mapper_entered_loops": mapper_entered,
        "mapper_entered_case_ids": [item["id"] for item in results if mapper_was_invoked(item)],
        "raw_route_mapped_loops": sum(raw_mapping_observed(item) for item in results),
        "raw_route_mapped_case_ids": [item["id"] for item in results if raw_mapping_observed(item)],
        "target_legal_loops": sum(TIERS.get(item.get("tier", "DISCOVERED"), 0) >= TIERS["TARGET_LEGAL"] for item in results),
        "target_legal_case_ids": [
            item["id"] for item in results
            if TIERS.get(item.get("tier", "DISCOVERED"), 0) >= TIERS["TARGET_LEGAL"]
        ],
        "hardware_executable_loops": sum(
            bool(item.get("backend", {}).get("hardware_executable"))
            for item in results if isinstance(item.get("backend"), dict)
        ),
        "stage_rejected": sum(int(stats.get("stage_rejected", 0)) for stats in backend_stats),
        "rf_rejected": sum(int(stats.get("rf_rejected", 0)) for stats in backend_stats),
        "metrics": {
            "median_mii": statistics.median(mii_values) if mii_values else None,
            "median_safe_ii": statistics.median(safe_ii_values) if safe_ii_values else None,
            "median_safe_ii_over_mii": statistics.median(ratio_values) if ratio_values else None,
            "p95_safe_ii_over_mii": quantile(ratio_values, 0.95),
            "median_compile_time_ms": statistics.median(compile_values) if compile_values else None,
            "p95_compile_time_ms": quantile(compile_values, 0.95),
        },
        "unknown_count": sum(1 for item in results if item.get("category") == "INTERNAL" or item.get("owner") == "UNKNOWN"),
        "timeout_count": sum(1 for item in results if item.get("category") == "TIMEOUT"),
        "first_blocker_distribution": dict(sorted(first_blockers.items())),
        "all_observed_opcodes": dict(sorted(features.items())),
        "all_observed_types": dict(sorted(feature_types.items())),
        "all_observed_icmp_predicates": dict(sorted(feature_predicates.items())),
        "all_observed_feature_counts": dict(sorted(feature_counts.items())),
        "generic_opcode_histogram": dict(sorted(isa.items())),
        "generic_operation_histogram": dict(sorted(operations.items())),
        "generic_edge_kind_histogram": dict(sorted(dfg_edge_kinds.items())),
        "generic_distance_histogram": dict(sorted(dfg_distances.items(), key=lambda item: int(item[0]))),
        "linear_multiblock": {
            "candidate_count": len(linear_results),
            "frontend_dfg_count": sum(
                TIERS.get(item.get("tier", "DISCOVERED"), 0) >= 2 for item in linear_results
            ),
            "mapped_count": sum(
                TIERS.get(item.get("tier", "DISCOVERED"), 0) >= 4 for item in linear_results
            ),
            "shape_rejection_count": sum(
                item.get("diagnostic_code") in {
                    "LLVM_FRONTEND_UNSUPPORTED_LOOP_SHAPE",
                    "LLVM_FRONTEND_LINEAR_LOOP_NO_PREHEADER",
                    "LLVM_FRONTEND_LINEAR_LOOP_NO_LATCH",
                    "LLVM_FRONTEND_LINEAR_LOOP_EXIT_SHAPE",
                    "LLVM_FRONTEND_LINEAR_LOOP_INTERNAL_BRANCH",
                    "LLVM_FRONTEND_LINEAR_LOOP_UNSUPPORTED_TERMINATOR",
                    "LLVM_FRONTEND_LINEAR_LOOP_NON_LINEAR_CFG",
                    "LLVM_FRONTEND_LINEAR_LOOP_NONHEADER_PHI",
                }
                for item in linear_results
            ),
        },
        "t020_outcome": {
            "frontend_dfg_or_higher": frontend_dfg_or_higher,
            "mapped_or_higher": mapped_or_higher,
            "mapped_kernel_directories": mapped_kernel_directories,
            "required_frontend_dfg_or_higher": 10,
            "required_mapped_or_higher": 8,
            "required_mapped_kernel_directories": 5,
            "pass": frontend_dfg_or_higher >= 10 and mapped_or_higher >= 8 and
                    mapped_kernel_directories >= 5,
        },
        "t020r_mapping_research": {
            "mapper_entered": mapper_entered,
            "raw_modulo_verified": sum(raw_mapping_observed(item) for item in results),
            "completed_modulo_mappings": completed_modulo_mappings,
            "rf_rejected_mappings": rf_rejected_mappings,
            "rf_constrained_mapped": rf_constrained_mappings or sum(
                strict_mapping_observed(item) for item in results
            ),
            # Historical key retained for consumers; it now means the strict
            # RF-constrained result rather than a route-only candidate.
            "modulo_mapping_verified": rf_constrained_mappings or sum(
                strict_mapping_observed(item) for item in results
            ),
            "mapper_entered_kernel_directories": mapper_entered_kernel_directories,
            "hardware_executable": sum(
                bool(item.get("backend", {}).get("hardware_executable"))
                for item in results
                if isinstance(item.get("backend"), dict)
            ),
            "mapping_status": dict(sorted(mapping_statuses.items())),
            "physical_status": dict(sorted(physical_statuses.items())),
            "physical_failure_reasons": dict(sorted(physical_reasons.items())),
            "required_mapper_entered": 60,
            "required_modulo_mapping_verified": 20,
            "required_mapper_entered_kernel_directories": 12,
            "pass": mapper_entered >= 60 and
                    (rf_constrained_mappings or sum(
                        strict_mapping_observed(item) for item in results
                    )) >= 20 and
                    mapper_entered_kernel_directories >= 12,
        },
        "computational_family_gate": computational_family_gate,
        "backend_metrics": {
            "cases_with_stats": len(backend_stats),
            "mii": [stats.get("mii") for stats in backend_stats if stats.get("mii") is not None],
            "resource_mii": [stats.get("resource_mii") for stats in backend_stats if stats.get("resource_mii") is not None],
            "recurrence_mii": [stats.get("recurrence_mii") for stats in backend_stats if stats.get("recurrence_mii") is not None],
            "recurrence_witnesses": [stats.get("mii_witness") for stats in backend_stats if stats.get("mii_witness") is not None],
            "mapped_ii": [stats.get("mapped_ii") for stats in backend_stats if stats.get("mapped_ii") is not None],
            "ii_attempts": sum(stats.get("ii_attempts", 0) for stats in backend_stats),
            "backtracks": sum(stats.get("backtracks", 0) for stats in backend_stats),
            "route_search_calls": sum(stats.get("route_search_calls", 0) for stats in backend_stats),
            "route_no_paths": sum(stats.get("route_no_paths", 0) for stats in backend_stats),
            "route_budget_exceeded": sum(stats.get("route_budget_exceeded", 0) for stats in backend_stats),
            "successful_placements": sum(stats.get("successful_placements", 0) for stats in backend_stats),
            "rejected_placements": sum(stats.get("rejected_placements", 0) for stats in backend_stats),
            "node_candidate_attempts": sum(stats.get("node_candidate_attempts", 0) for stats in backend_stats),
            "route_state_expansions": sum(stats.get("route_state_expansions", 0) for stats in backend_stats),
            "completed_modulo_mappings": sum(stats.get("completed_modulo_mappings", 0) for stats in backend_stats),
            "rf_rejected": sum(stats.get("rf_rejected", 0) for stats in backend_stats),
            "rf_budget_exceeded": sum(stats.get("rf_budget_exceeded", 0) for stats in backend_stats),
            "rf_constrained_mappings": rf_constrained_mappings,
            "rf_rejected_by_ii": dict(sorted(rf_rejected_by_ii.items())),
            "rf_rejected_by_reason": dict(sorted(rf_rejected_by_reason.items())),
            "totals": dict(sorted(backend_totals.items())),
            "per_case": [
                {"id": item["id"], **item["backend"]["stats"]}
                for item in results
                if isinstance(item.get("backend"), dict) and item.get("backend", {}).get("stats")
            ],
        },
        "kernel_rollup": {
            kernel: {
                "source_count": len({item.get("source") for item in items}),
                "loop_count": sum(item.get("loop_header") is not None for item in items),
                "best_tier": max(items, key=lambda item: TIERS.get(item.get("tier", "DISCOVERED"), 0)).get("tier", "DISCOVERED"),
                "frontend_success": sum(item.get("tier") not in {"DISCOVERED", "LLVM_BUILT"} for item in items),
                "target_legal": sum(TIERS.get(item.get("tier", "DISCOVERED"), 0) >= 3 for item in items),
                "mapped": sum(TIERS.get(item.get("tier", "DISCOVERED"), 0) >= 4 for item in items),
                "manifest_complete": sum(TIERS.get(item.get("tier", "DISCOVERED"), 0) >= 6 for item in items),
                "dominant_blocker": Counter(item.get("diagnostic_code", "") for item in items).most_common(1)[0][0],
            }
            for kernel, items in sorted(kernels.items())
        },
    }
    write_json(out / "summary.json", summary)
    with (out / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["metric", "value"])
        writer.writerows([("candidate_loops", candidate_loop_count), ("terminal_results", len(results)), *sorted(tiers.items()), *sorted(terminal.items())])
    lines = ["# CGRA-Bench Audit Summary", "", f"- Source units represented: {source_count}/{len(expected_sources)}", f"- Candidate loops represented: {len(represented_loops)}/{candidate_loop_count}", f"- Terminal results: {len(results)}", f"- UNKNOWN/unclassified: {summary['unknown_count']}", f"- Timeouts: {summary['timeout_count']}", f"- Reconciliation: {'PASS' if summary['reconciliation']['ok'] else 'FAIL'}", f"- Linear multi-block: candidates={summary['linear_multiblock']['candidate_count']}, frontend={summary['linear_multiblock']['frontend_dfg_count']}, mapped={summary['linear_multiblock']['mapped_count']}, shape-rejected={summary['linear_multiblock']['shape_rejection_count']}", f"- T020 outcome: frontend={frontend_dfg_or_higher}/10, mapped={mapped_or_higher}/8, mapped kernels={mapped_kernel_directories}/5 ({'PASS' if summary['t020_outcome']['pass'] else 'FAIL'})", f"- T020R research outcome: mapper entered={mapper_entered}/60, raw route cases={summary['t020r_mapping_research']['raw_modulo_verified']}, RF-constrained mapped={summary['t020r_mapping_research']['rf_constrained_mapped']}/20, mapper-entered kernels={mapper_entered_kernel_directories}/12 ({'PASS' if summary['t020r_mapping_research']['pass'] else 'FAIL'})", f"- Computational families: mapped={computational_family_gate['mapped_family_count']}/{computational_family_gate['required_family_count']} ({'PASS' if computational_family_gate['pass'] else 'FAIL'})", "", "## Tiers", ""]
    lines.extend(f"- {key}: {value}" for key, value in sorted(tiers.items()))
    lines.extend(["", "## First blockers", ""])
    lines.extend(f"- {key}: {value}" for key, value in sorted(first_blockers.items()))
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    frontend_details = []
    for opcode, count in sorted(features.items()):
        observed = [item for item in results if item.get("feature", {}).get("opcode_histogram", {}).get(opcode, 0)]
        succeeded = [item for item in observed if TIERS.get(item.get("tier", "DISCOVERED"), 0) >= 2]
        frontend_details.append({
            "construct": opcode,
            "observed_count": count,
            "observed_loop_count": len(observed),
            "frontend_success_loop_count": len(succeeded),
            "blocked_loop_count": len(observed) - len(succeeded),
            "examples": [item["id"] for item in observed[:3]],
        })
    frontend_rows = [(item["construct"], item["observed_count"], item["observed_loop_count"], item["frontend_success_loop_count"], item["blocked_loop_count"]) for item in frontend_details]
    write_json(out / "frontend_coverage.json", {"schema": "cgra.cgra_bench.frontend_coverage.v1", "loop_count": candidate_loop_count, "constructs": frontend_details, "opcode_histogram": dict(sorted(features.items())), "type_histogram": dict(sorted(feature_types.items())), "icmp_predicate_histogram": dict(sorted(feature_predicates.items())), "feature_counts": dict(sorted(feature_counts.items())), "first_blocker_distribution": dict(sorted(first_blockers.items()))})
    with (out / "frontend_coverage.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["llvm_construct", "observed_count", "observed_loops", "frontend_success_loops", "blocked_loops"])
        writer.writerows(frontend_rows)
        writer.writerow([])
        writer.writerow(["first_blocker", "count"])
        writer.writerows(sorted(first_blockers.items()))
        writer.writerow([])
        writer.writerow(["llvm_type", "observed_count"])
        writer.writerows(sorted(feature_types.items()))
    (out / "frontend_coverage.md").write_text(
        "# Frontend Coverage\n\n"
        f"Candidate loops: {candidate_loop_count}\n\n"
        + "\n".join(f"- `{opcode}`: {count} observations in {observed} loops; frontend success={success}, blocked={blocked}" for opcode, count, observed, success, blocked in frontend_rows)
        + "\n\n## First blockers\n\n"
        + "\n".join(f"- `{code}`: {count}" for code, count in sorted(first_blockers.items()))
        + "\n",
        encoding="utf-8",
    )
    isa_details = []
    for operation_key, count in sorted(operations.items()):
        observed = [
            item for item, operation_histogram in zip(results, operation_histograms)
            if operation_key in operation_histogram
        ]
        target_legal = [item for item in observed if TIERS.get(item.get("tier", "DISCOVERED"), 0) >= 3]
        target_blocked = [item for item in observed if item.get("category") == "TARGET_ISA"]
        opcode = "ICmp" if operation_key.upper() == "ICMP" else operation_key
        isa_details.append({
            "opcode": opcode,
            "operation_key": operation_key,
            "observed_count": count,
            "case_count": len(observed),
            "target_contract": target_contract_summary(operation_key, target_operations, opcode),
            "target_legal_case_count": len(target_legal),
            "target_blocked_case_count": len(target_blocked),
            "examples": [item["id"] for item in observed[:3]],
        })
    isa_rows = [(item["operation_key"], item["observed_count"], item["case_count"], item["target_contract"], item["target_legal_case_count"], item["target_blocked_case_count"]) for item in isa_details]
    write_json(out / "isa_coverage.json", {"schema": "cgra.cgra_bench.isa_coverage.v1", "loop_count": candidate_loop_count, "generic_opcode_histogram": dict(sorted(isa.items())), "generic_operation_histogram": dict(sorted(operations.items())), "target_operations": sorted(target_operations), "operations": isa_details})
    with (out / "isa_coverage.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["generic_operation", "observed_count", "case_count", "target_contract", "target_legal_cases", "target_blocked_cases"])
        writer.writerows(isa_rows)
    (out / "isa_coverage.md").write_text(
        "# Target ISA Coverage\n\n"
        + "\n".join(f"- `{opcode}`: {count} observations in {case_count} cases; contract={contract}, legal={legal}, blocked={blocked}" for opcode, count, case_count, contract, legal, blocked in isa_rows)
        + "\n",
        encoding="utf-8",
    )
    gaps = [{
        "diagnostic_code": code,
        "blocked_cases": count,
        "blocked_loops": sum(item.get("loop_header") is not None and item.get("diagnostic_code") == code for item in results),
        "blocked_kernels": len({item.get("kernel") for item in results if item.get("diagnostic_code") == code}),
        "category": next((item.get("category") for item in results if item.get("diagnostic_code") == code), "INTERNAL"),
    } for code, count in first_blockers.most_common()]
    write_json(out / "gap_ranking.json", {"schema": "cgra.cgra_bench.gap_ranking.v1", "gaps": gaps})
    (out / "gap_ranking.md").write_text("# CGRA-Bench Gap Ranking\n\n" + "\n".join(f"- `{item['diagnostic_code']}`: {item['blocked_loops']} loops / {item['blocked_cases']} terminal cases / {item['blocked_kernels']} kernels ({item['category']})" for item in gaps) + "\n", encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", required=True, type=pathlib.Path)
    args = parser.parse_args()
    corpus = read_json(args.run / "corpus.json")
    results = [json.loads(line) for line in (args.run / "results.jsonl").read_text(encoding="utf-8").splitlines() if line.strip()]
    report(args.run, corpus, results)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
