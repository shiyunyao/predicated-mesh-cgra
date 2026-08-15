#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a deterministic Markdown summary from T036 raw timing reports."""

import argparse
import hashlib
import json
import pathlib
import sys

import run_synth_area as area
import run_yosys_slang as sanity

TARGETS = ("small", "default")
EXPECTED_PERIOD_PS = 10_000.0
EXPECTED_FREQUENCY_MHZ = 100.0
EXPECTED_TIME_UNIT = "1ps"
EXPECTED_ABC_SCRIPT = "synth/asap7_timing.abc"
EXPECTED_CONSTRAINT_FILE = "synth/asap7_timing.constr"
EXPECTED_INPUT_DRIVER = "BUFx2_ASAP7_75t_R"
EXPECTED_OUTPUT_LOAD_FF = 1.0
EXPECTED_TREE_ESTIMATION_RATIO = 10
VALID_TIMING_STATUSES = {
    "ABC_LIBERTY_COMBINATIONAL_TIMING_ESTIMATE",
    "ABC_LIBERTY_COMBINATIONAL_TIMING_ESTIMATE_PARTIAL_MEMORY_UNMODELED",
}


def load_report(path: pathlib.Path) -> dict[str, object] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict) or value.get("schema") != "cgra.asap7_timing_report.v1":
        return None
    return value


