#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fetch and normalize the pinned ASAP7 v28 RVT/TT NLDM views for T035."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
ASAP7_REPOSITORY = "https://github.com/The-OpenROAD-Project/asap7.git"
ASAP7_REPOSITORY_REVISION = "d24f8b857ff74cf5b21ab18a7e1b11a3954c449b"
ASAP7_SC7P5T28_REVISION = "f970bd3c3292b79ae4d022a3ec80533534614066"
ASAP7_SC7P5T28_RAW = (
    "https://raw.githubusercontent.com/The-OpenROAD-Project/asap7sc7p5t_28/"
    f"{ASAP7_SC7P5T28_REVISION}"
)
NLDM_ARCHIVES = (
    "asap7sc7p5t_AO_RVT_TT_nldm_211120.lib.7z",
    "asap7sc7p5t_INVBUF_RVT_TT_nldm_220122.lib.7z",
    "asap7sc7p5t_OA_RVT_TT_nldm_211120.lib.7z",
    "asap7sc7p5t_SEQ_RVT_TT_nldm_220123.lib.7z",
    "asap7sc7p5t_SIMPLE_RVT_TT_nldm_211120.lib.7z",
)
RVT_LEF = "asap7sc7p5t_28_R_1x_220121a.lef"


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def download(url: str, destination: pathlib.Path, force: bool) -> None:
    if destination.exists() and not force:
        return
    temporary = destination.with_name(destination.name + ".part")
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            os.replace(temporary, destination)
            return
        except OSError:
            temporary.unlink(missing_ok=True)
            if attempt == 3:
                raise
            time.sleep(2**attempt)


def find_7z(explicit: str | None) -> str | None:
    candidates = [explicit, os.environ.get("ASAP7_7Z"), shutil.which("7z")]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def extract_archive(
    seven_zip: str | None, archive: pathlib.Path, library_dir: pathlib.Path
) -> None:
    expected = library_dir / archive.name.removesuffix(".7z")
    if expected.exists():
        return
    if seven_zip is None:
        raise RuntimeError(
            "7z is required to extract the official ASAP7 Liberty archives; "
            "install p7zip-full or set ASAP7_7Z to a 7z executable."
        )
    completed = subprocess.run(
        [seven_zip, "x", "-y", f"-o{library_dir}", str(archive)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if completed.returncode != 0 or not expected.exists():
        raise RuntimeError(
            f"could not extract {archive.name} with {seven_zip}:\n{completed.stdout}"
        )


def cell_blocks(text: str, source: pathlib.Path) -> list[str]:
    blocks: list[str] = []
    for match in re.finditer(r"\bcell\s*\([^)]*\)\s*\{", text):
        start = match.start()
        cursor = match.end() - 1
        depth = 0
        while cursor < len(text):
            if text[cursor] == "{":
                depth += 1
            elif text[cursor] == "}":
                depth -= 1
                if depth == 0:
                    blocks.append(text[start : cursor + 1])
                    break
            cursor += 1
        else:
            raise ValueError(f"unterminated Liberty cell in {source}")
    if not blocks:
        raise ValueError(f"no Liberty cells found in {source}")
    return blocks


def merge_libraries(library_dir: pathlib.Path, destination: pathlib.Path) -> dict[str, int]:
    sources = [library_dir / archive.removesuffix(".7z") for archive in NLDM_ARCHIVES]
    texts = [path.read_text(encoding="utf-8") for path in sources]
    first_cell = re.search(r"\bcell\s*\(", texts[0])
    if first_cell is None:
        raise ValueError(f"no Liberty preamble found in {sources[0]}")
    preamble = texts[0][: first_cell.start()]
    blocks = [
        block
        for source, text in zip(sources, texts)
        for block in cell_blocks(text, source)
    ]
    destination.write_text(
        preamble.rstrip() + "\n\n" + "\n\n".join(blocks) + "\n}\n",
        encoding="utf-8",
    )
    return {path.name: len(cell_blocks(text, path)) for path, text in zip(sources, texts)}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--destination",
        type=pathlib.Path,
        default=REPO_ROOT / "synth/asap7/v28/RVT_TT",
        help="directory for the pinned ASAP7 cache",
    )
    parser.add_argument(
        "--seven-zip",
        help="path to a 7z-compatible executable; alternatively set ASAP7_7Z",
    )
    parser.add_argument("--force", action="store_true", help="re-download archive inputs")
    args = parser.parse_args(argv)

    root = args.destination.resolve()
    archive_dir = root / "archive"
    library_dir = root / "lib"
    lef_dir = root / "lef"
    for directory in (archive_dir, library_dir, lef_dir):
        directory.mkdir(parents=True, exist_ok=True)

    seven_zip = find_7z(args.seven_zip)
    missing_extracted = [
        name.removesuffix(".7z")
        for name in NLDM_ARCHIVES
        if not (library_dir / name.removesuffix(".7z")).exists()
    ]
    if missing_extracted and seven_zip is None:
        print(
            "7z is required to extract the official ASAP7 Liberty archives; "
            "install p7zip-full or set ASAP7_7Z to a 7z executable.",
            file=sys.stderr,
        )
        return 2
    for archive_name in NLDM_ARCHIVES:
        archive = archive_dir / archive_name
        download(f"{ASAP7_SC7P5T28_RAW}/LIB/NLDM/{archive_name}", archive, args.force)
        extract_archive(seven_zip, archive, library_dir)

    lef = lef_dir / RVT_LEF
    download(f"{ASAP7_SC7P5T28_RAW}/LEF/{RVT_LEF}", lef, args.force)
    merged = library_dir / "asap7sc7p5t_RVT_TT_nldm_merged.lib"
    cell_counts = merge_libraries(library_dir, merged)
    manifest = {
        "schema": "cgra.asap7_cache.v1",
        "repository": ASAP7_REPOSITORY,
        "repository_revision": ASAP7_REPOSITORY_REVISION,
        "standard_cell_submodule": "asap7sc7p5t_28",
        "standard_cell_revision": ASAP7_SC7P5T28_REVISION,
        "corner": "RVT_TT",
        "liberty_model": "NLDM",
        "archives": [
            {
                "file": archive.name,
                "sha256": sha256(archive),
                "source": f"{ASAP7_SC7P5T28_RAW}/LIB/NLDM/{archive.name}",
            }
            for archive in (archive_dir / name for name in NLDM_ARCHIVES)
        ],
        "lef": {
            "file": lef.name,
            "sha256": sha256(lef),
            "source": f"{ASAP7_SC7P5T28_RAW}/LEF/{RVT_LEF}",
        },
        "merged_liberty": {"file": merged.name, "sha256": sha256(merged)},
        "cell_counts_by_source": cell_counts,
        "cell_count": sum(cell_counts.values()),
    }
    (root / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"ASAP7 cache ready: {root.relative_to(REPO_ROOT)}")
    print(f"merged Liberty: {merged.relative_to(REPO_ROOT)}")
    print(f"RVT LEF: {lef.relative_to(REPO_ROOT)}")
    print(f"cell count: {manifest['cell_count']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
