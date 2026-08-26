#!/usr/bin/env python3
"""Check user Store issue/commit semantics in Golden and RTL traces."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys


STORE_OPS = {"2", "STORE"}
COMMITTED = {"1", "true", "True"}


def trace_word(text: str) -> int:
    return int(text, 16)


def observations(path: pathlib.Path) -> list[tuple[int, int, int, bool]]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"{path}: trace is empty")
    required = {"cycle", "lsu_op", "lsu_addr", "lsu_store_data", "lsu_store_commit"}
    missing = required - set(rows[0])
    if missing:
        raise ValueError(f"{path}: trace is missing fields {sorted(missing)}")
    return [
        (
            int(row["cycle"]),
            trace_word(row["lsu_addr"]),
            trace_word(row["lsu_store_data"]),
            row["lsu_store_commit"] in COMMITTED,
        )
        for row in rows
        if row["lsu_op"] in STORE_OPS
    ]


def check_trace(path: pathlib.Path, spec: dict) -> list[tuple[int, int, int, bool]]:
    observed = observations(path)
    address = int(spec["word_address"])
    if any(item[1] != address for item in observed):
        raise ValueError(f"{path}: unexpected user Store address in {observed}")
    if len(observed) != int(spec["issued_count"]):
        raise ValueError(
            f"{path}: Store issue count differs; "
            f"expected={spec['issued_count']} observed={len(observed)}"
        )
    committed = [item[2] & 0xFFFFFFFF for item in observed if item[3]]
    expected = [int(value) & 0xFFFFFFFF for value in spec["committed_data"]]
    if committed != expected:
        raise ValueError(
            f"{path}: committed Store data differs; expected={expected} observed={committed}"
        )
    return observed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--layout", required=True, type=pathlib.Path)
    parser.add_argument("--expectation", required=True, type=pathlib.Path)
    parser.add_argument("--golden", required=True, type=pathlib.Path)
    parser.add_argument("--rtl", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        layout = json.loads(args.layout.read_text(encoding="utf-8"))
        expectation = json.loads(args.expectation.read_text(encoding="utf-8"))
        if expectation.get("schema") != "cgra.predicated_store.expectation.v1":
            raise ValueError("unexpected predicated Store expectation schema")
        if layout.get("outputs"):
            raise ValueError("predicated Store fixture must not contain ABI output Stores")
        spec = expectation["user_store"]
        golden = check_trace(args.golden, spec)
        rtl = check_trace(args.rtl, spec)
        if golden != rtl:
            raise ValueError("Golden and RTL Store issue/commit observations differ")
        commits = sum(item[3] for item in golden)
        print(f"predicated Store observations passed: issues={len(golden)} commits={commits}")
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"predicated Store observation failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
