#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Lower the deliberately small ``cgra.dfg.v1`` format to a legal manifest.

This is a deterministic prototype, not a general compiler. It accepts a
topologically ordered constant-only DFG and emits either a one-tile program or
a fixed 1x2 data-route program.  The output remains an ordinary
``cgra.program_manifest.v1`` artifact consumed by the existing validator,
legality checker, and golden model.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass, replace
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from model.golden_model import RouteDirControl, TileControl, encode_control_chunks, execute_fu, load_target, run_manifest
from tools.check_schedule import check_manifest
from tools.validate_program import validate_program


HEX32_RE = re.compile(r"^0x[0-9a-fA-F]{8}$")
DATA_OPS = {
    "pass": ("PASS", 1),
    "add": ("ADD", 2),
    "sub": ("SUB", 2),
    "mul": ("MUL", 2),
    "and": ("AND", 2),
    "or": ("OR", 2),
    "xor": ("XOR", 2),
    "shl": ("SHL", 2),
    "lshr": ("LSHR", 2),
}
COMPARE_OPS = {"cmp_eq": "CMP_EQ", "cmp_ne": "CMP_NE", "cmp_ult": "CMP_ULT", "cmp_ule": "CMP_ULE"}
PRED_OPS = {"ppass": ("PPASS", 1), "pnot": ("PNOT", 1), "pand": ("PAND", 2), "por": ("POR", 2)}
CONST_OPS = {"const", "const_pred"}


class DFGError(ValueError):
    """Raised when a DFG lies outside the prototype compiler contract."""


@dataclass(frozen=True)
class Node:
    node_id: str
    op: str
    inputs: tuple[str, ...]
    value: int | None
    kind: str


@dataclass(frozen=True)
class DFGProgram:
    name: str
    topology: str
    nodes: tuple[Node, ...]
    output: str


def _read_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise DFGError("DFG must be a JSON object")
    return value


def _parse_hex32(value: Any, field: str) -> int:
    if not isinstance(value, str) or not HEX32_RE.fullmatch(value):
        raise DFGError(f"{field} must be an 8-digit 32-bit hex string")
    return int(value, 16)


def _node_kind(op: str) -> str:
    if op == "const" or op in DATA_OPS or op == "select":
        return "data"
    if op == "const_pred" or op in COMPARE_OPS or op in PRED_OPS:
        return "predicate"
    raise DFGError(f"unsupported node op {op!r}")


def parse_dfg(payload: dict[str, Any]) -> DFGProgram:
    if payload.get("schema") != "cgra.dfg.v1":
        raise DFGError("schema must be cgra.dfg.v1")
    if payload.get("version") != 1:
        raise DFGError("version must be 1")
    name = payload.get("name")
    if not isinstance(name, str) or not name:
        raise DFGError("name must be a non-empty string")
    topology = payload.get("topology")
    if topology not in {"single_tile", "1x2"}:
        raise DFGError("topology must be single_tile or 1x2")
    raw_nodes = payload.get("nodes")
    if not isinstance(raw_nodes, list) or not raw_nodes:
        raise DFGError("nodes must be a non-empty list")

    known: dict[str, Node] = {}
    nodes: list[Node] = []
    for index, raw in enumerate(raw_nodes):
        if not isinstance(raw, dict):
            raise DFGError(f"nodes[{index}] must be an object")
        node_id = raw.get("id")
        op = raw.get("op")
        if not isinstance(node_id, str) or not node_id:
            raise DFGError(f"nodes[{index}].id must be a non-empty string")
        if node_id in known:
            raise DFGError(f"duplicate node id {node_id!r}")
        if not isinstance(op, str):
            raise DFGError(f"nodes[{index}].op must be a string")
        op = op.lower()
        kind = _node_kind(op)
        raw_inputs = raw.get("inputs", [])
        if not isinstance(raw_inputs, list) or not all(isinstance(item, str) for item in raw_inputs):
            raise DFGError(f"nodes[{index}].inputs must be a list of node ids")
        inputs = tuple(raw_inputs)
        if any(item not in known for item in inputs):
            raise DFGError(f"nodes[{index}] inputs must reference earlier nodes")

        expected_inputs: int | None = None
        value: int | None = None
        if op == "const":
            expected_inputs = 0
            value = _parse_hex32(raw.get("value"), f"nodes[{index}].value")
        elif op == "const_pred":
            expected_inputs = 0
            if not isinstance(raw.get("value"), bool):
                raise DFGError(f"nodes[{index}].value must be a boolean for const_pred")
            value = int(raw["value"])
        elif op in DATA_OPS:
            expected_inputs = DATA_OPS[op][1]
            if any(known[item].kind != "data" for item in inputs):
                raise DFGError(f"nodes[{index}] data op inputs must be data values")
        elif op in COMPARE_OPS:
            expected_inputs = 2
            if any(known[item].kind != "data" for item in inputs):
                raise DFGError(f"nodes[{index}] compare inputs must be data values")
        elif op in PRED_OPS:
            expected_inputs = PRED_OPS[op][1]
            if any(known[item].kind != "predicate" for item in inputs):
                raise DFGError(f"nodes[{index}] predicate op inputs must be predicates")
        elif op == "select":
            expected_inputs = 3
            if len(inputs) == 3 and (known[inputs[0]].kind != "data" or known[inputs[1]].kind != "data" or known[inputs[2]].kind != "predicate"):
                raise DFGError(f"nodes[{index}] select inputs must be data, data, predicate")
        if len(inputs) != expected_inputs:
            raise DFGError(f"nodes[{index}] op {op} expects {expected_inputs} inputs, got {len(inputs)}")
        node = Node(node_id=node_id, op=op, inputs=inputs, value=value, kind=kind)
        known[node_id] = node
        nodes.append(node)

    outputs = payload.get("outputs")
    if not isinstance(outputs, list) or len(outputs) != 1 or not isinstance(outputs[0], str):
        raise DFGError("outputs must contain exactly one node id")
    output = outputs[0]
    if output not in known:
        raise DFGError("output must reference a declared node")
    if known[output].kind != "data":
        raise DFGError("the prototype output must be a data node")
    if output != nodes[-1].node_id:
        raise DFGError("the sole output must be the final topologically ordered node")
    return DFGProgram(name=name, topology=topology, nodes=tuple(nodes), output=output)


