#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Statically reject illegal CGRA v1 program schedules before RTL simulation.

The checker consumes the same explicit tile-image manifests as the golden
model. It deliberately does not schedule, reroute, insert bubbles, or repair
an image: a manifest either respects the target timing contract or receives a
stable diagnostic at the offending cycle and tile.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
from dataclasses import dataclass
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from model.golden_model import (
    DIRECTIONS,
    GoldenModelError,
    RouteDirControl,
    TileControl,
    decode_control_chunks,
    execute_fu,
    load_target,
)
from tools.validate_program import validate_program


DATA_NETWORK_SOURCES = {
    "NORTH_DATA_IN", "SOUTH_DATA_IN", "EAST_DATA_IN", "WEST_DATA_IN",
}
PRED_NETWORK_SOURCES = {
    "NORTH_PRED_IN", "SOUTH_PRED_IN", "EAST_PRED_IN", "WEST_PRED_IN",
}
DATA_NEIGHBORS = {
    "north": (-1, 0, "SOUTH_DATA_IN"),
    "south": (1, 0, "NORTH_DATA_IN"),
    "east": (0, 1, "WEST_DATA_IN"),
    "west": (0, -1, "EAST_DATA_IN"),
}
PRED_NEIGHBORS = {
    "north": (-1, 0, "SOUTH_PRED_IN"),
    "south": (1, 0, "NORTH_PRED_IN"),
    "east": (0, 1, "WEST_PRED_IN"),
    "west": (0, -1, "EAST_PRED_IN"),
}


@dataclass(frozen=True)
class ScheduleDiagnostic:
    """A deterministic schedule-rule violation."""

    code: str
    cycle: int | None
    coord: tuple[int, int] | None
    detail: str

    def __str__(self) -> str:
        location = []
        if self.cycle is not None:
            location.append(f"cycle={self.cycle}")
        if self.coord is not None:
            location.append(f"tile=({self.coord[0]},{self.coord[1]})")
        prefix = " ".join(location)
        return f"{self.code} {prefix}: {self.detail}" if prefix else f"{self.code}: {self.detail}"


@dataclass
class _TileState:
    data_valid: list[bool]
    data_values: list[int | None]
    pred_valid: list[bool]
    pred_values: list[int | None]
    const_values: list[int]
    scratch_values: list[int | None]
    pending_loads: dict[int, int | None]


