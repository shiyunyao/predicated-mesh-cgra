#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run external compiler program manifests through the RTL replay flow.

The executable source of truth is a target-encoded
``cgra.program_manifest.v1``. This module validates the supplied image,
materializes its configuration stream and testbench, and compares the RTL
trace with the cycle-level golden model.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from model.golden_model import manifest_execution_steps, run_manifest, write_trace_csv
from tools.check_schedule import check_manifest
from tools.emit_config import emit_config_manifest, validate_config_manifest, write_config_manifest
from tools.trace_compare import compare_trace_paths
from tools.validate_program import load_json, validate_program


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


class ProgramRunnerError(ValueError):
    """Raised when a program manifest cannot be replayed safely."""


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


def emit_testbench(
    config: dict[str, Any],
    manifest_path: pathlib.Path,
    config_path: pathlib.Path,
    path: pathlib.Path,
) -> None:
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
    execution_steps = manifest_execution_steps(config)
    loop = config.get("loop")
    loop_enabled = isinstance(loop, dict) and loop.get("enabled") is True
    cases = []
    for index, write in enumerate(writes):
        cases.append(
            "        "
            f"{index}: drive_cfg_int(2'd{write['cfg_mem_type']}, "
            f"CTRL_PC_WIDTH'({write['cfg_tile_row']}), CTRL_PC_WIDTH'({write['cfg_tile_col']}), "
            f"SCRATCHPAD_ADDR_WIDTH'({write['cfg_addr']}), 2'({write['cfg_word_idx']}), "
            f"{_sv_value(write['cfg_wdata'])});"
        )
    case_text = "\n".join(cases)
    pc_case_text = "\n".join(
        f"        {index}: expected_pc = CTRL_PC_WIDTH'({step.pc});"
        for index, step in enumerate(execution_steps)
    )
    loop_write_indices = {
        write["cfg_addr"]: index
        for index, write in enumerate(writes)
        if write["mem_type"] == "LOOP_DESC"
    }
    descriptor_overrides = ""
    if loop_enabled:
        descriptor_overrides = f"""
      if (index == {loop_write_indices[0]}) begin
        cfg_wdata = 32'(active_prologue);
      end
      if (index == {loop_write_indices[2]}) begin
        cfg_wdata = 32'(active_trip_count);
      end
      if (index == {loop_write_indices[3]}) begin
        cfg_wdata = 32'(active_epilogue);
      end
      if (($test$plusargs("SKIP_LOOP_COMMIT") != 0) && (index == {loop_write_indices[4]})) begin
        clear_cfg();
      end
      if (($test$plusargs("INVALID_LOOP_SPAN") != 0) && (index == {loop_write_indices[0]})) begin
        cfg_wdata = 32'd255;
      end
      if (($test$plusargs("INVALID_LOOP_II") != 0) && (index == {loop_write_indices[1]})) begin
        cfg_wdata = 32'd0;
      end"""
    loop_localparams = """
  localparam bit LOOP_MODE = 1'b0;
  localparam int LOOP_PROLOGUE = 0;
  localparam int LOOP_II = 0;
  localparam int LOOP_TRIP_COUNT = 0;
  localparam int LOOP_EPILOGUE = 0;"""
    expected_pc_body = f"""unique case (index)
{pc_case_text}
        default: expected_pc = 'x;
      endcase"""
    if loop_enabled:
        loop_localparams = f"""
  localparam bit LOOP_MODE = 1'b1;
  localparam int LOOP_PROLOGUE = {loop['prologue_cycles']};
  localparam int LOOP_II = {loop['ii']};
  localparam int LOOP_TRIP_COUNT = {loop['trip_count']};
  localparam int LOOP_EPILOGUE = {loop['epilogue_cycles']};"""
        expected_pc_body = """if (!active_loop_mode) begin
        expected_pc = CTRL_PC_WIDTH'(index);
      end else if (index < active_prologue) begin
        expected_pc = CTRL_PC_WIDTH'(index);
      end else if (index < active_prologue + active_trip_count * LOOP_II) begin
        expected_pc = CTRL_PC_WIDTH'(active_prologue + ((index - active_prologue) % LOOP_II));
      end else begin
        expected_pc = CTRL_PC_WIDTH'(active_prologue + LOOP_II
                                     + index - (active_prologue + active_trip_count * LOOP_II));
      end"""
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
{loop_localparams}
  localparam string INPUT_MANIFEST = {_sv_path(manifest_path)};
  localparam string CONFIG_STREAM = {_sv_path(config_path)};

  logic rst_n;
  logic cfg_valid;
  logic cfg_ready;
  logic cfg_we;
  logic [1:0] cfg_mem_type;
  logic [CTRL_PC_WIDTH-1:0] cfg_tile_row;
  logic [CTRL_PC_WIDTH-1:0] cfg_tile_col;
  logic [SCRATCHPAD_ADDR_WIDTH-1:0] cfg_addr;
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
  bit active_loop_mode;
  int active_prologue;
  int active_trip_count;
  int active_epilogue;
  int active_run_cycles;

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
                               input logic [SCRATCHPAD_ADDR_WIDTH-1:0] addr,
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
        default: $fatal(1, "Generated config stream index out of range manifest=%s config=%s write=%0d", INPUT_MANIFEST, CONFIG_STREAM, index);
      endcase
{descriptor_overrides}
    end
  endtask

  function automatic logic [CTRL_PC_WIDTH-1:0] expected_pc(input int index);
    begin
      {expected_pc_body}
    end
  endfunction
