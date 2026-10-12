#!/usr/bin/env python3
"""Remove regenerable probe artifacts from the ignored build/ directory.

Dry run by default: prints what would be removed and the space it frees.
Pass --apply to delete. Anything a stage table, input script or tool
depends on is kept (see KEEP and the paths referenced by reference/desmume-trace/stages*.json).
Never touches ROM/, extracted/ or reference/.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Top-level names (relative to build/) that must survive.
KEEP = {
    "gen", "game", "game-fast", "game-trace-fast", "mrc", "arm7-decomp", "arm7cov",
    "playthrough-oracle", "playthrough-oracle-late", "playthrough-fresh.sav",
    "decomp-queue.frozen.json", "event-scripts.json", "script-tables.json",
    "resources.json", "integrate.lock", "freerun", "windows", "deck", "scratch",
}
# Explicitly excluded even if a future junk pattern matches one of these names.
PROTECTED = KEEP | {"author-batches", "eur", "oracle", "parity"}


def _build_dir() -> Path:
    build = ROOT / "build"
    resolved_root = ROOT.resolve()
    resolved_build = build.resolve()
    if build.is_symlink() or resolved_build != resolved_root / "build":
        raise ValueError(f"refusing build directory outside project build/: {build} -> {resolved_build}")
    return resolved_build


def _validate_days(value: str) -> float:
    try:
        days = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("days must be a non-negative number") from error
    if not math.isfinite(days) or days < 0:
        raise argparse.ArgumentTypeError("days must be a finite non-negative number")
    return days


def _contained(path: Path, build: Path) -> bool:
    try:
        path.resolve().relative_to(build)
        return True
    except ValueError:
        return False


def candidates(days: float) -> list[tuple[Path, int]]:
    if days < 0:
        raise ValueError("days must be non-negative")
    build = _build_dir()
    if not build.is_dir():
        return []
    keep = PROTECTED | referenced_paths()
    cutoff = time.time() - days * 86400
    junk = [re.compile(p) for p in JUNK_PATTERNS]
    found = []
    for entry in sorted(build.iterdir()):
        if not _contained(entry, build):
            raise ValueError(f"refusing path outside build directory: {entry} -> {entry.resolve()}")
        if entry.name in keep:
            continue
        if entry.lstat().st_mtime > cutoff:
            continue
        if any(rx.fullmatch(entry.name) for rx in junk):
            found.append((entry, size_of(entry)))
    return found
# Probe leftovers: removed when older than --days.
JUNK_PATTERNS = [
    r".*\.log$", r".*\.sav$", r".*-fatal-ram\.bin$", r".*-ram\.bin$", r".*\.trace$",
    r".*-frames$", r".*-native$", r".*-oracle$", r".*-calls\.txt$", r".*_calls\.txt$",
    r".*-trace\.txt$", r".*\.ppm$", r".*\.png$", r".*\.wav$", r".*\.prof$",
    r"resources-(before|after)-.*\.json$", r"decomp-queue\.(pre-refresh)?\.?json$",
    r"x-.*", r"r3d.*", r"pd[0-9]+", r"pt[0-9]+", r"rr[0-9]+", r"gx-.*", r"vram.*",
    r"powcnt-.*", r"final-.*", r"native-.*", r"menu-.*", r"fifo-.*", r"regress-.*",
    r"render3d-.*", r"character_creation-.*", r"cc-.*", r"stages-bisect.*",
    r"playthrough-(bisect|boot|final|native|regs|stage).*",
    r"game-trace$", r"game-check$", r"dq9_foundation",
]


def referenced_paths() -> set[str]:
    keep: set[str] = set()
    for table in (ROOT / "reference" / "desmume-trace").glob("stages*.json"):
        for match in re.findall(r'build/([^"/]+)', table.read_text(encoding="utf-8", errors="replace")):
            keep.add(match)
    return keep


def size_of(path: Path) -> int:
    if path.is_file() or path.is_symlink():
        return path.lstat().st_size
    return sum(p.lstat().st_size for p in path.rglob("*") if p.is_file() or p.is_symlink())

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--apply", action="store_true", help="delete (default: list only)")
    parser.add_argument("--days", type=_validate_days, default=1.0, help="only entries older than this many days")
    args = parser.parse_args(argv)
    try:
        found = candidates(args.days)
    except ValueError as error:
        parser.error(str(error))
    total = sum(size for _, size in found)
    for path, size in sorted(found, key=lambda item: -item[1])[:40]:
        print(f"{size / 1e6:9.1f} MB  {path.relative_to(ROOT)}")
    print(f"{len(found)} entries, {total / 1e9:.2f} GB {'removed' if args.apply else 'would be removed'}")
    if args.apply:
        for path, _ in found:
            shutil.rmtree(path) if path.is_dir() and not path.is_symlink() else path.unlink()
    return 0



if __name__ == "__main__":
    sys.exit(main())
