#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail-closed validation for FN-CACTI evidence."""

from __future__ import annotations

import argparse
import copy
import json
import pathlib
import sys
from typing import Any

import run_fn_cacti as flow
import fn_cacti_common as common


def load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected an object")
    return value


def resolve_artifact(record: Any, expected_path: str, label: str, errors: list[str]) -> pathlib.Path | None:
    if not isinstance(record, dict):
        errors.append(f"{label}: artifact record is missing")
        return None
    if record.get("path") != expected_path:
        errors.append(f"{label}: path is not canonical")
        return None
    path = common.REPO_ROOT / expected_path
    if not path.is_file() or path.stat().st_size == 0:
        errors.append(f"{label}: artifact is missing or empty")
        return None
    if record.get("size_bytes") != path.stat().st_size:
        errors.append(f"{label}: size mismatch")
    if record.get("sha256") != common.sha256(path):
        errors.append(f"{label}: hash mismatch")
    if record.get("mode_octal") != format(path.stat().st_mode & 0o777, "03o"):
        errors.append(f"{label}: mode mismatch")
    return path


def expected_root(run_kind: str) -> pathlib.Path:
    return common.REPORT_ROOT / run_kind


def validate_common(record: dict[str, Any], run_kind: str) -> list[str]:
    errors: list[str] = []
    if record.get("schema") != "cgra.fn_cacti.v1":
        errors.append("unexpected manifest schema")
    if record.get("run_kind") != run_kind:
        errors.append("manifest run kind mismatch")
    if record.get("pcacti_used") is not False:
        errors.append("manifest must record pcacti_used false")
    if record.get("source_snapshot") != common.source_snapshot():
        errors.append("input source snapshot is stale")
    if record.get("fn_cacti") != common.fn_cacti_snapshot():
        errors.append("FN-CACTI source snapshot is stale")
    if record.get("whole_design_power_status") != "TOTAL_POWER_NOT_COMBINABLE":
        errors.append("whole-design power status is unsafe")
    if record.get("model_basis") != flow.model_basis():
        errors.append("model technology basis is stale or unsafe")
    preflight_path = expected_root(run_kind) / "standalone_preflight.json"
    resolve_artifact(
        record.get("standalone_preflight"),
        common.relative(preflight_path),
        "standalone preflight",
        errors,
    )
    resolve_artifact(
        record.get("summary"),
        common.relative(expected_root(run_kind) / "memory_area_power_summary.md"),
        "summary",
        errors,
    )
    return errors


def validate_blocked(record: dict[str, Any], run_kind: str) -> list[str]:
    errors = validate_common(record, run_kind)
    if record.get("status") != "BLOCKED_PREREQUISITE":
        errors.append("blocked manifest has wrong status")
    if record.get("targets") != []:
        errors.append("blocked manifest must not contain model targets")
    blockers = record.get("blockers")
    if not isinstance(blockers, list) or not blockers:
        errors.append("blocked manifest has no blockers")
        return errors
    preflight_path = expected_root(run_kind) / "standalone_preflight.json"
    try:
        preflight = load_json(preflight_path)
    except ValueError as error:
        return errors + [str(error)]
    if preflight.get("pcacti_used") is not False:
        errors.append("preflight incorrectly reports pCACTI use")
    if preflight.get("forbidden_commands") != ["fncacti_patch.sh", "pcacti.tgz", "pcacti.tar"]:
        errors.append("preflight forbidden-command policy is unsafe")
    build = preflight.get("build")
    if not isinstance(build, dict):
        errors.append("preflight build record is missing")
    else:
        command = build.get("command")
        if command is not None and (
            not isinstance(command, list)
            or any("pcacti" in str(part).lower() or "fncacti_patch" in str(part).lower() for part in command)
        ):
            errors.append("preflight build command introduces pCACTI")
    if preflight.get("fn_cacti") != common.fn_cacti_snapshot():
        errors.append("preflight FN-CACTI snapshot is stale")
    if preflight.get("inventory_validation", {}).get("errors"):
        errors.append("inventory validation must pass before standalone preflight")
    if preflight.get("blockers") != blockers:
        errors.append("manifest/preflight blocker lists differ")
    observed_codes = {item.get("code") for item in blockers if isinstance(item, dict)}
    allowed_codes = {
        "FNC_ACTI_ROOT_MISSING",
        "PCACTI_LIVE_DEPENDENCY_PRESENT",
        "FNC_ACTI_INCOMPLETE_STANDALONE_SNAPSHOT",
        "FNC_ACTI_STANDALONE_BUILD_FAILED",
        "FNC_ACTI_EXECUTABLE_PERMISSION_MISSING",
    }
    if not observed_codes or not observed_codes <= allowed_codes:
        errors.append("blocked manifest has unknown blocker code")
    if common.fn_cacti_snapshot()["live_pcacti_paths"]:
        if "PCACTI_LIVE_DEPENDENCY_PRESENT" not in observed_codes:
            errors.append("live pCACTI dependency is not reported")
    elif "PCACTI_LIVE_DEPENDENCY_PRESENT" in observed_codes:
        errors.append("manifest falsely reports a live pCACTI dependency")
    if common.fn_cacti_snapshot()["required_source_missing"] and not common.FNC_EXECUTABLE.is_file():
        if "FNC_ACTI_INCOMPLETE_STANDALONE_SNAPSHOT" not in observed_codes:
            errors.append("missing standalone source files are not reported")
    return errors


