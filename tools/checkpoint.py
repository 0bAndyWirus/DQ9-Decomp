#!/usr/bin/env python3
"""Record and replay boot-from-save checkpoint segments.

Each ladder directory contains NAME/segment.json, start.sav, input.txt,
expected.hashes and optionally oracle.hashes. Runs never modify start.sav.
Exit 0 on success, 1 on a comparison mismatch, 2 on configuration or dq9 error.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

try:
    from .compare_hashes import read_hashes
    from .compare_frames import compare, read_image
    from .host_tools import data_root, executable_name
    from .debug_bundle import DEFAULT_DEBUG_DIR, select_bundle, parse_comment, _capture
except ImportError:
    from compare_hashes import read_hashes
    from compare_frames import compare, read_image
    from host_tools import data_root, executable_name
    from debug_bundle import DEFAULT_DEBUG_DIR, select_bundle, parse_comment, _capture

ROOT = Path(__file__).resolve().parents[1]
NAME = re.compile(r"^[A-Za-z0-9_-]+$")
LANGUAGES = ("en", "fr", "de", "es", "it")


def sha(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def hashes(path: Path, frames: int | None = None) -> dict[int, tuple[str, str]]:
    rows, problems = read_hashes(path)
    if problems:
        raise ValueError("\n".join(problems))
    if frames is not None and set(rows) != set(range(frames)):
        raise ValueError(f"{path}: expected exactly frames 0..{frames - 1}")
    return rows


@dataclass
class RunResult:
    exit_code: int
    hashes: dict[int, tuple[str, str]]
    end_save_sha256: str
    audio_sha256: str


def run_segment(segment_dir: Path, meta: dict, binary: Path, rom: Path,
                work_dir: Path, dump_frame: int | None = None) -> RunResult:
    save = work_dir / "run.sav"
    hash_path = work_dir / "run.hashes"
    audio = work_dir / "run.wav"
    log = work_dir / "run.log"
    shutil.copyfile(segment_dir / "start.sav", save)
    command = [str(binary.resolve()), "--headless", "--rom", str(rom.resolve()),
               "--save", str(save), "--frames", str(meta["frames"]),
               "--input-script", str((segment_dir / "input.txt").resolve()),
               "--language", meta["language"], "--rtc-epoch", str(meta["rtc_epoch"]),
               "--rtc-timezone-offset", str(meta["rtc_timezone_offset_minutes"]),
               "--hash-file", str(hash_path), "--dump-audio", str(audio)] + meta["extra_args"]
    if dump_frame is not None:
        command += ["--dump-frames", str(dump_frame), "--dump-dir", str(work_dir)]
    with log.open("w") as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError(f"dq9 exited {result.returncode}; log: {log}")
    return RunResult(result.returncode, hashes(hash_path, meta["frames"]), sha(save), sha(audio))


def preserve(work: Path, name: str, label: str) -> Path:
    root = data_root(ROOT) / "build/checkpoint-runs"
    root.mkdir(parents=True, exist_ok=True)
    dest = Path(tempfile.mkdtemp(prefix=f"{name}-{label}-", dir=root))
    for filename in ("run.log", "run.hashes"):
        if (work / filename).is_file():
            shutil.copyfile(work / filename, dest / filename)
    return dest


def execute(seg: Path, meta: dict, binary: Path, rom: Path, work: Path, label: str,
            dump_frame: int | None = None) -> RunResult:
    try:
        return run_segment(seg, meta, binary, rom, work, dump_frame)
    except (OSError, ValueError) as exc:
        dest = preserve(work, meta["name"], label)
        raise ValueError(f"{exc}; retained run files: {dest}") from exc


def write_meta(seg: Path, meta: dict) -> None:
    (seg / "segment.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")


def load_meta(seg: Path) -> dict:
    meta = json.loads((seg / "segment.json").read_text(encoding="utf-8"))
    if meta["schema"] != 1 or meta["name"] != seg.name or meta["frames"] <= 0:
        raise ValueError(f"invalid segment metadata: {seg}")
    return meta


def segment(ladder: Path, name: str) -> Path:
    if not NAME.fullmatch(name):
        raise ValueError(f"invalid segment name: {name}")
    return ladder / name


def names(ladder: Path) -> list[str]:
    return sorted(p.name for p in ladder.iterdir() if p.is_dir() and (p / "segment.json").is_file()) if ladder.exists() else []


def record(seg: Path, meta: dict, binary: Path, rom: Path, work: Path,
           dump_frame: int | None = None) -> None:
    result = execute(seg, meta, binary, rom, work, "record", dump_frame)
    shutil.copyfile(work / "run.hashes", seg / "expected.hashes")
    meta["recorded"] = {"binary": str(binary.resolve()), "binary_sha256": sha(binary),
                        "date": datetime.now().astimezone().isoformat(),
                        "end_save_sha256": result.end_save_sha256, "audio_sha256": result.audio_sha256}
    write_meta(seg, meta)
    print(f"RECORDED {meta['name']}: {meta['frames']} frames, save {result.end_save_sha256[:12]}, audio {result.audio_sha256[:12]}")


def create(ladder: Path, name: str, save: Path, inputs: Path, frames: int, language: str,
           epoch: int, timezone: int, note: str, extra: list[str], source: str,
           oracle: Path | None, binary: Path, rom: Path, bundle: Path | None = None) -> int:
    seg = segment(ladder, name)
    if seg.exists():
        raise ValueError(f"segment already exists: {seg}")
    for path in (save, inputs, *((oracle,) if oracle else ())):
        if not path.is_file():
            raise ValueError(f"missing required file: {path}")
    if frames <= 0 or language not in LANGUAGES:
        raise ValueError("frames must be positive and language must be en, fr, de, es or it")
    if oracle:
        hashes(oracle)
    meta = {"schema": 1, "name": name, "note": note, "frames": frames, "language": language,
            "rtc_epoch": epoch, "rtc_timezone_offset_minutes": timezone, "extra_args": extra,
            "source": source, "recorded": None, "oracle_hashes": "oracle.hashes" if oracle else None,
            "bundle_screens_match": None}
    ladder.mkdir(parents=True, exist_ok=True)
    seg.mkdir()
    try:
        shutil.copyfile(save, seg / "start.sav")
        shutil.copyfile(inputs, seg / "input.txt")
        if oracle:
            shutil.copyfile(oracle, seg / "oracle.hashes")
        write_meta(seg, meta)
        with tempfile.TemporaryDirectory(prefix="dq9-checkpoint-") as tmp:
            work = Path(tmp)
            record(seg, meta, binary, rom, work, frames - 1 if bundle else None)
            if bundle:
                match = True
                for screen in ("top", "bottom"):
                    capture = _capture(work, frames - 1, screen)
                    reference = next((bundle / f"screenshot_{screen}.{ext}" for ext in ("png", "ppm")
                                      if (bundle / f"screenshot_{screen}.{ext}").is_file()), None)
                    if capture is None or reference is None:
                        raise ValueError(f"missing {screen} capture or screenshot")
                    result = compare(read_image(capture), read_image(reference))
                    print(f"{screen}: {result['different']}/{result['pixels']} pixels differ")
                    match &= result["different"] == 0
                meta["bundle_screens_match"] = match
                write_meta(seg, meta)
                return 0 if match else 1
    except Exception:
        shutil.rmtree(seg)
        raise
    return 0


def oracle_report(name: str, actual: dict, path: Path) -> None:
    oracle = hashes(path)
    common = sorted(actual.keys() & oracle.keys())
    if not common:
        raise ValueError(f"{path}: no common frames")
    bad = [[f for f in common if actual[f][i] != oracle[f][i]] for i in (0, 1)]
    if not any(bad):
        print(f"ORACLE {name}: identical on {len(common)} common frames")
    else:
        parts = [f"{screen} first={frames[0] if frames else 'none'}/{len(frames)} frames differ"
                 for screen, frames in zip(("top", "bottom"), bad)]
        print(f"ORACLE {name}: " + ", ".join(parts))


def run(ladder: Path, selected: list[str], repeat: int, use_oracle: bool, binary: Path, rom: Path) -> int:
    if repeat < 1:
        raise ValueError("--repeat must be positive")
    passed = failed = 0
    binary_sha = sha(binary)
    for name in selected:
        seg = segment(ladder, name)
        meta = load_meta(seg)
        expected = hashes(seg / "expected.hashes", meta["frames"])
        recorded = meta["recorded"]
        if binary_sha != recorded["binary_sha256"]:
            print(f"NOTE {name}: binary changed since record ({recorded['date']})")
        for k in range(1, repeat + 1):
            with tempfile.TemporaryDirectory(prefix="dq9-checkpoint-") as tmp:
                work = Path(tmp)
                result = execute(seg, meta, binary, rom, work, f"run{k}")
                bad = [f for f in range(meta["frames"]) if result.hashes[f] != expected[f]]
                errors = []
                if bad:
                    errors.append(f"hashes first={bad[0]} ({len(bad)} frames differ)")
                if result.end_save_sha256 != recorded["end_save_sha256"]:
                    errors.append("save sha differs")
                if result.audio_sha256 != recorded["audio_sha256"]:
                    errors.append("audio sha differs")
                if errors:
                    preserve(work, name, f"run{k}")
                    failed += 1
                    print(f"FAIL {name} run{k}: " + " | ".join(errors))
                else:
                    passed += 1
                    print(f"PASS {name} run{k}: {meta['frames']} frames, save+audio identical")
                if k == 1 and use_oracle and meta["oracle_hashes"]:
                    oracle_report(name, result.hashes, seg / meta["oracle_hashes"])
    print(f"SUMMARY: {passed} passed, {failed} failed of {passed + failed} runs")
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ladder", type=Path, default=data_root(ROOT) / "build/checkpoints")
    parser.add_argument("--dq9", type=Path, default=ROOT / "build/game-fast/game" / executable_name("dq9"))
    parser.add_argument("--rom", type=Path, help="default: $DQ9_ROM, else the first ROM/*.nds")
    sub = parser.add_subparsers(dest="command", required=True)
    add = sub.add_parser("add")
    add.add_argument("name")
    add.add_argument("--save", type=Path, required=True)
    add.add_argument("--input", type=Path, required=True)
    add.add_argument("--frames", type=int, required=True)
    add.add_argument("--language", choices=LANGUAGES, default="en")
    add.add_argument("--rtc-epoch", type=int, default=0)
    add.add_argument("--rtc-timezone-offset", type=int, default=0)
    add.add_argument("--note", default="")
    add.add_argument("--oracle-hashes", type=Path)
    add.add_argument("--dq9-arg", action="append", default=[])
    rec = sub.add_parser("record")
    rec.add_argument("names", nargs="*")
    rec.add_argument("--all", action="store_true")
    replay = sub.add_parser("run")
    replay.add_argument("names", nargs="*")
    replay.add_argument("--repeat", type=int, default=2)
    replay.add_argument("--oracle", action="store_true")
    bundle = sub.add_parser("from-bundle")
    bundle.add_argument("bundle")
    bundle.add_argument("name")
    bundle.add_argument("--debug-dir", type=Path, default=DEFAULT_DEBUG_DIR)
    sub.add_parser("list")
    args = parser.parse_args(argv)
    try:
        if args.command == "list":
            for name in names(args.ladder):
                meta = load_meta(segment(args.ladder, name))
                recorded = meta["recorded"] or {}
                print(f"{name}  frames={meta['frames']}  {meta['source']}  recorded={recorded.get('date', '')} bin={recorded.get('binary_sha256', '')[:12]}  oracle={'yes' if meta['oracle_hashes'] else 'no'}  {meta['note']}")
            return 0
        if args.rom is None:
            if os.environ.get("DQ9_ROM"):
                args.rom = Path(os.environ["DQ9_ROM"])
            else:
                roms = sorted((data_root(ROOT) / "ROM").glob("*.nds"))
                if not roms:
                    raise ValueError("no ROM: pass --rom, set DQ9_ROM, or place a .nds in ROM/")
                args.rom = roms[0]
        for path in (args.dq9, args.rom):
            if not path.is_file():
                raise ValueError(f"missing required file: {path}")
        if args.command == "add":
            return create(args.ladder, args.name, args.save, args.input, args.frames, args.language,
                          args.rtc_epoch, args.rtc_timezone_offset, args.note, args.dq9_arg, "manual",
                          args.oracle_hashes, args.dq9, args.rom)
        if args.command == "from-bundle":
            directory = select_bundle(args.debug_dir, args.bundle)
            comment, metadata = parse_comment(directory / "comment.txt")
            if "frame" not in metadata:
                raise ValueError("missing bundle metadata: frame")
            _, env = parse_comment(directory / "env.txt")
            backup = metadata.get("backup_type") or env.get("backup_type")
            return create(args.ladder, args.name, directory / "session_start.sav", directory / "session_input.txt",
                          int(metadata["frame"]) + 1, env.get("language", "en"), int(metadata.get("rtc_epoch", 0)),
                          int(metadata.get("rtc_timezone_offset_minutes", 0)), comment,
                          ["--backup-type", backup] if backup else [], f"bundle:{directory.name}", None,
                          args.dq9, args.rom, directory)
        if args.command == "record":
            if args.all == bool(args.names):
                raise ValueError("record requires NAME... or --all")
            for name in names(args.ladder) if args.all else args.names:
                seg = segment(args.ladder, name)
                meta = load_meta(seg)
                with tempfile.TemporaryDirectory(prefix="dq9-checkpoint-") as tmp:
                    record(seg, meta, args.dq9, args.rom, Path(tmp))
            return 0
        return run(args.ladder, args.names or names(args.ladder), args.repeat, args.oracle, args.dq9, args.rom)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