/* verilator lint_on BLKSEQ */

  initial begin
    cycle = 0;
    rst_n = 1'b0;
    clear_cfg();
    start = 1'b0;
    run_cycles = '0;
    active_loop_mode = LOOP_MODE && ($test$plusargs("SKIP_LOOP_COMMIT") == 0);
    active_prologue = LOOP_PROLOGUE;
    active_trip_count = LOOP_TRIP_COUNT;
    active_epilogue = LOOP_EPILOGUE;
    if (LOOP_MODE && ($test$plusargs("LOOP_ZERO_BOUNDARIES") != 0)) begin
      active_prologue = 0;
      active_epilogue = 0;
    end
    if (LOOP_MODE && ($test$plusargs("LOOP_TRIP_COUNT_1") != 0)) begin
      active_trip_count = 1;
    end
    if (LOOP_MODE && ($test$plusargs("LOOP_TRIP_COUNT_2") != 0)) begin
      active_trip_count = 2;
    end
    if (LOOP_MODE && ($test$plusargs("LOOP_TRIP_COUNT_4") != 0)) begin
      active_trip_count = 4;
    end
    if (LOOP_MODE && ($test$plusargs("LOOP_TRIP_COUNT_7") != 0)) begin
      active_trip_count = 7;
    end
    if (LOOP_MODE && ($value$plusargs("LOOP_TRIP_COUNT=%d", active_trip_count) != 0)) begin
    end
    active_run_cycles = active_loop_mode
                        ? active_prologue + active_trip_count * LOOP_II + active_epilogue
                        : RUN_CYCLES;
    if (LOOP_MODE && (active_trip_count <= 0)) begin
      $fatal(1, "LOOP_TRIP_COUNT plusarg must be positive");
    end
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
        $fatal(1, "Generated config stream stalled manifest=%s config=%s seed=none cycle=%0d write=%0d", INPUT_MANIFEST, CONFIG_STREAM, cycle, cycle - 1);
      end
      drive_stream_write(cycle - 1);
    end else if (cycle == START_CYCLE) begin
      if (!cfg_ready) begin
        $fatal(1, "Generated start rejected manifest=%s config=%s seed=none cycle=%0d", INPUT_MANIFEST, CONFIG_STREAM, cycle);
      end
      run_cycles <= CTRL_PC_WIDTH'(active_run_cycles);
      start <= 1'b1;
    end else if ((cycle >= START_CYCLE + 1) && (cycle <= START_CYCLE + active_run_cycles)) begin
      if (!busy) begin
        $fatal(1, "Generated run ended early manifest=%s config=%s seed=none cycle=%0d", INPUT_MANIFEST, CONFIG_STREAM, cycle);
      end
      if (kernel_pc !== expected_pc(cycle - START_CYCLE - 1)) begin
        $fatal(1, "Generated pc mismatch manifest=%s config=%s seed=none cycle=%0d expected_pc=%0d actual_pc=%0d", INPUT_MANIFEST, CONFIG_STREAM, cycle, expected_pc(cycle - START_CYCLE - 1), kernel_pc);
      end
      if ($test$plusargs("LOOP_DESC_DURING_RUN") != 0) begin
        drive_cfg_int(2'd3, '0, '0, '0, '0, 32'd1);
      end
    end else if (cycle == START_CYCLE + active_run_cycles + 1) begin
      if (!done) begin
        $fatal(1, "Generated done pulse missing manifest=%s config=%s seed=none cycle=%0d", INPUT_MANIFEST, CONFIG_STREAM, cycle);
      end
      if ($test$plusargs("LOOP_DESC_DURING_DONE") != 0) begin
        drive_cfg_int(2'd3, '0, '0, '0, '0, 32'd1);
      end
    end else if (cycle == START_CYCLE + active_run_cycles + 2) begin
      if (done) begin
        $fatal(1, "Generated done pulse did not clear manifest=%s config=%s seed=none cycle=%0d", INPUT_MANIFEST, CONFIG_STREAM, cycle);
      end
      $display("PROGRAM_RTL_PASS manifest=%s config=%s seed=none run_cycles=%0d", INPUT_MANIFEST, CONFIG_STREAM, active_run_cycles);
      $finish;
    end else if (cycle > START_CYCLE + active_run_cycles + 4) begin
      $fatal(1, "Generated program timed out manifest=%s config=%s seed=none cycle=%0d", INPUT_MANIFEST, CONFIG_STREAM, cycle);
    end
  end
