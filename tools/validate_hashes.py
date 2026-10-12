#!/usr/bin/env python3
"""Validate per-frame screen-hash files before comparing them.

`compare_hashes.py` and `align_hashes.py` silently skip malformed rows, keep the last of
duplicated frames, and compare only frames both files have. This checks each file
strictly (rows `frame top_fnv1a64 bottom_fnv1a64`, hex digests) and, with a second file,
reports which frames only one side has, so a "match" over a small intersection is visible.

Exit status: 0 valid, 1 problems found, 2 unreadable file.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Sequence

_HEX = re.compile(r"[0-9a-fA-F]{1,16}\Z")


def parse_hash_file(text: str) -> tuple[dict[int, tuple[str, str]], list[str]]:
    """Return rows by frame and a list of human-readable problems (line numbers are 1-based)."""
    rows: dict[int, tuple[str, str]] = {}
    problems: list[str] = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        parts = line.split()
        if len(parts) != 3:
            problems.append(f"line {number}: expected 3 fields, got {len(parts)}")
            continue
        if not parts[0].isdigit():
            problems.append(f"line {number}: frame index {parts[0]!r} is not a non-negative integer")
            continue
        if not (_HEX.match(parts[1]) and _HEX.match(parts[2])):
            problems.append(f"line {number}: digests must be 1-16 hex digits")
            continue
        frame = int(parts[0])
        if frame in rows:
            problems.append(f"line {number}: duplicate frame {frame}")
        rows[frame] = (parts[1].lower(), parts[2].lower())
    return rows, problems


def missing_ranges(frames: Sequence[int]) -> list[tuple[int, int]]:
    """Inclusive ranges of absent frame indices between the first and last present frame."""
    ordered = sorted(set(frames))
    gaps = []
    for before, after in zip(ordered, ordered[1:]):
        if after > before + 1:
            gaps.append((before + 1, after - 1))
    return gaps


def describe(name: str, rows: dict[int, tuple[str, str]]) -> list[str]:
    if not rows:
        return [f"{name}: no rows"]
    frames = sorted(rows)
    gaps = missing_ranges(frames)
    lines = [f"{name}: {len(rows)} frames, {frames[0]}..{frames[-1]}, {len(gaps)} gap(s)"]
    lines += [f"  missing {a}..{b}" if a != b else f"  missing {a}" for a, b in gaps[:10]]
    if len(gaps) > 10:
        lines.append(f"  ... {len(gaps) - 10} more gap(s)")
    return lines


def coverage_difference(first: dict[int, tuple[str, str]], second: dict[int, tuple[str, str]]) -> dict[str, list[int]]:
    return {
        "only_first": sorted(first.keys() - second.keys()),
        "only_second": sorted(second.keys() - first.keys()),
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("hashes", type=Path, nargs="+", help="one hash file, or two to compare coverage")
    parser.add_argument("--allow-gaps", action="store_true", help="do not treat missing frames inside a file as a problem")
    args = parser.parse_args(argv)
    if len(args.hashes) > 2:
        parser.error("give one or two hash files")
    parsed = []
    status = 0
    for path in args.hashes:
        try:
            rows, problems = parse_hash_file(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError) as error:
            print(f"validate_hashes.py: {path}: {error}", file=sys.stderr)
            return 2
        parsed.append(rows)
        print("\n".join(describe(str(path), rows)))
        for problem in problems[:20]:
            print(f"  {problem}")
        if len(problems) > 20:
            print(f"  ... {len(problems) - 20} more problem(s)")
        if problems or not rows or (not args.allow_gaps and missing_ranges(list(rows))):
            status = 1
    if len(parsed) == 2:
        difference = coverage_difference(parsed[0], parsed[1])
        common = len(parsed[0].keys() & parsed[1].keys())
        print(f"common frames: {common}; only in first: {len(difference['only_first'])}; "
              f"only in second: {len(difference['only_second'])}")
        if difference["only_first"] or difference["only_second"]:
            status = 1
    return status


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:  # consumer such as `head` closed the pipe
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        raise SystemExit(1)
