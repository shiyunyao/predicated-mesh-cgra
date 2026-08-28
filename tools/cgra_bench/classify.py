#!/usr/bin/env python3
"""Map production compiler results to the T019 terminal taxonomy."""

from __future__ import annotations

import re
from typing import Any


RULES = [
    ("LLVM_FRONTEND_", "FRONTEND", "FRONTEND"),
    ("LLVM_FRONTEND_", "LLVM", "FRONTEND"),
    ("unsupported_loop", "LOOP_SELECTION", "FRONTEND"),
    ("function not found", "LOOP_SELECTION", "FRONTEND"),
    ("invalid invocation", "INVOCATION", "ABI"),
    ("abi_", "ABI", "ABI"),
    ("invalid_source_dfg", "GENERIC_IR", "FRONTEND"),
    ("generic_dfg", "GENERIC_IR", "FRONTEND"),
    ("target_legalization", "TARGET_ISA", "ISA"),
    ("unsupported operation", "TARGET_ISA", "ISA"),
    ("mii", "MII", "MAPPER"),
    ("budget", "MAPPING_BUDGET", "MAPPER"),
    ("mapping", "MAPPING_INFEASIBLE", "MAPPER"),
    ("stage", "STAGE_SCHEDULE", "SCHEDULER"),
    ("rf", "RF", "RF"),
    ("material", "MATERIALIZATION", "LOWERING"),
    ("lower", "TARGET_LOWERING", "LOWERING"),
    ("manifest", "MANIFEST", "LOWERING"),
]


def classify(stage: str, message: str, returncode: int = 1) -> dict[str, str]:
    text = f"{stage} {message}".lower()
    if returncode == 124 or "timed out" in text or "timeout" in text:
        return {"category": "TIMEOUT", "owner": "HARNESS", "diagnostic_code": "STAGE_TIMEOUT"}
    if "mapping" in text and "verif" in text:
        return {"category": "MAPPING_VERIFY", "owner": "MAPPER", "diagnostic_code": "MAPPING_VERIFICATION_FAILED"}
    diagnostic = re.search(r"\b(?:LLVM_FRONTEND|ABI|RFA|TARGET|MAPPING|MAP|RF|MEMORY|GENERIC|MII|STAGE|MANIFEST)_[A-Z0-9_]+\b", message)
    if diagnostic:
        code = diagnostic.group(0)
        if code.startswith("LLVM_FRONTEND_"):
            return {"category": "FRONTEND", "owner": "FRONTEND", "diagnostic_code": code}
        if code.startswith("ABI_"):
            return {"category": "ABI", "owner": "ABI", "diagnostic_code": code}
        if code.startswith("RFA_") or code.startswith("RF_"):
            return {"category": "RF", "owner": "RF", "diagnostic_code": code}
        if code.startswith("MAPPING_") or code.startswith("MAP_"):
            limited = code == "MAP_NO_MAPPING_WITHIN_II_LIMIT"
            category = "MAPPING_BUDGET" if "BUDGET" in code or limited else "MAPPING_INFEASIBLE"
            return {"category": category, "owner": "MAPPER", "diagnostic_code": code}
        if code.startswith("TARGET_"):
            return {"category": "TARGET_ISA", "owner": "ISA", "diagnostic_code": code}
    stage_defaults = {
        "S0": ("CORPUS", "HARNESS", "CORPUS_AUDIT"),
        "S1": ("BUILD", "HARNESS", "SOURCE_BUILD_FAILED"),
        "S2": ("LLVM", "HARNESS", "LLVM_CANONICALIZE_FAILED"),
        "S3": ("LOOP_SELECTION", "FRONTEND", "LOOP_SELECTION_FAILED"),
        "S4": ("FRONTEND", "FRONTEND", "FRONTEND_LOWERING_FAILED"),
        "S5": ("GENERIC_IR", "FRONTEND", "GENERIC_DFG_VERIFICATION_FAILED"),
        "S6": ("INVOCATION", "ABI", "INVOCATION_SYNTHESIS_FAILED"),
        "S7": ("ABI", "ABI", "ABI_BINDING_FAILED"),
        "S8": ("TARGET_ISA", "ISA", "TARGET_LEGALIZATION_FAILED"),
        "S9": ("MII", "MAPPER", "MII_ANALYSIS_FAILED"),
        "S10": ("MAPPING_INFEASIBLE", "MAPPER", "MAPPING_FAILED"),
        "S11": ("STAGE_SCHEDULE", "SCHEDULER", "STAGE_SCHEDULING_FAILED"),
        "S12": ("RF", "RF", "RF_ALLOCATION_FAILED"),
        "S13": ("MATERIALIZATION", "LOWERING", "MATERIALIZATION_FAILED"),
        "S14": ("TARGET_LOWERING", "LOWERING", "TARGET_LOWERING_FAILED"),
        "S15": ("MANIFEST", "LOWERING", "MANIFEST_FAILED"),
        "INTERNAL": ("INTERNAL", "HARNESS", "HARNESS_EXCEPTION"),
    }
    prefix = stage.split("_", 1)[0]
    if prefix in stage_defaults:
        category, owner, code = stage_defaults[prefix]
        if prefix == "S10" and "budget" in text:
            category, code = "MAPPING_BUDGET", "MAPPING_BUDGET_EXCEEDED"
        return {"category": category, "owner": owner, "diagnostic_code": code}
    for needle, category, owner in RULES:
        if needle.lower() in text:
            code = re.search(r"[A-Z][A-Z0-9_]{4,}", message)
            return {"category": category, "owner": owner, "diagnostic_code": code.group(0) if code else needle.upper().replace(" ", "_")}
    if returncode < 0:
        return {"category": "INTERNAL", "owner": "HARNESS", "diagnostic_code": "PROCESS_SIGNAL"}
    return {"category": "INTERNAL", "owner": "UNKNOWN", "diagnostic_code": "UNCLASSIFIED_RESULT"}
