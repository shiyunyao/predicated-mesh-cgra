#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared FN-CACTI paths, source snapshots, and inventory validation."""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import subprocess
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
REPORT_ROOT = REPO_ROOT / "reports/synthesis/fn_cacti"
FNC_ROOT = REPO_ROOT / "third_party/fncacti"
FNC_SOURCE = FNC_ROOT
FNC_EXECUTABLE = FNC_ROOT / "cacti"
INVENTORY_PATH = REPO_ROOT / "synth/fn_cacti/storage_inventory.json"
TARGET_PATH = REPO_ROOT / "synth/fn_cacti/cgra_target.json"
FILELIST_PATH = REPO_ROOT / "synth/rtl_files.f"
RTL_PATHS = {
    "control_mem": REPO_ROOT / "rtl/control_mem_bank.sv",
    "data_rf": REPO_ROOT / "rtl/data_rf.sv",
    "predicate_rf": REPO_ROOT / "rtl/pred_rf.sv",
    "const_mem": REPO_ROOT / "rtl/tile.sv",
    "scratchpad": REPO_ROOT / "rtl/scratchpad_bank.sv",
    "lsu": REPO_ROOT / "rtl/lsu.sv",
}
WORKFLOW_PATHS = (
    REPO_ROOT / "scripts/fn_cacti_common.py",
    REPO_ROOT / "scripts/run_fn_cacti.py",
    REPO_ROOT / "scripts/check_fn_cacti.py",
    REPO_ROOT / "scripts/run_synth_area.py",
    REPO_ROOT / "Makefile",
)
TARGETS = {
    "small": {"rows": 2, "cols": 2},
    "default": {"rows": 4, "cols": 4},
}
CLASS_ORDER = (
    "control_mem",
    "data_rf",
    "predicate_rf",
    "const_mem",
    "scratchpad",
)
EXPECTED_CLASSES = {
    "control_mem": {
        "module": "control_mem_bank",
        "target_depth": "ctrl_mem_depth",
        "target_width": "physical_control_word_width_bits",
        "read_ports": 1,
        "write_ports": 1,
        "read_mode": "asynchronous",
        "write_mode": "synchronous",
        "rtl_markers": ("module control_mem_bank", "assign read_data = mem[read_addr]"),
    },
    "data_rf": {
        "module": "data_rf",
        "target_depth": "data_rf_depth",
        "target_width": "data_width",
        "read_ports": 2,
        "write_ports": 2,
        "read_mode": "asynchronous",
        "write_mode": "synchronous",
        "rtl_markers": ("module data_rf", "assign rdata_a = mem[raddr_a]", "assign rdata_b = mem[raddr_b]"),
    },
    "predicate_rf": {
        "module": "pred_rf",
        "target_depth": "pred_rf_depth",
        "target_width": "pred_width",
        "read_ports": 2,
        "write_ports": 2,
        "read_mode": "asynchronous",
        "write_mode": "synchronous",
        "rtl_markers": ("module pred_rf", "assign rdata_a = mem[raddr_a]", "assign rdata_b = mem[raddr_b]"),
    },
    "const_mem": {
        "module": "tile.const_mem",
        "target_depth": "const_mem_depth",
        "target_width": "data_width",
        "read_ports": 1,
        "write_ports": 1,
        "read_mode": "asynchronous",
        "write_mode": "synchronous",
        "rtl_markers": ("logic [DATA_WIDTH-1:0] const_mem", "assign const_data = const_mem"),
    },
    "scratchpad": {
        "module": "scratchpad_bank",
        "target_depth": "scratch_bank_depth",
        "target_width": "data_width",
        "read_ports": 1,
        "write_ports": 2,
        "read_mode": "asynchronous",
        "write_mode": "synchronous",
        "rtl_markers": ("module scratchpad_bank", "assign read_data = mem[read_addr]", "if (write_en)", "if (cfg_write_en)"),
    },
}
REQUIRED_FNC_SOURCES = (
    "area.cc",
    "bank.cc",
    "mat.cc",
    "main.cc",
    "Ucache.cc",
    "io.cc",
    "technology.cc",
    "basic_circuit.cc",
    "parameter.cc",
    "decoder.cc",
    "component.cc",
    "uca.cc",
    "subarray.cc",
    "wire.cc",
    "htree2.cc",
    "cacti_interface.cc",
    "router.cc",
    "nuca.cc",
    "crossbar.cc",
    "arbiter.cc",
)
LIVE_PCACTI_PATHS = (
    FNC_ROOT / "pcacti.tgz",
    FNC_ROOT / "pcacti.tar",
    FNC_ROOT / "pcacti_xml",
    FNC_ROOT / "pcacti",
)


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relative(path: pathlib.Path) -> str:
    return path.resolve().relative_to(REPO_ROOT.resolve()).as_posix()


