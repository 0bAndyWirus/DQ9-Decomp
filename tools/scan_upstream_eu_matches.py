#!/usr/bin/env python3
"""Compile USA upstream units and locate their functions in the extracted EU images."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import struct
from pathlib import Path
from typing import Sequence

import yaml

try:
    from .find_eu_matches import Module, scan_object
except ImportError:
    from find_eu_matches import Module, scan_object


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = ROOT / "extracted" / "dqix-decomp-eu"
USA_VERSION = "usa"
USA_FLAGS = [
    "-O2",
    "-enum", "int",
    "-char", "signed",
    "-str", "noreuse",
    "-proc", "arm946e",
    "-gccext,on",
    "-fp", "soft",
    "-inline", "noauto",
    "-lang=c++",
    "-Cpp_exceptions", "off",
    "-RTTI", "off",
    "-interworking",
    "-w", "off",
    "-sym", "on",
    "-gccinc",
    "-nolink",
    "-msgstyle", "gcc",
    "-str", "pool,reuse",
]


def load_eu_modules(project: Path) -> list[Module]:
    """Load configured ARM9 images and their metadata-supplied load addresses."""
    build = project / "build" / "eur" / "build"
    modules: list[Module] = []
    main_path = build / "arm9.bin"
    main_metadata = yaml.safe_load((build / "arm9.yaml").read_text(encoding="utf-8"))
    modules.append(Module("main", int(main_metadata["base_address"]), main_path, main_path.read_bytes()))

    for name in ("itcm", "dtcm"):
        image_path = build / f"{name}.bin"
        metadata_path = build / f"{name}.yaml"
        metadata = yaml.safe_load(metadata_path.read_text(encoding="utf-8"))
        modules.append(
            Module(name, int(metadata["base_address"]), image_path, image_path.read_bytes())
        )

    overlay_path = project / "extract" / "eur" / "arm9_overlays" / "overlays.yaml"
    overlays = yaml.safe_load(overlay_path.read_text(encoding="utf-8"))["overlays"]
    for overlay in overlays:
        name = f"ov{int(overlay['id']):03d}"
        image_path = build / f"arm9_{name}.bin"
        modules.append(
            Module(name, int(overlay["base_address"]), image_path, image_path.read_bytes())
        )
    return modules


def summarize_sources(rows: Sequence[dict[str, object]]) -> dict[str, object]:
    """Aggregate source compilation and unique byte-match counts."""
    summaries = [row.get("summary", {}) for row in rows]
    return {
        "source_count": len(rows),
        "compiled": sum(row.get("compile_returncode") == 0 for row in rows),
        "compile_failed": sum(row.get("compile_returncode") != 0 for row in rows),
        "scan_failed": sum("scan_error" in row for row in rows),
        "all_functions_unique_units": sum(
            bool(summary.get("all_functions_unique")) for summary in summaries
        ),
        "function_count": sum(int(summary.get("function_count", 0)) for summary in summaries),
        "uniquely_matched_functions": sum(
            int(summary.get("uniquely_matched_functions", 0)) for summary in summaries
        ),
        "uniquely_matched_function_bytes": sum(
            int(summary.get("uniquely_matched_function_bytes", 0)) for summary in summaries
        ),
    }


def compile_source(
    project: Path,
    source: Path,
    object_path: Path,
    wine: Path,
    compiler: Path,
) -> subprocess.CompletedProcess[str]:
    """Compile a source file with MWCC's USA game define and project flags."""
    source_arg = source.relative_to(project)
    object_arg = object_path.relative_to(project)
    command = [
        str(wine),
        str(compiler),
        *USA_FLAGS,
        "-i", str(project / "include"),
        "-d", USA_VERSION,
        "-MD", "-c", str(source_arg),
        "-o", str(object_arg),
    ]
    return subprocess.run(command, cwd=project, capture_output=True, text=True, check=False)


def scan_upstream(
    project: Path,
    source_root: Path,
    output_root: Path,
    modules: Sequence[Module],
    wine: Path,
    compiler: Path,
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for source in sorted(source_root.rglob("*.cpp")):
        relative = source.relative_to(source_root)
        object_path = output_root / relative.with_suffix(".o")
        object_path.parent.mkdir(parents=True, exist_ok=True)
        compiled = compile_source(project, source, object_path, wine, compiler)
        row: dict[str, object] = {
            "source": relative.as_posix(),
            "compile_returncode": compiled.returncode,
        }
        if compiled.returncode != 0 or not object_path.is_file():
            row["compile_error"] = (compiled.stdout + compiled.stderr).strip()
        else:
            try:
                match = scan_object(object_path, modules)
                row["summary"] = match["summary"]
                row["functions"] = match["functions"]
            except (OSError, ValueError, struct.error) as error:
                row["scan_error"] = str(error)
        rows.append(row)
    return rows


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Compile each src-upstream C++ file with MWCC's USA define, then scan its "
            "relocation-masked functions against all extracted EU ARM9 modules."
        )
    )
    parser.add_argument("--project-root", type=Path, default=DEFAULT_PROJECT,
                        help="EU decompilation project (default: extracted/dqix-decomp-eu)")
    parser.add_argument("--source-root", type=Path,
                        help="source directory (default: <project-root>/src-upstream)")
    parser.add_argument("--output", type=Path,
                        help="JSON report (default: <project-root>/build/eur/usa-scan/matches.json)")
    parser.add_argument("--wibo", type=Path, help="override the wibo executable")
    parser.add_argument("--compiler", type=Path, help="override mwccarm.exe")
    args = parser.parse_args(argv)

    project = args.project_root.resolve()
    source_root = (args.source_root or project / "src-upstream").resolve()
    output = (args.output or project / "build" / "eur" / "usa-scan" / "matches.json").resolve()
    wine = (args.wibo or project / "wibo").resolve()
    compiler = (args.compiler or project / "tools" / "mwccarm" / "2.0" / "sp2p2" / "mwccarm.exe").resolve()
    output_root = project / "build" / "eur" / "usa-objects"

    try:
        if not source_root.is_dir():
            raise ValueError(f"source directory does not exist: {source_root}")
        modules = load_eu_modules(project)
        rows = scan_upstream(project, source_root, output_root, modules, wine, compiler)
    except (OSError, ValueError, KeyError, TypeError, yaml.YAMLError) as error:
        print(f"scan_upstream_eu_matches.py: {error}", file=sys.stderr)
        return 2

    report = {
        "summary": summarize_sources(rows),
        "modules": [
            {"name": module.name, "base": module.base, "path": str(module.path)}
            for module in modules
        ],
        "sources": rows,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))
    print(f"Wrote {output}")
    return 1 if report["summary"]["compile_failed"] or report["summary"]["scan_failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
