#!/usr/bin/env python3
"""Build one CGRA-Bench translation unit with the frozen T019 LLVM profile."""

from __future__ import annotations

import argparse
import json
import pathlib
import platform
import shutil
import subprocess
import sys
import time
from typing import Any

try:
    from .schemas import sha256_file, write_json
except ImportError:  # pragma: no cover - direct script execution
    from schemas import sha256_file, write_json


def run(command: list[str], cwd: pathlib.Path, timeout: int, log: pathlib.Path) -> subprocess.CompletedProcess[str]:
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        log.write_text(
            f"timeout_seconds={timeout}\ncommand={' '.join(command)}\nstdout={error.stdout or ''}\nstderr={error.stderr or ''}\n",
            encoding="utf-8",
        )
        raise
    log.write_text(
        f"duration_ms={int((time.monotonic() - started) * 1000)}\ncommand={' '.join(command)}\n"
        f"returncode={result.returncode}\n\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}\n",
        encoding="utf-8",
    )
    return result


SOURCE_ABI_FLAGS = {
    "m32": ["-m32"],
    "native": [],
}


def build(
    source: pathlib.Path,
    output: pathlib.Path,
    timeout: int,
    extra_flags: list[str] | None = None,
    source_abi: str = "m32",
) -> dict[str, Any]:
    started = time.monotonic()

    def finish(result: dict[str, Any]) -> dict[str, Any]:
        result["duration_ms"] = int((time.monotonic() - started) * 1000)
        return result

    output.mkdir(parents=True, exist_ok=True)
    source = source.resolve()
    compiler = shutil.which("clang++-14" if source.suffix.lower() in {".cc", ".cpp", ".cxx"} else "clang-14")
    opt = shutil.which("opt-14")
    llvm_dis = shutil.which("llvm-dis-14")
    if not compiler or not opt or not llvm_dis:
        return finish({"status": "SOURCE_BUILD_FAILED", "diagnostic_code": "TOOLCHAIN_MISSING", "message": "clang-14/clang++-14, opt-14, and llvm-dis-14 are required"})
    if source_abi not in SOURCE_ABI_FLAGS:
        raise ValueError(f"unsupported source ABI profile: {source_abi}")
    flags = list(extra_flags or [])
    raw_bc = output / "source.raw.bc"
    raw_ll = output / "source.raw.ll"
    canonical_bc = output / "source.bc"
    canonical_ll = output / "source.canonical.ll"
    compile_command = [
        compiler,
        "-O0",
        "-Xclang",
        "-disable-O0-optnone",
        "-fno-discard-value-names",
        *SOURCE_ABI_FLAGS[source_abi],
        *flags,
        "-emit-llvm",
        "-c",
        str(source),
        "-o",
        str(raw_bc),
    ]
    raw_disassemble_command = [llvm_dis, str(raw_bc), "-o", str(raw_ll)]
    canonical_command = [opt, "-mem2reg", "-loop-simplify", "-lcssa", "-simplifycfg", str(raw_bc), "-o", str(canonical_bc)]
    canonical_disassemble_command = [llvm_dis, str(canonical_bc), "-o", str(canonical_ll)]
    commands = {
        "compile": compile_command,
        "raw_disassemble": raw_disassemble_command,
        "canonicalize": canonical_command,
        "canonical_disassemble": canonical_disassemble_command,
    }
    try:
        compiled = run(compile_command, source.parent, timeout, output / "source-build.log")
    except subprocess.TimeoutExpired:
        return finish({"status": "SOURCE_BUILD_FAILED", "diagnostic_code": "SOURCE_BUILD_TIMEOUT", "message": "source compilation timed out", "commands": commands})
    if compiled.returncode != 0:
        return finish({"status": "SOURCE_BUILD_FAILED", "diagnostic_code": "SOURCE_BUILD_FAILED", "message": compiled.stderr[-4000:], "commands": commands})
    try:
        disassembled = run(raw_disassemble_command, source.parent, timeout, output / "raw-disassemble.log")
    except subprocess.TimeoutExpired:
        return finish({"status": "LLVM_CANONICALIZE_FAILED", "diagnostic_code": "RAW_IR_DISASSEMBLY_TIMEOUT", "message": "raw LLVM disassembly timed out", "commands": commands})
    if disassembled.returncode != 0:
        return finish({"status": "LLVM_CANONICALIZE_FAILED", "diagnostic_code": "RAW_IR_DISASSEMBLY_FAILED", "message": disassembled.stderr[-4000:], "commands": commands})
    try:
        canonicalized = run(canonical_command, source.parent, timeout, output / "canonicalize.log")
    except subprocess.TimeoutExpired:
        return finish({"status": "LLVM_CANONICALIZE_FAILED", "diagnostic_code": "LLVM_CANONICALIZE_TIMEOUT", "message": "LLVM canonicalization timed out", "commands": commands})
    if canonicalized.returncode != 0:
        return finish({"status": "LLVM_CANONICALIZE_FAILED", "diagnostic_code": "LLVM_CANONICALIZE_FAILED", "message": canonicalized.stderr[-4000:], "commands": commands})
    try:
        disassembled = run(canonical_disassemble_command, source.parent, timeout, output / "canonical-disassemble.log")
    except subprocess.TimeoutExpired:
        return finish({"status": "LLVM_CANONICALIZE_FAILED", "diagnostic_code": "CANONICAL_IR_DISASSEMBLY_TIMEOUT", "message": "canonical LLVM disassembly timed out", "commands": commands})
    if disassembled.returncode != 0:
        return finish({"status": "LLVM_CANONICALIZE_FAILED", "diagnostic_code": "CANONICAL_IR_DISASSEMBLY_FAILED", "message": disassembled.stderr[-4000:], "commands": commands})
    return finish({
        "status": "LLVM_BUILT",
        "source": str(source),
        "raw_bitcode": str(raw_bc),
        "raw_ir": str(raw_ll),
        "bitcode": str(canonical_bc),
        "ir": str(canonical_ll),
        "source_sha256": sha256_file(source),
        "data_layout": next((line.split(' = ', 1)[1].strip('"') for line in canonical_ll.read_text(encoding="utf-8").splitlines() if line.startswith("target datalayout = ")), None),
        "triple": next((line.split(' = ', 1)[1].strip('"') for line in canonical_ll.read_text(encoding="utf-8").splitlines() if line.startswith("target triple = ")), None),
        "compiler": compiler,
        "opt": opt,
        "profile": {
            "optimization": "O0",
            "canonicalization": ["mem2reg", "loop-simplify", "lcssa", "simplifycfg"],
            "source_abi": source_abi,
        },
        "host": platform.platform(),
        "commands": commands,
    })


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--source-abi", choices=sorted(SOURCE_ABI_FLAGS), default="m32")
    args = parser.parse_args()
    try:
        result = build(args.source, args.out, args.timeout, source_abi=args.source_abi)
        write_json(args.out / "build.json", result)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["status"] == "LLVM_BUILT" else 1
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"cgra-bench build: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
