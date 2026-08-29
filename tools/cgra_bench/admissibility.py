#!/usr/bin/env python3
"""Architecture-admissibility and strict coverage status mapping."""

from __future__ import annotations

from typing import Any


TERMINAL_STATUSES = {
    "FEASIBLE_II",
    "ARCH_UNSUPPORTED_OPERATION",
    "ARCH_UNSUPPORTED_CONTROL",
    "ARCH_UNSUPPORTED_MEMORY",
    "RESOURCE_INFEASIBLE",
    "NOT_ACCELERATION_REGION",
    "COMPILER_BUG",
}

ADMISSIBILITY_STATUSES = {
    "ADMISSIBLE",
    "ARCH_UNSUPPORTED",
    "NOT_ACCELERATION_REGION",
    "UNKNOWN_DUE_TO_COMPILER",
}

RESOURCE_PROOF_KINDS = {
    "ZERO_COMPATIBLE_TILES",
    "UNSUPPORTED_TARGET_TYPE",
    "SCRATCHPAD_CAPACITY_LOWER_BOUND",
    "CONTROL_MEMORY_LOWER_BOUND",
    "PROVEN_RF_PRESSURE_LOWER_BOUND",
    "EXACT_SEARCH_EXHAUSTED_WITHOUT_BUDGET",
}


def _backend(result: dict[str, Any]) -> dict[str, Any]:
    value = result.get("backend")
    return value if isinstance(value, dict) else {}


def _has_resource_proof(result: dict[str, Any]) -> bool:
    backend = _backend(result)
    for witness in (result.get("witness"), backend.get("resource_proof"), backend.get("witness")):
        if isinstance(witness, dict) and witness.get("proof_kind") in RESOURCE_PROOF_KINDS:
            return True
    return False


def terminal_status(result: dict[str, Any]) -> str:
    """Derive a conservative terminal status from structured case evidence.

    This function never upgrades a route-only candidate to a success. Resource
    infeasibility is reported only when the backend supplied a concrete RF or
    stage rejection witness; budget and timeout remain compiler/harness defects
    for a strict coverage run.
    """
    if result.get("excluded") or result.get("status") == "EXCLUDED":
        return "NOT_ACCELERATION_REGION"
    backend = _backend(result)
    if result.get("status") == "PASS":
        if backend.get("rf_constrained_mapping_found") or result.get("tier") in {
            "RF_CONSTRAINED_MAPPED",
            "RF_COMPLETE",
            "MANIFEST_COMPLETE",
            "FUNCTIONAL_RTL_VALIDATED",
        }:
            return "FEASIBLE_II"
        if result.get("tier") == "ROUTE_MAPPED":
            if _has_resource_proof(result):
                return "RESOURCE_INFEASIBLE"
            return "COMPILER_BUG"
    category = str(result.get("category", ""))
    code = str(result.get("diagnostic_code", "")).upper()
    message = str(result.get("message", "")).lower()
    if category == "TIMEOUT" or category == "INTERNAL" or result.get("owner") == "UNKNOWN":
        return "COMPILER_BUG"
    if category == "MAPPING_BUDGET" or "budget" in code or "timed out" in message:
        return "COMPILER_BUG"
    if category in {"RF", "STAGE_SCHEDULE", "MAPPING_INFEASIBLE", "MAPPING_VERIFY"}:
        if _has_resource_proof(result):
            return "RESOURCE_INFEASIBLE"
        return "COMPILER_BUG"
    if "PREDICATED_LOAD" in code or "MEMORY" in code or "ADDRESS" in code or "POINTER" in code:
        return "ARCH_UNSUPPORTED_MEMORY"
    if "CONTROL" in code or "BRANCH" in code or "LOOP_SHAPE" in code or "PREDICATION" in code:
        return "ARCH_UNSUPPORTED_CONTROL"
    if "TYPE" in code or "OPCODE" in code or "OPERATION" in code or category == "TARGET_ISA":
        return "ARCH_UNSUPPORTED_OPERATION"
    # A source/toolchain build failure is neither an architecture capability
    # result nor a successful audit outcome.  Keep it explicit as a compiler
    # defect/environment failure so it cannot inflate architecture coverage.
    if category in {"BUILD", "LLVM"}:
        return "COMPILER_BUG"
    if category in {"LOOP_SELECTION", "FRONTEND", "GENERIC_IR", "INVOCATION", "ABI"}:
        if "MEMORY" in code or "ADDRESS" in code or "POINTER" in code:
            return "ARCH_UNSUPPORTED_MEMORY"
        if "CONTROL" in code or "LOOP" in code or "BRANCH" in code:
            return "ARCH_UNSUPPORTED_CONTROL"
        return "COMPILER_BUG" if category in {"GENERIC_IR", "INTERNAL"} else "ARCH_UNSUPPORTED_OPERATION"
    return "COMPILER_BUG"


def admissibility_status(result: dict[str, Any]) -> str:
    """Classify architecture admissibility independently of terminal outcome."""
    if result.get("excluded") or result.get("status") == "EXCLUDED":
        return "NOT_ACCELERATION_REGION"
    status = result.get("terminal_status") or terminal_status(result)
    if status in {"FEASIBLE_II", "RESOURCE_INFEASIBLE"}:
        return "ADMISSIBLE"
    if status in {"ARCH_UNSUPPORTED_OPERATION", "ARCH_UNSUPPORTED_CONTROL",
                  "ARCH_UNSUPPORTED_MEMORY"}:
        return "ARCH_UNSUPPORTED"
    return "UNKNOWN_DUE_TO_COMPILER"


def attach_terminal_status(result: dict[str, Any]) -> dict[str, Any]:
    status = terminal_status(result)
    result["terminal_status"] = status
    result["admissibility_status"] = admissibility_status(result)
    backend = _backend(result)
    stats = backend.get("stats", {}) if isinstance(backend, dict) else {}
    if status == "FEASIBLE_II":
        result["feasible_ii"] = backend.get("stats", {}).get("safe_ii", backend.get("stats", {}).get("mapped_ii"))
    elif status == "RESOURCE_INFEASIBLE":
        result.setdefault("witness", {})
        if result.get("message"):
            result["witness"].setdefault("diagnostic", result["message"][-2000:])
        if not _has_resource_proof(result):
            result["terminal_status"] = "COMPILER_BUG"
            result["admissibility_status"] = "UNKNOWN_DUE_TO_COMPILER"
    return result
