#!/usr/bin/env python3
"""Apply filename and size heuristics to a package directory or archive.

Checks directories, .zip, .tar and .tar.gz/.tgz files for suspicious names or
sizes. It does not inspect file contents or establish whether a package is
safe to publish; review its findings and the actual contents separately.
The optional --source-tree allowlist permits HEAD-tracked original C++ sources
for source kits while forbidding generated-source names and binary/private data.

Exit status: 0 no heuristic findings, 1 findings present, 2 unreadable input.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import sys
import tarfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Iterable, Sequence

# Filename patterns that may indicate local images or generated guest C++.
FORBIDDEN_PATTERNS = (
    "*.nds", "*.srl", "*.dsi", "*.ids",
    "arm9.bin", "arm7.bin", "*.arm9", "*.arm7",
    "fn_table*.cpp", "*.manifest.json",
    "*.cpp", "*.cc", "*.cxx",
)
# Build-system scratch (e.g. CMake's compiler-id probe) is not guest code.
IGNORED_PATH_PARTS = ("CMakeFiles",)
# Flag unusually large files unless their extension denotes a binary/archive.
MAX_PLAIN_FILE_BYTES = 64 * 1024 * 1024
SOURCE_EXTENSIONS = {".cpp", ".cc", ".cxx"}
KIT_BINARY_EXTENSIONS = {".nds", ".sav", ".bin", ".exe", ".dll"}


def classify(name: str, size: int, source_paths: set[str] | None = None) -> str | None:
    """Return a heuristic finding for this member, or None."""
    path = PurePosixPath(name)
    if source_paths is not None:
        if path.suffix.lower() in KIT_BINARY_EXTENSIONS:
            return "binary or private data is forbidden in a source kit"
        if fnmatch.fnmatch(path.name.lower(), "chunk_*.cpp"):
            return "generated game source is forbidden in a source kit"
    if source_paths is None and any(part in IGNORED_PATH_PARTS for part in path.parts):
        return None
    base = PurePosixPath(name).name.lower()
    for pattern in FORBIDDEN_PATTERNS:
        if fnmatch.fnmatch(base, pattern):
            if (source_paths is not None and pattern in ("*.cpp", "*.cc", "*.cxx")
                    and name in source_paths and path.suffix.lower() in SOURCE_EXTENSIONS):
                continue
            return f"matches forbidden pattern {pattern}"
    if size > MAX_PLAIN_FILE_BYTES and not base.endswith((".exe", ".dll", ".so", ".zip", ".gz", ".tgz", ".tar")) and "." in base:
        return f"unexpectedly large ({size} bytes)"
    return None


def members(path: Path) -> Iterable[tuple[str, int]]:
    if path.is_dir():
        for item in sorted(path.rglob("*")):
            if item.is_file():
                yield item.relative_to(path).as_posix(), item.stat().st_size
    elif zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                if not info.is_dir():
                    yield info.filename, info.file_size
    elif tarfile.is_tarfile(path):
        with tarfile.open(path) as archive:
            for info in archive.getmembers():
                if info.isfile():
                    yield info.name, info.size
    else:
        raise OSError(f"{path}: not a directory, zip or tar archive")


def audit(path: Path, source_paths: set[str] | None = None) -> tuple[int, list[tuple[str, str]]]:
    total = 0
    findings = []
    for name, size in members(path):
        total += 1
        reason = classify(name, size, source_paths)
        if reason:
            findings.append((name, reason))
    return total, findings


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("package", type=Path, nargs="+")
    parser.add_argument("--source-tree", type=Path, metavar="HEAD_PATHS_JSON",
                        help="allow C++ sources listed in a JSON array of HEAD-tracked paths; "
                             "generated names and private/binary data remain forbidden")
    args = parser.parse_args(argv)
    source_paths = None
    if args.source_tree is not None:
        try:
            paths = json.loads(args.source_tree.read_text(encoding="utf-8"))
            if not isinstance(paths, list) or any(not isinstance(path, str) for path in paths):
                raise ValueError("expected a JSON array of source paths")
            source_paths = set(paths)
        except (OSError, ValueError) as error:
            print(f"audit_package.py: {error}", file=sys.stderr)
            return 2
    status = 0
    for package in args.package:
        try:
            total, findings = audit(package, source_paths)
        except (OSError, zipfile.BadZipFile, tarfile.TarError) as error:
            print(f"audit_package.py: {error}", file=sys.stderr)
            return 2
        print(f"{package}: {total} file(s), {len(findings)} forbidden")
        for name, reason in findings[:50]:
            print(f"  {name}: {reason}")
        if len(findings) > 50:
            print(f"  ... {len(findings) - 50} more")
        if findings:
            status = 1
    return status


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:  # consumer such as `head` closed the pipe
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        raise SystemExit(1)
