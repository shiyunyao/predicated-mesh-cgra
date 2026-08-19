#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate CGRA program/run-manifest JSON files."""

import argparse
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
ALLOWED_OBSERVATION_MODES = {"trace_only", "top_output", "future_readback", "unsupported"}
REQUIRED_TOP_LEVEL = {"schema", "name", "version", "target", "run", "program"}
REQUIRED_TILE_KEYS = {"row", "col", "control", "const_memory", "scratchpad_preload"}
LOOP_KEYS = {"enabled", "prologue_cycles", "ii", "trip_count", "epilogue_cycles"}
HEX32_PREFIX = "0x"


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def require(condition, message, errors):
    if not condition:
        errors.append(message)


def is_hex32(value):
    if not isinstance(value, str):
        return False
    if not value.startswith(HEX32_PREFIX):
        return False
    digits = value[len(HEX32_PREFIX):]
    if len(digits) != 8:
        return False
    try:
        int(digits, 16)
    except ValueError:
        return False
    return True


def load_target(target_obj, errors):
    if not isinstance(target_obj, dict):
        errors.append("target must be an object")
        return None
    target_path = target_obj.get("path")
    if not isinstance(target_path, str):
        errors.append("target.path must be a string")
        return None
    path = REPO_ROOT / target_path
    if not path.exists():
        errors.append(f"target.path does not exist: {target_path}")
        return None
    target = load_json(path)
    if target_obj.get("name") != target.get("name"):
        errors.append(f"target.name mismatch: manifest={target_obj.get('name')!r} target={target.get('name')!r}")
    if target_obj.get("schema") != target.get("schema"):
        errors.append(f"target.schema mismatch: manifest={target_obj.get('schema')!r} target={target.get('schema')!r}")
    return target


def validate_tile_coord(tile, target, prefix, errors):
    row = tile.get("row")
    col = tile.get("col")
    rows = target["parameters"]["array_rows"]
    cols = target["parameters"]["array_cols"]
    require(isinstance(row, int), f"{prefix}.row must be an integer", errors)
    require(isinstance(col, int), f"{prefix}.col must be an integer", errors)
    if isinstance(row, int):
        require(0 <= row < rows, f"{prefix}.row out of range: {row}", errors)
    if isinstance(col, int):
        require(0 <= col < cols, f"{prefix}.col out of range: {col}", errors)


def loop_descriptor(manifest):
    """Return the optional loop descriptor without interpreting malformed input."""

    loop = manifest.get("loop") if isinstance(manifest, dict) else None
    return loop if isinstance(loop, dict) else None


def loop_total_cycles(loop):
    """Return P + N*II + E for a structurally valid loop descriptor."""

    return loop["prologue_cycles"] + loop["trip_count"] * loop["ii"] + loop["epilogue_cycles"]