def artifact(path: pathlib.Path) -> dict[str, Any]:
    return {
        "path": relative(path),
        "sha256": sha256(path),
        "size_bytes": path.stat().st_size,
        "mode_octal": format(path.stat().st_mode & 0o777, "03o"),
    }


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def load_json(path: pathlib.Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{relative(path)} must contain a JSON object")
    return value


def git_revision() -> dict[str, Any]:
    if not (FNC_ROOT / ".git").exists():
        return {"available": False, "revision": None, "status": None}
    revision = subprocess.run(
        ["git", "-C", str(FNC_ROOT), "rev-parse", "HEAD"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    status = subprocess.run(
        ["git", "-C", str(FNC_ROOT), "status", "--short"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return {
        "available": revision.returncode == 0,
        "revision": revision.stdout.strip() if revision.returncode == 0 else None,
        "status": status.stdout.splitlines() if status.returncode == 0 else None,
    }


def source_snapshot() -> dict[str, str | None]:
    paths = [INVENTORY_PATH, TARGET_PATH, FILELIST_PATH, *RTL_PATHS.values(), *WORKFLOW_PATHS]
    return {relative(path): sha256(path) if path.is_file() else None for path in paths}


def source_tree_sha256() -> dict[str, Any]:
    files = (
        sorted(
            path
            for path in FNC_SOURCE.rglob("*")
            if path.is_file()
            and (
                path.suffix in {".cc", ".h"}
                or path.name in {"makefile", "cacti.mk", "cache.cfg"}
                or path.suffix == ".cfg"
            )
        )
        if FNC_SOURCE.is_dir()
        else []
    )
    digest = hashlib.sha256()
    for path in files:
        digest.update(relative(path).encode("utf-8"))
        digest.update(b"\0")
        digest.update(sha256(path).encode("ascii"))
        digest.update(b"\n")
    return {"file_count": len(files), "sha256": digest.hexdigest() if files else None}


def fn_cacti_snapshot() -> dict[str, Any]:
    tracked = [
        FNC_ROOT / "README",
        FNC_ROOT / "makefile",
        FNC_ROOT / "cacti.mk",
        FNC_ROOT / "configs/2MB_fin.cfg",
        FNC_ROOT / "configs/config.cfg",
    ]
    return {
        "root": relative(FNC_ROOT) if FNC_ROOT.exists() else "third_party/fncacti",
        "source_dir": relative(FNC_SOURCE) if FNC_SOURCE.exists() else "third_party/fncacti",
        "git": git_revision(),
        "upstream_lineage": {
            "repository": "https://github.com/marg-tools/FN-CACTI.git",
            "declared_revision": "f37debd04a5b1ac15470b8fdb6d32115e727e7b2",
            "verification": "pinned upstream reference; the user-supplied standalone snapshot has no nested Git revision and is not claimed byte-equivalent",
        },
        "tracked_artifacts": {
            relative(path): artifact(path) if path.is_file() else None for path in tracked
        },
        "executable": artifact(FNC_EXECUTABLE) if FNC_EXECUTABLE.is_file() else None,
        "required_source_missing": [
            name for name in REQUIRED_FNC_SOURCES if not (FNC_SOURCE / name).is_file()
        ],
        "live_pcacti_paths": [relative(path) for path in LIVE_PCACTI_PATHS if path.exists()],
        "source_tree": source_tree_sha256(),
        "license_status": {
            "root_license_files": [
                relative(path)
                for path in (FNC_ROOT / "LICENSE", FNC_ROOT / "COPYING", FNC_ROOT / "NOTICE")
                if path.is_file()
            ],
            "source_notice": (
                "This RTL-focused repository vendors the supplied FN-CACTI executable and "
                "upstream README, not the incomplete source snapshot from the parent project"
            ),
        },
    }


def load_inventory() -> dict[str, Any]:
    return load_json(INVENTORY_PATH)


def expected_instance_count(target: str, name: str, target_doc: dict[str, Any]) -> int:
    if target not in TARGETS:
        raise ValueError(f"unknown target {target}")
    tiles = TARGETS[target]["rows"] * TARGETS[target]["cols"]
    if name != "scratchpad":
        return tiles
    lsu = target_doc.get("lsu")
    enabled = lsu.get("enabled_tiles") if isinstance(lsu, dict) else None
    if not isinstance(enabled, list):
        raise ValueError("target LSU enabled_tiles must be a list")
    if target == "default":
        return len(enabled)
    return TARGETS[target]["rows"]


def validate_inventory() -> tuple[list[str], dict[str, Any]]:
    errors: list[str] = []
    details: dict[str, Any] = {"classes": {}, "source_snapshot": source_snapshot()}
    try:
        inventory = load_inventory()
        target_doc = load_json(TARGET_PATH)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        return [f"cannot load inventory/target input: {error}"], details
    if inventory.get("schema") != "cgra.storage_inventory.v1":
        errors.append("storage inventory schema mismatch")
    if target_doc.get("schema") != "cgra.fn_cacti_target.v1":
        errors.append("FN-CACTI target schema mismatch")
    if inventory.get("technology") != "ASAP7":
        errors.append("storage inventory technology must be ASAP7")
    parameters = target_doc.get("parameters")
    if not isinstance(parameters, dict):
        return errors + ["target parameters are missing"], details
    entries = inventory.get("classes")
    if not isinstance(entries, list):
        return errors + ["storage inventory classes must be a list"], details
    by_name = {
        item.get("name"): item
        for item in entries
        if isinstance(item, dict) and isinstance(item.get("name"), str)
    }
    if tuple(by_name) != CLASS_ORDER or len(by_name) != len(entries):
        errors.append(f"storage class set/order mismatch: got {list(by_name)}")
    for name in CLASS_ORDER:
        spec = EXPECTED_CLASSES[name]
        item = by_name.get(name)
        if not isinstance(item, dict):
            errors.append(f"missing storage class {name}")
            continue
        expected_depth = parameters.get(spec["target_depth"])
        expected_width = parameters.get(spec["target_width"])
        observed = {
            "depth": item.get("depth"),
            "width_bits": item.get("width_bits"),
            "read_ports": item.get("read_ports"),
            "write_ports": item.get("write_ports"),
            "read_mode": item.get("read_mode"),
            "write_mode": item.get("write_mode"),
            "module": item.get("module"),
        }
        expected = {
            "depth": expected_depth,
            "width_bits": expected_width,
            "read_ports": spec["read_ports"],
            "write_ports": spec["write_ports"],
            "read_mode": spec["read_mode"],
            "write_mode": spec["write_mode"],
            "module": spec["module"],
        }
        if observed != expected:
            errors.append(f"{name}: inventory/target semantic mismatch")
        instances = item.get("instances")
        instance_errors = []
        if not isinstance(instances, dict):
            instance_errors.append("instances is not an object")
        else:
            for target in TARGETS:
                try:
                    expected_count = expected_instance_count(target, name, target_doc)
                except ValueError as error:
                    errors.append(str(error))
                    continue
                if instances.get(target) != expected_count:
                    instance_errors.append(
                        f"{target} count {instances.get(target)!r} != {expected_count}"
                    )
        rtl_path = RTL_PATHS[name]
        text = rtl_path.read_text(encoding="utf-8") if rtl_path.is_file() else ""
        missing_markers = [marker for marker in spec["rtl_markers"] if marker not in text]
        if name == "scratchpad":
            lsu_text = RTL_PATHS["lsu"].read_text(encoding="utf-8") if RTL_PATHS["lsu"].is_file() else ""
            if "scratchpad_bank #(" not in lsu_text:
                missing_markers.append("lsu.sv: scratchpad_bank #(")
        if missing_markers:
            errors.append(f"{name}: RTL markers missing: {missing_markers}")
        details["classes"][name] = {
            "expected": expected,
            "observed": observed,
            "instances": instances,
            "instance_errors": instance_errors,
            "rtl_path": relative(rtl_path),
            "rtl_sha256": sha256(rtl_path) if rtl_path.is_file() else None,
            "rtl_markers": list(spec["rtl_markers"]),
            "missing_rtl_markers": missing_markers,
        }
        errors.extend(f"{name}: {error}" for error in instance_errors)
    return errors, details


def parse_report(path: pathlib.Path) -> dict[str, float]:
    text = path.read_text(encoding="utf-8", errors="replace")
    patterns = {
        "access_time_ns": r"Access time\s*:\s*([0-9.eE+-]+)\s*ns",
        "cycle_time_ns": r"Cycle time\s*:\s*([0-9.eE+-]+)\s*ns",
        "read_energy_nj": r"Total dynamic read energy per access\s*:\s*([0-9.eE+-]+)\s*nJ",
        "write_energy_nj": r"Total dynamic write energy per access\s*:\s*([0-9.eE+-]+)\s*nJ",
        "leakage_mw": r"Total leakage power of a bank\s*:\s*([0-9.eE+-]+)\s*mW",
        "area_mm2": r"Cache area\s*:\s*([0-9.eE+-]+)\s*mm2",
    }
    result: dict[str, float] = {}
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        if match is None:
            raise ValueError(f"FN-CACTI report lacks {key}")
        result[key] = float(match.group(1))
    if any(value <= 0.0 for value in result.values()):
        raise ValueError("FN-CACTI report contains non-positive PPA metric")
    return result


def normalized_metrics(raw: dict[str, float]) -> dict[str, float]:
    return {
        "area_mm2": raw["area_mm2"],
        "area_um2": raw["area_mm2"] * 1_000_000.0,
        "read_energy_pj": raw["read_energy_nj"] * 1_000.0,
        "write_energy_pj": raw["write_energy_nj"] * 1_000.0,
        "leakage_mw": raw["leakage_mw"],
        "access_time_ns": raw["access_time_ns"],
        "cycle_time_ns": raw["cycle_time_ns"],
    }