def display(value: object, digits: int | None = None) -> str:
    if value is None:
        return "not available"
    if digits is not None and isinstance(value, (int, float)):
        return f"{value:.{digits}f}"
    return str(value)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_reports(report_dir: pathlib.Path) -> list[str]:
    errors: list[str] = []
    try:
        rtl_files = sanity.read_filelist(sanity.FILELIST)
        current_source_hash = sanity.sha256_sources(rtl_files)
        current_filelist_hash = sha256(sanity.FILELIST)
        current_source_list_hash = hashlib.sha256(
            ("\n".join(rtl_files) + "\n").encode("utf-8")
        ).hexdigest()
        current_liberty_hash = sha256(area.ASAP7_LIBERTY)
        current_manifest_hash = sha256(area.ASAP7_MANIFEST)
        current_constraint_hash = sha256(sanity.REPO_ROOT / EXPECTED_CONSTRAINT_FILE)
        current_abc_script_hash = sha256(sanity.REPO_ROOT / EXPECTED_ABC_SCRIPT)
    except (OSError, ValueError) as error:
        return [f"cannot hash the canonical timing inputs: {error}"]

    source_hashes: set[object] = set()
    for target in TARGETS:
        path = report_dir / "raw" / f"asap7_timing_{target}.json"
        report = load_report(path)
        if report is None:
            errors.append(f"{target}: report missing or schema invalid")
            continue
        expected_cfg = sanity.TARGETS[target]
        if report.get("target") != target:
            errors.append(f"{target}: report target field does not match filename")
        if report.get("top") != expected_cfg["top"]:
            errors.append(f"{target}: top module does not match the canonical target")
        if report.get("parameters") != expected_cfg["parameters"]:
            errors.append(f"{target}: parameters do not match the canonical target")
        if report.get("define") != "SYNTHESIS":
            errors.append(f"{target}: SYNTHESIS define metadata is missing")
        if report.get("returncode") != 0:
            errors.append(f"{target}: timing tool return code is not zero")
        if report.get("status") not in VALID_TIMING_STATUSES:
            errors.append(f"{target}: timing flow status is not a valid estimate")

        basename = f"reports/synthesis/raw/asap7_timing_{target}"
        for path_key, hash_key, expected_path in (
            ("script", "script_sha256", f"{basename}.ys"),
            ("raw_log", "raw_log_sha256", f"{basename}.log"),
            ("raw_stat", "raw_stat_sha256", f"{basename}_stat.json"),
            (
                "critical_path_report",
                "critical_path_report_sha256",
                f"{basename}_critical_path.rpt",
            ),
        ):
            relative_path = report.get(path_key)
            expected_hash = report.get(hash_key)
            if relative_path != expected_path:
                errors.append(f"{target}: {path_key} path is not canonical")
                continue
            artifact_path = report_dir / "raw" / pathlib.Path(expected_path).name
            if (
                not isinstance(expected_hash, str)
                or not artifact_path.is_file()
                or artifact_path.stat().st_size == 0
                or sha256(artifact_path) != expected_hash
            ):
                errors.append(f"{target}: {path_key} artifact is missing, empty, or stale")

        expected_filelist = sanity.FILELIST.relative_to(sanity.REPO_ROOT).as_posix()
        if report.get("source_filelist") != expected_filelist:
            errors.append(f"{target}: source filelist path is not canonical")
        if report.get("source_filelist_sha256") != current_filelist_hash:
            errors.append(f"{target}: source filelist hash is stale")
        if report.get("source_list_sha256") != current_source_list_hash:
            errors.append(f"{target}: ordered source-list hash is stale")
        source_hash = report.get("source_contents_sha256")
        source_hashes.add(source_hash)
        if source_hash != current_source_hash:
            errors.append(f"{target}: report source hash is stale")
        expected_snapshot = {
            "source_filelist_sha256": current_filelist_hash,
            "source_list_sha256": current_source_list_hash,
            "source_contents_sha256": current_source_hash,
            "liberty_sha256": current_liberty_hash,
            "manifest_sha256": current_manifest_hash,
            "constraint_file_sha256": current_constraint_hash,
            "abc_script_sha256": current_abc_script_hash,
        }
        if report.get("input_snapshot") != expected_snapshot:
            errors.append(f"{target}: pre-run input snapshot is stale or incomplete")

        clock = report.get("clock_assumption", {})
        clock = clock if isinstance(clock, dict) else {}
        if clock.get("comparison_period_ps") != EXPECTED_PERIOD_PS:
            errors.append(f"{target}: comparison period is not 10000 ps")
        if clock.get("abc_delay_target_ps") != EXPECTED_PERIOD_PS:
            errors.append(f"{target}: ABC delay target is not 10000 ps")
        if clock.get("comparison_frequency_mhz") != EXPECTED_FREQUENCY_MHZ:
            errors.append(f"{target}: comparison frequency is not 100 MHz")

        technology = report.get("technology", {})
        technology = technology if isinstance(technology, dict) else {}
        if technology.get("liberty_time_unit") != EXPECTED_TIME_UNIT:
            errors.append(f"{target}: Liberty time unit is not 1ps")
        expected_liberty = area.ASAP7_LIBERTY.relative_to(sanity.REPO_ROOT).as_posix()
        if technology.get("liberty") != expected_liberty:
            errors.append(f"{target}: Liberty path is not canonical")
        if technology.get("liberty_sha256") != current_liberty_hash:
            errors.append(f"{target}: Liberty hash is stale")
        if technology.get("manifest_sha256") != current_manifest_hash:
            errors.append(f"{target}: ASAP7 manifest hash is stale")

        summary = report.get("summary", {})
        summary = summary if isinstance(summary, dict) else {}
        delay = summary.get("estimated_combinational_delay_ps")
        if summary.get("available") is not True:
            errors.append(f"{target}: timing metric is not marked available")
        if summary.get("stat_available") is not True:
            errors.append(f"{target}: mapped stat evidence is not available")
        if summary.get("delay_unit") != "ps":
            errors.append(f"{target}: parsed delay unit is not ps")
        delay_is_number = isinstance(delay, (int, float)) and not isinstance(delay, bool)
        if not delay_is_number or delay <= 0 or delay > EXPECTED_PERIOD_PS:
            errors.append(f"{target}: estimated delay is not a positive value at or below 10000 ps")
        elif summary.get("comparison_margin_ps") != EXPECTED_PERIOD_PS - delay:
            errors.append(f"{target}: comparison margin is inconsistent with delay")
        reciprocal = summary.get("delay_reciprocal_mhz")
        if (
            delay_is_number
            and (
                not isinstance(reciprocal, (int, float))
                or isinstance(reciprocal, bool)
                or abs(reciprocal - (1_000_000.0 / delay)) > 1e-9
            )
        ):
            errors.append(f"{target}: delay reciprocal is inconsistent with delay")
        if summary.get("timing_target_met") is not True:
            errors.append(f"{target}: timing_target_met is not true")
        if report.get("target_status") != "LOGIC_ONLY_TIMING_TARGET_MET":
            errors.append(f"{target}: target status is not MET")

        boundary = report.get("boundary_assumption", {})
        boundary = boundary if isinstance(boundary, dict) else {}
        if boundary.get("abc_script") != EXPECTED_ABC_SCRIPT:
            errors.append(f"{target}: ABC script path is not canonical")
        if boundary.get("constraint_file") != EXPECTED_CONSTRAINT_FILE:
            errors.append(f"{target}: constraint file path is not canonical")
        if boundary.get("input_driver_cell") != EXPECTED_INPUT_DRIVER:
            errors.append(f"{target}: input driver metadata is inconsistent")
        if boundary.get("output_load_ff") != EXPECTED_OUTPUT_LOAD_FF:
            errors.append(f"{target}: output load metadata is inconsistent")
        if boundary.get("high_fanout_tree_estimation_ratio") != EXPECTED_TREE_ESTIMATION_RATIO:
            errors.append(f"{target}: high-fanout estimation metadata is inconsistent")
        for path_key, hash_key in (
            ("abc_script", "abc_script_sha256"),
            ("constraint_file", "constraint_file_sha256"),
        ):
            relative_path = boundary.get(path_key)
            expected_hash = boundary.get(hash_key)
            if not isinstance(relative_path, str) or not isinstance(expected_hash, str):
                errors.append(f"{target}: missing {path_key} hash metadata")
                continue
            source_path = sanity.REPO_ROOT / relative_path
            if not source_path.is_file() or sha256(source_path) != expected_hash:
                errors.append(f"{target}: {path_key} hash is stale")

    if len(source_hashes) > 1:
        errors.append("small/default source hashes do not match")
    return errors


