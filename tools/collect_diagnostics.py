#!/usr/bin/env python3
"""Collect a small, redacted support ZIP, never local game data (exit 2 on refusal)."""
from __future__ import annotations

import argparse
import ast
from collections import deque
import configparser
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import zipfile

try:
    from .audit_package import KIT_BINARY_EXTENSIONS, classify
except ImportError:
    from audit_package import KIT_BINARY_EXTENSIONS, classify

MEMBERS = frozenset({"system.json", "tools.json", "configuration.json", "binaries.json",
                     "script-sha256.json", "build-log-tail.txt"})
SCRIPT_SUFFIXES = {".py", ".bat", ".ps1", ".sh"}
ROOT = Path(__file__).resolve().parents[1]


def check_name(name: str) -> None:
    path = PurePosixPath(name.replace("\\", "/"))
    if (path.is_absolute() or ".." in path.parts or
            path.suffix.lower() in KIT_BINARY_EXTENSIONS or classify(path.as_posix(), 0)):
        raise ValueError(f"Refusing forbidden diagnostics file: {name}")


def redact(text: str, root: Path, homes: list[str] | None = None) -> str:
    paths = [(str(root.resolve()), "<KIT>")]
    paths += [(value, "<HOME>") for value in (homes if homes is not None else
              [str(Path.home()), os.environ.get("USERPROFILE", ""), os.environ.get("HOME", "")]) if value]
    # Longest first so a kit nested under the profile retains the more useful label.
    variants = {}
    for path, label in paths:
        for variant in (path, path.replace("\\", "/"), path.replace("/", "\\")):
            variants[variant.rstrip("/\\")] = label
    for path in sorted(variants, key=len, reverse=True):
        text = re.sub(re.escape(path), lambda _: variants[path], text, flags=re.IGNORECASE)
    return text


def read_text(path: Path) -> str:
    check_name(path.name)
    if path.is_symlink():
        raise ValueError(f"Refusing symlink input: {path.name}")
    return path.read_text(encoding="utf-8")


def log_tail(path: Path) -> str:
    check_name(path.name)
    if not path.exists():
        return "Build log not present.\n"
    if path.is_symlink():
        raise ValueError("Refusing symlink build log")
    with path.open(encoding="utf-8") as stream:
        return "".join(deque(stream, maxlen=300))


def command(args: list[str], root: Path) -> dict:
    try:
        result = subprocess.run(args, cwd=root, capture_output=True, text=True,
                                encoding="utf-8", timeout=30)
        return {"exit": result.returncode, "output": result.stdout.strip(),
                "error": result.stderr.strip()}
    except (OSError, UnicodeError, subprocess.TimeoutExpired) as error:
        return {"unavailable": str(error)}


def pins(root: Path) -> dict:
    # Parse literal declarations, not imports with optional build dependencies.
    wanted = {"DSD_RELEASE", "GODOT_CPP_TAG", "GODOT_EDITOR_ASSET", "ZIG_WINDOWS_REQUIREMENT"}
    result = {}
    source = root / "tools/build_port.py"
    if source.exists():
        for node in ast.parse(read_text(source)).body:
            if isinstance(node, ast.Assign) and isinstance(node.value, ast.Constant):
                for target in node.targets:
                    if isinstance(target, ast.Name) and target.id in wanted:
                        value = node.value.value
                        result[target.id] = value.rsplit("/", 1)[-1] if target.id == "DSD_RELEASE" else value
    return result


def configuration(root: Path) -> dict:
    abi = root / "build/gen/abi.ini"
    backup = "not present"
    if abi.exists():
        parser = configparser.ConfigParser(interpolation=None)
        parser.read_string(read_text(abi))
        backup = parser.get("cartridge", "backup_type", fallback="not specified")
    exe_dir = root / "build/game/game"
    portable = (exe_dir / "portable.txt").exists()
    if portable:
        config = exe_dir / "portable/config/frontend.conf"
    else:
        config = Path(os.environ.get("APPDATA", str(Path.home() / ".config")) if os.name == "nt"
                      else os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))) / "dq9-decomp/frontend.conf"
    summary = {"present": config.exists(), "portable": portable, "settings": {}}
    if config.exists():
        for line in read_text(config).splitlines():
            key, sep, value = line.partition("=")
            key, value = key.strip(), value.strip()
            if not sep or key.startswith("#"):
                continue
            if key in {"rom_path", "save_path"}:
                summary["settings"][key + "_configured"] = bool(value)
            elif key == "language" and value in {"en", "fr", "de", "es", "it"}:
                summary["settings"][key] = value
            elif key == "backup_type" and re.fullmatch(r"[a-z0-9]+", value):
                summary["settings"][key] = value
            elif re.fullmatch(r"-?\d{1,9}", value):
                summary["settings"][key] = int(value)
    return {"abi_backup_type": backup, "pins": pins(root), "launcher": summary}


