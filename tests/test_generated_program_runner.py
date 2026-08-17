#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generated-program RTL harness coverage."""

from __future__ import annotations

import csv
import json
import pathlib
import subprocess
import sys

from tools.emit_config import validate_config_manifest
from tools.generated_program_runner import compare_generated_traces, prepare_generated_program
from tools.validate_program import validate_program


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_prepare_archives_dfg_manifest_config_golden_trace_and_protocol_testbench(tmp_path: pathlib.Path):
    artifacts = prepare_generated_program(REPO_ROOT / "examples" / "dfg" / "add_chain.json", tmp_path / "add_chain")

    for key in ("input_dfg", "program_manifest", "config_stream", "golden_trace", "testbench", "metadata"):
        assert artifacts[key].is_file(), key

    with artifacts["program_manifest"].open(encoding="utf-8") as handle:
        manifest = json.load(handle)
    with artifacts["config_stream"].open(encoding="utf-8") as handle:
        config = json.load(handle)
    assert validate_program(manifest) == []
    assert validate_config_manifest(config) == []
    assert config["config_stream"]["run"] == {"command": "START", "run_cycles": 4}

    testbench = artifacts["testbench"].read_text(encoding="utf-8")
    assert "module generated_program_tb" in testbench
    assert "cfg_valid = 1'b1;" in testbench
    assert "if (!cfg_ready)" in testbench
    assert "pack_tile_control_word" not in testbench
    assert "32'h001de181" in testbench

    with artifacts["metadata"].open(encoding="utf-8") as handle:
        metadata = json.load(handle)
    assert metadata["schema"] == "cgra.generated_program_rtl.v1"
    assert metadata["seed"] is None
    assert metadata["source_dfg"]["sha256"]


def test_trace_mismatch_reports_dfg_config_seed_cycle_tile_and_field(tmp_path: pathlib.Path):
    artifacts = prepare_generated_program(REPO_ROOT / "examples" / "dfg" / "add_chain.json", tmp_path / "add_chain")
    bad_rtl = tmp_path / "bad_rtl_trace.csv"
    with artifacts["golden_trace"].open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    rows[0]["fu_data_result"] = "deadbeef"
    with artifacts["golden_trace"].open(newline="", encoding="utf-8") as handle:
        writer_fields = csv.DictReader(handle).fieldnames
    assert writer_fields is not None
    with bad_rtl.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=writer_fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    diagnostics = compare_generated_traces(
        dfg=artifacts["input_dfg"],
        config=artifacts["config_stream"],
        golden=artifacts["golden_trace"],
        rtl=bad_rtl,
    )
    assert len(diagnostics) == 1
    diagnostic = diagnostics[0]
    assert "GENERATED_PROGRAM_TRACE_MISMATCH" in diagnostic
    assert "dfg=" in diagnostic
    assert "config=" in diagnostic
    assert "seed=none" in diagnostic
    assert "cycle=0 tile=(0,0) field=fu_data_result" in diagnostic

    result = subprocess.run(
        [
            sys.executable,
            "tools/generated_program_runner.py",
            "--compare",
            "--dfg", str(artifacts["input_dfg"]),
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
