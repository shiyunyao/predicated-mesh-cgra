#!/usr/bin/env python3
"""Freeze the L4-or-higher CGRA-Bench cases from one completed audit."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any

try:
    from .report import TIERS
    from .schemas import read_json, write_json
except ImportError:  # pragma: no cover - direct script execution
    from report import TIERS
    from schemas import read_json, write_json


def freeze(results: list[dict[str, Any]], environment: dict[str, Any]) -> dict[str, Any]:
    """Return the deterministic mapped-case contract for an audit result set."""
    mapped = sorted(
        {
            result["id"]
            for result in results
            if TIERS.get(result.get("tier", "DISCOVERED"), 0) >= TIERS["MAPPED"]
        }
    )
    return {
        "schema": "cgra.cgra_bench.known_supported.v1",
        "source": {
            "project_sha": environment.get("project_sha"),
            "corpus_sha": environment.get("corpus_sha"),
            "target_sha256": environment.get("target_sha256"),
            "profile": environment.get("profile", {}).get("name"),
        },
        "cases": [{"id": case_id, "minimum_tier": "MAPPED"} for case_id in mapped],
        "note": (
            "Generated from a completed audit. An empty list means that no case "
            "reached L4 MAPPED in that exact environment."
            if not mapped
            else "Generated from a completed audit; listed cases must not regress below L4 MAPPED."
        ),
    }


def load_completed_run(run: pathlib.Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Load evidence only when the audit is eligible to become a release baseline."""
    summary = read_json(run / "summary.json")
    if summary.get("schema") != "cgra.cgra_bench.summary.v1":
        raise ValueError("audit summary schema mismatch")
    reconciliation = summary.get("reconciliation", {})
    if not reconciliation.get("ok"):
        raise ValueError("cannot freeze an audit whose corpus reconciliation failed")
    if summary.get("unknown_count"):
        raise ValueError("cannot freeze an audit with UNKNOWN results")
    if summary.get("timeout_count"):
        raise ValueError("cannot freeze an audit with timeouts")
    environment = read_json(run / "environment.json")
    if environment.get("schema") != "cgra.cgra_bench.environment.v1":
        raise ValueError("audit environment schema mismatch")
    if environment.get("project_dirty"):
        raise ValueError("cannot freeze an audit from a dirty project worktree")
    results = [
        json.loads(line)
        for line in (run / "results.jsonl").read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    if len(results) != summary.get("denominator", {}).get("terminal_results"):
        raise ValueError("audit results do not match the summary terminal-result count")
    case_ids = [result.get("id") for result in results]
    if None in case_ids or len(case_ids) != len(set(case_ids)):
        raise ValueError("audit results contain missing or duplicate case IDs")
    return results, environment


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", required=True, type=pathlib.Path)
    parser.add_argument(
        "--out",
        type=pathlib.Path,
        default=pathlib.Path("benchmarks/cgra-bench/known_supported.v1.json"),
    )
    args = parser.parse_args()
    try:
        results, environment = load_completed_run(args.run)
        write_json(args.out, freeze(results, environment))
    except (OSError, KeyError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