def validate_program(manifest):
    errors = []
    require(isinstance(manifest, dict), "manifest must be a JSON object", errors)
    if errors:
        return errors

    missing = sorted(REQUIRED_TOP_LEVEL - set(manifest))
    require(not missing, f"missing top-level keys: {missing}", errors)
    require(manifest.get("schema") == "cgra.program_manifest.v1", f"schema must be cgra.program_manifest.v1, got {manifest.get('schema')!r}", errors)
    require(isinstance(manifest.get("name"), str) and manifest.get("name"), "name must be a non-empty string", errors)
    require(manifest.get("version") == 1, f"version must be 1, got {manifest.get('version')!r}", errors)

    target = load_target(manifest.get("target"), errors)
    if target is None:
        return errors

    params = target["parameters"]
    memory = target.get("memory")
    require(isinstance(memory, dict), "target.memory must be an object", errors)
    if isinstance(memory, dict):
        require(
            memory.get("model") == "shared_multiport_scratchpad",
            "target.memory.model must be shared_multiport_scratchpad",
            errors,
        )
        require(memory.get("address_unit") == "word", "target.memory.address_unit must be word", errors)
    chunks_per_control = params["control_word_chunks"]
    ctrl_depth = params["ctrl_mem_depth"]
    const_depth = params["const_mem_depth"]
    scratch_depth = params.get("scratchpad_depth")
    require(isinstance(scratch_depth, int) and scratch_depth > 0, "target scratchpad_depth must be positive", errors)
    if not isinstance(scratch_depth, int) or scratch_depth <= 0:
        return errors
    enabled_lsus = target.get("lsu", {}).get("enabled_tiles", [])
    shared_ports = params.get("shared_mem_ports")
    require(isinstance(shared_ports, int) and shared_ports > 0, "target shared_mem_ports must be positive", errors)
    if isinstance(shared_ports, int):
        require(
            len(enabled_lsus) <= shared_ports,
            f"target enables {len(enabled_lsus)} LSUs but has only {shared_ports} shared memory ports",
            errors,
        )

    loop = manifest.get("loop")
    loop_enabled = False
    loop_span = None
    loop_total = None
    if loop is not None:
        require(isinstance(loop, dict), "loop must be an object when present", errors)
        if isinstance(loop, dict):
            missing_loop = sorted(LOOP_KEYS - set(loop))
            extra_loop = sorted(set(loop) - LOOP_KEYS)
            require(not missing_loop, f"loop missing keys: {missing_loop}", errors)
            require(not extra_loop, f"loop has unsupported keys: {extra_loop}", errors)
            enabled = loop.get("enabled")
            require(type(enabled) is bool, "loop.enabled must be a boolean", errors)
            loop_enabled = enabled is True
            for field in ("prologue_cycles", "ii", "trip_count", "epilogue_cycles"):
                value = loop.get(field)
                require(type(value) is int, f"loop.{field} must be an integer", errors)
                if type(value) is int:
                    require(0 <= value <= 0xffff_ffff, f"loop.{field} must fit unsigned 32 bits", errors)
            numeric = all(
                type(loop.get(field)) is int
                for field in ("prologue_cycles", "ii", "trip_count", "epilogue_cycles")
            )
            if numeric:
                loop_span = loop["prologue_cycles"] + loop["ii"] + loop["epilogue_cycles"]
                require(
                    loop_span <= ctrl_depth,
                    f"loop phase span P+II+E exceeds control memory depth: {loop_span} > {ctrl_depth}",
                    errors,
                )
                if loop_enabled:
                    require(loop["ii"] > 0, "loop.ii must be positive when loop.enabled is true", errors)
                    require(loop["trip_count"] > 0, "loop.trip_count must be positive when loop.enabled is true", errors)
                    loop_total = loop_total_cycles(loop)

    run = manifest.get("run")
    require(isinstance(run, dict), "run must be an object", errors)
    if isinstance(run, dict):
        run_cycles = run.get("run_cycles")
        require(isinstance(run_cycles, int), "run.run_cycles must be an integer", errors)
        if isinstance(run_cycles, int):
            if loop_enabled and loop_total is not None:
                require(
                    run_cycles == loop_total,
                    f"run.run_cycles must equal P+trip_count*II+E ({loop_total}) in loop mode",
                    errors,
                )
            else:
                require(
                    0 < run_cycles < ctrl_depth,
                    f"run.run_cycles must be in [1, {ctrl_depth - 1}]",
                    errors,
                )
        observation = run.get("result_observation")
        require(isinstance(observation, dict), "run.result_observation must be an object", errors)
        if isinstance(observation, dict):
            mode = observation.get("mode")
            require(mode in ALLOWED_OBSERVATION_MODES, f"unsupported result_observation mode: {mode!r}", errors)

    program = manifest.get("program")
    require(isinstance(program, dict), "program must be an object", errors)
    if not isinstance(program, dict):
        return errors

    require(program.get("format") == "explicit_tile_images", "program.format must be explicit_tile_images", errors)
    require(program.get("control_word_encoding") == "lsb_first_32bit_chunks", "program.control_word_encoding must be lsb_first_32bit_chunks", errors)
    tiles = program.get("tiles")
    require(isinstance(tiles, list) and tiles, "program.tiles must be a non-empty list", errors)
    if not isinstance(tiles, list):
        return errors

    seen_tiles = set()
    seen_scratch: dict[int, str] = {}
    for index, tile in enumerate(tiles):
        prefix = f"program.tiles[{index}]"
        require(isinstance(tile, dict), f"{prefix} must be an object", errors)
        if not isinstance(tile, dict):
            continue
        missing_tile = sorted(REQUIRED_TILE_KEYS - set(tile))
        require(not missing_tile, f"{prefix} missing keys: {missing_tile}", errors)
        validate_tile_coord(tile, target, prefix, errors)
        coord = (tile.get("row"), tile.get("col"))
        require(coord not in seen_tiles, f"duplicate tile image for row={coord[0]} col={coord[1]}", errors)
        seen_tiles.add(coord)

        control = tile.get("control")
        require(isinstance(control, list) and control, f"{prefix}.control must be a non-empty list", errors)
        if isinstance(control, list):
            seen_pc = set()
            for ctrl_index, entry in enumerate(control):
                eprefix = f"{prefix}.control[{ctrl_index}]"
                require(isinstance(entry, dict), f"{eprefix} must be an object", errors)
                if not isinstance(entry, dict):
                    continue
                pc = entry.get("pc")
                chunks = entry.get("chunks")
                require(isinstance(pc, int), f"{eprefix}.pc must be an integer", errors)
                if isinstance(pc, int):
                    require(0 <= pc < ctrl_depth, f"{eprefix}.pc out of range: {pc}", errors)
                    require(pc not in seen_pc, f"duplicate control pc in {prefix}: {pc}", errors)
                    seen_pc.add(pc)
                require(isinstance(chunks, list), f"{eprefix}.chunks must be a list", errors)
                if isinstance(chunks, list):
                    require(len(chunks) == chunks_per_control, f"{eprefix}.chunks length must be {chunks_per_control}", errors)
                    for chunk_index, chunk in enumerate(chunks):
                        require(is_hex32(chunk), f"{eprefix}.chunks[{chunk_index}] must be 32-bit hex string", errors)
            if loop_enabled and loop_span is not None:
                expected_pc = set(range(loop_span))
                missing_pc = sorted(expected_pc - seen_pc)
                extra_pc = sorted(seen_pc - expected_pc)
                require(not missing_pc, f"{prefix}.control missing loop phase PCs: {missing_pc}", errors)
                require(not extra_pc, f"{prefix}.control has PCs outside loop phase span: {extra_pc}", errors)

        const_memory = tile.get("const_memory")
        require(isinstance(const_memory, list), f"{prefix}.const_memory must be a list", errors)
        if isinstance(const_memory, list):
            seen_const = set()
            for const_index, entry in enumerate(const_memory):
                eprefix = f"{prefix}.const_memory[{const_index}]"
                require(isinstance(entry, dict), f"{eprefix} must be an object", errors)
                if not isinstance(entry, dict):
                    continue
                addr = entry.get("addr")
                value = entry.get("value")
                require(isinstance(addr, int), f"{eprefix}.addr must be an integer", errors)
                if isinstance(addr, int):
                    require(0 <= addr < const_depth, f"{eprefix}.addr out of range: {addr}", errors)
                    require(addr not in seen_const, f"duplicate const addr in {prefix}: {addr}", errors)
                    seen_const.add(addr)
                require(is_hex32(value), f"{eprefix}.value must be 32-bit hex string", errors)

        scratchpad = tile.get("scratchpad_preload")
        require(isinstance(scratchpad, list), f"{prefix}.scratchpad_preload must be a list", errors)
        if isinstance(scratchpad, list):
            for scratch_index, entry in enumerate(scratchpad):
                eprefix = f"{prefix}.scratchpad_preload[{scratch_index}]"
                require(isinstance(entry, dict), f"{eprefix} must be an object", errors)
                if not isinstance(entry, dict):
                    continue
                addr = entry.get("addr")
                value = entry.get("value")
                require(isinstance(addr, int), f"{eprefix}.addr must be an integer", errors)
                if isinstance(addr, int):
                    require(0 <= addr < scratch_depth, f"{eprefix}.addr out of range: {addr}", errors)
                    require(
                        addr not in seen_scratch,
                        f"duplicate global scratchpad preload address {addr}: {seen_scratch.get(addr)} and {eprefix}",
                        errors,
                    )
                    seen_scratch.setdefault(addr, eprefix)
                require(is_hex32(value), f"{eprefix}.value must be 32-bit hex string", errors)

    symbolic = manifest.get("symbolic_dfg")
    if symbolic is not None:
        require(isinstance(symbolic, dict), "symbolic_dfg must be an object when present", errors)
        if isinstance(symbolic, dict):
            require(isinstance(symbolic.get("nodes", []), list), "symbolic_dfg.nodes must be a list", errors)
            require(isinstance(symbolic.get("edges", []), list), "symbolic_dfg.edges must be a list", errors)

    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", help="Program/run manifest JSON path")
    args = parser.parse_args()

    manifest_path = pathlib.Path(args.manifest)
    manifest = load_json(manifest_path)
    errors = validate_program(manifest)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(f"program validation passed: {manifest_path}")
    print(f"schema=cgra.program_manifest.v1 target={manifest['target']['name']} format=explicit_tile_images result_observation=trace_only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
