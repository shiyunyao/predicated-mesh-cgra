#!/usr/bin/env python3
"""Compare two complete CGRA-Bench audit directories by stable case ID."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
from collections import Counter
from typing import Any


def load_results(path: pathlib.Path) -> dict[str, dict[str, Any]]:
    file = path / "results.jsonl"
    if not file.is_file():
        raise ValueError(f"missing results.jsonl: {file}")
    result: dict[str, dict[str, Any]] = {}
    for line in file.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        item = json.loads(line)
        case_id = item.get("id") or item.get("case_id")
        if not case_id or case_id in result:
            raise ValueError(f"invalid or duplicate case id in {file}: {case_id}")
        result[case_id] = item
    return result


def _metric(item: dict[str, Any], name: str) -> Any:
    if name in item:
        return item[name]
    dfg = item.get("dfg", {})
    if name == "node_count":
        return dfg.get("node_count")
    if name == "edge_count":
        return dfg.get("edge_count")
    backend = item.get("backend", {})
    stats = backend.get("stats", {}) if isinstance(backend, dict) else {}
    aliases = {"mii": "mii", "safe_ii": "safe_ii", "mapped_ii": "mapped_ii", "compile_ms": "abi_backend"}
    value = stats.get(aliases.get(name, name))
    if value is not None:
        return value
    if name == "compile_ms":
        durations = item.get("duration_ms", {})
        if isinstance(durations, dict):
            return sum(value for value in durations.values() if isinstance(value, (int, float)))
    return None


def compare(baseline: pathlib.Path, candidate: pathlib.Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    before = load_results(baseline)
    after = load_results(candidate)
    ids = sorted(set(before) | set(after))
    rows: list[dict[str, Any]] = []
    migrations: Counter[tuple[str, str]] = Counter()
    for case_id in ids:
        lhs = before.get(case_id)
        rhs = after.get(case_id)
        row = {
            "case_id": case_id,
            "present_baseline": lhs is not None,
            "present_candidate": rhs is not None,
            "baseline_status": lhs.get("terminal_status", lhs.get("diagnostic_code", "MISSING")) if lhs else "MISSING",
            "candidate_status": rhs.get("terminal_status", rhs.get("diagnostic_code", "MISSING")) if rhs else "MISSING",
            "baseline_first_blocker": lhs.get("diagnostic_code") if lhs else "MISSING",
            "candidate_first_blocker": rhs.get("diagnostic_code") if rhs else "MISSING",
        }
        for name in ("mii", "safe_ii", "mapped_ii", "node_count", "edge_count", "compile_ms"):
            row[f"baseline_{name}"] = _metric(lhs, name) if lhs else None
            row[f"candidate_{name}"] = _metric(rhs, name) if rhs else None
        rows.append(row)
        if lhs and rhs:
            migrations[(str(row["baseline_status"]), str(row["candidate_status"]))] += 1
    summary = {
        "schema": "cgra.cgra_bench.impact_summary.v1",
        "baseline_cases": len(before),
        "candidate_cases": len(after),
        "added_cases": sorted(set(after) - set(before)),
        "removed_cases": sorted(set(before) - set(after)),
        "unmatched_case_count": len(set(before) ^ set(after)),
        "status_migrations": {
            f"{lhs}->{rhs}": count for (lhs, rhs), count in sorted(migrations.items())
        },
        "baseline_status_histogram": dict(Counter(row["baseline_status"] for row in rows)),
        "candidate_status_histogram": dict(Counter(row["candidate_status"] for row in rows)),
    }
    return summary, rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=pathlib.Path, required=True)
    parser.add_argument("--candidate", type=pathlib.Path, required=True)
    parser.add_argument("--json", type=pathlib.Path, required=True)
    parser.add_argument("--csv", type=pathlib.Path, required=True)
    parser.add_argument("--blocker-csv", type=pathlib.Path, required=True)
    args = parser.parse_args()
    summary, rows = compare(args.baseline, args.candidate)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    with args.csv.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]) if rows else ["case_id"])
        writer.writeheader()
        writer.writerows(rows)
    with args.blocker_csv.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["baseline_status", "candidate_status", "case_count"])
        for key, count in sorted(summary["status_migrations"].items()):
            lhs, _, rhs = key.partition("->")
            writer.writerow([lhs, rhs, count])
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
