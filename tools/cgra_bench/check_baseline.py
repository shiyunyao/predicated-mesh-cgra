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
    parsed = [
        json.loads(line)
        for line in (run / "results.jsonl").read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    result_ids = [result["id"] for result in parsed]
    if len(result_ids) != len(set(result_ids)):
        return ["audit results contain duplicate case IDs"]
    results = {result["id"]: result for result in parsed}
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


def check_expectations(run: pathlib.Path, expectations_path: pathlib.Path) -> list[str]:
    expectations = read_json(expectations_path)
    if expectations.get("schema") != "cgra.cgra_bench.smoke_expectations.v1":
        return ["smoke expectation schema mismatch"]
    parsed = [
        json.loads(line)
        for line in (run / "results.jsonl").read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    result_ids = [result["id"] for result in parsed]
    if len(result_ids) != len(set(result_ids)):
        return ["audit results contain duplicate case IDs"]
    results = {result["id"]: result for result in parsed}
    failures = []
    for expected in expectations.get("cases", []):
        result = results.get(expected["id"])
        if result is None:
            failures.append(f"smoke expectation missing case: {expected['id']}")
            continue
        if result.get("terminal_stage") != expected["terminal_stage"]:
            failures.append(
                f"{expected['id']}: terminal stage changed from {expected['terminal_stage']} "
                f"to {result.get('terminal_stage')}"
            )
        accepted = expected.get("diagnostic_codes", [expected.get("diagnostic_code")])
        if result.get("diagnostic_code") not in accepted:
            failures.append(
                f"{expected['id']}: diagnostic changed to {result.get('diagnostic_code')}; "
                f"expected one of {accepted}"
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
    parser.add_argument(
        "--expectations",
        type=pathlib.Path,
        default=None,
        help="optional fixed smoke classification contract",
    )
    args = parser.parse_args()
    try:
        failures = check(args.run, args.baseline)
        if args.expectations:
            failures.extend(check_expectations(args.run, args.expectations))
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
