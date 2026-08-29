#!/usr/bin/env python3
"""Run the four local RF-closure audit configurations.

The runner deliberately forwards identical corpus, target, profile and ABI
arguments to each lane.  It never invokes inventory, so tracked case overrides
remain untouched.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


CONFIGS = {
    "A_baseline": {"rf_port_aware": "off", "software_rotation": "off"},
    "B_port_only": {"rf_port_aware": "on", "software_rotation": "off"},
    "C_mve_only": {"rf_port_aware": "off", "software_rotation": "on"},
    "D_combined": {"rf_port_aware": "on", "software_rotation": "on"},
}


def command_for(root: pathlib.Path, build: pathlib.Path, output: pathlib.Path,
                profile: str, abi: str, name: str,
                corpus: pathlib.Path | None = None,
                cases: pathlib.Path | None = None,
                target: pathlib.Path | None = None,
                timeout: int | None = None) -> list[str]:
    config = CONFIGS[name]
    corpus = corpus or root / "third_party/CGRA-Bench"
    cases = cases or root / "benchmarks/cgra-bench/cases.v1.json"
    target = target or root / "target/cgra_mapping64_v1.json"
    command = [
        sys.executable, str(root / "tools/cgra_bench/run.py"),
        "--corpus", str(corpus),
        "--cases", str(cases),
        "--target", str(target),
        "--frontend-bin", str(build / "bin/cgra-llvm-loop-lower"),
        "--compile-kernel-bin", str(build / "bin/cgrac-compile-kernel"),
        "--out", str(output / name), "--all", "--profile", profile,
        "--lane", "mapping-research", "--source-abi", abi,
        "--mapping-objective", "find-any-feasible", "--enable-recurrence-ingress",
        "--rf-port-aware", config["rf_port_aware"],
        "--software-rotation", config["software_rotation"],
    ]
    if timeout is not None:
        command.extend(["--timeout", str(timeout)])
    if cases.name == "smoke_cases.v1.json":
        command.append("--allow-subset")
    return command


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler-build", type=pathlib.Path, required=True)
    parser.add_argument("--output-root", type=pathlib.Path, required=True)
    parser.add_argument("--profile", default="research")
    parser.add_argument("--source-abi", default="m32")
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--corpus", type=pathlib.Path)
    parser.add_argument("--cases", type=pathlib.Path)
    parser.add_argument("--target", type=pathlib.Path)
    parser.add_argument("--timeout", type=int)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    args.output_root.mkdir(parents=True, exist_ok=True)
    failures = 0
    for name in CONFIGS:
        command = command_for(args.repo, args.compiler_build, args.output_root,
                              args.profile, args.source_abi, name,
                              args.corpus, args.cases, args.target, args.timeout)
        (args.output_root / name / "command.txt").parent.mkdir(parents=True, exist_ok=True)
        (args.output_root / name / "command.txt").write_text(" ".join(command) + "\n")
        if args.dry_run:
            continue
        completed = subprocess.run(command, cwd=args.repo)
        if completed.returncode:
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
