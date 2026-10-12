#!/usr/bin/env python3
"""Read-only Ghidra queries for DQ9 modules (Ghidra 12.1.4 + JDK 21).

The wrapper uses Ghidra's analyzeHeadless launcher and PyGhidra postScripts.
Imports run analysis once and save one program per module under build/ghidra;
all query processes open the cached program with analysis disabled and never save.

PyGhidra is bootstrapped from Ghidra's bundled offline wheels into ignored
build/ghidra/python; no system Python install or network server is required.
Each invocation pays JVM startup cost; ``batch <module> queries.json`` accepts
up to 100 objects shaped ``{"command":"funcs","args":{"grep":"foo","limit":10}}``
to amortize it. Functions, strings, and symbols are capped; decompiled C is
truncated at 40,000 characters and disassembly at 4,096 bytes.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zipfile
import tempfile
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]
GHIDRA_DIR = Path(os.environ.get("GHIDRA_INSTALL_DIR", Path.home() / "Applications/ghidra_12.1.4_PUBLIC"))
HEADLESS = GHIDRA_DIR / "support" / "pyghidraRun"
SCRIPT_DIR = ROOT / "tools" / "ghidra"
CACHE = ROOT / "build" / "ghidra"
SYMBOLS_ROOT = ROOT / "extracted" / "dqix-decomp-eu" / "config" / "eur" / "arm9"
EXTRACT = ROOT / "extracted" / "dqix-decomp-eu" / "extract" / "eur"
ARM7_MANIFEST = ROOT / "build" / "gen" / "arm7.manifest.json"
ARM_MODULES = {"main", "itcm", "dtcm"} | {f"ov{i:03}" for i in range(35)}
ARM7_MODULES = {"arm7main", "arm7wram", "arm7autoload"}
MARKER = "GHIDRA_QUERY_JSON:"


def pyghidra_environment() -> dict[str, str]:
    """Use Ghidra's bundled, offline PyGhidra wheels without modifying system Python."""
    wheel_dir = GHIDRA_DIR / "Ghidra" / "Features" / "PyGhidra" / "pypkg" / "dist"
    package_dir = CACHE / "python"
    package_dir.mkdir(parents=True, exist_ok=True)
    if not (package_dir / "pyghidra").is_dir():
        py_tag = f"cp{sys.version_info.major}{sys.version_info.minor}"
        patterns = (
            "pyghidra-*.whl",
            f"jpype1-*-{py_tag}-{py_tag}-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
            "packaging-*.whl",
        )
        for pattern in patterns:
            candidates = sorted(wheel_dir.glob(pattern))
            if not candidates:
                raise FileNotFoundError(f"no compatible bundled PyGhidra wheel for {pattern} in {wheel_dir}")
            with zipfile.ZipFile(candidates[0]) as wheel:
                wheel.extractall(package_dir)
    env = os.environ.copy()
    env["PYTHONPATH"] = str(package_dir) + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
    return env


def module_spec(module: str) -> dict[str, object]:
    """Return source image, base, language and DSD symbol metadata for module."""
    if module in ARM_MODULES:
        if module == "main":
            image, config = EXTRACT / "arm9" / "arm9.bin", EXTRACT / "arm9" / "arm9.yaml"
            symbol_file = SYMBOLS_ROOT / "symbols.txt"
        elif module in {"itcm", "dtcm"}:
            image, config = EXTRACT / "arm9" / f"{module}.bin", EXTRACT / "arm9" / f"{module}.yaml"
            symbol_file = SYMBOLS_ROOT / module / "symbols.txt"
        else:
            image = EXTRACT / "arm9_overlays" / f"ov{int(module[2:]):03}.bin"
            config = EXTRACT / "arm9_overlays" / "overlays.yaml"
            symbol_file = SYMBOLS_ROOT / "overlays" / module / "symbols.txt"
        if not image.is_file():
            # DSD-built modules are equivalent decompressed module images and are useful
            # where the extracted overlay files are stored in a separately named directory.
            built = ROOT / "extracted" / "dqix-decomp-eu" / "build" / "eur" / "build"
            image = built / ("arm9.bin" if module == "main" else f"arm9_{module}.bin")
        text = config.read_text(encoding="utf-8")
        if module.startswith("ov"):
            match = re.search(rf"(?ms)^\s*- id: {int(module[2:])}\s*\n(.*?)(?=^\s*- id:|\Z)", text)
            if not match or not (base_match := re.search(r"base_address:\s*(\d+)", match.group(1))):
                raise ValueError(f"no base address configured for {module}")
            base = int(base_match.group(1))
        else:
            match = re.search(r"(?m)^base_address:\s*(\d+)", text)
            if not match:
                raise ValueError(f"no base address configured for {module}")
            base = int(match.group(1))
        return {"image": image, "base": base, "language": "ARM:LE:32:v5t", "symbols": symbol_file, "module": module}
    if module in ARM7_MODULES:
        if not ARM7_MANIFEST.is_file():
            raise FileNotFoundError(f"ARM7 module metadata is missing: {ARM7_MANIFEST}")
        manifest = json.loads(ARM7_MANIFEST.read_text(encoding="utf-8"))
        record = next((entry for entry in manifest["modules"] if entry["name"] == module), None)
        if record is None:
            raise ValueError(f"ARM7 module {module!r} not found in manifest")
        image = Path(record["image"])
        if not image.is_absolute():
            image = ROOT / image
        return {"image": image, "base": record["base"], "language": "ARM:LE:32:v4t", "manifest": ARM7_MANIFEST, "module": module}
    raise ValueError(f"unknown module {module!r}; expected main, itcm, dtcm, ov000..ov034, arm7main, arm7wram, or arm7autoload")


