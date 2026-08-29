#!/usr/bin/env python3
"""Extract a small deterministic RF-failure corpus from an audit run."""

from __future__ import annotations

import argparse
import json
import pathlib
from collections import defaultdict
from typing import Any


def _reason(item: dict[str, Any]) -> str:
    backend = item.get("backend", {})
    if isinstance(backend, dict):
        reasons = backend.get("stats", {}).get("rf_rejected_by_reason", {})
        if isinstance(reasons, dict) and reasons:
            return str(max(reasons, key=lambda key: int(reasons[key])))
        physical = backend.get("physical_realizability", {})
        if isinstance(physical, dict) and physical.get("reason_code"):
            return str(physical["reason_code"])
    return str(item.get("diagnostic_code", ""))


def _bucket(reason: str) -> str:
    value = reason.lower()
    if "same_address" in value:
        return "same_address"
    if "self_overlap" in value:
        return "fixed_self_overlap"
    if "read_port" in value:
        return "read_port"
    if "write_port" in value or "write_source" in value:
        return "write_port"
    if "depth" in value or "register" in value:
        return "depth"
    if "rf" in value:
        return "mixed"
    return "mixed"


def extract(run: pathlib.Path) -> dict[str, Any]:
    results = run / "results.jsonl"
    if not results.is_file():
        raise ValueError(f"missing results.jsonl: {results}")
    buckets: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for line in results.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        item = json.loads(line)
        reason = _reason(item)
        if not reason or "rf" not in reason.lower():
            continue
        buckets[_bucket(reason)].append({
            "id": item.get("id"),
            "reason_code": reason,
            "tier": item.get("tier"),
            "terminal_status": item.get("terminal_status"),
        })
    # Stable order makes the manifest suitable for checked-in regression use.
    return {
        "schema": "cgra.cgra_bench.rf_failure_cases.v1",
        "source_run": str(run),
        "fixed_self_overlap": sorted(buckets.get("fixed_self_overlap", []), key=lambda x: str(x["id"])),
        "read_port": sorted(buckets.get("read_port", []), key=lambda x: str(x["id"])),
        "write_port": sorted(buckets.get("write_port", []), key=lambda x: str(x["id"])),
        "same_address": sorted(buckets.get("same_address", []), key=lambda x: str(x["id"])),
        "depth": sorted(buckets.get("depth", []), key=lambda x: str(x["id"])),
        "mixed": sorted(buckets.get("mixed", []), key=lambda x: str(x["id"])),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    value = extract(args.run)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({key: len(value[key]) for key in value if isinstance(value[key], list)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
