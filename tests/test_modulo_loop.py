#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Finite global modulo-loop coverage for external program manifests."""

from __future__ import annotations

import copy
import json
import pathlib

import pytest

from model.golden_model import (
    RouteDirControl,
    TileControl,
    encode_control_chunks,
    load_target,
    manifest_execution_steps,
    run_manifest,
)
from tools.check_schedule import _check_cycles, check_manifest
from tools.emit_config import emit_config_manifest, validate_config_manifest
from tools.modulo_loop_runner import prepare_modulo_loop
from tools.validate_program import validate_program


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
EXAMPLE = REPO_ROOT / "examples" / "schedules" / "modulo_mesh_feedback.json"


def load_example() -> dict:
    with EXAMPLE.open(encoding="utf-8") as handle:
        return json.load(handle)


def write_manifest(tmp_path: pathlib.Path, manifest: dict, name: str = "loop.json") -> pathlib.Path:
    path = tmp_path / name
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def with_trip_count(manifest: dict, trip_count: int) -> dict:
    result = copy.deepcopy(manifest)
    result["loop"]["trip_count"] = trip_count
    result["run"]["run_cycles"] = (
        result["loop"]["prologue_cycles"]
        + trip_count * result["loop"]["ii"]
        + result["loop"]["epilogue_cycles"]
    )
    return result


def test_manifest_config_and_periodic_checker_are_consistent():
    manifest = load_example()
    assert validate_program(manifest) == []
    assert check_manifest(EXAMPLE) == []

    config = emit_config_manifest(manifest)
    assert validate_config_manifest(config) == []
    descriptor = [
        write for write in config["config_stream"]["writes"]
        if write["mem_type"] == "LOOP_DESC"
    ]
    assert [(write["cfg_addr"], write["cfg_wdata"]) for write in descriptor] == [
        (0, "0x00000002"),
        (1, "0x00000002"),
        (2, "0x00000004"),
        (3, "0x00000001"),
        (4, "0x00000001"),
    ]
    assert config["config_stream"]["run"] == {
        "command": "START",
        "run_cycles": 11,
        "mode": "LOOP",
    }
    control_writes = [
        write for write in config["config_stream"]["writes"]
        if write["mem_type"] == "CONTROL_MEM"
    ]
    assert len(control_writes) == 4 * 4 * 5 * 4

    huge = with_trip_count(manifest, 0xffff_ffff)
    target = load_target("target/cgra_v2.json")
    proof_cycles, proof_periods = _check_cycles(huge, target)
    assert proof_periods is not None and proof_periods < 0xffff_ffff
    assert len(proof_cycles) == 2 + proof_periods * 2 + 1
    assert validate_program(huge) == []


@pytest.mark.parametrize("trip_count", [1, 2, 4, 7])
def test_golden_expands_small_trip_counts_and_drains_recurrence(
    tmp_path: pathlib.Path,
    trip_count: int,
):
    manifest = with_trip_count(load_example(), trip_count)
    path = write_manifest(tmp_path, manifest, f"loop_n{trip_count}.json")
    assert validate_program(manifest) == []
    assert check_manifest(path) == []

    steps = manifest_execution_steps(manifest)
    assert len(steps) == 2 + trip_count * 2 + 1
    assert [step.pc for step in steps] == [0, 1] + [2, 3] * trip_count + [4]
    rows = run_manifest(path)
    assert len(rows) == len(steps) * 16
    final_tile = next(
        row for row in rows
        if row["cycle"] == str(len(steps) - 1)
        and row["tile_row"] == "0"
        and row["tile_col"] == "0"
    )
    assert final_tile["loop_phase"] == "3"
    assert final_tile["loop_iteration"] == str(trip_count)
    assert final_tile["data_out_n_valid"] == "1"
    assert final_tile["data_out_n_value"] == f"{trip_count:08x}"


