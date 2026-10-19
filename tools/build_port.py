#!/usr/bin/env python3
"""Build a local SDL host from an image you already own (experimental, local-only).

Everything this script produces is derived from your image. It stays under
`build/` and `extracted/` (both git-ignored) and must never be uploaded or shared.

Steps:
  1. optionally verify the image against a private fingerprint profile;
  2. extract the image and initialise the analysis workspace with ds-decomp (`dsd`);
  3. build the ARM9 manifest from the analysis files;
  4. discover ARM7 code from the extracted ARM7 binary;
  5. recompile both CPUs to host C++ (python -m recomp);
  6. configure and build the `dq9` executable (unless --generate-only).

Known limitation: modules that ship in protected form are NOT unpacked by this
public tool. A host built without them can fail early at runtime. This project
does not currently claim a playable game; see docs/GETTING_STARTED.md.

Usage:
  python tools/build_port.py --rom PATH --dsd PATH --arm7-metadata PATH.json
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

try:
    from .host_tools import executable_name
except ImportError:
    from host_tools import executable_name

ROOT = Path(__file__).resolve().parent.parent
WORKSPACE = ROOT / "extracted" / "workspace"
GEN = ROOT / "build" / "gen"


def run(*cmd: object, cwd: Path = ROOT) -> None:
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], cwd=cwd, check=True)


def arm7_header_layout(rom: Path) -> tuple[int, int]:
    """Return (ram address, entry address) of the ARM7 binary from the standard NDS header."""
    with rom.open("rb") as f:
        header = f.read(0x40)
    if len(header) < 0x40:
        raise SystemExit(f"{rom}: too small to be a Nintendo DS image")
    entry, ram = struct.unpack_from("<II", header, 0x34)
    return ram, entry


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--rom", type=Path, required=True, help="your own image")
    p.add_argument("--dsd", type=Path, required=True, help="ds-decomp executable you installed yourself")
    p.add_argument("--arm7-metadata", type=Path, required=True,
                   help="private JSON with at least autoload_callback (see tools/arm7_analyze.py)")
    p.add_argument("--profile", type=Path, help="private fingerprint profile JSON for verify_reference_rom.py")
    p.add_argument("--workspace", type=Path, default=WORKSPACE)
    p.add_argument("--build-dir", type=Path, default=ROOT / "build" / "game")
    p.add_argument("--config", default="RelWithDebInfo")
    p.add_argument("--generate-only", action="store_true", help="stop after generating C++")
    args = p.parse_args(argv)

    rom = args.rom.resolve()
    py = sys.executable
    tools = ROOT / "tools"

    if args.profile:
        run(py, tools / "verify_reference_rom.py", "--rom", rom, "--profile", args.profile)

    extract = args.workspace / "extract"
    config_dir = args.workspace / "config"
    build_dir = args.workspace / "build"
    config = config_dir / "arm9" / "config.yaml"
    if not config.exists():
        extract.mkdir(parents=True, exist_ok=True)
        run(args.dsd, "rom", "extract", "--rom", rom, "--output-path", extract)
        run(py, tools / "prepare_dsd_config.py", extract / "config.yaml", extract / "config-dsd.yaml")
        run(args.dsd, "init", "--rom-config", extract / "config-dsd.yaml",
            "--output-path", config_dir, "--build-path", build_dir)

    images = build_dir / "build"
    images.mkdir(parents=True, exist_ok=True)
    pairs = [(extract / "arm9" / n, images / n) for n in ("arm9.bin", "itcm.bin", "dtcm.bin")]
    pairs += [(f, images / f"arm9_{f.name}") for f in sorted((extract / "arm9_overlays").glob("ov*.bin"))]
    for src, dst in pairs:
        if src.exists() and not dst.exists():
            dst.write_bytes(src.read_bytes())

    GEN.mkdir(parents=True, exist_ok=True)
    manifest9 = GEN / "arm9.manifest.json"
    run(py, "-m", "recomp.dsd_manifest", "--config", config, "--out", manifest9)

    base, entry = arm7_header_layout(rom)
    meta = json.loads(args.arm7_metadata.read_text(encoding="utf-8"))
    if "autoload_callback" not in meta:
        raise SystemExit("--arm7-metadata must contain autoload_callback")
    run(py, tools / "arm7_analyze.py", "--arm7", extract / "arm7" / "arm7.bin", "--out", GEN,
        "--base", hex(base), "--entry", hex(entry), "--metadata", args.arm7_metadata)

    run(py, "-m", "recomp", "--manifest", manifest9, "--out", GEN / "arm9")
    run(py, "-m", "recomp", "--manifest", GEN / "arm7.manifest.json", "--out", GEN / "arm7")
    if args.generate_only:
        print(f"generated C++ in {GEN}")
        return 0

    run("cmake", "-S", ROOT, "-B", args.build_dir, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DDQ9_GENERATED_DIR={GEN}")
    run("cmake", "--build", args.build_dir, "--target", "dq9")
    print(f"built {args.build_dir / 'game' / executable_name('dq9')} (local only; never distribute)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
