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
              gpu3d: bool = False, widescreen: str = "off") -> dict:
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
    command += ["--widescreen", widescreen]
    env = os.environ.copy()
    env["XDG_DATA_HOME"] = str(work / "xdg-data")
    result = subprocess.run(command, env=env)
    if result.returncode:
        raise ValueError(f"{meta['name']}: Godot exited {result.returncode}")
    return hashes(output, frames)


def run(ladder: Path, selected: list[str], godot: Path, rom: Path,
        toggles: bool, gpu3d: bool, widescreen: str = "off",
        enhanced_output: Path | None = None) -> int:
    passed = failed = skipped = 0
    print("NOTE: audio is not compared in Godot runs")
    for name in selected:
        seg = segment(ladder, name)
        meta = load_meta(seg)
        if meta["extra_args"]:
            print(f"SKIP {name}: extra_args unsupported in Godot")
            skipped += 1
            continue
        expected = None if widescreen == "full" else hashes(seg / "expected.hashes", meta["frames"])
        with tempfile.TemporaryDirectory(prefix="dq9-godot-checkpoint-") as tmp:
            work = Path(tmp)
            save = work / "run.sav"
            output = work / "run.hashes"
            shutil.copyfile(seg / "start.sav", save)
            actual = godot_run(seg, meta, godot, rom, work, save, output,
                               meta["frames"], toggles, gpu3d, widescreen)
            if widescreen == "full":
                repeat_save = work / "repeat.sav"
                shutil.copyfile(seg / "start.sav", repeat_save)
                expected = godot_run(seg, meta, godot, rom, work, repeat_save,
                                     work / "repeat.hashes", meta["frames"], toggles,
                                     gpu3d, widescreen)
                expected_save = sha(repeat_save)
                if enhanced_output is not None:
                    destination = enhanced_output / name
                    destination.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(output, destination / "run.hashes")
                    shutil.copyfile(work / "repeat.hashes", destination / "repeat.hashes")
                    shutil.copyfile(save, destination / "run.sav")
                    shutil.copyfile(repeat_save, destination / "repeat.sav")
            else:
                expected_save = meta["recorded"]["end_save_sha256"]
            bad = [frame for frame in range(meta["frames"]) if actual[frame] != expected[frame]]
            label = "godot" + (" +toggles" if toggles else "") + (" +gpu3d" if gpu3d else "") + f" widescreen={widescreen}"
            errors = []
            if bad:
                errors.append(f"hashes first={bad[0]} ({len(bad)} frames differ)")
            if sha(save) != expected_save:
                errors.append("save sha differs")
            if errors:
                failed += 1
                print(f"FAIL {name} {label}: " + " | ".join(errors))
            else:
                passed += 1
                print(f"PASS {name} {label}: {meta['frames']} frames, save identical")
    print(f"SUMMARY: {passed} passed, {failed} failed, {skipped} skipped")
    return 1 if failed else 0


def concat_inputs(path_a: Path, path_b: Path, frames_a: int, frames_b: int,
                  output: Path) -> None:
    """Make one native input stream; each sparse script is reset at its boundary."""
    rows = []
    for path, count, offset in ((path_a, frames_a, 0), (path_b, frames_b, frames_a)):
        for line in path.read_text(encoding="utf-8").splitlines():
            fields = line.split()
            if not fields:
                continue
            if len(fields) != 4:
                raise ValueError(f"{path}: invalid input row")
            try:
                frame, mask, x, y = map(int, fields)
            except ValueError as exc:
                raise ValueError(f"{path}: invalid input row") from exc
            if 0 <= frame < count:
                rows.append(f"{frame + offset} {mask} {x} {y}\n")
    output.write_text("".join(rows), encoding="utf-8")


