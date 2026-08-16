#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture RTL activity and run the T037 uncalibrated ASAP7 power probe."""

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys

import run_synth_area as area
import run_yosys_slang as sanity

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
REPORT_DIR = REPO_ROOT / "reports/synthesis"
RAW_DIR = REPORT_DIR / "raw"
SIM_DIR = REPO_ROOT / "sim/synthesis"
REPORT = REPORT_DIR / "power_feasibility.md"
RUNNER = pathlib.Path(__file__).resolve()
CHECKER = REPO_ROOT / "scripts/check_power_feasibility.py"
TRACE_MAIN = SIM_DIR / "power_trace_main.cpp"
ABC_SCRIPT = REPO_ROOT / "synth/asap7_power.abc"
CONSTRAINTS = REPO_ROOT / "synth/asap7_timing.constr"
POWER_TARGETS = ("small", "default")
ACTIVITY_TARGETS = {
    "small": {
        "testbench": REPO_ROOT / "tb/cgra_top_tb.sv",
        "top_module": "cgra_top_tb",
        "description": "directed 2x2 configuration/run test",
    },
    "default": {
        "testbench": REPO_ROOT / "tb/cgra_4x4_smoke_tb.sv",
        "top_module": "cgra_4x4_smoke_tb",
        "description": "directed default 4x4 smoke test",
    },
}
POWER_FRAMES = 64
POWER_PREFIX_FRAMES = 16
CLOCK_PERIOD_PS = 10_000
ACTIVITY_SOURCE = "ABC_INTERNAL_SWITCHING_SIMULATION_FRAMES"
ANNOTATION_STATUS = "NOT_ANNOTATED_TO_ABC"
LIMITATIONS = [
    "The RTL SAIF is not annotated onto the mapped netlist.",
    "ABC power uses internal switching-simulation frames, not workload activity.",
    "ABC prints no power-unit label; raw values are not converted to SI units.",
    "ABC analyzes only its mapped combinational snippet; mapped DFF power is excluded.",
    "Residual $mem_v2 cells have no SRAM area, timing, or power model.",
    "Clock-tree, interconnect, placement, routing, parasitics, and variation are absent.",
    "This is an uncalibrated feasibility prototype, not a power or PPA claim.",
]

POWER_RE = re.compile(
    r'WireLoad\s*=\s*"(?P<wireload>[^"]+)"\s+'
    r"Frames\s*=\s*(?P<frames>\d+)\s+"
    r"Prefix\s*=\s*(?P<prefix>\d+)\s+"
    r"Power\s*=\s*(?P<total>[0-9.eE+-]+)\s+"
    r"Static\s*=\s*(?P<static>[0-9.eE+-]+)\s+\([^)]*\)\s+"
    r"Dynamic\s*=\s*(?P<dynamic>[0-9.eE+-]+)\s+\([^)]*\)\s+"
    r"Internal\s*=\s*(?P<internal>[0-9.eE+-]+)\s+"
    r"External\s*=\s*(?P<external>[0-9.eE+-]+)\s+"
    r"Arcs\s*=\s*(?P<arcs>\d+)"
)
SAIF_DURATION_RE = re.compile(r"\(DURATION\s+(\d+)\)")
SAIF_TIMESCALE_RE = re.compile(r"\(TIMESCALE\s+([^\)]+)\)")
SAIF_TOGGLE_RE = re.compile(r"\(TC\s+(\d+)\)")
LIBERTY_UNIT_PATTERNS = {
    "time_unit": re.compile(r'^\s*time_unit\s*:\s*"([^"]+)"\s*;', re.MULTILINE),
    "voltage_unit": re.compile(
        r'^\s*voltage_unit\s*:\s*"([^"]+)"\s*;', re.MULTILINE
    ),
    "current_unit": re.compile(
        r'^\s*current_unit\s*:\s*"([^"]+)"\s*;', re.MULTILINE
    ),
    "leakage_power_unit": re.compile(
        r'^\s*leakage_power_unit\s*:\s*"([^"]+)"\s*;', re.MULTILINE
    ),
}


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def relative(path: pathlib.Path) -> str:
    return path.relative_to(REPO_ROOT).as_posix()