def _node_values(program: DFGProgram) -> dict[str, int]:
    values: dict[str, int] = {}
    for node in program.nodes:
        if node.op in CONST_OPS:
            assert node.value is not None
            values[node.node_id] = node.value
            continue
        inputs = [values[item] for item in node.inputs]
        if node.op in DATA_OPS:
            _, data, _, _ = execute_fu(DATA_OPS[node.op][0], inputs[0], inputs[1] if len(inputs) == 2 else 0)
            values[node.node_id] = data
        elif node.op in COMPARE_OPS:
            _, _, _, predicate = execute_fu(COMPARE_OPS[node.op], inputs[0], inputs[1])
            values[node.node_id] = predicate
        elif node.op in PRED_OPS:
            _, _, _, predicate = execute_fu(PRED_OPS[node.op][0], 0, 0, inputs[0], inputs[1] if len(inputs) == 2 else 0)
            values[node.node_id] = predicate
        else:
            _, data, _, _ = execute_fu("SELECT", inputs[0], inputs[1], inputs[2])
            values[node.node_id] = data
    return values


def _placements(program: DFGProgram) -> tuple[dict[str, tuple[int, int]], Node | None]:
    compute_nodes = [node for node in program.nodes if node.op not in CONST_OPS]
    if not compute_nodes:
        raise DFGError("DFG must contain at least one computational node")
    if program.topology == "single_tile":
        return {node.node_id: (0, 0) for node in compute_nodes}, None
    if len(compute_nodes) < 2:
        raise DFGError("1x2 topology requires at least two computational nodes")
    first = compute_nodes[0]
    if first.kind != "data":
        raise DFGError("the first computational node of a 1x2 program must produce data")
    if any(node.op not in CONST_OPS for node in (next(item for item in program.nodes if item.node_id == source) for source in first.inputs)):
        raise DFGError("the first 1x2 computational node may depend only on constants")
    placement = {first.node_id: (0, 0)}
    placement.update({node.node_id: (0, 1) for node in compute_nodes[1:]})
    return placement, first