def alignment(control: dict, restarted: dict, min_frames: int = 30):
    """Find a non-static identical suffix overlap, avoiding black-screen matches."""
    if min_frames < 2:
        raise ValueError("min_frames must be at least 2")
    left = [control[i] for i in range(len(control))]
    right = [restarted[i] for i in range(len(restarted))]
    windows = {}
    for j in range(len(right) - min_frames + 1):
        window = tuple(right[j:j + min_frames])
        if len(set(window)) > 1:
            windows.setdefault(window, []).append(j)
    best = None
    checked = {}
    for i in range(len(left) - min_frames + 1):
        window = tuple(left[i:i + min_frames])
        for j in windows.get(window, ()):
            diagonal = i - j
            if i < checked.get(diagonal, -1):
                continue
            start_i, start_j = i, j
            while start_i and start_j and left[start_i - 1] == right[start_j - 1]:
                start_i -= 1
                start_j -= 1
            end_i, end_j = i + min_frames, j + min_frames
            while end_i < len(left) and end_j < len(right) and left[end_i] == right[end_j]:
                end_i += 1
                end_j += 1
            checked[diagonal] = end_i
            length = end_i - start_i
            if (end_i == len(left) or end_j == len(right)) and (best is None or length > best[2]):
                best = (start_i, start_j, length)
    return best

def chain(ladder: Path, a: str, b: str, frames_b: int | None,
          godot: Path, rom: Path, binary: Path, control: bool = False) -> int:
    seg_a, seg_b = segment(ladder, a), segment(ladder, b)
    meta_a, meta_b = load_meta(seg_a), load_meta(seg_b)
    if meta_a["extra_args"] or meta_b["extra_args"]:
        raise ValueError("chain does not support segment extra_args in Godot")
    frames = meta_b["frames"] if frames_b is None else frames_b
    if frames <= 0:
        raise ValueError("--frames-b must be positive")
    if control and meta_a["language"] != meta_b["language"]:
        raise ValueError("control requires matching segment languages")
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
        if control:
            inputs = work / "control-input.txt"
            concat_inputs(seg_a / "input.txt", seg_b / "input.txt",
                          meta_a["frames"], frames, inputs)
            control_save = work / "control.sav"
            shutil.copyfile(seg_a / "start.sav", control_save)
            control_hashes = work / "control.hashes"
            total = meta_a["frames"] + frames
            command = [str(binary.resolve()), "--headless", "--rom", str(rom.resolve()),
                       "--save", str(control_save), "--frames", str(total),
                       "--input-script", str(inputs), "--language", meta_a["language"],
                       "--rtc-epoch", str(meta_a["rtc_epoch"]), "--rtc-timezone-offset",
                       str(meta_a["rtc_timezone_offset_minutes"]), "--hash-file", str(control_hashes)]
            result = subprocess.run(command, env=os.environ.copy())
            if result.returncode:
                raise ValueError(f"uninterrupted control: dq9 exited {result.returncode}")
            uninterrupted = hashes(control_hashes, total)
            if any(uninterrupted[f] != expected[f] for f in range(meta_a["frames"])):
                print("FAIL control: uninterrupted A differs from recorded A")
                return 1
            post_a = {f: uninterrupted[meta_a["frames"] + f] for f in range(frames)}
            match = alignment(post_a, reference)
            save_equal = sha(control_save) == sha(native_save)
            print("NOTE: uninterrupted control retains A's RTC; restarted B uses B's RTC")
            if match is None:
                print(f"FAIL control {a}->{b}: no non-static aligned suffix; "
                      f"end saves {'identical' if save_equal else 'differ'}")
                return 1
            print(f"CONTROL alignment: uninterrupted B frame {match[0]}, "
                  f"restarted B frame {match[1]}, {match[2]} identical frames")
            if not save_equal:
                print(f"FAIL control {a}->{b}: end saves differ")
                return 1
            print(f"PASS control {a}->{b}: aligned hashes and end saves identical")
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
    replay.add_argument("--widescreen", choices=("off", "picture", "full"), default="off",
                        help="Full compares two independent enhanced runs, not Classic hashes")
    replay.add_argument("--enhanced-output", type=Path,
                        help="retain Full-mode hashes and saves in this directory")
    chaining = sub.add_parser("chain")
    chaining.add_argument("a")
    chaining.add_argument("b")
    chaining.add_argument("--frames-b", type=int)
    chaining.add_argument("--control", action="store_true",
                          help="also compare against one uninterrupted native A+B process")
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
                         args.godot, args.rom, args.dq9, args.control)
        return run(args.ladder, args.names or names(args.ladder), args.godot, args.rom,
                   args.toggles, args.gpu3d, args.widescreen, args.enhanced_output)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
