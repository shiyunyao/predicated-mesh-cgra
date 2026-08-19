#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Emit deterministic CGRA v1 configuration streams from program manifests.

The emitted document remains a ``cgra.program_manifest.v1`` object so the
existing program and schedule validators can consume it. It adds one
derived ``config_stream`` section describing the ordered configuration-bus
writes and the subsequent START command. Sparse compiler images are expanded
here: every target tile and every executable control slot receives a
deterministic control write, with omitted entries represented by the all-zero
NOP image. This keeps the loader contract deterministic even when the RTL
memory has no reset value. It is intentionally a static loader artifact: no
host readback, runtime retry, or RTL protocol is implied.
"""

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

from model.golden_model import GoldenModelError, decode_control_chunks, encode_control_chunks, load_target
from tools.validate_program import load_json, validate_program


CONFIG_STREAM_SCHEMA = "cgra.config_stream.v1"
CONFIG_MEM_TYPES = {
    "CONTROL_MEM": 0,
    "CONST_MEM": 1,
    "SCRATCHPAD_BANK": 2,
    "LOOP_DESC": 3,
}


class ConfigStreamError(ValueError):
    """Raised when a valid program image cannot form a configuration stream."""


def _target_for(manifest: dict[str, Any]) -> dict[str, Any]:
    target_obj = manifest.get("target")
    if not isinstance(target_obj, dict) or not isinstance(target_obj.get("path"), str):
        raise ConfigStreamError("program manifest has no usable target.path")
    try:
        return load_target(target_obj["path"])
    except (OSError, json.JSONDecodeError, KeyError) as exc:
        raise ConfigStreamError(f"cannot load target: {exc}") from exc


def _round_trip_chunks(chunks: list[str], target: dict[str, Any], context: str) -> list[str]:
    """Prove that an image follows the target's LSB-first packing order."""

    try:
        packed_again = encode_control_chunks(decode_control_chunks(chunks, target), target)
    except (GoldenModelError, TypeError, ValueError, KeyError) as exc:
        raise ConfigStreamError(f"{context} cannot round-trip through control pack/unpack: {exc}") from exc
    if packed_again != chunks:
        raise ConfigStreamError(
            f"{context} control chunk round-trip changed the image: "
            f"source={chunks!r} packed={packed_again!r}"
        )
    return packed_again


def _write(sequence: int, mem_type: str, row: int, col: int, addr: int, word_idx: int, data: str) -> dict[str, Any]:
    return {
        "sequence": sequence,
        "mem_type": mem_type,
        "cfg_mem_type": CONFIG_MEM_TYPES[mem_type],
        "cfg_tile_row": row,
        "cfg_tile_col": col,
        "cfg_addr": addr,
        "cfg_word_idx": word_idx,
        "cfg_wdata": data,
    }