def run_capture(command: list[str], log_path: pathlib.Path) -> int:
    with log_path.open("w", encoding="utf-8") as log:
        completed = subprocess.run(
            command,
            cwd=REPO_ROOT,
            check=False,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
    return completed.returncode


def version(command: list[str]) -> tuple[int, str]:
    completed = subprocess.run(
        command,
        cwd=REPO_ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=30,
    )
    return completed.returncode, completed.stdout.strip()


def first_line(text: str) -> str:
    return text.splitlines()[0] if text else "<no version output>"


def first_non_banner_line(text: str) -> str:
    for line in text.splitlines():
        if line and not line.startswith("========"):
            return line
    return first_line(text)


def parse_capability_outputs(output: dict[str, str]) -> dict[str, object]:
    verilator_formats = [
        name
        for name, flag in (
            ("VCD", "--trace-vcd"),
            ("FST", "--trace-fst"),
            ("SAIF", "--trace-saif"),
        )
        if flag in output["verilator"]
    ]
    missing_yosys_command = "No such command or cell type: {}"
    return {
        "verilator_trace_formats": verilator_formats,
        "yosys_read_vcd": missing_yosys_command.format("read_vcd")
        not in output["yosys"],
        "yosys_read_saif": missing_yosys_command.format("read_saif")
        not in output["yosys"],
        "yosys_sim_waveform_support": "-vcd <filename>" in output["yosys"],
        "abc_power_command": "usage: power" in output["abc"],
        "abc_power_vcd_option": "vcd" in output["abc"].lower(),
        "abc_power_saif_option": "saif" in output["abc"].lower(),
    }


def run_capability_probe(verilator: str, yosys: str, abc: str) -> dict[str, object]:
    paths = {
        "verilator": SIM_DIR / "power_capability_verilator.log",
        "yosys": SIM_DIR / "power_capability_yosys.log",
        "abc": SIM_DIR / "power_capability_abc.log",
    }
    commands = {
        "verilator": [verilator, "--help"],
        "yosys": [yosys, "-Q", "-p", "help read_vcd; help read_saif; help sim"],
        "abc": [abc, "-c", "power -h"],
    }
    for path in paths.values():
        path.unlink(missing_ok=True)
    for name, command in commands.items():
        if run_capture(command, paths[name]) != 0:
            raise RuntimeError(
                f"{name} capability probe failed; inspect {relative(paths[name])}"
            )
    output = {
        name: path.read_text(encoding="utf-8") for name, path in paths.items()
    }
    return {
        **parse_capability_outputs(output),
        "commands": commands,
        "logs": {name: artifact(path) for name, path in paths.items()},
    }


def target_config(target: str) -> dict[str, object]:
    if target not in POWER_TARGETS:
        raise ValueError(f"unsupported T037 target: {target}")
    return ACTIVITY_TARGETS[target]


def target_paths(target: str) -> dict[str, pathlib.Path]:
    target_config(target)
    return {
        "build_dir": SIM_DIR / f"power_activity_{target}_build",
        "activity_build_log": SIM_DIR / f"power_activity_{target}_build.log",
        "activity_run_log": SIM_DIR / f"power_activity_{target}_run.log",
        "activity_replay_log": SIM_DIR / f"power_activity_{target}_replay.log",
        "canonical_saif": RAW_DIR / f"asap7_power_activity_{target}.saif",
        "replay_saif": RAW_DIR / f"asap7_power_activity_{target}_replay.saif",
        "power_script": RAW_DIR / f"asap7_power_{target}.ys",
        "power_log": RAW_DIR / f"asap7_power_{target}.log",
        "power_stat": RAW_DIR / f"asap7_power_{target}_stat.json",
        "power_replay_script": RAW_DIR / f"asap7_power_{target}_replay.ys",
        "power_replay_log": RAW_DIR / f"asap7_power_{target}_replay.log",
        "power_replay_stat": RAW_DIR / f"asap7_power_{target}_replay_stat.json",
        "metadata": RAW_DIR / f"asap7_power_{target}.json",
    }


def input_snapshot(rtl_files: list[str], target: str) -> dict[str, str]:
    activity_target = target_config(target)
    testbench = activity_target["testbench"]
    assert isinstance(testbench, pathlib.Path)
    return {
        "source_filelist_sha256": sha256(sanity.FILELIST),
        "source_list_sha256": hashlib.sha256(
            ("\n".join(rtl_files) + "\n").encode("utf-8")
        ).hexdigest(),
        "source_contents_sha256": sanity.sha256_sources(rtl_files),
        "testbench_sha256": sha256(testbench),
        "trace_main_sha256": sha256(TRACE_MAIN),
        "runner_sha256": sha256(RUNNER),
        "checker_sha256": sha256(CHECKER),
        "liberty_sha256": sha256(area.ASAP7_LIBERTY),
        "manifest_sha256": sha256(area.ASAP7_MANIFEST),
        "abc_script_sha256": sha256(ABC_SCRIPT),
        "constraints_sha256": sha256(CONSTRAINTS),
    }


def parse_saif(path: pathlib.Path) -> dict[str, object]:
    text = path.read_text(encoding="utf-8")
    duration_match = SAIF_DURATION_RE.search(text)
    timescale_match = SAIF_TIMESCALE_RE.search(text)
    toggles = [int(value) for value in SAIF_TOGGLE_RE.findall(text)]
    return {
        "duration": int(duration_match.group(1)) if duration_match else None,
        "timescale": timescale_match.group(1).strip() if timescale_match else None,
        "activity_records": len(toggles),
        "total_toggle_count": sum(toggles),
    }


def parse_power(log_path: pathlib.Path) -> dict[str, object] | None:
    matches = list(POWER_RE.finditer(log_path.read_text(encoding="utf-8")))
    if not matches:
        return None
    match = matches[-1]
    return {
        "wireload": match.group("wireload"),
        "frames": int(match.group("frames")),
        "prefix_frames": int(match.group("prefix")),
        "total_raw": float(match.group("total")),
        "static_raw": float(match.group("static")),
        "dynamic_raw": float(match.group("dynamic")),
        "internal_raw": float(match.group("internal")),
        "external_raw": float(match.group("external")),
        "arcs": int(match.group("arcs")),
    }


def format_saif_duration(activity: dict[str, object]) -> str:
    duration = activity["duration"]
    timescale = activity["timescale"]
    if timescale == "1ps" and isinstance(duration, int):
        return f"{duration} x 1 ps ({duration / 1000:g} ns)"
    return f"{duration} x {timescale}"


def liberty_summary() -> dict[str, object]:
    text = area.ASAP7_LIBERTY.read_text(encoding="utf-8")
    units = {
        name: match.group(1) if (match := pattern.search(text)) else None
        for name, pattern in LIBERTY_UNIT_PATTERNS.items()
    }
    return {
        **units,
        "internal_power_groups": len(re.findall(r"^\s*internal_power\s*\(\)", text, re.MULTILINE)),
        "leakage_power_groups": len(re.findall(r"^\s*leakage_power\s*\(\)", text, re.MULTILINE)),
        "rise_power_tables": len(re.findall(r"^\s*rise_power\s*\(", text, re.MULTILINE)),
        "fall_power_tables": len(re.findall(r"^\s*fall_power\s*\(", text, re.MULTILINE)),
        "explicit_power_unit": bool(re.search(r"^\s*power_unit\s*:", text, re.MULTILINE)),
    }


def yosys_script(target: str, rtl_files: list[str], stat_path: pathlib.Path) -> str:
    cfg = sanity.TARGETS[target]
    return "\n".join(
        [
            "# Auto-generated by scripts/run_power_feasibility.py for T037.",
            "# ABC uses internal switching frames; no SAIF is annotated here.",
            "plugin -i slang",
            sanity.read_slang_cmd(target, rtl_files),
            f"hierarchy -check -top {cfg['top']}",
            "proc",
            "flatten",
            "opt",
            "memory_collect",
            "techmap */t:* */t:$mem_v2 %d",
            "opt",
            "techmap */t:* */t:$mem_v2 %d",
            "opt",
            "dffunmap",
            f"dfflibmap -liberty {relative(area.ASAP7_LIBERTY)}",
            "abc -fast "
            f"-D {CLOCK_PERIOD_PS} "
            f"-script {relative(ABC_SCRIPT)} "
            f"-constr {relative(CONSTRAINTS)} "
            f"-liberty {relative(area.ASAP7_LIBERTY)}",
            "clean",
            f"tee -o {relative(stat_path)} stat -json -liberty {relative(area.ASAP7_LIBERTY)}",
            "",
        ]
    )


def run_activity(
    target: str, verilator: str, rtl_files: list[str]
) -> tuple[dict[str, object], list[str], list[str]]:
    activity_target = target_config(target)
    testbench = activity_target["testbench"]
    top_module = activity_target["top_module"]
    assert isinstance(testbench, pathlib.Path)
    assert isinstance(top_module, str)
    paths = target_paths(target)
    build_dir = paths["build_dir"]
    build_log = paths["activity_build_log"]
    run_log = paths["activity_run_log"]
    replay_log = paths["activity_replay_log"]
    canonical = paths["canonical_saif"]
    replay = paths["replay_saif"]
    for path in (canonical, replay, build_log, run_log, replay_log):
        path.unlink(missing_ok=True)
    shutil.rmtree(build_dir, ignore_errors=True)
    build_dir.mkdir(parents=True, exist_ok=True)
    command = [
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
        relative(build_dir),
        "--prefix",
        "Vtop",
        *rtl_files,
        relative(testbench),
        relative(TRACE_MAIN),
        "--top-module",
        top_module,
    ]
    if run_capture(command, build_log) != 0:
        raise RuntimeError(f"Verilator activity build failed; inspect {relative(build_log)}")
    executable = build_dir / "Vtop"
    if not executable.is_file():
        raise RuntimeError("Verilator activity executable was not produced")
    run_command = [relative(executable), f"+CGRA_POWER_SAIF={relative(canonical)}"]
    if run_capture(run_command, run_log) != 0:
        raise RuntimeError(f"activity simulation failed; inspect {relative(run_log)}")
    replay_command = [relative(executable), f"+CGRA_POWER_SAIF={relative(replay)}"]
    if run_capture(replay_command, replay_log) != 0:
        raise RuntimeError(f"activity replay failed; inspect {relative(replay_log)}")
    for path in (canonical, replay):
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f"activity capture is missing or empty: {relative(path)}")
    if sha256(canonical) != sha256(replay):
        raise RuntimeError("independent RTL SAIF captures are not byte-identical")
    summary = parse_saif(canonical)
    if (
        summary["duration"] is None
        or summary["activity_records"] == 0
        or summary["total_toggle_count"] == 0
    ):
        raise RuntimeError("SAIF capture has no usable duration or toggle activity")
    return summary, command, run_command


