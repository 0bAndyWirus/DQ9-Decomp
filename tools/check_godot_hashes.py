#!/usr/bin/env python3
"""Compare Godot and native dq9 per-frame RGB FNV hash streams."""
from __future__ import annotations

import argparse
from pathlib import Path


def parse(path: Path) -> list[tuple[int, int, int]]:
    rows = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        fields = line.split()
        if len(fields) != 3:
            raise ValueError(f"{path}:{number}: expected frame and two hashes")
        rows.append((int(fields[0]), int(fields[1], 16) & 0xFFFFFFFFFFFFFFFF,
                     int(fields[2], 16) & 0xFFFFFFFFFFFFFFFF))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("native", type=Path)
    parser.add_argument("godot", type=Path)
    parser.add_argument("--frames", type=int, default=3000)
    args = parser.parse_args()
    native, godot = parse(args.native), parse(args.godot)
    if len(native) != args.frames or len(godot) != args.frames:
        raise SystemExit(f"hash count mismatch: native={len(native)}, Godot={len(godot)}, expected={args.frames}")
    for index, (expected, actual) in enumerate(zip(native, godot)):
        if expected != actual:
            raise SystemExit(f"frame {index} mismatch: native={expected[1]:016x}/{expected[2]:016x} Godot={actual[1]:016x}/{actual[2]:016x}")
    print(f"PASS: {args.frames} frames; top and bottom RGB hashes match native dq9")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