def _compute_control(node: Node, coord: tuple[int, int], data_locations: dict[tuple[tuple[int, int], str], int], pred_locations: dict[tuple[tuple[int, int], str], int]) -> TileControl:
    def data_addr(node_id: str) -> int:
        return data_locations[(coord, node_id)]

    def pred_addr(node_id: str) -> int:
        return pred_locations[(coord, node_id)]

    if node.op in DATA_OPS:
        op, arity = DATA_OPS[node.op]
        return TileControl(
            op=op,
            src_a="RF_A",
            src_b="RF_B" if arity == 2 else "ZERO",
            data_rf_raddr_a=data_addr(node.inputs[0]),
            data_rf_raddr_b=data_addr(node.inputs[1]) if arity == 2 else 0,
            data_w0_we=True,
            data_w0_addr=data_addr(node.node_id),
        )
    if node.op in COMPARE_OPS:
        return TileControl(
            op=COMPARE_OPS[node.op],
            src_a="RF_A",
            src_b="RF_B",
            data_rf_raddr_a=data_addr(node.inputs[0]),
            data_rf_raddr_b=data_addr(node.inputs[1]),
            pred_w0_we=True,
            pred_w0_addr=pred_addr(node.node_id),
        )
    if node.op in PRED_OPS:
        op, arity = PRED_OPS[node.op]
        return TileControl(
            op=op,
            src_p0="RF_A",
            src_p1="RF_B" if arity == 2 else "CONST_FALSE",
            pred_rf_raddr_a=pred_addr(node.inputs[0]),
            pred_rf_raddr_b=pred_addr(node.inputs[1]) if arity == 2 else 0,
            pred_w0_we=True,
            pred_w0_addr=pred_addr(node.node_id),
        )
    if node.op == "select":
        return TileControl(
            op="SELECT",
            src_a="RF_A",
            src_b="RF_B",
            src_p0="RF_A",
            data_rf_raddr_a=data_addr(node.inputs[0]),
            data_rf_raddr_b=data_addr(node.inputs[1]),
            pred_rf_raddr_a=pred_addr(node.inputs[2]),
            data_w0_we=True,
            data_w0_addr=data_addr(node.node_id),
        )
    raise DFGError(f"cannot lower node op {node.op}")