def run_power_probe(
    target: str, yosys: str, rtl_files: list[str], suffix: str
) -> tuple[dict[str, object], dict[str, object], list[str], pathlib.Path, pathlib.Path, pathlib.Path]:
    paths = target_paths(target)
    names = {
        "": ("power_script", "power_log", "power_stat"),
        "_replay": ("power_replay_script", "power_replay_log", "power_replay_stat"),
    }
    script_name, log_name, stat_name = names[suffix]
    script_path = paths[script_name]
    log_path = paths[log_name]
    stat_path = paths[stat_name]
    for path in (script_path, log_path, stat_path):
        path.unlink(missing_ok=True)
    script_path.write_text(yosys_script(target, rtl_files, stat_path), encoding="utf-8")
    command = [yosys, "-Q", "-s", relative(script_path)]
    if run_capture(command, log_path) != 0:
        raise RuntimeError(f"Yosys/ABC power probe failed; inspect {relative(log_path)}")
    power = parse_power(log_path)
    if power is None:
        raise RuntimeError(f"ABC power metric is missing from {relative(log_path)}")
    if power["frames"] != POWER_FRAMES or power["prefix_frames"] != POWER_PREFIX_FRAMES:
        raise RuntimeError("ABC power frames do not match the fixed T037 recipe")
    resource = area.stat_summary(stat_path, sanity.TARGETS[target]["top"])
    if not resource["stat_available"]:
        raise RuntimeError("Yosys mapping stat JSON is missing or invalid")
    residual = resource["unmapped_cell_types"]
    if not residual or any("mem" not in name.lower() for name in residual):
        raise RuntimeError("power probe has unexpected unmapped non-memory logic")
    return power, resource, command, script_path, log_path, stat_path


