#!/usr/bin/env python3
"""Drive the patched local DeSmuME CLI as a deterministic capture oracle."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib

KEY_BITS = {
    "A": 0, "B": 1, "SELECT": 2, "START": 3, "RIGHT": 4, "LEFT": 5,
    "UP": 6, "DOWN": 7, "R": 8, "L": 9, "X": 10, "Y": 11,
}


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)


def ppm_to_png(source: Path, target: Path) -> None:
    data = source.read_bytes()
    # P6 header has a fixed shape in this tool's output.
    match = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
    if not match:
        raise ValueError(f"invalid PPM framebuffer: {source}")
    width, height = map(int, match.groups())
    raw = data[match.end():]
    if len(raw) != width * height * 3:
        raise ValueError(f"truncated PPM framebuffer: {source}")
    scanlines = b"".join(b"\0" + raw[y * width * 3:(y + 1) * width * 3] for y in range(height))
    target.write_bytes(b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0)) + png_chunk(b"IDAT", zlib.compress(scanlines, 6)) + png_chunk(b"IEND", b""))
    source.unlink()



def parse_snapshots(text: str, frame_count: int) -> list[int]:
    if not text.strip():
        raise ValueError("snapshot list must not be empty")
    frames: set[int] = set()
    for token in text.split(","):
        token = token.strip()
        if not token:
            raise ValueError("snapshot list contains an empty item")
        match = re.fullmatch(r"(\d+)(?:-(\d+)(?:/(\d+))?)?", token)
        if not match:
            raise ValueError(f"invalid snapshot range {token!r}")
        first = int(match.group(1))
        last = int(match.group(2) or first)
        step = int(match.group(3) or 1)
        if last < first:
            raise ValueError(f"snapshot range ends before it starts: {token!r}")
        if step < 1:
            raise ValueError(f"snapshot step must be positive: {token!r}")
        if last >= frame_count:
            raise ValueError(f"snapshot frame {last} is outside 0..{frame_count - 1}")
        frames.update(range(first, last + 1, step))
    return sorted(frames)

def manifest_entries(*manifests: Path | None) -> set[int]:
    addresses: set[int] = set()
    for manifest_path in manifests:
        if manifest_path and manifest_path.exists():
            manifest = json.loads(manifest_path.read_text())
            for module in manifest.get("modules", []):
                for function in module.get("functions", []):
                    addresses.add(int(function["addr"]))
    return addresses


def symbol_entries(
    symbols: Path | None, arm9_manifest: Path | None, arm7_manifest: Path | None
) -> set[int]:
    addresses: set[int] = set()
    if symbols and symbols.exists():
        for line in symbols.read_text(errors="replace").splitlines():
            if "kind:function" in line:
                match = re.search(r"addr:(0x[0-9a-fA-F]+)", line)
                if match:
                    addresses.add(int(match.group(1), 16))
    addresses.update(manifest_entries(arm9_manifest, arm7_manifest))
    return addresses


def rom_arm9_entry(rom: Path) -> int:
    """Read the ARM9 entry field from the standard Nintendo DS header."""
    with rom.open("rb") as source:
        source.seek(0x24)
        field = source.read(4)
    if len(field) != 4:
        raise ValueError("ROM header is too short to contain the ARM9 entry field")
    return struct.unpack("<I", field)[0]


def make_input_file(script: Path | None, target: Path) -> None:
    actions = json.loads(script.read_text()) if script else []
    if not isinstance(actions, list):
        raise ValueError("input script must be a JSON array")
    by_frame: dict[int, tuple[int, int, int]] = {}
    for action in actions:
        frame = int(action["frame"])
        mask = 0
        for key in action.get("buttons", []):
            try:
                mask |= 1 << KEY_BITS[str(key).upper()]
            except KeyError as exc:
                raise ValueError(f"unknown button {key!r}") from exc
        touch = action.get("touch")
        x, y = (int(touch[0]), int(touch[1])) if touch is not None else (-1, -1)
        if touch is not None and not (0 <= x < 256 and 0 <= y < 192):
            raise ValueError(f"touch coordinates out of range: {touch}")
        by_frame[frame] = (mask, x, y)
    target.write_text("".join(f"{frame} {mask} {x} {y}\n" for frame, (mask, x, y) in sorted(by_frame.items())))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--entry", type=lambda text: int(text, 0),
                        help="override the ARM9 entry address normally read from ROM header offset 0x24")
    parser.add_argument("--frames", required=True, type=int)
    parser.add_argument("--input", type=Path, help="JSON input script, frame-indexed")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--binary", type=Path, default=Path("reference/desmume-trace/build/cli/cli/desmume-cli"))
    parser.add_argument("--symbols", type=Path, help="optional caller-supplied function symbol export")
    parser.add_argument("--arm9-manifest", type=Path, default=Path("build/gen/arm9.manifest.json"))
    parser.add_argument("--arm7-manifest", type=Path, default=Path("build/gen/arm7.manifest.json"))
    parser.add_argument("--snapshots", default="0,100,300,599",
                        help="comma-separated frame indices; 'a-b' and 'a-b/step' are inclusive ranges")
    parser.add_argument("--hashes", type=Path,
                        help="write 'frame top_fnv1a64 bottom_fnv1a64' for every executed frame (RGB bytes of the PNG captures)")
    parser.add_argument("--no-ram", action="store_true",
                        help="skip the 16 MiB main-RAM and register snapshots (PNG captures only)")
    parser.add_argument("--coverage", action="store_true", help="record unique executed PCs by CPU and instruction set")
    parser.add_argument("--call-targets", type=Path,
                        help="record decoder-derived call targets with live code fingerprints (requires install_call_targets.py)")
    parser.add_argument("--trace-limit", type=int, default=1_000_000,
                        help="maximum number of function-entry trace rows")
    args = parser.parse_args(argv)
    if args.frames < 1 or args.trace_limit < 1:
        parser.error("frames and trace-limit must be positive")
    try:
        parse_snapshots(args.snapshots, args.frames)
    except ValueError as exc:
        parser.error(str(exc))
    if not args.rom.is_file():
        parser.error(f"ROM not found: {args.rom}")
    if not args.binary.is_file():
        parser.error(f"DeSmuME binary not found: {args.binary}; see tools/oracle/README.md")
    try:
        entry = args.entry if args.entry is not None else rom_arm9_entry(args.rom)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    if not 0 <= entry <= 0xffffffff:
        parser.error("entry must be a 32-bit address")
    if args.hashes:
        if args.hashes.exists() and not args.hashes.is_file():
            parser.error(f"hash destination is not a regular file: {args.hashes}")
        if args.hashes.exists() and not os.access(args.hashes, os.W_OK):
            parser.error(f"hash destination is not writable: {args.hashes}")
        hash_path = args.hashes.resolve()
        protected = [args.rom, args.binary, args.input, args.symbols,
                     args.arm9_manifest, args.arm7_manifest,
                     args.output / "inputs.txt", args.output / "entries.txt", args.output / "calls.txt"]
        if any(path is not None and hash_path == path.resolve() for path in protected):
            parser.error("hash destination must not overwrite an input, binary, or oracle control file")
        try:
            args.hashes.parent.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            parser.error(f"cannot prepare hash destination {args.hashes}: {exc}")
        if not os.access(args.hashes.parent, os.W_OK):
            parser.error(f"hash destination directory is not writable: {args.hashes.parent}")
    args.output.mkdir(parents=True, exist_ok=True)
    inputs = args.output / "inputs.txt"
    entries = args.output / "entries.txt"
    make_input_file(args.input, inputs)
    addresses = symbol_entries(args.symbols, args.arm9_manifest, args.arm7_manifest)
    # Always trace the supplied image's entry, even without optional symbols.
    addresses.add(entry)
    entries.write_text("".join(f"{address:x}\n" for address in sorted(addresses)))
    env = os.environ.copy()
    env.update({
        "DS_ORACLE_FRAMES": str(args.frames),
        "DS_ORACLE_INPUT": str(inputs.resolve()),
        "DS_ORACLE_OUT": str(args.output.resolve()),
        "DS_ORACLE_ENTRIES": str(entries.resolve()),
        "DS_ORACLE_TRACE": str((args.output / "calls.txt").resolve()),
        "DS_ORACLE_TRACE_LIMIT": str(args.trace_limit),
        "DS_ORACLE_SNAPSHOTS": args.snapshots,
    })
    if args.coverage:
        env["DS_ORACLE_COVERAGE"] = "1"
    else:
        env.pop("DS_ORACLE_COVERAGE", None)
    if args.call_targets:
        args.call_targets.parent.mkdir(parents=True, exist_ok=True)
        env["DS_ORACLE_CALL_TARGETS"] = str(args.call_targets.resolve())
    else:
        env.pop("DS_ORACLE_CALL_TARGETS", None)
    if args.hashes:
        env["DS_ORACLE_HASH_FILE"] = str(args.hashes.resolve())
    else:
        env.pop("DS_ORACLE_HASH_FILE", None)
    if args.no_ram:
        env["DS_ORACLE_NO_RAM"] = "1"
    else:
        env.pop("DS_ORACLE_NO_RAM", None)
    command = [str(args.binary.resolve()), "--disable-limiter", "--disable-sound", "--3d-render", "SW", str(args.rom.resolve())]
    print("Running:", " ".join(command), flush=True)
    result = subprocess.run(command, env=env)
    if result.returncode:
        return result.returncode
    for ppm in sorted(args.output.glob("frame_*_*.ppm")):
        ppm_to_png(ppm, ppm.with_suffix(".png"))
    print(f"Captured {len(list(args.output.glob('frame_*_top.png')))} top and {len(list(args.output.glob('frame_*_bottom.png')))} bottom PNGs")
    print(f"Function-entry trace: {args.output / 'calls.txt'}")
    omitted = [f"--{name} ({path if path is not None else 'not supplied'})" for name, path in (
        ("symbols", args.symbols), ("arm9-manifest", args.arm9_manifest), ("arm7-manifest", args.arm7_manifest)
    ) if path is None or not path.is_file()]
    print("Optional trace inputs omitted: " + (", ".join(omitted) if omitted else "none"))
    if args.coverage:
        for cpu in ("arm9", "arm7"):
            path = args.output / f"pcs_{cpu}.txt"
            count = sum(1 for _ in path.open()) if path.exists() else 0
            print(f"{cpu} unique PCs: {count} ({path})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"oracle: {error}", file=sys.stderr)
        raise SystemExit(2)
