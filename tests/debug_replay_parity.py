#!/usr/bin/env python3
"""Replay a captured fresh-save session using its recorded launch settings."""
from __future__ import annotations

import argparse
import configparser
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.debug_bundle import build_replay_command

RTC_EPOCH = 1_704_067_190
RTC_OFFSET_MINUTES = 120
DEFAULT_FRAME_COUNT = 3000


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
    backup_types = (
        "auto", "eeprom512b", "eeprom8k", "eeprom64k", "eeprom128k",
        "flash256k", "flash512k", "flash1m", "flash8m")
    parser.add_argument("--backup-type", choices=backup_types,
                        help="explicit CLI override (omitted by default, distinct from explicit auto)")
    parser.add_argument("--abi-backup-type", choices=backup_types,
                        help="set this backup choice in the local ABI copy instead of removing it")
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAME_COUNT)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.frames < 1:
        parser.error("--frames must be positive")
    if args.abi_backup_type is not None and args.abi_config is None:
        parser.error("--abi-backup-type requires --abi-config")
    note_frame = args.frames - 1
    project = Path.cwd()
    if any(not path.is_file() for path in (args.dq9, args.rom, args.input)):
        print("SKIP: dq9 binary, ROM, or input script is unavailable", file=sys.stderr)
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
    env.update({"XDG_DATA_HOME": str(xdg_data), "XDG_CONFIG_HOME": str(xdg_config),
                "APPDATA": str(xdg_data), "LOCALAPPDATA": str(work / "local-data"),
                "SDL_VIDEODRIVER": "dummy", "SDL_RENDER_DRIVER": "software",
                "SDL_AUDIODRIVER": "dummy"})
    abi_config = None
    if args.abi_config:
        # Normally remove ABI backup overrides to expose a missing replay CLI
        # option. An explicit ABI choice also tests CLI auto overriding it.
        config = configparser.ConfigParser()
        with args.abi_config.open(encoding="utf-8") as source:
            config.read_file(source)
        if config.has_section("cartridge"):
            config.remove_option("cartridge", "backup_type")
        if args.abi_backup_type is not None:
            if not config.has_section("cartridge"):
                config.add_section("cartridge")
            config.set("cartridge", "backup_type", args.abi_backup_type)
        abi_config = work / "abi.ini"
        with abi_config.open("w", encoding="utf-8") as destination:
            config.write(destination)
    source_save = work / "capture-save.sav"
    source_save.write_bytes(b"")
    capture_hashes = work / "capture.hashes"
    capture_command = [
        str(args.dq9.resolve()), "--debug-selftest", "--rom", str(args.rom.resolve()),
        "--save", str(source_save), "--input-script", str(args.input.resolve()),
        "--frames", str(args.frames), "--rtc-epoch", str(RTC_EPOCH),
        "--rtc-timezone-offset", str(RTC_OFFSET_MINUTES), "--hash-file", str(capture_hashes),
    ]
    if abi_config:
        capture_command += ["--abi-config", str(abi_config)]
    if args.backup_type is not None:
        capture_command += ["--backup-type", args.backup_type]
    run(capture_command, project, env)

    debug_root = xdg_data / "dq9-decomp" / "debug"
    index = debug_root / "index.tsv"
    rows = [line.split("\t", 3) for line in index.read_text(encoding="utf-8").splitlines() if line]
    if not rows or len(rows[-1]) != 4:
        raise RuntimeError("debug capture did not append a valid index.tsv row")
    bundle = debug_root / rows[-1][1]
    metadata = read_metadata(bundle / "comment.txt")
    if int(metadata.get("frame", "-1")) != note_frame:
        raise RuntimeError(f"captured note frame is not {note_frame}: {metadata}")
    if int(metadata.get("rtc_epoch", "-1")) != RTC_EPOCH:
        raise RuntimeError(f"bundle did not store the injected RTC epoch: {metadata}")
    if int(metadata.get("rtc_timezone_offset_minutes", "9999")) != RTC_OFFSET_MINUTES:
        raise RuntimeError(f"bundle did not store the injected timezone offset: {metadata}")
    note_save = bundle / "save_at_note.sav"
    if not note_save.is_file():
        raise RuntimeError("capture did not save save_at_note.sav")
    if note_save.stat().st_size == 0:
        raise RuntimeError("capture did not persist backup memory; use more --frames or a longer input script")
    if note_save.read_bytes() != source_save.read_bytes():
        raise RuntimeError("save_at_note.sav is not the flushed save at the capture frame")
    expected_save_hash = hashlib.sha256(note_save.read_bytes()).hexdigest()
    if metadata.get("save_at_note_sha256") != expected_save_hash:
        raise RuntimeError("save_at_note.sav SHA-256 metadata does not match its contents")
    if (bundle / "session_start.sav").read_bytes() != b"":
        raise RuntimeError("capture did not retain the zero-byte session-start save")
    if int((bundle / "session_input.txt.frames").read_text().strip()) != args.frames:
        raise RuntimeError(f"session input sidecar does not record {args.frames} frames")
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

    # Exercise the recorded replay invocation, not a manually reconstructed
    # command that could accidentally restore a missing --backup-type itself.
    captured_files = {
        path.relative_to(bundle): hashlib.sha256(path.read_bytes()).digest()
        for path in bundle.rglob("*") if path.is_file()
    }
    source_save_bytes = source_save.read_bytes()
    replay_hashes = work / "replay.hashes"
    dump_dir = work / "replay-frames"
    replay_command = build_replay_command(
        bundle, note_frame, dq9=args.dq9.resolve(), rom=args.rom.resolve(), out=dump_dir)
    replay_save = Path(replay_command[replay_command.index("--save") + 1])
    replay_command += ["--pause-at-frame", str(note_frame), "--hash-file", str(replay_hashes)]
    output = run(replay_command, bundle, env)
    pause = re.search(r"\[pause-at-frame\] frame=(\d+) ticks=(\d+)", output)
    if not pause:
        raise RuntimeError("replay did not report the pause frame and emulated ticks")
    if int(pause.group(1)) != note_frame or int(pause.group(2)) != int(metadata["emulated_ticks"]):
        raise RuntimeError("replay frame/tick position differs from the captured annotation")
    if capture_hashes.read_bytes() != replay_hashes.read_bytes():
        raise RuntimeError("capture and replay per-frame screen hashes differ")
    for screen in ("top", "bottom"):
        captured = (bundle / f"screenshot_{screen}.ppm").read_bytes()
        replayed = (dump_dir / f"frame_{note_frame:04d}_{screen}.ppm").read_bytes()
        if captured != replayed:
            raise RuntimeError(f"replayed {screen} screenshot differs from captured note")
    replay_save_bytes = replay_save.read_bytes()
    if replay_save_bytes != note_save.read_bytes():
        raise RuntimeError(
            "replay save bytes/capacity differ from the captured save "
            f"(capture={note_save.stat().st_size}, replay={len(replay_save_bytes)} bytes)")
    if source_save.read_bytes() != source_save_bytes:
        raise RuntimeError("replay modified the source session save")
    replayed_capture_files = {
        path.relative_to(bundle): hashlib.sha256(path.read_bytes()).digest()
        for path in bundle.rglob("*") if path.is_file()
    }
    if replayed_capture_files != captured_files:
        raise RuntimeError("replay modified the source capture files")
    env_metadata = read_metadata(bundle / "env.txt")
    if env_metadata.get("backup_type") != (args.backup_type or "auto"):
        raise RuntimeError(f"capture did not record requested backup type {args.backup_type or 'auto'!r}")
    # An interrupted recording must not silently drop its incomplete final input.
    truncated_input = work / "truncated-input.txt"
    truncated_input.write_text("0 0 -1 -1\n1 1 -1", encoding="utf-8")
    invalid_command = [
        str(args.dq9.resolve()), "--rom", str(args.rom.resolve()),
        "--save", str(work / "invalid-input-save.sav"), "--headless", "--frames", "1",
        "--input-script", str(truncated_input),
    ]
    if abi_config:
        invalid_command += ["--abi-config", str(abi_config)]
    rejected = subprocess.run(invalid_command, cwd=project, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if rejected.returncode != 2:
        raise RuntimeError(f"truncated input must fail with status 2, got {rejected.returncode}: {rejected.stdout}")
    print(f"PASS: {args.frames}-frame fresh-save replay matched RTC epoch, ticks={pause.group(2)}, "
          "hashes, screenshots, save bytes/capacity, and preserved the capture")
    print(f"Bundle: {bundle}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