def validate_modeled_class(
    item: Any, target: str, root: pathlib.Path, errors: list[str]
) -> dict[str, Any] | None:
    if not isinstance(item, dict):
        errors.append(f"{target}: class record is malformed")
        return None
    name = item.get("name")
    if name not in common.CLASS_ORDER:
        errors.append(f"{target}: unknown class {name!r}")
        return None
    expected = next(entry for entry in common.load_inventory()["classes"] if entry["name"] == name)
    if item.get("model_status") != flow.class_model_status(expected):
        errors.append(f"{target}/{name}: unsupported model status")
    inventory = item.get("inventory")
    if inventory != expected:
        errors.append(f"{target}/{name}: inventory record drift")
    mapping = item.get("parameter_mapping")
    if not isinstance(mapping, dict):
        errors.append(f"{target}/{name}: parameter mapping is missing")
    else:
        expected_mapping = flow.storage_mapping(expected)
        if mapping != expected_mapping:
            errors.append(f"{target}/{name}: parameter mapping drift")
    class_root = root / target / name
    config_path = class_root / f"{name}.cfg"
    log_path = class_root / "fn_cacti.log"
    report_path = class_root / "pcacti_report.txt"
    result_path = class_root / "result.json"
    resolved_config = resolve_artifact(item.get("config"), common.relative(config_path), f"{target}/{name} config", errors)
    resolve_artifact(item.get("raw_log"), common.relative(log_path), f"{target}/{name} log", errors)
    resolved_report = resolve_artifact(item.get("raw_report"), common.relative(report_path), f"{target}/{name} report", errors)
    resolve_artifact(item.get("result"), common.relative(result_path), f"{target}/{name} result", errors)
    try:
        stored_result = load_json(result_path)
        expected_result = copy.deepcopy(item)
        expected_result.pop("result", None)
        if stored_result != expected_result:
            errors.append(f"{target}/{name}: result manifest content mismatch")
    except ValueError as error:
        errors.append(f"{target}/{name}: {error}")
    if item.get("command") != [str(common.FNC_EXECUTABLE), "-infile", config_path.name]:
        errors.append(f"{target}/{name}: command is not canonical")
    if item.get("returncode") != 0:
        errors.append(f"{target}/{name}: FN-CACTI return code is not zero")
    if resolved_config is not None and resolved_config.read_text(encoding="utf-8") != flow.config_text(expected):
        errors.append(f"{target}/{name}: configuration content mismatch")
    if resolved_report is not None:
        try:
            raw = common.parse_report(resolved_report)
            if item.get("raw_metrics") != raw:
                errors.append(f"{target}/{name}: raw metrics mismatch")
            normalized = common.normalized_metrics(raw)
            if item.get("normalized_metrics") != normalized:
                errors.append(f"{target}/{name}: normalized metrics mismatch")
            expected_scenario = flow.scenario(normalized, expected["instances"][target], expected)
            if item.get("power_scenarios") != expected_scenario:
                errors.append(f"{target}/{name}: scenario totals/formula mismatch")
        except ValueError as error:
            errors.append(f"{target}/{name}: {error}")
    return item


