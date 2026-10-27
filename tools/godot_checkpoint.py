#!/usr/bin/env python3
"""Replay checkpoint segments through the Godot frontend.

Exit 0 on success, 1 on a comparison mismatch, and 2 on configuration or
Godot errors. Audio is not compared by this runner.
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
    from .checkpoint import load_meta, segment, hashes, sha, names
    from .host_tools import data_root, executable_name
except ImportError:
    from checkpoint import load_meta, segment, hashes, sha, names
    from host_tools import data_root, executable_name

ROOT = Path(__file__).resolve().parents[1]


def godot_run(seg: Path, meta: dict, godot: Path, rom: Path, work: Path,
              save: Path, output: Path, frames: int, toggles: bool = False,
              gpu3d: bool = False) -> dict:
    command = ["xvfb-run", "-a", str(godot.resolve()), "--path", str(ROOT / "godot"),
               "--script", "res://tests/checkpoint_run.gd", "--",
               "--rom", str(rom.resolve()), "--save", str(save),
               "--input", str((seg / "input.txt").resolve()),
               "--frames", str(frames), "--language", meta["language"],
               "--rtc-epoch", str(meta["rtc_epoch"]), "--rtc-timezone-offset",
               str(meta["rtc_timezone_offset_minutes"]), "--hashes", str(output)]
    if toggles:
        command.append("--toggles")
    if gpu3d:
        command.append("--gpu3d")
    env = os.environ.copy()
    env["XDG_DATA_HOME"] = str(work / "xdg-data")
    result = subprocess.run(command, env=env)
    if result.returncode:
        raise ValueError(f"{meta['name']}: Godot exited {result.returncode}")
    return hashes(output, frames)


def run(ladder: Path, selected: list[str], godot: Path, rom: Path,
        toggles: bool, gpu3d: bool) -> int:
    passed = failed = skipped = 0
    print("NOTE: audio is not compared in Godot runs")
    for name in selected:
        seg = segment(ladder, name)
        meta = load_meta(seg)
        if meta["extra_args"]:
            print(f"SKIP {name}: extra_args unsupported in Godot")
            skipped += 1
            continue
        expected = hashes(seg / "expected.hashes", meta["frames"])
        with tempfile.TemporaryDirectory(prefix="dq9-godot-checkpoint-") as tmp:
            work = Path(tmp)
            save = work / "run.sav"
            output = work / "run.hashes"
            shutil.copyfile(seg / "start.sav", save)
            actual = godot_run(seg, meta, godot, rom, work, save, output,
                               meta["frames"], toggles, gpu3d)
            bad = [frame for frame in range(meta["frames"]) if actual[frame] != expected[frame]]
            label = "godot" + (" +toggles" if toggles else "") + (" +gpu3d" if gpu3d else "")
            errors = []
            if bad:
                errors.append(f"hashes first={bad[0]} ({len(bad)} frames differ)")
            if sha(save) != meta["recorded"]["end_save_sha256"]:
                errors.append("save sha differs")
            if errors:
                failed += 1
                print(f"FAIL {name} {label}: " + " | ".join(errors))
            else:
                passed += 1
                print(f"PASS {name} {label}: {meta['frames']} frames, save identical")
    print(f"SUMMARY: {passed} passed, {failed} failed, {skipped} skipped")
    return 1 if failed else 0


def chain(ladder: Path, a: str, b: str, frames_b: int | None,
          godot: Path, rom: Path, binary: Path) -> int:
    seg_a, seg_b = segment(ladder, a), segment(ladder, b)
    meta_a, meta_b = load_meta(seg_a), load_meta(seg_b)
    if meta_a["extra_args"] or meta_b["extra_args"]:
        raise ValueError("chain does not support segment extra_args in Godot")
    frames = meta_b["frames"] if frames_b is None else frames_b
    if frames <= 0:
        raise ValueError("--frames-b must be positive")
    if not binary.is_file():
        raise ValueError(f"missing required file: {binary}")
    expected = hashes(seg_a / "expected.hashes", meta_a["frames"])
    print("NOTE: audio is not compared in Godot runs")
    with tempfile.TemporaryDirectory(prefix="dq9-godot-chain-") as tmp:
        work = Path(tmp)
        save_a = work / "a.sav"
        shutil.copyfile(seg_a / "start.sav", save_a)
        actual = godot_run(seg_a, meta_a, godot, rom, work, save_a,
                           work / "a.hashes", meta_a["frames"])
        bad = [f for f in range(meta_a["frames"]) if actual[f] != expected[f]]
        errors = []
        if bad:
            errors.append(f"hashes first={bad[0]} ({len(bad)} frames differ)")
        if sha(save_a) != meta_a["recorded"]["end_save_sha256"]:
            errors.append("save sha differs")
        if errors:
            print(f"FAIL {a} godot: " + " | ".join(errors))
            return 1
        print(f"PASS {a} godot: {meta_a['frames']} frames, save identical")
        if sha(save_a) == sha(seg_a / "start.sav"):
            raise ValueError(f"{a}: Godot-written save is unchanged; chain proves nothing")
        native_save, godot_save = work / "native.sav", work / "godot.sav"
        shutil.copyfile(save_a, native_save)
        shutil.copyfile(save_a, godot_save)
        native_hashes = work / "native.hashes"
        command = [str(binary.resolve()), "--headless", "--rom", str(rom.resolve()),
                   "--save", str(native_save), "--frames", str(frames),
                   "--input-script", str((seg_b / "input.txt").resolve()),
                   "--language", meta_b["language"], "--rtc-epoch", str(meta_b["rtc_epoch"]),
                   "--rtc-timezone-offset", str(meta_b["rtc_timezone_offset_minutes"]),
                   "--hash-file", str(native_hashes)]
        result = subprocess.run(command, env=os.environ.copy())
        if result.returncode:
            raise ValueError(f"{b}: dq9 exited {result.returncode}")
        reference = hashes(native_hashes, frames)
        actual = godot_run(seg_b, meta_b, godot, rom, work, godot_save,
                           work / "godot.hashes", frames)
        bad = [f for f in range(frames) if actual[f] != reference[f]]
        errors = []
        if bad:
            errors.append(f"hashes first={bad[0]} ({len(bad)} frames differ)")
        if sha(godot_save) != sha(native_save):
            errors.append("save sha differs")
        if errors:
            print(f"FAIL chain {a}->{b}: " + " | ".join(errors))
            return 1
        print(f"PASS chain {a}->{b}: {frames} frames, save identical")
        return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ladder", type=Path, default=data_root(ROOT) / "build/checkpoints")
    parser.add_argument("--godot", type=Path, default=Path(os.environ.get("GODOT") or
                        Path.home() / "Applications/godot/Godot_v4.5-stable_linux.x86_64"))
    parser.add_argument("--rom", type=Path, help="default: $DQ9_ROM, else the first ROM/*.nds")
    parser.add_argument("--dq9", type=Path, default=ROOT / "build/game/game" / executable_name("dq9"))
    sub = parser.add_subparsers(dest="command", required=True)
    replay = sub.add_parser("run")
    replay.add_argument("names", nargs="*")
    replay.add_argument("--toggles", action="store_true")
    replay.add_argument("--gpu3d", action="store_true")
    chaining = sub.add_parser("chain")
    chaining.add_argument("a")
    chaining.add_argument("b")
    chaining.add_argument("--frames-b", type=int)
    args = parser.parse_args(argv)
    try:
        if args.rom is None:
            if os.environ.get("DQ9_ROM"):
                args.rom = Path(os.environ["DQ9_ROM"])
            else:
                roms = sorted((data_root(ROOT) / "ROM").glob("*.nds"))
                if not roms:
                    raise ValueError("no ROM: pass --rom, set DQ9_ROM, or place a .nds in ROM/")
                args.rom = roms[0]
        for path in (args.godot, args.rom):
            if not path.is_file():
                raise ValueError(f"missing required file: {path}")
        if args.command == "chain":
            return chain(args.ladder, args.a, args.b, args.frames_b,
                         args.godot, args.rom, args.dq9)
        return run(args.ladder, args.names or names(args.ladder), args.godot, args.rom,
                   args.toggles, args.gpu3d)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