def build_summary(report_dir: pathlib.Path) -> str:
    raw_dir = report_dir / "raw"
    reports = {
        target: load_report(raw_dir / f"asap7_timing_{target}.json")
        for target in TARGETS
    }
    reference = next((report for report in reports.values() if report), {})
    technology = reference.get("technology", {}) if isinstance(reference, dict) else {}
    technology = technology if isinstance(technology, dict) else {}
    tool = reference.get("tool", {}) if isinstance(reference, dict) else {}
    tool = tool if isinstance(tool, dict) else {}
    clock = reference.get("clock_assumption", {}) if isinstance(reference, dict) else {}
    clock = clock if isinstance(clock, dict) else {}
    boundary = reference.get("boundary_assumption", {}) if isinstance(reference, dict) else {}
    boundary = boundary if isinstance(boundary, dict) else {}

    lines = [
        "# ASAP7 Timing Estimate Summary",
        "",
        "T036S reports a logic-only, pre-layout 100 MHz timing estimate for the flattened "
        "`cgra_top` using ABC `stime -X 10 -p` and the ASAP7 7.5-track v28 RVT/TT NLDM "
        "Liberty. It is not an SDC-constrained STA result, achieved Fmax, signoff "
        "timing, or timing-closure evidence.",
        "",
        "| Target | Top | Parameters | Status | Comb. delay (ps) | Delay reciprocal (MHz) | 100 MHz comparison margin (ps) | Path points | Unmodeled memories | Critical module |",
        "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for target in TARGETS:
        report = reports[target]
        if report is None:
            lines.append(
                f"| {target} | not available | not available | REPORT_MISSING | not available | not available | not available | not available | not available | not available |"
            )
            continue
        summary = report.get("summary", {})
        summary = summary if isinstance(summary, dict) else {}
        lines.append(
            "| {target} | {top} | `{parameters}` | {status} | {delay} | {reciprocal} | "
            "{margin} | {points} | {memories} | {module} |".format(
                target=target,
                top=display(report.get("top")),
                parameters=json.dumps(report.get("parameters", {}), sort_keys=True),
                status=display(report.get("status")),
                delay=display(summary.get("estimated_combinational_delay_ps"), 2),
                reciprocal=display(summary.get("delay_reciprocal_mhz"), 2),
                margin=display(summary.get("comparison_margin_ps"), 2),
                points=display(summary.get("path_point_count")),
                memories=display(summary.get("memory_cells_unmodeled")),
                module=display(summary.get("critical_module")),
            )
        )
    lines.extend(
        [
            "",
            "## Assumptions",
            "",
            f"- Tool: `{display(tool.get('yosys_version'))}`; timing engine `{display(tool.get('timing_engine'))}`.",
            f"- Library: `{display(technology.get('standard_cell_library'))}`, `{display(technology.get('corner'))}` `{display(technology.get('liberty_model'))}`, nominal `{display(technology.get('nominal_voltage_v'))} V` / `{display(technology.get('nominal_temperature_c'))} C`.",
            f"- Clock comparison baseline: `{display(clock.get('comparison_period_ps'))} ps` (`{display(clock.get('comparison_frequency_mhz'))} MHz`) on top-level `clk`; passed to ABC as a mapping delay target, not an SDC clock.",
            f"- Input driver / output load: `{display(boundary.get('input_driver_cell'))}` / `{display(boundary.get('output_load_ff'))} fF`.",
            f"- High-fanout handling: ABC `stime -X {display(boundary.get('high_fanout_tree_estimation_ratio'))}` estimates a buffer tree when output-to-average load ratio reaches the recorded threshold; no physical tree is inserted.",
            "- Reset: synchronous active-low `rst_n`; it is not modeled as a timed clock domain.",
            "",
            "## Interpretation",
            "",
            "`Comb. delay` is the worst mapped combinational cone reported by ABC for "
            "the recorded Liberty and boundary conditions. `Delay reciprocal` is only "
            "the mathematical reciprocal of that logic delay. `100 MHz comparison margin` "
            "is period minus combinational delay and is not setup slack because flop "
            "clock-to-Q/setup arcs are not included.",
            "",
            "The flow preserves inferred `$mem_v2` cells. With no selected SRAM macro "
            "or memory timing model, paths through those cells are absent. Wire delay, "
            "placement, routing, extracted parasitics, clock tree, and variation are also "
            "absent. OpenROAD is therefore not required or run for T036S; a later physical "
            "flow must use the official 1x views and provide its own constraints before "
            "making closure or Fmax claims.",
            "",
            "Raw Yosys logs, generated scripts, machine-readable metadata, mapped "
            "statistics, and per-target critical-path extracts are under "
            "`reports/synthesis/raw/`.",
            "",
        ]
    )
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report_dir", type=pathlib.Path)
    args = parser.parse_args(argv)
    report_dir = args.report_dir.resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    errors = validate_reports(report_dir)
    if errors:
        for error in errors:
            print(f"timing summary validation failed: {error}", file=sys.stderr)
        return 1
    destination = report_dir / "timing_summary.md"
    destination.write_text(build_summary(report_dir), encoding="utf-8")
    print(destination)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
