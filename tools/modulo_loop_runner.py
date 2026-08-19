#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare and compare a finite modulo-loop external program replay."""

from __future__ import annotations

import argparse
import copy
import json
import pathlib
import sys
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from model.golden_model import run_manifest, write_trace_csv
from tools.check_schedule import check_manifest
from tools.emit_config import emit_config_manifest, validate_config_manifest, write_config_manifest
from tools.program_runner import _sha256, _write_json, emit_testbench, rtl_compatible_trace_rows
from tools.trace_compare import compare_trace_paths
from tools.validate_program import load_json, validate_program


class ModuloLoopError(ValueError):
    """Raised when loop replay artifacts cannot be produced consistently."""


def _resolve(path: str | pathlib.Path) -> pathlib.Path:
    resolved = pathlib.Path(path)
    return resolved if resolved.is_absolute() else REPO_ROOT / resolved


def prepare_modulo_loop(
    program: str | pathlib.Path,
    out_dir: str | pathlib.Path,
    trip_count: int | None = None,
    zero_boundaries: bool = False,
) -> dict[str, pathlib.Path]:
    source = _resolve(program)
    if not source.is_file():
        raise ModuloLoopError(f"program manifest does not exist: {source}")
    output = _resolve(out_dir)
    output.mkdir(parents=True, exist_ok=True)
    artifacts = {
        "input_program": output / "input_program.json",
        "config_stream": output / "config_stream.json",
        "golden_trace": output / "golden_trace.csv",
        "testbench": output / "generated_program_tb.sv",
        "metadata": output / "artifacts.json",
    }

    manifest: dict[str, Any] = copy.deepcopy(load_json(source))
    if trip_count is not None:
        if trip_count <= 0 or trip_count > 0xffff_ffff:
            raise ModuloLoopError("trip-count override must fit unsigned 32 bits and be positive")
        manifest["loop"]["trip_count"] = trip_count
        manifest["run"]["run_cycles"] = (
            manifest["loop"]["prologue_cycles"]
            + trip_count * manifest["loop"]["ii"]
            + manifest["loop"]["epilogue_cycles"]
        )
    if zero_boundaries:
        manifest["loop"]["prologue_cycles"] = 0
        manifest["loop"]["epilogue_cycles"] = 0
        manifest["run"]["run_cycles"] = manifest["loop"]["trip_count"] * manifest["loop"]["ii"]
        span = manifest["loop"]["ii"]
        for tile in manifest["program"]["tiles"]:
            tile["control"] = [entry for entry in tile["control"] if entry["pc"] < span]
    errors = validate_program(manifest)
    if errors:
        raise ModuloLoopError("invalid loop manifest: " + "; ".join(errors))
    if manifest.get("loop", {}).get("enabled") is not True:
        raise ModuloLoopError("modulo-loop runner requires loop.enabled=true")
    _write_json(artifacts["input_program"], manifest)
    diagnostics = check_manifest(artifacts["input_program"])
    if diagnostics:
        raise ModuloLoopError(f"schedule checker rejected loop manifest: {diagnostics[0]}")

    config = emit_config_manifest(manifest)
    config_errors = validate_config_manifest(config)
    if config_errors:
        raise ModuloLoopError("invalid emitted config stream: " + "; ".join(config_errors))
    write_config_manifest(config, artifacts["config_stream"])
    golden_rows = rtl_compatible_trace_rows(run_manifest(artifacts["input_program"]))
    write_trace_csv(artifacts["golden_trace"], golden_rows)
    emit_testbench(config, artifacts["input_program"], artifacts["config_stream"], artifacts["testbench"])

    metadata = {
        "schema": "cgra.modulo_loop_rtl.v1",
        "version": 1,
        "source_program": {"path": str(program), "sha256": _sha256(source)},
        "loop": manifest["loop"],
        "expanded_run_cycles": manifest["run"]["run_cycles"],
        "artifacts": {
            key: {"path": value.name, "sha256": _sha256(value)}
            for key, value in artifacts.items()
            if key != "metadata"
        },
    }
    _write_json(artifacts["metadata"], metadata)
    return artifacts


def compare_modulo_traces(golden: str | pathlib.Path, rtl: str | pathlib.Path) -> list[str]:
    try:
        diagnostics = compare_trace_paths(golden, rtl)
    except (OSError, ValueError) as exc:
        return [f"MODULO_LOOP_TRACE_MISMATCH field=trace_format: {exc}"]
    return [f"MODULO_LOOP_TRACE_MISMATCH {diagnostic}" for diagnostic in diagnostics]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--prepare", metavar="PROGRAM")
    mode.add_argument("--compare", action="store_true")
    parser.add_argument("--out-dir")
    parser.add_argument("--trip-count", type=int)
    parser.add_argument("--zero-boundaries", action="store_true")
    parser.add_argument("--golden")
    parser.add_argument("--rtl")
    args = parser.parse_args()
    try:
        if args.prepare:
            if not args.out_dir:
                parser.error("--out-dir is required with --prepare")
            artifacts = prepare_modulo_loop(
                args.prepare,
                args.out_dir,
                trip_count=args.trip_count,
                zero_boundaries=args.zero_boundaries,
            )
            print(f"MODULO_LOOP_PREPARED artifacts={artifacts['metadata'].parent}")
            return 0
        if not args.golden or not args.rtl:
            parser.error("--compare requires --golden and --rtl")
        diagnostics = compare_modulo_traces(args.golden, args.rtl)
        if diagnostics:
            for diagnostic in diagnostics:
                print(diagnostic, file=sys.stderr)
            return 1
        print(f"MODULO_LOOP_TRACE_MATCH golden={args.golden} rtl={args.rtl}")
        return 0
    except (ModuloLoopError, OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"MODULO_LOOP_ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
