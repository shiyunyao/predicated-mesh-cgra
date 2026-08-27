#!/usr/bin/env python3
"""Optional source-reference, Golden, and RTL validation for selected audit cases."""

from __future__ import annotations

import json
import pathlib
import subprocess
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Any

try:
    from .schemas import read_json, write_json
except ImportError:  # pragma: no cover - direct script execution
    from schemas import read_json, write_json


SCHEMA = "cgra.cgra_bench.functional_cases.v1"
COMMAND_ADAPTER = "command_observation_v1"


class FunctionalCaseError(ValueError):
    """A declared functional case is malformed or cannot provide source semantics."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class FunctionalCaseAdapter(ABC):
    """A source-semantic adapter for one functional CGRA-Bench loop case."""

    name: str

    @abstractmethod
    def build_kernel_invocation(self, spec: dict[str, Any]) -> dict[str, Any]:
        """Produce the non-synthetic invocation for the generated kernel."""

    @abstractmethod
    def prepare_native_input(self, spec: dict[str, Any], directory: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
        """Write the source input and expected source-semantic observation."""

    @abstractmethod
    def run_native_reference(self, spec: dict[str, Any], paths: dict[str, pathlib.Path], root: pathlib.Path,
                             timeout: int, directory: pathlib.Path) -> "FunctionalValidation":
        """Execute the benchmark-native reference path."""

    @abstractmethod
    def expected_observations(self, spec: dict[str, Any]) -> dict[str, Any]:
        """Return the checked source-semantic observation for this input."""

    @abstractmethod
    def compare_golden(self, native: Any, golden: Any) -> "FunctionalValidation":
        """Compare Golden observations against the native reference."""

    @abstractmethod
    def compare_rtl(self, native: Any, rtl: Any) -> "FunctionalValidation":
        """Compare RTL observations against the native reference."""

    @abstractmethod
    def validate(self, spec: dict[str, Any], result: dict[str, Any], root: pathlib.Path,
                 out: pathlib.Path, timeout: int) -> "FunctionalValidation":
        """Run the complete native -> Golden -> RTL functional contract."""


@dataclass(frozen=True)
class FunctionalValidation:
    ok: bool
    code: str
    message: str
    duration_ms: int = 0


def load_cases(path: pathlib.Path) -> dict[str, dict[str, Any]]:
    manifest = read_json(path)
    if manifest.get("schema") != SCHEMA:
        raise FunctionalCaseError("FUNCTIONAL_SCHEMA_INVALID", "functional cases schema mismatch")
    cases = manifest.get("cases")
    if not isinstance(cases, list):
        raise FunctionalCaseError("FUNCTIONAL_SCHEMA_INVALID", "functional cases must be an array")
    by_id: dict[str, dict[str, Any]] = {}
    for spec in cases:
        if not isinstance(spec, dict) or not isinstance(spec.get("id"), str):
            raise FunctionalCaseError("FUNCTIONAL_SCHEMA_INVALID", "functional case needs a string id")
        case_id = spec["id"]
        if case_id in by_id:
            raise FunctionalCaseError("FUNCTIONAL_DUPLICATE_CASE", f"duplicate functional case: {case_id}")
        if spec.get("adapter") not in ADAPTERS:
            raise FunctionalCaseError(
                "FUNCTIONAL_UNKNOWN_ADAPTER",
                f"{case_id}: unsupported functional adapter {spec.get('adapter')!r}",
            )
        invocation = spec.get("invocation")
        if not isinstance(invocation, dict) or invocation.get("schema") != "cgra.kernel_invocation.v1":
            raise FunctionalCaseError("FUNCTIONAL_INVOCATION_INVALID", f"{case_id}: missing KernelInvocation")
        if invocation.get("synthetic") is True:
            raise FunctionalCaseError(
                "FUNCTIONAL_SYNTHETIC_INVOCATION_FORBIDDEN",
                f"{case_id}: a synthetic invocation cannot claim L7",
            )
        commands = spec.get("commands")
        if not isinstance(commands, dict) or set(commands) != {"native", "golden", "rtl"}:
            raise FunctionalCaseError(
                "FUNCTIONAL_COMMANDS_INVALID",
                f"{case_id}: commands must contain native, golden, and rtl",
            )
        for phase, command in commands.items():
            if not isinstance(command, list) or not command or not all(isinstance(token, str) for token in command):
                raise FunctionalCaseError(
                    "FUNCTIONAL_COMMANDS_INVALID", f"{case_id}: {phase} command must be a non-empty argv list"
                )
            joined = "\n".join(command)
            required = {"native": ("{input}", "{native}"), "golden": ("{manifest}", "{golden}"),
                        "rtl": ("{manifest}", "{rtl}")}[phase]
            if any(placeholder not in joined for placeholder in required):
                raise FunctionalCaseError(
                    "FUNCTIONAL_COMMANDS_INVALID",
                    f"{case_id}: {phase} command lacks required input/output placeholders",
                )
            forbidden = {
                "native": ("{manifest}", "{expected}", "{golden}", "{rtl}"),
                "golden": ("{expected}", "{native}", "{rtl}"),
                "rtl": ("{expected}", "{native}", "{golden}"),
            }[phase]
            if any(placeholder in joined for placeholder in forbidden):
                raise FunctionalCaseError(
                    "FUNCTIONAL_ORACLE_LEAK",
                    f"{case_id}: {phase} command reads another phase or expected oracle",
                )
        if not isinstance(spec.get("inputs"), dict) or not isinstance(spec.get("expected"), dict):
            raise FunctionalCaseError(
                "FUNCTIONAL_SCHEMA_INVALID", f"{case_id}: inputs and expected must be JSON objects"
            )
        by_id[case_id] = spec
    return by_id


def _expand(command: list[str], paths: dict[str, pathlib.Path]) -> list[str]:
    expanded = []
    for token in command:
        for name, path in paths.items():
            token = token.replace("{" + name + "}", str(path))
        expanded.append(token)
    return expanded


def _run_phase(command: list[str], cwd: pathlib.Path, timeout: int, log: pathlib.Path) -> FunctionalValidation:
    started = time.monotonic()
    try:
        completed = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        log.write_text(
            f"timeout_seconds={timeout}\nstdout={error.stdout or ''}\nstderr={error.stderr or ''}\n",
            encoding="utf-8",
        )
        return FunctionalValidation(False, "FUNCTIONAL_TIMEOUT", "functional command timed out", int((time.monotonic() - started) * 1000))
    log.write_text(
        f"returncode={completed.returncode}\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}\n",
        encoding="utf-8",
    )
    if completed.returncode:
        return FunctionalValidation(False, "FUNCTIONAL_COMMAND_FAILED", f"functional command exited {completed.returncode}", int((time.monotonic() - started) * 1000))
    return FunctionalValidation(True, "FUNCTIONAL_PHASE_PASS", "functional command passed", int((time.monotonic() - started) * 1000))


class CommandObservationAdapter(FunctionalCaseAdapter):
    """Adapter for checked-in commands that emit comparable JSON observations."""

    name = COMMAND_ADAPTER

    def build_kernel_invocation(self, spec: dict[str, Any]) -> dict[str, Any]:
        invocation = dict(spec["invocation"])
        invocation.pop("synthetic", None)
        return invocation

    def prepare_native_input(self, spec: dict[str, Any], directory: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
        input_path = directory / "input.json"
        expected_path = directory / "expected.json"
        write_json(input_path, spec["inputs"])
        write_json(expected_path, self.expected_observations(spec))
        return input_path, expected_path

    def run_native_reference(self, spec: dict[str, Any], paths: dict[str, pathlib.Path], root: pathlib.Path,
                             timeout: int, directory: pathlib.Path) -> FunctionalValidation:
        return _run_phase(
            _expand(spec["commands"]["native"], paths), root, timeout, directory / "native.log"
        )

    def expected_observations(self, spec: dict[str, Any]) -> dict[str, Any]:
        return dict(spec["expected"])

    def compare_golden(self, native: Any, golden: Any) -> FunctionalValidation:
        if golden != native:
            return FunctionalValidation(False, "FUNCTIONAL_GOLDEN_MISMATCH", "Golden observation differs from native reference")
        return FunctionalValidation(True, "FUNCTIONAL_GOLDEN_MATCH", "Golden observation matches native reference")

    def compare_rtl(self, native: Any, rtl: Any) -> FunctionalValidation:
        if rtl != native:
            return FunctionalValidation(False, "FUNCTIONAL_RTL_MISMATCH", "RTL observation differs from native reference")
        return FunctionalValidation(True, "FUNCTIONAL_RTL_MATCH", "RTL observation matches native reference")

    def validate(self, spec: dict[str, Any], result: dict[str, Any], root: pathlib.Path,
                 out: pathlib.Path, timeout: int) -> FunctionalValidation:
        if result.get("synthetic_invocation"):
            return FunctionalValidation(
                False,
                "FUNCTIONAL_SYNTHETIC_INVOCATION_FORBIDDEN",
                "synthetic invocations cannot claim functional RTL validation",
            )
        artifact_root = out / result["artifact_directory"]
        functional_dir = artifact_root / "functional"
        functional_dir.mkdir(parents=True, exist_ok=True)
        input_path, expected_path = self.prepare_native_input(spec, functional_dir)
        observations = {phase: functional_dir / f"{phase}.json" for phase in ("native", "golden", "rtl")}
        paths = {
            "root": root,
            "case_dir": artifact_root,
            "manifest": artifact_root / "abi" / "program_manifest.json",
            "invocation": artifact_root / "invocation.json",
            "input": input_path,
            "expected": expected_path,
            **observations,
        }
        if not paths["manifest"].is_file():
            return FunctionalValidation(
                False,
                "FUNCTIONAL_MANIFEST_MISSING",
                "S15 did not leave a compiler-generated program manifest",
            )
        total_duration = 0
        native_phase = self.run_native_reference(spec, paths, root, timeout, functional_dir)
        total_duration += native_phase.duration_ms
        if not native_phase.ok:
            return FunctionalValidation(False, native_phase.code, f"native: {native_phase.message}", total_duration)
        for phase in ("golden", "rtl"):
            phase_result = _run_phase(
                _expand(spec["commands"][phase], paths), root, timeout, functional_dir / f"{phase}.log"
            )
            total_duration += phase_result.duration_ms
            if not phase_result.ok:
                return FunctionalValidation(False, phase_result.code, f"{phase}: {phase_result.message}", total_duration)
        if not all(path.is_file() for path in observations.values()):
            missing = next(phase for phase, path in observations.items() if not path.is_file())
            return FunctionalValidation(False, "FUNCTIONAL_OBSERVATION_MISSING", f"{missing}: command did not write observation JSON", total_duration)
        try:
            expected = read_json(expected_path)
            native = read_json(observations["native"])
            golden = read_json(observations["golden"])
            rtl = read_json(observations["rtl"])
        except (OSError, ValueError, json.JSONDecodeError) as error:
            return FunctionalValidation(False, "FUNCTIONAL_OBSERVATION_INVALID", str(error), total_duration)
        if native != expected:
            return FunctionalValidation(False, "FUNCTIONAL_NATIVE_MISMATCH", "native observation differs from expected source semantics", total_duration)
        golden_check = self.compare_golden(native, golden)
        if not golden_check.ok:
            return FunctionalValidation(False, golden_check.code, golden_check.message, total_duration)
        rtl_check = self.compare_rtl(native, rtl)
        if not rtl_check.ok:
            return FunctionalValidation(False, rtl_check.code, rtl_check.message, total_duration)
        return FunctionalValidation(True, "FUNCTIONAL_RTL_VALIDATED", "native, Golden, and RTL observations agree", total_duration)


ADAPTERS: dict[str, FunctionalCaseAdapter] = {COMMAND_ADAPTER: CommandObservationAdapter()}


def build_kernel_invocation(spec: dict[str, Any]) -> dict[str, Any]:
    """Return a source-derived, non-synthetic invocation for a functional case."""
    return ADAPTERS[spec["adapter"]].build_kernel_invocation(spec)


def validate_case(spec: dict[str, Any], result: dict[str, Any], root: pathlib.Path, out: pathlib.Path,
                  timeout: int) -> FunctionalValidation:
    return ADAPTERS[spec["adapter"]].validate(spec, result, root, out, timeout)