def _load_manifest(path: pathlib.Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _source_data_reads(ctrl: TileControl) -> set[int]:
    reads: set[int] = set()
    sources = [ctrl.src_a, ctrl.src_b]
    if ctrl.data_w1_we:
        sources.append(ctrl.data_w1_src)
    if ctrl.lsu_op != "NONE":
        sources.append(ctrl.lsu_addr_src)
        if ctrl.lsu_op == "STORE":
            sources.append(ctrl.lsu_store_data_src)
    for route in ctrl.data_routes.values():
        if route.we:
            sources.append(route.src)
    for source in sources:
        if source == "RF_A":
            reads.add(ctrl.data_rf_raddr_a)
        elif source == "RF_B":
            reads.add(ctrl.data_rf_raddr_b)
    return reads


def _source_pred_reads(ctrl: TileControl) -> set[int]:
    reads: set[int] = set()
    sources = [ctrl.src_p0, ctrl.src_p1]
    if ctrl.pred_w1_we:
        sources.append(ctrl.pred_w1_src)
    if ctrl.lsu_op == "STORE" and ctrl.lsu_commit_pred_enable:
        sources.append(ctrl.lsu_commit_pred_src)
    for route in ctrl.pred_routes.values():
        if route.we:
            sources.append(route.src)
    for source in sources:
        if source == "RF_A":
            reads.add(ctrl.pred_rf_raddr_a)
        elif source == "RF_B":
            reads.add(ctrl.pred_rf_raddr_b)
    return reads


def _decode_manifest_control(entry: dict[str, Any], manifest: dict[str, Any], target: dict[str, Any]) -> TileControl:
    """Decode one target-format control image."""

    return decode_control_chunks(entry["chunks"], target)


def _decode_controls(
    manifest: dict[str, Any], target: dict[str, Any], add: Any,
) -> dict[tuple[int, int], dict[int, TileControl]]:
    controls: dict[tuple[int, int], dict[int, TileControl]] = {}
    for tile_image in manifest["program"]["tiles"]:
        coord = (tile_image["row"], tile_image["col"])
        image_controls: dict[int, TileControl] = {}
        for entry in tile_image["control"]:
            try:
                image_controls[entry["pc"]] = _decode_manifest_control(entry, manifest, target)
            except (GoldenModelError, TypeError, ValueError) as exc:
                add("SCHED_CONTROL_DECODE", entry.get("pc"), coord, str(exc))
        controls[coord] = image_controls
    return controls


def _duplicate_link_conflicts(manifest: dict[str, Any], target: dict[str, Any]) -> list[ScheduleDiagnostic]:
    """Find multiple explicit tile images claiming one directional link.

    A structurally valid manifest permits only one image per tile, making the
    fixed mesh single-writer by construction. This pre-validation check gives
    malformed multi-driver images a schedule-specific link diagnostic.
    """

    claims: dict[tuple[int, tuple[int, int], str, str], int] = {}
    diagnostics: list[ScheduleDiagnostic] = []
    program = manifest.get("program")
    if not isinstance(program, dict) or not isinstance(program.get("tiles"), list):
        return diagnostics
    for tile_image in program["tiles"]:
        if not isinstance(tile_image, dict):
            continue
        row, col = tile_image.get("row"), tile_image.get("col")
        if not isinstance(row, int) or not isinstance(col, int):
            continue
        controls = tile_image.get("control")
        if not isinstance(controls, list):
            continue
        for entry in controls:
            if not isinstance(entry, dict) or not isinstance(entry.get("pc"), int):
                continue
            try:
                ctrl = _decode_manifest_control(entry, manifest, target)
            except (GoldenModelError, KeyError, TypeError, ValueError):
                continue
            for kind, routes in (("data", ctrl.data_routes), ("predicate", ctrl.pred_routes)):
                for direction, route in routes.items():
                    if not route.we:
                        continue
                    key = (entry["pc"], (row, col), kind, direction)
                    claims[key] = claims.get(key, 0) + 1
                    if claims[key] == 2:
                        diagnostics.append(ScheduleDiagnostic(
                            "SCHED_LINK_CONFLICT",
                            entry["pc"],
                            (row, col),
                            f"multiple explicit {kind} drivers claim {direction} output",
                        ))
    return diagnostics


def _check_cycles(manifest: dict[str, Any], target: dict[str, Any]) -> tuple[list[tuple[int, int]], int | None]:
    """Build a bounded structural proof window instead of unrolling N iterations."""

    loop = manifest.get("loop")
    if not isinstance(loop, dict) or loop.get("enabled") is not True:
        return [(cycle, cycle) for cycle in range(manifest["run"]["run_cycles"])], None

    prologue = loop["prologue_cycles"]
    ii = loop["ii"]
    trip_count = loop["trip_count"]
    epilogue = loop["epilogue_cycles"]
    max_latency = max(target["parameters"]["mesh_hop_latency"], target["parameters"]["load_latency"])
    proof_periods = min(trip_count, max(3, 2 + math.ceil(max_latency / ii)))
    cycles: list[tuple[int, int]] = []
    cycle = 0
    for pc in range(prologue):
        cycles.append((cycle, pc))
        cycle += 1
    for _ in range(proof_periods):
        for slot in range(ii):
            cycles.append((cycle, prologue + slot))
            cycle += 1
    for offset in range(epilogue):
        cycles.append((cycle, prologue + ii + offset))
        cycle += 1
    return cycles, proof_periods


def check_manifest(manifest_path: str | pathlib.Path) -> list[ScheduleDiagnostic]:
    """Return every static legality violation in one program manifest."""

    path = pathlib.Path(manifest_path)
    try:
        manifest = _load_manifest(path)
    except (OSError, json.JSONDecodeError) as exc:
        return [ScheduleDiagnostic("SCHED_MANIFEST_INVALID", None, None, str(exc))]

    target: dict[str, Any] | None = None
    target_path = manifest.get("target", {}).get("path") if isinstance(manifest, dict) else None
    if isinstance(target_path, str):
        try:
            target = load_target(target_path)
        except (OSError, json.JSONDecodeError, KeyError) as exc:
            return [ScheduleDiagnostic("SCHED_MANIFEST_INVALID", None, None, f"cannot load target: {exc}")]

    validation_errors = validate_program(manifest)
    if validation_errors:
        link_diagnostics = _duplicate_link_conflicts(manifest, target) if target is not None else []
        return link_diagnostics + [ScheduleDiagnostic("SCHED_MANIFEST_INVALID", None, None, error) for error in validation_errors]
    assert target is not None

    diagnostics: list[ScheduleDiagnostic] = []
    seen: set[tuple[str, int | None, tuple[int, int] | None, str]] = set()

    def add(code: str, cycle: int | None, coord: tuple[int, int] | None, detail: str) -> None:
        key = (code, cycle, coord, detail)
        if key not in seen:
            seen.add(key)
            diagnostics.append(ScheduleDiagnostic(code, cycle, coord, detail))

    controls_by_tile = _decode_controls(manifest, target, add)
    params = target["parameters"]
    rows = params["array_rows"]
    cols = params["array_cols"]
    load_latency = params["load_latency"]
    enabled_lsus = {(entry["row"], entry["col"]) for entry in target["lsu"]["enabled_tiles"]}

    states: dict[tuple[int, int], _TileState] = {}
    for row in range(rows):
        for col in range(cols):
            states[(row, col)] = _TileState(
                data_valid=[False] * params["data_rf_depth"],
                data_values=[None] * params["data_rf_depth"],
                pred_valid=[False] * params["pred_rf_depth"],
                pred_values=[None] * params["pred_rf_depth"],
                const_values=[0] * params["const_mem_depth"],
                scratch_values=[0] * params["scratch_bank_depth"],
                pending_loads={},
            )

    for tile_image in manifest["program"]["tiles"]:
        state = states[(tile_image["row"], tile_image["col"])]
        for entry in tile_image["const_memory"]:
            state.const_values[entry["addr"]] = int(entry["value"], 16)
        for entry in tile_image["scratchpad_preload"]:
            state.scratch_values[entry["addr"]] = int(entry["value"], 16)

    data_inputs: dict[tuple[int, int], dict[str, int | None]] = {coord: {} for coord in states}
    pred_inputs: dict[tuple[int, int], dict[str, int | None]] = {coord: {} for coord in states}

    def emit_link(
        next_inputs: dict[tuple[int, int], dict[str, int | None]],
        neighbor_map: dict[str, tuple[int, int, str]],
        coord: tuple[int, int],
        direction: str,
        value: int | None,
        cycle: int,
        kind: str,
    ) -> None:
        row, col = coord
        row_delta, col_delta, destination_source = neighbor_map[direction]
        destination = (row + row_delta, col + col_delta)
        if not (0 <= destination[0] < rows and 0 <= destination[1] < cols):
            return
        if destination_source in next_inputs[destination]:
            add(
                "SCHED_LINK_CONFLICT",
                cycle,
                coord,
                f"multiple {kind} drivers arrive at tile=({destination[0]},{destination[1]}) {destination_source}",
            )
            return
        next_inputs[destination][destination_source] = value

    execution_cycles, proof_periods = _check_cycles(manifest, target)
    loop = manifest.get("loop")
    loop_enabled = isinstance(loop, dict) and loop.get("enabled") is True
    kernel_period_snapshots: list[tuple[Any, ...]] = []

    def structural_snapshot() -> tuple[Any, ...]:
        state_snapshot = []
        for coord in sorted(states):
            state = states[coord]
            state_snapshot.append((
                coord,
                tuple(state.data_valid),
                tuple(state.pred_valid),
                tuple(sorted(state.pending_loads)),
            ))
        input_snapshot = tuple(
            (coord, tuple(sorted(data_inputs[coord])), tuple(sorted(pred_inputs[coord])))
            for coord in sorted(states)
        )
        return tuple(state_snapshot) + input_snapshot

    for cycle, control_pc in execution_cycles:
        load_responses: dict[tuple[int, int], tuple[bool, int | None]] = {}
        for coord, state in states.items():
            if cycle in state.pending_loads:
                load_responses[coord] = (True, state.pending_loads.pop(cycle))
            else:
                load_responses[coord] = (False, None)

        next_data_inputs: dict[tuple[int, int], dict[str, int | None]] = {coord: {} for coord in states}
        next_pred_inputs: dict[tuple[int, int], dict[str, int | None]] = {coord: {} for coord in states}
        pending_effects: list[tuple[
            tuple[int, int], list[tuple[int, int | None]], list[tuple[int, int | None]],
            list[tuple[str, int | None]], list[tuple[str, int | None]],
            tuple[int, int | None] | None, tuple[int, int | None] | None,
        ]] = []

        for coord, state in states.items():
            ctrl = controls_by_tile.get(coord, {}).get(control_pc, TileControl())

            data_write_addrs = ({ctrl.data_w0_addr} if ctrl.data_w0_we else set()) | ({ctrl.data_w1_addr} if ctrl.data_w1_we else set())
            pred_write_addrs = ({ctrl.pred_w0_addr} if ctrl.pred_w0_we else set()) | ({ctrl.pred_w1_addr} if ctrl.pred_w1_we else set())
            if ctrl.data_w0_we and ctrl.data_w1_we and ctrl.data_w0_addr == ctrl.data_w1_addr:
                add("SCHED_DATA_RF_WRITE_COLLISION", cycle, coord, f"W0 and W1 both write r{ctrl.data_w0_addr}")
            if ctrl.pred_w0_we and ctrl.pred_w1_we and ctrl.pred_w0_addr == ctrl.pred_w1_addr:
                add("SCHED_PRED_RF_WRITE_COLLISION", cycle, coord, f"PW0 and PW1 both write p{ctrl.pred_w0_addr}")

            data_reads = _source_data_reads(ctrl)
            pred_reads = _source_pred_reads(ctrl)
            for addr in sorted(data_reads & data_write_addrs):
                add("SCHED_DATA_RF_READ_WRITE_HAZARD", cycle, coord, f"read/write overlap on r{addr}")
            for addr in sorted(pred_reads & pred_write_addrs):
                add("SCHED_PRED_RF_READ_WRITE_HAZARD", cycle, coord, f"read/write overlap on p{addr}")
            for addr in sorted(data_reads):
                if not state.data_valid[addr]:
                    add("SCHED_DATA_RF_UNINITIALIZED_READ", cycle, coord, f"read r{addr} before initialization")
            for addr in sorted(pred_reads):
                if not state.pred_valid[addr]:
                    add("SCHED_PRED_RF_UNINITIALIZED_READ", cycle, coord, f"read p{addr} before initialization")

            def data_source(source: str, field: str) -> int | None:
                if source == "RF_A":
                    return state.data_values[ctrl.data_rf_raddr_a]
                if source == "RF_B":
                    return state.data_values[ctrl.data_rf_raddr_b]
                if source == "CONST_DATA":
                    return state.const_values[ctrl.const_addr]
                if source == "ZERO":
                    return 0
                if source == "LSU_LOAD_DATA":
                    valid, value = load_responses[coord]
                    if not valid:
                        add("SCHED_LSU_LOAD_RESPONSE_TIMING", cycle, coord, f"{field} selects LSU_LOAD_DATA without a response")
                    return value if valid else None
                if source in DATA_NETWORK_SOURCES:
                    if source not in data_inputs[coord]:
                        add("SCHED_NETWORK_ARRIVAL", cycle, coord, f"{field} selects {source} without a one-hop arrival")
                        return None
                    return data_inputs[coord][source]
                add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, f"{field} selects unsupported data source {source}")
                return None

            def pred_source(source: str, field: str) -> int | None:
                if source == "RF_A":
                    return state.pred_values[ctrl.pred_rf_raddr_a]
                if source == "RF_B":
                    return state.pred_values[ctrl.pred_rf_raddr_b]
                if source == "CONST_TRUE":
                    return 1
                if source == "CONST_FALSE":
                    return 0
                if source in PRED_NETWORK_SOURCES:
                    if source not in pred_inputs[coord]:
                        add("SCHED_NETWORK_ARRIVAL", cycle, coord, f"{field} selects {source} without a one-hop arrival")
                        return None
                    return pred_inputs[coord][source]
                add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, f"{field} selects unsupported predicate source {source}")
                return None

            a = data_source(ctrl.src_a, "src_a")
            b = data_source(ctrl.src_b, "src_b")
            p0 = pred_source(ctrl.src_p0, "src_p0")
            p1 = pred_source(ctrl.src_p1, "src_p1")
            fu_data_valid, fu_data_value, fu_pred_valid, fu_pred_value = execute_fu(ctrl.op, a or 0, b or 0, p0 or 0, p1 or 0)

            data_writes: list[tuple[int, int | None]] = []
            pred_writes: list[tuple[int, int | None]] = []
            if ctrl.data_w0_we:
                if not fu_data_valid:
                    add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, f"DataRF W0 is enabled for {ctrl.op} without a data result")
                else:
                    data_writes.append((ctrl.data_w0_addr, fu_data_value))
            if ctrl.data_w1_we:
                data_writes.append((ctrl.data_w1_addr, data_source(ctrl.data_w1_src, "data_w1_src")))
            if ctrl.pred_w0_we:
                if not fu_pred_valid:
                    add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, f"PredicateRF PW0 is enabled for {ctrl.op} without a predicate result")
                else:
                    pred_writes.append((ctrl.pred_w0_addr, fu_pred_value))
            if ctrl.pred_w1_we:
                pred_writes.append((ctrl.pred_w1_addr, pred_source(ctrl.pred_w1_src, "pred_w1_src")))

            data_routes: list[tuple[str, int | None]] = []
            for direction in DIRECTIONS:
                route: RouteDirControl = ctrl.data_routes[direction]
                if not route.we:
                    continue
                if route.src == "NONE":
                    add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, f"data route {direction} enables source NONE")
                elif route.src == "FU_DATA_RESULT":
                    if not fu_data_valid:
                        add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, f"data route {direction} selects FU_DATA_RESULT for {ctrl.op}")
                    else:
                        data_routes.append((direction, fu_data_value))
                else:
                    data_routes.append((direction, data_source(route.src, f"data_route_{direction}")))

            pred_routes: list[tuple[str, int | None]] = []
            for direction in DIRECTIONS:
                route = ctrl.pred_routes[direction]
                if not route.we:
                    continue
                if route.src == "NONE":
                    add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, f"predicate route {direction} enables source NONE")
                elif route.src == "FU_PRED_RESULT":
                    if not fu_pred_valid:
                        add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, f"predicate route {direction} selects FU_PRED_RESULT for {ctrl.op}")
                    else:
                        pred_routes.append((direction, fu_pred_value))
                else:
                    pred_routes.append((direction, pred_source(route.src, f"pred_route_{direction}")))

            load_issue: tuple[int, int | None] | None = None
            scratch_write: tuple[int, int | None] | None = None
            if ctrl.lsu_op != "NONE":
                if coord not in enabled_lsus:
                    add("SCHED_NON_LSU_OPERATION", cycle, coord, f"{ctrl.lsu_op} issued on a tile without an LSU")
                elif ctrl.lsu_op == "RESERVED":
                    add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, "reserved LSU operation")
                elif ctrl.lsu_op == "LOAD" and ctrl.lsu_commit_pred_enable:
                    add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, "LOAD enables predicate commit")
                else:
                    lsu_addr = data_source(ctrl.lsu_addr_src, "lsu_addr_src")
                    valid_addr = lsu_addr is not None and 0 <= lsu_addr < len(state.scratch_values)
                    if lsu_addr is not None and not valid_addr:
                        add("SCHED_SCRATCHPAD_ADDRESS_RANGE", cycle, coord, f"address {lsu_addr} is outside [0, {len(state.scratch_values) - 1}]")
                    if ctrl.lsu_op == "LOAD" and valid_addr:
                        load_issue = (cycle + load_latency, state.scratch_values[lsu_addr])
                    elif ctrl.lsu_op == "STORE" and valid_addr:
                        store_data = data_source(ctrl.lsu_store_data_src, "lsu_store_data_src")
                        commit = True
                        if ctrl.lsu_commit_pred_enable:
                            predicate = pred_source(ctrl.lsu_commit_pred_src, "lsu_commit_pred_src")
                            commit = predicate is not None and bool(predicate ^ int(ctrl.lsu_commit_pred_invert))
                        if commit:
                            scratch_write = (lsu_addr, store_data)
                    elif ctrl.lsu_op not in {"LOAD", "STORE"}:
                        add("SCHED_UNSUPPORTED_CONTROL", cycle, coord, f"unsupported LSU operation {ctrl.lsu_op}")

            pending_effects.append((coord, data_writes, pred_writes, data_routes, pred_routes, load_issue, scratch_write))

        for coord, data_writes, pred_writes, data_routes, pred_routes, load_issue, scratch_write in pending_effects:
            state = states[coord]
            for addr, value in data_writes:
                state.data_valid[addr] = True
                state.data_values[addr] = value
            for addr, value in pred_writes:
                state.pred_valid[addr] = True
                state.pred_values[addr] = value
            if scratch_write is not None:
                addr, value = scratch_write
                state.scratch_values[addr] = value
            if load_issue is not None:
                ready_cycle, value = load_issue
                if ready_cycle in state.pending_loads:
                    add("SCHED_LSU_LOAD_RESPONSE_TIMING", cycle, coord, f"multiple responses scheduled for cycle {ready_cycle}")
                else:
                    state.pending_loads[ready_cycle] = value
            for direction, value in data_routes:
                emit_link(next_data_inputs, DATA_NEIGHBORS, coord, direction, value, cycle, "data")
            for direction, value in pred_routes:
                emit_link(next_pred_inputs, PRED_NEIGHBORS, coord, direction, value, cycle, "predicate")

        data_inputs = next_data_inputs
        pred_inputs = next_pred_inputs

        if loop_enabled:
            prologue = loop["prologue_cycles"]
            ii = loop["ii"]
            kernel_proof_end = prologue + (proof_periods or 0) * ii
            if prologue <= cycle < kernel_proof_end and (cycle - prologue + 1) % ii == 0:
                kernel_period_snapshots.append(structural_snapshot())

    if loop_enabled and loop["trip_count"] > len(kernel_period_snapshots):
        if len(kernel_period_snapshots) < 2 or kernel_period_snapshots[-1] != kernel_period_snapshots[-2]:
            add(
                "SCHED_LOOP_PERIODIC_UNPROVEN",
                None,
                None,
                "bounded kernel proof did not reach a structural fixed point; schedule may contain a non-periodic hazard",
            )

    return diagnostics