def validate_proxy(record: dict[str, Any], run_kind: str) -> list[str]:
    errors = validate_common(record, run_kind)
    if record.get("status") != flow.PROXY_STATUS:
        errors.append("proxy manifest has wrong status")
    if record.get("blockers") != []:
        errors.append("proxy manifest must not contain flow blockers")
    preflight_path = expected_root(run_kind) / "standalone_preflight.json"
    try:
        preflight = load_json(preflight_path)
        if preflight.get("pcacti_used") is not False:
            errors.append("proxy preflight incorrectly reports pCACTI use")
        build = preflight.get("build")
        command = build.get("command") if isinstance(build, dict) else None
        if command is not None and (
            not isinstance(command, list)
            or any("pcacti" in str(part).lower() or "fncacti_patch" in str(part).lower() for part in command)
        ):
            errors.append("proxy preflight build command introduces pCACTI")
        if preflight.get("blockers") != []:
            errors.append("proxy preflight must not contain blockers")
    except ValueError as error:
        errors.append(str(error))
    targets = record.get("targets")
    if not isinstance(targets, list) or len(targets) != len(common.TARGETS):
        return errors + ["proxy manifest must contain both targets"]
    root = expected_root(run_kind)
    by_target = {entry.get("target"): entry for entry in targets if isinstance(entry, dict)}
    if set(by_target) != set(common.TARGETS):
        return errors + ["proxy manifest target set mismatch"]
    validated_targets: list[dict[str, Any]] = []
    for target in common.TARGETS:
        target_record = by_target[target]
        if target_record.get("status") != flow.PROXY_STATUS:
            errors.append(f"{target}: target status mismatch")
        classes = target_record.get("classes")
        if not isinstance(classes, list) or [item.get("name") for item in classes if isinstance(item, dict)] != list(common.CLASS_ORDER):
            errors.append(f"{target}: class coverage/order mismatch")
            continue
        validated = [validate_modeled_class(item, target, root, errors) for item in classes]
        if all(item is not None for item in validated):
            aggregate = flow.aggregate_classes([item for item in validated if item is not None])
            if target_record.get("aggregate") != aggregate:
                errors.append(f"{target}: aggregate mismatch")
        target_manifest = root / target / "target_manifest.json"
        resolve_artifact(target_record.get("manifest"), common.relative(target_manifest), f"{target} manifest", errors)
        try:
            stored_target = load_json(target_manifest)
            expected_target = copy.deepcopy(target_record)
            expected_target.pop("manifest", None)
            if stored_target != expected_target:
                errors.append(f"{target}: target manifest content mismatch")
        except ValueError as error:
            errors.append(f"{target}: {error}")
        validated_targets.append(target_record)
    logic_area = record.get("logic_area")
    logic_root = common.REPORT_ROOT / "logic_area" / run_kind
    if not isinstance(logic_area, dict):
        errors.append("logic-area evidence is missing")
    else:
        expected_tail = [
            "scripts/run_synth_area.py",
            "--target",
            "all",
            "--report-dir",
            common.relative(logic_root),
        ]
        command = logic_area.get("command")
        if (
            not isinstance(command, list)
            or len(command) != len(expected_tail) + 1
            or pathlib.Path(str(command[0])).name not in {"python", "python3", "tabbypy3"}
            or command[1:] != expected_tail
        ):
            errors.append("logic-area refresh command is not canonical")
        if logic_area.get("returncode") != 0:
            errors.append("logic-area refresh return code is not zero")
        if logic_area.get("units") != "ASAP7_1X_LIBERTY_AREA_UM2":
            errors.append("logic-area units are not verified")
        resolve_artifact(
            logic_area.get("log"),
            common.relative(logic_root / "logic_area_refresh.log"),
            "logic-area refresh log",
            errors,
        )
        logic_targets = logic_area.get("targets")
        if not isinstance(logic_targets, dict) or set(logic_targets) != set(common.TARGETS):
            errors.append("logic-area target coverage mismatch")
        else:
            for target in common.TARGETS:
                entry = logic_targets[target]
                report_path = logic_root / "raw" / f"asap7_area_{target}.json"
                resolve_artifact(entry.get("report") if isinstance(entry, dict) else None, common.relative(report_path), f"logic-area {target} report", errors)
                if not isinstance(entry, dict):
                    continue
                try:
                    report = load_json(report_path)
                    summary = report.get("summary")
                    area = summary.get("technology_area") if isinstance(summary, dict) else None
                    if report.get("status") != "TECHNOLOGY_MAPPED_AREA_PARTIAL_MEMORY_UNMAPPED":
                        errors.append(f"logic-area {target} status is invalid")
                    if entry.get("technology_area_um2") != float(area):
                        errors.append(f"logic-area {target} area mismatch")
                except (ValueError, TypeError):
                    errors.append(f"logic-area {target} report cannot be parsed")
    probe = record.get("device_node_probe")
    probe_root = expected_root(run_kind) / "device_node_probe"
    if not isinstance(probe, dict):
        errors.append("7 nm device-node probe is missing")
    else:
        if probe.get("status") != "UNUSABLE_NONZERO_EXIT":
            errors.append("7 nm device-node probe status is unsafe")
        if probe.get("requested_device_node_um") != 0.007:
            errors.append("7 nm device-node probe node mismatch")
        if probe.get("command") != [str(common.FNC_EXECUTABLE), "-infile", "device_7nm_probe.cfg"]:
            errors.append("7 nm device-node probe command is not canonical")
        if not isinstance(probe.get("returncode"), int) or probe["returncode"] == 0:
            errors.append("7 nm device-node probe did not fail")
        if probe.get("nonzero_exit_observed") is not True:
            errors.append("7 nm device-node probe did not record the nonzero exit")
        if not isinstance(probe.get("tool_output_empty"), bool):
            errors.append("7 nm device-node probe output evidence is malformed")
        if probe.get("fresh_report_produced") is not False:
            errors.append("7 nm device-node probe unexpectedly produced a report")
        resolved_config = resolve_artifact(
            probe.get("config"),
            common.relative(probe_root / "device_7nm_probe.cfg"),
            "7 nm device-node probe config",
            errors,
        )
        resolve_artifact(
            probe.get("raw_log"),
            common.relative(probe_root / "device_7nm_probe.log"),
            "7 nm device-node probe log",
            errors,
        )
        expected_control = next(item for item in common.load_inventory()["classes"] if item["name"] == "control_mem")
        if resolved_config is not None and resolved_config.read_text(encoding="utf-8") != flow.config_text(expected_control, device_node_um=0.007):
            errors.append("7 nm device-node probe configuration mismatch")
    if isinstance(logic_area, dict) and len(validated_targets) == len(common.TARGETS):
        expected_area = flow.memory_inclusive_area(validated_targets, logic_area)
        if record.get("memory_inclusive_area") != expected_area:
            errors.append("memory-inclusive area status/aggregation mismatch")
    return errors


