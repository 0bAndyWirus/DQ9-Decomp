#!/usr/bin/env python3
"""Check caller-selected frames across repeated renderer thread/affinity runs.

Every run starts from a fresh save copy, uses caller-supplied input and fixed
RTC, and compares both screens with a supplied baseline hash file. Logs and
hashes stay in the chosen local output directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

try:
    from .compare_hashes import read_hashes
    from .host_tools import executable_name, run_with_affinity
except ImportError:
    from compare_hashes import read_hashes
    from host_tools import executable_name, run_with_affinity

ROOT = Path(__file__).resolve().parents[1]
THREADS = (0, 1, 2, 6)




def checked_hashes(path: Path, frames: int) -> dict[int, tuple[str, str]]:
    rows, problems = read_hashes(path)
    if problems:
        raise ValueError("\n".join(problems))
    if set(rows) != set(range(frames)):
        raise ValueError(f"{path}: expected exactly frames 0..{frames - 1}, got {len(rows)} rows")
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/game-ui/game" / executable_name("dq9"))
    parser.add_argument("--rom", type=Path, help="already locally available compatible image (or DQ9_ROM)")
    parser.add_argument("--save", type=Path, required=True, help="local source save copied for each run")
    parser.add_argument("--input", type=Path, required=True, help="local input script")
    parser.add_argument("--baseline", type=Path, required=True, help="local expected screen hashes")
    parser.add_argument("--frames", type=int, required=True, help="number of frames to compare")
    parser.add_argument("--out", type=Path, default=ROOT / "build/determinism-check")
    args = parser.parse_args(argv)
    if args.frames <= 0:
        parser.error("--frames must be positive")
    rom = args.rom or (Path(os.environ["DQ9_ROM"]) if os.environ.get("DQ9_ROM") else None)
    if rom is None:
        print("select an already locally available compatible image with --rom or DQ9_ROM", file=sys.stderr)
        return 2
    save, inputs, baseline = args.save, args.input, args.baseline
    for path in (args.binary, rom, save, inputs, baseline):
        if not path.is_file():
            print(f"missing required file: {path}", file=sys.stderr)
            return 2
    if os.name != "nt" and shutil.which("taskset") is None:
        print("taskset is required for the CPU-0 run", file=sys.stderr)
        return 2
    try:
        expected = checked_hashes(baseline, args.frames)
        args.out.mkdir(parents=True, exist_ok=True)
        runs = [(threads, repeat, False) for threads in THREADS for repeat in range(1, 4)]
        runs.append((6, 1, True))
        for threads, repeat, pinned in runs:
            label = f"threads{threads}-run{repeat}" + ("-cpu0" if pinned else "")
            hashes = (args.out / f"{label}.hashes").resolve()
            log = (args.out / f"{label}.log").resolve()
            # Do not let a previous invocation's hashes hide missing output.
            hashes.unlink(missing_ok=True)
            with tempfile.TemporaryDirectory(prefix="dq9-determinism-") as tmp:
                run_save = Path(tmp) / "game.sav"
                shutil.copyfile(save, run_save)
                command = [str(args.binary.resolve()), "--headless", "--rom", str(rom.resolve()),
                           "--save", str(run_save), "--frames", str(args.frames), "--input-script", str(inputs.resolve()),
                           "--rtc-epoch", "0", "--rtc-timezone-offset", "0", "--hash-file", str(hashes)]
                environment = dict(os.environ, DSR_RENDER_THREADS=str(threads))
                with log.open("w") as stream:
                    result = run_with_affinity(command, 0 if pinned else None,
                                               env=environment, stdout=stream, stderr=subprocess.STDOUT)
            if result.returncode != 0:
                print(f"FAIL {label}: process exit {result.returncode}; log: {log}", file=sys.stderr)
                return 1
            actual = checked_hashes(hashes, args.frames)
            bad = [frame for frame in range(args.frames) if actual[frame] != expected[frame]]
            if bad:
                first = bad[0]
                print(f"FAIL {label}: {len(bad)} differing frames, first={first}; "
                      f"expected={expected[first]}, actual={actual[first]}; hashes: {hashes}", file=sys.stderr)
                return 1
            print(f"PASS {label}: {args.frames} frames, both screens identical", flush=True)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"PASS: all {len(runs)} runs match the baseline ({len(runs) * args.frames} frames)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
