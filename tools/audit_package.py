#!/usr/bin/env python3
"""Apply filename and size heuristics to a package directory or archive.

Checks directories, .zip, .tar and .tar.gz/.tgz files for suspicious names or
sizes. It does not inspect file contents or establish whether a package is
safe to publish; review its findings and the actual contents separately.

Exit status: 0 no heuristic findings, 1 findings present, 2 unreadable input.
"""

from __future__ import annotations

import argparse
import fnmatch
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


def classify(name: str, size: int) -> str | None:
    """Return a heuristic finding for this member, or None."""
    if any(part in IGNORED_PATH_PARTS for part in PurePosixPath(name).parts):
        return None
    base = PurePosixPath(name).name.lower()
    for pattern in FORBIDDEN_PATTERNS:
        if fnmatch.fnmatch(base, pattern):
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


def audit(path: Path) -> tuple[int, list[tuple[str, str]]]:
    total = 0
    findings = []
    for name, size in members(path):
        total += 1
        reason = classify(name, size)
        if reason:
            findings.append((name, reason))
    return total, findings


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("package", type=Path, nargs="+")
    args = parser.parse_args(argv)
    status = 0
    for package in args.package:
        try:
            total, findings = audit(package)
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
