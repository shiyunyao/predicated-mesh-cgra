#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate T037 metadata against every referenced raw artifact."""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

import run_power_feasibility as power_flow
import run_synth_area as area
import run_yosys_slang as sanity

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_METADATA = REPO_ROOT / "reports/synthesis/raw/asap7_power_small.json"
REQUIRED_ARTIFACT_PATHS = {
    "activity_build_log": "sim/synthesis/power_activity_build.log",
    "activity_run_log": "sim/synthesis/power_activity_run.log",
    "activity_replay_log": "sim/synthesis/power_activity_replay.log",
    "capability_verilator_log": "sim/synthesis/power_capability_verilator.log",
    "capability_yosys_log": "sim/synthesis/power_capability_yosys.log",
    "capability_abc_log": "sim/synthesis/power_capability_abc.log",
    "power_script": "reports/synthesis/raw/asap7_power_small.ys",
    "power_log": "reports/synthesis/raw/asap7_power_small.log",
    "power_stat": "reports/synthesis/raw/asap7_power_small_stat.json",
    "power_replay_script": "reports/synthesis/raw/asap7_power_small_replay.ys",
    "power_replay_log": "reports/synthesis/raw/asap7_power_small_replay.log",
    "power_replay_stat": "reports/synthesis/raw/asap7_power_small_replay_stat.json",
}
REQUIRED_ACTIVITY_PATHS = {
    "canonical": "reports/synthesis/raw/asap7_power_activity_small.saif",
    "replay": "reports/synthesis/raw/asap7_power_activity_small_replay.saif",
}


def resolve_artifact(record: dict[str, object]) -> pathlib.Path:
    path = record.get("path")
    if not isinstance(path, str):
        raise ValueError("artifact path is missing")
    resolved = (REPO_ROOT / path).resolve()
    try:
        resolved.relative_to(REPO_ROOT.resolve())
    except ValueError as error:
        raise ValueError(f"artifact escapes repository: {path}") from error
    return resolved


