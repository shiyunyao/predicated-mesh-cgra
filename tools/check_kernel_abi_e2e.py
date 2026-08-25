#!/usr/bin/env python3
"""Check ABI output semantics using the generated signature and layout artifacts."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys
from collections import Counter


STORE_OPS = {"2", "STORE"}
COMMITTED = {"1", "true", "True"}


def parse_word(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        return int(text, 16)


def stores(path: pathlib.Path) -> list[tuple[int, int, int]]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"{path}: trace is empty")
    required = {"cycle", "lsu_op", "lsu_addr", "lsu_store_data", "lsu_store_commit"}
    missing = required - set(rows[0])
    if missing:
        raise ValueError(f"{path}: trace is missing fields {sorted(missing)}")
    return [
        (int(row["cycle"]), parse_word(row["lsu_addr"]), parse_word(row["lsu_store_data"]))
        for row in rows
        if row["lsu_op"] in STORE_OPS and row["lsu_store_commit"] in COMMITTED
    ]


def output_addresses(signature: dict, layout: dict, expected: dict) -> dict[str, int]:
    by_name = {item["name"]: item["id"] for item in signature.get("outputs", [])}
    by_id = {item["id"]: item["scratchpad_word_address"] for item in layout.get("outputs", [])}
    result: dict[str, int] = {}
    for name in expected.get("outputs", {}):
        if name not in by_name:
            raise ValueError(f"expected output {name!r} is absent from kernel signature")
        output_id = by_name[name]
        if output_id not in by_id:
            raise ValueError(f"signature output {name!r} is absent from ABI layout")
        result[name] = by_id[output_id]
    return result


def check_trace(path: pathlib.Path, addresses: dict[str, int], expected: dict) -> list[tuple[int, int, int]]:
    observed = stores(path)
    expected_outputs = expected.get("outputs", {})
    allowed = set(addresses.values())
    unexpected = [item for item in observed if item[1] not in allowed]
    if unexpected:
        raise ValueError(f"{path}: unexpected committed stores {unexpected}")
    for name, address in addresses.items():
        values = [item for item in observed if item[1] == address]
        spec = expected_outputs[name]
        if len(values) != int(spec["store_count"]):
            raise ValueError(
                f"{path}: output {name!r} store count differs; "
                f"expected={spec['store_count']} observed={len(values)}"
            )
        expected_value = (
            parse_word(spec["final_value"])
            if isinstance(spec["final_value"], str)
            else int(spec["final_value"])
        )
        if not values or values[-1][2] & 0xFFFFFFFF != expected_value & 0xFFFFFFFF:
            actual = None if not values else values[-1][2] & 0xFFFFFFFF
            raise ValueError(
                f"{path}: output {name!r} final value differs; "
                f"expected={expected_value & 0xFFFFFFFF} observed={actual}"
            )
    return observed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--signature", required=True, type=pathlib.Path)
    parser.add_argument("--layout", required=True, type=pathlib.Path)
    parser.add_argument("--expectation", required=True, type=pathlib.Path)
    parser.add_argument("--golden", required=True, type=pathlib.Path)
    parser.add_argument("--rtl", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        signature = json.loads(args.signature.read_text(encoding="utf-8"))
        layout = json.loads(args.layout.read_text(encoding="utf-8"))
        expected = json.loads(args.expectation.read_text(encoding="utf-8"))
        if expected.get("schema") != "cgra.kernel_abi.expectation.v1":
            raise ValueError("unexpected ABI expectation schema")
        addresses = output_addresses(signature, layout, expected)
        golden = check_trace(args.golden, addresses, expected)
        rtl = check_trace(args.rtl, addresses, expected)
        if Counter((cycle, address, data & 0xFFFFFFFF) for cycle, address, data in golden) != Counter(
            (cycle, address, data & 0xFFFFFFFF) for cycle, address, data in rtl
        ):
            raise ValueError("golden and RTL ABI output observations differ")
        print(f"kernel ABI observations passed: golden={len(golden)} rtl={len(rtl)}")
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"kernel ABI observation failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
