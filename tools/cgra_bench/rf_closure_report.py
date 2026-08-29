#!/usr/bin/env python3
"""Create machine-readable and Markdown RF-closure ablation summaries."""
from __future__ import annotations

import argparse
import csv
import json
import pathlib
from collections import Counter


def load_run(path: pathlib.Path) -> tuple[dict, list[dict]]:
    summary = json.loads((path / "summary.json").read_text())
    results = [json.loads(line) for line in (path / "results.jsonl").read_text().splitlines() if line]
    return summary, results


def metrics(summary: dict, results: list[dict]) -> dict:
    backend = summary.get("backend_metrics", {})
    strict = sum(item.get("tier") == "RF_CONSTRAINED_MAPPED" for item in results)
    raw = sum(item.get("tier") == "ROUTE_MAPPED" for item in results)
    return {
        "all_discovered_loops": len(results),
        "mapper_entered": sum(item.get("tier") in {"ROUTE_MAPPED", "RF_CONSTRAINED_MAPPED"} for item in results),
        "raw_route_mapped": raw,
        "strict_rf_mapped": strict,
        "timeout": summary.get("timeout_count", 0),
        "unknown": summary.get("unknown_count", 0),
        "stage_rejected": backend.get("stage_rejected", 0),
        "rf_rejected": backend.get("rf_rejected", 0),
        "late_read_port_conflicts": backend.get("late_read_port_conflicts", 0),
        "late_write_port_conflicts": backend.get("late_write_port_conflicts", 0),
    }


def _metric(item: dict, *keys: str):
    backend = item.get("backend", {})
    stats = backend.get("stats", {}) if isinstance(backend, dict) else {}
    for key in keys:
        if key in item:
            return item[key]
        if key in stats:
            return stats[key]
    return ""


def generate(run_paths: dict[str, pathlib.Path], output: pathlib.Path) -> dict:
    runs = {name: load_run(path) for name, path in run_paths.items()}
    case_maps = {name: {item.get("id"): item for item in values[1]} for name, values in runs.items()}
    case_ids = sorted(set().union(*(mapping for mapping in case_maps.values())))
    rows = []
    for case_id in case_ids:
        row = {"case_id": case_id}
        for name, mapping in case_maps.items():
            item = mapping.get(case_id, {})
            row[f"{name}_status"] = item.get("terminal_status", item.get("status", "MISSING"))
            row[f"{name}_blocker"] = item.get("diagnostic_code", "")
            row[f"{name}_mii"] = _metric(item, "mii")
            row[f"{name}_safe_ii"] = _metric(item, "safe_ii", "safeII")
        rows.append(row)
    output.mkdir(parents=True, exist_ok=True)
    summary = {name: metrics(*runs[name]) for name in runs}
    summary["case_ids"] = case_ids
    (output / "impact_summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    with (output / "per_case_delta.csv").open("w", newline="") as stream:
        fields = list(rows[0]) if rows else ["case_id"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    blocker = Counter((row.get("baseline_blocker", ""), row.get("combined_blocker", "")) for row in rows)
    with (output / "rf_failure_migration.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["baseline_blocker", "combined_blocker", "case_count"])
        writer.writerows([*key, count] for key, count in sorted(blocker.items()))
    # Keep the historical filename as a compatibility alias while the
    # contents now explicitly describe diagnostic migration.
    (output / "blocker_migration.csv").write_text(
        (output / "rf_failure_migration.csv").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    terminal = Counter((row.get("baseline_status", ""), row.get("combined_status", ""))
                       for row in rows)
    with (output / "terminal_status_migration.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["baseline_status", "combined_status", "case_count"])
        writer.writerows([*key, count] for key, count in sorted(terminal.items()))
    with (output / "safe_ii_distribution.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["case_id", "mii", "safe_ii", "safe_ii_over_mii", "solution_kind"])
        for item in case_maps["combined"].values():
            mii = _metric(item, "mii")
            safe = _metric(item, "safe_ii", "safeII")
            ratio = ""
            if isinstance(mii, (int, float)) and isinstance(safe, (int, float)) and mii:
                ratio = safe / mii
            writer.writerow([item.get("id", ""), mii, safe, ratio,
                             _metric(item, "mapping_solution_kind", "solution_kind")])
    markdown = [
        "# RF Closure Impact Report", "",
        "The four lanes use the same corpus, target, profile, ABI and budgets.", "",
        "| Metric | Baseline | Port only | MVE only | Combined |",
        "|---|---:|---:|---:|---:|",
    ]
    names = ("baseline", "port_only", "mve_only", "combined")
    for key in ("mapper_entered", "raw_route_mapped", "strict_rf_mapped", "timeout", "unknown", "stage_rejected", "rf_rejected"):
        markdown.append("| " + key + " | " + " | ".join(str(summary[name][key]) for name in names) + " |")
    markdown.extend(["", f"Cases compared: {len(case_ids)}", ""])
    (output / "RF_CLOSURE_IMPACT_REPORT.md").write_text("\n".join(markdown) + "\n")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ("baseline", "port-only", "mve-only", "combined"):
        parser.add_argument("--" + name, type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    generate({
        "baseline": args.baseline,
        "port_only": args.port_only,
        "mve_only": args.mve_only,
        "combined": args.combined,
    }, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