def validate(metadata_path: pathlib.Path) -> list[str]:
    errors = []
    try:
        record = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return [f"cannot read metadata: {error}"]
    if not isinstance(record, dict):
        return ["metadata root must be an object"]
    if record.get("schema") != "cgra.asap7_power_feasibility.v1":
        errors.append("unexpected metadata schema")
    if record.get("status") != "PROTOTYPE_AVAILABLE_UNCALIBRATED":
        errors.append("power feasibility status is not the accepted uncalibrated status")
    if record.get("target") != power_flow.TARGET:
        errors.append("target is not the canonical small target")
    if record.get("clock_period_ps") != power_flow.CLOCK_PERIOD_PS:
        errors.append("clock period is not 10,000 ps")
    target_config = sanity.TARGETS[power_flow.TARGET]
    if record.get("top") != target_config["top"]:
        errors.append("top does not match the canonical target")
    if record.get("parameters") != target_config["parameters"]:
        errors.append("parameters do not match the canonical target")

    rtl_files = []
    try:
        rtl_files = sanity.read_filelist(sanity.FILELIST)
        if record.get("input_snapshot") != power_flow.input_snapshot(rtl_files):
            errors.append("durable input snapshot is stale")
    except (OSError, ValueError) as error:
        errors.append(f"cannot reconstruct input snapshot: {error}")

    activity = record.get("rtl_activity")
    if not isinstance(activity, dict):
        return errors + ["RTL activity object is missing"]
    try:
        canonical_record = activity["canonical"]
        replay_record = activity["replay"]
        canonical_path = resolve_artifact(canonical_record)
        replay_path = resolve_artifact(replay_record)
        if canonical_record.get("path") != REQUIRED_ACTIVITY_PATHS["canonical"]:
            errors.append("canonical SAIF path is not canonical")
        if replay_record.get("path") != REQUIRED_ACTIVITY_PATHS["replay"]:
            errors.append("replay SAIF path is not canonical")
        if canonical_path == replay_path:
            errors.append("canonical and replay SAIF paths are not distinct")
        for name, artifact_record, path in (
            ("canonical SAIF", canonical_record, canonical_path),
            ("replay SAIF", replay_record, replay_path),
        ):
            if not path.is_file() or path.stat().st_size == 0:
                errors.append(f"{name} is missing or empty")
                continue
            if artifact_record.get("size_bytes") != path.stat().st_size:
                errors.append(f"{name} size does not match metadata")
            if artifact_record.get("sha256") != power_flow.sha256(path):
                errors.append(f"{name} hash does not match metadata")
        if canonical_path.is_file() and replay_path.is_file():
            if power_flow.sha256(canonical_path) != power_flow.sha256(replay_path):
                errors.append("canonical and replay SAIF hashes differ")
            parsed_activity = power_flow.parse_saif(canonical_path)
            for field in ("duration", "timescale", "activity_records", "total_toggle_count"):
                if activity.get(field) != parsed_activity[field]:
                    errors.append(f"SAIF {field} does not match metadata")
        if activity.get("source") != "Verilator RTL SAIF from tb/cgra_top_tb.sv":
            errors.append("RTL activity source is not canonical")
        if activity.get("power_annotation_status") != power_flow.ANNOTATION_STATUS:
            errors.append("RTL activity annotation status is unsafe")
        if activity.get("byte_identical_replay") is not True:
            errors.append("RTL activity replay is not declared byte-identical")
    except (KeyError, OSError, ValueError, TypeError) as error:
        errors.append(f"cannot validate SAIF artifacts: {error}")

    artifacts = record.get("artifacts")
    if not isinstance(artifacts, dict):
        return errors + ["artifact manifest is missing"]
    required_artifacts = set(REQUIRED_ARTIFACT_PATHS)
    missing_artifacts = sorted(required_artifacts - artifacts.keys())
    if missing_artifacts:
        errors.append(f"artifact manifest is missing required keys: {missing_artifacts}")
    unexpected_artifacts = sorted(artifacts.keys() - required_artifacts)
    if unexpected_artifacts:
        errors.append(f"artifact manifest has unexpected keys: {unexpected_artifacts}")
    resolved = {}
    for name, artifact_record in artifacts.items():
        if not isinstance(artifact_record, dict):
            errors.append(f"artifact record is invalid: {name}")
            continue
        try:
            path = resolve_artifact(artifact_record)
            expected_path = REQUIRED_ARTIFACT_PATHS.get(name)
            if expected_path is not None and artifact_record.get("path") != expected_path:
                errors.append(f"artifact path is not canonical: {name}")
            if not path.is_file() or path.stat().st_size == 0:
                errors.append(f"artifact is missing or empty: {name}")
                continue
            resolved[name] = path
            if artifact_record.get("size_bytes") != path.stat().st_size:
                errors.append(f"artifact size mismatch: {name}")
            if artifact_record.get("sha256") != power_flow.sha256(path):
                errors.append(f"artifact hash mismatch: {name}")
        except (OSError, ValueError) as error:
            errors.append(f"cannot validate artifact {name}: {error}")

    generated_scripts = {
        "power_script": power_flow.yosys_script(
            rtl_files, REPO_ROOT / REQUIRED_ARTIFACT_PATHS["power_stat"]
        ),
        "power_replay_script": power_flow.yosys_script(
            rtl_files, REPO_ROOT / REQUIRED_ARTIFACT_PATHS["power_replay_stat"]
        ),
    }
    for name, expected_text in generated_scripts.items():
        path = resolved.get(name)
        if path is not None and path.read_text(encoding="utf-8") != expected_text:
            errors.append(f"generated Yosys script does not match the canonical recipe: {name}")

    probe = record.get("abc_power_probe")
    if not isinstance(probe, dict):
        errors.append("ABC power probe object is missing")
    else:
        if probe.get("saif_consumed") is not False:
            errors.append("metadata incorrectly claims that ABC consumed SAIF")
        if probe.get("activity_source") != power_flow.ACTIVITY_SOURCE:
            errors.append("ABC activity source is unsafe")
        if probe.get("metrics_identical_replay") is not True:
            errors.append("ABC power replay is not declared identical")
        if probe.get("unit_policy") != "RAW_ABC_LIBRARY_UNITS_NO_SI_CONVERSION":
            errors.append("ABC unit policy is missing or unsafe")
        if {"power_log", "power_replay_log"}.issubset(resolved):
            parsed = power_flow.parse_power(resolved["power_log"])
            replay = power_flow.parse_power(resolved["power_replay_log"])
            expected_fields = (
                "wireload",
                "frames",
                "prefix_frames",
                "total_raw",
                "static_raw",
                "dynamic_raw",
                "internal_raw",
                "external_raw",
                "arcs",
            )
            if parsed is None or replay is None:
                errors.append("ABC power line is missing from a raw log")
            else:
                if parsed != replay:
                    errors.append("canonical and replay ABC power metrics differ")
                for field in expected_fields:
                    if probe.get(field) != parsed[field]:
                        errors.append(f"ABC {field} does not match metadata")

    mapping = record.get("mapping")
    if not isinstance(mapping, dict):
        errors.append("mapping summary is missing")
    elif mapping.get("stat_json_identical_replay") is not True:
        errors.append("mapping-stat replay is not declared identical")
    if {"power_stat", "power_replay_stat"}.issubset(resolved):
        if power_flow.sha256(resolved["power_stat"]) != power_flow.sha256(
            resolved["power_replay_stat"]
        ):
            errors.append("canonical and replay stat JSON hashes differ")
        resource = area.stat_summary(resolved["power_stat"], sanity.TARGETS[power_flow.TARGET]["top"])
        if isinstance(mapping, dict):
            expected_mapping = {
                "mapped_cells": resource["mapped_cells"],
                "register_cells": resource["register_cells"],
                "unmodeled_memory_cells": resource["memory_cells"],
                "unmapped_cell_types": resource["unmapped_cell_types"],
            }
            for field, value in expected_mapping.items():
                if mapping.get(field) != value:
                    errors.append(f"mapping {field} does not match stat JSON")

    capability_keys = {
        "capability_verilator_log",
        "capability_yosys_log",
        "capability_abc_log",
    }
    capabilities = record.get("activity_capabilities")
    if not isinstance(capabilities, dict):
        errors.append("activity capability audit is missing")
    elif capability_keys.issubset(resolved):
        output = {
            "verilator": resolved["capability_verilator_log"].read_text(encoding="utf-8"),
            "yosys": resolved["capability_yosys_log"].read_text(encoding="utf-8"),
            "abc": resolved["capability_abc_log"].read_text(encoding="utf-8"),
        }
        parsed_capabilities = power_flow.parse_capability_outputs(output)
        for field, value in parsed_capabilities.items():
            if capabilities.get(field) != value:
                errors.append(f"activity capability {field} does not match raw help logs")

    tool_paths = {
        "verilator": shutil.which("verilator"),
        "yosys": shutil.which("yosys"),
        "abc": shutil.which("yosys-abc"),
    }
    if any(path is None for path in tool_paths.values()):
        errors.append("required tools are unavailable for provenance validation")
    else:
        assert all(path is not None for path in tool_paths.values())
        verilator = tool_paths["verilator"]
        yosys = tool_paths["yosys"]
        abc = tool_paths["abc"]
        expected_capability_commands = {
            "verilator": [verilator, "--help"],
            "yosys": [yosys, "-Q", "-p", "help read_vcd; help read_saif; help sim"],
            "abc": [abc, "-c", "power -h"],
        }
        if isinstance(capabilities, dict) and capabilities.get("commands") != expected_capability_commands:
            errors.append("capability probe commands are not canonical")
        if isinstance(capabilities, dict):
            expected_capability_logs = {
                "verilator": artifacts.get("capability_verilator_log"),
                "yosys": artifacts.get("capability_yosys_log"),
                "abc": artifacts.get("capability_abc_log"),
            }
            if capabilities.get("logs") != expected_capability_logs:
                errors.append("capability log provenance does not match the artifact manifest")
        tools = record.get("tools")
        try:
            statuses_and_versions = {
                "verilator": power_flow.version([verilator, "--version"]),
                "yosys": power_flow.version([yosys, "-V"]),
                "abc": power_flow.version([abc, "-c", "version"]),
            }
        except (OSError, subprocess.SubprocessError) as error:
            errors.append(f"cannot reconstruct tool versions: {error}")
        else:
            if any(status != 0 for status, _ in statuses_and_versions.values()):
                errors.append("tool version reconstruction failed")
            expected_tools = {
                "verilator_path": verilator,
                "verilator_version": power_flow.first_line(statuses_and_versions["verilator"][1]),
                "yosys_path": yosys,
                "yosys_version": power_flow.first_line(statuses_and_versions["yosys"][1]),
                "abc_path": abc,
                "abc_version": power_flow.first_non_banner_line(statuses_and_versions["abc"][1]),
                "abc_engine": "Yosys abc pass / ABC power",
                "opensta_path": shutil.which("sta"),
                "openroad_path": shutil.which("openroad"),
            }
            if tools != expected_tools:
                errors.append("tool provenance does not match the selected environment")
        expected_power_commands = {
            "command": [yosys, "-Q", "-s", "reports/synthesis/raw/asap7_power_small.ys"],
            "replay_command": [
                yosys,
                "-Q",
                "-s",
                "reports/synthesis/raw/asap7_power_small_replay.ys",
            ],
        }
        if isinstance(probe, dict):
            for field, expected in expected_power_commands.items():
                if probe.get(field) != expected:
                    errors.append(f"ABC {field} is not canonical")
        expected_activity_build = [
            verilator,
            "-Wall",
            "-Wno-fatal",
            "--cc",
            "--exe",
            "--build",
            "--trace-saif",
            "--trace-depth",
            "4",
            "--Mdir",
            power_flow.relative(power_flow.BUILD_DIR),
            "--prefix",
            "Vtop",
            *rtl_files,
            power_flow.relative(power_flow.TESTBENCH),
            power_flow.relative(power_flow.TRACE_MAIN),
            "--top-module",
            "cgra_top_tb",
        ]
        expected_activity_run = [
            power_flow.relative(power_flow.BUILD_DIR / "Vtop"),
            "+CGRA_POWER_SAIF=reports/synthesis/raw/asap7_power_activity_small.saif",
        ]
        if activity.get("build_command") != expected_activity_build:
            errors.append("RTL activity build command is not canonical")
        if activity.get("run_command") != expected_activity_run:
            errors.append("RTL activity run command is not canonical")

    try:
        manifest = json.loads(area.ASAP7_MANIFEST.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f"cannot reconstruct technology provenance: {error}")
    else:
        expected_technology = {
            "name": "ASAP7 7nm predictive PDK",
            "corner": manifest.get("corner"),
            "liberty_model": manifest.get("liberty_model"),
            "repository_revision": manifest.get("repository_revision"),
            "standard_cell_revision": manifest.get("standard_cell_revision"),
            "liberty": power_flow.relative(area.ASAP7_LIBERTY),
        }
        if record.get("technology") != expected_technology:
            errors.append("technology provenance does not match the pinned manifest")
    if record.get("limitations") != power_flow.LIMITATIONS:
        errors.append("feasibility limitations are missing or altered")

    liberty = record.get("liberty_power_data")
    if liberty != power_flow.liberty_summary():
        errors.append("Liberty power-data summary is stale")
    report_path = power_flow.REPORT
    try:
        report_text = report_path.read_text(encoding="utf-8")
    except OSError as error:
        errors.append(f"cannot read stable report: {error}")
    else:
        try:
            expected_report = power_flow.render_report(record)
        except (KeyError, TypeError, ValueError) as error:
            errors.append(f"cannot render stable report from metadata: {error}")
        else:
            if report_text != expected_report:
                errors.append("stable report does not exactly match validated metadata")
    return errors