def expected_writes(manifest: dict[str, Any], target: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    """Reconstruct canonical writes from a validated program manifest.

    Ordering is deliberately independent of JSON-list order: tile coordinates,
    memory addresses, and control PCs are each sorted before emitting writes.
    The target's complete executable tile/PC image is emitted so omitted
    compiler entries cannot leave an implementation-dependent value in control
    memory. Loop manifests initialize only P+II+E physical control slots.
    """

    target = target or _target_for(manifest)
    params = target["parameters"]
    tile_images = {
        (tile["row"], tile["col"]): tile for tile in manifest["program"]["tiles"]
    }
    nop_chunks = ["0x00000000"] * params["control_word_chunks"]
    loop = manifest.get("loop")
    control_span = (
        loop["prologue_cycles"] + loop["ii"] + loop["epilogue_cycles"]
        if isinstance(loop, dict) and loop.get("enabled") is True
        else manifest["run"]["run_cycles"]
    )
    writes: list[dict[str, Any]] = []

    for row in range(params["array_rows"]):
        for col in range(params["array_cols"]):
            tile = tile_images.get((row, col))
            if tile is not None:
                for entry in sorted(tile["const_memory"], key=lambda image: image["addr"]):
                    writes.append(_write(len(writes), "CONST_MEM", row, col, entry["addr"], 0, entry["value"]))
                for entry in sorted(tile["scratchpad_preload"], key=lambda image: image["addr"]):
                    writes.append(_write(len(writes), "SCRATCHPAD_BANK", row, col, entry["addr"], 0, entry["value"]))

            explicit_controls = {} if tile is None else {
                entry["pc"]: entry for entry in tile["control"]
            }
            # Preserve explicitly supplied PCs outside the run window while
            # initializing every PC that START can execute.
            pcs = sorted(set(range(control_span)) | set(explicit_controls))
            for pc in pcs:
                entry = explicit_controls.get(pc)
                chunks = nop_chunks if entry is None else _round_trip_chunks(
                    entry["chunks"], target, f"tile=({row},{col}) pc={pc}"
                )
                for word_idx, chunk in enumerate(chunks):
                    writes.append(_write(len(writes), "CONTROL_MEM", row, col, pc, word_idx, chunk))
    if isinstance(loop, dict):
        descriptor_values = (
            loop["prologue_cycles"],
            loop["ii"],
            loop["trip_count"],
            loop["epilogue_cycles"],
            int(loop["enabled"]),
        )
        for addr, value in enumerate(descriptor_values):
            writes.append(_write(len(writes), "LOOP_DESC", 0, 0, addr, 0, f"0x{value:08x}"))
    return writes


def _stream_metadata(manifest: dict[str, Any], target: dict[str, Any]) -> dict[str, Any]:
    params = target["parameters"]
    return {
        "schema": CONFIG_STREAM_SCHEMA,
        "version": 1,
        "target": copy.deepcopy(manifest["target"]),
        "control_word": {
            "encoding": manifest["program"]["control_word_encoding"],
            "chunks_per_word": params["control_word_chunks"],
            "physical_width_bits": params["physical_control_word_width_bits"],
            "chunk_order": "lsb_first",
        },
    }


def emit_config_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    """Return a copy of *manifest* augmented with its canonical config stream."""

    errors = validate_program(manifest)
    if errors:
        raise ConfigStreamError("program manifest is invalid: " + "; ".join(errors))
    target = _target_for(manifest)
    emitted = copy.deepcopy(manifest)
    writes = expected_writes(manifest, target)
    stream = _stream_metadata(manifest, target)
    stream["writes"] = writes
    stream["write_count"] = len(writes)
    stream["run"] = {"command": "START", "run_cycles": manifest["run"]["run_cycles"]}
    if manifest.get("loop", {}).get("enabled") is True:
        stream["run"]["mode"] = "LOOP"
    emitted["config_stream"] = stream
    return emitted


def emit_config_path(path: str | pathlib.Path) -> dict[str, Any]:
    """Load a program manifest and return its configuration-stream form."""

    return emit_config_manifest(load_json(path))


def validate_config_manifest(manifest: dict[str, Any]) -> list[str]:
    """Validate an emitted configuration stream by rebuilding it from source images."""

    errors = validate_program(manifest)
    if errors:
        return errors
    target = _target_for(manifest)
    stream = manifest.get("config_stream")
    if not isinstance(stream, dict):
        return ["missing config_stream object"]

    expected_metadata = _stream_metadata(manifest, target)
    for key, expected in expected_metadata.items():
        if stream.get(key) != expected:
            errors.append(f"config_stream.{key} does not match target/program metadata")

    try:
        writes = expected_writes(manifest, target)
    except ConfigStreamError as exc:
        return errors + [str(exc)]
    if stream.get("writes") != writes:
        errors.append("config_stream.writes do not match deterministic program-derived writes")
    if stream.get("write_count") != len(writes):
        errors.append("config_stream.write_count does not match config_stream.writes")
    expected_run = {"command": "START", "run_cycles": manifest["run"]["run_cycles"]}
    if manifest.get("loop", {}).get("enabled") is True:
        expected_run["mode"] = "LOOP"
    if stream.get("run") != expected_run:
        errors.append("config_stream.run does not match manifest run-cycle count")
    return errors


def write_config_manifest(manifest: dict[str, Any], path: str | pathlib.Path) -> None:
    """Write one deterministic, human-readable configuration manifest."""

    output = pathlib.Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", help="Input cgra.program_manifest.v1 JSON file")
    parser.add_argument("--out", required=True, help="Output config-stream JSON file")
    args = parser.parse_args()

    try:
        config_manifest = emit_config_path(args.manifest)
        errors = validate_config_manifest(config_manifest)
    except (ConfigStreamError, OSError, json.JSONDecodeError) as exc:
        print(f"CONFIG_STREAM_ERROR: {exc}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"CONFIG_STREAM_ERROR: {error}", file=sys.stderr)
        return 1

    write_config_manifest(config_manifest, args.out)
    print(f"CONFIG_STREAM_EMITTED: {args.out} writes={config_manifest['config_stream']['write_count']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