def validate_manifest(path: pathlib.Path, run_kind: str) -> list[str]:
    try:
        record = load_json(path)
    except ValueError as error:
        return [str(error)]
    if record.get("status") == "BLOCKED_PREREQUISITE":
        return validate_blocked(record, run_kind)
    if record.get("status") == flow.PROXY_STATUS:
        return validate_proxy(record, run_kind)
    return ["manifest has unsupported status"]


def self_test() -> int:
    canonical = common.REPORT_ROOT / "canonical/run_manifest.json"
    if not canonical.is_file():
        print("FN_CACTI_CHECKER_SELF_TEST_SKIPPED_NO_CANONICAL_MANIFEST", file=sys.stderr)
        return 1
    record = load_json(canonical)
    cases: list[tuple[str, dict[str, Any], str]] = []
    stale = copy.deepcopy(record)
    stale["source_snapshot"] = {"forged": "hash"}
    cases.append(("stale_source", stale, "input source snapshot is stale"))
    pcacti = copy.deepcopy(record)
    pcacti["pcacti_used"] = True
    cases.append(("pcacti", pcacti, "pcacti_used false"))
    total = copy.deepcopy(record)
    total["whole_design_power_status"] = "MEMORY_INCLUSIVE_POWER_AVAILABLE"
    cases.append(("unsafe_total", total, "whole-design power status is unsafe"))
    if record.get("status") == flow.PROXY_STATUS:
        missing_class = copy.deepcopy(record)
        missing_class["targets"][0]["classes"].pop()
        cases.append(("missing_class", missing_class, "class coverage/order mismatch"))
        padded = copy.deepcopy(record)
        predicate = next(item for item in padded["targets"][0]["classes"] if item["name"] == "predicate_rf")
        predicate["parameter_mapping"]["fn_cacti_size_bytes"] = 2
        cases.append(("minimum_capacity", padded, "parameter mapping drift"))
        incompatible_area = copy.deepcopy(record)
        incompatible_area["memory_inclusive_area"]["status"] = "MEMORY_INCLUSIVE_AREA_AVAILABLE"
        cases.append(("incompatible_area", incompatible_area, "memory-inclusive area status/aggregation mismatch"))
        invalid_units = copy.deepcopy(record)
        invalid_units["targets"][0]["classes"][0]["normalized_metrics"]["area_um2"] *= 2.0
        cases.append(("invalid_units", invalid_units, "normalized metrics mismatch"))
        duplicate = copy.deepcopy(record)
        duplicate["targets"][0]["classes"][-1]["name"] = "control_mem"
        cases.append(("duplicate_class", duplicate, "class coverage/order mismatch"))
    else:
        malformed = copy.deepcopy(record)
        malformed["blockers"] = []
        cases.append(("missing_blocker", malformed, "blocked manifest has no blockers"))
    failures = []
    for name, mutated, required in cases:
        if mutated.get("status") == "BLOCKED_PREREQUISITE":
            errors = validate_blocked(mutated, "canonical")
        else:
            errors = validate_proxy(mutated, "canonical")
        if not any(required in error for error in errors):
            failures.append(name)
    if failures:
        print(f"FN_CACTI_CHECKER_SELF_TEST_FAILED: {', '.join(failures)}", file=sys.stderr)
        return 1
    print("FN_CACTI_CHECKER_SELF_TEST_PASS")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=pathlib.Path, help="manifest directory or manifest JSON")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--require-complete", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    path = args.path or common.REPORT_ROOT
    if path.is_dir():
        canonical = path / "canonical/run_manifest.json"
        replay = path / "replay/run_manifest.json"
    else:
        canonical = path
        replay = common.REPORT_ROOT / "replay/run_manifest.json"
    errors = []
    errors.extend(f"canonical: {error}" for error in validate_manifest(canonical, "canonical"))
    if replay.is_file():
        errors.extend(f"replay: {error}" for error in validate_manifest(replay, "replay"))
        try:
            canonical_record = load_json(canonical)
            replay_record = load_json(replay)
            if canonical_record.get("source_snapshot") != replay_record.get("source_snapshot"):
                errors.append("canonical/replay input snapshots differ")
            if canonical_record.get("status") != replay_record.get("status"):
                errors.append("canonical/replay statuses differ")
        except ValueError as error:
            errors.append(str(error))
    else:
        errors.append("replay manifest is missing")
    if errors:
        for error in errors:
            print(f"FN_CACTI_EVIDENCE_INVALID: {error}", file=sys.stderr)
        return 1
    status = load_json(canonical).get("status")
    if args.require_complete and status != "MEMORY_PPA_COMPLETE_ANALYTICAL":
        print("FN_CACTI_MEMORY_PPA_NOT_COMPLETE", file=sys.stderr)
        return 2
    if status == "BLOCKED_PREREQUISITE":
        print("FN_CACTI_MEMORY_PPA_BLOCKED_EVIDENCE_VALID")
    elif status == flow.PROXY_STATUS:
        print("FN_CACTI_MEMORY_PPA_PROXY_EVIDENCE_VALID")
    else:
        print("FN_CACTI_MEMORY_PPA_EVIDENCE_VALID")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
