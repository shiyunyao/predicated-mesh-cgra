#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CGRA v1 cycle-level golden execution model.

The model covers a multi-tile array, registered one-hop mesh links, LSU
scratchpad/load/store timing, and contract-compatible trace CSV emission.
"""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
from dataclasses import dataclass, field
from typing import Any

MASK32 = 0xFFFF_FFFF
REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

OP_BY_VALUE = {
    0: "NOP",
    1: "PASS",
    2: "ADD",
    3: "SUB",
    4: "MUL",
    5: "AND",
    6: "OR",
    7: "XOR",
    8: "SHL",
    9: "LSHR",
    10: "SELECT",
    16: "CMP_EQ",
    17: "CMP_NE",
    18: "CMP_ULT",
    19: "CMP_ULE",
    32: "PPASS",
    33: "PNOT",
    34: "PAND",
    35: "POR",
}
OP_VALUE = {name: value for value, name in OP_BY_VALUE.items()}

DATA_SRC_BY_VALUE = {
    0: "RF_A",
    1: "RF_B",
    2: "NORTH_DATA_IN",
    3: "SOUTH_DATA_IN",
    4: "EAST_DATA_IN",
    5: "WEST_DATA_IN",
    6: "CONST_DATA",
    7: "LSU_LOAD_DATA",
    8: "ZERO",
}
DATA_SRC_VALUE = {name: value for value, name in DATA_SRC_BY_VALUE.items()}

PRED_SRC_BY_VALUE = {
    0: "RF_A",
    1: "RF_B",
    2: "NORTH_PRED_IN",
    3: "SOUTH_PRED_IN",
    4: "EAST_PRED_IN",
    5: "WEST_PRED_IN",
    6: "CONST_TRUE",
    7: "CONST_FALSE",
}
PRED_SRC_VALUE = {name: value for value, name in PRED_SRC_BY_VALUE.items()}

DATA_ROUTE_SRC_BY_VALUE = {
    0: "NONE",
    1: "NORTH_DATA_IN",
    2: "SOUTH_DATA_IN",
    3: "EAST_DATA_IN",
    4: "WEST_DATA_IN",
    5: "FU_DATA_RESULT",
    6: "RF_A",
    7: "RF_B",
    8: "CONST_DATA",
    9: "LSU_LOAD_DATA",
    10: "ZERO",
}
DATA_ROUTE_SRC_VALUE = {name: value for value, name in DATA_ROUTE_SRC_BY_VALUE.items()}

PRED_ROUTE_SRC_BY_VALUE = {
    0: "NONE",
    1: "NORTH_PRED_IN",
    2: "SOUTH_PRED_IN",
    3: "EAST_PRED_IN",
    4: "WEST_PRED_IN",
    5: "FU_PRED_RESULT",
    6: "RF_A",
    7: "RF_B",
    8: "CONST_TRUE",
    9: "CONST_FALSE",
}
PRED_ROUTE_SRC_VALUE = {name: value for value, name in PRED_ROUTE_SRC_BY_VALUE.items()}

LSU_OP_BY_VALUE = {0: "NONE", 1: "LOAD", 2: "STORE", 3: "RESERVED"}
LSU_OP_VALUE = {name: value for value, name in LSU_OP_BY_VALUE.items()}

DIRECTIONS = ("north", "south", "east", "west")
TRACE_FIELDNAMES = [
    "cycle", "kernel_pc", "tile_row", "tile_col", "op",
    "src_a_valid", "src_a_value", "src_b_valid", "src_b_value",
    "src_p0_valid", "src_p0_value", "src_p1_valid", "src_p1_value",
    "fu_data_valid", "fu_data_result", "fu_pred_valid", "fu_pred_result",
    "data_w0_we", "data_w0_addr", "data_w0_data",
    "data_w1_we", "data_w1_addr", "data_w1_data",
    "pred_w0_we", "pred_w0_addr", "pred_w0_data",
    "pred_w1_we", "pred_w1_addr", "pred_w1_data",
    "data_out_n_valid", "data_out_n_value",
    "data_out_s_valid", "data_out_s_value",
    "data_out_e_valid", "data_out_e_value",
    "data_out_w_valid", "data_out_w_value",
    "pred_out_n_valid", "pred_out_n_value",
    "pred_out_s_valid", "pred_out_s_value",
    "pred_out_e_valid", "pred_out_e_value",
    "pred_out_w_valid", "pred_out_w_value",
    "lsu_op", "lsu_addr", "lsu_store_data", "lsu_store_commit",
    "lsu_load_resp_valid", "lsu_load_resp_data",
]


class GoldenModelError(ValueError):
    """Raised when a schedule/control word violates the CGRA v1 contract."""


def mask32(value: int) -> int:
    return value & MASK32


def bit(value: bool | int) -> int:
    return 1 if bool(value) else 0


def _extract(value: int, lsb: int, width: int) -> int:
    return (value >> lsb) & ((1 << width) - 1)


def _insert(word: int, field_value: int, lsb: int, width: int) -> int:
    mask = ((1 << width) - 1) << lsb
    return (word & ~mask) | ((field_value & ((1 << width) - 1)) << lsb)


def _enum(mapping: dict[int, str], value: int, field: str) -> str:
    if value not in mapping:
        raise GoldenModelError(f"invalid {field} enum value {value}")
    return mapping[value]


def _hex_chunk(value: int) -> str:
    return f"0x{value & MASK32:08x}"


def _trace_bool(value: bool | int) -> str:
    return "1" if bool(value) else "0"


def _trace_hex(value: int) -> str:
    return f"{mask32(value):08x}"


@dataclass(frozen=True)
class RouteDirControl:
    we: bool = False
    src: str = "NONE"


@dataclass(frozen=True)
class TileControl:
    op: str = "NOP"
    src_a: str = "ZERO"
    src_b: str = "ZERO"
    src_p0: str = "CONST_FALSE"
    src_p1: str = "CONST_FALSE"
    data_rf_raddr_a: int = 0
    data_rf_raddr_b: int = 0
    pred_rf_raddr_a: int = 0
    pred_rf_raddr_b: int = 0
    data_w0_we: bool = False
    data_w0_addr: int = 0
    data_w1_we: bool = False
    data_w1_addr: int = 0
    data_w1_src: str = "ZERO"
    pred_w0_we: bool = False
    pred_w0_addr: int = 0
    pred_w1_we: bool = False
    pred_w1_addr: int = 0
    pred_w1_src: str = "CONST_FALSE"
    data_routes: dict[str, RouteDirControl] = field(default_factory=dict)
    pred_routes: dict[str, RouteDirControl] = field(default_factory=dict)
    const_addr: int = 0
    lsu_op: str = "NONE"
    lsu_addr_src: str = "ZERO"
    lsu_store_data_src: str = "ZERO"
    lsu_commit_pred_enable: bool = False
    lsu_commit_pred_invert: bool = False
    lsu_commit_pred_src: str = "CONST_FALSE"

    def __post_init__(self) -> None:
        object.__setattr__(self, "data_routes", _with_default_routes(self.data_routes))
        object.__setattr__(self, "pred_routes", _with_default_routes(self.pred_routes))


@dataclass(frozen=True)
class StepResult:
    op: str
    src_a_valid: bool
    src_a_value: int
    src_b_valid: bool
    src_b_value: int
    src_p0_valid: bool
    src_p0_value: int
    src_p1_valid: bool
    src_p1_value: int
    fu_data_valid: bool
    fu_data_result: int
    fu_pred_valid: bool
    fu_pred_result: int
    data_writes: dict[str, tuple[int, int]]
    pred_writes: dict[str, tuple[int, int]]
    data_routes: dict[str, tuple[bool, int]]
    pred_routes: dict[str, tuple[bool, int]]
    lsu_op: str
    lsu_addr: int
    lsu_store_data: int
    lsu_store_commit: bool
    lsu_load_resp_valid: bool
    lsu_load_resp_data: int


def _with_default_routes(routes: dict[str, RouteDirControl] | None) -> dict[str, RouteDirControl]:
    merged = {direction: RouteDirControl() for direction in DIRECTIONS}
    if routes:
        for direction, ctrl in routes.items():
            if direction not in merged:
                raise GoldenModelError(f"invalid route direction {direction}")
            merged[direction] = ctrl
    return merged


def load_target(path: str | pathlib.Path) -> dict[str, Any]:
    target_path = pathlib.Path(path)
    if not target_path.is_absolute():
        target_path = REPO_ROOT / target_path
    with open(target_path, encoding="utf-8") as f:
        return json.load(f)


def load_json(path: str | pathlib.Path) -> Any:
    json_path = pathlib.Path(path)
    if not json_path.is_absolute():
        json_path = REPO_ROOT / json_path
    with open(json_path, encoding="utf-8") as f:
        return json.load(f)


def decode_control_chunks(chunks: list[str | int], target: dict[str, Any]) -> TileControl:
    expected_chunks = target["parameters"]["control_word_chunks"]
    if len(chunks) != expected_chunks:
        raise GoldenModelError(f"expected {expected_chunks} control chunks, got {len(chunks)}")

    word = 0
    for index, chunk in enumerate(chunks):
        value = int(chunk, 16) if isinstance(chunk, str) else int(chunk)
        if value < 0 or value > MASK32:
            raise GoldenModelError(f"control chunk {index} is not a 32-bit value")
        word |= value << (32 * index)

    padding_lsb = target["parameters"]["raw_control_word_width_bits"]
    physical_width = target["parameters"]["physical_control_word_width_bits"]
    if _extract(word, padding_lsb, physical_width - padding_lsb) != 0:
        raise GoldenModelError("control word padding bits must be zero")

    data_rf_addr_width = target["derived_widths"]["data_rf_addr_width"]
    pred_rf_addr_width = target["derived_widths"]["pred_rf_addr_width"]
    const_addr_width = target["derived_widths"]["const_addr_width"]

    op_width = 6
    data_src_width = 4
    pred_src_width = 4
    data_route_src_width = 4
    pred_route_src_width = 4
    lsu_op_width = 2

    exec_lsb = 0
    exec_width = op_width + (2 * data_src_width) + (2 * pred_src_width) + (2 * data_rf_addr_width) + (2 * pred_rf_addr_width)
    data_rf_lsb = exec_lsb + exec_width
    data_rf_width = 2 + (2 * data_rf_addr_width) + data_src_width
    pred_rf_lsb = data_rf_lsb + data_rf_width
    pred_rf_width = 2 + (2 * pred_rf_addr_width) + pred_src_width
    data_route_lsb = pred_rf_lsb + pred_rf_width
    route_dir_width = 1 + data_route_src_width
    data_route_width = 4 * route_dir_width
    pred_route_lsb = data_route_lsb + data_route_width
    pred_route_dir_width = 1 + pred_route_src_width
    pred_route_width = 4 * pred_route_dir_width
    const_lsb = pred_route_lsb + pred_route_width
    lsu_lsb = const_lsb + const_addr_width

    lsb = exec_lsb
    op = _enum(OP_BY_VALUE, _extract(word, lsb, op_width), "op")
    lsb += op_width
    src_a = _enum(DATA_SRC_BY_VALUE, _extract(word, lsb, data_src_width), "src_a")
    lsb += data_src_width
    src_b = _enum(DATA_SRC_BY_VALUE, _extract(word, lsb, data_src_width), "src_b")
    lsb += data_src_width
    src_p0 = _enum(PRED_SRC_BY_VALUE, _extract(word, lsb, pred_src_width), "src_p0")
    lsb += pred_src_width
    src_p1 = _enum(PRED_SRC_BY_VALUE, _extract(word, lsb, pred_src_width), "src_p1")
    lsb += pred_src_width
    data_rf_raddr_a = _extract(word, lsb, data_rf_addr_width)
    lsb += data_rf_addr_width
    data_rf_raddr_b = _extract(word, lsb, data_rf_addr_width)
    lsb += data_rf_addr_width
    pred_rf_raddr_a = _extract(word, lsb, pred_rf_addr_width)
    lsb += pred_rf_addr_width
    pred_rf_raddr_b = _extract(word, lsb, pred_rf_addr_width)

    lsb = data_rf_lsb
    data_w0_we = bool(_extract(word, lsb, 1))
    lsb += 1
    data_w0_addr = _extract(word, lsb, data_rf_addr_width)
    lsb += data_rf_addr_width
    data_w1_we = bool(_extract(word, lsb, 1))
    lsb += 1
    data_w1_addr = _extract(word, lsb, data_rf_addr_width)
    lsb += data_rf_addr_width
    data_w1_src = _enum(DATA_SRC_BY_VALUE, _extract(word, lsb, data_src_width), "data_w1_src")

    lsb = pred_rf_lsb
    pred_w0_we = bool(_extract(word, lsb, 1))
    lsb += 1
    pred_w0_addr = _extract(word, lsb, pred_rf_addr_width)
    lsb += pred_rf_addr_width
    pred_w1_we = bool(_extract(word, lsb, 1))
    lsb += 1
    pred_w1_addr = _extract(word, lsb, pred_rf_addr_width)
    lsb += pred_rf_addr_width
    pred_w1_src = _enum(PRED_SRC_BY_VALUE, _extract(word, lsb, pred_src_width), "pred_w1_src")

    data_routes = {}
    for index, direction in enumerate(DIRECTIONS):
        lsb = data_route_lsb + index * route_dir_width
        data_routes[direction] = RouteDirControl(
            we=bool(_extract(word, lsb, 1)),
            src=_enum(DATA_ROUTE_SRC_BY_VALUE, _extract(word, lsb + 1, data_route_src_width), f"data_route_{direction}"),
        )

    pred_routes = {}
    for index, direction in enumerate(DIRECTIONS):
        lsb = pred_route_lsb + index * pred_route_dir_width
        pred_routes[direction] = RouteDirControl(
            we=bool(_extract(word, lsb, 1)),
            src=_enum(PRED_ROUTE_SRC_BY_VALUE, _extract(word, lsb + 1, pred_route_src_width), f"pred_route_{direction}"),
        )

    const_addr = _extract(word, const_lsb, const_addr_width)
    lsb = lsu_lsb
    lsu_op = _enum(LSU_OP_BY_VALUE, _extract(word, lsb, lsu_op_width), "lsu_op")
    lsb += lsu_op_width
    lsu_addr_src = _enum(DATA_SRC_BY_VALUE, _extract(word, lsb, data_src_width), "lsu_addr_src")
    lsb += data_src_width
    lsu_store_data_src = _enum(DATA_SRC_BY_VALUE, _extract(word, lsb, data_src_width), "lsu_store_data_src")
    lsb += data_src_width
    lsu_commit_pred_enable = bool(_extract(word, lsb, 1))
    lsb += 1
    lsu_commit_pred_invert = bool(_extract(word, lsb, 1))
    lsb += 1
    lsu_commit_pred_src = _enum(PRED_SRC_BY_VALUE, _extract(word, lsb, pred_src_width), "lsu_commit_pred_src")

    return TileControl(
        op=op,
        src_a=src_a,
        src_b=src_b,
        src_p0=src_p0,
        src_p1=src_p1,
        data_rf_raddr_a=data_rf_raddr_a,
        data_rf_raddr_b=data_rf_raddr_b,
        pred_rf_raddr_a=pred_rf_raddr_a,
        pred_rf_raddr_b=pred_rf_raddr_b,
        data_w0_we=data_w0_we,
        data_w0_addr=data_w0_addr,
        data_w1_we=data_w1_we,
        data_w1_addr=data_w1_addr,
        pred_w0_we=pred_w0_we,
        pred_w0_addr=pred_w0_addr,
        pred_w1_we=pred_w1_we,
        pred_w1_addr=pred_w1_addr,
        data_w1_src=data_w1_src,
        pred_w1_src=pred_w1_src,
        data_routes=data_routes,
        pred_routes=pred_routes,
        const_addr=const_addr,
        lsu_op=lsu_op,
        lsu_addr_src=lsu_addr_src,
        lsu_store_data_src=lsu_store_data_src,
        lsu_commit_pred_enable=lsu_commit_pred_enable,
        lsu_commit_pred_invert=lsu_commit_pred_invert,
        lsu_commit_pred_src=lsu_commit_pred_src,
    )


def encode_control_chunks(ctrl: TileControl, target: dict[str, Any]) -> list[str]:
    data_rf_addr_width = target["derived_widths"]["data_rf_addr_width"]
    pred_rf_addr_width = target["derived_widths"]["pred_rf_addr_width"]
    const_addr_width = target["derived_widths"]["const_addr_width"]
    chunks = target["parameters"]["control_word_chunks"]

    op_width = 6
    data_src_width = 4
    pred_src_width = 4
    data_route_src_width = 4
    pred_route_src_width = 4
    lsu_op_width = 2

    exec_width = op_width + (2 * data_src_width) + (2 * pred_src_width) + (2 * data_rf_addr_width) + (2 * pred_rf_addr_width)
    data_rf_lsb = exec_width
    data_rf_width = 2 + (2 * data_rf_addr_width) + data_src_width
    pred_rf_lsb = data_rf_lsb + data_rf_width
    pred_rf_width = 2 + (2 * pred_rf_addr_width) + pred_src_width
    data_route_lsb = pred_rf_lsb + pred_rf_width
    route_dir_width = 1 + data_route_src_width
    pred_route_lsb = data_route_lsb + (4 * route_dir_width)
    pred_route_dir_width = 1 + pred_route_src_width
    const_lsb = pred_route_lsb + (4 * pred_route_dir_width)
    lsu_lsb = const_lsb + const_addr_width

    word = 0
    lsb = 0
    word = _insert(word, OP_VALUE[ctrl.op], lsb, op_width)
    lsb += op_width
    word = _insert(word, DATA_SRC_VALUE[ctrl.src_a], lsb, data_src_width)
    lsb += data_src_width
    word = _insert(word, DATA_SRC_VALUE[ctrl.src_b], lsb, data_src_width)
    lsb += data_src_width
    word = _insert(word, PRED_SRC_VALUE[ctrl.src_p0], lsb, pred_src_width)
    lsb += pred_src_width
    word = _insert(word, PRED_SRC_VALUE[ctrl.src_p1], lsb, pred_src_width)
    lsb += pred_src_width
    word = _insert(word, ctrl.data_rf_raddr_a, lsb, data_rf_addr_width)
    lsb += data_rf_addr_width
    word = _insert(word, ctrl.data_rf_raddr_b, lsb, data_rf_addr_width)
    lsb += data_rf_addr_width
    word = _insert(word, ctrl.pred_rf_raddr_a, lsb, pred_rf_addr_width)
    lsb += pred_rf_addr_width
    word = _insert(word, ctrl.pred_rf_raddr_b, lsb, pred_rf_addr_width)

    lsb = data_rf_lsb
    word = _insert(word, bit(ctrl.data_w0_we), lsb, 1)
    lsb += 1
    word = _insert(word, ctrl.data_w0_addr, lsb, data_rf_addr_width)
    lsb += data_rf_addr_width
    word = _insert(word, bit(ctrl.data_w1_we), lsb, 1)
    lsb += 1
    word = _insert(word, ctrl.data_w1_addr, lsb, data_rf_addr_width)
    lsb += data_rf_addr_width
    word = _insert(word, DATA_SRC_VALUE[ctrl.data_w1_src], lsb, data_src_width)

    lsb = pred_rf_lsb
    word = _insert(word, bit(ctrl.pred_w0_we), lsb, 1)
    lsb += 1
    word = _insert(word, ctrl.pred_w0_addr, lsb, pred_rf_addr_width)
    lsb += pred_rf_addr_width
    word = _insert(word, bit(ctrl.pred_w1_we), lsb, 1)
    lsb += 1
    word = _insert(word, ctrl.pred_w1_addr, lsb, pred_rf_addr_width)
    lsb += pred_rf_addr_width
    word = _insert(word, PRED_SRC_VALUE[ctrl.pred_w1_src], lsb, pred_src_width)

    for index, direction in enumerate(DIRECTIONS):
        route = ctrl.data_routes[direction]
        lsb = data_route_lsb + index * route_dir_width
        word = _insert(word, bit(route.we), lsb, 1)
        word = _insert(word, DATA_ROUTE_SRC_VALUE[route.src], lsb + 1, data_route_src_width)

    for index, direction in enumerate(DIRECTIONS):
        route = ctrl.pred_routes[direction]
        lsb = pred_route_lsb + index * pred_route_dir_width
        word = _insert(word, bit(route.we), lsb, 1)
        word = _insert(word, PRED_ROUTE_SRC_VALUE[route.src], lsb + 1, pred_route_src_width)

    word = _insert(word, ctrl.const_addr, const_lsb, const_addr_width)
    lsb = lsu_lsb
    word = _insert(word, LSU_OP_VALUE[ctrl.lsu_op], lsb, lsu_op_width)
    lsb += lsu_op_width
    word = _insert(word, DATA_SRC_VALUE[ctrl.lsu_addr_src], lsb, data_src_width)
    lsb += data_src_width
    word = _insert(word, DATA_SRC_VALUE[ctrl.lsu_store_data_src], lsb, data_src_width)
    lsb += data_src_width
    word = _insert(word, bit(ctrl.lsu_commit_pred_enable), lsb, 1)
    lsb += 1
    word = _insert(word, bit(ctrl.lsu_commit_pred_invert), lsb, 1)
    lsb += 1
    word = _insert(word, PRED_SRC_VALUE[ctrl.lsu_commit_pred_src], lsb, pred_src_width)

    return [_hex_chunk(word >> (32 * index)) for index in range(chunks)]


class SingleTileGolden:
    def __init__(self, target: dict[str, Any], has_lsu: bool = False, bank_id: int | None = None):
        self.target = target
        self.has_lsu = has_lsu
        self.bank_id = bank_id
        self.cycle = 0
        params = target["parameters"]
        self.data_rf = [0] * params["data_rf_depth"]
        self.data_valid = [False] * params["data_rf_depth"]
        self.pred_rf = [0] * params["pred_rf_depth"]
        self.pred_valid = [False] * params["pred_rf_depth"]
        self.const_mem = [0] * params["const_mem_depth"]
        self.scratchpad = [0] * params["scratch_bank_depth"]
        self._pending_loads: list[tuple[int, int]] = []

    def write_const(self, addr: int, value: int) -> None:
        self._check_const_addr(addr)
        self.const_mem[addr] = mask32(value)

    def write_scratchpad(self, addr: int, value: int) -> None:
        self._check_scratch_addr(addr)
        if not self.has_lsu:
            raise GoldenModelError("scratchpad preload is only valid for LSU-enabled tiles")
        self.scratchpad[addr] = mask32(value)

    def poke_data_rf(self, addr: int, value: int) -> None:
        self._check_data_addr(addr)
        self.data_rf[addr] = mask32(value)
        self.data_valid[addr] = True

    def poke_pred_rf(self, addr: int, value: int | bool) -> None:
        self._check_pred_addr(addr)
        self.pred_rf[addr] = bit(value)
        self.pred_valid[addr] = True

    def step(self, ctrl: TileControl, inputs: dict[str, tuple[bool, int]] | None = None) -> StepResult:
        inputs = inputs or {}
        load_resp_valid, load_resp_data = self._load_response_for_current_cycle()

        data_read_addrs = self._data_read_addrs(ctrl)
        pred_read_addrs = self._pred_read_addrs(ctrl)
        self._check_read_write_hazards(ctrl, data_read_addrs, pred_read_addrs)

        data_rf_reads = {addr: self._read_data_rf(addr) for addr in data_read_addrs}
        pred_rf_reads = {addr: self._read_pred_rf(addr) for addr in pred_read_addrs}
        const_data = self.const_mem[ctrl.const_addr]

        data_ctx = {
            "RF_A": data_rf_reads.get(ctrl.data_rf_raddr_a, 0),
            "RF_B": data_rf_reads.get(ctrl.data_rf_raddr_b, 0),
            "CONST_DATA": const_data,
            "ZERO": 0,
        }
        if load_resp_valid:
            data_ctx["LSU_LOAD_DATA"] = load_resp_data
        pred_ctx = {
            "RF_A": pred_rf_reads.get(ctrl.pred_rf_raddr_a, 0),
            "RF_B": pred_rf_reads.get(ctrl.pred_rf_raddr_b, 0),
            "CONST_TRUE": 1,
            "CONST_FALSE": 0,
        }

        src_a = self._data_source(ctrl.src_a, data_ctx, inputs)
        src_b = self._data_source(ctrl.src_b, data_ctx, inputs)
        src_p0 = self._pred_source(ctrl.src_p0, pred_ctx, inputs)
        src_p1 = self._pred_source(ctrl.src_p1, pred_ctx, inputs)
        fu_data_valid, fu_data_result, fu_pred_valid, fu_pred_result = execute_fu(
            ctrl.op, src_a, src_b, src_p0, src_p1
        )

        data_ctx["FU_DATA_RESULT"] = fu_data_result
        pred_ctx["FU_PRED_RESULT"] = fu_pred_result

        if ctrl.data_w0_we and not fu_data_valid:
            raise GoldenModelError("DataRF W0 enabled without FU data result")
        if ctrl.pred_w0_we and not fu_pred_valid:
            raise GoldenModelError("PredicateRF PW0 enabled without FU predicate result")
        if ctrl.data_w0_we and ctrl.data_w1_we and ctrl.data_w0_addr == ctrl.data_w1_addr:
            raise GoldenModelError("DataRF W0/W1 write collision")
        if ctrl.pred_w0_we and ctrl.pred_w1_we and ctrl.pred_w0_addr == ctrl.pred_w1_addr:
            raise GoldenModelError("PredicateRF PW0/PW1 write collision")

        lsu_addr = 0
        lsu_store_data = 0
        lsu_store_commit = False
        if ctrl.lsu_op != "NONE":
            lsu_addr, lsu_store_data, lsu_store_commit = self._execute_lsu(ctrl, data_ctx, pred_ctx, inputs)

        data_writes = {}
        pred_writes = {}
        if ctrl.data_w0_we:
            data_writes["w0"] = (ctrl.data_w0_addr, fu_data_result)
        if ctrl.data_w1_we:
            data_writes["w1"] = (ctrl.data_w1_addr, self._data_source(ctrl.data_w1_src, data_ctx, inputs))
        if ctrl.pred_w0_we:
            pred_writes["w0"] = (ctrl.pred_w0_addr, fu_pred_result)
        if ctrl.pred_w1_we:
            pred_writes["w1"] = (ctrl.pred_w1_addr, self._pred_source(ctrl.pred_w1_src, pred_ctx, inputs))

        data_routes = {}
        for direction, route in ctrl.data_routes.items():
            if route.we:
                if route.src == "NONE":
                    raise GoldenModelError(f"DataSwitchBox {direction} invalid source NONE")
                data_routes[direction] = (True, self._data_route_source(route.src, data_ctx, inputs))
            else:
                data_routes[direction] = (False, 0)

        pred_routes = {}
        for direction, route in ctrl.pred_routes.items():
            if route.we:
                if route.src == "NONE":
                    raise GoldenModelError(f"PredicateSwitchBox {direction} invalid source NONE")
                pred_routes[direction] = (True, self._pred_route_source(route.src, pred_ctx, inputs))
            else:
                pred_routes[direction] = (False, 0)

        for addr, value in data_writes.values():
            self._write_data_rf(addr, value)
        for addr, value in pred_writes.values():
            self._write_pred_rf(addr, value)

        self.cycle += 1
        return StepResult(
            op=ctrl.op,
            src_a_valid=True,
            src_a_value=src_a,
            src_b_valid=True,
            src_b_value=src_b,
            src_p0_valid=True,
            src_p0_value=src_p0,
            src_p1_valid=True,
            src_p1_value=src_p1,
            fu_data_valid=fu_data_valid,
            fu_data_result=fu_data_result,
            fu_pred_valid=fu_pred_valid,
            fu_pred_result=fu_pred_result,
            data_writes=data_writes,
            pred_writes=pred_writes,
            data_routes=data_routes,
            pred_routes=pred_routes,
            lsu_op=ctrl.lsu_op,
            lsu_addr=lsu_addr,
            lsu_store_data=lsu_store_data,
            lsu_store_commit=lsu_store_commit,
            lsu_load_resp_valid=load_resp_valid,
            lsu_load_resp_data=load_resp_data,
        )

    def _load_response_for_current_cycle(self) -> tuple[bool, int]:
        ready = [data for ready_cycle, data in self._pending_loads if ready_cycle == self.cycle]
        self._pending_loads = [(ready_cycle, data) for ready_cycle, data in self._pending_loads if ready_cycle != self.cycle]
        if len(ready) > 1:
            raise GoldenModelError("multiple LSU load responses in one cycle")
        if ready:
            return True, ready[0]
        return False, 0

    def _execute_lsu(
        self,
        ctrl: TileControl,
        data_ctx: dict[str, int],
        pred_ctx: dict[str, int],
        inputs: dict[str, tuple[bool, int]],
    ) -> tuple[int, int, bool]:
        if not self.has_lsu:
            raise GoldenModelError("non-LSU tile issued active LSU operation")
        if ctrl.lsu_op == "RESERVED":
            raise GoldenModelError("reserved LSU operation")
        if ctrl.lsu_op == "LOAD" and ctrl.lsu_commit_pred_enable:
            raise GoldenModelError("LSU_LOAD with predicate commit is illegal")

        addr = self._data_source(ctrl.lsu_addr_src, data_ctx, inputs)
        self._check_scratch_addr(addr)
        store_data = 0
        store_commit = False
        if ctrl.lsu_op == "LOAD":
            ready_cycle = self.cycle + self.target["parameters"]["load_latency"]
            self._pending_loads.append((ready_cycle, self.scratchpad[addr]))
        elif ctrl.lsu_op == "STORE":
            store_data = self._data_source(ctrl.lsu_store_data_src, data_ctx, inputs)
            if ctrl.lsu_commit_pred_enable:
                pred = self._pred_source(ctrl.lsu_commit_pred_src, pred_ctx, inputs)
                store_commit = bool(pred ^ bit(ctrl.lsu_commit_pred_invert))
            else:
                store_commit = True
            if store_commit:
                self.scratchpad[addr] = mask32(store_data)
        else:
            raise GoldenModelError(f"unsupported LSU op {ctrl.lsu_op}")
        return addr, store_data, store_commit

    def _data_read_addrs(self, ctrl: TileControl) -> set[int]:
        addrs = set()
        data_sources = [ctrl.src_a, ctrl.src_b]
        if ctrl.data_w1_we:
            data_sources.append(ctrl.data_w1_src)
        if ctrl.lsu_op != "NONE":
            data_sources.append(ctrl.lsu_addr_src)
            if ctrl.lsu_op == "STORE":
                data_sources.append(ctrl.lsu_store_data_src)
        for src in data_sources:
            if src == "RF_A":
                addrs.add(ctrl.data_rf_raddr_a)
            elif src == "RF_B":
                addrs.add(ctrl.data_rf_raddr_b)
        for route in ctrl.data_routes.values():
            if route.we and route.src == "RF_A":
                addrs.add(ctrl.data_rf_raddr_a)
            elif route.we and route.src == "RF_B":
                addrs.add(ctrl.data_rf_raddr_b)
        return addrs

    def _pred_read_addrs(self, ctrl: TileControl) -> set[int]:
        addrs = set()
        pred_sources = [ctrl.src_p0, ctrl.src_p1]
        if ctrl.pred_w1_we:
            pred_sources.append(ctrl.pred_w1_src)
        if ctrl.lsu_op == "STORE" and ctrl.lsu_commit_pred_enable:
            pred_sources.append(ctrl.lsu_commit_pred_src)
        for src in pred_sources:
            if src == "RF_A":
                addrs.add(ctrl.pred_rf_raddr_a)
            elif src == "RF_B":
                addrs.add(ctrl.pred_rf_raddr_b)
        for route in ctrl.pred_routes.values():
            if route.we and route.src == "RF_A":
                addrs.add(ctrl.pred_rf_raddr_a)
            elif route.we and route.src == "RF_B":
                addrs.add(ctrl.pred_rf_raddr_b)
        return addrs

    def _check_read_write_hazards(self, ctrl: TileControl, data_reads: set[int], pred_reads: set[int]) -> None:
        data_write_addrs = []
        if ctrl.data_w0_we:
            data_write_addrs.append(ctrl.data_w0_addr)
        if ctrl.data_w1_we:
            data_write_addrs.append(ctrl.data_w1_addr)
        for addr in data_reads:
            if addr in data_write_addrs:
                raise GoldenModelError(f"DataRF same-cycle read/write hazard on r{addr}")

        pred_write_addrs = []
        if ctrl.pred_w0_we:
            pred_write_addrs.append(ctrl.pred_w0_addr)
        if ctrl.pred_w1_we:
            pred_write_addrs.append(ctrl.pred_w1_addr)
        for addr in pred_reads:
            if addr in pred_write_addrs:
                raise GoldenModelError(f"PredicateRF same-cycle read/write hazard on p{addr}")

    def _data_source(self, src: str, data_ctx: dict[str, int], inputs: dict[str, tuple[bool, int]]) -> int:
        if src in data_ctx:
            return mask32(data_ctx[src])
        return self._input_source(src, inputs, "data")

    def _pred_source(self, src: str, pred_ctx: dict[str, int], inputs: dict[str, tuple[bool, int]]) -> int:
        if src in pred_ctx:
            return bit(pred_ctx[src])
        return bit(self._input_source(src, inputs, "predicate"))

    def _data_route_source(self, src: str, data_ctx: dict[str, int], inputs: dict[str, tuple[bool, int]]) -> int:
        if src in data_ctx:
            return mask32(data_ctx[src])
        return self._input_source(src, inputs, "data route")

    def _pred_route_source(self, src: str, pred_ctx: dict[str, int], inputs: dict[str, tuple[bool, int]]) -> int:
        if src in pred_ctx:
            return bit(pred_ctx[src])
        return bit(self._input_source(src, inputs, "predicate route"))

    def _input_source(self, src: str, inputs: dict[str, tuple[bool, int]], kind: str) -> int:
        if src not in inputs:
            raise GoldenModelError(f"{kind} source selected without valid input: {src}")
        valid, value = inputs[src]
        if not valid:
            raise GoldenModelError(f"{kind} source selected without valid input: {src}")
        return mask32(value)

    def _read_data_rf(self, addr: int) -> int:
        self._check_data_addr(addr)
        if not self.data_valid[addr]:
            raise GoldenModelError(f"DataRF uninitialized read r{addr}")
        return self.data_rf[addr]

    def _read_pred_rf(self, addr: int) -> int:
        self._check_pred_addr(addr)
        if not self.pred_valid[addr]:
            raise GoldenModelError(f"PredicateRF uninitialized read p{addr}")
        return self.pred_rf[addr]

    def _write_data_rf(self, addr: int, value: int) -> None:
        self._check_data_addr(addr)
        self.data_rf[addr] = mask32(value)
        self.data_valid[addr] = True

    def _write_pred_rf(self, addr: int, value: int) -> None:
        self._check_pred_addr(addr)
        self.pred_rf[addr] = bit(value)
        self.pred_valid[addr] = True

    def _check_data_addr(self, addr: int) -> None:
        if not 0 <= addr < len(self.data_rf):
            raise GoldenModelError(f"DataRF address out of range: {addr}")

    def _check_pred_addr(self, addr: int) -> None:
        if not 0 <= addr < len(self.pred_rf):
            raise GoldenModelError(f"PredicateRF address out of range: {addr}")

    def _check_const_addr(self, addr: int) -> None:
        if not 0 <= addr < len(self.const_mem):
            raise GoldenModelError(f"const memory address out of range: {addr}")

    def _check_scratch_addr(self, addr: int) -> None:
        if not 0 <= addr < len(self.scratchpad):
            raise GoldenModelError(f"scratchpad address out of range: {addr}")


class MultiTileGolden:
    def __init__(self, target: dict[str, Any], rows: int | None = None, cols: int | None = None):
        self.target = target
        params = target["parameters"]
        self.rows = rows if rows is not None else params["array_rows"]
        self.cols = cols if cols is not None else params["array_cols"]
        enabled = {(tile["row"], tile["col"]): tile["bank_id"] for tile in target["lsu"]["enabled_tiles"]}
        self.tiles: dict[tuple[int, int], SingleTileGolden] = {}
        for row in range(self.rows):
            for col in range(self.cols):
                bank_id = enabled.get((row, col))
                self.tiles[(row, col)] = SingleTileGolden(target, has_lsu=bank_id is not None, bank_id=bank_id)
        self.data_inputs = self._empty_inputs()
        self.pred_inputs = self._empty_inputs()
        self.cycle = 0

    def _empty_inputs(self) -> dict[tuple[int, int], dict[str, tuple[bool, int]]]:
        return {(row, col): {} for row in range(self.rows) for col in range(self.cols)}

    def step(self, controls: dict[tuple[int, int], TileControl]) -> list[dict[str, str]]:
        results: dict[tuple[int, int], StepResult] = {}
        next_data = self._empty_inputs()
        next_pred = self._empty_inputs()
        records = []
        for row in range(self.rows):
            for col in range(self.cols):
                coord = (row, col)
                ctrl = controls.get(coord, TileControl())
                inputs = {**self.data_inputs[coord], **self.pred_inputs[coord]}
                result = self.tiles[coord].step(ctrl, inputs)
                results[coord] = result
                records.append(trace_record(self.cycle, self.cycle, row, col, result))

        for (row, col), result in results.items():
            self._route_outputs(row, col, result, next_data, next_pred)
        self.data_inputs = next_data
        self.pred_inputs = next_pred
        self.cycle += 1
        return records

    def run(self, controls_by_tile: dict[tuple[int, int], list[TileControl]], run_cycles: int) -> list[dict[str, str]]:
        rows = []
        for pc in range(run_cycles):
            controls = {coord: ctrls[pc] for coord, ctrls in controls_by_tile.items() if pc < len(ctrls)}
            rows.extend(self.step(controls))
        return rows

    def _route_outputs(
        self,
        row: int,
        col: int,
        result: StepResult,
        next_data: dict[tuple[int, int], dict[str, tuple[bool, int]]],
        next_pred: dict[tuple[int, int], dict[str, tuple[bool, int]]],
    ) -> None:
        data_neighbors = {
            "north": (row - 1, col, "SOUTH_DATA_IN"),
            "south": (row + 1, col, "NORTH_DATA_IN"),
            "east": (row, col + 1, "WEST_DATA_IN"),
            "west": (row, col - 1, "EAST_DATA_IN"),
        }
        pred_neighbors = {
            "north": (row - 1, col, "SOUTH_PRED_IN"),
            "south": (row + 1, col, "NORTH_PRED_IN"),
            "east": (row, col + 1, "WEST_PRED_IN"),
            "west": (row, col - 1, "EAST_PRED_IN"),
        }
        for direction, (valid, value) in result.data_routes.items():
            nr, nc, src = data_neighbors[direction]
            if valid and 0 <= nr < self.rows and 0 <= nc < self.cols:
                next_data[(nr, nc)][src] = (True, value)
        for direction, (valid, value) in result.pred_routes.items():
            nr, nc, src = pred_neighbors[direction]
            if valid and 0 <= nr < self.rows and 0 <= nc < self.cols:
                next_pred[(nr, nc)][src] = (True, value)


def _op_uses_data_a(op: str) -> bool:
    return op in {"PASS", "ADD", "SUB", "MUL", "AND", "OR", "XOR", "SHL", "LSHR", "SELECT", "CMP_EQ", "CMP_NE", "CMP_ULT", "CMP_ULE"}


def _op_uses_data_b(op: str) -> bool:
    return op in {"ADD", "SUB", "MUL", "AND", "OR", "XOR", "SHL", "LSHR", "SELECT", "CMP_EQ", "CMP_NE", "CMP_ULT", "CMP_ULE"}


def _op_uses_pred_p0(op: str) -> bool:
    return op in {"SELECT", "PPASS", "PNOT", "PAND", "POR"}


def _op_uses_pred_p1(op: str) -> bool:
    return op in {"PAND", "POR"}


def trace_record(cycle: int, kernel_pc: int, row: int, col: int, result: StepResult) -> dict[str, str]:
    rec = {name: "0" for name in TRACE_FIELDNAMES}
    src_a_valid = result.src_a_valid and _op_uses_data_a(result.op)
    src_b_valid = result.src_b_valid and _op_uses_data_b(result.op)
    src_p0_valid = result.src_p0_valid and _op_uses_pred_p0(result.op)
    src_p1_valid = result.src_p1_valid and _op_uses_pred_p1(result.op)
    rec.update({
        "cycle": str(cycle),
        "kernel_pc": str(kernel_pc),
        "tile_row": str(row),
        "tile_col": str(col),
        "op": str(OP_VALUE[result.op]),
        "src_a_valid": _trace_bool(src_a_valid),
        "src_a_value": _trace_hex(result.src_a_value if src_a_valid else 0),
        "src_b_valid": _trace_bool(src_b_valid),
        "src_b_value": _trace_hex(result.src_b_value if src_b_valid else 0),
        "src_p0_valid": _trace_bool(src_p0_valid),
        "src_p0_value": str(bit(result.src_p0_value if src_p0_valid else 0)),
        "src_p1_valid": _trace_bool(src_p1_valid),
        "src_p1_value": str(bit(result.src_p1_value if src_p1_valid else 0)),
        "fu_data_valid": _trace_bool(result.fu_data_valid),
        "fu_data_result": _trace_hex(result.fu_data_result),
        "fu_pred_valid": _trace_bool(result.fu_pred_valid),
        "fu_pred_result": str(bit(result.fu_pred_result)),
        "lsu_op": str(LSU_OP_VALUE[result.lsu_op]),
        "lsu_addr": _trace_hex(result.lsu_addr),
        "lsu_store_data": _trace_hex(result.lsu_store_data),
        "lsu_store_commit": _trace_bool(result.lsu_store_commit),
        "lsu_load_resp_valid": _trace_bool(result.lsu_load_resp_valid),
        "lsu_load_resp_data": _trace_hex(result.lsu_load_resp_data),
    })
    for port, prefix in (("w0", "data_w0"), ("w1", "data_w1")):
        if port in result.data_writes:
            addr, value = result.data_writes[port]
            rec[f"{prefix}_we"] = "1"
            rec[f"{prefix}_addr"] = str(addr)
            rec[f"{prefix}_data"] = _trace_hex(value)
    for port, prefix in (("w0", "pred_w0"), ("w1", "pred_w1")):
        if port in result.pred_writes:
            addr, value = result.pred_writes[port]
            rec[f"{prefix}_we"] = "1"
            rec[f"{prefix}_addr"] = str(addr)
            rec[f"{prefix}_data"] = str(bit(value))
    for direction, short in (("north", "n"), ("south", "s"), ("east", "e"), ("west", "w")):
        valid, value = result.data_routes[direction]
        rec[f"data_out_{short}_valid"] = _trace_bool(valid)
        rec[f"data_out_{short}_value"] = _trace_hex(value)
        valid, value = result.pred_routes[direction]
        rec[f"pred_out_{short}_valid"] = _trace_bool(valid)
        rec[f"pred_out_{short}_value"] = str(bit(value))
    return rec


def write_trace_csv(path: str | pathlib.Path, rows: list[dict[str, str]]) -> None:
    trace_path = pathlib.Path(path)
    if not trace_path.is_absolute():
        trace_path = REPO_ROOT / trace_path
    trace_path.parent.mkdir(parents=True, exist_ok=True)
    with open(trace_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=TRACE_FIELDNAMES, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def execute_fu(op: str, a: int = 0, b: int = 0, p0: int = 0, p1: int = 0) -> tuple[bool, int, bool, int]:
    a = mask32(a)
    b = mask32(b)
    p0 = bit(p0)
    p1 = bit(p1)

    if op == "NOP":
        return False, 0, False, 0
    if op == "PASS":
        return True, a, False, 0
    if op == "ADD":
        return True, mask32(a + b), False, 0
    if op == "SUB":
        return True, mask32(a - b), False, 0
    if op == "MUL":
        return True, mask32(a * b), False, 0
    if op == "AND":
        return True, a & b, False, 0
    if op == "OR":
        return True, a | b, False, 0
    if op == "XOR":
        return True, a ^ b, False, 0
    if op == "SHL":
        return True, mask32(a << (b & 0x1F)), False, 0
    if op == "LSHR":
        return True, (a >> (b & 0x1F)), False, 0
    if op == "SELECT":
        return True, a if p0 else b, False, 0
    if op == "CMP_EQ":
        return False, 0, True, bit(a == b)
    if op == "CMP_NE":
        return False, 0, True, bit(a != b)
    if op == "CMP_ULT":
        return False, 0, True, bit(a < b)
    if op == "CMP_ULE":
        return False, 0, True, bit(a <= b)
    if op == "PPASS":
        return False, 0, True, p0
    if op == "PNOT":
        return False, 0, True, bit(not p0)
    if op == "PAND":
        return False, 0, True, p0 & p1
    if op == "POR":
        return False, 0, True, p0 | p1
    raise GoldenModelError(f"unsupported FU op {op}")


def _manifest_tile_control(entry: dict[str, Any], target: dict[str, Any]) -> TileControl:
    return decode_control_chunks(entry["chunks"], target)


def run_manifest(manifest_path: str | pathlib.Path, trace_out: str | pathlib.Path | None = None) -> list[dict[str, str]]:
    manifest = load_json(manifest_path)
    target = load_target(manifest["target"]["path"])
    array = MultiTileGolden(target)
    controls_by_tile: dict[tuple[int, int], list[TileControl]] = {}
    for tile_image in manifest["program"]["tiles"]:
        coord = (tile_image["row"], tile_image["col"])
        tile = array.tiles[coord]
        for entry in tile_image.get("const_memory", []):
            tile.write_const(entry["addr"], int(entry["value"], 16))
        for entry in tile_image.get("scratchpad_preload", []):
            tile.write_scratchpad(entry["addr"], int(entry["value"], 16))
        controls = [TileControl() for _ in range(manifest["run"]["run_cycles"])]
        for ctrl_entry in tile_image["control"]:
            pc = ctrl_entry["pc"]
            if pc < len(controls):
                controls[pc] = _manifest_tile_control(ctrl_entry, target)
        controls_by_tile[coord] = controls
    rows = array.run(controls_by_tile, manifest["run"]["run_cycles"])
    if trace_out is not None:
        write_trace_csv(trace_out, rows)
    return rows


def run_self_test(target_path: str) -> None:
    target = load_target(target_path)
    tile = SingleTileGolden(target)
    tile.write_const(0, 0x12345678)
    result = tile.step(TileControl(op="PASS", src_a="CONST_DATA", const_addr=0, data_w0_we=True, data_w0_addr=2))
    assert result.fu_data_valid and result.fu_data_result == 0x12345678
    assert tile.data_rf[2] == 0x12345678 and tile.data_valid[2]

    tile.poke_data_rf(0, 0xFFFF_FFFF)
    tile.write_const(1, 2)
    add = tile.step(TileControl(op="ADD", src_a="RF_A", src_b="CONST_DATA", data_rf_raddr_a=0, const_addr=1))
    assert add.fu_data_valid and add.fu_data_result == 1

    tile.poke_pred_rf(0, 1)
    pand = tile.step(TileControl(op="PAND", src_p0="RF_A", src_p1="CONST_TRUE", pred_rf_raddr_a=0, pred_w0_we=True, pred_w0_addr=1))
    assert pand.fu_pred_valid and pand.fu_pred_result == 1

    ctrl = TileControl(op="XOR", src_a="RF_A", src_b="CONST_DATA", data_rf_raddr_a=0, const_addr=1)
    decoded = decode_control_chunks(encode_control_chunks(ctrl, target), target)
    assert decoded.op == ctrl.op and decoded.src_a == ctrl.src_a and decoded.const_addr == ctrl.const_addr

    try:
        tile.step(TileControl(op="NOP", data_w0_we=True, data_w0_addr=0))
    except GoldenModelError as exc:
        assert "W0 enabled without FU data result" in str(exc)
    else:
        raise AssertionError("expected W0 no-result error")

    lsu_tile = SingleTileGolden(target, has_lsu=True, bank_id=0)
    lsu_tile.write_scratchpad(3, 0x55)
    lsu_tile.write_const(0, 3)
    lsu_tile.step(TileControl(lsu_op="LOAD", lsu_addr_src="CONST_DATA", const_addr=0))
    lsu_tile.step(TileControl())
    load = lsu_tile.step(TileControl(op="PASS", src_a="LSU_LOAD_DATA", data_w0_we=True, data_w0_addr=1))
    assert load.fu_data_valid and load.fu_data_result == 0x55


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default="target/cgra_v1.json")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", help="Run a cgra.program_manifest.v1 manifest")
    parser.add_argument("--trace-out", help="Write golden trace CSV when --run is used")
    args = parser.parse_args()

    if args.self_test:
        run_self_test(args.target)
        print(f"golden model self-test passed: target={args.target}")
        return 0
    if args.run:
        rows = run_manifest(args.run, args.trace_out)
        print(f"golden model run passed: manifest={args.run} records={len(rows)}")
        if args.trace_out:
            print(f"trace_out={args.trace_out}")
        return 0

    parser.error("no action requested; use --self-test or --run")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
