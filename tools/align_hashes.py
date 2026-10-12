#!/usr/bin/env python3
"""Align a native run to an oracle run when their timelines drift.

Both emulators run the same input script, but boot, loading and transition timing
differ by a few to tens of frames (their CPU timing models differ), so equal frame
indices rarely show the same content. Given two per-frame hash files (rows
`frame top_fnv1a64 bottom_fnv1a64`, from `dq9 --hash-file` and the oracle's
`--hashes`) this tool finds a monotone piecewise-constant frame offset
`native = oracle + offset` that maximises the number of identical screens
(banded dynamic programming; each offset change costs `--penalty` matches) and
prints the segments with their offsets and match rates. A segment whose match rate
is below 100% marks frames whose content really differs (rendering or logic), not
just timing. Malformed files, non-contiguous frame IDs (including a nonzero first
frame), invalid ranges, and non-positive penalties are rejected.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path


try:
    from .validate_hashes import parse_hash_file
except ImportError:  # Direct execution as tools/align_hashes.py.
    from validate_hashes import parse_hash_file

def read_hashes(path: Path) -> list[tuple[str, str]]:
    try:
        text = path.read_text()
    except (OSError, UnicodeError) as exc:
        raise ValueError(f"{path}: {exc}") from exc
    rows, problems = parse_hash_file(text)
    if problems:
        raise ValueError("\n".join(f"{path}: {problem}" for problem in problems))
    if not rows:
        raise ValueError(f"{path}: no rows")
    ordered = sorted(rows)
    if ordered[0] != 0 or any(current != previous + 1 for previous, current in zip(ordered, ordered[1:])):
        raise ValueError(f"{path}: frame indices must be contiguous starting at 0")
    return [rows[i] for i in ordered]



def _positive_float(text: str) -> float:
    try:
        value = float(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a positive finite number") from exc
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be a positive finite number")
    return value
def align(native: list[tuple[str, str]], oracle: list[tuple[str, str]], band: int, penalty: float,
          first: int, last: int) -> list[tuple[int, int, int]]:
    if band < 0:
        raise ValueError("band must be non-negative")
    if not math.isfinite(penalty) or penalty <= 0:
        raise ValueError("penalty must be positive and finite")
    if not native or not oracle:
        raise ValueError("hash inputs must not be empty")
    if first < 0 or last < first or last >= len(oracle):
        raise ValueError(f"invalid oracle frame range {first}..{last} for {len(oracle)} frames")
    width = 2 * band + 1
    neg = float("-inf")
    count = last - first + 1
    score = [[neg] * width for _ in range(count)]
    back = [[0] * width for _ in range(count)]

    def match(f: int, d: int) -> int:
        g = f + d
        if g < 0 or g >= len(native):
            return 0
        return (native[g][0] == oracle[f][0]) + (native[g][1] == oracle[f][1])

    for k in range(width):
        score[0][k] = match(first, k - band) - abs(k - band) * 0.01
    for i in range(1, count):
        f = first + i
        prev = score[i - 1]
        row = score[i]
        for k in range(width):
            best = prev[k]
            arg = k
            lo = max(0, k - 1)
            hi = min(width - 1, k + 1)
            for j in range(lo, hi + 1):
                if j == k:
                    continue
                cand = prev[j] - penalty
                if cand > best:
                    best, arg = cand, j
            if best == neg:
                continue
            row[k] = best + match(f, k - band)
            back[i][k] = arg
    k = max(range(width), key=lambda x: score[count - 1][x])
    offsets = [0] * count
    for i in range(count - 1, -1, -1):
        offsets[i] = k - band
        k = back[i][k]
    segments: list[tuple[int, int, int]] = []
    start = 0
    for i in range(1, count + 1):
        if i == count or offsets[i] != offsets[start]:
            segments.append((first + start, first + i - 1, offsets[start]))
            start = i
    return segments


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("native", type=Path)
    parser.add_argument("oracle", type=Path)
    parser.add_argument("--first", type=int, default=0, help="first oracle frame")
    parser.add_argument("--last", type=int, default=None, help="last oracle frame")
    parser.add_argument("--band", type=int, default=60, help="max |offset| in frames")
    parser.add_argument("--penalty", type=_positive_float, default=3.0, help="matches lost per offset step")
    parser.add_argument("--min-length", type=int, default=1, help="hide segments shorter than this")
    args = parser.parse_args(argv)
    if args.first < 0 or args.band < 0 or args.min_length < 1:
        parser.error("--first and --band must be non-negative; --min-length must be positive")
    try:
        native = read_hashes(args.native)
        oracle = read_hashes(args.oracle)
    except ValueError as exc:
        parser.error(str(exc))
    last = args.last if args.last is not None else len(oracle) - 1
    if last < args.first or last >= len(oracle):
        parser.error(f"invalid oracle frame range {args.first}..{last} for {len(oracle)} frames")
    segments = align(native, oracle, args.band, args.penalty, args.first, last)
    total_top = total_bottom = total = 0
    print("oracle frames      offset  top%   bottom%  (offset = native frame - oracle frame)")
    for first, last_frame, offset in segments:
        top = bottom = 0
        length = last_frame - first + 1
        for f in range(first, last_frame + 1):
            g = f + offset
            if 0 <= g < len(native):
                top += native[g][0] == oracle[f][0]
                bottom += native[g][1] == oracle[f][1]
        total_top += top
        total_bottom += bottom
        total += length
        if length >= args.min_length:
            print(f"{first:6d}..{last_frame:<6d}  {offset:+5d}  {100 * top / length:5.1f}  {100 * bottom / length:6.1f}")
    print(f"overall: top {100 * total_top / total:.1f}% bottom {100 * total_bottom / total:.1f}% identical "
          f"over {total} oracle frames in {len(segments)} segment(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