def self_test() -> None:
    canonical_record = json.loads(DEFAULT_METADATA.read_text(encoding="utf-8"))
    record = json.loads(json.dumps(canonical_record))
    record["artifacts"] = {}
    probe = record["abc_power_probe"]
    probe["total_raw"] = 0
    probe["dynamic_raw"] = 0
    probe["activity_source"] = "RTL_SAIF_WORKLOAD"
    probe["metrics_identical_replay"] = False
    record["rtl_activity"]["power_annotation_status"] = "ANNOTATED_TO_ABC"
    record["rtl_activity"]["byte_identical_replay"] = False
    record["mapping"]["stat_json_identical_replay"] = False
    record["top"] = "fake_top"
    record["technology"] = {}
    record["tools"]["abc_version"] = "fake"
    record["limitations"] = []
    with tempfile.TemporaryDirectory(prefix="t037-power-check-") as directory:
        path = pathlib.Path(directory) / "missing-artifacts.json"
        path.write_text(json.dumps(record), encoding="utf-8")
        errors = validate(path)
    assert any("missing required keys" in error for error in errors)
    assert any("activity source is unsafe" in error for error in errors)
    assert any("annotation status is unsafe" in error for error in errors)
    assert any("replay" in error for error in errors)
    assert any("top does not match" in error for error in errors)
    assert any("technology provenance" in error for error in errors)
    assert any("tool provenance" in error for error in errors)
    assert any("limitations" in error for error in errors)
    assert any("stable report does not exactly match" in error for error in errors)

    substituted = json.loads(json.dumps(canonical_record))
    substituted["artifacts"]["power_script"] = substituted["artifacts"][
        "activity_build_log"
    ]
    with tempfile.TemporaryDirectory(prefix="t037-power-path-check-") as directory:
        path = pathlib.Path(directory) / "substituted-artifact.json"
        path.write_text(json.dumps(substituted), encoding="utf-8")
        errors = validate(path)
    assert any("artifact path is not canonical: power_script" in error for error in errors)

    substituted = json.loads(json.dumps(canonical_record))
    substituted["rtl_activity"]["replay"] = substituted["rtl_activity"]["canonical"]
    with tempfile.TemporaryDirectory(prefix="t037-power-saif-path-check-") as directory:
        path = pathlib.Path(directory) / "substituted-saif.json"
        path.write_text(json.dumps(substituted), encoding="utf-8")
        errors = validate(path)
    assert any("replay SAIF path is not canonical" in error for error in errors)
    assert any("SAIF paths are not distinct" in error for error in errors)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata", nargs="?", type=pathlib.Path, default=DEFAULT_METADATA)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        print("T037_POWER_FEASIBILITY_CHECKER_SELF_TEST_PASS")
        return 0
    metadata = args.metadata if args.metadata.is_absolute() else REPO_ROOT / args.metadata
    errors = validate(metadata)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("T037_POWER_FEASIBILITY_EVIDENCE_VALID")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
