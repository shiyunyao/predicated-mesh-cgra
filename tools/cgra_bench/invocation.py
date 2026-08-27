#!/usr/bin/env python3
"""Deterministic synthetic invocation synthesis for structural audits."""

from __future__ import annotations

from typing import Any


class InvocationSynthesisError(ValueError):
    """The audit cannot safely assign a deterministic invocation."""


def address_external_ids(dfg: dict[str, Any], memory_analysis: dict[str, Any] | None = None) -> set[int]:
    """Find externals in the dataflow cone of every memory address operand."""
    nodes = {node["id"]: node for node in dfg.get("nodes", [])}
    incoming: dict[int, list[int]] = {}
    for edge in dfg.get("edges", []):
        if edge.get("kind") == "data":
            incoming.setdefault(edge["dst"], []).append(edge["src"])
    bindings: dict[int, set[int]] = {}
    for binding in dfg.get("external_bindings", []):
        if "external" in binding:
            bindings.setdefault(binding["node"], set()).add(binding["external"])

    if memory_analysis:
        base_names = {
            str(access.get("base", "")).lstrip("%")
            for access in memory_analysis.get("accesses", [])
            if access.get("base")
        }
        by_name = {
            value.get("name"): value.get("id")
            for value in dfg.get("external_values", [])
        }
        named = {by_name[name] for name in base_names if name in by_name}
        if named:
            return named

    roots = {
        edge["src"]
        for edge in dfg.get("edges", [])
        if edge.get("kind") == "data"
        and edge.get("operand") == 0
        and nodes.get(edge.get("dst"), {}).get("opcode") in {"Load", "Store"}
    }
    found = {
        binding["external"]
        for binding in dfg.get("external_bindings", [])
        if "external" in binding
        and binding.get("operand") == 0
        and nodes.get(binding.get("node"), {}).get("opcode") in {"Load", "Store"}
    }
    pending = list(roots)
    visited: set[int] = set()
    while pending:
        node_id = pending.pop()
        if node_id in visited:
            continue
        visited.add(node_id)
        found.update(bindings.get(node_id, set()))
        pending.extend(incoming.get(node_id, []))
    return found


def synthesize_invocation(
    dfg: dict[str, Any],
    trip_count: int,
    memory_analysis: dict[str, Any] | None = None,
    scratchpad_depth: int | None = None,
) -> dict[str, Any]:
    pointer_ids = address_external_ids(dfg, memory_analysis)
    if len(pointer_ids) > 16:
        raise InvocationSynthesisError("more than 16 scratchpad pointer windows are required")
    scalar_inputs = {}
    pointer_index = 0
    scalar_index = 0
    for value in sorted(dfg.get("external_values", []), key=lambda item: item["id"]):
        value_type = value.get("type", {})
        if value["id"] in pointer_ids:
            bits = pointer_index * 256
            pointer_index += 1
        elif value_type.get("kind") == "predicate" or value_type.get("bits") == 1:
            bits = scalar_index % 2
            scalar_index += 1
        else:
            bits = 0x101 + scalar_index
            scalar_index += 1
        scalar_inputs[value["name"]] = f"0x{bits:08x}"
    if scratchpad_depth is not None:
        if any(int(value, 16) >= scratchpad_depth for name, value in scalar_inputs.items()
               if next((entry["id"] for entry in dfg.get("external_values", [])
                        if entry.get("name") == name), None) in pointer_ids):
            raise InvocationSynthesisError("synthetic scratchpad base is outside target depth")
        if memory_analysis:
            for access in memory_analysis.get("accesses", []):
                base_id = next((entry["id"] for entry in dfg.get("external_values", [])
                                if entry.get("name") == str(access.get("base", "")).lstrip("%")), None)
                base = int(scalar_inputs.get(str(access.get("base", "")).lstrip("%"), "0"), 16)
                offset = int(access.get("offset_words", 0))
                stride = int(access.get("stride_words", 0))
                endpoints = [base + offset, base + offset + stride * max(trip_count - 1, 0)]
                if base_id in pointer_ids and (min(endpoints) < 0 or max(endpoints) >= scratchpad_depth):
                    raise InvocationSynthesisError("synthetic scratchpad access exceeds target depth")
    return {
        "schema": "cgra.kernel_invocation.v1",
        "trip_count": trip_count,
        "scalar_inputs": scalar_inputs,
        "scratchpad_preload": [],
        "synthetic": True,
    }
