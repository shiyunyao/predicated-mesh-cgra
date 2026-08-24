#!/usr/bin/env python3
"""Check handwritten semantic observations against independent execution traces."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys
from collections import Counter


STORE_OPS = {"2", "STORE"}
COMMITTED = {"1", "true", "True"}


def load_stores(path: pathlib.Path) -> list[tuple[int, int, int]]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"{path}: trace is empty")
    required = {"cycle", "lsu_op", "lsu_addr", "lsu_store_data", "lsu_store_commit"}
    missing = required - set(rows[0])
    if missing:
        raise ValueError(f"{path}: trace is missing fields {sorted(missing)}")
    stores: list[tuple[int, int, int]] = []
    for row in rows:
        if row["lsu_op"] not in STORE_OPS or row["lsu_store_commit"] not in COMMITTED:
            continue
        stores.append((int(row["cycle"]), int(row["lsu_addr"], 0), int(row["lsu_store_data"], 0)))
    return stores


def expected_stores(expectation: dict) -> Counter[tuple[int, int]]:
    result: Counter[tuple[int, int]] = Counter()
    for item in expectation.get("committed_stores", []):
        result[(int(item["address"]), int(item["data"]) & 0xFFFFFFFF)] += int(item["count"])
    return result


def check_trace(path: pathlib.Path, expectation: dict) -> list[tuple[int, int, int]]:
    stores = load_stores(path)
    observed = Counter((address, data & 0xFFFFFFFF) for _, address, data in stores)
    expected = expected_stores(expectation)
    if observed != expected:
        raise ValueError(f"{path}: committed stores differ; expected={dict(expected)} observed={dict(observed)}")
    if expectation.get("unexpected_store_policy") == "forbid":
        expected_addresses = {address for address, _ in expected}
        unexpected = [entry for entry in stores if entry[1] not in expected_addresses]
        if unexpected:
            raise ValueError(f"{path}: unexpected committed stores {unexpected}")
    return stores


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expectation", required=True, type=pathlib.Path)
    parser.add_argument("--golden", required=True, type=pathlib.Path)
    parser.add_argument("--rtl", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        expectation = json.loads(args.expectation.read_text(encoding="utf-8"))
        golden = check_trace(args.golden, expectation)
        rtl = check_trace(args.rtl, expectation)
        if Counter((address, data & 0xFFFFFFFF) for _, address, data in golden) != Counter(
            (address, data & 0xFFFFFFFF) for _, address, data in rtl
        ):
            raise ValueError("golden and RTL committed-store observations differ")
        print(f"compiler E2E observations passed: golden={len(golden)} rtl={len(rtl)}")
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"compiler E2E observation failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