def compile_dfg(program: DFGProgram) -> dict[str, Any]:
    target = load_target("target/cgra_v1.json")
    params = target["parameters"]
    node_by_id = {node.node_id: node for node in program.nodes}
    placement, cross_node = _placements(program)
    active_tiles = [(0, 0)] if program.topology == "single_tile" else [(0, 0), (0, 1)]
    constant_tiles: dict[str, set[tuple[int, int]]] = {node.node_id: set() for node in program.nodes if node.op in CONST_OPS}
    for node in program.nodes:
        if node.op in CONST_OPS:
            continue
        for source in node.inputs:
            if node_by_id[source].op in CONST_OPS:
                constant_tiles[source].add(placement[node.node_id])

    data_locations: dict[tuple[tuple[int, int], str], int] = {}
    pred_locations: dict[tuple[tuple[int, int], str], int] = {}
    const_slots: dict[tuple[tuple[int, int], str], int] = {}
    for coord in active_tiles:
        next_data = 0
        next_pred = 0
        next_const = 0
        for node in program.nodes:
            if node.op == "const" and coord in constant_tiles[node.node_id]:
                data_locations[(coord, node.node_id)] = next_data
                const_slots[(coord, node.node_id)] = next_const
                next_data += 1
                next_const += 1
            elif node.op == "const_pred" and coord in constant_tiles[node.node_id]:
                pred_locations[(coord, node.node_id)] = next_pred
                next_pred += 1
        if cross_node is not None and coord == (0, 1):
            data_locations[(coord, cross_node.node_id)] = next_data
            next_data += 1
        for node in program.nodes:
            if node.op in CONST_OPS or placement[node.node_id] != coord:
                continue
            if node.kind == "data":
                data_locations[(coord, node.node_id)] = next_data
                next_data += 1
            else:
                pred_locations[(coord, node.node_id)] = next_pred
                next_pred += 1
        if next_data > params["data_rf_depth"] or next_pred > params["pred_rf_depth"] or next_const > params["const_mem_depth"]:
            raise DFGError(f"prototype RF/constant allocation exceeds target capacity at tile {coord}")

    controls: dict[tuple[int, int], dict[int, TileControl]] = {coord: {} for coord in active_tiles}
    const_images: dict[tuple[int, int], list[dict[str, Any]]] = {coord: [] for coord in active_tiles}
    init_lists: dict[tuple[int, int], list[Node]] = {
        coord: [node for node in program.nodes if node.op in CONST_OPS and coord in constant_tiles[node.node_id]]
        for coord in active_tiles
    }
    for coord, init_nodes in init_lists.items():
        for cycle, node in enumerate(init_nodes):
            if node.op == "const":
                addr = const_slots[(coord, node.node_id)]
                const_images[coord].append({"addr": addr, "value": f"0x{node.value:08x}"})
                controls[coord][cycle] = TileControl(op="PASS", src_a="CONST_DATA", const_addr=addr, data_w0_we=True, data_w0_addr=data_locations[(coord, node.node_id)])
            else:
                controls[coord][cycle] = TileControl(pred_w1_we=True, pred_w1_addr=pred_locations[(coord, node.node_id)], pred_w1_src="CONST_TRUE" if node.value else "CONST_FALSE")

    cycle = max((len(items) for items in init_lists.values()), default=0)
    compute_nodes = [node for node in program.nodes if node.op not in CONST_OPS]
    if cross_node is None:
        for node in compute_nodes:
            controls[(0, 0)][cycle] = _compute_control(node, (0, 0), data_locations, pred_locations)
            cycle += 1
    else:
        left_control = _compute_control(cross_node, (0, 0), data_locations, pred_locations)
        controls[(0, 0)][cycle] = replace(left_control, data_routes={"east": RouteDirControl(we=True, src="FU_DATA_RESULT")})
        controls[(0, 1)][cycle + 1] = TileControl(op="PASS", src_a="WEST_DATA_IN", data_w0_we=True, data_w0_addr=data_locations[((0, 1), cross_node.node_id)])
        cycle += 2
        for node in compute_nodes[1:]:
            controls[(0, 1)][cycle] = _compute_control(node, (0, 1), data_locations, pred_locations)
            cycle += 1

    values = _node_values(program)
    output_coord = placement[program.output]
    output_addr = data_locations[(output_coord, program.output)]
    target_ref = {"path": "target/cgra_v1.json", "name": target["name"], "schema": target["schema"]}
    tile_images = []
    for coord in active_tiles:
        entries = [
            {"pc": pc, "chunks": encode_control_chunks(ctrl, target)}
            for pc, ctrl in sorted(controls[coord].items())
        ]
        tile_images.append({
            "row": coord[0], "col": coord[1], "control": entries,
            "const_memory": const_images[coord], "scratchpad_preload": [],
        })
    manifest = {
        "schema": "cgra.program_manifest.v1",
        "name": program.name,
        "version": 1,
        "target": target_ref,
        "run": {"run_cycles": cycle, "result_observation": {"mode": "trace_only"}},
        "program": {"format": "explicit_tile_images", "control_word_encoding": "lsb_first_32bit_chunks", "tiles": tile_images},
        "symbolic_dfg": {
            "schema": "cgra.dfg.v1", "topology": program.topology,
            "nodes": [{"id": node.node_id, "op": node.op} for node in program.nodes],
            "edges": [{"from": source, "to": node.node_id} for node in program.nodes for source in node.inputs],
        },
        "compiler": {
            "name": "minimal_dfg_to_schedule", "topology": program.topology,
            "active_tiles": [list(coord) for coord in active_tiles],
            "cross_tile_value": cross_node.node_id if cross_node is not None else None,
            "limitations": [
                "constant-only inputs", "topologically ordered single-output DFG", "no_lsu", "no_4x4_kernel",
                "1x2 supports one left-to-right data transfer and no predicate transfer",
            ],
        },
        "expected": {
            "output_node": program.output,
            "data_rf": {"row": output_coord[0], "col": output_coord[1], "addr": output_addr},
            "value": f"0x{values[program.output]:08x}",
        },
    }
    errors = validate_program(manifest)
    if errors:
        raise DFGError(f"compiler emitted invalid manifest: {errors[0]}")
    return manifest


def compile_dfg_path(path: str | pathlib.Path) -> dict[str, Any]:
    return compile_dfg(parse_dfg(_read_json(pathlib.Path(path))))


def write_manifest(manifest: dict[str, Any], path: str | pathlib.Path) -> None:
    output = pathlib.Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dfg", help="cgra.dfg.v1 JSON input")
    parser.add_argument("--out", required=True, help="output cgra.program_manifest.v1 path")
    args = parser.parse_args()
    try:
        manifest = compile_dfg_path(args.dfg)
        write_manifest(manifest, args.out)
        diagnostics = check_manifest(args.out)
        if diagnostics:
            raise DFGError(f"checker rejected emitted manifest: {diagnostics[0]}")
        run_manifest(args.out)
    except (DFGError, OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"DFG_INVALID: {exc}", file=sys.stderr)
        return 1
    print(f"SCHEDULE_EMITTED: {args.out} run_cycles={manifest['run']['run_cycles']} topology={manifest['compiler']['topology']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
