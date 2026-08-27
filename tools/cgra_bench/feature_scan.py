#!/usr/bin/env python3
"""Conservative source-wide feature scan for standalone audit diagnostics."""

from __future__ import annotations

import argparse
import pathlib
import re
from collections import Counter

try:
    from .schemas import write_json
except ImportError:  # pragma: no cover - direct script execution
    from schemas import write_json


OPCODE_RE = re.compile(
    r"^\s*(?:[%@][^ ]+\s*=\s*)?(?:tail\s+|musttail\s+|notail\s+)?([A-Za-z][A-Za-z0-9_.]*)\b"
)
TYPE_RE = re.compile(r"\b(i[0-9]+|half|float|double|<[^>]+>|ptr|%[A-Za-z_.$][\w.$]*)\b")


def scan(path: pathlib.Path) -> dict:
    opcodes: Counter[str] = Counter()
    types: Counter[str] = Counter()
    predicates: Counter[str] = Counter()
    functions: set[str] = set()
    blocks = 0
    phis = branches = loads = stores = geps = calls = atomics = volatile = selects = pointer_phis = 0
    text = path.read_text(encoding="utf-8")
    current_function = None
    for line in text.splitlines():
        if line.startswith("define "):
            match = re.search(r"@([^ (]+)\(", line)
            current_function = match.group(1) if match else None
            if current_function:
                functions.add(current_function)
        if current_function and line and not line.startswith(" ") and line.endswith(":"):
            blocks += 1
        if not line[:1].isspace():
            continue
        if re.match(r"i[0-9]+\s", line.strip()):
            continue
        opcode_match = OPCODE_RE.match(line)
        if not opcode_match:
            continue
        opcode = opcode_match.group(1).lower()
        opcodes[opcode] += 1
        types.update(TYPE_RE.findall(line))
        if opcode == "phi":
            phis += 1
            if "*" in line or re.search(r"\bptr\b", line):
                pointer_phis += 1
        if opcode == "br":
            branches += 1
        if opcode == "load":
            loads += 1
        if opcode == "store":
            stores += 1
        if opcode == "getelementptr":
            geps += 1
        if opcode in {"call", "invoke", "callbr"}:
            calls += 1
        if opcode in {"atomicrmw", "cmpxchg", "fence"}:
            atomics += 1
        if " volatile " in f" {line} ":
            volatile += 1
        if opcode == "select":
            selects += 1
        if opcode == "icmp":
            match = re.search(r"icmp\s+([a-z]+)", line)
            if match:
                predicates[match.group(1)] += 1
    return {
        "schema": "cgra.llvm_feature_inventory.v1",
        "source": str(path),
        "functions": sorted(functions),
        "opcode_histogram": dict(sorted(opcodes.items())),
        "type_histogram": dict(sorted(types.items())),
        "icmp_predicate_histogram": dict(sorted(predicates.items())),
        "counts": {
            "basic_blocks": blocks, "phis": phis, "pointer_phis": pointer_phis,
            "branches": branches, "loads": loads, "stores": stores, "geps": geps,
            "calls": calls, "atomics_or_fences": atomics, "volatile": volatile,
            "selects": selects,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ir", type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()
    write_json(args.out, scan(args.ir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
