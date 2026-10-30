#!/usr/bin/env python3
"""Inspect, replay, and export in-game debug bundles."""
from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_frames import ImageError, compare, read_image
from host_tools import executable_name

_DATA_DIR = (Path(os.environ.get("LOCALAPPDATA") or os.environ.get("APPDATA") or
                  str(Path.home() / "AppData/Roaming"))
             if os.name == "nt" else Path(os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local/share")))
DEFAULT_DEBUG_DIR = _DATA_DIR / "dq9-remastered/debug"
LEGACY_DEBUG_DIR = _DATA_DIR / "dq9-decomp/debug"
DEFAULT_DQ9 = Path("build/game-fast/game") / executable_name("dq9")
DUMP_FILES = {"mainram.bin", "vram.bin"}
ROM_SUFFIXES = {".nds", ".rom", ".nrom"}


def parse_index(path: Path) -> list[dict[str, object]]:
    """Read valid index rows; preserve order for callers to sort by timestamp."""
    rows = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return rows
    for line in lines:
        columns = line.split("\t", 3)
        if len(columns) != 4:
            continue
        timestamp, directory, comment, frame_text = columns
        try:
            frame = int(frame_text)
        except ValueError:
            continue
        rows.append({"timestamp": timestamp, "directory": directory, "comment": comment, "frame": frame})
    return rows


def sorted_index(debug_dir: Path | None) -> list[dict[str, object]]:
    if debug_dir is not None:
        rows = parse_index(debug_dir / "index.tsv")
    else:
        rows = []
        for root in (DEFAULT_DEBUG_DIR, LEGACY_DEBUG_DIR):
            for row in parse_index(root / "index.tsv"):
                row["directory"] = str(bundle_path(root, str(row["directory"])))
                rows.append(row)
    return sorted(rows, key=lambda row: str(row["timestamp"]), reverse=True)


def parse_comment(path: Path) -> tuple[str, dict[str, str]]:
    """Return free-text comment and all subsequent key/value metadata."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return "", {}
    comment_lines = []
    metadata: dict[str, str] = {}
    in_metadata = False
    for line in text.splitlines():
        if not comment_lines and not in_metadata and line.startswith("audio_debug: "):
            metadata["audio_debug"] = line.removeprefix("audio_debug: ")
            continue
        if ":" in line:
            key, value = line.split(":", 1)
            if key.strip() and value.strip():
                in_metadata = True
                metadata[key.strip()] = value.strip()
                continue
        if in_metadata:
            # Blank lines between metadata entries are harmless; free text after
            # metadata remains metadata text only when it has a key separator.
            continue
        comment_lines.append(line)
    return "\n".join(comment_lines).strip(), metadata


def parse_replay_command(path: Path, *, out: Path | None = None) -> list[str]:
    """Extract the dq9 invocation and resolve replay-local shell variables."""
    logical_lines = []
    pending = ""
    preamble = {"set", "cd", "mkdir", "cp", "export"}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        first_word = line.split(None, 1)[0]
        if not pending and first_word in preamble:
            continue
        assignment_word = first_word.split("=", 1)[0]
        if (not pending and "=" in first_word and assignment_word.isidentifier()
                and "--headless" not in line and "dq9" not in line):
            continue
        if pending:
            line = pending + line
        if line.endswith("\\"):
            pending = line[:-1] + " "
            continue
        logical_lines.append(line)
        pending = ""
    if pending:
        logical_lines.append(pending.strip())

    candidates = []
    for line in logical_lines:
        try:
            command = shlex.split(line, posix=True)
        except ValueError as error:
            raise ValueError(f"invalid shell quoting in {path}: {error}") from error
        while command and command[0] in {"exec", "command"}:
            command.pop(0)
        if not command:
            continue
        while command and "=" in command[0] and not command[0].startswith("--"):
            name, _, _ = command[0].partition("=")
            if not name.isidentifier():
                break
            command.pop(0)
        if command and ("--headless" in command or Path(command[0]).name == "dq9"):
            candidates.append(command)
    if not candidates:
        raise ValueError(f"no dq9 invocation in {path}")
    command = candidates[-1]
    if out is not None:
        variables = {"SCRIPT_DIR": str(path.resolve().parent), "OUT": str(out.resolve())}
        pattern = re.compile(r"\$\{(SCRIPT_DIR|OUT)\}|\$(SCRIPT_DIR|OUT)(?![A-Za-z0-9_])")
        command = [
            pattern.sub(lambda match: variables[match.group(1) or match.group(2)], argument)
            for argument in command
        ]
        path_options = {"--save", "--dump-dir", "--dump-audio"}
        for index, argument in enumerate(command[:-1]):
            if argument in path_options:
                command[index + 1] = _map_replay_output_path(command[index + 1], path.resolve().parent, out)
    return command


def _map_replay_output_path(value: str, directory: Path, out: Path) -> str:
    bundle_output = (directory / "replay_out").resolve()
    path = Path(value)
    resolved = path if path.is_absolute() else directory / path
    resolved = resolved.resolve()
    try:
        relative = resolved.relative_to(bundle_output)
    except ValueError:
        return value
    return str(out.resolve() / relative)


def bundle_path(debug_dir: Path, value: str | Path) -> Path:
    path = Path(value)
    return path if path.is_absolute() else debug_dir / path


def inventory(directory: Path) -> list[dict[str, object]]:
    """List regular bundle files, tolerating missing files and broken symlinks."""
    if not directory.is_dir():
        return []
    files = []
    for path in sorted(directory.rglob("*")):
        try:
            if path.is_file() and not path.is_symlink():
                files.append({"name": path.relative_to(directory).as_posix(), "size": path.stat().st_size})
        except OSError:
            continue
    return files


def select_bundle(debug_dir: Path | None, value: str) -> Path:
    if value.isdecimal():
        rows = sorted_index(debug_dir)
        number = int(value)
        if number < 1 or number > len(rows):
            raise ValueError(f"index number {number} is out of range (1-{len(rows)})")
        path = Path(str(rows[number - 1]["directory"]))
        return path if debug_dir is None else bundle_path(debug_dir, path)
    path = Path(value)
    if path.is_absolute():
        return path
    if debug_dir is not None:
        return bundle_path(debug_dir, path)
    for root in (DEFAULT_DEBUG_DIR, LEGACY_DEBUG_DIR):
        candidate = bundle_path(root, path)
        if candidate.exists():
            return candidate
    return bundle_path(DEFAULT_DEBUG_DIR, path)


def show_bundle(directory: Path) -> None:
    comment, metadata = parse_comment(directory / "comment.txt")
    print(f"Bundle: {directory}")
    print(f"Comment: {comment or '(missing)'}")
    for key, value in metadata.items():
        print(f"{key}: {value}")
    stats = directory / "audio_stats.txt"
    if stats.is_file():
        print("Audio stats:")
        try:
            for line in stats.read_text(encoding="utf-8").splitlines():
                print(f"  {line}")
        except OSError:
            print("  (unreadable)")
    files = inventory(directory)
    print("Files:")
    if files:
        for file in files:
            print(f"  {file['name']} ({file['size']} bytes)")
    else:
        print("  (none)")
    print(f"replay.sh: {'yes' if (directory / 'replay.sh').is_file() else 'no'}")

def _option(command: list[str], name: str, value: str | None) -> None:
    while name in command:
        index = command.index(name)
        del command[index:min(index + 2, len(command))]
    if value is not None:
        command.extend((name, value))
def _set_output_path(command: list[str], option: str, out: Path, default: Path) -> Path:
    value = default.resolve()
    if option in command:
        index = command.index(option)
        if index + 1 >= len(command):
            raise ValueError(f"{option} has no path")
        recorded = Path(command[index + 1])
        candidate = recorded if recorded.is_absolute() else Path.cwd() / recorded
        candidate = candidate.resolve()
        try:
            candidate.relative_to(out.resolve())
            value = candidate
        except ValueError:
            pass
    _option(command, option, str(value))
    if option == "--dump-dir":
        value.mkdir(parents=True, exist_ok=True)
    else:
        value.parent.mkdir(parents=True, exist_ok=True)
    return value




def _prepare_save(directory: Path, out: Path, command: list[str]) -> None:
    """Run dq9 against an output-local save initialized from a bundle baseline."""
    directory = directory.resolve()
    out = out.resolve()
    start_save = directory / "session_start.sav"
    copied_start = out / "session_start.sav"
    if start_save.is_file():
        copied_start.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(start_save, copied_start)

    if "--save" not in command:
        if start_save.is_file():
            command.extend(("--save", str(copied_start)))
        return
    index = command.index("--save")
    if index + 1 >= len(command):
        raise ValueError("--save has no path")

    recorded = Path(command[index + 1])
    resolved = (recorded if recorded.is_absolute() else directory / recorded).resolve()
    bundle_replay_out = (directory / "replay_out").resolve()
    try:
        replay_relative = resolved.relative_to(bundle_replay_out)
    except ValueError:
        replay_relative = None
    if replay_relative is not None:
        target = out / replay_relative
        seed_from_start = True
    else:
        try:
            resolved.relative_to(out)
            output_local = True
        except ValueError:
            output_local = False
        if output_local:
            target = resolved
            seed_from_start = True
        elif start_save.is_file() and resolved == start_save.resolve():
            target = copied_start
            seed_from_start = True
        else:
            source = directory / recorded if not recorded.is_absolute() and (directory / recorded).is_file() else recorded
            target = out / (recorded.name or "replay-save.sav")
            seed_from_start = not source.is_file()
            if source.is_file():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)

    target.parent.mkdir(parents=True, exist_ok=True)
    if seed_from_start and start_save.is_file():
        shutil.copy2(start_save, target)
    command[index + 1] = str(target)


def build_replay_command(directory: Path, frame: int, *, dq9: Path | None = None,
                         rom: Path | None = None, out: Path, frames_before: int = 0) -> list[str]:
    out = out.resolve()
    script = directory / "replay.sh"
    if script.is_file():
        command = parse_replay_command(script, out=out)
    else:
        input_file = directory / "session_input.txt"
        if not input_file.is_file():
            raise ValueError("replay.sh and session_input.txt are both missing")
        command = [str(dq9 or DEFAULT_DQ9)]
    command[0] = str(dq9 or command[0])
    if rom:
        _option(command, "--rom", str(rom))
    elif "--rom" not in command:
        raise ValueError("replay requires --rom PATH")
    if "--input-script" not in command:
        input_file = directory / "session_input.txt"
        if not input_file.is_file():
            raise ValueError("session_input.txt is missing")
        command.extend(("--input-script", str(input_file)))
    else:
        index = command.index("--input-script")
        if index + 1 >= len(command):
            raise ValueError("--input-script has no path")
        recorded_input = Path(command[index + 1])
        local_input = directory / "session_input.txt"
        if local_input.is_file() and recorded_input.name == local_input.name:
            command[index + 1] = str(local_input.resolve())
    requested_frames = list(range(max(0, frame - frames_before), frame + 1))
    _option(command, "--frames", str(frame + 1))
    _option(command, "--dump-frames", ",".join(map(str, requested_frames)))
    _set_output_path(command, "--dump-dir", out, out)
    _set_output_path(command, "--dump-audio", out, out / "audio.wav")
    _prepare_save(directory, out, command)
    if "--headless" not in command:
        command.append("--headless")
    return command


def _capture(directory: Path, frame: int, screen: str) -> Path | None:
    for suffix in ("png", "ppm"):
        path = directory / f"frame_{frame:04d}_{screen}.{suffix}"
        if path.is_file():
            return path
    return None


def replay(directory: Path, *, frames_before: int = 0, dq9: Path | None = None,
           rom: Path | None = None, out: Path | None = None, runner=None,
           interactive: bool = False) -> int:
    runner = runner or subprocess.run
    if frames_before < 0:
        raise ValueError("--frames-before must be non-negative")
    _, metadata = parse_comment(directory / "comment.txt")
    try:
        frame = int(metadata["frame"])
    except (KeyError, ValueError):
        index_frame = None
        for row in parse_index(directory.parent / "index.tsv"):
            if bundle_path(directory.parent, str(row["directory"])).resolve() == directory.resolve():
                index_frame = int(row["frame"])
                break
        if index_frame is None:
            raise ValueError("bundle has no frame metadata or matching index.tsv entry")
        frame = index_frame
    if frame < 0:
        raise ValueError("bundle frame must be non-negative")
    if out is None:
        out = Path(tempfile.mkdtemp(prefix="dq9-debug-replay-"))
    out.mkdir(parents=True, exist_ok=True)
    out = out.resolve()
    command = build_replay_command(directory, frame, dq9=dq9, rom=rom, out=out,
                                   frames_before=frames_before)
    for key, option in (("rtc_epoch", "--rtc-epoch"),
                        ("rtc_timezone_offset_minutes", "--rtc-timezone-offset")):
        if key in metadata:
            _option(command, option, metadata[key])
    if interactive:
        _option(command, "--frames", None)
        _option(command, "--pause-at-frame", str(frame))
        _option(command, "--dump-audio", None)
        command = [argument for argument in command if argument != "--headless"]
        if "--debug-tool" not in command:
            command.append("--debug-tool")
        save_path = command[command.index("--save") + 1]
        print(f"Interactive quick reload at frame {frame}; writable save: {save_path}")
        result = runner(command)
        if result.returncode:
            print(f"INTERACTIVE REPLAY FAILED: dq9 exited {result.returncode}")
            return 2
        return 0
    dump_directory = Path(command[command.index("--dump-dir") + 1])
    result = runner(command)
    if result.returncode:
        print(f"REPLAY FAILED: dq9 exited {result.returncode}")
        return 2
    mismatches = 0
    compared = 0
    for screen in ("top", "bottom"):
        expected = next((directory / f"screenshot_{screen}.{ext}"
                         for ext in ("png", "ppm") if (directory / f"screenshot_{screen}.{ext}").is_file()), None)
        actual = _capture(dump_directory, frame, screen)
        if expected is None or actual is None:
            continue
        compared += 1
        diff = compare(read_image(expected), read_image(actual))
        different = int(diff["different"])
        mismatches += different
        print(f"{screen}: {different}/{diff['pixels']} pixels differ")
    if not compared:
        raise ValueError("no matching bundle and replay screenshots (top/bottom PNG or PPM)")
    print(f"Replay {'MATCH' if mismatches == 0 else 'MISMATCH'} ({compared} screen(s), {mismatches} differing pixels)")
    return 0 if mismatches == 0 else 1


def export_bundle(directory: Path, destination: Path, *, with_dumps: bool = False) -> bool:
    if not directory.is_dir():
        raise ValueError(f"bundle directory does not exist: {directory}")
    found_dumps = False
    destination = destination.resolve()
    with tarfile.open(destination, "w:gz") as archive:
        for path in sorted(directory.rglob("*")):
            if path.resolve() == destination or not path.is_file() or path.is_symlink():
                continue
            name = path.name.lower()
            if path.suffix.lower() in ROM_SUFFIXES or name in {"rom", "rom.nds", "game.nds"}:
                continue
            if path.name in DUMP_FILES:
                found_dumps = True
                if not with_dumps:
                    continue
            archive.add(path, arcname=f"{directory.name}/{path.relative_to(directory).as_posix()}", recursive=False)
    if found_dumps and not with_dumps:
        print("warning: mainram.bin/vram.bin omitted; use --with-dumps to include ROM-derived memory dumps", file=sys.stderr)
    return found_dumps


def main(argv: list[str] | None = None, runner=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--debug-dir", type=Path)
    subparsers = parser.add_subparsers(dest="action", required=True)
    listing = subparsers.add_parser("list", help="list bundles newest first")
    listing.add_argument("--limit", type=int)
    showing = subparsers.add_parser("show", help="show bundle details")
    showing.add_argument("bundle")
    replay_parser = subparsers.add_parser("replay", help="replay a bundle")
    replay_parser.add_argument("bundle")
    replay_parser.add_argument("--frames-before", type=int, default=0)
    replay_parser.add_argument("--interactive", action="store_true",
                               help="open dq9 at the noted frame with a fresh writable replay save")
    replay_parser.add_argument("--dq9", type=Path)
    replay_parser.add_argument("--rom", type=Path)
    replay_parser.add_argument("--out", type=Path)
    exporting = subparsers.add_parser("export", help="create a shareable tar.gz")
    exporting.add_argument("bundle")
    exporting.add_argument("--out", type=Path, required=True)
    exporting.add_argument("--with-dumps", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.action == "list":
            if args.limit is not None and args.limit < 0:
                raise ValueError("--limit must be non-negative")
            rows = sorted_index(args.debug_dir)
            if args.limit is not None:
                rows = rows[:args.limit]
            for number, row in enumerate(rows, 1):
                print(f"{number}. {row['timestamp']} {row['directory']} frame={row['frame']} {row['comment']}")
            return 0
        directory = select_bundle(args.debug_dir, args.bundle)
        if args.action == "show":
            show_bundle(directory)
            return 0
        if args.action == "replay":
            return replay(directory, frames_before=args.frames_before, dq9=args.dq9, rom=args.rom,
                          out=args.out, runner=runner, interactive=args.interactive)
        print(f"Exported {directory} -> {args.out}")
        return 0
    except (OSError, ValueError, ImageError, tarfile.TarError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
