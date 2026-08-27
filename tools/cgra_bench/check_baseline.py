#!/usr/bin/env python3
"""Reject regressions from the checked-in CGRA-Bench mapped-case baseline."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

try:
    from .evidence import STAGES
    from .report import TIERS
    from .schemas import read_json
except ImportError:  # pragma: no cover - direct script execution
    from evidence import STAGES
    from report import TIERS
    from schemas import read_json


def check(run: pathlib.Path, baseline_path: pathlib.Path) -> list[str]:
    baseline = read_json(baseline_path)
    if baseline.get("schema") != "cgra.cgra_bench.known_supported.v1":
        return ["known-supported baseline schema mismatch"]
    expected = baseline.get("cases", [])
    ids = [entry["id"] if isinstance(entry, dict) else entry for entry in expected]
    if len(ids) != len(set(ids)):
        return ["known-supported baseline contains duplicate case IDs"]
    results = {
        result["id"]: result
        for line in (run / "results.jsonl").read_text(encoding="utf-8").splitlines()
        if line.strip()
        for result in [json.loads(line)]
    }
    failures = []
    for case_id, result in sorted(results.items()):
        if result.get("terminal_stage") not in STAGES:
            failures.append(f"{case_id}: invalid terminal stage {result.get('terminal_stage')}")
        if not result.get("category") or not result.get("diagnostic_code") or not result.get("owner"):
            failures.append(f"{case_id}: incomplete terminal classification")
        if result.get("category") == "INTERNAL" or result.get("owner") == "UNKNOWN":
            failures.append(f"{case_id}: unclassified terminal result")
    for entry in expected:
        case_id = entry["id"] if isinstance(entry, dict) else entry
        minimum = entry.get("minimum_tier", "MAPPED") if isinstance(entry, dict) else "MAPPED"
        if minimum not in TIERS:
            failures.append(f"{case_id}: unknown minimum tier {minimum}")
            continue
        result = results.get(case_id)
        if result is None:
            failures.append(f"{case_id}: missing from audit results")
        elif TIERS.get(result.get("tier", "DISCOVERED"), 0) < TIERS[minimum]:
            failures.append(
                f"{case_id}: regressed to {result.get('tier')} "
                f"({result.get('diagnostic_code')}); expected at least {minimum}"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", required=True, type=pathlib.Path)
    parser.add_argument(
        "--baseline",
        type=pathlib.Path,
        default=pathlib.Path("benchmarks/cgra-bench/known_supported.v1.json"),
    )
    args = parser.parse_args()
    try:
        failures = check(args.run, args.baseline)
    except (OSError, KeyError, ValueError, json.JSONDecodeError) as error:
        print(f"cgra-bench baseline: {error}", file=sys.stderr)
        return 2
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("CGRA-Bench known-supported baseline: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