/* verilator lint_on BLKSEQ */
endmodule : generated_program_tb
"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def prepare_program_manifest(
    manifest_path: str | pathlib.Path,
    out_dir: str | pathlib.Path,
) -> dict[str, pathlib.Path]:
    """Materialize artifacts needed to replay one external program manifest."""

    source_path = pathlib.Path(manifest_path)
    if not source_path.is_absolute():
        source_path = REPO_ROOT / source_path
    if not source_path.is_file():
        raise ProgramRunnerError(f"program manifest does not exist: {source_path}")
    output = pathlib.Path(out_dir)
    if not output.is_absolute():
        output = REPO_ROOT / output
    output.mkdir(parents=True, exist_ok=True)

    artifacts = {
        "program_manifest": output / "program_manifest.json",
        "config_stream": output / "config_stream.json",
        "golden_trace": output / "golden_trace.csv",
        "testbench": output / "generated_program_tb.sv",
        "metadata": output / "artifacts.json",
    }
    shutil.copyfile(source_path, artifacts["program_manifest"])
    manifest = load_json(artifacts["program_manifest"])
    manifest_errors = validate_program(manifest)
    if manifest_errors:
        raise ProgramRunnerError("program manifest is invalid: " + "; ".join(manifest_errors))
    schedule_errors = check_manifest(artifacts["program_manifest"])
    if schedule_errors:
        raise ProgramRunnerError(f"schedule checker rejected program manifest: {schedule_errors[0]}")

    config = emit_config_manifest(manifest)
    config_errors = validate_config_manifest(config)
    if config_errors:
        raise ProgramRunnerError("config stream is invalid: " + "; ".join(config_errors))
    write_config_manifest(config, artifacts["config_stream"])
    config_schedule_errors = check_manifest(artifacts["config_stream"])
    if config_schedule_errors:
        raise ProgramRunnerError(f"schedule checker rejected config stream: {config_schedule_errors[0]}")

    golden_rows = rtl_compatible_trace_rows(run_manifest(artifacts["program_manifest"]))
    write_trace_csv(artifacts["golden_trace"], golden_rows)
    emit_testbench(config, artifacts["program_manifest"], artifacts["config_stream"], artifacts["testbench"])
    metadata = {
        "schema": "cgra.program_rtl.v1",
        "version": 1,
        "source_manifest": {"path": str(manifest_path), "sha256": _sha256(source_path)},
        "seed": None,
        "artifacts": {
            key: {"path": value.name, "sha256": _sha256(value)}
            for key, value in artifacts.items()
            if key != "metadata"
        },
    }
    _write_json(artifacts["metadata"], metadata)
    return artifacts


def compare_program_traces(
    *,
    manifest: str | pathlib.Path,
    config: str | pathlib.Path,
    golden: str | pathlib.Path,
    rtl: str | pathlib.Path,
) -> list[str]:
    """Decorate shared trace diagnostics with program-manifest provenance."""

    context = f"manifest={manifest} config={config} seed=none"
    try:
        diagnostics = compare_trace_paths(golden, rtl)
    except (OSError, ValueError) as exc:
        return [
            "PROGRAM_TRACE_MISMATCH "
            f"{context} cycle=unknown tile=(unknown,unknown) field=trace_format: {exc}"
        ]
    return [f"PROGRAM_TRACE_MISMATCH {context} {diagnostic}" for diagnostic in diagnostics]