def artifact(path: pathlib.Path) -> dict[str, object]:
    return {
        "path": relative(path),
        "size_bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def render_report(records: list[dict[str, object]]) -> str:
    if [record["target"] for record in records] != list(POWER_TARGETS):
        raise ValueError("stable T037 report requires exactly small and default records")
    reference = records[0]
    liberty = reference["liberty_power_data"]
    tool = reference["tools"]
    capabilities = reference["activity_capabilities"]
    verilator_formats = ", ".join(capabilities["verilator_trace_formats"])
    yosys_readers = [
        name
        for name, available in (
            ("read_vcd", capabilities["yosys_read_vcd"]),
            ("read_saif", capabilities["yosys_read_saif"]),
        )
        if available
    ]
    yosys_reader_text = (
        f"exposes {', '.join(yosys_readers)}"
        if yosys_readers
        else "exposes neither `read_vcd` nor `read_saif`"
    )
    abc_activity_options = [
        name
        for name, available in (
            ("VCD", capabilities["abc_power_vcd_option"]),
            ("SAIF", capabilities["abc_power_saif_option"]),
        )
        if available
    ]
    abc_option_text = (
        f"advertises {'/'.join(abc_activity_options)} options"
        if abc_activity_options
        else "advertises no VCD or SAIF option"
    )
    summary_rows = []
    mapping_rows = []
    for record in records:
        target = str(record["target"])
        activity = record["rtl_activity"]
        power = record["abc_power_probe"]
        mapping = record["mapping"]
        summary_rows.append(
            f"| {target} | {target_config(target)['description']} | "
            f"{format_saif_duration(activity)} | {activity['activity_records']} | "
            f"{activity['total_toggle_count']} | {power['total_raw']:g} | "
            f"{power['dynamic_raw']:g} | {power['arcs']} |"
        )
        mapping_rows.append(
            f"| {target} | {mapping['mapped_cells']} | {mapping['register_cells']} | "
            f"{mapping['unmodeled_memory_cells']} | {power['frames']}/{power['prefix_frames']} |"
        )
    lines = [
        "# T037 Toggle/Power Feasibility",
        "",
        "Status: `PROTOTYPE_AVAILABLE_UNCALIBRATED`",
        "",
        "T037 confirms that the local open toolchain can capture deterministic RTL activity and can independently run ABC's Liberty-based power probe. It cannot annotate the captured SAIF onto the mapped netlist, so the ABC values below are not workload power.",
        "",
        "## Reproducible prototype",
        "",
        "| Target | RTL SAIF stimulus | Duration | Records | Toggles | ABC total, raw | ABC dynamic, raw | Arcs |",
        "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |",
        *summary_rows,
        "",
        "The ABC fields are retained in raw library/tool units. ABC prints no unit label for this line. The Liberty declares `leakage_power_unit : \"1pW\"` but no explicit `power_unit`; therefore this report does not convert the total, static, dynamic, internal, or external values to W, mW, or uW.",
        "",
        "## Activity and power relationship",
        "",
        "The SAIF and ABC results are separate feasibility artifacts:",
        "",
        "- Verilator records actual RTL activity from distinct directed 2x2 and default-4x4 tests at a 10 ns clock period.",
        "- ABC `power` uses its own internally generated switching-simulation frames after ASAP7 mapping. It does not consume the SAIF.",
        "- The values therefore must not be called workload-annotated power, energy per operation, calibrated power, or a power-scaling comparison.",
        "",
        "## Tool capability audit",
        "",
        f"- Verilator `{tool['verilator_version']}` advertises {verilator_formats} tracing; T037 selects SAIF.",
        f"- The selected Yosys build {yosys_reader_text}. Its `sim` waveform support does not annotate activity into this ABC power flow.",
        f"- ABC exposes `power -F <frames> -P <prefix>` and {abc_option_text}.",
        "- These capability statements are parsed from archived tool-help logs and validated against metadata hashes.",
        f"- OpenSTA path: `{tool['opensta_path'] or 'unavailable'}`; OpenROAD path: `{tool['openroad_path'] or 'unavailable'}`.",
        "",
        "## Library and Mapping Coverage",
        "",
        f"The pinned ASAP7 RVT/TT NLDM Liberty contains {liberty['internal_power_groups']} `internal_power` groups, {liberty['leakage_power_groups']} `leakage_power` groups, and {liberty['rise_power_tables']}/{liberty['fall_power_tables']} rise/fall power tables.",
        "",
        "| Target | Mapped cells | Excluded mapped DFF cells | Unmodeled `$mem_v2` cells | ABC frames/prefix |",
        "| --- | ---: | ---: | ---: | ---: |",
        *mapping_rows,
        "",
        "Each ABC probe covers only its mapped combinational snippet. It excludes mapped DFF leakage, internal, and clock-pin power, and leaves `$mem_v2` cells unmodeled because no SRAM macro or SRAM power model is selected.",
        "",
        "## Missing prerequisites for a power claim",
        "",
        "A workload-correlated estimate requires a gate-level SAIF/VCD name-mapped to the final netlist and a power engine that consumes that activity. A physical estimate additionally requires SRAM power models, clock-tree activity, placed/routed capacitance and parasitics, representative workloads, and explicit PVT/voltage assumptions. None is present in T037.",
        "",
        "## Command and artifacts",
        "",
        "```bash",
        "source ../oss-cad-suite/environment",
        "make synth-power-feasibility",
        "```",
        "",
        "Machine-readable metadata and raw/replay SAIF, logs, Yosys scripts, and stat JSON are under `reports/synthesis/raw/` and `sim/synthesis/`.",
        "",
    ]
    return "\n".join(lines)


def write_report(records: list[dict[str, object]]) -> None:
    REPORT.write_text(render_report(records), encoding="utf-8")


def write_blocked_report(findings: list[str]) -> None:
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    lines = [
        "# T037 Toggle/Power Feasibility",
        "",
        "Status: `BLOCKED_BY_MISSING_PREREQUISITES`",
        "",
        "The prototype could not run in this environment:",
        "",
        *[f"- {finding}" for finding in findings],
        "",
        "Source `../oss-cad-suite/environment`, ensure the pinned ASAP7 cache is present, and rerun `make synth-power-feasibility`.",
        "",
    ]
    REPORT.write_text("\n".join(lines), encoding="utf-8")


def self_test() -> None:
    sample = (
        'ABC: WireLoad = "none" Frames = 64 Prefix = 16 Power = 10 '
        "Static = 6 ( 60.0 %) Dynamic = 4 ( 40.0 %) Internal = 3 "
        "External = 1 Arcs = 99\n"
    )
    match = POWER_RE.search(sample)
    assert match is not None and int(match.group("frames")) == 64
    assert float(match.group("dynamic")) == 4
    assert POWER_RE.search("ABC: Power = unavailable") is None
    saif = "(SAIFILE\n(TIMESCALE 1 ps)\n(DURATION 20)\n(TC 3)\n(TC 5)\n)"
    assert SAIF_DURATION_RE.search(saif).group(1) == "20"
    assert sum(int(value) for value in SAIF_TOGGLE_RE.findall(saif)) == 8
    assert tuple(ACTIVITY_TARGETS) == POWER_TARGETS
    assert target_config("default")["top_module"] == "cgra_4x4_smoke_tb"


def run(targets: tuple[str, ...]) -> int:
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    SIM_DIR.mkdir(parents=True, exist_ok=True)
    findings = []
    verilator = shutil.which("verilator")
    yosys = shutil.which("yosys")
    abc = shutil.which("yosys-abc")
    if verilator is None:
        findings.append("Verilator is unavailable; RTL SAIF capture cannot run.")
    if yosys is None:
        findings.append("Yosys is unavailable; the ASAP7/ABC probe cannot run.")
    if abc is None:
        findings.append("yosys-abc is unavailable; power capability cannot be audited.")
    for path, description in (
        (area.ASAP7_LIBERTY, "merged ASAP7 Liberty"),
        (area.ASAP7_MANIFEST, "ASAP7 provenance manifest"),
        (TRACE_MAIN, "SAIF trace driver"),
        (ABC_SCRIPT, "ABC power script"),
        (CONSTRAINTS, "ABC constraints"),
    ):
        if not path.is_file():
            findings.append(f"Missing {description}: {relative(path)}")
    for target in targets:
        testbench = target_config(target)["testbench"]
        assert isinstance(testbench, pathlib.Path)
        if not testbench.is_file():
            findings.append(f"Missing {target} activity testbench: {relative(testbench)}")
    try:
        rtl_files = sanity.read_filelist(sanity.FILELIST)
    except ValueError as error:
        findings.append(str(error))
        rtl_files = []
    if findings:
        write_blocked_report(findings)
        for finding in findings:
            print(finding, file=sys.stderr)
        return 1

    assert verilator is not None and yosys is not None and abc is not None
    try:
        capabilities = run_capability_probe(verilator, yosys, abc)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        write_blocked_report([str(error)])
        return 1
    if "SAIF" not in capabilities["verilator_trace_formats"]:
        write_blocked_report(["Selected Verilator does not expose --trace-saif."])
        return 1
    if not capabilities["abc_power_command"]:
        write_blocked_report(["Selected ABC does not expose the power command."])
        return 1
    version_status, verilator_version = version([verilator, "--version"])
    yosys_status, yosys_version = version([yosys, "-V"])
    abc_status, abc_version = version([abc, "-c", "version"])
    if version_status != 0 or yosys_status != 0 or abc_status != 0:
        write_blocked_report(["Tool version discovery failed."])
        return 1

    liberty = liberty_summary()
    manifest = json.loads(area.ASAP7_MANIFEST.read_text(encoding="utf-8"))
    records: list[dict[str, object]] = []
    for target in targets:
        before = input_snapshot(rtl_files, target)
        paths = target_paths(target)
        try:
            activity, activity_build_command, activity_run_command = run_activity(
                target, verilator, rtl_files
            )
            power, resource, power_command, script_path, log_path, stat_path = run_power_probe(
                target, yosys, rtl_files, ""
            )
            replay_power, replay_resource, replay_command, replay_script, replay_log, replay_stat = (
                run_power_probe(target, yosys, rtl_files, "_replay")
            )
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            write_blocked_report([f"{target}: {error}"])
            print(error, file=sys.stderr)
            return 1
        if before != input_snapshot(rtl_files, target):
            write_blocked_report(["A durable flow input changed during execution."])
            return 1
        if power != replay_power:
            write_blocked_report([f"{target}: independent ABC power probes produced different metrics."])
            return 1
        if sha256(stat_path) != sha256(replay_stat):
            write_blocked_report([f"{target}: independent ABC mapping stat JSON files are not identical."])
            return 1
        if resource != replay_resource:
            write_blocked_report([f"{target}: independent ABC mapping resource summaries differ."])
            return 1
        activity_target = target_config(target)
        testbench = activity_target["testbench"]
        assert isinstance(testbench, pathlib.Path)
        record = {
            "schema": "cgra.asap7_power_feasibility.v1",
            "status": "PROTOTYPE_AVAILABLE_UNCALIBRATED",
            "target": target,
            "top": sanity.TARGETS[target]["top"],
            "parameters": sanity.TARGETS[target]["parameters"],
            "clock_period_ps": CLOCK_PERIOD_PS,
            "input_snapshot": before,
            "rtl_activity": {
                **activity,
                "source": f"Verilator RTL SAIF from {relative(testbench)}",
                "power_annotation_status": ANNOTATION_STATUS,
                "canonical": artifact(paths["canonical_saif"]),
                "replay": artifact(paths["replay_saif"]),
                "byte_identical_replay": True,
                "build_command": activity_build_command,
                "run_command": activity_run_command,
            },
            "abc_power_probe": {
                **power,
                "activity_source": ACTIVITY_SOURCE,
                "saif_consumed": False,
                "unit_policy": "RAW_ABC_LIBRARY_UNITS_NO_SI_CONVERSION",
                "command": power_command,
                "replay_command": replay_command,
                "metrics_identical_replay": True,
            },
            "mapping": {
                "mapped_cells": resource["mapped_cells"],
                "register_cells": resource["register_cells"],
                "unmodeled_memory_cells": resource["memory_cells"],
                "unmapped_cell_types": resource["unmapped_cell_types"],
                "stat_json_identical_replay": True,
            },
            "activity_capabilities": capabilities,
            "liberty_power_data": liberty,
            "technology": {
                "name": "ASAP7 7nm predictive PDK",
                "corner": manifest.get("corner"),
                "liberty_model": manifest.get("liberty_model"),
                "repository_revision": manifest.get("repository_revision"),
                "standard_cell_revision": manifest.get("standard_cell_revision"),
                "liberty": relative(area.ASAP7_LIBERTY),
            },
            "tools": {
                "verilator_path": verilator,
                "verilator_version": first_line(verilator_version),
                "yosys_path": yosys,
                "yosys_version": first_line(yosys_version),
                "abc_path": abc,
                "abc_version": first_non_banner_line(abc_version),
                "abc_engine": "Yosys abc pass / ABC power",
                "opensta_path": shutil.which("sta"),
                "openroad_path": shutil.which("openroad"),
            },
            "artifacts": {
                "activity_build_log": artifact(paths["activity_build_log"]),
                "activity_run_log": artifact(paths["activity_run_log"]),
                "activity_replay_log": artifact(paths["activity_replay_log"]),
                "capability_verilator_log": capabilities["logs"]["verilator"],
                "capability_yosys_log": capabilities["logs"]["yosys"],
                "capability_abc_log": capabilities["logs"]["abc"],
                "power_script": artifact(script_path),
                "power_log": artifact(log_path),
                "power_stat": artifact(stat_path),
                "power_replay_script": artifact(replay_script),
                "power_replay_log": artifact(replay_log),
                "power_replay_stat": artifact(replay_stat),
            },
            "limitations": LIMITATIONS,
        }
        paths["metadata"].write_text(
            json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        records.append(record)
    if targets != POWER_TARGETS:
        write_blocked_report(["The stable report requires both small and default target evidence."])
        return 1
    write_report(records)
    print(f"T037_POWER_FEASIBILITY_PASS: {relative(REPORT)}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--target", choices=(*POWER_TARGETS, "all"), default="all")
    args = parser.parse_args(argv)
    self_test()
    targets = POWER_TARGETS if args.target == "all" else (args.target,)
    return 0 if args.self_test else run(targets)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
