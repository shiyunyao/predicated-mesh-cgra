#!/usr/bin/env python3
"""Inventory the pinned CGRA-Bench kernels corpus without changing sources."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
from typing import Any

try:
    from .schemas import read_json, sha256_file, source_language, write_json
except ImportError:  # pragma: no cover - direct script execution
    from schemas import read_json, sha256_file, source_language, write_json


PIN = "6729aaf225d0320e4e0d3b419e20483069a5a69b"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
OVERRIDES_SCHEMA = "cgra.cgra_bench.case_overrides.v1"
ALLOWED_OVERRIDE_KEYS = {
    "source",
    "compile_flags",
    "defines",
    "include_dirs",
    "language",
    "function",
    "loop_header",
}


def git_sha(path: pathlib.Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def _load_case_overrides(path: pathlib.Path, commit: str, source_paths: set[str]) -> dict[str, dict[str, Any]]:
    if not path.is_file():
        raise ValueError(f"missing case overrides manifest: {path}")
    manifest = read_json(path)
    if manifest.get("schema") != OVERRIDES_SCHEMA:
        raise ValueError("case overrides schema mismatch")
    manifest_commit = manifest.get("corpus_sha")
    if manifest_commit != commit:
        raise ValueError(
            f"case overrides corpus mismatch: expected {commit}, got {manifest_commit}"
        )
    entries = manifest.get("overrides")
    if not isinstance(entries, list):
        raise ValueError("case overrides must contain an array")
    overrides: dict[str, dict[str, Any]] = {}
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("source"), str):
            raise ValueError("case override needs a string source")
        source = entry["source"]
        if source not in source_paths:
            raise ValueError(f"case override source is outside corpus: {source}")
        unknown = set(entry) - ALLOWED_OVERRIDE_KEYS
        if unknown:
            raise ValueError(f"case override has unsupported keys: {sorted(unknown)}")
        if source in overrides:
            raise ValueError(f"duplicate case override: {source}")
        for key in ("compile_flags", "defines", "include_dirs"):
            if key in entry and (not isinstance(entry[key], list) or
                                 not all(isinstance(value, str) for value in entry[key])):
                raise ValueError(f"case override {source}.{key} must be a string array")
        if "language" in entry and entry["language"] not in {"c", "c++"}:
            raise ValueError(f"case override {source}.language is invalid")
        for key in ("function", "loop_header"):
            if key in entry and entry[key] is not None and not isinstance(entry[key], str):
                raise ValueError(f"case override {source}.{key} must be a string or null")
        overrides[source] = {
            key: value for key, value in entry.items() if key != "source"
        }
    return overrides


def inventory(
    corpus: pathlib.Path,
    output: pathlib.Path,
    cases_output: pathlib.Path | None = None,
    overrides_path: pathlib.Path | None = None,
    base_cases_output: pathlib.Path | None = None,
) -> dict[str, Any]:
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
            "sha256": sha256_file(path),
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
        base_manifest = {"schema": "cgra.cgra_bench.cases.base.v1", "corpus_sha": commit, "cases": cases}
        if base_cases_output is not None:
            write_json(base_cases_output, base_manifest)
        if overrides_path is not None:
            overrides = _load_case_overrides(overrides_path, commit, {entry["path"] for entry in entries})
            by_source = {case["source"]: case for case in cases}
            for source, override in overrides.items():
                by_source[source].update(override)
        else:
            overrides = {}
        write_json(cases_output, {
            "schema": "cgra.cgra_bench.cases.v1",
            "corpus_sha": commit,
            "base_schema": base_manifest["schema"],
            "override_schema": OVERRIDES_SCHEMA if overrides_path is not None else None,
            "cases": cases,
        })
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=pathlib.Path, default=pathlib.Path("third_party/CGRA-Bench"))
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("benchmarks/cgra-bench/corpus.lock.json"))
    parser.add_argument("--cases-out", type=pathlib.Path, default=pathlib.Path("benchmarks/cgra-bench/cases.v1.json"))
    parser.add_argument(
        "--overrides",
        type=pathlib.Path,
        default=pathlib.Path("benchmarks/cgra-bench/cases.overrides.v1.json"),
    )
    parser.add_argument(
        "--base-cases-out",
        type=pathlib.Path,
        default=pathlib.Path("benchmarks/cgra-bench/cases.base.v1.json"),
    )
    args = parser.parse_args()
    try:
        result = inventory(
            args.corpus.resolve(), args.out, args.cases_out,
            args.overrides.resolve(), args.base_cases_out,
        )
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
