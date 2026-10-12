#!/usr/bin/env python3
"""Headless unthrottled emulation speed per scene range.

Usage: tools/bench_render.py --image PATH --save PATH --input-script PATH
    --scene NAME:START:END [--scene NAME:START:END ...] [--binary PATH]
    [--out FILE] [--cpu N]
Use --cpu -1 for an unpinned run (including multithreaded rendering).
Runs the supplied input from frame 0 up to the end of each caller-defined range
(the guest state needs the whole prefix) and times only the final range by
differencing two runs: fps(range) = frames_in_range / (t(end) - t(start)).
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

if __package__:
    from .host_tools import executable_name, run_with_affinity
else:
    from host_tools import executable_name, run_with_affinity

ROOT = Path(__file__).resolve().parents[1]


def scene_range(value: str) -> tuple[str, int, int]:
    try:
        name, start, stop = value.split(":")
        begin, end = int(start), int(stop)
        if not name.strip() or not 0 <= begin < end:
            raise ValueError
    except ValueError:
        raise argparse.ArgumentTypeError("scene must be NAME:START:END with 0 <= START < END") from None
    return name, begin, end


def run(binary: Path, frames: int, cpu: int | None, *,
        image: Path, save: Path, input_script: Path) -> float:
    with tempfile.TemporaryDirectory(prefix="dq9-bench-") as tmp:
        fresh_save = Path(tmp) / "s.sav"
        shutil.copy(save, fresh_save)
        cmd = [str(binary), "--headless", "--rom", str(image), "--save", str(fresh_save), "--frames", str(frames),
               "--input-script", str(input_script), "--rtc-epoch", "0", "--rtc-timezone-offset", "0"]
        start = time.perf_counter()
        run_with_affinity(cmd, cpu, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        return time.perf_counter() - start


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build/game-ui/game" / executable_name("dq9")))
    parser.add_argument("--out", default="")
    parser.add_argument("--cpu", type=int, default=5, help="CPU affinity; -1 disables pinning (default 5)")
    parser.add_argument("--input-script", type=Path, required=True)
    parser.add_argument("--save", type=Path, required=True, help="save copied fresh for each timing run")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--scene", type=scene_range, action="append", required=True,
                        help="caller-defined NAME:START:END range; repeat for multiple scenes")
    args = parser.parse_args(argv)
    binary = Path(args.binary)
    for label, path in (("binary", binary), ("image", args.image),
                        ("save", args.save), ("input script", args.input_script)):
        if not path.is_file():
            print(f"missing {label}: {path}", file=sys.stderr)
            return 2
    lines = [f"binary {binary}", f"{'range':10s} {'frames':>14s} {'fps':>8s}"]
    for name, begin, end in args.scene:
        inputs = {"image": args.image, "save": args.save, "input_script": args.input_script}
        t_end = run(binary, end, args.cpu, **inputs)
        t_begin = run(binary, begin, args.cpu, **inputs) if begin else 0.0
        fps = (end - begin) / max(t_end - t_begin, 1e-9)
        lines.append(f"{name:10s} {begin:>6d}-{end:<7d} {fps:8.1f}")
    text = "\n".join(lines)
    print(text)
    if args.out:
        Path(args.out).write_text(text + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
