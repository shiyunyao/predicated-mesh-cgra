#!/usr/bin/env python3
"""Inventory the pinned CGRA-Bench kernels corpus without changing sources."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys
from typing import Any

try:
    from .schemas import source_language, write_json
except ImportError:  # pragma: no cover - direct script execution
    from schemas import source_language, write_json


PIN = "6729aaf225d0320e4e0d3b419e20483069a5a69b"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_sha(path: pathlib.Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def inventory(corpus: pathlib.Path, output: pathlib.Path, cases_output: pathlib.Path | None = None) -> dict[str, Any]:
    kernels = corpus / "kernels"
    if not kernels.is_dir():
        raise ValueError(f"missing corpus directory: {kernels}")
    commit = git_sha(corpus)
    if commit != PIN:
        raise ValueError(f"CGRA-Bench commit mismatch: expected {PIN}, got {commit}")
    dirty = subprocess.check_output(["git", "-C", str(corpus), "status", "--porcelain"], text=True)
    if dirty:
        raise ValueError("CGRA-Bench submodule is dirty")
    directories = sorted(path for path in kernels.iterdir() if path.is_dir())
    sources = sorted(
        path for path in kernels.rglob("*") if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
    )
    entries = [
        {
            "path": path.relative_to(corpus).as_posix(),
            "kernel": path.relative_to(kernels).parts[0],
            "language": source_language(path),
            "sha256": sha256(path),
            "enabled": True,
            "exclusion": None,
        }
        for path in sources
    ]
    result = {
        "schema": "cgra.cgra_bench.corpus.v1",
        "repository": "https://github.com/tancheng/CGRA-Bench",
        "commit": commit,
        "license": "BSD-3-Clause",
        "scope": "kernels/",
        "nested_submodules": False,
        "kernel_directories": [path.name for path in directories],
        "sources": entries,
        "denominator": {
            "kernel_directories": len(directories),
            "source_translation_units": len(sources),
            "candidate_loops": None,
        },
    }
    write_json(output, result)
    if cases_output is not None:
        cases = []
        for entry in entries:
            source = entry["path"]
            cases.append(
                {
                    "id": f"{source}::auto",
                    "kernel": entry["kernel"],
                    "source": source,
                    "language": entry["language"],
                    "compile_flags": [],
                    "defines": [],
                    "include_dirs": [],
                    "function": None,
                    "loop_header": None,
                    "enabled": True,
                    "exclusion": None,
                }
            )
        write_json(cases_output, {"schema": "cgra.cgra_bench.cases.v1", "cases": cases})
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=pathlib.Path, default=pathlib.Path("third_party/CGRA-Bench"))
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("benchmarks/cgra-bench/corpus.lock.json"))
    parser.add_argument("--cases-out", type=pathlib.Path, default=pathlib.Path("benchmarks/cgra-bench/cases.v1.json"))
    args = parser.parse_args()
    try:
        result = inventory(args.corpus.resolve(), args.out, args.cases_out)
        print(
            f"inventoried {result['denominator']['kernel_directories']} kernel directories and "
            f"{result['denominator']['source_translation_units']} source units at {result['commit']}"
        )
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"cgra-bench inventory: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
