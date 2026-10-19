#!/usr/bin/env python3
"""Pre-build readiness checks for tools/build_port.py.

Reports, before the long build starts, whether Python, the pip packages, CMake/Ninja, a
compiler, free disk space and the Nintendo DS image are usable. The image check is purely
structural (size, header CRC, payloads inside the file); it contains no game-specific data.

Usage: python tools/build_port.py --check --rom PATH
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

OK, WARN, FAIL = "OK", "WARN", "FAIL"
MIN_FREE_GB = 5


@dataclass
class Check:
    name: str
    status: str
    detail: str


def _crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def check_rom_image(path: Path) -> Check:
    """Structural DS image check mirroring dsr::check_rom_image in the runtime."""
    name = "Nintendo DS image"
    if not path.is_file():
        return Check(name, FAIL, f"not found: {path}")
    size = path.stat().st_size
    if size < 0x4000:
        return Check(name, FAIL, "the file is too small to be a Nintendo DS image")
    with path.open("rb") as handle:
        header = handle.read(0x200)
    arm9_off, arm9_size, arm7_off, arm7_size = struct.unpack_from("<4I", header, 0x20)[0], struct.unpack_from("<I", header, 0x2C)[0], struct.unpack_from("<I", header, 0x30)[0], struct.unpack_from("<I", header, 0x3C)[0]
    used = struct.unpack_from("<I", header, 0x80)[0]
    if used > size and used < (1 << 30):
        return Check(name, FAIL, f"the image is truncated ({size} of {used} bytes); copy or dump it again")
    if not arm9_size or not arm7_size or arm9_off + arm9_size > size or arm7_off + arm7_size > size:
        return Check(name, FAIL, "this does not look like a Nintendo DS image (its header is not valid)")
    if _crc16(header[:0x15E]) != struct.unpack_from("<H", header, 0x15E)[0]:
        return Check(name, WARN, "the header checksum does not match (patched or modified image?)")
    return Check(name, OK, f"{size / 1048576:.0f} MB, header valid")


def check_python() -> Check:
    version = ".".join(map(str, sys.version_info[:3]))
    if sys.version_info < (3, 10):
        return Check("Python", FAIL, f"{version}; 3.10 or newer is required")
    return Check("Python", OK, version)


def check_modules() -> Check:
    missing = [m for m in ("yaml", "capstone", "unicorn") if importlib.util.find_spec(m) is None]
    if missing:
        names = {"yaml": "PyYAML"}
        packages = " ".join(names.get(m, m) for m in missing)
        return Check("Python packages", FAIL, f"missing {', '.join(missing)}; run: {sys.executable} -m pip install {packages}")
    return Check("Python packages", OK, "PyYAML, capstone, unicorn")


def _runs(*cmd: str) -> bool:
    try:
        return subprocess.run(cmd, capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def check_build_tools() -> list[Check]:
    checks = []
    if _runs(sys.executable, "-m", "cmake", "--version"):
        checks.append(Check("CMake (pip)", OK, "python -m cmake works"))
    else:
        checks.append(Check("CMake (pip)", FAIL, f"run: {sys.executable} -m pip install cmake"))
    if _runs(sys.executable, "-m", "ninja", "--version") or shutil.which("ninja"):
        checks.append(Check("Ninja", OK, "found"))
    else:
        checks.append(Check("Ninja", FAIL, f"run: {sys.executable} -m pip install ninja"))
    if os.name == "nt":
        if importlib.util.find_spec("ziglang") is not None:
            checks.append(Check("C++ compiler (Zig)", OK, "ziglang installed"))
        else:
            checks.append(Check("C++ compiler (Zig)", WARN, "ziglang will be installed by the build (pip, needs internet)"))
    elif shutil.which("g++") or shutil.which("clang++") or shutil.which("c++"):
        checks.append(Check("C++ compiler", OK, "found"))
    else:
        checks.append(Check("C++ compiler", FAIL, "no g++/clang++ found; install build-essential (or your distribution's C++ toolchain)"))
    return checks


def check_disk(path: Path) -> Check:
    probe = path
    while not probe.exists() and probe.parent != probe:
        probe = probe.parent
    free_gb = shutil.disk_usage(probe).free / 1e9
    if free_gb < MIN_FREE_GB:
        return Check("Free disk space", FAIL, f"{free_gb:.1f} GB free; about {MIN_FREE_GB} GB is needed")
    return Check("Free disk space", OK, f"{free_gb:.0f} GB free")


def run_checks(rom: Path | None, work_dir: Path) -> list[Check]:
    checks = [check_python(), check_modules(), *check_build_tools(), check_disk(work_dir)]
    if rom is not None:
        checks.append(check_rom_image(rom))
    return checks


def report(checks: list[Check]) -> bool:
    """Prints one line per check; returns True when nothing failed."""
    for check in checks:
        print(f"[{check.status:^4}] {check.name}: {check.detail}")
    return all(check.status != FAIL for check in checks)
