#!/usr/bin/env python3
"""Headless end-to-end determinism check for a captured debug session."""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

RTC_EPOCH = 1_704_067_190
RTC_OFFSET_MINUTES = 120
REPLAY_FRAME_COUNT = 600
NOTE_FRAME = REPLAY_FRAME_COUNT - 1


def read_metadata(path: Path) -> dict[str, str]:
    metadata: dict[str, str] = {}
    in_metadata = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            if key.strip() and value.strip():
                metadata[key.strip()] = value.strip()
                in_metadata = True
                continue
        if in_metadata:
            continue
    return metadata


def run(command: list[str], cwd: Path, env: dict[str, str]) -> str:
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end="")
    if result.returncode == 77:
        raise SystemExit(77)
    if result.returncode:
        raise RuntimeError(f"command exited {result.returncode}: {command!r}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dq9", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--abi-config", type=Path, help="optional private layout metadata")
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    project = Path.cwd()
    if any(not path.is_file() for path in (args.dq9, args.rom, args.save, args.input)):
        print("SKIP: dq9 binary, EU ROM, save, or input script is unavailable", file=sys.stderr)
        return 77

    work = args.work_dir.resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    xdg_data = work / "xdg-data"
    xdg_config = work / "xdg-config"
    xdg_data.mkdir()
    xdg_config.mkdir()
    env = os.environ.copy()
    env.update({"XDG_DATA_HOME": str(xdg_data), "XDG_CONFIG_HOME": str(xdg_config)})
    source_save = work / "capture-save.sav"
    shutil.copy2(args.save, source_save)
    capture_hashes = work / "capture.hashes"
    capture_command = [
        str(args.dq9.resolve()), "--debug-selftest", "--rom", str(args.rom.resolve()),
        "--save", str(source_save), "--input-script", str(args.input.resolve()),
        "--frames", str(REPLAY_FRAME_COUNT), "--rtc-epoch", str(RTC_EPOCH),
        "--rtc-timezone-offset", str(RTC_OFFSET_MINUTES), "--hash-file", str(capture_hashes),
    ]
    if args.abi_config:
        capture_command += ["--abi-config", str(args.abi_config.resolve())]
    run(capture_command, project, env)

    debug_root = xdg_data / "dq9-decomp" / "debug"
    index = debug_root / "index.tsv"
    rows = [line.split("\t", 3) for line in index.read_text(encoding="utf-8").splitlines() if line]
    if not rows or len(rows[-1]) != 4:
        raise RuntimeError("debug capture did not append a valid index.tsv row")
    bundle = debug_root / rows[-1][1]
    metadata = read_metadata(bundle / "comment.txt")
    if int(metadata.get("frame", "-1")) != NOTE_FRAME:
        raise RuntimeError(f"captured note frame is not {NOTE_FRAME}: {metadata}")
    if int(metadata.get("rtc_epoch", "-1")) != RTC_EPOCH:
        raise RuntimeError(f"bundle did not store the injected RTC epoch: {metadata}")
    if int(metadata.get("rtc_timezone_offset_minutes", "9999")) != RTC_OFFSET_MINUTES:
        raise RuntimeError(f"bundle did not store the injected timezone offset: {metadata}")
    note_save = bundle / "save_at_note.sav"
    if not note_save.is_file():
        raise RuntimeError("capture did not save save_at_note.sav")
    if note_save.read_bytes() != source_save.read_bytes():
        raise RuntimeError("save_at_note.sav is not the flushed save at the capture frame")
    expected_save_hash = hashlib.sha256(note_save.read_bytes()).hexdigest()
    if metadata.get("save_at_note_sha256") != expected_save_hash:
        raise RuntimeError("save_at_note.sav SHA-256 metadata does not match its contents")
    if (bundle / "session_start.sav").read_bytes() != args.save.read_bytes():
        raise RuntimeError("capture changed the session-start save baseline")
    if int((bundle / "session_input.txt.frames").read_text().strip()) != REPLAY_FRAME_COUNT:
        raise RuntimeError("session input sidecar does not record 600 frames")
    for slot in range(4):
        texture_path = bundle / f"texture_slot{slot}.png"
        try:
            header = texture_path.read_bytes()[:24]
        except OSError as error:
            raise RuntimeError(f"texture inspector did not write {texture_path.name}: {error}") from error
        if (len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n"
                or int.from_bytes(header[16:20], "big") != 256
                or int.from_bytes(header[20:24], "big") != 512):
            raise RuntimeError(f"{texture_path.name} must be a 256x512 PNG")

    replay_save = work / "replay-save.sav"
    shutil.copy2(bundle / "session_start.sav", replay_save)
    replay_hashes = work / "replay.hashes"
    dump_dir = work / "replay-frames"
    replay_command = [
        str(args.dq9.resolve()), "--rom", str(args.rom.resolve()), "--save", str(replay_save),
        "--language", "en", "--headless", "--frames", str(REPLAY_FRAME_COUNT),
        "--pause-at-frame", str(NOTE_FRAME), "--input-script", str(bundle / "session_input.txt"),
        "--rtc-epoch", metadata["rtc_epoch"], "--rtc-timezone-offset",
        metadata["rtc_timezone_offset_minutes"], "--hash-file", str(replay_hashes),
        "--dump-frames", str(NOTE_FRAME), "--dump-dir", str(dump_dir),
    ]
    if args.abi_config:
        replay_command += ["--abi-config", str(args.abi_config.resolve())]
    output = run(replay_command, project, env)
    pause = re.search(r"\[pause-at-frame\] frame=(\d+) ticks=(\d+)", output)
    if not pause:
        raise RuntimeError("replay did not report the pause frame and emulated ticks")
    if int(pause.group(1)) != NOTE_FRAME or int(pause.group(2)) != int(metadata["emulated_ticks"]):
        raise RuntimeError("replay frame/tick position differs from the captured annotation")
    if capture_hashes.read_bytes() != replay_hashes.read_bytes():
        raise RuntimeError("capture and replay per-frame screen hashes differ")
    for screen in ("top", "bottom"):
        captured = (bundle / f"screenshot_{screen}.ppm").read_bytes()
        replayed = (dump_dir / f"frame_{NOTE_FRAME:04d}_{screen}.ppm").read_bytes()
        if captured != replayed:
            raise RuntimeError(f"replayed {screen} screenshot differs from captured note")
    if (bundle / "session_start.sav").read_bytes() != args.save.read_bytes():
        raise RuntimeError("replay modified the captured session-start save")
    print(f"PASS: 600-frame replay matched RTC epoch, ticks={pause.group(2)}, hashes, and screenshots")
    print(f"Bundle: {bundle}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
