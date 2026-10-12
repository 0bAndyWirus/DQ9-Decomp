#!/usr/bin/env python3
"""Run the native build with an input script and report per-frame diffs against oracle PNGs.

Usage (.venv/bin/python from the repository root):

    .venv/bin/python tools/parity_run.py --native-script build/freerun/opening-advance-held12.txt \\
        --frames 6501 --dump-frames 1200,1900,3000 --out build/parity/base \\
        --oracle-dir build/freerun/opening-held12-oracle

Runs `dq9 --headless`, dumps requested frames into --out, then prints one line per
frame/screen. Missing captures are summarized; `--strict` turns any missing native
or oracle capture into exit status 1. Diffing is tools/compare_frames.py.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from compare_frames import ImageError, compare, read_image
from host_tools import executable_name

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_NATIVE = ROOT / "build" / "game-fast" / "game" / executable_name("dq9")


def _default_rom() -> str:
    if os.environ.get("DQ9_ROM"):
        return os.environ["DQ9_ROM"]
    roms = sorted((ROOT / "ROM").glob("*.nds"))
    if not roms:
        raise SystemExit("error: no ROM: pass --rom, set DQ9_ROM, or place a .nds in ROM/")
    return str(roms[0])


def _parse_frames(text: str) -> list[int]:
    tokens = text.split(",")
    if not tokens or any(not part.strip() for part in tokens):
        raise argparse.ArgumentTypeError("frames must be a non-empty comma-separated list")
    frames: list[int] = []
    seen: set[int] = set()
    for part in tokens:
        token = part.strip()
        if not token.isdigit():
            raise argparse.ArgumentTypeError(f"invalid frame number {token!r}")
        frame = int(token)
        if frame in seen:
            raise argparse.ArgumentTypeError(f"duplicate frame {frame}")
        seen.add(frame)
        frames.append(frame)
    return frames

def describe(native: Path, oracle: Path) -> str:
    if not native.exists():
        return "no native dump"
    if not oracle.exists():
        return "no oracle"
    try:
        result = compare(read_image(native), read_image(oracle))
    except ImageError as error:
        return f"error: {error}"
    return (f"{result['different']}/{result['pixels']} maxdelta {result['max_delta']} "
            f"bbox {result['bbox']}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-script", type=Path, required=True,
                        help="native input script (frame button_mask touch_x touch_y lines)")
    parser.add_argument("--frames", type=int, required=True, help="frame count passed to dq9 --frames")
    parser.add_argument("--dump-frames", type=_parse_frames, required=True, help="comma-separated frame numbers")
    parser.add_argument("--out", type=Path, required=True, help="dump directory")
    parser.add_argument("--oracle-dir", type=Path, help="directory with frame_NNNN_{top,bottom}.png")
    parser.add_argument("--native", type=Path, default=DEFAULT_NATIVE)
    parser.add_argument("--rom", default=None)
    parser.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                        help="further arguments appended to the dq9 command (e.g. --dump-ram)")
    parser.add_argument("--strict", action="store_true",
                        help="return nonzero if any requested native or oracle capture is missing")
    args = parser.parse_args(argv)
    if args.frames < 1:
        parser.error("--frames must be positive")
    frames = args.dump_frames
    args.out.mkdir(parents=True, exist_ok=True)
    # dq9 only writes frames it reaches; stale dumps from an earlier run must not be compared.
    for frame in frames:
        for screen in ("top", "bottom"):
            (args.out / f"frame_{frame:04d}_{screen}.ppm").unlink(missing_ok=True)
    command = [
        str(args.native), "--rom", args.rom or _default_rom(), "--headless",
        "--frames", str(args.frames), "--input-script", str(args.native_script),
        "--dump-frames", ",".join(map(str, frames)), "--dump-dir", str(args.out),
        *args.extra,
    ]
    log = args.out / "run.log"
    with log.open("wb") as handle:
        code = subprocess.run(command, stdout=handle, stderr=subprocess.STDOUT).returncode
    if code != 0:
        print(f"error: dq9 exited {code} (log: {log})", file=sys.stderr)
        return 2
    missing_native: list[str] = []
    missing_oracle: list[str] = []
    for frame in frames:
        for screen in ("top", "bottom"):
            native = args.out / f"frame_{frame:04d}_{screen}.ppm"
            oracle = args.oracle_dir / f"frame_{frame:04d}_{screen}.png" if args.oracle_dir else Path("/nonexistent")
            result = describe(native, oracle)
            if not native.exists():
                missing_native.append(f"{frame}/{screen}")
            if not oracle.exists():
                missing_oracle.append(f"{frame}/{screen}")
            print(f"frame {frame} {screen}: {result}")
    def listed(items: list[str], limit: int = 20) -> str:
        shown = ", ".join(items[:limit]) or "none"
        return shown + (f", ... {len(items) - limit} more" if len(items) > limit else "")

    print(f"missing captures: native {len(missing_native)} [{listed(missing_native)}]; "
          f"oracle {len(missing_oracle)} [{listed(missing_oracle)}]")
    return 1 if args.strict and (missing_native or missing_oracle) else 0


if __name__ == "__main__":
    raise SystemExit(main())
