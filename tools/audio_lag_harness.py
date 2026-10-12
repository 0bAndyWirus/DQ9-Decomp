#!/usr/bin/env python3
"""Measure the real SDL frontend while replaying a scripted native run.

The default mode runs a 60–90 second window. ``--frames`` can replay a longer
script from frame zero; use the per-second frontend metrics to select a later
segment without replacing or shifting the recorded inputs.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

if __package__:
    from .host_tools import executable_name
else:
    from host_tools import executable_name

FRAME_RATE = 33_513_982.0 / 560_190.0
ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/game-fast/game" / executable_name("dq9"))
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--input-script", type=Path, required=True)
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--config-home", type=Path)
    parser.add_argument("--duration-seconds", type=int, choices=range(60, 91), default=75)
    parser.add_argument("--frames", type=int, help="run this many frames from script start instead of the short window")
    parser.add_argument("--audio-driver", help="set SDL_AUDIODRIVER (omit to use SDL's real default)")
    args = parser.parse_args()
    if args.frames is not None and args.frames <= 0:
        parser.error("--frames must be positive")

    for name, path in (("binary", args.binary), ("ROM", args.rom), ("input script", args.input_script)):
        if not path.is_file():
            parser.error(f"{name} does not exist: {path}")
    args.save.parent.mkdir(parents=True, exist_ok=True)
    if args.config_home is not None:
        args.config_home.mkdir(parents=True, exist_ok=True)

    frames = args.frames if args.frames is not None else round(args.duration_seconds * FRAME_RATE)
    expected_seconds = frames / FRAME_RATE
    command = [
        str(args.binary.resolve()),
        "--rom", str(args.rom.resolve()),
        "--save", str(args.save.resolve()),
        "--frames", str(frames),
        "--input-script", str(args.input_script.resolve()),
    ]
    env = os.environ.copy()
    env["DSR_AUDIO_METRICS"] = "1"
    if args.audio_driver:
        env["SDL_AUDIODRIVER"] = args.audio_driver
    if args.config_home is not None:
        env["APPDATA" if os.name == "nt" else "XDG_CONFIG_HOME"] = str(args.config_home.resolve())

    driver = env.get("SDL_AUDIODRIVER", "SDL default")
    print(
        f"[audio-harness] frames={frames} expected_wall_seconds={expected_seconds:.3f} "
        f"frame_rate={FRAME_RATE:.6f} requested_audio_driver={driver}",
        flush=True,
    )
    if driver == "dummy":
        print(
            "[audio-harness] SDL dummy audio drains synthetically; it cannot demonstrate audible playback, "
            "hardware buffering, or real underrun behavior.",
            flush=True,
        )
    print(f"[audio-harness] command={shlex.join(command)}", flush=True)
    started = time.monotonic()
    result = subprocess.run(command, env=env, check=False)
    elapsed = time.monotonic() - started
    print(
        f"[audio-harness] exit={result.returncode} elapsed_wall_seconds={elapsed:.3f} "
        f"requested_seconds={args.duration_seconds if args.frames is None else 'full-script'} frames={frames}",
        flush=True,
    )
    if result.returncode != 0:
        return result.returncode
    if args.frames is None and not 60.0 <= elapsed <= 90.0:
        print("[audio-harness] ERROR: measured run did not last between 60 and 90 seconds", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
