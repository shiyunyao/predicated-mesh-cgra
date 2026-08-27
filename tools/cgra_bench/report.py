#!/usr/bin/env python3
"""Aggregate structured T019 case results into reproducible reports."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
from collections import Counter, defaultdict
from typing import Any

try:
    from .schemas import read_json, write_json
except ImportError:  # pragma: no cover - direct script execution
    from schemas import read_json, write_json


TIERS = {"DISCOVERED": 0, "LLVM_BUILT": 1, "FRONTEND_DFG": 2, "TARGET_LEGAL": 3, "MAPPED": 4, "RF_COMPLETE": 5, "MANIFEST_COMPLETE": 6, "FUNCTIONAL_RTL_VALIDATED": 7}


def report(out: pathlib.Path, corpus: dict[str, Any], results: list[dict[str, Any]]) -> dict[str, Any]:
    terminal = Counter(item.get("category", "INTERNAL") for item in results)
    tiers = Counter(item.get("tier", "DISCOVERED") for item in results)
    first_blockers = Counter(item.get("diagnostic_code", "UNCLASSIFIED_RESULT") for item in results if item.get("tier") != "MANIFEST_COMPLETE")
    features = Counter()
    feature_types = Counter()
    feature_predicates = Counter()
    feature_counts = Counter()
    isa = Counter()
    for item in results:
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
    target_operations: dict[str, Any] = {}
    environment_path = out / "environment.json"
    if environment_path.exists():
        try:
            environment = read_json(environment_path)
            target_path = pathlib.Path(environment.get("target", ""))
            if target_path.exists():
                target_operations = read_json(target_path).get("operations", {})
        except (OSError, ValueError, json.JSONDecodeError):
            target_operations = {}
    candidate_loop_count = sum(item.get("loop_header") is not None for item in results)
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
            "excluded_sources": len(excluded_sources),
            "missing_sources": missing_sources,
            "unexpected_sources": unexpected_sources,
            "ok": not missing_sources and not unexpected_sources,
        },
        "tiers": dict(sorted(tiers.items())),
        "terminal_categories": dict(sorted(terminal.items())),
        "unknown_count": sum(1 for item in results if item.get("category") == "INTERNAL" or item.get("owner") == "UNKNOWN"),
        "first_blocker_distribution": dict(sorted(first_blockers.items())),
        "all_observed_opcodes": dict(sorted(features.items())),
        "all_observed_types": dict(sorted(feature_types.items())),
        "all_observed_icmp_predicates": dict(sorted(feature_predicates.items())),
        "all_observed_feature_counts": dict(sorted(feature_counts.items())),
        "generic_opcode_histogram": dict(sorted(isa.items())),
        "backend_metrics": {
            "cases_with_stats": len(backend_stats),
            "mii": [stats.get("mii") for stats in backend_stats if stats.get("mii") is not None],
            "mapped_ii": [stats.get("mapped_ii") for stats in backend_stats if stats.get("mapped_ii") is not None],
            "node_candidate_attempts": sum(stats.get("node_candidate_attempts", 0) for stats in backend_stats),
            "route_state_expansions": sum(stats.get("route_state_expansions", 0) for stats in backend_stats),
            "completed_modulo_mappings": sum(stats.get("completed_modulo_mappings", 0) for stats in backend_stats),
            "rf_rejected": sum(stats.get("rf_rejected", 0) for stats in backend_stats),
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
    lines = ["# CGRA-Bench Audit Summary", "", f"- Source units represented: {source_count}/{len(expected_sources)}", f"- Candidate loops: {candidate_loop_count}", f"- Terminal results: {len(results)}", f"- UNKNOWN/unclassified: {summary['unknown_count']}", f"- Reconciliation: {'PASS' if summary['reconciliation']['ok'] else 'FAIL'}", "", "## Tiers", ""]
    lines.extend(f"- {key}: {value}" for key, value in sorted(tiers.items()))
    lines.extend(["", "## First blockers", ""])
    lines.extend(f"- {key}: {value}" for key, value in sorted(first_blockers.items()))
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    frontend_rows = [(opcode, count) for opcode, count in sorted(features.items())]
    write_json(out / "frontend_coverage.json", {"schema": "cgra.cgra_bench.frontend_coverage.v1", "loop_count": candidate_loop_count, "opcode_histogram": dict(frontend_rows), "type_histogram": dict(sorted(feature_types.items())), "icmp_predicate_histogram": dict(sorted(feature_predicates.items())), "feature_counts": dict(sorted(feature_counts.items())), "first_blocker_distribution": dict(sorted(first_blockers.items()))})
    with (out / "frontend_coverage.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["llvm_construct", "observed_count"])
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
        + "\n".join(f"- `{opcode}`: {count}" for opcode, count in frontend_rows)
        + "\n\n## First blockers\n\n"
        + "\n".join(f"- `{code}`: {count}" for code, count in sorted(first_blockers.items()))
        + "\n",
        encoding="utf-8",
    )
    isa_rows = [
        (opcode, count, opcode.upper() in {key.upper() for key in target_operations}, sum(1 for item in results if opcode in item.get("dfg", {}).get("opcode_histogram", {})))
        for opcode, count in sorted(isa.items())
    ]
    write_json(out / "isa_coverage.json", {"schema": "cgra.cgra_bench.isa_coverage.v1", "loop_count": candidate_loop_count, "generic_opcode_histogram": dict(sorted(isa.items())), "target_operations": sorted(target_operations), "operations": [{"opcode": opcode, "observed_count": count, "target_supported": supported, "case_count": case_count} for opcode, count, supported, case_count in isa_rows]})
    with (out / "isa_coverage.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["generic_op", "observed_count", "case_count", "target_supported"])
        writer.writerows(isa_rows)
    (out / "isa_coverage.md").write_text(
        "# Target ISA Coverage\n\n"
        + "\n".join(f"- `{opcode}`: {count} observations in {case_count} cases; target_supported={supported}" for opcode, count, supported, case_count in isa_rows)
        + "\n",
        encoding="utf-8",
    )
    gaps = [{"diagnostic_code": code, "blocked_loops": count, "category": next((item.get("category") for item in results if item.get("diagnostic_code") == code), "INTERNAL")} for code, count in first_blockers.most_common()]
    write_json(out / "gap_ranking.json", {"schema": "cgra.cgra_bench.gap_ranking.v1", "gaps": gaps})
    (out / "gap_ranking.md").write_text("# CGRA-Bench Gap Ranking\n\n" + "\n".join(f"- `{item['diagnostic_code']}`: {item['blocked_loops']} loops ({item['category']})" for item in gaps) + "\n", encoding="utf-8")
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
