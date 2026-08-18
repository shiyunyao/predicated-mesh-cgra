#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared CSV trace comparison for golden-model and RTL artifacts."""

from __future__ import annotations

import csv
import pathlib


KEY_FIELDS = ("cycle", "tile_row", "tile_col")

PAYLOAD_GATES = {
    "src_a_value": "src_a_valid",
    "src_b_value": "src_b_valid",
    "src_p0_value": "src_p0_valid",
    "src_p1_value": "src_p1_valid",
    "fu_data_result": "fu_data_valid",
    "fu_pred_result": "fu_pred_valid",
    "data_w0_data": "data_w0_we",
    "data_w1_data": "data_w1_we",
    "pred_w0_data": "pred_w0_we",
    "pred_w1_data": "pred_w1_we",
    "data_out_n_value": "data_out_n_valid",
    "data_out_s_value": "data_out_s_valid",
    "data_out_e_value": "data_out_e_valid",
    "data_out_w_value": "data_out_w_valid",
    "pred_out_n_value": "pred_out_n_valid",
    "pred_out_s_value": "pred_out_s_valid",
    "pred_out_e_value": "pred_out_e_valid",
    "pred_out_w_value": "pred_out_w_valid",
    "lsu_load_resp_data": "lsu_load_resp_valid",
}


def _comparison_row(row: dict[str, str]) -> dict[str, str]:
    """Blank payloads whose corresponding valid/enable signal is low.

    The RTL trace records some combinational payloads even when their write or
    valid signal is low. Those values are not architectural observations and
    may legitimately differ from the golden model's zero placeholder.
    """

    normalized = dict(row)
    for payload, gate in PAYLOAD_GATES.items():
        if normalized.get(gate) in {"0", "false", "False"}:
            normalized[payload] = ""

    lsu_op = normalized.get("lsu_op")
    if lsu_op != "2":
        normalized["lsu_store_data"] = ""
        normalized["lsu_store_commit"] = ""
    if lsu_op != "1":
        normalized["lsu_addr"] = "" if lsu_op == "0" else normalized.get("lsu_addr", "")
    return normalized


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
            records[key] = _comparison_row(row)
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