def binary_metadata(root: Path) -> dict:
    paths = [root / "build/game/game/dq9.exe"]
    paths += sorted((root / "godot/bin").glob("*.dll"))
    return {path.relative_to(root).as_posix(): {"exists": path.is_file(),
            "size": path.stat().st_size if path.is_file() else None} for path in paths}


def script_hashes(root: Path) -> dict:
    manifest = root / "script-paths.json"
    if manifest.exists():
        names = json.loads(read_text(manifest))
    else:
        result = command(["git", "ls-files", "-z"], root)
        if result.get("exit") != 0:
            return {"unavailable": "No kit script manifest or Git tracked-file list"}
        names = result["output"].split("\0")
    if not isinstance(names, list) or any(not isinstance(name, str) for name in names):
        raise ValueError("Invalid kit script manifest")
    hashes = {}
    for name in sorted(names):
        if Path(name).suffix.lower() not in SCRIPT_SUFFIXES:
            continue
        check_name(name)
        path = root / name
        if not path.resolve().is_relative_to(root.resolve()) or path.is_symlink():
            raise ValueError("Script path escapes kit")
        if path.is_file():
            hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hashes


def collect(root: Path) -> dict[str, str]:
    tools = {"python": sys.version, "packages": command([sys.executable, "-m", "pip", "list", "--format=json", "--disable-pip-version-check"], root),
             "zig": command([sys.executable, "-m", "ziglang", "version"], root),
             "cmake": command([sys.executable, "-m", "cmake", "--version"], root)}
    asset = "dsd-windows-x86_64.exe" if os.name == "nt" else "dsd-linux-x86_64"
    dsd = root / "build/tools" / asset
    tools["dsd"] = command([str(dsd) if dsd.exists() else "dsd", "--version"], root)
    system = {"os": platform.platform(), "architecture": platform.machine(),
              "free_disk_bytes": shutil.disk_usage(root).free}
    data = {"system.json": system, "tools.json": tools, "configuration.json": configuration(root),
            "binaries.json": binary_metadata(root), "script-sha256.json": script_hashes(root)}
    # Redact strings before JSON encoding (Windows backslashes become escaped).
    members = {name: json.dumps(redact_values(value, root), ensure_ascii=False, indent=2)
               for name, value in data.items()}
    members["build-log-tail.txt"] = redact(log_tail(root / "build/build-log.txt"), root)
    return members


def redact_values(value, root: Path):
    if isinstance(value, str):
        return redact(value, root)
    if isinstance(value, dict):
        return {redact(str(key), root): redact_values(item, root) for key, item in value.items()}
    if isinstance(value, list):
        return [redact_values(item, root) for item in value]
    return value


def write_archive(output: Path, members: dict[str, str]) -> None:
    for name in members:
        check_name(name)
        if name not in MEMBERS:
            raise ValueError(f"Not a diagnostics allowlist member: {name}")
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, text in sorted(members.items()):
            archive.writestr(name, text.encode("utf-8"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kit-dir", type=Path, default=ROOT)
    parser.add_argument("--output-dir", type=Path,
                        help="optional destination for inspecting a read-only kit (default: kit folder)")
    args = parser.parse_args(argv)
    root = args.kit_dir.resolve()
    output = (args.output_dir or root) / ("diagnostics-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + ".zip")
    try:
        write_archive(output, collect(root))
    except (OSError, ValueError, configparser.Error, SyntaxError) as error:
        print(redact(f"Diagnostics refused: {error}", root), file=sys.stderr)
        return 2
    print(f"Created {output.name}. Review the redacted text before sharing with support.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
