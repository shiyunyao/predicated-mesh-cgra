#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared scratchpad model, validation, and schedule-checker coverage."""

from __future__ import annotations

import copy
import json
import pathlib

import pytest

from model.golden_model import (
    GoldenModelError,
    MultiTileGolden,
    TileControl,
    load_target,
    manifest_execution_steps,
)
from tools.check_schedule import check_manifest
from tools.emit_config import emit_config_manifest
from tools.validate_program import validate_program


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
EXAMPLE = REPO_ROOT / "examples" / "schedules" / "shared_memory_cross_lsu_4x4.json"


def load_example() -> dict:
    with EXAMPLE.open(encoding="utf-8") as handle:
        return json.load(handle)


def lsu_coords(target: dict) -> list[tuple[int, int]]:
    return sorted((entry["row"], entry["col"]) for entry in target["lsu"]["enabled_tiles"])


def test_shared_state_cross_lsu_and_four_same_address_loads():
    target = load_target("target/cgra_v2.json")
    array = MultiTileGolden(target)
    coords = lsu_coords(target)

    for coord in coords:
        array.tiles[coord].poke_data_rf(0, 100)
    array.tiles[coords[0]].poke_data_rf(1, 0xDEAD_BEEF)

    array.step({
        coords[0]: TileControl(
            data_rf_raddr_a=0,
            data_rf_raddr_b=1,
            lsu_op="STORE",
            lsu_addr_src="RF_A",
            lsu_store_data_src="RF_B",
        )
    })
    assert array.shared_scratchpad[100] == 0xDEAD_BEEF

    load = TileControl(data_rf_raddr_a=0, lsu_op="LOAD", lsu_addr_src="RF_A")
    array.step({coord: load for coord in reversed(coords)})
    array.step({})
    consume = TileControl(
        op="PASS",
        src_a="LSU_LOAD_DATA",
        data_w0_we=True,
        data_w0_addr=2,
    )
    array.step({coord: consume for coord in reversed(coords)})
    assert [array.tiles[coord].data_rf[2] for coord in coords] == [0xDEAD_BEEF] * 4


def test_load_response_and_shared_state_cross_modulo_kernel_period():
    target = load_target("target/cgra_v2.json")
    array = MultiTileGolden(target)
    coord = lsu_coords(target)[0]
    array.tiles[coord].poke_data_rf(0, 200)
    array.tiles[coord].poke_data_rf(1, 0xCAFE_BABE)

    store = TileControl(
        data_rf_raddr_a=0,
        data_rf_raddr_b=1,
        lsu_op="STORE",
        lsu_addr_src="RF_A",
        lsu_store_data_src="RF_B",
    )
    load = TileControl(
        data_rf_raddr_a=0,
        lsu_op="LOAD",
        lsu_addr_src="RF_A",
    )
    controls = {coord: [store, TileControl(), load]}
    steps = manifest_execution_steps({
        "loop": {
            "enabled": True,
            "prologue_cycles": 1,
            "ii": 2,
            "trip_count": 2,
            "epilogue_cycles": 0,
        }
    })

    records = array.run(controls, run_cycles=len(steps), execution_steps=steps)

    assert array.shared_scratchpad[200] == 0xCAFE_BABE
    response = next(
        record for record in records
        if record["cycle"] == "4"
        and record["tile_row"] == str(coord[0])
        and record["tile_col"] == str(coord[1])
    )
    assert response["loop_iteration"] == "1"
    assert response["kernel_slot"] == "1"
    assert response["lsu_load_resp_valid"] == "1"
    assert response["lsu_load_resp_data"] == "cafebabe"


@pytest.mark.parametrize("second_is_store", [False, True])
def test_same_address_access_with_any_store_is_rejected_before_commit(second_is_store: bool):
    target = load_target("target/cgra_v2.json")
    array = MultiTileGolden(target)
    first, second = lsu_coords(target)[:2]
    for coord in (first, second):
        array.tiles[coord].poke_data_rf(0, 77)
        array.tiles[coord].poke_data_rf(1, 0x100 + coord[0])

    store = TileControl(
        data_rf_raddr_a=0,
        data_rf_raddr_b=1,
        lsu_op="STORE",
        lsu_addr_src="RF_A",
        lsu_store_data_src="RF_B",
    )
    other = store if second_is_store else TileControl(
        data_rf_raddr_a=0,
        lsu_op="LOAD",
        lsu_addr_src="RF_A",
    )
    with pytest.raises(GoldenModelError, match="same-address store conflict"):
        array.step({first: store, second: other})
    assert array.shared_scratchpad[77] == 0


def test_global_preload_validation_and_emission_are_tile_independent():
    manifest = load_example()
    assert validate_program(manifest) == []
    writes = emit_config_manifest(manifest)["config_stream"]["writes"]
    scratch_writes = [write for write in writes if write["mem_type"] == "SHARED_SCRATCHPAD"]
    assert [(write["cfg_tile_row"], write["cfg_tile_col"]) for write in scratch_writes] == [(0, 0)] * 3

    duplicate = copy.deepcopy(manifest)
    duplicate["program"]["tiles"][0]["scratchpad_preload"].append(
        {"addr": 101, "value": "0x99999999"}
    )
    assert any("duplicate global scratchpad preload address 101" in error for error in validate_program(duplicate))


def test_schedule_checker_reports_static_same_address_store_conflict(tmp_path: pathlib.Path):
    manifest = load_example()
    tile_1 = next(
        tile for tile in manifest["program"]["tiles"]
        if (tile["row"], tile["col"]) == (1, 0)
    )
    tile_1["const_memory"][0]["value"] = "0x00000064"
    path = tmp_path / "conflict.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")

    diagnostics = check_manifest(path)
    assert any(diag.code == "SCHED_MEMORY_SAME_ADDR_STORE_CONFLICT" for diag in diagnostics)