def project_paths(module: str) -> tuple[Path, str]:
    path = CACHE / module
    return path, module


def build_command(module: str, operation: str, script_args: Sequence[str] = ()) -> list[str]:
    """Build an analyzeHeadless invocation through Ghidra's standalone PyGhidra runtime."""
    project, project_name = project_paths(module)
    command = [sys.executable, "-m", "pyghidra.ghidra_launch", "--install-dir", str(GHIDRA_DIR), "ghidra.app.util.headless.AnalyzeHeadless", str(project), project_name]
    if operation == "import":
        spec = module_spec(module)
        command += ["-import", str(spec["image"]), "-overwrite", "-loader", "BinaryLoader", "-loader-baseAddr", f"0x{int(spec['base']):x}", "-processor", str(spec["language"]), "-noanalysis", "-postScript", str(SCRIPT_DIR / "import_module.py")]
        command += [json.dumps({k: (str(v) if isinstance(v, Path) else v) for k, v in spec.items() if k != "image"})]
    else:
        command += ["-process", Path(module_spec(module)["image"]).name, "-noanalysis", "-postScript", str(SCRIPT_DIR / "query_module.py"), operation, *script_args]
    return command


MAX_LOG_BYTES = 4 * 1024 * 1024


def _capture_bounded(command: Sequence[str]) -> tuple[int, bytes]:
    """Run a command while retaining only its final MAX_LOG_BYTES in a temp log."""
    total = 0
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(
            command, cwd=ROOT, env=pyghidra_environment(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        assert process.stdout is not None
        while True:
            chunk = process.stdout.read(65536)
            if not chunk:
                break
            if len(chunk) >= MAX_LOG_BYTES:
                offset = (total + len(chunk) - MAX_LOG_BYTES) % MAX_LOG_BYTES
                tail = chunk[-MAX_LOG_BYTES:]
                first = MAX_LOG_BYTES - offset
                log.seek(offset)
                log.write(tail[:first])
                if first < MAX_LOG_BYTES:
                    log.seek(0)
                    log.write(tail[first:])
            else:
                offset = total % MAX_LOG_BYTES
                first = min(len(chunk), MAX_LOG_BYTES - offset)
                log.seek(offset)
                log.write(chunk[:first])
                if first < len(chunk):
                    log.seek(0)
                    log.write(chunk[first:])
            total += len(chunk)
        returncode = process.wait()
        retained = min(total, MAX_LOG_BYTES)
        start = total % MAX_LOG_BYTES if total > MAX_LOG_BYTES else 0
        if total > MAX_LOG_BYTES:
            log.seek(start)
            data = log.read(MAX_LOG_BYTES - start)
            log.seek(0)
            data += log.read(start)
        else:
            log.seek(0)
            data = log.read(retained)
    return returncode, data


def nonnegative_int(text: str) -> int:
    value = int(text, 0)
    if value < 0:
        raise argparse.ArgumentTypeError("must be zero or greater")
    return value

def run(module: str, operation: str, script_args: Sequence[str] = ()) -> tuple[dict[str, object], float]:
    command = build_command(module, operation, script_args)
    start = time.perf_counter()
    returncode, output = _capture_bounded(command)
    elapsed = time.perf_counter() - start
    text = output.decode("utf-8", errors="replace")
    found = None
    for line in text.splitlines():
        if line.startswith(MARKER):
            found = line[len(MARKER):]
    detail = text[-8000:]
    if returncode:
        raise RuntimeError(f"Ghidra command failed ({returncode}): {detail.strip()}")
    if found is None:
        raise RuntimeError(f"Ghidra script did not emit JSON: {detail.strip()}")
    try:
        result = json.loads(found)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Ghidra emitted invalid JSON: {exc}") from exc
    if isinstance(result, dict) and result.get("error"):
        raise RuntimeError(f"Ghidra query failed: {result['error']}")
    result["latency_seconds"] = round(elapsed, 3)
    return result, elapsed


def positive_int(text: str) -> int:
    value = int(text, 0)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return value


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    imp = sub.add_parser("import", help="create/refresh a cached module and run analysis once")
    imp.add_argument("module")
    imp.add_argument("--max-import-failures", type=nonnegative_int, default=0, help="failure count above which import is marked with a warning")
    imp.add_argument("--strict-import", action="store_true", help="exit nonzero when import failures exceed --max-import-failures")
    for name in ("funcs", "strings", "symbols"):
        p = sub.add_parser(name)
        p.add_argument("module")
        p.add_argument("--grep", dest="grep")
        if name in {"funcs", "strings"}:
            p.add_argument("--limit", type=positive_int, default=100)
    for name in ("decompile", "callers", "callees"):
        p = sub.add_parser(name)
        p.add_argument("module")
        p.add_argument("target", help="function name or address (hex or decimal)")
    p = sub.add_parser("disasm")
    p.add_argument("module")
    p.add_argument("addr")
    p.add_argument("--len", type=positive_int, default=0x80)
    p = sub.add_parser("xrefs")
    p.add_argument("module")
    p.add_argument("addr")
    p = sub.add_parser("batch", help="run multiple query objects in one JVM")
    p.add_argument("module")
    p.add_argument("queries", type=Path)
    args = parser.parse_args(argv)
    try:
        if not HEADLESS.is_file():
            raise FileNotFoundError(
                f"Ghidra analyzeHeadless not found at checked path {HEADLESS}; "
                f"set GHIDRA_INSTALL_DIR (currently {os.environ.get('GHIDRA_INSTALL_DIR', '<unset>')})"
            )
        if args.command == "import":
            spec = module_spec(args.module)
            if not Path(spec["image"]).is_file():
                raise FileNotFoundError(f"module image not found: {spec['image']}")
            CACHE.mkdir(parents=True, exist_ok=True)
            project_paths(args.module)[0].mkdir(parents=True, exist_ok=True)
            result, _ = run(args.module, "import")
            failures = sum(int(result.get(key, 0)) for key in ("functions_failed", "labels_failed", "symbol_lines_skipped"))
            result["incomplete"] = failures > 0
            print(json.dumps(result, indent=2))
            if failures > args.max_import_failures:
                print(f"ghidra_query: import incomplete: {failures} failures (functions={result.get('functions_failed', 0)}, labels={result.get('labels_failed', 0)}, skipped_lines={result.get('symbol_lines_skipped', 0)})", file=sys.stderr)
                if args.strict_import:
                    return 1
            return 0
        query_args: list[str]
        if args.command == "batch":
            requests = json.loads(args.queries.read_text(encoding="utf-8"))
            if not isinstance(requests, list) or len(requests) > 100:
                raise ValueError("batch file must be a JSON list of at most 100 query objects")
            query_args = [json.dumps(requests)]
        elif args.command in {"funcs", "strings", "symbols"}:
            query_args = [json.dumps({"grep": args.grep, "limit": getattr(args, "limit", 500)})]
        elif args.command == "disasm":
            query_args = [json.dumps({"addr": args.addr, "length": args.len})]
        elif args.command == "xrefs":
            query_args = [json.dumps({"addr": args.addr})]
        else:
            query_args = [json.dumps({"target": args.target})]
        result, _ = run(args.module, args.command, query_args)
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"ghidra_query: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