def test_zero_length_prologue_and_epilogue_are_legal(tmp_path: pathlib.Path):
    manifest = with_trip_count(load_example(), 1)
    manifest["loop"]["prologue_cycles"] = 0
    manifest["loop"]["epilogue_cycles"] = 0
    manifest["run"]["run_cycles"] = 2
    for tile in manifest["program"]["tiles"]:
        tile["control"] = [entry for entry in tile["control"] if entry["pc"] < 2]
    path = write_manifest(tmp_path, manifest)
    assert validate_program(manifest) == []
    assert check_manifest(path) == []
    assert len(run_manifest(path)) == 2 * 16


@pytest.mark.parametrize(
    ("mutate", "message"),
    [
        (lambda manifest: manifest["loop"].update(ii=0), "loop.ii must be positive"),
        (lambda manifest: manifest["loop"].update(trip_count=0), "loop.trip_count must be positive"),
        (lambda manifest: manifest["loop"].update(prologue_cycles=255, ii=2), "exceeds control memory depth"),
        (lambda manifest: manifest["program"]["tiles"][0]["control"].pop(), "missing loop phase PCs"),
    ],
)
def test_invalid_loop_manifests_fail_closed(mutate, message):
    manifest = load_example()
    mutate(manifest)
    assert any(message in error for error in validate_program(manifest))


def test_config_validator_rejects_malformed_descriptor():
    config = emit_config_manifest(load_example())
    descriptor = next(
        write for write in config["config_stream"]["writes"]
        if write["mem_type"] == "LOOP_DESC"
    )
    descriptor["cfg_tile_col"] = 1
    assert any("writes do not match" in error for error in validate_config_manifest(config))


def test_duplicate_phase_pc_and_dynamic_exit_are_rejected():
    duplicate = load_example()
    duplicate["program"]["tiles"][0]["control"].append(
        copy.deepcopy(duplicate["program"]["tiles"][0]["control"][2])
    )
    assert any("duplicate control pc" in error for error in validate_program(duplicate))

    dynamic = load_example()
    dynamic["loop"]["exit"] = {"source": "predicate"}
    assert any("unsupported keys" in error for error in validate_program(dynamic))


def test_same_cycle_same_address_recurrence_is_rejected(tmp_path: pathlib.Path):
    manifest = load_example()
    target = load_target("target/cgra_v2.json")
    hazard = TileControl(
        op="ADD",
        src_a="RF_A",
        src_b="CONST_DATA",
        data_rf_raddr_a=0,
        data_w0_we=True,
        data_w0_addr=0,
        const_addr=1,
        data_routes={"east": RouteDirControl(True, "FU_DATA_RESULT")},
    )
    manifest["program"]["tiles"][0]["control"][2]["chunks"] = encode_control_chunks(hazard, target)
    path = write_manifest(tmp_path, manifest)
    codes = {diagnostic.code for diagnostic in check_manifest(path)}
    assert "SCHED_DATA_RF_READ_WRITE_HAZARD" in codes


def test_runner_materializes_external_manifest_artifacts(tmp_path: pathlib.Path):
    artifacts = prepare_modulo_loop(EXAMPLE, tmp_path / "artifacts")
    assert all(path.is_file() for path in artifacts.values())
    testbench = artifacts["testbench"].read_text(encoding="utf-8")
    assert "active_trip_count * LOOP_II" in testbench
    assert "(index - active_prologue) % LOOP_II" in testbench
    assert "INVALID_LOOP_II" in testbench
    assert "LOOP_DESC_DURING_DONE" in testbench
    assert "INPUT_MANIFEST" in testbench

    zero = prepare_modulo_loop(EXAMPLE, tmp_path / "zero", trip_count=1, zero_boundaries=True)
    zero_manifest = json.loads(zero["input_program"].read_text(encoding="utf-8"))
    assert zero_manifest["loop"]["prologue_cycles"] == 0
    assert zero_manifest["loop"]["epilogue_cycles"] == 0
    assert zero_manifest["run"]["run_cycles"] == 2
