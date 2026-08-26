#!/usr/bin/env python3
"""Check compiler-generated user-memory Store semantics in Golden and RTL traces."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys


STORE_OPS = {"2", "STORE"}
COMMITTED = {"1", "true", "True"}


def expected_word(value: str | int) -> int:
    if isinstance(value, int):
        return value & 0xFFFFFFFF
    return int(value, 0) & 0xFFFFFFFF


def trace_address(value: str) -> int:
    # The RTL trace writes addresses with %0h and no 0x prefix.
    return int(value, 16) & 0xFFFFFFFF


def trace_store_data(value: str) -> int:
    # The RTL trace writes Store payloads with %0d.
    return int(value, 10) & 0xFFFFFFFF


def store_observations(path: pathlib.Path) -> list[tuple[int, int, int, bool]]:
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
            trace_address(row["lsu_addr"]),
            trace_store_data(row["lsu_store_data"]),
            row["lsu_store_commit"] in COMMITTED,
        )
        for row in rows
        if row["lsu_op"] in STORE_OPS
    ]


def expected_stores(expectation: dict) -> list[tuple[int, int]]:
    return [
        (expected_word(item["word_address"]), expected_word(item["value"]))
        for item in expectation["committed_stores"]
    ]


def check_trace(path: pathlib.Path, expectation: dict) -> list[tuple[int, int, int, bool]]:
    observed = store_observations(path)
    if len(observed) != int(expectation.get("issued_count", len(observed))):
        raise ValueError(
            f"{path}: Store issue count differs; expected={expectation['issued_count']} "
            f"observed={len(observed)}"
        )
    if not expectation.get("allow_uncommitted", False) and any(not item[3] for item in observed):
        raise ValueError(f"{path}: unexpected uncommitted user Store issue")
    committed = [(item[1], item[2]) for item in observed if item[3]]
    expected = expected_stores(expectation)
    if committed != expected:
        raise ValueError(
            f"{path}: committed Store sequence differs; expected={expected} observed={committed}"
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
        if expectation.get("schema") != "cgra.llvm_memory.expectation.v1":
            raise ValueError("unexpected LLVM memory expectation schema")
        if layout.get("outputs"):
            raise ValueError("T018 memory fixture must not contain ABI output Stores")
        golden = check_trace(args.golden, expectation)
        rtl = check_trace(args.rtl, expectation)
        if golden != rtl:
            raise ValueError("Golden and RTL user-memory Store observations differ")
        print(
            "LLVM memory observations passed: "
            f"issues={len(golden)} commits={sum(item[3] for item in golden)}"
        )
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"LLVM memory observation failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
