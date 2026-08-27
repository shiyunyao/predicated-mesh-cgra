#!/usr/bin/env python3
"""Small schema and serialization helpers for the T019 audit artifacts."""

from __future__ import annotations

import json
import pathlib
from typing import Any


def write_json(path: pathlib.Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_json(path: pathlib.Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def source_language(path: pathlib.Path) -> str:
    suffix = path.suffix.lower()
    if suffix == ".c":
        return "c"
    if suffix in {".cc", ".cpp", ".cxx"}:
        return "c++"
    raise ValueError(f"unsupported source extension: {path}")
