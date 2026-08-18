#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""External program-manifest RTL harness coverage."""

from __future__ import annotations

import csv
import json
import pathlib
import subprocess
import sys

from tools.emit_config import validate_config_manifest
from tools.program_runner import compare_program_traces, prepare_program_manifest
from tools.validate_program import validate_program


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]


MANIFEST = REPO_ROOT / "examples" / "schedules" / "fir32_transposed_predicated_ii7_4x4.semantic.json"


def test_prepare_archives_manifest_config_golden_trace_and_protocol_testbench(tmp_path: pathlib.Path):
    artifacts = prepare_program_manifest(MANIFEST, tmp_path / "fir32")

    for key in ("program_manifest", "config_stream", "golden_trace", "testbench", "metadata"):
        assert artifacts[key].is_file(), key

    with artifacts["program_manifest"].open(encoding="utf-8") as handle:
        manifest = json.load(handle)
    with artifacts["config_stream"].open(encoding="utf-8") as handle:
        config = json.load(handle)
    assert validate_program(manifest) == []
    assert validate_config_manifest(config) == []
    assert config["config_stream"]["run"] == {"command": "START", "run_cycles": 242}
    control_writes = [
        write for write in config["config_stream"]["writes"]
        if write["mem_type"] == "CONTROL_MEM"
    ]
    assert len(control_writes) == 4 * 4 * 242 * 4
    omitted_pc = [
        write for write in control_writes
        if (write["cfg_tile_row"], write["cfg_tile_col"], write["cfg_addr"]) == (0, 0, 229)
    ]
    assert [write["cfg_wdata"] for write in omitted_pc] == ["0x00000000"] * 4

    testbench = artifacts["testbench"].read_text(encoding="utf-8")
    assert "module generated_program_tb" in testbench
    assert "cfg_valid = 1'b1;" in testbench
    assert "if (!cfg_ready)" in testbench
    assert "pack_tile_control_word" not in testbench
    assert "INPUT_MANIFEST" in testbench

    with artifacts["metadata"].open(encoding="utf-8") as handle:
        metadata = json.load(handle)
    assert metadata["schema"] == "cgra.program_rtl.v1"
    assert metadata["seed"] is None
    assert metadata["source_manifest"]["sha256"]


def test_manifest_run_length_cannot_wrap_the_8_bit_kernel_pc():
    with MANIFEST.open(encoding="utf-8") as handle:
        manifest = json.load(handle)
    manifest["run"]["run_cycles"] = 256

    errors = validate_program(manifest)

    assert "run.run_cycles must be in [1, 255]" in errors


def test_trace_mismatch_reports_manifest_config_seed_cycle_tile_and_field(tmp_path: pathlib.Path):
    artifacts = prepare_program_manifest(MANIFEST, tmp_path / "fir32")
    bad_rtl = tmp_path / "bad_rtl_trace.csv"
    with artifacts["golden_trace"].open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    changed = next(row for row in rows if row["fu_data_valid"] == "1")
    changed["fu_data_result"] = "deadbeef"
    with artifacts["golden_trace"].open(newline="", encoding="utf-8") as handle:
        writer_fields = csv.DictReader(handle).fieldnames
    assert writer_fields is not None
    with bad_rtl.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=writer_fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    diagnostics = compare_program_traces(
        manifest=artifacts["program_manifest"],
        config=artifacts["config_stream"],
        golden=artifacts["golden_trace"],
        rtl=bad_rtl,
    )
    assert len(diagnostics) == 1
    diagnostic = diagnostics[0]
    assert "PROGRAM_TRACE_MISMATCH" in diagnostic
    assert "manifest=" in diagnostic
    assert "config=" in diagnostic
    assert "seed=none" in diagnostic
    assert f"cycle={changed['cycle']} tile=({changed['tile_row']},{changed['tile_col']}) field=fu_data_result" in diagnostic

    result = subprocess.run(
        [
            sys.executable,
            "tools/program_runner.py",
            "--compare",
            "--manifest", str(artifacts["program_manifest"]),
            "--config", str(artifacts["config_stream"]),
            "--golden", str(artifacts["golden_trace"]),
            "--rtl", str(bad_rtl),
        ],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 1
    assert diagnostic in result.stderr
