#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a deterministic Markdown summary from T035 raw synthesis reports."""

import argparse
import json
import pathlib
import sys

TARGETS = ("small", "default")


def load_report(path: pathlib.Path) -> dict[str, object] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict) or value.get("schema") != "cgra.asap7_area_report.v1":
        return None
    return value


def display(value: object) -> str:
    return "not available" if value is None else str(value)


def build_summary(report_dir: pathlib.Path) -> str:
    raw_dir = report_dir / "raw"
    reports = {
        target: load_report(raw_dir / f"asap7_area_{target}.json") for target in TARGETS
    }
    technology = next(
        (
            report.get("technology", {})
            for report in reports.values()
            if report and isinstance(report.get("technology"), dict)
        ),
        {},
    )
    lef = technology.get("lef")
    geometry_scale = "1x" if isinstance(lef, str) and "_1x_" in lef else "unverified"
    if geometry_scale == "1x":
        scale_interpretation = (
            "The selected physical LEF is the official `*_1x_*` view, not the separate "
            "historical `LEF/scaled/*_4x_*` view. The Liberty cell areas match the 1x "
            "LEF geometry, so no 1/16 area correction is applied to T035. A physical "
            "flow that deliberately selects a 4x view must record that choice and undo "
            "the geometric scaling before comparing real-PDK dimensions or area."
        )
    else:
        scale_interpretation = (
            "The physical LEF scale is not verified as 1x. Do not interpret or compare "
            "its geometric area until the selected physical view and any required scale "
            "correction have been recorded explicitly."
        )
    lines = [
        "# ASAP7 Area Summary",
        "",
        "T035 technology-maps the synthesis-visible `cgra_top` RTL using the ASAP7 "
        "7.5-track v28 RVT/TT NLDM standard-cell view. These are cell-area and "
        "resource reports only: no clock constraint, placement, routing, timing, "
        "power, or PPA claim is made.",
        "",
        "| Target | Top | Parameters | Status | Total cells | Mapped cells | Unmapped cells | Register cells | Memory cells | Cell area | OpenROAD |",
        "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for target in TARGETS:
        report = reports[target]
        if report is None:
            lines.append(
                f"| {target} | not available | not available | REPORT_MISSING | not available | not available | not available | not available | not available | not available | not available |"
            )
            continue
        summary = report.get("summary", {})
        summary = summary if isinstance(summary, dict) else {}
        tool = report.get("tool", {})
        tool = tool if isinstance(tool, dict) else {}
        lines.append(
            "| {target} | {top} | `{parameters}` | {status} | {total} | {mapped} | "
            "{unmapped} | {registers} | {memories} | {area} | {openroad} |".format(
                target=target,
                top=display(report.get("top")),
                parameters=json.dumps(report.get("parameters", {}), sort_keys=True),
                status=display(report.get("status")),
                total=display(summary.get("total_cells")),
                mapped=display(summary.get("mapped_cells")),
                unmapped=display(summary.get("unmapped_cells")),
                registers=display(summary.get("register_cells")),
                memories=display(summary.get("memory_cells")),
                area=display(summary.get("technology_area")),
                openroad=display(tool.get("openroad_status")),
            )
        )
    lines.extend(
        [
            "",
            "## Technology Input",
            "",
            f"- Library: `{display(technology.get('standard_cell_library'))}`",
            f"- Corner/model: `{display(technology.get('corner'))}` / `{display(technology.get('liberty_model'))}`",
            f"- ASAP7 repository revision: `{display(technology.get('repository_revision'))}`",
            f"- Standard-cell revision: `{display(technology.get('standard_cell_revision'))}`",
            f"- Physical LEF: `{display(lef)}`",
            f"- Geometry scale: `{geometry_scale}`",
            "",
            "## Interpretation",
            "",
            "`Cell area` is Yosys `stat -liberty` area in the Liberty library's area "
            "units. `Memory cells` counts only residual internal Yosys memory cells; "
            "the T035 flow deliberately preserves them because this scoped ASAP7 setup "
            "does not select SRAM macros. Their area is excluded. Any non-memory internal "
            "cell or a mapping-failed status makes the logic area invalid.",
            "",
            "The `28` in `asap7sc7p5t_28` is standard-cell library release v28, not "
            "a 28 nm process-node label.",
            "",
            scale_interpretation,
            "",
            "Raw tool logs, generated Yosys scripts, machine-readable statistics, and "
            "per-target metadata are under `reports/synthesis/raw/`.",
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
    summary = build_summary(report_dir)
    destination = report_dir / "area_summary.md"
    destination.write_text(summary, encoding="utf-8")
    print(destination)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
