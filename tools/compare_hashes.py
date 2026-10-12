#!/usr/bin/env python3
"""Compare two per-frame screen-hash files and report where they first diverge.

Each file has rows `frame top_fnv1a64 bottom_fnv1a64` (hex digests), as written by
`dq9 --hash-file` and `tools/oracle/run_oracle.py --hashes`. The tool prints, per
screen, the first mismatching frame, the number of mismatching frames and the
mismatching frame ranges (collapsed), so a long scripted run can be bisected to the
frames worth dumping and diffing with tools/compare_frames.py. Malformed or unreadable
inputs exit 2; mismatches and asymmetric frame coverage exit 1. `--allow-partial` allows
asymmetric coverage and compares only common frames.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from .validate_hashes import coverage_difference, parse_hash_file
except ImportError:  # Direct execution as tools/compare_hashes.py.
    from validate_hashes import coverage_difference, parse_hash_file


def read_hashes(path: Path) -> tuple[dict[int, tuple[str, str]], list[str]]:
    try:
        text = path.read_text()
    except (OSError, UnicodeError) as exc:
        raise ValueError(f"{path}: {exc}") from exc
    rows, problems = parse_hash_file(text)
    if not rows:
        problems.append("file contains no hash rows")
    return rows, [f"{path}: {problem}" for problem in problems]


def ranges(frames: list[int]) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for frame in frames:
        if out and frame == out[-1][1] + 1:
            out[-1] = (out[-1][0], frame)
        else:
            out.append((frame, frame))
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("native", type=Path)
    parser.add_argument("oracle", type=Path)
    parser.add_argument("--first", type=int, default=0, help="ignore frames before this index")
    parser.add_argument("--last", type=int, default=None, help="ignore frames after this index")
    parser.add_argument("--max-ranges", type=int, default=20, help="mismatch ranges to print per screen")
    parser.add_argument("--allow-partial", action="store_true",
                        help="allow asymmetric frame coverage; compare frames present in both files")
    args = parser.parse_args(argv)
    if args.first < 0 or (args.last is not None and args.last < args.first):
        parser.error("--first must be non-negative and --last must be >= --first")
    if args.max_ranges < 0:
        parser.error("--max-ranges must be non-negative")
    try:
        native, native_problems = read_hashes(args.native)
        oracle, oracle_problems = read_hashes(args.oracle)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    problems = native_problems + oracle_problems
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 2
    within = lambda f: f >= args.first and (args.last is None or f <= args.last)
    native = {f: h for f, h in native.items() if within(f)}
    oracle = {f: h for f, h in oracle.items() if within(f)}
    differences = coverage_difference(native, oracle)
    coverage_mismatch = bool(differences["only_first"] or differences["only_second"])
    if coverage_mismatch:
        for label, key in (("native", "only_first"), ("oracle", "only_second")):
            frames = differences[key]
            if frames:
                spans = ranges(frames)
                shown = ", ".join(f"{a}..{b}" if a != b else str(a) for a, b in spans[:args.max_ranges])
                more = f", ... {len(spans) - args.max_ranges} more range(s)" if len(spans) > args.max_ranges else ""
                print(f"frames only in {label}: {len(frames)} in {len(spans)} range(s): {shown}{more}", file=sys.stderr)
    common = sorted(native.keys() & oracle.keys())
    if not common:
        print("no common frames", file=sys.stderr)
        return 1
    print(f"compared {len(common)} frames ({common[0]}..{common[-1]})")
    diverged = False
    for index, name in enumerate(("top", "bottom")):
        bad = [f for f in common if native[f][index] != oracle[f][index]]
        if not bad:
            print(f"{name}: all frames identical")
            continue
        diverged = True
        spans = ranges(bad)
        print(f"{name}: first mismatch at frame {bad[0]}; {len(bad)} mismatching frames in {len(spans)} range(s)")
        for first, last in spans[: args.max_ranges]:
            print(f"  {first}..{last}" if first != last else f"  {first}")
        if len(spans) > args.max_ranges:
            print(f"  ... {len(spans) - args.max_ranges} more")
    both = [f for f in common if native[f] != oracle[f]]
    if both:
        print(f"first frame where either screen differs: {both[0]}")
    return 1 if diverged or (coverage_mismatch and not args.allow_partial) else 0




if __name__ == "__main__":
    raise SystemExit(main())
