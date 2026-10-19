#!/usr/bin/env python3
"""Build a local SDL host from an image you already own.

Everything this script produces is derived from your image. It stays under
`build/` and `extracted/` (both git-ignored) and must never be uploaded or shared.

Steps:
  1. optionally verify the image against a private fingerprint profile;
  2. extract the image and initialise the analysis workspace with ds-decomp (`dsd`);
  3. build the ARM9 manifest and resolve protected overlay code from your own image;
  4. discover ARM7 code from the extracted ARM7 binary;
  5. derive the thread-layer configuration `build/gen/abi.ini` from your own binaries;
  6. recompile both CPUs to host C++ (python -m recomp);
  7. configure and build the `dq9` executable (unless --generate-only).

Usage:
  python tools/build_port.py --rom PATH --dsd PATH
  build/game/game/dq9 --rom PATH --abi-config build/gen/abi.ini [--language en|fr|de|es|it]

See docs/GETTING_STARTED.md for the verified status.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import yaml

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


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--rom", type=Path, required=True, help="your own image")
    p.add_argument("--dsd", type=Path, required=True, help="ds-decomp executable you installed yourself")
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
    # The manifest builder reads per-module load addresses from these YAML files
    # next to the images; without them every non-main module gets base 0.
    for name in ("arm9.yaml", "itcm.yaml", "dtcm.yaml"):
        shutil.copyfile(extract / "arm9" / name, images / name)
    overlays_yaml = (extract / "arm9_overlays" / "overlays.yaml").read_text(encoding="utf-8")
    (images / "arm9_overlays.yaml").write_text(
        re.sub(r"file_name: ov(\d+)\.bin", r"file_name: arm9_ov\1.bin", overlays_yaml), encoding="utf-8")

    GEN.mkdir(parents=True, exist_ok=True)
    manifest9 = GEN / "arm9.manifest.json"
    run(py, "-m", "recomp.dsd_manifest", "--config", config, "--out", manifest9)
    run(py, tools / "unwrap_protected_overlay.py", "--manifest", manifest9, "--out", GEN)

    # DSD writes the ARM7 load address, entry and autoload callback while extracting.
    arm7_meta = yaml.safe_load((extract / "arm7" / "arm7.yaml").read_text(encoding="utf-8"))
    for key in ("base_address", "entry_function", "autoload_callback"):
        if key not in arm7_meta:
            raise SystemExit(f"{extract / 'arm7' / 'arm7.yaml'} lacks {key}")
    run(py, tools / "arm7_analyze.py", "--arm7", extract / "arm7" / "arm7.bin", "--out", GEN,
        "--base", hex(arm7_meta["base_address"]), "--entry", hex(arm7_meta["entry_function"]),
        "--autoload-callback", hex(arm7_meta["autoload_callback"]))

    abi = GEN / "abi.ini"
    run(py, tools / "derive_abi_config.py", "--workspace", args.workspace, "--out", abi, "--report", GEN / "abi-report.json")

    run(py, "-m", "recomp", "--manifest", manifest9, "--out", GEN / "arm9")
    run(py, "-m", "recomp", "--manifest", GEN / "arm7.manifest.json", "--out", GEN / "arm7")
    if args.generate_only:
        print(f"generated C++ in {GEN}")
        return 0

    run("cmake", "-S", ROOT, "-B", args.build_dir, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DDQ9_GENERATED_DIR={GEN}")
    run("cmake", "--build", args.build_dir, "--target", "dq9")
    exe = args.build_dir / "game" / executable_name("dq9")
    print(f"built {exe} (local only; never distribute)")
    print(f'run: "{exe}" --rom "{rom}" --abi-config "{abi}" --language en')
    return 0


if __name__ == "__main__":
    sys.exit(main())
