#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare and compare compiler-generated RTL simulations.

The harness deliberately keeps the executable source of truth in the normal
DFG -> program-manifest -> config-stream chain.  It materializes a small
testbench from the emitted writes only, then compares the RTL's existing CSV
trace against a golden-model trace for that same manifest.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import shutil
import sys
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from model.golden_model import run_manifest, write_trace_csv
from tools.check_schedule import check_manifest
from tools.dfg_to_schedule import compile_dfg_path, write_manifest
from tools.emit_config import emit_config_manifest, validate_config_manifest, write_config_manifest
from tools.trace_compare import compare_trace_paths
from tools.validate_program import validate_program


TRACE_HEX_FIELDS = {
    "src_a_value",
    "src_b_value",
    "fu_data_result",
    "data_w0_data",
    "data_w1_data",
    "data_out_n_value",
    "data_out_s_value",
    "data_out_e_value",
    "data_out_w_value",
    "lsu_addr",
    "lsu_load_resp_data",
}


class GeneratedProgramError(ValueError):
    """Raised when a generated-program artifact cannot be prepared safely."""


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _write_json(path: pathlib.Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def rtl_compatible_trace_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    """Normalize golden-model numerals to the RTL trace writer's CSV format."""

    normalized: list[dict[str, str]] = []
    for row in rows:
        converted = copy.deepcopy(row)
        for field in TRACE_HEX_FIELDS:
            converted[field] = f"{int(converted[field], 16):x}"
        converted["lsu_store_data"] = str(int(converted["lsu_store_data"], 16))
        normalized.append(converted)
    return normalized


def _sv_value(value: int | str) -> str:
    integer = int(value, 16) if isinstance(value, str) else value
    return f"32'h{integer:08x}"


def _sv_path(path: pathlib.Path) -> str:
    return json.dumps(str(path))


def _lsu_mask(target: dict[str, Any]) -> tuple[int, str]:
    params = target["parameters"]
    rows = params["array_rows"]
    cols = params["array_cols"]
    tiles = rows * cols
    mask = 0
    for entry in target["lsu"]["enabled_tiles"]:
        mask |= 1 << (entry["row"] * cols + entry["col"])
    digits = max(1, (tiles + 3) // 4)
    return tiles, f"{tiles}'h{mask:0{digits}x}"


def emit_testbench(config: dict[str, Any], dfg_path: pathlib.Path, config_path: pathlib.Path, path: pathlib.Path) -> None:
    """Emit one testbench whose only configuration values come from *config*."""

    target_path = REPO_ROOT / config["target"]["path"]
    with target_path.open(encoding="utf-8") as handle:
        target = json.load(handle)
    params = target["parameters"]
    rows = params["array_rows"]
    cols = params["array_cols"]
    tiles, lsu_mask = _lsu_mask(target)
    writes = config["config_stream"]["writes"]
    run_cycles = config["config_stream"]["run"]["run_cycles"]
    cases = []
    for index, write in enumerate(writes):
        cases.append(
            "        "
            f"{index}: drive_cfg_int(2'd{write['cfg_mem_type']}, "
            f"CTRL_PC_WIDTH'({write['cfg_tile_row']}), CTRL_PC_WIDTH'({write['cfg_tile_col']}), "
            f"SCRATCH_ADDR_WIDTH'({write['cfg_addr']}), 2'({write['cfg_word_idx']}), "
            f"{_sv_value(write['cfg_wdata'])});"
        )
    case_text = "\n".join(cases)
    content = f"""// SPDX-License-Identifier: MIT
`timescale 1ns/1ps

module generated_program_tb(input logic clk);
  import cgra_pkg::*;

  localparam int ROWS = {rows};
  localparam int COLS = {cols};
  localparam int TILES = {tiles};
  localparam logic [TILES-1:0] HAS_LSU_MASK = {lsu_mask};
  localparam int WRITE_COUNT = {len(writes)};
  localparam int RUN_CYCLES = {run_cycles};
  localparam int START_CYCLE = 1 + WRITE_COUNT;
  localparam string INPUT_DFG = {_sv_path(dfg_path)};
  localparam string CONFIG_STREAM = {_sv_path(config_path)};

  logic rst_n;
  logic cfg_valid;
  logic cfg_ready;
  logic cfg_we;
  logic [1:0] cfg_mem_type;
  logic [CTRL_PC_WIDTH-1:0] cfg_tile_row;
  logic [CTRL_PC_WIDTH-1:0] cfg_tile_col;
  logic [SCRATCH_ADDR_WIDTH-1:0] cfg_addr;
  logic [1:0] cfg_word_idx;
  logic [31:0] cfg_wdata;
  logic start;
  logic [CTRL_PC_WIDTH-1:0] run_cycles;
  logic busy;
  logic done;
  logic [CTRL_PC_WIDTH-1:0] kernel_pc;
  logic [TILES-1:0] north_data_we;
  logic [TILES*DATA_WIDTH-1:0] north_data_out;
  logic [TILES-1:0] south_data_we;
  logic [TILES*DATA_WIDTH-1:0] south_data_out;
  logic [TILES-1:0] east_data_we;
  logic [TILES*DATA_WIDTH-1:0] east_data_out;
  logic [TILES-1:0] west_data_we;
  logic [TILES*DATA_WIDTH-1:0] west_data_out;
  logic unused_outputs;
  int cycle;

  cgra_top #(
    .ROWS(ROWS),
    .COLS(COLS),
    .HAS_LSU_MASK(HAS_LSU_MASK)
  ) dut (
    .clk(clk),
    .rst_n(rst_n),
    .cfg_valid(cfg_valid),
    .cfg_ready(cfg_ready),
    .cfg_we(cfg_we),
    .cfg_mem_type(cfg_mem_type),
    .cfg_tile_row(cfg_tile_row),
    .cfg_tile_col(cfg_tile_col),
    .cfg_addr(cfg_addr),
    .cfg_word_idx(cfg_word_idx),
    .cfg_wdata(cfg_wdata),
    .start(start),
    .run_cycles(run_cycles),
    .busy(busy),
    .done(done),
    .kernel_pc(kernel_pc),
    .north_data_we(north_data_we),
    .north_data_out(north_data_out),
    .south_data_we(south_data_we),
    .south_data_out(south_data_out),
    .east_data_we(east_data_we),
    .east_data_out(east_data_out),
    .west_data_we(west_data_we),
    .west_data_out(west_data_out)
  );

  assign unused_outputs = (|north_data_we) ^ (^north_data_out)
                        ^ (|south_data_we) ^ (^south_data_out)
                        ^ (|east_data_we) ^ (^east_data_out)
                        ^ (|west_data_we) ^ (^west_data_out);

  always_comb begin
    if (unused_outputs) begin
    end
  end

/* verilator lint_off BLKSEQ */
  task automatic clear_cfg;
    begin
      cfg_valid = 1'b0;
      cfg_we = 1'b0;
      cfg_mem_type = '0;
      cfg_tile_row = '0;
      cfg_tile_col = '0;
      cfg_addr = '0;
      cfg_word_idx = '0;
      cfg_wdata = '0;
    end
  endtask

  task automatic drive_cfg_int(input logic [1:0] mem_type,
                               input logic [CTRL_PC_WIDTH-1:0] row,
                               input logic [CTRL_PC_WIDTH-1:0] col,
                               input logic [SCRATCH_ADDR_WIDTH-1:0] addr,
                               input logic [1:0] word_idx,
                               input logic [31:0] data);
    begin
      cfg_valid = 1'b1;
      cfg_we = 1'b1;
      cfg_mem_type = mem_type;
      cfg_tile_row = row;
      cfg_tile_col = col;
      cfg_addr = addr;
      cfg_word_idx = word_idx;
      cfg_wdata = data;
    end
  endtask

  task automatic drive_stream_write(input int index);
    begin
      unique case (index)
{case_text}
        default: $fatal(1, "Generated config stream index out of range dfg=%s config=%s write=%0d", INPUT_DFG, CONFIG_STREAM, index);
      endcase
    end
  endtask
/* verilator lint_on BLKSEQ */

  initial begin
    cycle = 0;
    rst_n = 1'b0;
    clear_cfg();
    start = 1'b0;
    run_cycles = '0;
  end

/* verilator lint_off BLKSEQ */
  always_ff @(negedge clk) begin
    cycle <= cycle + 1;
    rst_n <= 1'b1;
    clear_cfg();
    start <= 1'b0;

    if (cycle == 0) begin
      rst_n <= 1'b0;
    end else if ((cycle >= 1) && (cycle < START_CYCLE)) begin
      if (!cfg_ready) begin
        $fatal(1, "Generated config stream stalled dfg=%s config=%s seed=none cycle=%0d write=%0d", INPUT_DFG, CONFIG_STREAM, cycle, cycle - 1);
      end
      drive_stream_write(cycle - 1);
    end else if (cycle == START_CYCLE) begin
      if (!cfg_ready) begin
        $fatal(1, "Generated start rejected dfg=%s config=%s seed=none cycle=%0d", INPUT_DFG, CONFIG_STREAM, cycle);
      end
      run_cycles <= CTRL_PC_WIDTH'(RUN_CYCLES);
      start <= 1'b1;
    end else if ((cycle >= START_CYCLE + 1) && (cycle <= START_CYCLE + RUN_CYCLES)) begin
      if (!busy) begin
        $fatal(1, "Generated run ended early dfg=%s config=%s seed=none cycle=%0d", INPUT_DFG, CONFIG_STREAM, cycle);
      end
      if (kernel_pc !== CTRL_PC_WIDTH'(cycle - START_CYCLE - 1)) begin
        $fatal(1, "Generated pc mismatch dfg=%s config=%s seed=none cycle=%0d expected_pc=%0d actual_pc=%0d", INPUT_DFG, CONFIG_STREAM, cycle, cycle - START_CYCLE - 1, kernel_pc);
      end
    end else if (cycle == START_CYCLE + RUN_CYCLES + 1) begin
      if (!done) begin
        $fatal(1, "Generated done pulse missing dfg=%s config=%s seed=none cycle=%0d", INPUT_DFG, CONFIG_STREAM, cycle);
      end
    end else if (cycle == START_CYCLE + RUN_CYCLES + 2) begin
      if (done) begin
        $fatal(1, "Generated done pulse did not clear dfg=%s config=%s seed=none cycle=%0d", INPUT_DFG, CONFIG_STREAM, cycle);
      end
      $display("GENERATED_PROGRAM_RTL_PASS dfg=%s config=%s seed=none run_cycles=%0d", INPUT_DFG, CONFIG_STREAM, RUN_CYCLES);
      $finish;
    end else if (cycle > START_CYCLE + RUN_CYCLES + 4) begin
      $fatal(1, "Generated program timed out dfg=%s config=%s seed=none cycle=%0d", INPUT_DFG, CONFIG_STREAM, cycle);
    end
  end
/* verilator lint_on BLKSEQ */
endmodule : generated_program_tb
"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def prepare_generated_program(dfg: str | pathlib.Path, out_dir: str | pathlib.Path) -> dict[str, pathlib.Path]:
    """Materialize all static artifacts needed for one generated RTL run."""

    dfg_path = pathlib.Path(dfg)
    if not dfg_path.is_absolute():
        dfg_path = REPO_ROOT / dfg_path
    if not dfg_path.is_file():
        raise GeneratedProgramError(f"DFG input does not exist: {dfg_path}")
    output = pathlib.Path(out_dir)
    if not output.is_absolute():
        output = REPO_ROOT / output
    output.mkdir(parents=True, exist_ok=True)

    artifacts = {
        "input_dfg": output / "input.dfg.json",
        "program_manifest": output / "program_manifest.json",
        "config_stream": output / "config_stream.json",
        "golden_trace": output / "golden_trace.csv",
        "testbench": output / "generated_program_tb.sv",
        "metadata": output / "artifacts.json",
    }
    shutil.copyfile(dfg_path, artifacts["input_dfg"])
    manifest = compile_dfg_path(dfg_path)
    manifest_errors = validate_program(manifest)
    if manifest_errors:
        raise GeneratedProgramError("generated manifest is invalid: " + "; ".join(manifest_errors))
    write_manifest(manifest, artifacts["program_manifest"])
    schedule_errors = check_manifest(artifacts["program_manifest"])
    if schedule_errors:
        raise GeneratedProgramError(f"schedule checker rejected generated manifest: {schedule_errors[0]}")

    config = emit_config_manifest(manifest)
    config_errors = validate_config_manifest(config)
    if config_errors:
        raise GeneratedProgramError("generated config stream is invalid: " + "; ".join(config_errors))
    write_config_manifest(config, artifacts["config_stream"])
    config_schedule_errors = check_manifest(artifacts["config_stream"])
    if config_schedule_errors:
        raise GeneratedProgramError(f"schedule checker rejected generated config stream: {config_schedule_errors[0]}")

    golden_rows = rtl_compatible_trace_rows(run_manifest(artifacts["program_manifest"]))
    write_trace_csv(artifacts["golden_trace"], golden_rows)
    emit_testbench(config, artifacts["input_dfg"], artifacts["config_stream"], artifacts["testbench"])
    metadata = {
        "schema": "cgra.generated_program_rtl.v1",
        "version": 1,
        "source_dfg": {"path": str(dfg), "sha256": _sha256(dfg_path)},
        "seed": None,
        "artifacts": {
            key: {"path": value.name, "sha256": _sha256(value)}
            for key, value in artifacts.items()
            if key != "metadata"
        },
    }
    _write_json(artifacts["metadata"], metadata)
    return artifacts


def compare_generated_traces(
    *,
    dfg: str | pathlib.Path,
    config: str | pathlib.Path,
    golden: str | pathlib.Path,
    rtl: str | pathlib.Path,
) -> list[str]:
    """Decorate shared trace diagnostics with generated-program provenance."""

    context = f"dfg={dfg} config={config} seed=none"
    try:
        diagnostics = compare_trace_paths(golden, rtl)
    except (OSError, ValueError) as exc:
        return [
            "GENERATED_PROGRAM_TRACE_MISMATCH "
            f"{context} cycle=unknown tile=(unknown,unknown) field=trace_format: {exc}"
        ]
    return [f"GENERATED_PROGRAM_TRACE_MISMATCH {context} {diagnostic}" for diagnostic in diagnostics]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--prepare", metavar="DFG", help="Compile and archive one cgra.dfg.v1 input")
    mode.add_argument("--compare", action="store_true", help="Compare prepared golden and RTL traces")
    parser.add_argument("--out-dir", help="Artifact directory for --prepare")
    parser.add_argument("--dfg", help="Archived source DFG for --compare")
    parser.add_argument("--config", help="Emitted config stream for --compare")
    parser.add_argument("--golden", help="Golden CSV trace for --compare")
    parser.add_argument("--rtl", help="RTL CSV trace for --compare")
    args = parser.parse_args()

    try:
        if args.prepare:
            if not args.out_dir:
                parser.error("--out-dir is required with --prepare")
            artifacts = prepare_generated_program(args.prepare, args.out_dir)
            print(
                "GENERATED_PROGRAM_PREPARED "
                f"dfg={args.prepare} config={artifacts['config_stream']} seed=none "
                f"artifacts={artifacts['metadata'].parent}"
            )
            return 0

        required = {"--dfg": args.dfg, "--config": args.config, "--golden": args.golden, "--rtl": args.rtl}
        missing = [flag for flag, value in required.items() if not value]
        if missing:
            parser.error("--compare requires " + ", ".join(missing))
        diagnostics = compare_generated_traces(dfg=args.dfg, config=args.config, golden=args.golden, rtl=args.rtl)
        if diagnostics:
            for diagnostic in diagnostics:
                print(diagnostic, file=sys.stderr)
            return 1
        print(f"GENERATED_PROGRAM_TRACE_MATCH dfg={args.dfg} config={args.config} seed=none")
        return 0
    except (GeneratedProgramError, OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"GENERATED_PROGRAM_ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
