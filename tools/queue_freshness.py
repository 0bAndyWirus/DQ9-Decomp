#!/usr/bin/env python3
"""Check whether a generated decomp queue is older than the state it was built from.

`decomp_queue.py` requires the objdiff report and DSD config (delinks/symbols/relocs).
Caller-supplied dispatch and handler catalogs are optional; only explicitly recorded or
overridden catalogs become dependencies. Newer dependencies are compared against the
queue's content fingerprints when available, otherwise by modification time.
This reads metadata only and never regenerates anything.

A missing required input makes the answer unknown, never "fresh".

Exit status: 0 fresh, 1 stale, 2 unknown (queue, report, catalog or config missing/unreadable).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROJECT = ROOT / "workspace"
DEFAULT_QUEUE = ROOT / "build" / "decomp-queue.json"
CONFIG_NAMES = ("config.yaml", "delinks.txt", "symbols.txt", "relocs.txt")


def recorded_catalogs(queue: Path, project: Path) -> dict[str, Path]:
    """Resolve explicitly recorded catalog paths relative to the project."""
    try:
        sources = json.loads(queue.read_text(encoding="utf-8")).get("catalog_sources") or {}
    except (OSError, ValueError, AttributeError):
        return {}
    found = {}
    for key in ("event_scripts", "script_tables"):
        value = sources.get(key)
        if isinstance(value, str) and value:
            path = Path(value)
            found[key] = path if path.is_absolute() else project / path
    return found


def required_inputs(project: Path, report: Path | None, event_catalog: Path | None,
                    table_catalog: Path | None) -> dict[str, Path]:
    inputs = {
        "report": report or project / "build" / "eur" / "report.json",
        "arm9 config": project / "config" / "eur" / "arm9" / "config.yaml",
    }
    if event_catalog is not None:
        inputs["event catalog"] = event_catalog
    if table_catalog is not None:
        inputs["script-table catalog"] = table_catalog
    return inputs


def dependencies(project: Path, inputs: dict[str, Path]) -> list[Path]:
    """Every file the queue was built from: the required inputs plus all DSD config files."""
    files = list(inputs.values())
    config_root = project / "config" / "eur"
    if config_root.is_dir():
        files += sorted(p for p in config_root.rglob("*") if p.is_file() and p.name in CONFIG_NAMES
                        and p not in files)
    return files


def missing_inputs(inputs: dict[str, Path]) -> list[str]:
    return [f"{label}: {path}" for label, path in inputs.items() if not path.is_file()]


def newer_than(queue: Path, files: Sequence[Path]) -> list[tuple[Path, float]]:
    """Dependencies modified after the queue, with how many seconds newer they are."""
    built = queue.stat().st_mtime
    return sorted(((path, path.stat().st_mtime - built) for path in files if path.stat().st_mtime > built),
                  key=lambda item: -item[1])


def recorded_fingerprints(queue: Path, project: Path) -> dict[str, dict]:
    """Fingerprints recorded by decomp_queue.py (`inputs.files`, paths relative to the project), by absolute path."""
    try:
        recorded = json.loads(queue.read_text(encoding="utf-8")).get("inputs", {}).get("files")
    except (OSError, ValueError, AttributeError):
        return {}
    if not isinstance(recorded, list):
        return {}
    return {str((project / item["path"]).resolve()): item for item in recorded
            if isinstance(item, dict) and isinstance(item.get("path"), str)}


def content_unchanged(path: Path, fingerprints: dict[str, dict]) -> bool:
    """True when the file's size and sha256 equal the queue's recorded fingerprint (a rewrite with the same bytes)."""
    entry = fingerprints.get(str(path.resolve()))
    if not entry or entry.get("sha256") is None:
        return False
    try:
        if path.stat().st_size != entry.get("size"):
            return False
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError:
        return False
    return digest.hexdigest() == entry["sha256"]


def changed_since(queue: Path, files: Sequence[Path], fingerprints: dict[str, dict]) -> list[tuple[Path, float]]:
    """Newer dependencies whose content really differs from what the queue was built from.

    A rollback or `configure` rewrites config files with identical bytes and a new mtime; judging by mtime alone
    would call the queue stale after every such rewrite. Files the queue has no fingerprint for stay mtime-judged.
    """
    return [(path, seconds) for path, seconds in newer_than(queue, files) if not content_unchanged(path, fingerprints)]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--queue", type=Path, default=DEFAULT_QUEUE)
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT)
    parser.add_argument("--report", type=Path, help="objdiff report (default: <project>/build/eur/report.json)")
    parser.add_argument("--event-catalog", type=Path, help="default: the queue's recorded path")
    parser.add_argument("--script-table-catalog", type=Path, help="default: the queue's recorded path")
    args = parser.parse_args(argv)
    if not args.queue.is_file():
        print(f"queue_freshness.py: unknown: queue not found: {args.queue}", file=sys.stderr)
        return 2
    recorded = recorded_catalogs(args.queue, args.project)
    inputs = required_inputs(args.project, args.report,
                             args.event_catalog or recorded.get("event_scripts"),
                             args.script_table_catalog or recorded.get("script_tables"))
    missing = missing_inputs(inputs)
    if missing:
        print("queue_freshness.py: unknown: a required input of decomp_queue.py is missing:", file=sys.stderr)
        for line in missing:
            print(f"  {line}", file=sys.stderr)
        return 2
    files = dependencies(args.project, inputs)
    stale = changed_since(args.queue, files, recorded_fingerprints(args.queue, args.project))
    age_hours = (time.time() - args.queue.stat().st_mtime) / 3600
    if not stale:
        print(f"fresh: {args.queue} ({age_hours:.1f} h old) is newer than {len(files)} dependency file(s)")
        return 0
    print(f"STALE: {args.queue} ({age_hours:.1f} h old); {len(stale)} of {len(files)} dependency file(s) are newer")
    for path, seconds in stale[:8]:
        try:
            shown = path.relative_to(args.project)
        except ValueError:
            shown = path
        print(f"  {shown}: {seconds / 3600:.1f} h newer")
    if len(stale) > 8:
        print(f"  ... {len(stale) - 8} more")
    print("regenerate with: .venv/bin/python tools/decomp_queue.py (only when the workspace is idle)")
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:  # consumer such as `head` closed the pipe
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        raise SystemExit(1)
