#!/usr/bin/env python3
"""Record reproducibility metadata after a compiler-generated program replay."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--dfg", required=True, type=pathlib.Path)
    parser.add_argument("--target", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--compiler-artifacts", required=True, type=pathlib.Path)
    parser.add_argument("--program-dir", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        compiler_report = json.loads(
            (args.compiler_artifacts / "compiler_pipeline_report.json").read_text(encoding="utf-8")
        )
        compare = args.program_dir / "compare.log"
        archived_manifest = args.program_dir / "program_manifest.json"
        rtl_trace = args.program_dir / "rtl_trace.csv"
        golden_trace = args.program_dir / "golden_trace.csv"
        manifest_match = archived_manifest.is_file() and sha256(archived_manifest) == sha256(args.manifest)
        replay_ok = compare.is_file() and "PROGRAM_TRACE_MATCH" in compare.read_text(encoding="utf-8")
        result = {
            "schema": "cgra.compiler_e2e.result.v1",
            "status": "success"
            if replay_ok and manifest_match and compiler_report.get("status") == "success"
            else "failure",
            "fixture": args.fixture,
            "compiler": {
                "status": compiler_report.get("status"),
                "stats": compiler_report.get("stats", {}),
                "dfg_sha256": sha256(args.dfg),
                "target_sha256": sha256(args.target),
                "manifest_sha256": sha256(args.manifest),
            },
            "replay": {
                "program_runner": "pass" if replay_ok else "failure",
                "golden_trace": golden_trace.is_file() and golden_trace.stat().st_size > 0,
                "rtl_trace": rtl_trace.is_file() and rtl_trace.stat().st_size > 0,
                "archived_manifest_sha256": sha256(archived_manifest)
                if archived_manifest.is_file()
                else None,
                "manifest_hash_matches": manifest_match,
                "compare_log": str(compare),
            },
            "artifacts": {
                "compiler": str(args.compiler_artifacts),
                "program": str(args.program_dir),
                "compiler_sha256": {
                    path.name: sha256(path)
                    for path in sorted(args.compiler_artifacts.glob("*.json"))
                    if path.is_file()
                },
            },
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"compiler E2E report: {args.output}")
        return 0 if result["status"] == "success" else 1
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"compiler E2E report failure: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