def run_program(
    manifest: str | pathlib.Path,
    out_dir: str | pathlib.Path,
    *,
    verilator: str = "verilator",
) -> dict[str, pathlib.Path]:
    """Prepare, build, run, and compare one external program manifest."""

    artifacts = prepare_program_manifest(manifest, out_dir)
    output = artifacts["metadata"].parent
    obj_dir = output / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    verilator_cmd = [
        verilator,
        "-Wall",
        "--cc",
        "--exe",
        "--build",
        "--Mdir",
        str(obj_dir),
        "--prefix",
        "Vtop",
        "-f",
        str(REPO_ROOT / "synth" / "rtl_files.f"),
        str(artifacts["testbench"]),
        str(REPO_ROOT / "sim" / "data_rf_main.cpp"),
        "--top-module",
        "generated_program_tb",
    ]
    subprocess.run(verilator_cmd, cwd=REPO_ROOT, check=True)
    rtl_trace = output / "rtl_trace.csv"
    artifacts["rtl_trace"] = rtl_trace
    simulation_log = output / "rtl_simulation.log"
    simulation_args = [
        str(obj_dir / "Vtop"),
        "+CGRA_TRACE",
        f"+CGRA_TRACE_FILE={rtl_trace}",
    ]
    archived_manifest = load_json(artifacts["program_manifest"])
    if archived_manifest.get("loop", {}).get("enabled") is True:
        simulation_args.append("+CGRA_LOOP_TRACE")
    with simulation_log.open("w", encoding="utf-8") as log:
        subprocess.run(
            simulation_args,
            cwd=REPO_ROOT,
            check=True,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
    diagnostics = compare_program_traces(
        manifest=artifacts["program_manifest"],
        config=artifacts["config_stream"],
        golden=artifacts["golden_trace"],
        rtl=rtl_trace,
    )
    if diagnostics:
        raise ProgramRunnerError("\n".join(diagnostics))
    print(
        "PROGRAM_TRACE_MATCH "
        f"manifest={artifacts['program_manifest']} config={artifacts['config_stream']} "
        f"artifacts={output}"
    )
    return artifacts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--prepare", metavar="MANIFEST", help="Prepare one cgra.program_manifest.v1 input")
    mode.add_argument("--run", metavar="MANIFEST", help="Prepare, run, and compare one program manifest")
    mode.add_argument("--compare", action="store_true", help="Compare prepared golden and RTL traces")
    parser.add_argument("--out-dir", help="Artifact directory for --prepare or --run")
    parser.add_argument("--verilator", default="verilator", help="Verilator executable for --run")
    parser.add_argument("--manifest", help="Archived program manifest for --compare")
    parser.add_argument("--config", help="Emitted config stream for --compare")
    parser.add_argument("--golden", help="Golden CSV trace for --compare")
    parser.add_argument("--rtl", help="RTL CSV trace for --compare")
    args = parser.parse_args()

    try:
        if args.prepare:
            if not args.out_dir:
                parser.error("--out-dir is required with --prepare")
            artifacts = prepare_program_manifest(args.prepare, args.out_dir)
            print(
                "PROGRAM_PREPARED "
                f"manifest={args.prepare} config={artifacts['config_stream']} seed=none "
                f"artifacts={artifacts['metadata'].parent}"
            )
            return 0

        if args.run:
            if not args.out_dir:
                parser.error("--out-dir is required with --run")
            run_program(args.run, args.out_dir, verilator=args.verilator)
            return 0

        required = {
            "--manifest": args.manifest,
            "--config": args.config,
            "--golden": args.golden,
            "--rtl": args.rtl,
        }
        missing = [flag for flag, value in required.items() if not value]
        if missing:
            parser.error("--compare requires " + ", ".join(missing))
        diagnostics = compare_program_traces(
            manifest=args.manifest,
            config=args.config,
            golden=args.golden,
            rtl=args.rtl,
        )
        if diagnostics:
            for diagnostic in diagnostics:
                print(diagnostic, file=sys.stderr)
            return 1
        print(f"PROGRAM_TRACE_MATCH manifest={args.manifest} config={args.config} seed=none")
        return 0
    except (ProgramRunnerError, subprocess.CalledProcessError, OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"PROGRAM_RUN_ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
