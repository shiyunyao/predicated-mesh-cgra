#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared CSV trace comparison for golden-model and RTL artifacts."""

from __future__ import annotations

import csv
import pathlib


KEY_FIELDS = ("cycle", "tile_row", "tile_col")


def load_trace(path: str | pathlib.Path) -> dict[tuple[str, str, str], dict[str, str]]:
    """Load one trace and reject duplicate cycle/tile records."""

    records: dict[tuple[str, str, str], dict[str, str]] = {}
    trace_path = pathlib.Path(path)
    with trace_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{trace_path}: missing CSV header")
        for field in KEY_FIELDS:
            if field not in reader.fieldnames:
                raise ValueError(f"{trace_path}: missing key field {field}")
        for row_num, row in enumerate(reader, start=2):
            key = tuple(row[field] for field in KEY_FIELDS)
            if key in records:
                raise ValueError(f"{trace_path}:{row_num}: duplicate trace key {key}")
            records[key] = row
    return records


def compare_trace_paths(golden_path: str | pathlib.Path, rtl_path: str | pathlib.Path) -> list[str]:
    """Return one deterministic diagnostic per missing, extra, or changed field."""

    golden = load_trace(golden_path)
    rtl = load_trace(rtl_path)
    diagnostics: list[str] = []
    for key in sorted(set(golden) | set(rtl), key=lambda item: tuple(int(value) for value in item)):
        if key not in golden:
            diagnostics.append(f"RTL has extra record at cycle={key[0]} tile=({key[1]},{key[2]})")
            continue
        if key not in rtl:
            diagnostics.append(f"RTL missing record at cycle={key[0]} tile=({key[1]},{key[2]})")
            continue
        for field in sorted(set(golden[key]) | set(rtl[key])):
            golden_value = golden[key].get(field, "")
            rtl_value = rtl[key].get(field, "")
            if golden_value != rtl_value:
                diagnostics.append(
                    "Trace mismatch "
                    f"cycle={key[0]} tile=({key[1]},{key[2]}) "
                    f"field={field}: golden={golden_value} rtl={rtl_value}"
                )
    return diagnostics