def run_negative_suite(directory: str | pathlib.Path) -> int:
    suite_dir = pathlib.Path(directory)
    paths = sorted(suite_dir.glob("*.json"))
    if not paths:
        print(f"no negative manifests found in {suite_dir}", file=sys.stderr)
        return 1
    failures = 0
    for path in paths:
        try:
            expected = _load_manifest(path).get("schedule_checker_expected_diagnostic")
        except (OSError, json.JSONDecodeError) as exc:
            print(f"{path}: cannot read expected diagnostic: {exc}", file=sys.stderr)
            failures += 1
            continue
        diagnostics = check_manifest(path)
        codes = {diagnostic.code for diagnostic in diagnostics}
        if isinstance(expected, str) and expected in codes:
            print(f"negative case rejected: {path.name}: {expected}")
        elif not diagnostics:
            print(f"negative case unexpectedly passed: {path.name}", file=sys.stderr)
            failures += 1
        else:
            found = ", ".join(sorted(codes))
            print(f"negative case wrong diagnostic: {path.name}: expected {expected!r}, got {found}", file=sys.stderr)
            failures += 1
    if failures:
        return 1
    print(f"schedule negative suite passed: {len(paths)} cases rejected")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", nargs="?", help="Program/run manifest JSON path")
    parser.add_argument("--negative-suite", metavar="DIR", help="Reject every expected-fail manifest in DIR")
    args = parser.parse_args()
    if args.negative_suite:
        if args.manifest:
            parser.error("manifest cannot be combined with --negative-suite")
        return run_negative_suite(args.negative_suite)
    if not args.manifest:
        parser.error("manifest path is required unless --negative-suite is used")
    diagnostics = check_manifest(args.manifest)
    if diagnostics:
        for diagnostic in diagnostics:
            print(diagnostic, file=sys.stderr)
        return 1
    print(f"SCHEDULE_LEGAL: {args.manifest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
